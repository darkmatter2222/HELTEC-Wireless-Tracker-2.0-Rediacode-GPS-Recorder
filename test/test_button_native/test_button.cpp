// Unit tests for the PRG-button / UI navigation helpers in src/button_logic.h.
//
// The real header is included (not a copy), so the assertions apply to the code
// that runs on the device. Run with: pio test -e native

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/button_logic.h"

static const uint16_t DEB = 30;    // cfg debounce window
static const uint16_t LNG = 800;   // cfg::BUTTON_LONG_MS

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Press-length engine
// ---------------------------------------------------------------------------
void test_idle_produces_nothing(void) {
    blogic::State s;
    for (uint32_t t = 0; t < 5000; t += 100) {
        TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(t, true, s, DEB, LNG));
    }
}

void test_short_press_sequence(void) {
    blogic::State s;
    // Press (LOW) at t=100 -> transition line, no event yet.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(100, false, s, DEB, LNG));
    // 5 ms later: inside the debounce window, still nothing.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(105, false, s, DEB, LNG));
    // Stable at exactly the debounce boundary: press is stamped, no event yet.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(130, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT32(130u, s.pressStartMs);
    TEST_ASSERT_TRUE(s.pressed);
    // Held but under the long threshold -> nothing.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(400, false, s, DEB, LNG));
    // Release: transition, then stable HIGH -> SHORT_PRESS exactly once.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(405, true, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::Short, blogic::poll(440, true, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(500, true, s, DEB, LNG));
    TEST_ASSERT_FALSE(s.pressed);
}

void test_long_press_fires_while_held_and_once(void) {
    blogic::State s;
    blogic::poll(100, false, s, DEB, LNG);          // transition
    blogic::poll(130, false, s, DEB, LNG);          // press stamped at 130
    // One ms short of the threshold.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(929, false, s, DEB, LNG));
    // Exactly 800 ms after the stamp -> LONG while still held.
    TEST_ASSERT_EQUAL_UINT8(blogic::Long, blogic::poll(930, false, s, DEB, LNG));
    // No repeat while the button stays down.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(1200, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(2000, false, s, DEB, LNG));
    // Release after a long: no short press is emitted on top of it.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(2100, true, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(2140, true, s, DEB, LNG));
}

