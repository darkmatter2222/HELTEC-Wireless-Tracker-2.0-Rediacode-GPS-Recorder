// Unit tests for the v2.4 disk-capacity gate in src/disk_gate.h.
//
// These include the REAL header (not a copy) so the assertions apply to the
// code that actually runs on the device. Run with: pio test -e native
//
// Reference data is the 2026-11-23 boot-loop incident: the on-chip 5.9 MB
// volume (6160384 B = 752 x 8192) filled to 100 % during a long upload
// outage; littlefs 2.9's ENOSPC log line divides by cfg->block_count (the
// autodetect sentinel, 0) and panics with IntegerDivideByZero on every boot.
// The gate keeps the volume permanently below full so ENOSPC is unreachable.

#include <unity.h>
#include <stdint.h>
#include <stddef.h>

#include "../../src/disk_gate.h"

// The real V2 volume geometry (hardware/partitions_tracker_v2.csv) and the
// configured reserve (src/config.h v2.4: DISK_RESERVE_PCT = 15).
static const size_t VOL = 6160384;   // 0x5E0000 = 752 blocks x 8192
static const int    PCT = 15;
static const size_t ROW = 70;        // typical 12-column CSV row length

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// reserveBytes: percentage math
// ---------------------------------------------------------------------------
void test_reserve_basic(void) {
    // 6160384 * 15 / 100 = 924057.6 -> 924057 (truncated).
    TEST_ASSERT_EQUAL_UINT64(924057, dgate::reserveBytes(VOL, PCT));
    TEST_ASSERT_EQUAL_UINT64(150, dgate::reserveBytes(1000, 15));
    TEST_ASSERT_EQUAL_UINT64(0, dgate::reserveBytes(0, 15));   // no backend
    TEST_ASSERT_EQUAL_UINT64(0, dgate::reserveBytes(1000, 0)); // disabled
    TEST_ASSERT_EQUAL_UINT64(0, dgate::reserveBytes(1000, -5));
}

void test_reserve_clamps(void) {
    TEST_ASSERT_EQUAL_UINT64(1000, dgate::reserveBytes(1000, 100));
    TEST_ASSERT_EQUAL_UINT64(1000, dgate::reserveBytes(1000, 101)); // clamped
    TEST_ASSERT_EQUAL_UINT64(0, dgate::reserveBytes(1, 15));  // 1*15/100 = 0
    TEST_ASSERT_EQUAL_UINT64(1, dgate::reserveBytes(7, 15));  // 7*15/100 = 1
}

void test_reserve_no_overflow(void) {
    // 0xFFFF0000 * 99 / 100 must be computed in 64-bit: a 32-bit product
    // wraps. 0xFFFF0000 = 4294901760; * 99 / 100 = 4251952742.
    TEST_ASSERT_EQUAL_UINT64(4251952742ULL,
                             dgate::reserveBytes(0xFFFF0000UL, 99));
}

// ---------------------------------------------------------------------------
// freeBytes: degenerate stats never go negative
// ---------------------------------------------------------------------------
void test_free_bytes(void) {
    TEST_ASSERT_EQUAL_UINT64(150, dgate::freeBytes(1000, 850));
    TEST_ASSERT_EQUAL_UINT64(0, dgate::freeBytes(1000, 1000));
    TEST_ASSERT_EQUAL_UINT64(0, dgate::freeBytes(1000, 1001)); // used > total
    TEST_ASSERT_EQUAL_UINT64(0, dgate::freeBytes(0, 5));
}

// ---------------------------------------------------------------------------
// selfHealNeeded: the boot self-heal predicate
// ---------------------------------------------------------------------------
void test_self_heal_not_needed_when_reserve_held(void) {
    // reserve(1000, 15) = 150. free >= 150 -> nothing to delete.
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(1000, 850, 15)); // free == 150
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(1000, 849, 15)); // free == 151
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(1000, 0, 15));   // empty volume
}

void test_self_heal_needed_below_reserve(void) {
    TEST_ASSERT_TRUE(dgate::selfHealNeeded(1000, 851, 15));  // free == 149
    TEST_ASSERT_TRUE(dgate::selfHealNeeded(1000, 1000, 15)); // 100 % full
    TEST_ASSERT_TRUE(dgate::selfHealNeeded(1000, 1001, 15)); // degenerate
}

void test_self_heal_disabled_cases(void) {
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(0, 0, 15));     // no backend
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(1000, 1000, 0)); // pct disabled
}

