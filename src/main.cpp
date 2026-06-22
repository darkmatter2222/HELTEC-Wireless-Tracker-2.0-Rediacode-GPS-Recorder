// HTIT-Tracker firmware entry point.
// - Heltec WiFi LoRa 32 V3 (ESP32-S3) on the HTIT-Tracker V1.2 or V2 carrier
//   (selected at compile time via TRACKER_HW_V1_2 / TRACKER_HW_V2 build flags).
// - Connects (BLE central) to a RadiaCode dosimeter.
// - Logs CSV samples to LittleFS (V2, internal flash) or SD card (V1.2, HW-125)
//   in the same schema as the Android app:
//     timestampMs,uSvPerHour,cps,latitude,longitude,deviceId

#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <cmath>
#include "config.h"
#include "button.h"
#include "event_log.h"
#include "gps_module.h"
#include "lifetime_stats.h"
#include "radiacode.h"
#include "session_store.h"
#include "ui.h"
#include "wifi_uploader.h"

namespace {
Button        gButton;
GpsModule     gGps;
LifetimeStats gLife;
RadiaCode     gRadia;
SessionStore  gStore;
Ui            gUi;
WifiUploader  gWifi;

// ---------------------------------------------------------------------------
// Spectrum collection mode (v1.1.0)
// When enabled, eid=1 spectrum segments from DATA_BUF are parsed and stored.
bool     gSpectrumMode = false;

static void loadSpectrumModeFromNvs() {
    Preferences p;
    p.begin("rctracker", true);  // read-only
    gSpectrumMode = p.getBool("spec_en", cfg::SPECTRUM_COLLECT_DEFAULT);
    p.end();
    Serial.printf("[SPEC] mode=%s\n", gSpectrumMode ? "on" : "off");
}
static void saveSpectrumModeToNvs() {
    Preferences p;
    p.begin("rctracker", false);  // read-write
    p.putBool("spec_en", gSpectrumMode);
    p.end();
}

// ---------------------------------------------------------------------------
// Cumulative trip dose accumulator (v0.5.0)
// Integrates uSv/hr * dt to produce total µSv since last user reset.
// Persisted to NVS every DOSE_NVS_SAVE_INTERVAL_MS so a crash loses at most
// that much accumulation rather than the entire trip total.
float    gTripDoseMicroSv   = 0.0f;
float    gLastSavedDoseUSv  = 0.0f;  // most recent value actually written to NVS
uint32_t gLastDoseMs        = 0;     // millis() of most recent sample intake
uint32_t gLastDoseSaveMs    = 0;     // millis() of most recent NVS write

static void loadDoseFromNvs() {
    Preferences p;
    p.begin("dose", true);  // read-only
    gTripDoseMicroSv = p.getFloat("usv", 0.0f);
    p.end();
    // ESP32 NVS can return NaN/inf if the float was corrupted or partially written.
    // Reset to 0 to prevent infinite dose from poisoning the accumulator.
    if (std::isnan(gTripDoseMicroSv) || std::isinf(gTripDoseMicroSv) || gTripDoseMicroSv < 0.0f) {
        gTripDoseMicroSv = 0.0f;
        Serial.printf("[DOSE] NVS corrupt, reset to 0\n");
    } else {
        Serial.printf("[DOSE] loaded %.4f uSv from NVS\n", gTripDoseMicroSv);
    }
    gLastSavedDoseUSv = gTripDoseMicroSv;
}
static void saveDoseToNvs() {
    Preferences p;
    p.begin("dose", false);
    p.putFloat("usv", gTripDoseMicroSv);
    p.end();
    gLastSavedDoseUSv = gTripDoseMicroSv;
}
// Pure decision helper (also called from the unit test). Returns true if
// the running dose should be flushed to NVS. Two conditions: change
// exceeds delta threshold, OR safety-ceiling interval elapsed.
bool shouldSaveDose(float current, float lastSaved, uint32_t msSinceLastSave,
                    float deltaThresholdUSv, uint32_t maxIntervalMs) {
    const float diff = current - lastSaved;
    const float absDiff = diff < 0.0f ? -diff : diff;
    if (absDiff >= deltaThresholdUSv) return true;
    if (msSinceLastSave >= maxIntervalMs) return true;
    return false;
}
static void resetDose() {
    Serial.printf("[DOSE] reset by user (was %.4f uSv)\n", gTripDoseMicroSv);
    gTripDoseMicroSv = 0.0f;
    gLastDoseMs      = 0;
    saveDoseToNvs();
}

// Pending sample queue. The BLE notification callback runs on the NimBLE
// host task (Core 0) which has a small stack; doing LittleFS open/write/close
// directly there overflowed the stack and panicked ("Stack canary watchpoint
// triggered (nimble_host)") right after the first GPS fix. The callback now
// just snapshots GPS+reading into this slot; the main Arduino loop (Core 1)
// picks it up and does the file I/O on its larger 8 KB stack.
struct PendingSample {
    bool      valid = false;
    uint64_t  ts = 0;
    float     uSv = 0.f, cps = 0.f;
    bool      hasGps = false;
    double    lat = 0.0, lng = 0.0;
    String    deviceId;
    float     speed = -1.f, bearing = -1.f, alt = -9999.f, hdop = -1.f, acc = -1.f;
    // Spectrum data (v1.2.2): working buffer for getSpectrumCache() + built string
    uint16_t  specBuf[cfg::SPECTRUM_MAX_CHANNELS];
    uint16_t  specCount = 0;
    String    spectrumData;  // "count1|count2|..." or empty
};
portMUX_TYPE  gSampleMux = portMUX_INITIALIZER_UNLOCKED;
PendingSample gPendingSample;
} // namespace

