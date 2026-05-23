// ManagePanel — data management UI: rename, soft-delete/restore/purge, merge, export, upload history.
import { useState, useMemo, useRef, useEffect } from 'react';
import { renameSession, deleteSession, restoreSession, purgeSession, mergeSessions, exportSession, exportBulk, fetchSessions, fetchSessionUploads } from './api.js';
import { sessionColor, fmtTs, fmtDose } from './colors.js';

const MIN_VALID_TS_MS = 1577836800000;

function fmtDuration(ms) {
  if (!ms || ms < 0) return '';
  const s = Math.round(ms / 1000);
  if (s < 60)   return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  return `${(s / 3600).toFixed(2)}h`;
}

// ---- Rename row -----------------------------------------------------------
function RenameRow({ session, onRenamed, onError }) {
  const [editing, setEditing] = useState(false);
  const [val, setVal]         = useState(session.displayName || session.sessionId);
  const [busy, setBusy]       = useState(false);

  async function save() {
    if (!val.trim()) return;
    setBusy(true);
    try {
      await renameSession(session.sessionId, val.trim());
      onRenamed(session.sessionId, val.trim());
      setEditing(false);
    } catch (e) {
      onError(String(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="mgmt-row">
      <span className="swatch" style={{ background: sessionColor(session._idx ?? 0) }} />
      {editing ? (
        <input className="rename-input" value={val} autoFocus
          onChange={e => setVal(e.target.value)}
          onKeyDown={e => { if (e.key === 'Enter') save(); if (e.key === 'Escape') setEditing(false); }}
        />
      ) : (
        <span className="mgmt-name" title={session.sessionId}>
          {session.displayName || session.sessionId}
          {session.displayName && (
            <span className="mgmt-sid-sub"> ({session.sessionId})</span>
          )}
        </span>
      )}
      {editing ? (
        <>
          <button onClick={save} disabled={busy} className="btn-sm btn-accent">Save</button>
          <button onClick={() => setEditing(false)} className="btn-sm">Cancel</button>
        </>
      ) : (
        <button onClick={() => { setVal(session.displayName || session.sessionId); setEditing(true); }}
          className="btn-sm">Rename</button>
      )}
    </div>
  );
}

// ---- Soft-delete / restore / purge row ------------------------------------
function SoftDeleteRow({ session, onSoftDeleted, onRestored, onPurged, onError }) {
  const [delStep,   setDelStep]   = useState(0); // 0=idle 1=confirm 2=deleting
  const [purgeStep, setPurgeStep] = useState(0); // 0=idle 1=warn 2=confirm 3=purging
  const [busy, setBusy]           = useState(false);
  const isDeleted = !!session.deletedAt;

  async function doSoftDelete() {
    setDelStep(2); setBusy(true);
    try {
      await deleteSession(session.sessionId);
      onSoftDeleted(session.sessionId);
    } catch (e) {
      onError(String(e)); setDelStep(0);
    } finally { setBusy(false); }
  }

  async function doRestore() {
    setBusy(true);
    try {
      await restoreSession(session.sessionId);
      onRestored(session.sessionId);
    } catch (e) {
      onError(String(e));
    } finally { setBusy(false); }
  }

  async function doPurge() {
    setPurgeStep(3); setBusy(true);
    try {
      await purgeSession(session.sessionId);
      onPurged(session.sessionId);
    } catch (e) {
      onError(String(e)); setPurgeStep(0);
    } finally { setBusy(false); }
  }

  const deletedDate = isDeleted ? new Date(session.deletedAt).toLocaleString() : null;

  return (
    <div className={`mgmt-row${isDeleted ? ' session-deleted' : ''}`}>
      <span className="swatch" style={{ background: sessionColor(session._idx ?? 0) }} />
      <span className="mgmt-name" title={session.sessionId}>
        {session.displayName || session.sessionId}
        {session.displayName && !isDeleted && (
          <span className="mgmt-sid-sub"> ({session.sessionId})</span>
        )}
        {isDeleted && (
          <span className="mgmt-deleted-tag"> — deleted {deletedDate}</span>
        )}
      </span>

      {/* Active session: soft-delete with single confirm */}
      {!isDeleted && (
        <>
          {delStep === 0 && (
            <button className="btn-sm btn-danger-outline" onClick={() => setDelStep(1)} disabled={busy}>
              Delete
            </button>
          )}
          {delStep === 1 && (
            <>
              <span className="warn-text">Move to deleted? (recoverable)</span>
              <button className="btn-sm btn-danger" onClick={doSoftDelete} disabled={busy}>Confirm</button>
              <button className="btn-sm" onClick={() => setDelStep(0)}>Cancel</button>
            </>
          )}
          {delStep === 2 && <span className="muted">Deleting…</span>}
        </>
      )}

      {/* Deleted session: restore or purge (purge is triple-step) */}
      {isDeleted && (
        <>
          <button className="btn-sm btn-accent" onClick={doRestore} disabled={busy}>
            ↺ Restore
          </button>
          {purgeStep === 0 && (
            <button className="btn-sm btn-danger-outline" onClick={() => setPurgeStep(1)} disabled={busy}>
              Purge
            </button>
          )}
          {purgeStep === 1 && (
            <>
              <span className="warn-text">⚠ Deletes all samples — permanent</span>
              <button className="btn-sm btn-danger" onClick={() => setPurgeStep(2)}>Continue</button>
              <button className="btn-sm" onClick={() => setPurgeStep(0)}>Cancel</button>
            </>
          )}
          {purgeStep === 2 && (
            <>
              <span className="warn-text">CANNOT BE UNDONE</span>
              <button className="btn-sm btn-danger-bright" onClick={doPurge} disabled={busy}>
                PURGE FOREVER
              </button>
              <button className="btn-sm" onClick={() => setPurgeStep(0)}>Cancel</button>
            </>
          )}
          {purgeStep === 3 && <span className="muted">Purging…</span>}
        </>
      )}
    </div>
  );
}

// ---- Delete/Restore tab ---------------------------------------------------
function DeleteRestoreTab({ sessions, onSoftDeleted, onRestored, onPurged, onError }) {
  const [showDeleted,    setShowDeleted]    = useState(false);
  const [deletedList,    setDeletedList]    = useState([]);
  const [loadingDeleted, setLoadingDeleted] = useState(false);
  // Generation counter so a stale in-flight fetch can't overwrite a newer one.
  // Toggling "show deleted" rapidly used to allow whichever response landed
  // last to win, regardless of which call was actually the most recent.
  const loadGenRef = useRef(0);

  async function loadDeleted() {
    const gen = ++loadGenRef.current;
    setLoadingDeleted(true);
    try {
      const all = await fetchSessions({ includeDeleted: true });
      if (gen !== loadGenRef.current) return;  // newer fetch already started
      setDeletedList(all.filter(s => s.deletedAt));
    } catch (e) {
      if (gen !== loadGenRef.current) return;
      onError(String(e));
    } finally {
      if (gen === loadGenRef.current) setLoadingDeleted(false);
    }
  }

  function toggleShowDeleted() {
    if (!showDeleted) loadDeleted();
    setShowDeleted(v => !v);
  }

  function handleSoftDeleted(sessionId) {
    onSoftDeleted(sessionId);
    if (showDeleted) loadDeleted();
  }

  function handleRestored(sessionId) {
    onRestored(sessionId);
    loadDeleted();
  }

  function handlePurged(sessionId) {
    onPurged(sessionId);
    setDeletedList(prev => prev.filter(s => s.sessionId !== sessionId));
  }

  return (
    <>
      <div className="section-head">
        Sessions are soft-deleted — hidden from the map but never removed from the database.
        Restore any deleted session at any time. Only Purge permanently removes data.
      </div>

      <div className="deleted-toggle-bar">
        <label className="deleted-toggle">
          <input type="checkbox" checked={showDeleted} onChange={toggleShowDeleted} />
          <span>Show deleted sessions</span>
        </label>
        {showDeleted && deletedList.length > 0 && (
          <span className="badge badge-failed">{deletedList.length} deleted</span>
        )}
      </div>

      <div className="section-sub-head">Active sessions</div>
      {sessions.length === 0 && (
        <div className="muted" style={{ padding: '8px 12px', fontSize: 11 }}>No active sessions.</div>
      )}
      {sessions.map(s => (
        <SoftDeleteRow key={s.sessionId} session={s}
          onSoftDeleted={handleSoftDeleted}
          onRestored={handleRestored}
          onPurged={handlePurged}
          onError={onError}
        />
      ))}

      {showDeleted && (
        <>
          <div className="section-sub-head">
            Deleted sessions {loadingDeleted && <span className="muted">(loading…)</span>}
          </div>
          {!loadingDeleted && deletedList.length === 0 && (
            <div className="muted" style={{ padding: '8px 12px', fontSize: 11 }}>No deleted sessions.</div>
          )}
          {deletedList.map(s => (
            <SoftDeleteRow key={s.sessionId} session={{ ...s, _idx: 0 }}
              onSoftDeleted={handleSoftDeleted}
              onRestored={handleRestored}
              onPurged={handlePurged}
              onError={onError}
            />
          ))}
        </>
      )}
    </>
  );
}

// ---- Merge panel ----------------------------------------------------------
function MergePanel({ sessions, onMerged, onError }) {
  const [selectedSrc, setSelectedSrc] = useState(new Set());
  const [targetName, setTargetName]   = useState('');
  const [busy, setBusy]               = useState(false);

  function toggleSrc(id) {
    const next = new Set(selectedSrc);
    if (next.has(id)) next.delete(id); else next.add(id);
    setSelectedSrc(next);
  }

  async function doMerge() {
    const srcs = [...selectedSrc];
    const tgt  = targetName.trim();
    if (srcs.length < 2) { onError('Select at least 2 sessions to merge'); return; }
    if (!tgt) { onError('Enter a target session name'); return; }
    setBusy(true);
    try {
      const res = await mergeSessions(srcs, tgt);
      onMerged(res);
      setSelectedSrc(new Set());
      setTargetName('');
    } catch (e) {
      onError(String(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="merge-panel">
      <div className="mgmt-sub-head">Select sessions to merge into a new target:</div>
      <div className="merge-list">
        {sessions.map(s => (
          <label key={s.sessionId} className="merge-check">
            <input type="checkbox"
              checked={selectedSrc.has(s.sessionId)}
              onChange={() => toggleSrc(s.sessionId)}
            />
            <span className="swatch" style={{ background: sessionColor(s._idx ?? 0) }} />
            <span>{s.displayName || s.sessionId}</span>
          </label>
        ))}
      </div>
      {selectedSrc.size > 0 && (
        <div className="merge-target-row">
          <input className="rename-input" placeholder="Target session name"
            value={targetName} onChange={e => setTargetName(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && doMerge()}
          />
          <button className="btn-sm btn-accent" onClick={doMerge} disabled={busy}>
            {busy ? 'Merging...' : `Merge ${selectedSrc.size} → target`}
          </button>
        </div>
      )}
    </div>
  );
}

// ---- Export panel ---------------------------------------------------------
function ExportPanel({ sessions, onError }) {
  const [exportSel, setExportSel] = useState(new Set());
  const [fmt, setFmt]             = useState('radiacode');
  const [busy, setBusy]           = useState(false);

  function toggleExport(id) {
    const next = new Set(exportSel);
    if (next.has(id)) next.delete(id); else next.add(id);
    setExportSel(next);
  }

  async function doBulkExport() {
    if (!exportSel.size) { onError('Select at least 1 session to export'); return; }
    setBusy(true);
    try {
      await exportBulk([...exportSel], fmt);
    } catch (e) {
      onError(String(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="export-panel">
      <div className="mgmt-sub-head">Format:</div>
      <div className="mode-grid" style={{ paddingTop: 0 }}>
        <button className={`mode-btn ${fmt === 'radiacode' ? 'active' : ''}`}
          onClick={() => setFmt('radiacode')}>
          RadiaCode CSV
        </button>
        <button className={`mode-btn ${fmt === 'internal' ? 'active' : ''}`}
          onClick={() => setFmt('internal')}>
          Internal CSV
        </button>
      </div>
      <div className="mgmt-sub-head">Sessions:</div>
      <div className="merge-list">
        {sessions.map(s => {
          const firstOk = s.firstTsMs && s.firstTsMs >= MIN_VALID_TS_MS;
          const lastOk  = s.lastTsMs  && s.lastTsMs  >= MIN_VALID_TS_MS;
          const dur = (firstOk && lastOk) ? fmtDuration(s.lastTsMs - s.firstTsMs) : null;
          return (
            <div key={s.sessionId} className="export-row">
              <label className="merge-check">
                <input type="checkbox"
                  checked={exportSel.has(s.sessionId)}
                  onChange={() => toggleExport(s.sessionId)}
                />
                <span className="swatch" style={{ background: sessionColor(s._idx ?? 0) }} />
                <span>{s.displayName || s.sessionId}</span>
                {dur && <span className="badge">{dur}</span>}
                {s.samples && <span className="badge">{s.samples} pts</span>}
              </label>
              <div className="export-row-btns">
                <button className="btn-sm" onClick={() => exportSession(s.sessionId, 'radiacode')}
                  title="Download this session as RadiaCode CSV">RC</button>
                <button className="btn-sm" onClick={() => exportSession(s.sessionId, 'internal')}
                  title="Download this session as internal CSV">INT</button>
              </div>
            </div>
          );
        })}
      </div>
      {exportSel.size > 0 && (
        <div style={{ padding: '8px 12px' }}>
          <button className="btn-sm btn-accent" onClick={doBulkExport} disabled={busy}
            style={{ width: '100%' }}>
            {busy ? 'Preparing download...' : `Export ${exportSel.size} session${exportSel.size > 1 ? 's' : ''} as merged ${fmt === 'radiacode' ? 'RadiaCode' : 'internal'} CSV`}
          </button>
        </div>
      )}
    </div>
  );
}

// ---- Uploads tab ---------------------------------------------------------
function UploadsTab({ sessions }) {
  const [selectedId, setSelectedId] = useState(() => sessions[0]?.sessionId ?? null);
  const [uploads,    setUploads]    = useState(null);
  const [loading,    setLoading]    = useState(false);
  const [error,      setError]      = useState(null);

  async function load(sessionId) {
    if (!sessionId) return;
    setSelectedId(sessionId);
    setLoading(true);
    setError(null);
    setUploads(null);
    try {
      const list = await fetchSessionUploads(sessionId);
      setUploads(list);
    } catch (e) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  }

  // Auto-load the first session on mount.
  useEffect(() => {
    if (sessions.length > 0) load(sessions[0].sessionId);
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  function fmtTime(ms) {
    if (!ms) return '—';
    return new Date(ms).toLocaleString();
  }
  function fmtKb(bytes) {
    if (bytes == null) return '—';
    return `${(bytes / 1024).toFixed(1)} KB`;
  }

  return (
    <div>
      {/* Session picker — single dropdown */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px' }}>
        <label style={{ fontSize: 11, color: 'var(--muted)', whiteSpace: 'nowrap' }}>Session:</label>
        <select
          value={selectedId ?? ''}
          onChange={e => load(e.target.value)}
          style={{
            flex: 1, background: 'var(--bg2)', color: 'var(--fg)',
            border: '1px solid var(--border)', borderRadius: 4,
            padding: '3px 6px', fontSize: 12,
          }}>
          {sessions.map(s => (
            <option key={s.sessionId} value={s.sessionId}>
              {s.displayName || s.sessionId}
            </option>
          ))}
        </select>
      </div>

      {loading && (
        <div className="muted" style={{ padding: '8px 12px', fontSize: 11 }}>Loading…</div>
      )}
      {error && (
        <div style={{ padding: '8px 12px', color: 'var(--danger)', fontSize: 11 }}>{error}</div>
      )}

      {uploads !== null && !loading && (
        uploads.length === 0 ? (
          <div className="muted" style={{ padding: '8px 12px', fontSize: 11 }}>
            No upload records found for this session.
            Records are written for each POST /ingest/csv starting from API v0.9.0.
          </div>
        ) : (
          <div style={{ overflowX: 'auto' }}>
            <table className="uploads-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th title="Rows inserted (new) / duplicate / rejected">Rows</th>
                  <th>Size</th>
                  <th>IP</th>
                  <th>User</th>
                  <th>Firmware</th>
                  <th title="Processing time in milliseconds">ms</th>
                </tr>
              </thead>
              <tbody>
                {uploads.map((u, i) => (
                  <tr key={i}
                    className={`upload-row${u.rowsInserted === 0 && (u.rowsDuplicate ?? 0) > 0 ? ' upload-row-dup' : ''}`}
                    title={`seen=${u.rowsSeen ?? '?'} accepted=${u.rowsAccepted ?? '?'} rejected=${u.rowsRejected ?? 0} inserted=${u.rowsInserted ?? '?'} duplicate=${u.rowsDuplicate ?? 0}`}>
                    <td className="upload-cell-time">{fmtTime(u.receivedAt)}</td>
                    <td className="upload-cell-rows">
                      {u.rowsInserted ?? '?'}↑
                      {(u.rowsDuplicate ?? 0) > 0 && <span className="upload-dup"> {u.rowsDuplicate}≡</span>}
                      {(u.rowsRejected ?? 0) > 0 && <span className="upload-rej"> {u.rowsRejected}✕</span>}
                    </td>
                    <td>{fmtKb(u.payloadBytes)}</td>
                    <td className="upload-cell-mono">{u.clientIp || '—'}</td>
                    <td className="upload-cell-mono">{u.username || '—'}</td>
                    <td className="upload-cell-mono">{u.firmware || '—'}</td>
                    <td>{u.durationMs ?? '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )
      )}
    </div>
  );
}

// ---- Main ManagePanel component ------------------------------------------
export function ManagePanel({ sessions, onRenamed, onDeleted, onMerged, onRestored, onPurged, onError }) {
  const [subTab, setSubTab] = useState('rename'); // rename | delete | merge | export | uploads

  return (
    <div className="panel-scroll">
      <div className="mgmt-subtabs">
        {[
          { key: 'rename',  label: '✏ Rename' },
          { key: 'delete',  label: '🗑 Delete / Restore' },
          { key: 'merge',   label: '⊕ Merge' },
          { key: 'export',  label: '↓ Export' },
          { key: 'uploads', label: '↑ Uploads' },
        ].map(t => (
          <button key={t.key}
            className={`tab ${subTab === t.key ? 'active' : ''}`}
            style={{ flex: 1 }}
            onClick={() => setSubTab(t.key)}>
            {t.label}
          </button>
        ))}
      </div>

      {subTab === 'rename' && (
        <>
          <div className="section-head">Click Rename to set a display name</div>
          {sessions.map(s => (
            <RenameRow key={s.sessionId} session={s}
              onRenamed={onRenamed} onError={onError} />
          ))}
        </>
      )}

      {subTab === 'delete' && (
        <DeleteRestoreTab
          sessions={sessions}
          onSoftDeleted={onDeleted}
          onRestored={onRestored}
          onPurged={onPurged}
          onError={onError}
        />
      )}

      {subTab === 'merge' && (
        <MergePanel sessions={sessions} onMerged={onMerged} onError={onError} />
      )}

      {subTab === 'export' && (
        <ExportPanel sessions={sessions} onError={onError} />
      )}

      {subTab === 'uploads' && (
        <UploadsTab sessions={sessions} />
      )}
    </div>
  );
}
