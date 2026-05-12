// Heltec HTIT-Tracker board config (supports V1.2 and V2 via build flags).
// Pin assignments mirror darkmatter2222/External-GPS-Receiver-Heltec-HTIT-Tracker-V1.2
// for the V1.2 layout (known-working reference). V2-specific overrides are
// selected by TRACKER_HW_V2 in platformio.ini.

#pragma once
#include <Arduino.h>

namespace cfg {

// ---------------- Hardware revision -------------------------------------------
// Selected by build_flags in platformio.ini.  Default to V1.2 if neither flag
// is supplied so legacy build commands keep working.
#if !defined(TRACKER_HW_V2) && !defined(TRACKER_HW_V1_2)
#define TRACKER_HW_V1_2 1
#endif

// ---------------- TFT (ST7735, 0.96" 160x80) ---------------------------------
// Same physical pins on both V1.2 and V2 -- only the panel init differs.
constexpr uint8_t  TFT_CS    = 38;
constexpr uint8_t  TFT_RST   = 39;
constexpr uint8_t  TFT_DC    = 40;
constexpr uint8_t  TFT_SCLK  = 41;
constexpr uint8_t  TFT_MOSI  = 42;
constexpr uint8_t  TFT_ROTATION = 1;       // landscape, ribbon at right
constexpr uint16_t TFT_W = 160;
constexpr uint16_t TFT_H = 80;
#if defined(TRACKER_HW_V2)
// HTIT-Tracker V2: Heltec's HT_st7735.h sets XSTART=0/YSTART=24 and calls
// st7735_invert_colors(false) when WIRELESS_TRACKER_V2 is defined.
constexpr uint8_t  TFT_X_OFFSET = 0;
constexpr uint8_t  TFT_Y_OFFSET = 24;
constexpr bool     TFT_INVERT   = false;
#else
// HTIT-Tracker V1.2: 160x80 mini panel offsets, color inversion enabled.
constexpr uint8_t  TFT_X_OFFSET = 1;
constexpr uint8_t  TFT_Y_OFFSET = 26;
constexpr bool     TFT_INVERT   = true;
#endif

// ---------------- GPS (UC6580 over UART2) -------------------------------------
constexpr int      GPS_UART_NUM = 1;       // HardwareSerial(1)
constexpr uint8_t  GPS_RX_PIN = 33;        // ESP RX  <-- GPS TX
constexpr uint8_t  GPS_TX_PIN = 34;        // ESP TX  --> GPS RX
constexpr uint32_t GPS_BAUD   = 115200;    // UC6580 default after auto-detect
constexpr uint32_t GPS_FALLBACK_BAUDS[] = {115200, 9600, 38400, 57600};

// ---------------- Power / peripheral rails (HTIT-Tracker V1.2) -----------------
// On the HTIT-Tracker V1.2 carrier the GNSS module and TFT share a 3.3V rail
// gated by VTFT_CTRL on GPIO 3. The Heltec HT_st7735 library drives this pin
// HIGH inside st7735_init() to enable the rail, and the working reference
// firmware (darkmatter2222) relies on that behaviour. So: HIGH = powered.
// The battery divider is on GPIO 2 (HIGH = enable). Backlight on GPIO 21.
constexpr uint8_t  VGNSS_CTRL_PIN = 3;     // HIGH = GPS+TFT powered
constexpr uint8_t  BL_CTRL_PIN    = 21;    // HIGH = backlight on
constexpr uint8_t  VBAT_EN_PIN    = 2;     // HIGH during ADC read
constexpr uint8_t  VBAT_ADC_PIN   = 1;     // ADC1_CH0
// Resistor divider ratio: VBAT → (~390 kΩ top + 100 kΩ bottom) → GND.
// V_adc = V_bat * 100/(100+390) ≈ V_bat/4.9; multiplier is the inverse.
// 5.05 is the empirically measured value for Heltec Wireless Tracker V2
// (same divider network as WiFi LoRa 32 V3).  Adjust if your multimeter
// disagrees: multiply = V_bat_measured / V_adc_measured.
constexpr float    VBAT_DIV_MULT  = 5.05f;

// ---------------- Button (PRG) ------------------------------------------------
constexpr uint8_t  BUTTON_PIN = 0;         // active LOW
constexpr uint16_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint16_t BUTTON_LONG_PRESS_MS = 800;

// ---------------- RadiaCode polling -------------------------------------------
// Poll interval. Matches Android (~1 Hz). Going slower (3s) was observed to
// make the RadiaCode-110 drop the link almost immediately after Ready --
// the peer apparently expects continuous client activity to keep the
// connection alive.
constexpr uint32_t RADIACODE_POLL_MS = 1000;
constexpr uint32_t RADIACODE_SCAN_MS = 8000;
constexpr uint32_t RADIACODE_RECONNECT_MS = 5000;
// Link-health watchdog. If state == Ready but no notification has arrived for
// this long, force a disconnect so the auto-scan loop can recover. Catches
// half-closed channels where NimBLE's supervision timeout silently extends.
// 15 s is ~15x the 1 Hz poll interval; chosen to allow normal jitter and the
// 5-9 s DATA_BUF cold-start delay seen on RadiaCode-110 without false trips.
constexpr uint32_t RADIACODE_LINK_STALL_MS = 15000;

// ---------------- Storage -----------------------------------------------------
constexpr const char* SESSIONS_DIR    = "/sessions";
constexpr const char* ACTIVE_FILE     = "/active.txt";   // current session id
constexpr size_t      MAX_LINE_BYTES  = 160;

// ---------------- SD card (HW-125 micro-SD breakout, SPI mode) ----------------
// Wiring (see heltec_tracker/AGENTS.md for the full table):
//   HW-125 GND  -> Heltec GND
//   HW-125 VCC  -> Heltec 5V   (NOT 3V3 -- AMS1117 LDO needs 5V input;
//                                card gets ~2V at 3V3 and will not respond)
//   HW-125 MISO -> GPIO 4
//   HW-125 MOSI -> GPIO 6
//   HW-125 SCK  -> GPIO 5
//   HW-125 CS   -> GPIO 7
// Dedicated SPI bus (HSPI), independent from the TFT bus on GPIO 38-42.
//
// V1.2 boards in the field have an HW-125 micro-SD breakout wired to
// GPIO 4/5/6/7. V2 boards in this project ship without an SD breakout, so
// we skip the SD probe entirely on V2 builds and go straight to the
// on-chip LittleFS partition. Otherwise the boot stalls for ~60 s in the
// SdFat retry loop before the UI ever paints.
#if defined(TRACKER_HW_V2)
constexpr bool     SD_ENABLED   = false;
constexpr bool     SD_REQUIRED  = false;
#else
constexpr bool     SD_ENABLED   = true;
// If true (default), the firmware REFUSES to fall back to on-chip LittleFS
// when the SD card can't be mounted. Recording stays disabled and the UI
// shows "STORAGE INIT FAILED -- REBOOT" until the user power-cycles. This
// is the safe choice for field collection: a silent fallback to the 1.5MB
// internal partition has historically caused users to lose hours of data
// thinking the SD card was being written. Set to false only if you want
// internal-only operation as a deliberate fallback.
constexpr bool     SD_REQUIRED  = true;
#endif
// On cold-boot from battery the HW-125's onboard LDO sometimes needs a few
// hundred ms longer than on USB power before the card responds. Retry the
// SdFat mount up to this many times with a short gap between attempts.
constexpr uint8_t  SD_INIT_RETRIES = 6;
constexpr uint16_t SD_INIT_RETRY_GAP_MS = 250;
// Wiring: HW-125 MISO -> GPIO 4, MOSI -> GPIO 6 (see heltec_tracker/AGENTS.md).
constexpr uint8_t  SD_MISO_PIN  = 4;
constexpr uint8_t  SD_MOSI_PIN  = 6;
constexpr uint8_t  SD_SCK_PIN   = 5;
constexpr uint8_t  SD_CS_PIN    = 7;
constexpr uint32_t SD_SPI_HZ    = 20000000;     // 20 MHz; back off to 4 MHz on poor cards

// ---------------- App ---------------------------------------------------------
constexpr uint32_t UI_TICK_MS = 100;
constexpr uint32_t HEARTBEAT_MS = 3000;
constexpr const char* FW_VERSION = "0.6.0";

// ---------------- Battery / Wi-Fi safety gate (v0.4.2) -----------------------
// Skip the Wi-Fi upload cycle entirely if VBAT is below this threshold (V).
// The Wi-Fi PA pulls 200-400 mA spikes during scan/associate; on a partially
// discharged battery those spikes collapse the rail and trigger a brown-out
// reset. 3.60 V is well above the BMS cutoff but below "fresh off charger",
// so day-to-day operation is unaffected and only critically low cells skip
// uploads. Recording (BLE + GPS + storage, ~80 mA) is always allowed.
constexpr float    VBAT_MIN_FOR_WIFI = 3.60f;

// ----------- Always-on autonomous recording (v0.4.0) ------------------------
// Recording is no longer user-toggled. As long as the RadiaCode is connected
// AND a valid GPS UTC timestamp is available AND the GPS has a fix, samples
// are written to a per-day CSV file. There is no Start/Stop UI; the STORAGE
// screen is purely informational. Files are named /sessions/<YYYY-MM-DD>.csv
// where the date is local Eastern time (the user's home timezone) so a single
// evening trip does not get split across two UTC days.
//
// POSIX TZ rule for America/New_York (EST/EDT, US DST rules since 2007).
// Format: stdoffset[dst[offset][,start[/time],end[/time]]]
//   EST5      = baseline UTC-5 (Eastern Standard Time)
//   EDT       = daylight-saving abbreviation
//   ,M3.2.0   = DST starts second Sunday of March at 02:00 local
//   ,M11.1.0  = DST ends first Sunday of November at 02:00 local
constexpr const char* LOCAL_TZ = "EST5EDT,M3.2.0,M11.1.0";

// ----------- Extended per-record telemetry fields ---------------------------
// Each flag controls whether that GPS field is sampled and written to the CSV.
// Disabled fields are still emitted as empty CSV columns so the 10-column
// schema is consistent across firmware builds; old 6-column uploads from
// earlier firmware are handled gracefully by the ingest API.
constexpr bool FIELD_SPEED_KPH   = true;  // GPS speed over ground, km/h
constexpr bool FIELD_BEARING_DEG = true;  // smoothed bearing, degrees [0, 360)
constexpr bool FIELD_ALTITUDE_M  = true;  // GPS altitude above MSL, metres
constexpr bool FIELD_HDOP        = true;  // Horizontal Dilution of Precision

// Number of GPS history positions used to compute the smoothed bearing.
// Must be between 2 and 8. At 1 Hz GPS rate, 4 = ~4-second smoothing lag,
// which reduces jitter without lagging noticeably through normal turns.
constexpr uint8_t BEARING_HISTORY_POINTS = 4;

// ---------------- Cumulative trip dose NVS persistence ----------------------
// The running total µSv is saved to NVS (Preferences namespace "dose", key
// "usv") every DOSE_NVS_SAVE_INTERVAL_MS so most accumulated dose survives
// an unexpected crash or reboot.  On a user-initiated reset (hold on DOSE
// screen) the NVS value is zeroed immediately.
constexpr uint32_t DOSE_NVS_SAVE_INTERVAL_MS = 30000;  // 30 s
// To reduce flash wear from a 30s-cadence NVS write (~1M writes/year), we
// only actually save when the dose has changed by at least this many µSv
// OR a longer "safety" interval has elapsed. Combined this drops typical
// idle writes from ~120/hour to single-digits/hour.
constexpr float    DOSE_NVS_DELTA_USV = 0.5f;        // µSv change threshold
constexpr uint32_t DOSE_NVS_MAX_INTERVAL_MS = 300000; // 5 min hard ceiling

// ---------------- Task watchdog (v0.6.0) ------------------------------------
// Wraps the Arduino loop task and the Wi-Fi uploader task in esp_task_wdt.
// A wedged subsystem (deadlocked semaphore, infinite BLE callback, etc.)
// will trigger a clean panic + reboot rather than hanging silently.
constexpr uint32_t TASK_WDT_TIMEOUT_S = 30;

// ---------------- GPS reliability (v0.6.0) ----------------------------------
// Default ESP32 HardwareSerial RX FIFO is 256 bytes -- at 115200 baud that
// only buffers ~22 ms of NMEA. A blocking BLE op or long flash flush can
// easily exceed that and corrupt sentences. Bump to 2 KB (~180 ms).
constexpr size_t   GPS_RX_BUFFER_BYTES = 2048;
// If the GPS has reported bytes but no NEW fix update for this long, run a
// baud re-probe to recover from a wedged UC6580 (rare but observed in field).
constexpr uint32_t GPS_FROZEN_FIX_RECOVERY_MS = 120000;  // 2 min

} // namespace cfg
