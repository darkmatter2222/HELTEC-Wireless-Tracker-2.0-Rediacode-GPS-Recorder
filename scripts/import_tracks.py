"""Import RadiaCode app-native track exports into the Radiological Map Ingest API.

The RadiaCode Android/iOS app can export a recorded track as a tab-separated
.txt file.  Format (verified against real exports from this user's RC-110):

    Line 1:  Track: YYYY-MM-DD HH-MM-SS \\t <SERIAL> \\t   \\t EC
             (the trailing fields are flag bytes from the app, ignore them)
    Line 2:  Timestamp \\t Time \\t Latitude \\t Longitude \\t Accuracy \\t
             DoseRate \\t CountRate \\t Comment
    Line 3+: <FILETIME ticks> \\t YYYY-MM-DD HH:MM:SS (UTC) \\t lat \\t lng \\t
             accuracy_m \\t uSvPerHour \\t cps \\t comment

Timestamp column is Windows FILETIME (100-ns intervals since 1601-01-01 UTC).
Conversion to unix milliseconds:

    unix_ms = (filetime_ticks - 116444736000000000) // 10000

This script:
  1. Walks tracks/ for *.txt files
  2. Parses every file into rows
  3. Buckets rows by **local-eastern YYYY-MM-DD** (matches firmware v0.4.0+
     daily-session model; the Ingest API expects this sessionId scheme)
  4. POSTs a 10-column CSV per (sessionId, file) combination to
     POST /ingest/csv -- the unique {sessionId, timestampMs} index makes
     re-runs idempotent so it's safe to interrupt and restart.

CSV row layout we emit (10 columns -- event column 11 always empty):

    timestampMs,uSvPerHour,cps,latitude,longitude,deviceId,
    speedKph,bearingDeg,altitudeM,hdop

The track exports do not include speed / bearing / altitude / HDOP, so
columns 7-10 are all empty.  Accuracy is in metres which is fundamentally
different from HDOP (unitless), so we deliberately do NOT map it -- pretending
otherwise would corrupt downstream stats.

Device identification: the serial number in the file header (e.g.
"RC-110-001069") is normalised to "RC110001069" and used as both deviceId
(per-row) and the X-Device-Id / X-Tracker-Id headers.  This is a different
ID space from the BLE-MAC deviceIds emitted by the Heltec firmware, which
is by design -- these are samples that never went through a tracker.

Usage:
    python scripts/import_tracks.py                 # uses defaults
    python scripts/import_tracks.py --dry-run       # parse only, no HTTP
    python scripts/import_tracks.py --api http://192.168.86.48:8030
    python scripts/import_tracks.py --tracks-dir tracks --limit 5

Defaults:
    --tracks-dir tracks
    --api        http://192.168.86.48:8030
    --tz         America/New_York
"""
from __future__ import annotations

import argparse
import csv
import io
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

# Windows FILETIME epoch (1601-01-01 UTC) measured in unix milliseconds.
# Equivalent to 11644473600 seconds before the unix epoch.
FILETIME_TO_UNIX_MS_OFFSET = 11_644_473_600_000

# Server-side gate in the Ingest API -- anything older is rejected.
MIN_VALID_TS_MS = 1_577_836_800_000  # 2020-01-01 00:00:00 UTC

FW_TAG = "import_v1"
TRACKER_TAG = "radiacode_app_export"


def filetime_to_unix_ms(ticks: int) -> int:
    """Convert FILETIME 100-ns ticks (since 1601-01-01 UTC) to unix ms."""
    return ticks // 10_000 - FILETIME_TO_UNIX_MS_OFFSET


def parse_header(line: str) -> tuple[str | None, str | None]:
    """Extract (serial, normalised_device_id) from the first line of a track.

    Header looks like:  'Track: 2025-09-08 18-47-16\\tRC-110-001069\\t \\tEC'
    """
    parts = [p.strip() for p in line.rstrip("\n\r").split("\t")]
    if len(parts) < 2:
        return None, None
    serial = parts[1] or None
    device_id = serial.replace("-", "") if serial else None
    return serial, device_id


