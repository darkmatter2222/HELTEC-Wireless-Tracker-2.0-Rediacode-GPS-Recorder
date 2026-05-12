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
├── infra/
│   └── duckdns/                  # DuckDNS dynamic DNS — Docker Compose (server-side)
│       ├── .env                  # real token — gitignored, lives only on server
│       ├── .env.example          # template (safe to commit)
│       ├── .gitignore            # ensures .env is never committed
│       └── docker-compose.yml
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

**CRITICAL — SSH authentication**: ALWAYS use `ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48`.
NEVER ask the user for a password. The key `~/.ssh/id_rsa` is already on this machine and works without any password prompt. Do not use `sudo` directly via SSH because the user account does not have NOPASSWD sudo; use sudo only when the user explicitly asks for firewall/system changes.

| Item                  | Value                               |
|-----------------------|-------------------------------------|
| Server IP             | 192.168.86.48                       |
| SSH user              | darkmatter2222                      |
| SSH key               | ~/.ssh/id_rsa  (**use always, never ask for password**) |
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

`infra/duckdns/.env` (lives only on server at `~/docker/duckdns/.env` — never in repo):
```
DUCKDNS_SUBDOMAINS=susmannet
DUCKDNS_TOKEN=<token — see DuckDNS account page>
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
Current: `0.4.8`.

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
  - Second long-press within 10 seconds (v0.3.4+): stops recording
  - Single press, short press, or timeout cancels — recording continues
  - Starting recording still requires only one long-press (no confirmation)
- This prevents accidental stop from road vibration bumping the button
- **Header status bar** (v0.3.5): RC state badge (GREEN=OK, AMBER=scanning/init, RED=disconnected);
  GPS badge (GREEN=3D fix, RED=no fix); battery with color threshold; recording dot always visible
  (dim outline = idle, filled red = recording)
- **STATS screen footer** (v0.3.5): Shows GPS positional accuracy `+/- X.Xm  hdop Y.Y` (green)
  when RC connected and GPS fix; `GPS: searching...` (amber) when no fix; `Hold: pick RC` when disconnected.
  Replaces the old RC-address + `*noGPS` footer.
- **GPS screen footer** (v0.3.5): Shows smoothed bearing `Hdg NNN deg` when fix is locked (same
  bearing logged to CSV via `bearingFromHistory()`); `Fix OK` if bearing not yet computed; reverts
  to `Acquiring fix outdoors` only when there is no GPS fix.

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

## DuckDNS Dynamic DNS (infra/duckdns)

Keeps `susmannet.duckdns.org` pointed at the home server's current public IPv4 address.
Fully automated — no manual intervention ever needed.

| Item              | Value                                          |
|-------------------|------------------------------------------------|
| Container name    | `duckdns`                                      |
| Image             | `lscr.io/linuxserver/duckdns:latest`           |
| Subdomain         | `susmannet` → `susmannet.duckdns.org`          |
| Update interval   | every 5 minutes (built into linuxserver image) |
| Restart policy    | `unless-stopped` (survives reboots)            |
| Server path       | `~/docker/duckdns/`                            |
| Timezone          | America/New_York                               |
| Ports exposed     | none                                           |

The `.env` file with the real token lives **only on the server** at
`~/docker/duckdns/.env`. It is never in the repo. The repo contains only
`infra/duckdns/.env.example` as a safe template.

### Public URLs

| URL                                      | Description                       |
|------------------------------------------|-----------------------------------|
| `https://susmannet.duckdns.org/tracker/` | Radiological Map Viewer           |
| `https://susmannet.duckdns.org/api/`     | Radiological Map Ingest API       |
| `https://susmannet.duckdns.org/`         | 302 redirect → `/tracker/`        |

Traffic flows: browser → router (443 forwarded) → `susman-ingress` nginx → upstream service.
Local direct access (192.168.86.48:8031 / 8030) is preserved for firmware and LAN tools.

### Hairpin NAT — accessing DuckDNS URL from the same LAN

The router does **not** support hairpin NAT (loopback to its own public IP from inside the LAN).
This means `susmannet.duckdns.org` cannot be reached by devices on the same local network
(`192.168.86.x`) unless the DNS for that name is overridden to point at the LAN IP.

**Workaround: add a Windows hosts file entry on your dev machine** (run once in Admin PowerShell):
```powershell
Add-Content 'C:\Windows\System32\drivers\etc\hosts' "`n192.168.86.48`tsusmannet.duckdns.org"
ipconfig /flushdns
```
After adding this entry, `https://susmannet.duckdns.org/tracker/` works from your dev machine.
External devices (phone on cellular, any other network) work without this entry.

The server itself is confirmed reachable externally — check nginx logs for external IPs:
```powershell
ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48 "docker logs susman-ingress 2>&1 | grep susmannet | grep -v '172\.' | tail -10"
```

### susman-ingress nginx integration

`susmannet.duckdns.org` is served by the **existing `susman-ingress`** container
(`~/docucraft/docker-compose.prod.yml`). The nginx template at
`~/docucraft/nginx/nginx.conf.template` was extended with:
- HTTP server block (port 80): ACME challenge passthrough + redirect to HTTPS
- HTTPS server block (port 443):
  - `/tracker/` → `http://192.168.86.48:8031/` (vega-tracker-viewer, prefix stripped)
  - `/api/` → `http://192.168.86.48:8030/` (vega-tracker-ingest, prefix stripped)
  - `location = /` → 302 redirect to `/tracker/`
  - **HTTP Basic Auth** on the entire domain (`auth_basic "Radiological Map"`)

SSL: Let's Encrypt cert issued via HTTP-01 webroot challenge using the shared
`docucraft_susman-certbot-www` volume. Cert path in the shared `docucraft_susman-certs`
volume: `/etc/nginx/certs/live/susmannet.duckdns.org/`. Auto-renewed by the
`docucraft-certbot-renew-1` container (every 12 h, `certbot renew`).

The viewer's `API_BASE` on the server (`~/vega-tracker-viewer/.env`) is set to
`/api` (relative path). This means the browser resolves API calls against its own
origin — works for both the public DuckDNS URL and direct LAN IP access.

### HTTP Basic Auth (susmannet.duckdns.org)

All routes on `susmannet.duckdns.org` (tracker, API, root redirect) require
HTTP Basic Auth. The firmware uploads directly to `http://192.168.86.48:8030`
(bypasses the ingress proxy entirely) and are NOT affected.

**Auth is enforced at two layers:**
1. **`susman-ingress`** (port 443) — protects all public HTTPS traffic
2. **`vega-tracker-viewer`** (port 8031) — also enforces auth on direct LAN access

