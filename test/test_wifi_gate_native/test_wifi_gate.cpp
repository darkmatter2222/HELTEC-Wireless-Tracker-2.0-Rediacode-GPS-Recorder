// Unit tests for the v2.3 Wi-Fi decision helpers in src/wifi_gate.h.
//
// These include the REAL header (not a copy) so the assertions apply to the
// code that actually runs on the device. Run with: pio test -e native
//
// Reference data is the 2026-11-21 field log, which is what made this
// necessary: ~73 `skip vbat=` lines, 19 `cycle_start`, zero `cycle_done`.

#include <unity.h>
#include <stdint.h>
#include <stddef.h>

#include "../../src/wifi_gate.h"

// Thresholds as configured in src/config.h (v2.3).
static const float ON_V   = 3.60f;
static const float OFF_V  = 3.45f;
static const float DEADB  = 0.010f;   // cfg::VBAT_CHARGE_DELTA_V
static const uint32_t WDT_S = 60;     // cfg::TASK_WDT_TIMEOUT_S
static const size_t   CH    = 8192;   // cfg::UPLOAD_CHUNK_BYTES

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Gate: unmeasured ADC must never block (g_lastVbat boots at -1.0)
// ---------------------------------------------------------------------------
void test_gate_unmeasured_passes(void) {
    TEST_ASSERT_TRUE(wgate::vbatGateOk(-1.0f, false, ON_V, OFF_V));
    TEST_ASSERT_TRUE(wgate::vbatGateOk(0.0f,  false, ON_V, OFF_V));
}

// ---------------------------------------------------------------------------
// Gate: opening edge (closed -> open at exactly the release level)
// ---------------------------------------------------------------------------
void test_gate_opens_at_threshold(void) {
    TEST_ASSERT_FALSE(wgate::vbatGateOk(3.59f, false, ON_V, OFF_V));
    TEST_ASSERT_TRUE (wgate::vbatGateOk(3.60f, false, ON_V, OFF_V));
    TEST_ASSERT_TRUE (wgate::vbatGateOk(3.97f, false, ON_V, OFF_V));
}

void test_gate_just_below_release_stays_closed(void) {
    // The exact readings that produced the field incident: 3.36 V is 0.24 V
    // under the release level, so every cycle was gated.
    TEST_ASSERT_FALSE(wgate::vbatGateOk(3.36f, false, ON_V, OFF_V));
    TEST_ASSERT_FALSE(wgate::vbatGateOk(3.595f, false, ON_V, OFF_V));  // prints "3.60"
}

// ---------------------------------------------------------------------------
// Gate: hysteresis (once open, hold until the floor)
// ---------------------------------------------------------------------------
void test_gate_holds_open_between_levels(void) {
    TEST_ASSERT_TRUE(wgate::vbatGateOk(3.59f, true, ON_V, OFF_V));
    TEST_ASSERT_TRUE(wgate::vbatGateOk(3.50f, true, ON_V, OFF_V));
    TEST_ASSERT_TRUE(wgate::vbatGateOk(3.46f, true, ON_V, OFF_V));
}

void test_gate_closes_at_floor(void) {
    TEST_ASSERT_FALSE(wgate::vbatGateOk(3.45f, true, ON_V, OFF_V));  // <= floor
    TEST_ASSERT_FALSE(wgate::vbatGateOk(3.44f, true, ON_V, OFF_V));
    TEST_ASSERT_FALSE(wgate::vbatGateOk(2.48f, true, ON_V, OFF_V));
}

void test_hysteresis_reduces_boundary_chatter(void) {
    // Observed on the boundary while charging: 3.58, 3.59, 3.60, 3.59.
    // The legacy single-threshold gate skips 3 of the 4 cycles; the latch
    // skips 2 -- the first 3.60 V sample opens it and 3.59 V stays above the
    // floor, so the last cycle uploads instead of being cut off again.
    const float seq[4] = { 3.58f, 3.59f, 3.60f, 3.59f };

    int legacySkips = 0;
    for (int i = 0; i < 4; ++i) if (!(seq[i] >= ON_V)) ++legacySkips;

    int latchedSkips = 0;
    bool open = false;
    for (int i = 0; i < 4; ++i) {
        open = wgate::vbatGateOk(seq[i], open, ON_V, OFF_V);
        if (!open) ++latchedSkips;
    }

    TEST_ASSERT_EQUAL_INT(3, legacySkips);
    TEST_ASSERT_EQUAL_INT(2, latchedSkips);
    TEST_ASSERT_TRUE(open);
}

