#include "ui.h"
#include "config.h"
#include "gps_module.h"
#include "lifetime_stats.h"
#include "session_store.h"
#include "wifi_uploader.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <algorithm>

namespace {
// Subclass exposes the protected setColRowStart() helper so we can apply the
// HTIT-Tracker mini panel's RAM offsets and kill the rainbow edges.
// V1.2: setColRowStart(26, 1).  V2: setColRowStart(24, 0).  These are the
// post-rotation memory addresses Heltec uses in their factory firmware.
class Tracker_ST7735 : public Adafruit_ST7735 {
public:
    using Adafruit_ST7735::Adafruit_ST7735;
    void applyMiniOffsets() {
#if defined(TRACKER_HW_V2)
        setColRowStart(24, 0);
#else
        setColRowStart(26, 1);
#endif
    }
};

// HTIT-Tracker V1.2 SPI pins (custom HSPI, not default).
// Software-SPI constructor avoids the lib re-binding the HW SPI peripheral.
Tracker_ST7735 tft(cfg::TFT_CS, cfg::TFT_DC,
                   cfg::TFT_MOSI, cfg::TFT_SCLK,
                   cfg::TFT_RST);

constexpr uint16_t COL_BG       = ST77XX_BLACK;
constexpr uint16_t COL_FG       = 0xFFFF;
constexpr uint16_t COL_DIM      = 0x8C71;     // light gray, readable on black
constexpr uint16_t COL_GREEN    = 0x07E0;
// This panel's MADCTL is BGR, not RGB, so R and B bit fields are swapped
// relative to the Adafruit defaults.  Green (center 6 bits) is unaffected.
// 0xF800 renders blue; 0x001F renders red.  0xFD20 renders cyan; 0x053F renders amber.
constexpr uint16_t COL_RED      = 0x001F;
constexpr uint16_t COL_AMBER    = 0x053F;
constexpr uint16_t COL_CYAN     = 0xFFE0;     // BGR-corrected cyan
constexpr uint16_t COL_HEADER   = 0x10A2;     // dark blue band
constexpr uint16_t COL_PICK     = 0x041F;     // selected row highlight
constexpr uint16_t COL_LIGHT_BLUE = 0x1C9F;   // baby blue for spectrum indicator

constexpr int HEADER_H = 12;

const char* stateName(RadiaCode::State s) {
    switch (s) {
        case RadiaCode::State::Idle:         return "IDLE";
        case RadiaCode::State::Scanning:     return "SCAN";
        case RadiaCode::State::Connecting:   return "CONN";
        case RadiaCode::State::Initializing: return "INIT";
        case RadiaCode::State::Ready:        return "OK  ";
        case RadiaCode::State::Disconnected: return "DISC";
    }
    return "?";
}
uint16_t stateColor(RadiaCode::State s) {
    switch (s) {
        case RadiaCode::State::Ready:        return COL_GREEN;
        case RadiaCode::State::Connecting:
        case RadiaCode::State::Initializing:
        case RadiaCode::State::Scanning:     return COL_AMBER;
        default:                             return COL_RED;
    }
}
} // namespace

// ---------------------------------------------------------------------------
void Ui::begin() {
    tft.initR(INITR_MINI160x80);
    tft.setRotation(cfg::TFT_ROTATION);
    tft.applyMiniOffsets();               // kills rainbow edge pixels
    // V1.2 panels are inverted (so invertDisplay(true) gives black BG); V2
    // panels are not. Mismatch == fully white screen, the symptom users see
    // when V1.2 firmware boots on a V2 board.
    tft.invertDisplay(cfg::TFT_INVERT);
    tft.fillScreen(COL_BG);
    tft.setTextWrap(false);
    tft.setTextColor(COL_FG, COL_BG);

    // Splash for ~600 ms so the user knows the panel is alive.
    tft.setTextSize(2);
    tft.setCursor(20, 22);
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.print("HTIT-RC");
    tft.setTextSize(1);
    tft.setCursor(40, 50);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.print("booting...");
    delay(600);
    tft.fillScreen(COL_BG);
    forceFullRedraw_ = true;
}

void Ui::setSources(GpsModule* gps, SessionStore* store, RadiaCode* rc) {
    gps_ = gps; store_ = store; rc_ = rc;
}

// ---------------------------------------------------------------------------
void Ui::onShortPress() {
    if (screen_ == SCREEN_PICKER) {
        if (pickList_.empty()) return;
        // cursor 0..N-1 = device index, N = "Cancel"
        pickerCursor_ = (pickerCursor_ + 1) % ((int)pickList_.size() + 1);
        forceFullRedraw_ = true;     // redraw rows so cursor is visible
        return;
    }
    if (screen_ == SCREEN_LIFETIME_CONFIRM) {
        // Short-press on confirm screen = cancel, return to originating screen.
        screen_          = confirmFromScreen_;
        forceFullRedraw_ = true;
        return;
    }
    // Cycle STATS -> GPS -> STORAGE -> STATS
    screen_ = (Screen)((screen_ + 1) % SCREEN_NORMAL_COUNT);
    forceFullRedraw_ = true;
}

