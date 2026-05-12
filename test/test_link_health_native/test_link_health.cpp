// Unit tests for the BLE link-health watchdog decision (v0.6.0).
//
// The firmware forces a disconnect when:
//   state == Ready && lastReadingMs != 0 && (now - lastReadingMs) > threshold
//
// The helper here mirrors that condition as a pure function so we can
// regression-test the boundary conditions on the host without booting BLE.

#include <unity.h>
#include <stdint.h>

static bool bleLinkStalled(uint32_t lastReadingMs, uint32_t nowMs,
                           uint32_t thresholdMs) {
    if (lastReadingMs == 0) return false;          // not yet armed
    return (nowMs - lastReadingMs) > thresholdMs;
}

void setUp(void) {}
void tearDown(void) {}

void test_no_arming_no_stall(void) {
    TEST_ASSERT_FALSE(bleLinkStalled(0, 100000, 15000));
    TEST_ASSERT_FALSE(bleLinkStalled(0, 0, 15000));
}

void test_fresh_reading_no_stall(void) {
    TEST_ASSERT_FALSE(bleLinkStalled(100000, 100500, 15000));
}

void test_at_threshold_not_yet(void) {
    // Strictly greater-than -- equal does NOT trip (matches firmware).
    TEST_ASSERT_FALSE(bleLinkStalled(100000, 115000, 15000));
}

void test_just_past_threshold_trips(void) {
    TEST_ASSERT_TRUE(bleLinkStalled(100000, 115001, 15000));
}

void test_long_stall_trips(void) {
    TEST_ASSERT_TRUE(bleLinkStalled(100000, 1000000, 15000));
}

void test_wraparound_safe(void) {
    // millis() wraps every ~49.7 days. Unsigned subtraction yields the
    // correct delta as long as the true elapsed time fits in uint32_t.
    const uint32_t kWrap = 0xFFFFFFFFu;
    // last reading 1 s before wrap, now 1 s after wrap -> 2 s elapsed.
    TEST_ASSERT_FALSE(bleLinkStalled(kWrap - 1000, 1000, 15000));
    // last reading 20 s before wrap, now 1 s after wrap -> 21 s elapsed.
    TEST_ASSERT_TRUE(bleLinkStalled(kWrap - 20000, 1000, 15000));
}

void test_threshold_zero_immediate(void) {
    TEST_ASSERT_TRUE(bleLinkStalled(100000, 100001, 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_no_arming_no_stall);
    RUN_TEST(test_fresh_reading_no_stall);
    RUN_TEST(test_at_threshold_not_yet);
    RUN_TEST(test_just_past_threshold_trips);
    RUN_TEST(test_long_stall_trips);
    RUN_TEST(test_wraparound_safe);
    RUN_TEST(test_threshold_zero_immediate);
    return UNITY_END();
}