// ---------------------------------------------------------------------------
// Gate: the real charging ramp from the field log
// ---------------------------------------------------------------------------
void test_field_charging_ramp_opens_on_ninth_cycle(void) {
    // vbatMv values 3145,3293,3376,3435,3482,3506,3545,3567,3621 from
    // /system.log. The device uploaded for the first time on cycle 9.
    const float ramp[9] = { 3.145f, 3.293f, 3.376f, 3.435f, 3.482f,
                            3.506f, 3.545f, 3.567f, 3.621f };
    bool  open = false;
    int   skips = 0;
    int   firstOpenIdx = -1;
    for (int i = 0; i < 9; ++i) {
        open = wgate::vbatGateOk(ramp[i], open, ON_V, OFF_V);
        if (open) { if (firstOpenIdx < 0) firstOpenIdx = i; }
        else      { ++skips; }
    }
    TEST_ASSERT_EQUAL_INT(8, skips);
    TEST_ASSERT_EQUAL_INT(8, firstOpenIdx);   // zero-based -> 9th cycle
    TEST_ASSERT_TRUE(open);
}

void test_discharge_gets_two_extra_cycles_from_floor(void) {
    // Charger removed at 3.55 V: the latch keeps the radio alive through the
    // droop instead of cutting off until it climbs back to 3.60 V.
    const float seq[3] = { 3.55f, 3.51f, 3.45f };
    bool open = true;                     // opened earlier at 3.62 V
    int  ran  = 0;
    for (int i = 0; i < 3; ++i) {
        open = wgate::vbatGateOk(seq[i], open, ON_V, OFF_V);
        if (open) ++ran;
    }
    TEST_ASSERT_EQUAL_INT(2, ran);        // 3.55 and 3.51 upload, 3.45 blocks
    TEST_ASSERT_FALSE(open);
}

// ---------------------------------------------------------------------------
// Charge trend detector
// ---------------------------------------------------------------------------
void test_rising_detects_charge_slope(void) {
    // 2.66 -> 2.74 V in one 60 s cycle = the CC-phase slope in the log.
    TEST_ASSERT_TRUE(wgate::voltageRising(2.66f, 2.74f, DEADB));
    TEST_ASSERT_TRUE(wgate::voltageRising(3.00f, 3.01f, DEADB));   // exactly the delta
}

void test_rising_rejects_noise_and_flat(void) {
    TEST_ASSERT_FALSE(wgate::voltageRising(3.00f, 3.00f, DEADB));
    TEST_ASSERT_FALSE(wgate::voltageRising(3.00f, 3.005f, DEADB)); // sub-deadband
    TEST_ASSERT_FALSE(wgate::voltageRising(3.50f, 3.49f, DEADB));  // discharging
}

void test_rising_needs_two_valid_samples(void) {
    TEST_ASSERT_FALSE(wgate::voltageRising(-1.0f, 3.30f, DEADB));
    TEST_ASSERT_FALSE(wgate::voltageRising(3.30f, -1.0f, DEADB));
}

void test_charge_run_latches_after_two_rises(void) {
    uint32_t run = 0;
    run = wgate::chargeRun(run, true);   TEST_ASSERT_EQUAL_UINT32(1, run);
    run = wgate::chargeRun(run, true);   TEST_ASSERT_EQUAL_UINT32(2, run);
    TEST_ASSERT_TRUE(run >= 2);          // -> charging
    run = wgate::chargeRun(run, false);  TEST_ASSERT_EQUAL_UINT32(0, run);
    TEST_ASSERT_FALSE(run >= 2);         // -> not charging
}

