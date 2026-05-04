# Heltec Tracker — Agent Operating Instructions

Full technical reference for AI agents working in this repo.
Read this **before** making any changes.

This repo is self-contained: firmware, ingest API, and web viewer all live here.

---

## Session Branch Management (MANDATORY — follow before anything else)

A **session** maps 1-to-1 with a Copilot Chat conversation. Every session must
have its own dedicated git branch. These rules act as lifecycle hooks since
Copilot has no native pre/post-turn hooks.

### Session start (do this first, every session)

1. Run `git branch --show-current`.
2. If on **`main`** (or `master`), create and switch to a session branch:
   ```powershell
   git checkout -b copilot/session-YYYYMMDD-HHMMSS
   ```
   Use the actual current local date-time. Example: `copilot/session-20260502-143000`
3. If already on a `copilot/session-*` branch, stay — do not create a second one.
4. Tell the user the active branch name.
5. **Review AGENTS.md and `.github/copilot-instructions.md`** at the start of
   every session. Both documents must be kept in sync with the codebase.
   After making any changes during the session, update both files to reflect
   what changed.

### After every turn that modifies files

Run this immediately after all edits are done in the turn:

```powershell
git add -A
git commit -m "<type>: <short description>"
git push --set-upstream origin HEAD
```

Commit types: `feat`, `fix`, `refactor`, `docs`, `chore`.

**Never skip this commit and push.** If nothing changed, skip silently.

### Session wrap-up (when user says "done", "wrap up", "merge")

1. Show a commit summary for the session branch vs main.
2. Ask whether to merge/squash into main or leave the branch open.
3. Never merge without explicit user confirmation.

---

## Repository Structure

```
heltec-tracker/                   <- repo root (was heltec_tracker/ in monorepo)
├── .gitignore
├── AGENTS.md                     # this file
├── RADIACODE_PROTOCOL.md         # full BLE/GATT protocol reference for RadiaCode devices
├── README.md                     # quick-start for humans
├── platformio.ini                # PlatformIO build config
├── partitions_tracker_v2.csv     # custom flash partition table
├── src/                          # ESP32-S3 firmware (C++)
│   ├── main.cpp                  # setup() / loop(), serial REPL
│   ├── config.h                  # ALL pin assignments + feature flags
│   ├── secrets.h                 # gitignored — WiFi/URL credentials
│   ├── secrets.h.example         # template for secrets.h
│   ├── button.{h,cpp}            # debounced GPIO 0 + long-press engine
│   ├── gps_module.{h,cpp}        # UC6580 GNSS: auto-baud, bestEpochMs()
│   ├── radiacode.{h,cpp}         # NimBLE central + RadiaCode protocol
│   ├── session_store.{h,cpp}     # SD/SdFat CSV writer + LittleFS fallback
│   ├── ui.{h,cpp}                # ST7735 TFT screens + button state machine
│   └── wifi_uploader.{h,cpp}     # FreeRTOS uploader task (core 0)
├── scripts/                      # Python dev-tools (serial, data, mapping)
│   ├── drive.py                  # serial console wrapper: cmd/listen/auto-connect
│   ├── download_sessions.py      # DUMPALL -> CSV files + optional wipe
│   ├── plot_session_map.py       # interactive Folium map from CSV
│   ├── overnight_watch.py        # log-tailing watchdog with reconnect
│   └── capture_boot.py           # trigger ESP32 reset via DTR/RTS and capture boot log
├── api/
│   └── vega-tracker-ingest/      # Radiological Map Ingest API — FastAPI service (Docker)
│       ├── .env                  # real credentials — gitignored
│       ├── .env.example          # template
│       ├── tracker_ingest_api.py # main FastAPI app
│       ├── deploy.ps1            # deploy to remote server over SSH
│       ├── docker-compose.yml
│       ├── Dockerfile
│       ├── requirements.txt
│       └── client_sample.py
└── web/
    └── vega-tracker-viewer/      # Radiological Map Viewer — React + Vite (Docker/nginx)
        ├── .env                  # real credentials — gitignored
        ├── .env.example          # template
        ├── src/                  # React source
        ├── deploy.ps1            # deploy to remote server over SSH
        ├── docker-compose.yml
        ├── Dockerfile
        ├── nginx.conf
        └── package.json
```

