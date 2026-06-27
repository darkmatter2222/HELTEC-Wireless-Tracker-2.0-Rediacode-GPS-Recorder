#include "event_log.h"

#include <LittleFS.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <rom/rtc.h>

// External: latest VBAT sample, defined in main.cpp.
extern float trackerLastVbat();

namespace {

constexpr const char* kLogPath    = "/system.log";
constexpr const char* kLogOldPath = "/system.log.old";
constexpr size_t      kLogMaxBytes = 12 * 1024;   // roll over at ~12 KB

bool g_ready = false;
bool g_lastResetWasCrash = false;  // set in beginBoot(); read by main.cpp
bool g_littlefsCorrupted = false;   // Track corrupted LittleFS state — once we detect corruption, never try again

// RTC slow memory survives software resets, brown-outs, and watchdog
// reboots (cleared only by full power-off or deep sleep). We use it to
// pass forensic state forward from one boot to the next.
RTC_NOINIT_ATTR uint32_t g_rtcMagic;     // sentinel "TRKR"
RTC_NOINIT_ATTR uint32_t g_rtcUptimeMs;  // millis() of last successful tick
RTC_NOINIT_ATTR uint32_t g_rtcWifiFlag;  // 1 if Wi-Fi cycle was in-flight
RTC_NOINIT_ATTR char     g_rtcPhase[16]; // last marked phase tag
RTC_NOINIT_ATTR uint32_t g_rtcHeapFree;  // ESP.getFreeHeap() at last tick

constexpr uint32_t kMagic = 0x544B5252;  // 'TKRR'

// Boot loop detection: if we've rebooted >3 times in 60 seconds,
// disable LittleFS writes to break the cycle and log to serial only.
RTC_NOINIT_ATTR uint32_t g_rtcBootCount;
RTC_NOINIT_ATTR uint32_t g_rtcBootTimeMs;
static bool checkBootLoop() {
    uint32_t now = millis();
    g_rtcBootCount++;
    if (g_rtcBootTimeMs == 0 || now < g_rtcBootTimeMs) {
        // Fresh start or millis wrapped
        g_rtcBootTimeMs = now;
        g_rtcBootCount = 1;
    } else if (now - g_rtcBootTimeMs > 60000 && g_rtcBootCount > 3) {
        // Booted >3 times in 60 seconds - boot loop detected
        Serial.println("[LOG] BOOT LOOP DETECTED: disable LittleFS to break cycle");
        g_littlefsCorrupted = true;
        return false;
    }
    return true;
}

const char* resetReasonName(esp_reset_reason_t rr) {
    switch (rr) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Roll /system.log -> /system.log.old when oversize. Keeps storage bounded.
void maybeRoll() {
    if (g_littlefsCorrupted) return;
    if (!LittleFS.exists(kLogPath)) return;
    File f = LittleFS.open(kLogPath, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz < kLogMaxBytes) return;
    if (LittleFS.exists(kLogOldPath)) LittleFS.remove(kLogOldPath);
    LittleFS.rename(kLogPath, kLogOldPath);
}

void appendLineRaw(const String& line) {
    if (!g_ready || g_littlefsCorrupted) return;
    try {
        maybeRoll();
        File f = LittleFS.open(kLogPath, FILE_APPEND);
        if (!f) { g_littlefsCorrupted = true; Serial.println("[LOG] LittleFS open failed, disabling log"); return; }
        f.print(line);
        f.print('\n');
        f.close();
    } catch (...) {
        g_littlefsCorrupted = true;
        Serial.println("[LOG] exception caught, disabling log");
    }
}

void anonymousBeginBoot() {
    // Check for boot loop BEFORE touching any LittleFS code
    if (!checkBootLoop()) return;
    
    if (!g_ready) {
        Serial.println("[LOG] ready");
        g_ready = true;
    }
}

} // namespace


namespace event_log {

bool ready()             { return g_ready; }
bool wasLastResetCrash() { return g_lastResetWasCrash; }

void beginBoot() {
    // Delegate to anonymous namespace which has boot loop protection
    ::anonymousBeginBoot();
}

void markPhase(const char* phase) {
    if (!phase) phase = "";
    // Write directly into RTC slow memory; survives a panic on the next
    // instruction.
    size_t i = 0;
    for (; i < sizeof(g_rtcPhase) - 1 && phase[i]; ++i) {
        g_rtcPhase[i] = phase[i];
    }
    g_rtcPhase[i] = '\0';
    g_rtcHeapFree = ESP.getFreeHeap();
    g_rtcMagic = kMagic;
}

void appendEvent(const char* tag, const char* msg) {
    if (!g_ready) return;
    char clean[120];
    // Sanitize: commas and newlines break the CSV format.
    size_t i = 0;
    if (msg) {
        for (; msg[i] && i < sizeof(clean) - 1; ++i) {
            char c = msg[i];
            if (c == ',' || c == '\n' || c == '\r') c = ' ';
            clean[i] = c;
        }
    }
    clean[i] = '\0';
    char line[180];
    int vbatMv = -1;
    float v = trackerLastVbat();
    if (v > 0.0f) vbatMv = (int)(v * 1000.0f + 0.5f);
    snprintf(line, sizeof(line),
             "%u,%u,%s,vbatMv=%d,%s",
             (unsigned)millis(), (unsigned)millis(),
             tag ? tag : "EVT", vbatMv, clean);
    appendLineRaw(String(line));
}

void markWifiInFlight(bool inFlight) {
    g_rtcWifiFlag = inFlight ? 1u : 0u;
    // Also record the current uptime so the *next* boot can see how far we
    // got before the reset (useful for diagnosing whether the reset happens
    // mid-connect, mid-POST, etc.).
    g_rtcUptimeMs = millis();
    g_rtcMagic    = kMagic;
}

size_t dump(Stream& out) {
    if (!g_ready) {
        out.println("[LOG] event log not ready");
        return 0;
    }
    size_t total = 0;
    if (LittleFS.exists(kLogOldPath)) {
        File f = LittleFS.open(kLogOldPath, FILE_READ);
        if (f) {
            out.println("--- /system.log.old ---");
            while (f.available()) {
                char c = f.read();
                out.write(c);
                ++total;
            }
            f.close();
        }
    }
    if (LittleFS.exists(kLogPath)) {
        File f = LittleFS.open(kLogPath, FILE_READ);
        if (f) {
            out.println("--- /system.log ---");
            while (f.available()) {
                char c = f.read();
                out.write(c);
                ++total;
            }
            f.close();
        }
    } else {
        out.println("[LOG] empty");
    }
    out.printf("\n--- (%u bytes total) ---\n", (unsigned)total);
    return total;
}

void clear() {
    if (LittleFS.exists(kLogPath))    LittleFS.remove(kLogPath);
    if (LittleFS.exists(kLogOldPath)) LittleFS.remove(kLogOldPath);
    Serial.println("[LOG] cleared");
}

} // namespace event_log
