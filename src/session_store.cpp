#include "session_store.h"
#include "config.h"

#include <LittleFS.h>
#include "event_log.h"
#include <SD.h>
#include <SD_MMC.h>
#include <SdFat.h>
#include <SPI.h>
#include <algorithm>
#include <time.h>

namespace {
// Dedicated SPI bus for the SD card. On ESP32-S3 the FSPI controller (SPI2)
// has IOMUX fast paths for low-numbered GPIOs (we're on 4-7), and tends to
// be more reliable for SD-over-SPI than the GPIO-matrix-only HSPI (SPI3).
// The TFT, when present, lives on GPIOs 38-42, so buses don't collide.
SPIClass gSdSpi(FSPI);

// SdFat instance for the diagnostic preflight. Templated on FAT16/32/exFAT
// so it handles modern SDHC/SDXC cards.
SdFs gSdFat;

// Send the SD-spec power-up sequence: with CS held HIGH, clock at least 74
// cycles so the card transitions from native-mode into SPI-mode. Many cheap
// cards don't latch onto SPI mode reliably without this. We bit-bang it
// since the SPI peripheral hasn't been configured for transactions yet.
void sdBitBangWakeup(uint8_t sckPin, uint8_t mosiPin, uint8_t csPin) {
    pinMode(sckPin,  OUTPUT);
    pinMode(mosiPin, OUTPUT);
    pinMode(csPin,   OUTPUT);
    digitalWrite(csPin,  HIGH);
    digitalWrite(mosiPin, HIGH);
    digitalWrite(sckPin,  LOW);
    delayMicroseconds(2000);  // Vdd ramp settle
    // 100 cycles, well over the 74 required.
    for (int i = 0; i < 100; ++i) {
        digitalWrite(sckPin, HIGH);
        delayMicroseconds(2);
        digitalWrite(sckPin, LOW);
        delayMicroseconds(2);
    }
    digitalWrite(csPin,  HIGH);
    digitalWrite(mosiPin, HIGH);
}
String makeSessionId() {
    // Legacy helper kept only because some old callers may reference it.
    // In the always-on v0.4.0+ model day-file naming is canonical and the
    // public API never generates synthetic boot ids.
    char buf[24];
    snprintf(buf, sizeof(buf), "boot_%lu", (unsigned long)millis());
    return String(buf);
}

String pathFor(const String& id) {
    // Bare id like "2026-05-11" -> /sessions/2026-05-11.csv
    // Id with embedded ".up" (rotated pending-upload) -> /sessions/<id>.csv
    return String(cfg::SESSIONS_DIR) + "/" + id + ".csv";
}

String pendingFilename(const String& dayId, uint32_t bootMs) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%s.%lu.up.csv", dayId.c_str(),
             (unsigned long)bootMs);
    return String(buf);
}

String stripCsvSuffix(const String& fname) {
    if (fname.endsWith(".csv")) return fname.substring(0, fname.length() - 4);
    return fname;
}

String fileBaseName(const String& path) {
    int slash = path.lastIndexOf('/');
    return (slash >= 0) ? path.substring(slash + 1) : path;
}

bool isPendingFilename(const String& name) {
    // matches "<day>.<digits>.up.csv"
    return name.endsWith(".up.csv");
}

bool isDayFilename(const String& name) {
    // matches "<day>.csv" only (no .up.). Note ".up.csv" also ends with ".csv"
    // so we explicitly exclude pending-upload files here.
    if (!name.endsWith(".csv")) return false;
    if (isPendingFilename(name)) return false;
    return true;
}

String dayIdFromFilename(const String& name) {
    // "<day>.csv"                 -> <day>
    // "<day>.<bootMs>.up.csv"     -> <day>
    int firstDot = name.indexOf('.');
    if (firstDot <= 0) return String();
    return name.substring(0, firstDot);
}
} // namespace

