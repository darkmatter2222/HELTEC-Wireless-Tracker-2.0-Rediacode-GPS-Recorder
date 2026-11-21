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
    // v2.3: setTxPower() only works once the driver reports STA_STARTED, and
    // that bit is set ASYNCHRONOUSLY by the WiFi event task when it dequeues
    // ARDUINO_EVENT_WIFI_STA_START (WiFiGeneric.cpp:1043-1045). Calling the
    // setter on the line after mode() logged "Neither AP or STA has been
    // started" and returned false, so the PA silently stayed at the 19.5 dBm
    // default and the brown-out mitigation was never applied. WiFi.status()
    // only leaves WL_NO_SHIELD in that same event handler, so it is a reliable
    // readiness probe. 50 x 10 ms = 500 ms bound; the task isn't spawned yet.
    for (int i = 0; i < 50 && WiFi.status() == WL_NO_SHIELD; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Keep PA current low to avoid brown-outs on battery with BLE coex.
    const bool txSet = WiFi.setTxPower(WIFI_POWER_8_5dBm);
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
    // v2.3: state the two numbers that decide whether an upload can happen at
    // all, so a boot log alone is enough to interpret later skip lines.
    // `tx_applied=0` means the PA stayed at the 19.5 dBm default. `tx_raw` is
    // the driver readback in 0.25 dBm units: 34 == 8.5 dBm, 78 == 19.5 dBm.
    Serial.printf("[WIFI] gate: vbat open>=%.2fV close<=%.2fV | connect budget=%ums/profile | tx_applied=%d tx_raw=%d\n",
                  (double)cfg::VBAT_MIN_FOR_WIFI, (double)cfg::VBAT_GATE_FLOOR,
                  (unsigned)secrets::WIFI_CONNECT_TIMEOUT_MS, (int)txSet,
                  (int)WiFi.getTxPower());
}