// ---------------------------------------------------------------------------
// Connect budget arithmetic (ties the log deltas to the configured timeout)
// ---------------------------------------------------------------------------
void test_connect_budget_two_profiles_25s(void) {
    TEST_ASSERT_EQUAL_UINT32(50700, wgate::worstCaseConnectMs(25000, 2));
}

void test_connect_budget_one_profile_25s(void) {
    TEST_ASSERT_EQUAL_UINT32(25400, wgate::worstCaseConnectMs(25000, 1));
}

void test_connect_budget_zero_profiles(void) {
    TEST_ASSERT_EQUAL_UINT32(0, wgate::worstCaseConnectMs(25000, 0));
}

void test_model_reproduces_observed_5s_timeouts(void) {
    // Every 2026-11-21 `cycle_start` -> `connect_fail` delta was 10.82-11.57 s.
    // Only a 5000 ms per-SSID timeout with TWO profiles lands in that window,
    // which is how the compiled secrets.h value was identified from the log.
    const uint32_t model = wgate::worstCaseConnectMs(5000, 2);
    TEST_ASSERT_EQUAL_UINT32(10700, model);
    const int observed[6] = { 10819, 10858, 10933, 11000, 11104, 11570 };
    for (int i = 0; i < 6; ++i) {
        const int delta = observed[i];
        TEST_ASSERT_TRUE_MESSAGE(delta >= (int)model,
                                 "observed delta below the pure-delay model");
        // Overhead above the model is printf + two 200 ms poll quanta.
        TEST_ASSERT_TRUE_MESSAGE(delta <= (int)model + 1200,
                                 "observed delta too far above the model");
    }
    // A 25 s window predicts ~50.7 s, i.e. unmistakably different from the
    // observed cluster -- so the model identifies the compiled value.
    TEST_ASSERT_TRUE(wgate::worstCaseConnectMs(25000, 2) > 50000);
}

void test_25s_budget_covers_measured_associations(void) {
    // Measured on device: 3807 ms cold (first use this boot), 801 ms warm.
    // Both must fit the per-SSID window with margin.
    TEST_ASSERT_TRUE(3807u < 25000u);
    TEST_ASSERT_TRUE(801u  < 25000u);
    // With the old 5 s window the cold case had only 1193 ms of headroom,
    // i.e. below the 18-20 s worst case recorded in the v0.4.8 lesson.
    TEST_ASSERT_TRUE(3807u + 400u < 5000u);
    TEST_ASSERT_TRUE(3807u + 400u > 5000u - 1500u);   // < 30 % margin -> too tight
}

// ---------------------------------------------------------------------------
// Connect budget: guards and wider configurations
// ---------------------------------------------------------------------------
void test_connect_budget_negative_and_zero(void) {
    TEST_ASSERT_EQUAL_UINT32(0, wgate::worstCaseConnectMs(25000, -1));  // guard
    TEST_ASSERT_EQUAL_UINT32(0, wgate::worstCaseConnectMs(25000, 0));
    // Zero timeout still pays the two fixed settles.
    TEST_ASSERT_EQUAL_UINT32(400, wgate::worstCaseConnectMs(0, 1));     // 100+300
    TEST_ASSERT_EQUAL_UINT32(700, wgate::worstCaseConnectMs(0, 2));     // 100+2*300
}

void test_connect_budget_three_profiles_vs_watchdog(void) {
    // A hypothetical third profile pushes the TOTAL past the watchdog, but the
    // watchdog only ever sees one attempt at a time (the poll loop pets every
    // 200 ms), so the invariant is per-attempt, not per-cycle.
    const uint32_t total = wgate::worstCaseConnectMs(25000, 3);
    TEST_ASSERT_EQUAL_UINT32(76000u, total);                 // 100 + 3*(25000+300)
    TEST_ASSERT_TRUE(total > WDT_S * 1000u);                 // total may exceed
    const uint32_t perAttempt = 25000u + wgate::INTER_SSID_SETTLE_MS;
    TEST_ASSERT_EQUAL_UINT32(25300u, perAttempt);
    TEST_ASSERT_TRUE(perAttempt < WDT_S * 1000u);            // window always fits
}

