#!/bin/sh
# Substitute API_BASE into /usr/share/nginx/html/config.js at container start
# so the same image can point at different backends without a rebuild.
set -eu

CONFIG_JS=/usr/share/nginx/html/config.js
# Default to /api (relative path) so the browser resolves against its current
# origin. This makes both DuckDNS proxy and direct LAN IP access work without
# hairpin NAT. Override with API_BASE env var if an absolute URL is needed.
API_BASE_VALUE="${API_BASE:-/api}"

# Escape ampersand and slashes for sed.
ESC=$(printf '%s' "$API_BASE_VALUE" | sed -e 's/[\/&]/\\&/g')

if [ -f "$CONFIG_JS" ]; then
    sed -i "s|__API_BASE__|${ESC}|g" "$CONFIG_JS"
    echo "[entrypoint] config.js patched with API_BASE=${API_BASE_VALUE}"
fi

# Generate htpasswd file for Basic Auth from env vars.
# Same credentials as susman-ingress proxy so both paths require the same login.
HTPASSWD_PATH="/etc/nginx/tracker_htpasswd"
if [ -n "${TRACKER_USER:-}" ] && [ -n "${TRACKER_PASS:-}" ]; then
    # openssl passwd -apr1 is available in the nginx:alpine base image.
    HASH=$(openssl passwd -apr1 "${TRACKER_PASS}")
    printf '%s:%s\n' "${TRACKER_USER}" "${HASH}" > "${HTPASSWD_PATH}"
    chmod 644 "${HTPASSWD_PATH}"
    echo "[entrypoint] tracker_htpasswd written for user '${TRACKER_USER}'"
else
    echo "[entrypoint] WARNING: TRACKER_USER or TRACKER_PASS not set — auth disabled"
    echo "disabled:!" > "${HTPASSWD_PATH}"
fi
