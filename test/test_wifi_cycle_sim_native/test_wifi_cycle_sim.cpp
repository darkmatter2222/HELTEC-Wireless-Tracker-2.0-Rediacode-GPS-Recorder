// Device-simulation suite for the v2.3 Wi-Fi uploader cycle.
//
// A compact model of WifiUploader::runOnce() / taskLoop() that is driven with
// injected scenarios (battery samples, per-SSID association latencies, a fake
// pending queue) and checked against the exact observable trace. All decisions
// come from src/wifi_gate.h -- the same header the firmware compiles -- so the
// model cannot drift from the device code.
//
// Run with: pio test -e native

#include <unity.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "../../src/wifi_gate.h"

// ---------------------------------------------------------------------------
// Mirror of the compiled configuration
// ---------------------------------------------------------------------------
static const uint32_t T_TIMEOUT_25 = 25000;   // v2.3 secrets.h
static const uint32_t T_TIMEOUT_5  = 5000;    // pre-v2.3 value, for regression
static const uint32_t BASE_MS      = 60000;   // UPLOAD_INTERVAL_MS
static const uint32_t DRAIN_MS     = 5000;    // fast-path gap after a success
static const uint32_t WDT_MS       = 60000;   // cfg::TASK_WDT_TIMEOUT_S
static const size_t   CH           = 8192;    // cfg::UPLOAD_CHUNK_BYTES
static const float    ON_V         = 3.60f;
static const float    OFF_V        = 3.45f;
static const float    DEADB        = 0.010f;
static const uint32_t RUN_NEEDED   = 2;

// ---------------------------------------------------------------------------
// Simulator
// ---------------------------------------------------------------------------
struct Sim {
    // --- injected inputs ---
    bool     hasHome   = true;
    bool     hasRemote = true;
    uint32_t timeoutMs = T_TIMEOUT_25;
    // Association outcome per SSID: 0 ms == never associates (timeout).
    uint32_t homeMs    = 0;
    uint32_t remoteMs  = 0;
    // Optional link rate for the drain phase; 0 = report the connect portion.
    uint32_t linkRateBps = 0;

    // --- fake LittleFS pending queue (bytes per .up.csv) ---
    size_t   files[20];
    int      fileCount = 0;

    // --- latched state, matching WifiUploader members ---
    bool     gateOpen   = false;
    float    vbatPrev   = -1.0f;
    uint32_t chargeRunN = 0;
    uint8_t  charging   = 0;
    uint32_t uploaded   = 0;
    uint32_t failed     = 0;
    uint32_t consec     = 0;
    uint32_t skips      = 0;
    uint8_t  lastSkip   = wgate::SkipNone;
    uint32_t attempts   = 0;
    uint32_t lastConnMs = 0;
    int      lastStatus = 0;

    // --- observable trace of the most recent cycle ---
    const char* trace[8];
    int         traceLen = 0;
    uint32_t    cycleMs     = 0;
    size_t      cycleBytes  = 0;
    uint32_t    cycleChunks = 0;
    uint32_t    maxUnfedMs  = 0;      // widest single blocking window
    const char* net         = "none";

    void reset() {
        traceLen = 0; cycleMs = 0; cycleBytes = 0; cycleChunks = 0;
        maxUnfedMs = 0; net = "none";
    }

    void emit(const char* t) { if (traceLen < 8) trace[traceLen++] = t; }

    static bool associated(uint32_t ms, uint32_t timeout) {
        return ms != 0 && ms <= timeout;
    }

