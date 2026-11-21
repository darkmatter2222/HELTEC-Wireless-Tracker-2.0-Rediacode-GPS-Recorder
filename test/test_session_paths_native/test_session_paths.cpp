// Unit tests for the session filename-grammar helpers in src/session_paths.h.
//
// The storage model is encoded entirely in file names, so these few predicates
// decide which file the uploader may take, under which X-Session-Id it is posted,
// and which name is deleted after the 2xx. A mismatch between the two directions
// orphans data silently, so the pair is asserted together -- including a full
// rotation chain over a fake directory.
//
// The real header is included (not a copy). Run with: pio test -e native

#include <unity.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../../src/session_paths.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// pendingName: "<day>.<bootMs>.up.csv"
// ---------------------------------------------------------------------------
void test_pending_name_format(void) {
    char buf[40];
    const int n = spath::pendingName(buf, sizeof(buf), "2026-11-21", 362620u);
    TEST_ASSERT_EQUAL_STRING("2026-11-21.362620.up.csv", buf);
    TEST_ASSERT_EQUAL_INT(24, n);                       // 10 + 1 + 6 + 7
    // bootMs == 0 is a legal millis() value in the first ms after boot.
    spath::pendingName(buf, sizeof(buf), "2026-01-01", 0u);
    TEST_ASSERT_EQUAL_STRING("2026-01-01.0.up.csv", buf);
    // Maximum uint32 millis (49.7 days of uptime) must not overflow the field.
    spath::pendingName(buf, sizeof(buf), "2026-11-21", 4294967295u);
    TEST_ASSERT_EQUAL_STRING("2026-11-21.4294967295.up.csv", buf);
}

void test_pending_name_guards(void) {
    char buf[40];
    TEST_ASSERT_EQUAL_INT(0, spath::pendingName(nullptr, 40, "2026-11-21", 1u));
    TEST_ASSERT_EQUAL_INT(0, spath::pendingName(buf, 0, "2026-11-21", 1u));
    TEST_ASSERT_EQUAL_INT(0, spath::pendingName(buf, sizeof(buf), nullptr, 1u));
}

void test_pending_name_truncation_is_detectable(void) {
    // snprintf reports the length it would have written, so a caller can see a
    // truncation instead of silently posting a mangled session id.
    char small[12];
    const int want = spath::pendingName(small, sizeof(small), "2026-11-21", 362620u);
    TEST_ASSERT_EQUAL_INT(24, want);
    TEST_ASSERT_TRUE((size_t)want >= sizeof(small));     // truncated
    TEST_ASSERT_EQUAL_STRING("2026-11-21.", small);      // exactly n-1 chars
    // A buffer sized for the real name writes it in full.
    char big[40];
    TEST_ASSERT_EQUAL_INT(24, spath::pendingName(big, sizeof(big), "2026-11-21", 362620u));
    TEST_ASSERT_EQUAL_STRING("2026-11-21.362620.up.csv", big);
}

// ---------------------------------------------------------------------------
// isPendingName / isDayName: the two lists must be disjoint
// ---------------------------------------------------------------------------
void test_is_pending_name(void) {
    TEST_ASSERT_TRUE (spath::isPendingName("2026-11-21.362620.up.csv"));
    TEST_ASSERT_TRUE (spath::isPendingName("a.0.up.csv"));
    TEST_ASSERT_TRUE (spath::isPendingName(".up.csv"));           // suffix only
    TEST_ASSERT_FALSE(spath::isPendingName("2026-11-21.csv"));
    TEST_ASSERT_FALSE(spath::isPendingName("2026-11-21.up"));     // missing .csv
    TEST_ASSERT_FALSE(spath::isPendingName("2026-11-21.up.csvx"));
    TEST_ASSERT_FALSE(spath::isPendingName("csv"));
    TEST_ASSERT_FALSE(spath::isPendingName(""));
    TEST_ASSERT_FALSE(spath::isPendingName(nullptr));
}

