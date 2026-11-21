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
    // Basic-auth availability for the remote profile (secrets::INGEST_USER).
    bool     credsPresent  = true;
    // POST failure injection: failOnFile is the 1-based index inside this
    // cycle; failAtChunk 0 means "on the last chunk". allFilesFail models a
    // link that dies right after the association (every POST errors).
    int      failOnFile   = 0;
    int      failAtChunk  = 0;
    bool     allFilesFail = false;

    // --- fake LittleFS pending queue (bytes per .up.csv; 0 == stale debris) ---
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
    int         phases[8] = { 0 };     // phase codes, deduplicated (see setPhase)
    int         phaseLen  = 0;
    uint32_t    cycleMs     = 0;
    size_t      cycleBytes  = 0;
    uint32_t    cycleChunks = 0;
    uint32_t    maxUnfedMs  = 0;       // widest single blocking window
    uint32_t    okFiles     = 0;       // files removed from the queue
    uint32_t    failFiles   = 0;       // files that keep their queue slot
    uint32_t    postCount   = 0;       // HTTP requests issued this cycle
    bool        authApplied = false;
    const char* net         = "none";

    void reset() {
        traceLen = 0; cycleMs = 0; cycleBytes = 0; cycleChunks = 0;
        maxUnfedMs = 0; net = "none"; okFiles = 0; failFiles = 0;
        postCount = 0; authApplied = false; phaseLen = 0;
    }

    void emit(const char* t) { if (traceLen < 8) trace[traceLen++] = t; }

    // Records the phase code, collapsing repeats -- exactly what WIFISTAT would
    // show if it were sampled per transition: Posting is re-stamped for every
    // file but reads as one contiguous window.
    void setPhase(int p) {
        if (phaseLen > 0 && phases[phaseLen - 1] == p) return;
        if (phaseLen < 8) phases[phaseLen++] = p;
    }

    static bool associated(uint32_t ms, uint32_t timeout) {
        return wgate::assocWithinWindow(ms, timeout);
    }

    void cycle(float vbat) {
        reset();
        // taskLoop stamps the phase before sleeping on the ladder, using the
        // multiplier implied by the failure streak carried INTO this cycle.
        setPhase(wgate::backoffMultiplier(consec) > 1 ? 2 : 1);

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
        setPhase(3);                                    // Connecting
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
            setPhase(1);                                   // Idle (early return)
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
        // Basic auth is attached only on the remote profile, and only when both
        // secrets::INGEST_USER and INGEST_PASS are non-empty.
        authApplied = (strcmp(net, "remote") == 0) && credsPresent;

        // ---- drain: one keep-alive POST per 8 KB chunk ---------------------
        size_t nextFiles[20]; int nextCount = 0;
        for (int i = 0; i < fileCount; ++i) {
            // v0.4.5: stale zero-byte .up.csv debris is removed without a POST.
            if (files[i] == 0) { ++okFiles; ++uploaded; continue; }
            setPhase(4);                                   // Posting
            const size_t n = wgate::chunkCount(files[i], CH);
            bool fileOk = !allFilesFail;
            for (size_t c = 1; c <= (int)n; ++c) {
                ++postCount;
                const uint32_t rate = linkRateBps ? linkRateBps : 400u;
                const uint32_t w = wgate::worstCasePostMs(CH, rate, 30000);
                if (w > maxUnfedMs) maxUnfedMs = w;
                // A dead link (allFilesFail) errors on the first chunk write;
                // an injected fault errors on one specific chunk of one file.
                const int stopAt = (failAtChunk > 0) ? failAtChunk : (int)n;
                const bool stop = allFilesFail ||
                                  (failOnFile == i + 1 && c == stopAt);
                if (stop) { fileOk = false; break; }
            }
            if (fileOk) {
                ++okFiles; ++uploaded;
                // cycle_done only accounts for confirmed files, as in runOnce().
                cycleBytes  += files[i];
                cycleChunks += (uint32_t)n;
                if (linkRateBps) {
                    cycleMs += (uint32_t)(((uint64_t)files[i] * 1000u + linkRateBps - 1u)
                                          / linkRateBps);
                }
            } else {
                ++failFiles; ++failed;
                if (nextCount < 20) nextFiles[nextCount++] = files[i];   // stays queued
            }
        }
        setPhase(5);                                      // Disconnecting
        setPhase(1);                                      // Idle
        // Failed files keep their queue slot for the next cycle.
        for (int i = 0; i < nextCount; ++i) files[i] = nextFiles[i];
        fileCount = nextCount;
        emit("cycle_done");
    }

    // taskLoop bookkeeping around runOnce(), including the poked fast path.
    uint32_t nextGapMs(uint32_t okBefore, uint32_t failBefore) {
        if (failed > failBefore && uploaded == okBefore) ++consec;
        else if (uploaded > okBefore) consec = 0;
        return wgate::nextGapMs(BASE_MS, DRAIN_MS, wgate::backoffMultiplier(consec),
                                uploaded > okBefore);
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
// Scenario 6: partial drain -- one file errors mid-transfer, the rest go out
// ---------------------------------------------------------------------------
void test_sim_partial_drain_keeps_failed_file_queued(void) {
    Sim s;
    s.fileCount = 3;
    s.files[0] = 200; s.files[1] = 16384; s.files[2] = 700;
    s.homeMs = 801;
    // The 2-byte-per-row middle file dies on the first of its two chunks.
    s.failOnFile = 2; s.failAtChunk = 1;
    s.cycle(3.90f);

    TEST_ASSERT_EQUAL_STRING("cycle_start", s.trace[0]);
    TEST_ASSERT_EQUAL_STRING("linked",      s.trace[1]);
    TEST_ASSERT_EQUAL_STRING("cycle_done",  s.trace[2]);
    TEST_ASSERT_EQUAL_UINT32(2, s.okFiles);          // files 1 and 3 confirmed
    TEST_ASSERT_EQUAL_UINT32(1, s.failFiles);        // file 2 stays queued
    // Only confirmed files land in the cycle accounting.
    TEST_ASSERT_EQUAL_UINT32(900u, (uint32_t)s.cycleBytes);      // 200 + 700
    TEST_ASSERT_EQUAL_UINT32(2u,   s.cycleChunks);               // 1 + 1
    // Requests issued: file1 1, file2 1 (aborted), file3 1.
    TEST_ASSERT_EQUAL_UINT32(3u, s.postCount);
    TEST_ASSERT_EQUAL_INT(1, s.fileCount);
    TEST_ASSERT_EQUAL_UINT32(16384u, (uint32_t)s.files[0]);
    // Partial progress still counts as progress: 5 s kick, ladder not ramped.
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, s.consec);

    // Chain: the next cycle finishes the job -- 16384 B is exactly two chunks.
    const uint32_t okB = s.uploaded, failB = s.failed;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_UINT32(1, s.okFiles);
    TEST_ASSERT_EQUAL_UINT32(2u, s.cycleChunks);
    TEST_ASSERT_EQUAL_UINT32(2u, s.postCount);
    TEST_ASSERT_EQUAL_INT(0, s.fileCount);
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(okB, failB));
    TEST_ASSERT_EQUAL_UINT32(0, s.consec);
}

