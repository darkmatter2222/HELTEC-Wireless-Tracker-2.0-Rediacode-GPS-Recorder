"""
Radiaverse daily sync — uploads RadiaCode .txt track files to Radiaverse
and records upload status in MongoDB (radiacode.radiaverse_uploads).

USAGE
─────
  python scripts/radiaverse_sync.py                  # upload all pending tracks
  python scripts/radiaverse_sync.py --dry-run        # preview without uploading
  python scripts/radiaverse_sync.py --status         # show upload status table
  python scripts/radiaverse_sync.py --wipe-all       # delete everything from Radiaverse
                                                     # and reset the DB so all tracks
                                                     # will be re-uploaded on next run
  python scripts/radiaverse_sync.py --file PATH      # upload one specific file
  python scripts/radiaverse_sync.py --login          # force browser re-login

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

    if failed > 0:
        print("\nFailed uploads:")
        for doc in coll.find({"status": "failed"}, {"filename": 1, "error": 1, "uploaded_at": 1}):
            ts = doc.get("uploaded_at", "?")
            print(f"  {doc['filename']}  [{ts}]  {doc.get('error', '?')}")

    if uploaded > 0:
        print("\nMost recently uploaded:")
        for doc in (
            coll.find({"status": "uploaded"}, {"filename": 1, "radiaverse_track_name": 1, "uploaded_at": 1})
                .sort("uploaded_at", -1)
                .limit(5)
        ):
            name = doc.get("radiaverse_track_name", "?")
            ts   = doc.get("uploaded_at", "?")
            print(f"  {doc['filename']}  →  {name}  [{ts}]")
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

    # ── Default: sync ─────────────────────────────────────────────────────────
    sync_tracks(dry_run=args.dry_run, single_file=args.file)


if __name__ == "__main__":
    main()