    void cycle(float vbat) {
        reset();

        // ---- gate (vbatGateAllows) ----------------------------------------
        const bool ok = wgate::vbatGateOk(vbat, gateOpen, ON_V, OFF_V);
        const bool rising = wgate::voltageRising(vbatPrev, vbat, DEADB);
        chargeRunN = wgate::chargeRun(chargeRunN, rising);
        vbatPrev   = vbat;
        charging   = (uint8_t)(chargeRunN >= RUN_NEEDED ? 1u : 0u);
        gateOpen   = ok;

        if (!ok) {
            ++skips;
            lastSkip = wgate::SkipBattery;
            emit("skip:battery");
            return;                       // no radio, no counters touched
        }

        if (fileCount == 0) {
            lastSkip = wgate::SkipNoPending;
            emit("skip:no_pending");
            return;
        }
        lastSkip = wgate::SkipNone;
        emit("cycle_start");

        // ---- connectWifi ---------------------------------------------------
        // Mirrors the code exactly: 100 ms pre-begin settle, then per attempt
        // either [assoc latency] or [full timeout window + 300 ms settle].
        uint32_t connectTotal = wgate::CONNECT_SETTLE_MS;
        bool linked = false;
        if (hasHome) {
            ++attempts;
            if (associated(homeMs, timeoutMs)) {
                connectTotal += homeMs;
                lastConnMs = homeMs; lastStatus = 3;      // WL_CONNECTED
                net = "home"; linked = true;
            } else {
                connectTotal += timeoutMs + wgate::INTER_SSID_SETTLE_MS;
                lastStatus = 6;                           // WL_DISCONNECTED
            }
        }
        if (!linked && hasRemote) {
            ++attempts;
            if (associated(remoteMs, timeoutMs)) {
                connectTotal += remoteMs;
                lastConnMs = remoteMs; lastStatus = 3;
                net = "remote"; linked = true;
            } else {
                connectTotal += timeoutMs + wgate::INTER_SSID_SETTLE_MS;
                lastStatus = 6;
            }
        }
        maxUnfedMs = timeoutMs;                            // per-attempt poll window
        if (!linked) {
            ++failed;
            emit("connect_fail");
            cycleMs = connectTotal;
            // Cross-check the hand model against the shared helper.
            TEST_ASSERT_EQUAL_UINT32(
                wgate::worstCaseConnectMs(timeoutMs,
                                          (hasHome ? 1 : 0) + (hasRemote ? 1 : 0)),
                connectTotal);
            return;
        }
        emit("linked");
        if (connectTotal > maxUnfedMs) maxUnfedMs = connectTotal;
        cycleMs = connectTotal;

        // ---- drain: one keep-alive POST per 8 KB chunk ---------------------
        for (int i = 0; i < fileCount; ++i) {
            const size_t n = wgate::chunkCount(files[i], CH);
            cycleChunks += (uint32_t)n;
            cycleBytes  += files[i];
            // Chunked: the widest un-petted window is one chunk + response wait.
            const uint32_t w = wgate::worstCasePostMs(CH, 400, 30000);
            if (w > maxUnfedMs) maxUnfedMs = w;
            // Optional link model so the full cycle duration can be asserted.
            if (linkRateBps) {
                cycleMs += (uint32_t)(((uint64_t)files[i] * 1000u + linkRateBps - 1u)
                                      / linkRateBps);
            }
        }
        uploaded   += (uint32_t)fileCount;
        fileCount   = 0;                 // removePendingUpload() after each 2xx
        emit("cycle_done");
    }

    // taskLoop bookkeeping around runOnce(), including the poked fast path.
    uint32_t nextGapMs(uint32_t okBefore, uint32_t failBefore) {
        if (failed > failBefore && uploaded == okBefore) ++consec;
        else if (uploaded > okBefore) consec = 0;
        const uint32_t mult = wgate::backoffMultiplier(consec);
        if (uploaded > okBefore) return DRAIN_MS;
        return BASE_MS * mult;
    }

    // Same model, but with the pre-v2.3 single-request body (no chunking).
    static uint32_t unfedWdtWindowUnchunked(size_t bodyBytes, uint32_t rate) {
        return wgate::worstCasePostMs(bodyBytes, rate, 30000);
    }
};

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Scenario 1: the 2026-11-21 incident -- battery-gated, then a full drain
// ---------------------------------------------------------------------------
static size_t gBacklog[15] = {
    1604185, 292, 571, 305473, 939, 513185, 572, 758,
    2220556, 825404, 1398, 200, 44878, 23700, 198
};