// Every POST errors after a good association (the errno=11 / lwIP pattern):
// both files stay queued and the ladder ramps from the base cadence.
void test_sim_all_posts_fail_then_link_recovers(void) {
    Sim s;
    s.fileCount = 2; s.files[0] = 500; s.files[1] = 600;
    s.homeMs = 801;
    s.allFilesFail = true;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("cycle_done", s.trace[2]);
    TEST_ASSERT_EQUAL_UINT32(0, s.okFiles);
    TEST_ASSERT_EQUAL_UINT32(2, s.failFiles);
    TEST_ASSERT_EQUAL_UINT32(2u, s.postCount);        // one aborted POST per file
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)s.cycleBytes);
    TEST_ASSERT_EQUAL_UINT32(0u, s.cycleChunks);
    TEST_ASSERT_EQUAL_INT(2, s.fileCount);            // nothing removed
    TEST_ASSERT_EQUAL_UINT32(0, s.uploaded);
    TEST_ASSERT_EQUAL_UINT32(2, s.failed);
    // Zero progress with link failures -> 1x of the base cadence.
    TEST_ASSERT_EQUAL_UINT32(BASE_MS, s.nextGapMs(0, 0));
    TEST_ASSERT_EQUAL_UINT32(1, s.consec);

    // Chain: the link recovers (a reboot clears the pbuf pools in the device).
    s.allFilesFail = false;
    const uint32_t okB = s.uploaded, failB = s.failed;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_UINT32(2, s.okFiles);
    TEST_ASSERT_EQUAL_UINT32(1100u, (uint32_t)s.cycleBytes);
    TEST_ASSERT_EQUAL_INT(0, s.fileCount);
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(okB, failB));
    TEST_ASSERT_EQUAL_UINT32(0, s.consec);             // ladder reset by progress
}

