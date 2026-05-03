// DatabasePanel — full database management: stats, backup, restore, delete backup.
import { useState, useEffect, useCallback } from 'react';
import { fetchDbStats, fetchBackups, createBackup, deleteBackup, restoreBackup } from './api.js';

function fmtBytes(bytes) {
  if (bytes == null || isNaN(bytes)) return '—';
  if (bytes < 1024)       return `${bytes} B`;
  if (bytes < 1048576)    return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1073741824) return `${(bytes / 1048576).toFixed(2)} MB`;
  return `${(bytes / 1073741824).toFixed(2)} GB`;
}

function fmtDate(tsMs) {
  if (!tsMs) return '—';
  return new Date(tsMs).toLocaleString();
}

function fmtElapsed(s) {
  if (s == null) return '';
  return s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(1)} s`;
}

// ---- small stat card -------------------------------------------------------
function DbCard({ label, value, sub, accent }) {
  return (
    <div className={`stat-card ${accent ? 'accent' : ''}`}>
      <div className="stat-label">{label}</div>
      <div className="stat-value">{value}</div>
      {sub && <div className="db-card-sub">{sub}</div>}
    </div>
  );
}

// ---- backup row with delete + restore (triple-confirm for restore) ---------
function BackupRow({ backup, onDeleted, onRestored, onError }) {
  const [delStep,  setDelStep]  = useState(0);   // 0=idle 1=confirm 2=deleting
  const [restStep, setRestStep] = useState(0);   // 0=idle 1=warn 2=confirm 3=restoring
  const [busy, setBusy]         = useState(false);

  async function doDelete() {
    setDelStep(2); setBusy(true);
    try {
      await deleteBackup(backup.name);
      onDeleted(backup.name);
    } catch (e) {
      onError(String(e)); setDelStep(0);
    } finally { setBusy(false); }
  }

  async function doRestore() {
    setRestStep(3); setBusy(true);
    try {
      const r = await restoreBackup(backup.name);
      onRestored(backup.name, r);
    } catch (e) {
      onError(String(e)); setRestStep(0);
    } finally { setBusy(false); }
  }

  const date = backup.tsMs
    ? new Date(backup.tsMs).toLocaleString()
    : backup.name;

  const sourceLabel = backup.source === 'cron' ? '⏰ cron'
                    : backup.source === 'manual' ? '👤 manual'
                    : null;

  return (
    <div className="backup-row">
      <div className="backup-info">
        <span className="backup-name">{backup.name}</span>
        <span className="backup-date">{date}</span>
        <span className="badge">{fmtBytes(backup.sizeBytes)}</span>
        {sourceLabel && (
          <span className={`badge badge-source-${backup.source}`}>{sourceLabel}</span>
        )}
        {backup.status === 'failed' && (
          <span className="badge badge-failed">✗ failed</span>
        )}
        {backup.elapsedSec != null && (
          <span className="badge badge-elapsed">{fmtElapsed(backup.elapsedSec)}</span>
        )}
      </div>

      <div className="backup-actions">
        {/* Delete flow */}
        {delStep === 0 && restStep === 0 && (
          <button className="btn-sm btn-danger-outline" onClick={() => setDelStep(1)} disabled={busy}>
            🗑 Delete
          </button>
        )}
        {delStep === 1 && (
          <>
            <span className="warn-text">Delete this backup?</span>
            <button className="btn-sm btn-danger" onClick={doDelete} disabled={busy}>Confirm</button>
            <button className="btn-sm" onClick={() => setDelStep(0)}>Cancel</button>
          </>
        )}
        {delStep === 2 && <span className="muted">Deleting…</span>}

        {/* Restore flow — triple confirm */}
        {delStep === 0 && restStep === 0 && (
          <button className="btn-sm" onClick={() => setRestStep(1)} disabled={busy}>
            ↺ Restore
          </button>
        )}
        {restStep === 1 && (
          <>
            <span className="warn-text">⚠ This will DROP current data</span>
            <button className="btn-sm btn-danger" onClick={() => setRestStep(2)}>I understand</button>
            <button className="btn-sm" onClick={() => setRestStep(0)}>Cancel</button>
          </>
        )}
        {restStep === 2 && (
          <>
            <span className="warn-text">ALL current data will be erased</span>
            <button className="btn-sm btn-danger-bright" onClick={doRestore} disabled={busy}>
              RESTORE NOW
            </button>
            <button className="btn-sm" onClick={() => setRestStep(0)}>Cancel</button>
          </>
        )}
        {restStep === 3 && <span className="muted">Restoring…</span>}
      </div>
    </div>
  );
}

// ---- main DatabasePanel ----------------------------------------------------
export function DatabasePanel({ onError }) {
  const [stats,    setStats]    = useState(null);
  const [backups,  setBackups]  = useState(null);
  const [bkMeta,   setBkMeta]   = useState(null);   // keepCount + lastBackup from API
  const [loading,  setLoading]  = useState(true);
  const [backing,  setBacking]  = useState(false);
  const [lastMsg,  setLastMsg]  = useState(null);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const [s, b] = await Promise.all([fetchDbStats(), fetchBackups()]);
      setStats(s);
      setBackups(b.backups || []);
      setBkMeta({ keepCount: b.keepCount, lastBackup: s.lastBackup });
    } catch (e) {
      onError(String(e));
    } finally {
      setLoading(false);
    }
  }, [onError]);

  useEffect(() => { load(); }, [load]);

  async function doBackup() {
    setBacking(true); setLastMsg(null);
    try {
      const r = await createBackup();
      setLastMsg(`Backup created: ${r.backup} (${fmtBytes(r.sizeBytes)}, ${fmtElapsed(r.elapsedSec)})`);
      load();
    } catch (e) {
      onError(String(e));
    } finally {
      setBacking(false);
    }
  }

  function handleDeleted(name) {
    setBackups(prev => prev.filter(b => b.name !== name));
    setLastMsg(`Deleted backup: ${name}`);
  }

  function handleRestored(name, r) {
    setLastMsg(`Restored from ${name} in ${fmtElapsed(r.elapsedSec)}`);
    load();
  }

  return (
    <div className="panel-scroll">
      {/* === DB STATS === */}
      <div className="section-head">Storage Metrics</div>
      {loading && <div className="db-loading">Loading…</div>}

      {stats && (
        <>
          <div className="stat-grid" style={{ gridTemplateColumns: 'repeat(2, 1fr)' }}>
            <DbCard label="Documents" value={stats.dbObjects?.toLocaleString()} />
            <DbCard label="Sessions" value={stats.sessions?.count?.toLocaleString()} />
            <DbCard label="Data size (uncompressed)" value={fmtBytes(stats.dbDataSize)} />
            <DbCard label="On-disk (compressed)" value={fmtBytes(stats.dbStorageSize)} accent />
            <DbCard label="Index size" value={fmtBytes(stats.dbIndexSize)} />
            <DbCard
              label="Avg sample size"
              value={fmtBytes(stats.samples?.avgDocBytes)}
              sub="per document"
            />
          </div>

          <div className="section-head">Samples Collection</div>
          <div className="stat-grid" style={{ gridTemplateColumns: 'repeat(2, 1fr)' }}>
            <DbCard label="Sample count" value={stats.samples?.count?.toLocaleString()} />
            <DbCard label="Samples storage" value={fmtBytes(stats.samples?.storageSize)} accent />
          </div>

          {!stats.mongodumpAvail && (
            <div className="db-warn-banner">
              ⚠ mongodump is not installed in this container — backups unavailable until
              image is rebuilt.
            </div>
          )}
        </>
      )}

      {/* === BACKUP CONTROLS === */}
      <div className="section-head">Backups</div>
      <div className="db-backup-info">
        Backups are stored at <code>{stats?.backupDir || '/backups'}</code> outside the
        container (host-mounted volume). They survive container restarts and upgrades.
      </div>

      {/* === LAST BACKUP SUMMARY === */}
      {bkMeta?.lastBackup ? (
        <div className="db-last-backup">
          <span className="lb-label">Last successful backup:</span>
          <span className="lb-time">{fmtDate(bkMeta.lastBackup.tsMs)}</span>
          {bkMeta.lastBackup.source === 'cron' && <span className="badge badge-source-cron">⏰ cron</span>}
          {bkMeta.lastBackup.source === 'manual' && <span className="badge badge-source-manual">👤 manual</span>}
          <span className="badge">{fmtBytes(bkMeta.lastBackup.sizeBytes)}</span>
        </div>
      ) : (
        <div className="db-last-backup lb-none">No successful backup recorded yet.</div>
      )}

      {lastMsg && <div className="db-ok-banner">{lastMsg}</div>}

      <div style={{ padding: '8px 12px' }}>
        <button
          className="btn-sm btn-accent"
          style={{ width: '100%' }}
          onClick={doBackup}
          disabled={backing || !stats?.mongodumpAvail}
        >
          {backing ? '⏳ Creating backup…' : '📦 Create backup now (mongodump)'}
        </button>
      </div>

      <div style={{ padding: '0 12px 8px', display: 'flex', justifyContent: 'flex-end' }}>
        <button className="btn-sm" onClick={load} disabled={loading}>↻ Refresh</button>
      </div>

      {/* === BACKUP LIST === */}
      {backups !== null && backups.length === 0 && (
        <div className="db-no-backups">
          No backups yet. Click "Create backup now" to take the first snapshot.
        </div>
      )}

      {backups && backups.map(b => (
        <BackupRow
          key={b.name}
          backup={b}
          onDeleted={handleDeleted}
          onRestored={handleRestored}
          onError={msg => { onError(msg); setLastMsg(null); }}
        />
      ))}

      {/* === CRON STATUS === */}
      <div className="section-head">Automatic Backups</div>
      <div className="db-backup-info">
        A weekly cron job runs every Sunday at 03:00 UTC and keeps the{' '}
        <strong>{bkMeta?.keepCount ?? 5} most recent</strong> snapshots.
        Backs up the <strong>entire database</strong> (all collections/databases).
        Snapshots are stored on the host at{' '}
        <code>/home/darkmatter2222/vega-tracker-backups/</code> and survive
        container restarts and upgrades.
      </div>
    </div>
  );
}