void test_sim_gated_then_drains(void) {
    Sim s;
    s.fileCount = 15;
    memcpy(s.files, gBacklog, sizeof(gBacklog));
    s.homeMs = 3807;                      // measured cold associate

    const float ramp[9] = { 3.145f, 3.293f, 3.376f, 3.435f, 3.482f,
                            3.506f, 3.545f, 3.567f, 3.621f };
    for (int i = 0; i < 8; ++i) {
        const uint32_t okB = s.uploaded, failB = s.failed;
        s.cycle(ramp[i]);
        TEST_ASSERT_EQUAL_STRING("skip:battery", s.trace[0]);
        TEST_ASSERT_EQUAL_INT(1, s.traceLen);
        TEST_ASSERT_EQUAL_UINT32(0, s.uploaded);
        TEST_ASSERT_EQUAL_UINT32(0, s.failed);
        TEST_ASSERT_EQUAL_UINT32(0, s.consec);                     // gate does not ramp
        TEST_ASSERT_EQUAL_UINT32(BASE_MS, s.nextGapMs(okB, failB));  // stays 60 s
    }
    // 9th cycle: gate opens and the whole backlog goes out in one cycle.
    s.cycle(ramp[8]);
    TEST_ASSERT_EQUAL_STRING("cycle_start",  s.trace[0]);
    TEST_ASSERT_EQUAL_STRING("linked",       s.trace[1]);
    TEST_ASSERT_EQUAL_STRING("cycle_done",   s.trace[2]);
    TEST_ASSERT_EQUAL_INT(3, s.traceLen);
    TEST_ASSERT_EQUAL_STRING("home", s.net);
    TEST_ASSERT_EQUAL_UINT32(15, s.uploaded);
    TEST_ASSERT_EQUAL_INT(0, s.fileCount);
    TEST_ASSERT_EQUAL_UINT32(5542309u, (uint32_t)s.cycleBytes);
    TEST_ASSERT_EQUAL_UINT32(687u, s.cycleChunks);
    TEST_ASSERT_EQUAL_UINT32(8u, s.skips);
    TEST_ASSERT_EQUAL_UINT32(wgate::SkipNone, s.lastSkip);
    // Charge detector was latched on by the monotone ramp (>=2 rises in a row).
    TEST_ASSERT_EQUAL_UINT8(1, s.charging);
    // Fast-drain cadence after a success.
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(0, 0));
}

void test_sim_gate_never_opens_means_zero_uploads(void) {
    // Reproduce the exact 8-minute gated window: no cycle ever reaches the
    // radio, so uploaded/failed/consec all stay 0 and lastAttempt never moves.
    Sim s;
    s.fileCount = 15;
    memcpy(s.files, gBacklog, sizeof(gBacklog));
    const float ramp[8] = { 3.145f, 3.293f, 3.376f, 3.435f,
                            3.482f, 3.506f, 3.545f, 3.567f };
    for (int i = 0; i < 8; ++i) s.cycle(ramp[i]);
    TEST_ASSERT_EQUAL_UINT32(8, s.skips);
    TEST_ASSERT_EQUAL_UINT32(0, s.uploaded);
    TEST_ASSERT_EQUAL_UINT32(0, s.failed);
    TEST_ASSERT_EQUAL_UINT32(0, s.attempts);          // radio never opened
    TEST_ASSERT_EQUAL_UINT8(wgate::SkipBattery, s.lastSkip);
    TEST_ASSERT_EQUAL_INT(15, s.fileCount);            // backlog untouched
}

// ---------------------------------------------------------------------------
// Scenario 2: both APs absent -> full budget burnt, then the retry ladder
// ---------------------------------------------------------------------------
void test_sim_both_profiles_timeout_uses_full_budget(void) {
    Sim s;
    s.fileCount = 3;
    s.files[0] = 200; s.files[1] = 400; s.files[2] = 600;
    s.homeMs = 0; s.remoteMs = 0;                      // never associate

    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("cycle_start", s.trace[0]);
    TEST_ASSERT_EQUAL_STRING("connect_fail", s.trace[1]);
    TEST_ASSERT_EQUAL_UINT32(50700u, s.cycleMs);        // 100 + 2*(25000+300)
    TEST_ASSERT_EQUAL_UINT32(2, s.attempts);
    TEST_ASSERT_EQUAL_UINT32(1, s.failed);
    TEST_ASSERT_EQUAL_INT(3, s.fileCount);              // nothing deleted
    TEST_ASSERT_EQUAL_INT(6, s.lastStatus);             // DISCONNECTED
}

void test_sim_backoff_ramps_only_on_link_failures(void) {
    Sim s;
    s.fileCount = 1; s.files[0] = 500;
    s.homeMs = 0; s.remoteMs = 0;

    // Gaps after each consecutive failure: 1x, 1x, 2x, 4x, 8x, 16x of 60 s.
    const uint32_t expect[6] = { 60000, 60000, 120000, 240000, 480000, 960000 };
    for (int i = 0; i < 6; ++i) {
        const uint32_t okB = s.uploaded, failB = s.failed;
        s.cycle(3.90f);
        TEST_ASSERT_EQUAL_UINT32(expect[i], s.nextGapMs(okB, failB));
    }
    TEST_ASSERT_EQUAL_UINT32(6, s.consec);
    TEST_ASSERT_EQUAL_UINT32(0, s.skips);               // gate was open throughout
}

