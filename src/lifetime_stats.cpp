#include "lifetime_stats.h"
#include <Preferences.h>
#include <math.h>

// M_PI is a POSIX extension; define a fallback for strict-ISO toolchains.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// NVS namespace and key names. Keep them <= 15 chars (NVS limit).
static const char* kNvsNS     = "life";
static const char* kKeyDist   = "dist";   // float km
static const char* kKeyAlt    = "alt";    // float m
static const char* kKeyRec    = "rec";    // uint32 seconds
static const char* kKeyUpl    = "upl";    // uint32 uploads
static const char* kKeySpk    = "spk";    // uint32 spikes
static const char* kKeyCells  = "cells";  // uint32 unique cells
static const char* kKeyBytesL = "bytL";   // uint32 low 32 bits of totalBytes
static const char* kKeyBytesH = "bytH";   // uint32 high 32 bits of totalBytes
static const char* kKeyBat    = "bat";    // uint32 battery cycles
static const char* kKeyIdle   = "idle";   // uint32 not-recording seconds

// Save interval: 60 seconds, matching wifi upload cadence.
static constexpr uint32_t kSaveIntervalMs = 60000;

// Thresholds that trigger an early NVS flush before the interval expires.
static constexpr float    kDeltaDist  = 0.5f;    // km
static constexpr float    kDeltaAlt   = 10.0f;   // m
static constexpr uint32_t kDeltaRec   = 60;      // seconds
static constexpr uint32_t kDeltaIdle  = 60;      // seconds not recording
static constexpr uint32_t kDeltaUpl   = 1;       // any new upload
static constexpr uint32_t kDeltaSpk   = 1;       // any spike
static constexpr uint32_t kDeltaCells = 5;       // cells
static constexpr uint32_t kDeltaBat   = 1;       // any cycle

// Haversine great-circle distance in kilometres.
// Pure function; uses only <math.h>; safe to call from any context.
static float haversineKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371.0;   // Earth radius km
    const double dLat = (lat2 - lat1) * M_PI / 180.0;
    const double dLon = (lon2 - lon1) * M_PI / 180.0;
    const double a = sin(dLat / 2) * sin(dLat / 2) +
                     cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
                     sin(dLon / 2) * sin(dLon / 2);
    const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return (float)(R * c);
}

// -----------------------------------------------------------------------
void LifetimeStats::begin() {
    memset(cellTable_, 0xFF, sizeof(cellTable_));  // 0xFFFFFFFF = empty sentinel
    loadFromNvs();
    ready_ = true;
    Serial.printf("[LIFE] loaded: dist=%.2fkm alt=%.1fm rec=%us idle=%us upl=%u spk=%u cells=%u bytes=%llu bat=%u\n",
                  distanceKm_, altGainM_, (unsigned)recordingSecs_, (unsigned)idleSecs_,
                  (unsigned)wifiUploads_, (unsigned)spikeEvents_,
                  (unsigned)uniqueCells_, (unsigned long long)totalBytes_,
                  (unsigned)battCycles_);
}

void LifetimeStats::loadFromNvs() {
    Preferences p;
    p.begin(kNvsNS, /*readOnly=*/true);
    distanceKm_    = p.getFloat(kKeyDist,   0.0f);
    altGainM_      = p.getFloat(kKeyAlt,    0.0f);
    recordingSecs_ = p.getUInt(kKeyRec,     0);
    idleSecs_      = p.getUInt(kKeyIdle,    0);
    wifiUploads_   = p.getUInt(kKeyUpl,     0);
    spikeEvents_   = p.getUInt(kKeySpk,     0);
    uniqueCells_   = p.getUInt(kKeyCells,   0);
    battCycles_    = p.getUInt(kKeyBat,     0);
    uint32_t bL    = p.getUInt(kKeyBytesL,  0);
    uint32_t bH    = p.getUInt(kKeyBytesH,  0);
    totalBytes_    = ((uint64_t)bH << 32) | (uint64_t)bL;
    p.end();

    // Snapshot saved-values so tickSave() knows what's already persisted.
    savedDistKm_  = distanceKm_;
    savedAltM_    = altGainM_;
    savedRecSecs_ = recordingSecs_;
    savedIdleSecs_= idleSecs_;
    savedUploads_ = wifiUploads_;
    savedSpikes_  = spikeEvents_;
    savedCells_   = uniqueCells_;
    savedBytes_   = totalBytes_;
    savedBatCyc_  = battCycles_;
}