// LiPo single-cell OCV-based discharge table (settled, ~20 °C, slow-to-moderate
// discharge).  Source: published TI fuel-gauge reference data / Adafruit LC709203
// equivalents.  The board has no coulomb counter, so voltage is all we have.
// Higher resolution at the top end (4.05–4.20 V) reduces the apparent "sharp drop"
// seen immediately after unplugging from a full charge.
static const struct { float v; int pct; } kLipoTable[] = {
    {4.20f, 100},
    {4.17f,  97},
    {4.14f,  94},
    {4.11f,  91},
    {4.08f,  87},
    {4.05f,  83},
    {4.02f,  79},
    {3.98f,  74},
    {3.95f,  70},
    {3.91f,  65},
    {3.87f,  60},
    {3.83f,  55},
    {3.79f,  50},
    {3.75f,  45},
    {3.71f,  40},
    {3.67f,  35},
    {3.61f,  29},
    {3.55f,  23},
    {3.49f,  17},
    {3.42f,  12},
    {3.36f,   7},
    {3.30f,   3},
    {3.27f,   0},
};
static int kLipoTableLen = (int)(sizeof(kLipoTable) / sizeof(kLipoTable[0]));

// Last measured VBAT voltage. Updated by readBatteryPercent() each call;
// queried by wifi_uploader to skip Wi-Fi when the battery is too weak to
// safely run the PA (which spikes 200-400 mA during scan/associate).
static volatile float g_lastVbat = -1.0f;
float trackerLastVbat() { return g_lastVbat; }

static int readBatteryPercent() {
    digitalWrite(cfg::VBAT_EN_PIN, HIGH);    // enable divider
    delay(10);                               // let the RC settle

    // Use analogReadMilliVolts() which applies the ESP32-S3 factory eFuse ADC
    // calibration curve — much more accurate than raw * 3.3 / 4095.
    // Average 4 readings to reduce noise.
    uint32_t sumMv = 0;
    for (int i = 0; i < 4; i++) {
        sumMv += analogReadMilliVolts(cfg::VBAT_ADC_PIN);
        if (i < 3) delay(2);
    }
    digitalWrite(cfg::VBAT_EN_PIN, LOW);     // disable to save power

    // Reconstruct battery voltage (V), compensating for the resistor divider.
    const float volts = (sumMv / 4.0f / 1000.0f) * cfg::VBAT_DIV_MULT;
    g_lastVbat = volts;

    // Clamp to table extremes.
    if (volts >= kLipoTable[0].v)             return kLipoTable[0].pct;
    if (volts <= kLipoTable[kLipoTableLen-1].v) return kLipoTable[kLipoTableLen-1].pct;

    // Linear interpolation between the two bracketing entries.
    for (int i = 0; i < kLipoTableLen - 1; i++) {
        if (volts >= kLipoTable[i + 1].v) {
            const float span = kLipoTable[i].v - kLipoTable[i + 1].v;
            const float frac = (volts - kLipoTable[i + 1].v) / span;
            return (int)(kLipoTable[i + 1].pct
                        + frac * (kLipoTable[i].pct - kLipoTable[i + 1].pct)
                        + 0.5f);
        }
    }
    return 0;
}

static void enablePeripherals() {
    // VTFT/VGNSS rail. The Heltec HT_st7735 library (used by the reference
    // darkmatter HTIT-Tracker firmware) drives this pin HIGH to enable the
    // 3.3V rail feeding both the ST7735 panel and the UC6580 GNSS module.
    pinMode(cfg::VGNSS_CTRL_PIN, OUTPUT);
    digitalWrite(cfg::VGNSS_CTRL_PIN, HIGH);

    // TFT backlight (active HIGH).
    pinMode(cfg::BL_CTRL_PIN, OUTPUT);
    digitalWrite(cfg::BL_CTRL_PIN, HIGH);

    // Battery divider control (idle LOW; pulsed HIGH only when sampling).
    pinMode(cfg::VBAT_EN_PIN, OUTPUT);
    digitalWrite(cfg::VBAT_EN_PIN, LOW);

    delay(250);   // let regulator + GNSS settle
}

