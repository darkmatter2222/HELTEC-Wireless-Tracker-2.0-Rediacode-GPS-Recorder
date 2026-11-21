// =============================================================================
// Wi-Fi uploader decision helpers (v2.3) — header-only, zero dependencies.
//
// Everything here is a pure function of its arguments so the exact same code
// can be compiled into the firmware AND into the host-side Unity suites
// (`test/test_wifi_gate_native`). The suites include this header directly
// rather than re-declaring the logic, so a test can never drift from the code
// that actually runs on the device.
//
// Extracted from wifi_uploader.cpp during the v2.3 upload-blackout triage:
// the field log showed ~73 `skip vbat=` cycles against 19 connect attempts and
// zero `cycle_done`, which was impossible to interpret from a single boolean
// gate. The decisions are now explicit, latched, and unit-tested.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace wgate {

// ---------------------------------------------------------------------------
// VBAT gate with hysteresis
// ---------------------------------------------------------------------------
// Schmitt-style latch for the Wi-Fi PA battery gate.
//
//   vbatV       last measured pack voltage in volts; <= 0 means "ADC has not
//               sampled yet" (g_lastVbat boots at -1.0) -> do not block.
//   gateWasOpen the latched state from the previous cycle.
//   onV         release level: a closed gate opens at >= onV.
//   offV        re-block level: an open gate closes again at <= offV.
//
// Why hysteresis: with a single 3.60 V threshold a pack resting right at the
// boundary alternates open/closed every 60 s cycle (observed: 3.58, 3.59, 3.60,
// 3.59 across four consecutive cycles while charging). Once the PA has proven
// it can come up at this charge level, the same level sustains it, so the
// re-block point sits a few tens of mV below the release point.
//
// Note on the charge current: a *charging* cell reads HIGHER than its resting
// OCV because of the IR drop, so "charging" is deliberately NOT used to widen
// the gate — the number is already optimistic while current is flowing.
inline bool vbatGateOk(float vbatV, bool gateWasOpen, float onV, float offV) {
    if (!(vbatV > 0.0f)) return true;             // unmeasured -> don't block
    if (gateWasOpen)     return !(vbatV <= offV); // stay open until the floor
    return vbatV >= onV;                          // closed until the release level
}

// ---------------------------------------------------------------------------
// Charge / discharge trend
// ---------------------------------------------------------------------------
// True when `curV` is at least `minDeltaV` above `prevV`. Both readings must be
// valid (> 0) or the answer is "unknown" == false. `minDeltaV` sits above the
// 12-bit ADC noise floor so a flat cell reads as flat, not as a slow climb.
// The 1e-4 epsilon absorbs the binary representation error of the subtraction
// (3.01f - 3.00f evaluates to 0.0099999... which would otherwise read as "no
// rise" at exactly the dead band).
inline bool voltageRising(float prevV, float curV, float minDeltaV) {
    if (!(prevV > 0.0f) || !(curV > 0.0f)) return false;
    return (curV - prevV) + 0.0001f >= minDeltaV;
}

// Latched "is the pack charging" state machine, evaluated once per upload
// cycle. Returns the new run length of consecutive rising samples; `charging`
// is true once `runNeeded` consecutive rises have been seen.
inline uint32_t chargeRun(uint32_t prevRun, bool rising) {
    return rising ? (prevRun + 1u) : 0u;
}

// ---------------------------------------------------------------------------
// Per-cycle Wi-Fi connect budget
// ---------------------------------------------------------------------------
// Fixed delays around each association attempt. These are the single source of
// truth: connectWifi() paces itself with these constants, and the unit test
// asserts the total budget against the field-measured cycle deltas.
constexpr uint32_t CONNECT_SETTLE_MS    = 100;  // after WiFi.disconnect(), before begin()
constexpr uint32_t INTER_SSID_SETTLE_MS = 300;  // between two SSID attempts