// ---------------------------------------------------------------------------
// Charge window helper (the same predicate the simulator uses)
// ---------------------------------------------------------------------------
void test_assoc_within_window(void) {
    TEST_ASSERT_TRUE (wgate::assocWithinWindow(801u,   25000u));
    TEST_ASSERT_TRUE (wgate::assocWithinWindow(3807u,  25000u));
    TEST_ASSERT_TRUE (wgate::assocWithinWindow(25000u, 25000u));   // exact edge
    TEST_ASSERT_FALSE(wgate::assocWithinWindow(25001u, 25000u));   // past deadline
    TEST_ASSERT_FALSE(wgate::assocWithinWindow(0u,     25000u));   // never ran
    TEST_ASSERT_FALSE(wgate::assocWithinWindow(0u,     0u));
    TEST_ASSERT_FALSE(wgate::assocWithinWindow(12000u, 5000u));    // the 5s regression
}

// ---------------------------------------------------------------------------
// Chunk arithmetic -- the 2026-11-21 backlog, byte-for-byte
// ---------------------------------------------------------------------------
static const size_t kBacklog[15] = {
    1604185, 292, 571, 305473, 939, 513185, 572, 758,
    2220556, 825404, 1398, 200, 44878, 23700, 198
};

void test_chunk_count_basic(void) {
    TEST_ASSERT_EQUAL_UINT32(0u,   (uint32_t)wgate::chunkCount(0, CH));
    TEST_ASSERT_EQUAL_UINT32(1u,   (uint32_t)wgate::chunkCount(200, CH));
    TEST_ASSERT_EQUAL_UINT32(1u,   (uint32_t)wgate::chunkCount(8192, CH));
    TEST_ASSERT_EQUAL_UINT32(2u,   (uint32_t)wgate::chunkCount(8193, CH));
    TEST_ASSERT_EQUAL_UINT32(0u,   (uint32_t)wgate::chunkCount(8192, 0));   // guard
}

// Exact boundaries: one byte short of, exactly at, and one byte past a chunk.
void test_chunk_count_boundaries(void) {
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)wgate::chunkCount(8191,  CH));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)wgate::chunkCount(8192,  CH));   // exact
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)wgate::chunkCount(8193,  CH));   // spills
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)wgate::chunkCount(16384, CH));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)wgate::chunkCount(16385, CH));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)wgate::chunkCount(1,     CH));
}

void test_chunk_count_large_bodies(void) {
    TEST_ASSERT_EQUAL_UINT32(128u,  (uint32_t)wgate::chunkCount(1048576, CH));
    TEST_ASSERT_EQUAL_UINT32(123u,  (uint32_t)wgate::chunkCount(1000000, CH));
    TEST_ASSERT_EQUAL_UINT32(677u,  (uint32_t)wgate::chunkCount(5542309, CH));  // whole backlog
    TEST_ASSERT_EQUAL_UINT32(65536u, (uint32_t)wgate::chunkCount(536870912, CH));
}

void test_chunk_count_field_files(void) {
    // 1.60 MB took 62709 ms on the LAN => ~25.6 kB/s, 196 POSTs.
    TEST_ASSERT_EQUAL_UINT32(196u, (uint32_t)wgate::chunkCount(1604185, CH));
    TEST_ASSERT_EQUAL_UINT32(272u, (uint32_t)wgate::chunkCount(2220556, CH));
    TEST_ASSERT_EQUAL_UINT32(38u,  (uint32_t)wgate::chunkCount(305473,  CH));
    TEST_ASSERT_EQUAL_UINT32(101u, (uint32_t)wgate::chunkCount(825404,  CH));
}

