#pragma once
#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>

// =============================================================================
// Always-on day-bucketed CSV writer (firmware v0.4.0+).
//
// The legacy "session" concept (one file per user-driven start/stop) has been
// replaced by automatic per-day rotation. As long as the RadiaCode is connected
// AND a valid GPS UTC timestamp with a GPS fix is available, samples flow into
// /sessions/<YYYY-MM-DD>.csv where the date is computed in the user's local
// timezone (see cfg::LOCAL_TZ in config.h).
//
// File lifecycle
// --------------
//   /sessions/<day>.csv               -- today's open, being-appended-to file.
//   /sessions/<day>.<bootMs>.up.csv   -- a rotated file queued for upload.
//
// The Wi-Fi uploader (core 0) calls rotateForUpload() at the start of every
// upload cycle. That call atomically renames the active <day>.csv to a
// <day>.<bootMs>.up.csv file under the same recording mutex that protects
// append(), then forgets it. The next append() reopens a fresh <day>.csv
// and continues without losing a single sample. On HTTP 2xx the uploader
// deletes the rotated file. On failure the .up.csv is retried next cycle.
// At no point are duplicate sample rows kept on the device.
//
// Schema (12 columns, firmware v0.8.0+):
//   timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,
//   speedKph,bearingDeg,altitudeM,hdop,event,accuracyM
//
// Column 11 `event` (added v0.7.0) holds GPS_LOST / GPS_REGAINED transition
// tags on dedicated event rows -- empty on normal samples.
// Column 12 `accuracyM` (added v0.8.0) holds estimated horizontal accuracy in
// metres, computed as `hdop * cfg::GPS_UERE_M`. Stored alongside the raw
// HDOP so downstream consumers (viewer, exports, historical RadiaCode track
// data that only has metres) can use either value uniformly.
// =============================================================================

class SessionStore {
public:
    enum class Backend { None, LittleFs, Sd, SdFat, Failed };

    struct SessionInfo {
        String   id;          // either "<day>" or "<day>.<bootMs>.up" for rotated
        size_t   sizeBytes;
        uint32_t samples;
    };

    struct PendingUpload {
        String filename;      // basename inside SESSIONS_DIR, e.g. "2026-05-11.1234567.up.csv"
        String sessionId;     // <day> derived from prefix, e.g. "2026-05-11"
        size_t sizeBytes;
    };

    bool begin();
    Backend       backend()       const { return backend_; }
    const char*   backendName()   const;
    bool          sdMounted()     const { return backend_ == Backend::Sd || backend_ == Backend::SdFat; }
    bool          storageFailed() const { return backend_ == Backend::Failed; }
    uint64_t      cardSizeMb()    const { return cardSizeMb_; }

    // True while a day file is currently open and being appended to.
    bool          isRecording() const { return recording_; }
    // Current day id ("YYYY-MM-DD") or empty string when waiting for first sample.
    const String& activeId()    const { return activeId_; }
    // Rows written to the currently-open day file since the most recent rotate.
    uint32_t      sampleCount() const { return sampleCount_; }
    // Total samples written this boot (never resets on rotate/rollover).
    // Use this for any "how much have I captured?" UI — sampleCount() drops to
    // 0 every upload cycle when the active file is rotated, which confuses users.
    uint32_t      lifetimeSamples() const { return lifetimeSamples_; }

    // Reopen today's day file if one is present from a previous run, and
    // rotate any stale non-today day files to pending-upload state. Safe to
    // call before GPS UTC is acquired (in that case stale files are left
    // alone until the next upload cycle takes care of them).
    bool resumeIfActive();

    // Append one sample. The always-on contract:
    //   - drops if no backend, OR
    //   - drops if hasGps == false, OR
    //   - drops if timestampMsFull is older than 2020-01-01.
    // When the local-eastern day rolls over mid-append, the previous day
    // file is rotated to pending-upload state and a new <today>.csv is opened
    // transparently. No samples are lost across the rollover.
    void append(uint32_t timestampMsLow,    // legacy, ignored
                uint64_t timestampMsFull,
                float uSvPerHour,
                float cps,
                bool hasGps, double lat, double lng,
                const String& deviceId,
                float speedKph   = -1.f,
                float bearingDeg = -1.f,
                float altitudeM  = -9999.f,
                float hdop       = -1.f,
                float accuracyM  = -1.f);

