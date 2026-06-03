---
mode: agent
description: Upload pending GPS sessions from MongoDB to Radiaverse. Triggered by phrases like "upload to Radiaverse", "sync to Radiaverse", "upload the latest data to Radiaverse", "push sessions to Radiaverse".
---

# Radiaverse Session Sync — Agent Runbook

## What this does

Finds every GPS session in MongoDB that has NOT yet been uploaded to Radiaverse and uploads it.
Sessions arrive in MongoDB automatically whenever the device comes within Wi-Fi range.
This sync step mirrors them to [map.radiaverse.com](https://map.radiaverse.com) in RadiaCode native .txt format.

## Prerequisites check

Before running, verify:
1. The venv exists at `.venv\Scripts\python.exe` in the repo root.
2. `scripts/radiaverse_tokens.json` exists (means the user has logged in at least once).
   - If it does NOT exist, tell the user to run: `python scripts\radiaverse_sync.py --login`
   - That opens Edge — the user signs in with Google, tokens are saved automatically.

## Execution steps

Always run in this exact order:

### Step 1 — Dry run (preview what will be uploaded)

```powershell
.venv\Scripts\python.exe scripts\radiaverse_sync.py --sessions --dry-run
```

Read the output. Report to the user:
- How many sessions are pending upload
- Which session IDs / date ranges will be uploaded
- Any sessions being skipped (no GPS data)

If 0 sessions are pending, tell the user everything is already synced and stop.

### Step 2 — Confirm and upload

If there are sessions to upload, proceed (no need to ask for confirmation — the user
triggered this intentionally). Run:

```powershell
.venv\Scripts\python.exe scripts\radiaverse_sync.py --sessions
```

Watch the output for:
- `OK  task_id=...` lines — successful uploads
- `FAIL` lines — failures to investigate
- Final summary line showing uploaded / skipped / no GPS / failed counts

### Step 3 — Report results

Tell the user:
- How many sessions were uploaded successfully
- Any failures and what the error was
- Reminder that Radiaverse processes uploads asynchronously — tracks may take a few
  minutes to appear on [map.radiaverse.com](https://map.radiaverse.com)

## Handling auth errors

If you see output like `[auth] Refresh failed` or `401` or `403` from the Radiaverse API:

1. Tell the user: "Your Radiaverse token has expired. Run this once to re-authenticate:"
   ```powershell
   python scripts\radiaverse_sync.py --login
   ```
2. That opens Edge — user signs in with Google. After it completes, re-run Step 1 above.

Tokens last ~7 days (access) / ~90 days (refresh). The refresh is silent and automatic.
Full re-login is only needed roughly every 90 days of inactivity.

## Uploading a specific session

If the user asks to upload a specific date (e.g. "upload June 1st"):

```powershell
.venv\Scripts\python.exe scripts\radiaverse_sync.py --session-id 2026-06-01
```

## Checking status without uploading

```powershell
.venv\Scripts\python.exe scripts\radiaverse_sync.py --status
```

Shows a table of uploaded / pending / failed counts, plus the 5 most recently uploaded sessions.

## Key facts

- **Token file**: `scripts/radiaverse_tokens.json` — gitignored, lives only on the dev machine.
- **Tracking**: Each uploaded session is recorded in MongoDB collection `radiaverse_uploads`
  (database `radiacode`) with key `session_<sessionId>`. Re-running is always safe — already
  uploaded sessions are skipped.
- **GPS-less sessions**: Silently skipped. Radiaverse doesn't create a visible track for sessions
  with no GPS coordinates, so uploading them wastes API quota.
- **Rate limit**: 3-second delay between uploads is built in. Don't add extra delays.
- **Async processing**: Radiaverse confirms upload immediately via `task_id` but the track
  may not appear on the map for several minutes. This is normal.
- **Cannot run on the server**: Tokens are personal Google account credentials on the local machine.
- **Daily automation**: Windows Task Scheduler job is set up via
  `powershell -ExecutionPolicy Bypass scripts\radiaverse_daily.ps1 -Register`
  and runs at 06:00 automatically.