---

## Server & Credentials

### Target server

| Item                  | Value                               |
|-----------------------|-------------------------------------|
| Server IP             | 192.168.86.48                       |
| SSH user              | darkmatter2222                      |
| SSH key               | ~/.ssh/id_rsa                       |
| API port              | 8030 (Radiological Map Ingest)      |
| Viewer port           | 8031 (Radiological Map Viewer)      |
| MongoDB port          | 27017 (host-native, not in Docker)  |
| MongoDB auth source   | admin                               |

### MongoDB connection

```
mongodb://ryan:Welcome123%21@host.docker.internal:27017/?authSource=admin
```

- User: `ryan`
- Password: `Welcome123!` (URL-encoded as `Welcome123%21`)
- Database: `radiacode`
- Collections: `tracker_samples`, `tracker_sessions`

Credentials are stored in:
- `api/vega-tracker-ingest/.env` (for the ingest API Docker container)
- Credentials are gitignored — copy from `.env.example` on a new machine.

### The .env files (gitignored, but committed credentials are below for setup)

`api/vega-tracker-ingest/.env`:
```
SSH_USER=darkmatter2222
SSH_HOST=192.168.86.48
SSH_KEY_PATH=~/.ssh/id_rsa
REMOTE_PATH=/home/darkmatter2222/vega-tracker-ingest
API_PORT=8030
MONGO_URI=mongodb://ryan:Welcome123%21@host.docker.internal:27017/?authSource=admin
MONGO_DB=radiacode
MONGO_SAMPLES_COLLECTION=tracker_samples
MONGO_SESSIONS_COLLECTION=tracker_sessions
```

`web/vega-tracker-viewer/.env`:
```
SSH_USER=darkmatter2222
SSH_HOST=192.168.86.48
SSH_KEY_PATH=~/.ssh/id_rsa
REMOTE_PATH=/home/darkmatter2222/vega-tracker-viewer
WEB_PORT=8031
API_BASE=http://192.168.86.48:8030
```

---

## Hardware

### Board: Heltec HTIT-Tracker V2

- **MCU**: ESP32-S3FN8 + SX1262 LoRa + UC6580 GNSS + ST7735 0.96" TFT (160x80)
- **PlatformIO env**: always `heltec_tracker_v2` — NEVER `heltec_tracker_v1_2`
  (V1.2 env inverts the display; produces a solid white screen on V2 hardware)
- **Panel offsets**: `XSTART=0 / YSTART=24`, `invertDisplay(false)`
- **Upload port**: COM4 (this dev machine); auto-detected by PlatformIO
- **Flash partition**: `partitions_tracker_v2.csv`

### GPIO assignments (do NOT reuse these)

| Pin(s)         | Function                                  |
|----------------|-------------------------------------------|
| 38,39,40,41,42 | TFT CS/DC/RST/SCLK/MOSI                  |
| 21             | TFT backlight (HIGH = on)                 |
| 33, 34         | UC6580 GNSS UART RX/TX (UART1)            |
| 3              | VTFT/VGNSS rail enable (HIGH = on)        |
| 2              | VBAT divider enable (HIGH only on sample) |
| 1              | VBAT ADC1_CH0                             |
| 0              | USER button (active LOW)                  |
| 19, 20         | USB D+/D-                                 |
| 4,5,6,7        | SD MISO/SCK/MOSI/CS (FSPI/SPI2)          |
| Internal       | SX1262 LoRa SPI                           |

### SD card: HiLetgo HW-125

**Critical**: wire VCC to Heltec 5V, NOT 3V3. The onboard AMS1117-3.3V LDO
needs at least 4.4 V input. At 3V3 the card gets ~2 V and will not respond.
This burned a full debug session — see Lessons Learned.

| HW-125 | Heltec | Notes              |
|--------|--------|--------------------|
| GND    | GND    |                    |
| VCC    | 5V     | feeds onboard LDO  |
| MISO   | GPIO 4 |                    |
| MOSI   | GPIO 6 |                    |
| SCK    | GPIO 5 |                    |
| CS     | GPIO 7 |                    |

---

## Firmware Build & Flash

