#pragma once
#include <Arduino.h>
#include <vector>
#include "lifetime_stats.h"
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
        SCREEN_RATIO_TREND,
        SCREEN_LIFETIME,
        SCREEN_LIFETIME2,
        SCREEN_PICKER,
        SCREEN_NORMAL_COUNT = SCREEN_PICKER, // STATS/GPS/STORAGE/DOSE/RATIO_TREND/LIFETIME/LIFETIME2 cycle
    };

    void begin();
    void setSources(GpsModule* gps, SessionStore* store, RadiaCode* rc);
    void setWifi(WifiUploader* w) { wifi_ = w; }
    void setLifetimeStats(LifetimeStats* l) { life_ = l; }

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
        ACTION_RESET_LIFETIME,  // LIFETIME screen long-press: zero all lifetime counters
        ACTION_FORCE_SYNC,      // STORAGE long-press: kick upload cycle now
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
    void renderRatioTrend();
    void _drawRatioChart();
    // D/C Trend helpers
    void updateRatioTrend(const RadiaCode::Reading& r, uint32_t nowMs);
    void finishRatioBin(uint32_t nowMs);
    void insertRatioPoint(float ratio, bool valid);
    float calculateDosePerCount(float uSvPerHour, float cps);
    float calculateDeviationPercent(float ratio, float baseline);
    int mapRatioDeviationToY(float deviationPct, float scalePct, int zeroY, int halfHeight);
    void drawRatioSparkline(int chartX, int chartY, int chartW, int chartH,
                             int zeroY, int halfHeight, float scalePct);
    void renderLifetime();
    void renderLifetime2();
    void renderPicker();

    Screen        screen_ = SCREEN_STATS;
    GpsModule*     gps_ = nullptr;
    SessionStore*  store_ = nullptr;
    RadiaCode*     rc_ = nullptr;
    WifiUploader*  wifi_ = nullptr;
    LifetimeStats* life_ = nullptr;

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

    static constexpr int MAX_FIELDS = 50;
    String   prevText_[MAX_FIELDS];
    uint16_t prevFg_[MAX_FIELDS] = {0};
    uint8_t  prevSize_[MAX_FIELDS] = {0};

    // Picker state
    std::vector<RadiaCode::ScanResult> pickList_;
    std::vector<int> pickerOrder_;          // sort order indices into pickList_
    int    pickerCursor_ = 0;
    String pickedAddr_;
    uint8_t pickedAddrType_ = 0;

    // ---- D/C Trend state (5-minute circular buffer) ----
    static constexpr uint32_t RATIO_BIN_MS = 5000;
    static constexpr size_t RATIO_POINT_COUNT = 60;
    static constexpr size_t BASELINE_WARMUP_BINS = 6;
    static constexpr uint16_t MIN_SAMPLES_PER_BIN = 3;
    static constexpr float MIN_VALID_CPS = 0.25f;
    static constexpr float BASELINE_ALPHA = 0.0083f;
    static constexpr float BASELINE_UPDATE_LIMIT_PCT = 25.0f;
    static constexpr float RATIO_NEUTRAL_PCT = 1.0f;
    static constexpr float MIN_GRAPH_SCALE_PCT = 10.0f;
    static constexpr float MAX_GRAPH_SCALE_PCT = 100.0f;
    static constexpr uint32_t RATIO_STALE_MS = 10000;
    static constexpr float MAX_PLAUSIBLE_RATIO = 1000.0f;

    // Bin accumulator
    float  ratioDoseSum_ = 0.0f;
    float  ratioCpsSum_ = 0.0f;
    uint16_t ratioBinSamples_ = 0;
    uint32_t ratioBinStartMs_ = 0;
    uint32_t ratioLastReadingMs_ = 0;

    // Circular buffer: 60 completed bins
    float ratioRaw_[RATIO_POINT_COUNT] = {};
    bool  ratioValid_[RATIO_POINT_COUNT] = {};
    size_t ratioWriteIndex_ = 0;
    size_t ratioCount_ = 0;

    // Baseline
    float ratioWarmup_[BASELINE_WARMUP_BINS] = {};
    size_t ratioWarmupCount_ = 0;
    bool  ratioBaselineValid_ = false;
    float ratioBaseline_ = 0.0f;
    float ratioCurrentDeviationPct_ = 0.0f;

    // Graph scale
    float ratioDisplayScalePct_ = MIN_GRAPH_SCALE_PCT;

    // Redraw & thread safety
    bool  ratioChartDirty_ = true;
    portMUX_TYPE ratioMux_ = portMUX_INITIALIZER_UNLOCKED;
};
