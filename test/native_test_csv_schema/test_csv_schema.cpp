// Native unit tests for CSV schema correctness and the MIN_VALID_TS_MS gate.
//
// Context:
//   - Firmware v0.3.0 extended the CSV from 6 to 10 columns.
//   - The MIN_VALID_TS_MS (2020-01-01 UTC) gate prevents millis()-since-boot
//     timestamps from being stored, keeping session firstTsMs accurate.
//   - Disabled optional fields emit an *empty string* (not a missing column),
//     so the column count stays fixed at 10 for all v0.3.0+ rows.
//   - The ingest API must accept both 6-column (pre-v0.3.0) and 10-column rows.
//
// Run:  pio test -e native
#include <unity.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Constants under test
// ---------------------------------------------------------------------------
static const uint64_t MIN_VALID_TS_MS = 1577836800000ULL;  // 2020-01-01 00:00:00 UTC

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool ts_is_valid(uint64_t ts) {
    return ts >= MIN_VALID_TS_MS;
}

// Count comma-separated fields in one CSV row (stops at '\n' or '\0').
static int count_fields(const char* row) {
    if (!row || !*row) return 0;
    int commas = 0;
    for (const char* p = row; *p && *p != '\n' && *p != '\r'; ++p) {
        if (*p == ',') ++commas;
    }
    return commas + 1;
}

// Return the value of field `idx` (0-based) copied into `dst`.
// Returns false if the row has fewer fields or `dst` is too small.
static bool get_field(const char* row, int idx, char* dst, size_t dstsz) {
    int field = 0;
    const char* start = row;
    for (const char* p = row; ; ++p) {
        if (*p == ',' || *p == '\n' || *p == '\r' || *p == '\0') {
            if (field == idx) {
                size_t len = (size_t)(p - start);
                if (len + 1 > dstsz) return false;
                memcpy(dst, start, len);
                dst[len] = '\0';
                return true;
            }
            if (*p == '\0' || *p == '\n' || *p == '\r') return false;
            ++field;
            start = p + 1;
        }
    }
}

// ---------------------------------------------------------------------------
// Timestamp gate tests
// ---------------------------------------------------------------------------

void test_min_valid_ts_exact(void) {
    // The boundary itself must be accepted.
    TEST_ASSERT_TRUE(ts_is_valid(MIN_VALID_TS_MS));
}

void test_one_ms_below_boundary_rejected(void) {
    TEST_ASSERT_FALSE(ts_is_valid(MIN_VALID_TS_MS - 1));
}

void test_millis_since_boot_rejected(void) {
    // A device booted 24 h ago has millis() ~= 86400000 -- far before 2020.
    TEST_ASSERT_FALSE(ts_is_valid(86400000ULL));
    // Single-digit-seconds boot timestamp also invalid.
    TEST_ASSERT_FALSE(ts_is_valid(1000ULL));
    TEST_ASSERT_FALSE(ts_is_valid(0ULL));
}

void test_realistic_2026_timestamp(void) {
    // 2026-05-07T00:00:00Z in ms.
    const uint64_t ts_2026 = 1746576000000ULL;
    TEST_ASSERT_TRUE(ts_is_valid(ts_2026));
}

void test_year_2020_start_accepted(void) {
    // Exactly 2020-01-01T00:00:00.000Z.
    TEST_ASSERT_TRUE(ts_is_valid(1577836800000ULL));
}

void test_far_future_accepted(void) {
    // 2035-01-01 -- should not be clamped by the gate.
    const uint64_t ts_2035 = 2051222400000ULL;
    TEST_ASSERT_TRUE(ts_is_valid(ts_2035));
}

// ---------------------------------------------------------------------------
// CSV schema / field count tests
// ---------------------------------------------------------------------------

void test_v030_header_has_10_fields(void) {
    const char* hdr =
        "timestampMs,uSvPerHour,cps,latitude,longitude,"
        "deviceId,speedKph,bearingDeg,altitudeM,hdop\n";
    TEST_ASSERT_EQUAL_INT(10, count_fields(hdr));
}

void test_v030_full_data_row_has_10_fields(void) {
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,"
        "5243066020F4,48.23,267.3,12.4,1.20\n";
    TEST_ASSERT_EQUAL_INT(10, count_fields(row));
}

void test_v030_sparse_row_still_10_fields(void) {
    // Optional extended fields disabled -- empty strings maintain column count.
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,"
        "5243066020F4,,,12.4,\n";
    TEST_ASSERT_EQUAL_INT(10, count_fields(row));
}

void test_v030_all_optional_empty_still_10_fields(void) {
    // All 4 optional fields disabled.
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,"
        "5243066020F4,,,,\n";
    TEST_ASSERT_EQUAL_INT(10, count_fields(row));
}

void test_pre_v030_old_6col_row(void) {
    // Ingest API accepts both schemas; verify the old one has exactly 6 fields.
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,5243066020F4\n";
    TEST_ASSERT_EQUAL_INT(6, count_fields(row));
}

// ---------------------------------------------------------------------------
// Field extraction tests
// ---------------------------------------------------------------------------

void test_get_field_0_is_timestamp(void) {
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,"
        "5243066020F4,48.23,267.3,12.4,1.20\n";
    char val[32];
    TEST_ASSERT_TRUE(get_field(row, 0, val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("1746114660123", val);
}

void test_get_field_5_is_device_id(void) {
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,"
        "5243066020F4,48.23,267.3,12.4,1.20\n";
    char val[32];
    TEST_ASSERT_TRUE(get_field(row, 5, val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("5243066020F4", val);
}

void test_get_field_empty_optional(void) {
    // speedKph is field 6 and is empty when FIELD_SPEED_KPH = false.
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,"
        "5243066020F4,,267.3,12.4,1.20\n";
    char val[32];
    TEST_ASSERT_TRUE(get_field(row, 6, val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("", val);
}

void test_get_field_out_of_bounds(void) {
    const char* row =
        "1746114660123,0.142,12.000,47.6062,-122.3321,5243066020F4\n";
    char val[32];
    // Field index 6 does not exist in a 6-column row.
    TEST_ASSERT_FALSE(get_field(row, 6, val, sizeof(val)));
}

// ---------------------------------------------------------------------------
int main(void) {
    UNITY_BEGIN();
    // Timestamp gate
    RUN_TEST(test_min_valid_ts_exact);
    RUN_TEST(test_one_ms_below_boundary_rejected);
    RUN_TEST(test_millis_since_boot_rejected);
    RUN_TEST(test_realistic_2026_timestamp);
    RUN_TEST(test_year_2020_start_accepted);
    RUN_TEST(test_far_future_accepted);
    // Schema field counts
    RUN_TEST(test_v030_header_has_10_fields);
    RUN_TEST(test_v030_full_data_row_has_10_fields);
    RUN_TEST(test_v030_sparse_row_still_10_fields);
    RUN_TEST(test_v030_all_optional_empty_still_10_fields);
    RUN_TEST(test_pre_v030_old_6col_row);
    // Field extraction
    RUN_TEST(test_get_field_0_is_timestamp);
    RUN_TEST(test_get_field_5_is_device_id);
    RUN_TEST(test_get_field_empty_optional);
    RUN_TEST(test_get_field_out_of_bounds);
    return UNITY_END();
}