void setup() {
    Serial.begin(115200);
    // Heltec V3 uses native USB CDC; give the host a moment to enumerate so
    // the first prints aren't lost.
    const uint32_t cdcDeadline = millis() + 1500;
    while (!Serial && millis() < cdcDeadline) { delay(10); }
    Serial.println();
    Serial.printf("HTIT-Tracker firmware v%s starting...\n", cfg::FW_VERSION);

    // Task watchdog (v0.6.0). Arms a 30 s timer on the Arduino loop task.
    // Any subsystem that wedges the main loop (deadlocked mutex, infinite
    // BLE callback, etc.) will trigger a clean ESP_RST_TASK_WDT reset
    // rather than the device silently going unresponsive. The Wi-Fi
    // uploader task subscribes itself in its own taskLoop().
    esp_task_wdt_init(cfg::TASK_WDT_TIMEOUT_S, /*panic=*/true);
    esp_task_wdt_add(NULL);
    Serial.printf("[BOOT] task_wdt armed: %us, panic-on-timeout\n",
                  (unsigned)cfg::TASK_WDT_TIMEOUT_S);

    // Log the previous reset reason so out-of-range / brown-out / watchdog
    // resets are immediately visible in the boot log instead of looking
    // like a normal power-on. Critical for diagnosing field reboots.
    {
        const esp_reset_reason_t rr = esp_reset_reason();
        const char* name = "unknown";
        switch (rr) {
            case ESP_RST_POWERON:   name = "POWERON";       break;
            case ESP_RST_EXT:       name = "EXT";           break;
            case ESP_RST_SW:        name = "SW";            break;
            case ESP_RST_PANIC:     name = "PANIC";         break;
            case ESP_RST_INT_WDT:   name = "INT_WDT";       break;
            case ESP_RST_TASK_WDT:  name = "TASK_WDT";      break;
            case ESP_RST_WDT:       name = "WDT";           break;
            case ESP_RST_DEEPSLEEP: name = "DEEPSLEEP";     break;
            case ESP_RST_BROWNOUT:  name = "BROWNOUT";      break;
            case ESP_RST_SDIO:      name = "SDIO";          break;
            default: break;
        }
        Serial.printf("[BOOT] reset reason: %s (%d)\n", name, (int)rr);
    }

    // Configure the local timezone so SessionStore::dayIdFromEpochMs() returns
    // local Eastern-time YYYY-MM-DD instead of UTC. Done once at boot so all
    // subsequent localtime_r() calls (in any task) see the same TZ.
    setenv("TZ", cfg::LOCAL_TZ, 1);
    tzset();
    Serial.printf("[BOOT] timezone set to %s (always-on day-bucketed recording)\n",
                  cfg::LOCAL_TZ);

    enablePeripherals();
    gButton.begin(cfg::BUTTON_PIN, cfg::BUTTON_DEBOUNCE_MS, cfg::BUTTON_LONG_PRESS_MS);

    gUi.begin();
    gGps.begin();

    if (!gStore.begin()) {
        if (gStore.storageFailed()) {
            Serial.println("[STORE] FATAL: SD card required but not detected.");
            Serial.println("[STORE]        Recording is DISABLED until reboot.");
            Serial.println("[STORE]        Reseat the card / check 5V on HW-125 VCC, then power-cycle.");
        } else {
            Serial.println("[STORE] FATAL: no backend available -- recording disabled");
        }
    } else {
        Serial.printf("[STORE] backend=%s used=%u total=%u",
                      gStore.backendName(),
                      (unsigned)gStore.usedBytes(), (unsigned)gStore.totalBytes());
        if (gStore.sdMounted()) {
            Serial.printf(" cardSizeMB=%llu", (unsigned long long)gStore.cardSizeMb());
        }
        Serial.println();
        gStore.resumeIfActive();
    }

    // Initialise the persistent event log on LittleFS. Reads RTC slow
    // memory markers from the previous boot, appends a BOOT record (incl.
    // reset reason + wifiInFlight flag) so we can diagnose battery
    // brown-outs that happen while the user isn't watching serial.
    event_log::beginBoot();

    // Reload the cumulative trip dose from NVS so crashes don't wipe the
    // user's accumulated total.  Done after gStore.begin() so LittleFS is
    // already mounted and the NVS partition is accessible.
    loadDoseFromNvs();

    // Load spectrum collection mode from NVS (v1.1.0).
    loadSpectrumModeFromNvs();

    // Load lifetime statistics from NVS (v1.0.0).
    gLife.begin();

    gWifi.begin(&gStore);
    // Wire lifetime upload counter — incremented on the wifi_up task (core 0).
    // LifetimeStats::onUploadSuccess() is a single atomic increment; safe.
    gWifi.setUploadSuccessCb([]() { gLife.onUploadSuccess(); });

    gUi.setSources(&gGps, &gStore, &gRadia);
    gUi.setWifi(&gWifi);
    gUi.setLifetimeStats(&gLife);
    gUi.setRadiaState(RadiaCode::State::Idle, String());

    gRadia.begin(
        // onReading
        [](const RadiaCode::Reading& r) {
            event_log::markPhase("RC_CB");
            gUi.setReading(r);

            // Build deviceId = address w/o colons (compact)
            String id = gRadia.peerAddress();
            id.replace(":", "");

            // CRITICAL (v0.4.4): do NOT touch the filesystem here. This
            // callback runs on the NimBLE host task (Core 0, ~4 KB stack);
            // LittleFS.open() during ST_OPEN_DAY consumed enough stack to
            // trip the canary and crash on the first GPS-fixed sample. We
            // snapshot the data into a portMUX-protected slot and let the
            // main Arduino loop (Core 1, 8 KB stack) call gStore.append().
            PendingSample snap;
            snap.valid    = true;
            snap.ts       = gGps.bestEpochMs();
            snap.uSv      = r.uSvPerHour;
            snap.cps      = r.cps;
            snap.hasGps   = gGps.hasFix();
            snap.lat      = gGps.latitude();
            snap.lng      = gGps.longitude();
            snap.deviceId = id;
            snap.speed    = cfg::FIELD_SPEED_KPH   ? (float)gGps.speedKph()           : -1.f;
            snap.bearing  = cfg::FIELD_BEARING_DEG ? (float)gGps.bearingFromHistory() : -1.f;
            snap.alt      = cfg::FIELD_ALTITUDE_M  ? (float)gGps.altitudeMeters()     : -9999.f;
            snap.hdop     = cfg::FIELD_HDOP        ? (float)gGps.hdop()               : -1.f;
            snap.acc      = cfg::FIELD_ACCURACY_M  ? (float)gGps.accuracyMeters()     : -1.f;
            // Spectrum data is NOT built here — the callback runs on the NimBLE
            // host task (Core 0, ~4 KB stack). String + pipe-delimited building
            // is deferred to the main loop (Core 1, 8 KB stack) below.
            portENTER_CRITICAL(&gSampleMux);
            gPendingSample = snap;
            portEXIT_CRITICAL(&gSampleMux);
        },
        // onState
        [](RadiaCode::State s, const String& addr) {
            gUi.setRadiaState(s, addr);
            Serial.printf("[RC] state=%d addr=%s\n", (int)s, addr.c_str());
        }
    );

    // Apply spectrum mode from NVS (v1.1.0).
    gRadia.setSpectrumMode(gSpectrumMode);
    gUi.setSpectrumMode(gSpectrumMode);

    Serial.println("Setup complete");
}

