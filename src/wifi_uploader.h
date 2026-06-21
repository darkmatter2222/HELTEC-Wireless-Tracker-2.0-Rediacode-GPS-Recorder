// =============================================================================
// HTIT-Tracker Wi-Fi uploader.
//
// On a fixed cadence (secrets::UPLOAD_INTERVAL_MS) this module:
//   1. If no Wi-Fi profile (home or hotspot) is configured, does nothing.
//   2. If there are no completed sessions to upload, does nothing.
//   3. Otherwise tries the HOME network first, then REMOTE (hotspot) as a
//      fallback. POSTs each pending session CSV to the endpoint associated
//      with whichever network connected. On HTTP 2xx removes the session
//      file from LittleFS.
//   4. Disconnects Wi-Fi when finished to save power and reduce BLE
//      coexistence noise.
//
// Dual-network flow (v0.9.0):
//   Home profile:   secrets::WIFI_SSID  + WIFI_PASSWORD  -> INGEST_URL
//   Remote profile: secrets::WIFI_SSID2 + WIFI_PASSWORD2 -> INGEST_URL2
//                   + optional HTTP Basic Auth (INGEST_USER / INGEST_PASS)
//                   + HTTPS supported (TLS cert verification skipped)
//
// Public surface is intentionally tiny: begin() once in setup(), tick()
// every loop iteration. Work runs on a dedicated FreeRTOS task (core 0).
// =============================================================================
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <vector>

class SessionStore;

class WifiUploader {
public:
    // Granular phase reporting for the UI. Replaces the previous boolean
    // "busy" which couldn't distinguish a 12s connect attempt from an
    // active POST.
    enum class Phase : uint8_t {
        Disabled = 0,    // no SSID/URL configured
        Idle,            // waiting for next cadence tick
        Backoff,         // waiting through exponential backoff
        Connecting,      // associating with AP
        Posting,         // streaming a file to the server
        Disconnecting,   // tearing the radio back down
    };

    // Which network is currently (or was last) used for uploading.
    enum class ActiveNet : uint8_t {
        None   = 0,  // not connected / never tried
        Home   = 1,  // home Wi-Fi -> INGEST_URL (direct LAN)
        Remote = 2,  // mobile hotspot -> INGEST_URL2 (internet)
    };

    void begin(SessionStore* store);

    // Optional callback invoked (from the wifi_up task, core 0) after each
    // successful HTTP 2xx file upload. Stored atomically; safe to set once
    // from setup() before the task starts or any time thereafter.
    // The callback MUST be fast and non-blocking (no file I/O, no serial).
    using UploadSuccessCb = std::function<void()>;
    void setUploadSuccessCb(UploadSuccessCb cb) { uploadSuccessCb_ = cb; }

    // Backwards-compatible no-op. The actual work runs on a dedicated
    // FreeRTOS task so the main Arduino loop is never blocked by Wi-Fi
    // connects or HTTP POSTs.
    void tick() {}

    // Force an upload cycle as soon as possible. Safe to call from any task.
    void requestNow();

    // Run one full upload cycle synchronously on the calling task. Blocks.
    // Use sparingly (e.g. from the SYNC serial command).
    uint32_t runOnce();

    // Diagnostic accessors. Reads/writes of these 32-bit fields are atomic
    // on Xtensa, so no lock is needed for diagnostics.
    bool      enabled()        const { return enabled_; }
    bool      busy()           const { return busy_; }
    Phase     phase()          const { return (Phase)phase_; }
    uint32_t  consecutiveFailures() const { return consecutiveFailures_; }
    uint32_t  uploadedCount()  const { return uploadedCount_; }
    uint32_t  failedCount()    const { return failedCount_; }
    uint32_t  lastAttemptMs()  const { return lastAttempt_; }
    uint32_t  lastSuccessMs()  const { return lastSuccess_; }
    // millis() at which the next upload cycle is scheduled (0 if unknown).
    uint32_t  nextAttemptMs()  const { return nextAttempt_; }
    int       lastHttpStatus() const { return lastHttpStatus_; }
    // Which network was used during the last (or current) upload cycle.
    ActiveNet activeNet()      const { return (ActiveNet)activeNet_; }

private:
    static void taskTrampoline(void* arg);
    void taskLoop();

    bool connectWifi();
    void disconnectWifi();
    bool uploadOne(const String& filename, const String& sessionId, size_t expectedBytes);

    // v1.0.3: Chunked upload helpers. Used by uploadOne() when the pending
    // file exceeds cfg::UPLOAD_LARGE_FILE_THRESHOLD. Splits the CSV into
    // chunks of cfg::UPLOAD_CHUNK_ROWS rows and POSTs each chunk separately
    // so the WDT can be petted between chunks. The API's (sessionId,
    // timestampMs) unique index makes re-sent rows on retry harmless.
    bool uploadChunked(const String& sessionId, const char* url, Stream* fileStream);
    bool postBody(const String& sessionId, const char* url, const String& body);

    SessionStore* store_          = nullptr;
    TaskHandle_t  task_           = nullptr;
    volatile bool enabled_        = false;
    volatile bool busy_           = false;
    volatile uint8_t  phase_         = 0;   // Phase enum
    volatile uint32_t consecutiveFailures_ = 0;
    volatile uint32_t lastAttempt_   = 0;
    volatile uint32_t lastSuccess_   = 0;
    volatile uint32_t nextAttempt_   = 0;
    volatile uint32_t uploadedCount_  = 0;
    volatile uint32_t failedCount_    = 0;
    volatile int      lastHttpStatus_ = 0;
    volatile uint8_t  activeNet_      = 0;  // ActiveNet enum

    UploadSuccessCb   uploadSuccessCb_;

    // Set by connectWifi() on success; cleared by disconnectWifi().
    // Only accessed from the wifi_up task (no lock needed).
    const char* activeIngestUrl_  = nullptr;
    const char* activeIngestUser_ = nullptr;
    const char* activeIngestPass_ = nullptr;
};
