// API_BASE is injected at runtime by nginx via /config.js (see public/config.js
// and the Dockerfile). Defaults to /api (relative) so the browser resolves it
// against whichever origin it loaded from — works for both the DuckDNS proxy
// and direct LAN IP access (viewer nginx proxies /api/ to the ingest port).
const RUNTIME = (typeof window !== 'undefined' && window.__APP_CONFIG__) || {};
export const API_BASE =
  RUNTIME.apiBase ||
  import.meta.env.VITE_API_URL ||
  '/api';

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

/** Return the upload history for a session, newest first.
 *  Each entry: receivedAt, payloadBytes, rowsSeen/Accepted/Rejected/Inserted/Duplicate,
 *  clientIp, username, firmware, trackerId, durationMs, httpStatus. */
export async function fetchSessionUploads(sessionId, limit = 100) {
  const r = await fetch(
    `${API_BASE}/sessions/${encodeURIComponent(sessionId)}/uploads?limit=${limit}`
  );
  if (!r.ok) throw new Error(`session uploads ${r.status}`);
  return r.json();
}

/** Return per-day upload activity aggregated across all sessions, oldest first.
 *  Each entry: {date, uploads, rowsInserted, rowsRejected}
 *  Date is YYYY-MM-DD in America/New_York timezone. */