// Worst-case milliseconds spent inside connectWifi() when every configured
// profile is tried and every one of them times out. Each attempt is
// [timeout window] + [300 ms settle], and the whole sequence is preceded by
// the 100 ms pre-begin settle:
//   2 profiles @ 25000 ms -> 100 + 2*(25000+300) = 50700 ms
//   2 profiles @  5000 ms -> 100 + 2*( 5000+300) = 10700 ms
// The 2026-11-21 field log showed 10.82-11.57 s between `cycle_start` and
// `connect_fail`, which identifies a 5000 ms per-SSID timeout with two
// profiles configured (the residual 119-870 ms is printf plus two 200 ms
// poll quanta).
inline uint32_t worstCaseConnectMs(uint32_t timeoutMs, int profiles) {
    if (profiles <= 0) return 0;
    return CONNECT_SETTLE_MS +
           (uint32_t)profiles * (timeoutMs + INTER_SSID_SETTLE_MS);
}

// ---------------------------------------------------------------------------
// Chunked POST arithmetic
// ---------------------------------------------------------------------------
// Number of whole-chunk POSTs needed for a body of `fileBytes` (ceil division;
// the last chunk is short). 0 bytes -> 0 requests, matching the early return
// in uploadOne().
inline size_t chunkCount(size_t fileBytes, size_t chunkBytes) {
    if (chunkBytes == 0 || fileBytes == 0) return 0;
    return (fileBytes + chunkBytes - 1) / chunkBytes;
}

// Worst-case un-petted window for one chunked POST: the link-limited write
// phase plus the response timeout. Must stay inside cfg::TASK_WDT_TIMEOUT_S.
//   8192 B @ 400 B/s + 30000 ms -> 20480 + 30000 = 50480 ms
inline uint32_t worstCasePostMs(size_t chunkBytes, uint32_t bytesPerSec,
                                uint32_t responseTimeoutMs) {
    if (chunkBytes == 0 || bytesPerSec == 0) return responseTimeoutMs;
    const uint32_t writeMs =
        (uint32_t)(((uint64_t)chunkBytes * 1000u + bytesPerSec - 1u) / bytesPerSec);
    return writeMs + responseTimeoutMs;
}

// ---------------------------------------------------------------------------
// Retry ladder
// ---------------------------------------------------------------------------
// Multiplier applied to secrets::UPLOAD_INTERVAL_MS after `consecutiveFailures`
// failures: 1x for the first two (so a post-boot device still drains at the
// base cadence), then 2x, 4x, 8x, 16x and stays capped at 16x.
inline uint32_t backoffMultiplier(uint32_t consecutiveFailures) {
    if (consecutiveFailures <= 2) return 1u;
    uint32_t shift = consecutiveFailures - 2u;
    if (shift > 4u) shift = 4u;
    return 1u << shift;
}

// ---------------------------------------------------------------------------
// Diagnostics: Wi-Fi driver status decoding
// ---------------------------------------------------------------------------
// Arduino-esp32 `wl_interface_status_t` (libraries/WiFi/src/WiFiType.h). The
// numeric values are baked into the core, so decoding them by hand keeps the
// serial log human-readable without pulling in the WiFi library (and keeps
// this header testable on the host).
inline const char* statusName(int status) {
    switch (status) {
        case 255: return "NO_SHIELD";        // WL_NO_SHIELD
        case 0:   return "IDLE";             // WL_IDLE_STATUS
        case 1:   return "NO_SSID_AVAIL";    // WL_NO_SSID_AVAIL
        case 2:   return "SCAN_COMPLETED";   // WL_SCAN_COMPLETED
        case 3:   return "CONNECTED";         // WL_CONNECTED
        case 4:   return "CONNECT_FAILED";   // WL_CONNECT_FAILED
        case 5:   return "CONNECTION_LOST";   // WL_CONNECTION_LOST
        case 6:   return "DISCONNECTED";      // WL_DISCONNECTED
        default:  return "UNKNOWN";
    }
}

// Why the most recent cycle ended without an HTTP request. Kept as a flat
// enum so WIFISTAT can print it as `skip=<name>`.
enum SkipReason : uint8_t {
    SkipNone         = 0,
    SkipBattery      = 1,   // VBAT below the gate -> PA would brown out
    SkipNoPending    = 2,   // nothing queued; radio never opened
    SkipDisabled     = 3,   // no complete SSID+URL profile
};

inline const char* skipName(uint8_t reason) {
    switch (reason) {
        case SkipBattery:   return "battery";
        case SkipNoPending: return "no_pending";
        case SkipDisabled:  return "disabled";
        default:            return "none";
    }
}

} // namespace wgate