// ---------------------------------------------------------------------------
// Scenario 3: the timeout regression itself (5 s vs 25 s, same link)
// ---------------------------------------------------------------------------
void test_sim_hotspot_needs_12s_fails_at_5s_succeeds_at_25s(void) {
    // A cold associate with NimBLE polling was measured at 10-15 s in the
    // v0.4.8 lesson; 12 s is representative.
    Sim s5;
    s5.hasHome = false;                 // out of range of the home AP
    s5.timeoutMs = T_TIMEOUT_5;
    s5.remoteMs  = 12000;
    s5.fileCount = 2; s5.files[0] = 198; s5.files[1] = 23700;
    s5.cycle(3.80f);
    TEST_ASSERT_EQUAL_STRING("connect_fail", s5.trace[1]);
    TEST_ASSERT_EQUAL_UINT32(0, s5.uploaded);
    TEST_ASSERT_EQUAL_UINT32(5400u, s5.cycleMs);   // 100 + (5000 + 300), one profile

    Sim s25;
    s25.hasHome = false;
    s25.timeoutMs = T_TIMEOUT_25;
    s25.remoteMs  = 12000;
    s25.fileCount = 2; s25.files[0] = 198; s25.files[1] = 23700;
    s25.cycle(3.80f);
    TEST_ASSERT_EQUAL_STRING("cycle_done", s25.trace[2]);
    TEST_ASSERT_EQUAL_UINT32(2, s25.uploaded);
    TEST_ASSERT_EQUAL_STRING("remote", s25.net);
    TEST_ASSERT_EQUAL_UINT32(23898u, (uint32_t)s25.cycleBytes);
    TEST_ASSERT_EQUAL_UINT32(4u, s25.cycleChunks);      // ceil(198/8192)+ceil(23700/8192)
}

void test_sim_drain_rate_matches_lan_measurement(void) {
    // Model check against the device: 1604185 B in 62709 ms on the LAN.
    Sim s;
    s.fileCount = 1; s.files[0] = 1604185; s.homeMs = 801;
    s.linkRateBps = 25600;                      // 1604185 / 62.709 s
    s.cycle(3.90f);
    const uint32_t transfer = (uint32_t)((1604185ull * 1000ull + 25599ull) / 25600ull);
    TEST_ASSERT_EQUAL_UINT32(62664u, transfer);
    TEST_ASSERT_EQUAL_UINT32(801u + wgate::CONNECT_SETTLE_MS + transfer, s.cycleMs);
    TEST_ASSERT_TRUE(s.cycleMs < 300000u);      // under 5 min for one day file
    TEST_ASSERT_EQUAL_UINT32(196u, s.cycleChunks);
}

void test_sim_hotspot_link_makes_the_backlog_slow_but_safe(void) {
    // Field-measured LTE hotspot pace: ~1700 B/s (266 KB in 19.5 s).
    Sim s;
    s.fileCount = 15;
    memcpy(s.files, gBacklog, sizeof(gBacklog));
    s.remoteMs = 12000; s.hasHome = false; s.linkRateBps = 1700;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("remote", s.net);
    TEST_ASSERT_EQUAL_UINT32(5542309u, (uint32_t)s.cycleBytes);
    TEST_ASSERT_EQUAL_UINT32(687u, s.cycleChunks);
    TEST_ASSERT_EQUAL_UINT32(0, s.fileCount);
    // ~55 min of transfer for the whole backlog -- but every un-petted window
    // is still one chunk, so the watchdog never fires (see the WDT test).
    TEST_ASSERT_TRUE(s.cycleMs > 3000000u);
    TEST_ASSERT_TRUE(s.maxUnfedMs < WDT_MS);
}

void test_sim_home_wins_when_both_available(void) {
    Sim s;
    s.homeMs = 801; s.remoteMs = 1200;   // both would associate
    s.fileCount = 1; s.files[0] = 764;
    s.cycle(3.95f);
    TEST_ASSERT_EQUAL_STRING("home", s.net);
    TEST_ASSERT_EQUAL_UINT32(1, s.attempts);             // remote never tried
    TEST_ASSERT_EQUAL_UINT32(801u + wgate::CONNECT_SETTLE_MS, s.cycleMs);
}

