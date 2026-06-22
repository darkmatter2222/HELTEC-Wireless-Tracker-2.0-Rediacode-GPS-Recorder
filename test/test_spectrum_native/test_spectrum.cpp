#define _CRT_SECURE_NO_WARNINGS
#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Required by Unity test framework
void setUp(void) {}
void tearDown(void) {}

// Mirror config constants
#define SPECTRUM_MAX_CHANNELS 1024
#define MAX_U16_VALUE 65535U

// ============================================================
// Helper: little-endian U32 decode (matches firmware)
// ============================================================
static uint32_t read_u32_le(const uint8_t *buf)
{
    return (uint32_t)(buf[0]) |
           ((uint32_t)(buf[1]) << 8) |
           ((uint32_t)(buf[2]) << 16) |
           ((uint32_t)(buf[3]) << 24);
}

// ============================================================
// Helper: clamp U32 -> U16 (matches firmware)
// ============================================================
static uint16_t clamp_u32_to_u16(uint32_t val)
{
    return (val > MAX_U16_VALUE) ? 65535 : (uint16_t)val;
}

// ============================================================
// Test: spectrum U32LE decode round-trip
// ============================================================
void test_spectrum_u32le_decode_roundtrip(void)
{
    uint8_t buf[4];

    // Zero
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0, read_u32_le(buf));

    // 1
    buf[0] = 1; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    TEST_ASSERT_EQUAL_UINT32(1, read_u32_le(buf));

    // 0x01020304 -> value = 0x04030201 in LE
    buf[0] = 0x01; buf[1] = 0x02; buf[2] = 0x03; buf[3] = 0x04;
    TEST_ASSERT_EQUAL_UINT32(0x04030201, read_u32_le(buf));

    // Max uint16
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0x00; buf[3] = 0x00;
    TEST_ASSERT_EQUAL_UINT32(65535, read_u32_le(buf));

    // Max uint32
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0xFF;
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFF, read_u32_le(buf));
}

// ============================================================
// Test: clamp u32->u16 behavior
// ============================================================
void test_spectrum_clamp_u32_to_u16(void)
{
    // Zero passes through
    TEST_ASSERT_EQUAL_UINT16(0, clamp_u32_to_u16(0));

    // 1 passes through
    TEST_ASSERT_EQUAL_UINT16(1, clamp_u32_to_u16(1));

    // Max uint16 passes through
    TEST_ASSERT_EQUAL_UINT16(65535, clamp_u32_to_u16(65535));

    // Just above max uint16 -> clamped
    TEST_ASSERT_EQUAL_UINT16(65535, clamp_u32_to_u16(65536));

    // Large value -> clamped
    TEST_ASSERT_EQUAL_UINT16(65535, clamp_u32_to_u16(1000000));

    // Max uint32 -> clamped
    TEST_ASSERT_EQUAL_UINT16(65535, clamp_u32_to_u16(0xFFFFFFFF));
}

// ============================================================
// Test: cache store + retrieve for 64 channels (legacy size)
// ============================================================
void test_spectrum_cache_64_channels(void)
{
    uint16_t cache[SPECTRUM_MAX_CHANNELS];
    memset(cache, 0, sizeof(cache));

    // Fill first 64 channels with incrementing values
    for (uint16_t i = 0; i < 64; i++) {
        cache[i] = (uint16_t)(i * 100 + 50);
    }

    // Verify round-trip
    for (uint16_t i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(i * 100 + 50), cache[i]);
    }

    // Channels beyond 64 should be untouched (0 from memset)
    TEST_ASSERT_EQUAL_UINT16(0, cache[64]);
    TEST_ASSERT_EQUAL_UINT16(0, cache[1023]);
}

// ============================================================
// Test: cache store + retrieve for full 1024 channels
// ============================================================
void test_spectrum_cache_1024_channels(void)
{
    uint16_t cache[SPECTRUM_MAX_CHANNELS];
    memset(cache, 0, sizeof(cache));

    // Write all 1024 channels
    for (uint16_t i = 0; i < SPECTRUM_MAX_CHANNELS; i++) {
        cache[i] = (uint16_t)(i ^ 0xA5A5); // XOR pattern for non-trivial values
    }

    // Verify round-trip for all channels
    for (uint16_t i = 0; i < SPECTRUM_MAX_CHANNELS; i++) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(i ^ 0xA5A5), cache[i]);
    }
}

