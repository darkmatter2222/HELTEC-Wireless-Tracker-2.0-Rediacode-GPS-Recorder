// Native unit tests for the GPS-fix transition detector that emits
// GPS_LOST / GPS_REGAINED event rows (firmware v0.7.0).
//
// The transition logic in main.cpp tracks two flags:
//   - prevHasGps:    previous GPS fix state
//   - initDone:      whether the prior state has been observed at least once
//
// On each tick:
//   - If !initDone: latch prevHasGps = curHasGps, set initDone, emit nothing.
//   - Else if curHasGps != prevHasGps:
//         emit "GPS_REGAINED" if curHasGps else "GPS_LOST"
//         prevHasGps = curHasGps
//   - Else: no-op.
//
// This file extracts that pure decision into `transitionEvent()` so the
// behaviour can be verified independently of FreeRTOS / Arduino.
//
// Run:  pio test -e native -f test_gps_transition_native
#include <unity.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Function under test (mirrors src/main.cpp lines 620..640).
// Returns:
//   ""              -> emit nothing
//   "GPS_LOST"      -> emit GPS_LOST
//   "GPS_REGAINED"  -> emit GPS_REGAINED
//
// `prev` and `init` are in/out: the caller is expected to thread them through
// successive calls.
// ---------------------------------------------------------------------------
static const char* transitionEvent(bool curHasGps, bool* prev, bool* init) {
    if (!*init) {
        *prev = curHasGps;
        *init = true;
        return "";
    }
    if (curHasGps == *prev) return "";
    const char* evt = curHasGps ? "GPS_REGAINED" : "GPS_LOST";
    *prev = curHasGps;
    return evt;
}

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// First-observation tests
// ---------------------------------------------------------------------------

void test_first_call_no_fix_emits_nothing(void) {
    bool prev = false, init = false;
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));
    TEST_ASSERT_TRUE(init);
    TEST_ASSERT_FALSE(prev);
}

void test_first_call_with_fix_emits_nothing(void) {
    bool prev = false, init = false;
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
    TEST_ASSERT_TRUE(init);
    TEST_ASSERT_TRUE(prev);
}

// ---------------------------------------------------------------------------
// Steady-state tests
// ---------------------------------------------------------------------------

void test_steady_no_fix_emits_nothing(void) {
    bool prev = false, init = true;
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));
}

void test_steady_with_fix_emits_nothing(void) {
    bool prev = true, init = true;
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
}

// ---------------------------------------------------------------------------
// Transition tests
// ---------------------------------------------------------------------------

void test_lose_fix_emits_gps_lost(void) {
    bool prev = true, init = true;
    TEST_ASSERT_EQUAL_STRING("GPS_LOST", transitionEvent(false, &prev, &init));
    TEST_ASSERT_FALSE(prev);
    // Subsequent same-state call must be silent.
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));
}

void test_regain_fix_emits_gps_regained(void) {
    bool prev = false, init = true;
    TEST_ASSERT_EQUAL_STRING("GPS_REGAINED", transitionEvent(true, &prev, &init));
    TEST_ASSERT_TRUE(prev);
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
}

void test_full_sequence_lose_regain_lose(void) {
    bool prev = false, init = false;
    // boot with fix
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
    // lose
    TEST_ASSERT_EQUAL_STRING("GPS_LOST", transitionEvent(false, &prev, &init));
    // still no fix
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));
    // regain
    TEST_ASSERT_EQUAL_STRING("GPS_REGAINED", transitionEvent(true, &prev, &init));
    // hold
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(true, &prev, &init));
    // lose again
    TEST_ASSERT_EQUAL_STRING("GPS_LOST", transitionEvent(false, &prev, &init));
}

void test_boot_without_fix_then_acquire(void) {
    // Typical cold-boot indoors -> walk outside scenario.
    bool prev = false, init = false;
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));   // boot, no fix
    TEST_ASSERT_EQUAL_STRING("", transitionEvent(false, &prev, &init));   // still no fix
    TEST_ASSERT_EQUAL_STRING("GPS_REGAINED", transitionEvent(true, &prev, &init));  // outside
}

// Rapid flap protection is NOT performed in firmware -- by design, every
// observed transition emits an event. The viewer is responsible for any
// debouncing/visualisation policy.
void test_rapid_flap_emits_every_transition(void) {
    bool prev = true, init = true;
    TEST_ASSERT_EQUAL_STRING("GPS_LOST",     transitionEvent(false, &prev, &init));
    TEST_ASSERT_EQUAL_STRING("GPS_REGAINED", transitionEvent(true,  &prev, &init));
    TEST_ASSERT_EQUAL_STRING("GPS_LOST",     transitionEvent(false, &prev, &init));
    TEST_ASSERT_EQUAL_STRING("GPS_REGAINED", transitionEvent(true,  &prev, &init));
}

// ---------------------------------------------------------------------------
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_call_no_fix_emits_nothing);
    RUN_TEST(test_first_call_with_fix_emits_nothing);
    RUN_TEST(test_steady_no_fix_emits_nothing);
    RUN_TEST(test_steady_with_fix_emits_nothing);
    RUN_TEST(test_lose_fix_emits_gps_lost);
    RUN_TEST(test_regain_fix_emits_gps_regained);
    RUN_TEST(test_full_sequence_lose_regain_lose);
    RUN_TEST(test_boot_without_fix_then_acquire);
    RUN_TEST(test_rapid_flap_emits_every_transition);
    return UNITY_END();
}
