// =============================================================================
// Disk-capacity gate (header-only, shared with test_disk_gate_native).
//
// On V2 hardware the on-chip LittleFS volume (~5.9 MB, ~24 h of 1 Hz
// recording) is the ONLY storage. If a long upload outage lets it fill to
// 100 %, the next LittleFS commit that needs a block allocation hits
// ENOSPC -- and littlefs 2.9's ENOSPC error log divides by
// cfg->block_count, which on the arduino-esp32 VFS mount is the
// "autodetect" sentinel (0): the log line itself panics with
// IntegerDivideByZero on every boot (2026-11-23 boot-loop incident, see
// AGENTS.md).
//
// The gate keeps the volume permanently below full by reserving a fixed
// percentage of the volume as headroom:
//   - append gate   : rows are dropped while writing would push the volume
//                     below the reserve (dgate::appendAllowed)
//   - boot self-heal: while free space is under the reserve, the oldest
//                     pending .up.csv file is deleted (dgate::selfHealNeeded)
//
// If both hold, ENOSPC becomes unreachable and the panic cannot happen.
//
// No Arduino types, no heap, no Serial -- the native suite compiles this
// directly, same pattern as wgate/blogic/gepoch/spath.
// =============================================================================

#pragma once

#include <cstddef>

namespace dgate {

// Headroom the volume must keep free at all times, in bytes.
// 100 % of a zero-sized volume is still zero (no backend mounted).
inline size_t reserveBytes(size_t totalBytes, int reservePct) {
    if (totalBytes == 0 || reservePct <= 0) return 0;
    if (reservePct >= 100) return totalBytes;
    // 64-bit intermediate: totalBytes * pct must not overflow 32-bit size_t.
    return (size_t)(((unsigned long long)totalBytes * (unsigned)reservePct) / 100ULL);
}

// Free space; a used > total reading (degenerate FS stats) means "full",
// never negative.
inline size_t freeBytes(size_t totalBytes, size_t usedBytes) {
    return (usedBytes <= totalBytes) ? (totalBytes - usedBytes) : 0;
}

// Boot self-heal: true while the oldest pending file should be deleted.
// totalBytes == 0 (no backend) or reservePct <= 0 (disabled) -> never heal.
inline bool selfHealNeeded(size_t totalBytes, size_t usedBytes, int reservePct) {
    return freeBytes(totalBytes, usedBytes) < reserveBytes(totalBytes, reservePct);
}

// Append gate: true if a row of `rowBytes` may be written while keeping the
// volume at or above the reserve afterwards (free - rowBytes >= reserve).
// totalBytes == 0 (no backend) or reservePct <= 0 (disabled) -> always allow.
inline bool appendAllowed(size_t totalBytes, size_t usedBytes, size_t rowBytes, int reservePct) {
    if (totalBytes == 0 || reservePct <= 0) return true;
    const size_t free = freeBytes(totalBytes, usedBytes);
    return free >= reserveBytes(totalBytes, reservePct)
        && free - reserveBytes(totalBytes, reservePct) >= rowBytes;
}

} // namespace dgate
