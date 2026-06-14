#pragma once
#include <Arduino.h>

// =============================================================================
// Lifetime statistics accumulator (firmware v1.0.0+)
//
// A small set of NVS-backed counters that survive every reboot and never
// consume significant flash storage (~64 bytes total in the "life" namespace).
// All accumulators are updated from the main Arduino loop (Core 1) so there
// is no cross-core contention. Public setters are plain integer/float ops —
// no locking required.
//
// Counters tracked:
//   distanceKm       — cumulative GPS haversine distance (km)
//   altGainM         — cumulative positive altitude gain (m)
//   recordingSecs    — seconds GPS-fixed + RadiaCode-connected (recording secs)
//   wifiUploads      — successful HTTP 2xx upload completions
//   spikeEvents      — times CPS exceeded SPIKE_CPS_THRESHOLD
//   uniqueCells      — distinct 0.01-degree grid cells visited
//   totalBytes       — bytes written to session CSV storage (cumulative)
//   battCycles       — charge/discharge cycles detected (>20% → >80%)
//
// NVS save policy (identical to trip-dose):
//   Written every LIFE_NVS_SAVE_INTERVAL_MS OR when any counter changes by
//   more than its individual delta threshold, whichever comes first.
//   This keeps flash wear to a few writes per hour in typical use.
//
// Reset policy:
//   Call reset() to zero all counters immediately and write NVS.
//   Intended to be triggered by a long-press on the LIFETIME screen.
// =============================================================================

class LifetimeStats {
public:
    // CPS threshold for counting a "radiation spike event".
    static constexpr float SPIKE_CPS_THRESHOLD = 50.0f;

    // Grid cell resolution for unique-location counting.
    // 0.01 degrees ≈ 1.1 km at the equator.
    static constexpr float CELL_DEG = 0.01f;

    // Load all counters from NVS. Call once in setup() after LittleFS ready.
    void begin();

    // ---------------------------------------------------------------------------
    // Update helpers — call from main loop with validated data.
    // All return immediately without blocking; only update in-memory state.
    // NVS is written by tickSave() on the periodic cadence.
    // ---------------------------------------------------------------------------

    // Call once per GPS fix with the current lat/lng/altitude.
    // Internally tracks previous position and computes haversine deltas.
    // altM: altitude in metres; altValid: false when alt data is unavailable.
    void onGpsFix(double lat, double lng, double altM, bool altValid);

    // Call with every valid RadiaCode reading that also has a GPS fix.
    // dt_ms is milliseconds since the previous call (for recording-time accrual).
    // cps is the raw count rate.
    void onSample(float cps, uint32_t dt_ms);
    // Called once per second when the device is powered on but NOT actively
    // recording (no GPS fix, RC disconnected, or sample not yet available).
    // dt_ms: elapsed milliseconds since the last idle tick (capped at 10 s).
    void onIdleTick(uint32_t dt_ms);
    // Called by WifiUploader after each successful HTTP 2xx response.
    void onUploadSuccess();

    // Called by SessionStore::append() with the number of bytes just written.
    void onBytesWritten(size_t bytes);

    // Battery cycle detection. Call with the current battery percent
    // (0-100) once per battery sample interval.
    void onBattery(int pct);

    // Periodic NVS flush. Call from main loop; handles its own internal timer.
    void tickSave();

    // Zero all counters and write NVS immediately.
    void reset();

    // ---------------------------------------------------------------------------
    // Accessors
    // ---------------------------------------------------------------------------
    float    distanceKm()     const { return distanceKm_; }
    float    altGainM()       const { return altGainM_; }
    uint32_t recordingSecs()  const { return recordingSecs_; }
    uint32_t idleSecs()       const { return idleSecs_; }
    uint32_t wifiUploads()    const { return wifiUploads_; }
    uint32_t spikeEvents()    const { return spikeEvents_; }
    uint32_t uniqueCells()    const { return uniqueCells_; }
    uint64_t totalBytes()     const { return totalBytes_; }
    uint32_t battCycles()     const { return battCycles_; }

    // True once begin() has been called successfully.
    bool ready() const { return ready_; }

private:
    // NVS helpers
    void loadFromNvs();
    void saveToNvs();

    // Unique-cell tracking — lightweight hash set using a fixed-size
    // open-address table. Stored ONLY in RAM (not NVS) because the
    // unique-cell count itself is persisted; the full set is rebuilt
    // lazily from GPS history. On reboot the counter resumes from the
    // NVS value; only NEW cells encountered post-reboot increment it,
    // so the NVS value is always a lower bound (never over-counts).
    static constexpr uint16_t CELL_TABLE_SIZE = 512;  // must be power of 2
    uint32_t cellTable_[CELL_TABLE_SIZE];
    bool     cellTableFull_ = false;

    // Encode a lat/lng pair into a 32-bit grid key (grid = 0.01 deg).
    // Returns 0xFFFFFFFF for invalid (sentinel "empty" value in table).
    static uint32_t cellKey(double lat, double lng);
    // Returns true if the cell was new (first visit); increments uniqueCells_.
    bool visitCell(double lat, double lng);

    // In-memory counters
    float    distanceKm_    = 0.0f;
    float    altGainM_      = 0.0f;
    uint32_t recordingSecs_ = 0;
    uint32_t idleSecs_      = 0;
    uint32_t wifiUploads_   = 0;
    uint32_t spikeEvents_   = 0;
    uint32_t uniqueCells_   = 0;
    uint64_t totalBytes_    = 0;
    uint32_t battCycles_    = 0;

    // Battery cycle state machine
    bool     batWasHigh_    = false;  // true after crossing 80%
    bool     batWasLow_     = false;  // true after crossing 20%

    // NVS save gate
    float    savedDistKm_   = 0.0f;
    float    savedAltM_     = 0.0f;
    uint32_t savedRecSecs_  = 0;
    uint32_t savedIdleSecs_ = 0;
    uint32_t savedUploads_  = 0;
    uint32_t savedSpikes_   = 0;
    uint32_t savedCells_    = 0;
    uint64_t savedBytes_    = 0;
    uint32_t savedBatCyc_   = 0;
    uint32_t lastSaveMs_    = 0;

    // GPS tracking for deltas
    double   prevLat_       = 0.0;
    double   prevLng_       = 0.0;
    double   prevAlt_       = 0.0;
    bool     prevPosValid_  = false;

    bool     ready_         = false;
};
