export const meta = {
  name: 'implement-upload-fixes',
  description: 'Implement all upload fixes and crash display',
  phases: [
    { title: 'Server Fix', detail: 'tracker_ingest_api.py header-only acceptance' },
    { title: 'Device Core', detail: 'session_store.cpp rotation validation, fs flush' },
    { title: 'Device Upload', detail: 'wifi_uploader.cpp pre-upload validation, quick-fail' },
    { title: 'Device UI', detail: 'crash display feature across ui.h, ui.cpp, main.cpp' },
    { title: 'Verification', detail: 'Build check and review all changes' },
  ],
}

// Phase 1: Server fix
phase('Server Fix')
const serverFix = await agent(`
Fix tracker_ingest_api.py line ~1706 to accept header-only CSV uploads.

Current code:
```python
if not docs:
    raise HTTPException(status_code=400, detail="no parseable rows")
```

Change to:
```python
if not docs and rejected == 0:
    # Header-only upload accepted (no data rows to insert, but valid request):
    return JSONResponse(
        content={
            "sessionId": session_id,
            "received": 0,
            "valid": 0,
            "rejected": 0,
            "inserted": 0,
            "duplicates": 0,
            "firstTsMs": 0,
            "lastTsMs": 0,
        },
        status_code=200,
    )
if not docs:
    raise HTTPException(status_code=400, detail="no parseable rows")
```

The key change: when docs is empty AND rejected is 0 (meaning the CSV was valid but had no data rows, only a header), accept it with HTTP 200 instead of rejecting with 400.

Report the exact line numbers changed and the diff.
`, { schema: {
  type: "object",
  properties: {
    file: { type: "string" },
    lineBefore: { type: "integer" },
    lineAfter: { type: "integer" },
    diff: { type: "string" },
  },
  required: ["file", "diff"],
}})

// Phase 2: Device core — session_store.cpp
phase('Device Core')
const deviceCore = await agent(`
Fix session_store.cpp for two issues:

### Fix 1: Validate data rows on rotation (Issue 3A)
In `rotateActiveToPending_()`, before renaming the file, count data rows.
Only proceed with rotation if data rows > 0.

Add a helper function `countDataRows_(const String& filename)` that opens a file and counts non-empty lines (or specifically, counts lines that are NOT the header row and NOT empty).

In `rotateActiveToPending_()` (around line 461), add:
```cpp
// Count data rows before rotating
uint32_t dataRows = countDataRows_(activeId_);
if (dataRows == 0) {
    // File has no data — don't rotate it to pending
    Serial.printf("[REC] skip rotate: %s has %u data rows\\n", activeId_.c_str(), (unsigned)dataRows);
    return false;
}
```

### Fix 2: Filesystem flush before/after critical ops (Issue 4A, 4B)
In `rotateStaleDayFiles_()` and `removePendingUpload()`, add fs flush calls:

For LittleFS backend:
- Call `LittleFS.commit()` before and after `fs_->rename()` and `fs_->remove()`

For SdFat backend:
- Call `gSdFat.sync()` after write operations

For Sd (SD library):
- Call `SD.end()`/`SD.begin()` pattern or use `SPI.end()`/`SPI.begin()` if needed

Also add `flush()` after `f.print(...)` or `f.write(...)` in `append()` before `f.close()`.

Report the exact line numbers changed and the diff.
`, { schema: {
  type: "object",
  properties: {
    file: { type: "string" },
    lineBefore: { type: "integer" },
    lineAfter: { type: "integer" },
    diff: { type: "string" },
  },
  required: ["file", "diff"],
}})

// Phase 3: Device upload — wifi_uploader.cpp
phase('Device Upload')
const deviceUpload = await agent(`
Fix wifi_uploader.cpp for three issues:

### Fix 1: Pre-upload validation (Issue 1B)
In `uploadOne()`, before calling `http.sendRequest()`, check if the file has data rows.
If the file is header-only (fileSize <= ~160 bytes, which is approximately the header row size), skip the upload and remove the file.

Add after the zero-byte check (around line 356-360):
```cpp
if (fileSize > 0 && fileSize < 200) {
    // Likely header-only file — skip upload, remove directly
    Serial.printf("[UPLOAD] %s: header-only file (%u bytes), removing\\n",
                  sessionId.c_str(), (unsigned)fileSize);
    store_->closeSessionStream();
    if (store_->removePendingUpload(filename)) {
        ++ok;
        ++uploadedCount_;
        return true;
    }
    return false;
}
```

### Fix 2: WiFi quick-fail for unreachable networks (Issue 5B)
In `connectWifi()`, add a simple network reachability tracker.
After `tryConnect` fails for a network, increment a counter.
If the counter for a network exceeds 5, skip it entirely (don't call `WiFi.begin`).

Add two member variables to wifi_uploader.h:
```cpp
volatile uint32_t homeFailCount_ = 0;
volatile uint32_t remoteFailCount_ = 0;
```

In `connectWifi()`'s `tryConnect` failure path:
- If home, `homeFailCount_++`, reset remoteFailCount_
- If remote, `remoteFailCount_++`, reset homeFailCount_
- If `homeFailCount_ > 5`, skip home network
- If `remoteFailCount_ > 5`, skip remote network

Report the exact line numbers changed and the diff.
`, { schema: {
  type: "object",
  properties: {
    file: { type: "string" },
    lineBefore: { type: "integer" },
    lineAfter: { type: "integer" },
    diff: { type: "string" },
  },
  required: ["file", "diff"],
}})

