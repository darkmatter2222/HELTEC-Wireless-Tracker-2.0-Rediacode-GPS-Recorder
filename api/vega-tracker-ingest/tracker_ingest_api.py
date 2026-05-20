"""Radiological Map Ingest API.

Accepts radiation/GPS session uploads from the Heltec field tracker
(or any client that produces the same CSV schema as the Android app)
and writes them into MongoDB.

Wire formats
------------

POST /ingest/csv
    Content-Type: text/csv (or application/octet-stream)
    Headers:
        X-Session-Id    required, e.g. "boot_328965" or "20260426_104210"
        X-Device-Id     optional, raw RadiaCode peer addr (no colons)
        X-Tracker-Id    optional, ESP32 chipId / MAC of the uploader
        X-Firmware      optional, firmware version string
    Body:
        timestampMs,uSvPerHour,cps,latitude,longitude,deviceId
        ...rows...

    The first row may be a header (auto-detected by the literal
    "timestampMs" prefix) and is skipped.

Endpoints
---------
GET    /health                          liveness + mongo ping
GET    /info                            collection counts, sample-rate stats, build info
GET    /sessions                        list sessions
POST   /ingest/csv                      upload one session (see above)
PATCH  /sessions/{id}                   rename display name
DELETE /sessions/{id}                   delete session + samples (requires confirm token)
POST   /admin/merge-sessions            merge N source sessions into one target
GET    /sessions/{id}/export            download session as CSV (format=internal|radiacode)
POST   /sessions/export-bulk            download multiple sessions merged as one CSV
POST   /admin/recompute-sessions        recompute session metadata from sample data
POST   /admin/backfill-accuracy         fill missing accuracyM<->hdop pairs (UERE=5.0)
POST   /admin/migrate-to-daily-sessions  one-shot migration (v0.5.0): rekey samples by local-eastern YYYY-MM-DD
GET    /admin/db-stats                  database size/storage metrics
GET    /admin/backups                   list available mongodump backups
POST   /admin/backup                   trigger a mongodump snapshot now
DELETE /admin/backup/{name}            delete a backup (requires confirm token)
POST   /admin/restore/{name}           restore a backup into mongo (requires confirm token)
"""
from __future__ import annotations

import csv
import glob
import io
import logging
import os
import shutil
import subprocess
import time
import zipfile
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Any

import pymongo
from fastapi import FastAPI, Header, HTTPException, Query, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel
from pymongo import MongoClient
from pymongo.errors import BulkWriteError, PyMongoError

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger("tracker-ingest")

MONGO_URI         = os.getenv("MONGO_URI", "mongodb://mongo:27017")

def _redact_mongo_uri(uri: str) -> str:
    """Strip the password from a mongodb:// URI for logging / API responses.

    Returns e.g. 'mongodb://ryan:***@host:27017/?authSource=admin' when input has
    'mongodb://ryan:Welcome123%21@host:27017/?authSource=admin'. Leaves
    credential-free URIs untouched.
    """
    try:
        # Find '://user:password@' segment and replace password with '***'.
        scheme_end = uri.find("://")
        if scheme_end < 0:
            return uri
        at = uri.find("@", scheme_end + 3)
        if at < 0:
            return uri
        creds = uri[scheme_end + 3:at]
        colon = creds.find(":")
        if colon < 0:
            return uri  # no password
        return uri[:scheme_end + 3] + creds[:colon] + ":***" + uri[at:]
    except Exception:
        return "<redacted>"
MONGO_DB          = os.getenv("MONGO_DB", "radiacode")
SAMPLES_COLL      = os.getenv("MONGO_SAMPLES_COLLECTION", "tracker_samples")
SESSIONS_COLL     = os.getenv("MONGO_SESSIONS_COLLECTION", "tracker_sessions")
BACKUPS_COLL      = "tracker_backups"    # telemetry: one doc per backup attempt
API_VERSION       = "0.8.0"
MAX_BODY_BYTES    = int(os.getenv("MAX_BODY_BYTES", str(8 * 1024 * 1024)))   # 8 MB
INGEST_BATCH_SIZE = int(os.getenv("INGEST_BATCH_SIZE", "1000"))
BACKUP_DIR        = os.getenv("BACKUP_DIR", "/backups")  # host-mounted volume
BACKUP_KEEP_COUNT = 5  # rolling window — the cron script prunes to this many snapshots

# Reject any sample timestamp older than 2020-01-01 UTC.  The Heltec tracker
# firmware used to fall back to millis()-since-boot (a few hundred ms to a
# few days worth of ms) when GPS UTC was not yet acquired.  One such row is
# enough to make firstTsMs look like 1970 and the session span 56 years.
MIN_VALID_TS_MS = 1_577_836_800_000  # 2020-01-01 00:00:00 UTC


@asynccontextmanager
async def lifespan(app: FastAPI):
    log.info("connecting to mongo at %s (db=%s)", _redact_mongo_uri(MONGO_URI), MONGO_DB)
    client = MongoClient(MONGO_URI, serverSelectionTimeoutMS=5000)
    # Force connect now so startup fails fast if mongo is unreachable.
    client.admin.command("ping")
    db = client[MONGO_DB]
    samples = db[SAMPLES_COLL]
    sessions = db[SESSIONS_COLL]

    # Indexes (safe to call on every boot).
    samples.create_index([("sessionId", 1), ("timestampMs", 1)], name="session_ts")
    samples.create_index([("deviceId", 1), ("timestampMs", -1)], name="device_ts")
    samples.create_index([("loc", "2dsphere")], name="loc_2dsphere",
                         partialFilterExpression={"loc": {"$type": "object"}})
    # Per-row idempotency: same session+timestamp is the same sample.
    samples.create_index([("sessionId", 1), ("timestampMs", 1)],
                         name="session_ts_unique", unique=True)
    sessions.create_index([("sessionId", 1)], name="session_id_unique", unique=True)

    backups = db[BACKUPS_COLL]
    backups.create_index([("name", 1)], name="backup_name_unique", unique=True)
    backups.create_index([("tsMs", -1)], name="backup_ts_desc")

    app.state.mongo    = client
    app.state.db       = db
    app.state.samples  = samples
    app.state.sessions = sessions
    app.state.backups  = backups
    log.info("mongo ready; collections=%s,%s,%s", SAMPLES_COLL, SESSIONS_COLL, BACKUPS_COLL)
    try:
        yield
    finally:
        client.close()


app = FastAPI(title="Radiological Map Ingest", version=API_VERSION, lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"], allow_credentials=False,
    allow_methods=["*"], allow_headers=["*"],
    expose_headers=["Content-Disposition"],
)


# ---------- helpers ---------------------------------------------------------

def _safe_float(v: str) -> float | None:
    if v is None or v == "":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def _safe_int(v: str) -> int | None:
    if v is None or v == "":
        return None
    try:
        return int(v)
    except ValueError:
        try:
            f = float(v)
            return int(f)
        except ValueError:
            return None