    // v0.7.0: Append a GPS-state-transition event row to the active day file.
    // Event rows occupy the 11th CSV column (`event`); all other fields
    // except timestamp and deviceId are empty. Bypasses the no-GPS gate in
    // append() because the entire point of GPS_LOST is "we had no fix".
    // Tag should be a short ASCII constant like "GPS_LOST" or "GPS_REGAINED".
    void appendEvent(uint64_t timestampMsFull,
                     const char* eventTag,
                     const String& deviceId);

    // Storage stats
    size_t totalBytes() const;
    size_t usedBytes() const;
    int    percentUsed() const;
    int    sessionCount() const;   // total .csv + .up.csv files

    // ---------------------------------------------------------------------
    // Upload integration (called by WifiUploader on core 0).
    // ---------------------------------------------------------------------

    // Rotate the active day file (if any) to a unique <day>.<millis>.up.csv
    // pending-upload file. Also rotates any stale non-today <day>.csv files.
    // Returns the total number of pending-upload files on disk afterwards.
    // Safe across cores: holds the same mutex as append().
    uint32_t rotateForUpload();

    // List every pending-upload file currently on disk (the .up.csv files
    // produced by rotateForUpload()).
    std::vector<PendingUpload> listPendingUploads() const;

    // Delete one pending-upload file by its basename.
    bool removePendingUpload(const String& filename);

    // Open a pending-upload file as a Stream so HTTPClient can post it
    // without buffering the whole body in heap. Returns nullptr for SdFat
    // (caller should fall back to readPendingUploadToString()).
    Stream* openPendingUploadStream(const String& filename, size_t& outSizeBytes);
    void    closeSessionStream();
    bool    readPendingUploadToString(const String& filename, size_t maxBytes, String& out) const;

    // ---------------------------------------------------------------------
    // Diagnostics / serial-console operations (unchanged surface).
    // ---------------------------------------------------------------------
    std::vector<SessionInfo> listSessions() const;
    bool     dumpSession(const String& id, Stream& out) const;
    void     dumpAll(Stream& out) const;
    uint32_t wipeAll();
    bool     removeSession(const String& id);
    bool     isActive(const String& id) const { return recording_ && activeId_ == id; }
    bool     readSessionToString(const String& id, size_t maxBytes, String& out) const;

    // Convert an epoch-ms UTC timestamp to its local-eastern YYYY-MM-DD string.
    // Returns "" if epochMs is pre-2020 (sentinel for "no UTC yet").
    // Requires tzset() to have been called with cfg::LOCAL_TZ.
    static String dayIdFromEpochMs(uint64_t epochMs);

private:
    bool hasUsableBackend() const {
        return backend_ == Backend::SdFat
            || backend_ == Backend::Sd
            || backend_ == Backend::LittleFs;
    }

    // Internal helpers. All called with mutex_ already held.
    bool openDayFile_(const String& dayId);   // creates/reopens <dayId>.csv
    bool rotateActiveToPending_();             // <activeId>.csv -> <activeId>.<ms>.up.csv
    uint32_t rotateStaleDayFiles_();           // any non-today *.csv -> *.<ms>.up.csv

    bool     recording_   = false;
    String   activeId_;
    uint32_t sampleCount_ = 0;
    uint32_t lifetimeSamples_ = 0;   // monotonic since boot, never reset on rotate
    Backend  backend_     = Backend::None;
    fs::FS*  fs_          = nullptr;
    uint64_t cardSizeMb_  = 0;
    bool     sdFatPreflightOk_ = false;
    fs::File openedStreamFile_;
    SemaphoreHandle_t mutex_ = nullptr;
};
