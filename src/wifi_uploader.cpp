#include "wifi_uploader.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_mac.h>
#include <algorithm>

#include "config.h"
#include "event_log.h"
#include "session_store.h"
#include "secrets.h"

// Defined in main.cpp: last sampled battery voltage (-1 until first sample).
extern float trackerLastVbat();

namespace {

// Concatenate the chip MAC into a stable id like "esp32-aabbccddeeff".
String chipIdString() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

// Read the entire CSV body for a session into a String. Stops if the file
// is larger than `maxBytes` (returns empty in that case so we don't blow
// the heap on a runaway session).
bool readWholeFile(SessionStore& store, const String& id, size_t maxBytes, String& out) {
    return store.readSessionToString(id, maxBytes, out);
}

// Returns true if a non-empty SSID + non-empty URL form a complete profile.
static bool hasProfile(const char* ssid, const char* url) {
    return ssid && ssid[0] != '\0' && url && url[0] != '\0';
}

// Returns true if this URL uses HTTPS.
static bool isHttpsUrl(const char* url) {
    // Compare the first 8 characters case-insensitively.
    if (!url) return false;
    const char prefix[] = "https://";
    for (int i = 0; i < 8; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c += 32;  // tolower, no ctype dependency
        if (c != prefix[i]) return false;
    }
    return true;
}

} // namespace


void WifiUploader::begin(SessionStore* store) {
    store_ = store;

    // Feature requires at least one complete profile (SSID + URL pair).
    // Either home, or remote/hotspot, or both.
    const bool hasHome   = hasProfile(secrets::WIFI_SSID,  secrets::INGEST_URL);
    const bool hasRemote = hasProfile(secrets::WIFI_SSID2, secrets::INGEST_URL2);
    if (!hasHome && !hasRemote) {
        enabled_ = false;
        phase_ = (uint8_t)Phase::Disabled;
        Serial.println("[WIFI] disabled (no complete SSID+URL profile in secrets.h)");
        return;
    }
    enabled_ = true;
    phase_ = (uint8_t)Phase::Idle;

    // v0.9.5: Keep WiFi in WIFI_STA permanently for the lifetime of the
    // firmware. Every prior version cycled to WIFI_OFF between upload cycles
    // which, on ESP32-S3 with NimBLE actively connected, races with the BLE
    // radio IRQ during esp_wifi_stop(). That race manifests as either:
    //   - INT_WDT (raw0=8):  IRQ masked >800 ms while mode-change completes
    //   - PANIC  (raw0=12): coex arbiter assert/abort()
    // Both were confirmed in field logs at uptimes from 114 s to 3314 s.
    // Keeping WiFi in STA+disconnected state between uploads is fully
    // compatible with BLE modem-sleep coex (WIFI_PS_MIN_MODEM). The coex
    // arbiter always knows where both radios are and schedules them cleanly.
    WiFi.mode(WIFI_STA);
    // Keep PA current low to avoid brown-outs on battery with BLE coex.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    // v0.4.8: persistent=true so the WiFi driver caches the AP info (BSSID,
    // channel, etc.) in NVS. Subsequent connects can use that cache to skip
    // a full scan. Was false, which forced a full active scan every cycle
    // and contributed to the 12 s timeout being too short.
    WiFi.persistent(true);
    WiFi.setAutoReconnect(false);
    // Start disconnected so the task's first cycle does a clean begin().
    WiFi.disconnect(false, false);

    // Pin the worker to core 0; the Arduino loop runs on core 1, so HTTP
    // POSTs and Wi-Fi connect timeouts can never freeze the UI/button polling.
    BaseType_t ok = xTaskCreatePinnedToCore(
        &WifiUploader::taskTrampoline,
        "wifi_up",
        8192,           // stack: HTTPClient + TLS-free POST is well under this
        this,
        1,              // priority: lower than NimBLE/loop
        &task_,
        0);             // core 0
    if (ok != pdPASS) {
        Serial.println("[WIFI] FATAL: failed to spawn uploader task");
        enabled_ = false;
        task_ = nullptr;
        return;
    }

    Serial.printf("[WIFI] uploader armed; home='%s' remote='%s' interval=%us trackerId=%s\n",
                  hasHome ? secrets::WIFI_SSID   : "(disabled)",
                  hasRemote ? secrets::WIFI_SSID2 : "(disabled)",
                  (unsigned)(secrets::UPLOAD_INTERVAL_MS / 1000),
                  chipIdString().c_str());
}