void test_bounce_only_shifts_the_window(void) {
    // A 10 ms contact bounce must not create two events, only move the stamp.
    blogic::State s;
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(100, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(108, true,  s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(120, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(155, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT32(155u, s.pressStartMs);        // stamped after the last edge
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(200, true, s, DEB, LNG));   // edge
    TEST_ASSERT_EQUAL_UINT8(blogic::Short, blogic::poll(240, true, s, DEB, LNG));  // stable HIGH
    // ...and the state is clean afterwards.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(300, true, s, DEB, LNG));
}

void test_no_event_inside_debounce_window(void) {
    blogic::State s;
    blogic::poll(1000, false, s, DEB, LNG);
    // Every read is < 30 ms after the level change -> never stabilises.
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(1010, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(1020, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(1029, false, s, DEB, LNG));
    TEST_ASSERT_FALSE(s.pressed);
}

void test_millis_wraparound_arithmetic(void) {
    // The engine uses unsigned deltas, so a wrap must not stall the long press.
    blogic::State s;
    s.lastRaw      = false;
    s.lastChangeMs = 0xFFFFFF00u;
    s.pressed      = false;
    TEST_ASSERT_EQUAL_UINT8(blogic::None, blogic::poll(0x00000040u, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT32(0x00000040u, s.pressStartMs);
    // Stamp a press start just below 2^32 and read past the wrap: 1064 ms later.
    s.pressStartMs = 0xFFFFFF00u;
    TEST_ASSERT_EQUAL_UINT8(blogic::Long, blogic::poll(0x00000328u, false, s, DEB, LNG));
}

void test_two_presses_in_a_row(void) {
    // Chain: short, short, long -- the state machine must reset between holds.
    // Each level change costs two calls: the edge, then the stable read.
    blogic::State s;
    blogic::poll(100,  false, s, DEB, LNG); blogic::poll(135,  false, s, DEB, LNG);
    blogic::poll(200,  true,  s, DEB, LNG);
    TEST_ASSERT_EQUAL_UINT8(blogic::Short, blogic::poll(240, true, s, DEB, LNG));

    blogic::poll(300, false, s, DEB, LNG); blogic::poll(335, false, s, DEB, LNG);
    blogic::poll(400, true,  s, DEB, LNG);
    TEST_ASSERT_EQUAL_UINT8(blogic::Short, blogic::poll(440, true, s, DEB, LNG));

    blogic::poll(500, false, s, DEB, LNG); blogic::poll(535, false, s, DEB, LNG);
    TEST_ASSERT_EQUAL_UINT8(blogic::Long,  blogic::poll(1335, false, s, DEB, LNG));
    TEST_ASSERT_EQUAL_UINT8(blogic::None,  blogic::poll(1400, true,  s, DEB, LNG));
}

// ---------------------------------------------------------------------------
// Screen and picker navigation
// ---------------------------------------------------------------------------
void test_screen_cycle(void) {
    const int kNormal = 4;                 // STATS, GPS, STORAGE, DOSE
    int cur = 0;
    const int kExpect[5] = { 1, 2, 3, 0, 1 };
    for (int i = 0; i < 5; ++i) { cur = blogic::nextScreen(cur, kNormal); TEST_ASSERT_EQUAL_INT(kExpect[i], cur); }
    // Degenerate counts leave the screen alone instead of dividing by zero.
    TEST_ASSERT_EQUAL_INT(2, blogic::nextScreen(2, 0));
    TEST_ASSERT_EQUAL_INT(2, blogic::nextScreen(2, -3));
}

void test_picker_cursor_and_cancel_slot(void) {
    // Three devices -> modulus 4 with index 3 as "Cancel".
    int c = 0;
    const int kExpect[4] = { 1, 2, 3, 0 };
    for (int i = 0; i < 4; ++i) { c = blogic::nextPickerCursor(c, 3); TEST_ASSERT_EQUAL_INT(kExpect[i], c); }
    TEST_ASSERT_FALSE(blogic::pickerCancelSlot(0, 3));
    TEST_ASSERT_FALSE(blogic::pickerCancelSlot(2, 3));
    TEST_ASSERT_TRUE (blogic::pickerCancelSlot(3, 3));
    TEST_ASSERT_TRUE (blogic::pickerCancelSlot(4, 3));   // past the end reads as Cancel too
    // Empty list: the only row is Cancel, and the cursor cannot move.
    TEST_ASSERT_EQUAL_INT(0, blogic::nextPickerCursor(0, 0));
    TEST_ASSERT_TRUE(blogic::pickerCancelSlot(0, 0));
}

// ---------------------------------------------------------------------------
// Dose display scaling
// ---------------------------------------------------------------------------
void test_dose_unit_switch(void) {
    TEST_ASSERT_FALSE(blogic::useMillisievert(0.0f));
    TEST_ASSERT_FALSE(blogic::useMillisievert(999.999f));
    TEST_ASSERT_TRUE (blogic::useMillisievert(1000.0f));   // exact boundary
    TEST_ASSERT_TRUE (blogic::useMillisievert(123456.0f));
}

void test_dose_decimals_fit_five_chars(void) {
    TEST_ASSERT_EQUAL_INT(3, blogic::mantissaDecimals(0.142f));
    TEST_ASSERT_EQUAL_INT(3, blogic::mantissaDecimals(9.999f));
    TEST_ASSERT_EQUAL_INT(2, blogic::mantissaDecimals(10.0f));
    TEST_ASSERT_EQUAL_INT(2, blogic::mantissaDecimals(99.9f));
    TEST_ASSERT_EQUAL_INT(1, blogic::mantissaDecimals(100.0f));
    TEST_ASSERT_EQUAL_INT(1, blogic::mantissaDecimals(999.0f));

    // The 5-char field is the reason the ladder exists: render and measure.
    const float kVals[6] = { 0.142f, 9.999f, 10.0f, 99.94f, 100.0f, 999.0f };
    for (int i = 0; i < 6; ++i) {
        char buf[10];
        snprintf(buf, sizeof(buf), "%5.*f", blogic::mantissaDecimals(kVals[i]),
                 (double)kVals[i]);
        TEST_ASSERT_EQUAL_UINT8(5u, (uint8_t)strlen(buf));
    }
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_idle_produces_nothing);
    RUN_TEST(test_short_press_sequence);
    RUN_TEST(test_long_press_fires_while_held_and_once);
    RUN_TEST(test_bounce_only_shifts_the_window);
    RUN_TEST(test_no_event_inside_debounce_window);
    RUN_TEST(test_millis_wraparound_arithmetic);
    RUN_TEST(test_two_presses_in_a_row);

    RUN_TEST(test_screen_cycle);
    RUN_TEST(test_picker_cursor_and_cancel_slot);

    RUN_TEST(test_dose_unit_switch);
    RUN_TEST(test_dose_decimals_fit_five_chars);

    return UNITY_END();
}