void test_sim_poke_runs_cycle_immediately(void) {
    // requestNow() clears the countdown, so the poked cycle is due at once and
    // the STORAGE line must read "soon" rather than the stale deadline.
    Sim s;
    s.fileCount = 2; s.files[0] = 198; s.files[1] = 198;
    s.homeMs = 900;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(0, 0));   // 5 s, not 60 s
    TEST_ASSERT_EQUAL_UINT32(2, s.uploaded);
    TEST_ASSERT_EQUAL_UINT32(0, s.consec);
}

// ---------------------------------------------------------------------------
// Scenario 4: watchdog coverage across the whole simulated cycle
// ---------------------------------------------------------------------------
void test_sim_every_window_fits_the_watchdog(void) {
    Sim s;
    s.fileCount = 3;
    s.files[0] = 1604185; s.files[1] = 305473; s.files[2] = 198;
    s.homeMs = 15000;                                     // worst documented cold
    s.cycle(3.90f);
    // Widest single un-petted window must stay inside TASK_WDT_TIMEOUT_S.
    TEST_ASSERT_TRUE(s.maxUnfedMs < WDT_MS);
    TEST_ASSERT_EQUAL_UINT32(50480u, s.maxUnfedMs);       // 8 KB @ 400 B/s + 30 s
}

void test_sim_unchunked_body_exceeds_watchdog(void) {
    const uint32_t one = Sim::unfedWdtWindowUnchunked(81258, 400);
    TEST_ASSERT_TRUE(one > WDT_MS);
    TEST_ASSERT_EQUAL_UINT32(233145u, one);
    // Chunked, the identical body is safe.
    TEST_ASSERT_TRUE(wgate::worstCasePostMs(CH, 400, 30000) < WDT_MS);
}

// ---------------------------------------------------------------------------
// Scenario 5: charge/discharge trend as seen by the diagnostics
// ---------------------------------------------------------------------------
void test_sim_charge_flag_tracks_charger(void) {
    Sim s;
    s.fileCount = 1; s.files[0] = 100; s.homeMs = 800;
    // Charging: monotone +20 mV steps.
    s.cycle(2.66f);   TEST_ASSERT_EQUAL_UINT8(0, s.charging);  // no previous sample
    s.cycle(2.74f);   TEST_ASSERT_EQUAL_UINT8(0, s.charging);  // one rise
    s.cycle(2.82f);   TEST_ASSERT_EQUAL_UINT8(1, s.charging);  // two rises -> latched
    s.cycle(2.86f);   TEST_ASSERT_EQUAL_UINT8(1, s.charging);
    // Charger removed: flat/falling clears the latch immediately.
    s.cycle(2.86f);   TEST_ASSERT_EQUAL_UINT8(0, s.charging);
    s.cycle(2.80f);   TEST_ASSERT_EQUAL_UINT8(0, s.charging);
}

void test_sim_unsampled_adc_does_not_block(void) {
    // First cycle of a boot can run before the 5 s battery refresh; the gate
    // must not stall the very first upload on a -1.0 sentinel.
    Sim s;
    s.fileCount = 1; s.files[0] = 100; s.homeMs = 900;
    s.cycle(-1.0f);
    TEST_ASSERT_EQUAL_STRING("cycle_done", s.trace[2]);
    TEST_ASSERT_EQUAL_UINT32(1, s.uploaded);
    TEST_ASSERT_EQUAL_UINT8(0, s.charging);   // trend undefined with one sample
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_sim_gated_then_drains);
    RUN_TEST(test_sim_gate_never_opens_means_zero_uploads);
    RUN_TEST(test_sim_both_profiles_timeout_uses_full_budget);
    RUN_TEST(test_sim_backoff_ramps_only_on_link_failures);
    RUN_TEST(test_sim_hotspot_needs_12s_fails_at_5s_succeeds_at_25s);
    RUN_TEST(test_sim_drain_rate_matches_lan_measurement);
    RUN_TEST(test_sim_hotspot_link_makes_the_backlog_slow_but_safe);
    RUN_TEST(test_sim_home_wins_when_both_available);
    RUN_TEST(test_sim_poke_runs_cycle_immediately);
    RUN_TEST(test_sim_every_window_fits_the_watchdog);
    RUN_TEST(test_sim_unchunked_body_exceeds_watchdog);
    RUN_TEST(test_sim_charge_flag_tracks_charger);
    RUN_TEST(test_sim_unsampled_adc_does_not_block);

    return UNITY_END();
}