void LifetimeStats::saveToNvs() {
    Preferences p;
    p.begin(kNvsNS, /*readOnly=*/false);
    p.putFloat(kKeyDist,   distanceKm_);
    p.putFloat(kKeyAlt,    altGainM_);
    p.putUInt(kKeyRec,     recordingSecs_);
    p.putUInt(kKeyIdle,    idleSecs_);
    p.putUInt(kKeyUpl,     wifiUploads_);
    p.putUInt(kKeySpk,     spikeEvents_);
    p.putUInt(kKeyCells,   uniqueCells_);
    p.putUInt(kKeyBat,     battCycles_);
    p.putUInt(kKeyBytesL,  (uint32_t)(totalBytes_ & 0xFFFFFFFFULL));
    p.putUInt(kKeyBytesH,  (uint32_t)(totalBytes_ >> 32));
    p.end();

    savedDistKm_  = distanceKm_;
    savedAltM_    = altGainM_;
    savedRecSecs_ = recordingSecs_;
    savedIdleSecs_= idleSecs_;
    savedUploads_ = wifiUploads_;
    savedSpikes_  = spikeEvents_;
    savedCells_   = uniqueCells_;
    savedBytes_   = totalBytes_;
    savedBatCyc_  = battCycles_;
    lastSaveMs_   = millis();
}

// -----------------------------------------------------------------------
// cellKey: encode lat/lng into a 32-bit grid identifier.
// Grid resolution = 0.01 deg (~1.1 km). Offset so negative coords stay
// positive: lat in [-90, 90) -> [0, 18000), lng in [-180, 180) -> [0, 36000).
// Combined into a 32-bit value with no overflow.
uint32_t LifetimeStats::cellKey(double lat, double lng) {
    // Guard: reject obviously invalid coordinates.
    if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0) {
        return 0xFFFFFFFFUL;  // sentinel = invalid
    }
    const int32_t iLat = (int32_t)((lat  + 90.0)  / CELL_DEG);  // 0..17999
    const int32_t iLng = (int32_t)((lng + 180.0)  / CELL_DEG);  // 0..35999
    // Pack: iLat uses 15 bits (max 17999), iLng uses 16 bits (max 35999).
    // Combined = iLat * 36000 + iLng, fits comfortably in uint32_t (max ~648M).
    return (uint32_t)((uint32_t)iLat * 36000u + (uint32_t)iLng);
}

bool LifetimeStats::visitCell(double lat, double lng) {
    if (cellTableFull_) {
        // Table is saturated — trust the NVS count, stop incrementing RAM table.
        // This is a conservative safety valve; the NVS count is already accurate
        // because we incremented it before the table filled.
        return false;
    }
    const uint32_t key = cellKey(lat, lng);
    if (key == 0xFFFFFFFFUL) return false;  // invalid coord

    // Open-address probing (linear, power-of-2 table).
    uint32_t slot = key & (CELL_TABLE_SIZE - 1);
    for (uint16_t i = 0; i < CELL_TABLE_SIZE; ++i) {
        const uint32_t v = cellTable_[slot];
        if (v == key) return false;           // already visited
        if (v == 0xFFFFFFFFUL) {              // empty slot — insert here
            cellTable_[slot] = key;
            ++uniqueCells_;
            return true;
        }
        slot = (slot + 1) & (CELL_TABLE_SIZE - 1);
    }
    // Table full (load factor = 1) — mark and stop probing.
    cellTableFull_ = true;
    return false;
}

// -----------------------------------------------------------------------
void LifetimeStats::onGpsFix(double lat, double lng, double altM, bool altValid) {
    // Distance delta — only if we have a valid previous position.
    if (prevPosValid_) {
        const float d = haversineKm(prevLat_, prevLng_, lat, lng);
        // Sanity cap: ignore GPS jumps > 5 km between consecutive 1-Hz samples
        // (those are noise, not real movement).
        if (d < 5.0f) {
            distanceKm_ += d;
        }

        // Altitude gain — positive delta only.
        if (altValid && prevAlt_ > -9000.0) {
            const float dAlt = (float)(altM - prevAlt_);
            if (dAlt > 0.0f && dAlt < 200.0f) {  // cap noise > 200m per sample
                altGainM_ += dAlt;
            }
        }
    }

    // Update previous position.
    prevLat_      = lat;
    prevLng_      = lng;
    prevAlt_      = altM;
    prevPosValid_ = true;

    // Unique-cell visit.
    visitCell(lat, lng);
}

