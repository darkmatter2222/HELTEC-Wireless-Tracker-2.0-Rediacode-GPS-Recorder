// HTIT-Tracker firmware entry point.
// - Heltec WiFi LoRa 32 V3 (ESP32-S3) on the HTIT-Tracker V1.2 or V2 carrier
//   (selected at compile time via TRACKER_HW_V1_2 / TRACKER_HW_V2 build flags).
// - Connects (BLE central) to a RadiaCode dosimeter.
// - Logs CSV samples to LittleFS (V2, internal flash) or SD card (V1.2, HW-125)
//   in the same schema as the Android app:
//     timestampMs,uSvPerHour,cps,latitude,longitude,deviceId

#include <Arduino.h>
#include <esp_system.h>
#include "config.h"
#include "button.h"
#include "event_log.h"
#include "gps_module.h"
#include "radiacode.h"
#include "session_store.h"
#include "ui.h"
#include "wifi_uploader.h"

namespace {
Button        gButton;
GpsModule     gGps;
RadiaCode     gRadia;
SessionStore  gStore;
Ui            gUi;
WifiUploader  gWifi;
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

    gWifi.begin(&gStore);

    gUi.setSources(&gGps, &gStore, &gRadia);
    gUi.setRadiaState(RadiaCode::State::Idle, String());

    gRadia.begin(
        // onReading
        [](const RadiaCode::Reading& r) {
            event_log::markPhase("RC_CB");
            gUi.setReading(r);

            // Build deviceId = address w/o colons (compact)
            String id = gRadia.peerAddress();
            id.replace(":", "");

            // The always-on contract gates live inside SessionStore::append():
            //   - no GPS fix  -> sample dropped
            //   - ts pre-2020 -> sample dropped (no UTC yet)
            //   - day rollover-> previous day's file rotated to pending-upload,
            //                    new <today>.csv opened transparently.
            // bestEpochMs() projects forward via millis() through GPS outages
            // so every accepted sample gets a unique, monotonic timestamp.
            const uint64_t ts = gGps.bestEpochMs();
            event_log::markPhase("RC_APPEND");
            gStore.append(0, ts, r.uSvPerHour, r.cps,
                          gGps.hasFix(), gGps.latitude(), gGps.longitude(),
                          id,
                          cfg::FIELD_SPEED_KPH   ? (float)gGps.speedKph()           : -1.f,
                          cfg::FIELD_BEARING_DEG ? (float)gGps.bearingFromHistory() : -1.f,
                          cfg::FIELD_ALTITUDE_M  ? (float)gGps.altitudeMeters()     : -9999.f,
                          cfg::FIELD_HDOP        ? (float)gGps.hdop()               : -1.f);
            event_log::markPhase("RC_APPEND_OUT");
        },
        // onState
        [](RadiaCode::State s, const String& addr) {
            gUi.setRadiaState(s, addr);
            Serial.printf("[RC] state=%d addr=%s\n", (int)s, addr.c_str());
        }
    );

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
        Serial.printf("[WIFI] enabled=%d busy=%d uploaded=%u failed=%u "
                      "lastAttempt=%ums lastSuccess=%ums lastHttp=%d\n",
                      (int)gWifi.enabled(), (int)gWifi.busy(),
                      (unsigned)gWifi.uploadedCount(), (unsigned)gWifi.failedCount(),
                      (unsigned)gWifi.lastAttemptMs(), (unsigned)gWifi.lastSuccessMs(),
                      gWifi.lastHttpStatus());
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
        gUi.setBatteryPercent(readBatteryPercent());
    }
    if ((now - lastBeat) > cfg::HEARTBEAT_MS) {
        lastBeat = now;
        Serial.printf("[HB] uptime=%lus fix=%d sats=%u hdop=%.2f gpsB=%u gpsAge=%ums baud=%u rcState=%d rec=%d samples=%u\n",
                      (unsigned long)(now / 1000),
                      (int)gGps.hasFix(),
                      (unsigned)gGps.satellites(),
                      gGps.hdop(),
                      (unsigned)gGps.bytesIn(),
                      (unsigned)(gGps.lastByteMs() ? (now - gGps.lastByteMs()) : 0),
                      (unsigned)gGps.baud(),
                      (int)gRadia.state(),
                      (int)gStore.isRecording(),
                      (unsigned)gStore.sampleCount());
    }

    gUi.tick();
    delay(2);
}
