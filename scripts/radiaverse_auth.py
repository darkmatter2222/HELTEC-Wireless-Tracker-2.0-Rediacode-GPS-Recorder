"""
Radiaverse authentication — JWT token management.

First run: launches Edge with Chrome DevTools Protocol (CDP), opens
map.radiaverse.com, user completes Google OAuth login normally in the browser
window, script captures JWT access + refresh tokens from the auth response and
saves them to a local file.

Subsequent runs: loads saved tokens, refreshes the access token via the refresh
token if expired, re-prompts browser login if refresh token is also expired
(typically ~90 days).

Token file: scripts/radiaverse_tokens.json  ← GITIGNORED — never committed
"""
from __future__ import annotations

import asyncio
import base64
import json
import os
import subprocess
import sys
import time
from pathlib import Path

TOKENS_FILE = Path(__file__).parent / "radiaverse_tokens.json"
MAP_BASE = "https://map.radiaverse.com"
API_BASE = "https://api.radiaverse.com"
CDP_PORT = 9222


# ──────────────────────────────────────────────────────────────────────────────
# JWT helpers (no signature verification — we trust our own tokens)
# ──────────────────────────────────────────────────────────────────────────────

def _jwt_exp(token: str) -> int:
    """Decode the exp claim from a JWT payload without signature verification."""
    try:
        payload = token.split(".")[1]
        payload += "==" * (-len(payload) % 4)  # pad to a multiple of 4
        return json.loads(base64.urlsafe_b64decode(payload)).get("exp", 0)
    except Exception:
        return 0


def is_token_valid(token: str, buffer_s: int = 300) -> bool:
    """True if the token expires more than buffer_s seconds from now."""
    return bool(token) and _jwt_exp(token) > time.time() + buffer_s


# ──────────────────────────────────────────────────────────────────────────────
# Token persistence
# ──────────────────────────────────────────────────────────────────────────────

def load_tokens() -> dict | None:
    """Load saved tokens from disk. Returns None if missing or corrupt."""
    if TOKENS_FILE.exists():
        try:
            return json.loads(TOKENS_FILE.read_text())
        except Exception:
            pass
    return None


def save_tokens(tokens: dict) -> None:
    """Save tokens to the gitignored local file."""
    TOKENS_FILE.write_text(json.dumps(tokens, indent=2))
    exp = _jwt_exp(tokens.get("access_token", ""))
    print(f"[auth] Tokens saved → {TOKENS_FILE.name}"
          f"  (access expires {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(exp))})")


# ──────────────────────────────────────────────────────────────────────────────
# Token refresh via Radiaverse refresh endpoint
# ──────────────────────────────────────────────────────────────────────────────

def refresh_access_token(refresh_token: str) -> dict | None:
    """
    Try to exchange the refresh token for a new access token.
    Returns a new tokens dict on success, None on failure.
    Tries both the map and api endpoints since the exact refresh URL isn't
    confirmed from the HAR (no refresh call was observed there).
    """
    import requests  # local import to keep module importable without requests

    candidates = [
        f"{MAP_BASE}/api/auth/refresh",
        f"{API_BASE}/api/v1/auth/refresh",
    ]
    headers = {
        "Content-Type": "application/json",
        "Origin": MAP_BASE,
        "Referer": f"{MAP_BASE}/",
    }
    for url in candidates:
        try:
            r = requests.post(
                url,
                json={"refresh_token": refresh_token},
                headers=headers,
                timeout=15,
            )
            if r.status_code == 200:
                data = r.json()
                user = data.get("user", data)
                access = user.get("accessToken") or user.get("access_token")
                new_refresh = (
                    user.get("refreshToken")
                    or user.get("refresh_token")
                    or refresh_token
                )
                if access:
                    print(f"[auth] Token refreshed via {url}")
                    return {
                        "access_token": access,
                        "refresh_token": new_refresh,
                        "saved_at": time.time(),
                    }
            print(f"[auth] Refresh at {url}: HTTP {r.status_code} — {r.text[:120]}")
        except Exception as exc:
            print(f"[auth] Refresh error at {url}: {exc}")
    return None


# ──────────────────────────────────────────────────────────────────────────────
# Browser-based one-time login via Edge + Chrome DevTools Protocol
# ──────────────────────────────────────────────────────────────────────────────

def _find_edge() -> str:
    """Locate the Edge executable on Windows."""
    candidates = [
        os.environ.get("EDGE_PATH", ""),
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        r"C:\Users\ryans\AppData\Local\Microsoft\Edge\Application\msedge.exe",
    ]
    for path in candidates:
        if path and Path(path).exists():
            return path
    raise RuntimeError(
        "msedge.exe not found. Set the EDGE_PATH environment variable "
        "to the full path of msedge.exe."
    )