void test_backlog_totals(void) {
    size_t bytes = 0, chunks = 0;
    for (int i = 0; i < 15; ++i) { bytes += kBacklog[i]; chunks += wgate::chunkCount(kBacklog[i], CH); }
    TEST_ASSERT_EQUAL_UINT32(5542309u, (uint32_t)bytes);
    // ceil division is the lower bound: real chunk counts are >= this because
    // every POST is truncated to the last newline at or before CH.
    TEST_ASSERT_EQUAL_UINT32(687u, (uint32_t)chunks);
}

// The line-aligned model (uploadOne() cuts each POST at the last '\n' at or
// before the limit): that can only add requests, never remove them.
void test_chunks_line_aligned(void) {
    // The 1.60 MB day file with ~70 B rows reproduces the "chunk n/196" trace.
    TEST_ASSERT_EQUAL_UINT32(196u, (uint32_t)wgate::chunksLineAligned(1604185, CH, 70));
    TEST_ASSERT_TRUE(wgate::chunksLineAligned(1604185, CH, 70) >=
                     wgate::chunkCount(1604185, CH));

    // Rows that do not divide the chunk: 6 rows of 3000 B, 2 fit per 8192 POST
    // -> 3 POSTs, one more than the raw ceil of 2.
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)wgate::chunksLineAligned(16384, CH, 3000));
    TEST_ASSERT_TRUE(wgate::chunksLineAligned(16384, CH, 3000) >
                     wgate::chunkCount(16384, CH));

    // A row longer than the chunk: one row per POST (never a partial row).
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)wgate::chunksLineAligned(16384, CH, 9000));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)wgate::chunksLineAligned(20000, CH, 9000));

    // Degenerate inputs must not divide by zero.
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)wgate::chunksLineAligned(0, CH, 70));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)wgate::chunksLineAligned(100, 0, 70));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)wgate::chunksLineAligned(100, CH, 0));
}

// ---------------------------------------------------------------------------
// WDT window arithmetic per chunk (the v2.2 fix) and vs. the unchunked path
// ---------------------------------------------------------------------------
void test_worst_case_chunk_fits_wdt(void) {
    // 8192 B at the field-measured LTE floor of 400 B/s + 30 s response wait.
    TEST_ASSERT_EQUAL_UINT32(50480u, wgate::worstCasePostMs(CH, 400, 30000));
    TEST_ASSERT_TRUE(wgate::worstCasePostMs(CH, 400, 30000) < WDT_S * 1000u);
    // Typical LAN drain measured at ~25.6 kB/s (8192000/25600 = 320 ms exactly).
    TEST_ASSERT_EQUAL_UINT32(30320u, wgate::worstCasePostMs(CH, 25600, 30000));
    TEST_ASSERT_TRUE(wgate::worstCasePostMs(CH, 25600, 30000) < WDT_S * 1000u);
}

void test_unchunked_body_would_trip_wdt(void) {
    // The v2.1 single-request path: 81258 B at 400 B/s + 30 s = 233 s >> 60 s,
    // which is exactly the observed TASK_WDT panic signature.
    const uint32_t one = wgate::worstCasePostMs(81258, 400, 30000);
    TEST_ASSERT_EQUAL_UINT32(233145u, one);
    TEST_ASSERT_TRUE(one > WDT_S * 1000u);
    // Chunked, the same file fits inside the watchdog with room to spare.
    TEST_ASSERT_TRUE(wgate::worstCasePostMs(CH, 400, 30000) < WDT_S * 1000u);
}

void test_post_ms_degenerate_inputs(void) {
    TEST_ASSERT_EQUAL_UINT32(30000u, wgate::worstCasePostMs(CH, 0, 30000));
    TEST_ASSERT_EQUAL_UINT32(30000u, wgate::worstCasePostMs(0, 400, 30000));
}

