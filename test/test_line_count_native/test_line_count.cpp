// Native unit tests for the buffered newline-counting algorithm.
//
// Bug context (v0.3.4): session_store.cpp used readStringUntil('\n') inside
// listSessions() and resumeIfActive(), allocating a heap String for every
// CSV row.  At 20,000 rows that stalled the LittleFS global mutex for
// hundreds of ms each 60-second upload cycle.  The fix counts newlines with
// a fixed 256-byte stack buffer -- no heap allocation, no mutex contention.
//
// These tests verify:
//   1. The fixed algorithm produces identical results to a naive loop.
//   2. Newlines at exact buffer boundaries are not missed.
//   3. Edge cases (empty, header-only, all-newline, no-newline) are handled.
//
// Run:  pio test -e native
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Algorithm under test (faithfully replicated from session_store.cpp fix).
// ---------------------------------------------------------------------------
static uint32_t count_newlines_buffered(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;
    uint32_t count = 0;
    uint8_t buf[256];
    size_t i = 0;
    while (i < len) {
        size_t n = len - i;
        if (n > sizeof(buf)) n = sizeof(buf);
        memcpy(buf, data + i, n);
        for (size_t j = 0; j < n; ++j) {
            if (buf[j] == '\n') ++count;
        }
        i += n;
    }
    return count;
}

// Reference naive implementation used for correctness cross-checks.
static uint32_t count_newlines_naive(const uint8_t* data, size_t len) {
    uint32_t count = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') ++count;
    }
    return count;
}

// Helper: build a synthetic CSV with a header line + num_rows data rows.
// Each row is ~90 bytes and ends with '\n'. Returns the bytes written.
static size_t make_csv(uint8_t* dst, size_t dstsize, int num_rows) {
    const char* hdr =
        "timestampMs,uSvPerHour,cps,latitude,longitude,"
        "deviceId,speedKph,bearingDeg,altitudeM,hdop\n";
    size_t pos = 0;
    size_t hlen = strlen(hdr);
    if (pos + hlen < dstsize) { memcpy(dst + pos, hdr, hlen); pos += hlen; }
    for (int i = 0; i < num_rows && pos + 100 < dstsize; ++i) {
        char row[100];
        int n = snprintf(row, sizeof(row),
            "1746114660%03d,0.142,12.000,47.6062,-122.3321,"
            "5243066020F4,48.23,267.3,12.4,1.20\n",
            i % 1000);
        if (n > 0 && pos + (size_t)n < dstsize) {
            memcpy(dst + pos, row, (size_t)n);
            pos += (size_t)n;
        }
    }
    return pos;
}

// ---------------------------------------------------------------------------
void setUp(void) {}    // Required by Unity
void tearDown(void) {} // Required by Unity
// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

void test_null_zero_len(void) {
    TEST_ASSERT_EQUAL_UINT32(0, count_newlines_buffered(nullptr, 0));
    const uint8_t c = 'x';
    TEST_ASSERT_EQUAL_UINT32(0, count_newlines_buffered(&c, 0));
}

void test_single_newline(void) {
    const uint8_t nl = '\n';
    TEST_ASSERT_EQUAL_UINT32(1, count_newlines_buffered(&nl, 1));
}

void test_no_newlines(void) {
    const char* s = "no newlines here at all";
    TEST_ASSERT_EQUAL_UINT32(0, count_newlines_buffered((const uint8_t*)s, strlen(s)));
}

void test_all_newlines(void) {
    uint8_t buf[300];
    memset(buf, '\n', sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(300, count_newlines_buffered(buf, sizeof(buf)));
}

void test_header_only_csv(void) {
    const char* hdr =
        "timestampMs,uSvPerHour,cps,latitude,longitude,"
        "deviceId,speedKph,bearingDeg,altitudeM,hdop\n";
    size_t len = strlen(hdr);
    uint32_t newlines = count_newlines_buffered((const uint8_t*)hdr, len);
    TEST_ASSERT_EQUAL_UINT32(1, newlines);
    // Subtracting the header line gives 0 data rows.
    TEST_ASSERT_EQUAL_UINT32(0, newlines - 1);
}

void test_small_csv_matches_naive(void) {
    static uint8_t buf[8192];
    size_t len = make_csv(buf, sizeof(buf), 10);
    uint32_t naive  = count_newlines_naive(buf, len);
    uint32_t fixed  = count_newlines_buffered(buf, len);
    TEST_ASSERT_EQUAL_UINT32(naive, fixed);
    // 1 header + 10 data rows = 11 newlines; header excluded = 10 samples.
    TEST_ASSERT_EQUAL_UINT32(11, fixed);
}

void test_large_csv_matches_naive(void) {
    // 500 rows -- buffer boundary crossed many times (each row ~90 bytes,
    // buffer is 256 bytes, so boundaries fall mid-row repeatedly).
    static uint8_t buf[100000];
    size_t len = make_csv(buf, sizeof(buf), 500);
    TEST_ASSERT_EQUAL_UINT32(count_newlines_naive(buf, len),
                             count_newlines_buffered(buf, len));
    TEST_ASSERT_EQUAL_UINT32(501, count_newlines_buffered(buf, len));
}

void test_newline_at_buffer_boundaries(void) {
    // Place '\n' at byte indices 255 (last byte of first buffer),
    // 256 (first byte of second buffer), and 511 (end of second).
    static uint8_t buf[512];
    memset(buf, 'x', sizeof(buf));
    buf[255] = '\n';
    buf[256] = '\n';
    buf[511] = '\n';
    uint32_t naive = count_newlines_naive(buf, sizeof(buf));
    uint32_t fixed = count_newlines_buffered(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(3, naive);
    TEST_ASSERT_EQUAL_UINT32(naive, fixed);
}

void test_sample_count_less_header(void) {
    // The firmware does: count = newlines; if (count > 0) --count;
    // Verify this yields the correct data-row count.
    static uint8_t buf[50000];
    int ROWS = 200;
    size_t len = make_csv(buf, sizeof(buf), ROWS);
    uint32_t newlines = count_newlines_buffered(buf, len);
    uint32_t samples  = (newlines > 0) ? (newlines - 1) : 0;  // strip header
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ROWS, samples);
}

void test_very_short_data(void) {
    const char* s = "a\n";
    TEST_ASSERT_EQUAL_UINT32(1, count_newlines_buffered((const uint8_t*)s, 2));
    const char* s2 = "ab";
    TEST_ASSERT_EQUAL_UINT32(0, count_newlines_buffered((const uint8_t*)s2, 2));
}

// ---------------------------------------------------------------------------
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_null_zero_len);
    RUN_TEST(test_single_newline);
    RUN_TEST(test_no_newlines);
    RUN_TEST(test_all_newlines);
    RUN_TEST(test_header_only_csv);
    RUN_TEST(test_small_csv_matches_naive);
    RUN_TEST(test_large_csv_matches_naive);
    RUN_TEST(test_newline_at_buffer_boundaries);
    RUN_TEST(test_sample_count_less_header);
    RUN_TEST(test_very_short_data);
    return UNITY_END();
}