export async function fetchDailyStats(days = 90) {
  const r = await fetch(`${API_BASE}/admin/daily-stats?days=${days}`);
  if (!r.ok) throw new Error(`daily-stats ${r.status}`);
  return r.json();
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

// ---- Explorer: missions ----------------------------------------------------

export async function fetchMissions(status = null) {
  const params = status ? `?status=${encodeURIComponent(status)}` : '';
  const r = await fetch(`${API_BASE}/missions${params}`);
  if (!r.ok) throw new Error(`missions ${r.status}`);
  return r.json();
}

export async function createMission({ name, polygon, centroid, areaKm2 = 0, score = 0, notes = '' }) {
  const r = await fetch(`${API_BASE}/missions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name, polygon, centroid, areaKm2, score, notes }),
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`create mission failed: ${msg}`);
  }
  return r.json();
}

export async function updateMission(missionId, updates) {
  const r = await fetch(`${API_BASE}/missions/${encodeURIComponent(missionId)}`, {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(updates),
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`update mission failed: ${msg}`);
  }
  return r.json();
}

export async function deleteMission(missionId) {
  const r = await fetch(`${API_BASE}/missions/${encodeURIComponent(missionId)}`, {
    method: 'DELETE',
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`delete mission failed: ${msg}`);
  }
  return r.json();
}

// ---- Explorer: coverage analysis -------------------------------------------

export async function analyzeCoverage({
  maxSpeedKph = 50, maxHdop = 3.0, maxAccuracyM = 15,
  gridDeg = 0.002, topN = 15, paddingFactor = 0.15,
  maxZoneSqMi = 25, minZoneSqMi = 0.3, distPeakKm = 5,
} = {}) {
  const params = new URLSearchParams({
    max_speed_kph:  maxSpeedKph,
    max_hdop:       maxHdop,
    max_accuracy_m: maxAccuracyM,
    grid_deg:       gridDeg,
    top_n:          topN,
    padding_factor: paddingFactor,
    max_zone_sq_mi: maxZoneSqMi,
    min_zone_sq_mi: minZoneSqMi,
    dist_peak_km:   distPeakKm,
  });
  const r = await fetch(`${API_BASE}/explorer/analyze-coverage?${params}`, { method: 'POST' });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(`coverage analysis failed: ${msg}`);
  }
  return r.json();
}

export async function fetchLatestAnalysis() {
  const r = await fetch(`${API_BASE}/explorer/analyses/latest`);
  if (r.status === 404) return null;
  if (!r.ok) throw new Error(`fetch latest analysis: ${r.status}`);
  return r.json();
}

export async function fetchZoneCoverage({ minLat, maxLat, minLng, maxLng, gridDeg = 0.002,
  maxSpeedKph = 50, maxHdop = 3.0, maxAccuracyM = 15 } = {}) {
  const params = new URLSearchParams({
    min_lat: minLat, max_lat: maxLat, min_lng: minLng, max_lng: maxLng,
    grid_deg: gridDeg, max_speed_kph: maxSpeedKph,
    max_hdop: maxHdop, max_accuracy_m: maxAccuracyM,
  });
  const r = await fetch(`${API_BASE}/explorer/zone-coverage?${params}`);
  if (!r.ok) throw new Error(`zone-coverage ${r.status}`);
  return r.json();
}

// ---- Explorer: live samples (real-time dose feed) --------------------------

export async function fetchLiveSamples({ sinceMs = 0, limit = 20 } = {}) {
  const params = new URLSearchParams({ since_ms: sinceMs, limit });
  const r = await fetch(`${API_BASE}/explorer/live-samples?${params}`);
  if (!r.ok) throw new Error(`live-samples ${r.status}`);
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

// ---- time-range export -----------------------------------------------------

/**
 * Get an estimate of rows / size for a time window without fetching data.
 * @param {number} startMs  Unix epoch ms, inclusive
 * @param {number} endMs    Unix epoch ms, inclusive
 * @returns {{ rowCount, estimatedBytes, estimatedMB, estimatedFiles }}
 */
export async function fetchExportPreview(startMs, endMs, gpsOnly = false) {
  const r = await fetch(`${API_BASE}/export/time-range/preview?startMs=${startMs}&endMs=${endMs}&gpsOnly=${gpsOnly}`);
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(msg);
  }
  return r.json();
}

/**
 * Export all samples in a time window.  Downloads a single file or a ZIP
 * automatically based on whether data exceeds maxBytesPerFile (10 MB).
 * @param {number} startMs
 * @param {number} endMs
 * @param {'radiacode_txt'|'radiacode'|'internal'} format
 * @param {number} maxBytesPerFile  default 10 MB
 */
export async function exportTimeRange(startMs, endMs, format = 'radiacode_txt', maxBytesPerFile = 10 * 1024 * 1024, label = '', gpsOnly = false) {
  const r = await fetch(`${API_BASE}/export/time-range`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ startMs, endMs, format, maxBytesPerFile, label, gpsOnly }),
  });
  if (!r.ok) {
    const msg = await r.text().catch(() => r.status);
    throw new Error(msg);
  }
  // Derive filename from Content-Disposition header.
  const cd = r.headers.get('Content-Disposition') || '';
  const fnMatch = cd.match(/filename="([^"]+)"/);
  const isZip = r.headers.get('Content-Type') === 'application/zip';
  // Fallback filename in case Content-Disposition is missing or stripped by a proxy.
  const extMap  = { radiacode_txt: 'txt', radiacode_trk: 'rctrk', radiacode: 'csv', internal: 'csv' };
  const slugMap = { radiacode_txt: 'radiacode', radiacode_trk: 'radiacode', radiacode: 'radiacode-csv', internal: 'internal-csv' };
  const startDateStr = new Date(startMs).toISOString().slice(0, 10);
  const endDateStr   = new Date(endMs).toISOString().slice(0, 10);
  const dateSlug = startDateStr === endDateStr ? startDateStr : `${startDateStr}_to_${endDateStr}`;
  const fmtExt  = extMap[format]  || 'txt';
  const fmtSlug = slugMap[format] || format;
  const fallback = isZip
    ? `radmap_${dateSlug}_${fmtSlug}.zip`
    : `radmap_${dateSlug}_${fmtSlug}.${fmtExt}`;
  const filename = fnMatch ? fnMatch[1] : fallback;

  const blob = await r.blob();
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
  return { filename, sizeBytes: blob.size };
}