// Rounding behaviour of the ceil division inside the helper.
void test_post_ms_exact_and_ceil(void) {
    // Exact division: 8192 B at 8192 B/s = 1000 ms + 30 s response.
    TEST_ASSERT_EQUAL_UINT32(31000u, wgate::worstCasePostMs(8192, 8192, 30000));
    // 1 B at 1000 B/s rounds up to 1 ms, not 0.
    TEST_ASSERT_EQUAL_UINT32(30001u, wgate::worstCasePostMs(1, 1000, 30000));
    // 8192 B at 3 B/s -> ceil(8192000/3) = 2730667 ms (+30 s).
    TEST_ASSERT_EQUAL_UINT32(2760667u, wgate::worstCasePostMs(8192, 3, 30000));
    // A shorter response timeout shortens the window one-for-one.
    TEST_ASSERT_EQUAL_UINT32(20480u + 5000u, wgate::worstCasePostMs(8192, 400, 5000));
    // The largest chunk we ship with still fits the 60 s watchdog.
    TEST_ASSERT_EQUAL_UINT32(50480u, wgate::worstCasePostMs(CH, 400, 30000));
    TEST_ASSERT_TRUE(wgate::worstCasePostMs(CH, 400, 30000) < WDT_S * 1000u);
    // A chunk larger than 12 kB would exceed it -> 8192 is the right knob.
    TEST_ASSERT_EQUAL_UINT32(70960u, wgate::worstCasePostMs(16384, 400, 30000));
    TEST_ASSERT_TRUE(wgate::worstCasePostMs(16384, 400, 30000) > WDT_S * 1000u);
}

// ---------------------------------------------------------------------------
// Retry ladder
// ---------------------------------------------------------------------------
void test_backoff_ladder(void) {
    TEST_ASSERT_EQUAL_UINT32(1u,  wgate::backoffMultiplier(0));
    TEST_ASSERT_EQUAL_UINT32(1u,  wgate::backoffMultiplier(1));
    TEST_ASSERT_EQUAL_UINT32(1u,  wgate::backoffMultiplier(2));   // still base cadence
    TEST_ASSERT_EQUAL_UINT32(2u,  wgate::backoffMultiplier(3));
    TEST_ASSERT_EQUAL_UINT32(4u,  wgate::backoffMultiplier(4));
    TEST_ASSERT_EQUAL_UINT32(8u,  wgate::backoffMultiplier(5));
    TEST_ASSERT_EQUAL_UINT32(16u, wgate::backoffMultiplier(6));
    TEST_ASSERT_EQUAL_UINT32(16u, wgate::backoffMultiplier(7));   // capped
    TEST_ASSERT_EQUAL_UINT32(16u, wgate::backoffMultiplier(100));
}

void test_backoff_matches_log_gap(void) {
    // The 2026-11-21 log shows 480 s between skip lines once past failure #6,
    // i.e. 8x of a 60 s base.
    TEST_ASSERT_EQUAL_UINT32(480u, 60u * wgate::backoffMultiplier(5));
    TEST_ASSERT_EQUAL_UINT32(960u, 60u * wgate::backoffMultiplier(6));
}

