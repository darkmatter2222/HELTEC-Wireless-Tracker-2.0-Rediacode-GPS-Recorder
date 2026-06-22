import React, { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import ReactDOM from 'react-dom';
import {
  MapContainer, TileLayer, Tooltip, useMap, CircleMarker,
} from 'react-leaflet';
import L from 'leaflet';
import { fetchSessions, fetchSessionRows } from './api.js';

// ---- Constants ----

// RC-110 typically reports ~64 energy channels (0-63 BGO spectrum bins).
// Some firmware readings report up to 379 due to extended parsing.
const DEFAULT_MAX_CHANNELS = 128;

// Spectrum color palettes for waterfall displays
const SPECTRUM_PALETTES = [
  { key: 'inferno', label: 'Inferno' },
  { key: 'viridis', label: 'Viridis' },
  { key: 'plasma', label: 'Plasma' },
  { key: 'magma', label: 'Magma' },
  { key: 'turbo', label: 'Turbo' },
];

// Tile layers (same as App.jsx)
const TILES = [
  {
    name: 'CartoDB Dark',
    url: 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',
    attribution: '&copy; OSM, &copy; CartoDB',
    maxNativeZoom: 20,
  },
  {
    name: 'OSM Streets',
    url: 'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    attribution: '&copy; OSM contributors',
    maxNativeZoom: 19,
  },
  {
    name: 'Satellite (Esri)',
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
    attribution: '&copy; Esri',
    maxNativeZoom: 18,
  },
];

// ---- Palette color functions ----

function paletteColor(key, t) {
  t = Math.max(0, Math.min(1, t));
  const stop = (r, g, b) => [Math.round(r), Math.round(g), Math.round(b)];
  let s;
  switch (key) {
    case 'inferno':
      s = [stop(0,0,4), stop(75,10,37), stop(154,29,41), stop(210,83,10), stop(240,165,15), stop(252,255,154)];
      break;
    case 'viridis':
      s = [stop(68,1,84), stop(72,59,133), stop(58,107,146), stop(33,145,140), stop(94,195,99), stop(253,231,37)];
      break;
    case 'plasma':
      s = [stop(13,8,135), stop(75,3,161), stop(136,3,167), stop(189,4,140), stop(228,51,81), stop(252,253,191)];
      break;
    case 'magma':
      s = [stop(0,0,4), stop(55,10,67), stop(113,16,88), stop(170,45,78), stop(219,100,69), stop(252,255,188)];
      break;
    case 'turbo':
      s = [stop(23,18,121), stop(51,95,254), stop(64,206,254), stop(147,251,62), stop(248,246,42), stop(236,33,26)];
      break;
    default:
      s = [stop(0,0,0), stop(0,255,0)];
  }
  const idx = t * (s.length - 1);
  const lo = Math.floor(idx);
  const hi = Math.min(lo + 1, s.length - 1);
  const frac = idx - lo;
  const r = Math.round(s[lo][0] + (s[hi][0] - s[lo][0]) * frac);
  const g = Math.round(s[lo][1] + (s[hi][1] - s[lo][1]) * frac);
  const b = Math.round(s[lo][2] + (s[hi][2] - s[lo][2]) * frac);
  return `rgb(${r},${g},${b})`;
}

// ---- Spectrum analysis helpers ----

/** Compute the average spectrum across an array of channel-count arrays. */
function avgSpectrum(spectra) {
  const valid = spectra.filter(s => Array.isArray(s) && s.length > 0);
  if (valid.length === 0) return null;
  const maxLen = Math.max(...valid.map(s => s.length));
  const result = new Float64Array(maxLen);
  for (const ch of valid) {
    for (let i = 0; i < ch.length; i++) {
      result[i] += Number(ch[i]) || 0;
    }
  }
  for (let i = 0; i < maxLen; i++) {
    result[i] /= valid.length;
  }
  return Array.from(result);
}

/** Find the peak channel index in a spectrum array. */
function peakChannel(spectrum) {
  if (!Array.isArray(spectrum)) return 0;
  let maxIdx = 0, maxVal = 0;
  for (let i = 0; i < spectrum.length; i++) {
    if (spectrum[i] > maxVal) { maxVal = spectrum[i]; maxIdx = i; }
  }
  return maxIdx;
}

/** Total counts in a spectrum reading. */
function totalCounts(spectrum) {
  if (!Array.isArray(spectrum)) return 0;
  return spectrum.reduce((sum, v) => sum + (Number(v) || 0), 0);
}

/** Compute cumulative dose proxy from CPS * dt for spectral data ordering. */
function computeSpectralStats(points) {
  const withSpectrum = points.filter(p => p.spectrumData != null && Array.isArray(p.spectrumData));
  if (withSpectrum.length === 0) return null;

  const avgSpec = avgSpectrum(withSpectrum.map(p => p.spectrumData));
  const peakCh = avgSpec != null ? peakChannel(avgSpec) : 0;
  const totalAvg = avgSpec != null ? avgSpec.reduce((a, b) => a + b, 0) : 0;

  return {
    count: withSpectrum.length,
    avgSpectrum: avgSpec,
    peakChannel: peakCh,
    totalAverage: totalAvg,
    channels: avgSpec?.length ?? DEFAULT_MAX_CHANNELS,
  };
}

// ---- Spectrum hex bin coloring helper ----

function spectrumIntensityColor(intensityLo, intensityHi, value) {
  if (intensityHi === intensityLo) return 'rgba(0,230,118,0.4)';
  const t = Math.max(0, Math.min(1, (value - intensityLo) / (intensityHi - intensityLo)));
  // Green -> Amber -> Red gradient matching the rest of the app
  if (t < 0.5) {
    const lt = t * 2;
    const r = Math.round(0 + (255 - 0) * lt);
    const g = Math.round(230 + (180 - 230) * lt);
    return `rgba(${r},${g},60,0.55)`;
  } else {
    const lt = (t - 0.5) * 2;
    const r = Math.round(255);
    const g = Math.round(180 + (60 - 180) * lt);
    return `rgba(${r},${g},60,0.55)`;
  }
}

// ============================================================
// SPECTRUM WATERFALL CANVAS COMPONENT
// Draws a channel-time heatmap showing spectral data evolution
// ============================================================

function SpectrumWaterfall({ points, palette, maxChannels, autoSort }) {
  const canvasRef = useRef(null);
  const [, setRedraw] = useState(0);
  const [hoverInfo, setHoverInfo] = useState(null);

  const spectralPoints = useMemo(() => {
    let pts = points.filter(p => p.spectrumData != null && Array.isArray(p.spectrumData));
    if (autoSort) {
      pts = [...pts].sort((a, b) => a.timestampMs - b.timestampMs);
    }
    return pts;
  }, [points, autoSort]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.width;
    const H = canvas.height;
    ctx.clearRect(0, 0, W, H);

    if (spectralPoints.length === 0) {
      ctx.fillStyle = 'rgba(255,255,255,0.15)';
      ctx.font = '12px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('No spectrum data in selection', W / 2, H / 2);
      return;
    }

    const chCount = Math.min(maxChannels, spectralPoints[0].spectrumData.length || DEFAULT_MAX_CHANNELS);
    const rowCount = spectralPoints.length;
    const cellW = W / rowCount;
    const cellH = H / chCount;

    // Compute max value for normalization per row
    const maxVal = Math.max(
      ...spectralPoints.map(p => {
        const spec = p.spectrumData.slice(0, chCount);
        return spec.reduce((mx, v) => Math.max(mx, Number(v) || 0), 0);
      }),
      1
    );

    // Also compute a global max for better contrast
    const globalMax = Math.max(
      ...spectralPoints.flatMap(p => p.spectrumData.slice(0, chCount).map(v => Number(v) || 0)),
      1
    );

    for (let row = 0; row < rowCount; row++) {
      const spec = spectralPoints[row].spectrumData.slice(0, chCount);
      for (let ch = 0; ch < chCount; ch++) {
        const val = Number(spec[ch]) || 0;
        const t = Math.min(1, val / globalMax);
        ctx.fillStyle = paletteColor(palette, t);
        ctx.fillRect(row * cellW, ch * cellH, Math.ceil(cellW), Math.ceil(cellH));
      }
    }

    // Channel labels on right side
    ctx.fillStyle = 'rgba(255,255,255,0.5)';
    ctx.font = '9px monospace';
    ctx.textAlign = 'left';
    const labelStep = Math.max(1, Math.floor(chCount / 10));
    for (let ch = 0; ch < chCount; ch += labelStep) {
      const y = ch * cellH + 4;
      ctx.fillText(`Ch ${ch}`, W - 30, y);
    }
  }, [spectralPoints, palette, maxChannels, setRedraw]);

  const handleMouseMove = useCallback(
    (e) => {
      const canvas = canvasRef.current;
      if (!canvas || spectralPoints.length === 0) return;
      const rect = canvas.getBoundingClientRect();
      const mx = e.clientX - rect.left;
      const my = e.clientY - rect.top;
      const chCount = Math.min(maxChannels, spectralPoints[0].spectrumData.length || DEFAULT_MAX_CHANNELS);
      const rowCount = spectralPoints.length;
      const cellW = canvas.width / rowCount;
      const cellH = canvas.height / chCount;

      const row = Math.floor(mx / cellW);
      const ch = Math.floor(my / cellH);

      if (row >= 0 && row < rowCount && ch >= 0 && ch < chCount) {
        const spec = spectralPoints[row].spectrumData;
        setHoverInfo({
          time: new Date(spectralPoints[row].timestampMs).toLocaleTimeString(),
          channel: Math.min(ch, spec.length - 1),
          value: Number(spec[ch]) || 0,
        });
      } else {
        setHoverInfo(null);
      }
    },
    [spectralPoints, maxChannels]
  );

  return (
    <div className="spectrum-waterfall-container">
      <canvas
        ref={canvasRef}
        className="spectrum-waterfall-canvas"
        width={Math.max(600, spectralPoints.length * 3)}
        height={Math.min(400, Math.max(200, maxChannels * 3))}
        onMouseMove={handleMouseMove}
        onMouseLeave={() => setHoverInfo(null)}
      />
      {hoverInfo && (
        <div className="spectrum-waterfall-hover">
          <span>{hoverInfo.time}</span>
          <span>Ch {hoverInfo.channel}</span>
          <span>{hoverInfo.value.toLocaleString()} counts</span>
        </div>
      )}
    </div>
  );
}

