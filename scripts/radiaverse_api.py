"""
Radiaverse REST API client.

Stateless Bearer-JWT auth — no cookies, no CSRF tokens needed.
The token is loaded from disk and refreshed automatically when expired.

Endpoints used (from HAR analysis):
  GET  https://api.radiaverse.com/api/v1/track/get               list tracks (paginated)
  GET  https://api.radiaverse.com/api/v1/track/get/{id}          single track
  POST https://api.radiaverse.com/api/v1/track/upload            upload (multipart)
  DELETE https://api.radiaverse.com/api/v1/track/delete/{id}     delete track
"""
from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Iterator

import requests

# Allow running from any working directory
sys.path.insert(0, str(Path(__file__).parent))

from radiaverse_auth import (
    ensure_valid_token,
    is_token_valid,
    load_tokens,
    refresh_access_token,
    save_tokens,
)

API_BASE = "https://api.radiaverse.com/api/v1"
MAP_BASE = "https://map.radiaverse.com"
_USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/148.0.0.0 Safari/537.36 Edg/148.0.0.0"
)


class RadiaverseClient:
    """
    HTTP client for the Radiaverse API.

    Auth is handled transparently:
      - Bearer token from radiaverse_tokens.json (gitignored)
      - Auto-refreshed when the access token expires
      - Falls back to browser login if refresh token is expired

    Usage:
        client = RadiaverseClient()
        tracks = client.list_all_tracks()
        client.upload_track("tracks/RadiaCode_ Track.txt")
        client.delete_track("e04a3ffc-...")
    """

    def __init__(self, access_token: str | None = None) -> None:
        self._token: str = access_token or ensure_valid_token()
        self._session = requests.Session()
        self._apply_token()

    # ── Internal helpers ──────────────────────────────────────────────────────

    def _apply_token(self) -> None:
        """Update the session with the current Bearer token."""
        self._session.headers.update(
            {
                "Authorization": f"Bearer {self._token}",
                "Origin": MAP_BASE,
                "Referer": f"{MAP_BASE}/",
                "User-Agent": _USER_AGENT,
            }
        )

    def _maybe_refresh(self) -> None:
        """Silently refresh the access token if it has expired."""
        if is_token_valid(self._token):
            return
        tokens = load_tokens()
        if tokens:
            new = refresh_access_token(tokens.get("refresh_token", ""))
            if new:
                save_tokens(new)
                self._token = new["access_token"]
                self._apply_token()
                return
        # Last resort: full browser re-login
        self._token = ensure_valid_token()
        self._apply_token()

    def _get(self, path: str, **kwargs) -> requests.Response:
        self._maybe_refresh()
        r = self._session.get(f"{API_BASE}{path}", **kwargs)
        r.raise_for_status()
        return r

    def _post(self, path: str, **kwargs) -> requests.Response:
        self._maybe_refresh()
        r = self._session.post(f"{API_BASE}{path}", **kwargs)
        r.raise_for_status()
        return r

    def _delete(self, path: str, **kwargs) -> requests.Response:
        self._maybe_refresh()
        r = self._session.delete(f"{API_BASE}{path}", **kwargs)
        r.raise_for_status()
        return r

    # ── Track listing ─────────────────────────────────────────────────────────

    def list_tracks(
        self,
        page: int = 1,
        size: int = 100,
        sort_by: str = "Uploaded at",
        sort_order: str = "desc",
        search: str = "",
    ) -> dict:
        """
        Fetch one page of tracks.

        Returns:
            {
              "items": [...],   # list of track dicts
              "total": N,       # total count across all pages
              "page": N,
              "size": N
            }

        Each track dict has at minimum:
            id, name, description, timestamp, published_at, is_public,
            data, preview, user_id, user_nickname, bookmarks_count
        """
        return self._get(
            "/track/get",
            params={
                "page": page,
                "size": size,
                "sort_by": sort_by,
                "sort_order": sort_order,
                "search": search,
            },
            timeout=30,
        ).json()

    def iter_tracks(self, size: int = 100) -> Iterator[dict]:
        """Yield every track one at a time, paginating automatically."""
        page = 1
        while True:
            data = self.list_tracks(page=page, size=size)
            items = data.get("items", [])
            yield from items
            if len(items) < size:
                break
            page += 1

    def list_all_tracks(self) -> list[dict]:
        """Return every track on the account as a flat list."""
        tracks: list[dict] = []
        page = 1
        while True:
            data = self.list_tracks(page=page, size=100)
            items = data.get("items", [])
            tracks.extend(items)
            total = data.get("total", 0)
            if len(tracks) >= total or not items:
                break
            page += 1
        print(f"[api] Listed {len(tracks)} tracks total")
        return tracks

    def get_track(self, track_id: str) -> dict:
        """Fetch a single track by its UUID."""
        return self._get(f"/track/get/{track_id}", timeout=30).json()

    # ── Upload ────────────────────────────────────────────────────────────────

    def upload_track(self, file_path: str | Path, is_public: bool = True) -> str:
        """
        Upload a RadiaCode .txt track file.

        The request is multipart/form-data with two fields:
          file      — raw binary content of the .txt file
          is_public — "true" | "false"

        Returns:
            task_id (str) — used to poll for completion via wait_for_upload()
        Raises:
            requests.HTTPError on a non-2xx response.
        """
        file_path = Path(file_path)
        size_kb = file_path.stat().st_size / 1024
        print(f"[api] Uploading {file_path.name}  ({size_kb:.1f} KB)...")

        self._maybe_refresh()
        with open(file_path, "rb") as fh:
            # NOTE: do NOT set Content-Type manually — requests sets the
            # multipart boundary automatically when `files=` is used.
            r = self._session.post(
                f"{API_BASE}/track/upload",
                files={"file": (file_path.name, fh, "application/octet-stream")},
                data={"is_public": "true" if is_public else "false"},
                timeout=120,
            )
        r.raise_for_status()
        task_id: str = r.json()["task_id"]
        print(f"[api] Upload accepted  task_id={task_id}")
        return task_id

    def wait_for_upload(
        self,
        pre_upload_count: int,
        timeout: int = 180,
        poll_interval: int = 8,
    ) -> dict | None:
        """
        After calling upload_track(), poll the track list until the total count
        increases (meaning the new track finished processing and is visible).

        Args:
            pre_upload_count: total track count BEFORE upload_track() was called.
            timeout:          seconds to wait before giving up.
            poll_interval:    seconds between polls.

        Returns:
            The new track dict (newest in the list), or None on timeout.
        """
        deadline = time.time() + timeout
        print("[api] Waiting for Radiaverse to process upload", end="", flush=True)
        while time.time() < deadline:
            time.sleep(poll_interval)
            print(".", end="", flush=True)
            try:
                data = self.list_tracks(page=1, size=1, sort_by="Uploaded at", sort_order="desc")
                if data.get("total", 0) > pre_upload_count:
                    newest = data["items"][0] if data.get("items") else None
                    if newest:
                        print(f"  done -> {newest['id']}")
                        return newest
            except Exception as exc:
                print(f"\n[api] Poll error: {exc}", end="")
        print("  TIMEOUT -- upload may still be processing on Radiaverse")
        return None

    # ── Delete ────────────────────────────────────────────────────────────────

    def delete_track(self, track_id: str) -> dict:
        """
        Delete a track by UUID.

        Returns:
            The deleted track's metadata dict (name, data, etc.).
        Raises:
            requests.HTTPError on failure.
        """
        r = self._delete(f"/track/delete/{track_id}", timeout=30)
        return r.json()

    def delete_all_tracks(self, dry_run: bool = False) -> int:
        """
        Delete every track on the account.

        Returns:
            Number of tracks successfully deleted.
        """
        tracks = self.list_all_tracks()
        print(f"[api] {'(dry-run) ' if dry_run else ''}Deleting {len(tracks)} tracks...")
        deleted = 0
        for t in tracks:
            tid, name = t["id"], t.get("name", t["id"])
            if dry_run:
                print(f"[api]   would delete: {name}  ({tid})")
                deleted += 1
                continue
            try:
                self.delete_track(tid)
                print(f"[api]   OK  {name}")
                deleted += 1
            except Exception as exc:
                print(f"[api]   FAIL  {name}: {exc}")
        return deleted