void Ui::onLongPress() {
    switch (screen_) {
        case SCREEN_STATS:
            pendingAction_ = ACTION_START_PICKER;
            break;
        case SCREEN_GPS:
            // Long-press on GPS advances screen (same as short-press).
            screen_ = (Screen)((screen_ + 1) % SCREEN_NORMAL_COUNT);
            forceFullRedraw_ = true;
            break;
        case SCREEN_STORAGE:
            // Long-press on STORAGE triggers an immediate Wi-Fi sync,
            // bypassing any exponential backoff countdown.
            pendingAction_ = ACTION_FORCE_SYNC;
            break;
        case SCREEN_DOSE:
            // Long-press on DOSE screen signals main.cpp to zero the accumulator.
            pendingAction_ = ACTION_RESET_DOSE;
            break;
        case SCREEN_LIFETIME:
        case SCREEN_LIFETIME2:
            // Long-press on either LIFETIME screen enters the reset confirmation screen.
            // Actual reset only happens after the user confirms on SCREEN_LIFETIME_CONFIRM.
            confirmFromScreen_ = screen_;
            screen_            = SCREEN_LIFETIME_CONFIRM;
            forceFullRedraw_   = true;
            break;
        case SCREEN_ABOUT:
            // Long-press on ABOUT toggles spectrum collection mode.
            pendingAction_ = ACTION_TOGGLE_SPECTRUM;
            break;
        case SCREEN_PICKER:
            if (pickerCursor_ >= (int)pickList_.size()) {
                pendingAction_ = ACTION_CANCEL_PICKER;
            } else {
                // pickerCursor_ indexes the displayed (sorted) order, so map
                // through pickerOrder_ to get the real pickList_ entry.
                int realIdx = pickerCursor_;
                if (pickerCursor_ < (int)pickerOrder_.size()) {
                    realIdx = pickerOrder_[pickerCursor_];
                }
                if (realIdx >= 0 && realIdx < (int)pickList_.size()) {
                    pickedAddr_ = String(pickList_[realIdx].address.c_str());
                    pickedAddrType_ = pickList_[realIdx].addrType;
                    pendingAction_ = ACTION_PICK_DEVICE;
                }
            }
            break;
        default: break;
    }
}

void Ui::setReading(const RadiaCode::Reading& r) {
    lastReading_ = r;
    if (r.battery > 0 && r.battery <= 100) {
        // RadiaCode reports its own battery; we treat it as the displayed value.
        // (USB-C powered ESP has its own divider but is less interesting here.)
    }
}
void Ui::setRadiaState(RadiaCode::State s, const String& addr) {
    rcState_ = s;
    rcAddr_  = addr;
}