void test_is_day_name_excludes_pending(void) {
    TEST_ASSERT_TRUE (spath::isDayName("2026-11-21.csv"));
    TEST_ASSERT_TRUE (spath::isDayName("a.csv"));
    TEST_ASSERT_TRUE (spath::isDayName("boot_362620.csv"));        // legacy id form
    TEST_ASSERT_FALSE(spath::isDayName("2026-11-21.362620.up.csv"));
    // A bare ".csv" is not a pending name (the ".up" component is missing), so it
    // classifies as a day name -- harmless, because dayIdFromName rejects it for
    // having no leading component. Same as the pre-extraction inline check.
    TEST_ASSERT_TRUE (spath::isDayName(".csv"));
    TEST_ASSERT_FALSE(spath::isDayName("2026-11-21"));             // no suffix
    TEST_ASSERT_FALSE(spath::isDayName(""));
    TEST_ASSERT_FALSE(spath::isDayName(nullptr));
}

void test_day_and_pending_lists_are_disjoint(void) {
    const char* kNames[6] = {
        "2026-11-21.csv", "2026-11-20.csv", "2026-11-21.362620.up.csv",
        "2026-11-20.12.up.csv", "boot_362620.csv", "system.log",
    };
    for (int i = 0; i < 5; ++i) {
        // Every CSV-named entry is exactly one of the two families.
        TEST_ASSERT_TRUE(spath::isDayName(kNames[i]) || spath::isPendingName(kNames[i]));
        TEST_ASSERT_TRUE(!(spath::isDayName(kNames[i]) && spath::isPendingName(kNames[i])));
    }
    // Non-CSV names belong to neither family and are ignored by both scans.
    TEST_ASSERT_FALSE(spath::isDayName(kNames[5]));
    TEST_ASSERT_FALSE(spath::isPendingName(kNames[5]));
    // The pending family specifically:
    TEST_ASSERT_TRUE(spath::isPendingName(kNames[2]));
    TEST_ASSERT_FALSE(spath::isDayName(kNames[2]));
}

// ---------------------------------------------------------------------------
// dayIdFromName: both directions of the grammar
// ---------------------------------------------------------------------------
void test_day_id_from_both_forms(void) {
    char buf[24];
    TEST_ASSERT_TRUE(spath::dayIdFromName(buf, sizeof(buf), "2026-11-21.csv"));
    TEST_ASSERT_EQUAL_STRING("2026-11-21", buf);
    TEST_ASSERT_TRUE(spath::dayIdFromName(buf, sizeof(buf), "2026-11-21.362620.up.csv"));
    TEST_ASSERT_EQUAL_STRING("2026-11-21", buf);
    // Round trip through pendingName is idempotent, which is the invariant the
    // upload path needs: the id posted must match the id the file is deleted by.
    char pend[40];
    spath::pendingName(pend, sizeof(pend), "2026-11-21", 362620u);
    char back[24];
    TEST_ASSERT_TRUE(spath::dayIdFromName(back, sizeof(back), pend));
    TEST_ASSERT_EQUAL_STRING("2026-11-21", back);
    char again[40];
    spath::pendingName(again, sizeof(again), back, 362620u);
    TEST_ASSERT_EQUAL_STRING(pend, again);
    // Legacy boot-style id: first dot still splits it.
    TEST_ASSERT_TRUE(spath::dayIdFromName(buf, sizeof(buf), "boot_362620.csv"));
    TEST_ASSERT_EQUAL_STRING("boot_362620", buf);
}

void test_day_id_rejects_malformed_names(void) {
    char buf[24];
    TEST_ASSERT_FALSE(spath::dayIdFromName(buf, sizeof(buf), "2026-11-21"));  // no dot
    TEST_ASSERT_FALSE(spath::dayIdFromName(buf, sizeof(buf), ".csv"));        // leading dot
    TEST_ASSERT_FALSE(spath::dayIdFromName(buf, sizeof(buf), ""));
    TEST_ASSERT_FALSE(spath::dayIdFromName(buf, sizeof(buf), nullptr));
    TEST_ASSERT_FALSE(spath::dayIdFromName(nullptr, 24, "2026-11-21.csv"));
    TEST_ASSERT_FALSE(spath::dayIdFromName(buf, 0, "2026-11-21.csv"));
    // Too small to hold the id: reject rather than hand back a clipped id, which
    // would post under an id no file matches.
    char tiny[10];
    TEST_ASSERT_FALSE(spath::dayIdFromName(tiny, sizeof(tiny), "2026-11-21.csv"));
    char exact[11];
    TEST_ASSERT_TRUE(spath::dayIdFromName(exact, sizeof(exact), "2026-11-21.csv"));
    TEST_ASSERT_EQUAL_STRING("2026-11-21", exact);
}