```powershell
# Always run from the repo root
cd <repo-root>

# Build
pio run -e heltec_tracker_v2

# Flash (device on COM4)
pio run -e heltec_tracker_v2 -t upload

# Flash with explicit port
pio run -e heltec_tracker_v2 -t upload --upload-port COM4

# Capture boot sequence (native USB-CDC needs this helper)
python capture_boot.py

# Live serial tail
python scripts\drive.py listen 30
```

**Firmware version**: tracked in `src/config.h` as `FW_VERSION`.
Current: `0.3.3`.

---

## Secrets (Firmware)

`src/secrets.h` is gitignored. Create it from `src/secrets.h.example`:

```cpp
namespace secrets {
constexpr const char* WIFI_SSID        = "YourNetwork";
constexpr const char* WIFI_PASSWORD    = "YourPassword";
constexpr const char* INGEST_URL       = "http://192.168.86.48:8030/ingest/csv";
constexpr uint32_t    UPLOAD_INTERVAL_MS = 60000;
}
```

Empty `WIFI_SSID` or `INGEST_URL` disables the Wi-Fi uploader silently.

---

## Firmware Subsystems

### BLE / RadiaCode — `radiacode.{h,cpp}`

> **Full BLE protocol reference** (GATT profile, frame format, all commands,
> DATA_BUF decoder, VS/VSFR tables, model quirks): see **[RADIACODE_PROTOCOL.md](RADIACODE_PROTOCOL.md)**

- NimBLE-Arduino 1.4.x; service UUID `e63215e5-7003-49d8-96b0-b024798fb901`
- States: `Idle → Scanning → Connecting → Initializing → Ready → Disconnected`
- **Critical build flags** (in `platformio.ini`) for RC-110 BT5 extended advertising:
  - `CONFIG_BT_NIMBLE_EXT_ADV=1`
  - `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=0`
  - `CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE=255`
  - `CONFIG_BT_CTRL_SCAN_DUPL_TYPE_DATA_DEVICE=1`
  - `CONFIG_BT_CTRL_SCAN_DUPL_TYPE=2`
- Pinned-target address (NVS key `pinned_peer`) prevents reconnect to wrong peer
- Name auto-grab pattern (NVS key `grab_pat`) connects immediately to any peer whose name matches
- Call `secureConnection()` immediately after connect for RC-110

### GPS — `gps_module.{h,cpp}`

- UC6580 on UART1 (RX=33, TX=34); auto-baud sweep 115200→9600→38400→57600
- **`bestEpochMs()`**: anchors UTC+millis pair on each GPS fix, advances
  monotonically through GPS outages so timestamps never stall or duplicate
- Samples skipped until `bestEpochMs() >= MIN_VALID_TS_MS` (2020-01-01)
  to prevent `millis()`-since-boot from poisoning session timestamps
- Serial log: `[GPS] UTC anchor set: ...` / `[GPS] UTC anchor refreshed: ... (drift Xms)`

### Session Storage — `session_store.{h,cpp}`

- Primary: SdFat on FSPI (GPIO 4-7)
- Fallback: LittleFS (5.9 MB partition) — currently disabled
  (`cfg::SD_REQUIRED = true`), failure is a hard error on screen
- CSV schema: `timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,speedKph,bearingDeg,altitudeM,hdop`
- Sessions are append-only; `removeSession()` refuses to delete active session
- Serial log on start/stop: `[REC] START: id=...` / `[REC] STOP: id=... samples=N`

### TFT UI — `ui.{h,cpp}`

- ST7735 landscape rotation=1; 160×80; colors: GREEN `#00E676`, RED, DIM_GREY
- Screens cycled by short-press: STATS → GPS → STORAGE → PICKER (long-press)
- **Stop-recording requires DOUBLE long-press** (added v0.2.0):
  - First long-press on STORAGE while recording: shows red "HOLD AGAIN: STOP REC"
  - Second long-press within 5 seconds: stops recording
  - Single press, short press, or 5-second timeout cancels — recording continues
  - Starting recording still requires only one long-press (no confirmation)
- This prevents accidental stop from road vibration bumping the button

### Wi-Fi Uploader — `wifi_uploader.{h,cpp}`

