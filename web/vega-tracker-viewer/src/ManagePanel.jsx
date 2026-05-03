// ManagePanel — data management UI: rename, delete (triple-confirm), merge, export.
import { useState, useMemo } from 'react';
import { renameSession, deleteSession, mergeSessions, exportSession, exportBulk } from './api.js';
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

// ---- Delete row (triple-confirm) -----------------------------------------
function DeleteRow({ session, onDeleted, onError }) {
  // step 0=idle 1=first-confirm 2=second-confirm 3=deleting
  const [step, setStep] = useState(0);

  async function handleStep() {
    if (step < 2) { setStep(s => s + 1); return; }
    setStep(3);
    try {
      await deleteSession(session.sessionId);
      onDeleted(session.sessionId);
    } catch (e) {
      onError(String(e));
      setStep(0);
    }
  }

  const labels = [
    'Delete',
    'Confirm delete?',
    '⚠ PERMANENTLY DELETE — FINAL CONFIRM',
  ];
  const classes = ['btn-sm btn-danger-outline', 'btn-sm btn-danger', 'btn-sm btn-danger-bright'];

  return (
    <div className="mgmt-row">
      <span className="swatch" style={{ background: sessionColor(session._idx ?? 0) }} />
      <span className="mgmt-name" title={session.sessionId}>
        {session.displayName || session.sessionId}
      </span>
      {step === 0 && (
        <button className="btn-sm btn-danger-outline" onClick={() => setStep(1)}>Delete</button>
      )}
      {step === 1 && (
        <>
          <span className="warn-text">Confirm?</span>
          <button className="btn-sm btn-danger" onClick={() => setStep(2)}>Yes, delete</button>
          <button className="btn-sm" onClick={() => setStep(0)}>Cancel</button>
        </>
      )}
      {step === 2 && (
        <>
          <span className="warn-text">PERMANENT — all samples lost</span>
          <button className="btn-sm btn-danger-bright" onClick={handleStep}>PERMANENTLY DELETE</button>
          <button className="btn-sm" onClick={() => setStep(0)}>Cancel</button>
        </>
      )}
      {step === 3 && <span className="muted">Deleting...</span>}
    </div>
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

// ---- Main ManagePanel component ------------------------------------------
export function ManagePanel({ sessions, onRenamed, onDeleted, onMerged, onError }) {
  const [subTab, setSubTab] = useState('rename'); // rename | delete | merge | export

  return (
    <div className="panel-scroll">
      <div className="mgmt-subtabs">
        {[
          { key: 'rename',  label: '✏ Rename' },
          { key: 'delete',  label: '🗑 Delete' },
          { key: 'merge',   label: '⊕ Merge' },
          { key: 'export',  label: '↓ Export' },
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
        <>
          <div className="section-head">Triple confirmation required</div>
          {sessions.map(s => (
            <DeleteRow key={s.sessionId} session={s}
              onDeleted={onDeleted} onError={onError} />
          ))}
        </>
      )}

      {subTab === 'merge' && (
        <MergePanel sessions={sessions} onMerged={onMerged} onError={onError} />
      )}

      {subTab === 'export' && (
        <ExportPanel sessions={sessions} onError={onError} />
      )}
    </div>
  );
}