void WifiUploader::requestNow() {
    if (task_) xTaskNotifyGive(task_);
}

void WifiUploader::taskTrampoline(void* arg) {
    static_cast<WifiUploader*>(arg)->taskLoop();
}

void WifiUploader::taskLoop() {
    // v0.6.0: subscribe this task to the global task watchdog. Pet it once
    // per outer iteration and just before each long-blocking call.
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();
    // 5s post-boot grace so BLE/GPS finish coming up before we light Wi-Fi.
    // Was 15s in v0.4.1; reduced in v0.4.7 because reboots happen often
    // enough that the user was waiting 75+s for the first upload attempt
    // after each one.
    vTaskDelay(pdMS_TO_TICKS(5000));
    // Backoff state for repeated connect failures (e.g. when out of Wi-Fi
    // range for hours). Without backoff we churn the radio and PA every
    // 60 s, which on a marginal power supply triggers a brown-out reset.
    for (;;) {
        esp_task_wdt_reset();
        const uint32_t okBefore = uploadedCount_;
        const uint32_t failBefore = failedCount_;
        runOnce();
        if (failedCount_ > failBefore && uploadedCount_ == okBefore) {
            ++consecutiveFailures_;
            // v0.8.1: self-heal for lwIP pool exhaustion / heap fragmentation.
            // After a long uptime with thousands of WiFi cycles, the lwIP pbuf
            // pool or general heap can fragment to the point where TCP writes
            // return EAGAIN on every attempt (observed at heap_free ~37KB after
            // 70+ hours / 2755 cycles). A soft reboot clears all networking
            // state and restores the pools. We only reboot if heap is genuinely
            // low -- if heap is healthy the failures are a real server problem
            // that a reboot cannot fix. LittleFS persists through ESP.restart()
            // so all pending .up.csv files and the active day file are safe.
            if (consecutiveFailures_ >= cfg::WIFI_FAIL_REBOOT_THRESHOLD) {
                const uint32_t freeHeap = ESP.getFreeHeap();
                if (freeHeap < cfg::WIFI_HEAL_MIN_HEAP) {
                    Serial.printf("[WIFI] self-heal: %u failures + heap=%u < %u -- rebooting\n",
                                  (unsigned)consecutiveFailures_,
                                  (unsigned)freeHeap,
                                  (unsigned)cfg::WIFI_HEAL_MIN_HEAP);
                    event_log::appendEvent("REBOOT", "wifi_heal_low_heap");
                    vTaskDelay(pdMS_TO_TICKS(2000)); // let serial flush
                    ESP.restart();
                } else {
                    Serial.printf("[WIFI] %u failures but heap=%u OK -- skipping self-reboot\n",
                                  (unsigned)consecutiveFailures_, (unsigned)freeHeap);
                }
            }
        } else if (uploadedCount_ > okBefore) {
            consecutiveFailures_ = 0;
        }
        // v0.4.7: Faster recovery. First two failures retry at the base
        // cadence (60s). From the 3rd consecutive failure onward, ramp
        // exponentially (60s -> 120s -> 240s -> ... cap 16 min). Previously
        // we ramped from the 2nd failure, which made every post-boot retry
        // take 2 min because the first two attempts after boot always fail.
        uint32_t mult = 1u;
        if (consecutiveFailures_ > 2) {
            uint32_t shift = consecutiveFailures_ - 2;
            if (shift > 4) shift = 4;   // cap at 16x
            mult = 1u << shift;
        }
        uint32_t sleepMs = secrets::UPLOAD_INTERVAL_MS * mult;
        // v0.4.7: drain the queue. If we just succeeded and there is still
        // more work in the active day file (i.e. samples have been written
        // since rotateForUpload was last called), kick another cycle in 5s
        // instead of waiting the full 60s. The store-side rotate is gated
        // by "only when no pending files", so this is safe — it just lets
        // a backlog drain quickly when the AP is finally in range.
        if (uploadedCount_ > okBefore && store_) {
            sleepMs = 5000;
        }
        nextAttempt_ = millis() + sleepMs;
        phase_ = (uint8_t)(mult > 1 ? Phase::Backoff : Phase::Idle);
        if (mult > 1) {
            Serial.printf("[WIFI] backoff: %u consecutive failures, next attempt in %us\n",
                          (unsigned)consecutiveFailures_, (unsigned)(sleepMs / 1000));
        }
        // Sleep until either the cadence elapses or requestNow() pokes us.
        // v0.6.0: chunk the sleep into <= 20 s slices so the task watchdog
        // (30 s timeout) stays petted during long backoff intervals.
        uint32_t remaining = sleepMs;
        while (remaining > 0) {
            const uint32_t chunk = (remaining > 20000u) ? 20000u : remaining;
            const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(chunk));
            esp_task_wdt_reset();
            if (got) break;  // poked by requestNow()
            remaining -= chunk;
        }
    }
}