// ============================================================
// SPECTRUM SINGLE-READING BAR CHART
// Shows a single spectrum reading as a vertical bar chart
// ============================================================

function SpectrumBarChart({ spectrum, palette, maxChannels }) {
  const canvasRef = useRef(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !Array.isArray(spectrum)) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.width;
    const H = canvas.height;
    ctx.clearRect(0, 0, W, H);

    const chCount = Math.min(maxChannels, spectrum.length);
    const barW = (W - 40) / chCount;
    const maxVal = Math.max(...spectrum.slice(0, chCount).map(v => Number(v) || 0), 1);

    for (let i = 0; i < chCount; i++) {
      const val = Number(spectrum[i]) || 0;
      const barH = (val / maxVal) * (H - 30);
      const t = val / maxVal;

      // Gradient fill for each bar
      const grad = ctx.createLinearGradient(0, H - 15 - barH, 0, H - 15);
      grad.addColorStop(0, paletteColor(palette, t));
      grad.addColorStop(1, paletteColor(palette, Math.max(0.05, t * 0.3)));

      ctx.fillStyle = grad;
      ctx.fillRect(20 + i * barW, H - 15 - barH, Math.max(barW - 1, 1), barH);
    }

    // Axis labels
    ctx.fillStyle = 'rgba(255,255,255,0.4)';
    ctx.font = '9px monospace';
    ctx.textAlign = 'center';
    const labelStep = Math.max(1, Math.floor(chCount / 8));
    for (let i = 0; i < chCount; i += labelStep) {
      ctx.fillText(String(i), 20 + i * barW + barW / 2, H - 2);
    }
  }, [spectrum, palette, maxChannels]);

  return (
    <canvas
      ref={canvasRef}
      className="spectrum-bar-canvas"
      width={500}
      height={200}
    />
  );
}