void Ui::enterPicker(const std::vector<RadiaCode::ScanResult>& results) {
    // Decide if anything visible actually changed -- only redraw on real change
    // to avoid flicker. Address membership change OR significant RSSI delta.
    bool changed = (results.size() != pickList_.size());
    if (!changed) {
        for (size_t i = 0; i < results.size(); ++i) {
            // Match by address (order may differ between snapshots).
            const auto& a = results[i];
            bool found = false;
            for (const auto& b : pickList_) {
                if (a.address == b.address) {
                    if (a.name != b.name ||
                        a.likelyMatch != b.likelyMatch ||
                        std::abs(a.rssi - b.rssi) > 8) {
                        changed = true;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) { changed = true; break; }
        }
    }
    pickList_ = results;
    if (screen_ != SCREEN_PICKER) {
        pickerCursor_ = 0;
        screen_ = SCREEN_PICKER;
        forceFullRedraw_ = true;
    } else if (changed) {
        forceFullRedraw_ = true;
    }
    // pickList_.size() entries + 1 Cancel row, so max valid cursor = size().
    if (pickerCursor_ > (int)pickList_.size()) pickerCursor_ = (int)pickList_.size();
}

// ---------------------------------------------------------------------------
void Ui::field(int idx, int x, int y, int w, int h,
               const char* str, uint16_t fg, uint16_t bg, uint8_t size) {
    if (idx < 0 || idx >= MAX_FIELDS) return;
    String s(str);
    if (!forceFullRedraw_ &&
        prevText_[idx] == s && prevFg_[idx] == fg && prevSize_[idx] == size) {
        return; // unchanged: skip entirely (no flicker)
    }
    tft.fillRect(x, y, w, h, bg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(size);
    tft.setCursor(x, y);
    tft.print(str);
    prevText_[idx] = s;
    prevFg_[idx]   = fg;
    prevSize_[idx] = size;
}

// ---------------------------------------------------------------------------
void Ui::tick() {
    if (screen_ != lastDrawnScreen_) {
        tft.fillScreen(COL_BG);
        for (int i = 0; i < MAX_FIELDS; ++i) prevText_[i] = "";
        forceFullRedraw_ = true;
        lastDrawnScreen_ = screen_;
    }

    renderHeader();
    switch (screen_) {
        case SCREEN_STATS:    renderStats();    break;
        case SCREEN_GPS:      renderGps();      break;
        case SCREEN_STORAGE:  renderStorage();  break;
        case SCREEN_DOSE:     renderDose();     break;
        case SCREEN_LIFETIME:  renderLifetime();        break;
        case SCREEN_LIFETIME2: renderLifetime2();       break;
        case SCREEN_ABOUT:     renderAbout();           break;
        case SCREEN_LIFETIME_CONFIRM: renderLifetimeConfirm(); break;
        case SCREEN_PICKER:   renderPicker();   break;
        default: break;
    }
    forceFullRedraw_ = false;
}

// ---------------------------------------------------------------------------
// Header layout (160 wide, 12 tall)
//   [0..32]  state badge   "OK  " / "SCAN"
//   [34..78] gps badge     "GPS 3D" / "GPS NO"
//   [80..134] battery      "BAT 87%"
//   [138..159] rec dot     filled red circle if recording
void Ui::renderHeader() {
    if (forceFullRedraw_) {
        tft.fillRect(0, 0, cfg::TFT_W, HEADER_H, COL_HEADER);
    }

    field(0, 2, 2, 30, 8,
          stateName(rcState_), stateColor(rcState_), COL_HEADER, 1);

    const bool fix = gps_ && gps_->hasFix();
    char gbuf[10]; snprintf(gbuf, sizeof(gbuf), "GPS %s", fix ? "3D" : "NO");
    // Red text on header bg when no fix; green text on header bg when locked.
    field(1, 36, 2, 44, 8, gbuf,
          fix ? COL_GREEN : COL_RED,
          COL_HEADER, 1);

    char bbuf[12];
    if (vbatPct_ >= 0) snprintf(bbuf, sizeof(bbuf), "BAT %3d%%", vbatPct_);
    else               snprintf(bbuf, sizeof(bbuf), "BAT --%%");
    // v0.4.9: green >= 40%, amber 20-39%, red < 20%, dim = unknown.
    uint16_t bcol = (vbatPct_ < 0) ? COL_DIM
                  : (vbatPct_ < 20 ? COL_RED
                  : (vbatPct_ < 40 ? COL_AMBER : COL_GREEN));
    field(2, 84, 2, 54, 8, bbuf, bcol, COL_HEADER, 1);

    // Recording dot: always draw the circle so the position is always visible.
    // Filled red = recording; dim outline = idle.
    const bool rec = store_ && store_->isRecording();
    static bool prevRec = false;
    if (forceFullRedraw_ || rec != prevRec) {
        tft.fillRect(140, 1, 18, 10, COL_HEADER);
        if (rec) tft.fillCircle(149, 6, 4, COL_RED);
        else     tft.drawCircle(149, 6, 4, COL_DIM);
        prevRec = rec;
    }
}

// ---------------------------------------------------------------------------
// STATS screen (160 x 68 below header)
//   y=14: "DOSE nSv/h"  (x=4..63)   | "Smp NNNNN"  (x=100..157)
//   y=22: big nSv/h value (size 3) ~24px tall
//   y=46: "CPS"   small dim    + count rate (size 2) ~16px
//   y=66: footer (errors / addr last 5)
void Ui::renderStats() {
    field(10, 4, 14, 60, 8, "DOSE nSv/h", COL_DIM, COL_BG, 1);

    // Spectrum indicator badge — "SPC" in baby blue when spectrum collection
    // is active, invisible when off. Placed between the DOSE label and Smp counter.
    if (spectrumEnabled_) {
        field(18, 68, 14, 24, 8, "SPC", COL_LIGHT_BLUE, COL_BG, 1);
    } else {
        // Clear the area so stale text doesn't remain after toggle.
        field(18, 68, 14, 24, 8, "", COL_DIM, COL_BG, 1);
    }

    // Sample counter — right side of the DOSE label row (96 px free).
    // Shows sampleCount(): rises as samples are recorded; drops to 0 after
    // each upload cycle when the active file is rotated. This gives the user
    // a live "currently buffered / awaiting upload" count without the data-
    // loss confusion of the monotonic lifetimeSamples counter.
    // Green when recording + RC ready (actively filling); dim otherwise.
    {
        char sbuf[12];
        const uint32_t sc = store_ ? store_->sampleCount() : 0;
        snprintf(sbuf, sizeof(sbuf), "Smp%5lu", (unsigned long)sc);
        const bool rcOk = (rcState_ == RadiaCode::State::Ready);
        const bool rec  = store_ && store_->isRecording();
        field(17, 100, 14, 58, 8, sbuf, (rec && rcOk) ? COL_GREEN : COL_DIM, COL_BG, 1);
    }

    char buf[24];
    if (lastReading_.valid) {
        const float nsv = lastReading_.uSvPerHour * 1000.0f;
        if (nsv < 100)        snprintf(buf, sizeof(buf), "%5.2f", nsv);
        else if (nsv < 1000)  snprintf(buf, sizeof(buf), "%5.1f", nsv);
        else                  snprintf(buf, sizeof(buf), "%5.0f", nsv);
    } else {
        strcpy(buf, " --- ");
    }
    field(11, 4, 22, 110, 22, buf, COL_GREEN, COL_BG, 3);

    if (lastReading_.valid) {
        char e[12]; snprintf(e, sizeof(e), "+/-%2.0f%%", lastReading_.doseErrPct);
        field(12, 116, 26, 42, 8, e, COL_DIM, COL_BG, 1);
    } else {
        field(12, 116, 26, 42, 8, "", COL_DIM, COL_BG, 1);
    }

    field(13, 4, 46, 30, 8, "CPS", COL_DIM, COL_BG, 1);

    if (lastReading_.valid) snprintf(buf, sizeof(buf), "%5.1f", lastReading_.cps);
    else strcpy(buf, " --- ");
    field(14, 36, 46, 80, 16, buf, COL_FG, COL_BG, 2);

    if (lastReading_.valid) {
        char e[12]; snprintf(e, sizeof(e), "+/-%2.0f%%", lastReading_.cpsErrPct);
        field(15, 116, 50, 42, 8, e, COL_DIM, COL_BG, 1);
    } else {
        field(15, 116, 50, 42, 8, "", COL_DIM, COL_BG, 1);
    }

    // Footer: GPS accuracy when RC connected + fix; searching prompt or pick hint otherwise.
    char foot[28];
    uint16_t footCol = COL_DIM;
    if (rcState_ != RadiaCode::State::Ready) {
        snprintf(foot, sizeof(foot), "Hold: pick RC");
    } else if (gps_ && gps_->hasFix()) {
        const float hdopF = (float)gps_->hdop();
        const float acc   = (float)gps_->accuracyMeters();
        snprintf(foot, sizeof(foot), "+/-%4.1fm  hdop %.1f", acc, hdopF);
        footCol = (acc > 15.0f) ? COL_RED : (acc > 6.0f ? COL_AMBER : COL_GREEN);
    } else {
        snprintf(foot, sizeof(foot), "GPS: searching...");
        footCol = COL_AMBER;
    }
    field(16, 4, 66, 156, 8, foot, footCol, COL_BG, 1);
}

// ---------------------------------------------------------------------------
// GPS screen (160 x 68)
//   col1 (4..78): SAT count, HDOP, fix
//   col2 (82..156): LAT, LON, ALT, SPD
void Ui::renderGps() {
    if (!gps_) return;
    char buf[24];

    // FIX badge: green text on black when locked; red text on black when not.
    const bool hasFix = gps_->hasFix();
    snprintf(buf, sizeof(buf), "FIX %s", hasFix ? "3D" : "NO");
    field(20, 4, 14, 76, 8, buf,
          hasFix ? COL_GREEN : COL_RED,
          COL_BG, 1);

    // Satellites: red < 4, amber 4-6, green >= 7
    const uint8_t sats = gps_->satellites();
    const uint16_t satCol = (sats < 4) ? COL_RED : (sats < 7 ? COL_AMBER : COL_GREEN);
    snprintf(buf, sizeof(buf), "Sats %u", (unsigned)sats);
    field(21, 4, 26, 76, 8, buf, satCol, COL_BG, 1);

    // HDOP: red > 5, amber 2-5, green <= 2
    const float hdop = (float)gps_->hdop();
    const uint16_t hdopCol = (hdop > 5.0f) ? COL_RED : (hdop > 2.0f ? COL_AMBER : COL_GREEN);
    snprintf(buf, sizeof(buf), "HDOP %.1f", hdop);
    field(22, 4, 38, 76, 8, buf, hdopCol, COL_BG, 1);

    // Accuracy: HDOP * ~3m UERE; red > 15m, amber 6-15m, green <= 6m
    if (hasFix && hdop < 50.0f) {
        const float acc = hdop * 3.0f;
        const uint16_t accCol = (acc > 15.0f) ? COL_RED : (acc > 6.0f ? COL_AMBER : COL_GREEN);
        snprintf(buf, sizeof(buf), "+/-%4.1fm", acc);
        field(23, 4, 50, 76, 8, buf, accCol, COL_BG, 1);
    } else {
        field(23, 4, 50, 76, 8, "+/- ---", COL_DIM, COL_BG, 1);
    }

    if (hasFix) {
        snprintf(buf, sizeof(buf), "%.5f", gps_->latitude());
        field(24, 84, 14, 76, 8, buf, COL_FG, COL_BG, 1);
        snprintf(buf, sizeof(buf), "%.5f", gps_->longitude());
        field(25, 84, 26, 76, 8, buf, COL_FG, COL_BG, 1);
        snprintf(buf, sizeof(buf), "%.0fm", gps_->altitudeMeters());
        field(26, 84, 38, 76, 8, buf, COL_FG, COL_BG, 1);
        snprintf(buf, sizeof(buf), "%.1fkph", gps_->speedKph());
        field(27, 84, 50, 76, 8, buf, COL_FG, COL_BG, 1);
    } else {
        field(24, 84, 14, 76, 8, "  ---  ", COL_DIM, COL_BG, 1);
        field(25, 84, 26, 76, 8, "  ---  ", COL_DIM, COL_BG, 1);
        field(26, 84, 38, 76, 8, "  ---  ", COL_DIM, COL_BG, 1);
        field(27, 84, 50, 76, 8, "  ---  ", COL_DIM, COL_BG, 1);
    }

    // Footer: show smoothed bearing when fix locked, else nudge to go outside.
    if (!hasFix) {
        field(28, 4, 66, 156, 8, "Acquiring fix outdoors", COL_DIM, COL_BG, 1);
    } else {
        const double brg = gps_->bearingFromHistory();
        if (brg >= 0.0) {
            snprintf(buf, sizeof(buf), "Hdg %3.0f deg", brg);
            field(28, 4, 66, 156, 8, buf, COL_DIM, COL_BG, 1);
        } else {
            field(28, 4, 66, 156, 8, "Fix OK", COL_GREEN, COL_BG, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// STORAGE screen
void Ui::renderStorage() {
    if (!store_) return;
    char buf[40];

    // Hard-failure mode: SD was required and didn't mount. Show a single
    // unambiguous message so the user knows recording is disabled and the
    // device needs a power cycle. No half-measures.
    if (store_->storageFailed()) {
        if (forceFullRedraw_) {
            tft.fillRect(0, HEADER_H, cfg::TFT_W, cfg::TFT_H - HEADER_H, COL_BG);
        }
        field(28, 4, 14, 156, 8, "STORAGE INIT FAILED", COL_RED, COL_BG, 1);
        field(29, 4, 26, 156, 8, "SD card not detected", COL_AMBER, COL_BG, 1);
        field(30, 4, 40, 156, 8, "Please reboot device", COL_FG, COL_BG, 1);
        field(31, 4, 54, 156, 8, "Check card seat / power", COL_DIM, COL_BG, 1);
        field(32, 4, 66, 156, 8, "Recording is DISABLED", COL_RED, COL_BG, 1);
        return;
    }

    const bool rec = store_->isRecording();
    const bool fix = gps_ && gps_->hasFix();
    field(30, 4, 14, 24, 8, "REC", COL_DIM, COL_BG, 1);
    // v0.4.0: "AUTO" replaces ON/OFF since recording is no longer user-toggled.
    // GREEN  = day file open AND GPS fix (samples being written)
    // AMBER  = day file open but no GPS fix right now (samples being dropped)
    // DIM    = waiting for the first valid GPS sample to open today's file
    // Labels are <= 7 chars (7 * 6 = 42px) so they fit in the 50px field
    // without overlapping the adjacent "Samp" field.
    // "NO GPS" replaces the old "AUTO -gps" which was 9 chars and bled into
    // the sample-count field.
    const char*    autoLabel = rec ? (fix ? "AUTO ok" : "NO GPS") : "WAIT...";
    const uint16_t autoCol   = rec ? (fix ? COL_GREEN : COL_AMBER) : COL_DIM;
    field(31, 30, 14, 50, 8, autoLabel, autoCol, COL_BG, 1);

    // v0.4.9: show sampleCount() not lifetimeSamples().  lifetimeSamples was
    // chosen to avoid a user seeing 326->0 on upload; but now users *want* the
    // counter to show "how many samples are on the device now" so they know when
    // the last upload cycle cleared everything.  sampleCount() resets to 0 when
    // rotateForUpload() renames the active file to .up.csv, matching the point
    // where Disk usage also drops to near-zero.
    snprintf(buf, sizeof(buf), "Samp %lu", (unsigned long)store_->sampleCount());
    field(32, 82, 14, 74, 8, buf, COL_FG, COL_BG, 1);

    if (rec) {
        snprintf(buf, sizeof(buf), "Day %s", store_->activeId().c_str());
    } else {
        strcpy(buf, "(awaiting GPS UTC)");
    }
    field(33, 4, 26, 156, 8, buf, COL_FG, COL_BG, 1);

    // Disk bar
    const int pct = store_->percentUsed();
    snprintf(buf, sizeof(buf), "Disk %d%%  %lu/%luK",
             pct,
             (unsigned long)(store_->usedBytes() / 1024),
             (unsigned long)(store_->totalBytes() / 1024));
    field(34, 4, 38, 156, 8, buf, COL_DIM, COL_BG, 1);

    // Bar drawing: only redraw when percentage rounded changed
    static int prevPct = -1;
    if (forceFullRedraw_ || pct != prevPct) {
        const int barX = 4, barY = 50, barW = cfg::TFT_W - 8, barH = 6;
        tft.drawRect(barX, barY, barW, barH, COL_DIM);
        tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, COL_BG);
        const int fill = (barW - 2) * pct / 100;
        tft.fillRect(barX + 1, barY + 1, fill, barH - 2,
                     pct > 85 ? COL_RED : (pct > 60 ? COL_AMBER : COL_GREEN));
        prevPct = pct;
    }



    // v0.4.9: "Pending" shows files awaiting upload (total - active), colored
    // green when clean, amber when data is queued.  Replaced the old
    // "Files on disk: N" label which was confusing to users.
    const int rawPending = (int)store_->sessionCount() - (store_->isRecording() ? 1 : 0);
    const int pending    = rawPending < 0 ? 0 : rawPending;
    snprintf(buf, sizeof(buf), "Pending: %d", pending);
    field(35, 4, 56, 156, 8, buf,
          pending > 0 ? COL_AMBER : COL_GREEN,
          COL_BG, 1);

    // v0.4.7: granular Wi-Fi state. The previous "busy = uploading..." lied
    // to the user during the 12-second connect attempts (most of which fail
    // after a reboot), so they thought uploads were stuck when they were
    // actually still trying to associate with the AP.
    char wifiBuf[40];
    uint16_t wifiCol = COL_DIM;
    if (!wifi_ || !wifi_->enabled()) {
        snprintf(wifiBuf, sizeof(wifiBuf), "Wi-Fi: disabled");
    } else {
        const auto ph = wifi_->phase();
        switch (ph) {
        case WifiUploader::Phase::Connecting:
            snprintf(wifiBuf, sizeof(wifiBuf), "Wi-Fi: connecting...");
            wifiCol = COL_AMBER;
            break;
        case WifiUploader::Phase::Posting:
            snprintf(wifiBuf, sizeof(wifiBuf), "Wi-Fi: uploading...");
            wifiCol = COL_GREEN;
            break;
        case WifiUploader::Phase::Disconnecting:
            snprintf(wifiBuf, sizeof(wifiBuf), "Wi-Fi: cleanup...");
            wifiCol = COL_DIM;
            break;
        default: {
            // Idle / Backoff -> show countdown.
            const uint32_t next = wifi_->nextAttemptMs();
            const uint32_t now  = millis();
            const bool inBackoff = (ph == WifiUploader::Phase::Backoff);
            if (next > now) {
                uint32_t remainMs = next - now;
                uint32_t remainS  = (remainMs + 999) / 1000;
                const char* prefix = inBackoff ? "Retry" : "Next sync";
                if (remainS >= 60) {
                    snprintf(wifiBuf, sizeof(wifiBuf), "%s: %um %02us",
                             prefix, (unsigned)(remainS / 60), (unsigned)(remainS % 60));
                } else {
                    snprintf(wifiBuf, sizeof(wifiBuf), "%s: %us", prefix, (unsigned)remainS);
                }
                wifiCol = inBackoff ? COL_AMBER : COL_GREEN;
            } else {
                snprintf(wifiBuf, sizeof(wifiBuf), inBackoff ? "Retrying..." : "Next sync: soon");
                wifiCol = COL_GREEN;
            }
            break;
        }
        }
    }
    field(36, 4, 64, 156, 8, wifiBuf, wifiCol, COL_BG, 1);
    // Hold-to-sync hint. Cleared with a blank field when Wi-Fi is disabled
    // so a previous force-full-redraw doesn't leave stale text.
    if (wifi_ && wifi_->enabled()) {
        field(37, 4, 72, 156, 8, "Hold: sync now", COL_DIM, COL_BG, 1);
    } else {
        field(37, 4, 72, 156, 8, "", COL_DIM, COL_BG, 1);
    }
}

// ---------------------------------------------------------------------------
// DOSE screen (160 x 68 below header)
//
// Pixel layout — all measurements from top-left of the 160x80 panel.
// Header occupies y=0..11.  Body starts at y=12.
//
//  y=14  "TOTAL DOSE"          DIM  size-1  x=4   w=78   (left label)
//  y=14  "since reset"         DIM  size-1  x=94  w=62   (right context)
//  y=24  5-char value          GREEN size-3  x=4   w=90   (24 px tall: y=24..47)
//  y=32  unit "uSv"/"mSv"      DIM  size-1  x=96  w=30   (vertically centred)
//  y=50  separator line        DIM  1px     x=4..155
//  y=56  "Rate: X.XXX uSv/h"   FG   size-1  x=4   w=156
//  y=68  "Hold: reset dose"    DIM  size-1  x=4   w=156
//
// Hold=long-press emits ACTION_RESET_DOSE, handled in main.cpp.
void Ui::renderDose() {
    // Static labels ---------------------------------------------------------
    field(10, 4,  14, 78, 8, "TOTAL DOSE",   COL_DIM,   COL_BG, 1);
    field(11, 94, 14, 62, 8, "since reset",  COL_DIM,   COL_BG, 1);

    // Big accumulated dose value (auto-scales µSv → mSv at 1 000 µSv) -------
    char val[10];
    char unit[5];
    const float dose = tripDoseMicroSv_;
    if (dose < 1000.0f) {
        if      (dose < 10.0f)  snprintf(val, sizeof(val), "%5.3f", dose);
        else if (dose < 100.0f) snprintf(val, sizeof(val), "%5.2f", dose);
        else                    snprintf(val, sizeof(val), "%5.1f", dose);
        snprintf(unit, sizeof(unit), "uSv");
    } else {
        const float mSv = dose / 1000.0f;
        if      (mSv < 10.0f)  snprintf(val, sizeof(val), "%5.3f", mSv);
        else if (mSv < 100.0f) snprintf(val, sizeof(val), "%5.2f", mSv);
        else                   snprintf(val, sizeof(val), "%5.1f", mSv);
        snprintf(unit, sizeof(unit), "mSv");
    }
    field(12, 4,  24, 90, 24, val,  COL_GREEN, COL_BG, 3);
    field(13, 96, 32, 30,  8, unit, COL_DIM,   COL_BG, 1);

    // Separator line (draw once on full redraw) ------------------------------
    if (forceFullRedraw_) {
        tft.drawFastHLine(4, 50, 152, COL_DIM);
    }

    // Current instantaneous rate for context --------------------------------
    char rateBuf[28];
    if (lastReading_.valid) {
        snprintf(rateBuf, sizeof(rateBuf), "Rate: %6.3f uSv/h",
                 lastReading_.uSvPerHour);
        field(14, 4, 56, 156, 8, rateBuf, COL_FG, COL_BG, 1);
    } else {
        field(14, 4, 56, 156, 8, "Rate: ---", COL_DIM, COL_BG, 1);
    }

    // Footer hint -----------------------------------------------------------
    field(15, 4, 68, 156, 8, "Hold: reset dose", COL_DIM, COL_BG, 1);
}

// ---------------------------------------------------------------------------
// LIFETIME screen (160 x 68 below header, v1.0.0)
//
// Two-column grid, 5 data rows + 1 hint footer.
// Pixel layout:
//   y=14  "DIST"                   (full width label)
//   y=22  value km + mi            (full width value)
//   — sep y=31 —
//   y=34  "REC TIME" | "NOT REC"   (half-width labels)
//   y=44  value      | value
//   — sep y=53 —
//   y=56  "ALT GAIN" | "UPLOADS"   (half-width labels)
//   y=64  value      | value
//   y=71  "Hold: reset?"
//
// LIFETIME screen 1/2: Distance (full width), Rec Time vs Not-Rec Time, Alt Gain | Uploads.
// Long-press navigates to SCREEN_LIFETIME_CONFIRM before any data is cleared.
void Ui::renderLifetime() {
    if (!life_) return;
    char buf[24];

    // ---- Row 1: DIST (full width) -------------------------------------------
    field(10, 4, 14, 152, 8, "DIST", COL_DIM, COL_BG, 1);
    {
        const float km = life_->distanceKm();
        const float mi = km * 0.621371f;
        if (km < 100.0f)       snprintf(buf, sizeof(buf), "%.1fkm  %.1fmi", km, mi);
        else if (km < 10000.f) snprintf(buf, sizeof(buf), "%.0fkm  %.0fmi", km, mi);
        else                   snprintf(buf, sizeof(buf), "%.0fkm", km);
        field(11, 4, 22, 152, 8, buf, COL_GREEN, COL_BG, 1);
    }

    // Separator line
    if (forceFullRedraw_) {
        tft.drawFastHLine(4, 31, 152, COL_DIM);
    }

    // ---- Row 2: REC TIME | NOT REC (side by side) ---------------------------
    field(12, 4,  34, 76, 8, "REC TIME", COL_DIM, COL_BG, 1);
    field(13, 84, 34, 72, 8, "NOT REC",  COL_DIM, COL_BG, 1);
    {
        const uint32_t totalSecs = life_->recordingSecs();
        const uint32_t days  = totalSecs / 86400;
        const uint32_t hours = (totalSecs % 86400) / 3600;
        const uint32_t mins  = (totalSecs % 3600)  / 60;
        if (days > 0)       snprintf(buf, sizeof(buf), "%ud %02uh",  (unsigned)days, (unsigned)hours);
        else if (hours > 0) snprintf(buf, sizeof(buf), "%uh %02um",  (unsigned)hours, (unsigned)mins);
        else                snprintf(buf, sizeof(buf), "%um",         (unsigned)mins);
        field(14, 4, 44, 76, 8, buf, COL_FG, COL_BG, 1);
    }
    {
        const uint32_t idleSecs = life_->idleSecs();
        const uint32_t days  = idleSecs / 86400;
        const uint32_t hours = (idleSecs % 86400) / 3600;
        const uint32_t mins  = (idleSecs % 3600)  / 60;
        if (days > 0)       snprintf(buf, sizeof(buf), "%ud %02uh",  (unsigned)days, (unsigned)hours);
        else if (hours > 0) snprintf(buf, sizeof(buf), "%uh %02um",  (unsigned)hours, (unsigned)mins);
        else                snprintf(buf, sizeof(buf), "%um",         (unsigned)mins);
        field(15, 84, 44, 72, 8, buf, COL_FG, COL_BG, 1);
    }

    // Separator line
    if (forceFullRedraw_) {
        tft.drawFastHLine(4, 53, 152, COL_DIM);
    }

    // ---- Row 3: ALT GAIN | UPLOADS (side by side) ---------------------------
    field(16, 4,  56, 76, 8, "ALT GAIN", COL_DIM, COL_BG, 1);
    field(17, 84, 56, 72, 8, "UPLOADS",  COL_DIM, COL_BG, 1);
    {
        const float m  = life_->altGainM();
        const float ft = m * 3.28084f;
        if (m < 10000.0f) snprintf(buf, sizeof(buf), "%.0fm %.0fft", m, ft);
        else              snprintf(buf, sizeof(buf), "%.0fm", m);
        field(18, 4, 64, 76, 8, buf, COL_GREEN, COL_BG, 1);
    }
    {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)life_->wifiUploads());
        field(19, 84, 64, 72, 8, buf, COL_FG, COL_BG, 1);
    }

    // Footer hint (y=71 → 8px font fits within 80px display)
    field(20, 4, 71, 156, 8, "Hold: reset?", COL_DIM, COL_BG, 1);
}

// LIFETIME screen 2/2: Spikes, Cells, Data written, Battery cycles.
// Long-press navigates to SCREEN_LIFETIME_CONFIRM before any data is cleared.
void Ui::renderLifetime2() {
    if (!life_) return;
    char buf[24];

    // ---- Row 1: SPIKES | CELLS ----------------------------------------------
    field(19, 4,  14, 76, 8, "SPIKES",     COL_DIM, COL_BG, 1);
    field(20, 84, 14, 72, 8, "CELLS",      COL_DIM, COL_BG, 1);

    {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)life_->spikeEvents());
        const uint16_t spkCol = life_->spikeEvents() > 0 ? COL_AMBER : COL_DIM;
        field(21, 4, 26, 76, 8, buf, spkCol, COL_BG, 1);
    }
    {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)life_->uniqueCells());
        field(22, 84, 26, 72, 8, buf, COL_FG, COL_BG, 1);
    }

    // Separator line
    if (forceFullRedraw_) {
        tft.drawFastHLine(4, 38, 152, COL_DIM);
    }

    // ---- Row 2: DATA WRITTEN | BAT CYCLES -----------------------------------
    field(23, 4,  42, 76, 8, "DATA",       COL_DIM, COL_BG, 1);
    field(24, 84, 42, 72, 8, "BAT CYCLES", COL_DIM, COL_BG, 1);

    // Total data: show in KB or MB
    {
        const uint64_t bytes = life_->totalBytes();
        if (bytes < 1024*1024ULL) snprintf(buf, sizeof(buf), "%luKB", (unsigned long)(bytes / 1024));
        else                      snprintf(buf, sizeof(buf), "%luMB", (unsigned long)(bytes / 1024 / 1024));
        field(25, 4, 54, 76, 8, buf, COL_FG, COL_BG, 1);
    }
    {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)life_->battCycles());
        field(26, 84, 54, 72, 8, buf, COL_FG, COL_BG, 1);
    }

    // Footer hint
    field(27, 4, 70, 156, 8, "Hold: reset?", COL_DIM, COL_BG, 1);
}

