// Unit tests for the dual-network selection and upload-auth helper logic
// introduced in v0.9.0.
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

    return UNITY_END();
}
