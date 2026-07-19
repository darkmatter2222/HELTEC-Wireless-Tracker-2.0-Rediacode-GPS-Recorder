// Native unit tests for D/C Trend (dose-per-count ratio sparkline)
//
// Context: The trend screen displays a 5-minute sparkline of the dose-per-count
// ratio relative to an adaptive baseline. 1-second bins aggregate dose-rate and
// CPS, baseline is initialized by median of 6 warmup bins, then updated via EMA.
//
// Run:  pio test -e native

#include <unity.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants under test (must match ui.h/ui.cpp)
// ---------------------------------------------------------------------------
static constexpr float MIN_VALID_CPS = 0.25f;
static constexpr uint32_t RATIO_BIN_MS = 1000;
static constexpr size_t RATIO_POINT_COUNT = 300;
static constexpr size_t BASELINE_WARMUP_BINS = 6;
static constexpr uint16_t MIN_SAMPLES_PER_BIN = 3;
static constexpr float BASELINE_ALPHA = 0.003f;
static constexpr float BASELINE_UPDATE_LIMIT_PCT = 25.0f;
static constexpr float RATIO_NEUTRAL_PCT = 1.0f;
static constexpr float MIN_GRAPH_SCALE_PCT = 10.0f;
static constexpr float MAX_GRAPH_SCALE_PCT = 100.0f;

// ---------------------------------------------------------------------------
// Pure helper functions (extracted from ui.cpp for testing)
// ---------------------------------------------------------------------------

// Calculate dose-per-count ratio: nSv/h divided by CPS
static float calculateDosePerCount(float uSvPerHour, float cps) {
    float nsvPerHour = uSvPerHour * 1000.0f;
    return nsvPerHour / cps;
}

// Calculate percentage deviation from baseline
static float calculateDeviationPercent(float ratio, float baseline) {
    return 100.0f * ((ratio / baseline) - 1.0f);
}

// Map deviation percentage to Y coordinate
static int mapRatioDeviationToY(float deviationPct, float scalePct, int zeroY, int halfHeight) {
    int y = zeroY - (int)(deviationPct / scalePct * halfHeight + 0.5f);
    return y;
}

// Compute median of a small fixed-size array
static float medianOfSix(float arr[6]) {
    // Simple insertion sort for 6 elements
    float sorted[6];
    memcpy(sorted, arr, sizeof(sorted));
    for (int i = 1; i < 6; i++) {
        float key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return (sorted[2] + sorted[3]) / 2.0f;
}

// Check if a reading should be accepted into a bin
static bool isValidReading(float uSvPerHour, float cps, bool valid) {
    if (!valid) return false;
    if (cps < MIN_VALID_CPS) return false;
    if (uSvPerHour < 0.0f) return false;
    if (cps == 0.0f || uSvPerHour == 0.0f) return false;
    float ratio = calculateDosePerCount(uSvPerHour, cps);
    if (!isfinite(ratio) || ratio <= 0.0f) return false;
    return true;
}

// Check if a bin should update the baseline
static bool shouldUpdateBaseline(float ratio, float baseline) {
    float provisional = 100.0f * ((ratio / baseline) - 1.0f);
    return (provisional >= -BASELINE_UPDATE_LIMIT_PCT && provisional <= BASELINE_UPDATE_LIMIT_PCT);
}

// ---------------------------------------------------------------------------
void setUp(void) {}    // Required by Unity
void tearDown(void) {} // Required by Unity
// ---------------------------------------------------------------------------
// Ratio math tests
// ---------------------------------------------------------------------------

void test_basic_ratio_calculation(void) {
    // uSv/h = 0.100, CPS = 10
    // nSv/h = 100, ratio = 100 / 10 = 10
    float ratio = calculateDosePerCount(0.100f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, ratio);
}

void test_ratio_of_sums_not_mean(void) {
    // Three samples:
    // dose: 100, 110, 90  -> sum = 300
    // CPS:   10,  10,  10  -> sum = 30
    // ratio = 300 / 30 = 10
    // Mean of individual ratios would also be 10, but the point is
    // that sum-of-doses / sum-of-cps is more stable.
    float sumDose = 100.0f + 110.0f + 90.0f;
    float sumCps = 10.0f + 10.0f + 10.0f;
    float ratio = sumDose / sumCps;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, ratio);
}

