// Unit tests for the dose-NVS save decision (v0.6.0).
//
// Mirrors the helper in src/main.cpp:
//   bool shouldSaveDose(current, lastSaved, msSinceLastSave,
//                       deltaThresholdUSv, maxIntervalMs);
//
// Decision rule: save when |current - lastSaved| >= deltaThresholdUSv
// OR msSinceLastSave >= maxIntervalMs.
//
// We replicate the algorithm here verbatim (PIO native tests do not link
// firmware sources -- they only build the test_*_native folder).

#include <unity.h>
#include <stdint.h>

static bool shouldSaveDose(float current, float lastSaved,
                           uint32_t msSinceLastSave,
                           float deltaThresholdUSv,
                           uint32_t maxIntervalMs) {
    const float diff = current - lastSaved;
    const float absDiff = diff < 0.0f ? -diff : diff;
    if (absDiff >= deltaThresholdUSv) return true;
    if (msSinceLastSave >= maxIntervalMs) return true;
    return false;
}

void setUp(void) {}
void tearDown(void) {}

void test_no_change_no_save(void) {
    TEST_ASSERT_FALSE(shouldSaveDose(10.0f, 10.0f, 30000, 0.5f, 300000));
}

void test_tiny_change_no_save(void) {
    TEST_ASSERT_FALSE(shouldSaveDose(10.05f, 10.0f, 30000, 0.5f, 300000));
    TEST_ASSERT_FALSE(shouldSaveDose(10.49f, 10.0f, 30000, 0.5f, 300000));
}

void test_at_threshold_saves(void) {
    TEST_ASSERT_TRUE(shouldSaveDose(10.5f, 10.0f, 30000, 0.5f, 300000));
}

void test_above_threshold_saves(void) {
    TEST_ASSERT_TRUE(shouldSaveDose(11.0f, 10.0f, 30000, 0.5f, 300000));
    TEST_ASSERT_TRUE(shouldSaveDose(1000.0f, 10.0f, 30000, 0.5f, 300000));
}

void test_negative_delta_uses_abs(void) {
    // Dose can decrease after a user reset; absolute value is what we care about.
    TEST_ASSERT_TRUE(shouldSaveDose(0.0f, 12.3f, 30000, 0.5f, 300000));
    TEST_ASSERT_FALSE(shouldSaveDose(10.4f, 10.5f, 30000, 0.5f, 300000));
}

void test_max_interval_forces_save_even_without_delta(void) {
    // No change but 5+ minutes have elapsed -> save anyway for safety.
    TEST_ASSERT_TRUE(shouldSaveDose(10.0f, 10.0f, 300000, 0.5f, 300000));
    TEST_ASSERT_TRUE(shouldSaveDose(10.0f, 10.0f, 600000, 0.5f, 300000));
}

void test_under_max_interval_and_under_delta_no_save(void) {
    // 4 minutes elapsed, 0.1 uSv change -> no save.
    TEST_ASSERT_FALSE(shouldSaveDose(10.1f, 10.0f, 240000, 0.5f, 300000));
}

void test_zero_threshold_always_saves_on_any_change(void) {
    // With a 0.0 threshold the absDiff >= 0.0 is true for any value including
    // no change -- this is fine because the firmware never sets threshold=0
    // (cfg::DOSE_NVS_DELTA_USV = 0.5f). The test below just locks in that
    // edge-case behaviour.
    TEST_ASSERT_TRUE(shouldSaveDose(10.001f, 10.0f, 1, 0.0f, 999999));
    TEST_ASSERT_TRUE(shouldSaveDose(10.0f,   10.0f, 1, 0.0f, 999999));
}

void test_typical_idle_pattern(void) {
    // Simulate a 30-second cadence with 0.001 uSv/sec accumulation:
    // first 7 cycles should NOT save (delta < 0.5), 8th cycle should save
    // because the safety interval (5 min) elapses first.
    float saved = 10.0f;
    float current = saved;
    uint32_t sinceSave = 0;
    int saveCount = 0;
    for (int i = 0; i < 20; ++i) {
        current += 0.03f;  // ~0.001 uSv/s * 30 s
        sinceSave += 30000;
        if (shouldSaveDose(current, saved, sinceSave, 0.5f, 300000)) {
            ++saveCount;
            saved = current;
            sinceSave = 0;
        }
    }
    // 20 cycles * 30 s = 600 s = 10 min. Total accumulation = 0.6 uSv.
    // Threshold-based saves: 1 (at ~0.5 uSv).
    // Safety-based saves: 1 more at the 5 min mark beyond that.
    // Total: <= 3 saves. Old code would have written 20 times.
    TEST_ASSERT_LESS_OR_EQUAL_INT(3, saveCount);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, saveCount);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_no_change_no_save);
    RUN_TEST(test_tiny_change_no_save);
    RUN_TEST(test_at_threshold_saves);
    RUN_TEST(test_above_threshold_saves);
    RUN_TEST(test_negative_delta_uses_abs);
    RUN_TEST(test_max_interval_forces_save_even_without_delta);
    RUN_TEST(test_under_max_interval_and_under_delta_no_save);
    RUN_TEST(test_zero_threshold_always_saves_on_any_change);
    RUN_TEST(test_typical_idle_pattern);
    return UNITY_END();
}