// ---------------------------------------------------------------------------
// Scenario 7: stale zero-byte .up.csv debris (v0.4.5 clean-up path)
// ---------------------------------------------------------------------------
void test_sim_stale_zero_byte_files_need_no_post(void) {
    Sim s;
    s.fileCount = 3;
    s.files[0] = 0; s.files[1] = 200; s.files[2] = 0;    // debris around one real file
    s.homeMs = 801;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_UINT32(3, s.okFiles);              // all three removed
    TEST_ASSERT_EQUAL_UINT32(0, s.failFiles);
    TEST_ASSERT_EQUAL_UINT32(1u, s.postCount);           // only the real file POSTs
    TEST_ASSERT_EQUAL_UINT32(200u, (uint32_t)s.cycleBytes);
    TEST_ASSERT_EQUAL_UINT32(1u, s.cycleChunks);
    TEST_ASSERT_EQUAL_INT(0, s.fileCount);
    // The Posting window still appears because the real file did POST.
    TEST_ASSERT_EQUAL_INT(5, s.phaseLen);
}

void test_sim_all_stale_files_skip_the_posting_phase(void) {
    // Only debris: the loop never reaches a POST, so `phase=` never reads
    // "posting" -- the exact trace WIFISTAT shows for that cycle.
    Sim s;
    s.fileCount = 2; s.files[0] = 0; s.files[1] = 0;
    s.homeMs = 801;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_UINT32(2, s.okFiles);
    TEST_ASSERT_EQUAL_UINT32(0u, s.postCount);
    TEST_ASSERT_EQUAL_UINT32(0u, s.cycleChunks);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)s.cycleBytes);
    const int kExpected[4] = { 1, 3, 5, 1 };            // idle,conn,cleanup,idle
    TEST_ASSERT_EQUAL_INT(4, s.phaseLen);
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_INT(kExpected[i], s.phases[i]);
    // Debris still counts as progress -> the 5 s fast path, no backoff ramp.
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(0, 0));
}

