import React, { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import ReactDOM from 'react-dom';
import {
  MapContainer, TileLayer, Polyline, CircleMarker, Polygon, Rectangle,
  Tooltip, useMap, useMapEvents, Marker,
} from 'react-leaflet';
import L from 'leaflet';
import { fetchSessions, fetchSessionRows, fetchAreaSessions } from './api.js';
import {
  doseColor, cpsColor, speedColor, altColor, hdopColor, accColor, dosePerCountColor,
  sessionColor, fmtTs, fmtDose,
  totalCountsColor, peakChannelColor, lowEnergyColor, highEnergyColor, spectralCentroidColor, spectralEntropyColor,
} from './colors.js';
import { SparkChart } from './SparkChart.jsx';
import { ManagePanel } from './ManagePanel.jsx';
import { TimelineView } from './TimelineView.jsx';
import { DatabasePanel } from './DatabasePanel.jsx';
import { DualRangeSlider } from './DualRangeSlider.jsx';
import RenderPanel from './RenderPanel.jsx';
import { ExportPanel } from './ExportPanel.jsx';
import { ThreeDView } from './ThreeDView.jsx';
import { ExplorerPanel } from './ExplorerPanel.jsx';
import { LiveTrackingPanel } from './LiveTrackingPanel.jsx';


// ---- constants -------------------------------------------------------------

const MIN_VALID_TS_MS = 1577836800000; // 2020-01-01 UTC

// Map display modes
const MAP_MODES = ['Track', 'Dots', 'Hex', 'Arrows'];

// Color channel options
const COLOR_CHANNELS = [
  { key: 'dose',    label: 'Dose rate' },
  { key: 'cps',     label: 'CPS' },
  { key: 'speed',   label: 'Speed' },
  { key: 'alt',     label: 'Altitude' },
  { key: 'hdop',    label: 'HDOP' },
  { key: 'accM',    label: 'Accuracy (m)' },
  { key: 'dpc',     label: 'Dose/Count' },
  { key: 'session', label: 'Session' },
];

// Spectrogram color channels — derive metrics from gamma energy spectrum
// channel counts stored in CSV column `spectrumData` (pipe-delimited uint16).
// Null when spectrum collection is disabled or the peer doesn't support DATA_BUF.
const SPECTROGRAM_CHANNELS = [
  { key: 'totalcounts', label: 'Total Counts' },
  { key: 'peakchannel', label: 'Peak Channel' },
  { key: 'lowenergy',   label: 'Low Energy' },
  { key: 'highenergy',  label: 'High Energy' },
  { key: 'centroid',    label: 'Spectral Centroid' },
  { key: 'entropy',     label: 'Spectral Entropy' },
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

// Max plausible straight-line distance between two consecutive 1 Hz GPS fixes.
// Based on 100 mph (160.9 km/h) with a 3-second buffer = ~134 m.
// Any two points farther apart than this get a gap drawn instead of a line,
// preventing the long phantom diagonals that appear when GPS jumps.
const MAX_SEGMENT_M = 150;

function haversineMeters(lat1, lng1, lat2, lng2) {
  const R = 6371000;
  const toRad = d => d * Math.PI / 180;
  const dLat  = toRad(lat2 - lat1);
  const dLng  = toRad(lng2 - lng1);
  const a = Math.sin(dLat / 2) ** 2
          + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLng / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

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

// Like FitBoundsOnce but accepts a plain [[minLat,minLng],[maxLat,maxLng]] array
// (Leaflet fitBounds natively accepts that format, no LatLngBounds object required)
function FitBboxOnce({ bbox, dep }) {
  const map = useMap();
  useEffect(() => {
    if (bbox) map.fitBounds(bbox, { padding: [40, 40], animate: true });
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

// Syncs hexBinZoom state to map zoom when auto-follow is enabled.
// Must be rendered inside MapContainer.
function MapZoomSync({ onZoomChange }) {
  useMapEvents({ zoom: (e) => onZoomChange(e.target.getZoom()) });
  return null;
}

// Hexagonal binning layer.
// binZoom controls the geographic resolution of bins — independent of the
// current map view zoom.  Higher binZoom = finer bins (more, smaller hexes).
// Lower binZoom = coarser bins (fewer, bigger hexes covering more area).
// The pixel scale factor 2^(mapZoom-binZoom) converts bin pixel coords from
// binZoom space to the current screen space on every draw, so geography is
// always correct regardless of the zoom mismatch.

// ---- Spectrum analysis helpers (for HexLayer bin accumulation + HexBinPanel) ----

/** Compute the average spectrum across an array of channel-count arrays. */
function avgSpectrum(spectra) {
  const valid = spectra.filter(s => Array.isArray(s) && s.length > 0);
  if (valid.length === 0) return null;
  let maxLen = 0;
  for (const s of valid) { if (s.length > maxLen) maxLen = s.length; }
  const result = new Float64Array(maxLen);
  for (const ch of valid) { for (let i = 0; i < ch.length; i++) result[i] += Number(ch[i]) || 0; }
  for (let i = 0; i < maxLen; i++) result[i] /= valid.length;
  return Array.from(result);
}

/** Total counts in a spectrum reading. */
function totalCounts(spectrum) {
  if (!Array.isArray(spectrum)) return 0;
  return spectrum.reduce((sum, v) => sum + (Number(v) || 0), 0);
}

/** Peak channel index in a spectrum array. */
function peakChannelIdx(spectrum) {
  if (!Array.isArray(spectrum)) return 0;
  let maxIdx = 0, maxVal = 0;
  for (let i = 0; i < spectrum.length; i++) { if (spectrum[i] > maxVal) { maxVal = spectrum[i]; maxIdx = i; } }
  return maxIdx;
}

/** Low-energy band sum (channels 0-24, ambient/background). */
function lowEnergySum(spectrum) {
  if (!Array.isArray(spectrum)) return 0;
  let sum = 0; for (let i = 0; i < Math.min(25, spectrum.length); i++) sum += Number(spectrum[i]) || 0;
  return sum;
}

/** High-energy band sum (tail channels, gamma). */
function highEnergySum(spectrum) {
  if (!Array.isArray(spectrum)) return 0;
  let sum = 0; for (let i = Math.max(0, spectrum.length - 25); i < spectrum.length; i++) sum += Number(spectrum[i]) || 0;
  return sum;
}

/** Spectral centroid — center of mass of the spectrum. */
function spectralCentroid(spectrum) {
  if (!Array.isArray(spectrum) || spectrum.length === 0) return 0;
  let weightedSum = 0, total = 0;
  for (let i = 0; i < spectrum.length; i++) { const v = Number(spectrum[i]) || 0; weightedSum += i * v; total += v; }
  return total > 0 ? weightedSum / total : 0;
}

/** Spectral entropy — spread/complexity (bits). */
function spectralEntropy(spectrum) {
  if (!Array.isArray(spectrum) || spectrum.length === 0) return 0;
  const total = totalCounts(spectrum);
  if (total === 0) return 0;
  let entropy = 0;
  for (let i = 0; i < spectrum.length; i++) { const p = (Number(spectrum[i]) || 0) / total; if (p > 0) entropy -= p * Math.log2(p); }
  return entropy;
}

/** Compute aggregate spectrum stats from a bin's raw pts array. */
function computeBinSpectrumStats(pts) {
  const specPts = pts.filter(p => p.spectrum != null && Array.isArray(p.spectrum));
  if (specPts.length === 0) return null;
  const avgSpec = avgSpectrum(specPts.map(p => p.spectrum));
  if (!avgSpec) return null;
  return { count: specPts.length, avgSpectrum: avgSpec, peakChannel: peakChannelIdx(avgSpec), totalAverage: totalCounts(avgSpec), channels: avgSpec.length };
}

function HexLayer({ traces, field, binZoom, onBinClick, onBinHover, ranges, radarEnabled = false }) {
  const map = useMap();

  useEffect(() => {
    // Flatten all traces into one point list; skip traces with no data.
    const allPoints = [];
    if (traces) {
      for (const t of traces) {
        for (const p of t.filtered) {
          if (p.lat != null && p.lng != null) allPoints.push({ ...p, _sid: t.id });
        }
      }
    }
    if (allPoints.length === 0) return;

    const HEX_R = 36;          // circumradius in pixels at binZoom — flat-top
    const S3    = Math.sqrt(3);

    const canvas = document.createElement('canvas');
    // Enable pointer events when click or hover handlers are provided.
    canvas.style.cssText = `position:absolute;top:0;left:0;pointer-events:${(onBinClick || onBinHover) ? 'auto' : 'none'};z-index:400;background:transparent;`;
    map.getContainer().appendChild(canvas);

    // Bin all points at binZoom resolution once per effect run.
    // Each bin stores aggregate stats for the flyout panel.
    const bins = new Map();
    for (const p of allPoints) {
      const gp  = map.project([p.lat, p.lng], binZoom);
      const q_f = ( 2 / 3 * gp.x) / HEX_R;
      const r_f = (-1 / 3 * gp.x + S3 / 3 * gp.y) / HEX_R;
      const s_f = -q_f - r_f;
      let q = Math.round(q_f), r = Math.round(r_f), s = Math.round(s_f);
      const dq = Math.abs(q - q_f), dr = Math.abs(r - r_f), ds = Math.abs(s - s_f);
      if      (dq > dr && dq > ds) q = -r - s;
      else if (dr > ds)            r = -q - s;
      const key = `${q},${r}`;
      if (bins.has(key)) {
        const b = bins.get(key);
        b.count++;
        b.latSum += p.lat; b.lngSum += p.lng;
        if (p.uSv  != null) { b.uSvSum  += p.uSv;  b.uSvN++;  if (p.uSv  < b.uSvMin)  b.uSvMin  = p.uSv;  if (p.uSv  > b.uSvMax)  b.uSvMax  = p.uSv;  }
        if (p.cps  != null) { b.cpsSum  += p.cps;  b.cpsN++;  if (p.cps  < b.cpsMin)  b.cpsMin  = p.cps;  if (p.cps  > b.cpsMax)  b.cpsMax  = p.cps;  }
        if (p.spd  != null) { b.spdSum  += p.spd;  b.spdN++;  if (p.spd  < b.spdMin)  b.spdMin  = p.spd;  if (p.spd  > b.spdMax)  b.spdMax  = p.spd;  }
        if (p.alt  != null) { b.altSum  += p.alt;  b.altN++;  if (p.alt  < b.altMin)  b.altMin  = p.alt;  if (p.alt  > b.altMax)  b.altMax  = p.alt;  }
        if (p.dpc  != null) { b.dpcSum  += p.dpc;  b.dpcN++;  if (p.dpc  < b.dpcMin)  b.dpcMin  = p.dpc;  if (p.dpc  > b.dpcMax)  b.dpcMax  = p.dpc;  }
        if (p.hdop != null) { b.hdopSum += p.hdop; b.hdopN++; if (p.hdop < b.hdopMin) b.hdopMin = p.hdop; if (p.hdop > b.hdopMax) b.hdopMax = p.hdop; }
        if (p.accM != null) { b.accMSum += p.accM; b.accMN++; if (p.accM < b.accMMin) b.accMMin = p.accM; if (p.accM > b.accMMax) b.accMMax = p.accM; }
        if (p._sid) b.sessionIds.add(p._sid);
        // Collect spectrum data for per-bin spectral analysis
        if (p.spectrum != null && Array.isArray(p.spectrum)) b.spec.push(p.spectrum);
        b.pts.push(p);
      } else {
        bins.set(key, {
          q, r, count: 1,
          latSum: p.lat, lngSum: p.lng,
          uSvSum:  p.uSv  ?? 0, uSvN:  p.uSv  != null ? 1 : 0, uSvMin:  p.uSv  ?? Infinity, uSvMax:  p.uSv  ?? -Infinity,
          cpsSum:  p.cps  ?? 0, cpsN:  p.cps  != null ? 1 : 0, cpsMin:  p.cps  ?? Infinity, cpsMax:  p.cps  ?? -Infinity,
          spdSum:  p.spd  ?? 0, spdN:  p.spd  != null ? 1 : 0, spdMin:  p.spd  ?? Infinity, spdMax:  p.spd  ?? -Infinity,
          altSum:  p.alt  ?? 0, altN:  p.alt  != null ? 1 : 0, altMin:  p.alt  ?? Infinity, altMax:  p.alt  ?? -Infinity,
          dpcSum:  p.dpc  ?? 0, dpcN:  p.dpc  != null ? 1 : 0, dpcMin:  p.dpc  ?? Infinity, dpcMax:  p.dpc  ?? -Infinity,
          hdopSum: p.hdop ?? 0, hdopN: p.hdop != null ? 1 : 0, hdopMin: p.hdop ?? Infinity, hdopMax: p.hdop ?? -Infinity,
          accMSum: p.accM ?? 0, accMN: p.accM != null ? 1 : 0, accMMin: p.accM ?? Infinity, accMMax: p.accM ?? -Infinity,
          sessionIds: new Set(p._sid ? [p._sid] : []),
          pts: [p],
          spec: p.spectrum != null && Array.isArray(p.spectrum) ? [p.spectrum] : [],
        });
      }
    }

    function draw() {
      const mapZoom = map.getZoom();
      // Scale bin pixel coords from binZoom space → current mapZoom screen space.
      const scale  = Math.pow(2, mapZoom - binZoom);
      const visR   = HEX_R * scale;

      const size   = map.getSize();
      canvas.width  = size.x;
      canvas.height = size.y;
      const ctx    = canvas.getContext('2d');
      const W      = size.x, H = size.y;
      ctx.clearRect(0, 0, W, H);

      // NW corner of viewport in global pixel space at current mapZoom
      const origin = map.project(map.getBounds().getNorthWest(), mapZoom);
      const ox = origin.x, oy = origin.y;

      // Per-bin field average: uses only non-null data points for the active channel.
      // This prevents null values (e.g., missing speedKph in RC track imports) from
      // dragging all bin averages toward zero and producing uniform coloring.
      // Spectrum fields derive metrics from the bin's accumulated spectrum arrays.
      function binFieldAvg(b) {
        if (field === 'cps')   return b.cpsN  ? b.cpsSum  / b.cpsN  : 0;
        if (field === 'speed') return b.spdN  ? b.spdSum  / b.spdN  : 0;
        if (field === 'alt')   return b.altN  ? b.altSum  / b.altN  : 0;
        if (field === 'dpc')   return b.dpcN  ? b.dpcSum  / b.dpcN  : 0;
        if (field === 'hdop')  return b.hdopN ? b.hdopSum / b.hdopN : 0;
        if (field === 'accM')  return b.accMN ? b.accMSum / b.accMN : 0;
        // Spectrum-derived channels — computed per-bin from avg spectrum
        if (b.spec && b.spec.length > 0) {
          const avgSpec = b._avgSpec || (b._avgSpec = avgSpectrum(b.spec));
          if (avgSpec) {
            if      (field === 'totalcounts') return totalCounts(avgSpec);
            if      (field === 'peakchannel') return peakChannelIdx(avgSpec);
            if      (field === 'lowenergy')   return lowEnergySum(avgSpec);
            if      (field === 'highenergy')  return highEnergySum(avgSpec);
            if      (field === 'centroid')    return spectralCentroid(avgSpec);
            if      (field === 'entropy')     return spectralEntropy(avgSpec);
          }
        }
        // For spectrum fields with no data in this bin, return 0 (will render dim)
        if      (field === 'totalcounts') return 0;
        if      (field === 'peakchannel') return 0;
        if      (field === 'lowenergy')   return 0;
        if      (field === 'highenergy')  return 0;
        if      (field === 'centroid')    return 0;
        if      (field === 'entropy')     return 0;
        return b.uSvN ? b.uSvSum / b.uSvN : 0; // dose + session fallback
      }

      // maxAvg for session/fallback channel normalisation (relative heat gradient).
      let maxAvg = 1e-9;
      for (const b of bins.values()) {
        const cx = HEX_R * 1.5 * b.q * scale - ox;
        const cy = HEX_R * S3 * (b.r + b.q / 2) * scale - oy;
        if (cx > -visR * 2 && cx < W + visR * 2 &&
            cy > -visR * 2 && cy < H + visR * 2) {
          const fa = binFieldAvg(b);
          if (fa > maxAvg) maxAvg = fa;
        }
      }

      for (const b of bins.values()) {
        const cx = HEX_R * 1.5 * b.q * scale - ox;
        const cy = HEX_R * S3 * (b.r + b.q / 2) * scale - oy;
        if (cx < -visR * 2 || cx > W + visR * 2 ||
            cy < -visR * 2 || cy > H + visR * 2) continue;

        const fa = binFieldAvg(b);
        let color;
        if      (field === 'dpc')   color = dosePerCountColor(fa, ranges.dpcMin,  ranges.dpcMax);
        else if (field === 'cps')   color = cpsColor(fa,          ranges.cpsMin,  ranges.cpsMax);
        else if (field === 'speed') color = speedColor(fa,         ranges.spdMin,  ranges.spdMax);
        else if (field === 'alt')   color = altColor(fa,           ranges.altMin,  ranges.altMax);
        else if (field === 'hdop')  color = hdopColor(fa,          ranges.hdopMin, ranges.hdopMax);
        else if (field === 'accM')  color = accColor(fa,           ranges.accMin,  ranges.accMax);
        // Spectrogram color channels — use pre-computed per-bin lo/hi for normalization
        else if (field === 'totalcounts') color = totalCountsColor(fa,        ranges.specTotalLow,   ranges.specTotalHigh);
        else if (field === 'peakchannel') color = peakChannelColor(fa,        ranges.specPeakLow,    ranges.specPeakHigh);
        else if (field === 'lowenergy')   color = lowEnergyColor(fa,          ranges.specLowELow,    ranges.specLowEHigh);
        else if (field === 'highenergy')  color = highEnergyColor(fa,         ranges.specHighELow,   ranges.specHighEHigh);
        else if (field === 'centroid')    color = spectralCentroidColor(fa,   ranges.specCentLow,    ranges.specCentHigh);
        else if (field === 'entropy')     color = spectralEntropyColor(fa,    ranges.specEntLow,     ranges.specEntHigh);
        else if (field === 'dose' || field == null) color = doseColor(fa, ranges.doseMin, ranges.doseMax);
        else                        color = heatGradientColor(Math.min(1, fa / maxAvg));
        const DR    = visR * 0.94;  // 94% — tight gap between neighbours

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

        // Spider-radar overlay — only when enabled and hex is large enough
        // Precomputes avg spectrum per-bin via _avgSpec cache (lazy, computed once)
        if (radarEnabled && DR >= 30 && b.spec && b.spec.length > 0) {
          const avgSpec = b._avgSpec || (b._avgSpec = avgSpectrum(b.spec));
          if (avgSpec && avgSpec.length >= 3) {
            ctx.save();
            // Downsample spectrum for radar spokes — use up to 12 channels
            const nSpokes = Math.min(avgSpec.length, 12);
            const step = Math.max(1, Math.floor(avgSpec.length / nSpokes));
            const maxSpc = Math.max(...avgSpec, 1);
            // Concentric rings at 33%, 66%, 100% of DR radius
            for (let r = 1; r <= 3; r++) {
              ctx.beginPath();
              const rr = DR * 0.85 * (r / 3);
              for (let i = 0; i < nSpokes; i++) {
                // Start from top (-PI/2), go clockwise
                const angle = -Math.PI / 2 + (2 * Math.PI / nSpokes) * i - Math.PI / (nSpokes * 2);
                const rx = cx + rr * Math.cos(angle);
                const ry = cy + rr * Math.sin(angle);
                i === 0 ? ctx.moveTo(rx, ry) : ctx.lineTo(rx, ry);
              }
              ctx.closePath();
              ctx.globalAlpha = 0.15;
              ctx.fillStyle = '#fcf0e8'; // White rings
            }
            // Draw data spider shape
            ctx.beginPath();
            for (let i = 0; i < nSpokes; i++) {
              const chIdx = i * step;
              const val = avgSpec[chIdx] / maxSpc;
              const rr = DR * 0.88 * Math.max(0.1, val);
              const angle = -Math.PI / 2 + (2 * Math.PI / nSpokes) * i - Math.PI / (nSpokes * 2);
              // Start from top (-PI/2), go clockwise
              const rx = cx + rr * Math.cos(angle);
              const ry = cy + rr * Math.sin(angle);
              i === 0 ? ctx.moveTo(rx, ry) : ctx.lineTo(rx, ry);
            }
            ctx.closePath();
            ctx.globalAlpha = 0.5;
            ctx.fillStyle = '#4fffc2'; // Bright cyan-green data overlay
            ctx.fill();
            ctx.globalAlpha = 1;

            // Also draw the spokes lines for readability
            for (let i = 0; i < nSpokes; i++) {
              const angle = -Math.PI / 2 + (2 * Math.PI / nSpokes) * i - Math.PI / (nSpokes * 2);
              ctx.beginPath();
              ctx.moveTo(cx, cy);
              ctx.lineTo(cx + DR * 0.90 * Math.cos(angle), cy + DR * 0.90 * Math.sin(angle));
              ctx.globalAlpha = 0.1;
              ctx.strokeStyle = '#fff';
              ctx.lineWidth = 0.5;
              ctx.stroke();
            }
            ctx.restore();
          }
        }

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

    // Click handler: convert screen pixel → hex cube coord → bin lookup → flyout.
    function handleClick(e) {
      if (!onBinClick) return;
      const rect     = canvas.getBoundingClientRect();
      const ex       = e.clientX - rect.left;
      const ey       = e.clientY - rect.top;
      const mapZoom  = map.getZoom();
      const scale    = Math.pow(2, mapZoom - binZoom);
      const origin   = map.project(map.getBounds().getNorthWest(), mapZoom);
      // Convert screen pixel → global pixel at binZoom
      const gx = (ex + origin.x) / scale;
      const gy = (ey + origin.y) / scale;
      // Inverse flat-top hex cube coord
      const q_f = ( 2 / 3 * gx) / HEX_R;
      const r_f = (-1 / 3 * gx + S3 / 3 * gy) / HEX_R;
      const s_f = -q_f - r_f;
      let q = Math.round(q_f), r = Math.round(r_f), s = Math.round(s_f);
      const dq = Math.abs(q - q_f), dr = Math.abs(r - r_f), ds = Math.abs(s - s_f);
      if      (dq > dr && dq > ds) q = -r - s;
      else if (dr > ds)            r = -q - s;
      const b = bins.get(`${q},${r}`);
      if (!b) return;
      const fmt1 = v => v != null && isFinite(v) ? v.toFixed(1) : '—';
      const fmt3 = v => v != null && isFinite(v) ? v.toFixed(3) : '—';
      // Sort raw points by timestamp for temporal charts.
      // Downsample to ≤1000 pts so chart drawing stays fast even for heavily
      // visited bins (the visible chart resolution is ≤280 px wide).
      const sortedPts = [...b.pts].sort((x, y) => x.ts - y.ts);
      const maxChartPts = 1000;
      const step = sortedPts.length > maxChartPts ? Math.ceil(sortedPts.length / maxChartPts) : 1;
      const chartPts = step > 1 ? sortedPts.filter((_, i) => i % step === 0) : sortedPts;
      onBinClick({
        lat: b.latSum / b.count,
        lng: b.lngSum / b.count,
        count: b.count,
        sessionIds: [...b.sessionIds],
        uSv:  { avg: b.uSvN ? b.uSvSum / b.uSvN : null, min: b.uSvN ? b.uSvMin : null, max: b.uSvN ? b.uSvMax : null },
        cps:  { avg: b.cpsN ? b.cpsSum / b.cpsN : null, min: b.cpsN ? b.cpsMin : null, max: b.cpsN ? b.cpsMax : null },
        spd:  { avg: b.spdN ? b.spdSum / b.spdN : null, min: b.spdN ? b.spdMin : null, max: b.spdN ? b.spdMax : null },
        alt:  { avg: b.altN ? b.altSum / b.altN : null, min: b.altN ? b.altMin : null, max: b.altN ? b.altMax : null },
        dpc:  { avg: b.dpcN ? b.dpcSum / b.dpcN : null, min: b.dpcN ? b.dpcMin : null, max: b.dpcN ? b.dpcMax : null },
        spectrum: computeBinSpectrumStats(b.pts),
        points: chartPts,
      });
    }

    // Hover: resolve which bin the mouse is over, emit compact stats to onBinHover.
    function resolveBin(e) {
      const rect    = canvas.getBoundingClientRect();
      const ex      = e.clientX - rect.left;
      const ey      = e.clientY - rect.top;
      const mapZoom = map.getZoom();
      const scale   = Math.pow(2, mapZoom - binZoom);
      const origin  = map.project(map.getBounds().getNorthWest(), mapZoom);
      const gx = (ex + origin.x) / scale;
      const gy = (ey + origin.y) / scale;
      const q_f = ( 2 / 3 * gx) / HEX_R;
      const r_f = (-1 / 3 * gx + S3 / 3 * gy) / HEX_R;
      const s_f = -q_f - r_f;
      let q = Math.round(q_f), r = Math.round(r_f), s = Math.round(s_f);
      const dq = Math.abs(q - q_f), dr = Math.abs(r - r_f), ds = Math.abs(s - s_f);
      if      (dq > dr && dq > ds) q = -r - s;
      else if (dr > ds)            r = -q - s;
      return bins.get(`${q},${r}`) || null;
    }

    function handleMouseMove(e) {
      if (!onBinHover) return;
      const b = resolveBin(e);
      if (!b) { onBinHover(null); return; }
      onBinHover({
        clientX: e.clientX, clientY: e.clientY,
        count: b.count,
        sessionIds: [...b.sessionIds],
        uSv: { avg: b.uSvN ? b.uSvSum / b.uSvN : null },
        cps: { avg: b.cpsN ? b.cpsSum / b.cpsN : null },
        spd: { avg: b.spdN ? b.spdSum / b.spdN : null },
        alt: { avg: b.altN ? b.altSum / b.altN : null },
        dpc: { avg: b.dpcN ? b.dpcSum / b.dpcN : null },
      });
    }
    function handleMouseLeave() { if (onBinHover) onBinHover(null); }

    if (onBinClick)  canvas.addEventListener('click',      handleClick);
    if (onBinHover)  canvas.addEventListener('mousemove',  handleMouseMove);
    if (onBinHover)  canvas.addEventListener('mouseleave', handleMouseLeave);
    map.on('move zoom viewreset', draw);
    draw();

    return () => {
      map.off('move zoom viewreset', draw);
      if (onBinClick)  canvas.removeEventListener('click',      handleClick);
      if (onBinHover)  canvas.removeEventListener('mousemove',  handleMouseMove);
      if (onBinHover)  canvas.removeEventListener('mouseleave', handleMouseLeave);
      canvas.remove();
    };
  }, [traces, field, binZoom, map, onBinClick, onBinHover, ranges]); // eslint-disable-line

  return null;
}

// ============================================================
// HEX BIN HOVER TOOLTIP — floating label on mouseover.
// ============================================================

/** Floating tooltip shown when hovering over a hex bin on the map. */
function HexTooltip({ data }) {
  if (!data) return null;
  const { clientX, clientY, count, uSv, cps, spd, alt, dpc, sessionIds } = data;
  return (
    <div className="hex-tooltip" style={{ left: clientX + 14, top: clientY - 8 }}>
      <div className="hex-tt-count">
        {count.toLocaleString()} pts · {sessionIds.length} session{sessionIds.length !== 1 ? 's' : ''}
      </div>
      {uSv?.avg != null && <div className="hex-tt-row"><span>Dose rate</span><strong>{uSv.avg.toFixed(3)} µSv/h</strong></div>}
      {cps?.avg != null && <div className="hex-tt-row"><span>CPS</span><strong>{cps.avg.toFixed(1)}</strong></div>}
      {dpc?.avg != null && <div className="hex-tt-row"><span>Dose/Count</span><strong>{dpc.avg.toFixed(4)} µSv/c</strong></div>}
      {spd?.avg != null && <div className="hex-tt-row"><span>Speed</span><strong>{spd.avg.toFixed(1)} km/h</strong></div>}
      {alt?.avg != null && <div className="hex-tt-row"><span>Altitude</span><strong>{alt.avg.toFixed(0)} m</strong></div>}
    </div>
  );
}

// ============================================================
// HEX CHART MODAL — full-screen interactive chart expansion.
// Opened when user clicks ⤢ on a chart section in HexBinPanel.
// Supports interactive hover crosshair + data point tooltip.
// ============================================================

function HexChartModal({ data, onClose }) {
  const canvasRef = useRef(null);
  const savedRef  = useRef(null); // saved base ImageData for hover restore
  const [zoom, setZoom] = useState(1);

  // Reset zoom to 100% whenever a different chart is opened.
  useEffect(() => { setZoom(1); }, [data?.label]);

  useEffect(() => {
    if (!data || !canvasRef.current) return;
    const c = canvasRef.current;
    if (data.type === 'line') {
      _drawHexLine(c, data.pts, data.getVal, data.strokeColor);
    } else {
      _drawHexScatter(c, data.pts, data.getX, data.getY, data.xLabel, data.yLabel);
    }
    savedRef.current = c.getContext('2d').getImageData(0, 0, c.width, c.height);
  }, [data]);

  const handleMouseMove = useCallback((e) => {
    if (!data || !canvasRef.current || !savedRef.current) return;
    const c   = canvasRef.current;
    const ctx = c.getContext('2d');
    const rect = c.getBoundingClientRect();
    const mx = (e.clientX - rect.left) * (c.width  / rect.width);
    const my = (e.clientY - rect.top)  * (c.height / rect.height);

    ctx.putImageData(savedRef.current, 0, 0);

    const pts = data.pts;
    const W = c.width, H = c.height;

    if (data.type === 'line') {
      const PL = 44, PR = 6, PT = 8, PB = 22;
      const cw = W - PL - PR, ch = H - PT - PB;
      const valid = pts.filter(p => { const v = data.getVal(p); return v != null && isFinite(v); });
      if (valid.length < 2) return;
      const minT = valid[0].ts, maxT = valid[valid.length - 1].ts;
      const vals = valid.map(p => data.getVal(p));
      const minV = Math.min(...vals), maxV = Math.max(...vals);
      const rangeV = maxV - minV || 0.001, rangeT = maxT - minT || 1;

      const frac    = Math.max(0, Math.min(1, (mx - PL) / cw));
      const targetT = minT + frac * rangeT;
      let best = valid[0], bestDt = Infinity;
      for (const p of valid) { const dt = Math.abs(p.ts - targetT); if (dt < bestDt) { best = p; bestDt = dt; } }
      const pv = data.getVal(best);
      const px = PL + (best.ts - minT) / rangeT * cw;
      const py = PT + (1 - (pv - minV) / rangeV) * ch;

      ctx.save();
      ctx.strokeStyle = 'rgba(255,255,255,0.4)'; ctx.lineWidth = 1; ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(px, PT); ctx.lineTo(px, PT + ch); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(PL, py); ctx.lineTo(PL + cw, py); ctx.stroke();
      ctx.setLineDash([]);
      ctx.beginPath(); ctx.arc(px, py, 5, 0, Math.PI * 2);
      ctx.fillStyle = data.strokeColor; ctx.fill();
      ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.stroke();

      const d = new Date(best.ts);
      const ts = `${String(d.getMonth()+1).padStart(2,'0')}/${String(d.getDate()).padStart(2,'0')} ${String(d.getHours()).padStart(2,'0')}:${String(d.getMinutes()).padStart(2,'0')}`;
      const absV = Math.abs(pv);
      const vs = absV < 0.1 ? pv.toFixed(4) : absV < 10 ? pv.toFixed(3) : pv.toFixed(1);
      const tipText = `${ts}   ${vs}`;
      ctx.font = 'bold 12px monospace';
      const tw = ctx.measureText(tipText).width + 14, th = 22;
      let tx = px + 10; if (tx + tw > W - 4) tx = px - tw - 10;
      const ty = Math.max(PT + 2, py - th / 2);
      ctx.fillStyle = 'rgba(10,14,20,0.9)'; ctx.fillRect(tx - 2, ty - 2, tw + 4, th + 4);
      ctx.fillStyle = '#eee'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
      ctx.fillText(tipText, tx + 5, ty + th / 2);
      ctx.restore();

    } else {
      const PL = 44, PR = 6, PT = 8, PB = 24;
      const cw = W - PL - PR, ch = H - PT - PB;
      const valid = pts.filter(p => {
        const xv = data.getX(p), yv = data.getY(p);
        return xv != null && isFinite(xv) && yv != null && isFinite(yv);
      });
      if (!valid.length) return;
      const xs = valid.map(data.getX), ys = valid.map(data.getY);
      const minX = Math.min(...xs), maxX = Math.max(...xs);
      const minY = Math.min(...ys), maxY = Math.max(...ys);
      const rx = maxX - minX || 0.001, ry = maxY - minY || 0.001;

      let bestPt = valid[0], bestDist = Infinity;
      for (const p of valid) {
        const px = PL + (data.getX(p) - minX) / rx * cw;
        const py = PT + (1 - (data.getY(p) - minY) / ry) * ch;
        const dist = Math.hypot(px - mx, py - my);
        if (dist < bestDist) { bestPt = p; bestDist = dist; }
      }
      if (bestDist > 50) return;
      const bx = PL + (data.getX(bestPt) - minX) / rx * cw;
      const by = PT + (1 - (data.getY(bestPt) - minY) / ry) * ch;
      const xv = data.getX(bestPt), yv = data.getY(bestPt);

      ctx.save();
      ctx.beginPath(); ctx.arc(bx, by, 6, 0, Math.PI * 2);
      ctx.fillStyle = '#fff'; ctx.fill();
      ctx.strokeStyle = '#222'; ctx.lineWidth = 1; ctx.stroke();
      const tipText = `${data.xLabel}: ${_hexFmtVal(xv)}   ${data.yLabel}: ${_hexFmtVal(yv)}`;
      ctx.font = 'bold 12px monospace';
      const tw = ctx.measureText(tipText).width + 14, th = 22;
      let tx = bx + 10; if (tx + tw > W - 4) tx = bx - tw - 10;
      const ty = Math.max(PT + 2, by - th / 2);
      ctx.fillStyle = 'rgba(10,14,20,0.9)'; ctx.fillRect(tx - 2, ty - 2, tw + 4, th + 4);
      ctx.fillStyle = '#eee'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
      ctx.fillText(tipText, tx + 5, ty + th / 2);
      ctx.restore();
    }
  }, [data]);

  const handleMouseLeave = useCallback(() => {
    if (!canvasRef.current || !savedRef.current) return;
    canvasRef.current.getContext('2d').putImageData(savedRef.current, 0, 0);
  }, []);

  if (!data) return null;
  const isScatter = data.type === 'scatter';
  const baseW = isScatter ? 900 : 1100;
  const baseH = isScatter ? 560 : 380;
  const zBtnStyle = {
    background: 'rgba(255,255,255,0.1)', border: '1px solid rgba(255,255,255,0.18)',
    color: '#fff', borderRadius: 4, width: 26, height: 26, cursor: 'pointer',
    fontSize: 16, lineHeight: 1, display: 'inline-flex', alignItems: 'center',
    justifyContent: 'center', flexShrink: 0,
  };
  return ReactDOM.createPortal(
    <div className="hex-chart-modal-overlay" onClick={onClose}>
      <div className="hex-chart-modal" onClick={e => e.stopPropagation()}>
        <div className="hex-chart-modal-header">
          <span className="hex-chart-modal-title">{data.label}</span>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginLeft: 'auto' }}>
            <button style={zBtnStyle} title="Zoom out"
                    onClick={() => setZoom(z => Math.max(1, Math.round((z - 0.25) * 100) / 100))}>−</button>
            <span style={{ fontSize: 11, color: 'rgba(255,255,255,0.5)', minWidth: 34, textAlign: 'center' }}>
              {Math.round(zoom * 100)}%
            </span>
            <button style={zBtnStyle} title="Zoom in"
                    onClick={() => setZoom(z => Math.min(4, Math.round((z + 0.25) * 100) / 100))}>+</button>
            <button className="hex-chart-modal-close" onClick={onClose}>✕</button>
          </div>
        </div>
        <div className="hex-chart-modal-hint">Hover to inspect · click outside to close · ± to zoom</div>
        <div style={{ overflow: 'auto', borderRadius: 6 }}>
          <div style={{ position: 'relative', width: baseW * zoom, height: baseH * zoom, flexShrink: 0 }}>
            <canvas
              ref={canvasRef}
              className="hex-chart-modal-canvas"
              style={{
                position: 'absolute', top: 0, left: 0,
                transform: `scale(${zoom})`, transformOrigin: '0 0',
                maxWidth: 'none',
              }}
              width={baseW}
              height={baseH}
              onMouseMove={handleMouseMove}
              onMouseLeave={handleMouseLeave}
            />
          </div>
        </div>
      </div>
    </div>,
    document.body
  );
}

// ============================================================
// HEX BIN ANALYSIS PANEL — full-height right-side flyout.
// Shown when user clicks a hexagon in Hex map mode.
// Displays aggregated stat cards + temporal canvas charts:
//   • Dose Rate over time   • CPS over time
//   • Speed over time       • Altitude over time
//   • Dose ↔ CPS correlation scatter (with R² regression)
//   • Dose ↔ Altitude correlation scatter
// Raw points are passed from HexLayer; up to 1000 pts after
// downsampling so chart drawing stays fast on large bins.
// ============================================================

/** Format a Unix-ms timestamp as MM/DD HH:MM for chart axes. */
function _hexFmtTime(ts) {
  const d  = new Date(ts);
  const mo = String(d.getMonth() + 1).padStart(2, '0');
  const da = String(d.getDate()).padStart(2, '0');
  const hr = String(d.getHours()).padStart(2, '0');
  const mi = String(d.getMinutes()).padStart(2, '0');
  return `${mo}/${da} ${hr}:${mi}`;
}

/** Format a number for a compact axis label. */
function _hexFmtVal(v) {
  if (v == null || !isFinite(v)) return '—';
  if (Math.abs(v) >= 1000) return v.toFixed(0);
  if (Math.abs(v) >= 10)   return v.toFixed(1);
  return v.toFixed(3);
}

/**
 * Draw a temporal line chart with gradient fill on a canvas.
 * @param {HTMLCanvasElement} canvas
 * @param {Array}  pts        – points sorted by ts
 * @param {Function} getVal   – p => numeric value (null = skip)
 * @param {string} strokeColor – CSS colour string
 */
function _drawHexLine(canvas, pts, getVal, strokeColor) {
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(255,255,255,0.03)'; ctx.fillRect(0, 0, W, H);

  const valid = pts.filter(p => { const v = getVal(p); return v != null && isFinite(v); });
  if (valid.length < 2) {
    ctx.fillStyle = 'rgba(255,255,255,0.18)'; ctx.font = '10px sans-serif';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText('no data', W / 2, H / 2); return;
  }

  const vals = valid.map(getVal);
  const minV = Math.min(...vals), maxV = Math.max(...vals);
  const rangeV = maxV - minV || 0.001;
  const minT = valid[0].ts, maxT = valid[valid.length - 1].ts;
  const rangeT = maxT - minT || 1;

  const PL = 44, PR = 6, PT = 8, PB = 22;
  const cw = W - PL - PR, ch = H - PT - PB;

  // horizontal grid lines
  ctx.strokeStyle = 'rgba(255,255,255,0.07)'; ctx.lineWidth = 0.7;
  for (let i = 0; i <= 3; i++) {
    const y = PT + ch * (i / 3);
    ctx.beginPath(); ctx.moveTo(PL, y); ctx.lineTo(PL + cw, y); ctx.stroke();
  }

  // Y-axis labels
  ctx.fillStyle = 'rgba(255,255,255,0.38)'; ctx.font = '10px monospace';
  ctx.textAlign = 'right'; ctx.textBaseline = 'alphabetic';
  ctx.fillText(_hexFmtVal(maxV), PL - 3, PT + 9);
  ctx.fillText(_hexFmtVal((maxV + minV) / 2), PL - 3, PT + ch / 2 + 5);
  ctx.fillText(_hexFmtVal(minV), PL - 3, PT + ch + 6);

  // gradient fill under line
  const grad = ctx.createLinearGradient(0, PT, 0, PT + ch);
  grad.addColorStop(0, strokeColor + 'aa');
  grad.addColorStop(1, strokeColor + '08');

  const toXY = p => [
    PL + (p.ts - minT) / rangeT * cw,
    PT + (1 - (getVal(p) - minV) / rangeV) * ch,
  ];

  ctx.beginPath();
  let first = true;
  for (const p of valid) {
    const [x, y] = toXY(p); first ? ctx.moveTo(x, y) : ctx.lineTo(x, y); first = false;
  }
  const [lx] = toXY(valid[valid.length - 1]);
  ctx.lineTo(lx, PT + ch); ctx.lineTo(PL, PT + ch); ctx.closePath();
  ctx.fillStyle = grad; ctx.fill();

  // line
  ctx.beginPath(); first = true;
  for (const p of valid) {
    const [x, y] = toXY(p); first ? ctx.moveTo(x, y) : ctx.lineTo(x, y); first = false;
  }
  ctx.strokeStyle = strokeColor; ctx.lineWidth = 1.5; ctx.stroke();

  // X-axis time labels
  ctx.fillStyle = 'rgba(255,255,255,0.28)'; ctx.font = '10px monospace';
  ctx.textBaseline = 'alphabetic';
  ctx.textAlign = 'left';  ctx.fillText(_hexFmtTime(minT), PL,      H - 5);
  ctx.textAlign = 'right'; ctx.fillText(_hexFmtTime(maxT), PL + cw, H - 5);
}

/**
 * Draw a scatter plot with optional linear regression and R² label.
 * Dots are colored early→blue, late→amber by timestamp.
 */
function _drawHexScatter(canvas, pts, getX, getY, xLabel, yLabel) {
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(255,255,255,0.03)'; ctx.fillRect(0, 0, W, H);

  const valid = pts.filter(p => {
    const x = getX(p), y = getY(p);
    return x != null && isFinite(x) && y != null && isFinite(y);
  });
  if (valid.length < 3) {
    ctx.fillStyle = 'rgba(255,255,255,0.18)'; ctx.font = '10px sans-serif';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText('insufficient data', W / 2, H / 2); return;
  }

  const xs = valid.map(getX), ys = valid.map(getY);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const rx = maxX - minX || 0.001, ry = maxY - minY || 0.001;

  const PL = 44, PR = 6, PT = 8, PB = 24;
  const cw = W - PL - PR, ch = H - PT - PB;

  // grid
  ctx.strokeStyle = 'rgba(255,255,255,0.07)'; ctx.lineWidth = 0.7;
  [0, 0.5, 1].forEach(f => {
    const y = PT + ch * f; ctx.beginPath(); ctx.moveTo(PL, y); ctx.lineTo(PL + cw, y); ctx.stroke();
    const x = PL + cw * f; ctx.beginPath(); ctx.moveTo(x, PT); ctx.lineTo(x, PT + ch); ctx.stroke();
  });

  // dots colored by time (early=blue, late=amber)
  const minT = Math.min(...valid.map(p => p.ts)), maxT = Math.max(...valid.map(p => p.ts));
  const rangeT = maxT - minT || 1;
  for (const p of valid) {
    const px = PL + (getX(p) - minX) / rx * cw;
    const py = PT + (1 - (getY(p) - minY) / ry) * ch;
    const t  = (p.ts - minT) / rangeT;
    ctx.beginPath(); ctx.arc(px, py, 3, 0, Math.PI * 2);
    ctx.fillStyle = `rgba(${Math.round(220*t)},${Math.round(160+60*(1-t))},${Math.round(230*(1-t))},0.65)`;
    ctx.fill();
  }

  // linear regression
  const n = valid.length;
  const xm = xs.reduce((a, v) => a + v, 0) / n;
  const ym = ys.reduce((a, v) => a + v, 0) / n;
  const ss = xs.reduce((a, v) => a + (v - xm) ** 2, 0);
  if (ss > 1e-12) {
    const slope = xs.reduce((a, v, i) => a + (v - xm) * (ys[i] - ym), 0) / ss;
    const icpt  = ym - slope * xm;
    const y1 = slope * minX + icpt, y2 = slope * maxX + icpt;
    ctx.beginPath();
    ctx.moveTo(PL,      PT + (1 - (y1 - minY) / ry) * ch);
    ctx.lineTo(PL + cw, PT + (1 - (y2 - minY) / ry) * ch);
    ctx.strokeStyle = 'rgba(255,210,60,0.75)'; ctx.lineWidth = 1.5;
    ctx.setLineDash([4, 3]); ctx.stroke(); ctx.setLineDash([]);

    // R² label
    const yPred = xs.map(x => slope * x + icpt);
    const sst = ys.reduce((a, v) => a + (v - ym) ** 2, 0);
    const sse = ys.reduce((a, v, i) => a + (v - yPred[i]) ** 2, 0);
    const r2  = sst > 0 ? Math.max(0, 1 - sse / sst) : 0;
    ctx.fillStyle = 'rgba(255,210,60,0.9)'; ctx.font = 'bold 11px monospace';
    ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
    ctx.fillText(`R² = ${r2.toFixed(3)}`, PL + 4, PT + 14);
  }

  // axis labels
  ctx.fillStyle = 'rgba(255,255,255,0.35)'; ctx.font = '10px monospace';
  ctx.textBaseline = 'alphabetic';
  ctx.textAlign = 'left';  ctx.fillText(_hexFmtVal(minX), PL,      H - 5);
  ctx.textAlign = 'right'; ctx.fillText(_hexFmtVal(maxX), PL + cw, H - 5);
  ctx.textAlign = 'right'; ctx.fillText(_hexFmtVal(maxY), PL - 3, PT + 9);
                           ctx.fillText(_hexFmtVal(minY), PL - 3, PT + ch + 6);
  // axis titles
  ctx.fillStyle = 'rgba(255,255,255,0.22)'; ctx.font = '10px monospace';
  ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
  ctx.fillText(xLabel, PL + cw / 2, H - 3);
  ctx.save(); ctx.translate(10, PT + ch / 2); ctx.rotate(-Math.PI / 2);
  ctx.fillText(yLabel, 0, 0); ctx.restore();
}

function _drawHexSpectrum(canvas, spectrum) {
  if (!canvas || !spectrum || spectrum.length === 0) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(255,255,255,0.03)'; ctx.fillRect(0, 0, W, H);

  const PL = 4, PR = 4, PT = 4, PB = 14;
  const cw = W - PL - PR, ch = H - PT - PB;
  const n = spectrum.length;
  const maxVal = Math.max(...spectrum, 1);

  // Multi-layer gamma spectrum: filled area + smooth line + energy color gradient
  // Energy increases left to right (channel 0 = low E, channel N-1 = high E)

  // Filled area under curve with subtle gradient
  const gradFill = ctx.createLinearGradient(0, PT, 0, PT + ch);
  gradFill.addColorStop(0, 'rgba(100,200,255,0.35)');
  gradFill.addColorStop(0.5, 'rgba(0,230,118,0.20)');
  gradFill.addColorStop(1, 'rgba(255,60,60,0.35)');

  ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const x = PL + (i / (n - 1)) * cw;
    const y = PT + ch - (spectrum[i] / maxVal) * ch;
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.lineTo(PL + cw, PT + ch);
  ctx.lineTo(PL, PT + ch);
  ctx.closePath();
  ctx.fillStyle = gradFill;
  ctx.fill();

  // Line on top with energy color (blue→green→red)
  for (let i = 1; i < n; i++) {
    const t0 = (i - 1) / (n - 1), t1 = i / (n - 1);
    const x0 = PL + t0 * cw, y0 = PT + ch - (spectrum[i - 1] / maxVal) * ch;
    const x1 = PL + t1 * cw, y1 = PT + ch - (spectrum[i] / maxVal) * ch;
    // Color based on energy position (low=blue, high=red via green transition)
    const hue = 240 * (1 - (t0 + t1) / 2); // 240(blue) → 120(green) → 0(red)
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.strokeStyle = `hsla(${hue},80%,55%,0.8)`;
    ctx.lineWidth = 1.2;
    ctx.stroke();
  }

  // X-axis labels as approximate keV (RC-110: ~0-3000 keV range)
  ctx.fillStyle = 'rgba(255,255,255,0.35)';
  ctx.font = '9px monospace';
  ctx.textAlign = 'left';   ctx.fillText('0 keV', PL, H - 2);
  ctx.textAlign = 'center'; ctx.fillText('~1.5 MeV', PL + cw / 2, H - 2);
  ctx.textAlign = 'right';  ctx.fillText('~3 MeV', PL + cw, H - 2);
}

// ============================================================
// SPECTROGRAM MODAL — full gamma spectroscopy analysis view
// Triggered by clicking ⤢ on the Spectrum section in HexBinPanel.
// Shows proper MCA-style spectrum chart with energy axis, peak
// annotations, interactive hover crosshair, and educational context.
// ============================================================

function SpectrogramModal({ avgSpectrum, numReadings, onClose }) {
  const canvasRef = useRef(null);
  const savedRef  = useRef(null);
  const [zoom, setZoom] = useState(1);

  useEffect(() => { setZoom(1); }, [avgSpectrum]);

  // RC-110 CsI(Tl) approximate energy calibration.
  // Channel index maps to energy; typical range is ~0-3 MeV.
  // We estimate keV = channel * (MAX_KEV / num_channels).
  const NUM_CH = avgSpectrum.length;
  const MAX_KEV = 3000; // ~3 MeV upper range for CsI(Tl)
  const keVForCh = (ch) => (ch / (NUM_CH - 1)) * MAX_KEV;

  // Known natural background radiation peaks to annotate
  const KNOWN_PEAKS = [
    { name: 'Pb-210/Bi-214', energy: 465, halfWidth: 30 },   // ~465 keV (thorium chain)
    { name: 'K-40',         energy: 1460, halfWidth: 80 },   // Potassium-40 ~1.46 MeV (most common background)
    { name: 'Bi-214',       energy: 2386, halfWidth: 120 }, // Uranium chain ~2.39 MeV
    { name: 'Tl-208',       energy: 2614, halfWidth: 120 }, // Thorium series
  ];

  useEffect(() => {
    if (!avgSpectrum || !canvasRef.current) return;
    _drawSpectrogramModal(canvasRef.current, avgSpectrum, NUM_CH, MAX_KEV, keVForCh, KNOWN_PEAKS);
    savedRef.current = canvasRef.current.getContext('2d').getImageData(0, 0, canvasRef.current.width, canvasRef.current.height);
  }, [avgSpectrum]);

  const handleMouseMove = useCallback((e) => {
    if (!canvasRef.current || !savedRef.current || !avgSpectrum) return;
    const c   = canvasRef.current;
    const ctx = c.getContext('2d');
    const rect = c.getBoundingClientRect();
    const mx = (e.clientX - rect.left) * (c.width / rect.width);
    const my = (e.clientY - rect.top)  * (c.height / rect.height);

    ctx.putImageData(savedRef.current, 0, 0);

    const PL = 56, PR = 12, PT = 14, PB = 32;
    const cw = c.width - PL - PR, ch = c.height - PT - PB;

    const frac = Math.max(0, Math.min(1, (mx - PL) / cw));
    const chIdx = Math.round(frac * (NUM_CH - 1));
    const clampedCh = Math.max(0, Math.min(NUM_CH - 1, chIdx));
    const px = PL + (clampedCh / (NUM_CH - 1)) * cw;
    const val = avgSpectrum[clampedCh] || 0;
    const maxVal = Math.max(...avgSpectrum, 1);
    const py = PT + ch - (val / maxVal) * ch;

    // Crosshair lines
    ctx.save();
    ctx.strokeStyle = 'rgba(255,255,255,0.3)';
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    ctx.beginPath(); ctx.moveTo(px, PT); ctx.lineTo(px, PT + ch); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(PL, py); ctx.lineTo(PL + cw, py); ctx.stroke();
    ctx.setLineDash([]);

    // Dot on curve
    ctx.beginPath(); ctx.arc(px, py, 5, 0, Math.PI * 2);
    const hue = 240 * (1 - clampedCh / (NUM_CH - 1));
    ctx.fillStyle = `hsl(${hue},80%,55%)`;
    ctx.fill();
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 1.5;
    ctx.stroke();

    // Tooltip
    const energyKeV = keVForCh(clampedCh);
    const energyStr = energyKeV >= 1000 ? `${(energyKeV / 1000).toFixed(2)} MeV` : `${energyKeV.toFixed(0)} keV`;
    const tipText = `ch ${clampedCh} · ${energyStr} · ${Math.round(val)} counts`;

    // Check for nearby known peaks
    let peakLabel = null;
    for (const pk of KNOWN_PEAKS) {
      if (Math.abs(energyKeV - pk.energy) < pk.halfWidth) {
        peakLabel = `⚠ ${pk.name} (${(pk.energy / 1000).toFixed(2)} MeV)`;
        break;
      }
    }

    ctx.font = 'bold 12px monospace';
    const lines = peakLabel ? [tipText, peakLabel] : [tipText];
    const tw = Math.max(...lines.map(l => ctx.measureText(l).width)) + 14;
    const th = 20;
    let tx = px + 12;
    if (tx + tw > c.width - 4) tx = px - tw - 12;
    const ty = Math.max(PT + 2, py - th * lines.length - 4);

    ctx.fillStyle = 'rgba(10,14,20,0.92)';
    ctx.fillRect(tx - 2, ty - 2, tw + 4, th * lines.length + 8);
    ctx.strokeStyle = 'rgba(255,255,255,0.15)';
    ctx.lineWidth = 1;
    ctx.strokeRect(tx - 2, ty - 2, tw + 4, th * lines.length + 8);

    ctx.fillStyle = peakLabel ? '#fff' : '#eee';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    for (let li = 0; li < lines.length; li++) {
      ctx.fillStyle = li === 1 ? 'rgba(255,200,50,0.9)' : '#eee';
      ctx.fillText(lines[li], tx + 5, ty + 4 + li * th);
    }
    ctx.restore();
  }, [avgSpectrum, NUM_CH, MAX_KEV, keVForCh, KNOWN_PEAKS]);

  const handleMouseLeave = useCallback(() => {
    if (!canvasRef.current || !savedRef.current) return;
    canvasRef.current.getContext('2d').putImageData(savedRef.current, 0, 0);
  }, []);

  if (!avgSpectrum) return null;

  const baseW = 1100;
  const baseH = 480;
  const zBtnStyle = {
    background: 'rgba(255,255,255,0.1)', border: '1px solid rgba(255,255,255,0.18)',
    color: '#fff', borderRadius: 4, width: 26, height: 26, cursor: 'pointer',
    fontSize: 16, lineHeight: 1, display: 'inline-flex', alignItems: 'center',
    justifyContent: 'center', flexShrink: 0,
  };

  // Compute stats for the info panel
  const maxVal = Math.max(...avgSpectrum, 1);
  const peakChIdx = peakChannelIdx(avgSpectrum);
  const totalAvg = totalCounts(avgSpectrum);

  return ReactDOM.createPortal(
    <div className="hex-chart-modal-overlay" onClick={onClose}>
      <div className="hex-chart-modal" onClick={e => e.stopPropagation()} style={{ maxWidth: '95vw' }}>
        <div className="hex-chart-modal-header">
          <span className="hex-chart-modal-title">Gamma Spectrum — CsI(Tl) Scintillator</span>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginLeft: 'auto' }}>
            <button style={zBtnStyle} title="Zoom out"
                    onClick={() => setZoom(z => Math.max(1, Math.round((z - 0.25) * 100) / 100))}>−</button>
            <span style={{ fontSize: 11, color: 'rgba(255,255,255,0.5)', minWidth: 34, textAlign: 'center' }}>
              {Math.round(zoom * 100)}%
            </span>
            <button style={zBtnStyle} title="Zoom in"
                    onClick={() => setZoom(z => Math.min(4, Math.round((z + 0.25) * 100) / 100))}>+</button>
            <button className="hex-chart-modal-close" onClick={onClose}>✕</button>
          </div>
        </div>
        <div className="hex-chart-modal-hint">Hover to inspect channel · color = photon energy (blue→red) · ± to zoom</div>

        {/* Info summary panel */}
        <div style={{
          display: 'flex', gap: 12, padding: '8px 0', flexWrap: 'wrap',
          fontSize: 12, color: 'rgba(255,255,255,0.6)'
        }}>
          <div style={{ background: 'rgba(255,255,255,0.06)', borderRadius: 6, padding: '6px 12px' }}>
            Channels: <strong style={{ color: '#fff' }}>{NUM_CH}</strong>
          </div>
          <div style={{ background: 'rgba(255,255,255,0.06)', borderRadius: 6, padding: '6px 12px' }}>
            Readings averaged: <strong style={{ color: '#fff' }}>{numReadings}</strong>
          </div>
          <div style={{ background: 'rgba(255,255,255,0.06)', borderRadius: 6, padding: '6px 12px' }}>
            Avg total counts: <strong style={{ color: '#fff' }}>{totalAvg.toFixed(0)}</strong>
          </div>
          <div style={{ background: 'rgba(255,255,255,0.06)', borderRadius: 6, padding: '6px 12px' }}>
            Peak channel: <strong style={{ color: '#fff' }}>{peakChIdx} (~{keVForCh(peakChIdx).toFixed(0)} keV)</strong>
          </div>
          <div style={{ background: 'rgba(255,255,255,0.06)', borderRadius: 6, padding: '6px 12px' }}>
            Energy range: <strong style={{ color: '#fff' }}>0 — ~{(MAX_KEV / 1000).toFixed(0)} MeV</strong>
          </div>
        </div>

        <div style={{ overflow: 'auto', borderRadius: 6 }}>
          <div style={{ position: 'relative', width: baseW * zoom, height: baseH * zoom, flexShrink: 0 }}>
            <canvas
              ref={canvasRef}
              className="hex-chart-modal-canvas"
              style={{
                position: 'absolute', top: 0, left: 0,
                transform: `scale(${zoom})`, transformOrigin: '0 0',
                maxWidth: 'none',
              }}
              width={baseW}
              height={baseH}
              onMouseMove={handleMouseMove}
              onMouseLeave={handleMouseLeave}
            />
          </div>
        </div>

        {/* Educational context */}
        <div style={{
          marginTop: 8, padding: '10px 14px', fontSize: 11, lineHeight: 1.5,
          color: 'rgba(255,255,255,0.4)', background: 'rgba(255,255,255,0.03)',
          borderRadius: 6
        }}>
          <strong style={{ color: 'rgba(255,255,255,0.6)' }}>Gamma spectroscopy:</strong>{' '}
          The RC-110 uses a CsI(Tl) scintillator crystal coupled to a photomultiplier tube.
          When ionizing radiation hits the crystal, it produces flash lights proportional to photon energy.
          The multi-channel analyzer sorts these pulses by height into {NUM_CH} energy bins (channels).
          <br />X-axis = gamma photon energy (blue = low keV → red = high MeV) · Y-axis = count rate.
          Sharp spikes are photopeaks (full-energy deposition); the smooth baseline is Compton scattering continuum.
          Natural background radiation typically shows K-40 at ~1.46 MeV and uranium/thorium decay chain lines.
        </div>
      </div>
    </div>,
    document.body
  );
}

/** Draw the full spectrogram modal canvas with MCA-style spectrum chart. */
function _drawSpectrogramModal(canvas, avgSpectrum, NUM_CH, MAX_KEV, keVForCh, KNOWN_PEAKS) {
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = 'rgba(13,17,23,0.95)'; ctx.fillRect(0, 0, W, H);

  const PL = 56, PR = 12, PT = 14, PB = 32;
  const cw = W - PL - PR, ch = H - PT - PB;
  const maxVal = Math.max(...avgSpectrum, 1);

  // Grid lines
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 0.7;
  for (let i = 0; i <= 4; i++) {
    const y = PT + ch * (i / 4);
    ctx.beginPath(); ctx.moveTo(PL, y); ctx.lineTo(PL + cw, y); ctx.stroke();
  }

  // Y-axis labels (counts)
  ctx.fillStyle = 'rgba(255,255,255,0.45)';
  ctx.font = '11px monospace';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'alphabetic';
  for (let i = 0; i <= 4; i++) {
    const v = Math.round(maxVal * (1 - i / 4));
    const y = PT + ch * (i / 4);
    ctx.fillText(String(v), PL - 6, y + 4);
  }

  // X-axis labels (energy in keV/MeV)
  ctx.textBaseline = 'top';
  ctx.textAlign = 'center';
  const xTicks = [0, 511, 1000, 1460, 2000, MAX_KEV];
  for (const e of xTicks) {
    const frac = Math.max(0, Math.min(1, e / MAX_KEV));
    const x = PL + frac * cw;
    const label = e >= 1000 ? `${(e / 1000).toFixed(1)} MeV` : `${e} keV`;
    ctx.fillText(label, x, PT + ch + 6);
  }

  // Filled area under curve with energy gradient
  const gradFill = ctx.createLinearGradient(PL, PT, PL + cw, PT);
  gradFill.addColorStop(0, 'rgba(60,120,255,0.30)');   // Blue (low energy)
  gradFill.addColorStop(0.35, 'rgba(0,200,120,0.25)'); // Green
  gradFill.addColorStop(0.7, 'rgba(255,160,40,0.25)'); // Orange
  gradFill.addColorStop(1, 'rgba(255,50,50,0.30)');    // Red (high energy)

  ctx.beginPath();
  for (let i = 0; i < NUM_CH; i++) {
    const x = PL + (i / (NUM_CH - 1)) * cw;
    const y = PT + ch - (avgSpectrum[i] / maxVal) * ch;
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.lineTo(PL + cw, PT + ch);
  ctx.lineTo(PL, PT + ch);
  ctx.closePath();
  ctx.fillStyle = gradFill;
  ctx.fill();

  // Line segments colored by energy
  for (let i = 1; i < NUM_CH; i++) {
    const t0 = (i - 1) / (NUM_CH - 1), t1 = i / (NUM_CH - 1);
    const x0 = PL + t0 * cw, y0 = PT + ch - (avgSpectrum[i - 1] / maxVal) * ch;
    const x1 = PL + t1 * cw, y1 = PT + ch - (avgSpectrum[i] / maxVal) * ch;
    const hue = 240 * (1 - (t0 + t1) / 2);
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.strokeStyle = `hsla(${hue},75%,55%,0.85)`;
    ctx.lineWidth = 1.8;
    ctx.lineJoin = 'round';
    ctx.stroke();
  }

  // Annotate known background radiation peaks (vertical dashed lines)
  for (const pk of KNOWN_PEAKS) {
    const frac = Math.max(0, Math.min(1, pk.energy / MAX_KEV));
    const x = PL + frac * cw;
    ctx.save();
    ctx.strokeStyle = 'rgba(255,200,50,0.35)';
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 4]);
    ctx.beginPath(); ctx.moveTo(x, PT); ctx.lineTo(x, PT + ch); ctx.stroke();
    ctx.setLineDash([]);

    // Label at top
    ctx.fillStyle = 'rgba(255,200,50,0.7)';
    ctx.font = '10px monospace';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'bottom';
    const label = pk.name.length > 14 ? pk.name.slice(0, 12) + '..' : pk.name;
    ctx.fillText(`▼ ${label}`, x, PT - 2);
    ctx.restore();
  }

  // Axis titles
  ctx.save();
  ctx.fillStyle = 'rgba(255,255,255,0.35)';
  ctx.font = '11px sans-serif';
  // X-axis title
  ctx.textAlign = 'center';
  ctx.fillText('Photon Energy (CsI(Tl) Scintillator Spectrum)', PL + cw / 2, H - 4);
  // Y-axis title (rotated)
  ctx.save();
  ctx.translate(12, PT + ch / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText('Count Rate', 0, 0);
  ctx.restore();
  ctx.restore();

  // Detector label at top-right
  ctx.fillStyle = 'rgba(255,255,255,0.18)';
  ctx.font = '10px monospace';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'top';
  ctx.fillText(`RC-110 CsI(Tl) · ${NUM_CH} ch · ~0—${(MAX_KEV / 1000).toFixed(0)} MeV`, PL + cw, PT + 1);
}

// StatCard and Section MUST live at module level — not inside the
// HexBinPanel function body.  If they were inner functions, every re-render
// of HexBinPanel (triggered by an inline onClose arrow changing reference)
// would create new function-object identities for StatCard and Section.
// React uses reference equality to identify component types, so a new
// reference looks like a brand-new component type: React unmounts the old
// subtree and mounts a fresh one, destroying canvas DOM nodes and erasing
// all painted chart content (charts appear black).  Stable module-level
// definitions eliminate this unmount/remount cycle entirely.

const HexStatCard = ({ label, val }) => (
  <div className="hex-panel-stat">
    <span>{label}</span>
    <strong>{val ?? '\u2014'}</strong>
  </div>
);

const Section = ({ label, children, onExpand }) => (
  <div className="hex-panel-section"
       onClick={onExpand}
       style={onExpand ? { cursor: 'pointer' } : undefined}>
    <div className="hex-panel-section-label">
      {label}
      {onExpand && (
        <button className="hex-section-expand"
                onClick={e => { e.stopPropagation(); onExpand(); }}
                title="Expand chart">⤢</button>
      )}
    </div>
    {children}
  </div>
);

/** Full-height right-side panel for hex bin analytics. */
function HexBinPanel({ data, onClose }) {
  const doseRef    = useRef(null), cpsRef   = useRef(null);
  const spdRef     = useRef(null), altRef   = useRef(null);
  const corrRef    = useRef(null), altCorrRef = useRef(null);
  const dpcRef     = useRef(null), specRef  = useRef(null);
  const [chartModal, setChartModal] = useState(null);
  const [specroModalOpen, setSpecroModalOpen] = useState(false);

  useEffect(() => {
    if (!data?.points?.length) return;
    const pts = data.points;            // already sorted by ts, downsampled
    _drawHexLine(doseRef.current,  pts, p => p.uSv, '#00E676');
    _drawHexLine(cpsRef.current,   pts, p => p.cps, '#4FC3F7');
    if (pts.some(p => p.dpc != null)) _drawHexLine(dpcRef.current,  pts, p => p.dpc, '#E040FB');
    if (pts.some(p => p.spd != null)) _drawHexLine(spdRef.current, pts, p => p.spd, '#FFB74D');
    if (pts.some(p => p.alt != null)) _drawHexLine(altRef.current, pts, p => p.alt, '#CE93D8');
    if (data.uSv.avg != null && data.cps.avg != null)
      _drawHexScatter(corrRef.current,    pts, p => p.cps, p => p.uSv, 'CPS', 'µSv/h');
    if (data.uSv.avg != null && data.alt.avg != null)
      _drawHexScatter(altCorrRef.current, pts, p => p.alt, p => p.uSv, 'Alt (m)', 'µSv/h');
    // Draw gamma spectrum mini-preview when spectrum data available
    if (data.spectrum && data.spectrum.avgSpectrum)
      _drawHexSpectrum(specRef.current, data.spectrum.avgSpectrum);
  }, [data]);

  if (!data) return null;
  const { lat, lng, count, sessionIds, uSv, cps, spd, alt, dpc, spectrum, points } = data;

  // Date range header from raw points
  let dateRange = null;
  if (points?.length) {
    const fmtDate = ts => {
      const d = new Date(ts);
      return `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,'0')}-${String(d.getDate()).padStart(2,'0')}`;
    };
    const d0 = fmtDate(points[0].ts), d1 = fmtDate(points[points.length - 1].ts);
    dateRange = d0 === d1 ? d0 : `${d0} → ${d1}`;
  }

  return (
    <>
    <div className="hex-panel">
      <div className="hex-panel-header">
        <div>
          <div className="hex-panel-title">⬡ Hex Bin Analysis</div>
          <div className="hex-panel-loc">{lat.toFixed(5)}, {lng.toFixed(5)}</div>
        </div>
        <button className="hex-panel-close" onClick={onClose}>✕</button>
      </div>

      <div className="hex-panel-body">

        {/* ── Summary chips ── */}
        <div className="hex-panel-chips">
          <div className="hex-panel-chip">
            <span>Samples</span><strong>{count.toLocaleString()}</strong>
          </div>
          <div className="hex-panel-chip">
            <span>Sessions</span><strong>{sessionIds.length}</strong>
          </div>
          {dateRange && (
            <div className="hex-panel-chip hex-panel-chip-wide">
              <span>Period</span><strong>{dateRange}</strong>
            </div>
          )}
        </div>

        {/* ── Dose Rate ── */}
        {uSv.avg != null && (<>
          <Section label="Dose Rate (µSv/h)">
            <div className="hex-panel-stat-row">
              <HexStatCard label="avg" val={uSv.avg.toFixed(3)} />
              <HexStatCard label="min" val={uSv.min.toFixed(3)} />
              <HexStatCard label="max" val={uSv.max.toFixed(3)} />
            </div>
          </Section>
          <Section label="Dose Rate over Time"
            onExpand={() => setChartModal({ type: 'line', label: 'Dose Rate over Time', pts: points, getVal: p => p.uSv, strokeColor: '#00E676' })}>
            <canvas ref={doseRef} className="hex-chart" width={560} height={160} />
          </Section>
        </>)}

        {/* ── CPS ── */}
        {cps.avg != null && (<>
          <Section label="CPS">
            <div className="hex-panel-stat-row">
              <HexStatCard label="avg" val={cps.avg.toFixed(1)} />
              <HexStatCard label="min" val={cps.min.toFixed(1)} />
              <HexStatCard label="max" val={cps.max.toFixed(1)} />
            </div>
          </Section>
          <Section label="CPS over Time"
            onExpand={() => setChartModal({ type: 'line', label: 'CPS over Time', pts: points, getVal: p => p.cps, strokeColor: '#4FC3F7' })}>
            <canvas ref={cpsRef} className="hex-chart" width={560} height={140} />
          </Section>
        </>)}

        {/* ── Dose per Count ── */}
        {dpc?.avg != null && (<>
          <Section label="Dose/Count (µSv/c)">
            <div className="hex-panel-stat-row">
              <HexStatCard label="avg" val={dpc.avg.toFixed(4)} />
              <HexStatCard label="min" val={dpc.min.toFixed(4)} />
              <HexStatCard label="max" val={dpc.max.toFixed(4)} />
            </div>
          </Section>
          <Section label="Dose/Count over Time"
            onExpand={() => setChartModal({ type: 'line', label: 'Dose/Count over Time', pts: points, getVal: p => p.dpc, strokeColor: '#E040FB' })}>
            <canvas ref={dpcRef} className="hex-chart" width={560} height={140} />
          </Section>
        </>) }

        {/* ── Dose ↔ CPS correlation ── */}
        {uSv.avg != null && cps.avg != null && (
          <Section label="Dose ↔ CPS Correlation"
            onExpand={() => setChartModal({ type: 'scatter', label: 'Dose ↔ CPS Correlation', pts: points, getX: p => p.cps, getY: p => p.uSv, xLabel: 'CPS', yLabel: 'µSv/h' })}>
            <canvas ref={corrRef} className="hex-scatter" width={560} height={260} />
            <div className="hex-panel-chart-note">
              early → blue · late → amber · dashed line = linear fit
            </div>
          </Section>
        )}

        {/* ── Speed ── */}
        {spd.avg != null && (<>
          <Section label="Speed (km/h)">
            <div className="hex-panel-stat-row">
              <HexStatCard label="avg" val={spd.avg.toFixed(1)} />
              <HexStatCard label="min" val={spd.min.toFixed(1)} />
              <HexStatCard label="max" val={spd.max.toFixed(1)} />
            </div>
          </Section>
          <Section label="Speed over Time"
            onExpand={() => setChartModal({ type: 'line', label: 'Speed over Time', pts: points, getVal: p => p.spd, strokeColor: '#FFB74D' })}>
            <canvas ref={spdRef} className="hex-chart" width={560} height={120} />
          </Section>
        </>)}

        {/* ── Altitude ── */}
        {alt.avg != null && (<>
          <Section label="Altitude (m)">
            <div className="hex-panel-stat-row">
              <HexStatCard label="avg" val={alt.avg.toFixed(0)} />
              <HexStatCard label="min" val={alt.min.toFixed(0)} />
              <HexStatCard label="max" val={alt.max.toFixed(0)} />
            </div>
          </Section>
          <Section label="Altitude over Time"
            onExpand={() => setChartModal({ type: 'line', label: 'Altitude over Time', pts: points, getVal: p => p.alt, strokeColor: '#CE93D8' })}>
            <canvas ref={altRef} className="hex-chart" width={560} height={120} />
          </Section>
          {uSv.avg != null && (
            <Section label="Dose ↔ Altitude Correlation"
              onExpand={() => setChartModal({ type: 'scatter', label: 'Dose ↔ Altitude Correlation', pts: points, getX: p => p.alt, getY: p => p.uSv, xLabel: 'Alt (m)', yLabel: 'µSv/h' })}>
              <canvas ref={altCorrRef} className="hex-scatter" width={560} height={220} />
            </Section>
          )}
        </>)}

        {/* ── Spectrum (gamma energy channels) ── */}
        {spectrum && spectrum.avgSpectrum && spectrum.avgSpectrum.length > 0 ? (
          <Section label={`Gamma Spectrum (${spectrum.channels} ch · ${spectrum.count} readings)`}
            onExpand={() => setSpecroModalOpen(true)}>
            <div className="hex-panel-stat-row">
              <HexStatCard label="peak ch" val={String(spectrum.peakChannel)} />
              <HexStatCard label="avg counts" val={`${spectrum.totalAverage.toFixed(0)}`} />
              <HexStatCard label="channels" val={String(spectrum.channels)} />
            </div>
            <canvas ref={specRef} className="hex-chart" width={560} height={120} />
            <div className="hex-panel-chart-note">
              line = counts per energy bin · blue→red = low→high keV · click ⤢ for full analysis
            </div>
          </Section>
        ) : (
          spectrum && !spectrum.avgSpectrum && (
            <Section label="Gamma Spectrum">
              <div style={{ color: 'rgba(255,255,255,0.35)', fontSize: '12px' }}>
                No spectrum data in this hex bin
              </div>
            </Section>
          )
        )}

        {/* ── Session IDs ── */}
        {sessionIds.length > 0 && (
          <Section label="Session IDs">
            <div className="hex-panel-sessions">{sessionIds.join(' · ')}</div>
          </Section>
        )}
      </div>
    </div>
    {chartModal && <HexChartModal data={chartModal} onClose={() => setChartModal(null)} />}
    {specroModalOpen && spectrum?.avgSpectrum && (
      <SpectrogramModal
        avgSpectrum={spectrum.avgSpectrum}
        numReadings={spectrum.count}
        onClose={() => setSpecroModalOpen(false)}
      />
    )}
    </>
  );
}

// ============================================================
// CANVAS TRACK LAYER
// Replaces per-segment <Polyline> React elements. All track
// segments for all sessions are drawn onto one raw <canvas>
// that overlays the Leaflet map.  Zero DOM nodes per point.
// Consecutive segments sharing the same color are batched into
// one beginPath/stroke call.  GPS gaps (gapBefore) break the
// path so no phantom line is drawn across missing track data.
// ============================================================
function CanvasTrackLayer({ filteredTraces, colorChannel, ranges, weight, opacity = 0.9 }) {
  const map = useMap();
  const propsRef  = useRef({});
  const drawFnRef = useRef(null);

  // Keep propsRef always current so the draw closure reads fresh values.
  propsRef.current = { filteredTraces, colorChannel, ranges, weight, opacity };

  // Re-draw whenever any rendering-relevant prop changes.
  useEffect(() => { drawFnRef.current?.(); },
    [filteredTraces, colorChannel, ranges, weight, opacity]); // eslint-disable-line

  // Mount the canvas once per map instance; attach Leaflet event listeners.
  useEffect(() => {
    const canvas = document.createElement('canvas');
    canvas.style.cssText = 'position:absolute;top:0;left:0;pointer-events:none;z-index:400;background:transparent;';
    map.getContainer().appendChild(canvas);

    function draw() {
      const { filteredTraces, colorChannel, ranges, weight, opacity } = propsRef.current;
      const size = map.getSize();
      canvas.width  = size.x;
      canvas.height = size.y;
      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, size.x, size.y);
      ctx.lineWidth   = weight;
      ctx.lineJoin    = 'round';
      ctx.lineCap     = 'round';
      ctx.globalAlpha = opacity;

      for (const t of filteredTraces) {
        if (!t.filtered || t.filtered.length < 2) continue;
        let currentColor = null;
        let pathOpen     = false;
        let prevPt       = null;

        for (let i = 0; i < t.filtered.length; i++) {
          const p = t.filtered[i];
          if (p.lat == null || p.lng == null) {
            if (pathOpen) { ctx.stroke(); pathOpen = false; }
            continue;
          }
          const pt = map.latLngToContainerPoint([p.lat, p.lng]);

          // GPS gap or first point: close current path, start fresh.
          if (i === 0 || p.gapBefore) {
            if (pathOpen) { ctx.stroke(); pathOpen = false; }
            prevPt = pt;
            currentColor = getPointColor(p, colorChannel, t.idx, ranges);
            continue;
          }

          const color = getPointColor(p, colorChannel, t.idx, ranges);

          // Color changed: close current path, start new one from prevPt.
          if (!pathOpen || color !== currentColor) {
            if (pathOpen) ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(prevPt.x, prevPt.y);
            ctx.strokeStyle = color;
            currentColor    = color;
            pathOpen        = true;
          }

          ctx.lineTo(pt.x, pt.y);
          prevPt = pt;
        }

        if (pathOpen) ctx.stroke();
      }

      ctx.globalAlpha = 1;
    }

    drawFnRef.current = draw;
    map.on('move zoom viewreset resize', draw);
    draw();

    return () => {
      map.off('move zoom viewreset resize', draw);
      drawFnRef.current = null;
      canvas.remove();
    };
  }, [map]); // eslint-disable-line

  return null;
}

// ============================================================
// CANVAS DOTS LAYER
// Replaces per-point <CircleMarker> React elements.
// Viewport-culls points outside the map bounds, then batches
// all dots sharing the same fill color into a single
// beginPath/fill call (≤ ~256 fill() calls regardless of N).
// ============================================================
function CanvasDotsLayer({ filteredTraces, colorChannel, ranges, radius, opacity = 0.9 }) {
  const map = useMap();
  const propsRef  = useRef({});
  const drawFnRef = useRef(null);

  propsRef.current = { filteredTraces, colorChannel, ranges, radius, opacity };

  useEffect(() => { drawFnRef.current?.(); },
    [filteredTraces, colorChannel, ranges, radius, opacity]); // eslint-disable-line

  useEffect(() => {
    const canvas = document.createElement('canvas');
    canvas.style.cssText = 'position:absolute;top:0;left:0;pointer-events:none;z-index:401;background:transparent;';
    map.getContainer().appendChild(canvas);

    function draw() {
      const { filteredTraces, colorChannel, ranges, radius, opacity } = propsRef.current;
      const size = map.getSize();
      canvas.width  = size.x;
      canvas.height = size.y;
      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, size.x, size.y);

      const bounds = map.getBounds().pad(0.05);

      // Group projected points by color for batch-fill.
      const byColor = new Map();
      for (const t of filteredTraces) {
        if (!t.filtered) continue;
        for (const p of t.filtered) {
          if (p.lat == null || p.lng == null) continue;
          if (!bounds.contains([p.lat, p.lng])) continue; // viewport cull
          const color = getPointColor(p, colorChannel, t.idx, ranges);
          if (!byColor.has(color)) byColor.set(color, []);
          byColor.get(color).push(map.latLngToContainerPoint([p.lat, p.lng]));
        }
      }

      ctx.globalAlpha = opacity;
      for (const [color, pts] of byColor) {
        ctx.beginPath();
        for (const pt of pts) {
          // moveTo before arc avoids implicit lineTo connecting circles.
          ctx.moveTo(pt.x + radius, pt.y);
          ctx.arc(pt.x, pt.y, radius, 0, Math.PI * 2);
        }
        ctx.fillStyle = color;
        ctx.fill();
      }
      ctx.globalAlpha = 1;
    }

    drawFnRef.current = draw;
    map.on('move zoom viewreset resize', draw);
    draw();

    return () => {
      map.off('move zoom viewreset resize', draw);
      drawFnRef.current = null;
      canvas.remove();
    };
  }, [map]); // eslint-disable-line

  return null;
}

// ============================================================
// CANVAS ARROWS LAYER
// Replaces the Arrows-mode SVG elements (dots + arrow heads +
// optional track underlay).  Everything is drawn on one canvas:
// 1. Optional track underlay (same batched-color path as track layer)
// 2. All dots batched by color (same technique as dots layer)
// 3. Every-N arrow heads drawn as canvas triangles with rotation
// ============================================================
function CanvasArrowsLayer({
  filteredTraces, colorChannel, ranges,
  dotRadius, dotOpacity, arrowEvery,
  showTrack, trackWeight, trackOpacity,
}) {
  const map = useMap();
  const propsRef  = useRef({});
  const drawFnRef = useRef(null);

  propsRef.current = {
    filteredTraces, colorChannel, ranges,
    dotRadius, dotOpacity, arrowEvery,
    showTrack, trackWeight, trackOpacity,
  };

  useEffect(() => { drawFnRef.current?.(); },
    [filteredTraces, colorChannel, ranges, dotRadius, dotOpacity, // eslint-disable-line
     arrowEvery, showTrack, trackWeight, trackOpacity]);           // eslint-disable-line

  useEffect(() => {
    const canvas = document.createElement('canvas');
    canvas.style.cssText = 'position:absolute;top:0;left:0;pointer-events:none;z-index:400;background:transparent;';
    map.getContainer().appendChild(canvas);

    function draw() {
      const {
        filteredTraces, colorChannel, ranges,
        dotRadius, dotOpacity, arrowEvery,
        showTrack, trackWeight, trackOpacity,
      } = propsRef.current;

      const size = map.getSize();
      canvas.width  = size.x;
      canvas.height = size.y;
      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, size.x, size.y);

      const bounds = map.getBounds().pad(0.05);

      // 1. Optional track underlay
      if (showTrack) {
        ctx.lineJoin    = 'round';
        ctx.lineCap     = 'round';
        ctx.globalAlpha = trackOpacity;

        for (const t of filteredTraces) {
          if (!t.filtered || t.filtered.length < 2) continue;
          let currentColor = null;
          let pathOpen     = false;
          let prevPt       = null;

          for (let i = 0; i < t.filtered.length; i++) {
            const p = t.filtered[i];
            if (p.lat == null || p.lng == null) {
              if (pathOpen) { ctx.stroke(); pathOpen = false; }
              continue;
            }
            const pt = map.latLngToContainerPoint([p.lat, p.lng]);

            if (i === 0 || p.gapBefore) {
              if (pathOpen) { ctx.stroke(); pathOpen = false; }
              prevPt = pt;
              currentColor = getPointColor(p, colorChannel, t.idx, ranges);
              continue;
            }

            const color = getPointColor(p, colorChannel, t.idx, ranges);
            if (!pathOpen || color !== currentColor) {
              if (pathOpen) ctx.stroke();
              ctx.beginPath();
              ctx.moveTo(prevPt.x, prevPt.y);
              ctx.strokeStyle = color;
              ctx.lineWidth   = trackWeight;
              currentColor    = color;
              pathOpen        = true;
            }
            ctx.lineTo(pt.x, pt.y);
            prevPt = pt;
          }
          if (pathOpen) ctx.stroke();
        }
        ctx.globalAlpha = 1;
      }

      // 2. Dots — viewport-culled, batched by color
      const byColor = new Map();
      for (const t of filteredTraces) {
        if (!t.filtered) continue;
        for (const p of t.filtered) {
          if (p.lat == null || p.lng == null) continue;
          if (!bounds.contains([p.lat, p.lng])) continue;
          const color = getPointColor(p, colorChannel, t.idx, ranges);
          if (!byColor.has(color)) byColor.set(color, []);
          byColor.get(color).push(map.latLngToContainerPoint([p.lat, p.lng]));
        }
      }
      ctx.globalAlpha = dotOpacity;
      for (const [color, pts] of byColor) {
        ctx.beginPath();
        for (const pt of pts) {
          ctx.moveTo(pt.x + dotRadius, pt.y);
          ctx.arc(pt.x, pt.y, dotRadius, 0, Math.PI * 2);
        }
        ctx.fillStyle = color;
        ctx.fill();
      }
      ctx.globalAlpha = 1;

      // 3. Arrow heads — canvas triangles (same shape as arrowIcon SVG polygon)
      //    Original SVG polygon points "10,2 14,16 10,13 6,16" in 20×20 viewBox.
      //    Normalised to center=(0,0), unit circumradius=8:
      //    tip=(0,-6.4), right=(3.2,4.8), notch=(0,2.4), left=(-3.2,4.8)
      ctx.globalAlpha = 1;
      for (const t of filteredTraces) {
        if (!t.filtered) continue;
        for (let i = 0; i < t.filtered.length; i++) {
          if (i % arrowEvery !== 0) continue;
          const p = t.filtered[i];
          if (p.lat == null || p.lng == null || p.brg == null) continue;
          if (!bounds.contains([p.lat, p.lng])) continue;

          const pt    = map.latLngToContainerPoint([p.lat, p.lng]);
          const color = getPointColor(p, colorChannel, t.idx, ranges);
          const s     = (8 + Math.min((p.spd ?? 0) / 10, 5)) / 8; // size scale

          ctx.save();
          ctx.translate(pt.x, pt.y);
          ctx.rotate((p.brg * Math.PI) / 180);
          ctx.beginPath();
          ctx.moveTo(0,        -6.4 * s);
          ctx.lineTo( 3.2 * s,  4.8 * s);
          ctx.lineTo(0,          2.4 * s);
          ctx.lineTo(-3.2 * s,  4.8 * s);
          ctx.closePath();
          ctx.fillStyle   = color;
          ctx.strokeStyle = 'rgba(0,0,0,0.4)';
          ctx.lineWidth   = 0.8;
          ctx.fill();
          ctx.stroke();
          ctx.restore();
        }
      }
    }

    drawFnRef.current = draw;
    map.on('move zoom viewreset resize', draw);
    draw();

    return () => {
      map.off('move zoom viewreset resize', draw);
      drawFnRef.current = null;
      canvas.remove();
    };
  }, [map]); // eslint-disable-line

  return null;
}

// Draws a rubber-band rectangle while the user drags on the map.
// `active=true` disables map panning and enables crosshair interaction.
// Calls `onBboxDrawn({ minLat, maxLat, minLng, maxLng })` on mouse-up.
function BBoxDrawLayer({ active, onBboxDrawn }) {
  const map = useMap();
  const startRef   = useRef(null);
  const drawingRef = useRef(false);
  const rectRef    = useRef(null);

  useEffect(() => {
    if (!active) {
      if (rectRef.current) { rectRef.current.remove(); rectRef.current = null; }
      map.dragging.enable();
      drawingRef.current = false;
      startRef.current   = null;
      return;
    }
    function onMouseDown(e) {
      drawingRef.current = true;
      startRef.current   = e.latlng;
      map.dragging.disable();
      if (rectRef.current) { rectRef.current.remove(); rectRef.current = null; }
      rectRef.current = L.rectangle(
        [e.latlng, e.latlng],
        { color: '#00e676', weight: 2, dashArray: '8,5', fillOpacity: 0.07, interactive: false }
      ).addTo(map);
    }
    function onMouseMove(e) {
      if (!drawingRef.current || !startRef.current || !rectRef.current) return;
      rectRef.current.setBounds(L.latLngBounds(startRef.current, e.latlng));
    }
    function onMouseUp(e) {
      if (!drawingRef.current || !startRef.current) return;
      drawingRef.current = false;
      map.dragging.enable();
      const start = startRef.current;
      startRef.current = null;
      if (rectRef.current) { rectRef.current.remove(); rectRef.current = null; }
      const sp = map.latLngToContainerPoint(start);
      const ep = map.latLngToContainerPoint(e.latlng);
      if (Math.abs(sp.x - ep.x) > 10 && Math.abs(sp.y - ep.y) > 10) {
        onBboxDrawn({
          minLat: Math.min(start.lat, e.latlng.lat),
          maxLat: Math.max(start.lat, e.latlng.lat),
          minLng: Math.min(start.lng, e.latlng.lng),
          maxLng: Math.max(start.lng, e.latlng.lng),
        });
      }
    }
    map.on('mousedown', onMouseDown);
    map.on('mousemove', onMouseMove);
    map.on('mouseup',   onMouseUp);
    return () => {
      map.off('mousedown', onMouseDown);
      map.off('mousemove', onMouseMove);
      map.off('mouseup',   onMouseUp);
      if (rectRef.current) { rectRef.current.remove(); rectRef.current = null; }
      map.dragging.enable();
    };
  }, [active, map, onBboxDrawn]);
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
    case 'accM':    return accColor(p.accM, ranges.accMin, ranges.accMax);
    case 'dpc':     return dosePerCountColor(p.dpc, ranges.dpcMin, ranges.dpcMax);
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
      // accuracyM (firmware 0.8.0+) is metres-based GPS accuracy. May be
      // measured (RadiaCode track imports) or derived from hdop via
      // /admin/backfill-accuracy (UERE=5.0). Both fields can coexist.
      accM: r.accuracyM  ?? null,
      // dose per count = µSv/h ÷ CPS; null when either is missing or CPS is zero
      dpc:  (r.uSvPerHour != null && r.cps != null && r.cps > 0) ? r.uSvPerHour / r.cps : null,
      // GPS_LOST / GPS_REGAINED transition marker (firmware 0.7.0+).
      // Event rows have no lat/lng so they're filtered out of `points`,
      // but their timestamps are used to break track polylines.
      event: r.event ?? null,
      // Gamma energy spectrum channel counts from RC-110 CsI(Tl) scintillator.
      // Array of uint16 channel counts (typically 64-379 channels). Null when
      // spectrum collection is disabled or the peer doesn't support DATA_BUF.
      spectrum: r.spectrumData ?? null,
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
  const [accMin,  setAccMin]    = useState(0);    const [accMax,  setAccMax]    = useState(25);    const [accManual,  setAccManual]    = useState(false);
  const [dpcMin,  setDpcMin]    = useState(0);    const [dpcMax,  setDpcMax]    = useState(0.05);  const [dpcManual,  setDpcManual]    = useState(false);
  // Spectrogram channel scale state + manual-override flags
  const [specTotalLow,  setSpecTotalLow]  = useState(0);      const [specTotalHigh,  setSpecTotalHigh]  = useState(500);    const [specTotalManual,  setSpecTotalManual]  = useState(false);
  const [specPeakLow,   setSpecPeakLow]   = useState(0);      const [specPeakHigh,   setSpecPeakHigh]   = useState(128);    const [specPeakManual,   setSpecPeakManual]   = useState(false);
  const [specLowELow,   setSpecLowELow]   = useState(0);      const [specLowEHigh,   setSpecLowEHigh]   = useState(200);    const [specLowEManual,   setSpecLowEManual]   = useState(false);
  const [specHighELow,  setSpecHighELow]  = useState(0);     const [specHighEHigh,  setSpecHighEHigh]  = useState(200);   const [specHighEManual,  setSpecHighEManual]  = useState(false);
  const [specCentLow,   setSpecCentLow]   = useState(0);      const [specCentHigh,   setSpecCentHigh]   = useState(64);     const [specCentManual,   setSpecCentManual]   = useState(false);
  const [specEntLow,    setSpecEntLow]    = useState(0);      const [specEntHigh,    setSpecEntHigh]    = useState(6);        const [specEntManual,    setSpecEntManual]    = useState(false);
  // stable track bounds for sliders — derived from raw data, never from the scale handles
  const [doseDataMax, setDoseDataMax] = useState(2.0);
  const [cpsDataMax,  setCpsDataMax]  = useState(100);
  const [spdDataMax,  setSpdDataMax]  = useState(120);
  const [altDataMax,  setAltDataMax]  = useState(1000);
  const [dpcDataMax,  setDpcDataMax]  = useState(0.1);
  // legacy alias kept for existing references
  const doseScaleManual = doseManual;
  const setDoseScaleManual = setDoseManual;

  // Map / display mode
  const [mapMode, setMapMode]         = useState('Hex');   // Track | Dots | Hex | Arrows
  const [colorChannel, setColorChannel] = useState('dose'); // dose | cps | speed | alt | hdop | session
  const [nanoMode, setNanoMode]         = useState(false);
  const [tileIdx, setTileIdx]           = useState(1);       // default CartoDB Dark
  const [trackWeight, setTrackWeight]   = useState(4);
  const [pointRadius, setPointRadius]   = useState(5);
  const [showTooltips, setShowTooltips] = useState(false);
  const [threeDMode,   setThreeDMode]   = useState(false);
  const [arrowEvery, setArrowEvery]         = useState(5);    // show 1-in-N arrows
  const [hexBinZoom, setHexBinZoom]         = useState(6);    // bin resolution (default = initial map zoom)
  const [hexBinAuto, setHexBinAuto]         = useState(true); // auto-follow map zoom
  const [hexFlyout,  setHexFlyout]          = useState(null); // clicked hex bin stats | null
  const [hexHover,   setHexHover]           = useState(null); // hovered hex bin stats | null
  const [radarEnabled, setRadarEnabled]     = useState(false); // spider-radar spectrum overlay on hexagons
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
  const [appMode, setAppMode] = useState('evaluate'); // evaluate | explorer | manage | render | export
  const [explorePanel, setExplorePanel] = useState('sessions'); // sessions | display | stats

  // Explorer mode state
  const [explorerSelectedZone, setExplorerSelectedZone] = useState(null); // GeoJSON Feature | null
  const [explorerAnalysisResult, setExplorerAnalysisResult] = useState(null); // FeatureCollection | null
  const [explorerZoneCoverage, setExplorerZoneCoverage]   = useState(null); // {coveredCells, uncoveredCells, coveragePct} | null
  const [liveMission, setLiveMission] = useState(null); // mission object | null
  const [searchFilter, setSearchFilter] = useState('');

  // Resizable sidebar
  const [sidebarWidth, setSidebarWidth] = useState(320);
  const sidebarRef = useRef(null);

  // Area bounding-box selection
  const [areaBboxActive, setAreaBboxActive] = useState(false);
  const [areaBbox, setAreaBbox] = useState(null); // { minLat, maxLat, minLng, maxLng } | null
  const [areaData, setAreaData] = useState(null);    // API result: { sessions, totalSamples }
  const [areaLoading, setAreaLoading] = useState(false);

  const handleBboxDrawn = useCallback((bbox) => {
    setAreaBbox(bbox);
    setAreaBboxActive(false);
    setAreaData(null);
    setAreaLoading(true);
    // One server-side geospatial query using MongoDB's 2dsphere index.
    // Returns only the matching points — no per-session bulk loading needed.
    fetchAreaSessions(bbox)
      .then(data => { setAreaData(data); setAreaLoading(false); })
      .catch(e => { setError(String(e)); setAreaLoading(false); });
  }, []);   // fetchAreaSessions is a module-level function, no deps needed

  // Stable refs for HexLayer props — prevents the canvas useEffect from
  // re-running (and rebuilding bins) on every parent render just because
  // the inline arrow functions change reference each time.
  const handleBinClick      = useCallback(bin => setHexFlyout(bin),  []);
  const handleBinHover      = useCallback(h   => setHexHover(h),     []);
  const handleBinPanelClose = useCallback(()  => setHexFlyout(null), []);

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
          const session = sessions.find(s => s.sessionId === id);
          const raw = await fetchSessionRows(id, {
            totalHint:  session?.samples,
            lastTsHint: session?.lastTsMs,
          });
          setRows(prev => ({ ...prev, [id]: compactRows(raw) }));
        } catch (e) {
          setError(String(e));
        }
      }
    }
    setSelected(next);
    setFitTrigger(t => t + 1);
  }, [selected, rowsBySession, sessions]);

  function selectAll() {
    const next = new Set(sessions.map(s => s.sessionId));
    setSelected(next);
    for (const s of sessions) {
      if (!rowsBySession[s.sessionId]) {
        fetchSessionRows(s.sessionId, { totalHint: s.samples, lastTsHint: s.lastTsMs })
          .then(raw => setRows(prev => ({ ...prev, [s.sessionId]: compactRows(raw) })))
          .catch(e => setError(String(e)));
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
      const points = [];
      // Build geo points in row order, marking `gapBefore` on any point that
      // is preceded (in raw time order) by a GPS_LOST event since the prior
      // geo point. Track-mode render skips segments where b.gapBefore is
      // true, leaving a visible break instead of drawing a phantom straight
      // line across the gap.
      let pendingGap = false;
      let prevPoint  = null;
      for (const r of rows) {
        if (r.event === 'GPS_LOST') {
          pendingGap = true;
          continue;
        }
        if (r.event) continue;  // GPS_REGAINED or other non-sample events
        if (r.lat == null || r.lng == null || (r.lat === 0 && r.lng === 0)) continue;
        const p = { ...r };
        if (pendingGap) {
          p.gapBefore = true;
          pendingGap  = false;
        } else if (prevPoint && haversineMeters(prevPoint.lat, prevPoint.lng, p.lat, p.lng) > MAX_SEGMENT_M) {
          // Distance jump too large for 100 mph travel — treat as a gap so no
          // phantom straight line is drawn across the missing track segment.
          p.gapBefore = true;
        }
        points.push(p);
        prevPoint = p;
      }
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

  // ---- area bbox filter
  // No bbox: return normal time-window filtered selected-session traces.
  // Bbox active + loading: return [] (spinner shown in sidebar).
  // Bbox active + loaded: convert server result (areaData) to the same trace
  //   format as filteredTraces so all map renderers work unchanged.
  const areaFilteredTraces = useMemo(() => {
    if (!areaBbox) return filteredTraces;
    if (!areaData) return [];  // still fetching from server
    return areaData.sessions
      .map(({ sessionId, rows: rawRows, meta }) => {
        // rawRows have the same field names as the /sessions/{id} endpoint;
        // compactRows() normalises them to the {ts, lat, lng, uSv, ...} shape.
        const compact = compactRows(rawRows);
        // Apply the same distance-gap guard used in the traces useMemo so that
        // area-filtered track rendering also avoids phantom straight segments.
        const points = compact.reduce((acc, r) => {
          if (r.lat == null || r.lng == null || (r.lat === 0 && r.lng === 0) || r.event) return acc;
          const p    = { ...r };
          const prev = acc.length ? acc[acc.length - 1] : null;
          if (prev && haversineMeters(prev.lat, prev.lng, p.lat, p.lng) > MAX_SEGMENT_M) {
            p.gapBefore = true;
          }
          acc.push(p);
          return acc;
        }, []);
        // Match the session's color index from the global sessions list.
        const idx = sessions.findIndex(s => s.sessionId === sessionId);
        return {
          id:       sessionId,
          color:    sessionColor(idx >= 0 ? idx : 0),
          points,
          filtered: points,
          rows:     compact,
          meta:     meta || { sessionId },
          idx:      idx >= 0 ? idx : 0,
        };
      })
      .filter(t => t.points.length > 0);
  }, [areaBbox, areaData, filteredTraces, sessions]);

  // ---- set of session IDs that have at least one point inside the bbox
  const sessionsInArea = useMemo(() => {
    if (!areaBbox) return new Set();
    return new Set(areaFilteredTraces.filter(t => t.filtered.length > 0).map(t => t.id));
  }, [areaFilteredTraces, areaBbox]);

  // ---- aggregate stats (from area-filtered data)
  const stats = useMemo(() => {
    let n = 0, sumDose = 0, maxDose = -Infinity, minDose = Infinity;
    let sumCps = 0, cpsN = 0, maxCps = -Infinity;
    let sumSpd = 0, spdN = 0, maxSpd = -Infinity;
    let maxBrg = null, lastBrg = null;
    for (const t of areaFilteredTraces) {
      for (const p of t.filtered) {
        if (p.uSv != null) { n++; sumDose += p.uSv; if (p.uSv > maxDose) maxDose = p.uSv; if (p.uSv < minDose) minDose = p.uSv; }
        if (p.cps != null) { cpsN++; sumCps += p.cps; if (p.cps > maxCps) maxCps = p.cps; }
        if (p.spd != null) { spdN++; sumSpd += p.spd; if (p.spd > maxSpd) maxSpd = p.spd; }
        if (p.brg != null) lastBrg = p.brg;
      }
    }
    return {
      count: n,
      avgDose: n ? sumDose / n : null,
      maxDose: n ? maxDose : null,
      minDose: n ? minDose : null,
      avgCps: cpsN ? sumCps / cpsN : null,
      maxCps: maxCps > -Infinity ? maxCps : null,
      avgSpd: spdN ? sumSpd / spdN : null,
      maxSpd: maxSpd > -Infinity ? maxSpd : null,
      lastBrg,
    };
  }, [areaFilteredTraces]);

  // All area-filtered points flattened for sparkline
  const allFilteredPoints = useMemo(() => {
    const arr = [];
    for (const t of areaFilteredTraces) for (const p of t.filtered) arr.push(p);
    arr.sort((a, b) => a.ts - b.ts);
    return arr;
  }, [areaFilteredTraces]);

  // ---- auto-scale all color channels from loaded data
  // When an area bbox is active and data is loaded, scale from the area-filtered
  // rows so that the color range fits the VISIBLE data, not the entire session.
  // Without this, a session with mostly background radiation produces a 98th-pct
  // max of ~0.115 µSv/h, but the drawn area may contain hotspots far above that,
  // causing every hex in the area to clamp to the max color.
  // NOTE: must be placed AFTER areaFilteredTraces is declared (TDZ guard).
  useEffect(() => {
    // If areaBbox is set but areaFilteredTraces is empty (still loading), skip —
    // the vals.length < 2 guard below will bail and leave the previous range intact.
    const useAreaData = areaBbox && areaFilteredTraces.length > 0;

    function collectVals(field) {
      const vals = [];
      if (useAreaData) {
        for (const t of areaFilteredTraces) {
          for (const r of (t.rows || [])) {
            const v = r[field];
            if (typeof v === 'number' && isFinite(v)) vals.push(v);
          }
        }
      } else {
        for (const id of selected) {
          const rows = rowsBySession[id];
          if (!rows) continue;
          for (const r of rows) {
            const v = r[field];
            if (typeof v === 'number' && isFinite(v)) vals.push(v);
          }
        }
      }
      return vals;
    }

    function autoScale(field, manual, setLo, setHi, decimals) {
      if (manual) return;
      const vals = collectVals(field);
      if (vals.length < 2) return;
      vals.sort((a, b) => a - b);
      const lo = vals[Math.floor(vals.length * 0.02)];
      const hi = vals[Math.floor(vals.length * 0.98)];
      if (!(hi > lo)) return;
      setLo(parseFloat(lo.toFixed(decimals)));
      setHi(parseFloat(hi.toFixed(decimals)));
    }

    autoScale('uSv',  doseManual, setDoseMin, setDoseMax, 3);
    autoScale('cps',  cpsManual,  setCpsMin,  setCpsMax,  1);
    autoScale('spd',  spdManual,  setSpdMin,  setSpdMax,  1);
    autoScale('alt',  altManual,  setAltMin,  setAltMax,  0);
    autoScale('hdop', hdopManual, setHdopMin, setHdopMax, 2);
    autoScale('accM', accManual,  setAccMin,  setAccMax,  0);
    autoScale('dpc',  dpcManual,  setDpcMin,  setDpcMax,  4);

    // Update track slider ceilings from absolute max of visible data.
    const rawMax = (field, fallback) => {
      let m = fallback;
      const src = useAreaData
        ? areaFilteredTraces.flatMap(t => t.rows || [])
        : [...selected].flatMap(id => rowsBySession[id] || []);
      for (const r of src) {
        const v = r[field];
        if (typeof v === 'number' && isFinite(v) && v > m) m = v;
      }
      return m;
    };
    setDoseDataMax(Math.max(rawMax('uSv',  0) * 1.2, 2));
    setCpsDataMax (Math.max(rawMax('cps',  0) * 1.2, 10));
    setSpdDataMax (Math.max(rawMax('spd',  0) * 1.2, 20));
    setAltDataMax (Math.max(rawMax('alt',  0) * 1.2, 100));
    setDpcDataMax (Math.max(rawMax('dpc',  0) * 1.2, 0.01));
  }, [rowsBySession, selected, areaBbox, areaFilteredTraces, doseManual, cpsManual, spdManual, altManual, hdopManual, accManual, dpcManual]); // eslint-disable-line

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
  // Memoized so canvas layers only redraw when a scale value actually changes,
  // not on every unrelated render (e.g. sidebar resize, tab switch).
  const ranges = useMemo(() => ({
    doseMin, doseMax, cpsMin, cpsMax, spdMin, spdMax, altMin, altMax, hdopMin, hdopMax, accMin, accMax, dpcMin, dpcMax,
  }), [doseMin, doseMax, cpsMin, cpsMax, spdMin, spdMax, altMin, altMax, hdopMin, hdopMax, accMin, accMax, dpcMin, dpcMax]); // eslint-disable-line
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
            className={`nav-mode-btn ${appMode === 'evaluate' ? 'active' : ''}`}
            onClick={() => setAppMode('evaluate')}>
            Evaluate
          </button>
          <button
            className={`nav-mode-btn ${appMode === 'explorer' ? 'active' : ''}`}
            onClick={() => setAppMode('explorer')}>
            Explorer
          </button>
          <button
            className={`nav-mode-btn ${appMode === 'manage' ? 'active' : ''}`}
            onClick={() => setAppMode('manage')}>
            Data Management
          </button>
          <button
            className={`nav-mode-btn ${appMode === 'render' ? 'active' : ''}`}
            onClick={() => setAppMode('render')}>
            Render
          </button>
          <button
            className={`nav-mode-btn ${appMode === 'export' ? 'active' : ''}`}
            onClick={() => setAppMode('export')}>
            Export
          </button>

        </div>
        <div className="nav-meta">
          <span>{sessions.length} session{sessions.length !== 1 ? 's' : ''}</span>
        </div>
      </nav>

      {/* === APP BODY === */}
      <div className="app-body">

      {/* === EVALUATE MODE (was Explore) === */}
      {appMode === 'evaluate' && (<>
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

            {/* ---- Area selection toolbar ---- */}
            <div className="area-toolbar">
              <button
                className={`area-draw-btn${areaBboxActive ? ' active' : ''}`}
                title="Drag a rectangle on the map to filter all data to that area"
                onClick={() => setAreaBboxActive(v => !v)}
              >
                {areaBboxActive ? '✏ Drawing\u2026 drag map' : areaBbox ? '✏ Redraw area' : '\u25a1 Select area'}
              </button>
              {areaBbox && (
                <button className="area-clear-btn" title="Clear area filter" onClick={() => {
                  setAreaBbox(null);
                  setAreaBboxActive(false);
                  setAreaData(null);
                  setAreaLoading(false);
                }}>&#x2715;</button>
              )}
            </div>

            {/* ---- Area summary card ---- */}
            {areaBbox && (() => {
              const ptCount   = areaData ? areaData.totalSamples : 0;
              const sessCount = areaFilteredTraces.length;
              return (
                <div className="area-info-card">
                  {areaLoading ? (
                    <div className="area-info-row">
                      <span className="area-info-label" style={{ color: '#ffc107' }}>
                        &#x23F3; Querying database…
                      </span>
                    </div>
                  ) : (
                    <>
                      <div className="area-info-row">
                        <span className="area-info-label">Points in area</span>
                        <span className="area-info-value">{ptCount.toLocaleString()}</span>
                      </div>
                      <div className="area-info-row">
                        <span className="area-info-label">Sessions with data</span>
                        <span className="area-info-value">{sessCount}</span>
                      </div>
                    </>
                  )}
                  <div className="area-info-coords">
                    {areaBbox.minLat.toFixed(4)}&deg;,{areaBbox.minLng.toFixed(4)}&deg;
                    {' \u2192 '}
                    {areaBbox.maxLat.toFixed(4)}&deg;,{areaBbox.maxLng.toFixed(4)}&deg;
                  </div>
                </div>
              );
            })()}

            <div className="search-row">
              <input className="search-input" placeholder="Filter sessions..."
                value={searchFilter} onChange={e => setSearchFilter(e.target.value)} />
              {searchFilter && (
                <button className="btn-sm" onClick={() => setSearchFilter('')}
                  style={{ flexShrink: 0 }}>&#x2715;</button>
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
                      ? rows.reduce((m, r) => Math.max(m, r.uSv ?? 0), 0).toFixed(3)
                      : null;
                    // Area filter indicator: true = loaded + has pts in area, false = loaded + not in area, null = not loaded
                    const inArea = areaBbox
                      ? (sessionsInArea.has(s.sessionId) ? true : (rows ? false : null))
                      : null;
                    return (
                      <li key={s.sessionId} className={`session-item ${isSel ? 'sel' : ''}${inArea === false && areaBbox ? ' area-dim' : ''}`}>
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
                          {inArea === true  && <span className="badge area-badge">in area</span>}
                          {inArea === null && areaBbox && <span className="badge area-badge-unknown">?</span>}
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
                  onClick={() => { setMapMode(m); if (m !== 'Hex') setHexFlyout(null); }}>
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
            {colorChannel === 'accM' && (
              <DualRangeSlider
                lo={0} hi={50}
                low={accMin} high={accMax}
                onLowChange={v  => { setAccManual(true); setAccMin(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setAccManual(true); setAccMax(parseFloat(v.toFixed(0))); }}
                colorFn={t => accColor(t, 0, 1)}
                label="Accuracy scale (m)"
                fmtVal={v => v.toFixed(0) + ' m'}
                onAuto={() => setAccManual(false)}
              />
            )}
            {colorChannel === 'dpc' && (
              <DualRangeSlider
                lo={0} hi={dpcDataMax}
                low={dpcMin} high={dpcMax}
                onLowChange={v  => { setDpcManual(true); setDpcMin(parseFloat(v.toFixed(4))); }}
                onHighChange={v => { setDpcManual(true); setDpcMax(parseFloat(v.toFixed(4))); }}
                colorFn={t => dosePerCountColor(t, 0, 1)}
                label="Dose/Count scale (µSv/c)"
                fmtVal={v => v.toFixed(4) + ' µSv/c'}
                onAuto={() => setDpcManual(false)}
              />
            )}

            {/* Spectrogram color channels scales */}
            {SPECTROGRAM_CHANNELS.some(ch => ch.key === colorChannel) && (
              <SectionHead>Spectrogram Scale</SectionHead>
            )}
            {colorChannel === 'totalcounts' && (
              <DualRangeSlider
                lo={0} hi={specTotalHigh || 1}
                low={specTotalLow} high={specTotalHigh}
                onLowChange={v => { setSpecTotalManual(true); setSpecTotalLow(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setSpecTotalManual(true); setSpecTotalHigh(parseFloat(v.toFixed(0))); }}
                colorFn={t => totalCountsColor(t, 0, 1)}
                label="Total counts scale"
                fmtVal={v => v.toFixed(0) + ' cnt'}
                onAuto={() => setSpecTotalManual(false)}
              />
            )}
            {colorChannel === 'peakchannel' && (
              <DualRangeSlider
                lo={0} hi={379}
                low={specPeakLow} high={specPeakHigh}
                onLowChange={v => { setSpecTotalManual(true); setSpecPeakLow(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setSpecTotalManual(true); setSpecPeakHigh(parseFloat(v.toFixed(0))); }}
                colorFn={t => peakChannelColor(t, 0, 1)}
                label="Peak channel scale"
                fmtVal={v => `ch ${v.toFixed(0)}`}
                onAuto={() => setSpecTotalManual(false)}
              />
            )}
            {colorChannel === 'lowenergy' && (
              <DualRangeSlider
                lo={0} hi={specLowEHigh || 1}
                low={specLowELow} high={specLowEHigh}
                onLowChange={v => { setSpecLowEManual(true); setSpecLowELow(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setSpecLowEManual(true); setSpecLowEHigh(parseFloat(v.toFixed(0))); }}
                colorFn={t => lowEnergyColor(t, 0, 1)}
                label="Low energy scale"
                fmtVal={v => v.toFixed(0) + ' cnt'}
                onAuto={() => setSpecLowEManual(false)}
              />
            )}
            {colorChannel === 'highenergy' && (
              <DualRangeSlider
                lo={0} hi={specHighELow || 1}
                low={specHighELow} high={specHighEHigh || 1}
                onLowChange={v => { setSpecTotalManual(true); setSpecHighELow(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setSpecTotalManual(true); setSpecHighEHigh(parseFloat(v.toFixed(0))); }}
                colorFn={t => highEnergyColor(t, 0, 1)}
                label="High energy scale"
                fmtVal={v => v.toFixed(0) + ' cnt'}
                onAuto={() => setSpecLowEManual(false)}
              />
            )}
            {colorChannel === 'centroid' && (
              <DualRangeSlider
                lo={0} hi={64}
                low={specCentLow} high={specCentHigh}
                onLowChange={v => { setSpecCentManual(true); setSpecCentLow(parseFloat(v.toFixed(0))); }}
                onHighChange={v => { setSpecCentManual(true); setSpecCentHigh(parseFloat(v.toFixed(0))); }}
                colorFn={t => spectralCentroidColor(t, 0, 1)}
                label="Spectral centroid scale"
                fmtVal={v => `ch ${v.toFixed(0)}`}
                onAuto={() => setSpecCentManual(false)}
              />
            )}
            {colorChannel === 'entropy' && (
              <DualRangeSlider
                lo={0} hi={6.5}
                low={specEntLow} high={specEntHigh}
                onLowChange={v => { setSpecTotalManual(true); setSpecEntLow(parseFloat(v.toFixed(2))); }}
                onHighChange={v => { setSpecTotalManual(true); setSpecEntHigh(parseFloat(v.toFixed(2))); }}
                colorFn={t => spectralEntropyColor(t, 0, 1)}
                label="Spectral entropy scale"
                fmtVal={v => v.toFixed(2)}
                onAuto={() => setSpecLowEManual(false)}
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
            <div className="render-cards">
              <label className="toggle-pill">
                <input type="checkbox" checked={threeDMode} onChange={e => setThreeDMode(e.target.checked)} />
                <span className="toggle-track"><span className="toggle-thumb" /></span>
                <span className="toggle-label">▲ 3D view (alt = Z)</span>
              </label>
              <label className="toggle-pill">
                <input type="checkbox" checked={nanoMode} onChange={e => setNanoMode(e.target.checked)} />
                <span className="toggle-track"><span className="toggle-thumb" /></span>
                <span className="toggle-label">nSv/h mode</span>
              </label>
              <label className="toggle-pill">
                <input type="checkbox" checked={showTooltips} onChange={e => setShowTooltips(e.target.checked)} />
                <span className="toggle-track"><span className="toggle-thumb" /></span>
                <span className="toggle-label">Tooltips</span>
              </label>
            </div>

            {mapMode === 'Track' && (
              <div className="render-cards">
                <div className="ctrl-card">
                  <div className="ctrl-card-header">
                    <span className="ctrl-card-label">Track width</span>
                    <span className="ctrl-card-value">{trackWeight}px</span>
                  </div>
                  <input type="range" className="ctrl-range" min="1" max="10" value={trackWeight}
                    onChange={e => setTrackWeight(Number(e.target.value))} />
                </div>
                <label className="toggle-pill">
                  <input type="checkbox" checked={trackShowDots}
                    onChange={e => setTrackShowDots(e.target.checked)} />
                  <span className="toggle-track"><span className="toggle-thumb" /></span>
                  <span className="toggle-label">Dot overlay</span>
                </label>
                {trackShowDots && (
                  <div className="ctrl-card">
                    <div className="ctrl-card-header">
                      <span className="ctrl-card-label">Dot opacity</span>
                      <span className="ctrl-card-value">{Math.round(trackDotOpacity * 100)}%</span>
                    </div>
                    <input type="range" className="ctrl-range" min="0.05" max="1" step="0.05" value={trackDotOpacity}
                      onChange={e => setTrackDotOpacity(Number(e.target.value))} />
                  </div>
                )}
              </div>
            )}
            {(mapMode === 'Dots' || mapMode === 'Arrows') && (
              <div className="render-cards">
                <div className="ctrl-card">
                  <div className="ctrl-card-header">
                    <span className="ctrl-card-label">Point radius</span>
                    <span className="ctrl-card-value">{pointRadius}px</span>
                  </div>
                  <input type="range" className="ctrl-range" min="2" max="16" value={pointRadius}
                    onChange={e => setPointRadius(Number(e.target.value))} />
                </div>
              </div>
            )}
            {mapMode === 'Hex' && (
              <div className="render-cards">
                <div className="ctrl-card">
                  <div className="ctrl-card-header">
                    <span className="ctrl-card-label">Hex bin level</span>
                    <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
                      <span className="ctrl-card-value">{hexBinZoom}</span>
                      <button
                        className={`auto-pill${hexBinAuto ? ' active' : ''}`}
                        onClick={() => setHexBinAuto(true)}>
                        auto
                      </button>
                    </div>
                  </div>
                  <input type="range" className="ctrl-range" min="1" max="22" value={hexBinZoom}
                    onChange={e => { setHexBinZoom(Number(e.target.value)); setHexBinAuto(false); }} />
                  <div className="ctrl-range-labels">
                    <span>coarse</span><span>fine</span>
                  </div>
                </div>
                <label className="toggle-pill">
                  <input type="checkbox" checked={radarEnabled}
                    onChange={e => setRadarEnabled(e.target.checked)} />
                  <span className="toggle-track"><span className="toggle-thumb" /></span>
                  <span className="toggle-label">Spectrum radar overlay</span>
                </label>
              </div>
            )}
            {mapMode === 'Arrows' && (
              <div className="render-cards">
                <div className="ctrl-card">
                  <div className="ctrl-card-header">
                    <span className="ctrl-card-label">Arrow every</span>
                    <span className="ctrl-card-value">{arrowEvery} pts</span>
                  </div>
                  <input type="range" className="ctrl-range" min="1" max="20" value={arrowEvery}
                    onChange={e => setArrowEvery(Number(e.target.value))} />
                </div>
                <div className="ctrl-card">
                  <div className="ctrl-card-header">
                    <span className="ctrl-card-label">Dot opacity</span>
                    <span className="ctrl-card-value">{Math.round(arrowDotOpacity * 100)}%</span>
                  </div>
                  <input type="range" className="ctrl-range" min="0" max="1" step="0.05" value={arrowDotOpacity}
                    onChange={e => setArrowDotOpacity(Number(e.target.value))} />
                </div>
                <label className="toggle-pill">
                  <input type="checkbox" checked={arrowShowTrack}
                    onChange={e => setArrowShowTrack(e.target.checked)} />
                  <span className="toggle-track"><span className="toggle-thumb" /></span>
                  <span className="toggle-label">Track underlay</span>
                </label>
                {arrowShowTrack && (
                  <div className="ctrl-card">
                    <div className="ctrl-card-header">
                      <span className="ctrl-card-label">Track opacity</span>
                      <span className="ctrl-card-value">{Math.round(arrowTrackOpacity * 100)}%</span>
                    </div>
                    <input type="range" className="ctrl-range" min="0.05" max="1" step="0.05" value={arrowTrackOpacity}
                      onChange={e => setArrowTrackOpacity(Number(e.target.value))} />
                  </div>
                )}
              </div>
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

      {/* === MAP / 3D VIEW === */}
      <main className={`map-pane${areaBboxActive ? ' bbox-select-active' : ''}`}>
        {/* 3D overlay — floats above the Leaflet map; can be combined with any map mode. */}
        {threeDMode && (
          <ThreeDView
            filteredTraces={filteredTraces}
            colorChannel={colorChannel}
            ranges={ranges}
            tileUrl={tile.url}
          />
        )}

        {/* 2D Leaflet map — hidden (not unmounted) while 3D overlay is active so
            zoom/pan state is preserved when switching back. */}
        <div style={{ display: threeDMode ? 'none' : 'contents' }}>
        <MapContainer center={[39.5, -98.35]} zoom={6} maxZoom={22} style={{ width: '100%', height: '100%' }}>
          <TileLayer
            key={tile.url}
            attribution={tile.attribution}
            url={tile.url}
            maxZoom={22}
            maxNativeZoom={tile.maxNativeZoom ?? 19}
          />
          <MapZoomSync onZoomChange={z => { if (hexBinAuto) setHexBinZoom(z); }} />
          {fitBounds && <FitBoundsOnce bounds={fitBounds} dep={fitTrigger} />}

          {/* Hex binning mode — single canvas merges all sessions; enables click → flyout */}
          {mapMode === 'Hex' && (
            <HexLayer
              traces={areaFilteredTraces}
              field={colorChannel}
              binZoom={hexBinZoom}
              onBinClick={handleBinClick}
              onBinHover={handleBinHover}
              ranges={ranges}
              radarEnabled={radarEnabled}
            />
          )}

          {/* Track mode — canvas polyline (no per-segment DOM nodes) */}
          {mapMode === 'Track' && (
            <CanvasTrackLayer
              filteredTraces={areaFilteredTraces}
              colorChannel={colorChannel}
              ranges={ranges}
              weight={trackWeight}
            />
          )}

          {/* Track mode — optional dot overlay — canvas */}
          {mapMode === 'Track' && trackShowDots && (
            <CanvasDotsLayer
              filteredTraces={areaFilteredTraces}
              colorChannel={colorChannel}
              ranges={ranges}
              radius={pointRadius}
              opacity={trackDotOpacity}
            />
          )}

          {/* Dots mode — canvas (viewport-culled, color-batched) */}
          {mapMode === 'Dots' && (
            <CanvasDotsLayer
              filteredTraces={areaFilteredTraces}
              colorChannel={colorChannel}
              ranges={ranges}
              radius={pointRadius}
              opacity={0.9}
            />
          )}

          {/* Arrows mode — canvas (track underlay + dots + arrow heads) */}
          {mapMode === 'Arrows' && (
            <CanvasArrowsLayer
              filteredTraces={areaFilteredTraces}
              colorChannel={colorChannel}
              ranges={ranges}
              dotRadius={pointRadius - 1}
              dotOpacity={arrowDotOpacity}
              arrowEvery={arrowEvery}
              showTrack={arrowShowTrack}
              trackWeight={trackWeight}
              trackOpacity={arrowTrackOpacity}
            />
          )}

          {/* End-of-track session ID markers — kept as SVG (interactive, 1 per session) */}
          {(mapMode === 'Track' || mapMode === 'Dots') && areaFilteredTraces.map(t => {
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

          {/* Area bbox draw tool */}
          <BBoxDrawLayer active={areaBboxActive} onBboxDrawn={handleBboxDrawn} />

          {/* Persistent area bbox rectangle */}
          {areaBbox && (
            <Rectangle
              bounds={[[areaBbox.minLat, areaBbox.minLng], [areaBbox.maxLat, areaBbox.maxLng]]}
              pathOptions={{ color: '#00e676', weight: 2, dashArray: '8,5', fillOpacity: 0.05, interactive: false }}
            />
          )}
        </MapContainer>
        </div>{/* end 2D Leaflet map wrapper */}

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

        {/* Hex bin hover tooltip */}
        {mapMode === 'Hex' && <HexTooltip data={hexHover} />}

        {/* Hex bin analysis panel — full-height right flyout on hex click */}
        {hexFlyout && mapMode === 'Hex' && (
          <HexBinPanel data={hexFlyout} onClose={handleBinPanelClose} />
        )}
      </main>
      </>)}
      {/* end explore mode */}

      {/* === EXPLORER MODE === */}
      {appMode === 'explorer' && (<>
        {/* Sidebar */}
        <aside className="sidebar" ref={sidebarRef} style={{ width: sidebarWidth }}>
          <ExplorerPanel
            onZoneSelect={zone => {
              setExplorerSelectedZone(zone);
              // Clear old coverage whenever a new zone is selected
              if (!zone) setExplorerZoneCoverage(null);
            }}
            selectedZone={explorerSelectedZone}
            onAnalysisResult={result => setExplorerAnalysisResult(result)}
            onGoLive={mission => setLiveMission(mission)}
            onZoneCoverageUpdate={cov => setExplorerZoneCoverage(cov)}
          />
          <div className="resize-handle" onMouseDown={startResize} />
        </aside>

        {/* Map — shows gap zones + covered/uncovered cells */}
        <main className="map-pane">
          <MapContainer center={[39.5, -98.35]} zoom={6} maxZoom={22} style={{ width: '100%', height: '100%' }}>
            <TileLayer
              attribution={tile.attribution}
              url={tile.url}
              maxZoom={22}
              maxNativeZoom={tile.maxNativeZoom ?? 19}
            />
            {/* Auto-fit map to selected zone bbox */}
            {explorerSelectedZone?.properties?.bbox && (() => {
              const [minLng, minLat, maxLng, maxLat] = explorerSelectedZone.properties.bbox;
              return <FitBboxOnce
                bbox={[[minLat, minLng], [maxLat, maxLng]]}
                dep={explorerSelectedZone.properties.rank}
              />;
            })()}

            {/* Gap zone polygons from last analysis */}
            {explorerAnalysisResult && explorerAnalysisResult.features.map((f, i) => {
              const coords = f.geometry?.coordinates?.[0];
              if (!coords) return null;
              const positions = coords.map(([lng, lat]) => [lat, lng]);
              const isSelected = explorerSelectedZone?.properties?.rank === f.properties?.rank;
              const sqMi = f.properties.areaSqMi ?? (f.properties.areaKm2 * 0.386102);
              return (
                <Polygon
                  key={`gap-${i}`}
                  positions={positions}
                  pathOptions={{
                    color: isSelected ? '#ffea00' : '#29b6f6',
                    weight: isSelected ? 2 : 1,
                    opacity: isSelected ? 1 : 0.7,
                    fillColor: isSelected ? '#ffea00' : '#29b6f6',
                    fillOpacity: isSelected ? 0.15 : 0.07,
                  }}>
                  <Tooltip>
                    Zone #{f.properties.rank} — {sqMi.toFixed(1)} sq mi
                    {' · '}score {f.properties.score.toFixed(2)}
                  </Tooltip>
                </Polygon>
              );
            })}

            {/* Zone coverage: green = already visited, orange = unvisited */}
            {explorerZoneCoverage?.coveredCells?.map(([lat, lng], i) => (
              <CircleMarker key={`cov-${i}`} center={[lat, lng]} radius={4}
                pathOptions={{ color: 'transparent', fillColor: '#00e676', fillOpacity: 0.55, weight: 0 }} />
            ))}
            {explorerZoneCoverage?.uncoveredCells?.map(([lat, lng], i) => (
              <CircleMarker key={`unc-${i}`} center={[lat, lng]} radius={4}
                pathOptions={{ color: 'transparent', fillColor: '#ff7043', fillOpacity: 0.30, weight: 0 }} />
            ))}
          </MapContainer>
        </main>
      </>)}
      {/* end explorer mode */}

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

      {/* === RENDER MODE === */}
      {appMode === 'render' && (
        <RenderPanel
          sessions={sessions}
          rowsBySession={rowsBySession}
          onRowsLoaded={(newRows) => setRows(prev => ({ ...prev, ...newRows }))}
        />
      )}

      {/* === EXPORT MODE === */}
      {appMode === 'export' && (
        <ExportPanel />
      )}



      {/* === LIVE TRACKING OVERLAY === */}
      {/* Renders on top of everything when a mission is active.           */}
      {/* Closing it sets liveMission to null (the mission stays in DB).   */}
      {liveMission && (
        <LiveTrackingPanel
          mission={liveMission}
          allRows={[]}
          onEnd={() => setLiveMission(null)}
        />
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
        {p.accM != null && <div className="tt-row"><span>Accuracy</span><b>+/- {p.accM.toFixed(1)} m</b></div>}
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