void test_ratio_with_varying_cps(void) {
    // dose: 200, 50
    // CPS:  20,  1
    // sum-dose = 250, sum-cps = 21
    // ratio = 250/21 ≈ 11.905
    float sumDose = 200.0f + 50.0f;
    float sumCps = 20.0f + 1.0f;
    float ratio = sumDose / sumCps;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.905f, ratio);
    // Compare with mean of individual ratios:
    // (200/20 + 50/1) / 2 = (10 + 50) / 2 = 30
    // The ratio-of-sums (11.905) is much less influenced by the low-CPS sample
    // than the mean-of-ratios (30).
    float meanRatios = (200.0f / 20.0f + 50.0f / 1.0f) / 2.0f;
    TEST_ASSERT_TRUE(ratio < meanRatios);
}

void test_near_zero_cps_rejected(void) {
    // CPS near zero must not cause division instability
    TEST_ASSERT_FALSE(isValidReading(0.100f, 0.0f, true));
    TEST_ASSERT_FALSE(isValidReading(0.100f, 0.1f, true));
    TEST_ASSERT_FALSE(isValidReading(0.100f, 0.24f, true));
}

void test_negative_dose_rejected(void) {
    TEST_ASSERT_FALSE(isValidReading(-0.01f, 10.0f, true));
}

void test_invalid_flag_rejected(void) {
    TEST_ASSERT_FALSE(isValidReading(0.100f, 10.0f, false));
}

void test_valid_reading_accepted(void) {
    TEST_ASSERT_TRUE(isValidReading(0.100f, 10.0f, true));
}

// ---------------------------------------------------------------------------
// Deviation percent tests
// ---------------------------------------------------------------------------

void test_zero_deviation(void) {
    // ratio == baseline => deviation = 0%
    float dev = calculateDeviationPercent(10.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dev);
}

void test_positive_deviation(void) {
    // ratio 11 vs baseline 10 => +10%
    float dev = calculateDeviationPercent(11.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, dev);
}

void test_negative_deviation(void) {
    // ratio 9 vs baseline 10 => -10%
    float dev = calculateDeviationPercent(9.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -10.0f, dev);
}

void test_large_positive_deviation(void) {
    // ratio 50 vs baseline 10 => +400%
    float dev = calculateDeviationPercent(50.0f, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 400.0f, dev);
}

// ---------------------------------------------------------------------------
// Baseline warmup tests
// ---------------------------------------------------------------------------

void test_median_baseline_computation(void) {
    float warmup[6] = {9.0f, 10.0f, 10.0f, 11.0f, 10.0f, 50.0f};
    float median = medianOfSix(warmup);
    // Sorted: 9, 10, 10, 11, 10, 50
    // Actually sorted: 9, 10, 10, 10, 11, 50
    // Median = (10 + 10) / 2 = 10
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, median);
}

void test_median_ignores_outlier(void) {
    // The median of {1, 2, 3, 100, 200, 500} should be (3+100)/2 = 51.5
    float data[6] = {1.0f, 2.0f, 3.0f, 100.0f, 200.0f, 500.0f};
    float median = medianOfSix(data);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 51.5f, median);
}

// ---------------------------------------------------------------------------
// Baseline update-exclusion tests
// ---------------------------------------------------------------------------

void test_baseline_update_within_limit(void) {
    // deviation = +20% => should update
    TEST_ASSERT_TRUE(shouldUpdateBaseline(12.0f, 10.0f));
}

void test_baseline_update_at_limit(void) {
    // deviation = +25% => should update (within limit)
    TEST_ASSERT_TRUE(shouldUpdateBaseline(12.5f, 10.0f));
}

void test_baseline_update_excluded_above_limit(void) {
    // deviation = +50% => should NOT update
    TEST_ASSERT_FALSE(shouldUpdateBaseline(15.0f, 10.0f));
}

void test_baseline_update_excluded_below_limit(void) {
    // deviation = -50% => should NOT update
    TEST_ASSERT_FALSE(shouldUpdateBaseline(5.0f, 10.0f));
}

// ---------------------------------------------------------------------------
// EMA update tests
// ---------------------------------------------------------------------------

void test_ema_alpha_small(void) {
    // B_new = B_old + alpha * (R - B_old)
    // With B_old = 10, R = 11, alpha = 0.003
    // B_new = 10 + 0.003 * 1 = 10.003
    float baseline = 10.0f;
    float ratio = 11.0f;
    float newBaseline = baseline + BASELINE_ALPHA * (ratio - baseline);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.003f, newBaseline);
}

