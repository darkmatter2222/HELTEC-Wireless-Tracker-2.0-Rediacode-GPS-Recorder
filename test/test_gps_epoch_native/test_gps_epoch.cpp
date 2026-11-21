// Unit tests for the GPS UTC anchor / projection helpers in src/gps_epoch.h.
//
// The real header is included (not a copy), so the assertions apply to the code
// that runs on the device. Run with: pio test -e native

#include <unity.h>
#include <stdint.h>

#include "../../src/gps_epoch.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// composeUtcMs: wall-clock fields -> epoch ms
// ---------------------------------------------------------------------------
void test_compose_epoch_zero(void) {
    // 1970-01-01T00:00:00Z is day 0.
    TEST_ASSERT_EQUAL_UINT64(0ULL, gepoch::composeUtcMs(0, 0, 0, 0, 0));
    // One centisecond is exactly 10 ms, and never a rounding artefact.
    TEST_ASSERT_EQUAL_UINT64(10ULL, gepoch::composeUtcMs(0, 0, 0, 0, 1));
    TEST_ASSERT_EQUAL_UINT64(570ULL, gepoch::composeUtcMs(0, 0, 0, 0, 57));
    TEST_ASSERT_EQUAL_UINT64(990ULL, gepoch::composeUtcMs(0, 0, 0, 0, 99));
    // Field weights: 1 h = 3600000, 1 min = 60000, 1 s = 1000.
    TEST_ASSERT_EQUAL_UINT64(3600000ULL, gepoch::composeUtcMs(0, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(60000ULL,   gepoch::composeUtcMs(0, 0, 1, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(1000ULL,    gepoch::composeUtcMs(0, 0, 0, 1, 0));
}

void test_compose_min_valid_timestamp(void) {
    // The gate value must be exactly 2020-01-01T00:00:00Z: 18 262 days.
    TEST_ASSERT_EQUAL_UINT64(1577836800000ULL,
                             gepoch::composeUtcMs(18262, 0, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(gepoch::MIN_VALID_TS_MS,
                             gepoch::composeUtcMs(18262, 0, 0, 0, 0));
}

void test_compose_field_values(void) {
    // 2026-11-21T21:09:04.000Z, the session's own first-fix style value.
    TEST_ASSERT_EQUAL_UINT64(1795295344000ULL,
                             gepoch::composeUtcMs(20778, 21, 9, 4, 0));
    // Same instant with centiseconds appended -> +570 ms for cs=57.
    TEST_ASSERT_EQUAL_UINT64(1795295344570ULL,
                             gepoch::composeUtcMs(20778, 21, 9, 4, 57));
    // One more day is exactly 86 400 000 ms (no DST in UTC).
    TEST_ASSERT_EQUAL_UINT64(1795295344000ULL + 86400000ULL,
                             gepoch::composeUtcMs(20779, 21, 9, 4, 0));
    // End-of-day rollover: 23:59:59.99 + 10 ms is exactly midnight of the
    // next day -- the day-id boundary the file rotation depends on.
    TEST_ASSERT_EQUAL_UINT64(gepoch::composeUtcMs(20779, 0, 0, 0, 0),
                             gepoch::composeUtcMs(20778, 23, 59, 59, 99) + 10ULL);
    // Day 20778 midnight, derived from the stamped value above.
    TEST_ASSERT_EQUAL_UINT64(1795219200000ULL, gepoch::composeUtcMs(20778, 0, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(1795219200000ULL + 86400000ULL,
                             gepoch::composeUtcMs(20779, 0, 0, 0, 0));
}

void test_compose_is_monotonic_per_field(void) {
    const uint64_t a = gepoch::composeUtcMs(20778, 21, 9, 4, 0);
    TEST_ASSERT_TRUE(gepoch::composeUtcMs(20778, 21, 9, 5, 0) > a);
    TEST_ASSERT_TRUE(gepoch::composeUtcMs(20778, 21, 10, 4, 0) > a);
    TEST_ASSERT_TRUE(gepoch::composeUtcMs(20778, 22, 9, 4, 0) > a);
    TEST_ASSERT_TRUE(gepoch::composeUtcMs(20779, 21, 9, 4, 0) > a);
    TEST_ASSERT_TRUE(gepoch::composeUtcMs(20778, 21, 9, 4, 1) > a);
}

// ---------------------------------------------------------------------------
// shouldReanchor: the fresh-fix + pacing gate
// ---------------------------------------------------------------------------
void test_reanchor_first_anchor_always_accepted(void) {
    // lastSyncMs == 0 means "never anchored" -> accept immediately.
    TEST_ASSERT_TRUE(gepoch::shouldReanchor(true, 123, 0u, 5000u));
    TEST_ASSERT_TRUE(gepoch::shouldReanchor(true, 0,   0u, 0u));
}

void test_reanchor_requires_fresh_sentence(void) {
    // Age exactly at the ceiling is stale.
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(true, gepoch::FRESH_AGE_MS, 0u, 1000u));
    TEST_ASSERT_TRUE (gepoch::shouldReanchor(true, gepoch::FRESH_AGE_MS - 1u, 0u, 1000u));
    // No UTC at all: never re-anchor, regardless of the other inputs.
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(false, 0, 0u, 1000u));
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(false, 0, 42000u, 99000u));
}

void test_reanchor_paces_at_thirty_seconds(void) {
    const uint32_t last = 100000u;
    // Just below the interval -> hold.
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(true, 10, last,
                                             last + gepoch::REANCHOR_INTERVAL_MS - 1u));
    // Exactly the interval -> accept.
    TEST_ASSERT_TRUE (gepoch::shouldReanchor(true, 10, last,
                                             last + gepoch::REANCHOR_INTERVAL_MS));
    TEST_ASSERT_TRUE (gepoch::shouldReanchor(true, 10, last, last + 999999u));
    // The delta is unsigned (the firmware's `(now - lastUtcSyncMs_)` form), so a
    // clock that read backwards still produces a huge positive delta and accepts.
    // That is the desired one-shot behaviour after a millis() wrap: at most one
    // extra anchor, then the pacing resumes.
    TEST_ASSERT_TRUE (gepoch::shouldReanchor(true, 10, last, last - 1u));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, (uint32_t)((last - 1u) - last));
}

void test_reanchor_zero_stamp_reads_as_never_anchored(void) {
    // Documented sentinel: lastUtcSyncMs_ starts at 0 and means "no anchor yet",
    // which is why the first fix always lands regardless of the elapsed time.
    TEST_ASSERT_TRUE(gepoch::shouldReanchor(true, 1, 0u, 0u));
    TEST_ASSERT_TRUE(gepoch::shouldReanchor(true, 1, 0u, 7u));
    // The first real anchor must therefore be stamped with a non-zero millis(),
    // which it always is in practice (the first fix arrives ~7 s after boot).
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(true, 1, 7400u, 7401u));
}

void test_reanchor_pace_chain(void) {
    // Chain: 1 Hz polls across the first minute of a session, as on the device,
    // starting from the first fix at millis()=8000. Anchors must land at
    // 8000, 38000 and 68000 and nowhere in between.
    const uint32_t t0 = 8000u;
    uint32_t last = 0u;                        // sentinel: never anchored
    int anchors = 0;
    int anchorAt[4] = { 0, 0, 0, 0 };
    for (uint32_t s = 0; s <= 60; ++s) {
        const uint32_t now = t0 + s * 1000u;
        if (gepoch::shouldReanchor(true, 5, last, now)) {
            anchorAt[anchors < 4 ? anchors : 3] = (int)s;
            ++anchors;
            last = now;                        // the firmware stamps lastSyncMs = now
        }
    }
    TEST_ASSERT_EQUAL_INT(3, anchors);
    TEST_ASSERT_EQUAL_INT(0, anchorAt[0]);
    TEST_ASSERT_EQUAL_INT(30, anchorAt[1]);
    TEST_ASSERT_EQUAL_INT(60, anchorAt[2]);
    TEST_ASSERT_EQUAL_UINT32(t0 + 60000u, last);
}

// ---------------------------------------------------------------------------
// millisAnchorFor / project / driftMs
// ---------------------------------------------------------------------------
void test_millis_anchor_accounts_for_age(void) {
    TEST_ASSERT_EQUAL_UINT32(10000u, gepoch::millisAnchorFor(10000u, 0u));
    TEST_ASSERT_EQUAL_UINT32(9995u,  gepoch::millisAnchorFor(10000u, 5u));
    TEST_ASSERT_EQUAL_UINT32(5000u,  gepoch::millisAnchorFor(10000u, 5000u));
    // Wraparound-safe unsigned arithmetic.
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFF00u, gepoch::millisAnchorFor(0xFFFFFF05u, 5u));
}

void test_project_is_the_anchor_plus_delta(void) {
    const uint64_t A = 1795295344000ULL;
    TEST_ASSERT_EQUAL_UINT64(A, gepoch::project(A, 12345u, 12345u));       // dt = 0
    TEST_ASSERT_EQUAL_UINT64(A + 1ULL, gepoch::project(A, 12345u, 12346u));
    TEST_ASSERT_EQUAL_UINT64(A + 60000ULL, gepoch::project(A, 12345u, 72345u));
    TEST_ASSERT_EQUAL_UINT64(A + 86400000ULL,
                             gepoch::project(A, 12345u, 12345u + 86400000u));
}

void test_project_advances_through_a_gps_outage(void) {
    // The whole point of the anchor: with the NMEA parser frozen, successive
    // reads still produce strictly increasing, unique timestamps, so the
    // unique {sessionId,timestampMs} index cannot deduplicate them.
    const uint64_t A = 1795295344000ULL;
    const uint32_t a = 5000u;
    uint64_t prev = 0;
    int n = 0;
    for (uint32_t t = a; n < 60; t += 1000u) {          // 1 Hz for 60 s
        const uint64_t ts = gepoch::project(A, a, t);
        if (n > 0) TEST_ASSERT_TRUE(ts > prev);
        prev = ts;
        ++n;
    }
    TEST_ASSERT_EQUAL_UINT64(A + 59000ULL, prev);
}

void test_project_across_millis_wraparound(void) {
    const uint64_t A = 1795295344000ULL;
    const uint32_t a = 0xFFFFFFF0u;                     // 16 ms before the wrap
    TEST_ASSERT_EQUAL_UINT64(A + 16ULL, gepoch::project(A, a, 0x00000000u));
    TEST_ASSERT_EQUAL_UINT64(A + 1016ULL, gepoch::project(A, a, 0x000003E8u));
    // Exactly 2^32 ms later the delta is 0 again -- the firmware re-anchors from
    // a fresh sentence long before that (30 s cadence), so no drift can hide.
    TEST_ASSERT_EQUAL_UINT64(A, gepoch::project(A, a, a));
}

void test_drift_ms_reads_the_old_pair(void) {
    // Projection of the old pair at the sentence instant is A+60000.
    const uint64_t A = 1795295344000ULL;
    TEST_ASSERT_EQUAL_INT64(0, gepoch::driftMs(A + 60000ULL, A, 5000u, 65000u));
    TEST_ASSERT_EQUAL_INT64(3, gepoch::driftMs(A + 60003ULL, A, 5000u, 65000u));
    TEST_ASSERT_EQUAL_INT64(-7, gepoch::driftMs(A + 59993ULL, A, 5000u, 65000u));
    // The projection is evaluated at the *sentence* instant, not at the parse
    // instant, so a perfect oscillator reads 0 instead of the -age the pre-2.3
    // code always printed. Feeding it the parse instant reproduces that offset.
    TEST_ASSERT_EQUAL_INT64(-4, gepoch::driftMs(A + 60000ULL, A, 5000u, 65004u));
    // Before the very first anchor both fields are 0.
    TEST_ASSERT_EQUAL_INT64(0, gepoch::driftMs(0, 0, 0u, 0u));
}

// ---------------------------------------------------------------------------
// MIN_VALID_TS_MS gate
// ---------------------------------------------------------------------------
void test_ts_gate_boundaries(void) {
    TEST_ASSERT_EQUAL_UINT64(1577836800000ULL, gepoch::MIN_VALID_TS_MS);
    TEST_ASSERT_FALSE(gepoch::tsWithinGate(0, gepoch::MIN_VALID_TS_MS));               // no anchor
    TEST_ASSERT_FALSE(gepoch::tsWithinGate(1777, gepoch::MIN_VALID_TS_MS));            // millis()-since-boot
    TEST_ASSERT_FALSE(gepoch::tsWithinGate(gepoch::MIN_VALID_TS_MS - 1, gepoch::MIN_VALID_TS_MS));
    TEST_ASSERT_TRUE (gepoch::tsWithinGate(gepoch::MIN_VALID_TS_MS, gepoch::MIN_VALID_TS_MS));
    TEST_ASSERT_TRUE (gepoch::tsWithinGate(1795295344000ULL, gepoch::MIN_VALID_TS_MS));
}

// ---------------------------------------------------------------------------
// Full chain: anchor -> project -> re-anchor, as bestEpochMs() runs it
// ---------------------------------------------------------------------------
void test_anchor_project_reanchor_chain(void) {
    const uint64_t A0 = 1795295344000ULL;      // first sentence, age 4 ms
    const uint32_t now = 5000u;

    // 1) cold: never anchored -> accept, and stamp the pair.
    TEST_ASSERT_TRUE(gepoch::shouldReanchor(true, 4, 0u, now));
    const uint32_t sentMs = gepoch::millisAnchorFor(now, 4);
    TEST_ASSERT_EQUAL_UINT32(4996u, sentMs);
    TEST_ASSERT_EQUAL_UINT64(A0 + 4ULL, gepoch::project(A0, sentMs, now));   // +age

    // 2) 1 Hz polls, still fresh: paced, but the projection keeps moving.
    for (uint32_t i = 1; i < 30; ++i) {
        TEST_ASSERT_FALSE(gepoch::shouldReanchor(true, 4, now, now + i * 1000u));
        TEST_ASSERT_EQUAL_UINT64(A0 + i * 1000ULL + 4ULL,
                                 gepoch::project(A0, sentMs, now + i * 1000u));
    }

    // 3) at t+30 s the gate opens again; a 2 ms-fast oscillator shows +2 drift
    //    measured against the OLD pair.
    const uint32_t t30 = now + 30000u;                     // 35000
    TEST_ASSERT_TRUE(gepoch::shouldReanchor(true, 4, now, t30));
    const uint32_t sentMs1 = gepoch::millisAnchorFor(t30, 4);             // 34996
    TEST_ASSERT_EQUAL_UINT32(34996u, sentMs1);
    const uint64_t A1 = A0 + 30002ULL;                     // 30000 elapsed + 2 fast
    TEST_ASSERT_EQUAL_INT64(2, gepoch::driftMs(A1, A0, sentMs, sentMs1));
    // Perfect sentence for the same instant reads 0 drift.
    TEST_ASSERT_EQUAL_INT64(0, gepoch::driftMs(A0 + 30000ULL, A0, sentMs, sentMs1));
    TEST_ASSERT_EQUAL_UINT64(A1 + 1004ULL, gepoch::project(A1, sentMs1, 36000u));

    // 4) outage: parser frozen / no UTC -> gate refuses, projection carries on.
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(false, 4, t30, 45000u));
    TEST_ASSERT_EQUAL_UINT64(A1 + 10004ULL, gepoch::project(A1, sentMs1, 45000u));
    // Timestamps written during the outage are still post-2020, so the row gate
    // keeps them -- this is what stopped the deduplication of frozen fixes.
    TEST_ASSERT_TRUE(gepoch::tsWithinGate(gepoch::project(A1, sentMs1, 45000u),
                                          gepoch::MIN_VALID_TS_MS));

    // 5) recovery after 5 min: the stale sentence is refused until it refreshes,
    //    then the 30 s pacing has long expired so it lands at once.
    const uint32_t t330 = t30 + 300000u;                   // 335000
    TEST_ASSERT_FALSE(gepoch::shouldReanchor(true, 90000u, t30, t330));   // age stale
    TEST_ASSERT_TRUE (gepoch::shouldReanchor(true, 6u, t30, t330));
    const uint32_t sentMs2 = gepoch::millisAnchorFor(t330, 6);             // 334994
    TEST_ASSERT_EQUAL_UINT32(334994u, sentMs2);
    // 334994 - 34996 = 299998 elapsed; the same +2 oscillator error is visible.
    TEST_ASSERT_EQUAL_INT64(2, gepoch::driftMs(A1 + 300000ULL, A1, sentMs1, sentMs2));
    TEST_ASSERT_EQUAL_UINT64(A1 + 299998ULL, gepoch::project(A1, sentMs1, sentMs2));
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_compose_epoch_zero);
    RUN_TEST(test_compose_min_valid_timestamp);
    RUN_TEST(test_compose_field_values);
    RUN_TEST(test_compose_is_monotonic_per_field);

    RUN_TEST(test_reanchor_first_anchor_always_accepted);
    RUN_TEST(test_reanchor_requires_fresh_sentence);
    RUN_TEST(test_reanchor_paces_at_thirty_seconds);
    RUN_TEST(test_reanchor_zero_stamp_reads_as_never_anchored);
    RUN_TEST(test_reanchor_pace_chain);

    RUN_TEST(test_millis_anchor_accounts_for_age);
    RUN_TEST(test_project_is_the_anchor_plus_delta);
    RUN_TEST(test_project_advances_through_a_gps_outage);
    RUN_TEST(test_project_across_millis_wraparound);
    RUN_TEST(test_drift_ms_reads_the_old_pair);

    RUN_TEST(test_ts_gate_boundaries);
    RUN_TEST(test_anchor_project_reanchor_chain);

    return UNITY_END();
}
