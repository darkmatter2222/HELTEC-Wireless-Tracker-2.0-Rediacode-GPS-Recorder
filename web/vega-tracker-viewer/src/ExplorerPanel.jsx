/**
 * ExplorerPanel.jsx
 *
 * The "Explorer" mode sidebar — two sub-tabs:
 *   Analysis  — run coverage gap analysis, view ranked zones, commit a mission
 *   Missions  — manage saved missions, launch live tracking
 */
import React, { useCallback, useEffect, useState } from 'react';
import {
  analyzeCoverage, createMission, deleteMission,
  fetchMissions, updateMission,
} from './api.js';

const STATUS_COLORS = {
  planning:  '#29b6f6',
  active:    '#00e676',
  complete:  '#aaa',
  abandoned: '#ef5350',
};

const STATUS_LABELS = {
  planning:  'Planning',
  active:    'Active',
  complete:  'Complete',
  abandoned: 'Abandoned',
};

function StatusBadge({ status }) {
  return (
    <span style={{
      fontSize: 10, fontWeight: 700, textTransform: 'uppercase',
      letterSpacing: 1, padding: '2px 6px', borderRadius: 3,
      background: (STATUS_COLORS[status] || '#666') + '22',
      color: STATUS_COLORS[status] || '#aaa',
      border: `1px solid ${STATUS_COLORS[status] || '#666'}44`,
    }}>
      {STATUS_LABELS[status] || status}
    </span>
  );
}

// ---------- Analysis tab ---------------------------------------------------