// Phase 4: Device UI — crash display
phase('Device UI')
const deviceUI = await agent(`
Implement crash display feature across ui.h, ui.cpp, and main.cpp.

### ui.h changes:
1. Add `SCREEN_CRASH` to the Screen enum (after SCREEN_NORMAL_COUNT = 7)
2. Add member variables:
   - `bool crashActive_ = false;`
   - `String crashReasonText_ = String();`
   - `String crashPhaseText_ = String();`
   - `String crashUptimeText_ = String();`
   - `String crashExplanationText_ = String();`
3. Add method:
   - `void enterCrashScreen(const char* resetReason, const char* phase, uint32_t uptimeMs, int vbatMv);`
4. Add helper:
   - `String formatUptime(uint32_t ms);` — converts ms to HH:MM:SS

### ui.cpp changes:
1. Add `renderCrash()` method that draws:
   - y=14: "CRASH DETECTED" in RED, size-2, centered
   - y=26: "Reason: <BROWNOUT/PANIC/etc>" in AMBER, size-1
   - y=36: "<explanation>" in DIM, size-1
   - y=50: "Phase: <phase>" in DIM, size-1
   - y=60: "Uptime: HH:MM:SS" in DIM, size-1
   - y=70: "Hold button to continue" in DIM, size-1

2. Add `case SCREEN_CRASH: renderCrash(); break;` in the existing tick() switch

3. Modify `onShortPress()`:
   - If `crashActive_`, return early (no-op)

4. Modify `onLongPress()`:
   - If `crashActive_`, clear crash: `crashActive_ = false; screen_ = SCREEN_STATS; forceFullRedraw_ = true; return;`

### main.cpp changes:
After `event_log::beginBoot()`, add:
```cpp
if (event_log::wasLastResetCrash()) {
    // Parse the last BOOT record for crash details
    // For now, just pass known values
    gUi.enterCrashScreen("BROWNOUT", "UNKNOWN", 0, -1);
}
```

Actually, the simplest approach: add a method to event_log.h/cpp that returns the last reset reason string. Then use that in main.cpp.

In event_log.cpp, the BOOT record is already written. But we need the info at boot time, BEFORE the log is written. The boot record uses `resetReasonName(esp_reset_reason())`. So in main.cpp, after event_log::beginBoot():
```cpp
if (event_log::wasLastResetCrash()) {
    gUi.enterCrashScreen(
        // We'll pass the string representation
        // But we need to get it from the boot record that was just written
        // Simplest: call reset_reason_name() again here
        resetReasonName(esp_reset_reason()), // This won't work because we're on current boot
        // Better: have event_log::beginBoot() return the previous reason
    );
}
```

Actually, the cleanest approach:
- Add `const char* event_log::lastCrashReason()` — but we need the PREVIOUS boot's reason, not this boot's.
- The BOOT record was just written for THIS boot. The PREVIOUS boot's reason is in the last line of system.log (or system.log.old).

Wait — actually, event_log::beginBoot() already has the previous boot's reset reason because it calls `esp_reset_reason()` and writes it. So we can store it in a static variable:

In event_log.h, add:
```cpp
// Returns the reset reason from the PREVIOUS boot's crash (if wasLastResetCrash() is true).
const char* previousResetReason();
```

In event_log.cpp, in beginBoot(), after computing rname:
```cpp
g_previousResetReason = rname;  // Store for UI
```

In main.cpp:
```cpp
event_log::beginBoot();
if (event_log::wasLastResetCrash()) {
    gUi.enterCrashScreen(
        event_log::previousResetReason(),
        // For phase, we need to read from RTC — add a method
        event_log::lastCrashPhase(),
        // For uptime, need to read from RTC
        (uint32_t)event_log::lastUptimeMs(),
        // For voltage, we have trackerLastVbat()
        -1 // Will be set by UI
    );
}
```

Add to event_log.h:
```cpp
const char* lastCrashPhase();
uint32_t lastUptimeMs();
```

In event_log.cpp, store RTC values as globals:
```cpp
static const char* g_lastCrashPhase = "";
static uint32_t g_lastUptime = 0;
```

In beginBoot(), set these from the RTC read values before resetting them.

Report the complete diffs for all three files (ui.h, ui.cpp, main.cpp).
`, { schema: {
  type: "object",
  properties: {
    file: { type: "string" },
    diff: { type: "string" },
  },
  required: ["file", "diff"],
}})

// Phase 5: Config fix
phase('Config Fix')
const configFix = await agent(`
Fix config.h — reduce WiFi connect timeout.

In config.h, add or modify:
```cpp
// WiFi connect timeout reduced from 25s to 15s for faster failure detection.
// Home APs typically respond within 2-3s. Hotspots within 1-2s.
// 15s still covers slow/stale APs. (was 25000, changed to 15000)
// This is in secrets.h as WIFI_CONNECT_TIMEOUT_MS.
```

Actually, WIFI_CONNECT_TIMEOUT_MS is in secrets.h, not config.h. In secrets.h:
```cpp
// WiFi connection timeout per SSID (ms). Reduced to 15s for faster failure.
// Was 25000.
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
```

Report the exact line change.
`)

// Combine results
console.log('Server Fix:', JSON.stringify(serverFix))
console.log('Device Core:', JSON.stringify(deviceCore))
console.log('Device Upload:', JSON.stringify(deviceUpload))
console.log('Device UI:', JSON.stringify(deviceUI))
console.log('Config:', JSON.stringify(configFix))