def parse_track_file(path: Path) -> tuple[str | None, list[dict]]:
    """Parse one track file. Returns (device_id, rows).

    Each row dict has: ts_ms, lat, lng, dose, cps. Invalid rows are skipped.
    """
    with path.open("r", encoding="utf-8", errors="replace") as f:
        first = f.readline()
        _serial, device_id = parse_header(first)
        # Discard the column-header line.
        f.readline()
        rows: list[dict] = []
        for raw in f:
            cols = raw.rstrip("\n\r").split("\t")
            if len(cols) < 7:
                continue
            try:
                ticks = int(cols[0])
                # cols[1] is the human-readable UTC string; we trust ticks.
                lat = float(cols[2])
                lng = float(cols[3])
                # cols[4] = accuracy in metres, not mappable to HDOP -- skipped.
                dose = float(cols[5])
                cps = float(cols[6])
            except ValueError:
                continue
            ts_ms = filetime_to_unix_ms(ticks)
            if ts_ms < MIN_VALID_TS_MS:
                continue
            rows.append({
                "ts": ts_ms,
                "lat": lat,
                "lng": lng,
                "dose": dose,
                "cps": cps,
            })
    return device_id, rows


def bucket_by_local_day(rows: list[dict], tz: ZoneInfo) -> dict[str, list[dict]]:
    """Bucket rows by local-eastern YYYY-MM-DD (firmware daily-session scheme)."""
    buckets: dict[str, list[dict]] = {}
    for r in rows:
        dt_local = datetime.fromtimestamp(r["ts"] / 1000, tz=tz)
        key = dt_local.strftime("%Y-%m-%d")
        buckets.setdefault(key, []).append(r)
    return buckets


def rows_to_csv(rows: list[dict], device_id: str) -> bytes:
    """Build a 10-column CSV body (matches firmware v0.7.0 schema minus event).

    The Ingest API tolerates 10-column rows (event column is optional).
    Output is sorted by timestamp ascending.
    """
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow([
        "timestampMs", "uSvPerHour", "cps", "latitude", "longitude",
        "deviceId", "speedKph", "bearingDeg", "altitudeM", "hdop",
    ])
    for r in sorted(rows, key=lambda x: x["ts"]):
        w.writerow([
            r["ts"], f"{r['dose']:.4f}", f"{r['cps']:.2f}",
            f"{r['lat']:.7f}", f"{r['lng']:.7f}",
            device_id, "", "", "", "",
        ])
    return buf.getvalue().encode("utf-8")


