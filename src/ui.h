#pragma once
#include <Arduino.h>
#include <vector>
#include "radiacode.h"

class GpsModule;
class SessionStore;
class WifiUploader;

class Ui {
public:
    enum Screen : uint8_t {
        SCREEN_STATS = 0,
        SCREEN_GPS,
        SCREEN_STORAGE,
        SCREEN_DOSE,
        SCREEN_PICKER,
        SCREEN_NORMAL_COUNT = SCREEN_PICKER, // STATS/GPS/STORAGE/DOSE cycle
    };

    void begin();
    void setSources(GpsModule* gps, SessionStore* store, RadiaCode* rc);
    void setWifi(WifiUploader* w) { wifi_ = w; }

    void onShortPress();
    void onLongPress();

    void setReading(const RadiaCode::Reading& r);
    void setRadiaState(RadiaCode::State s, const String& addr);
    void setBatteryPercent(int pct) { vbatPct_ = pct; }
    // Cumulative trip dose (µSv) accumulated since last reset. Updated each
    // main-loop iteration; read by renderDose().
    void setTripDose(float microSv) { tripDoseMicroSv_ = microSv; }

    // Picker entry / exit
    void enterPicker(const std::vector<RadiaCode::ScanResult>& results);
    void exitPicker() { screen_ = SCREEN_STATS; forceFullRedraw_ = true; }

    void tick();

    enum LongAction : uint8_t {
        ACTION_NONE = 0,
        ACTION_START_PICKER,    // Stats long-press: scan + show picker
        ACTION_PICK_DEVICE,     // Picker: connect to selected
        ACTION_CANCEL_PICKER,
        ACTION_RESET_DOSE,      // DOSE screen long-press: zero accumulator
    };
    LongAction lastLongAction() {
        LongAction a = pendingAction_;
        pendingAction_ = ACTION_NONE;
        return a;
    }
    String pickedAddress() const { return pickedAddr_; }
    uint8_t pickedAddrType() const { return pickedAddrType_; }

private:
    // Flicker-free field redraw. Each call site picks a unique index 0..MAX_FIELDS-1.
    void field(int idx, int x, int y, int w, int h,
               const char* str, uint16_t fg, uint16_t bg, uint8_t size);

    void renderHeader();
    void renderStats();
    void renderGps();
    void renderStorage();
    void renderDose();
    void renderPicker();

    Screen        screen_ = SCREEN_STATS;
    GpsModule*    gps_ = nullptr;
    SessionStore* store_ = nullptr;
    RadiaCode*    rc_ = nullptr;
    WifiUploader* wifi_ = nullptr;

    RadiaCode::Reading lastReading_{};
    RadiaCode::State   rcState_ = RadiaCode::State::Idle;
    String             rcAddr_;
    int                vbatPct_ = -1;

    LongAction         pendingAction_ = ACTION_NONE;
    float              tripDoseMicroSv_ = 0.0f;  // µSv accumulated since last reset
    // v0.4.0: recording is always-on whenever RadiaCode + GPS fix are present,
    // so the legacy double-long-press stop-confirmation no longer exists.
    bool               forceFullRedraw_ = true;
    Screen             lastDrawnScreen_ = SCREEN_NORMAL_COUNT;

    static constexpr int MAX_FIELDS = 40;
    String   prevText_[MAX_FIELDS];
    uint16_t prevFg_[MAX_FIELDS] = {0};
    uint8_t  prevSize_[MAX_FIELDS] = {0};

    // Picker state
    std::vector<RadiaCode::ScanResult> pickList_;
    std::vector<int> pickerOrder_;          // sort order indices into pickList_
    int    pickerCursor_ = 0;
    String pickedAddr_;
    uint8_t pickedAddrType_ = 0;
};