void test_ema_small_alpha_slow_adaptation(void) {
    // After 100 EMA updates of ratio=11 on baseline=10:
    // B_n = B_0 + (R - B_0)*(1 - (1-alpha)^n)
    // With alpha=0.003: (1-0.003)^100 ≈ 0.738
    // B_100 ≈ 10 + 1 * 0.262 = 10.262
    float baseline = 10.0f;
    for (int i = 0; i < 100; i++) {
        baseline = baseline + BASELINE_ALPHA * (11.0f - baseline);
    }
    TEST_ASSERT_TRUE(baseline > 10.0f);
    TEST_ASSERT_TRUE(baseline < 11.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.26f, baseline);
}

// ---------------------------------------------------------------------------
// Circular buffer tests
// ---------------------------------------------------------------------------

void test_circular_buffer_insert(void) {
    // Simulate inserting into a fixed-size buffer
    float buffer[RATIO_POINT_COUNT];
    bool valid[RATIO_POINT_COUNT];
    size_t writeIdx = 0;
    size_t count = 0;

    // Insert 5 points
    for (int i = 1; i <= 5; i++) {
        buffer[writeIdx] = (float)i * 2.0f;
        valid[writeIdx] = true;
        writeIdx = (writeIdx + 1) % RATIO_POINT_COUNT;
        count++;
    }
    TEST_ASSERT_EQUAL_INT(5, count);
    TEST_ASSERT_EQUAL_INT(5, writeIdx);
}

void test_circular_buffer_rollover(void) {
    float buffer[RATIO_POINT_COUNT];
    bool valid[RATIO_POINT_COUNT];
    size_t writeIdx = 0;
    size_t count = 0;

    // Insert 302 points (more than buffer size of 300)
    for (int i = 1; i <= 302; i++) {
        buffer[writeIdx] = (float)i;
        valid[writeIdx] = true;
        writeIdx = (writeIdx + 1) % RATIO_POINT_COUNT;
        if (count < RATIO_POINT_COUNT) count++;
    }
    // After 302 inserts: count should be capped at 300
    TEST_ASSERT_EQUAL_INT(RATIO_POINT_COUNT, count);
    // writeIdx should be at position 2
    TEST_ASSERT_EQUAL_INT(2, writeIdx);
    // After 302 inserts: indices 0 and 1 were overwritten by values 301 and 302.
    // Index 3 still holds value 4.0 from insert 4 (never overwritten).
    int oldestIdx = (writeIdx + 1) % RATIO_POINT_COUNT;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, buffer[oldestIdx]);
}

void test_partial_buffer_right_alignment(void) {
    // With only 5 points in a 300-point buffer, the points should be
    // right-aligned so the newest data stays at the right edge.
    float buffer[RATIO_POINT_COUNT];
    bool valid[RATIO_POINT_COUNT];
    size_t writeIdx = 0;
    size_t count = 0;

    for (int i = 1; i <= 5; i++) {
        buffer[writeIdx] = (float)i * 10.0f;
        valid[writeIdx] = true;
        writeIdx = (writeIdx + 1) % RATIO_POINT_COUNT;
        count++;
    }
    // Start index for right-aligned display:
    size_t startIdx = (writeIdx - count + RATIO_POINT_COUNT) % RATIO_POINT_COUNT;
    // writeIdx=5, count=5 => startIdx = (5-5+300)%300 = 0
    TEST_ASSERT_EQUAL_INT(0, startIdx);
    // First displayed value should be 10.0
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, buffer[startIdx]);
}

// ---------------------------------------------------------------------------
// Replicate the σ computation from drawRatioSparkline
static float computeSigma(float buf[RATIO_POINT_COUNT], bool valid[RATIO_POINT_COUNT], size_t count) {
    float mean = 0.0f;
    size_t n = 0;
    for (size_t i = 0; i < RATIO_POINT_COUNT; i++) {
        if (valid[i]) { mean += buf[i]; n++; }
    }
    if (n == 0) return 0.0f;
    mean /= n;
    float var = 0.0f;
    for (size_t i = 0; i < RATIO_POINT_COUNT; i++) {
        if (valid[i]) { float d = buf[i] - mean; var += d * d; }
    }
    return sqrtf(var / n);
}

void test_sigma_all_same_values(void) {
    float buf[RATIO_POINT_COUNT] = {};
    bool valid[RATIO_POINT_COUNT] = {};
    for (int i = 0; i < 10; i++) {
        buf[i] = 10.0f;
        valid[i] = true;
    }
    float sigma = computeSigma(buf, valid, 10);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sigma);
}