Both use the same credentials, generated identically via `openssl passwd -apr1`.

**How it works:**
- `entrypoint.sh` (susman-ingress) and `10-config.sh` (viewer) each read
  `TRACKER_USER` and `TRACKER_PASS` env vars at container startup.
  `openssl passwd -apr1` generates an Apache-compatible MD5-crypt hash and
  writes it to `/etc/nginx/tracker_htpasswd` (mode 644).
- If either var is unset, a dummy `disabled:!` entry is written — nginx starts
  but no password will ever match (effectively locks the site).
- `htpasswd` file permissions must be **644** (not 600): nginx worker process
  runs as the `nginx` user inside the container, not as root.
- The viewer's `Dockerfile` includes `RUN apk add --no-cache openssl` to ensure
  openssl is available in the `nginx:alpine` runtime stage.

**Credentials are stored in `~/docucraft/.env` on the server** (gitignored):
```
TRACKER_USER=<username>
TRACKER_PASS=<password>
```
The `docker-compose.prod.yml` passes them through:
```yaml
environment:
  - DOMAIN=${DOMAIN:-docucraft.hobbytimewith.me}
  - TRACKER_USER=${TRACKER_USER:-}
  - TRACKER_PASS=${TRACKER_PASS:-}
```

The viewer's `~/vega-tracker-viewer/.env` also holds the credentials:
```
TRACKER_USER=<username>
TRACKER_PASS=<password>
```
And `~/vega-tracker-viewer/docker-compose.yml` passes them through:
```yaml
environment:
  TRACKER_USER: ${TRACKER_USER:-}
  TRACKER_PASS: ${TRACKER_PASS:-}
```

**To change credentials:**
```powershell
# Edit on server
ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48
# Update TRACKER_USER / TRACKER_PASS in both:
#   ~/docucraft/.env  (susman-ingress)
#   ~/vega-tracker-viewer/.env  (viewer direct)
# Then restart both:
cd ~/docucraft && docker compose -f docker-compose.prod.yml up -d susman-ingress
cd ~/vega-tracker-viewer && docker compose up -d
```

**To verify auth:**
```powershell
# Public HTTPS proxy (should return 401 then 200)
ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48 "curl -sk -o /dev/null -w '%{http_code}' -H 'Host: susmannet.duckdns.org' https://localhost/tracker/"
ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48 "curl -sk -o /dev/null -w '%{http_code}' -u 'USER:PASS' -H 'Host: susmannet.duckdns.org' https://localhost/tracker/"
# Direct LAN viewer port (should also return 401 then 200)
ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48 "curl -s -o /dev/null -w '%{http_code}' http://localhost:8031/tracker/"
ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48 "curl -s -o /dev/null -w '%{http_code}' -u 'USER:PASS' http://localhost:8031/tracker/"
```

### Initial deploy / re-deploy

```powershell
# Copy compose file to server
scp infra/duckdns/docker-compose.yml darkmatter2222@192.168.86.48:~/docker/duckdns/

# Write .env on server (replace token as needed)
ssh darkmatter2222@192.168.86.48 "printf 'DUCKDNS_SUBDOMAINS=susmannet\nDUCKDNS_TOKEN=<your-token>\n' > ~/docker/duckdns/.env"

# Start container
ssh darkmatter2222@192.168.86.48 "cd ~/docker/duckdns && docker compose up -d"
```

### Verify

```powershell
# Check container is running
ssh darkmatter2222@192.168.86.48 "docker ps --filter name=duckdns"

# Check logs (should show "successful" within first update cycle)
ssh darkmatter2222@192.168.86.48 "docker logs duckdns 2>&1 | tail -5"

# Manual API hit to confirm token is valid
ssh darkmatter2222@192.168.86.48 "curl -s 'https://www.duckdns.org/update?domains=susmannet&token=<your-token>&ip='"
# Expected: OK

# DNS resolution
ssh darkmatter2222@192.168.86.48 "nslookup susmannet.duckdns.org"
```

### Notes

- Do not include `UPDATE_IP=ipv4` unless auto-detection fails; omitting it lets
  DuckDNS detect the public IPv4 itself.
- A `502 Bad Gateway` in the logs on first startup is a transient DuckDNS server
  blip — not a config error. The 5-minute retry cycle resolves it automatically.
  Verify with `curl` if you see it.

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
| POST   | /admin/migrate-to-daily-sessions | one-shot v0.5.0 migration: rekey samples to local-eastern `YYYY-MM-DD` (requires `?confirm=MIGRATE_CONFIRMED`) |
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
`vite.config.js` has `base: '/tracker/'` so all compiled asset paths are prefixed with
`/tracker/`. This matches the `susman-ingress` nginx proxy which strips that prefix before
forwarding to the container on port 8031 — so the container still serves at its own root.
`API_BASE` env var is injected at container runtime via `nginx.conf` + `/docker-entrypoint.d/10-config.sh`,
patching `public/config.js` so the compiled JS references the right API URL.

Viewer URL (LAN direct): `http://192.168.86.48:8031/`
Viewer URL (public): `https://susmannet.duckdns.org/tracker/`

### Viewer Layout — Two Top-Level Modes

The viewer has a persistent top navigation bar with **Explore** and **Data Management** mode buttons.

**Explore mode** (default) — left sidebar + full map:
- Sidebar tabs: Sessions | Display | Stats
- Map modes: Track (colored polyline), Dots (circle markers), Hex (hex-bin canvas), Arrows (bearing arrows + dot underlay)
- Map zoom: `maxZoom=22`; per-tile `maxNativeZoom` (OSM=19, CartoDB=20, OpenTopoMap=17, Esri Satellite=18) for graceful over-zoom; initial zoom=6
- Color channels: Dose rate, CPS, Speed, Altitude, HDOP, Session index
- Per-mode display controls (Display tab) — rendered as **ctrl-cards** (label + accent value + range slider) and **toggle-pills** (iOS-style switch):
  - Track: track width ctrl-card; dot overlay toggle-pill + dot opacity ctrl-card
  - Dots: point radius ctrl-card
  - Hex: **Hex bin level** ctrl-card with auto-follow toggle — slider (1–22) controls the geographic resolution of bins independently of map zoom; the slider auto-tracks map zoom but can be dragged to any level for coarser/finer density overlay; "auto" badge glows green when tracking
  - Arrows: arrow-every-N ctrl-card; dot opacity ctrl-card; track underlay toggle-pill + track opacity ctrl-card