void LifetimeStats::onSample(float cps, uint32_t dt_ms) {
    // Recording-time accumulation.
    // Cap dt to 10 s to avoid phantom accumulation after a BLE gap.
    if (dt_ms > 0 && dt_ms < 10000) {
        recordingSecs_ += dt_ms / 1000;
    }

    // Spike detection.
    if (cps >= SPIKE_CPS_THRESHOLD) {
        ++spikeEvents_;
    }
}

void LifetimeStats::onIdleTick(uint32_t dt_ms) {
    // Not-recording time: device is on but no GPS-locked RC sample arrived.
    // Cap dt to 10 s same as onSample to avoid phantom accumulation.
    if (dt_ms > 0 && dt_ms < 10000) {
        idleSecs_ += dt_ms / 1000;
    }
}

void LifetimeStats::onUploadSuccess() {
    ++wifiUploads_;
}

void LifetimeStats::onBytesWritten(size_t bytes) {
    totalBytes_ += (uint64_t)bytes;
}

void LifetimeStats::onBattery(int pct) {
    // Cycle = crossed LOW (<=20%) then HIGH (>=80%). Track rising and
    // falling legs independently so a single partial discharge doesn't count.
    if (pct <= 20) {
        batWasLow_  = true;
        batWasHigh_ = false;
    } else if (pct >= 80 && batWasLow_) {
        batWasHigh_ = true;
        batWasLow_  = false;
        ++battCycles_;
    }
}

// -----------------------------------------------------------------------
void LifetimeStats::tickSave() {
    if (!ready_) return;
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastSaveMs_;

    // Check if any counter has moved beyond its delta threshold.
    const bool distDirty  = (distanceKm_    - savedDistKm_)  >= kDeltaDist;
    const bool altDirty   = (altGainM_      - savedAltM_)    >= kDeltaAlt;
    const bool recDirty   = (recordingSecs_ - savedRecSecs_)  >= kDeltaRec;
    const bool idleDirty  = (idleSecs_      - savedIdleSecs_) >= kDeltaIdle;
    const bool uplDirty   = (wifiUploads_   - savedUploads_)  >= kDeltaUpl;
    const bool spkDirty   = (spikeEvents_   - savedSpikes_)   >= kDeltaSpk;
    const bool cellDirty  = (uniqueCells_   - savedCells_)    >= kDeltaCells;
    const bool byteDirty  = (totalBytes_    - savedBytes_)     >= (uint64_t)1024;
    const bool batDirty   = (battCycles_    - savedBatCyc_)   >= kDeltaBat;

    const bool anyDirty = distDirty || altDirty || recDirty || idleDirty ||
                          uplDirty  || spkDirty  || cellDirty || byteDirty || batDirty;

    if (anyDirty || elapsed >= kSaveIntervalMs) {
        saveToNvs();
    }
}

// -----------------------------------------------------------------------
void LifetimeStats::reset() {
    Serial.printf("[LIFE] reset by user (dist=%.2fkm alt=%.1fm rec=%us idle=%us upl=%u spk=%u cells=%u)\n",
                  distanceKm_, altGainM_, (unsigned)recordingSecs_, (unsigned)idleSecs_,
                  (unsigned)wifiUploads_, (unsigned)spikeEvents_, (unsigned)uniqueCells_);
    distanceKm_    = 0.0f;
    altGainM_      = 0.0f;
    recordingSecs_ = 0;
    idleSecs_      = 0;
    wifiUploads_   = 0;
    spikeEvents_   = 0;
    uniqueCells_   = 0;
    totalBytes_    = 0;
    battCycles_    = 0;
    batWasHigh_    = false;
    batWasLow_     = false;
    prevPosValid_  = false;
    memset(cellTable_, 0xFF, sizeof(cellTable_));
    cellTableFull_ = false;
    saveToNvs();
}