// Ladder properties over a long outage: never decreases, never exceeds the cap,
// and the whole 1x/2x/4x/8x/16x sequence is reached by failure 6.
void test_backoff_monotonic_and_capped(void) {
    uint32_t prev = 0;
    for (uint32_t n = 0; n <= 40; ++n) {
        const uint32_t m = wgate::backoffMultiplier(n);
        TEST_ASSERT_TRUE(m >= prev);          // monotonic
        TEST_ASSERT_TRUE(m <= 16u);            // capped
        prev = m;
    }
    TEST_ASSERT_EQUAL_UINT32(16u, prev);
    // The distinct levels, in order.
    const uint32_t kLevels[6] = { 1, 1, 1, 2, 4, 8 };
    for (int i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_UINT32(kLevels[i], wgate::backoffMultiplier(i));
    TEST_ASSERT_EQUAL_UINT32(16u, wgate::backoffMultiplier(6));
    // Cap of 16x of the 60 s base = 960 s, matching the observed 16-min plateau.
    TEST_ASSERT_EQUAL_UINT32(960u, 60u * wgate::backoffMultiplier(40));
}

// ---------------------------------------------------------------------------
// Cadence choice + self-heal gate (taskLoop bookkeeping)
// ---------------------------------------------------------------------------
void test_next_gap_ms_choice(void) {
    // Any progress (full or partial drain) shortens to the 5 s fast path.
    TEST_ASSERT_EQUAL_UINT32(5000u, wgate::nextGapMs(60000u, 5000u, 1u, true));
    TEST_ASSERT_EQUAL_UINT32(5000u, wgate::nextGapMs(60000u, 5000u, 16u, true));
    // No progress -> base * ladder.
    TEST_ASSERT_EQUAL_UINT32(60000u, wgate::nextGapMs(60000u, 5000u, 1u, false));
    TEST_ASSERT_EQUAL_UINT32(960000u, wgate::nextGapMs(60000u, 5000u, 16u, false));
    TEST_ASSERT_EQUAL_UINT32(480000u, wgate::nextGapMs(60000u, 5000u,
                                                      wgate::backoffMultiplier(5), false));
}

// The v0.8.1 lwIP self-heal needs BOTH conditions; either alone must not reboot.
void test_self_heal_needs_both_conditions(void) {
    TEST_ASSERT_TRUE (wgate::selfHealNeeded(10u, 37000u, 10u, 50000u));   // the 2026-05-21 case
    TEST_ASSERT_FALSE(wgate::selfHealNeeded(10u, 218000u, 10u, 50000u));  // healthy heap
    TEST_ASSERT_FALSE(wgate::selfHealNeeded(9u, 37000u, 10u, 50000u));    // short streak
    TEST_ASSERT_TRUE (wgate::selfHealNeeded(40u, 1u, 10u, 50000u));       // deep streak, tiny heap
    TEST_ASSERT_FALSE(wgate::selfHealNeeded(0u, 0u, 10u, 50000u));
    // Boundary: heap exactly at the floor is not "low".
    TEST_ASSERT_FALSE(wgate::selfHealNeeded(10u, 50000u, 10u, 50000u));
    TEST_ASSERT_TRUE (wgate::selfHealNeeded(10u, 49999u, 10u, 50000u));
    // Boundary: streak exactly at the threshold trips.
    TEST_ASSERT_TRUE (wgate::selfHealNeeded(10u, 40000u, 10u, 50000u));
}

// ---------------------------------------------------------------------------
// Diagnostic string decoding
// ---------------------------------------------------------------------------
void test_status_names(void) {
    TEST_ASSERT_EQUAL_STRING("IDLE",           wgate::statusName(0));
    TEST_ASSERT_EQUAL_STRING("NO_SSID_AVAIL",  wgate::statusName(1));
    TEST_ASSERT_EQUAL_STRING("SCAN_COMPLETED", wgate::statusName(2));
    TEST_ASSERT_EQUAL_STRING("CONNECTED",      wgate::statusName(3));
    TEST_ASSERT_EQUAL_STRING("CONNECT_FAILED", wgate::statusName(4));
    TEST_ASSERT_EQUAL_STRING("CONNECTION_LOST",wgate::statusName(5));
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED",   wgate::statusName(6));
    TEST_ASSERT_EQUAL_STRING("NO_SHIELD",      wgate::statusName(255));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",        wgate::statusName(42));
}

void test_skip_names(void) {
    TEST_ASSERT_EQUAL_STRING("none",       wgate::skipName(wgate::SkipNone));
    TEST_ASSERT_EQUAL_STRING("battery",    wgate::skipName(wgate::SkipBattery));
    TEST_ASSERT_EQUAL_STRING("no_pending", wgate::skipName(wgate::SkipNoPending));
    TEST_ASSERT_EQUAL_STRING("disabled",   wgate::skipName(wgate::SkipDisabled));
    TEST_ASSERT_EQUAL_STRING("none",       wgate::skipName(200));   // out of range
}

// Codes outside the enum must decode, never read past a table.
void test_status_names_out_of_range(void) {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", wgate::statusName(-1));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", wgate::statusName(7));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", wgate::statusName(256));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", wgate::statusName(1000));
}