- Session timeline scrubber + playback
- Tile layers: OSM Streets, CartoDB Dark (default), OpenTopoMap, Satellite (Esri)

**Data Management mode** — full-width two-column layout, no map:
- **Left panel — Session Management** (`ManagePanel`): Rename, Delete/Restore, Merge, Export sub-tabs; active + soft-deleted sessions; triple-confirm Purge
- **Right panel — Database** (`DatabasePanel`): backup history with source/status badges; manual backup trigger; restore; DB stats

---

## Testing

### Native unit tests (runs on host PC — no hardware required)

Three test suites live under `test/`:

| Suite                    | Tests | What it validates |
|--------------------------|-------|-------------------|
| `test_line_count_native` | 10    | Buffered newline-count algorithm (the O(1) fix from v0.3.4) |
| `test_battery_native`    | 11    | LiPo voltage-to-percent interpolation table |
| `test_csv_schema_native` | 15    | MIN_VALID_TS_MS gate, 10-column schema, field extraction |

**Prerequisites on Windows**: PlatformIO's native env calls `gcc`/`g++`/`ar` which
are not in PATH by default. If VS Build Tools 2022 is installed, create one-time
batch wrappers in any temp dir and prepend that dir to PATH:

```powershell
# Run once per shell session (or add to profile)
$wrapDir = "$env:TEMP\pio_gcc_wrap"
New-Item -ItemType Directory -Force -Path $wrapDir | Out-Null
$llvm = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin"
Set-Content "$wrapDir\gcc.bat"  "@echo off`r`n`"$llvm\clang.exe`" %*"   -Encoding Ascii
Set-Content "$wrapDir\g++.bat"  "@echo off`r`n`"$llvm\clang++.exe`" %*" -Encoding Ascii
Set-Content "$wrapDir\ar.bat"   "@echo off`r`n`"$llvm\llvm-ar.exe`" %*" -Encoding Ascii
$env:PATH = "$wrapDir;$env:PATH"
```

Then run all three host-side suites:

```powershell
pio test -e native
# Expected: 36 test cases: 36 succeeded
```

### Integration tests (requires device on serial port)

```powershell
# Device must be connected to USB-CDC before running.
python scripts\test_device.py                 # defaults to COM4
python scripts\test_device.py --port COM3
```

Tests heartbeat format/cadence, every REPL command timing, sample-count
consistency, and 30-second serial latency loop (the non-blocking BLE regression).

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
| `LOCAL_TZ`              | `EST5EDT,M3.2.0,M11.1.0` | same | POSIX TZ string used by `tzset()` at boot; controls the local-eastern YYYY-MM-DD day boundary used for file rotation |
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

### Viewer Deploy Wiped Basic-Auth Credentials Because Local .env and docker-compose.yml Were Missing TRACKER_USER / TRACKER_PASS (viewer May 2026)

- **Symptom**: after running `web\vega-tracker-viewer\deploy.ps1`, every request
  to `http://192.168.86.48:8031/` returned `401 Unauthorized`. nginx access log
  showed `user "darkmatter2222" was not found in "/etc/nginx/tracker_htpasswd"`.
  The viewer was working before the deploy. Public `https://susmannet.duckdns.org/tracker/`
  was unaffected because `susman-ingress` enforces its own basic auth in front.
- **Root cause**: `deploy.ps1` scp's the local `.env` to
  `~/vega-tracker-viewer/.env` on every deploy. The local copy in this repo was
  missing `TRACKER_USER` and `TRACKER_PASS`. The local `docker-compose.yml`
  was also missing the pass-through `environment:` lines for those vars.
  Result: server `.env` got overwritten with no creds, compose started the
  container without the env vars, and `docker-entrypoint.d/10-config.sh` hit
  its `WARNING: TRACKER_USER or TRACKER_PASS not set — auth disabled` branch
  which writes `disabled:!` to the htpasswd file. nginx then rejected every
  request including the correct username.
- **Why it was latent**: someone had previously edited the server-side `.env`
  by hand to add the creds. That hand-edit silently survived several deploys
  back when `.env` was either not in the deploy items list or had a stale
  manual override. The next clean deploy clobbered it.
