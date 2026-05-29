/**
 * ExplorerPanel.jsx
 *
 * The "Explorer" mode sidebar — two sub-tabs:
 *   Analysis  — run coverage gap analysis, view ranked zones, commit a mission
 *   Missions  — manage saved missions, launch live tracking
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  analyzeCoverage, createMission, deleteMission,
  fetchLatestAnalysis, fetchMissions, fetchZoneCoverage, updateMission,
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

function timeAgo(msEpoch) {
  if (!msEpoch) return '';
  const diffSec = Math.floor((Date.now() - msEpoch) / 1000);
  if (diffSec < 60)  return `${diffSec}s ago`;
  if (diffSec < 3600) return `${Math.floor(diffSec / 60)}m ago`;
  if (diffSec < 86400) return `${Math.floor(diffSec / 3600)}h ago`;
  return `${Math.floor(diffSec / 86400)}d ago`;
}

function AnalysisTab({ onZoneSelect, selectedZone, analysisResult, setAnalysisResult,
                       onZoneCoverageUpdate }) {
  const [params, setParams] = useState({
    maxSpeedKph:   50,
    maxHdop:       3.0,
    maxAccuracyM:  15.0,
    gridDeg:       0.002,   // coarse grid for full-DB zone discovery (fine = slow timeout)
    topN:          15,
    paddingFactor: 0.15,
    maxZoneSqMi:   25,
    minZoneSqMi:   0.3,
    distPeakKm:    5,
  });
  const [analyzing, setAnalyzing]   = useState(false);
  const [error, setError]           = useState(null);
  const [showParams, setShowParams] = useState(false);
  const [lastRunAt, setLastRunAt]   = useState(null); // ms epoch
  const [loading, setLoading]       = useState(true);

  // zone-coverage detail for selected zone
  const [zoneCoverage, setZoneCoverage]   = useState(null);
  const [coverFetching, setCoverFetching] = useState(false);
  const zoneCovAbort = useRef(null);

  // commit-to-mission state
  const [commitZone, setCommitZone]   = useState(null);
  const [commitName, setCommitName]   = useState('');
  const [commitNotes, setCommitNotes] = useState('');
  const [committing, setCommitting]   = useState(false);
  const [commitOk, setCommitOk]       = useState(false);

  // Load the most-recent stored analysis on mount
  useEffect(() => {
    fetchLatestAnalysis()
      .then(data => {
        if (data) {
          setAnalysisResult(data);
          setLastRunAt(data.metadata?.runAt ?? null);
          if (data.metadata?.params) {
            const p = data.metadata.params;
            setParams(prev => ({...prev,
              maxSpeedKph:  p.maxSpeedKph  ?? prev.maxSpeedKph,
              maxHdop:      p.maxHdop      ?? prev.maxHdop,
              maxAccuracyM: p.maxAccuracyM ?? prev.maxAccuracyM,
              gridDeg:      p.gridDeg      ?? prev.gridDeg,
              topN:         p.topN         ?? prev.topN,
              maxZoneSqMi:  p.maxZoneSqMi  ?? prev.maxZoneSqMi,
              minZoneSqMi:  p.minZoneSqMi  ?? prev.minZoneSqMi,
              distPeakKm:   p.distPeakKm   ?? prev.distPeakKm,
            }));
          }
        }
      })
      .catch(() => {}) // 404 = no prior analysis, fine
      .finally(() => setLoading(false));
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  // Fetch zone coverage detail whenever selected zone changes
  useEffect(() => {
    if (!selectedZone) { setZoneCoverage(null); onZoneCoverageUpdate?.(null); return; }
    const bbox = selectedZone.properties?.bbox;
    if (!bbox) return; // [min_lng, min_lat, max_lng, max_lat]
    const [minLng, minLat, maxLng, maxLat] = bbox;

    // Cancel any previous in-flight fetch
    if (zoneCovAbort.current) zoneCovAbort.current.abort();
    const ctl = new AbortController();
    zoneCovAbort.current = ctl;

    setCoverFetching(true);
    setZoneCoverage(null);
    fetchZoneCoverage({
      minLat, maxLat, minLng, maxLng,
      gridDeg:      0.0005,  // always fine for per-zone dot detail (~50m cells)
      maxSpeedKph:  params.maxSpeedKph,
      maxHdop:      params.maxHdop,
      maxAccuracyM: params.maxAccuracyM,
    })
      .then(data => {
        if (ctl.signal.aborted) return;
        setZoneCoverage(data);
        onZoneCoverageUpdate?.(data);
      })
      .catch(() => {})
      .finally(() => { if (!ctl.signal.aborted) setCoverFetching(false); });
  }, [selectedZone]); // eslint-disable-line react-hooks/exhaustive-deps

  const runAnalysis = useCallback(async () => {
    setAnalyzing(true);
    setError(null);
    setAnalysisResult(null);
    setCommitZone(null);
    setZoneCoverage(null);
    onZoneCoverageUpdate?.(null);
    try {
      const result = await analyzeCoverage(params);
      setAnalysisResult(result);
      setLastRunAt(result.metadata?.runAt ?? Date.now());
    } catch (e) {
      setError(e.message);
    } finally {
      setAnalyzing(false);
    }
  }, [params, setAnalysisResult, onZoneCoverageUpdate]);

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

      {/* Last-run recall banner */}
      {!loading && lastRunAt && !analyzing && (
        <div className="explorer-recall-banner">
          <span>📍 Analysis from {timeAgo(lastRunAt)}</span>
          <button className="explorer-recall-refresh" onClick={runAnalysis}
            title="Re-run and replace with fresh result">↻ Refresh</button>
        </div>
      )}

      {/* Params toggle */}
      <button
        className="explorer-section-toggle"
        onClick={() => setShowParams(v => !v)}>
        ⚙ Parameters {showParams ? '▲' : '▼'}
      </button>

      {showParams && (
        <div className="explorer-params">
          <div className="explorer-param-section-label">Quality filters</div>
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

          <div className="explorer-param-section-label">Grid &amp; zone size</div>
          <label className="explorer-param-row">
            <span>Grid cell size</span>
            <select value={params.gridDeg}
              onChange={e => setParams(p => ({ ...p, gridDeg: +e.target.value }))}>
              <option value={0.001}>Fine — 0.001° (~110 m)</option>
              <option value={0.002}>Normal — 0.002° (~220 m)</option>
              <option value={0.005}>Coarse — 0.005° (~550 m)</option>
              <option value={0.01}>Very coarse — 0.01° (~1.1 km)</option>
            </select>
          </label>
          <label className="explorer-param-row">
            <span>Max zone size (sq mi)</span>
            <input type="number" min={1} max={500} step={1}
              value={params.maxZoneSqMi}
              onChange={e => setParams(p => ({ ...p, maxZoneSqMi: +e.target.value }))} />
          </label>
          <label className="explorer-param-row">
            <span>Min zone size (sq mi)</span>
            <input type="number" min={0.1} max={10} step={0.1}
              value={params.minZoneSqMi}
              onChange={e => setParams(p => ({ ...p, minZoneSqMi: +e.target.value }))} />
          </label>

          <div className="explorer-param-section-label">Ranking</div>
          <label className="explorer-param-row">
            <span>Top zones shown</span>
            <input type="number" min={5} max={30} step={1}
              value={params.topN}
              onChange={e => setParams(p => ({ ...p, topN: +e.target.value }))} />
          </label>
          <label className="explorer-param-row">
            <span>Preferred distance (km)</span>
            <input type="number" min={1} max={100} step={1}
              value={params.distPeakKm}
              title="Zones closest to this distance from your data center score highest"
              onChange={e => setParams(p => ({ ...p, distPeakKm: +e.target.value }))} />
          </label>
          <div className="explorer-param-hint">
            Zones near your preferred distance from your existing tracks score highest.
            Increase this to find zones farther from home.
          </div>
        </div>
      )}

      <button
        className="explorer-run-btn"
        disabled={analyzing}
        onClick={runAnalysis}>
        {analyzing ? '⏳ Analysing…'
          : analysisResult ? '↻ Run New Analysis'
          : '▶ Run Coverage Analysis'}
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
            const sqMi = p.areaSqMi ?? (p.areaKm2 * 0.386102);
            return (
              <div key={p.rank}>
                <div
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
                      <span>{sqMi.toFixed(1)} sq mi</span>
                      <span>·</span>
                      <span>{p.areaKm2.toFixed(2)} km²</span>
                      <span>·</span>
                      <span>{p.distFromCenterKm.toFixed(1)} km away</span>
                    </div>
                    <div className="explorer-zone-coords">
                      {p.centroid[1].toFixed(4)}°, {p.centroid[0].toFixed(4)}°
                    </div>
                  </div>
                </div>

                {/* Zone detail inline — only shown for selected zone */}
                {isSelected && (
                  <div className="explorer-zone-detail">
                    {coverFetching ? (
                      <div className="explorer-zone-detail-loading">
                        <div className="explorer-spinner-sm" /> Fetching coverage…
                      </div>
                    ) : zoneCoverage ? (
                      <>
                        <div className="explorer-zone-detail-bar">
                          <div className="explorer-zone-detail-bar-fill"
                            style={{ width: `${zoneCoverage.coveragePct}%` }} />
                        </div>
                        <div className="explorer-zone-detail-stats">
                          <span className="covered"
                            title="Grid cells you have already visited">
                            ✅ {zoneCoverage.coveredCount} covered
                          </span>
                          <span className="uncovered"
                            title="Empty grid cells — go here!">
                            🟧 {zoneCoverage.uncoveredCount} to visit
                          </span>
                          <span className="pct">{zoneCoverage.coveragePct}% done</span>
                        </div>
                        <div className="explorer-zone-detail-hint">
                          {zoneCoverage.coveragePct < 10
                            ? '🗺 Mostly uncharted — any road in this zone adds data.'
                            : zoneCoverage.coveragePct < 40
                            ? '🔍 Partially explored — find the orange patches on the map.'
                            : zoneCoverage.coveragePct < 75
                            ? '📍 Getting there — fill in the gaps between your existing tracks.'
                            : '🏁 Well covered — look for the remaining orange cells.'}
                        </div>
                        <div className="explorer-zone-detail-hint muted">
                          Orange squares on the map = unvisited cells.
                          Green = already covered.
                        </div>
                      </>
                    ) : null}
                  </div>
                )}
              </div>
            );
          })}
        </div>
      )}

      {commitZone && (
        <div className="explorer-commit-panel">
          <div className="explorer-commit-title">
            Save Zone {commitZone.properties.rank} as Mission
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

export function ExplorerPanel({
  onZoneSelect, selectedZone, onAnalysisResult, onGoLive, onZoneCoverageUpdate,
}) {
  const [subTab, setSubTab]                 = useState('analysis');
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
          onZoneCoverageUpdate={onZoneCoverageUpdate}
        />
      )}
      {subTab === 'missions' && (
        <MissionsTab onGoLive={onGoLive} />
      )}
    </div>
  );
}
