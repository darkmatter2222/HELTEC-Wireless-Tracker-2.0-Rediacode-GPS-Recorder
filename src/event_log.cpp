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

// RTC slow memory survives software resets, brown-outs, and watchdog
// reboots (cleared only by full power-off or deep sleep). We use it to
// pass forensic state forward from one boot to the next.
RTC_NOINIT_ATTR uint32_t g_rtcMagic;     // sentinel "TRKR"
RTC_NOINIT_ATTR uint32_t g_rtcUptimeMs;  // millis() of last successful tick
RTC_NOINIT_ATTR uint32_t g_rtcWifiFlag;  // 1 if Wi-Fi cycle was in-flight
RTC_NOINIT_ATTR char     g_rtcPhase[16]; // last marked phase tag

constexpr uint32_t kMagic = 0x544B5252;  // 'TKRR'

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
    if (!g_ready) return;
    maybeRoll();
    File f = LittleFS.open(kLogPath, FILE_APPEND);
    if (!f) return;
    f.print(line);
    f.print('\n');
    f.close();
}

} // namespace


namespace event_log {

bool ready() { return g_ready; }

void beginBoot() {
    // CRITICAL: do NOT call LittleFS.begin() here. SessionStore already
    // mounted LittleFS with a custom partition label ("littlefs"). The
    // Arduino LittleFS singleton can only mount one partition at a time,
    // and calling begin() again with the default "spiffs" label silently
    // UNMOUNTS the previously-mounted partition before failing to find
    // "spiffs" -- this is what caused the v0.4.2 boot loop (sessions/*.csv
    // suddenly returned "no permits for creation" and the nimble_host task
    // stack-canary'd while error-handling the SessionStore append failure).
    // We trust the caller to have invoked gStore.begin() first.
    g_ready = true;

    const esp_reset_reason_t rr = esp_reset_reason();
    const char* rname = resetReasonName(rr);
    // v0.4.7: also capture the raw hardware reset reason for each CPU.
    // esp_reset_reason() returns ESP_RST_UNKNOWN when the IDF couldn't
    // classify the reset; the raw register usually still gives us a clue.
    const int rawRr0 = (int)rtc_get_reset_reason(0);
    const int rawRr1 = (int)rtc_get_reset_reason(1);

    // Capture forensic state from RTC memory.
    bool magicValid = (g_rtcMagic == kMagic);
    uint32_t lastUptime = magicValid ? g_rtcUptimeMs : 0u;
    uint32_t wifiInFlight = magicValid ? g_rtcWifiFlag : 0u;
    char lastPhase[16] = {0};
    if (magicValid) {
        memcpy(lastPhase, g_rtcPhase, sizeof(lastPhase));
        lastPhase[15] = '\0';
        // Sanitize so we never crash on garbage RTC contents.
        for (int i = 0; i < 15 && lastPhase[i]; ++i) {
            char c = lastPhase[i];
            if (c < 32 || c > 126 || c == ',' || c == '\n') { lastPhase[i] = '?'; }
        }
    }

    // Reset RTC state for the new boot cycle.
    g_rtcMagic     = kMagic;
    g_rtcUptimeMs  = 0;
    g_rtcWifiFlag  = 0;
    memset((void*)g_rtcPhase, 0, sizeof(g_rtcPhase));
    strncpy((char*)g_rtcPhase, "BOOTED", sizeof(g_rtcPhase) - 1);

    // Battery voltage at boot (mV). If the ADC hasn't been sampled yet
    // (likely on the first call), -1 is fine; main loop will sample within
    // a few seconds and subsequent events will have it.
    const float vbat = trackerLastVbat();
    int vbatMv = (vbat > 0.0f) ? (int)(vbat * 1000.0f + 0.5f) : -1;

    char buf[240];
    snprintf(buf, sizeof(buf),
             "%u,%u,BOOT,%s,raw0=%d,raw1=%d,vbatMv=%d,lastUptimeMs=%u,wifiInFlight=%u,lastPhase=%s",
             (unsigned)millis(), (unsigned)millis(), rname,
             rawRr0, rawRr1,
             vbatMv, (unsigned)lastUptime, (unsigned)wifiInFlight,
             lastPhase[0] ? lastPhase : "NONE");
    appendLineRaw(String(buf));

    // Also print to serial so anyone tailing sees it immediately.
    Serial.printf("[LOG] %s\n", buf);
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