bool WifiUploader::connectWifi() {
    // Reset per-cycle state so a stale pointer from a previous cycle is never
    // used if begin() was somehow called twice or in a weird order.
    activeIngestUrl_  = nullptr;
    activeIngestUser_ = nullptr;
    activeIngestPass_ = nullptr;
    activeNet_ = (uint8_t)ActiveNet::None;

    // ------------------------------------------------------------------ //
    // v0.9.5: WiFi stays in WIFI_STA for the entire firmware lifetime (set
    // once in begin()). We only flush the previous association here.
    // No mode() calls at all -- cycling WIFI_OFF<->WIFI_STA while NimBLE
    // has an active connection races with the BLE radio IRQ and causes
    // INT_WDT (raw0=8) or coex-arbiter PANIC (raw0=12). See begin() comment.
    //
    // v0.9.3: do NOT call WiFi.setSleep(false). On ESP32-S3 with NimBLE
    // active, the WiFi driver calls abort() if modem sleep is disabled
    // ("Error! Should enable WiFi modem sleep when both WiFi and Bluetooth
    // are enabled"). The BLE coexistence arbiter requires WIFI_PS_MIN_MODEM.
    // ------------------------------------------------------------------ //
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(100));   // brief settle before begin()

    // ------------------------------------------------------------------ //
    // Inner helper: associate with one SSID. Already in WIFI_STA mode;
    // only calls begin() / disconnect() -- never mode().
    // ------------------------------------------------------------------ //
    auto tryConnect = [&](const char* ssid, const char* pass) -> bool {
        Serial.printf("[WIFI] trying '%s'...\n", ssid);
        WiFi.begin(ssid, pass);
        const uint32_t start    = millis();
        const uint32_t deadline = start + secrets::WIFI_CONNECT_TIMEOUT_MS;
        while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
            esp_task_wdt_reset();   // v0.7.1: pet inside the blocking wait
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] connected ip=%s rssi=%d dBm in %ums ch=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                          (unsigned)(millis() - start), WiFi.channel());
            return true;
        }
        Serial.printf("[WIFI] '%s' timeout (status=%d after %ums)\n",
                      ssid, (int)WiFi.status(), (unsigned)(millis() - start));
        // Between SSIDs: stop the current association attempt without cycling
        // the radio stack (that's what triggers INT_WDT on the second attempt).
        WiFi.disconnect(false, false);
        vTaskDelay(pdMS_TO_TICKS(300));
        return false;
    };

    // ---- 1. Try home profile first ----------------------------------------
    const bool hasHome = hasProfile(secrets::WIFI_SSID, secrets::INGEST_URL);
    if (hasHome) {
        if (tryConnect(secrets::WIFI_SSID, secrets::WIFI_PASSWORD)) {
            activeIngestUrl_ = secrets::INGEST_URL;
            activeNet_ = (uint8_t)ActiveNet::Home;
            return true;
        }
    }

    // ---- 2. Fall back to remote (mobile hotspot) --------------------------
    const bool hasRemote = hasProfile(secrets::WIFI_SSID2, secrets::INGEST_URL2);
    if (hasRemote) {
        if (tryConnect(secrets::WIFI_SSID2, secrets::WIFI_PASSWORD2)) {
            activeIngestUrl_ = secrets::INGEST_URL2;
            // Apply Basic Auth credentials if configured for remote endpoint.
            if (secrets::INGEST_USER && secrets::INGEST_USER[0] != '\0') {
                activeIngestUser_ = secrets::INGEST_USER;
                activeIngestPass_ = secrets::INGEST_PASS;
            }
            activeNet_ = (uint8_t)ActiveNet::Remote;
            return true;
        }
    }

    // Neither network connected -- stay in STA+disconnected (no mode change).
    WiFi.disconnect(false, false);
    return false;
}


