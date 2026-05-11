#pragma once
#include <Arduino.h>

// =============================================================================
// Persistent event log to LittleFS.
//
// Used to diagnose silent reboots in the field (brown-out, panic, watchdog).
// On every boot we append a single CSV line to /system.log capturing:
//
//   epochMs,bootMs,resetReason,vbatMv,lastUptimeMs,wifiInFlight,extraInfo
//
// Where:
//   epochMs        = millis() since boot when the line is written (no UTC
//                    yet at the moment of writing on first boot)
//   bootMs         = millis() at time of write (same field for forensic value)
//   resetReason    = esp_reset_reason() short name (POWERON, BROWNOUT, etc.)
//   vbatMv         = current battery voltage in millivolts (-1 if not sampled)
//   lastUptimeMs   = uptime in ms of the previous boot cycle (from RTC mem)
//   wifiInFlight   = "1" if the previous boot died while connectWifi()/HTTP
//                    was running (set in RTC memory before begin(), cleared
//                    after disconnect)
//   extraInfo      = optional free-form note
//
// The file is bounded (rolled to /system.log.old when it crosses ~12 KB) so
// LittleFS does not fill up over time.
// =============================================================================

namespace event_log {

// Initialize. Reads the RTC slow-mem markers planted by the previous boot,
// appends a boot-record line, and resets the markers for this cycle.
// Safe to call even if LittleFS isn't ready - logs are then suppressed.
void beginBoot();

// Append an arbitrary one-line event. The line is auto-timestamped with
// millis(). The message MUST NOT contain commas or newlines (they will be
// replaced with spaces).
void appendEvent(const char* tag, const char* msg);

// RTC-memory markers. wifi_uploader uses these so a brown-out *during* the
// connect attempt can be attributed correctly on the next boot.
void markWifiInFlight(bool inFlight);

// Stream the entire log file to a Stream (typically Serial). Returns the
// total bytes printed.
size_t dump(Stream& out);

// Erase the log file (and rotated old file). Used by the LOGCLEAR command.
void clear();

// True once beginBoot() has been called.
bool ready();

} // namespace event_log