// --- Serial command interface ----------------------------------------------
// Lets the host PC drive the device without physical button presses:
//   s         start a manual BLE scan (15s)
//   l         list current scan results (with index, addrType, rssi, name)
//   c <idx>   connect to scan-result index (uses captured addrType)
//   x         cancel scan / disconnect
//   f         forget last peer (and trigger rescan)
//   ?         help
static String  gCmdBuf;
static bool    gManualScanArmedSerial = false;

static void handleSerialCommand(const String& line) {
    if (line.length() == 0) return;
    const char c = line[0];
    if (c == '?' || c == 'h') {
        Serial.println("[CMD] commands: s, l, c <idx|addr [type]>, x, f, D, t <pat>");
        Serial.println("[CMD]           LS                  - list sessions");
        Serial.println("[CMD]           DUMP <id>           - stream one session csv");
        Serial.println("[CMD]           DUMPALL             - stream all sessions");
        Serial.println("[CMD]           WIPE <count>        - delete all sessions (count must match LS)");
        Serial.println("[CMD]           STATFS              - show filesystem usage");
        Serial.println("[CMD]           GPASSTHRU [secs]    - dump raw GPS NMEA");
        Serial.println("[CMD]           GREBAUD             - re-probe GPS bauds");
        Serial.println("[CMD]           g                   - GPS quick status");
        Serial.println("[CMD]           SYNC                - force Wi-Fi upload now");
        Serial.println("[CMD]           WIFISTAT            - Wi-Fi uploader status");
        Serial.println("[CMD]           SDSTAT              - SD/LittleFS backend status");
        Serial.println("[CMD]           LOG                 - dump persistent event log");
        Serial.println("[CMD]           LOGCLEAR            - erase persistent event log");
        Serial.println("[CMD]           SPCON               - enable spectrum collection");
        Serial.println("[CMD]           SPOFF               - disable spectrum collection");
        Serial.println("[CMD]           SPSTAT              - show spectrum mode status");
        Serial.println("[CMD]           REBOOT              - soft-reset device (data safe)");
        return;
    }

    // Multi-char keyword commands (case-insensitive). Handled before the
    // single-letter fallthrough so e.g. `LS` doesn't get matched as `l`.
    String upper = line; upper.toUpperCase(); upper.trim();
    if (upper == "LS") {
        auto sessions = gStore.listSessions();
        Serial.printf("[LS] %u sessions on /sessions:\n", (unsigned)sessions.size());
        for (const auto& s : sessions) {
            Serial.printf("  %s  bytes=%u  samples=%u%s\n",
                          s.id.c_str(), (unsigned)s.sizeBytes, (unsigned)s.samples,
                          (gStore.activeId() == s.id) ? "  (active)" : "");
        }
        Serial.printf("[LS-END] count=%u\n", (unsigned)sessions.size());
        return;
    }
    if (upper.startsWith("DUMP ")) {
        String id = line.substring(5); id.trim();
        gStore.dumpSession(id, Serial);
        return;
    }
    if (upper == "DUMPALL") {
        gStore.dumpAll(Serial);
        return;
    }
    if (upper == "STATFS") {
        Serial.printf("[STATFS] backend=%s used=%u total=%u pct=%d sessions=%d",
                      gStore.backendName(),
                      (unsigned)gStore.usedBytes(), (unsigned)gStore.totalBytes(),
                      gStore.percentUsed(), gStore.sessionCount());
        if (gStore.sdMounted()) {
            Serial.printf(" cardSizeMB=%llu", (unsigned long long)gStore.cardSizeMb());
        }
        Serial.println();
        return;
    }
    if (upper == "LOG") {
        event_log::dump(Serial);
        return;
    }
    if (upper == "LOGCLEAR") {
        event_log::clear();
        return;
    }
    if (upper == "SDSTAT") {
        Serial.printf("[SDSTAT] backend=%s mounted=%d cardSizeMB=%llu used=%u total=%u\n",
                      gStore.backendName(), (int)gStore.sdMounted(),
                      (unsigned long long)gStore.cardSizeMb(),
                      (unsigned)gStore.usedBytes(), (unsigned)gStore.totalBytes());
        return;
    }
    if (upper.startsWith("GPASSTHRU")) {
        // Pipe raw GPS UART bytes to USB serial for ground-truth diagnosis.
        String args = line.substring(9); args.trim();
        uint32_t secs = (args.length() > 0) ? (uint32_t)args.toInt() : 10;
        if (secs == 0 || secs > 120) secs = 10;
        gGps.passthru(Serial, secs);
        return;
    }
    if (upper == "GREBAUD") {
        Serial.println("[CMD] re-probing GPS bauds...");
        gGps.begin();
        Serial.printf("[GPS] now @ %u baud, lastByteMs=%u\n",
                      (unsigned)gGps.baud(), (unsigned)gGps.lastByteMs());
        return;
    }
    if (upper == "SYNC") {
        // Force an immediate Wi-Fi upload cycle, regardless of the cadence.
        if (!gWifi.enabled()) {
            Serial.println("[SYNC] uploader disabled (set WIFI_SSID + INGEST_URL in secrets.h)");
            return;
        }
        Serial.println("[SYNC] kicking upload task...");
        gWifi.requestNow();
        Serial.println("[SYNC] (running in background; check WIFISTAT)");
        return;
    }
    if (upper == "WIFISTAT") {
        const char* netStr = "none";
        switch (gWifi.activeNet()) {
            case WifiUploader::ActiveNet::Home:   netStr = "home";   break;
            case WifiUploader::ActiveNet::Remote: netStr = "remote"; break;
            default: break;
        }
        Serial.printf("[WIFI] enabled=%d busy=%d net=%s uploaded=%u failed=%u "
                      "lastAttempt=%ums lastSuccess=%ums lastHttp=%d heap_free=%u\n",
                      (int)gWifi.enabled(), (int)gWifi.busy(),
                      netStr,
                      (unsigned)gWifi.uploadedCount(), (unsigned)gWifi.failedCount(),
                      (unsigned)gWifi.lastAttemptMs(), (unsigned)gWifi.lastSuccessMs(),
                      gWifi.lastHttpStatus(), (unsigned)ESP.getFreeHeap());
        return;
    }
    if (upper == "REBOOT") {
        Serial.println("[REBOOT] soft-resetting device — LittleFS data is safe");
        vTaskDelay(pdMS_TO_TICKS(500)); // let serial flush
        ESP.restart();
        return;
    }
    if (upper == "SPCON") {
        gSpectrumMode = true;
        saveSpectrumModeToNvs();
        Serial.println("[SPEC] spectrum collection ENABLED");
        gRadia.setSpectrumMode(true);
        gUi.setSpectrumMode(true);
        return;
    }
    if (upper == "SPOFF") {
        gSpectrumMode = false;
        saveSpectrumModeToNvs();
        Serial.println("[SPEC] spectrum collection DISABLED");
        gRadia.setSpectrumMode(false);
        gUi.setSpectrumMode(false);
        return;
    }
    if (upper == "SPSTAT") {
        Serial.printf("[SPSTAT] enabled=%d\n", (int)gSpectrumMode);
        return;
    }
    if (upper.startsWith("WIPE")) {
        // Require the user to pass the current session count as a guard
        // against accidental invocation. `WIPE 0` will clear an empty fs;
        // `WIPE` (no arg) prints help.
        String args = line.substring(4); args.trim();
        if (args.length() == 0) {
            Serial.println("[WIPE] usage: WIPE <expected-count>  (run LS first)");
            return;
        }
        int expected = args.toInt();
        int have = gStore.sessionCount();
        if (expected != have) {
            Serial.printf("[WIPE-ABORT] expected=%d have=%d (run LS, retry with matching count)\n",
                          expected, have);
            return;
        }
        uint32_t removed = gStore.wipeAll();
        Serial.printf("[WIPE-DONE] removed=%u\n", (unsigned)removed);
        return;
    }

    if (c == 's') {
        // Optional arg: duration in seconds (default 15)
        uint32_t ms = 15000;
        String sargs = (line.length() > 2) ? line.substring(2) : String();
        sargs.trim();
        if (sargs.length() > 0) {
            int secs = sargs.toInt();
            if (secs > 0 && secs < 600) ms = (uint32_t)secs * 1000;
        }
        Serial.printf("[CMD] starting manual scan (%ums)\n", (unsigned)ms);
        gRadia.startManualScan(ms);
        gManualScanArmedSerial = true;
        return;
    }
    if (c == 'l') {
        const auto& rs = gRadia.getScanResults();
        Serial.printf("[CMD] %u scan results:\n", (unsigned)rs.size());
        for (size_t i = 0; i < rs.size(); ++i) {
            Serial.printf("  [%u] %s type=%u rssi=%d likely=%d name='%s'\n",
                (unsigned)i, rs[i].address.c_str(), (unsigned)rs[i].addrType,
                rs[i].rssi, rs[i].likelyMatch ? 1 : 0, rs[i].name.c_str());
        }
        return;
    }
    if (c == 'c') {
        // c <idx>                    -> connect to scan result at index
        // c <addr> <type>            -> connect to raw address with type
        // c <addr>                   -> connect to raw address, type=1 (random)
        String args = (line.length() > 2) ? line.substring(2) : String();
        args.trim();
        // Address contains colons; index does not.
        if (args.indexOf(':') >= 0) {
            int sp = args.indexOf(' ');
            String addr = (sp > 0) ? args.substring(0, sp) : args;
            uint8_t aType = 1;  // default random for raw connects (RadiaCode 110)
            if (sp > 0) aType = (uint8_t) args.substring(sp + 1).toInt();
            addr.toLowerCase();
            std::string saddr(addr.c_str());
            Serial.printf("[CMD] connecting to raw %s type=%u\n",
                          saddr.c_str(), (unsigned)aType);
            gRadia.connectTo(saddr, aType);
            return;
        }
        int idx = args.toInt();
        const auto& rs = gRadia.getScanResults();
        if (idx < 0 || idx >= (int)rs.size()) {
            Serial.printf("[CMD] bad idx %d (have %u)\n", idx, (unsigned)rs.size());
            return;
        }
        const auto& r = rs[idx];
        Serial.printf("[CMD] connecting to [%d] %s type=%u name='%s'\n",
                      idx, r.address.c_str(), (unsigned)r.addrType, r.name.c_str());
        gRadia.connectTo(r.address, r.addrType);
        return;
    }
    if (c == 'x') {
        Serial.println("[CMD] cancel scan / disconnect");
        gRadia.cancelManualScan();
        return;
    }
    if (c == 'f') {
        Serial.println("[CMD] forget peer + rescan");
        gRadia.disconnectAndForget();
        gRadia.requestScan();
        return;
    }
    if (c == 'D') {
        Serial.println("[CMD] disconnect (keep pin)");
        gRadia.disconnectKeepPin();
        return;
    }
    if (c == 't') {
        // t <pattern>     -> auto-grab any connectable peer with name matching
        // t                -> clear grab pattern
        String pat = (line.length() > 2) ? line.substring(2) : String();
        pat.trim();
        gRadia.setNameGrabPattern(std::string(pat.c_str()));
        if (pat.length()) Serial.printf("[CMD] auto-grab armed for name~='%s'\n", pat.c_str());
        else              Serial.println("[CMD] auto-grab cleared");
        return;
    }
    if (c == 'g') {
        // GPS quick status. Use 'GPASSTHRU [secs]' for raw NMEA.
        const uint32_t now = millis();
        const uint32_t age = gGps.lastByteMs() ? (now - gGps.lastByteMs()) : 0;
        Serial.printf("[GPS] baud=%u bytes=%u lastChar=%ums ago fix=%d sats=%u hdop=%.2f\n",
                      (unsigned)gGps.baud(), (unsigned)gGps.bytesIn(),
                      (unsigned)age, (int)gGps.hasFix(),
                      (unsigned)gGps.satellites(), gGps.hdop());
        Serial.printf("[GPS] checksum pass=%u fail=%u sentencesWithFix=%u\n",
                      (unsigned)gGps.passedChecksum(), (unsigned)gGps.failedChecksum(),
                      (unsigned)gGps.sentencesWithFix());
        if (gGps.hasFix()) {
            Serial.printf("[GPS] lat=%.7f lng=%.7f alt=%.1fm spd=%.1fkph\n",
                          gGps.latitude(), gGps.longitude(),
                          gGps.altitudeMeters(), gGps.speedKph());
        } else if (gGps.bytesIn() == 0) {
            Serial.println("[GPS] *** NO BYTES from module. Check VGNSS rail (GPIO3) "
                           "and RX/TX wiring. Try GREBAUD or GPASSTHRU 5.");
        } else if (gGps.passedChecksum() == 0 && gGps.bytesIn() > 100) {
            Serial.println("[GPS] *** bytes arriving but no valid NMEA. Wrong baud? Try GREBAUD.");
        } else {
            Serial.println("[GPS] *** valid NMEA flowing but no fix yet. Move outdoors with clear sky view, "
                           "cold-start may need 30-90 seconds.");
        }
        return;
    }
    Serial.printf("[CMD] unknown '%s' (use ?)\n", line.c_str());
}