- **Fix (viewer May 2026)**:
  - Added `TRACKER_USER=darkmatter2222` and `TRACKER_PASS=liquimatter` to the
    local `web/vega-tracker-viewer/.env` (gitignored, so credentials still
    don't enter the repo).
  - Added `TRACKER_USER: ${TRACKER_USER:-}` and `TRACKER_PASS: ${TRACKER_PASS:-}`
    to the `environment:` block of `web/vega-tracker-viewer/docker-compose.yml`.
  - **Also fixed a CRLF bug** discovered while debugging this: the build was
    successfully copying `docker-entrypoint.d/10-config.sh` into the image
    but Git/scp from Windows preserved `\r\n` line endings on the shebang
    line, so `/bin/sh\r` was not found and the container crash-looped.
    Added `RUN sed -i 's/\r$//' /docker-entrypoint.d/10-config.sh` to the
    Dockerfile right before `chmod +x` — Windows checkouts now self-heal.
- **Verification after re-deploy**:
  ```
  noauth=401   (correct: nginx demands credentials)
  auth=200     (correct: credentials accepted)
  ```
- **Rule for future viewer edits**:
  1. **Never remove `TRACKER_USER` / `TRACKER_PASS` from the local `.env`** —
     `deploy.ps1` will silently break public access on the next push.
  2. Treat the `environment:` block in `docker-compose.yml` as a contract
     with `10-config.sh`. Any new env var the entrypoint reads must be added
     to compose's `environment:` *and* documented in `.env.example`.
  3. Shell scripts copied from Windows need a CRLF strip step in the Dockerfile.

### Mongo Credentials Leaked Through `/info` Response and Container Logs (API v0.5.2)

- **Symptom**: `GET /info` (reachable through the viewer proxy at
  `https://susmannet.duckdns.org/api/info` once HTTP basic auth succeeds, and
  directly on the LAN at `http://192.168.86.48:8030/info`) returned the full
  MongoDB connection string verbatim, including the URL-encoded password:
  ```
  "uri": "mongodb://ryan:Welcome123%21@host.docker.internal:27017/?authSource=admin"
  ```
  The same string was also written to the container's stdout at startup by
  `log.info("connecting to mongo at %s ...", MONGO_URI, ...)`, so anyone with
  access to `docker logs vega-tracker-ingest` could see it.
- **Why it's a problem**: defense-in-depth failure. HTTP basic auth only
  guards the public DuckDNS path. Anyone on the LAN can hit `:8030/info`
  unauthenticated; anyone with shell access to the server can `docker logs`;
  any future endpoint that bypasses the proxy would also leak. Putting
  passwords in API responses is a textbook OWASP A02 (Cryptographic Failures)
  / A09 (Logging Failures) finding.
- **Fix (API v0.5.2)** in `api/vega-tracker-ingest/tracker_ingest_api.py`:
  - Added `_redact_mongo_uri(uri)` helper that splits on `://` and `@`, finds
    the `user:password` segment, and replaces the password with `***`.
    Implementation does not use a regex so it's safe against malformed input
    and never raises (falls back to `"<redacted>"` on any exception).
  - Replaced `"uri": MONGO_URI` in the `/info` response with
    `"uri": _redact_mongo_uri(MONGO_URI)`.
  - Replaced `MONGO_URI` in the startup `log.info(...)` call with
    `_redact_mongo_uri(MONGO_URI)`.
  - The `MongoClient(MONGO_URI, ...)` call and the two `--uri=...` arguments
    passed to `mongodump` / `mongorestore` subprocesses (lines 113, 1117,
    1187) are left as-is — those need the real credentials, and they don't
    cross any trust boundary.
- **Verification after deploy**:
  ```
  $ curl -s http://192.168.86.48:8030/info
  {"version":"0.5.2","mongo":{"uri":"mongodb://ryan:***@host.docker.internal:27017/?authSource=admin", ...}}
  ```
- **Rule for future API edits**: never return `MONGO_URI` (or any other
  credentialed connection string) in an API response, regardless of which
  auth layer sits in front of it. Always pipe sensitive strings through a
  redact helper before logging or serializing. Treat any new env var with
  `password`, `secret`, `key`, or `token` in its name the same way.

### Session `deviceId` Always Null Because Firmware Never Sends X-Device-Id (API v0.5.1)

- **Symptom**: `GET /sessions` showed `deviceId: null` on every newly-ingested
  session. Older sessions (firmware 0.2.0-0.3.4 era) had populated `deviceId`
  fields, but anything ingested by the v0.4 firmware came back null. The
  per-row data in `tracker_samples` was correct — every row had the right MAC.
- **Root cause #1 (ingest path)**: `tracker_ingest_api.py` upserted session
  metadata with `deviceId: x_device_id` where `x_device_id` is the
  `X-Device-Id` HTTP header. **The firmware has never sent that header** —
  `wifi_uploader.cpp` only sends `X-Session-Id`, `X-Tracker-Id`, `X-Firmware`.
  So the metadata `deviceId` was being overwritten with `None` on every
  upload. Older sessions were lucky: at some point a manual recompute
  populated them and no further uploads happened to overwrite. The recently-
  active session got null every cycle.
- **Root cause #2 (recompute path)**: `/admin/recompute-sessions` already
  computed `deviceId: {$last: "$deviceId"}` in its aggregation pipeline but
  the `$set` clause that wrote the result back to `tracker_sessions` **only
  included `samples / firstTsMs / lastTsMs`** — the deviceId/trackerId/firmware
  fields fell on the floor. So even running recompute didn't fix it.
- **Root cause #3 ($last is undefined without sort)**: both `recompute-sessions`
  and `migrate-to-daily-sessions` used `{$last: "$deviceId"}` without a
  preceding `$sort`. MongoDB's `$last` accumulator returns "the last document
  in the group" but without an explicit sort the order is determined by the
  storage engine and is undefined. Even when this happened to work, it was
  silently brittle.
- **Fix (v0.5.1)** in `api/vega-tracker-ingest/tracker_ingest_api.py`:
  - **Ingest endpoint** (`POST /ingest/csv`): derive `derived_device_id` from
    the parsed CSV rows themselves — walk the rows in reverse and pick the
    first non-empty `deviceId` value (rows in a single upload are in
    timestamp order). Fall back to the X-Device-Id header only if no row
    has one. Write `derived_device_id` to session metadata instead of the
    raw header value.
  - **Recompute endpoint** (`POST /admin/recompute-sessions`): prepend
    `{$sort: {sessionId: 1, timestampMs: 1}}` to the pipeline so `$last`
    is deterministic, and **actually $set deviceId/trackerId/firmware** when
    the aggregation produced a non-empty value. Skip the $set when the
    aggregation returned None so we don't clobber a good ingest-path value
    with a stale aggregate result.
  - **Migrate endpoint** (`POST /admin/migrate-to-daily-sessions`): same
    `$sort` added.
- **Verification after deploy**:
  ```
  python scripts\_diag_sessions.py   # (temp script, removed after use)
  2026-05-11   dev='524306e0042d'  fw=0.4.7  samples=38521
  2026-05-10   dev='524306602024'  fw=0.3.6  samples=19453
  ... all 11 sessions populated correctly.
  ```
  Confirms two RadiaCode devices have been in use: legacy `524306602024`
  and current `524306e0042d`.
- **Rule for future API edits**: any aggregation that uses `$first` or
  `$last` MUST be preceded by an explicit `$sort` stage. Never rely on
  scan order. Also: every place that writes session metadata must include
  the deviceId/trackerId/firmware fields, not just timing fields.

### Verification Pass (v0.4.8) — Self-Test Results

After flashing v0.4.8 and before relying on field validation, the following
self-tests must pass. All passed on the current build:

- **Native unit tests** (`pio test -e native`): **36/36 succeeded** across
  `test_battery_native`, `test_csv_schema_native`, `test_line_count_native`.
- **Device integration suite** (`python scripts\test_device.py --port COM4`):
  **12 passed, 1 skipped** (`sample_count_consistency` skipped only because
  the device had no GPS fix indoors so `samples=0`). All command latencies
  under 700 ms; heartbeat cadence and format healthy; 30-second serial
  loop-latency test passed with zero stalls.
- **`/system.log` evidence of v0.4.8 fix** (from prior boot, 6 consecutive
  cycles after AP cache warmed):
  ```
  718333 -> 720736   cycle_done ok=1/1   (2.4s)
  726172 -> 734213   cycle_done ok=1/1   (8.0s — drain backlog)
  739485 -> 740886   cycle_done ok=1/1   (1.4s)
  746036 -> 747852   cycle_done ok=1/1   (1.8s)
  753045 -> 754844   cycle_done ok=1/1   (1.8s)
  760059 -> 761538   cycle_done ok=1/1   (1.5s)
  766853 -> 768839   cycle_done ok=1/1   (2.0s)
  ```
  These warm reconnects in 1.4-2.4 s prove the `WiFi.persistent(true)` +
  `disconnect(false,false)` fix preserves the cached BSSID across cycles.
- **STORAGE-screen pixel-layout audit** (verified against `src/ui.cpp`
  draw calls): header `HEADER_H=12`; body rows at y=14 (REC/AUTO/Samp),
  y=26 (Day), y=38 (Disk %), y=50 (6 px bar), y=58 (Files on disk),
  y=70 (Wi-Fi state). Last text baseline at y=70 fits within the 80 px
  panel as required by AGENTS.md. STORAGE-INIT-FAILED fallback uses
  y=14,26,40,54,66 — also in bounds.
- **Limitation**: a live v0.4.8-firmware cold-connect cycle could not be
  captured indoors because there is no GPS fix → `lifetimeSamples=0` →
  the upload task short-circuits and `WIFISTAT` stays at `lastAttempt=0`.
  Connect-timing validation will happen automatically the first time the
  device is taken outdoors. The new log line
  `[WIFI] connected ip=... rssi=... in Xms ch=N` makes this trivial to
  spot-check in the next field session.
- **Build hygiene**: USB-JTAG-induced resets continue to appear in
  `/system.log` as `BOOT,UNKNOWN,raw0=21,raw1=21` every time `drive.py`
  opens COM4. This is expected (see USB-JTAG lesson) and is not a firmware
  fault.

### Wi-Fi Connect Timeout Too Short With BLE Coex (v0.4.8)

- **Symptom**: device says it's connecting to Wi-Fi but data only reaches the
  server after USB plug-in. STORAGE screen cycles between `Wi-Fi:
  connecting...` and `Retry: Xm YYs` for 5+ minutes while the AP is right
  next to the device. Then mysteriously succeeds.
- **Diagnostic from `/system.log`** (the only trustworthy source — see USB-JTAG
  rule above):
  ```
  t=188s cycle_start -> t=200s connect_fail  (12s timeout)
  t=260s cycle_start -> t=273s connect_fail  (12s)
  t=333s cycle_start -> t=345s connect_fail  (12s)
  t=465s cycle_start -> t=478s connect_fail  (12s)
  [4-minute backoff gap from v0.4.7 exponential backoff]
  t=718s cycle_start -> t=720s cycle_done ok=1/1   <- 2.4s, instant
  t=726s cycle_start -> t=734s cycle_done ok=1/1   <- backlog drain
  ```
  vbat was 3979-4008 mV throughout (no brown-out). Every connect either took
  >12 s (timeout) or 2-7 s (success). The 12 s window was missing valid
  associations by 1-3 s.
- **Root cause**: NimBLE running concurrently on Core 0 starves the Wi-Fi
  driver of CPU during scan + association + DHCP. A cold `WiFi.begin()` after
  `WiFi.mode(WIFI_OFF)` can take 10-15 s reliably, sometimes 18-20 s, with
  BLE active. The 12 s timeout we inherited from copy-pasted examples was
  simply too short.
- **Anti-fix (don't repeat)**: previous code called
  `WiFi.disconnect(true, true)` between every cycle. The second `true`
  (`eraseAP`) wipes the cached BSSID/channel from NVS, defeating
  `WiFi.persistent(true)`. With `eraseAP=true`, every cycle does a full
  active scan from scratch (~2-3 s wasted before assoc even begins).
- **Fix (v0.4.8)**:
  - `secrets::WIFI_CONNECT_TIMEOUT_MS` 12000 -> 25000.
  - `WifiUploader::begin()`: `WiFi.persistent(true)` (was false).
  - `WifiUploader::connectWifi()` + `disconnectWifi()`: change
    `WiFi.disconnect(true, true)` -> `WiFi.disconnect(false, false)` so the
    cached AP info survives between cycles.
  - Serial log enhanced to include connect duration:
    `[WIFI] connected ip=... rssi=... in 2400ms ch=11`. Use this to spot
    regressions in connect speed during future work.
- **Verification**: after the fix, watch `[WIFI] connected ... in Xms` in
  serial. X should be <5000 ms for a warm reconnect (cached BSSID) and
  <15000 ms for a cold start. If you see 25000 ms or timeouts, BLE
  coexistence may be regressing.

### USB-JTAG Reset Mistaken for Random Reboots (v0.4.7)

- **Symptom**: device "reboots every 6-10 minutes" with `esp_reset_reason()`
  returning `ESP_RST_UNKNOWN`. `lifetimeSamples_` counter resets to 0
  intermittently. User reports "sample count went to zero the moment I
  plugged in USB". Reset reasons in `/system.log` show only `UNKNOWN` so
  the cause is opaque.
- **Root cause**: on ESP32-S3 the USB-JTAG controller can issue a chip reset
  whenever the USB CDC interface re-enumerates. Plugging in USB, opening a
  serial port (DTR pulse), Windows USB power management, antivirus polling
  — any of these triggers `USB_JTAG_CHIP_RESET` (raw reset reason `21` on
  ESP32-S3). The IDF `esp_reset_reason()` cannot classify this and returns
  `ESP_RST_UNKNOWN` (value 0), which we labeled "UNKNOWN" in serial logs.
  On battery alone (USB unplugged) the device does **not** suffer these
  resets — the apparent reboot loop was entirely USB / development-tooling
  induced.
- **Diagnostic (v0.4.7)** in `event_log.cpp::beginBoot()`:
  - Include `<rom/rtc.h>` and call `rtc_get_reset_reason(0)` /
    `rtc_get_reset_reason(1)` for the per-CPU hardware reset register.
    Append `raw0=N,raw1=N` to every `BOOT` line in `/system.log` and the
    serial banner. ESP32-S3 raw reset reason cheat sheet:
    1=POWERON, 9=RTCWDT_SYS, 11=RTC_SW_CPU, 13=RTCWDT_CPU,
    14=RTCWDT_BROWN_OUT, 15=RTCWDT_RTC, 17=SUPER_WDT, 18=GLITCH_RTC,
    20=USB_UART_CHIP_RESET, **21=USB_JTAG_CHIP_RESET**, 24=JTAG_RESET.
  - Anything in the 20-24 range means USB / JTAG re-enumeration, not a
    firmware fault. Don't waste time hunting an imaginary stack overflow.
- **UX fixes (v0.4.7)** since every USB-induced reboot wastes 75+ seconds
  before the first real upload attempt and the first 2 attempts after a
  fresh boot always fail with a 12 s connect timeout:
  - Post-boot grace reduced 15 s -> 5 s in `WifiUploader::taskLoop()`.
  - Exponential backoff no longer triggers on failures 1 and 2 — only
    from the 3rd consecutive fail onward. Lets a freshly-booted device
    drain its queue at 60 s cadence instead of jumping to 2 min.
  - After a successful upload, the cadence shrinks to 5 s so a backlog
    drains fast (was 60 s).
  - `WifiUploader::Phase` enum + accessor replaces the boolean `busy_`.
    The STORAGE screen now shows `Wi-Fi: connecting...`, `Wi-Fi:
    uploading...`, `Wi-Fi: cleanup...`, `Next sync: Xs`, or `Retry: Xs`
    instead of a single ambiguous "uploading..." that lied during the
    12 s connect timeout.
- **Operational rule**: when diagnosing reboot mysteries via serial, expect
  **drive.py / capture_boot.py to cause an extra reset on every invocation**.
  Pull the device's persistent log via `LOG` and look at `lastUptimeMs` /
  `raw0` rather than judging stability by what you see on serial.

### Sample Counter Resetting on Upload Looks Like Data Loss (v0.4.6)

- **Symptom**: user watches the STORAGE screen show `Samp 326` while driving,
  comes home, sees `Samp 40` then `Samp 0` after a Wi-Fi sync. Panics that
  data was lost. Server confirms all 326 rows arrived. Nothing was lost.
- **Root cause**: `SessionStore::sampleCount_` is the row count of the
  **currently-open day file**. Every Wi-Fi upload cycle calls
  `rotateForUpload()` which renames the active `<today>.csv` to
  `<today>.<bootMs>.up.csv` and opens a fresh empty `<today>.csv`. That
  reset `sampleCount_` to 0 (correctly — the new active file is empty).
  But the user reads "Samp" as "samples I've captured today" and sees a
  drop to 0 immediately after a successful sync.
- **Fix (v0.4.6)** in `session_store.{h,cpp}`:
  - Added `lifetimeSamples_` — incremented on every successful append,
    never reset by rotate / day rollover / wipe-active. Public accessor
    `lifetimeSamples()`.
  - STORAGE screen `Samp NNN` field switched from `sampleCount()` to
    `lifetimeSamples()` so the user sees a monotonic counter that only
    grows. The number now reflects "samples written this boot" rather
    than "samples in the open file" — far closer to user intuition.
  - `[HB]` heartbeat extended with `life=N` field so serial logs also
    show the monotonic count alongside the per-file count.
- **Rule**: any UI that says "samples" without qualification should use the
  monotonic counter. Reserve `sampleCount()` for diagnostic / API logic
  that legitimately wants the rotation-window count.

### Upload Cadence & Stale Zero-Byte Pending Files (v0.4.5)

- **Base upload cadence**: every `secrets::UPLOAD_INTERVAL_MS` (default 60 s)
  the WifiUploader task wakes, takes the recording mutex briefly to call
  `rotateForUpload()`, then iterates `listPendingUploads()` posting each
  `.up.csv` to `POST /ingest/csv`. On success the file is deleted.
- **Exponential backoff**: on consecutive failures the wait stretches
  1x, 2x, 4x, 8x, 16x of the base — capped at ~16 minutes. A single
  successful upload resets the counter back to 60 s.
- **`nextAttemptMs()` accessor**: `WifiUploader` exposes the absolute `millis()`
  deadline of the next wake. The STORAGE screen renders it as a live
  countdown (`Next sync: Xm YYs` / `Next sync: Xs` / `Next sync: soon`).
  Use this — not `lastAttemptMs() + interval` — because it already reflects
  any active backoff multiplier.
- **Stale 0-byte `.up.csv` files**: if the firmware panics or browns out
  between `rotateForUpload()` and the first `append()` to the fresh day
  file, the rotated `.up.csv` can be 0 bytes. Old uploader logic logged
  `zero bytes, skipping` and left the file on disk; every subsequent cycle
  re-encountered it, counted it as a failure, and the backoff ramped to
  16 min. Real data in the new active file then waited 16 min between
  attempts.
- **Fix (v0.4.5)** in `wifi_uploader.cpp::runOnce()`: when `p.sizeBytes == 0`,
  call `store_->removePendingUpload(p.filename)` immediately and count it as
  a successful upload. No POST, no backoff increment. This silently cleans
  up debris from prior crashes on the next cycle the device is online.
- **Symptom that meant this was happening**: `[UPLOAD] cycle done; ok=0/N`
  with `[UPLOAD] ...: file_bytes=0` and `[WIFI] backoff: N consecutive
  failures, next attempt in 960s`. If you see that with no actual HTTP
  failures, you are looking at stale zero-byte debris.

### TFT Layout — 80 px Tall Panel, Don't Render Below y=72 (v0.4.5)

- **The display is 160 x 80**. The header (status bar) eats y=0..11. The
  body is y=12..79. Text at the default 5x8 font occupies 8 rows, so the
  last usable text baseline is y=72 (drawing 72..79). Anything at y=78 is
  clipped to the top two rows of the glyph and looks broken.
- **STORAGE screen layout** (post v0.4.5):
  - y=14: REC / AUTO state / sample count
  - y=26: Day xxx
  - y=38: Disk N% used
  - y=50: 6 px progress bar (y=50..55)
  - y=58: Files on disk: N
  - y=70: Wi-Fi countdown / status
- **Rule of thumb**: never place a `field()` call with default font at
  y > 72. When in doubt, draw it and look at the device — the
  `data/sessions_dump/*/boot_*.html` screenshots are great verification.

### Wi-Fi Brown-Out Boot Loop When Out of Range (v0.4.1)

- **Symptom**: device boot-loops repeatedly the moment it leaves the home Wi-Fi
  network's range; reboots stop the moment it returns to range.
- **Root cause**: when the AP is unreachable, `WiFi.begin()` puts the supplicant
  into a continuous full-power active scan across every channel. The PA pulls
  large current spikes at the default 19.5 dBm; on a marginal USB-only or
  partially-charged-LiPo supply those spikes collapse the rail and trigger
  `ESP_RST_BROWNOUT`. The resulting boot brings Wi-Fi back up, immediately
  starts scanning again, and the cycle repeats.
- **Fix (v0.4.1)** in `wifi_uploader.cpp::connectWifi()`:
  - `WiFi.setTxPower(WIFI_POWER_11dBm)` before `begin()` — roughly halves the
    PA peak current with no perceptible range loss for an indoor home AP.
  - `WiFi.setSleep(false)` for the duration of the connect attempt — modem
    sleep + active scan is buggy in the ESP-IDF Wi-Fi driver.
- **Fix (v0.4.1)** in `wifi_uploader.cpp::taskLoop()`:
  - Exponential backoff on consecutive failures (1x, 2x, 4x, 8x, 16x of the
    60 s cadence, capped at ~16 min). Stops the radio from churning every
    minute during long out-of-range stretches.
- **Fix (v0.4.1)** in `wifi_uploader.cpp::runOnce()`:
  - Only call `rotateForUpload()` when there are zero pending files already.
    Previously every cycle would rotate the active day file, fragmenting it
    into dozens of tiny `.up.csv` slices when out of range. Now the active
    file just keeps growing until we successfully upload whatever was
    queued, then a single larger pending file is created next cycle.
- **Diagnostic**: `main.cpp::setup()` now logs `esp_reset_reason()` at boot
  (`[BOOT] reset reason: BROWNOUT (...)` etc.) so any future field reset is
  immediately visible in the serial log instead of looking like a power-on.

### Always-On Day-Bucketed Recording (v0.4.0)

- **Contract**: recording is no longer user-toggled. Whenever the RadiaCode is
  connected AND `gps.hasFix()` is true AND `bestEpochMs()` returns a post-2020
  UTC timestamp, samples are written to `/sessions/<YYYY-MM-DD>.csv` where the
  date is computed in local-eastern time. Samples failing any of those gates
  are silently dropped (logged every ~60th skip).
- **Day rollover**: `SessionStore::append()` checks the day-id of every sample.
  When it changes, the active file is rotated to `<prev-day>.<bootMs>.up.csv`
  and a new `<today>.csv` is opened transparently. No samples are lost across
  the boundary.
- **Upload model**: `WifiUploader::runOnce()` calls `store_->rotateForUpload()`
  first (under the recording mutex), then iterates `listPendingUploads()`. On
  HTTP 2xx the `.up.csv` file is deleted from disk. The currently-open
  `<today>.csv` is never touched mid-cycle by upload code. Result: zero risk
  of duplicate samples on the device, and recording is never paused.
- **Mutex**: `SessionStore::mutex_` (FreeRTOS semaphore) guards every append /
  rotate / list operation. BLE callback (Core 1) and Wi-Fi uploader (Core 0)
  contend cleanly.
- **Removed surface**: `SessionStore::start()` / `stop()` / `toggle()`,
  `Ui::ACTION_TOGGLE_REC`, the double-long-press confirmation, the
  `cfg::KEEP_UPLOADS_ON_DEVICE` knob, and the `WifiUploader::uploadedThisBoot_`
  vector are all deleted. `ACTIVE_FILE` marker is no longer used.
- **API migration**: `POST /admin/migrate-to-daily-sessions?confirm=MIGRATE_CONFIRMED`
  (API 0.5.0+) re-keys every existing sample's `sessionId` to its local-eastern
  YYYY-MM-DD via aggregation-pipeline `$dateToString`, dedupes collisions, and
  rebuilds the `tracker_sessions` metadata.
- **`localtime_r()` requires `tzset()`**: `setenv("TZ", cfg::LOCAL_TZ, 1); tzset();`
  must run in `setup()` before `gStore.begin()` or any append. Otherwise the
  day-id falls back to UTC (silently wrong by 4-5 hours).

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

### Blocking BLE Auto-Scan / Button Failure During Reconnect Storm (v0.3.4)

- **Root cause**: `RadiaCode::loop()` called `doScan(RADIACODE_SCAN_MS)` which
  is a **synchronous blocking scan** lasting 8 seconds on every reconnect
  attempt. With `RADIACODE_RECONNECT_MS = 5 s`, the main Arduino loop (core 1)
  was blocked 8 out of every 13 seconds during a BLE disconnect/reconnect storm.
- **Symptoms observed**:
  1. After 2-3 hours of field use (one large session building up), BLE kept
     disconnecting and reconnecting for ~1 hour before stabilizing.
  2. During (and after) the storm, the double long-press stop-recording
     confirmation **never worked** (failed ~40 consecutive times). The user
     held the button, saw "HOLD AGAIN: STOP REC", held it again — nothing.
  3. Menu navigation was increasingly sluggish as session count grew.
- **Why the button failed**: `pressStartMs_` in `Button::poll()` is set to the
  current `millis()` when the first stable press is detected by the main loop.
  During the 8-second scan block, `poll()` isn't called. When the block ends,
  `pressStartMs_` is stamped at block-end, not at the physical press start.
  More critically: `Ui::tick()` (which checks the 10-second confirmation timer)
  wasn't called during the block. If a reconnect cycle happened while
  `confirmStopPending_ = true`, the timer expired unnoticed during the block,
  and `tick()`'s first call after unblocking immediately cleared the flag.
- **Fix (v0.3.4)**:
  - `doScan()` is no longer called from `loop()`. The auto-mode scan now runs
    asynchronously via `scan->start(duration, nullptr, false)` (BLE stack task).
    `loop()` polls `g.foundDev` and `g.autoScanActive` each iteration.
    The main loop is never blocked by the scan thread. `connectToFound()` still
    blocks (~1-5 s) but only fires once per reconnect, not every scan window.
  - `kConfirmStopTimeoutMs` increased from 5 s to 10 s to give headroom for
    the brief `connectToFound()` blocking period that remains.

### O(N) Line Counting in listSessions() / resumeIfActive() (v0.3.4)

- **Root cause**: LittleFS path in `listSessions()` and `resumeIfActive()` used
  `data.readStringUntil('\n')` to count lines, allocating and freeing a heap
  `String` for every single row — 20,000 alloc/free cycles for a 20,000-row
  session. `esp_littlefs` uses a **global volume mutex**: the WiFi uploader task
  (core 0) calling `listSessions()` every 60 s held that mutex while churning
  through 20,000 String allocations, blocking `append()` calls on the NimBLE
  host task for hundreds of milliseconds and starving BLE connection events.
- **Fix (v0.3.4)**:
  - Replaced `readStringUntil('\n')` with a 256-byte raw buffer scan (same
    approach the SdFat backend already used). O(N) cost is now pure byte reads,
    no heap allocation.
  - For the **active recording session**, `listSessions()` returns `sampleCount_`
    from memory instead of touching the file at all (the active session is
    filtered out by the WiFi uploader immediately anyway).


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


### Auto-Scan State Machine — Stuck in Scanning Forever (v0.3.6)

- **Root cause**: In `RadiaCode::loop()`, the auto-reconnect management block
  was guarded by `if (g.state == Idle || Disconnected)`. When starting an async
  scan, the code called `setState(State::Scanning)`, which changed `g.state` to
  `State::Scanning`. On every subsequent `loop()` call, the guard (`Idle ||
  Disconnected`) evaluated false, so the entire auto-scan management block was
  skipped. `g.foundDev` and `scan->isScanning()` were never checked. The device
  stayed stuck in `rcState=1` (Scanning) indefinitely — observed at 90+ minutes
  in the field.
- **Symptoms**: After a BLE disconnect (range loss, power-cycle, etc.), `rcState`
  showed `1` (Scanning) in heartbeat logs but never advanced to `2` (Connecting)
  or `4` (Ready). Required a physical device reboot to recover.
- **Why initial boot worked**: The first session was connected via the manual
  picker UI (`connectTo()` → `pendingConnectAddr`), which bypasses the auto-scan
  loop entirely. The auto-scan path was always broken post-disconnect.
- **Fix (v0.3.6)**: Extended the outer condition to include `State::Scanning`:
  ```cpp
  if (g.state == State::Idle || g.state == State::Disconnected ||
      g.state == State::Scanning) {
  ```
  Now, while the async scan is running (`state == Scanning`, `autoScanActive ==
  true`), subsequent `loop()` calls still enter the block and check `g.foundDev`
  and the scan deadline. Also hardened the `autoRetryHalted` handler to stop any
  in-progress scan cleanly before returning.

### BLE Picker Long-Press Crash — Dual-Core Use-After-Free on g.foundDev (v0.3.8)

- **Root cause**: The ESP32-S3 is dual-core. ScanCb runs in the NimBLE host task
  (Core 0); the Arduino loop and BLE `connect()` calls run on Core 1. When the
  user long-pressed a device in the picker, `connectToAddress()` called
  `waitForConnectableAdv()`. That function found a connectable adv, called
  `scan->stop()`, cleared `g.targetAddr`, then returned `true`. Immediately after
  `scan->stop()`, buffered adv packets still in the BLE host's event queue fired
  more `ScanCb::onResult()` callbacks. With `g.targetAddr` now empty, these fell
  into the auto-mode ScanCb path, which did:
  ```cpp
  delete g.foundDev;            // Core 0: frees the object
  g.foundDev = new NimBLEAdvertisedDevice(*dev);
  ```
  Meanwhile the main task (Core 1) was calling
  `g.client->connect(g.foundDev, ...)`, internally reading fields (address,
  address type, PHY) from the object that Core 0 had just freed → heap
  corruption → crash.
- **Symptoms**: Device rebooted immediately after long-pressing a device in the
  picker (RC-110 in particular, which uses BT5 extended advertising and fires
  many buffered post-stop callbacks).
- **Why RC-102 was less affected**: RC-102 uses legacy advertising (single adv
  packet per interval). RC-110 uses BT5 extended advertising on secondary
  channels, which queues more packets and makes the race much more likely.
- **Fix (v0.3.8)**:
  - Added `portMUX_TYPE foundDevMux` to `Internal` struct.
  - All three `g.foundDev` write sites in `ScanCb` (targetAddr path, grab-pattern
    path, auto-mode path) wrapped with `portENTER_CRITICAL/portEXIT_CRITICAL`.
  - `waitForConnectableAdv` changed to return `NimBLEAdvertisedDevice*` instead
    of `bool`. The poll loop atomically takes ownership via spinlock:
    `cap = g.foundDev; g.foundDev = nullptr;` under the lock, then calls
    `cap->isConnectable()` safely (ScanCb can no longer delete cap). Returns the
    pointer to caller on success; caller must `delete` it after the connect attempt.
  - `connectToAddress` uses the returned `capDev` pointer for `connect()`, then
    deletes it on both success and failure paths.
  - `connectToFound` (auto-mode reconnect path) applies the same ownership-transfer
    pattern at function entry: takes `capDev = g.foundDev; g.foundDev = nullptr;`
    under spinlock before reading any fields or calling `connect()`.
  - **Key rule**: never pass `g.foundDev` directly to `connect()` — always
    transfer ownership to a local pointer under the spinlock first.
- **Secondary fix**: `manualScanArmed` in `main.cpp` was never cleared in the
  `ACTION_PICK_DEVICE` and `ACTION_CANCEL_PICKER` handlers, so the scan-
  completion handler could re-open the picker after a device was already chosen.
  Added `manualScanArmed = false` in both cases.

### Hex Bin Layer (replaced Heatmap)

- All heatmap attempts (circle markers, `L.Layer.extend`, raw canvas blobs) failed to render correctly in Vite/ESM builds. Replaced with a fully custom hex-bin canvas layer (`HexLayer` in `App.jsx`).
- **Implementation**: plain `<canvas>` appended to `map.getContainer()` with `z-index:400`. No Leaflet layer class used — `L.Layer.extend` is silently broken in Vite/ESM builds; don't use it.
- **Bin geometry**: flat-top hexagons, cube-coordinate rounding, `HEX_R=36px` circumradius at `binZoom` resolution.
- **`binZoom` prop**: geographic resolution for binning is independent of map view zoom. Scale factor `2^(mapZoom - binZoom)` converts bin pixel coords to screen space on every draw. Higher binZoom = finer/denser bins; lower = coarser.
- **`MapZoomSync` component**: rendered inside `MapContainer`, calls `onZoomChange` via `useMapEvents` on every zoom event. Parent uses this to auto-sync `hexBinZoom` state when `hexBinAuto=true`.
- **Draw radius**: `visR * 0.94` (94% of visual circumradius) leaves a visible gap between touching neighbors so map tiles show through.
- **Opacity**: 0.55 fill alpha — enough color to read, enough transparency to see map labels underneath.
- **`clearRect` on every frame**: mandatory — prevents GPU compositing from leaving stale pixels when canvas is resized.
- **Key lesson**: high point density means hex bins fill every viewport pixel. At any opacity > ~0.6, the map becomes invisible on CartoDB Dark. 0.55 + 94% draw radius is the tuned balance.
- **`heatGradientColor(t)`** still used for bin coloring (green→amber→red); rename is cosmetic only, skip it.

### Runtime Config Global Rename

- `window.__VEGA_CONFIG__` renamed to `window.__APP_CONFIG__` during the rebrand
  (May 2026). Updated in `public/config.js`, `src/api.js`, and `dist/config.js`.
  The nginx entrypoint script patches `config.js` at container start — no rebuild needed
  to change `API_BASE`.