async def _capture_tokens_via_cdp(ws_debug_url: str) -> dict:
    """
    Attach to Edge via CDP WebSocket, enable Network recording, then wait for
    the user to complete Google login.  When the browser posts to
    /api/auth/google and gets a 200, capture the access + refresh tokens from
    the response body.
    """
    import websockets  # websockets 10+ (async)

    print(
        "[auth] Browser is open — complete the Google login in the Edge window.\n"
        "[auth] The script will capture your tokens automatically (timeout: 5 min)."
    )

    async with websockets.connect(ws_debug_url) as ws:
        # Enable Network event tracking
        await ws.send(json.dumps({"id": 1, "method": "Network.enable"}))
        await ws.recv()  # ignore the ack

        seen_req_ids: set[str] = set()
        loop = asyncio.get_event_loop()
        deadline = loop.time() + 300  # 5-minute window

        while loop.time() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
            except asyncio.TimeoutError:
                continue

            msg = json.loads(raw)
            evt = msg.get("method", "")

            # Detect auth response
            if evt == "Network.responseReceived":
                resp = msg["params"]["response"]
                if (
                    "auth/google" in resp.get("url", "")
                    and resp.get("status") == 200
                ):
                    req_id = msg["params"]["requestId"]
                    if req_id not in seen_req_ids:
                        seen_req_ids.add(req_id)
                        # Request the response body from CDP
                        await ws.send(json.dumps({
                            "id": 2,
                            "method": "Network.getResponseBody",
                            "params": {"requestId": req_id},
                        }))

            # Handle response body
            elif msg.get("id") == 2 and "result" in msg:
                body_text = msg["result"].get("body", "")
                try:
                    data = json.loads(body_text)
                    user = data.get("user", {})
                    access = user.get("accessToken")
                    refresh = user.get("refreshToken")
                    if access and refresh:
                        return {
                            "access_token": access,
                            "refresh_token": refresh,
                            "saved_at": time.time(),
                        }
                except Exception as exc:
                    print(f"[auth] Warning: failed to parse auth response: {exc}")

    raise TimeoutError("[auth] Login not completed within 5 minutes.")


def login_via_browser() -> dict:
    """
    Launch Edge with CDP debugging, wait for the user to complete Google OAuth
    on map.radiaverse.com, then capture and return the JWT tokens.

    The browser opens with an isolated profile directory so it does not conflict
    with any normally-running Edge instance.
    """
    import requests as req_lib
    import tempfile

    edge_path = _find_edge()
    profile_dir = Path(tempfile.gettempdir()) / "radiaverse_cdp_profile"
    profile_dir.mkdir(exist_ok=True)

    print(f"[auth] Launching Edge on port {CDP_PORT} → {MAP_BASE}")
    proc = subprocess.Popen(
        [
            edge_path,
            f"--remote-debugging-port={CDP_PORT}",
            f"--user-data-dir={profile_dir}",
            "--no-first-run",
            "--no-default-browser-check",
            MAP_BASE,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Poll for the CDP endpoint
    ws_debug_url: str | None = None
    for _ in range(30):
        time.sleep(1)
        try:
            targets = req_lib.get(
                f"http://localhost:{CDP_PORT}/json", timeout=2
            ).json()
            page = next((t for t in targets if t.get("type") == "page"), None)
            if page:
                ws_debug_url = page["webSocketDebuggerUrl"]
                break
        except Exception:
            pass
    else:
        proc.terminate()
        raise RuntimeError(
            f"Edge CDP did not respond on port {CDP_PORT} within 30 s."
        )

    try:
        tokens = asyncio.run(_capture_tokens_via_cdp(ws_debug_url))
    finally:
        try:
            proc.terminate()
        except Exception:
            pass

    print("[auth] Login successful — tokens captured!")
    return tokens


# ──────────────────────────────────────────────────────────────────────────────
# Public entry point
# ──────────────────────────────────────────────────────────────────────────────

def ensure_valid_token() -> str:
    """
    Return a valid Radiaverse access token.

    Decision tree:
      1. Load tokens from disk.
      2. If access token still valid → return it.
      3. If refresh token still valid → attempt refresh → return new access token.
      4. If refresh fails or refresh token expired → open browser for login.
    """
    tokens = load_tokens()
    if tokens:
        access = tokens.get("access_token", "")
        refresh = tokens.get("refresh_token", "")

        if is_token_valid(access):
            exp_in = int(_jwt_exp(access) - time.time())
            hrs, mins = divmod(exp_in, 3600)
            print(f"[auth] Cached access token valid (expires in {hrs}h {mins // 60}m)")
            return access

        if is_token_valid(refresh, buffer_s=60):
            print("[auth] Access token expired — attempting refresh...")
            new_tokens = refresh_access_token(refresh)
            if new_tokens:
                save_tokens(new_tokens)
                return new_tokens["access_token"]
            print("[auth] Refresh failed — falling back to browser login")
        else:
            print("[auth] Refresh token expired — browser re-login required")

    tokens = login_via_browser()
    save_tokens(tokens)
    return tokens["access_token"]


# ──────────────────────────────────────────────────────────────────────────────
# CLI: run standalone to force a fresh login
# ──────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Radiaverse auth — token management")
    parser.add_argument(
        "--force-login",
        action="store_true",
        help="Force a new browser login even if cached tokens are still valid",
    )
    args = parser.parse_args()

    if args.force_login:
        tokens = login_via_browser()
        save_tokens(tokens)
    else:
        token = ensure_valid_token()

    loaded = load_tokens()
    if loaded:
        acc_exp = _jwt_exp(loaded.get("access_token", ""))
        ref_exp = _jwt_exp(loaded.get("refresh_token", ""))
        print(f"\nAccess  token expires: {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(acc_exp))}")
        print(f"Refresh token expires: {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(ref_exp))}")
