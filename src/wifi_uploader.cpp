#include "wifi_uploader.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
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

} // namespace


void WifiUploader::begin(SessionStore* store) {
    store_ = store;

    // No SSID == feature disabled; this is the default for `secrets.h.example`
    // so the firmware still builds for users who haven't set up Wi-Fi yet.
    if (!secrets::WIFI_SSID || secrets::WIFI_SSID[0] == '\0' ||
        !secrets::INGEST_URL || secrets::INGEST_URL[0] == '\0') {
        enabled_ = false;
        Serial.println("[WIFI] disabled (no SSID/INGEST_URL in secrets.h)");
        return;
    }
    enabled_ = true;

    // Park the radio so the cadenced task starts from a known state.
    WiFi.mode(WIFI_OFF);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);

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

    Serial.printf("[WIFI] uploader armed; ssid='%s' url='%s' interval=%us trackerId=%s\n",
                  secrets::WIFI_SSID, secrets::INGEST_URL,
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
    // 15s post-boot grace so BLE/GPS finish coming up before we light Wi-Fi.
    vTaskDelay(pdMS_TO_TICKS(15000));
    // Backoff state for repeated connect failures (e.g. when out of Wi-Fi
    // range for hours). Without backoff we churn the radio and PA every
    // 60 s, which on a marginal power supply triggers a brown-out reset.
    uint32_t consecutiveFailures = 0;
    for (;;) {
        const uint32_t okBefore = uploadedCount_;
        const uint32_t failBefore = failedCount_;
        runOnce();
        if (failedCount_ > failBefore && uploadedCount_ == okBefore) {
            ++consecutiveFailures;
        } else if (uploadedCount_ > okBefore) {
            consecutiveFailures = 0;
        }
        // Exponential backoff: 1x, 2x, 4x, 8x, 16x cadence (cap ~16 min).
        // Caps at 5 to avoid silly long sleeps when the AP eventually returns.
        uint32_t mult = 1u;
        if (consecutiveFailures > 0) {
            uint32_t shift = consecutiveFailures > 5 ? 5 : (consecutiveFailures - 1);
            mult = 1u << shift;
        }
        const uint32_t sleepMs = secrets::UPLOAD_INTERVAL_MS * mult;
        if (mult > 1) {
            Serial.printf("[WIFI] backoff: %u consecutive failures, next attempt in %us\n",
                          (unsigned)consecutiveFailures, (unsigned)(sleepMs / 1000));
        }
        // Sleep until either the cadence elapses or requestNow() pokes us.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleepMs));
    }
}


bool WifiUploader::connectWifi() {
    Serial.printf("[WIFI] connecting to '%s'...\n", secrets::WIFI_SSID);

    // Hard reset radio state. After repeated connect failures (e.g. roaming
    // out of and back into Wi-Fi range during a bike ride) the driver can
    // get stuck unless we fully bounce the mode.
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(150));
    WiFi.mode(WIFI_STA);

    // Cap TX power at 8.5 dBm before begin(). v0.4.1 used 11 dBm but
    // brown-outs continued on battery, so we drop another notch. 8.5 dBm
    // is still ample for any indoor home AP at <30 m and cuts the PA peak
    // current roughly in half again vs. 11 dBm.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    // CRITICAL (v0.4.3): leave Wi-Fi modem sleep ENABLED. With BT (NimBLE)
    // running simultaneously, the ESP32 Wi-Fi/BT coex driver calls abort()
    // if modem sleep is off:
    //   "Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled"
    // The v0.4.1 setSleep(false) call (intended to stabilize connect) was
    // the actual root cause of the boot loop, not brown-outs.
    WiFi.begin(secrets::WIFI_SSID, secrets::WIFI_PASSWORD);

    const uint32_t deadline = millis() + secrets::WIFI_CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && (int32_t)(deadline - millis()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] connect timeout (status=%d)\n", (int)WiFi.status());
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    Serial.printf("[WIFI] connected ip=%s rssi=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}


void WifiUploader::disconnectWifi() {
    event_log::markPhase("WIFI_DISCO_IN");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    event_log::markPhase("WIFI_DISCO_OUT");
}


bool WifiUploader::uploadOne(const String& filename, const String& sessionId, size_t expectedBytes) {
    Serial.printf("[UPLOAD] %s (file=%s): file_bytes=%u heap_free=%u\n",
                  sessionId.c_str(), filename.c_str(), (unsigned)expectedBytes,
                  (unsigned)ESP.getFreeHeap());

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
    if (!http.begin(secrets::INGEST_URL)) {
        Serial.printf("[UPLOAD] %s: http.begin('%s') failed\n",
                      sessionId.c_str(), secrets::INGEST_URL);
        store_->closeSessionStream();
        return false;
    }
    http.setTimeout(30000);   // larger sessions need more time to stream
    http.addHeader("Content-Type", "text/csv");
    http.addHeader("X-Session-Id", sessionId);
    http.addHeader("X-Tracker-Id", chipIdString());
    http.addHeader("X-Firmware",   cfg::FW_VERSION);
    if (secrets::INGEST_TOKEN && secrets::INGEST_TOKEN[0] != '\0') {
        http.addHeader("Authorization", String("Bearer ") + secrets::INGEST_TOKEN);
    }

    const uint32_t t0 = millis();
    int code;
    if (fileStream) {
        // Stream path: no heap allocation for body.
        code = http.sendRequest("POST", fileStream, fileSize);
    } else {
        code = http.POST((uint8_t*)body.c_str(), body.length());
    }
    const String resp = (code > 0) ? http.getString() : String();
    http.end();
    store_->closeSessionStream();
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
        event_log::markWifiInFlight(false);
        event_log::appendEvent("WIFI", "connect_fail");
        return 0;
    }

    uint32_t ok = 0;
    for (const auto& p : pending) {
        event_log::markPhase("WIFI_POST");
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
    }

    disconnectWifi();
    busy_ = false;
    event_log::markWifiInFlight(false);
    char done[48];
    snprintf(done, sizeof(done), "cycle_done ok=%u/%u",
             (unsigned)ok, (unsigned)pending.size());
    event_log::appendEvent("WIFI", done);
    Serial.printf("[UPLOAD] cycle done; ok=%u/%u heap_free=%u\n",
                  (unsigned)ok, (unsigned)pending.size(), (unsigned)ESP.getFreeHeap());
    return ok;
}