// ---------------------------------------------------------------------------
// Scenario 8: full charge / unplug / recharge gate lifecycle
// ---------------------------------------------------------------------------
void test_sim_charge_unplug_recharge_lifecycle(void) {
    Sim s;
    s.homeMs = 612;
    const float seq[11] = {
        3.40f,   // 1: closed, no previous sample
        3.46f,   // 2: rising, one sample
        3.52f,   // 3: rising, latch -> charging
        3.58f,   // 4: rising, still just below the release level
        3.62f,   // 5: gate OPENS
        3.62f,   // 6: charger removed -> flat -> charging cleared
        3.60f,   // 7: falling, still above the floor -> holds open
        3.55f,   // 8: two extra cycles of margin from the hysteresis
        3.44f,   // 9: at/below the floor -> CLOSES
        3.50f,   // 10: closed, rising but below the release level
        3.61f,   // 11: re-opens
    };
    for (int i = 0; i < 11; ++i) {
        s.fileCount = 1; s.files[0] = 198;             // recording keeps rotating
        s.cycle(seq[i]);
        switch (i) {
            case 0: case 1:   // before two consecutive rises
                TEST_ASSERT_EQUAL_UINT8(0, s.charging); break;
            case 2: case 3: case 4:                    // charging
                TEST_ASSERT_EQUAL_UINT8(1, s.charging); break;
            case 5: case 6: case 7:                    // unplugged / discharging
                TEST_ASSERT_EQUAL_UINT8(0, s.charging); break;
        }
    }
    // Skips: cycles 1-4, then 9 and 10 -> 6 gated cycles; 5 uploads total.
    TEST_ASSERT_EQUAL_UINT32(6, s.skips);
    TEST_ASSERT_EQUAL_UINT32(5, s.uploaded);
    TEST_ASSERT_EQUAL_UINT32(0, s.failed);
    TEST_ASSERT_EQUAL_UINT32(0, s.consec);              // gating never ramps the ladder
    // The last observed cycle was a full drain (5 s fast path).
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(0, 0));
}

// ---------------------------------------------------------------------------
// Scenario 9: no_pending -> data arrives -> no_pending again
// ---------------------------------------------------------------------------
void test_sim_no_pending_then_data_arrives_chain(void) {
    Sim s;
    s.homeMs = 801;
    s.fileCount = 0;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("skip:no_pending", s.trace[0]);
    TEST_ASSERT_EQUAL_INT(1, s.traceLen);
    TEST_ASSERT_EQUAL_UINT8(wgate::SkipNoPending, s.lastSkip);
    TEST_ASSERT_EQUAL_UINT32(0, s.attempts);            // radio never opened
    TEST_ASSERT_EQUAL_UINT32(0, s.skips);               // battery counter untouched
    TEST_ASSERT_EQUAL_INT(1, s.phaseLen);               // stayed idle
    TEST_ASSERT_EQUAL_INT(1, s.phases[0]);
    TEST_ASSERT_EQUAL_UINT32(BASE_MS, s.nextGapMs(0, 0));

    // Chain: samples rotate into a pending file, next cycle drains it.
    s.fileCount = 2; s.files[0] = 198; s.files[1] = 23700;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("cycle_start", s.trace[0]);
    TEST_ASSERT_EQUAL_UINT32(2, s.okFiles);
    TEST_ASSERT_EQUAL_UINT8(wgate::SkipNone, s.lastSkip);
    TEST_ASSERT_EQUAL_UINT32(1, s.attempts);            // one attempt, home worked
    TEST_ASSERT_EQUAL_UINT32(4u, s.cycleChunks);        // ceil(198/8192)+ceil(23700/8192)
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(0, 0));

    // Chain: queue empty again -> back to the no_pending skip.
    s.fileCount = 0;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("skip:no_pending", s.trace[0]);
    TEST_ASSERT_EQUAL_UINT8(wgate::SkipNoPending, s.lastSkip);
    TEST_ASSERT_EQUAL_UINT32(0, s.skips);
}