// ---------------------------------------------------------------------------
// ABOUT screen: firmware version, build info, flash/storage stats.
// Long-press toggles spectrum collection mode.
void Ui::renderAbout() {
    char buf[32];

    // ---- Row 1: FW version + build date -------------------------------------
    field(50, 4, 14, 152, 8, "FW", COL_DIM, COL_BG, 1);
    {
        snprintf(buf, sizeof(buf), "%s %s", cfg::FW_VERSION, __DATE__);
        field(51, 4, 22, 152, 8, buf, COL_GREEN, COL_BG, 1);
    }

    // Separator line
    if (forceFullRedraw_) {
        tft.drawFastHLine(4, 31, 152, COL_DIM);
    }

    // ---- Row 2: Flash total | Free heap ------------------------------------
    field(52, 4,  34, 76, 8, "FLASH",      COL_DIM, COL_BG, 1);
    field(53, 84, 34, 72, 8, "HEAP FREE",  COL_DIM, COL_BG, 1);
    {
        const uint32_t flashBytes = ESP.getFlashChipSize();
        snprintf(buf, sizeof(buf), "%luKB", (unsigned long)(flashBytes / 1024));
        field(54, 4, 44, 76, 8, buf, COL_FG, COL_BG, 1);
    }
    {
        const uint32_t heap = ESP.getFreeHeap();
        if (heap > 100 * 1024)
            snprintf(buf, sizeof(buf), "%luKB", (unsigned long)(heap / 1024));
        else
            snprintf(buf, sizeof(buf), "%luB", (unsigned long)heap);
        uint16_t heapCol = COL_FG;
        if (heap < cfg::WIFI_HEAL_MIN_HEAP) heapCol = COL_RED;
        else if (heap < 80000) heapCol = COL_AMBER;
        field(55, 84, 44, 72, 8, buf, heapCol, COL_BG, 1);
    }

    // ---- Row 3: Storage backend + uptime ------------------------------------
    field(56, 4,  56, 76, 8, "STORAGE",    COL_DIM, COL_BG, 1);
    field(57, 84, 56, 72, 8, "UPTIME",     COL_DIM, COL_BG, 1);
    {
        if (store_) {
            snprintf(buf, sizeof(buf), "%s", store_->backendName());
            uint16_t col = store_->storageFailed() ? COL_RED : COL_GREEN;
            field(58, 4, 64, 76, 8, buf, col, COL_BG, 1);
        } else {
            field(58, 4, 64, 76, 8, "none", COL_RED, COL_BG, 1);
        }
    }
    {
        const uint32_t secs = millis() / 1000;
        const uint32_t mins = secs / 60;
        const uint32_t hrs  = mins / 60;
        if (hrs > 24)       snprintf(buf, sizeof(buf), "%ud %uh", (unsigned)(hrs/24), (unsigned)(hrs%24));
        else if (hrs > 0)   snprintf(buf, sizeof(buf), "%uh %02um", (unsigned)hrs, (unsigned)(mins%60));
        else                snprintf(buf, sizeof(buf), "%um %02us", (unsigned)mins, (unsigned)(secs%60));
        field(59, 84, 64, 72, 8, buf, COL_FG, COL_BG, 1);
    }

    // Footer: spectrum mode status + toggle hint
    {
        snprintf(buf, sizeof(buf), "Spectrum: %s", spectrumEnabled_ ? "ON" : "OFF");
        uint16_t col = spectrumEnabled_ ? COL_GREEN : COL_DIM;
        field(60, 4, 71, 156, 8, buf, col, COL_BG, 1);
    }

    // Toggle hint
    field(61, 4, 63, 156, 8, "Hold: toggle spec", COL_DIM, COL_BG, 1);
}

