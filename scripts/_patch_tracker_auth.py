#!/usr/bin/env python3
"""
Patch susman-ingress for tracker HTTP Basic Auth.
Run on the server: python3 /tmp/patch_tracker_auth.py
"""
import re, sys

# ── 1. Patch nginx.conf.template ────────────────────────────────────────────
TMPL = "/home/darkmatter2222/docucraft/nginx/nginx.conf.template"
tmpl = open(TMPL).read()

# Find the susmannet HTTPS server block and add auth_basic after the
# X-XSS-Protection header line, before the location blocks.
OLD_HEADERS = '''\
    add_header X-Frame-Options "SAMEORIGIN" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header X-XSS-Protection "1; mode=block" always;

    # Redirect bare root to /tracker/'''

NEW_HEADERS = '''\
    add_header X-Frame-Options "SAMEORIGIN" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header X-XSS-Protection "1; mode=block" always;

    # HTTP Basic Auth — guards the entire tracker domain.
    # Credentials are generated from TRACKER_USER / TRACKER_PASS env vars
    # at container startup by entrypoint.sh.
    auth_basic "Radiological Map";
    auth_basic_user_file /etc/nginx/tracker_htpasswd;

    # The firmware uploads directly to :8030 (bypasses this proxy entirely),
    # so auth here does NOT affect device uploads.

    # Redirect bare root to /tracker/'''

if OLD_HEADERS not in tmpl:
    print("ERROR: could not find target block in nginx template (already patched?)")
    sys.exit(1)

# Only patch the SECOND occurrence (susmannet HTTPS server block, not docucraft)
# Count occurrences to be safe
count = tmpl.count(OLD_HEADERS)
if count != 1:
    print(f"WARNING: found {count} occurrences of the header block; patching first/only match")

tmpl_new = tmpl.replace(OLD_HEADERS, NEW_HEADERS, 1)
open(TMPL, "w").write(tmpl_new)
print(f"nginx.conf.template: auth_basic block added ({TMPL})")

# ── 2. Patch entrypoint.sh ───────────────────────────────────────────────────
EP = "/root/docucraft/nginx/entrypoint.sh"
ep = open(EP).read()

HTPASSWD_BLOCK = '''
# Generate htpasswd file for tracker auth from env vars.
# TRACKER_USER and TRACKER_PASS must be set in the container environment.
if [ -n "${TRACKER_USER:-}" ] && [ -n "${TRACKER_PASS:-}" ]; then
  HTPASSWD_PATH="/etc/nginx/tracker_htpasswd"
  # openssl passwd -apr1 produces an MD5 crypt hash compatible with nginx auth_basic.
  HASH=$(openssl passwd -apr1 "${TRACKER_PASS}")
  printf '%s:%s\\n' "${TRACKER_USER}" "${HASH}" > "${HTPASSWD_PATH}"
  chmod 600 "${HTPASSWD_PATH}"
  echo "[susman-ingress] tracker_htpasswd written for user '${TRACKER_USER}'"
else
  echo "[susman-ingress] WARNING: TRACKER_USER or TRACKER_PASS not set — auth disabled"
  # Write an impossible htpasswd so nginx starts but no credentials work
  echo "disabled:!" > /etc/nginx/tracker_htpasswd
fi
'''

# Insert the htpasswd block right before `exec nginx -g 'daemon off;'`
OLD_EXEC = "exec nginx -g 'daemon off;'"
if OLD_EXEC not in ep:
    print("ERROR: could not find exec line in entrypoint.sh")
    sys.exit(1)

ep_new = ep.replace(OLD_EXEC, HTPASSWD_BLOCK + OLD_EXEC)
open(EP, "w").write(ep_new)
print(f"entrypoint.sh: htpasswd generation block added ({EP})")

print("\nDone. Next steps:")
print("  1. Set TRACKER_USER and TRACKER_PASS in ~/docucraft/.env (or the compose env)")
print("  2. cd ~/docucraft && docker compose -f docker-compose.prod.yml build susman-ingress")
print("  3. docker compose -f docker-compose.prod.yml up -d susman-ingress")