- FreeRTOS task pinned to core 0 (keeps BLE/GPS on core 1 uninterrupted)
- Uploads completed sessions via `POST /ingest/csv` every 60 seconds
- Deletes session file from SD after successful 2xx response
- Headers: `X-Session-Id`, `X-Device-Id`, `X-Tracker-Id`, `X-Firmware`

---

## Serial Console Reference

Connection: 115200 baud, USB-CDC. Type `?` for the live list on device.

| Command           | Effect |
|-------------------|--------|
| `?`               | command list |
| `s [secs]`        | BLE scan (default 15s; `RADIACODE_SCAN_MS` only governs auto-reconnect) |
| `l`               | list scan results (index, address, addrType, RSSI, name) |
| `c <idx>`         | connect to scan result by index |
| `c <addr> <type>` | connect to raw BLE address with type (0=public, 1=random) |
| `x`               | cancel scan / disconnect |
| `f`               | forget saved peer + trigger immediate rescan |
| `D`               | disconnect, keep pinned peer |
| `t <pattern>`     | set auto-grab name pattern (e.g. `t RadiaCode`); bare `t` clears |
| `g`               | GPS quick status (baud, fix, sats, hdop, age) |
| `LS`              | list sessions (id, bytes, samples) |
| `DUMP <id>`       | stream one session CSV to serial |
| `DUMPALL`         | stream all session CSVs |
| `WIPE <count>`    | delete all sessions; count must match `LS` output (safety guard) |
| `STATFS`          | filesystem usage (bytes used/total, session count) |
| `SDSTAT`          | SD / LittleFS backend status |
| `GPASSTHRU [s]`   | pipe raw GPS NMEA to serial (default 10s) |
| `GREBAUD`         | re-probe GPS baud rates |
| `SYNC`            | force immediate Wi-Fi upload cycle |
| `WIFISTAT`        | Wi-Fi uploader diagnostics (enabled, busy, counts, last HTTP status) |

Commands that **do not exist** (do not add them):
- `HB` — heartbeat is automatic every 3s, no serial trigger
- `START` / `STOP` — recording is button-only, no serial toggle
- `RM <id>` — `removeSession()` is internal, not serial-exposed
- `WIPE` (no count) — prints usage; count arg is mandatory

Heartbeat format (every 3 s):
```
[HB] uptime=Xs fix=N sats=N hdop=H gpsB=N gpsAge=Xms baud=N rcState=N rec=0/1 samples=N
```

---

## Deploy Ingest API (vega-tracker-ingest)

Branded: **Radiological Map Ingest API**. Docker container name is `vega-tracker-ingest` (server-side infrastructure name; not renamed to avoid orphaning running containers).

```powershell
cd api\vega-tracker-ingest
.\deploy.ps1              # copy + build + start container
.\deploy.ps1 -TestOnly    # health check only
.\deploy.ps1 -SkipCopy    # rebuild container without re-copying files
```

What `deploy.ps1` does:
1. Reads `.env` for SSH creds, remote path, port, Mongo URI
2. `rsync`/`scp` source to `darkmatter2222@192.168.86.48:/home/darkmatter2222/vega-tracker-ingest`
3. `docker compose up --build -d` on the remote
4. Polls `GET /health` until healthy

Verify manually:
```powershell
ssh darkmatter2222@192.168.86.48 "curl -s http://localhost:8030/health"
ssh darkmatter2222@192.168.86.48 "curl -s http://localhost:8030/info"
```

### API Endpoints

| Method | Path                       | Description |
|--------|----------------------------|-------------|
| GET    | /health                    | liveness + mongo ping |
| GET    | /info                      | version, collection counts, sample rate |
| GET    | /sessions                  | list sessions; `?include_deleted=true` to include soft-deleted |
| GET    | /session/{id}              | session detail (up to 5000 samples) |
| POST   | /ingest/csv                | upload one session CSV |
| DELETE | /sessions/{id}             | **soft-delete** — sets `deletedAt`/`deletedBy`, not hard-deleted |
| PATCH  | /sessions/{id}/restore     | restore a soft-deleted session |
| POST   | /admin/purge/{id}          | permanent hard-delete; requires session already soft-deleted + `?confirm=PURGE_CONFIRMED` |
| POST   | /admin/recompute-sessions  | purge pre-2020 rows, recompute all session metadata |
| POST   | /admin/backup              | trigger full-db mongodump; `?source=cron\|manual` |
| GET    | /admin/backups             | list backups with source/status/elapsed |
| POST   | /admin/restore/{name}      | restore from a named backup (full mongorestore) |