// ---------------------------------------------------------------------------
// Scenario 10: home times out, hotspot answers, Basic Auth is attached
// ---------------------------------------------------------------------------
void test_sim_home_timeout_then_remote_with_auth(void) {
    Sim s;
    s.homeMs = 0; s.remoteMs = 12000;                   // home absent, LTE up
    s.fileCount = 2; s.files[0] = 198; s.files[1] = 23700;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("remote", s.net);
    TEST_ASSERT_EQUAL_UINT32(2, s.attempts);            // home burnt its whole window
    TEST_ASSERT_EQUAL_UINT32(37400u, s.cycleMs);        // 100 + 25300 + 12000
    TEST_ASSERT_EQUAL_UINT32(3, s.lastStatus);          // final status is CONNECTED
    TEST_ASSERT_TRUE(s.authApplied);                    // remote profile uses Basic Auth
    TEST_ASSERT_EQUAL_UINT32(2, s.okFiles);

    // Without credentials the same path posts anonymously.
    Sim n;
    n.homeMs = 0; n.remoteMs = 12000; n.credsPresent = false;
    n.fileCount = 1; n.files[0] = 198;
    n.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("remote", n.net);
    TEST_ASSERT_FALSE(n.authApplied);
    TEST_ASSERT_EQUAL_UINT32(1, n.okFiles);
}

// ---------------------------------------------------------------------------
// Scenario 11: association landing exactly on the deadline edge
// ---------------------------------------------------------------------------
void test_sim_assoc_exactly_at_deadline_edge(void) {
    Sim s;
    s.homeMs = 25000;                                   // lands on the deadline
    s.fileCount = 1; s.files[0] = 198;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("home", s.net);
    TEST_ASSERT_EQUAL_UINT32(1, s.attempts);            // remote never tried
    TEST_ASSERT_EQUAL_UINT32(25100u, s.cycleMs);        // 100 + 25000
    // The widest un-petted window in the whole cycle is still one chunk, which
    // is wider than even a full-length association window.
    TEST_ASSERT_EQUAL_UINT32(wgate::worstCasePostMs(CH, 400u, 30000u), s.maxUnfedMs);
    TEST_ASSERT_EQUAL_UINT32(50480u, s.maxUnfedMs);

    // One ms later the driver would not have been polled in time: the attempt
    // burns the whole window and the fallback carries the cycle.
    Sim e;
    e.homeMs = 25001; e.remoteMs = 800;
    e.fileCount = 1; e.files[0] = 198;
    e.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("remote", e.net);
    TEST_ASSERT_EQUAL_UINT32(2, e.attempts);
    TEST_ASSERT_EQUAL_UINT32(26200u, e.cycleMs);        // 100 + 25300 + 800
    TEST_ASSERT_EQUAL_UINT32(1, e.okFiles);
}

// ---------------------------------------------------------------------------
// Scenario 12: phase traces match the shared reference tables
// ---------------------------------------------------------------------------
void test_sim_phase_trace_matches_reference_tables(void) {
    // Fully successful cycle.
    Sim s;
    s.fileCount = 2; s.files[0] = 198; s.files[1] = 23700; s.homeMs = 801;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_INT(5, s.phaseLen);
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_INT(wgate::phaseSeqOk(i), s.phases[i]);
        TEST_ASSERT_EQUAL_STRING("ok", strcmp(wgate::phaseName((uint8_t)s.phases[i]),
                                              "unknown") == 0 ? "bad" : "ok");
    }

    // Association failure: connecting is entered, then straight back to idle.
    Sim c;
    c.fileCount = 1; c.files[0] = 198; c.homeMs = 0; c.remoteMs = 0;
    c.cycle(3.90f);
    TEST_ASSERT_EQUAL_INT(3, c.phaseLen);
    for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_INT(wgate::phaseSeqConnect(i), c.phases[i]);

    // Gated skip: the task never leaves its wait phase.
    Sim g;
    g.fileCount = 1; g.files[0] = 198;
    g.cycle(3.10f);
    TEST_ASSERT_EQUAL_INT(1, g.phaseLen);
    TEST_ASSERT_EQUAL_STRING("idle", wgate::phaseName((uint8_t)g.phases[0]));
}