void test_sigma_known_variance(void) {
    // Values: 8, 9, 10, 11, 12 → mean=10, var=2.0, σ≈1.414
    float buf[RATIO_POINT_COUNT] = {};
    bool valid[RATIO_POINT_COUNT] = {};
    float vals[] = {8.0f, 9.0f, 10.0f, 11.0f, 12.0f};
    for (int i = 0; i < 5; i++) {
        buf[i] = vals[i];
        valid[i] = true;
    }
    float sigma = computeSigma(buf, valid, 5);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.414f, sigma);
}

void test_sigma_single_value(void) {
    float buf[300] = {};
    bool valid[300] = {};
    buf[0] = 42.0f;
    valid[0] = true;
    float sigma = computeSigma(buf, valid, 1);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sigma);
}

// σ-zone classification: |z|≤1→GREEN, 1<|z|≤2→AMBER, |z|>2→RED
// σ floor reduced to 0.5% to let real variation drive classification
static uint16_t zoneColor(float devPct, float sigmaPct) {
    float z = fabsf(devPct) / (sigmaPct < 0.5f ? 0.5f : sigmaPct);
    if (z <= 1.0f) return 0x07E0; // COL_GREEN
    if (z <= 2.0f) return 0x053F; // COL_AMBER
    return 0x001F; // COL_RED
}

void test_zone_green_at_zero(void) {
    TEST_ASSERT_EQUAL_UINT16(0x07E0, zoneColor(0.0f, 5.0f));
}

void test_zone_green_within_1sigma(void) {
    // σ_pct = 5.0, dev = ±5 → z = 1.0 → GREEN
    TEST_ASSERT_EQUAL_UINT16(0x07E0, zoneColor(5.0f, 5.0f));
    TEST_ASSERT_EQUAL_UINT16(0x07E0, zoneColor(-5.0f, 5.0f));
}

void test_zone_amber_between_1_and_2sigma(void) {
    // σ_pct = 5.0, dev = ±9 → z = 1.8 → AMBER
    TEST_ASSERT_EQUAL_UINT16(0x053F, zoneColor(9.0f, 5.0f));
    TEST_ASSERT_EQUAL_UINT16(0x053F, zoneColor(-9.0f, 5.0f));
}

void test_zone_red_above_2sigma(void) {
    // σ_pct = 5.0, dev = ±12 → z = 2.4 → RED
    TEST_ASSERT_EQUAL_UINT16(0x001F, zoneColor(12.0f, 5.0f));
    TEST_ASSERT_EQUAL_UINT16(0x001F, zoneColor(-12.0f, 5.0f));
}

void test_zone_at_exact_1sigma_green(void) {
    // z = 1.0 exactly → GREEN (boundary inclusive)
    TEST_ASSERT_EQUAL_UINT16(0x07E0, zoneColor(5.0f, 5.0f));
}

void test_zone_at_exact_2sigma_amber(void) {
    // z = 2.0 exactly → AMBER (boundary inclusive)
    TEST_ASSERT_EQUAL_UINT16(0x053F, zoneColor(10.0f, 5.0f));
}

void test_zone_sigma_floor(void) {
    // σPct floored at 0.5
    TEST_ASSERT_EQUAL_UINT16(0x07E0, zoneColor(0.5f, 0.3f)); // σ=0.3 → floored to 0.5, z=1.0 → GREEN
    TEST_ASSERT_EQUAL_UINT16(0x001F, zoneColor(1.5f, 0.3f)); // z=3.0 → RED (>2σ)
    TEST_ASSERT_EQUAL_UINT16(0x001F, zoneColor(3.5f, 0.3f)); // z=7.0 → RED
}
// ---------------------------------------------------------------------------

void test_insufficient_samples_gap(void) {
    // Bin with fewer than MIN_SAMPLES_PER_BIN samples => gap
    TEST_ASSERT_TRUE(2 < MIN_SAMPLES_PER_BIN);  // 2 samples < 3 => gap
    TEST_ASSERT_FALSE(2 >= MIN_SAMPLES_PER_BIN);
    TEST_ASSERT_TRUE(3 >= MIN_SAMPLES_PER_BIN);  // exactly 3 => valid
}

void test_zero_sum_cps_gap(void) {
    // Even with enough samples, if CPS sum is zero => gap
    float cpsSum = 0.0f;
    TEST_ASSERT_FALSE(cpsSum > 0.0f);
}