### Ingest CSV Headers

```
X-Session-Id    required   "boot_362620" or "20260426_104210"
X-Device-Id     optional   RadiaCode BLE MAC without colons
X-Tracker-Id    optional   ESP32 chip ID / MAC
X-Firmware      optional   firmware version string
```

### MIN_VALID_TS_MS

All rows with `timestampMs < 1_577_836_800_000` (2020-01-01 UTC) are rejected
by the API. Old firmware used `millis()`-since-boot (e.g. 1777) as timestamps
before GPS UTC was acquired — these would corrupt session metadata.

### MongoDB Admin

```powershell
# Get a Mongo shell on the server
ssh darkmatter2222@192.168.86.48

# Mongo shell (server-side)
mongosh "mongodb://ryan:Welcome123!@localhost:27017/?authSource=admin"

# Useful queries
use radiacode
db.tracker_sessions.find().sort({firstTsMs:-1}).limit(10).pretty()
db.tracker_samples.countDocuments({sessionId:"boot_362620"})
db.tracker_samples.find({sessionId:"boot_362620"}).sort({timestampMs:1}).limit(5)

# Recompute sessions after data fix
curl -X POST http://192.168.86.48:8030/admin/recompute-sessions
```

---

## Deploy Web Viewer (vega-tracker-viewer)

Branded: **Radiological Map Viewer**. Docker container name is `vega-tracker-viewer` (server-side infrastructure; not renamed to avoid orphaning running containers).

Runtime config is injected at container start via `/docker-entrypoint.d/10-config.sh`, which patches `window.__APP_CONFIG__` in `config.js` with the live `API_BASE` value. The React app reads `window.__APP_CONFIG__.apiBase` (previously `window.__VEGA_CONFIG__` — renamed during rebrand).

```powershell
cd web\vega-tracker-viewer
.\deploy.ps1              # copy + build Docker image + start container
.\deploy.ps1 -TestOnly    # check container health only
.\deploy.ps1 -SkipCopy    # rebuild image without re-copying source
```

The React app is built inside the Docker image at build time (Vite build).
`API_BASE` env var is injected at container runtime via `nginx.conf` + `/docker-entrypoint.d/10-config.sh`,
patching `public/config.js` so the compiled JS references the right API URL.

Viewer URL: `http://192.168.86.48:8031/`

### Viewer Layout — Two Top-Level Modes

The viewer has a persistent top navigation bar with **Explore** and **Data Management** mode buttons.

**Explore mode** (default) — left sidebar + full map:
- Sidebar tabs: Sessions | Display | Stats
- Map modes: Track (colored polyline), Dots (circle markers), Heatmap (native canvas, no plugin), Arrows (bearing arrows + dot underlay)
- Map zoom: `maxZoom=20`; per-tile `maxNativeZoom` (OSM=19, CartoDB=20, OpenTopoMap=17, Esri Satellite=18) for graceful over-zoom
- Color channels: Dose rate, CPS, Speed, Altitude, HDOP, Session index
- Per-mode display controls (Display tab):
  - Track: track width slider; optional dot overlay + dot opacity slider
  - Dots: point radius slider
  - Heatmap: native `L.circleMarker + L.canvas` renderer; no extra controls
  - Arrows: arrow-every-N slider; dot opacity slider; optional track underlay + track opacity slider
- Session timeline scrubber + playback
- Tile layers: OSM Streets, CartoDB Dark (default), OpenTopoMap, Satellite (Esri)

**Data Management mode** — full-width two-column layout, no map:
- **Left panel — Session Management** (`ManagePanel`): Rename, Delete/Restore, Merge, Export sub-tabs; active + soft-deleted sessions; triple-confirm Purge
- **Right panel — Database** (`DatabasePanel`): backup history with source/status badges; manual backup trigger; restore; DB stats

---

## Python Dev Tools (scripts/)