def _parse_csv(body: str, session_id: str, header_device_id: str | None,
               tracker_id: str | None, firmware: str | None
               ) -> tuple[list[dict[str, Any]], int]:
    """Parse a CSV upload into Mongo-ready docs.

    Returns (valid_docs, rejected_count).  Rows with timestampMs < MIN_VALID_TS_MS
    (pre-2020, i.e. raw millis()-since-boot from old firmware) are counted but
    never inserted -- they would poison firstTsMs and make sessions span 56 years.

    Schema (per row):
        sessionId, deviceId, trackerId, firmware,
        timestampMs (int64), uSvPerHour, cps,
        latitude, longitude, loc {type:Point, coordinates:[lng,lat]}  (only if non-zero)
    """
    out: list[dict[str, Any]] = []
    rejected = 0
    rdr = csv.reader(io.StringIO(body))
    for row in rdr:
        if not row:
            continue
        # Skip header row.
        if row[0].strip().lower() == "timestampms":
            continue
        # Pad to minimum 6 cols defensively.
        while len(row) < 6:
            row.append("")
        ts   = _safe_int(row[0])
        usv  = _safe_float(row[1])
        cps  = _safe_float(row[2])
        lat  = _safe_float(row[3])
        lng  = _safe_float(row[4])
        dev  = row[5].strip() or (header_device_id or None)

        # Extended fields added in firmware 0.3.0 (columns 6-9).
        # Pre-0.3.0 uploads have 6 columns; these default to None.
        speed_kph   = _safe_float(row[6]) if len(row) > 6 else None
        bearing_deg = _safe_float(row[7]) if len(row) > 7 else None
        altitude_m  = _safe_float(row[8]) if len(row) > 8 else None
        hdop_val    = _safe_float(row[9]) if len(row) > 9 else None
        # Column 10 (event) added in firmware 0.7.0 -- GPS_LOST / GPS_REGAINED
        # transition markers. Normal samples leave this empty; event rows have
        # no lat/lng/dose values, only a timestamp + deviceId + tag. Stored on
        # the sample doc so the viewer can split polylines at gaps.
        event_tag   = row[10].strip() if len(row) > 10 and row[10].strip() else None
        # Column 11 (accuracyM) added in firmware 0.8.0 -- estimated horizontal
        # accuracy in metres. The firmware computes this from HDOP via the
        # `accuracyM = hdop * 5.0` UERE rule of thumb; the RadiaCode app track
        # importer carries the measured value directly. Stored alongside hdop
        # so consumers can pick whichever they prefer.
        accuracy_m  = _safe_float(row[11]) if len(row) > 11 else None

        if ts is None:
            rejected += 1
            continue
        # Server-side sanity gate: reject pre-2020 timestamps.  The firmware
        # now filters these out before writing to SD card, but old session
        # files created before that fix get uploaded verbatim and must be
        # caught here to prevent session metadata corruption.
        if ts < MIN_VALID_TS_MS:
            rejected += 1
            log.debug("ingest sessionId=%s rejecting row ts=%d (pre-2020)",
                      session_id, ts)
            continue
        doc: dict[str, Any] = {
            "sessionId":  session_id,
            "deviceId":   dev,
            "trackerId":  tracker_id,
            "firmware":   firmware,
            "timestampMs": ts,
            "uSvPerHour": usv,
            "cps":        cps,
            "latitude":   lat,
            "longitude":  lng,
        }
        # Store extended telemetry only when the firmware actually sent them
        # (non-None). This keeps documents from pre-0.3.0 uploads lean.
        if speed_kph   is not None: doc["speedKph"]   = speed_kph
        if bearing_deg is not None: doc["bearingDeg"] = bearing_deg
        if altitude_m  is not None: doc["altitudeM"]  = altitude_m
        if hdop_val    is not None: doc["hdop"]        = hdop_val
        if event_tag   is not None: doc["event"]       = event_tag
        if accuracy_m  is not None: doc["accuracyM"]   = accuracy_m
        if lat is not None and lng is not None and not (lat == 0.0 and lng == 0.0):
            doc["loc"] = {"type": "Point", "coordinates": [lng, lat]}
        out.append(doc)
    if rejected:
        log.warning("ingest sessionId=%s rejected %d pre-2020 row(s) (old firmware artifact)",
                    session_id, rejected)
    return out, rejected


def _bulk_insert(coll, docs: list[dict[str, Any]]) -> tuple[int, int]:
    """Insert in batches with ordered=False so duplicates don't abort.
    Returns (inserted, duplicate_skipped)."""
    inserted = 0
    duplicates = 0
    for i in range(0, len(docs), INGEST_BATCH_SIZE):
        batch = docs[i:i + INGEST_BATCH_SIZE]
        try:
            res = coll.insert_many(batch, ordered=False)
            inserted += len(res.inserted_ids)
        except BulkWriteError as bwe:
            wr_err = bwe.details.get("writeErrors", [])
            for e in wr_err:
                if e.get("code") == 11000:  # duplicate key
                    duplicates += 1
                else:
                    log.warning("bulk-write error code=%s: %s", e.get("code"), e.get("errmsg"))
            inserted += bwe.details.get("nInserted", len(batch) - len(wr_err))
    return inserted, duplicates


def _bulk_upsert_merge(coll, docs: list[dict[str, Any]]) -> tuple[int, int, int]:
    """Upsert by {sessionId, timestampMs}, merging any new fields into existing
    docs via $set / $setOnInsert. Used by /ingest/csv-merge so that re-imports
    of historical data (e.g. the RadiaCode track files) can add new columns
    like `accuracyM` to rows that were ingested before the schema knew about
    them. Returns (inserted, modified, matched_unchanged)."""
    from pymongo import UpdateOne
    inserted  = 0
    modified  = 0
    unchanged = 0
    # Fields that should only be written once (immutable identity / source-of-truth):
    immutable_keys = {"sessionId", "timestampMs", "deviceId", "trackerId",
                       "firmware", "loc"}
    for i in range(0, len(docs), INGEST_BATCH_SIZE):
        batch = docs[i:i + INGEST_BATCH_SIZE]
        ops = []
        for d in batch:
            filt = {"sessionId": d["sessionId"], "timestampMs": d["timestampMs"]}
            set_doc = {k: v for k, v in d.items() if k not in immutable_keys}
            set_on_insert = {k: v for k, v in d.items() if k in immutable_keys}
            update = {}
            if set_doc:        update["$set"]         = set_doc
            if set_on_insert:  update["$setOnInsert"] = set_on_insert
            ops.append(UpdateOne(filt, update, upsert=True))
        if not ops:
            continue
        res = coll.bulk_write(ops, ordered=False)
        inserted  += res.upserted_count
        modified  += res.modified_count
        unchanged += res.matched_count - res.modified_count
    return inserted, modified, unchanged


# ---------- routes ----------------------------------------------------------

@app.get("/health")
def health():
    try:
        app.state.mongo.admin.command("ping")
        mongo_ok = True
    except PyMongoError as e:
        log.error("mongo ping failed: %s", e)
        mongo_ok = False
    return {
        "status":   "healthy" if mongo_ok else "degraded",
        "mongo":    mongo_ok,
        "version":  API_VERSION,
    }


@app.get("/info")
def info():
    samples = app.state.samples
    sessions = app.state.sessions
    return {
        "version":  API_VERSION,
        "mongo": {
            "uri":       _redact_mongo_uri(MONGO_URI),
            "db":        MONGO_DB,
            "samples":   samples.estimated_document_count(),
            "sessions":  sessions.estimated_document_count(),
            "collections": [SAMPLES_COLL, SESSIONS_COLL],
        },
        "limits": {
            "max_body_bytes":    MAX_BODY_BYTES,
            "ingest_batch_size": INGEST_BATCH_SIZE,
        },
    }


@app.get("/sessions")
def list_sessions(limit: int = 200, include_deleted: bool = Query(default=False)):
    """List ingested sessions, newest first.

    sizeBytes is an estimate: samples * avg_doc_storageSize from collStats.
    Accurate to within ~10% for typical sessions.

    By default, soft-deleted sessions (deletedAt is set) are excluded.
    Pass ?include_deleted=true to include them.
    """
    try:
        cs = app.state.samples.command("collStats", "tracker_samples")
        avg_bytes = (cs["storageSize"] / cs["count"]) if cs.get("count") else 150.0
    except Exception:
        avg_bytes = 150.0  # fallback estimate

    # deletedAt=None matches both missing field and explicit null — both mean "active".
    filter_q = {} if include_deleted else {"deletedAt": None}
    cur = app.state.sessions.find(filter_q, sort=[("lastIngestMs", -1)], limit=limit)
    result = []
    for d in cur:
        samples = d.get("samples") or 0
        result.append({
            "sessionId":     d.get("sessionId"),
            "displayName":   d.get("displayName"),
            "deviceId":      d.get("deviceId"),
            "trackerId":     d.get("trackerId"),
            "firmware":      d.get("firmware"),
            "samples":       samples,
            "sizeBytes":     round(samples * avg_bytes),
            "firstTsMs":     d.get("firstTsMs"),
            "lastTsMs":      d.get("lastTsMs"),
            "firstIngestMs": d.get("firstIngestMs"),
            "lastIngestMs":  d.get("lastIngestMs"),
            "uploads":       d.get("uploads", 1),
            "deletedAt":     d.get("deletedAt"),
            "deletedBy":     d.get("deletedBy"),
        })
    return result


@app.get("/sessions/{session_id}")
def session_detail(session_id: str, limit: int = 5000, skip: int = 0):
    """Return raw samples for a session (paged).  Default page size raised to
    5000 to match the viewer's fetch page size and reduce round-trips."""
    cur = (app.state.samples
           .find({"sessionId": session_id}, sort=[("timestampMs", 1)])
           .skip(skip).limit(limit))
    rows = []
    for d in cur:
        d["_id"] = str(d["_id"])
        rows.append(d)
    return {"sessionId": session_id, "skip": skip, "limit": limit, "rows": rows}


# ---- session management routes --------------------------------------------

class RenameBody(BaseModel):
    displayName: str


