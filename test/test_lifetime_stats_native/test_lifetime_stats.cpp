// Unit tests for LifetimeStats pure logic (v1.0.0).
//
// Native tests do not link firmware sources or hardware headers. We replicate
// the pure mathematical helpers here (haversine, cell key, spike detection,
// altitude gain capping) and test them in isolation.
//
// Run with: pio test -e native

#include <unity.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

// M_PI is a POSIX extension not available in strict ISO C on MSVC/clang-cl.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Haversine distance (km) — replicated from lifetime_stats.cpp
// ---------------------------------------------------------------------------
static float haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    const double dLat = (lat2 - lat1) * M_PI / 180.0;
    const double dLon = (lon2 - lon1) * M_PI / 180.0;
    const double a = sin(dLat / 2) * sin(dLat / 2) +
                     cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
                     sin(dLon / 2) * sin(dLon / 2);
    const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return (float)(R * c);
}

void test_haversine_zero_distance(void) {
    // Same point -> 0 km.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, haversineKm(47.6062, -122.3321, 47.6062, -122.3321));
}

void test_haversine_known_distance(void) {
    // Seattle (47.6062, -122.3321) to Portland (45.5051, -122.6750) ≈ 234 km.
    const float d = haversineKm(47.6062, -122.3321, 45.5051, -122.6750);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 234.0f, d);
}

void test_haversine_short_distance(void) {
    // 0.01 degree latitude ≈ 1.11 km.
    const float d = haversineKm(47.6000, -122.3321, 47.6100, -122.3321);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.11f, d);
}

void test_haversine_symmetry(void) {
    const float d1 = haversineKm(47.6062, -122.3321, 45.5051, -122.6750);
    const float d2 = haversineKm(45.5051, -122.6750, 47.6062, -122.3321);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, d1, d2);
}

// ---------------------------------------------------------------------------
// Distance noise gate — skip jumps > 5 km
// ---------------------------------------------------------------------------
static bool isDistanceAcceptable(float km) {
    return (km < 5.0f);
}

void test_noise_gate_accepts_normal_movement(void) {
    TEST_ASSERT_TRUE(isDistanceAcceptable(0.0f));
    TEST_ASSERT_TRUE(isDistanceAcceptable(4.99f));
}

void test_noise_gate_rejects_gps_jumps(void) {
    TEST_ASSERT_FALSE(isDistanceAcceptable(5.0f));
    TEST_ASSERT_FALSE(isDistanceAcceptable(100.0f));
}

// ---------------------------------------------------------------------------
// Altitude gain — accumulate only positive deltas; cap noise > 200m
// ---------------------------------------------------------------------------
static float altGainDelta(float prevAlt, float currAlt) {
    const float dAlt = currAlt - prevAlt;
    if (dAlt > 0.0f && dAlt < 200.0f) return dAlt;
    return 0.0f;
}

void test_alt_gain_ascending(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, altGainDelta(100.0f, 110.0f));
}

void test_alt_gain_descending_ignored(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, altGainDelta(110.0f, 100.0f));
}

void test_alt_gain_flat_ignored(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, altGainDelta(100.0f, 100.0f));
}

void test_alt_gain_noise_cap_200m(void) {
    // GPS glitch producing 200m+ instant jump should be ignored.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, altGainDelta(100.0f, 300.1f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, altGainDelta(100.0f, 1000.0f));
    // Exactly 199.9 m is fine.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 199.9f, altGainDelta(0.0f, 199.9f));
}

// ---------------------------------------------------------------------------
// Spike event detection — CPS >= threshold
// ---------------------------------------------------------------------------
static constexpr float SPIKE_THRESHOLD = 50.0f;
static bool isSpike(float cps) { return cps >= SPIKE_THRESHOLD; }

void test_spike_below_threshold(void) {
    TEST_ASSERT_FALSE(isSpike(0.0f));
    TEST_ASSERT_FALSE(isSpike(49.9f));
}

void test_spike_at_threshold(void) {
    TEST_ASSERT_TRUE(isSpike(50.0f));
}

void test_spike_above_threshold(void) {
    TEST_ASSERT_TRUE(isSpike(51.0f));
    TEST_ASSERT_TRUE(isSpike(10000.0f));
}

// ---------------------------------------------------------------------------
// Recording time accumulation — cap dt to 10 s
// ---------------------------------------------------------------------------
static uint32_t recordingDtSecs(uint32_t dt_ms) {
    if (dt_ms > 0 && dt_ms < 10000) return dt_ms / 1000;
    return 0;
}

void test_recording_time_normal(void) {
    TEST_ASSERT_EQUAL_UINT32(1, recordingDtSecs(1000));
    TEST_ASSERT_EQUAL_UINT32(1, recordingDtSecs(1500));
    TEST_ASSERT_EQUAL_UINT32(9, recordingDtSecs(9999));
}

void test_recording_time_gap_capped(void) {
    // A BLE gap > 10 s should not contribute fake recording time.
    TEST_ASSERT_EQUAL_UINT32(0, recordingDtSecs(10000));
    TEST_ASSERT_EQUAL_UINT32(0, recordingDtSecs(60000));
}

