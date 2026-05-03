// API_BASE is injected at runtime by nginx via /config.js (see public/config.js
// and the Dockerfile). Falls back to same-origin for `npm run dev`.
const RUNTIME = (typeof window !== 'undefined' && window.__VEGA_CONFIG__) || {};
export const API_BASE =
  RUNTIME.apiBase ||
  import.meta.env.VITE_API_URL ||
  'http://192.168.86.48:8030';

// ---- read ------------------------------------------------------------------

export async function fetchSessions({ includeDeleted = false } = {}) {
  const params = new URLSearchParams({ limit: 500 });
  if (includeDeleted) params.set('include_deleted', 'true');
  const r = await fetch(`${API_BASE}/sessions?${params}`);
  if (!r.ok) throw new Error(`sessions ${r.status}`);
  return r.json();
}

// Pull all rows for a session; the API pages at 5000/page so we walk it.
export async function fetchSessionRows(sessionId, { pageSize = 5000 } = {}) {
  let skip = 0;
  const out = [];
  for (;;) {
    const r = await fetch(
      `${API_BASE}/sessions/${encodeURIComponent(sessionId)}?limit=${pageSize}&skip=${skip}`
    );
    if (!r.ok) throw new Error(`session ${sessionId} ${r.status}`);
    const j = await r.json();
    if (!j.rows || j.rows.length === 0) break;
    for (const row of j.rows) out.push(row);
    if (j.rows.length < pageSize) break;
    skip += j.rows.length;
    if (skip > 200000) break; // safety cap
  }
  return out;
}

// ---- management ------------------------------------------------------------

/** Rename a session (sets displayName field; sessionId is not changed). */
export async function renameSession(sessionId, displayName) {
  const r = await fetch(`${API_BASE}/sessions/${encodeURIComponent(sessionId)}`, {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ displayName }),
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`rename failed: ${msg}`);
  }
  return r.json();
}

/** Soft-delete a session (sets deletedAt; samples untouched; recoverable via restoreSession). */
export async function deleteSession(sessionId) {
  const r = await fetch(
    `${API_BASE}/sessions/${encodeURIComponent(sessionId)}?confirm=DELETE_CONFIRMED`,
    { method: 'DELETE' }
  );
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`delete failed: ${msg}`);
  }
  return r.json();
}

/** Restore a soft-deleted session (clears deletedAt; all samples still intact). */
export async function restoreSession(sessionId) {
  const r = await fetch(
    `${API_BASE}/sessions/${encodeURIComponent(sessionId)}/restore`,
    { method: 'PATCH' }
  );
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`restore failed: ${msg}`);
  }
  return r.json();
}

/** Permanently purge a session and ALL its samples.
 *  Session must already be soft-deleted. This is irreversible. */
export async function purgeSession(sessionId) {
  const r = await fetch(
    `${API_BASE}/admin/purge/${encodeURIComponent(sessionId)}?confirm=PURGE_CONFIRMED`,
    { method: 'POST' }
  );
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`purge failed: ${msg}`);
  }
  return r.json();
}

/** Merge source sessions into targetId. Sources are removed after merge. */
export async function mergeSessions(sourceIds, targetId) {
  const r = await fetch(`${API_BASE}/admin/merge-sessions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sourceIds, targetId }),
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`merge failed: ${msg}`);
  }
  return r.json();
}

// ---- export ----------------------------------------------------------------

/** Trigger a browser download of one session.
  format: 'radiacode' | 'internal'  */
export function exportSession(sessionId, format = 'radiacode') {
  const url = `${API_BASE}/sessions/${encodeURIComponent(sessionId)}/export?format=${format}`;
  _triggerDownload(url, `${sessionId}_${format}.csv`);
}

/** POST bulk export — multiple sessions merged into one CSV download. */
export async function exportBulk(ids, format = 'radiacode') {
  const r = await fetch(`${API_BASE}/sessions/export-bulk`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ids, format }),
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`bulk export failed: ${msg}`);
  }
  const blob = await r.blob();
  const filename = `bulk_export_${format}.csv`;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  a.click();
  URL.revokeObjectURL(a.href);
}

function _triggerDownload(url, filename) {
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
}

// ---- database stats & backups ----------------------------------------------

export async function fetchDbStats() {
  const r = await fetch(`${API_BASE}/admin/db-stats`);
  if (!r.ok) throw new Error(`db-stats ${r.status}`);
  return r.json();
}

export async function fetchBackups() {
  const r = await fetch(`${API_BASE}/admin/backups`);
  if (!r.ok) throw new Error(`backups ${r.status}`);
  return r.json();
}

export async function createBackup() {
  const r = await fetch(`${API_BASE}/admin/backup`, { method: 'POST' });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`backup failed: ${msg}`);
  }
  return r.json();
}

export async function deleteBackup(name) {
  const r = await fetch(
    `${API_BASE}/admin/backup/${encodeURIComponent(name)}?confirm=DELETE_CONFIRMED`,
    { method: 'DELETE' }
  );
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`delete backup failed: ${msg}`);
  }
  return r.json();
}

export async function restoreBackup(name) {
  const r = await fetch(
    `${API_BASE}/admin/restore/${encodeURIComponent(name)}?confirm=RESTORE_CONFIRMED`,
    { method: 'POST' }
  );
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`restore failed: ${msg}`);
  }
  return r.json();
}