```powershell
# Tail serial for 30 seconds
python scripts\drive.py listen 30

# Send a command and tail output
python scripts\drive.py cmd "LS" --listen 4

# Scan for RadiaCode devices, then auto-connect to the best candidate
python scripts\drive.py auto-connect 18

# Download all sessions from device to local CSVs (default port COM3)
python scripts\download_sessions.py
python scripts\download_sessions.py --no-wipe   # keep files on SD card

# Port note: drive.py defaults to COM4; download_sessions.py defaults to COM3.
# Both are hardcoded. Change the PORT / DEFAULT_PORT constant at the top of
# each script to match your machine, or pass --port on the command line.

# Plot a session on an interactive map
python scripts\plot_session_map.py path\to\session.csv

# Watch overnight logging
python scripts\overnight_watch.py

# Trigger device reset and capture the first 12 seconds of boot output
python scripts\capture_boot.py
```

---

## Data Schema

CSV on device (and what gets POSTed to the API):
```
timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,speedKph,bearingDeg,altitudeM,hdop
1746114660123,0.142,12.0,47.6062,-122.3321,5243066020F4,48.23,267.3,12.4,1.20
```

- `timestampMs`: Unix epoch ms (GPS-derived via `bestEpochMs()`)
- `uSvPerHour`: dose rate in micro-Sieverts per hour (raw from RadiaCode)
- `cps`: counts per second (raw from RadiaCode)
- `latitude` / `longitude`: decimal degrees (empty = no GPS fix)
- `deviceId`: RadiaCode BLE MAC without colons (e.g. `5243066020F4`)
- `speedKph`: GPS speed over ground in km/h (empty if `FIELD_SPEED_KPH = false`)
- `bearingDeg`: calculated bearing in degrees [0, 360), derived from last
  `BEARING_HISTORY_POINTS` GPS positions via forward-azimuth formula
  (empty if `FIELD_BEARING_DEG = false` or fewer than 2 history points)
- `altitudeM`: GPS altitude above MSL in metres (empty if `FIELD_ALTITUDE_M = false`)
- `hdop`: Horizontal Dilution of Precision — positional accuracy indicator;
  lower is better (empty if `FIELD_HDOP = false`)

Columns 6–9 are always present in the header (firmware 0.3.0+) but may be
empty strings if the corresponding config flag is false or data is unavailable.
Pre-0.3.0 uploads have 6 columns; the ingest API handles both formats.

MongoDB stores the same fields plus:
- `sessionId`: from `X-Session-Id` header
- `trackerId`: from `X-Tracker-Id` header
- `firmware`: from `X-Firmware` header
- `loc`: GeoJSON Point `{type:"Point", coordinates:[lng,lat]}` (non-zero GPS only)

---

## Configuration Knobs

All in `src/config.h` under `namespace cfg`. Edit here and recompile.

| Knob                    | V2 default | V1.2 default | Notes |
|-------------------------|------------|--------------|-------|
| `KEEP_UPLOADS_ON_DEVICE`| `false`    | `false`      | Delete session CSV from device after successful upload. Set true to retain files (re-uploads are idempotent via server unique index on {sessionId,timestampMs}); re-upload within same boot is skipped via `uploadedThisBoot_` vector |
| `SD_ENABLED`            | `false`    | `true`       | V2 uses internal LittleFS; SD disabled |
| `SD_REQUIRED`           | `false`    | `true`       | V1.2: hard error on SD failure rather than silent LittleFS fallback |
| `SD_INIT_RETRIES`       | 6          | 6            | cold-boot retries (LDO ramp time) |
| `SD_INIT_RETRY_GAP_MS`  | 250 ms     | 250 ms       | ms between SD init retries |
| `RADIACODE_POLL_MS`     | 1000 ms    | 1000 ms      | ~1 Hz BLE poll; keeps RC-110 link alive |
| `RADIACODE_SCAN_MS`     | 8000 ms    | 8000 ms      | auto-reconnect scan duration (manual `s` default is 15s) |
| `UI_TICK_MS`            | 100 ms     | 100 ms       | TFT redraw cadence |
| `HEARTBEAT_MS`          | 3000 ms    | 3000 ms      | `[HB]` serial heartbeat cadence |
| `FIELD_SPEED_KPH`       | `true`     | `true`       | Write GPS speed (km/h) column to CSV |
| `FIELD_BEARING_DEG`     | `true`     | `true`       | Write smoothed bearing (deg 0–360) column to CSV |
| `FIELD_ALTITUDE_M`      | `true`     | `true`       | Write GPS altitude (m above MSL) column to CSV |
| `FIELD_HDOP`            | `true`     | `true`       | Write HDOP accuracy indicator column to CSV |
| `BEARING_HISTORY_POINTS`| 4          | 4            | GPS history points for bearing calc (2–8); higher = smoother, more lag |