# ──────────────────────────────────────────────────────────────────────────────
# CLI — quick smoke-test / manual operations
# ──────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Radiaverse API client")
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("list", help="List all tracks")
    p_get = sub.add_parser("get", help="Get a single track")
    p_get.add_argument("track_id")
    p_del = sub.add_parser("delete", help="Delete a track")
    p_del.add_argument("track_id")
    sub.add_parser("delete-all", help="Delete ALL tracks (with confirmation)")
    p_up = sub.add_parser("upload", help="Upload a track file")
    p_up.add_argument("file", type=Path)

    args = parser.parse_args()
    client = RadiaverseClient()

    if args.cmd == "list":
        for t in client.list_all_tracks():
            print(f"  {t['id']}  {t.get('name', '?'):40}  published={t.get('published_at')}")

    elif args.cmd == "get":
        import json as _json
        print(_json.dumps(client.get_track(args.track_id), indent=2))

    elif args.cmd == "delete":
        result = client.delete_track(args.track_id)
        print(f"Deleted: {result.get('name', args.track_id)}")

    elif args.cmd == "delete-all":
        confirm = input("Type 'DELETE ALL' to confirm: ")
        if confirm.strip() == "DELETE ALL":
            n = client.delete_all_tracks()
            print(f"Deleted {n} tracks.")
        else:
            print("Aborted.")

    elif args.cmd == "upload":
        pre_count_data = client.list_tracks(page=1, size=1)
        pre_count = pre_count_data.get("total", 0)
        task_id = client.upload_track(args.file)
        track = client.wait_for_upload(pre_upload_count=pre_count)
        if track:
            print(f"Uploaded: {track['id']}  {track.get('name')}")
        else:
            print(f"Upload submitted (task_id={task_id}) but could not confirm completion.")

    else:
        parser.print_help()