// ---------------------------------------------------------------------------
// LIFETIME CONFIRM screen: safety gate before wiping all lifetime counters.
// Short-press = cancel (returns to the LIFETIME screen that triggered it).
// Long-press  = confirm reset (emits ACTION_RESET_LIFETIME, returns to same screen).
void Ui::renderLifetimeConfirm() {
    if (!forceFullRedraw_) return;
    tft.fillRect(0, HEADER_H, cfg::TFT_W, cfg::TFT_H - HEADER_H, COL_BG);

    // Title
    tft.setTextSize(1);
    tft.setTextColor(COL_AMBER, COL_BG);
    tft.setCursor(4, 14); tft.print("RESET LIFETIME?");

    // Warning body
    tft.setTextColor(COL_FG, COL_BG);
    tft.setCursor(4, 26); tft.print("This clears ALL");
    tft.setCursor(4, 35); tft.print("lifetime counters.");

    // Separator
    tft.drawFastHLine(4, 46, 152, COL_DIM);

    // Instructions
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setCursor(4, 50); tft.print("Short: cancel");
    tft.setTextColor(COL_AMBER, COL_BG);
    tft.setCursor(4, 60); tft.print("Long:  CONFIRM");
}

// ---------------------------------------------------------------------------
// PICKER screen: list of nearby BLE devices found during scan.
// RadiaCode 110 sometimes advertises with no name, so we list ALL nearby
// advertisers sorted by signal strength. Likely RadiaCode matches (name or
// service UUID) are prefixed with '*' in green.
void Ui::renderPicker() {
    if (!forceFullRedraw_) return;       // only redraw on real changes
    tft.fillRect(0, HEADER_H, cfg::TFT_W, cfg::TFT_H - HEADER_H, COL_BG);

    if (pickList_.empty()) {
        tft.setTextSize(1);
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(4, 16); tft.print("Scanning...");
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(4, 30); tft.print("no devices yet");
        tft.setCursor(4, 46); tft.print("long press = cancel");
        return;
    }

    // Build sorted view (descending RSSI, likely matches first within tie).
    std::vector<int> order(pickList_.size());
    for (size_t i = 0; i < pickList_.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b){
        const auto& ra = pickList_[a];
        const auto& rb = pickList_[b];
        if (ra.likelyMatch != rb.likelyMatch) return ra.likelyMatch && !rb.likelyMatch;
        return ra.rssi > rb.rssi;
    });
    pickerOrder_ = order;   // remember mapping so onLongPress picks correct entry

    const int rowH = 12;
    const int total = (int)pickList_.size() + 1;       // +1 for Cancel
    const int show = total < 5 ? total : 5;

    char line[40];
    for (int row = 0; row < show; ++row) {
        const int y = HEADER_H + 1 + row * rowH;
        const bool selected = (row == pickerCursor_);
        const uint16_t bg = selected ? COL_PICK : COL_BG;
        tft.fillRect(0, y, cfg::TFT_W, rowH, bg);
        tft.setTextSize(1);

        if (row < (int)pickList_.size()) {
            const auto& r = pickList_[order[row]];
            // Label: name if present; if it's a likely RadiaCode without a
            // resolved name, show "RadiaCode?" so the user knows to pick it.
            // Otherwise fall back to last 3 octets of the MAC.
            char label[20];
            if (!r.name.empty()) {
                snprintf(label, sizeof(label), "%-16.16s", r.name.c_str());
            } else if (r.likelyMatch) {
                snprintf(label, sizeof(label), "%-16.16s", "RadiaCode?");
            } else {
                std::string a = r.address;
                std::string tail = a.length() >= 8
                    ? a.substr(a.length() - 8) : a;     // "xx:xx:xx"
                char tmp[20];
                snprintf(tmp, sizeof(tmp), "?%s", tail.c_str());
                snprintf(label, sizeof(label), "%-16.16s", tmp);
            }
            const uint16_t fg = r.likelyMatch ? COL_GREEN : 0xFFFF;
            const char marker = r.likelyMatch ? '*' : (selected ? '>' : ' ');
            snprintf(line, sizeof(line), "%c%s", marker, label);
            tft.setTextColor(fg, bg);
            tft.setCursor(2, y + 2); tft.print(line);
            // RSSI right-aligned
            char rs[8]; snprintf(rs, sizeof(rs), "%4d", r.rssi);
            tft.setTextColor(0xFFFF, bg);
            tft.setCursor(cfg::TFT_W - 26, y + 2); tft.print(rs);
        } else {
            snprintf(line, sizeof(line), "%c [Cancel]",
                     selected ? '>' : ' ');
            tft.setTextColor(0xFFFF, bg);
            tft.setCursor(2, y + 2); tft.print(line);
        }
    }
}
