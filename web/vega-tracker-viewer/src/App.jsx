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
import { DualRangeSlider } from './DualRangeSlider.jsx';

// ---- constants -------------------------------------------------------------

const MIN_VALID_TS_MS = 1577836800000; // 2020-01-01 UTC

// Map display modes
const MAP_MODES = ['Track', 'Dots', 'Hex', 'Arrows'];

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
    maxNativeZoom: 19,
  },
  {
    name: 'CartoDB Dark',
    url: 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',
    attribution: '&copy; OpenStreetMap contributors, &copy; CartoDB',
    maxNativeZoom: 20,
  },
  {
    name: 'OpenTopoMap',
    url: 'https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png',
    attribution: '&copy; OpenStreetMap contributors, &copy; OpenTopoMap',
    maxNativeZoom: 17,
  },
  {
    name: 'Satellite (Esri)',
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
    attribution: '&copy; Esri World Imagery',
    maxNativeZoom: 18,
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

// Maps t ∈ [0,1] → green (#00e676) → yellow (#ffea00) → orange (#ff6d00) → red (#d50000)
function heatGradientColor(t) {
  let r, g, b;
  if (t < 0.4) {
    const u = t / 0.4;
    r = Math.round(u * 255);
    g = Math.round(230 + u * 4);
    b = Math.round(118 * (1 - u));
  } else if (t < 0.75) {
    const u = (t - 0.4) / 0.35;
    r = 255;
    g = Math.round(234 - u * 125);
    b = 0;
  } else {
    const u = (t - 0.75) / 0.25;
    r = Math.round(255 - u * 42);
    g = Math.round(109 - u * 109);
    b = 0;
  }
  return `rgb(${r},${g},${b})`;
}

// Hexagonal binning layer: points are aggregated into a flat-top hex grid
// defined in global Leaflet pixel coordinates.  Bins are recomputed on zoom
// change (geography changes per cell); on pan the existing bins are just
// redrawn at the new viewport offset — no per-point work needed during pan.
// Hex size is fixed in pixels (HEX_R) so geographic coverage scales naturally
// with zoom: zooming in reveals smaller geographic cells, zooming out merges
// many points into fewer coarse cells.  Ideal for millions of data points.
function HexLayer({ points, field }) {
  const map = useMap();

  useEffect(() => {
    if (!points || points.length === 0) return;

    const HEX_R = 36;          // circumradius in pixels — flat-top orientation
    const S3    = Math.sqrt(3);

    const canvas = document.createElement('canvas');
    canvas.style.cssText = 'position:absolute;top:0;left:0;pointer-events:none;z-index:400;background:transparent;';
    map.getContainer().appendChild(canvas);

    let bins    = new Map();
    let lastZoom = -1;

    // Recompute which hex each point falls into at the current zoom level.
    // Uses map.project() for global (pan-independent) pixel coordinates.
    function rebin(zoom) {
      lastZoom = zoom;
      bins = new Map();
      for (const p of points) {
        const gp  = map.project([p.lat, p.lng], zoom);
        // Flat-top hex: pixel (x, y) → fractional axial coords (q_f, r_f)
        const q_f = ( 2 / 3 * gp.x) / HEX_R;
        const r_f = (-1 / 3 * gp.x + S3 / 3 * gp.y) / HEX_R;
        const s_f = -q_f - r_f;
        // Cube-coordinate rounding
        let q = Math.round(q_f), r = Math.round(r_f), s = Math.round(s_f);
        const dq = Math.abs(q - q_f), dr = Math.abs(r - r_f), ds = Math.abs(s - s_f);
        if      (dq > dr && dq > ds) q = -r - s;
        else if (dr > ds)            r = -q - s;
        const val = field === 'cps'   ? (p.cps ?? 0)
                  : field === 'speed' ? (p.spd ?? 0)
                  :                     (p.uSv ?? 0);
        const key = `${q},${r}`;
        if (bins.has(key)) {
          const b = bins.get(key);
          b.sum += val;
          b.count++;
        } else {
          bins.set(key, { q, r, sum: val, count: 1 });
        }
      }
    }

    function draw() {
      const zoom = map.getZoom();
      if (zoom !== lastZoom) rebin(zoom);

      const size   = map.getSize();
      canvas.width  = size.x;
      canvas.height = size.y;
      const ctx    = canvas.getContext('2d');
      const W      = size.x, H = size.y;
      // Explicit clear guards against GPU compositing leaving stale pixels
      ctx.clearRect(0, 0, W, H);

      // Top-left corner of the viewport in global pixel space
      const origin = map.project(map.getBounds().getNorthWest(), zoom);
      const ox = origin.x, oy = origin.y;

      // Normalise against the max average of the currently VISIBLE hexes
      // so colour contrast is always high regardless of absolute values.
      let maxAvg = 1e-9;
      for (const b of bins.values()) {
        const cx = HEX_R * 1.5 * b.q - ox;
        const cy = HEX_R * S3 * (b.r + b.q / 2) - oy;
        if (cx > -HEX_R * 2 && cx < W + HEX_R * 2 &&
            cy > -HEX_R * 2 && cy < H + HEX_R * 2) {
          const avg = b.sum / b.count;
          if (avg > maxAvg) maxAvg = avg;
        }
      }

      for (const b of bins.values()) {
        // Hex centre in screen-pixel space for the current viewport
        const cx = HEX_R * 1.5 * b.q - ox;
        const cy = HEX_R * S3 * (b.r + b.q / 2) - oy;
        if (cx < -HEX_R * 2 || cx > W + HEX_R * 2 ||
            cy < -HEX_R * 2 || cy > H + HEX_R * 2) continue;

        const t     = Math.min(1, (b.sum / b.count) / maxAvg);
        const color = heatGradientColor(t);

        // Draw at 94% of bin radius — minimal gap between hexes.
        const DR = HEX_R * 0.94;

        // Draw flat-top hexagon (vertex 0 at angle 0° = right)
        ctx.beginPath();
        for (let i = 0; i < 6; i++) {
          const a  = (Math.PI / 3) * i;
          const vx = cx + DR * Math.cos(a);
          const vy = cy + DR * Math.sin(a);
          i === 0 ? ctx.moveTo(vx, vy) : ctx.lineTo(vx, vy);
        }
        ctx.closePath();
        ctx.globalAlpha = 0.55;
        ctx.fillStyle = color;
        ctx.fill();
        ctx.globalAlpha = 1;
        ctx.strokeStyle = 'rgba(255,255,255,0.25)';
        ctx.lineWidth   = 0.8;
        ctx.stroke();

        // Inset count label — only when hex is large enough to be legible
        if (b.count > 1 && DR >= 18) {
          ctx.globalAlpha  = 0.9;
          ctx.fillStyle    = '#fff';
          ctx.font         = `bold ${Math.max(9, Math.round(DR * 0.36))}px sans-serif`;
          ctx.textAlign    = 'center';
          ctx.textBaseline = 'middle';
          ctx.fillText(b.count > 9999 ? '9k+' : String(b.count), cx, cy);
          ctx.globalAlpha  = 1;
        }
      }
    }

    rebin(map.getZoom());
    map.on('move zoom viewreset', draw);
    draw();

    return () => {
      map.off('move zoom viewreset', draw);
      canvas.remove();
    };
  }, [points, field, map]);

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

function getPointColor(p, channel, idx, ranges) {
  switch (channel) {
    case 'cps':     return cpsColor(p.cps,  ranges.cpsMin,  ranges.cpsMax);
    case 'speed':   return speedColor(p.spd, ranges.spdMin,  ranges.spdMax);
    case 'alt':     return altColor(p.alt,   ranges.altMin,  ranges.altMax);
    case 'hdop':    return hdopColor(p.hdop, ranges.hdopMin, ranges.hdopMax);
    case 'session': return sessionColor(idx);
    default:        return doseColor(p.uSv,  ranges.doseMin, ranges.doseMax);
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
  // Per-channel [lo, hi] scale state + manual-override flags
  const [doseMin, setDoseMin]   = useState(0);    const [doseMax, setDoseMax]   = useState(1.0);   const [doseManual, setDoseManual]   = useState(false);
  const [cpsMin,  setCpsMin]    = useState(0);    const [cpsMax,  setCpsMax]    = useState(50);    const [cpsManual,  setCpsManual]    = useState(false);
  const [spdMin,  setSpdMin]    = useState(0);    const [spdMax,  setSpdMax]    = useState(80);    const [spdManual,  setSpdManual]    = useState(false);
  const [altMin,  setAltMin]    = useState(0);    const [altMax,  setAltMax]    = useState(500);   const [altManual,  setAltManual]    = useState(false);
  const [hdopMin, setHdopMin]   = useState(0);    const [hdopMax, setHdopMax]   = useState(5);     const [hdopManual, setHdopManual]   = useState(false);
  // stable track bounds for sliders — derived from raw data, never from the scale handles
  const [doseDataMax, setDoseDataMax] = useState(2.0);
  const [cpsDataMax,  setCpsDataMax]  = useState(100);
  const [spdDataMax,  setSpdDataMax]  = useState(120);
  const [altDataMax,  setAltDataMax]  = useState(1000);
  // legacy alias kept for existing references
  const doseScaleManual = doseManual;
  const setDoseScaleManual = setDoseManual;

  // Map / display mode
  const [mapMode, setMapMode]         = useState('Track');  // Track | Dots | Hex | Arrows
  const [colorChannel, setColorChannel] = useState('dose'); // dose | cps | speed | alt | hdop | session
  const [nanoMode, setNanoMode]         = useState(false);
  const [tileIdx, setTileIdx]           = useState(1);       // default CartoDB Dark
  const [trackWeight, setTrackWeight]   = useState(4);
  const [pointRadius, setPointRadius]   = useState(5);
  const [showTooltips, setShowTooltips] = useState(true);
  const [arrowEvery, setArrowEvery]         = useState(5);    // show 1-in-N arrows
  const [trackShowDots, setTrackShowDots]   = useState(false);
  const [trackDotOpacity, setTrackDotOpacity] = useState(0.5);
  const [arrowDotOpacity, setArrowDotOpacity] = useState(0.12);
  const [arrowShowTrack, setArrowShowTrack] = useState(false);
  const [arrowTrackOpacity, setArrowTrackOpacity] = useState(0.25);

  // Timeline
  const [timeFrac, setTimeFrac]     = useState(1.0);
  const [windowFrac, setWindowFrac] = useState(1.0);
  const [playing, setPlaying]       = useState(false);
  const [fitTrigger, setFitTrigger] = useState(0);
  const playRef = useRef();

  // App mode and explore sidebar panel
  const [appMode, setAppMode] = useState('explore'); // explore | manage
  const [explorePanel, setExplorePanel] = useState('sessions'); // sessions | display | stats
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

  // ---- auto-scale all color channels from loaded data
  function autoScaleChannel(field, manualFlag, setLo, setHi, decimals = 2) {
    const vals = [];
    for (const id of selected) {
      const rows = rowsBySession[id];
      if (!rows) continue;
      for (const r of rows) {
        const v = r[field];
        if (typeof v === 'number' && isFinite(v)) vals.push(v);
      }
    }
    if (vals.length < 2) return;
    vals.sort((a, b) => a - b);
    const lo = vals[Math.floor(vals.length * 0.02)];
    const hi = vals[Math.floor(vals.length * 0.98)];
    if (!(hi > lo)) return;
    setLo(parseFloat(lo.toFixed(decimals)));
    setHi(parseFloat(hi.toFixed(decimals)));
  }

  useEffect(() => {
    if (!doseManual) autoScaleChannel('uSv',  false, setDoseMin, setDoseMax, 3);
    if (!cpsManual)  autoScaleChannel('cps',  false, setCpsMin,  setCpsMax,  1);
    if (!spdManual)  autoScaleChannel('spd',  false, setSpdMin,  setSpdMax,  1);
    if (!altManual)  autoScaleChannel('alt',  false, setAltMin,  setAltMax,  0);
    if (!hdopManual) autoScaleChannel('hdop', false, setHdopMin, setHdopMax, 2);
    // always update stable track bounds from raw data max
    const rawMax = (field, fallback) => {
      let m = fallback;
      for (const id of selected) {
        const rows = rowsBySession[id];
        if (!rows) continue;
        for (const r of rows) {
          const v = r[field];
          if (typeof v === 'number' && isFinite(v) && v > m) m = v;
        }
      }
      return m;
    };
    setDoseDataMax(Math.max(rawMax('uSv',  0) * 1.2, 2));
    setCpsDataMax (Math.max(rawMax('cps',  0) * 1.2, 10));
    setSpdDataMax (Math.max(rawMax('spd',  0) * 1.2, 20));
    setAltDataMax (Math.max(rawMax('alt',  0) * 1.2, 100));
  }, [rowsBySession, selected, doseManual, cpsManual, spdManual, altManual, hdopManual]); // eslint-disable-line

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
  const ranges = { doseMin, doseMax, cpsMin, cpsMax, spdMin, spdMax, altMin, altMax, hdopMin, hdopMax };
  function getColor(p, traceIdx) {
    return getPointColor(p, colorChannel, traceIdx, ranges);
  }

  // ---- render ------------------------------------------------------------
  return (
    <div className="app">
      {/* === TOP NAV === */}
      <nav className="top-nav">
        <div className="nav-brand">
          <span className="app-icon">☢</span>
          <span className="nav-title">Radiological Map</span>
        </div>
        <div className="nav-modes">
          <button
            className={`nav-mode-btn ${appMode === 'explore' ? 'active' : ''}`}
            onClick={() => setAppMode('explore')}>
            Explore
          </button>
          <button
            className={`nav-mode-btn ${appMode === 'manage' ? 'active' : ''}`}
            onClick={() => setAppMode('manage')}>
            Data Management
          </button>
        </div>
        <div className="nav-meta">
          <span>{sessions.length} session{sessions.length !== 1 ? 's' : ''}</span>
        </div>
      </nav>

      {/* === APP BODY === */}
      <div className="app-body">

      {/* === EXPLORE MODE === */}
      {appMode === 'explore' && (<>
      {/* === SIDEBAR === */}
      <aside className="sidebar" ref={sidebarRef} style={{ width: sidebarWidth }}>

        {/* Tab bar */}
        <div className="tab-bar">
          {[['sessions','Sessions'],['display','Display'],['stats','Stats']].map(([t,label]) => (
            <button key={t} className={`tab ${explorePanel === t ? 'active' : ''}`} onClick={() => setExplorePanel(t)}>
              {label}
            </button>
          ))}
        </div>

        {error && <div className="error-banner">{error}</div>}
        {loading && <div className="muted px16">Loading sessions...</div>}

        {/* === SESSIONS PANEL === */}
        {explorePanel === 'sessions' && (
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
        {explorePanel === 'display' && (
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
              <DualRangeSlider
                lo={0} hi={doseDataMax}
                low={doseMin} high={doseMax}
                onLowChange={v  => { setDoseManual(true); setDoseMin(parseFloat(v.toFixed(3))); }}
                onHighChange={v => { setDoseManual(true); setDoseMax(parseFloat(v.toFixed(3))); }}
                colorFn={t => doseColor(t, 0, 1)}
                label={`Dose rate scale (${nanoMode ? 'nSv/h' : 'µSv/h'})`}
                fmtVal={v => nanoMode ? (v * 1000).toFixed(0) + ' nSv/h' : v.toFixed(3) + ' µSv/h'}
                onAuto={() => setDoseManual(false)}
              />
            )}
            {colorChannel === 'cps' && (
              <DualRangeSlider
                lo={0} hi={cpsDataMax}
                low={cpsMin} high={cpsMax}
                onLowChange={v  => { setCpsManual(true); setCpsMin(parseFloat(v.toFixed(1))); }}
                onHighChange={v => { setCpsManual(true); setCpsMax(parseFloat(v.toFixed(1))); }}
                colorFn={t => cpsColor(t, 0, 1)}
                label="CPS scale"
                fmtVal={v => v.toFixed(0) + ' cps'}
                onAuto={() => setCpsManual(false)}
              />
            )}
            {colorChannel === 'speed' && (
              <DualRangeSlider
                lo={0} hi={spdDataMax}
                low={spdMin} high={spdMax}
                onLowChange={v  => { setSpdManual(true); setSpdMin(parseFloat(v.toFixed(1))); }}
                onHighChange={v => { setSpdManual(true); setSpdMax(parseFloat(v.toFixed(1))); }}
                colorFn={t => speedColor(t, 0, 1)}
                label="Speed scale (km/h)"
                fmtVal={v => v.toFixed(0) + ' km/h'}
                onAuto={() => setSpdManual(false)}
              />
            )}
            {colorChannel === 'alt' && (
              <DualRangeSlider
                lo={0} hi={altDataMax}
                low={altMin} high={altMax}
                onLowChange={v  => { setAltManual(true); setAltMin(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setAltManual(true); setAltMax(parseFloat(v.toFixed(0))); }}
                colorFn={t => altColor(t, 0, 1)}
                label="Altitude scale (m)"
                fmtVal={v => v.toFixed(0) + ' m'}
                onAuto={() => setAltManual(false)}
              />
            )}
            {colorChannel === 'hdop' && (
              <DualRangeSlider
                lo={0} hi={10}
                low={hdopMin} high={hdopMax}
                onLowChange={v  => { setHdopManual(true); setHdopMin(parseFloat(v.toFixed(2))); }}
                onHighChange={v => { setHdopManual(true); setHdopMax(parseFloat(v.toFixed(2))); }}
                colorFn={t => hdopColor(t, 0, 1)}
                label="HDOP scale"
                fmtVal={v => v.toFixed(1)}
                onAuto={() => setHdopManual(false)}
              />
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
            {mapMode === 'Track' && (
              <>
                <div className="slider-row">
                  <span>Track width {trackWeight}px</span>
                  <input type="range" min="1" max="10" value={trackWeight}
                    onChange={e => setTrackWeight(Number(e.target.value))} />
                </div>
                <label className="check">
                  <input type="checkbox" checked={trackShowDots}
                    onChange={e => setTrackShowDots(e.target.checked)} />
                  Overlay dots on track
                </label>
                {trackShowDots && (
                  <div className="slider-row">
                    <span>Dot opacity {Math.round(trackDotOpacity * 100)}%</span>
                    <input type="range" min="0.05" max="1" step="0.05" value={trackDotOpacity}
                      onChange={e => setTrackDotOpacity(Number(e.target.value))} />
                  </div>
                )}
              </>
            )}
            {(mapMode === 'Dots' || mapMode === 'Arrows') && (
              <div className="slider-row">
                <span>Point radius {pointRadius}px</span>
                <input type="range" min="2" max="16" value={pointRadius}
                  onChange={e => setPointRadius(Number(e.target.value))} />
              </div>
            )}
            {mapMode === 'Arrows' && (
              <>
                <div className="slider-row">
                  <span>Arrow every {arrowEvery} pts</span>
                  <input type="range" min="1" max="20" value={arrowEvery}
                    onChange={e => setArrowEvery(Number(e.target.value))} />
                </div>
                <div className="slider-row">
                  <span>Dot opacity {Math.round(arrowDotOpacity * 100)}%</span>
                  <input type="range" min="0" max="1" step="0.05" value={arrowDotOpacity}
                    onChange={e => setArrowDotOpacity(Number(e.target.value))} />
                </div>
                <label className="check">
                  <input type="checkbox" checked={arrowShowTrack}
                    onChange={e => setArrowShowTrack(e.target.checked)} />
                  Show track underlay
                </label>
                {arrowShowTrack && (
                  <div className="slider-row">
                    <span>Track opacity {Math.round(arrowTrackOpacity * 100)}%</span>
                    <input type="range" min="0.05" max="1" step="0.05" value={arrowTrackOpacity}
                      onChange={e => setArrowTrackOpacity(Number(e.target.value))} />
                  </div>
                )}
              </>
            )}
          </div>
        )}

        {/* === STATS PANEL === */}
        {explorePanel === 'stats' && (
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

        {/* Resize handle */}
        <div className="resize-handle" onMouseDown={startResize} />
      </aside>

      {/* === MAP === */}
      <main className="map-pane">
        <MapContainer center={[39.5, -98.35]} zoom={6} maxZoom={22} style={{ width: '100%', height: '100%' }}>
          <TileLayer
            key={tile.url}
            attribution={tile.attribution}
            url={tile.url}
            maxZoom={22}
            maxNativeZoom={tile.maxNativeZoom ?? 19}
          />
          {fitBounds && <FitBoundsOnce bounds={fitBounds} dep={fitTrigger} />}

          {/* Hex binning mode */}
          {mapMode === 'Hex' && filteredTraces.map(t =>
            t.filtered.length > 0
              ? <HexLayer key={t.id} points={t.filtered} field={colorChannel} />
              : null
          )}

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

          {/* Track mode — optional dot overlay */}
          {mapMode === 'Track' && trackShowDots && filteredTraces.map(t => (
            <React.Fragment key={`${t.id}-tdots`}>
              {t.filtered.map((p, i) => (
                <CircleMarker key={i} center={[p.lat, p.lng]} radius={pointRadius}
                  pathOptions={{
                    color: 'transparent',
                    fillColor: getColor(p, t.idx),
                    fillOpacity: trackDotOpacity,
                    weight: 0,
                  }} />
              ))}
            </React.Fragment>
          ))}

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

          {/* Arrows mode — optional track underlay */}
          {mapMode === 'Arrows' && arrowShowTrack && filteredTraces.map(t => {
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
                    opacity: arrowTrackOpacity,
                  }}
                />
              );
            }
            return <React.Fragment key={`${t.id}-atrack`}>{segs}</React.Fragment>;
          })}

          {/* Arrows mode — shows dot + bearing arrow */}
          {mapMode === 'Arrows' && filteredTraces.map(t => (
            <React.Fragment key={t.id}>
              {t.filtered.map((p, i) => {
                const col = getColor(p, t.idx);
                return (
                  <React.Fragment key={i}>
                    <CircleMarker center={[p.lat, p.lng]} radius={pointRadius - 1}
                      pathOptions={{ color: col, fillColor: col, fillOpacity: arrowDotOpacity, weight: 0 }}>
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
      </>)}
      {/* end explore mode */}

      {/* === DATA MANAGEMENT MODE === */}
      {appMode === 'manage' && (
        <div className="data-mgmt-view">
          <div className="data-mgmt-panel">
            <div className="data-mgmt-panel-header">Session Management</div>
            <ManagePanel
              sessions={sessions.map((s, i) => ({ ...s, _idx: i }))}
              onRenamed={handleRenamed}
              onDeleted={handleDeleted}
              onMerged={handleMerged}
              onRestored={handleRestored}
              onPurged={handlePurged}
              onError={msg => { setError(msg); setTimeout(() => setError(null), 5000); }}
            />
          </div>
          <div className="data-mgmt-panel">
            <div className="data-mgmt-panel-header">Database</div>
            <DatabasePanel
              onError={msg => { setError(msg); setTimeout(() => setError(null), 8000); }}
            />
          </div>
        </div>
      )}

      {appMode === 'manage' && error && (
        <div style={{ position: 'fixed', bottom: 16, right: 16, zIndex: 9999 }}>
          <div className="error-banner" style={{ maxWidth: 400 }}>{error}</div>
        </div>
      )}

      </div>{/* app-body */}
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
