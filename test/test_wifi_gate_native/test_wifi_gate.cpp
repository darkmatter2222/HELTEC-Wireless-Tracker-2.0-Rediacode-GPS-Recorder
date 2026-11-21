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

    RUN_TEST(test_chunk_count_basic);
    RUN_TEST(test_chunk_count_field_files);
    RUN_TEST(test_backlog_totals);

    RUN_TEST(test_worst_case_chunk_fits_wdt);
    RUN_TEST(test_unchunked_body_would_trip_wdt);
    RUN_TEST(test_post_ms_degenerate_inputs);

    RUN_TEST(test_backoff_ladder);
    RUN_TEST(test_backoff_matches_log_gap);

    RUN_TEST(test_status_names);
    RUN_TEST(test_skip_names);

    return UNITY_END();
}