void WifiUploader::requestNow() {
    // v2.3: stamp the deadline as "now" so the STORAGE countdown flips to
    // "Next sync: soon" immediately. Previously nextAttempt_ kept the pre-poke
    // value, so the screen kept showing e.g. "Next sync: 3m 12s" while the
    // poked cycle was already running -- which read as "the button does nothing".
    nextAttempt_ = millis();
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
        // v2.3: the ladder itself lives in wgate:: so the native suite can
        // assert it. Note a battery/no-pending skip changes neither counter,
        // so it deliberately holds the current multiplier instead of ramping:
        // a gated cycle is not a link failure.
        const uint32_t mult = wgate::backoffMultiplier(consecutiveFailures_);
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


// One association attempt against one SSID. Uses the whole per-SSID budget
// (secrets::WIFI_CONNECT_TIMEOUT_MS) and reports both the measured duration
// and the driver status code so the caller can log why it failed.
bool WifiUploader::tryConnect(const char* ssid, const char* pass,
                              uint32_t* elapsedMs, int* statusCode) {
    Serial.printf("[WIFI] trying '%s' (budget %ums)...\n", ssid,
                  (unsigned)secrets::WIFI_CONNECT_TIMEOUT_MS);
    const uint32_t start    = millis();
    WiFi.begin(ssid, pass);
    const uint32_t deadline = start + secrets::WIFI_CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
        esp_task_wdt_reset();   // v0.7.1: pet inside the blocking wait
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    const uint32_t took = millis() - start;
    const int      st   = (int)WiFi.status();
    if (elapsedMs)  *elapsedMs  = took;
    if (statusCode) *statusCode = st;
    ++connectAttempts_;
    lastConnectMs_     = took;
    lastConnectStatus_ = st;
    if (st == (int)WL_CONNECTED) {
        Serial.printf("[WIFI] connected ip=%s rssi=%d dBm in %ums ch=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                      (unsigned)took, WiFi.channel());
        return true;
    }
    // v2.3: decode the driver status. `status=6(DISCONNECTED)` means the
    // supplicant never got past scan/assoc (AP absent or too weak), whereas
    // `4(CONNECT_FAILED)` is a wrong password / refused association. Those two
    // looked identical before and cost a full debug cycle to separate.
    Serial.printf("[WIFI] '%s' timeout status=%d(%s) after %ums\n",
                  ssid, st, wgate::statusName(st), (unsigned)took);
    // Between SSIDs: stop the current association attempt without cycling
    // the radio stack (that's what triggers INT_WDT on the second attempt).
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(wgate::INTER_SSID_SETTLE_MS));
    return false;
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
    vTaskDelay(pdMS_TO_TICKS(wgate::CONNECT_SETTLE_MS));   // brief settle before begin()

    // ---- 1. Try home profile first ----------------------------------------
    const bool hasHome   = hasProfile(secrets::WIFI_SSID,   secrets::INGEST_URL);
    const bool hasRemote = hasProfile(secrets::WIFI_SSID2,  secrets::INGEST_URL2);
    const int  nProfiles = (hasHome ? 1 : 0) + (hasRemote ? 1 : 0);

    // v2.3: print the arithmetic budget up front. Comparing this number to the
    // observed cycle_start -> connect_fail delta is what identifies a burnt
    // timeout vs. an early success, straight from the persistent log.
    Serial.printf("[WIFI] profiles=%d timeout=%ums worst_case=%ums\n",
                  nProfiles, (unsigned)secrets::WIFI_CONNECT_TIMEOUT_MS,
                  (unsigned)wgate::worstCaseConnectMs(secrets::WIFI_CONNECT_TIMEOUT_MS,
                                                      nProfiles));

    uint32_t elapsed  = 0;
    int      status   = 0;
    if (hasHome) {
        if (tryConnect(secrets::WIFI_SSID, secrets::WIFI_PASSWORD, &elapsed, &status)) {
            activeIngestUrl_ = secrets::INGEST_URL;
            activeNet_ = (uint8_t)ActiveNet::Home;
            Serial.printf("[WIFI] home ok in %ums net=home\n", (unsigned)elapsed);
            return true;
        }
    }

    // ---- 2. Fall back to remote (mobile hotspot) --------------------------
    if (hasRemote) {
        if (tryConnect(secrets::WIFI_SSID2, secrets::WIFI_PASSWORD2, &elapsed, &status)) {
            activeIngestUrl_ = secrets::INGEST_URL2;
            // Apply Basic Auth credentials if configured for remote endpoint.
            if (secrets::INGEST_USER && secrets::INGEST_USER[0] != '\0') {
                activeIngestUser_ = secrets::INGEST_USER;
                activeIngestPass_ = secrets::INGEST_PASS;
            }
            activeNet_ = (uint8_t)ActiveNet::Remote;
            Serial.printf("[WIFI] remote ok in %ums net=remote auth=%d\n",
                          (unsigned)elapsed, activeIngestUser_ ? 1 : 0);
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
    int code = 0;
    String resp;
    // v0.7.1: pet the WDT immediately before AND after every HTTP request.
    // v2.2: the body is sent in whole-CSV-line chunks of
    // cfg::UPLOAD_CHUNK_BYTES over one keep-alive connection, so no single
    // un-petted window can exceed TASK_WDT_TIMEOUT_S even at ~300 B/s.
    // One sendRequest blocks for (chunkBytes/linkRate) + 30 s response
    // timeout ~= 57 s worst case; 8192/400 + 30 = 50.5 s at the field-
    // measured LTE pace. The API parses each chunk independently (header
    // rows skipped value-based, upserts idempotent), so chunked == single.
    esp_task_wdt_reset();
    constexpr size_t CH = cfg::UPLOAD_CHUNK_BYTES;
    if (fileStream) {
        // Stream path: no heap allocation for the whole body; one chunk
        // buffer is reused for every POST.
        uint8_t* cbuf = (uint8_t*)malloc(CH);
        if (!cbuf) {
            Serial.printf("[UPLOAD] %s: chunk buffer alloc failed (%u)\n",
                          sessionId.c_str(), (unsigned)CH);
            store_->closeSessionStream();
            delete secureClient;
            secureClient = nullptr;
            return false;
        }
        size_t sent = 0;   // body bytes POSTed so far
        size_t carry = 0;  // partial-line bytes held over to the next chunk
        uint32_t chunks = 0;
        bool eof = false;
        bool okAll = true;
        while (okAll && (!eof || carry > 0)) {
            size_t n = carry;
            if (!eof) {
                size_t want = CH - carry;
                int got = fileStream->readBytes((char*)cbuf + carry, (unsigned)want);
                if (got < 0) got = 0;
                n += (size_t)got;
                if ((size_t)got < want) eof = true;  // short read == EOF
                if (n == 0) { okAll = false; break; }
            }
            // Send only whole lines: a row split across two POSTs would be
            // rejected by the API's per-request CSV parse. On EOF the tail
            // IS the final row, so send everything held.
            size_t last = n;
            if (!eof) {
                while (last > 0 && cbuf[last - 1] != '\n') --last;
                if (last == 0) last = n;  // single row longer than CH
            }
            carry = n - last;
            if (carry) memmove(cbuf, cbuf + last, carry);
            if (last) {
                code = http.POST(cbuf, last);
                if (code > 0) resp = http.getString();  // drain body for reuse
                if (code < 200 || code >= 300) { okAll = false; break; }
                sent += last;
                ++chunks;
            }
            esp_task_wdt_reset();
        }
        free(cbuf);
        lastChunks_ = chunks;
        if (okAll && sent != fileSize) okAll = false;
        if (!okAll && code == 0) code = HTTPC_ERROR_SEND_PAYLOAD_FAILED;
        if (!okAll) {
            // v2.3: report how far the drain got. `sent` vs the ceil division
            // tells a stalled write apart from a rejected chunk immediately:
            // sent == 0 -> nothing left the radio; sent < expected -> mid-body
            // stop; chunks == expected with a bad code -> server-side refusal.
            Serial.printf("[UPLOAD] %s: chunk %u/%u failed at %u/%u bytes http=%d\n",
                          sessionId.c_str(), (unsigned)chunks,
                          (unsigned)wgate::chunkCount(fileSize, CH),
                          (unsigned)sent, (unsigned)fileSize, code);
            store_->closeSessionStream(); delete secureClient; secureClient = nullptr; return false;
        }
    } else {
        // String fallback (SdFat backend): same line-aligned chunking.
        size_t off = 0;
        uint32_t chunks = 0;
        bool okAll = true;
        while (off < fileSize && okAll) {
            size_t want = CH;
            if (fileSize - off < want) want = fileSize - off;
            size_t endAt = off + want;
            int nl = (int)endAt - 1;
            if (endAt < fileSize) {
                int found = body.lastIndexOf('\n', (unsigned)(endAt - 1));
                if (found >= (int)off) nl = found;
            }
            size_t len = (size_t)nl - off + 1;
            code = http.POST((uint8_t*)body.c_str() + off, len);
            if (code > 0) resp = http.getString();
            if (code < 200 || code >= 300) { okAll = false; break; }
            off += len;
            ++chunks;
            esp_task_wdt_reset();
        }
        lastChunks_ = chunks;
    }
    esp_task_wdt_reset();
    http.end();
    store_->closeSessionStream();
    // Clean up the heap-allocated TLS client (if any) after http.end().
    delete secureClient;
    secureClient = nullptr;
    lastHttpStatus_ = code;

    if (code >= 200 && code < 300) {
        const uint32_t took = (uint32_t)(millis() - t0);
        lastCycleMs_    = took;
        lastCycleBytes_ = fileSize;
        // v2.3: add chunks + effective rate so a slow-link diagnosis no longer
        // needs a server-side stopwatch. rate is in bytes/s (integer).
        const uint32_t rate = took ? (uint32_t)((uint64_t)fileSize * 1000u / took) : 0u;
        Serial.printf("[UPLOAD] %s OK http=%d %ums file_bytes=%u chunks=%u rate=%uB/s resp=%s\n",
                      sessionId.c_str(), code, (unsigned)took,
                      (unsigned)fileSize, (unsigned)lastChunks_, (unsigned)rate,
                      resp.length() > 200 ? "<truncated>" : resp.c_str());
        return true;
    }
    lastCycleBytes_ = fileSize;
    Serial.printf("[UPLOAD] %s FAIL http=%d %ums resp=%s\n",
                  sessionId.c_str(), code, (unsigned)(millis() - t0),
                  resp.length() > 200 ? "<truncated>" : resp.c_str());
    return false;
}


// Evaluates the VBAT hysteresis latch for this cycle and refreshes the charge
// trend from the same sample pair. Pure decisions live in wgate:: so the host
// suite can exercise them without hardware (see test/test_wifi_gate_native).
bool WifiUploader::vbatGateAllows() {
    const float vbat = trackerLastVbat();
    const bool  ok   = wgate::vbatGateOk(vbat, vbatGateOpen_,
                                          cfg::VBAT_MIN_FOR_WIFI,
                                          cfg::VBAT_GATE_FLOOR);
    // 60 s between cycles; the charger's CC phase moves a 1S cell ~20 mV/min,
    // so the 10 mV dead band separates "rising" from ADC noise cleanly.
    const bool rising = wgate::voltageRising(vbatPrev_, vbat, cfg::VBAT_CHARGE_DELTA_V);
    chargeRun_ = wgate::chargeRun(chargeRun_, rising);
    vbatPrev_  = vbat;
    charging_  = (uint8_t)(chargeRun_ >= (uint32_t)cfg::VBAT_CHARGE_RUN ? 1u : 0u);

    if (ok != vbatGateOpen_) {
        Serial.printf("[WIFI] gate %s vbat=%.2fV (opens>=%.2fV closes<=%.2fV) chg=%u\n",
                      ok ? "OPEN" : "CLOSED", (double)vbat,
                      (double)cfg::VBAT_MIN_FOR_WIFI, (double)cfg::VBAT_GATE_FLOOR,
                      (unsigned)charging_);
    }
    vbatGateOpen_ = ok;
    return ok;
}


uint32_t WifiUploader::runOnce() {
    if (!enabled_ || !store_) return 0;

    // v2.3: list the queue first (cheap, ~16 entries) so BOTH the gated and
    // the ungated path can report the backlog size, and the rotate decision
    // below needs only one listing. Read-only, so the "no pending -> never
    // open the radio" optimization is unchanged.
    auto pending = store_->listPendingUploads();
    size_t pendingBytes = 0;
    for (const auto& p : pending) pendingBytes += p.sizeBytes;

    // v0.4.2 / v2.3: VBAT gate with hysteresis. The Wi-Fi PA spikes (200-400
    // mA) collapse a weak battery rail and brown-out the MCU mid-cycle, leading
    // to reboots every 1-2 minutes on a partially discharged pack. Recording
    // (BLE + GPS, ~80 mA steady) is fine; only the radio is risky. Skip the
    // cycle and try again next interval -- pending files stay queued.
    if (!vbatGateAllows()) {
        ++skippedCount_;
        lastSkipReason_ = wgate::SkipBattery;
        lastSkipMv_     = (int)(trackerLastVbat() * 1000.0f + 0.5f);
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "skip vbat=%.2fV<%.2fV gate=closed floor=%.2fV chg=%u pend=%u bytes=%u",
                 (double)trackerLastVbat(), (double)cfg::VBAT_MIN_FOR_WIFI,
                 (double)cfg::VBAT_GATE_FLOOR,
                 (unsigned)charging_, (unsigned)pending.size(), (unsigned)pendingBytes);
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
    if (pending.empty()) {
        store_->rotateForUpload();
        pending = store_->listPendingUploads();
        pendingBytes = 0;
        for (const auto& p : pending) pendingBytes += p.sizeBytes;
    }

    if (pending.empty()) {
        // Nothing queued; don't bring up the radio at all.
        lastSkipReason_ = wgate::SkipNoPending;
        lastSkipMv_     = -1;
        // v2.3: print it. Before this, an idle device emitted nothing at all
        // around a poked SYNC, which made "nothing to send" indistinguishable
        // from "uploader wedged" on serial. One line per cadence is cheap next
        // to the 3 s heartbeat.
        Serial.printf("[WIFI] skip=no_pending radio=off heap_free=%u\n",
                      (unsigned)ESP.getFreeHeap());
        return 0;
    }

    busy_ = true;
    phase_ = (uint8_t)Phase::Connecting;
    lastAttempt_ = millis();
    lastSkipReason_ = wgate::SkipNone;
    // v2.3: `bytes=` lets the drain be reconciled against LS byte-for-byte,
    // and `vbat=` puts the gate input next to the outcome in one line.
    Serial.printf("[UPLOAD] cycle start; %u file(s) pending bytes=%u heap_free=%u "
                  "vbat=%.2fV gate=open chg=%u\n",
                  (unsigned)pending.size(), (unsigned)pendingBytes,
                  (unsigned)ESP.getFreeHeap(),
                  (double)trackerLastVbat(), (unsigned)charging_);

    // Plant the RTC marker BEFORE turning on the radio. If we brown-out
    // during connect, the next boot will see wifiInFlight=1 in the log
    // and we can correlate the reset reason to the PA current spike.
    event_log::markWifiInFlight(true);
    event_log::markPhase("WIFI_CONNECT");
    {
        // v2.3: put the queue size in the persistent log too. Before this a
        // `cycle_start` line told you nothing about how much work was queued,
        // so a partial drain could not be identified without `LS`.
        char startMsg[48];
        snprintf(startMsg, sizeof(startMsg), "cycle_start files=%u bytes=%u",
                 (unsigned)pending.size(), (unsigned)pendingBytes);
        event_log::appendEvent("WIFI", startMsg);
    }

    if (!connectWifi()) {
        ++failedCount_;
        busy_ = false;
        phase_ = (uint8_t)Phase::Idle;
        event_log::markWifiInFlight(false);
        // v2.3: record the driver status of the last attempt. `status=6`
        // (DISCONNECTED) means no association happened at all; 4 is a refused
        // association. Distinguishing "AP absent" from "wrong password" from
        // the persistent log saves a whole debug cycle next time.
        char failMsg[48];
        snprintf(failMsg, sizeof(failMsg), "connect_fail attempts=%u last_status=%d(%s)",
                 (unsigned)connectAttempts_, lastConnectStatus_,
                 wgate::statusName(lastConnectStatus_));
        event_log::appendEvent("WIFI", failMsg);
        Serial.printf("[WIFI] no network; %u attempt(s) this cycle last_status=%d(%s)\n",
                      (unsigned)connectAttempts_, lastConnectStatus_,
                      wgate::statusName(lastConnectStatus_));
        return 0;
    }

    uint32_t ok = 0;
    size_t   bytesSent  = 0;
    uint32_t totalChunks = 0;
    const uint32_t cycleT0 = millis();
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
            bytesSent   += p.sizeBytes;
            totalChunks += lastChunks_;
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
    const uint32_t cycleMs = (uint32_t)(millis() - cycleT0);
    lastCycleMs_ = cycleMs;
    lastCycleBytes_ = bytesSent;
    char done[64];
    snprintf(done, sizeof(done), "cycle_done ok=%u/%u bytes=%u chunks=%u ms=%u",
             (unsigned)ok, (unsigned)pending.size(), (unsigned)bytesSent,
             (unsigned)totalChunks, (unsigned)cycleMs);
    event_log::appendEvent("WIFI", done);
    Serial.printf("[UPLOAD] %s heap_free=%u\n", done, (unsigned)ESP.getFreeHeap());
    return ok;
}