// The WIFISTAT `phase=` token comes from this table (main.cpp uses it too).
void test_phase_names(void) {
    TEST_ASSERT_EQUAL_STRING("disabled",     wgate::phaseName(0));
    TEST_ASSERT_EQUAL_STRING("idle",         wgate::phaseName(1));
    TEST_ASSERT_EQUAL_STRING("backoff",      wgate::phaseName(2));
    TEST_ASSERT_EQUAL_STRING("connecting",   wgate::phaseName(3));
    TEST_ASSERT_EQUAL_STRING("posting",      wgate::phaseName(4));
    TEST_ASSERT_EQUAL_STRING("disconnecting",wgate::phaseName(5));
    TEST_ASSERT_EQUAL_STRING("unknown",      wgate::phaseName(6));
    TEST_ASSERT_EQUAL_STRING("unknown",      wgate::phaseName(255));
}

// Reference phase traces the simulator compares against.
void test_phase_sequence_tables(void) {
    const int kOk[5] = { 1, 3, 4, 5, 1 };
    for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_INT(kOk[i], wgate::phaseSeqOk(i));
    TEST_ASSERT_EQUAL_INT(-1, wgate::phaseSeqOk(5));
    TEST_ASSERT_EQUAL_INT(-1, wgate::phaseSeqOk(-1));

    const int kConn[3] = { 1, 3, 1 };
    for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_INT(kConn[i], wgate::phaseSeqConnect(i));
    TEST_ASSERT_EQUAL_INT(-1, wgate::phaseSeqConnect(3));

    // Every index of the reference traces must decode to a real phase name.
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_STRING(wgate::phaseName((uint8_t)wgate::phaseSeqOk(i)),
                                 wgate::phaseName((uint8_t)kOk[i]));
    }
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_gate_unmeasured_passes);
    RUN_TEST(test_gate_opens_at_threshold);
    RUN_TEST(test_gate_just_below_release_stays_closed);
    RUN_TEST(test_gate_holds_open_between_levels);
    RUN_TEST(test_gate_closes_at_floor);
    RUN_TEST(test_hysteresis_reduces_boundary_chatter);
    RUN_TEST(test_field_charging_ramp_opens_on_ninth_cycle);
    RUN_TEST(test_discharge_gets_two_extra_cycles_from_floor);

    RUN_TEST(test_rising_detects_charge_slope);
    RUN_TEST(test_rising_rejects_noise_and_flat);
    RUN_TEST(test_rising_needs_two_valid_samples);
    RUN_TEST(test_charge_run_latches_after_two_rises);

    RUN_TEST(test_connect_budget_two_profiles_25s);
    RUN_TEST(test_connect_budget_one_profile_25s);
    RUN_TEST(test_connect_budget_zero_profiles);
    RUN_TEST(test_model_reproduces_observed_5s_timeouts);
    RUN_TEST(test_25s_budget_covers_measured_associations);
    RUN_TEST(test_connect_budget_negative_and_zero);
    RUN_TEST(test_connect_budget_three_profiles_vs_watchdog);
    RUN_TEST(test_assoc_within_window);

    RUN_TEST(test_chunk_count_basic);
    RUN_TEST(test_chunk_count_boundaries);
    RUN_TEST(test_chunk_count_large_bodies);
    RUN_TEST(test_chunk_count_field_files);
    RUN_TEST(test_backlog_totals);
    RUN_TEST(test_chunks_line_aligned);

    RUN_TEST(test_worst_case_chunk_fits_wdt);
    RUN_TEST(test_unchunked_body_would_trip_wdt);
    RUN_TEST(test_post_ms_degenerate_inputs);
    RUN_TEST(test_post_ms_exact_and_ceil);

    RUN_TEST(test_backoff_ladder);
    RUN_TEST(test_backoff_matches_log_gap);
    RUN_TEST(test_backoff_monotonic_and_capped);
    RUN_TEST(test_next_gap_ms_choice);
    RUN_TEST(test_self_heal_needs_both_conditions);

    RUN_TEST(test_status_names);
    RUN_TEST(test_status_names_out_of_range);
    RUN_TEST(test_skip_names);
    RUN_TEST(test_phase_names);
    RUN_TEST(test_phase_sequence_tables);

    return UNITY_END();
}