void test_recording_time_zero_dt(void) {
    TEST_ASSERT_EQUAL_UINT32(0, recordingDtSecs(0));
}

// ---------------------------------------------------------------------------
// Cell key encoding — verify encode/decode roundtrip
// ---------------------------------------------------------------------------
static constexpr float CELL_DEG = 0.01f;

static uint32_t cellKey(double lat, double lng) {
    if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0)
        return 0xFFFFFFFFUL;
    const int32_t iLat = (int32_t)((lat  + 90.0)  / CELL_DEG);
    const int32_t iLng = (int32_t)((lng + 180.0)  / CELL_DEG);
    return (uint32_t)((uint32_t)iLat * 36000u + (uint32_t)iLng);
}

void test_cell_key_same_cell(void) {
    // Two points within the same 0.01-deg cell produce the same key.
    // Use values clearly inside one cell: start at exact cell boundary,
    // offset second point by 0.001 deg (1/10 of a cell), both in same cell.
    const uint32_t k1 = cellKey(47.600, -122.330);
    const uint32_t k2 = cellKey(47.601, -122.329);
    TEST_ASSERT_EQUAL_UINT32(k1, k2);
}

void test_cell_key_different_cells(void) {
    const uint32_t k1 = cellKey(47.600, -122.330);
    const uint32_t k2 = cellKey(47.610, -122.330);
    TEST_ASSERT_NOT_EQUAL(k1, k2);
}

void test_cell_key_invalid_coords(void) {
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, cellKey(91.0, 0.0));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, cellKey(0.0, 181.0));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, cellKey(-91.0, 0.0));
}

void test_cell_key_no_overflow(void) {
    // Max values should not overflow uint32_t.
    // iLat_max = (90+90)/0.01 - 1 = 17999, iLng_max = 35999
    // Key_max = 17999 * 36000 + 35999 = 647,999,999 < 2^32
    const uint32_t k = cellKey(89.99, 179.99);
    TEST_ASSERT_NOT_EQUAL(0xFFFFFFFFUL, k);
    TEST_ASSERT_TRUE(k < 648000000UL);
}

// ---------------------------------------------------------------------------
// Battery cycle detection state machine
// ---------------------------------------------------------------------------
struct BatState {
    bool wasHigh = false;
    bool wasLow  = false;
    uint32_t cycles = 0;
};
static void onBattery(BatState& s, int pct) {
    if (pct <= 20) {
        s.wasLow  = true;
        s.wasHigh = false;
    } else if (pct >= 80 && s.wasLow) {
        s.wasHigh = true;
        s.wasLow  = false;
        ++s.cycles;
    }
}

void test_battery_cycle_full_cycle(void) {
    BatState s;
    onBattery(s, 15);   // discharge below 20%
    onBattery(s, 85);   // charge above 80%
    TEST_ASSERT_EQUAL_UINT32(1, s.cycles);
}

void test_battery_no_discharge_no_cycle(void) {
    BatState s;
    // Never went below 20% -> no cycle
    onBattery(s, 50);
    onBattery(s, 90);
    TEST_ASSERT_EQUAL_UINT32(0, s.cycles);
}

void test_battery_partial_discharge_no_cycle(void) {
    BatState s;
    onBattery(s, 30);   // dropped to 30%, not below 20%
    onBattery(s, 90);
    TEST_ASSERT_EQUAL_UINT32(0, s.cycles);
}

void test_battery_two_cycles(void) {
    BatState s;
    onBattery(s, 10);
    onBattery(s, 85);
    onBattery(s, 10);
    onBattery(s, 85);
    TEST_ASSERT_EQUAL_UINT32(2, s.cycles);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_haversine_zero_distance);
    RUN_TEST(test_haversine_known_distance);
    RUN_TEST(test_haversine_short_distance);
    RUN_TEST(test_haversine_symmetry);

    RUN_TEST(test_noise_gate_accepts_normal_movement);
    RUN_TEST(test_noise_gate_rejects_gps_jumps);

    RUN_TEST(test_alt_gain_ascending);
    RUN_TEST(test_alt_gain_descending_ignored);
    RUN_TEST(test_alt_gain_flat_ignored);
    RUN_TEST(test_alt_gain_noise_cap_200m);

    RUN_TEST(test_spike_below_threshold);
    RUN_TEST(test_spike_at_threshold);
    RUN_TEST(test_spike_above_threshold);

    RUN_TEST(test_recording_time_normal);
    RUN_TEST(test_recording_time_gap_capped);
    RUN_TEST(test_recording_time_zero_dt);

    RUN_TEST(test_cell_key_same_cell);
    RUN_TEST(test_cell_key_different_cells);
    RUN_TEST(test_cell_key_invalid_coords);
    RUN_TEST(test_cell_key_no_overflow);

    RUN_TEST(test_battery_cycle_full_cycle);
    RUN_TEST(test_battery_no_discharge_no_cycle);
    RUN_TEST(test_battery_partial_discharge_no_cycle);
    RUN_TEST(test_battery_two_cycles);

    return UNITY_END();
}