// ---------------------------------------------------------------------------
// appendAllowed: the per-row gate
// ---------------------------------------------------------------------------
void test_append_keeps_reserve(void) {
    // reserve(1000, 15) = 150, row = 70.
    // allowed  <=> free >= 150 AND free - 150 >= 70  <=> free >= 220.
    TEST_ASSERT_TRUE(dgate::appendAllowed(1000, 780, 70, 15));   // free 220
    TEST_ASSERT_FALSE(dgate::appendAllowed(1000, 781, 70, 15));  // free 219
    TEST_ASSERT_FALSE(dgate::appendAllowed(1000, 850, 70, 15));  // free 150
    TEST_ASSERT_FALSE(dgate::appendAllowed(1000, 1000, 70, 15)); // full
    TEST_ASSERT_TRUE(dgate::appendAllowed(1000, 0, 70, 15));     // empty
}

void test_append_disabled_and_zero_row(void) {
    TEST_ASSERT_TRUE(dgate::appendAllowed(0, 0, 70, 15));      // no backend
    TEST_ASSERT_TRUE(dgate::appendAllowed(1000, 1000, 70, 0)); // pct disabled
    // Zero-byte row: allowed exactly while the reserve itself is held.
    TEST_ASSERT_TRUE(dgate::appendAllowed(1000, 850, 0, 15));  // free 150
    TEST_ASSERT_FALSE(dgate::appendAllowed(1000, 851, 0, 15)); // free 149
}

void test_field_volume_math(void) {
    // Real volume: reserve = 924057. A 70 B row is allowed while
    // free >= 924057 + 70 = 924127, i.e. used <= 6160384 - 924127 = 5236257.
    TEST_ASSERT_TRUE(dgate::appendAllowed(VOL, 5236257, ROW, PCT));
    TEST_ASSERT_FALSE(dgate::appendAllowed(VOL, 5236258, ROW, PCT));
    TEST_ASSERT_FALSE(dgate::appendAllowed(VOL, VOL, ROW, PCT));
}

// ---------------------------------------------------------------------------
// Chained scenario: the 2026-11-23 incident, end to end.
// Phase 1 (pre-gate firmware): 1 Hz recording with failed uploads fills the
// volume to 100 %. Phase 2 (gated firmware boots on the same volume): the
// self-heal deletes the oldest pending file until the reserve holds, and
// recording resumes. Phase 3 (gate active): a fresh fill stops at the
// reserve boundary, so a 100 % volume can never form again.
// ---------------------------------------------------------------------------
void test_chain_fill_then_heal(void) {
    // Phase 1: no gate. Fresh volume, 2 blocks (16384 B) used by the empty FS.
    size_t used = 16384;
    size_t appended = 0;
    while (used + ROW <= VOL) {
        used += ROW;
        appended++;
    }
    // (6160384 - 16384) / 70 = 87771.4 -> 87771 rows = 24.4 h at 1 Hz.
    TEST_ASSERT_EQUAL_UINT64(87771, appended);
    TEST_ASSERT_TRUE(used >= VOL - ROW);          // volume is (nearly) full
    TEST_ASSERT_TRUE(dgate::selfHealNeeded(VOL, used, PCT));

    // Phase 2: new firmware boots. Self-heal deletes the oldest pending file
    // (a 2 MB rotation) until free >= reserve (924057).
    size_t pending = 2ULL * 1024 * 1024;
    size_t deleted = 0;
    while (dgate::selfHealNeeded(VOL, used, PCT)) {
        TEST_ASSERT_TRUE(pending > 0);
        used -= pending;
        pending = 0;
        deleted++;
    }
    TEST_ASSERT_EQUAL_UINT64(1, deleted);
    TEST_ASSERT_EQUAL_UINT64(6160354ULL - 2ULL * 1024 * 1024, used); // 4063202
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(VOL, used, PCT));
    TEST_ASSERT_TRUE(dgate::appendAllowed(VOL, used, ROW, PCT));

    // Phase 3: with the gate active, fill from that healed state. The gate
    // must stop the day file before the volume can reach 100 %.
    while (dgate::appendAllowed(VOL, used, ROW, PCT)) {
        used += ROW;
    }
    TEST_ASSERT_TRUE(dgate::freeBytes(VOL, used) >= dgate::reserveBytes(VOL, PCT));
    TEST_ASSERT_FALSE(dgate::selfHealNeeded(VOL, used, PCT));
    // The fill stops exactly at the reserve boundary: free in
    // [reserve, reserve + row - 1].
    TEST_ASSERT_TRUE(dgate::freeBytes(VOL, used) < dgate::reserveBytes(VOL, PCT) + ROW);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_reserve_basic);
    RUN_TEST(test_reserve_clamps);
    RUN_TEST(test_reserve_no_overflow);
    RUN_TEST(test_free_bytes);
    RUN_TEST(test_self_heal_not_needed_when_reserve_held);
    RUN_TEST(test_self_heal_needed_below_reserve);
    RUN_TEST(test_self_heal_disabled_cases);
    RUN_TEST(test_append_keeps_reserve);
    RUN_TEST(test_append_disabled_and_zero_row);
    RUN_TEST(test_field_volume_math);
    RUN_TEST(test_chain_fill_then_heal);

    return UNITY_END();
}