> **Note:** `default_envs = heltec_tracker_v2` — for V2 builds SD is skipped entirely at boot.

---

## Code Style Rules

- **No emojis** — anywhere (comments, serial logs, UI strings, docs)
- No C++14 digit separators (`20'000'000`) — this toolchain rejects them
- Comments explain *why*, not *what*; lean toward "for posterity" on weird knobs
- Serial output is the only debug surface — be loud and unambiguous at boot
- One subsystem per `.h`/`.cpp` pair; keep `main.cpp` thin

---

## Stop Conditions (when to ask the user)

Pause and ask only if:
1. Credentials / secrets are needed that are not in `.env` or `secrets.h`
2. Production impact — live services that receive real device data
3. Destructive operations — session data is append-only, never delete rows from MongoDB
4. Ambiguous requirements

Otherwise, iterate to completion.

---

## Lessons Learned — Do Not Re-Litigate

### BLE — RadiaCode-110

- **Silent connect failures**: RC-110 needs `CONFIG_BT_NIMBLE_EXT_ADV=1`
  (BT5 extended advertising on secondary channels). Without this flag the
  device scans but never connects. See `platformio.ini` build_flags.
- **Disconnect within 750ms**: RC-110 requires `secureConnection()` immediately
  after connect or the peer drops the link.
- **Reconnects to wrong peer**: use NVS-pinned target MAC (`pinned_peer` NVS key).

### GPS Timestamps

- **millis()-since-boot as timestamp**: if GPS UTC has not been acquired yet,
  old code used raw `millis()` (e.g. 177 ms) as the timestamp. One such row
  makes `firstTsMs` look like 1970 and the session span 56 years.
  Fix: `MIN_VALID_TS_MS` gate (2020-01-01) in both firmware and API;
  `bestEpochMs()` projects forward via `millis()` delta until GPS locks.
- **TinyGPS++ latches last fix**: `utcEpochMs()` returns the same frozen
  timestamp when GPS signal is lost indoors. All rows become identical
  timestamps and get deduplicated by the unique index on `{sessionId, timestampMs}`.
  Fix: `bestEpochMs()` advances via `millis()` delta through GPS outages so
  each row gets a unique, monotonically increasing timestamp.

### Extended Telemetry (v0.3.0)

- **Bearing is calculated from a GPS position ring buffer**, not from NMEA COG.
  `BEARING_HISTORY_POINTS` (default 4) controls how many positions are averaged.
  Fewer points = more responsive but noisier; more = smoother but lags turns.
  Returns -1.0 (empty CSV column) until at least 2 positions have been recorded.
- **Column positions 6–9 are always present in firmware 0.3.0+ CSV headers**
  (`speedKph,bearingDeg,altitudeM,hdop`). Disabled fields emit empty strings
  rather than omitting the column, so the schema is positionally stable.
  Pre-0.3.0 files have 6 columns; the ingest API handles both.
- **MongoDB documents are sparse**: extended fields are stored only when non-None
  (i.e. not on old uploads). No schema migration needed; MongoDB is schema-less.

### Double Long-Press Stop (v0.2.0)

- Road vibration while cycling: a bump would short-press to STORAGE screen,
  then a sustained bounce = long-press = recording silently stopped.
  The Wi-Fi uploader uploads and deletes the session file within ~60 seconds.
  Fix: first long-press shows "HOLD AGAIN: STOP REC" confirmation (5-second
  window). Second long-press stops. Any other input or timeout cancels.

### V1.2 vs V2 Panel