class MergeBody(BaseModel):
    sourceIds: list[str]
    targetId:  str


class BulkExportBody(BaseModel):
    ids:    list[str]
    format: str = "radiacode"


def _session_to_radiacode_csv(rows: list[dict]) -> str:
    """Convert sample rows to the RadiaCode track CSV format.

    The RadiaCode app exports:
        DateTime,DoseRate,DoseRateErr,TotalDose,CountRate,Latitude,Longitude,Accuracy,Comment
        2025-01-15 10:30:00.123,0.0880,0.0088,0.0024,6.1,47.6062,-122.3321,5.20,

    Where:
        DateTime     ISO-like  YYYY-MM-DD HH:MM:SS.mmm (UTC)
        DoseRate     µSv/h
        DoseRateErr  error estimate ≈ DoseRate × 0.10
        TotalDose    cumulative µSv (running integral over the track)
        CountRate    cps
        Latitude     decimal degrees (empty if no GPS)
        Longitude    decimal degrees (empty if no GPS)
        Accuracy     GPS accuracy m — prefers the measured `accuracyM` field;
                     falls back to `hdop * 5.0` (UERE rule) if only HDOP is
                     stored on the sample.
        Comment      empty
    """
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["DateTime", "DoseRate", "DoseRateErr", "TotalDose",
                "CountRate", "Latitude", "Longitude", "Accuracy", "Comment"])
    running_dose = 0.0
    prev_ts = None
    for row in sorted(rows, key=lambda r: r.get("timestampMs", 0)):
        ts_ms = row.get("timestampMs")
        usv   = row.get("uSvPerHour")
        cps   = row.get("cps")
        lat   = row.get("latitude")
        lng   = row.get("longitude")
        hdop  = row.get("hdop")
        acc_m = row.get("accuracyM")

        if ts_ms is None:
            continue
        dt_str = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S.") + f"{(ts_ms % 1000):03d}"

        if usv is not None and prev_ts is not None:
            interval_h = (ts_ms - prev_ts) / 3_600_000
            running_dose += usv * interval_h
        prev_ts = ts_ms

        err  = round(usv * 0.10, 6) if usv is not None else ""
        dose = round(running_dose, 6)
        # Prefer the measured/imported accuracy value; fall back to UERE
        # estimate from HDOP only if accuracyM is absent on this row.
        if acc_m is not None:
            acc = round(acc_m, 2)
        elif hdop is not None:
            acc = round(hdop * 5.0, 2)
        else:
            acc = ""

        lat_s = f"{lat:.6f}" if lat is not None and not (lat == 0 and lng == 0) else ""
        lng_s = f"{lng:.6f}" if lng is not None and not (lat == 0 and lng == 0) else ""
        if not lat_s:
            acc = ""

        w.writerow([
            dt_str,
            f"{usv:.6f}" if usv is not None else "",
            err,
            dose,
            f"{cps:.3f}" if cps is not None else "",
            lat_s,
            lng_s,
            acc,
            "",
        ])
    return out.getvalue()


def _session_to_internal_csv(rows: list[dict]) -> str:
    """Reproduce the firmware/tracker CSV format (v0.8.0+ 12-column schema)."""
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["timestampMs", "uSvPerHour", "cps", "latitude", "longitude",
                "deviceId", "speedKph", "bearingDeg", "altitudeM", "hdop",
                "event", "accuracyM"])
    for row in sorted(rows, key=lambda r: r.get("timestampMs", 0)):
        w.writerow([
            row.get("timestampMs", ""),
            row.get("uSvPerHour",  ""),
            row.get("cps",         ""),
            row.get("latitude",    ""),
            row.get("longitude",   ""),
            row.get("deviceId",    ""),
            row.get("speedKph",    ""),
            row.get("bearingDeg",  ""),
            row.get("altitudeM",   ""),
            row.get("hdop",        ""),
            row.get("event",       ""),
            row.get("accuracyM",   ""),
        ])
    return out.getvalue()


# Windows FILETIME epoch offset: 100-nanosecond ticks between 1601-01-01 and 1970-01-01.
# Used to produce Timestamp values matching the RadiaCode app's native .txt export.
_FILETIME_EPOCH_DIFF = 116_444_736_000_000_000


def _ms_to_filetime(ms: int) -> int:
    """Convert Unix epoch milliseconds to Windows FILETIME (100-ns ticks since 1601-01-01 UTC)."""
    return ms * 10_000 + _FILETIME_EPOCH_DIFF


def _fmt_num(v) -> str:
    """Format a float with minimal decimal places, matching RadiaCode's native style.
    e.g. 5.08 -> '5.08', 100.0 -> '100', 3.5 -> '3.5'
    """
    if v is None:
        return ""
    # Convert to float, strip trailing zeros after decimal point.
    s = f"{float(v):.10g}"  # up to 10 significant digits, no trailing zeros
    return s


def _has_gps(row: dict) -> bool:
    lat = row.get("latitude")
    lng = row.get("longitude")
    return (
        lat is not None and lng is not None
        and not (lat == 0 and lng == 0)
    )


def _session_to_radiacode_txt(rows: list[dict], gps_only: bool = False) -> str:
    """Produce a RadiaCode native .txt format (tab-separated with FILETIME timestamps).

    This exactly matches the format exported by the RadiaCode Android app:

      Track: YYYY-MM-DD HH-MM-SS<TAB><device><TAB> <TAB>EC
      Timestamp<TAB>Time<TAB>Latitude<TAB>Longitude<TAB>Accuracy<TAB>DoseRate<TAB>CountRate<TAB>Comment
      <FILETIME><TAB>YYYY-MM-DD HH:MM:SS<TAB>lat<TAB>lng<TAB>acc<TAB>dose<TAB>cps<TAB> 

    Timestamp: Windows FILETIME (100-nanosecond ticks since 1601-01-01 UTC).
    Time:      UTC datetime string YYYY-MM-DD HH:MM:SS.
    Accuracy:  metres (prefers measured accuracyM; falls back to hdop*5.0).
    Event-only rows (no dose/GPS data) are skipped — they have no meaning in this format.
    If gps_only is True, rows without a valid lat/lng are also dropped.
    """
    sorted_rows = sorted(rows, key=lambda r: r.get("timestampMs", 0))
    if gps_only:
        sorted_rows = [r for r in sorted_rows if _has_gps(r)]

    # Track header: use first row timestamp for the track date.
    if sorted_rows:
        first_ms = sorted_rows[0].get("timestampMs", 0)
        track_dt = datetime.fromtimestamp(first_ms / 1000, tz=timezone.utc)
        track_date_str = track_dt.strftime("%Y-%m-%d %H-%M-%S")
    else:
        track_date_str = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d %H-%M-%S")

    # Device name: derive from the deviceId field on the first row that has one.
    device_name = "Radmap"
    for r in sorted_rows:
        dev = r.get("deviceId")
        if dev and len(dev) >= 6:
            device_name = f"RC-{dev[-6:]}"
            break
        elif dev:
            device_name = dev
            break

    out = io.StringIO()
    out.write(f"Track: {track_date_str}\t{device_name}\t \tEC\n")
    out.write("Timestamp\tTime\tLatitude\tLongitude\tAccuracy\tDoseRate\tCountRate\tComment\n")

    for row in sorted_rows:
        ts_ms = row.get("timestampMs")
        usv   = row.get("uSvPerHour")
        cps   = row.get("cps")
        lat   = row.get("latitude")
        lng   = row.get("longitude")
        hdop  = row.get("hdop")
        acc_m = row.get("accuracyM")

        if ts_ms is None:
            continue
        # Skip event-only rows (GPS_LOST / GPS_REGAINED markers with no dose data).
        if usv is None and cps is None:
            continue

        filetime = _ms_to_filetime(ts_ms)
        time_str = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

        if acc_m is not None:
            acc = _fmt_num(acc_m)
        elif hdop is not None:
            acc = _fmt_num(hdop * 5.0)
        else:
            acc = ""

        lat_s  = _fmt_num(lat) if lat is not None and not (lat == 0 and (lng or 0) == 0) else ""
        lng_s  = _fmt_num(lng) if lng is not None and not (lat == 0 and (lng or 0) == 0) else ""
        dose_s = _fmt_num(usv) if usv is not None else ""
        cps_s  = _fmt_num(cps) if cps is not None else ""

        out.write(f"{filetime}\t{time_str}\t{lat_s}\t{lng_s}\t{acc}\t{dose_s}\t{cps_s}\t \n")

    return out.getvalue()