bool SessionStore::begin() {
    fs_ = nullptr;
    backend_ = Backend::None;
    cardSizeMb_ = 0;
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();

    // ---- Try SD first ------------------------------------------------------
    if (cfg::SD_ENABLED) {
        // ===== Preflight: SdFat (greiman) ===================================
        // SdFat ships its own SPI driver that bypasses ESP-IDF's sd_diskio
        // entirely. The stock arduino-esp32 SD driver has a long history of
        // 'no token received' regressions (see espressif/arduino-esp32#6081
        // and schreibfaul1/ESP32-audioI2S#245). On real-world testing both
        // SD and SD_MMC fail on cards that SdFat handles fine, even with
        // identical wiring and good 5V power. So SdFat is now the primary
        // backend - if it mounts we adopt it directly and skip the broken
        // stock drivers entirely.
        Serial.printf("[SD] trying SdFat on SCK=%u MISO=%u MOSI=%u CS=%u\n",
                      cfg::SD_SCK_PIN, cfg::SD_MISO_PIN, cfg::SD_MOSI_PIN,
                      cfg::SD_CS_PIN);
        pinMode(cfg::SD_MISO_PIN, INPUT_PULLUP);
        sdBitBangWakeup(cfg::SD_SCK_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
        gSdSpi.begin(cfg::SD_SCK_PIN, cfg::SD_MISO_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
        pinMode(cfg::SD_CS_PIN, OUTPUT);
        digitalWrite(cfg::SD_CS_PIN, HIGH);
        delay(50);
        // Try a few clocks. SdFat takes &gSdSpi so it shares our bus.
        // Once mounted we leave it at the mount clock; session writes are
        // tiny so there's nothing to gain from a faster bus.
        const uint32_t kSdFatClocks[] = { SD_SCK_MHZ(8), SD_SCK_MHZ(4),
                                          SD_SCK_MHZ(1), SD_SCK_HZ(400000) };
        // On battery cold-boot the HW-125 LDO sometimes needs more than 50ms
        // to ramp before the card responds. Retry the entire clock sweep a
        // few times with a short gap so we don't give up too early.
        for (uint8_t attempt = 0; attempt < cfg::SD_INIT_RETRIES; ++attempt) {
            if (attempt > 0) {
                Serial.printf("[SdFat] retry %u/%u after %ums\n",
                              (unsigned)attempt,
                              (unsigned)(cfg::SD_INIT_RETRIES - 1),
                              (unsigned)cfg::SD_INIT_RETRY_GAP_MS);
                delay(cfg::SD_INIT_RETRY_GAP_MS);
                sdBitBangWakeup(cfg::SD_SCK_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
            }
            for (uint32_t hz : kSdFatClocks) {
                SdSpiConfig spiCfg(cfg::SD_CS_PIN, SHARED_SPI, hz, &gSdSpi);
                if (gSdFat.begin(spiCfg)) {
                    uint64_t cardSizeBytes =
                        (uint64_t)gSdFat.card()->sectorCount() * 512ULL;
                    cardSizeMb_ = cardSizeBytes / (1024ULL * 1024ULL);
                    Serial.printf("[SdFat] mounted at %u Hz attempt=%u: size=%lluMB fatType=%u\n",
                                  (unsigned)hz, (unsigned)attempt,
                                  (unsigned long long)cardSizeMb_,
                                  (unsigned)gSdFat.fatType());
                    sdFatPreflightOk_ = true;
                    backend_ = Backend::SdFat;
                    fs_ = nullptr;  // SdFat doesn't expose fs::FS
                    if (!gSdFat.exists(cfg::SESSIONS_DIR)) {
                        gSdFat.mkdir(cfg::SESSIONS_DIR);
                    }
                    return true;
                }
                Serial.printf("[SdFat] %u Hz failed: errCode=0x%02X errData=0x%02X (attempt %u)\n",
                              (unsigned)hz, gSdFat.sdErrorCode(), gSdFat.sdErrorData(),
                              (unsigned)attempt);
            }
        }
        Serial.println("[SdFat] mount FAILED across all clocks and retries - "
                       "card not responding on this bus.");

        // ===== Attempt 2: SD_MMC peripheral, 1-bit mode =====================
        // The S3 has a dedicated SDMMC controller that is *not* SPI. It uses
        // a different driver, different DMA path, and different pin signaling
        // than the SD-over-SPI library. Many cards that fail in SPI mode work
        // here because the SDMMC peripheral's clock/timing is more forgiving.
        // 1-bit mode needs only CLK, CMD, D0 -- we map them onto the same
        // physical wires the user already soldered for SPI:
        //   SPI SCK  (GPIO 5) -> MMC CLK
        //   SPI MOSI (GPIO 6) -> MMC CMD
        //   SPI MISO (GPIO 4) -> MMC D0
        // SPI CS    (GPIO 7) is unused by MMC; we tie it HIGH so the card
        // sees a quiet line on that side.
        Serial.printf("[SD] trying SD_MMC 1-bit: CLK=%u CMD=%u D0=%u\n",
                      cfg::SD_SCK_PIN, cfg::SD_MOSI_PIN, cfg::SD_MISO_PIN);
        pinMode(cfg::SD_CS_PIN, OUTPUT);
        digitalWrite(cfg::SD_CS_PIN, HIGH);
        // setPins on ESP32-S3 takes (clk, cmd, d0) for 1-bit mode.
        if (SD_MMC.setPins(cfg::SD_SCK_PIN, cfg::SD_MOSI_PIN, cfg::SD_MISO_PIN)) {
            // mode=true selects 1-bit, format_if_mount_failed=true,
            // max_files=5, frequency=BOARD_MAX_SDMMC_FREQ (auto).
            if (SD_MMC.begin("/sdmmc", true, true, SDMMC_FREQ_DEFAULT, 5)) {
                uint8_t cardType = SD_MMC.cardType();
                const char* typeName = "UNKNOWN";
                switch (cardType) {
                    case CARD_NONE:  typeName = "NONE";  break;
                    case CARD_MMC:   typeName = "MMC";   break;
                    case CARD_SD:    typeName = "SD";    break;
                    case CARD_SDHC:  typeName = "SDHC";  break;
                }
                if (cardType != CARD_NONE) {
                    cardSizeMb_ = SD_MMC.cardSize() / (1024ULL * 1024ULL);
                    Serial.printf("[SD_MMC] mounted: type=%s size=%lluMB\n",
                                  typeName, (unsigned long long)cardSizeMb_);
                    fs_ = &SD_MMC;
                    backend_ = Backend::Sd;
                    if (!SD_MMC.exists(cfg::SESSIONS_DIR)) {
                        SD_MMC.mkdir(cfg::SESSIONS_DIR);
                    }
                    return true;
                }
                Serial.println("[SD_MMC] CARD_NONE after mount, ending");
                SD_MMC.end();
            } else {
                Serial.println("[SD_MMC] begin() failed, falling through to SPI");
            }
        } else {
            Serial.println("[SD_MMC] setPins() failed, falling through to SPI");
        }

        // ===== Attempt 3: SD over SPI =======================================
        Serial.printf("[SD] trying SPI: SCK=%u MISO=%u MOSI=%u CS=%u (init@1MHz)\n",
                      cfg::SD_SCK_PIN, cfg::SD_MISO_PIN, cfg::SD_MOSI_PIN,
                      cfg::SD_CS_PIN);
        // Cheap HW-125 modules + jumper wires often need an explicit pullup
        // on MISO so the line doesn't float when the card hasn't asserted it.
        pinMode(cfg::SD_MISO_PIN, INPUT_PULLUP);
        // Spec-mandated SPI-mode power-up sequence: 74+ clocks with CS HIGH.
        sdBitBangWakeup(cfg::SD_SCK_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
        gSdSpi.begin(cfg::SD_SCK_PIN, cfg::SD_MISO_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
        // Drive CS high before the first transaction so the card sees a clean
        // CS edge on its very first command.
        pinMode(cfg::SD_CS_PIN, OUTPUT);
        digitalWrite(cfg::SD_CS_PIN, HIGH);
        delay(50);  // Let Vdd settle and card finish its internal init.

        // Start at a conservative clock; jumper-wire setups rarely tolerate
        // 20MHz on first contact. We can raise it once mounted if we want.
        // Each attempt requires SD.end() between calls because the FATFS
        // driver caches state from failed mounts. Re-issue the wakeup
        // sequence between attempts to give the card a clean re-init.
        auto tryMount = [&](uint32_t hz, bool formatIfEmpty) {
            SD.end();
            delay(50);
            sdBitBangWakeup(cfg::SD_SCK_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
            gSdSpi.begin(cfg::SD_SCK_PIN, cfg::SD_MISO_PIN, cfg::SD_MOSI_PIN, cfg::SD_CS_PIN);
            return SD.begin(cfg::SD_CS_PIN, gSdSpi, hz, "/sd", 5, formatIfEmpty);
        };
        bool mounted = tryMount(1000000, false);
        if (!mounted) {
            Serial.println("[SD] 1MHz init failed, retrying at 400kHz");
            mounted = tryMount(400000, false);
        }
        if (!mounted) {
            // Card may respond at SPI level but have an unreadable filesystem
            // (unformatted, exFAT on SDXC, corrupted). Let FATFS reformat.
            Serial.println("[SD] still failed, retrying at 400kHz with format-if-empty");
            mounted = tryMount(400000, true);
        }
        if (!mounted) {
            Serial.println("[SD] retrying at 1MHz with format-if-empty");
            mounted = tryMount(1000000, true);
        }
        if (!mounted) {
            Serial.println("[SD] retrying at 4MHz with format-if-empty");
            mounted = tryMount(4000000, true);
        }
        if (mounted) {
            uint8_t cardType = SD.cardType();
            const char* typeName = "UNKNOWN";
            switch (cardType) {
                case CARD_NONE:  typeName = "NONE";  break;
                case CARD_MMC:   typeName = "MMC";   break;
                case CARD_SD:    typeName = "SD";    break;
                case CARD_SDHC:  typeName = "SDHC";  break;
            }
            cardSizeMb_ = SD.cardSize() / (1024ULL * 1024ULL);
            Serial.printf("[SD] mounted: type=%s size=%lluMB total=%lluMB used=%lluMB\n",
                          typeName, (unsigned long long)cardSizeMb_,
                          (unsigned long long)(SD.totalBytes()  / (1024ULL*1024ULL)),
                          (unsigned long long)(SD.usedBytes()   / (1024ULL*1024ULL)));
            if (cardType == CARD_NONE) {
                Serial.println("[SD] CARD_NONE after mount, falling back to LittleFS");
                SD.end();
                gSdSpi.end();
            } else {
                fs_ = &SD;
                backend_ = Backend::Sd;
                if (!SD.exists(cfg::SESSIONS_DIR)) {
                    SD.mkdir(cfg::SESSIONS_DIR);
                }
                return true;
            }
        } else {
            Serial.println("[SD] mount failed; falling back to LittleFS");
            gSdSpi.end();
        }
    }

    // ---- SD required gate -------------------------------------------------
    // If the user requires SD (default), refuse to silently fall through to
    // the on-chip 1.5MB LittleFS partition. They want a clear failure state
    // they can react to (reboot / reseat the card) rather than logging
    // hours of field data to a tiny internal partition by accident.
    if (cfg::SD_ENABLED && cfg::SD_REQUIRED) {
        Serial.println("[STORE] FATAL: SD_REQUIRED=true and SD did not mount. "
                       "Recording will stay disabled until reboot.");
        backend_ = Backend::Failed;
        fs_ = nullptr;
        return false;
    }

    // ---- Fallback: LittleFS -----------------------------------------------
    // Our partition table labels the LittleFS partition "littlefs" (subtype
    // "spiffs" because LittleFS reuses the SPIFFS subtype on ESP-IDF). The
    // Arduino LittleFS wrapper defaults to label "spiffs" — pass our actual
    // label so the mount succeeds.
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        log_e("LittleFS mount failed even after format");
        return false;
    }
    if (!LittleFS.exists(cfg::SESSIONS_DIR)) {
        LittleFS.mkdir(cfg::SESSIONS_DIR);
    }
    fs_ = &LittleFS;
    backend_ = Backend::LittleFs;
    return true;
}

const char* SessionStore::backendName() const {
    switch (backend_) {
        case Backend::Sd:       return "SD";
        case Backend::SdFat:    return "SdFat";
        case Backend::LittleFs: return "LittleFS";
        case Backend::Failed:   return "FAILED";
        default:                return "none";
    }
}


// =============================================================================
// Locking helper: hold mutex_ for the duration of a scope. Safe to construct
// even before begin() runs (mutex_ may be null) -- becomes a no-op in that
// case so unit tests / cold paths don't crash.
// =============================================================================
namespace {
struct Lock {
    SemaphoreHandle_t s_;
    explicit Lock(SemaphoreHandle_t s) : s_(s) {
        // v0.6.0: bounded wait (5 s) instead of portMAX_DELAY. If we genuinely
        // deadlock, the task watchdog will reboot us cleanly in <30 s --
        // better than blocking forever. The warning log makes any real
        // contention visible.
        if (s_) {
            if (xSemaphoreTake(s_, pdMS_TO_TICKS(5000)) != pdTRUE) {
                log_w("SessionStore::Lock: timeout waiting 5s for mutex; proceeding without it");
                s_ = nullptr;  // skip Give in dtor
            }
        }
    }
    ~Lock() { if (s_) xSemaphoreGive(s_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
} // namespace

// =============================================================================
// Public: day-id derivation (local Eastern time)
// =============================================================================
String SessionStore::dayIdFromEpochMs(uint64_t epochMs) {
    constexpr uint64_t MIN_VALID_TS_MS = 1577836800000ULL;
    if (epochMs < MIN_VALID_TS_MS) return String();
    time_t t = (time_t)(epochMs / 1000ULL);
    struct tm tmv;
    // localtime_r honours the TZ env var set in setup() to cfg::LOCAL_TZ
    // ("EST5EDT,M3.2.0,M11.1.0"). On ESP32 newlib supports the POSIX TZ
    // string fully including the DST start/end rules so this is correct
    // year-round without any NTP roundtrip.
    localtime_r(&t, &tmv);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return String(buf);
}

// =============================================================================
// Internal helpers (called with mutex_ held)
// =============================================================================

// Create or reopen <dayId>.csv and recompute sampleCount_. Returns true if
// the file is ready for appending.
bool SessionStore::openDayFile_(const String& dayId) {
    activeId_   = dayId;
    sampleCount_ = 0;
    String path = pathFor(dayId);

    if (backend_ == Backend::SdFat) {
        bool existed = gSdFat.exists(path.c_str());
        FsFile f = gSdFat.open(path.c_str(),
                               existed ? (O_RDWR | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC));
        if (!f) {
            log_e("openDayFile_: open failed for %s", path.c_str());
            activeId_ = "";
            recording_ = false;
            return false;
        }
        if (!existed) {
            f.println("timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,speedKph,bearingDeg,altitudeM,hdop");
        }
        f.close();
        if (existed) {
            FsFile data = gSdFat.open(path.c_str(), O_RDONLY);
            if (data) {
                uint8_t buf[256];
                int n;
                while ((n = data.read(buf, sizeof(buf))) > 0) {
                    for (int i = 0; i < n; ++i) if (buf[i] == '\n') ++sampleCount_;
                }
                if (sampleCount_ > 0) --sampleCount_;
                data.close();
            }
        }
        recording_ = true;
        Serial.printf("[REC] open day file: %s existed=%d samples=%u\n",
                      dayId.c_str(), (int)existed, (unsigned)sampleCount_);
        return true;
    }

    if (!fs_) { activeId_ = ""; recording_ = false; return false; }
    bool existed = fs_->exists(path);
    File f = fs_->open(path, existed ? "a" : "w", true);
    if (!f) {
        log_e("openDayFile_: open failed for %s", path.c_str());
        activeId_ = "";
        recording_ = false;
        return false;
    }
    if (!existed) {
        f.println(F("timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,speedKph,bearingDeg,altitudeM,hdop"));
    }
    f.close();
    if (existed) {
        // Pre-count rows so the UI sample counter reflects pre-existing data.
        File data = fs_->open(path, "r");
        if (data) {
            uint8_t buf[256];
            size_t n;
            while ((n = data.read(buf, sizeof(buf))) > 0) {
                for (size_t i = 0; i < n; ++i) if (buf[i] == '\n') ++sampleCount_;
            }
            if (sampleCount_ > 0) --sampleCount_;
            data.close();
        }
    }
    recording_ = true;
    Serial.printf("[REC] open day file: %s existed=%d samples=%u backend=%s\n",
                  dayId.c_str(), (int)existed, (unsigned)sampleCount_, backendName());
    return true;
}

// Rename the currently-open <activeId>.csv to <activeId>.<millis>.up.csv.
// Resets recording_ / activeId_ / sampleCount_. Returns true if a rename
// actually happened.
bool SessionStore::rotateActiveToPending_() {
    if (!recording_ || !activeId_.length()) return false;
    String oldName = activeId_ + ".csv";
    String newName = pendingFilename(activeId_, millis());
    String oldPath = String(cfg::SESSIONS_DIR) + "/" + oldName;
    String newPath = String(cfg::SESSIONS_DIR) + "/" + newName;

    bool ok = false;
    if (backend_ == Backend::SdFat) {
        ok = gSdFat.rename(oldPath.c_str(), newPath.c_str());
    } else if (fs_) {
        ok = fs_->rename(oldPath, newPath);
    }
    if (ok) {
        Serial.printf("[REC] rotate: %s -> %s (samples=%u)\n",
                      oldName.c_str(), newName.c_str(), (unsigned)sampleCount_);
    } else {
        Serial.printf("[REC] rotate FAILED: %s -> %s\n",
                      oldPath.c_str(), newPath.c_str());
    }
    recording_   = false;
    activeId_    = "";
    sampleCount_ = 0;
    return ok;
}

// Rename any non-today <day>.csv files to pending-upload state. Useful at
// boot after a power-cycle and at every upload cycle to make sure stale
// daily files don't accumulate. Returns count of files rotated.
uint32_t SessionStore::rotateStaleDayFiles_() {
    // "today" is computed from current best-known epoch ms. If we haven't
    // acquired UTC yet, todayId is empty and EVERY day file is considered
    // stale, which is the correct behaviour: we'd rather upload pre-reboot
    // data eagerly than risk overwriting it once UTC arrives.
    String todayId;
    {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            todayId = dayIdFromEpochMs((uint64_t)now * 1000ULL);
        }
    }

    std::vector<String> toRotate;
    if (backend_ == Backend::SdFat) {
        FsFile dir = gSdFat.open(cfg::SESSIONS_DIR, O_RDONLY);
        if (!dir || !dir.isDir()) return 0;
        FsFile child;
        while (child.openNext(&dir, O_RDONLY)) {
            if (!child.isDir()) {
                char nameBuf[64];
                child.getName(nameBuf, sizeof(nameBuf));
                String name(nameBuf);
                if (isDayFilename(name)) {
                    String dayId = dayIdFromFilename(name);
                    if (todayId.length() == 0 || dayId != todayId) {
                        toRotate.push_back(name);
                    }
                }
            }
            child.close();
        }
    } else if (fs_) {
        File dir = fs_->open(cfg::SESSIONS_DIR);
        if (!dir || !dir.isDirectory()) return 0;
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String name = fileBaseName(String(f.name()));
                if (isDayFilename(name)) {
                    String dayId = dayIdFromFilename(name);
                    if (todayId.length() == 0 || dayId != todayId) {
                        toRotate.push_back(name);
                    }
                }
            }
            f = dir.openNextFile();
        }
    }

    uint32_t rotated = 0;
    uint32_t seq = millis();
    for (const auto& name : toRotate) {
        String dayId = dayIdFromFilename(name);
        String newName = pendingFilename(dayId, seq++);
        String oldPath = String(cfg::SESSIONS_DIR) + "/" + name;
        String newPath = String(cfg::SESSIONS_DIR) + "/" + newName;
        bool ok;
        if (backend_ == Backend::SdFat) {
            ok = gSdFat.rename(oldPath.c_str(), newPath.c_str());
        } else {
            ok = fs_->rename(oldPath, newPath);
        }
        if (ok) {
            Serial.printf("[REC] rotate stale: %s -> %s\n",
                          name.c_str(), newName.c_str());
            ++rotated;
        }
    }
    return rotated;
}

// =============================================================================
// Public: resume + append + rotate
// =============================================================================

bool SessionStore::resumeIfActive() {
    if (!hasUsableBackend()) return false;
    Lock lk(mutex_);

    // No legacy /active.txt marker any more -- day file naming is canonical.
    // If today's day file exists we reopen it; any other stale day files
    // get rotated to pending-upload state so the next upload cycle picks
    // them up.
    rotateStaleDayFiles_();

    // Best effort: only open today's file if we already know today's date.
    // First append() will (re-)open it once GPS UTC anchors otherwise.
    time_t now = time(nullptr);
    if (now > 1700000000) {
        String today = dayIdFromEpochMs((uint64_t)now * 1000ULL);
        if (today.length() == 10) {
            String path = pathFor(today);
            bool exists = (backend_ == Backend::SdFat) ? gSdFat.exists(path.c_str())
                                                       : (fs_ && fs_->exists(path));
            if (exists) {
                openDayFile_(today);
                return true;
            }
        }
    }
    return false;
}

void SessionStore::append(uint32_t /*tsLow*/, uint64_t timestampMsFull,
                          float uSvPerHour, float cps,
                          bool hasGps, double lat, double lng,
                          const String& deviceId,
                          float speedKph, float bearingDeg,
                          float altitudeM, float hdop) {
    if (!hasUsableBackend()) return;

    // ---- Always-on contract gates ----------------------------------------
    // No GPS fix => sample is discarded entirely. The device's purpose is
    // geo-tagged radiation logging; rows without coordinates have no value
    // and would pad the database with noise.
    if (!hasGps) {
        static uint32_t skippedNoGps = 0;
        if ((++skippedNoGps % 60) == 1) {
            Serial.printf("[REC] skip: no GPS fix (skipped=%u)\n",
                          (unsigned)skippedNoGps);
        }
        return;
    }
    constexpr uint64_t MIN_VALID_TS_MS = 1577836800000ULL;
    if (timestampMsFull < MIN_VALID_TS_MS) return;

    String day = dayIdFromEpochMs(timestampMsFull);
    if (day.length() != 10) return;

    Lock lk(mutex_);
    event_log::markPhase("ST_APPEND");

    // ---- Auto-rotate on day rollover / first sample ---------------------
    if (!recording_ || activeId_ != day) {
        if (recording_ && activeId_.length() && activeId_ != day) {
            // Day boundary crossed mid-trip. Rotate previous day's file
            // immediately so the uploader can post it without waiting.
            event_log::markPhase("ST_ROTATE_DAY");
            rotateActiveToPending_();
        }
        event_log::markPhase("ST_OPEN_DAY");
        if (!openDayFile_(day)) {
            event_log::markPhase("ST_OPEN_FAIL");
            return;
        }
    }

    // ---- Format the CSV row ---------------------------------------------
    char spd[12] = "", brg[12] = "", alt[12] = "", hdp[12] = "";
    if (speedKph   >= 0.f)     snprintf(spd, sizeof(spd), "%.2f", speedKph);
    if (bearingDeg >= 0.f)     snprintf(brg, sizeof(brg), "%.1f", bearingDeg);
    if (altitudeM  > -9000.f)  snprintf(alt, sizeof(alt), "%.1f", altitudeM);
    if (hdop       >= 0.f)     snprintf(hdp, sizeof(hdp), "%.2f", hdop);

    char line[cfg::MAX_LINE_BYTES];
    int len = snprintf(line, sizeof(line), "%llu,%.6f,%.3f,%.7f,%.7f,%s,%s,%s,%s,%s\n",
                       (unsigned long long)timestampMsFull,
                       uSvPerHour, cps, lat, lng, deviceId.c_str(),
                       spd, brg, alt, hdp);
    if (len <= 0) return;

    String path = pathFor(activeId_);
    if (backend_ == Backend::SdFat) {
        FsFile f = gSdFat.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
        if (!f) { log_w("append: open failed"); return; }
        f.write((const uint8_t*)line, (size_t)len);
        f.close();
        ++sampleCount_;
        ++lifetimeSamples_;
        return;
    }
    if (!fs_) return;
    event_log::markPhase("ST_OPEN_APPEND");
    File f = fs_->open(path, "a");
    if (!f) { log_w("append: open failed"); event_log::markPhase("ST_OPEN_FAIL2"); return; }
    event_log::markPhase("ST_WRITE");
    size_t written = f.print(line);
    event_log::markPhase("ST_CLOSE");
    f.close();
    event_log::markPhase("ST_DONE");
    if ((int)written < len) {
        Serial.printf("[REC] WRITE ERR: tried %d bytes wrote %u heap=%u\n",
                      len, (unsigned)written, (unsigned)ESP.getFreeHeap());
        return;
    }
    ++sampleCount_;
    ++lifetimeSamples_;
    if ((sampleCount_ % 100) == 0) {
        Serial.printf("[REC] %u samples written today=%s heap=%u\n",
                      (unsigned)sampleCount_, activeId_.c_str(),
                      (unsigned)ESP.getFreeHeap());
    }
}

// =============================================================================
// Public: storage stats
// =============================================================================

size_t SessionStore::totalBytes() const {
    if (backend_ == Backend::Sd)       return (size_t)std::min<uint64_t>(SD.totalBytes(),       SIZE_MAX);
    if (backend_ == Backend::SdFat)    return (size_t)std::min<uint64_t>(cardSizeMb_ * 1024ULL * 1024ULL, SIZE_MAX);
    if (backend_ == Backend::LittleFs) return LittleFS.totalBytes();
    return 0;
}

size_t SessionStore::usedBytes() const {
    if (backend_ == Backend::Sd)       return (size_t)std::min<uint64_t>(SD.usedBytes(), SIZE_MAX);
    if (backend_ == Backend::SdFat)    return 0;     // SdFat freeClusterCount() is slow
    if (backend_ == Backend::LittleFs) return LittleFS.usedBytes();
    return 0;
}

int SessionStore::percentUsed() const {
    const size_t t = totalBytes();
    if (!t) return 0;
    return (int)((usedBytes() * 100ULL) / t);
}

int SessionStore::sessionCount() const {
    if (backend_ == Backend::SdFat) {
        FsFile dir = gSdFat.open(cfg::SESSIONS_DIR, O_RDONLY);
        if (!dir || !dir.isDir()) return 0;
        int n = 0;
        FsFile child;
        while (child.openNext(&dir, O_RDONLY)) {
            if (!child.isDir()) ++n;
            child.close();
        }
        return n;
    }
    if (!fs_) return 0;
    File dir = fs_->open(cfg::SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) ++n;
        f = dir.openNextFile();
    }
    return n;
}

// =============================================================================
// Public: upload integration
// =============================================================================

uint32_t SessionStore::rotateForUpload() {
    if (!hasUsableBackend()) return 0;
    Lock lk(mutex_);
    event_log::markPhase("ST_ROT_FOR_UP");
    if (recording_ && sampleCount_ > 0) {
        rotateActiveToPending_();
    }
    rotateStaleDayFiles_();
    event_log::markPhase("ST_ROT_DONE");
    return (uint32_t)listPendingUploads().size();
}

std::vector<SessionStore::PendingUpload> SessionStore::listPendingUploads() const {
    std::vector<PendingUpload> out;
    if (backend_ == Backend::SdFat) {
        FsFile dir = gSdFat.open(cfg::SESSIONS_DIR, O_RDONLY);
        if (!dir || !dir.isDir()) return out;
        FsFile child;
        while (child.openNext(&dir, O_RDONLY)) {
            if (!child.isDir()) {
                char nameBuf[64];
                child.getName(nameBuf, sizeof(nameBuf));
                String name(nameBuf);
                if (isPendingFilename(name)) {
                    PendingUpload p;
                    p.filename  = name;
                    p.sessionId = dayIdFromFilename(name);
                    p.sizeBytes = (size_t)child.size();
                    out.push_back(p);
                }
            }
            child.close();
        }
        return out;
    }
    if (!fs_) return out;
    File dir = fs_->open(cfg::SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return out;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = fileBaseName(String(f.name()));
            if (isPendingFilename(name)) {
                PendingUpload p;
                p.filename  = name;
                p.sessionId = dayIdFromFilename(name);
                p.sizeBytes = (size_t)f.size();
                out.push_back(p);
            }
        }
        f = dir.openNextFile();
    }
    return out;
}

bool SessionStore::removePendingUpload(const String& filename) {
    if (filename.length() == 0 || !hasUsableBackend()) return false;
    if (!isPendingFilename(filename)) return false;
    String path = String(cfg::SESSIONS_DIR) + "/" + filename;
    if (backend_ == Backend::SdFat) {
        return gSdFat.remove(path.c_str());
    }
    if (!fs_) return false;
    return fs_->remove(path);
}

Stream* SessionStore::openPendingUploadStream(const String& filename, size_t& outSizeBytes) {
    if ((backend_ == Backend::LittleFs || backend_ == Backend::Sd) && fs_) {
        String path = String(cfg::SESSIONS_DIR) + "/" + filename;
        openedStreamFile_ = fs_->open(path, "r");
        if (!openedStreamFile_) {
            log_w("openPendingUploadStream: open failed for %s", filename.c_str());
            return nullptr;
        }
        outSizeBytes = (size_t)openedStreamFile_.size();
        return &openedStreamFile_;
    }
    return nullptr;
}

void SessionStore::closeSessionStream() {
    if (openedStreamFile_) openedStreamFile_.close();
}

bool SessionStore::readPendingUploadToString(const String& filename, size_t maxBytes, String& out) const {
    if (!hasUsableBackend()) return false;
    String path = String(cfg::SESSIONS_DIR) + "/" + filename;
    if (backend_ == Backend::SdFat) {
        FsFile f = gSdFat.open(path.c_str(), O_RDONLY);
        if (!f) return false;
        size_t sz = (size_t)f.size();
        if (sz > maxBytes) { f.close(); return false; }
        out.reserve(sz);
        uint8_t buf[256];
        int n;
        while ((n = f.read(buf, sizeof(buf))) > 0) {
            for (int i = 0; i < n; ++i) out += (char)buf[i];
        }
        f.close();
        return true;
    }
    if (!fs_) return false;
    File f = fs_->open(path, "r");
    if (!f) return false;
    if (f.size() > maxBytes) { f.close(); return false; }
    out.reserve((size_t)f.size());
    uint8_t buf[256];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        for (size_t i = 0; i < n; ++i) out += (char)buf[i];
    }
    f.close();
    return true;
}

// =============================================================================
// Public: diagnostic listing / dumping / wiping
// =============================================================================

std::vector<SessionStore::SessionInfo> SessionStore::listSessions() const {
    std::vector<SessionInfo> out;
    if (backend_ == Backend::SdFat) {
        FsFile dir = gSdFat.open(cfg::SESSIONS_DIR, O_RDONLY);
        if (!dir || !dir.isDir()) return out;
        FsFile child;
        while (child.openNext(&dir, O_RDONLY)) {
            if (!child.isDir()) {
                char nameBuf[64];
                child.getName(nameBuf, sizeof(nameBuf));
                String name(nameBuf);
                if (name.endsWith(".csv")) {
                    SessionInfo info;
                    info.id        = stripCsvSuffix(name);
                    info.sizeBytes = (size_t)child.size();
                    info.samples   = 0;
                    uint8_t buf[256];
                    int n;
                    child.seek(0);
                    while ((n = child.read(buf, sizeof(buf))) > 0) {
                        for (int i = 0; i < n; ++i) if (buf[i] == '\n') ++info.samples;
                    }
                    if (info.samples > 0) --info.samples;
                    out.push_back(info);
                }
            }
            child.close();
        }
        return out;
    }
    if (!fs_) return out;
    File dir = fs_->open(cfg::SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return out;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = fileBaseName(String(f.name()));
            if (name.endsWith(".csv")) {
                SessionInfo info;
                info.id        = stripCsvSuffix(name);
                info.sizeBytes = f.size();
                info.samples   = 0;
                if (info.id == activeId_) {
                    // Active day file: use in-memory count instead of reading
                    // through the global LittleFS mutex.
                    info.samples = sampleCount_;
                } else {
                    File data = fs_->open(String(cfg::SESSIONS_DIR) + "/" + name, "r");
                    if (data) {
                        uint8_t buf[256];
                        size_t n;
                        while ((n = data.read(buf, sizeof(buf))) > 0) {
                            for (size_t i = 0; i < n; ++i) if (buf[i] == '\n') ++info.samples;
                        }
                        if (info.samples > 0) --info.samples;
                        data.close();
                    }
                }
                out.push_back(info);
            }
        }
        f = dir.openNextFile();
    }
    return out;
}

bool SessionStore::dumpSession(const String& id, Stream& out) const {
    if (!hasUsableBackend()) {
        out.printf("[DUMP-ERR] id=%s reason=no-backend\n", id.c_str());
        return false;
    }
    String path = String(cfg::SESSIONS_DIR) + "/" + id + ".csv";
    if (backend_ == Backend::SdFat) {
        FsFile f = gSdFat.open(path.c_str(), O_RDONLY);
        if (!f) {
            out.printf("[DUMP-ERR] id=%s reason=open-failed\n", id.c_str());
            return false;
        }
        uint32_t bytes = (uint32_t)f.size();
        uint32_t samples = 0;
        {
            FsFile counter = gSdFat.open(path.c_str(), O_RDONLY);
            if (counter) {
                uint8_t cbuf[256];
                int n;
                while ((n = counter.read(cbuf, sizeof(cbuf))) > 0) {
                    for (int i = 0; i < n; ++i) if (cbuf[i] == '\n') ++samples;
                }
                if (samples > 0) --samples;
                counter.close();
            }
        }
        out.printf("[DUMP-BEGIN] id=%s bytes=%u samples=%u\n",
                   id.c_str(), (unsigned)bytes, (unsigned)samples);
        uint8_t buf[256];
        int n;
        while ((n = f.read(buf, sizeof(buf))) > 0) {
            out.write(buf, (size_t)n);
            yield();
        }
        f.close();
        out.print('\n');
        out.printf("[DUMP-END] id=%s\n", id.c_str());
        return true;
    }
    if (!fs_) {
        out.printf("[DUMP-ERR] id=%s reason=no-backend\n", id.c_str());
        return false;
    }
    File f = fs_->open(path, "r");
    if (!f) {
        out.printf("[DUMP-ERR] id=%s reason=open-failed\n", id.c_str());
        return false;
    }
    uint32_t samples = 0;
    File counter = fs_->open(path, "r");
    if (counter) {
        uint8_t cbuf[256];
        size_t cn;
        while ((cn = counter.read(cbuf, sizeof(cbuf))) > 0) {
            for (size_t ci = 0; ci < cn; ++ci) if (cbuf[ci] == '\n') ++samples;
        }
        if (samples > 0) --samples;
        counter.close();
    }
    out.printf("[DUMP-BEGIN] id=%s bytes=%u samples=%u\n",
               id.c_str(), (unsigned)f.size(), (unsigned)samples);
    uint8_t buf[256];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n > 0) out.write(buf, n);
        yield();
    }
    f.close();
    out.print('\n');
    out.printf("[DUMP-END] id=%s\n", id.c_str());
    return true;
}

