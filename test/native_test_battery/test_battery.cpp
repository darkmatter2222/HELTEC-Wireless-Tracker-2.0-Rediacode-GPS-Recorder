// Native unit tests for the LiPo battery voltage-to-percent interpolation.
//
// The interpolation table and algorithm live in main.cpp.  These tests verify:
//   1. Exact table entries are reproduced correctly.
//   2. Values above and below the table are clamped (no under-/overflow).
//   3. The function is monotonically non-decreasing: higher voltage => higher
//      (or equal) battery percent.  This is the critical correctness property.
//   4. All outputs are in [0, 100].
//
// Run:  pio test -e native
#include <unity.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Replicate kLipoTable and batteryPercent() from main.cpp.
// ---------------------------------------------------------------------------
static const struct { float v; int pct; } kLipoTable[] = {
    {4.20f, 100}, {4.17f,  97}, {4.14f,  94}, {4.11f,  91},
    {4.08f,  87}, {4.05f,  83}, {4.02f,  79}, {3.98f,  74},
    {3.95f,  70}, {3.91f,  65}, {3.87f,  60}, {3.83f,  55},
    {3.79f,  50}, {3.75f,  45}, {3.71f,  40}, {3.67f,  35},
    {3.61f,  29}, {3.55f,  23}, {3.49f,  17}, {3.42f,  12},
    {3.36f,   7}, {3.30f,   3}, {3.27f,   0},
};
static const int kLipoTableLen = (int)(sizeof(kLipoTable) / sizeof(kLipoTable[0]));

static int batteryPercent(float volts) {
    if (volts >= kLipoTable[0].v)               return kLipoTable[0].pct;
    if (volts <= kLipoTable[kLipoTableLen-1].v) return kLipoTable[kLipoTableLen-1].pct;
    for (int i = 0; i < kLipoTableLen - 1; i++) {
        if (volts >= kLipoTable[i+1].v) {
            float span = kLipoTable[i].v - kLipoTable[i+1].v;
            float frac = (volts - kLipoTable[i+1].v) / span;
            return (int)(kLipoTable[i+1].pct
                        + frac * (kLipoTable[i].pct - kLipoTable[i+1].pct)
                        + 0.5f);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

void test_full_charge(void) {
    TEST_ASSERT_EQUAL(100, batteryPercent(4.20f));
}

void test_above_full_clamped(void) {
    // Overcharged / measurement noise should not exceed 100 %.
    TEST_ASSERT_EQUAL(100, batteryPercent(4.50f));
    TEST_ASSERT_EQUAL(100, batteryPercent(5.00f));
}

void test_dead_battery(void) {
    TEST_ASSERT_EQUAL(0, batteryPercent(3.27f));
}

void test_below_dead_clamped(void) {
    // Deeply discharged cell should not return negative.
    TEST_ASSERT_EQUAL(0, batteryPercent(3.00f));
    TEST_ASSERT_EQUAL(0, batteryPercent(0.00f));
}

void test_midpoint_50pct(void) {
    // 3.79 V is an exact table entry -> 50 %.
    TEST_ASSERT_EQUAL(50, batteryPercent(3.79f));
}

void test_exact_table_entry_97(void) {
    TEST_ASSERT_EQUAL(97, batteryPercent(4.17f));
}

void test_exact_table_entry_65(void) {
    TEST_ASSERT_EQUAL(65, batteryPercent(3.91f));
}

void test_exact_table_entry_7(void) {
    TEST_ASSERT_EQUAL(7, batteryPercent(3.36f));
}

void test_all_outputs_in_range(void) {
    // Coarse sweep: no output should be outside [0, 100].
    for (int i = 0; i <= 200; ++i) {
        float v = 2.50f + (float)i * 0.01f;  // 2.50 V .. 4.50 V
        int p = batteryPercent(v);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, p, "Below 0");
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(100, p, "Above 100");
    }
}

void test_monotone_increasing(void) {
    // For every 0.01 V step from dead (3.00 V) to full (4.30 V),
    // percent must be >= the previous step.  A regression in the
    // interpolation formula would violate this property.
    float v = 3.00f;
    int prev = batteryPercent(v);
    while (v < 4.31f) {
        v += 0.01f;
        int cur = batteryPercent(v);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(prev, cur,
            "Percent decreased as voltage increased");
        prev = cur;
    }
}

void test_interpolation_between_entries(void) {
    // Midpoint between 3.87 V (60 %) and 3.83 V (55 %) should be ~57-58 %.
    float mid = (3.87f + 3.83f) / 2.0f;  // 3.85 V
    int p = batteryPercent(mid);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(55, p);
    TEST_ASSERT_LESS_OR_EQUAL_INT(60, p);
}

// ---------------------------------------------------------------------------
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_charge);
    RUN_TEST(test_above_full_clamped);
    RUN_TEST(test_dead_battery);
    RUN_TEST(test_below_dead_clamped);
    RUN_TEST(test_midpoint_50pct);
    RUN_TEST(test_exact_table_entry_97);
    RUN_TEST(test_exact_table_entry_65);
    RUN_TEST(test_exact_table_entry_7);
    RUN_TEST(test_all_outputs_in_range);
    RUN_TEST(test_monotone_increasing);
    RUN_TEST(test_interpolation_between_entries);
    return UNITY_END();
}