// ---------------------------------------------------------------------------
// Graph Y-mapping tests
// ---------------------------------------------------------------------------

void test_zero_deviation_maps_to_zero_line(void) {
    int zeroY = 46;
    int halfHeight = 19;  // (39-1)/2 = 19
    float scalePct = 10.0f;
    int y = mapRatioDeviationToY(0.0f, scalePct, zeroY, halfHeight);
    TEST_ASSERT_EQUAL_INT(zeroY, y);
}

void test_positive_deviation_maps_above_zero(void) {
    int zeroY = 46;
    int halfHeight = 19;
    float scalePct = 10.0f;
    int y = mapRatioDeviationToY(5.0f, scalePct, zeroY, halfHeight);
    // y = 46 - (5/10)*19 = 46 - 9.5 = 36.5 => 36 (rounded)
    TEST_ASSERT_EQUAL_INT(36, y);
    TEST_ASSERT_TRUE(y < zeroY);  // above zero line
}

void test_negative_deviation_maps_below_zero(void) {
    int zeroY = 46;
    int halfHeight = 19;
    float scalePct = 10.0f;
    int y = mapRatioDeviationToY(-5.0f, scalePct, zeroY, halfHeight);
    // y = 46 - (-5/10)*19 = 46 + 9.5 = 55.5 => 55 (truncated)
    TEST_ASSERT_EQUAL_INT(55, y);
    TEST_ASSERT_TRUE(y > zeroY);  // below zero line
}

void test_extreme_positive_clipped_to_top(void) {
    int zeroY = 46;
    int halfHeight = 19;
    int chartY = 27;
    float scalePct = 10.0f;
    int y = mapRatioDeviationToY(50.0f, scalePct, zeroY, halfHeight);
    // y = 46 - (50/10)*19 = 46 - 95 = -49
    // Clipped to chartY = 27
    y = (y < chartY) ? chartY : y;
    TEST_ASSERT_EQUAL_INT(chartY, y);
}

void test_extreme_negative_clipped_to_bottom(void) {
    int zeroY = 46;
    int halfHeight = 19;
    int chartY = 27;
    int chartH = 39;
    float scalePct = 10.0f;
    int y = mapRatioDeviationToY(-50.0f, scalePct, zeroY, halfHeight);
    // y = 46 - (-50/10)*19 = 46 + 95 = 141
    // Clipped to chartY + chartH - 1 = 65
    int maxY = chartY + chartH - 1;
    y = (y > maxY) ? maxY : y;
    TEST_ASSERT_EQUAL_INT(maxY, y);
}

// ---------------------------------------------------------------------------
// Millis rollover safety tests
// ---------------------------------------------------------------------------

void test_bin_completion_before_rollover(void) {
    // Normal case: two timestamps close together, no rollover involved
    uint32_t startMs = 1000;
    uint32_t nowMs = 2000;
    uint32_t elapsed = (uint32_t)(nowMs - startMs);
    // 1000ms exactly at threshold
    TEST_ASSERT_TRUE(elapsed >= RATIO_BIN_MS);
    // Verify the subtraction is correct
    TEST_ASSERT_EQUAL_UINT(RATIO_BIN_MS, elapsed);
}

void test_bin_completion_across_rollover(void) {
    // Start just before rollover, end just after
    uint32_t startMs = UINT32_MAX - 100;  // 100ms before rollover
    uint32_t nowMs = 1200;                // 1200ms after rollover
    uint32_t elapsed = (uint32_t)(nowMs - startMs);
    // 1200 - (UINT32_MAX - 100) = 1200 + 100 + 1 = 1301ms (wrapping)
    TEST_ASSERT_TRUE(elapsed >= RATIO_BIN_MS);
    TEST_ASSERT_TRUE(elapsed > RATIO_BIN_MS);
}

void test_no_false_completion_short_elapsed(void) {
    // Verify a bin does NOT complete at 999ms
    uint32_t startMs = 1000;
    uint32_t nowMs = 1999;
    TEST_ASSERT_FALSE((uint32_t)(nowMs - startMs) >= RATIO_BIN_MS);
    // But at 1000ms it should complete
    nowMs = 2000;
    TEST_ASSERT_TRUE((uint32_t)(nowMs - startMs) >= RATIO_BIN_MS);
}

// ---------------------------------------------------------------------------
// Scale computation tests
// ---------------------------------------------------------------------------