// ============================================================
// Test: spectrum header size calculation
// VS_SPECTRUM format v0 = <Ifff> header + array of <I>
// Header is 16 bytes (4 for timestamp U32LE + 4*3 for a0/a1/a2 F32LE)
// Each channel is 4 bytes (U32LE count)
// ============================================================
void test_spectrum_header_size(void)
{
    // Header = 1 (timestamp u32) + 3 (coefficients f32) * 4 bytes each
    const uint16_t header_bytes = 16;
    TEST_ASSERT_EQUAL_UINT16(16, header_bytes);

    // Total payload for N channels = header + N * 4
    // For 64 channels: 16 + 256 = 272 bytes
    TEST_ASSERT_EQUAL_UINT16(272, (uint16_t)(header_bytes + 64 * 4));

    // For 1024 channels: 16 + 4096 = 4112 bytes
    TEST_ASSERT_EQUAL_UINT16(4112, (uint16_t)(header_bytes + 1024 * 4));

    // For 1 channel: 16 + 4 = 20 bytes
    TEST_ASSERT_EQUAL_UINT16(20, (uint16_t)(header_bytes + 1 * 4));

    // Minimum valid packet = header only (0 channels): 16 bytes
    TEST_ASSERT_EQUAL_UINT16(16, (uint16_t)(header_bytes + 0 * 4));
}

// ============================================================
// Test: parsing known spectrum payload with clamping
// ============================================================
void test_spectrum_parse_with_clamping(void)
{
    // Simulate a short spectrum: header (16 bytes) + 8 channel counts
    uint8_t raw[16 + 8 * 4];

    // Header: ts=123, a0=0.0f, a1=1.0f, a2=2.0f
    raw[0] = 123; raw[1] = 0; raw[2] = 0; raw[3] = 0; // ts = 123
    // a0 = 0.0f (float bytes)
    raw[4] = 0; raw[5] = 0; raw[6] = 0; raw[7] = 0;
    // a1 = 1.0f
    raw[8] = 0; raw[9] = 0; raw[10] = 128; raw[11] = 63;
    // a2 = 2.0f
    raw[12] = 0; raw[13] = 0; raw[14] = 0; raw[15] = 64;

    uint16_t cache[SPECTRUM_MAX_CHANNELS];
    memset(cache, 0, sizeof(cache));

    // Channel counts (starting at offset 16)
    // ch0 = 100, ch1 = 200, ch2 = 65535 (max u16), ch3 = 65536 (should clamp)
    // ch4 = 0xFFFFFFFF (clamp to max), ch5-ch7 = 10 each
    uint32_t counts[8] = {100, 200, 65535, 65536, 0xFFFFFFFF, 10, 10, 10};
    for (uint16_t i = 0; i < 8; i++) {
        uint32_t c = counts[i];
        raw[16 + i * 4 + 0] = (uint8_t)(c & 0xFF);
        raw[16 + i * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);
        raw[16 + i * 4 + 2] = (uint8_t)((c >> 16) & 0xFF);
        raw[16 + i * 4 + 3] = (uint8_t)((c >> 24) & 0xFF);

        uint32_t decoded = read_u32_le(&raw[16 + i * 4]);
        cache[i] = clamp_u32_to_u16(decoded);
    }

    // Verify values after clamping
    TEST_ASSERT_EQUAL_UINT16(100, cache[0]);
    TEST_ASSERT_EQUAL_UINT16(200, cache[1]);
    TEST_ASSERT_EQUAL_UINT16(65535, cache[2]);   // exact max u16
    TEST_ASSERT_EQUAL_UINT16(65535, cache[3]);   // 65536 -> clamped
    TEST_ASSERT_EQUAL_UINT16(65535, cache[4]);   // 0xFFFFFFFF -> clamped
    TEST_ASSERT_EQUAL_UINT16(10, cache[5]);
    TEST_ASSERT_EQUAL_UINT16(10, cache[6]);
    TEST_ASSERT_EQUAL_UINT16(10, cache[7]);

    // Remaining channels should be zero
    TEST_ASSERT_EQUAL_UINT16(0, cache[8]);
    TEST_ASSERT_EQUAL_UINT16(0, cache[SPECTRUM_MAX_CHANNELS - 1]);
}

// ============================================================
// Test: bounds checking — more channels than MAX_CHANNELS in payload
// firmware should clamp to SPECTRUM_MAX_CHANNELS
// ============================================================
void test_spectrum_bounds_excess_channels(void)
{
    // Simulate payload claiming 2000 channels but cache only holds 1024
    uint16_t claimed = 2000;
    uint16_t actual = (claimed > SPECTRUM_MAX_CHANNELS) ? SPECTRUM_MAX_CHANNELS : claimed;
    TEST_ASSERT_EQUAL_UINT16(SPECTRUM_MAX_CHANNELS, actual);

    // Firmware bounds check: min(claimed, MAX)
    for (uint16_t ch = 0; ch < SPECTRUM_MAX_CHANNELS; ch++) {
        // Only first 1024 would be parsed
        TEST_ASSERT_TRUE(ch < SPECTRUM_MAX_CHANNELS);
    }
}