def _split_to_parts(content: str, max_bytes: int, header_line_count: int) -> list[str]:
    """Split newline-delimited content into chunks where each chunk (including
    its header re-attached) fits within max_bytes.  The first header_line_count
    lines are treated as header and prepended to every chunk.

    Returns a list of chunk strings (each already includes the header).
    """
    lines = content.split("\n")
    header_lines = lines[:header_line_count]
    data_lines   = [l for l in lines[header_line_count:] if l]  # skip blank trailing lines
    header_text  = "\n".join(header_lines) + "\n"
    header_bytes = len(header_text.encode("utf-8"))

    parts: list[str] = []
    current: list[str] = []
    current_bytes = header_bytes

    for line in data_lines:
        line_bytes = len((line + "\n").encode("utf-8"))
        if current_bytes + line_bytes > max_bytes and current:
            parts.append(header_text + "\n".join(current) + "\n")
            current = [line]
            current_bytes = header_bytes + line_bytes
        else:
            current.append(line)
            current_bytes += line_bytes

    if current:
        parts.append(header_text + "\n".join(current) + "\n")

    return parts if parts else [content]


def _fetch_all_rows(samples_coll, session_id: str) -> list[dict]:
    return list(samples_coll.find(
        {"sessionId": session_id},
        sort=[("timestampMs", 1)],
        projection={"_id": 0},
    ))


@app.patch("/sessions/{session_id}")
def rename_session(session_id: str, body: RenameBody):
    """Set a human-readable display name for a session.
    The internal sessionId (sample foreign key) is never changed.
    """
    name = body.displayName.strip()
    if not name:
        raise HTTPException(status_code=400, detail="displayName must not be empty")
    result = app.state.sessions.update_one(
        {"sessionId": session_id},
        {"$set": {"displayName": name}},
    )
    if result.matched_count == 0:
        raise HTTPException(status_code=404, detail=f"session {session_id!r} not found")
    log.info("renamed session %s -> displayName=%r", session_id, name)
    return {"sessionId": session_id, "displayName": name}


@app.delete("/sessions/{session_id}")
def delete_session(session_id: str, confirm: str = Query(default="")):
    """Soft-delete a session.  Sets deletedAt/deletedBy; samples are NOT removed.

    Requires confirm=DELETE_CONFIRMED.  The session can be fully restored at any
    time via PATCH /sessions/{id}/restore.  For permanent removal use
    POST /admin/purge/{id} (session must already be soft-deleted).
    """
    if confirm != "DELETE_CONFIRMED":
        raise HTTPException(
            status_code=400,
            detail="Pass ?confirm=DELETE_CONFIRMED to confirm soft-delete.",
        )
    now_ms = int(time.time() * 1000)
    result = app.state.sessions.update_one(
        {"sessionId": session_id, "deletedAt": None},
        {"$set": {"deletedAt": now_ms, "deletedBy": "web-ui"}},
    )
    if result.matched_count == 0:
        existing = app.state.sessions.find_one(
            {"sessionId": session_id}, projection={"deletedAt": 1}
        )
        if not existing:
            raise HTTPException(status_code=404, detail=f"session {session_id!r} not found")
        raise HTTPException(
            status_code=409,
            detail=f"session {session_id!r} is already soft-deleted",
        )
    log.warning("soft-deleted session %s", session_id)
    return {"softDeleted": session_id, "deletedAt": now_ms}


@app.patch("/sessions/{session_id}/restore")
def restore_session(session_id: str):
    """Restore a soft-deleted session.  Clears deletedAt and deletedBy.
    Samples are still in the database untouched — nothing was ever removed.
    """
    result = app.state.sessions.update_one(
        {"sessionId": session_id, "deletedAt": {"$ne": None}},
        {"$unset": {"deletedAt": "", "deletedBy": ""}},
    )
    if result.matched_count == 0:
        existing = app.state.sessions.find_one({"sessionId": session_id})
        if not existing:
            raise HTTPException(status_code=404, detail=f"session {session_id!r} not found")
        raise HTTPException(
            status_code=409,
            detail=f"session {session_id!r} is not soft-deleted; nothing to restore",
        )
    log.info("restored session %s", session_id)
    return {"restored": session_id}


@app.post("/admin/purge/{session_id}")
def purge_session(session_id: str, confirm: str = Query(default="")):
    """Permanently purge a soft-deleted session and ALL its samples.

    Requires confirm=PURGE_CONFIRMED.  The session must already be soft-deleted
    (deletedAt must be set) — this two-step requirement means accidental purges
    require at minimum two separate API calls.
    """
    if confirm != "PURGE_CONFIRMED":
        raise HTTPException(
            status_code=400,
            detail="Pass ?confirm=PURGE_CONFIRMED to confirm permanent purge.",
        )
    existing = app.state.sessions.find_one({"sessionId": session_id})
    if not existing:
        raise HTTPException(status_code=404, detail=f"session {session_id!r} not found")
    if not existing.get("deletedAt"):
        raise HTTPException(
            status_code=409,
            detail=(
                f"session {session_id!r} must be soft-deleted first."
                " Call DELETE /sessions/{id} before purging."
            ),
        )
    samples_del = app.state.samples.delete_many({"sessionId": session_id})
    app.state.sessions.delete_one({"sessionId": session_id})
    log.warning("PURGED session %s (%d samples removed)", session_id, samples_del.deleted_count)
    return {
        "purged":         session_id,
        "samplesRemoved": samples_del.deleted_count,
    }


@app.post("/admin/merge-sessions")
def merge_sessions(body: MergeBody):
    """Merge one or more source sessions into a target session.

    Samples from each sourceId are re-tagged with targetId and bulk-inserted
    into the samples collection (duplicates are skipped via unique index).
    Source sessions + their samples are then removed.

    The target session metadata is recomputed from the merged samples.
    """
    if not body.sourceIds:
        raise HTTPException(status_code=400, detail="sourceIds must not be empty")
    if body.targetId in body.sourceIds:
        raise HTTPException(status_code=400, detail="targetId must not appear in sourceIds")

    samples = app.state.samples
    sessions = app.state.sessions

    # Ensure target session exists (upsert a skeleton if needed).
    now_ms = int(time.time() * 1000)
    sessions.update_one(
        {"sessionId": body.targetId},
        {"$setOnInsert": {
            "sessionId":     body.targetId,
            "createdMs":     now_ms,
            "lastIngestMs":  now_ms,
            "firstIngestMs": now_ms,
        }},
        upsert=True,
    )

    total_moved = 0
    for src_id in body.sourceIds:
        src_rows = list(samples.find({"sessionId": src_id}, projection={"_id": 0}))
        # Re-tag all rows with targetId.
        for r in src_rows:
            r["sessionId"] = body.targetId
        inserted, _ = _bulk_insert(samples, src_rows)
        total_moved += inserted
        # Delete source samples + session record.
        samples.delete_many({"sessionId": src_id})
        sessions.delete_one({"sessionId": src_id})
        log.info("merge: moved %d rows from %s -> %s", inserted, src_id, body.targetId)

    # Recompute target session metadata.
    agg = list(samples.aggregate([
        {"$match":  {"sessionId": body.targetId, "timestampMs": {"$gte": MIN_VALID_TS_MS}}},
        {"$group":  {
            "_id":       None,
            "count":     {"$sum": 1},
            "firstTsMs": {"$min": "$timestampMs"},
            "lastTsMs":  {"$max": "$timestampMs"},
        }},
    ]))
    if agg:
        r = agg[0]
        sessions.update_one(
            {"sessionId": body.targetId},
            {"$set": {
                "samples":      r["count"],
                "firstTsMs":    r["firstTsMs"],
                "lastTsMs":     r["lastTsMs"],
                "lastIngestMs": now_ms,
            }},
        )

    log.info("merge complete: target=%s sources=%s totalMoved=%d",
             body.targetId, body.sourceIds, total_moved)
    return {
        "targetId":   body.targetId,
        "merged":     body.sourceIds,
        "totalMoved": total_moved,
    }