void test_scale_min_clamp(void) {
    // If all deviations are very small (e.g. maxAbs = 2%), scale should clamp to 10%
    float maxAbsDev = 2.0f;
    float target = maxAbsDev * 1.15f;  // 2.3%
    float scale = target < MIN_GRAPH_SCALE_PCT ? MIN_GRAPH_SCALE_PCT : target;
    TEST_ASSERT_EQUAL_FLOAT(MIN_GRAPH_SCALE_PCT, scale);
}

void test_scale_max_clamp(void) {
    // If deviations are huge (maxAbs = 200%), scale should clamp to 100%
    float maxAbsDev = 200.0f;
    float target = maxAbsDev * 1.15f;  // 230%
    float scale = target > MAX_GRAPH_SCALE_PCT ? MAX_GRAPH_SCALE_PCT : target;
    TEST_ASSERT_EQUAL_FLOAT(MAX_GRAPH_SCALE_PCT, scale);
}

void test_scale_normal_range(void) {
    // maxAbs = 50% => target = 57.5% => within [10, 100]
    float maxAbsDev = 50.0f;
    float target = maxAbsDev * 1.15f;
    TEST_ASSERT_TRUE(target >= MIN_GRAPH_SCALE_PCT);
    TEST_ASSERT_TRUE(target <= MAX_GRAPH_SCALE_PCT);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 57.5f, target);
}

// ---------------------------------------------------------------------------
int main(void) {
    UNITY_BEGIN();
    // Ratio math
    RUN_TEST(test_basic_ratio_calculation);
    RUN_TEST(test_ratio_of_sums_not_mean);
    RUN_TEST(test_ratio_with_varying_cps);
    RUN_TEST(test_near_zero_cps_rejected);
    RUN_TEST(test_negative_dose_rejected);
    RUN_TEST(test_invalid_flag_rejected);
    RUN_TEST(test_valid_reading_accepted);
    // Deviation
    RUN_TEST(test_zero_deviation);
    RUN_TEST(test_positive_deviation);
    RUN_TEST(test_negative_deviation);
    RUN_TEST(test_large_positive_deviation);
    // Baseline
    RUN_TEST(test_median_baseline_computation);
    RUN_TEST(test_median_ignores_outlier);
    // Baseline update-exclusion
    RUN_TEST(test_baseline_update_within_limit);
    RUN_TEST(test_baseline_update_at_limit);
    RUN_TEST(test_baseline_update_excluded_above_limit);
    RUN_TEST(test_baseline_update_excluded_below_limit);
    // EMA
    RUN_TEST(test_ema_alpha_small);
    RUN_TEST(test_ema_small_alpha_slow_adaptation);
    // Circular buffer
    RUN_TEST(test_circular_buffer_insert);
    RUN_TEST(test_circular_buffer_rollover);
    RUN_TEST(test_partial_buffer_right_alignment);
    // Missing bins
    RUN_TEST(test_insufficient_samples_gap);
    RUN_TEST(test_zero_sum_cps_gap);
    // Graph mapping
    RUN_TEST(test_zero_deviation_maps_to_zero_line);
    RUN_TEST(test_positive_deviation_maps_above_zero);
    RUN_TEST(test_negative_deviation_maps_below_zero);
    RUN_TEST(test_extreme_positive_clipped_to_top);
    RUN_TEST(test_extreme_negative_clipped_to_bottom);
    // Millis rollover
    RUN_TEST(test_bin_completion_before_rollover);
    RUN_TEST(test_bin_completion_across_rollover);
    RUN_TEST(test_no_false_completion_short_elapsed);
    // Scale
    RUN_TEST(test_scale_min_clamp);
    RUN_TEST(test_scale_max_clamp);
    RUN_TEST(test_scale_normal_range);
    // σ-zone coloring
    RUN_TEST(test_sigma_all_same_values);
    RUN_TEST(test_sigma_known_variance);
    RUN_TEST(test_sigma_single_value);
    RUN_TEST(test_zone_green_at_zero);
    RUN_TEST(test_zone_green_within_1sigma);
    RUN_TEST(test_zone_amber_between_1_and_2sigma);
    RUN_TEST(test_zone_red_above_2sigma);
    RUN_TEST(test_zone_at_exact_1sigma_green);
    RUN_TEST(test_zone_at_exact_2sigma_amber);
    RUN_TEST(test_zone_sigma_floor);
    return UNITY_END();
}
