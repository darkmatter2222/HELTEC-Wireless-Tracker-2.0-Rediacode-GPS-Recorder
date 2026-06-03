"""
Radiaverse daily sync — uploads RadiaCode .txt track files to Radiaverse
and records upload status in MongoDB (radiacode.radiaverse_uploads).

USAGE
─────
  python scripts/radiaverse_sync.py                  # upload all pending tracks/ files
  python scripts/radiaverse_sync.py --sessions       # upload new MongoDB sessions daily
  python scripts/radiaverse_sync.py --session-id ID  # upload one specific session
  python scripts/radiaverse_sync.py --dry-run        # preview without uploading
  python scripts/radiaverse_sync.py --status         # show upload status table
  python scripts/radiaverse_sync.py --wipe-all       # delete everything from Radiaverse
                                                     # and reset the DB so all tracks
                                                     # will be re-uploaded on next run
  python scripts/radiaverse_sync.py --file PATH      # upload one specific file
  python scripts/radiaverse_sync.py --login          # force browser re-login

For daily automatic session uploads register the Task Scheduler wrapper:
  powershell -ExecutionPolicy Bypass scripts\radiaverse_daily.ps1 -Register

MONGODB SCHEMA  (collection: radiaverse_uploads, db: radiacode)
────────────────────────────────────────────────────────────────
  filename              str   — original filename  (unique index)
  file_hash             str   — sha256 hex; re-upload if file content changes
  status                str   — "uploaded" | "failed" | "pending" | "deleted"
  radiaverse_track_id   str   — UUID assigned by Radiaverse after upload
  radiaverse_track_name str   — track name shown on Radiaverse
  task_id               str   — async task_id returned by the upload endpoint
  uploaded_at           datetime (UTC)
  error                 str | None — error message if status == "failed"

The tracks/ folder is gitignored, so this script connects directly to MongoDB
at 192.168.86.48:27017 (same server as the ingest API).
"""
from __future__ import annotations

import argparse
import hashlib
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# ── Path setup ────────────────────────────────────────────────────────────────
ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = ROOT / "scripts"
TRACKS_DIR = ROOT / "tracks"
sys.path.insert(0, str(SCRIPTS_DIR))

# ── MongoDB connection ─────────────────────────────────────────────────────────
MONGO_URI = "mongodb://ryan:Welcome123%21@192.168.86.48:27017/?authSource=admin"
MONGO_DB = "radiacode"
UPLOAD_COLLECTION = "radiaverse_uploads"

# Delay between successive uploads (seconds) — be a polite API client
INTER_UPLOAD_DELAY = 3

# Windows FILETIME epoch: 100-ns ticks between 1601-01-01 and 1970-01-01
_FILETIME_EPOCH_DIFF = 116_444_736_000_000_000


def _fmt_num(v) -> str:
    """Format a float with minimal decimal places (strips trailing zeros)."""
    if v is None:
        return ""
    return f"{float(v):.10g}"


