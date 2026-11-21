// =============================================================================
// Session filename-grammar helpers — header-only, zero deps.
//
// The storage model is entirely encoded in file names, so the grammar is the
// contract:
//
//   active day file     2026-11-21.csv                  (written by append())
//   rotated pending     2026-11-21.362620.up.csv        (uploaded, then deleted)
//
// rotateForUpload() renames the active file to "<dayId>.<bootMs>.up.csv"; the
// uploader recovers the day id from that name to build the X-Session-Id header.
// A mismatch between the two directions silently orphans data (the file is
// posted under one id and deleted under another), which is why both directions
// live here as one pair and are asserted together in
// `test/test_session_paths_native`.
//
// session_store.cpp includes this header and wraps the results in Arduino
// Strings, so the host suite tests the code that actually runs.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace spath {

// "<dayId>.<bootMs>.up.csv" -> out. Returns snprintf's count, i.e. the length
// the name WOULD have: when that exceeds n-1 the write was truncated, which is
// how callers detect an undersized buffer. Guarded inputs give 0.
inline int pendingName(char* out, size_t n, const char* dayId, uint32_t bootMs) {
    if (!out || n == 0 || !dayId) return 0;
    return snprintf(out, n, "%s.%lu.up.csv", dayId, (unsigned long)bootMs);
}

// True for "<anything>.up.csv" (the rotated, awaiting-upload form).
inline bool isPendingName(const char* name) {
    if (!name) return false;
    const size_t len = strlen(name);
    const size_t suf = strlen(".up.csv");
    return len >= suf && strcmp(name + len - suf, ".up.csv") == 0;
}

// True for "<day>.csv" but NOT for "<day>.<bootMs>.up.csv" — the two lists must
// stay disjoint or a pending file would be treated as the active day file and
// skipped by the uploader.
inline bool isDayName(const char* name) {
    if (!name) return false;
    const size_t len = strlen(name);
    const size_t suf = strlen(".csv");
    if (len < suf || strcmp(name + len - suf, ".csv") != 0) return false;
    return !isPendingName(name);
}

// "<day>.csv" and "<day>.<bootMs>.up.csv" both yield <day>. Returns false when
// there is no leading component (a name starting with '.', or no '.' at all),
// matching the empty-String result the firmware produces.
inline bool dayIdFromName(char* out, size_t n, const char* name) {
    if (!out || n == 0 || !name) return false;
    const char* dot = strchr(name, '.');
    if (!dot || dot == name) return false;
    const size_t len = (size_t)(dot - name);
    if (len >= n) return false;              // would truncate: reject, not clip
    memcpy(out, name, len);
    out[len] = '\0';
    return true;
}

// Which day files the uploader may take. With no UTC yet there is no "today",
// so EVERY day file is a candidate (documented in SessionStore::rotateForUpload).
// Once today is known, only files that are not the active one are eligible.
inline bool pendingCandidate(const char* dayId, const char* todayId) {
    if (!todayId || todayId[0] == '\0') return true;
    if (!dayId) return false;
    return strcmp(dayId, todayId) != 0;
}

// Two-part rotation decision for append(), matching the two conditions in
// SessionStore::append() one-for-one:
//   shouldOpenDay   -- a fresh day file must be created/reopened
//   shouldRotateDay -- the previously open file is first renamed to .up.csv
// An empty new day id (no UTC yet) changes nothing; an empty open id means this
// is the first sample of the boot, so there is nothing to rotate.
inline bool shouldOpenDay(const char* openDayId, const char* newDayId) {
    if (!newDayId || newDayId[0] == '\0') return false;
    if (!openDayId || openDayId[0] == '\0') return true;
    return strcmp(openDayId, newDayId) != 0;
}
inline bool shouldRotateDay(const char* openDayId, const char* newDayId) {
    if (!openDayId || openDayId[0] == '\0') return false;
    if (!newDayId || newDayId[0] == '\0') return false;
    return strcmp(openDayId, newDayId) != 0;
}

} // namespace spath