// ---------------------------------------------------------------------------
// pendingCandidate: which day files the uploader may take
// ---------------------------------------------------------------------------
void test_no_utc_makes_every_day_candidate(void) {
    // Documented behaviour of rotateStaleDayFiles_(): before UTC arrives there is
    // no "today", so every day file is rotated eagerly.
    TEST_ASSERT_TRUE(spath::pendingCandidate("2026-11-21", ""));
    TEST_ASSERT_TRUE(spath::pendingCandidate("2020-01-01", ""));
    TEST_ASSERT_TRUE(spath::pendingCandidate("", ""));
    TEST_ASSERT_TRUE(spath::pendingCandidate(nullptr, ""));
    TEST_ASSERT_TRUE(spath::pendingCandidate("2026-11-21", nullptr));
}

void test_today_is_exempt_once_utc_is_known(void) {
    TEST_ASSERT_FALSE(spath::pendingCandidate("2026-11-21", "2026-11-21"));
    TEST_ASSERT_TRUE (spath::pendingCandidate("2026-11-20", "2026-11-21"));
    TEST_ASSERT_TRUE (spath::pendingCandidate("2026-11-22", "2026-11-21"));
    // An empty day id is not equal to today, so it IS a candidate -- matching the
    // inline form the firmware used before the extraction (`dayId != todayId`).
    TEST_ASSERT_TRUE (spath::pendingCandidate("", "2026-11-21"));
    // Only a missing (null) id short-circuits, which the String wrapper never
    // produces -- it is the defensive branch.
    TEST_ASSERT_FALSE(spath::pendingCandidate(nullptr, "2026-11-21"));
}

// ---------------------------------------------------------------------------
// shouldOpenDay / shouldRotateDay: the append() pair
// ---------------------------------------------------------------------------
void test_open_and_rotate_truth_table(void) {
    // First sample of a boot: nothing open -> open, nothing to rotate.
    TEST_ASSERT_TRUE (spath::shouldOpenDay("", "2026-11-21"));
    TEST_ASSERT_FALSE(spath::shouldRotateDay("", "2026-11-21"));
    // Same day: neither.
    TEST_ASSERT_FALSE(spath::shouldOpenDay("2026-11-21", "2026-11-21"));
    TEST_ASSERT_FALSE(spath::shouldRotateDay("2026-11-21", "2026-11-21"));
    // Boundary crossed: rotate then open.
    TEST_ASSERT_TRUE (spath::shouldOpenDay("2026-11-20", "2026-11-21"));
    TEST_ASSERT_TRUE (spath::shouldRotateDay("2026-11-20", "2026-11-21"));
    // No UTC yet on the new sample: keep the current file, change nothing.
    TEST_ASSERT_FALSE(spath::shouldOpenDay("2026-11-20", ""));
    TEST_ASSERT_FALSE(spath::shouldRotateDay("2026-11-20", ""));
    TEST_ASSERT_FALSE(spath::shouldOpenDay("", ""));
    TEST_ASSERT_FALSE(spath::shouldRotateDay("", ""));
    TEST_ASSERT_FALSE(spath::shouldOpenDay("2026-11-20", nullptr));
    TEST_ASSERT_FALSE(spath::shouldRotateDay(nullptr, "2026-11-21"));
    // Invariant the writer relies on: a rotation always implies an open.
    const char* kOpen[3] = { "", "2026-11-20", "2026-11-21" };
    const char* kNew[4]  = { "", "2026-11-20", "2026-11-21", "2026-11-22" };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (spath::shouldRotateDay(kOpen[i], kNew[j]))
                TEST_ASSERT_TRUE(spath::shouldOpenDay(kOpen[i], kNew[j]));
}