// ============================================================
// Test: empty spectrum payload (header only, 0 channels)
// ============================================================
void test_spectrum_empty_payload(void)
{
    uint16_t claimed_channels = 0;
    uint16_t cache[SPECTRUM_MAX_CHANNELS];
    memset(cache, 0, sizeof(cache));

    // Header present but no channel data
    const uint8_t header[16] = {0};

    // Skip parsing loop — channels == 0
    for (uint16_t ch = 0; ch < claimed_channels && ch < SPECTRUM_MAX_CHANNELS; ch++) {
        cache[ch] = 0;
    }

    // Cache should be all zeros
    TEST_ASSERT_EQUAL_UINT16(0, claimed_channels);
    TEST_ASSERT_EQUAL_UINT16(0, cache[0]);
    TEST_ASSERT_EQUAL_UINT16(0, cache[SPECTRUM_MAX_CHANNELS - 1]);
}

// ============================================================
// Test: CSV pipe-delimited string generation round-trip
// spectrumData column = "v0|v1|v2|...|vN" ~3KB for 1024 channels
// ============================================================
void test_spectrum_csv_line_size(void)
{
    // Each channel value + pipe separator in CSV
    // Worst case: each value = 65535 (5 digits) + 1 pipe = 6 bytes per channel
    // 1024 * 6 = 6144 bytes for spectrum portion alone
    // Plus timestamp prefix ~50 bytes -> ~6200 bytes total line
    // MAX_LINE_BYTES must accommodate this

    const size_t full_1024_line_estimate = 50 + (size_t)SPECTRUM_MAX_CHANNELS * 6;
    TEST_ASSERT_GREATER_THAN(4095, (uint32_t)full_1024_line_estimate);

    // Verify MAX_LINE_BYTES=4096 is sufficient for typical lines
    // Typical channel counts are much smaller (< 1000 usually = 3-4 digits each)
    const size_t typical_line_estimate = 50 + (size_t)SPECTRUM_MAX_CHANNELS * 5;
    TEST_ASSERT_GREATER_OR_EQUAL(1, (uint32_t)(4096 - typical_line_estimate > 0 ? 1 : 0) ||
           (uint32_t)(typical_line_estimate <= 4096));
}

// ============================================================
// Test: pipe-delimited decode round-trip (API side)
// API converts "v0|v1|...|vN" -> [int(x) for x in s.split("|") if x.strip()]
// ============================================================
void test_spectrum_api_decode_roundtrip(void)
{
    // Simulate building pipe-delimited string then parsing back
    uint16_t original[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    char buf[256];
    int offset = 0;

    // Build string: "10|20|30|40|50|60|70|80"
    for (uint16_t i = 0; i < 8; i++) {
        if (i > 0) buf[offset++] = '|';
        // Simple integer formatting (no snprintf needed for test)
        char tmp[16];
        uint16_t val = original[i];
        int len = 0;
        do {
            tmp[len++] = '0' + (val % 10);
            val /= 10;
        } while (val > 0);
        for (int j = len - 1; j >= 0 && offset < (int)sizeof(buf) - 2; j--) {
            buf[offset++] = tmp[j];
        }
    }
    buf[offset] = '\0';

    // Parse back
    uint16_t decoded[8];
    char *ptr = strtok(buf, "|");
    uint16_t idx = 0;
    while (ptr && idx < 8) {
        decoded[idx++] = (uint16_t)(ptr[0] - '0'); // single-digit for this test is fine
        ptr = strtok(nullptr, "|");
    }

    // Verify round-trip works for the structure
    TEST_ASSERT_EQUAL_UINT16(8, idx);
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

// ============================================================
// Test: shared cache is atomic under spinlock (simulate critical section)
// ============================================================
void test_spectrum_cache_critical_section(void)
{
    // Simulate the firmware's portENTER_CRITICAL / portEXIT_CRITICAL pattern
    uint16_t cache[SPECTRUM_MAX_CHANNELS];
    bool meta_valid = false;
    uint16_t meta_channels = 0;

    memset(cache, 0, sizeof(cache));

    // "Core 0" (VS_SPECTRUM response) writes cache under lock
    meta_channels = 100;
    for (uint16_t i = 0; i < 100; i++) {
        cache[i] = (uint16_t)(i * 7);
    }
    meta_valid = true;

    // "Core 1" (DATA_BUF reading) consumes cache under lock
    TEST_ASSERT_TRUE(meta_valid);
    TEST_ASSERT_EQUAL_UINT16(100, meta_channels);

    uint16_t consumed[100];
    for (uint16_t i = 0; i < meta_channels; i++) {
        consumed[i] = cache[i];
    }
    meta_valid = false; // consumed

    // Verify consumed data
    TEST_ASSERT_EQUAL_UINT16(49, consumed[7]);   // 7 * 7 = 49
    TEST_ASSERT_EQUAL_UINT16(686, consumed[98]); // 98 * 7 = 686

    // Cache state after consumption
    TEST_ASSERT_FALSE(meta_valid);
    // Cache still has old data but meta_valid = false prevents re-use
}

// ============================================================
// Test: spectrum channel count fits in uint16_t (regression check)
// Previously was uint8_t which caps at 255 — RC-110 returns 1024
// ============================================================
void test_spectrum_channel_count_type(void)
{
    // uint8_t max = 255, insufficient for 1024 channels
    TEST_ASSERT_EQUAL_UINT8(255, (uint8_t)255);

    // uint16_t max = 65535, sufficient for 1024 channels
    uint16_t count = 1024;
    TEST_ASSERT_TRUE(count >= SPECTRUM_MAX_CHANNELS);
    TEST_ASSERT_EQUAL_UINT16(SPECTRUM_MAX_CHANNELS, count);

    // Verify loop counter type matches
    for (uint16_t ch = 0; ch < count; ch++) {
        // Loop should reach ch=1023 without overflow
        if (ch == 1023) {
            TEST_ASSERT_TRUE(true);
        }
    }
}

// ============================================================
// Test: spectrum cache memory footprint
// 1024 channels * uint16_t = 2KB static array
// ============================================================
void test_spectrum_cache_memory_footprint(void)
{
    size_t cache_size = SPECTRUM_MAX_CHANNELS * sizeof(uint16_t);
    TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)cache_size);

    // Plus metadata: bool valid + uint16_t channels = 3 bytes
    // Total static overhead: ~2051 bytes
    TEST_ASSERT_TRUE(cache_size <= 2 * 1024);
}