def post_csv(api_base: str, session_id: str, device_id: str, body: bytes,
             timeout: float = 60.0) -> dict:
    """POST one CSV chunk to /ingest/csv. Returns parsed JSON response.

    Raises urllib.error.HTTPError on non-2xx (the unique-index dedup is reported
    in the JSON body as "duplicates", NOT as an HTTP error).
    """
    req = urllib.request.Request(
        url=f"{api_base.rstrip('/')}/ingest/csv",
        data=body,
        method="POST",
        headers={
            "Content-Type": "text/csv",
            "X-Session-Id": session_id,
            "X-Device-Id": device_id,
            "X-Tracker-Id": TRACKER_TAG,
            "X-Firmware": FW_TAG,
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
    import json
    return json.loads(raw.decode("utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tracks-dir", default="tracks", type=Path)
    ap.add_argument("--api", default="http://192.168.86.48:8030",
                    help="Base URL of the Ingest API (default: LAN-direct, no auth)")
    ap.add_argument("--tz", default="America/New_York",
                    help="Local timezone for daily-session bucketing")
    ap.add_argument("--limit", type=int, default=0,
                    help="Process at most N files (0 = all). Useful for trial runs.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Parse and bucket files, but do not POST to the API.")
    args = ap.parse_args()

    tracks_dir: Path = args.tracks_dir
    if not tracks_dir.is_dir():
        print(f"ERROR: tracks dir not found: {tracks_dir}", file=sys.stderr)
        return 2

    files = sorted(tracks_dir.glob("*.txt"))
    if args.limit:
        files = files[: args.limit]
    if not files:
        print(f"No *.txt files in {tracks_dir}")
        return 0

    tz = ZoneInfo(args.tz)
    print(f"Found {len(files)} track file(s). API={args.api} "
          f"tz={args.tz} dry_run={args.dry_run}")

    # Phase 1: parse every file, accumulate rows per (file, day) so we can
    # report totals before the network phase.
    per_file: list[tuple[Path, str, dict[str, list[dict]]]] = []
    grand_rows = 0
    detected_device: str | None = None
    parse_t0 = time.time()
    for i, path in enumerate(files, 1):
        try:
            device_id, rows = parse_track_file(path)
        except OSError as e:
            print(f"  [{i}/{len(files)}] {path.name}: read error: {e}")
            continue
        if device_id and not detected_device:
            detected_device = device_id
        if not rows:
            print(f"  [{i}/{len(files)}] {path.name}: 0 rows (skipped)")
            continue
        buckets = bucket_by_local_day(rows, tz)
        per_file.append((path, device_id or detected_device or "unknown", buckets))
        grand_rows += len(rows)
        if i % 10 == 0 or i == len(files):
            print(f"  parsed {i}/{len(files)} files, "
                  f"{grand_rows:,} rows so far")
    parse_dt = time.time() - parse_t0
    print(f"Parse phase: {grand_rows:,} valid rows from {len(per_file)} files "
          f"in {parse_dt:.1f}s")

    if args.dry_run:
        # Summarise per-day totals across all files.
        daily: dict[str, int] = {}
        for _p, _dev, buckets in per_file:
            for day, rs in buckets.items():
                daily[day] = daily.get(day, 0) + len(rs)
        print(f"\nDry-run: would POST {sum(daily.values()):,} rows across "
              f"{len(daily)} daily session(s):")
        for day in sorted(daily):
            print(f"  sessionId={day}  rows={daily[day]:,}")
        return 0

    # Phase 2: POST per-file, per-day so each request is bounded in size.
    total_ok = 0
    total_dup = 0
    total_rej = 0
    total_failed_files = 0
    post_t0 = time.time()
    for fi, (path, device_id, buckets) in enumerate(per_file, 1):
        for day in sorted(buckets):
            rs = buckets[day]
            body = rows_to_csv(rs, device_id)
            try:
                resp = post_csv(args.api, day, device_id, body)
            except urllib.error.HTTPError as e:
                total_failed_files += 1
                err_body = e.read().decode("utf-8", errors="replace")[:300]
                print(f"  [{fi}/{len(per_file)}] {path.name} day={day}: "
                      f"HTTP {e.code} -- {err_body}")
                continue
            except urllib.error.URLError as e:
                total_failed_files += 1
                print(f"  [{fi}/{len(per_file)}] {path.name} day={day}: "
                      f"network error: {e.reason}")
                continue
            ok = int(resp.get("inserted", 0))
            dup = int(resp.get("duplicates", 0))
            rej = int(resp.get("rejected", 0))
            total_ok += ok
            total_dup += dup
            total_rej += rej
            print(f"  [{fi}/{len(per_file)}] {path.name} day={day}  "
                  f"sent={len(rs):,}  inserted={ok:,}  dup={dup:,}  rej={rej:,}")
    post_dt = time.time() - post_t0
    print(f"\nDone in {post_dt:.1f}s.  "
          f"inserted={total_ok:,}  duplicates={total_dup:,}  "
          f"rejected={total_rej:,}  failed_uploads={total_failed_files}")
    print("Re-runs are safe: duplicates are silently dropped by the unique "
          "{sessionId,timestampMs} index.")
    return 0 if total_failed_files == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