// ---------------------------------------------------------------------------
// Scenario 13: long outage -- ladder, backoff phase, then recovery
// ---------------------------------------------------------------------------
void test_sim_long_outage_ladder_then_recovery(void) {
    Sim s;
    s.fileCount = 2; s.files[0] = 198; s.files[1] = 23700;
    s.homeMs = 0; s.remoteMs = 0;                       // both APs absent

    const uint32_t kGaps[10] = { 60000, 60000, 120000, 240000, 480000,
                                 960000, 960000, 960000, 960000, 960000 };
    for (int i = 0; i < 10; ++i) {
        const uint32_t okB = s.uploaded, failB = s.failed;
        s.cycle(3.90f);
        TEST_ASSERT_EQUAL_STRING("connect_fail", s.trace[1]);
        TEST_ASSERT_EQUAL_UINT32(kGaps[i], s.nextGapMs(okB, failB));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(i + 1), s.consec);
        // The wait phase only reads "backoff" once the multiplier exceeds 1x.
        // It is stamped with the streak known when the task went to sleep, so
        // it lags the post-cycle counter by exactly one cycle.
        const int expectPhase = (s.consec > 3) ? 2 : 1;
        TEST_ASSERT_EQUAL_INT(expectPhase, s.phases[0]);
        // Each burnt cycle pays the full two-profile budget.
        TEST_ASSERT_EQUAL_UINT32(50700u, s.cycleMs);
    }
    // The v0.8.1 self-heal fires only with 10 fails AND low heap.
    TEST_ASSERT_TRUE(wgate::selfHealNeeded(s.consec, 37000u, 10u, 50000u));
    TEST_ASSERT_FALSE(wgate::selfHealNeeded(s.consec, 60000u, 10u, 50000u));
    TEST_ASSERT_FALSE(wgate::selfHealNeeded(9u, 37000u, 10u, 50000u));

    // Chain: the AP comes back -> one cycle drains, ladder and phase reset.
    s.homeMs = 612;
    const uint32_t okB = s.uploaded, failB = s.failed;
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_STRING("cycle_done", s.trace[2]);
    TEST_ASSERT_EQUAL_UINT32(2, s.okFiles);
    TEST_ASSERT_EQUAL_UINT32(4u, s.cycleChunks);            // 1 + 3 POSTs
    TEST_ASSERT_EQUAL_INT(5, s.phaseLen);                   // full five-phase drain
    // This cycle still woke from the long backoff: the phase is stamped when the
    // task goes to sleep, so it lags the recovery by one cycle.
    TEST_ASSERT_EQUAL_INT(2, s.phases[0]);
    // taskLoop folds the result into the ladder after runOnce() returns.
    TEST_ASSERT_EQUAL_UINT32(DRAIN_MS, s.nextGapMs(okB, failB));
    TEST_ASSERT_EQUAL_UINT32(0, s.consec);

    // Next wake reads plain idle again, with nothing queued (skip, no radio).
    s.cycle(3.90f);
    TEST_ASSERT_EQUAL_INT(1, s.phases[0]);
    TEST_ASSERT_EQUAL_STRING("skip:no_pending", s.trace[0]);
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
    RUN_TEST(test_sim_partial_drain_keeps_failed_file_queued);
    RUN_TEST(test_sim_all_posts_fail_then_link_recovers);
    RUN_TEST(test_sim_stale_zero_byte_files_need_no_post);
    RUN_TEST(test_sim_all_stale_files_skip_the_posting_phase);
    RUN_TEST(test_sim_charge_unplug_recharge_lifecycle);
    RUN_TEST(test_sim_no_pending_then_data_arrives_chain);
    RUN_TEST(test_sim_home_timeout_then_remote_with_auth);
    RUN_TEST(test_sim_assoc_exactly_at_deadline_edge);
    RUN_TEST(test_sim_phase_trace_matches_reference_tables);
    RUN_TEST(test_sim_long_outage_ladder_then_recovery);

    return UNITY_END();
}