@app.get("/sessions/{session_id}/export")
def export_session(session_id: str, format: str = Query(default="radiacode")):
    """Download session data as a CSV file.

    format=radiacode  — RadiaCode track CSV (compatible with official app / upload sites)
    format=internal   — original firmware schema (timestampMs,uSvPerHour,cps,...)
    """
    rows = _fetch_all_rows(app.state.samples, session_id)
    if not rows:
        raise HTTPException(status_code=404, detail=f"session {session_id!r} not found or empty")

    fmt = format.lower()
    if fmt == "radiacode":
        body = _session_to_radiacode_csv(rows)
        filename = f"{session_id}_radiacode.csv"
    elif fmt == "internal":
        body = _session_to_internal_csv(rows)
        filename = f"{session_id}_internal.csv"
    else:
        raise HTTPException(status_code=400, detail=f"unknown format {format!r}; use radiacode or internal")

    return StreamingResponse(
        iter([body]),
        media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


@app.post("/sessions/export-bulk")
async def export_bulk(request: Request):
    """Download multiple sessions merged into a single CSV file.

    Body JSON: { "ids": ["id1", "id2", ...], "format": "radiacode" }
    The rows are sorted chronologically across all selected sessions.
    """
    body = await request.json()
    ids    = body.get("ids", [])
    fmt    = body.get("format", "radiacode").lower()

    if not ids:
        raise HTTPException(status_code=400, detail="ids must not be empty")

    all_rows: list[dict] = []
    for sid in ids:
        all_rows.extend(_fetch_all_rows(app.state.samples, sid))

    if not all_rows:
        raise HTTPException(status_code=404, detail="no rows found for provided ids")

    if fmt == "radiacode":
        content  = _session_to_radiacode_csv(all_rows)
        filename = "bulk_export_radiacode.csv"
    elif fmt == "internal":
        content  = _session_to_internal_csv(all_rows)
        filename = "bulk_export_internal.csv"
    else:
        raise HTTPException(status_code=400, detail=f"unknown format {fmt!r}")

    return StreamingResponse(
        iter([content]),
        media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


@app.post("/admin/recompute-sessions")
def recompute_sessions():
    """Recompute firstTsMs, lastTsMs, and samples for every session from actual
    DB sample data (only counting rows with timestampMs >= MIN_VALID_TS_MS).
    Also purges any sample rows with pre-2020 timestamps.

    Call this once after upgrading the API to clear up sessions that were
    poisoned by millis()-since-boot timestamps from old firmware.
    """
    samples = app.state.samples
    sessions_coll = app.state.sessions

    # 1. Delete all pre-2020 sample rows.
    del_result = samples.delete_many({"timestampMs": {"$lt": MIN_VALID_TS_MS}})
    purged = del_result.deleted_count
    log.info("recompute-sessions: purged %d pre-2020 sample rows", purged)

    # 2. Recompute per-session stats from remaining samples.
    # v0.5.1: explicit $sort before $group so $last is deterministic (without
    # a sort, $last picks whichever document the storage engine yields last,
    # which is undefined). Also write deviceId/trackerId/firmware back to
    # session metadata -- the previous version computed them but discarded.
    pipeline = [
        {"$match": {"timestampMs": {"$gte": MIN_VALID_TS_MS}}},
        {"$sort":  {"sessionId": 1, "timestampMs": 1}},
        {"$group": {
            "_id":       "$sessionId",
            "samples":   {"$sum": 1},
            "firstTsMs": {"$min": "$timestampMs"},
            "lastTsMs":  {"$max": "$timestampMs"},
            "deviceId":  {"$last": "$deviceId"},
            "trackerId": {"$last": "$trackerId"},
            "firmware":  {"$last": "$firmware"},
        }},
    ]
    updated = 0
    for row in samples.aggregate(pipeline, allowDiskUse=True):
        sid = row["_id"]
        update_set = {
            "samples":   row["samples"],
            "firstTsMs": row["firstTsMs"],
            "lastTsMs":  row["lastTsMs"],
        }
        # Only overwrite identity fields when aggregation produced a value;
        # otherwise leave whatever the ingest path wrote (e.g. trackerId
        # from the X-Tracker-Id header may be the only source for sessions
        # with no per-row trackerId).
        if row.get("deviceId"):
            update_set["deviceId"]  = row["deviceId"]
        if row.get("trackerId"):
            update_set["trackerId"] = row["trackerId"]
        if row.get("firmware"):
            update_set["firmware"]  = row["firmware"]
        sessions_coll.update_one(
            {"sessionId": sid},
            {"$set": update_set},
            upsert=False,
        )
        updated += 1
        log.info("recompute-sessions: %s -> samples=%d firstTs=%d lastTs=%d",
                 sid, row["samples"], row["firstTsMs"], row["lastTsMs"])

    return {
        "purgedSampleRows": purged,
        "sessionsUpdated":  updated,
        "minValidTsMs":     MIN_VALID_TS_MS,
    }


@app.post("/admin/backfill-accuracy")
def backfill_accuracy():
    """Fill missing accuracyM <-> hdop pairs on every sample row.

    Uses the canonical UERE rule of thumb: accuracyM = hdop * 5.0.

    Two passes:
      1. Rows that have `hdop` but no `accuracyM` -> compute accuracyM = hdop*5.0,
         tag with accEstimated=true.
      2. Rows that have `accuracyM` but no `hdop` -> compute hdop = accuracyM/5.0,
         tag with hdopEstimated=true.

    Rows that already have both fields are left untouched. Rows that have
    neither (e.g. event marker rows, indoors-no-GPS rows) are also untouched.

    The *Estimated booleans let the viewer / downstream consumers tell
    measured from derived values.
    """
    samples_coll = app.state.samples

    UERE = 5.0  # cfg::GPS_UERE_M in firmware; canonical across the stack.

    # Pass 1: derive accuracyM from hdop.
    res_acc = samples_coll.update_many(
        {
            "hdop":      {"$exists": True, "$ne": None},
            "accuracyM": {"$exists": False},
        },
        [{"$set": {
            "accuracyM":    {"$multiply": ["$hdop", UERE]},
            "accEstimated": True,
        }}],
    )

    # Pass 2: derive hdop from accuracyM.
    res_hdop = samples_coll.update_many(
        {
            "accuracyM": {"$exists": True, "$ne": None},
            "hdop":      {"$exists": False},
        },
        [{"$set": {
            "hdop":          {"$divide": ["$accuracyM", UERE]},
            "hdopEstimated": True,
        }}],
    )

    log.info("backfill-accuracy: accuracyM filled on %d rows, hdop filled on %d rows (UERE=%.1f)",
             res_acc.modified_count, res_hdop.modified_count, UERE)

    return {
        "accuracyMFilled": res_acc.modified_count,
        "hdopFilled":      res_hdop.modified_count,
        "uereMeters":      UERE,
    }


@app.post("/admin/migrate-to-daily-sessions")
def migrate_to_daily_sessions(confirm: str = ""):
    """One-shot migration to the firmware v0.4.0+ day-bucketed schema.

    Re-keys every sample row so its sessionId is the local-eastern
    YYYY-MM-DD string of its timestampMs (using the America/New_York
    timezone, which honors DST automatically). Drops the
    {sessionId,timestampMs} unique index for the duration of the rewrite,
    deduplicates any rows that collide on the new key, recreates the
    unique index, then rebuilds the tracker_sessions metadata collection.

    Idempotent: safe to re-run after a partial failure. Old sessionIds
    that already match `^\\d{4}-\\d{2}-\\d{2}$` are untouched.

    Guard: ?confirm=MIGRATE_CONFIRMED is required.
    """
    if confirm != "MIGRATE_CONFIRMED":
        raise HTTPException(
            status_code=400,
            detail="confirm=MIGRATE_CONFIRMED query parameter required",
        )

    samples       = app.state.samples
    sessions_coll = app.state.sessions

    # 1. Purge anything before 2020 - those rows have no usable date anyway
    #    and would create bogus sessionIds like "1970-01-01".
    pre2020 = samples.delete_many({"timestampMs": {"$lt": MIN_VALID_TS_MS}})
    log.info("migrate: purged %d pre-2020 sample rows", pre2020.deleted_count)

    # 2. Drop the unique index. Updating sessionId in-place would otherwise
    #    fail on every row whose new key collides with an existing row in
    #    a different (old) session - we'll dedupe explicitly in step 4.
    try:
        samples.drop_index("session_ts_unique")
        log.info("migrate: dropped session_ts_unique index")
    except Exception as e:
        log.info("migrate: drop_index skipped (%s)", e)

    # 3. Rewrite sessionId on every sample using a server-side aggregation
    #    update so we don't pull the whole collection through Python.
    upd = samples.update_many(
        {},
        [
            {
                "$set": {
                    "sessionId": {
                        "$dateToString": {
                            "format":   "%Y-%m-%d",
                            "date":     {"$toDate": "$timestampMs"},
                            "timezone": "America/New_York",
                        }
                    }
                }
            }
        ],
    )
    log.info("migrate: rewrote sessionId on %d sample rows", upd.modified_count)

    # 4. Deduplicate rows that now collide on {sessionId,timestampMs}.
    #    For each duplicate group we keep the first _id and delete the rest.
    dedup_pipeline = [
        {"$group": {
            "_id":  {"sessionId": "$sessionId", "timestampMs": "$timestampMs"},
            "ids":  {"$push": "$_id"},
            "n":    {"$sum": 1},
        }},
        {"$match": {"n": {"$gt": 1}}},
    ]
    removed_dupes = 0
    for grp in samples.aggregate(dedup_pipeline, allowDiskUse=True):
        ids = grp["ids"]
        # keep ids[0], delete the rest
        r = samples.delete_many({"_id": {"$in": ids[1:]}})
        removed_dupes += r.deleted_count
    log.info("migrate: removed %d duplicate sample rows", removed_dupes)

    # 5. Recreate the unique index.
    samples.create_index(
        [("sessionId", 1), ("timestampMs", 1)],
        name="session_ts_unique",
        unique=True,
    )
    log.info("migrate: recreated session_ts_unique index")

    # 6. Drop legacy session metadata documents whose ids don't match the
    #    new YYYY-MM-DD format. The recompute pass below will rebuild them.
    legacy = sessions_coll.delete_many(
        {"sessionId": {"$not": {"$regex": r"^\d{4}-\d{2}-\d{2}$"}}}
    )
    log.info("migrate: dropped %d legacy session metadata rows", legacy.deleted_count)

    # 7. Rebuild tracker_sessions metadata from the now-renamed samples.
    # v0.5.1: explicit $sort so $last is deterministic.
    pipeline = [
        {"$match": {"timestampMs": {"$gte": MIN_VALID_TS_MS}}},
        {"$sort":  {"sessionId": 1, "timestampMs": 1}},
        {"$group": {
            "_id":       "$sessionId",
            "samples":   {"$sum": 1},
            "firstTsMs": {"$min": "$timestampMs"},
            "lastTsMs":  {"$max": "$timestampMs"},
            "deviceId":  {"$last": "$deviceId"},
            "trackerId": {"$last": "$trackerId"},
            "firmware":  {"$last": "$firmware"},
        }},
    ]
    rebuilt = 0
    for row in samples.aggregate(pipeline, allowDiskUse=True):
        sid = row["_id"]
        sessions_coll.update_one(
            {"sessionId": sid},
            {"$set": {
                "sessionId": sid,
                "samples":   row["samples"],
                "firstTsMs": row["firstTsMs"],
                "lastTsMs":  row["lastTsMs"],
                "deviceId":  row.get("deviceId"),
                "trackerId": row.get("trackerId"),
                "firmware":  row.get("firmware"),
            }},
            upsert=True,
        )
        rebuilt += 1
        log.info("migrate: rebuilt session %s samples=%d firstTs=%d lastTs=%d",
                 sid, row["samples"], row["firstTsMs"], row["lastTsMs"])

    return {
        "purgedPre2020":      pre2020.deleted_count,
        "sampleRowsUpdated":  upd.modified_count,
        "duplicatesRemoved":  removed_dupes,
        "legacySessionsDropped": legacy.deleted_count,
        "sessionsRebuilt":    rebuilt,
        "timezone":           "America/New_York",
        "apiVersion":         API_VERSION,
    }


# ---- database stats --------------------------------------------------------

@app.get("/admin/db-stats")
def db_stats():
    """Return storage-level metrics for the radiacode database.

    Sizes are in bytes.  storageSize is the actual on-disk WiredTiger footprint
    (after compression); dataSize is the uncompressed BSON size.
    """
    db = app.state.db
    try:
        dbs = db.command("dbStats")
        ss  = db.command("collStats", "tracker_samples")
        qs  = db.command("collStats", "tracker_sessions")
    except Exception as e:
        raise HTTPException(status_code=503, detail=f"mongo error: {e}")

    avg_sample_bytes = (ss["storageSize"] / ss["count"]) if ss.get("count") else 0

    return {
        "db":              MONGO_DB,
        "dbDataSize":      dbs.get("dataSize", 0),
        "dbStorageSize":   dbs.get("storageSize", 0),
        "dbIndexSize":     dbs.get("indexSize", 0),
        "dbObjects":       dbs.get("objects", 0),
        "samples": {
            "count":        ss.get("count", 0),
            "dataSize":     ss.get("size", 0),
            "storageSize":  ss.get("storageSize", 0),
            "avgDocBytes":  round(avg_sample_bytes, 1),
        },
        "sessions": {
            "count":        qs.get("count", 0),
            "dataSize":     qs.get("size", 0),
            "storageSize":  qs.get("storageSize", 0),
        },
        "backupDir":       BACKUP_DIR,
        "mongodumpAvail":  shutil.which("mongodump") is not None,
        "keepCount":       BACKUP_KEEP_COUNT,
        "lastBackup":      (lambda d: {
            "name":       d.get("name"),
            "tsMs":       d.get("tsMs"),
            "source":     d.get("source"),
            "status":     d.get("status"),
            "sizeBytes":  d.get("sizeBytes"),
            "elapsedSec": d.get("elapsedSec"),
        } if d else None)(
            app.state.backups.find_one(
                {"status": "success"},
                sort=[("tsMs", -1)],
                projection={"_id": 0},
            )
        ),
    }


# ---- backup helpers --------------------------------------------------------

def _backup_path(name: str) -> str:
    """Resolve backup directory safely — rejects path traversal attempts."""
    if "/" in name or "\\" in name or ".." in name or not name:
        raise HTTPException(status_code=400, detail="invalid backup name")
    return os.path.join(BACKUP_DIR, name)


def _dir_size(path: str) -> int:
    """Recursively sum file sizes under a directory."""
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def _record_backup(backups_coll, name: str, ts_ms: int, size_bytes: int,
                   elapsed_sec: float, source: str, status: str) -> None:
    """Upsert a backup telemetry record into tracker_backups."""
    try:
        backups_coll.update_one(
            {"name": name},
            {"$set": {
                "name":       name,
                "tsMs":       ts_ms,
                "sizeBytes":  size_bytes,
                "elapsedSec": elapsed_sec,
                "source":     source,   # "manual" | "cron"
                "status":     status,   # "success" | "failed"
                "scope":      "all",    # full-database dump (no --db filter)
            }},
            upsert=True,
        )
    except Exception as e:
        log.warning("backup: failed to write telemetry to mongo: %s", e)


def _list_backups():
    os.makedirs(BACKUP_DIR, exist_ok=True)
    entries = []
    for path in sorted(glob.glob(os.path.join(BACKUP_DIR, "*")), reverse=True):
        if not os.path.isdir(path):
            continue
        name = os.path.basename(path)
        total = _dir_size(path)
        ts_ms = None
        try:
            dt = datetime.strptime(name, "%Y-%m-%d_%H-%M-%S")
            ts_ms = int(dt.replace(tzinfo=timezone.utc).timestamp() * 1000)
        except ValueError:
            pass
        entries.append({"name": name, "sizeBytes": total, "tsMs": ts_ms})
    return entries


@app.get("/admin/backups")
def list_backups():
    """List all available mongodump backup snapshots in BACKUP_DIR.

    Each entry is enriched with telemetry from tracker_backups (source,
    status, elapsedSec) when available.  Backups created before telemetry
    was introduced show source='unknown'.
    """
    fs_entries = _list_backups()

    # Enrich with DB telemetry (source, status, elapsed).
    try:
        telem_map = {
            doc["name"]: doc
            for doc in app.state.backups.find(
                {"name": {"$in": [e["name"] for e in fs_entries]}},
                projection={"_id": 0},
            )
        }
    except Exception as e:
        log.warning("list_backups: telemetry fetch failed: %s", e)
        telem_map = {}

    for entry in fs_entries:
        telem = telem_map.get(entry["name"])
        if telem:
            entry["source"]     = telem.get("source", "unknown")
            entry["status"]     = telem.get("status", "success")
            entry["elapsedSec"] = telem.get("elapsedSec")
        else:
            # Pre-telemetry backup — directory exists but no DB record.
            entry["source"]     = "unknown"
            entry["status"]     = "success"
            entry["elapsedSec"] = None

    return {
        "backupDir":      BACKUP_DIR,
        "backups":        fs_entries,
        "mongodumpAvail": shutil.which("mongodump") is not None,
        "keepCount":      BACKUP_KEEP_COUNT,
    }


@app.post("/admin/backup")
def create_backup(source: str = Query(default="manual")):
    """Trigger a mongodump snapshot of ALL databases.

    Dumps every non-local database (radiacode, admin, etc.) to
    BACKUP_DIR/<YYYY-MM-DD_HH-MM-SS>/ with gzip compression.

    ?source=manual (default) — triggered from the web UI
    ?source=cron             — triggered by the weekly cron job

    Each attempt is recorded in tracker_backups for audit/telemetry.
    """
    # Sanitise source to a known value.
    if source not in ("manual", "cron"):
        source = "manual"

    if not shutil.which("mongodump"):
        raise HTTPException(status_code=501, detail="mongodump is not installed in this container")

    ts_name = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d_%H-%M-%S")
    dest    = os.path.join(BACKUP_DIR, ts_name)
    ts_ms   = int(time.time() * 1000)
    os.makedirs(BACKUP_DIR, exist_ok=True)

    # No --db flag — dumps ALL databases (radiacode, admin, ...) excluding local.
    cmd = [
        "mongodump",
        f"--uri={MONGO_URI}",
        f"--out={dest}",
        "--gzip",
    ]
    log.info("backup: starting full mongodump (source=%s) -> %s", source, dest)
    t0 = time.monotonic()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        shutil.rmtree(dest, ignore_errors=True)
        _record_backup(app.state.backups, ts_name, ts_ms, 0, 300.0, source, "failed")
        raise HTTPException(status_code=504, detail="mongodump timed out after 300s")

    elapsed = round(time.monotonic() - t0, 2)

    if result.returncode != 0:
        shutil.rmtree(dest, ignore_errors=True)
        log.error("backup failed (source=%s): %s", source, result.stderr)
        _record_backup(app.state.backups, ts_name, ts_ms, 0, elapsed, source, "failed")
        raise HTTPException(status_code=500, detail=f"mongodump failed: {result.stderr[:400]}")

    total = _dir_size(dest)
    _record_backup(app.state.backups, ts_name, ts_ms, total, elapsed, source, "success")
    log.info("backup: completed %s source=%s in %.2fs size=%d bytes",
             ts_name, source, elapsed, total)
    return {
        "backup":     ts_name,
        "sizeBytes":  total,
        "elapsedSec": elapsed,
        "source":     source,
        "dest":       dest,
    }


@app.delete("/admin/backup/{backup_name}")
def delete_backup(backup_name: str, confirm: str = Query(default="")):
    """Permanently delete a backup directory.  Requires confirm=DELETE_CONFIRMED."""
    if confirm != "DELETE_CONFIRMED":
        raise HTTPException(status_code=400,
                            detail="Pass ?confirm=DELETE_CONFIRMED to confirm deletion.")
    path = _backup_path(backup_name)
    if not os.path.isdir(path):
        raise HTTPException(status_code=404, detail=f"backup {backup_name!r} not found")
    shutil.rmtree(path)
    log.warning("DELETED backup %s", path)
    return {"deleted": backup_name}


@app.post("/admin/restore/{backup_name}")
def restore_backup(backup_name: str, confirm: str = Query(default="")):
    """Restore a full mongodump backup (ALL databases).

    WARNING: uses --drop which drops each collection before restoring.  This
    DESTROYS all current data in every database in the dump.
    Requires confirm=RESTORE_CONFIRMED.
    """
    if confirm != "RESTORE_CONFIRMED":
        raise HTTPException(status_code=400,
                            detail="Pass ?confirm=RESTORE_CONFIRMED to confirm restore.")
    if not shutil.which("mongorestore"):
        raise HTTPException(status_code=501, detail="mongorestore is not installed")

    path = _backup_path(backup_name)
    if not os.path.isdir(path):
        raise HTTPException(status_code=404, detail=f"backup {backup_name!r} not found")

    # Restore from the backup root — mongorestore recurses into per-db subdirectories.
    # No --db flag so every database in the dump is restored.
    cmd = [
        "mongorestore",
        f"--uri={MONGO_URI}",
        "--drop",    # drop existing collections before restore
        "--gzip",
        path,
    ]
    log.warning("restore: starting full mongorestore from %s (--drop)", path)
    t0 = time.monotonic()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail="mongorestore timed out after 300s")

    elapsed = round(time.monotonic() - t0, 2)
    if result.returncode != 0:
        log.error("restore failed: %s", result.stderr)
        raise HTTPException(status_code=500, detail=f"mongorestore failed: {result.stderr[:400]}")

    log.warning("restore: completed from %s in %.2fs", backup_name, elapsed)
    return {
        "restored":   backup_name,
        "elapsedSec": elapsed,
        "stdout":     result.stdout[-500:] if result.stdout else "",
    }


@app.post("/ingest/csv")
async def ingest_csv(
    request: Request,
    x_session_id: str = Header(..., alias="X-Session-Id"),
    x_device_id:  str | None = Header(None, alias="X-Device-Id"),
    x_tracker_id: str | None = Header(None, alias="X-Tracker-Id"),
    x_firmware:   str | None = Header(None, alias="X-Firmware"),
):
    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="empty body")
    if len(body) > MAX_BODY_BYTES:
        raise HTTPException(status_code=413, detail=f"body > {MAX_BODY_BYTES} bytes")

    log.info("ingest request sessionId=%s tracker=%s firmware=%s bodyBytes=%d",
             x_session_id, x_tracker_id, x_firmware, len(body))

    text = body.decode("utf-8", errors="replace")
    docs, rejected = _parse_csv(text, x_session_id, x_device_id, x_tracker_id, x_firmware)

    if not docs and rejected > 0:
        log.error("ingest sessionId=%s ALL %d rows rejected (pre-2020 timestamps); "
                  "old firmware artifact -- not inserting", x_session_id, rejected)
        raise HTTPException(
            status_code=400,
            detail=f"All {rejected} rows have pre-2020 timestamps (old firmware artifact). "
                   "Flash updated firmware to stop recording millis()-since-boot as timestamps.",
        )
    if not docs:
        raise HTTPException(status_code=400, detail="no parseable rows")

    inserted, duplicates = _bulk_insert(app.state.samples, docs)

    now_ms = int(time.time() * 1000)
    # first_ts/last_ts computed ONLY from the validated (>= MIN_VALID_TS_MS) docs.
    # This is critical: using $min on raw rows lets one bad row permanently
    # corrupt firstTsMs for the session.
    first_ts = min(d["timestampMs"] for d in docs)
    last_ts  = max(d["timestampMs"] for d in docs)

    # v0.5.1: derive deviceId from CSV row data, not just the X-Device-Id header.
    # Firmware never sends that header -- it embeds the RadiaCode MAC in row
    # column 6 -- so the session-metadata deviceId was always null. Pick the
    # last non-empty value (rows are in timestamp order within the upload).
    derived_device_id = x_device_id
    for d in reversed(docs):
        v = d.get("deviceId")
        if v:
            derived_device_id = v
            break

    # Upsert session metadata.  Use $min/$max only over valid timestamps.
    # NOTE: firstTsMs uses $min which means a subsequent upload with a smaller
    # but still-valid timestamp is fine (correct early boundary).  But we
    # must never let pre-2020 values reach here -- filtered above.
    app.state.sessions.update_one(
        {"sessionId": x_session_id},
        {
            "$set": {
                "sessionId":     x_session_id,
                "deviceId":      derived_device_id,
                "trackerId":     x_tracker_id,
                "firmware":      x_firmware,
                "lastIngestMs":  now_ms,
            },
            "$min": {"firstTsMs": first_ts, "firstIngestMs": now_ms},
            "$max": {"lastTsMs":  last_ts},
            "$inc": {"samples":  inserted, "uploads": 1},
            "$setOnInsert": {"createdMs": now_ms},
        },
        upsert=True,
    )

    log.info(
        "ingest OK sessionId=%s received=%d valid=%d rejected=%d inserted=%d dup=%d "
        "firstTsMs=%d lastTsMs=%d",
        x_session_id, len(docs) + rejected, len(docs), rejected,
        inserted, duplicates, first_ts, last_ts,
    )
    return JSONResponse({
        "sessionId":   x_session_id,
        "received":    len(docs) + rejected,
        "valid":       len(docs),
        "rejected":    rejected,
        "inserted":    inserted,
        "duplicates":  duplicates,
        "firstTsMs":   first_ts,
        "lastTsMs":    last_ts,
    })


