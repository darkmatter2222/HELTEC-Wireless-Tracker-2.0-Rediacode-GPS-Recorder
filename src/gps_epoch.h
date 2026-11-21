// =============================================================================
// GPS UTC anchor / epoch projection helpers — header-only, zero deps.
//
// Included directly by gps_module.cpp AND by `test/test_gps_epoch_native`, so
// the timestamp arithmetic that every CSV row depends on is the same code the
// host asserts on.
//
// The problem this solves (see the "GPS Timestamps" lesson): before the anchor
// existed, rows written while TinyGPS++ had no fix all carried the same frozen
// timestamp and were silently deduplicated by the unique {sessionId,timestampMs}
// index. The anchor + millis() projection keeps timestamps strictly increasing
// through outages, and the fresh-fix re-anchor keeps them honest long term.
// =============================================================================
#pragma once
#include <stdint.h>

namespace gepoch {

// Pacing constants, as used by GpsModule::bestEpochMs().
constexpr uint32_t FRESH_AGE_MS          = 5000;   // NMEA sentence age ceiling
constexpr uint32_t REANCHOR_INTERVAL_MS  = 30000;  // min gap between re-anchors

// 2020-01-01T00:00:00Z. Rows below this are millis()-since-boot values written
// before the first fix; both the firmware writers and the ingest API reject
// them. Single source of truth for every `>= MIN_VALID_TS_MS` comparison.
constexpr uint64_t MIN_VALID_TS_MS = 1577836800000ULL;

// ---------------------------------------------------------------------------
// Wall-clock fields -> epoch milliseconds.
// `daysSinceEpoch` is TinyGPS++'s daysFromCivil(y, m, d) result (days since
// 1970-01-01), so this helper stays a pure composition with no calendar logic.
// ---------------------------------------------------------------------------
inline uint64_t composeUtcMs(int64_t daysSinceEpoch, int hour, int minute,
                            int second, int centisecond) {
    const int64_t secs = daysSinceEpoch * 86400LL
                       + (int64_t)hour * 3600LL
                       + (int64_t)minute * 60LL
                       + (int64_t)second;
    return (uint64_t)secs * 1000ULL + (uint64_t)centisecond * 10ULL;
}

// ---------------------------------------------------------------------------
// Re-anchor gate: a fresh fix, and not too soon after the previous one.
// `lastSyncMs == 0` means "never anchored", which always accepts.
// ---------------------------------------------------------------------------
inline bool shouldReanchor(bool hasUtc, uint32_t timeAgeMs,
                           uint32_t lastSyncMs, uint32_t nowMs) {
    if (!hasUtc) return false;
    if (timeAgeMs >= FRESH_AGE_MS) return false;
    if (lastSyncMs == 0) return true;
    return (uint32_t)(nowMs - lastSyncMs) >= REANCHOR_INTERVAL_MS;
}

// The NMEA sentence is already `ageMs` old when it is parsed, so the millis()
// value that corresponds to the anchored UTC instant sits that far in the past.
inline uint32_t millisAnchorFor(uint32_t nowMs, uint32_t ageMs) {
    return nowMs - ageMs;
}

// Project the anchor forward on the monotonic clock. Strictly increasing as
// long as millis() advances, which is what stops duplicate-row deduplication.
inline uint64_t project(uint64_t anchorMs, uint32_t anchorMillis, uint32_t nowMillis) {
    return anchorMs + (uint64_t)(uint32_t)(nowMillis - anchorMillis);
}

// Disagreement between a freshly parsed sentence and the projection that was in
// effect just before the re-anchor. All three clock values refer to the same
// instant: pass the millis value the sentence describes, i.e.
// millisAnchorFor(now, age). Positive means the projection ran slow (the fresh
// value is ahead). Computed from the OLD anchor pair, so the log line is
// meaningful -- and a perfect oscillator reads exactly 0.
inline int64_t driftMs(uint64_t freshMs, uint64_t anchorMs,
                       uint32_t anchorMillis, uint32_t sentenceMillis) {
    return (int64_t)freshMs - (int64_t)project(anchorMs, anchorMillis, sentenceMillis);
}

// The MIN_VALID_TS_MS gate (2020-01-01T00:00:00Z) rejects millis()-since-boot
// values written before the first fix. 0 means "no anchor yet" and is rejected.
inline bool tsWithinGate(uint64_t tsMs, uint64_t minValidMs) {
    return tsMs >= minValidMs;
}

} // namespace gepoch
