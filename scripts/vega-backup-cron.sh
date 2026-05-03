#!/bin/bash
# Weekly vega-tracker backup. Called by cron at 03:00 UTC every Sunday.
# Creates a full-database backup via the API (source=cron), then prunes
# to the 5 newest snapshots (keeps min 2 to avoid accidental mass delete).
set -euo pipefail

LOG=/home/darkmatter2222/vega-tracker-backups/cron.log
BACKUP_ROOT=/home/darkmatter2222/vega-tracker-backups

echo "--- $(date --utc +%Y-%m-%dT%H:%M:%SZ) backup start ---" >> "$LOG"

# Trigger backup via API, tag as cron so telemetry shows source=cron
RESULT=$(curl -sf -X POST 'http://localhost:8030/admin/backup?source=cron' 2>&1) && \
  echo "BACKUP OK: $RESULT" >> "$LOG" || \
  echo "BACKUP FAILED: $RESULT" >> "$LOG"

# Prune: keep the 5 newest backup directories, delete the rest
COUNT=$(ls -1d "$BACKUP_ROOT"/20??-??-??_??-??-?? 2>/dev/null | wc -l)
KEEP=5
if [ "$COUNT" -gt "$KEEP" ]; then
  ls -1d "$BACKUP_ROOT"/20??-??-??_??-??-?? | sort | head -n $(($COUNT - $KEEP)) | while read -r OLD; do
    rm -rf "$OLD"
    echo "PRUNED: $OLD" >> "$LOG"
  done
fi

echo "--- backup done ($COUNT dirs before prune) ---" >> "$LOG"
