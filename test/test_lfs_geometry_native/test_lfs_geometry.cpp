// Unit tests for the LittleFS volume-geometry gate (v2.1).
//
// session_store.cpp mirrors this condition in lfsGeometryOk():
//   total != 0 && used <= total
// A mount that returns total == 0 carries block_count == 0, and the first
// lfs_alloc() modulo then panics with IntegerDivideByZero (observed 2026-11-18).

#include <unity.h>
#include <stddef.h>

static bool lfsGeometryOk(size_t total, size_t used) {
    return total != 0 && used <= total;
}

void setUp(void) {}
void tearDown(void) {}

void test_zero_zero_invalid(void) {
    // The exact crash signature: degenerate mount after downgrade.
    TEST_ASSERT_FALSE(lfsGeometryOk(0, 0));
}

void test_total_zero_used_nonzero_invalid(void) {
    TEST_ASSERT_FALSE(lfsGeometryOk(0, 16384));
}

void test_healthy_post_format(void) {
    // Numbers captured live after re-format on device.
    TEST_ASSERT_TRUE(lfsGeometryOk(6160384, 16384));
}

void test_full_but_consistent_is_valid(void) {
    // used == total is a legal, merely-full volume: no divide-by-zero risk
    // because block_count is nonzero.
    TEST_ASSERT_TRUE(lfsGeometryOk(6160384, 6160384));
}

void test_used_exceeds_total_invalid(void) {
    TEST_ASSERT_FALSE(lfsGeometryOk(6160384, 6160385));
}

void test_minimal_valid_geometry(void) {
    TEST_ASSERT_TRUE(lfsGeometryOk(1, 0));
    TEST_ASSERT_TRUE(lfsGeometryOk(4096, 4096));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_zero_zero_invalid);
    RUN_TEST(test_total_zero_used_nonzero_invalid);
    RUN_TEST(test_healthy_post_format);
    RUN_TEST(test_full_but_consistent_is_valid);
    RUN_TEST(test_used_exceeds_total_invalid);
    RUN_TEST(test_minimal_valid_geometry);
    return UNITY_END();
}
