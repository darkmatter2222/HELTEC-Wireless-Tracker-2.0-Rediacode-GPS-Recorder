import React, { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import {
  MapContainer, TileLayer, Polyline, CircleMarker,
  Tooltip, useMap, Marker,
} from 'react-leaflet';
import L from 'leaflet';
import { fetchSessions, fetchSessionRows } from './api.js';
import {
  doseColor, cpsColor, speedColor, altColor, hdopColor,
  sessionColor, fmtTs, fmtDose,
} from './colors.js';
import { SparkChart } from './SparkChart.jsx';
import { ManagePanel } from './ManagePanel.jsx';
import { TimelineView } from './TimelineView.jsx';
import { DatabasePanel } from './DatabasePanel.jsx';

// ---- constants -------------------------------------------------------------

const MIN_VALID_TS_MS = 1577836800000; // 2020-01-01 UTC

// Map display modes
const MAP_MODES = ['Track', 'Dots', 'Heatmap', 'Arrows'];

// Color channel options
const COLOR_CHANNELS = [
  { key: 'dose',  label: 'Dose rate' },
  { key: 'cps',   label: 'CPS' },
  { key: 'speed', label: 'Speed' },
  { key: 'alt',   label: 'Altitude' },
  { key: 'hdop',  label: 'HDOP' },
  { key: 'session', label: 'Session' },
];

// Tile layers
const TILES = [
  {
    name: 'OSM Streets',
    url: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    attribution: '&copy; OpenStreetMap contributors',
  },
  {
    name: 'CartoDB Dark',
    url: 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',
    attribution: '&copy; OpenStreetMap contributors, &copy; CartoDB',
  },
  {
    name: 'OpenTopoMap',
    url: 'https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png',
    attribution: '&copy; OpenStreetMap contributors, &copy; OpenTopoMap',
  },
  {
    name: 'Satellite (Esri)',
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
    attribution: '&copy; Esri World Imagery',
  },
];

// ---- helpers ---------------------------------------------------------------

function bboxFromPoints(points) {
  if (!points.length) return null;
  let minLat = points[0].lat, maxLat = points[0].lat;
  let minLng = points[0].lng, maxLng = points[0].lng;
  for (const p of points) {
    if (p.lat < minLat) minLat = p.lat;
    if (p.lat > maxLat) maxLat = p.lat;
    if (p.lng < minLng) minLng = p.lng;
    if (p.lng > maxLng) maxLng = p.lng;
  }
  return L.latLngBounds([minLat, minLng], [maxLat, maxLng]);
}

function FitBoundsOnce({ bounds, dep }) {
  const map = useMap();
  useEffect(() => {
    if (bounds && bounds.isValid()) {
      map.fitBounds(bounds.pad(0.1), { animate: true });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [dep]);
  return null;
}

function boundsKey(b) {
  if (!b || !b.isValid()) return '';
  const sw = b.getSouthWest();
  const ne = b.getNorthEast();
  return `${sw.lat.toFixed(4)},${sw.lng.toFixed(4)},${ne.lat.toFixed(4)},${ne.lng.toFixed(4)}`;
}

// Leaflet heatmap layer component. Uses leaflet.heat plugin if available;
// falls back to dense CircleMarkers if the plugin didn't load.
function HeatmapLayer({ points, field, doseMin, doseMax }) {
  const map = useMap();
  const layerRef = useRef(null);

  useEffect(() => {
    if (!points || points.length === 0) return;

    // Build intensity array: [lat, lng, intensity 0-1]
    const vals = points.map(p => {
      if (field === 'cps') return p.cps ?? 0;
      if (field === 'speed') return p.spd ?? 0;
      return p.uSv ?? 0;
    });
    const maxVal = Math.max(...vals, 1e-6);

    const heat = points.map((p, i) => [p.lat, p.lng, vals[i] / maxVal]);

    if (L.heatLayer) {
      if (layerRef.current) map.removeLayer(layerRef.current);
      layerRef.current = L.heatLayer(heat, {
        radius: 20,
        blur: 18,
        maxZoom: 17,
        gradient: { 0.0: '#00e676', 0.4: '#ffea00', 0.75: '#ff6d00', 1.0: '#d50000' },
      });
      layerRef.current.addTo(map);
    }
    return () => {
      if (layerRef.current) { map.removeLayer(layerRef.current); layerRef.current = null; }
    };
  }, [points, field, doseMin, doseMax, map]);

  return null;
}

// Returns an SVG arrow icon for bearing display.
function arrowIcon(bearing, color, size = 20) {
  const r = bearing ?? 0;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 20 20">
    <g transform="rotate(${r} 10 10)">
      <polygon points="10,2 14,16 10,13 6,16" fill="${color}" stroke="rgba(0,0,0,0.4)" stroke-width="0.8"/>
    </g>
  </svg>`;
  return L.divIcon({
    html: svg,
    className: '',
    iconSize: [size, size],
    iconAnchor: [size / 2, size / 2],
  });
}

function getPointColor(p, channel, idx, doseMin, doseMax) {
  switch (channel) {
    case 'cps':     return cpsColor(p.cps, 0, 50);
    case 'speed':   return speedColor(p.spd, 0, 80);
    case 'alt':     return altColor(p.alt, 0, 500);
    case 'hdop':    return hdopColor(p.hdop, 0, 5);
    case 'session': return sessionColor(idx);
    default:        return doseColor(p.uSv, doseMin, doseMax);
  }
}

function compactRows(raw) {
  return raw
    .filter(r => r.timestampMs != null && r.timestampMs >= MIN_VALID_TS_MS)
    .map(r => ({
      ts:   r.timestampMs,
      lat:  r.latitude,
      lng:  r.longitude,
      uSv:  r.uSvPerHour,
      cps:  r.cps,
      spd:  r.speedKph   ?? null,
      brg:  r.bearingDeg ?? null,
      alt:  r.altitudeM  ?? null,
      hdop: r.hdop       ?? null,
    }));
}

function fmtBytes(bytes) {
  if (bytes == null || isNaN(bytes)) return null;
  if (bytes < 1024)       return `${bytes} B`;
  if (bytes < 1048576)    return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1048576).toFixed(2)} MB`;
}

function fmtDuration(ms) {
  if (!ms || ms < 0) return '';
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m`;
  return `${(s / 3600).toFixed(1)}h`;
}

function fmtSpeed(kph) {
  if (kph == null) return '-';
  return `${kph.toFixed(1)} km/h`;
}

function fmtBearing(deg) {
  if (deg == null) return '-';
  const dirs = ['N','NE','E','SE','S','SW','W','NW'];
  return `${dirs[Math.round(deg / 45) % 8]} ${deg.toFixed(0)}°`;
}

// ---- main app --------------------------------------------------------------

export default function App() {
  const [sessions, setSessions]   = useState([]);
  const [loading, setLoading]     = useState(true);
  const [error, setError]         = useState(null);
  const [rowsBySession, setRows]  = useState({});
  const [selected, setSelected]   = useState(new Set());
  const [doseMin, setDoseMin]     = useState(0);
  const [doseMax, setDoseMax]     = useState(1.0);
  const [doseScaleManual, setDoseScaleManual] = useState(false);

  // Map / display mode
  const [mapMode, setMapMode]         = useState('Track');  // Track | Dots | Heatmap | Arrows
  const [colorChannel, setColorChannel] = useState('dose'); // dose | cps | speed | alt | hdop | session
  const [nanoMode, setNanoMode]         = useState(false);
  const [tileIdx, setTileIdx]           = useState(1);       // default CartoDB Dark
  const [trackWeight, setTrackWeight]   = useState(4);
  const [pointRadius, setPointRadius]   = useState(5);
  const [showTooltips, setShowTooltips] = useState(true);
  const [arrowEvery, setArrowEvery]     = useState(5);      // show 1-in-N arrows

  // Timeline
  const [timeFrac, setTimeFrac]     = useState(1.0);
  const [windowFrac, setWindowFrac] = useState(1.0);
  const [playing, setPlaying]       = useState(false);
  const [fitTrigger, setFitTrigger] = useState(0);
  const playRef = useRef();

  // Sidebar panel
  const [panel, setPanel] = useState('sessions'); // sessions | display | stats | manage | db
  const [searchFilter, setSearchFilter] = useState('');

  // Resizable sidebar
  const [sidebarWidth, setSidebarWidth] = useState(320);
  const sidebarRef = useRef(null);

  function startResize(e) {
    e.preventDefault();
    const startX = e.clientX;
    const startW = sidebarRef.current ? sidebarRef.current.offsetWidth : sidebarWidth;
    function onMove(ev) {
      const w = Math.max(220, Math.min(600, startW + ev.clientX - startX));
      setSidebarWidth(w);
    }
    function onUp() {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
    }
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  }
  const [showTimeline, setShowTimeline] = useState(false);

  // ---- load session list once
  useEffect(() => {
    fetchSessions()
      .then(s => { setSessions(s); setLoading(false); })
      .catch(e => { setError(String(e)); setLoading(false); });
  }, []);

  function handleRenamed(sessionId, displayName) {
    setSessions(prev => prev.map(s => s.sessionId === sessionId ? { ...s, displayName } : s));
  }
  function handleDeleted(sessionId) {
    setSessions(prev => prev.filter(s => s.sessionId !== sessionId));
    setSelected(prev => { const n = new Set(prev); n.delete(sessionId); return n; });
    setRows(prev => { const n = { ...prev }; delete n[sessionId]; return n; });
  }
  function handleMerged() {
    fetchSessions().then(s => setSessions(s)).catch(e => setError(String(e)));
  }
  function handleRestored() {
    // Re-fetch so the restored session reappears on the map and session list.
    fetchSessions().then(s => setSessions(s)).catch(e => setError(String(e)));
  }
  function handlePurged(sessionId) {
    // Same cleanup as a hard delete: remove from all UI state.
    setSessions(prev => prev.filter(s => s.sessionId !== sessionId));
    setSelected(prev => { const n = new Set(prev); n.delete(sessionId); return n; });
    setRows(prev => { const n = { ...prev }; delete n[sessionId]; return n; });
  }

  // ---- toggle session (lazy-fetch rows)
  const toggleSession = useCallback(async (id) => {
    const next = new Set(selected);
    if (next.has(id)) {
      next.delete(id);
    } else {
      next.add(id);
      if (!rowsBySession[id]) {
        try {
          const raw = await fetchSessionRows(id);
          setRows(prev => ({ ...prev, [id]: compactRows(raw) }));
        } catch (e) {
          setError(String(e));
        }
      }
    }
    setSelected(next);
    setFitTrigger(t => t + 1);
  }, [selected, rowsBySession]);

  function selectAll() {
    const next = new Set(sessions.map(s => s.sessionId));
    setSelected(next);
    for (const id of next) {
      if (!rowsBySession[id]) {
        fetchSessionRows(id).then(raw => {
          setRows(prev => ({ ...prev, [id]: compactRows(raw) }));
        }).catch(e => setError(String(e)));
      }
    }
  }
  function selectNone() { setSelected(new Set()); }

  // ---- precompute trace data
  const traces = useMemo(() => {
    const out = [];
    let idx = 0;
    for (const s of sessions) {
      if (!selected.has(s.sessionId)) { idx++; continue; }
      const rows = rowsBySession[s.sessionId];
      if (!rows) {
        out.push({ id: s.sessionId, color: sessionColor(idx), points: null, loading: true });
        idx++; continue;
      }
      const points = rows
        .filter(r => r.lat != null && r.lng != null && !(r.lat === 0 && r.lng === 0))
        .map(r => ({ ...r }));
      out.push({ id: s.sessionId, color: sessionColor(idx), points, rows, meta: s, idx });
      idx++;
    }
    return out;
  }, [sessions, selected, rowsBySession]);

  // ---- global time bounds
  const tBounds = useMemo(() => {
    let lo = Infinity, hi = -Infinity;
    for (const t of traces) {
      if (!t.rows) continue;
      for (const r of t.rows) {
        if (r.ts < lo) lo = r.ts;
        if (r.ts > hi) hi = r.ts;
      }
    }
    if (!isFinite(lo)) return null;
    return { lo, hi, span: Math.max(1, hi - lo) };
  }, [traces]);

  // ---- fit bounds
  const fitBounds = useMemo(() => {
    const all = [];
    for (const t of traces) if (t.points) for (const p of t.points) all.push(p);
    return bboxFromPoints(all);
  }, [traces]);

  const fitKey = boundsKey(fitBounds);
  const lastFitKeyRef = useRef('');
  useEffect(() => {
    if (!fitKey) return;
    if (fitKey === lastFitKeyRef.current) return;
    lastFitKeyRef.current = fitKey;
    setFitTrigger(t => t + 1);
  }, [fitKey]);

  // ---- auto-scale dose color
  useEffect(() => {
    if (doseScaleManual) return;
    const vals = [];
    for (const id of selected) {
      const rows = rowsBySession[id];
      if (!rows) continue;
      for (const r of rows) {
        if (typeof r.uSv === 'number' && isFinite(r.uSv)) vals.push(r.uSv);
      }
    }
    if (vals.length < 2) return;
    vals.sort((a, b) => a - b);
    const lo = vals[Math.floor(vals.length * 0.05)];
    const hi = vals[Math.floor(vals.length * 0.95)];
    if (!(hi > lo)) return;
    setDoseMin(parseFloat(lo.toFixed(3)));
    setDoseMax(parseFloat(hi.toFixed(3)));
  }, [rowsBySession, selected, doseScaleManual]);

  // ---- play
  useEffect(() => {
    if (!playing) { clearInterval(playRef.current); return; }
    playRef.current = setInterval(() => {
      setTimeFrac(prev => { const n = prev + 0.005; return n >= 1 ? 1 : n; });
    }, 60);
    return () => clearInterval(playRef.current);
  }, [playing]);
  useEffect(() => { if (playing && timeFrac >= 1) setPlaying(false); }, [timeFrac, playing]);

  // ---- filtered points (windowed)
  const filteredTraces = useMemo(() => {
    if (!tBounds) return traces.map(t => ({ ...t, filtered: t.points || [] }));
    const cursor = tBounds.lo + tBounds.span * timeFrac;
    const winSpan = tBounds.span * windowFrac;
    const windowLo = cursor - winSpan;
    return traces.map(t => {
      if (!t.points) return { ...t, filtered: [] };
      const filtered = t.points.filter(p => p.ts >= windowLo && p.ts <= cursor);
      return { ...t, filtered, cursorTs: cursor };
    });
  }, [traces, tBounds, timeFrac, windowFrac]);

  // ---- aggregate stats
  const stats = useMemo(() => {
    let n = 0, sumDose = 0, maxDose = -Infinity, minDose = Infinity;
    let sumCps = 0, maxCps = -Infinity;
    let sumSpd = 0, spdN = 0, maxSpd = -Infinity;
    let maxBrg = null, lastBrg = null;
    for (const t of filteredTraces) {
      for (const p of t.filtered) {
        if (p.uSv != null) { n++; sumDose += p.uSv; if (p.uSv > maxDose) maxDose = p.uSv; if (p.uSv < minDose) minDose = p.uSv; }
        if (p.cps != null) { sumCps += p.cps; if (p.cps > maxCps) maxCps = p.cps; }
        if (p.spd != null) { spdN++; sumSpd += p.spd; if (p.spd > maxSpd) maxSpd = p.spd; }
        if (p.brg != null) lastBrg = p.brg;
      }
    }
    return {
      count: n,
      avgDose: n ? sumDose / n : null,
      maxDose: n ? maxDose : null,
      minDose: n ? minDose : null,
      avgCps: n ? sumCps / n : null,
      maxCps: maxCps > -Infinity ? maxCps : null,
      avgSpd: spdN ? sumSpd / spdN : null,
      maxSpd: maxSpd > -Infinity ? maxSpd : null,
      lastBrg,
    };
  }, [filteredTraces]);

  // All filtered points flattened for sparkline
  const allFilteredPoints = useMemo(() => {
    const arr = [];
    for (const t of filteredTraces) for (const p of t.filtered) arr.push(p);
    arr.sort((a, b) => a.ts - b.ts);
    return arr;
  }, [filteredTraces]);

  // ---- date-grouped sessions for sessions sidebar panel
  const dateGroupedSessions = useMemo(() => {
    const q = searchFilter.toLowerCase().trim();
    const indexed = sessions.map((s, i) => ({ s: { ...s, _idx: i }, i }));
    const filtered = q
      ? indexed.filter(({ s }) => {
          const name = (s.displayName || s.sessionId).toLowerCase();
          return name.includes(q) || s.sessionId.toLowerCase().includes(q);
        })
      : indexed;
    const groups = new Map();
    for (const item of filtered) {
      const { s } = item;
      const firstOk = s.firstTsMs && s.firstTsMs >= MIN_VALID_TS_MS;
      const dateKey = firstOk
        ? new Date(s.firstTsMs).toLocaleDateString(undefined, {
            weekday: 'short', year: 'numeric', month: 'short', day: 'numeric',
          })
        : 'Unknown date';
      const ts = firstOk ? s.firstTsMs : 0;
      if (!groups.has(dateKey)) groups.set(dateKey, { ts, items: [] });
      groups.get(dateKey).items.push(item);
    }
    return [...groups.entries()]
      .sort((a, b) => b[1].ts - a[1].ts)
      .map(([date, { items }]) => ({ date, items }));
  }, [sessions, searchFilter]);

  // ---- tile
  const tile = TILES[tileIdx];

  // ---- color fn shortcut
  function getColor(p, traceIdx) {
    return getPointColor(p, colorChannel, traceIdx, doseMin, doseMax);
  }

  // ---- render ------------------------------------------------------------
  return (
    <div className="app">
      {/* === SIDEBAR === */}
      <aside className="sidebar" ref={sidebarRef} style={{ width: sidebarWidth }}>
        <header className="sidebar-header">
          <div className="app-title">
            <span className="app-icon">☢</span>
            <h1>Radiological Map</h1>
          </div>
          <div className="muted">{sessions.length} sessions loaded</div>
        </header>

        {/* Tab bar */}
        <div className="tab-bar">
          {[['sessions','Sessions'],['display','Display'],['stats','Stats'],['manage','Manage'],['db','DB']].map(([t,label]) => (
            <button key={t} className={`tab ${panel === t ? 'active' : ''}`} onClick={() => setPanel(t)}>
              {label}
            </button>
          ))}
        </div>

        {error && <div className="error-banner">{error}</div>}
        {loading && <div className="muted px16">Loading sessions...</div>}

        {/* === SESSIONS PANEL === */}
        {panel === 'sessions' && (
          <>
            <div className="btn-row">
              <button onClick={selectAll}>All</button>
              <button onClick={selectNone}>None</button>
              <button onClick={() => setFitTrigger(x => x + 1)}>Fit</button>
            </div>
            <div className="search-row">
              <input className="search-input" placeholder="Filter sessions..."
                value={searchFilter} onChange={e => setSearchFilter(e.target.value)} />
              {searchFilter && (
                <button className="btn-sm" onClick={() => setSearchFilter('')}
                  style={{ flexShrink: 0 }}>✕</button>
              )}
            </div>
            <ul className="sessions">
              {dateGroupedSessions.map(group => (
                <React.Fragment key={group.date}>
                  <li className="date-group-header">{group.date}</li>
                  {group.items.map(({ s, i }) => {
                    const isSel = selected.has(s.sessionId);
                    const c = sessionColor(i);
                    const firstOk = s.firstTsMs && s.firstTsMs >= MIN_VALID_TS_MS;
                    const dt = firstOk ? new Date(s.firstTsMs) : null;
                    const lastOk = s.lastTsMs && s.lastTsMs >= MIN_VALID_TS_MS;
                    const dur = (firstOk && lastOk) ? fmtDuration(s.lastTsMs - s.firstTsMs) : null;
                    const rows = rowsBySession[s.sessionId];
                    const maxDoseInSession = rows && rows.length
                      ? Math.max(...rows.map(r => r.uSv ?? 0)).toFixed(3)
                      : null;
                    return (
                      <li key={s.sessionId} className={`session-item ${isSel ? 'sel' : ''}`}>
                        <label className="session-label">
                          <input type="checkbox" checked={isSel} onChange={() => toggleSession(s.sessionId)} />
                          <span className="swatch" style={{ background: c }} />
                          <span className="sid">{s.displayName || s.sessionId}</span>
                        </label>
                        <div className="session-meta">
                          <span>{dt ? dt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '—'}</span>
                          <span className="badge">{s.samples ?? 0} pts</span>
                          {dur && <span className="badge">{dur}</span>}
                          {s.sizeBytes != null && fmtBytes(s.sizeBytes) && (
                            <span className="badge">{fmtBytes(s.sizeBytes)}</span>
                          )}
                          {maxDoseInSession && <span className="badge dose-badge">{maxDoseInSession} µSv/h max</span>}
                        </div>
                        {s.displayName && <div className="session-sub">{s.sessionId}</div>}
                        {s.trackerId && (
                          <div className="session-device">tracker {s.trackerId.slice(-8)}</div>
                        )}
                      </li>
                    );
                  })}
                </React.Fragment>
              ))}
            </ul>
          </>
        )}

        {/* === DISPLAY PANEL === */}
        {panel === 'display' && (
          <div className="panel-scroll">
            <SectionHead>Map Mode</SectionHead>
            <div className="mode-grid">
              {MAP_MODES.map(m => (
                <button key={m} className={`mode-btn ${mapMode === m ? 'active' : ''}`}
                  onClick={() => setMapMode(m)}>
                  {modeIcon(m)} {m}
                </button>
              ))}
            </div>

            <SectionHead>Color Channel</SectionHead>
            <div className="channel-grid">
              {COLOR_CHANNELS.map(ch => (
                <button key={ch.key}
                  className={`channel-btn ${colorChannel === ch.key ? 'active' : ''}`}
                  onClick={() => setColorChannel(ch.key)}>
                  {ch.label}
                </button>
              ))}
            </div>

            {(colorChannel === 'dose') && (
              <>
                <SectionHead>Dose scale ({nanoMode ? 'nSv/h' : 'µSv/h'})</SectionHead>
                <div className="number-row">
                  <label>min
                    <input type="number" step="0.001" value={doseMin}
                      onChange={e => { setDoseScaleManual(true); setDoseMin(parseFloat(e.target.value) || 0); }} />
                  </label>
                  <label>max
                    <input type="number" step="0.001" value={doseMax}
                      onChange={e => { setDoseScaleManual(true); setDoseMax(parseFloat(e.target.value) || 0.001); }} />
                  </label>
                  <button onClick={() => setDoseScaleManual(false)} title="Reset to auto">↺</button>
                </div>
                <div className="legend-bar">
                  {[0,0.25,0.5,0.75,1].map(t => (
                    <span key={t} style={{ background: doseColor(doseMin + (doseMax - doseMin) * t, doseMin, doseMax) }} />
                  ))}
                </div>
                <div className="legend-labels">
                  <span>{nanoMode ? (doseMin * 1000).toFixed(0) + ' nSv/h' : doseMin.toFixed(3)}</span>
                  <span>{nanoMode ? (doseMax * 1000).toFixed(0) + ' nSv/h' : doseMax.toFixed(3)}</span>
                </div>
              </>
            )}

            <SectionHead>Map Tiles</SectionHead>
            <div className="tile-grid">
              {TILES.map((tile, i) => (
                <button key={tile.name}
                  className={`tile-btn ${tileIdx === i ? 'active' : ''}`}
                  onClick={() => setTileIdx(i)}>
                  {tile.name}
                </button>
              ))}
            </div>

            <SectionHead>Rendering</SectionHead>
            <label className="check">
              <input type="checkbox" checked={nanoMode} onChange={e => setNanoMode(e.target.checked)} />
              Display nSv/h
            </label>
            <label className="check">
              <input type="checkbox" checked={showTooltips} onChange={e => setShowTooltips(e.target.checked)} />
              Show tooltips
            </label>
            {(mapMode === 'Track') && (
              <div className="slider-row">
                <span>Track width {trackWeight}px</span>
                <input type="range" min="1" max="10" value={trackWeight}
                  onChange={e => setTrackWeight(Number(e.target.value))} />
              </div>
            )}
            {(mapMode === 'Dots' || mapMode === 'Arrows') && (
              <div className="slider-row">
                <span>Point radius {pointRadius}px</span>
                <input type="range" min="2" max="16" value={pointRadius}
                  onChange={e => setPointRadius(Number(e.target.value))} />
              </div>
            )}
            {mapMode === 'Arrows' && (
              <div className="slider-row">
                <span>Arrow every {arrowEvery} pts</span>
                <input type="range" min="1" max="20" value={arrowEvery}
                  onChange={e => setArrowEvery(Number(e.target.value))} />
              </div>
            )}
          </div>
        )}

        {/* === STATS PANEL === */}
        {panel === 'stats' && (
          <div className="panel-scroll">
            <SectionHead>Window summary</SectionHead>
            <div className="stat-grid">
              <StatCard label="Points" value={stats.count} />
              <StatCard label="Avg dose" value={fmtDose(stats.avgDose, nanoMode)} accent />
              <StatCard label="Max dose" value={fmtDose(stats.maxDose, nanoMode)} accent />
              <StatCard label="Min dose" value={fmtDose(stats.minDose, nanoMode)} />
              <StatCard label="Avg CPS" value={stats.avgCps != null ? stats.avgCps.toFixed(1) : '-'} />
              <StatCard label="Max CPS" value={stats.maxCps != null ? stats.maxCps.toFixed(1) : '-'} />
              <StatCard label="Avg speed" value={fmtSpeed(stats.avgSpd)} />
              <StatCard label="Max speed" value={fmtSpeed(stats.maxSpd)} />
              <StatCard label="Bearing" value={fmtBearing(stats.lastBrg)} />
            </div>

            <SectionHead>Dose rate trend</SectionHead>
            <SparkChart points={allFilteredPoints} field="dose" doseMin={doseMin} doseMax={doseMax} height={70} />

            <SectionHead>CPS trend</SectionHead>
            <SparkChart points={allFilteredPoints} field="cps" doseMin={doseMin} doseMax={doseMax} height={70} />

            {stats.avgSpd != null && (
              <>
                <SectionHead>Speed trend</SectionHead>
                <SparkChart points={allFilteredPoints} field="speed" doseMin={0} doseMax={stats.maxSpd || 1} height={70} />
              </>
            )}
          </div>
        )}

        {/* === MANAGE PANEL === */}
        {panel === 'manage' && (
          <ManagePanel
            sessions={sessions.map((s, i) => ({ ...s, _idx: i }))}
            onRenamed={handleRenamed}
            onDeleted={handleDeleted}
            onMerged={handleMerged}
            onRestored={handleRestored}
            onPurged={handlePurged}
            onError={msg => { setError(msg); setTimeout(() => setError(null), 5000); }}
          />
        )}

        {/* === DATABASE PANEL === */}
        {panel === 'db' && (
          <DatabasePanel
            onError={msg => { setError(msg); setTimeout(() => setError(null), 8000); }}
          />
        )}

        {/* Resize handle */}
        <div className="resize-handle" onMouseDown={startResize} />
      </aside>

      {/* === MAP === */}
      <main className="map-pane">
        <MapContainer center={[39.5, -98.35]} zoom={4} style={{ width: '100%', height: '100%' }}>
          <TileLayer
            key={tile.url}
            attribution={tile.attribution}
            url={tile.url}
          />
          {fitBounds && <FitBoundsOnce bounds={fitBounds} dep={fitTrigger} />}

          {/* Heatmap mode */}
          {mapMode === 'Heatmap' && filteredTraces.map(t => (
            t.filtered.length > 0 &&
            <HeatmapLayer key={t.id} points={t.filtered} field={colorChannel} doseMin={doseMin} doseMax={doseMax} />
          ))}

          {/* Track mode */}
          {mapMode === 'Track' && filteredTraces.map(t => {
            if (!t.filtered || t.filtered.length === 0) return null;
            const segs = [];
            for (let i = 1; i < t.filtered.length; i++) {
              const a = t.filtered[i - 1], b = t.filtered[i];
              segs.push(
                <Polyline key={`${t.id}-${i}`}
                  positions={[[a.lat, a.lng], [b.lat, b.lng]]}
                  pathOptions={{
                    color: getColor(b, t.idx),
                    weight: trackWeight,
                    opacity: 0.9,
                  }}
                />
              );
            }
            return <React.Fragment key={t.id}>{segs}</React.Fragment>;
          })}

          {/* Dots mode */}
          {mapMode === 'Dots' && filteredTraces.map(t => (
            <React.Fragment key={t.id}>
              {t.filtered.map((p, i) => (
                <CircleMarker key={i} center={[p.lat, p.lng]} radius={pointRadius}
                  pathOptions={{
                    color: getColor(p, t.idx),
                    fillColor: getColor(p, t.idx),
                    fillOpacity: 0.9,
                    weight: 1,
                  }}>
                  {showTooltips && <SampleTooltip p={p} sessionId={t.id} nanoMode={nanoMode} />}
                </CircleMarker>
              ))}
            </React.Fragment>
          ))}

          {/* Arrows mode — shows dot + bearing arrow */}
          {mapMode === 'Arrows' && filteredTraces.map(t => (
            <React.Fragment key={t.id}>
              {t.filtered.map((p, i) => {
                const col = getColor(p, t.idx);
                return (
                  <React.Fragment key={i}>
                    <CircleMarker center={[p.lat, p.lng]} radius={pointRadius - 1}
                      pathOptions={{ color: col, fillColor: col, fillOpacity: 0.7, weight: 1 }}>
                      {showTooltips && <SampleTooltip p={p} sessionId={t.id} nanoMode={nanoMode} />}
                    </CircleMarker>
                    {/* Arrow every N points, only when bearing is available */}
                    {i % arrowEvery === 0 && p.brg != null && (
                      <ArrowMarker key={`arrow-${i}`} p={p} color={col} size={16 + Math.min((p.spd ?? 0) / 10, 10)} />
                    )}
                  </React.Fragment>
                );
              })}
            </React.Fragment>
          ))}

          {/* Track + Arrows overlay: end markers */}
          {(mapMode === 'Track' || mapMode === 'Dots') && filteredTraces.map(t => {
            if (!t.filtered || t.filtered.length === 0) return null;
            const last = t.filtered[t.filtered.length - 1];
            return (
              <CircleMarker key={`${t.id}-end`}
                center={[last.lat, last.lng]} radius={7}
                pathOptions={{ color: '#fff', fillColor: sessionColor(t.idx), fillOpacity: 1, weight: 2 }}>
                <Tooltip direction="top" permanent>{t.id}</Tooltip>
              </CircleMarker>
            );
          })}
        </MapContainer>

        {/* === SCRUBBER === */}
        <div className="scrubber">
          <div className="scrubber-btns">
            <button onClick={() => { setTimeFrac(0); setPlaying(false); }}>⏮</button>
            <button onClick={() => setPlaying(p => !p)}>{playing ? '⏸' : '▶'}</button>
            <button onClick={() => { setTimeFrac(1); setPlaying(false); }}>⏭</button>
          </div>
          <div className="scrubber-sliders">
            <div className="t-row">
              <label>Cursor</label>
              <input type="range" min="0" max="1" step="0.001"
                value={timeFrac} onChange={e => setTimeFrac(parseFloat(e.target.value))} />
              <span className="t-val">{tBounds ? fmtTs(tBounds.lo + tBounds.span * timeFrac) : '--'}</span>
            </div>
            <div className="t-row">
              <label>Window</label>
              <input type="range" min="0.005" max="1" step="0.005"
                value={windowFrac} onChange={e => setWindowFrac(parseFloat(e.target.value))} />
              <span className="t-val">
                {tBounds ? fmtDuration(tBounds.span * windowFrac) : '--'}
                &nbsp;·&nbsp;{allFilteredPoints.length} pts visible
              </span>
            </div>
          </div>
          <button className="timeline-toggle"
            onClick={() => setShowTimeline(v => !v)}
            title="Toggle session timeline">
            {showTimeline ? '▲ Timeline' : '▼ Timeline'}
          </button>
        </div>
        {showTimeline && (
          <TimelineView
            sessions={sessions.map((s, i) => ({ ...s, _idx: i }))}
            selected={selected}
            onToggle={id => toggleSession(id)}
          />
        )}
      </main>
    </div>
  );
}

// ---- small components ------------------------------------------------------

function SectionHead({ children }) {
  return <div className="section-head">{children}</div>;
}

function StatCard({ label, value, accent }) {
  return (
    <div className={`stat-card ${accent ? 'accent' : ''}`}>
      <div className="stat-label">{label}</div>
      <div className="stat-value">{value ?? '-'}</div>
    </div>
  );
}

function SampleTooltip({ p, sessionId, nanoMode }) {
  return (
    <Tooltip direction="top" offset={[0, -4]} opacity={0.95}>
      <div className="tt">
        <div className="tt-head">{sessionId}</div>
        <div>{fmtTs(p.ts)}</div>
        <div className="tt-row"><span>Dose</span><b>{fmtDose(p.uSv, nanoMode)}</b></div>
        <div className="tt-row"><span>CPS</span><b>{p.cps?.toFixed?.(1) ?? '—'}</b></div>
        {p.spd  != null && <div className="tt-row"><span>Speed</span><b>{p.spd.toFixed(1)} km/h</b></div>}
        {p.brg  != null && <div className="tt-row"><span>Bearing</span><b>{fmtBearing(p.brg)}</b></div>}
        {p.alt  != null && <div className="tt-row"><span>Alt</span><b>{p.alt.toFixed(0)} m</b></div>}
        {p.hdop != null && <div className="tt-row"><span>HDOP</span><b>{p.hdop.toFixed(2)}</b></div>}
      </div>
    </Tooltip>
  );
}

// ArrowMarker uses react-leaflet Marker with a custom DivIcon SVG arrow.
function ArrowMarker({ p, color, size }) {
  const icon = arrowIcon(p.brg, color, Math.round(size));
  return <Marker position={[p.lat, p.lng]} icon={icon} />;
}

function modeIcon(mode) {
  switch (mode) {
    case 'Track':   return '〜';
    case 'Dots':    return '⬤';
    case 'Heatmap': return '🌡';
    case 'Arrows':  return '➤';
    default: return '';
  }
}