@app.post("/ingest/csv-merge")
async def ingest_csv_merge(
    request: Request,
    x_session_id: str = Header(..., alias="X-Session-Id"),
    x_device_id:  str | None = Header(None, alias="X-Device-Id"),
    x_tracker_id: str | None = Header(None, alias="X-Tracker-Id"),
    x_firmware:   str | None = Header(None, alias="X-Firmware"),
):
    """Like /ingest/csv, but performs an UPSERT-MERGE on the
    (sessionId, timestampMs) key instead of insert-or-skip. New columns
    in the CSV are $set onto matching existing docs, so re-ingesting an
    expanded version of historical data (e.g. RadiaCode track files now
    including accuracyM) backfills the new field rather than dropping it.

    Identity fields (sessionId, timestampMs, deviceId, trackerId, firmware,
    loc) are only written on insert -- they are never overwritten on an
    existing doc.

    The firmware uploader should NOT use this endpoint -- it's an admin
    path for one-shot enrichment runs.
    """
    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="empty body")
    if len(body) > MAX_BODY_BYTES:
        raise HTTPException(status_code=413, detail=f"body > {MAX_BODY_BYTES} bytes")

    log.info("ingest-merge sessionId=%s tracker=%s firmware=%s bodyBytes=%d",
             x_session_id, x_tracker_id, x_firmware, len(body))

    text = body.decode("utf-8", errors="replace")
    docs, rejected = _parse_csv(text, x_session_id, x_device_id, x_tracker_id, x_firmware)
    if not docs:
        raise HTTPException(status_code=400, detail="no parseable rows")

    inserted, modified, unchanged = _bulk_upsert_merge(app.state.samples, docs)

    log.info("ingest-merge OK sessionId=%s valid=%d inserted=%d modified=%d unchanged=%d rejected=%d",
             x_session_id, len(docs), inserted, modified, unchanged, rejected)
    return JSONResponse({
        "sessionId":  x_session_id,
        "valid":      len(docs),
        "rejected":   rejected,
        "inserted":   inserted,
        "modified":   modified,
        "unchanged":  unchanged,
    })