static void pollSerialCommands() {
    while (Serial.available() > 0) {
        const int ci = Serial.read();
        if (ci < 0) break;
        const char ch = (char)ci;
        if (ch == '\r') continue;
        if (ch == '\n') {
            String line = gCmdBuf;
            gCmdBuf = "";
            line.trim();
            handleSerialCommand(line);
        } else {
            gCmdBuf += ch;
            if (gCmdBuf.length() > 64) gCmdBuf = "";  // overflow guard
        }
    }
}

void loop() {
    esp_task_wdt_reset();
    pollSerialCommands();
    gGps.update();

    // GPS baud-rate self-healing: the begin() baud probe can false-positive at
    // 9600 baud when the GPS module is actually sending at 115200 (UART baud
    // aliasing makes some 115200 frames look like valid start bits at 9600).
    // If bytes are flowing but TinyGPS++ has never decoded a valid sentence
    // after 15 seconds, re-run the probe — the GPS is definitely active by
    // then and the 115200 probe will win immediately.
    {
        static uint32_t gpsBaudHealAt = 0;
        if (gGps.passedChecksum() > 0) {
            gpsBaudHealAt = 0;  // NMEA flowing correctly; cancel watchdog
        } else if (gGps.bytesIn() > 2000) {
            if (gpsBaudHealAt == 0) gpsBaudHealAt = millis() + 15000;
            else if ((int32_t)(millis() - gpsBaudHealAt) >= 0) {
                gpsBaudHealAt = millis() + 30000;  // retry again in 30 s
                Serial.printf("[GPS] baud-heal: %u bytes, 0 valid NMEA; was %u baud, re-probing\n",
                              (unsigned)gGps.bytesIn(), (unsigned)gGps.baud());
                gGps.begin();
            }
        }
    }
    gRadia.loop();
    gWifi.tick();

    // v0.7.0: GPS fix transition tracking. When the device walks under a
    // bridge / into a building / loses sky view, we don't want the viewer
    // to draw a straight line from the last good fix to wherever we
    // re-emerge. Emit an explicit GPS_LOST event row at the moment of
    // transition (and GPS_REGAINED when the fix comes back) so the viewer
    // can break its polyline cleanly at the gap.
    //
    // Both events are gated on (a) we have a usable UTC timestamp and
    // (b) recording is already in progress today (no point creating an
    // orphan day file just to record "we never had a fix today").
    {
        static bool prevHasGps = false;
        static bool gpsInitDone = false;
        const bool curHasGps = gGps.hasFix();
        if (!gpsInitDone) {
            // First observation post-boot. Normally silent (no prior state to
            // transition from). Exception: if the previous boot ended in a
            // crash (PANIC / INT_WDT / TASK_WDT / BROWNOUT), emit GPS_LOST +
            // GPS_REGAINED so the viewer breaks the polyline at the crash
            // boundary rather than drawing a straight line across it.
            if (event_log::wasLastResetCrash() && curHasGps) {
                const uint64_t ts = gGps.bestEpochMs();
                constexpr uint64_t MIN_VALID_TS_MS = 1577836800000ULL;
                if (ts >= MIN_VALID_TS_MS && gStore.isRecording()) {
                    String id = gRadia.peerAddress();
                    id.replace(":", "");
                    gStore.appendEvent(ts, "GPS_LOST",     id);
                    gStore.appendEvent(ts, "GPS_REGAINED", id);
                    Serial.printf("[GPS] crash-boot gap: wrote GPS_LOST+GPS_REGAINED at %llu\n",
                                  (unsigned long long)ts);
                }
            }
            prevHasGps = curHasGps;
            gpsInitDone = true;
        } else if (curHasGps != prevHasGps) {
            const uint64_t ts = gGps.bestEpochMs();
            constexpr uint64_t MIN_VALID_TS_MS = 1577836800000ULL;
            if (ts >= MIN_VALID_TS_MS && gStore.isRecording()) {
                String id = gRadia.peerAddress();
                id.replace(":", "");
                gStore.appendEvent(ts,
                                   curHasGps ? "GPS_REGAINED" : "GPS_LOST",
                                   id);
            }
            prevHasGps = curHasGps;
        }
    }

    // Drain pending sample from BLE callback. Filesystem work runs here
    // (Core 1, main loop stack), not on the NimBLE host task.
    PendingSample s;
    bool have = false;
    portENTER_CRITICAL(&gSampleMux);
    if (gPendingSample.valid) {
        s = gPendingSample;
        gPendingSample.valid = false;
        have = true;
    }
    portEXIT_CRITICAL(&gSampleMux);
    if (have) {
        event_log::markPhase("MAIN_APPEND");

        // Try to consume the latest spectrum snapshot from the shared cache
        // (v1.2.2: moved from NimBLE callback on Core 0 to here on Core 1
        // to avoid stack overflow — String building needs >4 KB stack).
        if (gRadia.getSpectrumCache(s.specBuf, cfg::SPECTRUM_MAX_CHANNELS, &s.specCount)) {
            for (uint16_t ch = 0; ch < s.specCount; ch++) {
                if (ch > 0) s.spectrumData += '|';
                s.spectrumData += String(s.specBuf[ch]);
            }
        }

        const size_t appendedBytes = gStore.append(0, s.ts, s.uSv, s.cps,
                      s.hasGps, s.lat, s.lng, s.deviceId,
                      s.speed, s.bearing, s.alt, s.hdop, s.acc,
                      s.spectrumData);
        // Clear spectrumData for next iteration to avoid String lingering
        s.spectrumData = "";
        if (appendedBytes > 0 && gLife.ready()) gLife.onBytesWritten(appendedBytes);

        // Integrate dose: µSv/hr × dt_ms / 3 600 000 = µSv accumulated.
        // Cap dt to 10 s to avoid a phantom spike after a long BLE gap.
        const uint32_t nowDose = millis();
        uint32_t dtMs32 = 0;
        if (s.uSv > 0.0f) {
            if (gLastDoseMs > 0) {
                const float dtMs = (float)(int32_t)(nowDose - gLastDoseMs);
                if (dtMs > 0.0f && dtMs < 10000.0f) {
                    gTripDoseMicroSv += s.uSv * dtMs / 3600000.0f;
                    dtMs32 = (uint32_t)dtMs;
                }
            }
            gLastDoseMs = nowDose;
        }

        // Update lifetime statistics for this sample.
        if (s.hasGps && gLife.ready()) {
            gLife.onGpsFix(s.lat, s.lng,
                           (double)s.alt, s.alt > -9000.f);
            gLife.onSample(s.cps, dtMs32);
        }

        event_log::markPhase("MAIN_LOOP");
    } else {
        // v0.4.7: keep the RTC uptime marker fresh even when there is no
        // GPS fix (samples skipped) so that lastUptimeMs in BOOT records
        // is a useful crash timestamp. Throttle to once per second.
        static uint32_t lastIdleMark = 0;
        uint32_t now = millis();
        if (now - lastIdleMark >= 1000) {
            // Accumulate not-recording time (device on, but no GPS+RC sample).
            if (gLife.ready()) {
                const uint32_t elapsed = (lastIdleMark > 0)
                    ? (uint32_t)(now - lastIdleMark) : 1000u;
                gLife.onIdleTick(elapsed);
            }
            event_log::markPhase("MAIN_IDLE");
            lastIdleMark = now;
        }
    }

    // If a manual scan was kicked off and just completed, hand the results
    // to the UI so the picker is populated.
    static bool manualScanArmed = false;
    if (manualScanArmed && !gRadia.isManualScanActive()) {
        manualScanArmed = false;
        gUi.enterPicker(gRadia.getScanResults());
    }

    // Button events
    switch (gButton.poll()) {
        case Button::SHORT_PRESS:
            gUi.onShortPress();
            break;
        case Button::LONG_PRESS:
            gUi.onLongPress();
            switch (gUi.lastLongAction()) {
                // ACTION_TOGGLE_REC is gone in v0.4.0 (always-on recording).
                // Long-press on STORAGE / GPS is a no-op now.
                case Ui::ACTION_START_PICKER:
                    Serial.println("[UI] starting RadiaCode picker scan");
                    gRadia.startManualScan(15000);
                    gUi.enterPicker({});      // show "Scanning..." placeholder
                    manualScanArmed = true;
                    break;
                case Ui::ACTION_PICK_DEVICE: {
                    String addr = gUi.pickedAddress();
                    uint8_t aType = gUi.pickedAddrType();
                    Serial.printf("[UI] picker chose %s (type=%u)\n", addr.c_str(), (unsigned)aType);
                    manualScanArmed = false;
                    gRadia.connectTo(std::string(addr.c_str()), aType);
                    gUi.exitPicker();
                    break;
                }
                case Ui::ACTION_CANCEL_PICKER:
                    manualScanArmed = false;
                    gRadia.cancelManualScan();
                    gUi.exitPicker();
                    break;
                case Ui::ACTION_RESET_DOSE:
                    resetDose();
                    break;
                case Ui::ACTION_RESET_LIFETIME:
                    gLife.reset();
                    break;
                case Ui::ACTION_FORCE_SYNC:
                    Serial.println("[UI] STORAGE long-press: forcing sync now");
                    gWifi.requestNow();
                    break;
                case Ui::ACTION_TOGGLE_SPECTRUM:
                    gSpectrumMode = !gSpectrumMode;
                    saveSpectrumModeToNvs();
                    gRadia.setSpectrumMode(gSpectrumMode);
                    gUi.setSpectrumMode(gSpectrumMode);
                    Serial.printf("[SPEC] %sed (about screen toggle)\n",
                                  gSpectrumMode ? "enabled" : "disabled");
                    break;
                default: break;
            }
            break;
        default: break;
    }

    // While in picker mode, push the (possibly growing) list to the UI ~2 Hz.
    // The UI itself decides if anything actually changed and only redraws then.
    if (manualScanArmed && gRadia.isManualScanActive()) {
        static uint32_t lastListPush = 0;
        const uint32_t now2 = millis();
        if ((now2 - lastListPush) > 500) {
            lastListPush = now2;
            gUi.enterPicker(gRadia.getScanResults());
        }
    }

    // Battery refresh + heartbeat log
    static uint32_t lastBat = 0;
    static uint32_t lastBeat = 0;
    const uint32_t now = millis();
    if ((now - lastBat) > 5000) {
        lastBat = now;
        const int bpct = readBatteryPercent();
        gUi.setBatteryPercent(bpct);
        if (gLife.ready()) gLife.onBattery(bpct);
    }
    // Persist cumulative dose to NVS only when the value has changed by at
    // least DOSE_NVS_DELTA_USV or the safety ceiling DOSE_NVS_MAX_INTERVAL_MS
    // has elapsed. Reduces flash wear from ~1M writes/year to a few
    // hundred/day in typical use.
    if ((now - gLastDoseSaveMs) >= cfg::DOSE_NVS_SAVE_INTERVAL_MS) {
        if (shouldSaveDose(gTripDoseMicroSv, gLastSavedDoseUSv,
                           now - gLastDoseSaveMs,
                           cfg::DOSE_NVS_DELTA_USV,
                           cfg::DOSE_NVS_MAX_INTERVAL_MS)) {
            saveDoseToNvs();
        }
        gLastDoseSaveMs = now;  // always advance the cadence anchor
    }
    if ((now - lastBeat) > cfg::HEARTBEAT_MS) {
        lastBeat = now;
        Serial.printf("[HB] uptime=%lus fix=%d sats=%u hdop=%.2f acc=%.1fm gpsB=%u gpsAge=%ums baud=%u rcState=%d rec=%d samples=%u life=%u dose=%.4fuSv\n",
                      (unsigned long)(now / 1000),
                      (int)gGps.hasFix(),
                      (unsigned)gGps.satellites(),
                      gGps.hdop(),
                      gGps.accuracyMeters(),
                      (unsigned)gGps.bytesIn(),
                      (unsigned)(gGps.lastByteMs() ? (now - gGps.lastByteMs()) : 0),
                      (unsigned)gGps.baud(),
                      (int)gRadia.state(),
                      (int)gStore.isRecording(),
                      (unsigned)gStore.sampleCount(),
                      (unsigned)gStore.lifetimeSamples(),
                      gTripDoseMicroSv);
    }

    // Periodic lifetime stats NVS flush.
    if (gLife.ready()) gLife.tickSave();

    gUi.setTripDose(gTripDoseMicroSv);
    gUi.tick();
    delay(2);
}