function AnalysisTab({ onZoneSelect, selectedZone, analysisResult, setAnalysisResult }) {
  const [params, setParams] = useState({
    maxSpeedKph:   50,
    maxHdop:       3.0,
    maxAccuracyM:  15.0,
    gridDeg:       0.002,
    topN:          15,
    paddingFactor: 0.15,
  });
  const [analyzing, setAnalyzing]   = useState(false);
  const [error, setError]           = useState(null);
  const [showParams, setShowParams] = useState(false);

  // commit-to-mission state
  const [commitZone, setCommitZone]   = useState(null);
  const [commitName, setCommitName]   = useState('');
  const [commitNotes, setCommitNotes] = useState('');
  const [committing, setCommitting]   = useState(false);
  const [commitOk, setCommitOk]       = useState(false);

  const runAnalysis = useCallback(async () => {
    setAnalyzing(true);
    setError(null);
    setAnalysisResult(null);
    setCommitZone(null);
    try {
      const result = await analyzeCoverage(params);
      setAnalysisResult(result);
    } catch (e) {
      setError(e.message);
    } finally {
      setAnalyzing(false);
    }
  }, [params, setAnalysisResult]);

  const handleCommit = useCallback(async () => {
    if (!commitZone || !commitName.trim()) return;
    setCommitting(true);
    try {
      await createMission({
        name:     commitName.trim(),
        polygon:  commitZone.geometry,
        centroid: commitZone.properties.centroid,
        areaKm2:  commitZone.properties.areaKm2,
        score:    commitZone.properties.score,
        notes:    commitNotes,
      });
      setCommitOk(true);
      setCommitZone(null);
      setCommitName('');
      setCommitNotes('');
      setTimeout(() => setCommitOk(false), 3000);
    } catch (e) {
      setError(e.message);
    } finally {
      setCommitting(false);
    }
  }, [commitZone, commitName, commitNotes]);

  const meta = analysisResult?.metadata;

  return (
    <div className="explorer-tab-content">
      {/* Params toggle */}
      <button
        className="explorer-section-toggle"
        onClick={() => setShowParams(v => !v)}>
        ⚙ Parameters {showParams ? '▲' : '▼'}
      </button>

      {showParams && (
        <div className="explorer-params">
          <label className="explorer-param-row">
            <span>Max speed (km/h)</span>
            <input type="number" min={5} max={200} step={5}
              value={params.maxSpeedKph}
              onChange={e => setParams(p => ({ ...p, maxSpeedKph: +e.target.value }))} />
          </label>
          <label className="explorer-param-row">
            <span>Max HDOP</span>
            <input type="number" min={0.5} max={10} step={0.5}
              value={params.maxHdop}
              onChange={e => setParams(p => ({ ...p, maxHdop: +e.target.value }))} />
          </label>
          <label className="explorer-param-row">
            <span>Max accuracy (m)</span>
            <input type="number" min={5} max={100} step={5}
              value={params.maxAccuracyM}
              onChange={e => setParams(p => ({ ...p, maxAccuracyM: +e.target.value }))} />
          </label>
          <label className="explorer-param-row">
            <span>Grid size (°)</span>
            <select value={params.gridDeg}
              onChange={e => setParams(p => ({ ...p, gridDeg: +e.target.value }))}>
              <option value={0.001}>0.001° (~110m)</option>
              <option value={0.002}>0.002° (~220m)</option>
              <option value={0.005}>0.005° (~550m)</option>
              <option value={0.01}>0.010° (~1.1km)</option>
            </select>
          </label>
          <label className="explorer-param-row">
            <span>Top zones</span>
            <input type="number" min={5} max={30} step={1}
              value={params.topN}
              onChange={e => setParams(p => ({ ...p, topN: +e.target.value }))} />
          </label>
        </div>
      )}

      <button
        className="explorer-run-btn"
        disabled={analyzing}
        onClick={runAnalysis}>
        {analyzing ? '⏳ Analysing…' : '▶ Run Coverage Analysis'}
      </button>

      {error && <div className="error-banner px16">{error}</div>}

      {analyzing && (
        <div className="explorer-loading">
          <div className="explorer-spinner" />
          <span>Querying millions of samples…<br />This may take 10–30 s</span>
        </div>
      )}

      {meta && (
        <div className="explorer-meta">
          <span>{meta.coveredCells?.toLocaleString()} covered cells</span>
          <span>·</span>
          <span>{meta.gapRegions} gap zones</span>
          <span>·</span>
          <span>{meta.totalSec?.toFixed(1)}s</span>
        </div>
      )}

      {analysisResult?.features?.length > 0 && (
        <div className="explorer-zone-list">
          {analysisResult.features.map(feature => {
            const p = feature.properties;
            const isSelected = selectedZone?.properties?.rank === p.rank;
            return (
              <div
                key={p.rank}
                className={`explorer-zone-card ${isSelected ? 'selected' : ''}`}
                onClick={() => {
                  onZoneSelect(isSelected ? null : feature);
                  if (!isSelected) setCommitZone(feature);
                }}>
                <div className="explorer-zone-rank">#{p.rank}</div>
                <div className="explorer-zone-info">
                  <div className="explorer-zone-title">
                    Zone {p.rank}
                    <span className="explorer-zone-score">score {p.score.toFixed(2)}</span>
                  </div>
                  <div className="explorer-zone-stats">
                    <span>{p.areaKm2.toFixed(2)} km²</span>
                    <span>·</span>
                    <span>{p.distFromCenterKm.toFixed(1)} km away</span>
                    <span>·</span>
                    <span>{p.cellCount} cells</span>
                  </div>
                  <div className="explorer-zone-coords">
                    {p.centroid[1].toFixed(4)}°, {p.centroid[0].toFixed(4)}°
                  </div>
                </div>
              </div>
            );
          })}
        </div>
      )}

      {commitZone && (
        <div className="explorer-commit-panel">
          <div className="explorer-commit-title">
            Commit Zone {commitZone.properties.rank} as Mission
          </div>
          <input
            className="explorer-commit-input"
            placeholder="Mission name…"
            value={commitName}
            onChange={e => setCommitName(e.target.value)} />
          <textarea
            className="explorer-commit-notes"
            placeholder="Notes (optional)"
            rows={2}
            value={commitNotes}
            onChange={e => setCommitNotes(e.target.value)} />
          <div className="explorer-commit-actions">
            <button onClick={() => setCommitZone(null)}>Cancel</button>
            <button
              className="explorer-commit-go"
              disabled={!commitName.trim() || committing}
              onClick={handleCommit}>
              {committing ? 'Saving…' : '✔ Save Mission'}
            </button>
          </div>
        </div>
      )}

      {commitOk && (
        <div className="explorer-commit-success">✔ Mission saved — go to Missions tab to activate</div>
      )}
    </div>
  );
}

// ---------- Missions tab ---------------------------------------------------