# ---- time-range export endpoints ------------------------------------------

@app.get("/export/time-range/preview")
def export_time_range_preview(startMs: int, endMs: int, gpsOnly: bool = False):
    """Return row count and size estimate for a time-range export without fetching data.

    Useful for the frontend to show the user what they'll get before committing
    to the download.  Uses count_documents (fast index scan) rather than fetching rows.
    """
    if startMs >= endMs:
        raise HTTPException(status_code=400, detail="startMs must be < endMs")

    query: dict = {"timestampMs": {"$gte": startMs, "$lte": endMs}}
    if gpsOnly:
        # Only count rows that have a real GPS fix (non-null, non-zero lat+lng).
        query["latitude"]  = {"$exists": True, "$nin": [None, 0.0]}
        query["longitude"] = {"$exists": True, "$nin": [None, 0.0]}

    count = app.state.samples.count_documents(query)

    # Rough byte estimate: ~90 bytes per row for radiacode_txt, ~75 for CSV.
    # Use the larger estimate so we don't surprise users with an unexpected zip.
    est_bytes = count * 90
    max_bytes = 10 * 1024 * 1024  # 10 MB
    # Ceiling division for number of files.
    est_files = max(1, (est_bytes + max_bytes - 1) // max_bytes)

    return {
        "startMs":        startMs,
        "endMs":          endMs,
        "rowCount":       count,
        "estimatedBytes": est_bytes,
        "estimatedMB":    round(est_bytes / (1024 * 1024), 2),
        "estimatedFiles": est_files,
    }


@app.post("/export/time-range")
async def export_time_range(request: Request):
    """Export all samples within a time window, auto-splitting into a ZIP when
    any single file would exceed maxBytesPerFile (default 10 MB).

    Request body (JSON):
        startMs          int   Unix epoch ms, inclusive
        endMs            int   Unix epoch ms, inclusive
        format           str   radiacode_txt | radiacode | internal
        maxBytesPerFile  int   optional, default 10485760 (10 MB)

    Response:
        text/plain       — single .txt file when format=radiacode_txt and data <= limit
        text/csv         — single .csv file when format=radiacode|internal and data <= limit
        application/zip  — ZIP archive with multiple part files when data > limit
    """
    body = await request.json()
    start_ms  = body.get("startMs")
    end_ms    = body.get("endMs")
    fmt       = body.get("format", "radiacode_txt").lower()
    max_bytes = int(body.get("maxBytesPerFile", 10 * 1024 * 1024))
    ui_label  = body.get("label", "")  # optional human-readable range name for filename
    gps_only  = bool(body.get("gpsOnly", False))

    if start_ms is None or end_ms is None:
        raise HTTPException(status_code=400, detail="startMs and endMs are required")
    if start_ms >= end_ms:
        raise HTTPException(status_code=400, detail="startMs must be < endMs")
    if fmt not in ("radiacode_txt", "radiacode_trk", "radiacode", "internal"):
        raise HTTPException(status_code=400,
                            detail=f"unknown format {fmt!r}; use radiacode_txt, radiacode_trk, radiacode, or internal")

    rows = list(app.state.samples.find(
        {"timestampMs": {"$gte": start_ms, "$lte": end_ms}},
        sort=[("timestampMs", 1)],
        projection={"_id": 0},
    ))

    # Filter to GPS-locked rows client-requested (or always for radiacode_txt/radiacode_trk
    # which require coordinates to be meaningful).
    if gps_only or fmt in ("radiacode_txt", "radiacode_trk"):
        rows = [r for r in rows if _has_gps(r) and r.get("uSvPerHour") is not None]

    if not rows:
        raise HTTPException(status_code=404,
                            detail="no data found in the specified time range")

    # Date range label for filenames — always include the actual dates so the file
    # is self-describing regardless of which preset the user picked.
    start_dt_str = datetime.fromtimestamp(start_ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d")
    end_dt_str   = datetime.fromtimestamp(end_ms   / 1000, tz=timezone.utc).strftime("%Y-%m-%d")
    import re as _re
    if start_dt_str == end_dt_str:
        date_slug = start_dt_str          # single day: just one date
    else:
        date_slug = f"{start_dt_str}_to_{end_dt_str}"
    range_label = date_slug               # e.g. "2026-05-01_to_2026-05-31"
    # Format suffix keeps the filename unambiguous when format != file extension.
    fmt_slug_map = {"radiacode_txt": "radiacode", "radiacode_trk": "radiacode", "radiacode": "radiacode-csv", "internal": "internal-csv"}
    fmt_slug = fmt_slug_map.get(fmt, fmt)

    log.info("export/time-range: %d rows, format=%s, range=%s", len(rows), fmt, range_label)

    # Generate the full export content.
    if fmt == "radiacode_txt":
        content      = _session_to_radiacode_txt(rows, gps_only=True)
        ext          = "txt"
        mime         = "text/plain"
        header_lines = 2  # "Track: ..." + column header
    elif fmt == "radiacode_trk":
        content      = _session_to_radiacode_txt(rows, gps_only=True)
        ext          = "rctrk"
        mime         = "text/plain"
        header_lines = 2  # "Track: ..." + column header
    elif fmt == "radiacode":
        content      = _session_to_radiacode_csv(rows)
        ext          = "csv"
        mime         = "text/csv"
        header_lines = 1
    else:  # internal
        content      = _session_to_internal_csv(rows)
        ext          = "csv"
        mime         = "text/csv"
        header_lines = 1

    content_bytes = content.encode("utf-8")

    if len(content_bytes) <= max_bytes:
        # Single file — stream directly.
        filename = f"radmap_{range_label}_{fmt_slug}.{ext}"
        return StreamingResponse(
            iter([content_bytes]),
            media_type=mime,
            headers={"Content-Disposition": f'attachment; filename="{filename}"'},
        )

    # Multiple files needed — split at row boundaries and ZIP them.
    parts = _split_to_parts(content, max_bytes, header_lines)
    zip_buf = io.BytesIO()
    with zipfile.ZipFile(zip_buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for i, part_text in enumerate(parts, start=1):
            fname = f"radmap_{range_label}_{fmt_slug}_part{i:02d}.{ext}"
            zf.writestr(fname, part_text.encode("utf-8"))

    zip_bytes    = zip_buf.getvalue()
    zip_filename = f"radmap_{range_label}_{fmt_slug}_{len(parts)}parts.zip"
    log.info("export/time-range: split into %d parts, zip size=%d bytes",
             len(parts), len(zip_bytes))

    return StreamingResponse(
        iter([zip_bytes]),
        media_type="application/zip",
        headers={"Content-Disposition": f'attachment; filename="{zip_filename}"'},
    )