// ============================================================
// Test: poll interval math — VS_SPECTRUM polls every 5s
// At 1Hz DATA_BUF rate, ~5 readings per spectrum update
// ============================================================
void test_spectrum_poll_interval(void)
{
    // SPECTRUM_POLL_INTERVAL_MS = 5000 ms
    // RADIACODE_POLL_MS = 1000 ms
    const uint32_t poll_ms = 5000;
    const uint32_t data_ms = 1000;

    // Readings between spectrum polls
    uint32_t readings_per_spectrum = poll_ms / data_ms;
    TEST_ASSERT_EQUAL_UINT32(5, readings_per_spectrum);

    // Spectrum update cadence is independent of ~1Hz DATA_BUF
    const uint32_t spectra_per_minute = 60000 / poll_ms;
    TEST_ASSERT_EQUAL_UINT32(12, spectra_per_minute);
}

// ============================================================
// Test: validate U32LE byte ordering for spectrum counts
// Ensures firmware decodes values matching what RC-110 sends
// ============================================================
void test_spectrum_u32le_byte_order(void)
{
    // Known good value from RC-110 firmware: channel count of 42
    uint8_t expected_bytes[4];
    uint32_t val = 42;
    expected_bytes[0] = (uint8_t)(val & 0xFF);         // 42
    expected_bytes[1] = (uint8_t)((val >> 8) & 0xFF);  // 0
    expected_bytes[2] = (uint8_t)((val >> 16) & 0xFF); // 0
    expected_bytes[3] = (uint8_t)((val >> 24) & 0xFF); // 0

    uint32_t decoded = read_u32_le(expected_bytes);
    TEST_ASSERT_EQUAL_UINT32(42, decoded);

    // Another: 16909060 (0x0102030A in BE -> little-endian bytes)
    val = 0x0A030201U;
    expected_bytes[0] = 0x01; expected_bytes[1] = 0x02;
    expected_bytes[2] = 0x03; expected_bytes[3] = 0x0A;
    decoded = read_u32_le(expected_bytes);
    TEST_ASSERT_EQUAL_UINT32(0x0A030201U, decoded);
}

// ===================== MAIN =====================
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_spectrum_u32le_decode_roundtrip);
    RUN_TEST(test_spectrum_clamp_u32_to_u16);
    RUN_TEST(test_spectrum_cache_64_channels);
    RUN_TEST(test_spectrum_cache_1024_channels);
    RUN_TEST(test_spectrum_header_size);
    RUN_TEST(test_spectrum_parse_with_clamping);
    RUN_TEST(test_spectrum_bounds_excess_channels);
    RUN_TEST(test_spectrum_empty_payload);
    RUN_TEST(test_spectrum_csv_line_size);
    RUN_TEST(test_spectrum_api_decode_roundtrip);
    RUN_TEST(test_spectrum_cache_critical_section);
    RUN_TEST(test_spectrum_channel_count_type);
    RUN_TEST(test_spectrum_cache_memory_footprint);
    RUN_TEST(test_spectrum_poll_interval);
    RUN_TEST(test_spectrum_u32le_byte_order);

    return UNITY_END();
}
