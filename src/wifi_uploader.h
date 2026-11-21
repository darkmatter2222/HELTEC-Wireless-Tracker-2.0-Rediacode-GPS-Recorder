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
#include <vector>

#include "wifi_gate.h"

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

    // Backwards-compatible no-op. The actual work runs on a dedicated
    // FreeRTOS task so the main Arduino loop is never blocked by Wi-Fi
    // connects or HTTP POSTs.
    void tick() {}

    // Force an upload cycle as soon as possible. Safe to call from any task.
    // Also clears the on-screen countdown so the STORAGE screen shows the
    // cycle as imminent instead of the stale pre-poke deadline (v2.3).
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

    // ---- v2.3 cycle diagnostics -------------------------------------------
    // These exist because the v2.2 field log could not distinguish "the gate
    // blocked us" from "the AP was unreachable" without cross-referencing
    // lastAttempt=0 against the raw event log.
    // Latched VBAT gate state (true = PA allowed to run).
    bool      vbatGateOpen()    const { return vbatGateOpen_; }
    // Why the last cycle ended with no HTTP request (wgate::SkipReason).
    uint8_t   lastSkipReason()  const { return lastSkipReason_; }
    // Millivolts of the VBAT sample that produced that decision (-1 = none).
    int       lastSkipVbatMv()  const { return lastSkipMv_; }
    // Number of cycles that ended early on the battery gate.
    uint32_t  skippedByBattery() const { return skippedCount_; }
    // 1 while the pack is measurably charging (consecutive rising samples).
    uint8_t   charging()         const { return charging_; }
    // Association attempts made so far, and duration of the most recent one.
    uint32_t  connectAttempts()  const { return connectAttempts_; }
    uint32_t  lastConnectMs()    const { return lastConnectMs_; }
    // Chunked-POST bookkeeping for the most recent file.
    uint32_t  lastChunks()       const { return lastChunks_; }
    uint32_t  lastCycleMs()      const { return lastCycleMs_; }
    size_t    lastCycleBytes()   const { return lastCycleBytes_; }
    // Wi-Fi driver status code of the last failed association (WL_* enum).
    int       lastConnectStatus() const { return lastConnectStatus_; }

private:
    static void taskTrampoline(void* arg);
    void taskLoop();

    bool connectWifi();
    void disconnectWifi();
    bool uploadOne(const String& filename, const String& sessionId, size_t expectedBytes);
    // One association attempt against a single SSID. Fills *elapsedMs with the
    // measured duration and *statusCode with the driver status on failure.
    bool tryConnect(const char* ssid, const char* pass,
                    uint32_t* elapsedMs, int* statusCode);
    // Evaluates + latches the VBAT hysteresis gate for this cycle.
    bool vbatGateAllows();

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

    // ---- v2.3 diagnostics (written by the wifi_up task, read from loop) ----
    volatile bool     vbatGateOpen_      = false;
    volatile uint8_t  lastSkipReason_    = wgate::SkipNone;
    volatile int      lastSkipMv_        = -1;
    volatile uint32_t skippedCount_      = 0;
    volatile uint8_t  charging_          = 0;
    volatile uint32_t connectAttempts_   = 0;
    volatile uint32_t lastConnectMs_     = 0;
    volatile uint32_t lastChunks_        = 0;
    volatile uint32_t lastCycleMs_       = 0;
    volatile size_t   lastCycleBytes_    = 0;
    volatile int      lastConnectStatus_ = 0;
    // Charge-trend tracker: previous sample + run length of rising samples.
    float             vbatPrev_          = -1.0f;
    uint32_t          chargeRun_         = 0;

    // Set by connectWifi() on success; cleared by disconnectWifi().
    // Only accessed from the wifi_up task (no lock needed).
    const char* activeIngestUrl_  = nullptr;
    const char* activeIngestUser_ = nullptr;
    const char* activeIngestPass_ = nullptr;
};