// ---------------------------------------------------------------------------
// Chain over a fake directory: classify -> rotate -> reclassify -> upload ids
// ---------------------------------------------------------------------------
void test_rotation_chain_over_fake_directory(void) {
    // Two day files, one already-pending file, the log, and an open active file.
    const char* kDir[6] = {
        "2026-11-20.csv", "2026-11-21.csv", "2026-11-19.8842.up.csv",
        "system.log", "2026-11-21.900000.up.csv", "boot.log",
    };
    const char* today = "2026-11-21";

    // 1) Classify exactly like rotateStaleDayFiles_() does.
    char taken[4][40]; int takenN = 0;
    for (int i = 0; i < 6; ++i) {
        if (!spath::isDayName(kDir[i])) continue;                 // log/pending skipped
        char id[24];
        if (!spath::dayIdFromName(id, sizeof(id), kDir[i])) continue;
        if (!spath::pendingCandidate(id, today)) continue;         // today stays active
        TEST_ASSERT_TRUE(spath::pendingName(taken[takenN], 40, id, 900001u + takenN) > 0);
        ++takenN;
    }
    // Only 2026-11-20 qualifies: 11-21 is today, the .up files and logs are not
    // day names.
    TEST_ASSERT_EQUAL_INT(1, takenN);
    TEST_ASSERT_EQUAL_STRING("2026-11-20.900001.up.csv", taken[0]);

    // 2) After the rename the file must still resolve to the same session id,
    //    which is what the uploader posts and then deletes by.
    char id[24];
    TEST_ASSERT_TRUE(spath::dayIdFromName(id, sizeof(id), taken[0]));
    TEST_ASSERT_EQUAL_STRING("2026-11-20", id);
    TEST_ASSERT_TRUE(spath::isPendingName(taken[0]));
    TEST_ASSERT_FALSE(spath::isDayName(taken[0]));                 // no double-take

    // 3) Re-classify the resulting directory: nothing is a candidate any more,
    //    so a second rotation pass in the same cycle is a no-op (the reason the
    //    v0.4.1 "only rotate when nothing is pending" gate holds).
    const char* kAfter[6] = {
        "2026-11-20.900001.up.csv", "2026-11-21.csv", "2026-11-19.8842.up.csv",
        "system.log", "2026-11-21.900000.up.csv", "boot.log",
    };
    int still = 0;
    for (int i = 0; i < 6; ++i) {
        if (!spath::isDayName(kAfter[i])) continue;
        char d[24];
        if (!spath::dayIdFromName(d, sizeof(d), kAfter[i])) continue;
        if (spath::pendingCandidate(d, today)) ++still;
    }
    TEST_ASSERT_EQUAL_INT(0, still);

    // 4) Every pending file yields a non-empty id (the X-Session-Id the API needs
    //    for the tracker_sessions upsert).
    for (int i = 0; i < 6; ++i) {
        if (!spath::isPendingName(kAfter[i])) continue;
        char d[24];
        TEST_ASSERT_TRUE(spath::dayIdFromName(d, sizeof(d), kAfter[i]));
        TEST_ASSERT_TRUE(d[0] != '\0');
    }
}

// Several pending files for the SAME day must get distinct names, otherwise the
// second rename clobbers the first and samples are lost between the two POSTs.
void test_same_day_multiple_pending_names_are_unique(void) {
    char a[40], b[40];
    TEST_ASSERT_TRUE(spath::pendingName(a, sizeof(a), "2026-11-21", 1000u) > 0);
    TEST_ASSERT_TRUE(spath::pendingName(b, sizeof(b), "2026-11-21", 1001u) > 0);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
    TEST_ASSERT_EQUAL_STRING("2026-11-21.1000.up.csv", a);
    TEST_ASSERT_EQUAL_STRING("2026-11-21.1001.up.csv", b);
    // Both still map back to the same day id -> the API merges them into one
    // session via the unique {sessionId,timestampMs} index.
    char ia[24], ib[24];
    TEST_ASSERT_TRUE(spath::dayIdFromName(ia, sizeof(ia), a));
    TEST_ASSERT_TRUE(spath::dayIdFromName(ib, sizeof(ib), b));
    TEST_ASSERT_EQUAL_STRING(ia, ib);
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_pending_name_format);
    RUN_TEST(test_pending_name_guards);
    RUN_TEST(test_pending_name_truncation_is_detectable);

    RUN_TEST(test_is_pending_name);
    RUN_TEST(test_is_day_name_excludes_pending);
    RUN_TEST(test_day_and_pending_lists_are_disjoint);

    RUN_TEST(test_day_id_from_both_forms);
    RUN_TEST(test_day_id_rejects_malformed_names);

    RUN_TEST(test_no_utc_makes_every_day_candidate);
    RUN_TEST(test_today_is_exempt_once_utc_is_known);

    RUN_TEST(test_open_and_rotate_truth_table);

    RUN_TEST(test_rotation_chain_over_fake_directory);
    RUN_TEST(test_same_day_multiple_pending_names_are_unique);

    return UNITY_END();
}