def _build_radiacode_txt(rows: list[dict]) -> str:
    """Build a RadiaCode native .txt export from MongoDB sample documents.

    Produces tab-separated output matching the RadiaCode app's own track export:
      Track: YYYY-MM-DD HH-MM-SS<TAB><device><TAB> <TAB>EC
      Timestamp<TAB>Time<TAB>Latitude<TAB>Longitude<TAB>Accuracy<TAB>DoseRate<TAB>CountRate<TAB>Comment
      <FILETIME><TAB>UTC datetime<TAB>...

    Event-only rows (GPS_LOST / GPS_REGAINED with no dose data) are skipped.
    Rows without GPS coordinates are included but leave lat/lng fields empty.
    """
    import io

    sorted_rows = sorted(rows, key=lambda r: r.get("timestampMs", 0))

    # Track header line — uses UTC datetime of first sample
    first_ms = sorted_rows[0].get("timestampMs", 0) if sorted_rows else 0
    track_dt = datetime.fromtimestamp(first_ms / 1_000, tz=timezone.utc)
    track_date_str = track_dt.strftime("%Y-%m-%d %H-%M-%S")

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
        # Skip pure event rows (GPS_LOST / GPS_REGAINED — no dose data)
        if usv is None and cps is None:
            continue

        filetime = ts_ms * 10_000 + _FILETIME_EPOCH_DIFF
        time_str = datetime.fromtimestamp(ts_ms / 1_000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

        if acc_m is not None:
            acc = _fmt_num(acc_m)
        elif hdop is not None:
            acc = _fmt_num(hdop * 5.0)
        else:
            acc = ""

        has_gps = lat and lng and not (lat == 0 and lng == 0)
        lat_s   = _fmt_num(lat) if has_gps else ""
        lng_s   = _fmt_num(lng) if has_gps else ""
        dose_s  = _fmt_num(usv) if usv is not None else ""
        cps_s   = _fmt_num(cps) if cps is not None else ""

        out.write(f"{filetime}\t{time_str}\t{lat_s}\t{lng_s}\t{acc}\t{dose_s}\t{cps_s}\t \n")

    return out.getvalue()


# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

def _sha256(path: Path) -> str:
    """Return the hex SHA-256 digest of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65_536), b""):
            h.update(chunk)
    return h.hexdigest()


def _get_collection():
    """Connect to MongoDB and return the radiaverse_uploads collection."""
    from pymongo import MongoClient, ASCENDING

    client = MongoClient(MONGO_URI, serverSelectionTimeoutMS=8_000)
    coll = client[MONGO_DB][UPLOAD_COLLECTION]
    # Ensure unique index on filename (one record per file)
    coll.create_index([("filename", ASCENDING)], unique=True, background=True)
    return coll


def _get_track_files() -> list[Path]:
    """Return all .txt files from the tracks/ directory, sorted."""
    if not TRACKS_DIR.exists():
        print(f"[sync] ERROR: tracks/ directory not found at {TRACKS_DIR}")
        return []
    files = sorted(TRACKS_DIR.glob("*.txt"))
    print(f"[sync] {len(files)} .txt file(s) found in {TRACKS_DIR.name}/")
    return files


# ──────────────────────────────────────────────────────────────────────────────
# Status display
# ──────────────────────────────────────────────────────────────────────────────

def show_status(coll) -> None:
    """Print a summary table of upload status."""
    total_local = len(list(TRACKS_DIR.glob("*.txt"))) if TRACKS_DIR.exists() else 0
    uploaded = coll.count_documents({"status": "uploaded"})
    failed   = coll.count_documents({"status": "failed"})
    deleted  = coll.count_documents({"status": "deleted"})
    pending  = total_local - uploaded

    line = "─" * 52
    print(f"\n{line}")
    print(f"  Radiaverse upload status")
    print(line)
    print(f"  Local .txt files        {total_local:>6}")
    print(f"  Uploaded to Radiaverse  {uploaded:>6}")
    print(f"  Pending upload          {pending:>6}")
    print(f"  Failed                  {failed:>6}")
    print(f"  Deleted from Radiaverse {deleted:>6}")
    print(line)

    # Session-based uploads
    session_up   = coll.count_documents({"source": "session", "status": "uploaded"})
    session_fail = coll.count_documents({"source": "session", "status": "failed"})
    if session_up + session_fail > 0:
        print()
        print(f"  Sessions -> Radiaverse  {session_up:>6}  uploaded")
        print(f"  Sessions failed         {session_fail:>6}")
        print(line)

    if failed > 0:
        print("\nFailed file uploads:")
        for doc in coll.find({"status": "failed", "source": {"$ne": "session"}},
                             {"filename": 1, "error": 1, "uploaded_at": 1}):
            ts = doc.get("uploaded_at", "?")
            print(f"  {doc['filename']}  [{ts}]  {doc.get('error', '?')}")

    if session_fail > 0:
        print("\nFailed session uploads:")
        for doc in coll.find({"source": "session", "status": "failed"},
                             {"sessionId": 1, "error": 1, "uploaded_at": 1}):
            ts = doc.get("uploaded_at", "?")
            print(f"  {doc.get('sessionId', doc['filename'])}  [{ts}]  {doc.get('error', '?')}")

    if uploaded > 0:
        print("\nMost recently uploaded (files):")
        for doc in (
            coll.find({"status": "uploaded", "source": {"$ne": "session"}},
                      {"filename": 1, "radiaverse_track_name": 1, "uploaded_at": 1})
                .sort("uploaded_at", -1)
                .limit(5)
        ):
            name = doc.get("radiaverse_track_name", "?")
            ts   = doc.get("uploaded_at", "?")
            print(f"  {doc['filename']}  ->  {name}  [{ts}]")

    if session_up > 0:
        print("\nMost recently uploaded (sessions):")
        for doc in (
            coll.find({"source": "session", "status": "uploaded"},
                      {"sessionId": 1, "uploaded_at": 1, "sampleCount": 1})
                .sort("uploaded_at", -1)
                .limit(5)
        ):
            ts = doc.get("uploaded_at", "?")
            n  = doc.get("sampleCount", "?")
            print(f"  {doc.get('sessionId', '?')}  [{ts}]  {n} samples")
    print()


# ──────────────────────────────────────────────────────────────────────────────
# Core sync
# ──────────────────────────────────────────────────────────────────────────────

def sync_tracks(dry_run: bool = False, single_file: Path | None = None) -> None:
    """
    Upload all unuploaded .txt track files to Radiaverse.

    Skips files that are already recorded as "uploaded" with the same sha256
    hash (content unchanged).  If the file was previously uploaded but the
    content has since changed, it is treated as a new upload.
    """
    from radiaverse_api import RadiaverseClient

    coll   = _get_collection()
    client = RadiaverseClient()
    files  = [single_file] if single_file else _get_track_files()

    if not files:
        print("[sync] Nothing to do.")
        return

    uploaded_count = 0
    skipped_count  = 0
    failed_count   = 0

    for path in files:
        fname = path.name
        fhash = _sha256(path)

        # Skip if already uploaded with the same content
        doc = coll.find_one({"filename": fname})
        if doc and doc.get("status") == "uploaded" and doc.get("file_hash") == fhash:
            skipped_count += 1
            continue

        print(f"\n[sync] {'(dry-run) ' if dry_run else ''}-> {fname}")

        if dry_run:
            uploaded_count += 1
            continue

        try:
            task_id = client.upload_track(path)

            # A task_id in the 200 response means Radiaverse accepted the file.
            # Track processing is async on their side — we don't wait.
            coll.update_one(
                {"filename": fname},
                {
                    "$set": {
                        "filename":               fname,
                        "file_hash":              fhash,
                        "status":                 "uploaded",
                        "radiaverse_track_id":    None,   # filled in later by reconcile
                        "radiaverse_track_name":  None,
                        "task_id":                task_id,
                        "uploaded_at":            datetime.now(timezone.utc),
                        "error":                  None,
                    }
                },
                upsert=True,
            )
            print(f"[sync]   OK  task_id={task_id}")
            uploaded_count += 1

        except Exception as exc:
            err = str(exc)
            print(f"[sync]   FAIL  FAILED: {err}")
            coll.update_one(
                {"filename": fname},
                {
                    "$set": {
                        "filename":    fname,
                        "file_hash":   fhash,
                        "status":      "failed",
                        "error":       err,
                        "uploaded_at": datetime.now(timezone.utc),
                    }
                },
                upsert=True,
            )
            failed_count += 1

        # Be polite — small gap between uploads
        time.sleep(INTER_UPLOAD_DELAY)

    print(
        f"\n[sync] Done — "
        f"uploaded: {uploaded_count}  "
        f"skipped (already done): {skipped_count}  "
        f"failed: {failed_count}"
    )
    if dry_run:
        print("[sync] (dry-run — no actual uploads were made)")


# ──────────────────────────────────────────────────────────────────────────────
# Session sync — daily job that mirrors MongoDB sessions to Radiaverse
# ──────────────────────────────────────────────────────────────────────────────

def sync_sessions(dry_run: bool = False, session_id: str | None = None) -> None:
    """
    Upload MongoDB tracker_sessions to Radiaverse that have not yet been uploaded.

    Tracking key: 'session_<sessionId>' in radiaverse_uploads (distinct from the
    file-based keys used by sync_tracks so both modes can coexist).

    Skips:
    - Sessions already marked 'uploaded' in radiaverse_uploads
    - Sessions with no GPS-locked rows (Radiaverse won't create a track entry)
    - Soft-deleted sessions (deletedAt is set)
    """
    import tempfile
    from pymongo import MongoClient, ASCENDING
    from radiaverse_api import RadiaverseClient

    mongo = MongoClient(MONGO_URI, serverSelectionTimeoutMS=8_000)
    db    = mongo[MONGO_DB]
    uploads_coll  = db[UPLOAD_COLLECTION]
    sessions_coll = db["tracker_sessions"]
    samples_coll  = db["tracker_samples"]

    uploads_coll.create_index([("filename", ASCENDING)], unique=True, background=True)

    client = RadiaverseClient()

    # Active (non-deleted) sessions, chronological
    filt = {"deletedAt": None}
    if session_id:
        filt["sessionId"] = session_id
    sessions = list(sessions_coll.find(filt).sort("firstTsMs", 1))
    print(f"[session-sync] {len(sessions)} active session(s) in MongoDB")

    uploaded = skipped = failed = no_gps = 0

    for sess in sessions:
        sid = sess["sessionId"]
        key = f"session_{sid}"

        # Skip if already uploaded
        doc = uploads_coll.find_one({"filename": key})
        if doc and doc.get("status") == "uploaded":
            skipped += 1
            continue

        # Fetch samples — only fields needed for the .txt export
        rows = list(samples_coll.find(
            {"sessionId": sid},
            {"_id": 0, "timestampMs": 1, "uSvPerHour": 1, "cps": 1,
             "latitude": 1, "longitude": 1, "hdop": 1, "accuracyM": 1, "deviceId": 1},
        ).sort("timestampMs", 1))

        # Skip sessions with no GPS data — Radiaverse won't create a visible track
        gps_rows = [
            r for r in rows
            if r.get("latitude") and r.get("longitude")
            and not (r["latitude"] == 0 and r["longitude"] == 0)
        ]
        if not gps_rows:
            no_gps += 1
            print(f"[session-sync] skip {sid} — no GPS rows ({len(rows)} samples)")
            continue

        n_total = len(rows)
        n_gps   = len(gps_rows)
        print(
            f"\n[session-sync] {'(dry-run) ' if dry_run else ''}"
            f"-> {sid}  ({n_total} samples, {n_gps} GPS)"
        )

        if dry_run:
            uploaded += 1
            continue

        # Write to a temp .txt file and upload
        txt_content = _build_radiacode_txt(rows)
        tmp_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                suffix=".txt", delete=False, mode="w", encoding="utf-8",
                prefix=f"rv_{sid}_",
            ) as tmp:
                tmp.write(txt_content)
                tmp_path = Path(tmp.name)

            task_id = client.upload_track(tmp_path)

            uploads_coll.update_one(
                {"filename": key},
                {"$set": {
                    "filename":              key,
                    "sessionId":             sid,
                    "source":                "session",
                    "file_hash":             None,
                    "status":                "uploaded",
                    "radiaverse_track_id":   None,
                    "radiaverse_track_name": None,
                    "task_id":               task_id,
                    "uploaded_at":           datetime.now(timezone.utc),
                    "error":                 None,
                    "sampleCount":           n_total,
                    "gpsCount":              n_gps,
                }},
                upsert=True,
            )
            print(f"[session-sync]   OK  task_id={task_id}")
            uploaded += 1

        except Exception as exc:
            err = str(exc)
            print(f"[session-sync]   FAIL  {err}")
            uploads_coll.update_one(
                {"filename": key},
                {"$set": {
                    "filename":    key,
                    "sessionId":   sid,
                    "source":      "session",
                    "status":      "failed",
                    "error":       err,
                    "uploaded_at": datetime.now(timezone.utc),
                }},
                upsert=True,
            )
            failed += 1

        finally:
            if tmp_path and tmp_path.exists():
                tmp_path.unlink()

        time.sleep(INTER_UPLOAD_DELAY)

    print(
        f"\n[session-sync] Done — "
        f"uploaded: {uploaded}  "
        f"skipped (already done): {skipped}  "
        f"no GPS (skipped): {no_gps}  "
        f"failed: {failed}"
    )
    if dry_run:
        print("[session-sync] (dry-run — no actual uploads were made)")


# ──────────────────────────────────────────────────────────────────────────────
# Wipe-all
# ──────────────────────────────────────────────────────────────────────────────

def wipe_all(force: bool = False) -> None:
    """
    Delete every track from Radiaverse and reset the MongoDB upload records
    so that the next sync run re-uploads everything from scratch.

    Requires explicit confirmation unless --force is passed.
    """
    from radiaverse_api import RadiaverseClient

    if not force:
        print(
            "\nWARNING: This will permanently delete ALL tracks from Radiaverse.\n"
            "         The MongoDB records will be reset so everything re-uploads\n"
            "         on the next sync run.\n"
        )
        answer = input("Type 'DELETE ALL' to confirm: ").strip()
        if answer != "DELETE ALL":
            print("[sync] Aborted — nothing was deleted.")
            return

    coll   = _get_collection()
    client = RadiaverseClient()

    deleted = client.delete_all_tracks()
    print(f"[sync] Deleted {deleted} track(s) from Radiaverse")

    result = coll.update_many(
        {"status": {"$in": ["uploaded", "failed"]}},
        {
            "$set": {
                "status":              "deleted",
                "radiaverse_track_id": None,
                "error":               "wiped by user",
                "uploaded_at":         datetime.now(timezone.utc),
            }
        },
    )
    print(f"[sync] Reset {result.modified_count} MongoDB record(s)")
    print("[sync] Wipe complete.  Run 'radiaverse_sync.py' to re-upload all tracks.")


# ──────────────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Radiaverse daily sync — upload track files and track status in MongoDB",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Show what would be uploaded without making any API calls",
    )
    parser.add_argument(
        "--status", action="store_true",
        help="Show upload status table and exit",
    )
    parser.add_argument(
        "--wipe-all", action="store_true",
        help="Delete ALL tracks from Radiaverse and reset the DB (requires confirmation)",
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Skip confirmation prompt for --wipe-all",
    )
    parser.add_argument(
        "--file", type=Path, metavar="PATH",
        help="Upload a single specific file instead of the whole tracks/ folder",
    )
    parser.add_argument(
        "--sessions", action="store_true",
        help="Upload MongoDB sessions (daily job mode) — uploads any session not yet in Radiaverse",
    )
    parser.add_argument(
        "--session-id", metavar="ID",
        help="With --sessions: upload only this specific session ID",
    )
    parser.add_argument(
        "--login", action="store_true",
        help="Force a new browser login to refresh tokens, then exit",
    )

    args = parser.parse_args()

    # ── Force login ────────────────────────────────────────────────────────────
    if args.login:
        from radiaverse_auth import login_via_browser, save_tokens
        tokens = login_via_browser()
        save_tokens(tokens)
        print("[sync] Login complete.")
        return

    # ── Status ────────────────────────────────────────────────────────────────
    if args.status:
        coll = _get_collection()
        show_status(coll)
        return

    # ── Wipe ──────────────────────────────────────────────────────────────────
    if args.wipe_all:
        wipe_all(force=args.force)
        return

    # ── Sessions mode ─────────────────────────────────────────────────────────
    if args.sessions or args.session_id:
        sync_sessions(dry_run=args.dry_run, session_id=args.session_id)
        return

    # ── Default: sync tracks/ files ───────────────────────────────────────────
    sync_tracks(dry_run=args.dry_run, single_file=args.file)


if __name__ == "__main__":
    main()