- Flashing the `heltec_tracker_v1_2` environment on V2 hardware inverts the
  display and produces a solid white screen. Always use `heltec_tracker_v2`.
  `default_envs = heltec_tracker_v2` is set in `platformio.ini`.

### SD Card Power (the big one)

- HW-125 SD module VCC must be **5V**, not 3V3. The AMS1117 LDO on the module
  has ~1.1V dropout; at 3V3 input the card gets ~2V and will not respond to
  any SPI command. Three different driver implementations all failed for the
  same root cause.
  Fix: one wire moved from the 3V3 pin to the 5V pin.
- Cold-boot intermittence: retry loop (6 attempts, 250ms gap) handles LDO
  power-rail ramp time.
- `SD_REQUIRED=true` makes storage failure a hard, visible error on the TFT
  instead of a silent downgrade to LittleFS.

### Wi-Fi Upload Truncation (v0.3.2)

- **Root cause**: `readSessionToString` loaded the entire CSV into an Arduino
  `String`. On a 320 KB DRAM device a ~2300-row session (~348 KB) exhausted
  the heap mid-String. The String silently truncated to ~750 rows; HTTPClient
  posted the truncated body, received HTTP 200, and the file was deleted.
  1568 rows permanently lost with no log error.
- **Fix**: `HTTPClient::sendRequest("POST", Stream*, size_t)` streams the
  `fs::File` directly through the TCP stack in small chunks — zero heap
  allocation for the body. `SessionStore::openSessionStream()` returns the
  file as a `Stream*`; `closeSessionStream()` closes it after the request.
- **Belt-and-suspenders**: `cfg::KEEP_UPLOADS_ON_DEVICE = true` keeps the file
  on device after a successful upload. `uploadedThisBoot_` vector in
  `WifiUploader` prevents re-uploading within the same boot cycle.
- **Heap logging**: every upload cycle logs `heap_free` before and after so
  future OOM events are immediately visible in the serial log.
- **SdFat fallback preserved**: if `openSessionStream` returns nullptr (SdFat
  backend on V1.2 hardware), the uploader falls back to the String path with a
  1 MB cap.

### Soft-Delete System (API v0.4.0+)

- `DELETE /sessions/{id}` is a **soft-delete**: sets `deletedAt` timestamp and `deletedBy` field; data is preserved.
- `GET /sessions` filters out soft-deleted sessions by default. Pass `?include_deleted=true` to see them.
- `PATCH /sessions/{id}/restore` clears `deletedAt` — session reappears in all views.
- `POST /admin/purge/{id}?confirm=PURGE_CONFIRMED` performs a permanent hard-delete. Requires the session to already be soft-deleted (two-step gate prevents accidental permanent deletion).
- The viewer's Manage tab → Delete/Restore sub-tab has a "Show deleted" toggle, Restore buttons, and a triple-confirm Purge flow.

### Automated Backups

- Weekly cron schedule (`POST /admin/backup?source=cron`) via server cron job.
- 5 rolling snapshots kept; oldest are pruned on each new backup.
- Full-db scope: mongodump of the entire `radiacode` database.
- Backup telemetry written to `tracker_backups` MongoDB collection with `source`, `status`, `elapsedSec`, `sizeBytes`.
- DatabasePanel in the viewer shows backup history with source/status badges and allows manual trigger + restore.


### Heatmap Plugin Removed

- `leaflet.heat` plugin was kept in `package.json` but its canvas lifecycle did not integrate
  cleanly with React's component mount/unmount cycle, producing invisible or stuck layers.
  Replaced with native `L.circleMarker()` + `L.canvas()` renderer inside `HeatmapLayer`
  React component. No plugin dependency needed; colors map to the same green→yellow→orange→red
  gradient via `heatGradientColor()`. `leaflet.heat` import in `main.jsx` is now unused but
  kept to avoid a missing-module build error until the dependency is removed in a future cleanup.

### Runtime Config Global Rename

- `window.__VEGA_CONFIG__` renamed to `window.__APP_CONFIG__` during the rebrand
  (May 2026). Updated in `public/config.js`, `src/api.js`, and `dist/config.js`.
  The nginx entrypoint script patches `config.js` at container start — no rebuild needed
  to change `API_BASE`.