function MissionsTab({ onGoLive }) {
  const [missions, setMissions]         = useState([]);
  const [loading, setLoading]           = useState(false);
  const [error, setError]               = useState(null);
  const [statusFilter, setStatusFilter] = useState('all');
  const [editingId, setEditingId]       = useState(null);
  const [editName, setEditName]         = useState('');

  const reload = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const data = await fetchMissions(statusFilter === 'all' ? null : statusFilter);
      setMissions(data);
    } catch (e) {
      setError(e.message);
    } finally {
      setLoading(false);
    }
  }, [statusFilter]);

  useEffect(() => { reload(); }, [reload]);

  const handleStatus = useCallback(async (missionId, status) => {
    try {
      await updateMission(missionId, { status });
      reload();
    } catch (e) {
      setError(e.message);
    }
  }, [reload]);

  const handleRename = useCallback(async (missionId) => {
    if (!editName.trim()) return;
    try {
      await updateMission(missionId, { name: editName.trim() });
      setEditingId(null);
      reload();
    } catch (e) {
      setError(e.message);
    }
  }, [editName, reload]);

  const handleDelete = useCallback(async (missionId) => {
    if (!window.confirm('Delete this mission? This cannot be undone.')) return;
    try {
      await deleteMission(missionId);
      reload();
    } catch (e) {
      setError(e.message);
    }
  }, [reload]);

  const filtered = statusFilter === 'all'
    ? missions
    : missions.filter(m => m.status === statusFilter);

  return (
    <div className="explorer-tab-content">
      {/* Status filter pills */}
      <div className="explorer-status-filters">
        {['all', 'planning', 'active', 'complete', 'abandoned'].map(s => (
          <button
            key={s}
            className={`explorer-status-pill ${statusFilter === s ? 'active' : ''}`}
            onClick={() => setStatusFilter(s)}>
            {s === 'all' ? 'All' : STATUS_LABELS[s]}
          </button>
        ))}
      </div>

      {error && <div className="error-banner px16">{error}</div>}
      {loading && <div className="muted px16">Loading missions…</div>}

      {!loading && filtered.length === 0 && (
        <div className="muted px16" style={{ marginTop: 20, textAlign: 'center' }}>
          No missions yet.<br />
          Run the Analysis tab to find exploration zones,<br />
          then commit one as a mission.
        </div>
      )}

      <div className="explorer-mission-list">
        {filtered.map(m => {
          const created = new Date(m.createdAt).toLocaleDateString();
          const isEditing = editingId === m.missionId;
          return (
            <div key={m.missionId} className="explorer-mission-card">
              <div className="explorer-mission-header">
                {isEditing ? (
                  <div className="explorer-mission-rename">
                    <input
                      value={editName}
                      onChange={e => setEditName(e.target.value)}
                      onKeyDown={e => {
                        if (e.key === 'Enter') handleRename(m.missionId);
                        if (e.key === 'Escape') setEditingId(null);
                      }}
                      autoFocus />
                    <button onClick={() => handleRename(m.missionId)}>✔</button>
                    <button onClick={() => setEditingId(null)}>✕</button>
                  </div>
                ) : (
                  <span
                    className="explorer-mission-name"
                    onDoubleClick={() => { setEditingId(m.missionId); setEditName(m.name); }}>
                    {m.name}
                  </span>
                )}
                <StatusBadge status={m.status} />
              </div>

              <div className="explorer-mission-meta">
                {m.areaKm2 > 0 && <span>{m.areaKm2.toFixed(2)} km²</span>}
                {m.areaKm2 > 0 && <span>·</span>}
                <span>Created {created}</span>
                {m.notes && <><span>·</span><span className="explorer-mission-notes">{m.notes}</span></>}
              </div>

              <div className="explorer-mission-actions">
                {m.status === 'planning' && (
                  <button
                    className="explorer-btn-activate"
                    onClick={() => handleStatus(m.missionId, 'active')}>
                    Activate
                  </button>
                )}
                {m.status === 'active' && (
                  <>
                    <button
                      className="explorer-btn-golive"
                      onClick={() => onGoLive(m)}>
                      🔴 Go Live
                    </button>
                    <button onClick={() => handleStatus(m.missionId, 'complete')}>
                      Mark Complete
                    </button>
                    <button onClick={() => handleStatus(m.missionId, 'abandoned')}>
                      Abandon
                    </button>
                  </>
                )}
                {(m.status === 'complete' || m.status === 'abandoned') && (
                  <>
                    <button onClick={() => handleStatus(m.missionId, 'active')}>
                      Reactivate
                    </button>
                  </>
                )}
                <button
                  className="explorer-btn-delete"
                  onClick={() => handleDelete(m.missionId)}>
                  Delete
                </button>
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}

// ---------- Public component -----------------------------------------------

export function ExplorerPanel({ onZoneSelect, selectedZone, onAnalysisResult, onGoLive }) {
  const [subTab, setSubTab]               = useState('analysis');
  const [analysisResult, setAnalysisResult] = useState(null);

  // Forward result to parent (App.jsx) so the map can show gap polygons
  function handleAnalysisResult(result) {
    setAnalysisResult(result);
    if (onAnalysisResult) onAnalysisResult(result);
  }

  return (
    <div className="explorer-panel">
      {/* Sub-tab bar */}
      <div className="tab-bar">
        {[['analysis', 'Analysis'], ['missions', 'Missions']].map(([t, label]) => (
          <button
            key={t}
            className={`tab ${subTab === t ? 'active' : ''}`}
            onClick={() => setSubTab(t)}>
            {label}
          </button>
        ))}
      </div>

      {subTab === 'analysis' && (
        <AnalysisTab
          onZoneSelect={onZoneSelect}
          selectedZone={selectedZone}
          analysisResult={analysisResult}
          setAnalysisResult={handleAnalysisResult}
        />
      )}
      {subTab === 'missions' && (
        <MissionsTab onGoLive={onGoLive} />
      )}
    </div>
  );
}