void SessionStore::dumpAll(Stream& out) const {
    auto sessions = listSessions();
    out.printf("[DUMP-ALL-BEGIN] count=%u\n", (unsigned)sessions.size());
    uint32_t ok = 0;
    for (const auto& s : sessions) {
        if (dumpSession(s.id, out)) ++ok;
    }
    out.printf("[DUMP-DONE] ok=%u total=%u\n", (unsigned)ok, (unsigned)sessions.size());
}

uint32_t SessionStore::wipeAll() {
    if (!hasUsableBackend()) return 0;
    Lock lk(mutex_);
    recording_   = false;
    activeId_    = "";
    sampleCount_ = 0;

    if (backend_ == Backend::SdFat) {
        FsFile dir = gSdFat.open(cfg::SESSIONS_DIR, O_RDONLY);
        if (!dir || !dir.isDir()) return 0;
        std::vector<String> paths;
        FsFile child;
        while (child.openNext(&dir, O_RDONLY)) {
            if (!child.isDir()) {
                char nameBuf[64];
                child.getName(nameBuf, sizeof(nameBuf));
                paths.push_back(String(cfg::SESSIONS_DIR) + "/" + String(nameBuf));
            }
            child.close();
        }
        uint32_t removed = 0;
        for (const auto& p : paths) {
            if (gSdFat.remove(p.c_str())) ++removed;
        }
        return removed;
    }
    if (!fs_) return 0;
    uint32_t removed = 0;
    File dir = fs_->open(cfg::SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    std::vector<String> paths;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            paths.push_back(String(cfg::SESSIONS_DIR) + "/" + fileBaseName(String(f.name())));
        }
        f = dir.openNextFile();
    }
    for (const auto& p : paths) {
        if (fs_->remove(p)) ++removed;
    }
    return removed;
}

