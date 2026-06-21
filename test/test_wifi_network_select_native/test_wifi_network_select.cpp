// Unit tests for the dual-network selection and upload-auth helper logic
// introduced in v0.9.0, extended in v1.0.3 with chunked-upload policy.
//
// Pure-function tests: no Wi-Fi hardware, no secrets.h dependency.
// The helpers here mirror the logic embedded in wifi_uploader.cpp so that
// boundary conditions can be regression-tested on the host with 'pio test -e native'.

#include <unity.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers -- mirrors of the private logic in wifi_uploader.cpp.
// ---------------------------------------------------------------------------

// A "complete" profile requires a non-empty SSID AND a non-empty URL.
static bool hasProfile(const char* ssid, const char* url) {
    return ssid && ssid[0] != '\0' && url && url[0] != '\0';
}

// The uploader is enabled if at least one complete profile is present.
static bool uploaderEnabled(const char* ssid,  const char* url,
                            const char* ssid2, const char* url2) {
    return hasProfile(ssid, url) || hasProfile(ssid2, url2);
}

// Returns true if the URL scheme is https:// (first 8 chars, case-sensitive
// since secrets.h constants are always lower-case).
static bool isHttpsUrl(const char* url) {
    if (!url) return false;
    const char prefix[] = "https://";
    for (int i = 0; i < 8; ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c += 32;   // tolower without <ctype.h>
        if (c != prefix[i]) return false;
    }
    return true;
}

// Returns true only if BOTH user and pass are non-empty strings.
static bool hasBasicAuthCreds(const char* user, const char* pass) {
    return user && user[0] != '\0' && pass && pass[0] != '\0';
}

// ---------------------------------------------------------------------------
// Helpers -- mirrors of the chunked-upload policy in wifi_uploader.cpp
// (v1.0.3). Mirrors cfg::UPLOAD_LARGE_FILE_THRESHOLD and
// cfg::UPLOAD_CHUNK_ROWS from config.h so tests run standalone on host.
// ---------------------------------------------------------------------------
static const size_t UPLOAD_LARGE_FILE_THRESHOLD = 32768;  // 32 KB
static const size_t UPLOAD_CHUNK_ROWS           = 300;

// True when the file is large enough to require chunked mode.
static bool fileNeedsChunking(size_t fileBytes) {
    return fileBytes >= UPLOAD_LARGE_FILE_THRESHOLD;
}

// Number of HTTP POSTs needed for a file with `totalRows` data rows at
// the configured chunk size. Returns 0 for empty files.
static size_t chunkCount(size_t totalRows) {
    if (totalRows == 0) return 0;
    return (totalRows + UPLOAD_CHUNK_ROWS - 1) / UPLOAD_CHUNK_ROWS;
}

// ---------------------------------------------------------------------------
// setUp / tearDown (required by Unity but unused here)
// ---------------------------------------------------------------------------
void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Tests: HTTPS URL detection
// ---------------------------------------------------------------------------
void test_https_url_detected() {
    TEST_ASSERT_TRUE(isHttpsUrl("https://susmannet.duckdns.org/api/ingest/csv"));
    TEST_ASSERT_TRUE(isHttpsUrl("https://example.com/api"));
}

void test_http_url_not_https() {
    TEST_ASSERT_FALSE(isHttpsUrl("http://192.168.86.48:8030/ingest/csv"));
    TEST_ASSERT_FALSE(isHttpsUrl("http://example.com"));
}

void test_empty_and_null_not_https() {
    TEST_ASSERT_FALSE(isHttpsUrl(""));
    TEST_ASSERT_FALSE(isHttpsUrl(nullptr));
}

// ---------------------------------------------------------------------------
// Tests: Basic Auth credential check
// ---------------------------------------------------------------------------
void test_both_creds_present() {
    TEST_ASSERT_TRUE(hasBasicAuthCreds("darkmatter2222", "liquimatter"));
}

void test_empty_user_no_auth() {
    TEST_ASSERT_FALSE(hasBasicAuthCreds("", "liquimatter"));
}

void test_empty_pass_no_auth() {
    TEST_ASSERT_FALSE(hasBasicAuthCreds("darkmatter2222", ""));
}

void test_both_empty_no_auth() {
    TEST_ASSERT_FALSE(hasBasicAuthCreds("", ""));
}

// ---------------------------------------------------------------------------
// Tests: uploader enabled/disabled based on configured profiles
// ---------------------------------------------------------------------------
void test_enabled_home_only() {
    TEST_ASSERT_TRUE(uploaderEnabled(
        "DrNerd",   "http://192.168.86.48:8030/ingest/csv",
        "",         ""));
}

void test_enabled_remote_only() {
    TEST_ASSERT_TRUE(uploaderEnabled(
        "",         "",
        "Hotspot",  "https://susmannet.duckdns.org/api/ingest/csv"));
}

void test_enabled_both_profiles() {
    TEST_ASSERT_TRUE(uploaderEnabled(
        "DrNerd",   "http://192.168.86.48:8030/ingest/csv",
        "Hotspot",  "https://susmannet.duckdns.org/api/ingest/csv"));
}