// ============================================================
// SPECTRUM HEX LAYER — canvas overlay for spectrum bins on map
// Adapted from HexLayer with spectrum-aware bin coloring
// ============================================================

function SpectrumHexLayer({ points, hexZoom, colorMetric }) {
  const map = useMap();
  const canvasRef = useRef(null);
  const propsRef = useRef({});

  propsRef.current = { points, hexZoom, colorMetric };

  useEffect(() => {
    const canvas = document.createElement('canvas');
    canvas.style.cssText =
      'position:absolute;top:0;left:0;pointer-events:none;z-index:400;background:transparent;';
    map.getContainer().appendChild(canvas);
    canvasRef.current = canvas;

    function draw() {
      const { points, hexZoom: hz, colorMetric } = propsRef.current;
      const size = map.getSize();
      canvas.width = size.x;
      canvas.height = size.y;
      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, size.x, size.y);

      // Collect points with spectrum data
      const specPoints = points.filter(p => p.latitude != null && p.longitude != null && p.spectrumData != null);
      if (specPoints.length === 0) return;

      // Bin size in meters at equator for a given zoom level
      const HEX_SIZE_M = Math.floor(156543.033928 / Math.pow(2, hz));
      const HEX_R = HEX_SIZE_M * 0.5;

      // Grid bins by lattice key
      const bins = new Map();
      for (const p of specPoints) {
        const key = binKey(p.latitude, p.longitude, HEX_SIZE_M);
        if (!bins.has(key)) {
          bins.set(key, { lat: p.latitude, lng: p.longitude, spectra: [], points: [] });
        }
        const bin = bins.get(key);
        bin.spectra.push(p.spectrumData);
        bin.points.push(p);
      }

      // Aggregate spectrum stats per bin and color
      let intensityValues = [];
      const binData = new Map();
      for (const [key, bin] of bins) {
        const avgSpec = avgSpectrum(bin.spectra);
        if (!avgSpec) continue;

        let intensity;
        switch (colorMetric) {
          case 'totalCounts':
            intensity = avgSpec.reduce((a, b) => a + b, 0) / bin.spectra.length;
            break;
          case 'peakChannel':
            intensity = peakChannel(avgSpec);
            break;
          case 'spectralWidth': {
            // Standard deviation of channel counts as a "width" metric
            const mean = avgSpec.reduce((a, b) => a + b, 0) / avgSpec.length;
            const variance = avgSpec.reduce((sum, v) => sum + (v - mean) ** 2, 0) / avgSpec.length;
            intensity = Math.sqrt(variance);
            break;
          }
          case 'sampleCount':
            intensity = bin.spectra.length;
            break;
          default:
            intensity = avgSpec.reduce((a, b) => a + b, 0) / bin.spectra.length;
        }

        const pt = map.latLngToContainerPoint([bin.lat, bin.lng]);
        // HEX_R in meters -> screen pixels
        const hexScreenR = (pt.distanceFrom(map.latLngToContainerPoint([bin.lat - HEX_R / 111320, bin.lng]))) ;
        intensityValues.push(intensity);
        binData.set(key, { pt, intensity, radius: Math.max(hexScreenR, 8) });
      }

      if (intensityValues.length === 0) return;

      const minI = Math.min(...intensityValues);
      const maxI = Math.max(...intensityValues);

      // Draw hex bins as circles with color gradient
      for (const [key, { pt, intensity, radius }] of binData) {
        const t = maxI === minI ? 0.5 : (intensity - minI) / (maxI - minI);
        ctx.fillStyle = spectrumIntensityColor(minI, maxI, intensity);

        // Draw hex shape (flat-top approximation as circles for performance)
        ctx.beginPath();
        ctx.arc(pt.x, pt.y, radius, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    map.on('move zoom viewreset resize', draw);
    draw();

    return () => {
      map.off('move zoom viewreset resize', draw);
      if (canvasRef.current) {
        canvasRef.current.remove();
        canvasRef.current = null;
      }
    };
  }, [map]);

  return null;
}

/** Compute a hex lattice key for a lat/lng at a given bin size in meters. */
function binKey(lat, lng, sizeM) {
  const x = Math.floor(((lng + 180) / 360) * (2 * 6378137 * Math.PI / sizeM));
  const latRad = lat * Math.PI / 180;
  const y = Math.floor(
    ((90 - lat) / 180) * (2 * 6378137 * Math.PI / sizeM) +
    0.5 * Math.log(Math.cosh(latRad)) * (6378137 / sizeM)
  );
  return `${x},${y}`;
}

// ============================================================
// SPECTRUM PANEL — main full-screen spectrum exploration mode
// ============================================================

function SpectrumView({ sessions, rowsBySession, onRowsLoaded }) {
  // Session selection
  const [selectedSessionIds, setSelectedSessionIds] = useState(new Set());
  const [loading, setLoading] = useState(false);

  // Map state
  const [mapCenter] = useState([39.5, -98.35]);
  const [mapZoom, setMapZoom] = useState(6);
  const [tileIdx, setTileIdx] = useState(0); // CartoDB Dark default

  // Spectrum hex state
  const [hexZoom, setHexZoom] = useState(12);
  const [colorMetric, setColorMetric] = useState('totalCounts');

  // Waterfall state
  const [palette, setPalette] = useState('inferno');
  const [maxChannels, setMaxChannels] = useState(64);
  const [waterfallPointLimit, setWaterfallPointLimit] = useState(500);

  // Selected bin flyout
  const [selectedBin, setSelectedBin] = useState(null);

  // Date range filter
  const [dateRangeStart, setDateRangeStart] = useState(null);
  const [dateRangeEnd, setDateRangeEnd] = useState(null);

  // Sidebar tab
  const [sideTab, setSideTab] = useState('sessions'); // 'sessions' | 'display' | 'analysis'

  // Load rows when session selection changes
  useEffect(() => {
    if (selectedSessionIds.size === 0) return;
    setLoading(true);
    const promises = [];
    for (const id of selectedSessionIds) {
      if (!rowsBySession[id]) {
        promises.push(
          fetchSessionRows(id).then(raw => {
            onRowsLoaded?.({ [id]: raw });
            return raw;
          }).catch(() => null)
        );
      }
    }
    Promise.all(promises).finally(() => setLoading(false));
  }, [selectedSessionIds]); // eslint-disable-line

  // Compact all selected session rows
  const allSpectrumPoints = useMemo(() => {
    const pts = [];
    for (const id of selectedSessionIds) {
      const rows = rowsBySession[id];
      if (!rows) continue;
      for (const row of rows) {
        // Apply date range filter
        if (dateRangeStart && row.timestampMs < dateRangeStart) continue;
        if (dateRangeEnd && row.timestampMs > dateRangeEnd) continue;
        pts.push(row);
      }
    }
    return pts;
  }, [selectedSessionIds, rowsBySession, dateRangeStart, dateRangeEnd]);

  const spectralStats = useMemo(
    () => computeSpectralStats(allSpectrumPoints),
    [allSpectrumPoints]
  );

  // Spectral points limited for waterfall display
  const waterfallPoints = useMemo(() => {
    const specPts = allSpectrumPoints.filter(p => p.spectrumData != null);
    return specPts.slice(0, waterfallPointLimit);
  }, [allSpectrumPoints, waterfallPointLimit]);

  // Average spectrum for bar chart
  const avgSpectrumReading = useMemo(() => {
    return spectralStats?.avgSpectrum ?? null;
  }, [spectralStats]);

  // Auto-detect date range from data
  useEffect(() => {
    if (allSpectrumPoints.length > 0 && !dateRangeStart) {
      const timestamps = allSpectrumPoints.map(p => p.timestampMs);
      setDateRangeStart(Math.min(...timestamps));
      setDateRangeEnd(Math.max(...timestamps));
    }
  }, [allSpectrumPoints]); // eslint-disable-line

  const handleSessionToggle = (id) => {
    setSelectedSessionIds(prev => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };

  const handleSelectAll = () => {
    setSelectedSessionIds(new Set(sessions.map(s => s.sessionId)));
  };

  const handleSelectNone = () => {
    setSelectedSessionIds(new Set());
  };

  const specPointCount = useMemo(
    () => allSpectrumPoints.filter(p => p.spectrumData != null).length,
    [allSpectrumPoints]
  );

  // Format epoch ms to date string for controls
  const fmtDate = (ts) => {
    if (!ts) return '—';
    const d = new Date(ts);
    return d.toLocaleString();
  };

  return (
    <div className="spectrum-view">
      {/* ===== Sidebar ===== */}
      <aside className="sidebar spectrum-sidebar" style={{ width: 300 }}>
        {/* Tab bar */}
        <div className="tab-bar">
          {[['sessions', 'Sessions'], ['display', 'Display'], ['analysis', 'Analysis']].map(
            ([t, label]) => (
              <button
                key={t}
                className={`tab ${sideTab === t ? 'active' : ''}`}
                onClick={() => setSideTab(t)}>
                {label}
              </button>
            )
          )}
        </div>

        <div className="panel-scroll">
          {/* --- Sessions Tab --- */}
          {sideTab === 'sessions' && (
            <>
              <div className="section-head" style={{ marginBottom: 8 }}>Session Selection</div>
              <div className="btn-row" style={{ marginBottom: 12 }}>
                <button onClick={handleSelectAll}>All</button>
                <button onClick={handleSelectNone}>None</button>
              </div>

              {loading && (
                <div className="muted px16" style={{ marginBottom: 8 }}>
                  Loading spectrum data...
                </div>
              )}

              <ul className="session-list">
                {sessions.map((s, i) => {
                  const isSel = selectedSessionIds.has(s.sessionId);
                  const dt = s.firstTsMs ? new Date(s.firstTsMs) : null;
                  const rows = rowsBySession[s.sessionId];
                  const specRows = rows?.filter(r => r.spectrumData != null)?.length ?? 0;

                  return (
                    <li key={s.sessionId} className={`session-item ${isSel ? 'sel' : ''}`}>
                      <label className="session-label">
                        <input
                          type="checkbox"
                          checked={isSel}
                          onChange={() => handleSessionToggle(s.sessionId)}
                        />
                        <span
                          className="swatch"
                          style={{ background: `hsl(${(i * 137) % 360}, 65%, 55%)` }}
                        />
                        <span className="sid">{s.displayName || s.sessionId}</span>
                      </label>
                      <div className="session-meta">
                        {dt && (
                          <span>{dt.toLocaleDateString()} {dt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}</span>
                        )}
                        <span className="badge">{s.samples ?? 0} pts</span>
                        {specRows > 0 && (
                          <span className="badge" style={{ color: '#CE93D8' }}>
                            {specRows.toLocaleString()} spec
                          </span>
                        )}
                      </div>
                    </li>
                  );
                })}
              </ul>

              {/* Date range summary */}
              {selectedSessionIds.size > 0 && (
                <div style={{ marginTop: 12, padding: '8px 12px', background: 'var(--panel2)', borderRadius: 4, fontSize: 11 }}>
                  <div style={{ color: 'var(--muted)', marginBottom: 4 }}>Spectrum Summary</div>
                  <div>{specPointCount.toLocaleString()} points with spectrum data</div>
                  {spectralStats && (
                    <>
                      <div>~{spectralStats.channels} channels per reading</div>
                      <div>Peak channel: {spectralStats.peakChannel}</div>
                    </>
                  )}
                </div>
              )}
            </>
          )}

          {/* --- Display Tab --- */}
          {sideTab === 'display' && (
            <>
              <div className="section-head">Color Metric</div>
              <div className="channel-grid" style={{ marginBottom: 16 }}>
                {[
                  { key: 'totalCounts', label: 'Total Counts' },
                  { key: 'peakChannel', label: 'Peak Channel' },
                  { key: 'spectralWidth', label: 'Spectral Width' },
                  { key: 'sampleCount', label: 'Sample Count' },
                ].map(ch => (
                  <button
                    key={ch.key}
                    className={`channel-btn ${colorMetric === ch.key ? 'active' : ''}`}
                    onClick={() => setColorMetric(ch.key)}>
                    {ch.label}
                  </button>
                ))}
              </div>

              <div className="section-head">Hex Bin Resolution</div>
              <div className="ctrl-card" style={{ marginBottom: 16 }}>
                <div className="ctrl-card-header">
                  <span className="ctrl-card-label">Level</span>
                  <span className="ctrl-card-value">{hexZoom}</span>
                </div>
                <input
                  type="range"
                  className="ctrl-range"
                  min="1"
                  max="20"
                  value={hexZoom}
                  onChange={e => setHexZoom(Number(e.target.value))}
                />
              </div>

              <div className="section-head">Waterfall Palette</div>
              <div className="channel-grid" style={{ marginBottom: 16 }}>
                {SPECTRUM_PALETTES.map(p => (
                  <button
                    key={p.key}
                    className={`channel-btn ${palette === p.key ? 'active' : ''}`}
                    onClick={() => setPalette(p.key)}>
                    {p.label}
                  </button>
                ))}
              </div>

              <div className="section-head">Max Channels</div>
              <div className="ctrl-card" style={{ marginBottom: 16 }}>
                <div className="ctrl-card-header">
                  <span className="ctrl-card-label">Channels</span>
                  <span className="ctrl-card-value">{maxChannels}</span>
                </div>
                <input
                  type="range"
                  className="ctrl-range"
                  min="8"
                  max={128}
                  step="8"
                  value={maxChannels}
                  onChange={e => setMaxChannels(Number(e.target.value))}
                />
              </div>

              <div className="section-head">Waterfall Points</div>
              <div className="ctrl-card" style={{ marginBottom: 16 }}>
                <div className="ctrl-card-header">
                  <span className="ctrl-card-label">Limit</span>
                  <span className="ctrl-card-value">{waterfallPointLimit}</span>
                </div>
                <input
                  type="range"
                  className="ctrl-range"
                  min="50"
                  max="2000"
                  step="50"
                  value={waterfallPointLimit}
                  onChange={e => setWaterfallPointLimit(Number(e.target.value))}
                />
              </div>

              <div className="section-head">Map Tiles</div>
              <div className="tile-grid">
                {TILES.map((tile, i) => (
                  <button
                    key={tile.name}
                    className={`tile-btn ${tileIdx === i ? 'active' : ''}`}
                    onClick={() => setTileIdx(i)}>
                    {tile.name}
                  </button>
                ))}
              </div>
            </>
          )}

          {/* --- Analysis Tab --- */}
          {sideTab === 'analysis' && (
            <>
              <div className="section-head">Spectrum Information</div>
              <div style={{ fontSize: 12, color: 'var(--muted)', lineHeight: 1.6 }}>
                <p>
                  Gamma energy spectrum data from RC-110 CsI(Tl) scintillator spectrometer,
                  collected at ~1 Hz via BLE. Up to 1024 energy channels per reading
                  (typically 64 for RC-110).
                </p>

                {spectralStats && (
                  <div style={{ background: 'var(--panel2)', padding: 12, borderRadius: 6, marginTop: 8 }}>
                    <div style={{ fontWeight: 700, marginBottom: 8, color: 'var(--text)' }}>
                      Current Selection Stats
                    </div>
                    <div style={{ display: 'grid', gap: 4 }}>
                      <div>Samples: {spectralStats.count.toLocaleString()}</div>
                      <div>Avg channels: {spectralStats.channels}</div>
                      <div>Peak channel: {spectralStats.peakChannel}</div>
                      <div>Total avg counts: {Math.round(spectralStats.totalAverage).toLocaleString()}</div>
                    </div>
                  </div>
                )}

                {!spectralStats && selectedSessionIds.size > 0 && (
                  <div style={{ marginTop: 8, color: 'var(--amber)' }}>
                    No spectrum data found in the selected sessions.
                    Ensure firmware v1.2.0+ with spectrum collection enabled (SPCON command).
                  </div>
                )}

                {selectedSessionIds.size === 0 && (
                  <div style={{ marginTop: 8 }}>
                    Select one or more sessions from the Sessions tab to begin analysis.
                  </div>
                )}
              </div>

              {/* Data quality notes */}
              <div className="section-head" style={{ marginTop: 16 }}>Notes</div>
              <div style={{ fontSize: 11, color: 'var(--muted)', lineHeight: 1.6 }}>
                <p>
                  <strong style={{ color: '#FFA726' }}>Saturation warning:</strong> Due to
                  UINT16_MAX (65535) capping, most channels may appear saturated until
                  the RC-110 detector is calibrated for spectrometry mode.
                </p>
                <p style={{ marginTop: 8 }}>
                  Channel counts represent integration over each BLE poll interval (~1s).
                  The temporal waterfall shows how the energy spectrum evolves during recording.
                </p>
              </div>
            </>
          )}
        </div>
      </aside>

      {/* ===== Map Area ===== */}
      <main className="map-pane spectrum-map">
        <MapContainer
          center={mapCenter}
          zoom={mapZoom}
          maxZoom={22}
          style={{ width: '100%', height: '100%' }}
          onZoomEnd={e => setMapZoom(e.target.getZoom())}>
          <TileLayer
            attribution={TILES[tileIdx].attribution}
            url={TILES[tileIdx].url}
            maxZoom={22}
            maxNativeZoom={TILES[tileIdx].maxNativeZoom ?? 19}
          />

          {/* Spectrum hex overlay */}
          {allSpectrumPoints.length > 0 && (
            <SpectrumHexLayer
              points={allSpectrumPoints}
              hexZoom={hexZoom}
              colorMetric={colorMetric}
            />
          )}

          {/* Circle markers for each spectral point location (fallback / complement) */}
          {allSpectrumPoints.filter(p => p.spectrumData != null).length > 0 && allSpectrumPoints.length <= 5000 && (
            <>
              {allSpectrumPoints.filter(p => p.spectrumData != null).map((p, i) => (
                <CircleMarker
                  key={`spec-${i}`}
                  center={[p.latitude, p.longitude]}
                  radius={3}
                  pathOptions={{
                    color: 'transparent',
                    fillColor: '#CE93D8',
                    fillOpacity: 0.25,
                    weight: 0,
                  }}
                >
                  {p.spectrumData && (
                    <Tooltip direction="top" opacity={0.9}>
                      <div style={{ fontSize: 11, lineHeight: 1.4 }}>
                        <div>{new Date(p.timestampMs).toLocaleString()}</div>
                        <div>Channels: {p.spectrumData.length}</div>
                        <div>Total: {totalCounts(p.spectrumData).toLocaleString()}</div>
                      </div>
                    </Tooltip>
                  )}
                </CircleMarker>
              ))}
            </>
          )}
        </MapContainer>

        {/* Map info overlay */}
        <div className="spectrum-map-info">
          {allSpectrumPoints.length > 0 && (
            <>
              <span>{specPointCount.toLocaleString()} spec pts</span>
              <span>|</span>
              <span>{selectedSessionIds.size} session{selectedSessionIds.size !== 1 ? 's' : ''}</span>
            </>
          )}
        </div>
      </main>

      {/* ===== Waterfall Bottom Panel ===== */}
      {waterfallPoints.length > 0 && (
        <div className="spectrum-waterfall-panel">
          <div className="spectrum-waterfall-header">
            <span className="section-head" style={{ margin: 0 }}>
              Temporal Spectrum Analysis
            </span>
            <div style={{ fontSize: 11, color: 'var(--muted)' }}>
              {waterfallPoints.length} readings · {maxChannels} channels
            </div>
          </div>

          {/* Waterfall canvas */}
          <SpectrumWaterfall
            points={waterfallPoints}
            palette={palette}
            maxChannels={maxChannels}
            autoSort={true}
          />

          {/* Average spectrum bar chart */}
          {avgSpectrumReading && (
            <div className="spectrum-legend-row">
              <div style={{ fontSize: 11, color: 'var(--muted)', padding: '4px 12px' }}>
                Average Spectrum Shape
              </div>
              <SpectrumBarChart
                spectrum={avgSpectrumReading}
                palette={palette}
                maxChannels={maxChannels}
              />
            </div>
          )}
        </div>
      )}

      {/* ===== Selected Bin Flyout ===== */}
      {selectedBin && (
        <SpectrumBinFlyout
          binData={selectedBin}
          palette={palette}
          maxChannels={maxChannels}
          onClose={() => setSelectedBin(null)}
        />
      )}
    </div>
  );
}

// ============================================================
// SPECTRUM BIN FLYOUT — details for a clicked hex bin
// ============================================================

function SpectrumBinFlyout({ binData, palette, maxChannels, onClose }) {
  const avgSpec = useMemo(() => avgSpectrum(binData.spectra), [binData]);
  const pointCount = binData.points?.length ?? 0;

  // Date range for this bin's data
  const dateRange = useMemo(() => {
    if (!binData.points || binData.points.length === 0) return null;
    const timestamps = binData.points.map(p => p.timestampMs);
    return {
      start: new Date(Math.min(...timestamps)).toLocaleString(),
      end: new Date(Math.max(...timestamps)).toLocaleString(),
    };
  }, [binData.points]);

  return (
    <div className="hex-panel spectrum-bin-flyout">
      <div className="hex-panel-header">
        <div>
          <div className="hex-panel-title">☢ Spectrum Analysis</div>
          <div className="hex-panel-loc">{binData.lat.toFixed(5)}, {binData.lng.toFixed(5)}</div>
        </div>
        <button className="hex-panel-close" onClick={onClose}>✕</button>
      </div>

      <div className="hex-panel-body">
        {/* Summary chips */}
        <div className="hex-panel-chips">
          <div className="hex-panel-chip">
            <span>Spectrum readings</span>
            <strong>{pointCount.toLocaleString()}</strong>
          </div>
          {dateRange && (
            <div className="hex-panel-chip hex-panel-chip-wide">
              <span>Period</span>
              <strong>{dateRange.start} → {dateRange.end}</strong>
            </div>
          )}
        </div>

        {/* Average spectrum bar chart */}
        {avgSpec && (
          <div style={{ marginTop: 12 }}>
            <div className="section-head">Average Spectrum</div>
            <SpectrumBarChart
              spectrum={avgSpec}
              palette={palette}
              maxChannels={maxChannels}
            />
            <div style={{ fontSize: 10, color: 'var(--muted)', textAlign: 'center', marginTop: 4 }}>
              Peak channel: {peakChannel(avgSpec)} · Total avg:{' '}
              {Math.round(avgSpec.reduce((a, b) => a + b, 0) / pointCount).toLocaleString()} counts/sample
            </div>
          </div>
        )}

        {/* Dose rate if available */}
        {binData.points && binData.points.some(p => p.uSv != null) && (() => {
          const doseVals = binData.points.map(p => p.uSv).filter(v => v != null);
          if (doseVals.length === 0) return null;
          const avg = doseVals.reduce((a, b) => a + b, 0) / doseVals.length;
          return (
            <div style={{ marginTop: 12 }}>
              <div className="section-head">Dose Rate at Location</div>
              <div className="hex-panel-stat-row">
                <div className="hex-stat-card">
                  <span>avg</span><strong>{avg.toFixed(3)} µSv/h</strong>
                </div>
                <div className="hex-stat-card">
                  <span>max</span><strong>{Math.max(...doseVals).toFixed(3)} µSv/h</strong>
                </div>
              </div>
            </div>
          );
        })()}
      </div>
    </div>
  );
}

export default SpectrumView;