bool SessionStore::removeSession(const String& id) {
    if (id.length() == 0 || !hasUsableBackend()) return false;
    if (recording_ && activeId_ == id) return false;
    String path = String(cfg::SESSIONS_DIR) + "/" + id + ".csv";
    if (backend_ == Backend::SdFat) return gSdFat.remove(path.c_str());
    if (!fs_) return false;
    return fs_->remove(path);
}

bool SessionStore::readSessionToString(const String& id, size_t maxBytes, String& out) const {
    if (!hasUsableBackend()) return false;
    String path = String(cfg::SESSIONS_DIR) + "/" + id + ".csv";
    if (backend_ == Backend::SdFat) {
        FsFile f = gSdFat.open(path.c_str(), O_RDONLY);
        if (!f) return false;
        size_t sz = (size_t)f.size();
        if (sz > maxBytes) { f.close(); return false; }
        out.reserve(sz);
        uint8_t buf[256];
        int n;
        while ((n = f.read(buf, sizeof(buf))) > 0) {
            for (int i = 0; i < n; ++i) out += (char)buf[i];
        }
        f.close();
        return true;
    }
    if (!fs_) return false;
    File f = fs_->open(path, "r");
    if (!f) return false;
    if (f.size() > maxBytes) { f.close(); return false; }
    out.reserve((size_t)f.size());
    uint8_t buf[256];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        for (size_t i = 0; i < n; ++i) out += (char)buf[i];
    }
    f.close();
    return true;
}