void test_disabled_no_profiles() {
    TEST_ASSERT_FALSE(uploaderEnabled("", "", "", ""));
}

void test_disabled_ssid_without_url() {
    // SSID present but URL missing -> incomplete profile -> disabled
    TEST_ASSERT_FALSE(uploaderEnabled("DrNerd", "", "", ""));
}

void test_disabled_url_without_ssid() {
    // URL present but SSID missing -> incomplete profile -> disabled
    TEST_ASSERT_FALSE(uploaderEnabled("", "http://192.168.86.48:8030/ingest/csv", "", ""));
}

void test_disabled_remote_ssid_without_url() {
    TEST_ASSERT_FALSE(uploaderEnabled("", "", "Hotspot", ""));
}

// ---------------------------------------------------------------------------
// Tests: chunked-upload threshold (v1.0.3)
// ---------------------------------------------------------------------------

// A normal 60-second rotation file (5-20 rows × ~110 bytes ≈ 550-2200 bytes)
// is well below the 32 KB threshold and uses the fast single-POST path.
void test_normal_rotation_file_below_threshold() {
    // Simulate a file from a 60-second rotation: 10 rows × 110 bytes = 1.1 KB
    TEST_ASSERT_FALSE(fileNeedsChunking(10 * 110));
    // Even a 5-minute accumulation (~300 rows × 110 = 33 KB) just crosses.
    // The key boundary: anything under 32 KB is single-POST.
    TEST_ASSERT_FALSE(fileNeedsChunking(32767));
}

// A day-file from a 2-day offline trip (60,000 rows × ~110 bytes = ~6.6 MB)
// is far above the threshold and must use chunked mode.
void test_large_offline_file_above_threshold() {
    // 60,000 samples × 110 bytes = 6,600,000 bytes
    TEST_ASSERT_TRUE(fileNeedsChunking(60000 * 110));
    // Even a modestly-sized backlog (e.g. 1 hour offline = 3,600 rows × 110)
    // exceeds the threshold.
    TEST_ASSERT_TRUE(fileNeedsChunking(3600 * 110));
}

// A file exactly at the threshold boundary must use chunked mode.
void test_file_exactly_at_threshold_uses_chunked() {
    TEST_ASSERT_TRUE(fileNeedsChunking(UPLOAD_LARGE_FILE_THRESHOLD));
}

// ---------------------------------------------------------------------------
// Tests: chunk count math (v1.0.3)
// ---------------------------------------------------------------------------

// An empty file produces zero chunks (guard against divide-by-zero).
void test_chunk_count_empty_file() {
    TEST_ASSERT_EQUAL(0, chunkCount(0));
}

// A file smaller than one chunk posts exactly 1 request.
void test_chunk_count_under_one_chunk() {
    TEST_ASSERT_EQUAL(1, chunkCount(1));
    TEST_ASSERT_EQUAL(1, chunkCount(UPLOAD_CHUNK_ROWS - 1));
}

// A file that exactly fills one chunk posts exactly 1 request.
void test_chunk_count_exact_one_chunk() {
    TEST_ASSERT_EQUAL(1, chunkCount(UPLOAD_CHUNK_ROWS));
}

// A 60,000-row file at 300 rows/chunk should require 200 requests.
void test_chunk_count_large_file() {
    // 60,000 / 300 = 200 exactly
    TEST_ASSERT_EQUAL(200, chunkCount(60000));
}

// A file with one row more than a full chunk requires 2 requests.
void test_chunk_count_one_row_overflow() {
    TEST_ASSERT_EQUAL(2, chunkCount(UPLOAD_CHUNK_ROWS + 1));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    // HTTPS detection
    RUN_TEST(test_https_url_detected);
    RUN_TEST(test_http_url_not_https);
    RUN_TEST(test_empty_and_null_not_https);

    // Basic Auth credential check
    RUN_TEST(test_both_creds_present);
    RUN_TEST(test_empty_user_no_auth);
    RUN_TEST(test_empty_pass_no_auth);
    RUN_TEST(test_both_empty_no_auth);

    // Enable/disable logic
    RUN_TEST(test_enabled_home_only);
    RUN_TEST(test_enabled_remote_only);
    RUN_TEST(test_enabled_both_profiles);
    RUN_TEST(test_disabled_no_profiles);
    RUN_TEST(test_disabled_ssid_without_url);
    RUN_TEST(test_disabled_url_without_ssid);
    RUN_TEST(test_disabled_remote_ssid_without_url);

    // Chunked-upload threshold (v1.0.3)
    RUN_TEST(test_normal_rotation_file_below_threshold);
    RUN_TEST(test_large_offline_file_above_threshold);
    RUN_TEST(test_file_exactly_at_threshold_uses_chunked);

    // Chunk count math (v1.0.3)
    RUN_TEST(test_chunk_count_empty_file);
    RUN_TEST(test_chunk_count_under_one_chunk);
    RUN_TEST(test_chunk_count_exact_one_chunk);
    RUN_TEST(test_chunk_count_large_file);
    RUN_TEST(test_chunk_count_one_row_overflow);

    return UNITY_END();
}