void WifiUploader::disconnectWifi() {
    event_log::markPhase("WIFI_DISCO_IN");
    // v0.4.8: eraseAP=false so persistent cache survives between cycles.
    // v0.9.5: no WiFi.mode(WIFI_OFF) -- stay in STA+disconnected permanently.
    WiFi.disconnect(false, false);
    // Clear per-cycle networking state so stale pointers aren't used
    // if something goes wrong between cycles.
    activeIngestUrl_  = nullptr;
    activeIngestUser_ = nullptr;
    activeIngestPass_ = nullptr;
    activeNet_ = (uint8_t)ActiveNet::None;
    event_log::markPhase("WIFI_DISCO_OUT");
}


bool WifiUploader::uploadOne(const String& filename, const String& sessionId, size_t expectedBytes) {
    // Use the URL selected when connectWifi() succeeded. Fall back to the
    // primary URL as a safety net (should never be needed in normal flow).
    const char* url = activeIngestUrl_ ? activeIngestUrl_ : secrets::INGEST_URL;

    Serial.printf("[UPLOAD] %s (file=%s): file_bytes=%u heap_free=%u net=%s url=%s\n",
                  sessionId.c_str(), filename.c_str(), (unsigned)expectedBytes,
                  (unsigned)ESP.getFreeHeap(),
                  activeNet_ == (uint8_t)ActiveNet::Remote ? "remote" : "home",
                  url);

    // --- Stream directly from the file into HTTPClient ---------------------
    // Avoids loading the whole CSV into a heap String. On a 320 KB DRAM
    // device a file with >~1500 rows (~225 KB) would exhaust the heap
    // mid-String, silently truncate the upload, and the partial 2xx response
    // would cause the file to be deleted with data permanently lost.
    // Using HTTPClient::sendRequest(type, Stream*, size) bypasses the heap
    // entirely: the HTTP stack reads the file in small chunks as it sends.
    size_t fileSize = 0;
    Stream* fileStream = store_->openPendingUploadStream(filename, fileSize);

    String body;  // only used as fallback for SdFat backend
    if (!fileStream) {
        // SdFat backend (V1.2): fall back to buffered read.
        // 1 MB cap keeps us within available heap on smaller files.
        constexpr size_t MAX_FALLBACK_BYTES = 1 * 1024 * 1024;
        Serial.printf("[UPLOAD] %s: stream unavailable (SdFat?), falling back to String read\n",
                      sessionId.c_str());
        if (!store_->readPendingUploadToString(filename, MAX_FALLBACK_BYTES, body) || body.length() == 0) {
            Serial.printf("[UPLOAD] %s: read failed (bytes=%u maxFallback=%u)\n",
                          sessionId.c_str(), (unsigned)expectedBytes, (unsigned)MAX_FALLBACK_BYTES);
            return false;
        }
        fileSize = body.length();
        Serial.printf("[UPLOAD] %s: fallback string_bytes=%u heap_after=%u\n",
                      sessionId.c_str(), (unsigned)fileSize, (unsigned)ESP.getFreeHeap());
    } else {
        Serial.printf("[UPLOAD] %s: streaming file_bytes=%u heap=%u\n",
                      sessionId.c_str(), (unsigned)fileSize, (unsigned)ESP.getFreeHeap());
    }

    if (fileSize == 0) {
        Serial.printf("[UPLOAD] %s: zero bytes, skipping\n", sessionId.c_str());
        store_->closeSessionStream();
        return false;
    }

    HTTPClient http;
    // v0.9.0: HTTPS support for the remote (internet-facing) endpoint.
    // WiFiClientSecure is heap-allocated so it doesn't blow the task stack.
    // setInsecure() skips certificate chain verification -- acceptable for
    // GPS/radiation telemetry over a residential hotspot; avoids needing to
    // embed a CA bundle in flash.
    WiFiClientSecure* secureClient = nullptr;
    bool beginOk;
    if (isHttpsUrl(url)) {
        secureClient = new WiFiClientSecure();
        secureClient->setInsecure();
        beginOk = http.begin(*secureClient, url);
    } else {
        beginOk = http.begin(url);
    }
    if (!beginOk) {
        Serial.printf("[UPLOAD] %s: http.begin('%s') failed\n",
                      sessionId.c_str(), url);
        store_->closeSessionStream();
        delete secureClient;
        return false;
    }
    http.setTimeout(30000);   // larger sessions need more time to stream
    http.addHeader("Content-Type", "text/csv");
    http.addHeader("X-Session-Id", sessionId);
    http.addHeader("X-Tracker-Id", chipIdString());
    http.addHeader("X-Firmware",   cfg::FW_VERSION);
    // v0.9.0: HTTP Basic Auth for the internet-facing endpoint (nginx proxy).
    // Applied only when creds are configured and we're on the remote network.
    if (activeIngestUser_ && activeIngestUser_[0] != '\0') {
        http.setAuthorization(activeIngestUser_, activeIngestPass_ ? activeIngestPass_ : "");
    }

    const uint32_t t0 = millis();
    int code;
    // v0.7.1: pet the WDT immediately before AND after the HTTP request.
    // sendRequest()/POST can block for up to http.setTimeout() (30 s) on a
    // slow link; without these resets a slow upload would WDT-panic. We do
    // not (and cannot) pet during the request itself because HTTPClient
    // does not yield a callback hook. The 60 s WDT timeout in config.h is
    // sized to cover the worst-case 30 s POST cleanly.
    esp_task_wdt_reset();
    if (fileStream) {
        // Stream path: no heap allocation for body.
        code = http.sendRequest("POST", fileStream, fileSize);
    } else {
        code = http.POST((uint8_t*)body.c_str(), body.length());
    }
    esp_task_wdt_reset();
    const String resp = (code > 0) ? http.getString() : String();
    http.end();
    store_->closeSessionStream();
    // Clean up the heap-allocated TLS client (if any) after http.end().
    delete secureClient;
    secureClient = nullptr;
    lastHttpStatus_ = code;

    if (code >= 200 && code < 300) {
        Serial.printf("[UPLOAD] %s OK http=%d %ums file_bytes=%u resp=%s\n",
                      sessionId.c_str(), code, (unsigned)(millis() - t0),
                      (unsigned)fileSize,
                      resp.length() > 200 ? "<truncated>" : resp.c_str());
        return true;
    }
    Serial.printf("[UPLOAD] %s FAIL http=%d %ums resp=%s\n",
                  sessionId.c_str(), code, (unsigned)(millis() - t0),
                  resp.length() > 200 ? "<truncated>" : resp.c_str());
    return false;
}


uint32_t WifiUploader::runOnce() {
    if (!enabled_ || !store_) return 0;

    // v0.4.2: VBAT gate. The Wi-Fi PA spikes (200-400 mA) collapse a weak
    // battery rail and brown-out the MCU mid-cycle, leading to reboots
    // every 1-2 minutes on a partially discharged pack. Recording (BLE +
    // GPS, ~80 mA steady) is fine; only the radio is risky. Skip the cycle
    // and try again next interval -- pending files stay queued.
    const float vbat = trackerLastVbat();
    if (vbat > 0.0f && vbat < cfg::VBAT_MIN_FOR_WIFI) {
        char msg[64];
        snprintf(msg, sizeof(msg), "skip vbat=%.2fV<%.2fV",
                 vbat, cfg::VBAT_MIN_FOR_WIFI);
        Serial.printf("[WIFI] %s -- recording continues\n", msg);
        event_log::appendEvent("WIFI", msg);
        return 0;
    }

    // v0.4.0: only rotate the active day file to pending-upload state when
    // there are zero pending files already. This prevents fragmenting the
    // active file into dozens of tiny .up.csv slices when we are out of
    // Wi-Fi range for a long time -- recording keeps appending to the
    // existing day file and we make a single, larger pending file once we
    // succeed in uploading whatever was already queued.
    auto pending = store_->listPendingUploads();
    if (pending.empty()) {
        store_->rotateForUpload();
        pending = store_->listPendingUploads();
    }

    if (pending.empty()) {
        return 0;   // nothing pending; don't bring up Wi-Fi
    }

    busy_ = true;
    phase_ = (uint8_t)Phase::Connecting;
    lastAttempt_ = millis();
    Serial.printf("[UPLOAD] cycle start; %u file(s) pending heap_free=%u\n",
                  (unsigned)pending.size(), (unsigned)ESP.getFreeHeap());

    // Plant the RTC marker BEFORE turning on the radio. If we brown-out
    // during connect, the next boot will see wifiInFlight=1 in the log
    // and we can correlate the reset reason to the PA current spike.
    event_log::markWifiInFlight(true);
    event_log::markPhase("WIFI_CONNECT");
    event_log::appendEvent("WIFI", "cycle_start");

    if (!connectWifi()) {
        ++failedCount_;
        busy_ = false;
        phase_ = (uint8_t)Phase::Idle;
        event_log::markWifiInFlight(false);
        event_log::appendEvent("WIFI", "connect_fail");
        return 0;
    }

    uint32_t ok = 0;
    for (const auto& p : pending) {
        // v0.4.5: stale zero-byte .up.csv files (left over from earlier
        // panic-crashes that rotated but died before writing) would pile
        // up forever and trip the exponential backoff. Treat them as a
        // successful upload so they get deleted from disk.
        if (p.sizeBytes == 0) {
            Serial.printf("[UPLOAD] %s: stale zero-byte file, removing (%s)\n",
                          p.sessionId.c_str(), p.filename.c_str());
            if (store_->removePendingUpload(p.filename)) {
                ++ok;
                ++uploadedCount_;
            }
            continue;
        }
        event_log::markPhase("WIFI_POST");
        phase_ = (uint8_t)Phase::Posting;
        if (uploadOne(p.filename, p.sessionId, p.sizeBytes)) {
            ++ok;
            ++uploadedCount_;
            lastSuccess_ = millis();
            // v0.4.0: always delete after confirmed server receipt.
            // The "never duplicate data on device" contract is the whole
            // point of the rotate-then-delete model.
            if (store_->removePendingUpload(p.filename)) {
                Serial.printf("[UPLOAD] %s: removed from device (%s)\n",
                              p.sessionId.c_str(), p.filename.c_str());
            } else {
                Serial.printf("[UPLOAD] %s: server OK but local remove failed (%s)\n",
                              p.sessionId.c_str(), p.filename.c_str());
                ++failedCount_;
            }
        } else {
            ++failedCount_;
        }
        // Brief yield between files so BLE/UI tasks keep running.
        vTaskDelay(pdMS_TO_TICKS(50));
        // v0.7.1: pet the WDT between files. A backlog of N files each
        // taking up to 30 s would otherwise WDT-panic on the 2nd file.
        esp_task_wdt_reset();
    }

    phase_ = (uint8_t)Phase::Disconnecting;
    disconnectWifi();
    esp_task_wdt_reset();
    busy_ = false;
    phase_ = (uint8_t)Phase::Idle;
    event_log::markWifiInFlight(false);
    char done[48];
    snprintf(done, sizeof(done), "cycle_done ok=%u/%u",
             (unsigned)ok, (unsigned)pending.size());
    event_log::appendEvent("WIFI", done);
    Serial.printf("[UPLOAD] cycle done; ok=%u/%u heap_free=%u\n",
                  (unsigned)ok, (unsigned)pending.size(), (unsigned)ESP.getFreeHeap());
    return ok;
}
