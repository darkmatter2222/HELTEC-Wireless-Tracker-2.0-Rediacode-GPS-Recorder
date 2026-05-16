// RenderPanel — full-screen "Render" mode.
//
// Lets the user select any number of tracks (potentially millions of points)
// and rasterise them into a single huge PNG up to ~8K resolution. Renders
// off-screen in chunks (with rAF yields) so the UI stays responsive, then
// hands the result to an interactive pan/zoom explorer with a Save button.
//
// Many of the visual options are intentionally "artistic" — additive
// compositing, palette presets, glow, vignette etc. — because the whole
// point is to make wall-poster-grade renders that wouldn't be possible in
// the live Leaflet map.

import React, { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import { fetchSessionRows } from './api.js';
import { sessionColor } from './colors.js';

const MIN_VALID_TS_MS = 1577836800000; // 2020-01-01 UTC

// ---- output size presets ---------------------------------------------------

const SIZE_PRESETS = [
  { label: 'HD 1080p',  w: 1920,  h: 1080  },
  { label: '2K square', w: 2048,  h: 2048  },
  { label: '4K UHD',    w: 3840,  h: 2160  },
  { label: '4K square', w: 4096,  h: 4096  },
  { label: '6K wide',   w: 6144,  h: 3456  },
  { label: '8K UHD',    w: 7680,  h: 4320  },
  { label: '8K square', w: 8192,  h: 8192  },
  { label: '12K wide',  w: 11520, h: 6480  },
  { label: '16K UHD',   w: 15360, h: 8640  },
  { label: '16K square',w: 16384, h: 16384 },
  { label: 'Poster 24x36 @300dpi', w: 7200, h: 10800 },
  { label: 'Poster 36x24 @300dpi', w: 10800, h: 7200 },
];

// Common aspect ratios for big custom renders. 'free' means width and
// height move independently. Any other key locks one to the other.
const ASPECT_RATIOS = [
  { key: 'free',  label: 'Free',     ratio: null },
  { key: '16:9',  label: '16:9',     ratio: 16/9 },
  { key: '9:16',  label: '9:16',     ratio: 9/16 },
  { key: '21:9',  label: '21:9',     ratio: 21/9 },
  { key: '4:3',   label: '4:3',      ratio: 4/3 },
  { key: '3:4',   label: '3:4',      ratio: 3/4 },
  { key: '3:2',   label: '3:2',      ratio: 3/2 },
  { key: '2:3',   label: '2:3',      ratio: 2/3 },
  { key: '1:1',   label: '1:1',      ratio: 1 },
  { key: '2:1',   label: '2:1',      ratio: 2 },
];

// Soft advisory thresholds. The render is NEVER blocked by size --
// the user explicitly asked for "no such thing as too big". The browser
// itself enforces hard limits (Chrome canvas dimension cap is ~32767 per
// side, total area ~268 MP; if exceeded createElement('canvas') silently
// downscales and we catch the resulting mismatch). Past PREVIEW_MAX_PIXELS
// we skip building an <img> preview (which would crash the tab) and
// auto-download the PNG straight away.
const HUGE_WARN_PIXELS    = 8192 * 8192;     // 268 MP
const MASSIVE_WARN_PIXELS = 16384 * 16384;   // 1 GP
const PREVIEW_MAX_PIXELS  = 64_000_000;      // ~64 MP -- safe for <img>

// Great-circle distance in metres between two WGS84 points (haversine).
// Used by the Render-mode "split far apart points" toggle to break track
// lines across stretches where the GPS clearly went out (older firmware
// before v0.7.0 didn't emit GPS_LOST / GPS_REGAINED event rows, so the
// only signal of an outage is two consecutive fixes hundreds of metres
// apart in time).
function haversineMeters(aLat, aLng, bLat, bLng) {
  const R = 6371000;
  const toRad = Math.PI / 180;
  const dLat = (bLat - aLat) * toRad;
  const dLng = (bLng - aLng) * toRad;
  const s1 = Math.sin(dLat / 2);
  const s2 = Math.sin(dLng / 2);
  const a = s1 * s1 + Math.cos(aLat * toRad) * Math.cos(bLat * toRad) * s2 * s2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
}

// ---- map modes -------------------------------------------------------------

const RENDER_MODES = [
  { key: 'track',   label: 'Track lines' },
  { key: 'dots',    label: 'Dots' },
  { key: 'hex',     label: 'Hex bins' },
  { key: 'heatmap', label: 'Heatmap' },
  { key: 'splat',   label: 'Gaussian splat' },
];

const COLOR_CHANNELS = [
  { key: 'dose',    label: 'Dose rate (µSv/h)' },
  { key: 'cps',     label: 'CPS' },
  { key: 'speed',   label: 'Speed (km/h)' },
  { key: 'alt',     label: 'Altitude (m)' },
  { key: 'hdop',    label: 'HDOP' },
  { key: 'accM',    label: 'Accuracy (m)' },
  { key: 'time',    label: 'Time (early→late)' },
  { key: 'session', label: 'Session' },
];

const COMPOSITE_MODES = [
  { key: 'source-over', label: 'Normal' },
  { key: 'lighter',     label: 'Additive (glow)' },
  { key: 'screen',      label: 'Screen' },
];

const BG_OPTIONS = [
  { key: 'transparent', label: 'Transparent', color: null },
  { key: 'black',       label: 'Black',       color: '#000000' },
  { key: 'dark',        label: 'Dark grey',   color: '#181a1d' },
  { key: 'paper',       label: 'Off-white',   color: '#f4f1e8' },
  { key: 'white',       label: 'White',       color: '#ffffff' },
];

const TILE_OPTIONS = [
  { key: 'none',     label: 'None' },
  { key: 'osm',      label: 'OSM Streets',   url: 'https://a.tile.openstreetmap.org/{z}/{x}/{y}.png', maxZoom: 19 },
  { key: 'dark',     label: 'CartoDB Dark',  url: 'https://a.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png',  maxZoom: 20 },
  { key: 'light',    label: 'CartoDB Light', url: 'https://a.basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png', maxZoom: 20 },
  { key: 'topo',     label: 'OpenTopoMap',   url: 'https://a.tile.opentopomap.org/{z}/{x}/{y}.png', maxZoom: 17 },
  { key: 'sat',      label: 'Esri Satellite',url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', maxZoom: 18 },
];

// ---- palettes --------------------------------------------------------------
// Each palette is a function t∈[0,1] → [r,g,b]. We bake to a 256-entry LUT
// at render time so the inner loop is just a table lookup.

function lerp(a, b, t) { return a + (b - a) * t; }
function lerpRGB(c0, c1, t) {
  return [Math.round(lerp(c0[0], c1[0], t)),
          Math.round(lerp(c0[1], c1[1], t)),
          Math.round(lerp(c0[2], c1[2], t))];
}

function makeStopsPalette(stops) {
  // stops: [[t, [r,g,b]], ...]
  return (t) => {
    t = Math.max(0, Math.min(1, t));
    for (let i = 1; i < stops.length; i++) {
      if (t <= stops[i][0]) {
        const u = (t - stops[i-1][0]) / Math.max(1e-9, stops[i][0] - stops[i-1][0]);
        return lerpRGB(stops[i-1][1], stops[i][1], u);
      }
    }
    return stops[stops.length-1][1];
  };
}

const PALETTES = {
  default:  makeStopsPalette([[0,[0,230,118]],[0.5,[255,234,0]],[1,[213,0,0]]]),
  inferno:  makeStopsPalette([[0,[0,0,4]],[0.25,[60,15,113]],[0.5,[186,54,85]],[0.75,[251,135,32]],[1,[252,255,164]]]),
  viridis:  makeStopsPalette([[0,[68,1,84]],[0.25,[59,82,139]],[0.5,[33,144,141]],[0.75,[93,201,99]],[1,[253,231,37]]]),
  plasma:   makeStopsPalette([[0,[13,8,135]],[0.25,[126,3,168]],[0.5,[204,71,120]],[0.75,[248,148,65]],[1,[240,249,33]]]),
  magma:    makeStopsPalette([[0,[0,0,4]],[0.25,[80,18,123]],[0.5,[183,55,121]],[0.75,[251,136,97]],[1,[252,253,191]]]),
  turbo:    makeStopsPalette([[0,[48,18,59]],[0.2,[64,131,236]],[0.4,[44,219,189]],[0.6,[166,252,80]],[0.8,[253,166,49]],[1,[122,4,3]]]),
  grayscale:makeStopsPalette([[0,[0,0,0]],[1,[255,255,255]]]),
  cyberpunk:makeStopsPalette([[0,[10,5,40]],[0.33,[140,40,200]],[0.66,[255,60,170]],[1,[0,240,255]]]),
  aurora:   makeStopsPalette([[0,[5,15,40]],[0.4,[0,170,120]],[0.7,[0,220,230]],[1,[180,255,255]]]),
  fire:     makeStopsPalette([[0,[0,0,0]],[0.3,[120,0,0]],[0.6,[240,80,0]],[0.85,[255,210,40]],[1,[255,255,240]]]),
  ice:      makeStopsPalette([[0,[5,10,40]],[0.5,[40,120,200]],[1,[240,255,255]]]),
  rainbow:  makeStopsPalette([[0,[148,0,211]],[0.2,[0,0,255]],[0.4,[0,255,0]],[0.6,[255,255,0]],[0.8,[255,127,0]],[1,[255,0,0]]]),
};

function buildLUT(paletteName) {
  const fn = PALETTES[paletteName] || PALETTES.default;
  const lut = new Uint8Array(256 * 4);
  for (let i = 0; i < 256; i++) {
    const [r, g, b] = fn(i / 255);
    lut[i*4+0] = r; lut[i*4+1] = g; lut[i*4+2] = b; lut[i*4+3] = 255;
  }
  return lut;
}

// ---- Mercator projection ---------------------------------------------------
// Standard Web Mercator EPSG:3857, projected to unit "world pixels" at zoom 0
// (the world is 256 px square), then scaled into the output bbox.

function mercX(lng) { return (lng + 180) / 360; }
function mercY(lat) {
  const s = Math.sin(lat * Math.PI / 180);
  const y = 0.5 - Math.log((1 + s) / Math.max(1e-9, 1 - s)) / (4 * Math.PI);
  return y;
}

// Build a projection function: latlng → [px, py] inside output canvas.
function buildProjection({ minLat, maxLat, minLng, maxLng, width, height, padding }) {
  // Pad bbox so tracks aren't flush against the edge.
  const padFrac = padding / 100;
  const latPad = (maxLat - minLat) * padFrac;
  const lngPad = (maxLng - minLng) * padFrac;
  const lat0 = minLat - latPad, lat1 = maxLat + latPad;
  const lng0 = minLng - lngPad, lng1 = maxLng + lngPad;

  // Project bbox corners to mercator unit space.
  const x0 = mercX(lng0), x1 = mercX(lng1);
  const yTop = mercY(lat1), yBot = mercY(lat0);
  const mw = x1 - x0, mh = yBot - yTop;

  // Fit while preserving aspect ratio: scale by the more constrained axis.
  const sx = width / mw, sy = height / mh;
  const s = Math.min(sx, sy);
  const renderW = mw * s, renderH = mh * s;
  const offX = (width - renderW) / 2;
  const offY = (height - renderH) / 2;

  function project(lat, lng) {
    const px = (mercX(lng) - x0) * s + offX;
    const py = (mercY(lat) - yTop) * s + offY;
    return [px, py];
  }

  // Inverse: pixel → (lat, lng) — useful for tile fetch.
  function unproject(px, py) {
    const mx = (px - offX) / s + x0;
    const my = (py - offY) / s + yTop;
    const lng = mx * 360 - 180;
    const n = Math.PI - 2 * Math.PI * my;
    const lat = (180 / Math.PI) * Math.atan(0.5 * (Math.exp(n) - Math.exp(-n)));
    return [lat, lng];
  }

  return { project, unproject, bbox: { lat0, lat1, lng0, lng1 }, scale: s, mercatorOrigin: { x0, yTop } };
}

// ---- value normalisation ---------------------------------------------------

function pickValue(p, channel, traceIdx, tFrac) {
  switch (channel) {
    case 'cps':     return p.cps;
    case 'speed':   return p.spd;
    case 'alt':     return p.alt;
    case 'hdop':    return p.hdop;
    case 'accM':    return p.accM;
    case 'time':    return tFrac;
    case 'session': return traceIdx;
    default:        return p.uSv;
  }
}

function autoRange(points, channel) {
  if (channel === 'session' || channel === 'time') return null;
  const vals = [];
  for (const p of points) {
    const v = channel === 'cps' ? p.cps
           : channel === 'speed' ? p.spd
           : channel === 'alt' ? p.alt
           : channel === 'hdop' ? p.hdop
           : channel === 'accM' ? p.accM
           : p.uSv;
    if (typeof v === 'number' && isFinite(v)) vals.push(v);
  }
  if (vals.length < 8) return null;
  vals.sort((a, b) => a - b);
  return { lo: vals[Math.floor(vals.length * 0.02)],
           hi: vals[Math.floor(vals.length * 0.98)] };
}

function applyScale(val, lo, hi, mode) {
  if (val == null || !isFinite(val)) return 0;
  let t;
  if (mode === 'log') {
    const v = Math.max(val, lo);
    t = Math.log(v - lo + 1) / Math.max(1e-9, Math.log(hi - lo + 1));
  } else if (mode === 'sqrt') {
    t = Math.sqrt(Math.max(0, val - lo) / Math.max(1e-9, hi - lo));
  } else {
    t = (val - lo) / Math.max(1e-9, hi - lo);
  }
  return Math.max(0, Math.min(1, t));
}

// ---- tile basemap fetcher --------------------------------------------------
// Computes the smallest Mercator zoom level where the bbox is at least
// `width` pixels wide in tile space, fetches all tiles in parallel (with a
// concurrency cap), and stamps them into the target canvas via the same
// projection used for points.

async function drawTileBasemap(ctx, proj, width, height, tileOpt, opacity, onProgress) {
  if (!tileOpt || tileOpt.key === 'none') return;
  const { bbox } = proj;
  // Choose zoom so that bbox spans roughly `width` device pixels.
  const widthFrac = mercX(bbox.lng1) - mercX(bbox.lng0);
  let zoom = Math.floor(Math.log2(width / 256 / Math.max(widthFrac, 1e-9)));
  zoom = Math.max(0, Math.min(tileOpt.maxZoom, zoom));
  const n = Math.pow(2, zoom);

  const xMin = Math.floor(mercX(bbox.lng0) * n);
  const xMax = Math.floor(mercX(bbox.lng1) * n);
  const yMin = Math.floor(mercY(bbox.lat1) * n);
  const yMax = Math.floor(mercY(bbox.lat0) * n);

  const tiles = [];
  for (let x = xMin; x <= xMax; x++) {
    for (let y = yMin; y <= yMax; y++) {
      if (y < 0 || y >= n) continue;
      const wrappedX = ((x % n) + n) % n;
      tiles.push({ x, y, wrappedX });
    }
  }

  // Concurrency-limited fetch.
  const CONCURRENT = 6;
  let idx = 0, done = 0, failed = 0;
  ctx.globalAlpha = opacity;
  async function worker() {
    while (idx < tiles.length) {
      const t = tiles[idx++];
      const url = tileOpt.url
        .replace('{z}', zoom).replace('{x}', t.wrappedX).replace('{y}', t.y);
      try {
        const img = await loadImage(url);
        // Project tile corners to canvas pixels via mercator.
        const lng0 = (t.x / n) * 360 - 180;
        const lng1 = ((t.x + 1) / n) * 360 - 180;
        const nLatTop = Math.PI - 2 * Math.PI * t.y / n;
        const lat0 = (180/Math.PI) * Math.atan(0.5 * (Math.exp(nLatTop) - Math.exp(-nLatTop)));
        const nLatBot = Math.PI - 2 * Math.PI * (t.y + 1) / n;
        const lat1 = (180/Math.PI) * Math.atan(0.5 * (Math.exp(nLatBot) - Math.exp(-nLatBot)));
        const [px0, py0] = proj.project(lat0, lng0);
        const [px1, py1] = proj.project(lat1, lng1);
        ctx.drawImage(img, Math.floor(px0), Math.floor(py0),
                      Math.ceil(px1 - px0), Math.ceil(py1 - py0));
      } catch (err) {
        failed++;
        if (failed <= 3) console.warn('[render] tile failed:', url, err && err.message);
      }
      done++;
      if (done % 4 === 0) onProgress(done / tiles.length);
    }
  }
  const workers = [];
  for (let i = 0; i < CONCURRENT; i++) workers.push(worker());
  await Promise.all(workers);
  ctx.globalAlpha = 1;
  console.log(`[render] tiles: ${tiles.length - failed}/${tiles.length} loaded @ z=${zoom} (${tileOpt.label})`);
  if (failed === tiles.length && tiles.length > 0) {
    throw new Error(`All ${tiles.length} basemap tiles failed to load. Check browser console / CORS / network.`);
  }
}

function loadImage(url) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.onload  = () => resolve(img);
    img.onerror = () => reject(new Error('img load failed'));
    img.src = url;
  });
}

// ---- yield helper ----------------------------------------------------------
function yieldToBrowser() {
  return new Promise(r => setTimeout(r, 0));
}

// ---- renderers -------------------------------------------------------------

async function renderTracks(ctx, traces, opts, proj, onProgress) {
  const { lineWidth, opacity, composite, lut, scaleLo, scaleHi, scaleMode,
          channel, glow, glowSize } = opts;
  ctx.globalCompositeOperation = composite;
  ctx.globalAlpha = opacity;
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';

  // For glow: draw an under-stroke at larger width + lower alpha.
  const passes = glow ? [{ widthMult: glowSize, alphaMult: 0.35 }, { widthMult: 1, alphaMult: 1 }]
                      : [{ widthMult: 1, alphaMult: 1 }];

  let totalPts = 0;
  for (const t of traces) totalPts += t.points.length;
  let done = 0;

  for (const pass of passes) {
    for (const t of traces) {
      const pts = t.points;
      const span = Math.max(1, t.tEnd - t.tStart);
      for (let i = 1; i < pts.length; i++) {
        const a = pts[i-1], b = pts[i];
        if (b.gapBefore) continue;
        const val = pickValue(b, channel, t.idx, (b.ts - t.tStart) / span);
        const ti = applyScale(val, scaleLo, scaleHi, scaleMode);
        const c = Math.floor(ti * 255);
        const r = lut[c*4], g = lut[c*4+1], bl = lut[c*4+2];
        ctx.strokeStyle = `rgba(${r},${g},${bl},${opacity * pass.alphaMult})`;
        ctx.lineWidth = lineWidth * pass.widthMult;
        const [x0, y0] = proj.project(a.lat, a.lng);
        const [x1, y1] = proj.project(b.lat, b.lng);
        ctx.beginPath();
        ctx.moveTo(x0, y0);
        ctx.lineTo(x1, y1);
        ctx.stroke();
        done++;
        if ((done & 4095) === 0) {
          onProgress(done / (totalPts * passes.length));
          await yieldToBrowser();
        }
      }
    }
  }
  ctx.globalAlpha = 1;
  ctx.globalCompositeOperation = 'source-over';
}

async function renderDots(ctx, traces, opts, proj, onProgress) {
  const { dotRadius, opacity, composite, lut, scaleLo, scaleHi, scaleMode, channel } = opts;
  ctx.globalCompositeOperation = composite;
  ctx.globalAlpha = opacity;

  let totalPts = 0;
  for (const t of traces) totalPts += t.points.length;
  let done = 0;

  for (const t of traces) {
    const pts = t.points;
    const span = Math.max(1, t.tEnd - t.tStart);
    for (const p of pts) {
      const val = pickValue(p, channel, t.idx, (p.ts - t.tStart) / span);
      const ti = applyScale(val, scaleLo, scaleHi, scaleMode);
      const c = Math.floor(ti * 255);
      const r = lut[c*4], g = lut[c*4+1], bl = lut[c*4+2];
      const [x, y] = proj.project(p.lat, p.lng);
      ctx.fillStyle = `rgba(${r},${g},${bl},${opacity})`;
      ctx.beginPath();
      ctx.arc(x, y, dotRadius, 0, Math.PI * 2);
      ctx.fill();
      done++;
      if ((done & 8191) === 0) {
        onProgress(done / totalPts);
        await yieldToBrowser();
      }
    }
  }
  ctx.globalAlpha = 1;
  ctx.globalCompositeOperation = 'source-over';
}

async function renderHex(ctx, traces, opts, proj, width, height, onProgress) {
  const { hexSize, opacity, lut, scaleMode, channel, hexBorder, hexLabels } = opts;
  // Bin by flat-top hex coords in canvas pixel space.
  const S3 = Math.sqrt(3);
  const bins = new Map();
  let totalPts = 0;
  for (const t of traces) totalPts += t.points.length;
  let scanned = 0;
  for (const t of traces) {
    const span = Math.max(1, t.tEnd - t.tStart);
    for (const p of t.points) {
      const [px, py] = proj.project(p.lat, p.lng);
      if (px < -hexSize || px > width + hexSize || py < -hexSize || py > height + hexSize) {
        scanned++; continue;
      }
      const q_f = (2/3 * px) / hexSize;
      const r_f = (-1/3 * px + S3/3 * py) / hexSize;
      const s_f = -q_f - r_f;
      let q = Math.round(q_f), r = Math.round(r_f), s = Math.round(s_f);
      const dq = Math.abs(q - q_f), dr = Math.abs(r - r_f), ds = Math.abs(s - s_f);
      if (dq > dr && dq > ds) q = -r - s;
      else if (dr > ds)       r = -q - s;
      const val = pickValue(p, channel, t.idx, (p.ts - t.tStart) / span);
      const key = `${q},${r}`;
      let bin = bins.get(key);
      if (!bin) { bin = { q, r, sum: 0, count: 0, max: -Infinity }; bins.set(key, bin); }
      if (typeof val === 'number' && isFinite(val)) {
        bin.sum += val; bin.count++;
        if (val > bin.max) bin.max = val;
      } else {
        bin.count++;
      }
      scanned++;
      if ((scanned & 16383) === 0) {
        onProgress(scanned / totalPts * 0.7);
        await yieldToBrowser();
      }
    }
  }

  // Auto-fit colour from bin averages.
  let maxAvg = 1e-9;
  for (const b of bins.values()) {
    const a = b.count ? b.sum / b.count : 0;
    if (a > maxAvg) maxAvg = a;
  }

  ctx.globalAlpha = opacity;
  let drawn = 0;
  const drawR = hexSize * 0.94;
  for (const b of bins.values()) {
    const cx = hexSize * 1.5 * b.q;
    const cy = hexSize * S3 * (b.r + b.q/2);
    const avg = b.count ? b.sum / b.count : 0;
    const t = applyScale(avg, 0, maxAvg, scaleMode);
    const c = Math.floor(t * 255);
    const r = lut[c*4], g = lut[c*4+1], bl = lut[c*4+2];
    ctx.fillStyle = `rgba(${r},${g},${bl},${opacity})`;
    ctx.beginPath();
    for (let i = 0; i < 6; i++) {
      const a = (Math.PI / 3) * i;
      const vx = cx + drawR * Math.cos(a);
      const vy = cy + drawR * Math.sin(a);
      i === 0 ? ctx.moveTo(vx, vy) : ctx.lineTo(vx, vy);
    }
    ctx.closePath();
    ctx.fill();
    if (hexBorder) {
      ctx.strokeStyle = 'rgba(255,255,255,0.25)';
      ctx.lineWidth = 0.8;
      ctx.stroke();
    }
    if (hexLabels && b.count > 1 && drawR >= 18) {
      ctx.fillStyle = '#fff';
      ctx.font = `bold ${Math.max(9, Math.round(drawR * 0.36))}px sans-serif`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(b.count > 9999 ? '9k+' : String(b.count), cx, cy);
    }
    drawn++;
    if ((drawn & 1023) === 0) {
      onProgress(0.7 + 0.3 * drawn / bins.size);
      await yieldToBrowser();
    }
  }
  ctx.globalAlpha = 1;
}

// Heatmap: accumulate intensity into a float grid (downsampled by kernel size
// for speed), apply box blur, map through palette. Linear/log/sqrt scaling.
async function renderHeatmap(ctx, traces, opts, proj, width, height, onProgress) {
  const { kernelRadius, opacity, lut, scaleMode, intensity } = opts;
  const cell = Math.max(1, Math.floor(kernelRadius));
  const gw = Math.ceil(width / cell);
  const gh = Math.ceil(height / cell);
  const grid = new Float32Array(gw * gh);

  let totalPts = 0;
  for (const t of traces) totalPts += t.points.length;
  let done = 0;

  for (const t of traces) {
    for (const p of t.points) {
      const [px, py] = proj.project(p.lat, p.lng);
      if (px < 0 || px >= width || py < 0 || py >= height) { done++; continue; }
      const gx = Math.floor(px / cell), gy = Math.floor(py / cell);
      grid[gy * gw + gx] += intensity;
      done++;
      if ((done & 16383) === 0) {
        onProgress(done / totalPts * 0.5);
        await yieldToBrowser();
      }
    }
  }

  // Two-pass box blur (separable) for cheap Gaussian-ish smoothing.
  const blurRadius = Math.max(1, Math.round(kernelRadius / cell));
  const tmp = new Float32Array(gw * gh);
  for (let y = 0; y < gh; y++) {
    for (let x = 0; x < gw; x++) {
      let s = 0, n = 0;
      for (let k = -blurRadius; k <= blurRadius; k++) {
        const xx = x + k;
        if (xx >= 0 && xx < gw) { s += grid[y * gw + xx]; n++; }
      }
      tmp[y * gw + x] = s / n;
    }
  }
  for (let x = 0; x < gw; x++) {
    for (let y = 0; y < gh; y++) {
      let s = 0, n = 0;
      for (let k = -blurRadius; k <= blurRadius; k++) {
        const yy = y + k;
        if (yy >= 0 && yy < gh) { s += tmp[yy * gw + x]; n++; }
      }
      grid[y * gw + x] = s / n;
    }
  }
  onProgress(0.7);
  await yieldToBrowser();

  // Find max for normalisation.
  let maxV = 1e-9;
  for (let i = 0; i < grid.length; i++) if (grid[i] > maxV) maxV = grid[i];

  // Produce ImageData at grid resolution, then drawImage-scale to canvas.
  const imgd = ctx.createImageData(gw, gh);
  for (let i = 0; i < grid.length; i++) {
    const ti = applyScale(grid[i], 0, maxV, scaleMode);
    if (ti <= 0) {
      imgd.data[i*4+3] = 0;
      continue;
    }
    const c = Math.floor(ti * 255);
    imgd.data[i*4+0] = lut[c*4+0];
    imgd.data[i*4+1] = lut[c*4+1];
    imgd.data[i*4+2] = lut[c*4+2];
    imgd.data[i*4+3] = Math.floor(255 * opacity * ti);
  }
  const off = document.createElement('canvas');
  off.width = gw; off.height = gh;
  off.getContext('2d').putImageData(imgd, 0, 0);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(off, 0, 0, width, height);
  onProgress(1);
}

// Gaussian splat: draw a radial-gradient disc at every point, additively
// composited. Produces a smooth bloomy density field. Slower than heatmap
// but no resolution loss.
async function renderSplat(ctx, traces, opts, proj, onProgress) {
  const { splatRadius, opacity, lut, scaleMode, channel, scaleLo, scaleHi } = opts;
  ctx.globalCompositeOperation = 'lighter';
  let totalPts = 0;
  for (const t of traces) totalPts += t.points.length;
  let done = 0;
  for (const t of traces) {
    const span = Math.max(1, t.tEnd - t.tStart);
    for (const p of t.points) {
      const [x, y] = proj.project(p.lat, p.lng);
      const val = pickValue(p, channel, t.idx, (p.ts - t.tStart) / span);
      const ti = applyScale(val, scaleLo, scaleHi, scaleMode);
      const c = Math.floor(ti * 255);
      const r = lut[c*4], g = lut[c*4+1], bl = lut[c*4+2];
      const grd = ctx.createRadialGradient(x, y, 0, x, y, splatRadius);
      grd.addColorStop(0, `rgba(${r},${g},${bl},${opacity})`);
      grd.addColorStop(1, `rgba(${r},${g},${bl},0)`);
      ctx.fillStyle = grd;
      ctx.beginPath();
      ctx.arc(x, y, splatRadius, 0, Math.PI * 2);
      ctx.fill();
      done++;
      if ((done & 2047) === 0) {
        onProgress(done / totalPts);
        await yieldToBrowser();
      }
    }
  }
  ctx.globalCompositeOperation = 'source-over';
}

// ---- post-processing -------------------------------------------------------

function applyVignette(ctx, width, height, strength) {
  const g = ctx.createRadialGradient(width/2, height/2, Math.min(width,height) * 0.3,
                                     width/2, height/2, Math.max(width,height) * 0.75);
  g.addColorStop(0, 'rgba(0,0,0,0)');
  g.addColorStop(1, `rgba(0,0,0,${strength})`);
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, width, height);
}

function applyGrain(ctx, width, height, amount) {
  const imgd = ctx.getImageData(0, 0, width, height);
  const d = imgd.data;
  for (let i = 0; i < d.length; i += 4) {
    const n = (Math.random() - 0.5) * amount * 255;
    d[i]   = Math.max(0, Math.min(255, d[i]   + n));
    d[i+1] = Math.max(0, Math.min(255, d[i+1] + n));
    d[i+2] = Math.max(0, Math.min(255, d[i+2] + n));
  }
  ctx.putImageData(imgd, 0, 0);
}

function drawTitleOverlay(ctx, width, height, title, subtitle, paletteName, scaleLo, scaleHi, channel) {
  const pad = Math.round(Math.min(width, height) * 0.025);
  ctx.save();
  // shadow box
  const boxH = Math.round(Math.min(width, height) * 0.08);
  const boxW = Math.round(width * 0.45);
  ctx.fillStyle = 'rgba(0,0,0,0.55)';
  ctx.fillRect(pad, height - pad - boxH, boxW, boxH);
  ctx.fillStyle = '#fff';
  ctx.font = `bold ${Math.round(boxH * 0.35)}px sans-serif`;
  ctx.textBaseline = 'top';
  ctx.fillText(title || 'Radiological Map', pad * 1.4, height - pad - boxH + pad * 0.4);
  ctx.font = `${Math.round(boxH * 0.18)}px sans-serif`;
  ctx.fillStyle = '#cfd2d8';
  ctx.fillText(subtitle || '', pad * 1.4, height - pad - boxH + pad * 0.4 + boxH * 0.45);

  // colour bar (top-right)
  const barW = Math.round(width * 0.18), barH = Math.round(boxH * 0.18);
  const bx = width - pad - barW, by = pad + barH * 1.5;
  const lut = buildLUT(paletteName);
  const imgd = ctx.createImageData(256, 1);
  for (let i = 0; i < 256; i++) {
    imgd.data[i*4+0] = lut[i*4+0];
    imgd.data[i*4+1] = lut[i*4+1];
    imgd.data[i*4+2] = lut[i*4+2];
    imgd.data[i*4+3] = 255;
  }
  const off = document.createElement('canvas');
  off.width = 256; off.height = 1;
  off.getContext('2d').putImageData(imgd, 0, 0);
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(off, bx, by, barW, barH);
  ctx.strokeStyle = '#fff';
  ctx.lineWidth = 1.2;
  ctx.strokeRect(bx, by, barW, barH);
  ctx.font = `${Math.round(barH * 0.95)}px sans-serif`;
  ctx.fillStyle = '#fff';
  ctx.textAlign = 'left';
  ctx.fillText(formatScale(scaleLo, channel), bx, by + barH + 2);
  ctx.textAlign = 'right';
  ctx.fillText(formatScale(scaleHi, channel), bx + barW, by + barH + 2);
  ctx.textAlign = 'center';
  ctx.font = `bold ${Math.round(barH * 1.0)}px sans-serif`;
  ctx.fillText(channelLabel(channel), bx + barW / 2, by - barH * 1.1);
  ctx.restore();
}

function channelLabel(ch) {
  const m = COLOR_CHANNELS.find(c => c.key === ch);
  return m ? m.label : ch;
}
function formatScale(v, ch) {
  if (typeof v !== 'number' || !isFinite(v)) return '';
  if (ch === 'dose')  return v.toFixed(3);
  if (ch === 'cps')   return v.toFixed(0);
  if (ch === 'speed') return v.toFixed(0);
  if (ch === 'alt')   return v.toFixed(0) + 'm';
  if (ch === 'hdop')  return v.toFixed(1);
  if (ch === 'accM')  return v.toFixed(0) + 'm';
  return String(v);
}

// ---- main component --------------------------------------------------------

export default function RenderPanel({ sessions, rowsBySession, onRowsLoaded }) {
  // -- track selection --
  const [chosen, setChosen] = useState(new Set());
  const [search, setSearch] = useState('');
  const [dateFrom, setDateFrom] = useState('');
  const [dateTo, setDateTo]     = useState('');

  // -- output --
  const [width, setWidth]   = useState(3840);
  const [height, setHeight] = useState(2160);
  const [paddingPct, setPaddingPct] = useState(4);
  const [aspectKey, setAspectKey]   = useState('16:9');

  // -- track gap splitting (for old files without GPS_LOST event rows) --
  const [splitFarPoints, setSplitFarPoints] = useState(true);
  const [maxGapMeters, setMaxGapMeters]     = useState(500);

  // -- mode & colour --
  const [mode, setMode]               = useState('track');
  const [channel, setChannel]         = useState('dose');
  const [palette, setPalette]         = useState('inferno');
  const [scaleMode, setScaleMode]     = useState('linear'); // linear | log | sqrt
  const [autoScale, setAutoScale]     = useState(true);
  const [scaleLo, setScaleLo]         = useState(0);
  const [scaleHi, setScaleHi]         = useState(1);
  const [composite, setComposite]     = useState('source-over');

  // -- style --
  const [lineWidth, setLineWidth]     = useState(2);
  const [opacity, setOpacity]         = useState(0.85);
  const [dotRadius, setDotRadius]     = useState(3);
  const [hexSize, setHexSize]         = useState(48);
  const [hexBorder, setHexBorder]     = useState(true);
  const [hexLabels, setHexLabels]     = useState(false);
  const [kernelRadius, setKernelRadius] = useState(20);
  const [intensity, setIntensity]     = useState(1.0);
  const [splatRadius, setSplatRadius] = useState(24);
  const [glow, setGlow]               = useState(false);
  const [glowSize, setGlowSize]       = useState(3);

  // -- background / effects --
  const [bgKey, setBgKey]             = useState('dark');
  const [tileKey, setTileKey]         = useState('dark');
  const [tileOpacity, setTileOpacity] = useState(0.75);
  const [vignette, setVignette]       = useState(0);
  const [grain, setGrain]             = useState(0);

  // -- overlays --
  const [showTitle, setShowTitle]     = useState(true);
  const [title, setTitle]             = useState('Radiological Map');
  const [subtitle, setSubtitle]       = useState('');

  // -- render state --
  const [renderState, setRenderState] = useState('idle'); // idle | preparing | rendering | done | error
  const [progress, setProgress]       = useState(0);
  const [statusMsg, setStatusMsg]     = useState('');
  const [error, setError]             = useState(null);
  const [renderBlobUrl, setRenderBlobUrl] = useState(null);
  // When the source canvas is too large for canvas.toBlob to encode in
  // one go (typically > ~268 MP), we slice it into NxM smaller tiles
  // (each <= TILE_MAX_PIXELS) and encode each separately. The tiles are
  // exposed via dedicated download buttons rather than a single PNG.
  const [renderTiles, setRenderTiles] = useState(null); // [{url, name, size, row, col, w, h}]
  const renderCanvasRef = useRef(null); // off-screen, holds the latest render
  const previewWrapRef  = useRef(null);
  const previewCanvasRef = useRef(null);

  // ---- session filtering ----

  const dateOptions = useMemo(() => {
    const set = new Set();
    for (const s of sessions) {
      if (s.firstTsMs && s.firstTsMs >= MIN_VALID_TS_MS) {
        set.add(new Date(s.firstTsMs).toISOString().slice(0, 10));
      }
    }
    return [...set].sort();
  }, [sessions]);

  const visibleSessions = useMemo(() => {
    const q = search.toLowerCase().trim();
    return sessions.filter(s => {
      if (q) {
        const n = (s.displayName || s.sessionId).toLowerCase();
        if (!n.includes(q) && !s.sessionId.toLowerCase().includes(q)) return false;
      }
      if (dateFrom || dateTo) {
        if (!s.firstTsMs) return false;
        const dKey = new Date(s.firstTsMs).toISOString().slice(0, 10);
        if (dateFrom && dKey < dateFrom) return false;
        if (dateTo   && dKey > dateTo)   return false;
      }
      return true;
    });
  }, [sessions, search, dateFrom, dateTo]);

  const selectAllVisible = () => {
    const n = new Set(chosen);
    for (const s of visibleSessions) n.add(s.sessionId);
    setChosen(n);
  };
  const clearAll = () => setChosen(new Set());
  const toggleOne = (id) => {
    const n = new Set(chosen);
    n.has(id) ? n.delete(id) : n.add(id);
    setChosen(n);
  };

  // Estimated point count (from session metadata; rows may not be loaded yet).
  const estPointCount = useMemo(() => {
    let n = 0;
    for (const id of chosen) {
      const s = sessions.find(x => x.sessionId === id);
      if (s && s.samples) n += s.samples;
    }
    return n;
  }, [chosen, sessions]);

  // ---- preset apply ----
  function applySizePreset(idx) {
    if (idx == null) return;
    const p = SIZE_PRESETS[idx];
    setWidth(p.w); setHeight(p.h);
    // Snap aspect dropdown to the closest matching ratio so the lock
    // doesn't immediately rewrite the height the user just chose.
    const r = p.w / p.h;
    let best = 'free', diff = Infinity;
    for (const a of ASPECT_RATIOS) {
      if (a.ratio == null) continue;
      const d = Math.abs(a.ratio - r);
      if (d < diff) { diff = d; best = a.key; }
    }
    setAspectKey(diff < 0.01 ? best : 'free');
  }

  // ---- aspect-locked width/height handlers ----
  // When an aspect ratio is selected (anything but 'free') the dimensions
  // are locked: typing in one box rewrites the other. Lets the user crank
  // width up to 16384 without doing the divide-by-aspect mental math.
  const aspectRatio = useMemo(() => {
    const a = ASPECT_RATIOS.find(x => x.key === aspectKey);
    return a ? a.ratio : null;
  }, [aspectKey]);

  function updateWidth(w) {
    const n = Math.max(128, Math.min(32768, parseInt(w) || 1024));
    setWidth(n);
    if (aspectRatio) setHeight(Math.max(128, Math.round(n / aspectRatio)));
  }
  function updateHeight(h) {
    const n = Math.max(128, Math.min(32768, parseInt(h) || 1024));
    setHeight(n);
    if (aspectRatio) setWidth(Math.max(128, Math.round(n * aspectRatio)));
  }
  function updateAspect(key) {
    setAspectKey(key);
    const a = ASPECT_RATIOS.find(x => x.key === key);
    if (a && a.ratio) {
      // Keep width; recompute height to match new ratio.
      setHeight(Math.max(128, Math.round(width / a.ratio)));
    }
  }

  const pixelCount = width * height;
  const memMb = (pixelCount * 4 / 1024 / 1024).toFixed(0);
  const tooBigToPreview = pixelCount > PREVIEW_MAX_PIXELS;
  const sizeWarn = pixelCount > MASSIVE_WARN_PIXELS
    ? `${memMb} MB - extreme size. The browser may refuse to allocate the canvas; if rendering fails, drop to <=16K. PNG will auto-download (preview disabled).`
    : pixelCount > HUGE_WARN_PIXELS
      ? `${memMb} MB - very large. Render may take minutes and consume significant RAM. PNG will auto-download (preview disabled).`
      : tooBigToPreview
        ? `${memMb} MB - PNG will auto-download because it's too large to show inline (>${(PREVIEW_MAX_PIXELS/1e6).toFixed(0)} MP).`
        : pixelCount > 4096 * 4096
          ? `${memMb} MB - large render, may take a minute.`
          : `${memMb} MB target`;

  // ---- the big render ----

  const runRender = useCallback(async () => {
    setError(null);
    setRenderState('preparing');
    setProgress(0);
    setStatusMsg('Loading session data...');

    try {
      if (chosen.size === 0) throw new Error('No tracks selected');
      if (pixelCount > MASSIVE_WARN_PIXELS) {
        console.warn(`[render] EXTREME size requested: ${width}x${height} (${memMb} MB). Browser may refuse.`);
      }

      // 1. Ensure all chosen sessions have rows loaded.
      const ids = [...chosen];
      const newRows = {};
      let loaded = 0;
      for (const id of ids) {
        let rows = rowsBySession[id];
        if (!rows) {
          const raw = await fetchSessionRows(id);
          rows = raw
            .filter(r => r.timestampMs != null && r.timestampMs >= MIN_VALID_TS_MS)
            .map(r => ({
              ts: r.timestampMs, lat: r.latitude, lng: r.longitude,
              uSv: r.uSvPerHour, cps: r.cps,
              spd: r.speedKph ?? null, brg: r.bearingDeg ?? null,
              alt: r.altitudeM ?? null, hdop: r.hdop ?? null,
              accM: r.accuracyM ?? null, event: r.event ?? null,
            }));
          newRows[id] = rows;
        }
        loaded++;
        setProgress(loaded / ids.length * 0.1);
        setStatusMsg(`Loading ${loaded}/${ids.length} sessions...`);
      }
      if (Object.keys(newRows).length && onRowsLoaded) onRowsLoaded(newRows);
      const allRows = { ...rowsBySession, ...newRows };

      // 2. Build traces with gap markers + bbox.
      let minLat =  Infinity, maxLat = -Infinity;
      let minLng =  Infinity, maxLng = -Infinity;
      const traces = [];
      let tIdx = 0;
      for (const id of ids) {
        const rows = allRows[id];
        if (!rows) { tIdx++; continue; }
        const points = [];
        let tStart = Infinity, tEnd = -Infinity;
        let pendingGap = false;
        let prevLat = null, prevLng = null;
        for (const r of rows) {
          if (r.event === 'GPS_LOST')     { pendingGap = true; continue; }
          if (r.event)                    { continue; }
          if (r.lat == null || r.lng == null) continue;
          if (r.lat === 0 && r.lng === 0) continue;
          const p = { ...r };
          // Distance-based gap: break the polyline when two consecutive
          // fixes are absurdly far apart (typical of a GPS outage on
          // pre-v0.7.0 firmware without explicit GPS_LOST event rows).
          if (splitFarPoints && prevLat != null) {
            const dM = haversineMeters(prevLat, prevLng, r.lat, r.lng);
            if (dM > maxGapMeters) pendingGap = true;
          }
          if (pendingGap) { p.gapBefore = true; pendingGap = false; }
          points.push(p);
          prevLat = r.lat; prevLng = r.lng;
          if (r.lat < minLat) minLat = r.lat;
          if (r.lat > maxLat) maxLat = r.lat;
          if (r.lng < minLng) minLng = r.lng;
          if (r.lng > maxLng) maxLng = r.lng;
          if (r.ts < tStart) tStart = r.ts;
          if (r.ts > tEnd)   tEnd   = r.ts;
        }
        if (points.length) traces.push({ id, points, idx: tIdx, tStart, tEnd });
        tIdx++;
      }
      if (!traces.length) throw new Error('Selected tracks have no geo-located points');

      // Auto-fit colour scale.
      let lo = scaleLo, hi = scaleHi;
      if (autoScale) {
        // Concat all points (sample down for speed if huge).
        const flat = [];
        for (const t of traces) for (const p of t.points) flat.push(p);
        const sampled = flat.length > 200000
          ? flat.filter((_, i) => i % Math.ceil(flat.length / 200000) === 0)
          : flat;
        const r = autoRange(sampled, channel);
        if (r) {
          lo = r.lo; hi = r.hi;
          setScaleLo(parseFloat(lo.toFixed(4)));
          setScaleHi(parseFloat(hi.toFixed(4)));
        } else if (channel === 'session') {
          lo = 0; hi = Math.max(1, traces.length - 1);
        } else if (channel === 'time') {
          lo = 0; hi = 1;
        }
      }

      // 3. Build canvas + projection.
      const canvas = document.createElement('canvas');
      canvas.width = width; canvas.height = height;
      // Verify the browser actually honoured the requested dimensions.
      // Chrome silently caps individual canvas dimensions at 32767 and
      // total area at ~268 MP, downscaling without telling you. If we
      // detect that, abort with an actionable message so the projection
      // doesn't go off the rails.
      if (canvas.width !== width || canvas.height !== height) {
        throw new Error(`Browser refused ${width}x${height} canvas (clamped to ${canvas.width}x${canvas.height}). ` +
          `Reduce dimensions -- Chrome caps individual sides at 32767 and area at ~268 MP.`);
      }
      const ctx = canvas.getContext('2d');
      if (!ctx) throw new Error('Failed to acquire 2D context -- canvas too large for this browser.');
      const bg = BG_OPTIONS.find(b => b.key === bgKey);
      if (bg && bg.color) { ctx.fillStyle = bg.color; ctx.fillRect(0, 0, width, height); }
      const proj = buildProjection({ minLat, maxLat, minLng, maxLng, width, height, padding: paddingPct });

      // 4. Tile basemap (optional).
      const tileOpt = TILE_OPTIONS.find(t => t.key === tileKey);
      if (tileOpt && tileOpt.key !== 'none') {
        setStatusMsg('Fetching basemap tiles...');
        setRenderState('rendering');
        await drawTileBasemap(ctx, proj, width, height, tileOpt, tileOpacity,
          (p) => setProgress(0.1 + p * 0.2));
      }

      // 5. Render points.
      setStatusMsg(`Rendering ${traces.length} tracks (${estPointCount.toLocaleString()} pts)...`);
      setRenderState('rendering');
      const lut = buildLUT(palette);
      const opts = {
        lineWidth, opacity, composite, lut,
        scaleLo: lo, scaleHi: hi, scaleMode, channel,
        dotRadius, hexSize, hexBorder, hexLabels,
        kernelRadius, intensity, splatRadius, glow, glowSize,
      };
      const baseProgress = (tileOpt && tileOpt.key !== 'none') ? 0.3 : 0.1;
      const setSub = (p) => setProgress(baseProgress + p * (0.9 - baseProgress));

      if (mode === 'track')        await renderTracks(ctx, traces, opts, proj, setSub);
      else if (mode === 'dots')    await renderDots(ctx, traces, opts, proj, setSub);
      else if (mode === 'hex')     await renderHex(ctx, traces, opts, proj, width, height, setSub);
      else if (mode === 'heatmap') await renderHeatmap(ctx, traces, opts, proj, width, height, setSub);
      else if (mode === 'splat')   await renderSplat(ctx, traces, opts, proj, setSub);

      // 6. Post-processing.
      setStatusMsg('Applying post-processing...');
      setProgress(0.92);
      await yieldToBrowser();
      if (vignette > 0) applyVignette(ctx, width, height, vignette);
      if (grain > 0)    applyGrain(ctx, width, height, grain);
      if (showTitle) {
        const sub = subtitle || buildAutoSubtitle(traces);
        drawTitleOverlay(ctx, width, height, title, sub, palette, lo, hi, channel);
      }

      // 7. Done -- convert to blob URL for preview + download.
      setStatusMsg('Finalising PNG...');
      setProgress(0.98);
      await yieldToBrowser();

      // Clean up any previous tile download URLs.
      if (renderTiles) {
        for (const t of renderTiles) URL.revokeObjectURL(t.url);
        setRenderTiles(null);
      }

      // Try a single-shot encode first. Browsers silently return null
      // from toBlob when the image is too large to encode (Chrome's
      // PNG codec roughly caps at ~268 MP/1 GB total).
      let blob = null;
      try {
        blob = await new Promise(r => canvas.toBlob(r, 'image/png'));
      } catch (_) { blob = null; }

      if (blob) {
        if (renderBlobUrl) URL.revokeObjectURL(renderBlobUrl);
        const url = URL.createObjectURL(blob);
        // For renders too large to safely show in an <img> (browsers cap
        // image element backing-store memory), skip the inline preview
        // and just trigger an immediate download.
        const autoDownload = pixelCount > PREVIEW_MAX_PIXELS;
        if (autoDownload) {
          setRenderBlobUrl(null);
          const a = document.createElement('a');
          a.href = url;
          const ts = new Date().toISOString().replace(/[:.]/g, '-');
          a.download = `radmap_${width}x${height}_${mode}_${palette}_${ts}.png`;
          document.body.appendChild(a);
          a.click();
          a.remove();
          setTimeout(() => URL.revokeObjectURL(url), 30_000);
        } else {
          setRenderBlobUrl(url);
        }
        renderCanvasRef.current = canvas;
        setProgress(1);
        setRenderState('done');
        setStatusMsg(autoDownload
          ? `Rendered ${width}x${height} (${(blob.size / 1048576).toFixed(1)} MB) - auto-downloaded.`
          : `Rendered ${width}x${height}, ${(blob.size / 1048576).toFixed(1)} MB`);
      } else {
        // ----- TILE FALLBACK -----
        // PNG codec refused. Carve the image up into <= 8192x8192 tiles,
        // encode each, and expose them as a grid of download buttons.
        // The user can stitch them later with ImageMagick / Photoshop /
        // any image editor that supports a contact-sheet workflow.
        setStatusMsg('Image too large to encode in one PNG -- splitting into tiles...');
        const TILE_MAX = 8192;
        const cols = Math.ceil(width  / TILE_MAX);
        const rows = Math.ceil(height / TILE_MAX);
        const tileW = Math.ceil(width  / cols);
        const tileH = Math.ceil(height / rows);
        const ts = new Date().toISOString().replace(/[:.]/g, '-');
        const baseName = `radmap_${width}x${height}_${mode}_${palette}_${ts}`;
        const tiles = [];
        let n = 0;
        const total = rows * cols;
        const tileCanvas = document.createElement('canvas');
        for (let r = 0; r < rows; r++) {
          for (let c = 0; c < cols; c++) {
            const sx = c * tileW;
            const sy = r * tileH;
            const sw = Math.min(tileW, width  - sx);
            const sh = Math.min(tileH, height - sy);
            tileCanvas.width = sw;
            tileCanvas.height = sh;
            if (tileCanvas.width !== sw || tileCanvas.height !== sh) {
              console.warn('[render] tile canvas was also clamped', sw, sh, '->', tileCanvas.width, tileCanvas.height);
            }
            const tctx = tileCanvas.getContext('2d');
            tctx.clearRect(0, 0, sw, sh);
            tctx.drawImage(canvas, sx, sy, sw, sh, 0, 0, sw, sh);
            const tBlob = await new Promise(rs => tileCanvas.toBlob(rs, 'image/png'));
            if (!tBlob) {
              console.warn(`[render] tile r${r}c${c} also failed to encode`);
              n++;
              setProgress(0.98 + (n / total) * 0.02);
              continue;
            }
            const url = URL.createObjectURL(tBlob);
            tiles.push({
              url, size: tBlob.size,
              name: `${baseName}_tile_r${r+1}c${c+1}_of_r${rows}c${cols}.png`,
              row: r + 1, col: c + 1, w: sw, h: sh,
            });
            n++;
            setStatusMsg(`Encoding tile ${n}/${total}...`);
            setProgress(0.98 + (n / total) * 0.02);
            await yieldToBrowser();
          }
        }
        if (!tiles.length) {
          throw new Error('Image too large -- even split into 8K tiles, the browser refused to encode. Try a smaller size.');
        }
        setRenderBlobUrl(null);
        setRenderTiles(tiles);
        renderCanvasRef.current = canvas;
        setProgress(1);
        setRenderState('done');
        const totalMb = tiles.reduce((a, t) => a + t.size, 0) / 1048576;
        setStatusMsg(`Rendered ${width}x${height} as ${rows}x${cols} tiles (${totalMb.toFixed(1)} MB total). Use the tile buttons to download.`);
      }
    } catch (e) {
      console.error(e);
      setError(String(e.message || e));
      setRenderState('error');
      setStatusMsg('');
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [chosen, rowsBySession, mode, channel, palette, scaleMode, autoScale,
      scaleLo, scaleHi, composite, lineWidth, opacity, dotRadius, hexSize,
      hexBorder, hexLabels, kernelRadius, intensity, splatRadius, glow,
      glowSize, bgKey, tileKey, tileOpacity, vignette, grain, showTitle,
      title, subtitle, width, height, paddingPct, pixelCount, estPointCount,
      splitFarPoints, maxGapMeters]);

  function buildAutoSubtitle(traces) {
    if (!traces.length) return '';
    let lo = Infinity, hi = -Infinity, pts = 0;
    for (const t of traces) {
      pts += t.points.length;
      if (t.tStart < lo) lo = t.tStart;
      if (t.tEnd   > hi) hi = t.tEnd;
    }
    const d0 = new Date(lo).toISOString().slice(0, 10);
    const d1 = new Date(hi).toISOString().slice(0, 10);
    return `${traces.length} tracks · ${pts.toLocaleString()} pts · ${d0} → ${d1}`;
  }

  // ---- preview pan/zoom ----
  // Display the blob URL inside an <img> with CSS transform.
  const [viewScale, setViewScale] = useState(1);
  const [viewX, setViewX] = useState(0);
  const [viewY, setViewY] = useState(0);

  useEffect(() => {
    // Fit-to-window when a new render lands.
    if (!renderBlobUrl || !previewWrapRef.current) return;
    const wrap = previewWrapRef.current;
    const fitS = Math.min(wrap.clientWidth / width, wrap.clientHeight / height);
    setViewScale(fitS || 1);
    setViewX(0); setViewY(0);
  }, [renderBlobUrl, width, height]);

  const dragRef = useRef(null);
  function onPreviewMouseDown(e) {
    dragRef.current = { x: e.clientX, y: e.clientY, vx: viewX, vy: viewY };
    e.preventDefault();
  }
  function onPreviewMouseMove(e) {
    if (!dragRef.current) return;
    setViewX(dragRef.current.vx + (e.clientX - dragRef.current.x));
    setViewY(dragRef.current.vy + (e.clientY - dragRef.current.y));
  }
  function onPreviewMouseUp() { dragRef.current = null; }
  function onPreviewWheel(e) {
    e.preventDefault();
    const wrap = previewWrapRef.current;
    const rect = wrap.getBoundingClientRect();
    const cx = e.clientX - rect.left;
    const cy = e.clientY - rect.top;
    const k = e.deltaY < 0 ? 1.15 : 1 / 1.15;
    const nextS = Math.max(0.02, Math.min(20, viewScale * k));
    // Zoom about cursor: keep canvas point under cursor stationary.
    const wx = (cx - viewX - wrap.clientWidth / 2) / viewScale;
    const wy = (cy - viewY - wrap.clientHeight / 2) / viewScale;
    setViewX(cx - wrap.clientWidth / 2 - wx * nextS);
    setViewY(cy - wrap.clientHeight / 2 - wy * nextS);
    setViewScale(nextS);
  }
  function resetView() {
    if (!previewWrapRef.current) return;
    const wrap = previewWrapRef.current;
    const fitS = Math.min(wrap.clientWidth / width, wrap.clientHeight / height);
    setViewScale(fitS || 1);
    setViewX(0); setViewY(0);
  }
  function zoom100() {
    setViewScale(1); setViewX(0); setViewY(0);
  }
  function savePng() {
    if (!renderBlobUrl) return;
    const a = document.createElement('a');
    a.href = renderBlobUrl;
    const ts = new Date().toISOString().replace(/[:T]/g, '-').slice(0, 19);
    a.download = `radmap_${width}x${height}_${mode}_${palette}_${ts}.png`;
    a.click();
  }

  return (
    <div className="render-view">

      {/* ===== SETUP COLUMN ===== */}
      <aside className="render-setup">
        <SectionHead>Tracks</SectionHead>
        <div className="rp-row">
          <input className="rp-input" placeholder="Search by name or id..."
            value={search} onChange={e => setSearch(e.target.value)} />
        </div>
        <div className="rp-row">
          <label className="rp-mini-label">From
            <input type="date" className="rp-input" value={dateFrom}
              onChange={e => setDateFrom(e.target.value)}
              min={dateOptions[0]} max={dateOptions[dateOptions.length-1]} />
          </label>
          <label className="rp-mini-label">To
            <input type="date" className="rp-input" value={dateTo}
              onChange={e => setDateTo(e.target.value)}
              min={dateOptions[0]} max={dateOptions[dateOptions.length-1]} />
          </label>
        </div>
        <div className="rp-row">
          <button className="rp-btn" onClick={selectAllVisible}>Select visible ({visibleSessions.length})</button>
          <button className="rp-btn" onClick={clearAll}>Clear</button>
        </div>
        <div className="rp-track-list">
          {visibleSessions.map((s, i) => {
            const isSel = chosen.has(s.sessionId);
            const date = s.firstTsMs && s.firstTsMs >= MIN_VALID_TS_MS
              ? new Date(s.firstTsMs).toLocaleDateString() : '';
            return (
              <label key={s.sessionId} className={`rp-track${isSel ? ' sel' : ''}`}>
                <input type="checkbox" checked={isSel}
                  onChange={() => toggleOne(s.sessionId)} />
                <span className="rp-swatch" style={{ background: sessionColor(i) }} />
                <span className="rp-tname">{s.displayName || s.sessionId}</span>
                <span className="rp-tmeta">{(s.samples ?? 0).toLocaleString()} pts · {date}</span>
              </label>
            );
          })}
        </div>
        <div className="rp-summary">
          <b>{chosen.size}</b> tracks selected ·{' '}
          <b>{estPointCount.toLocaleString()}</b> points
        </div>

        <SectionHead>Output Size</SectionHead>
        <div className="rp-presets">
          {SIZE_PRESETS.map((p, i) => (
            <button key={p.label}
              className={`rp-preset${width === p.w && height === p.h ? ' active' : ''}`}
              onClick={() => applySizePreset(i)}>
              {p.label}<small>{p.w}×{p.h}</small>
            </button>
          ))}
        </div>
        <div className="rp-row">
          <label className="rp-mini-label">Width
            <input type="number" className="rp-input" value={width} min={128} max={32768}
              onChange={e => updateWidth(e.target.value)} />
          </label>
          <label className="rp-mini-label">Height
            <input type="number" className="rp-input" value={height} min={128} max={32768}
              onChange={e => updateHeight(e.target.value)} />
          </label>
        </div>
        <div className="rp-row">
          <label className="rp-mini-label" style={{ flex: 1 }}>Aspect lock
            <select className="rp-input" value={aspectKey} onChange={e => updateAspect(e.target.value)}>
              {ASPECT_RATIOS.map(a => <option key={a.key} value={a.key}>{a.label}</option>)}
            </select>
          </label>
        </div>
        <Slider label="Bbox padding" value={paddingPct} min={0} max={25} step={1}
          onChange={setPaddingPct} fmt={v => `${v}%`} />
        <div className={`rp-warn${pixelCount > MASSIVE_WARN_PIXELS ? ' bad' : ''}`}>{sizeWarn}</div>

        <SectionHead>Track Gaps</SectionHead>
        <label className="rp-check">
          <input type="checkbox" checked={splitFarPoints}
            onChange={e => setSplitFarPoints(e.target.checked)} />
          <span>Break lines across long jumps</span>
        </label>
        {splitFarPoints && (
          <Slider label="Max gap distance" value={maxGapMeters}
            min={50} max={5000} step={50}
            onChange={setMaxGapMeters}
            fmt={v => v >= 1000 ? `${(v/1000).toFixed(1)} km` : `${v} m`} />
        )}
        <div className="rp-mini-label" style={{ opacity: 0.7, fontSize: 11 }}>
          Old files (pre-v0.7.0 firmware) drew straight lines across GPS
          outages. Enable this to break the polyline when two consecutive
          fixes are absurdly far apart. Newer files use GPS_LOST event
          rows automatically.
        </div>

        <SectionHead>Render Mode</SectionHead>
        <div className="rp-grid2">
          {RENDER_MODES.map(m => (
            <button key={m.key}
              className={`rp-pill${mode === m.key ? ' active' : ''}`}
              onClick={() => setMode(m.key)}>{m.label}</button>
          ))}
        </div>

        <SectionHead>Color</SectionHead>
        <select className="rp-input" value={channel} onChange={e => setChannel(e.target.value)}>
          {COLOR_CHANNELS.map(c => <option key={c.key} value={c.key}>{c.label}</option>)}
        </select>
        <div className="rp-palette-grid">
          {Object.keys(PALETTES).map(k => (
            <button key={k}
              className={`rp-palette${palette === k ? ' active' : ''}`}
              onClick={() => setPalette(k)}
              style={{ background: paletteGradientCss(k) }}>
              <span>{k}</span>
            </button>
          ))}
        </div>
        <div className="rp-row">
          {['linear','log','sqrt'].map(m => (
            <button key={m}
              className={`rp-pill${scaleMode === m ? ' active' : ''}`}
              onClick={() => setScaleMode(m)}>{m}</button>
          ))}
        </div>
        <label className="rp-check">
          <input type="checkbox" checked={autoScale}
            onChange={e => setAutoScale(e.target.checked)} />
          Auto-fit colour scale to data
        </label>
        {!autoScale && (
          <div className="rp-row">
            <label className="rp-mini-label">Lo
              <input type="number" className="rp-input" value={scaleLo} step="any"
                onChange={e => setScaleLo(parseFloat(e.target.value))} />
            </label>
            <label className="rp-mini-label">Hi
              <input type="number" className="rp-input" value={scaleHi} step="any"
                onChange={e => setScaleHi(parseFloat(e.target.value))} />
            </label>
          </div>
        )}
        <label className="rp-mini-label">Compositing
          <select className="rp-input" value={composite}
            onChange={e => setComposite(e.target.value)}>
            {COMPOSITE_MODES.map(c => <option key={c.key} value={c.key}>{c.label}</option>)}
          </select>
        </label>

        <SectionHead>Style</SectionHead>
        {mode === 'track' && (<>
          <Slider label="Line width" value={lineWidth} min={0.5} max={20} step={0.5}
            onChange={setLineWidth} fmt={v => `${v}px`} />
          <Slider label="Opacity" value={opacity} min={0.05} max={1} step={0.05}
            onChange={setOpacity} fmt={v => `${Math.round(v*100)}%`} />
          <label className="rp-check">
            <input type="checkbox" checked={glow} onChange={e => setGlow(e.target.checked)} />
            Glow / bloom
          </label>
          {glow && (
            <Slider label="Glow size" value={glowSize} min={1.5} max={8} step={0.5}
              onChange={setGlowSize} fmt={v => `${v}×`} />
          )}
        </>)}
        {mode === 'dots' && (<>
          <Slider label="Dot radius" value={dotRadius} min={0.5} max={20} step={0.5}
            onChange={setDotRadius} fmt={v => `${v}px`} />
          <Slider label="Opacity" value={opacity} min={0.05} max={1} step={0.05}
            onChange={setOpacity} fmt={v => `${Math.round(v*100)}%`} />
        </>)}
        {mode === 'hex' && (<>
          <Slider label="Hex size" value={hexSize} min={8} max={200} step={1}
            onChange={setHexSize} fmt={v => `${v}px`} />
          <Slider label="Opacity" value={opacity} min={0.1} max={1} step={0.05}
            onChange={setOpacity} fmt={v => `${Math.round(v*100)}%`} />
          <label className="rp-check">
            <input type="checkbox" checked={hexBorder}
              onChange={e => setHexBorder(e.target.checked)} />Hex borders
          </label>
          <label className="rp-check">
            <input type="checkbox" checked={hexLabels}
              onChange={e => setHexLabels(e.target.checked)} />Count labels
          </label>
        </>)}
        {mode === 'heatmap' && (<>
          <Slider label="Kernel radius" value={kernelRadius} min={4} max={120} step={1}
            onChange={setKernelRadius} fmt={v => `${v}px`} />
          <Slider label="Intensity" value={intensity} min={0.1} max={5} step={0.1}
            onChange={setIntensity} fmt={v => `${v}×`} />
          <Slider label="Opacity" value={opacity} min={0.1} max={1} step={0.05}
            onChange={setOpacity} fmt={v => `${Math.round(v*100)}%`} />
        </>)}
        {mode === 'splat' && (<>
          <Slider label="Splat radius" value={splatRadius} min={4} max={120} step={1}
            onChange={setSplatRadius} fmt={v => `${v}px`} />
          <Slider label="Opacity" value={opacity} min={0.02} max={0.5} step={0.02}
            onChange={setOpacity} fmt={v => `${Math.round(v*100)}%`} />
        </>)}

        <SectionHead>Background</SectionHead>
        <div className="rp-grid3">
          {BG_OPTIONS.map(b => (
            <button key={b.key}
              className={`rp-pill${bgKey === b.key ? ' active' : ''}`}
              onClick={() => setBgKey(b.key)}>{b.label}</button>
          ))}
        </div>
        <label className="rp-mini-label">Tile basemap
          <select className="rp-input" value={tileKey}
            onChange={e => setTileKey(e.target.value)}>
            {TILE_OPTIONS.map(t => <option key={t.key} value={t.key}>{t.label}</option>)}
          </select>
        </label>
        {tileKey !== 'none' && (
          <Slider label="Tile opacity" value={tileOpacity} min={0.1} max={1} step={0.05}
            onChange={setTileOpacity} fmt={v => `${Math.round(v*100)}%`} />
        )}

        <SectionHead>Effects</SectionHead>
        <Slider label="Vignette" value={vignette} min={0} max={1} step={0.05}
          onChange={setVignette} fmt={v => v === 0 ? 'off' : `${Math.round(v*100)}%`} />
        <Slider label="Film grain" value={grain} min={0} max={0.3} step={0.01}
          onChange={setGrain} fmt={v => v === 0 ? 'off' : `${Math.round(v*100)}%`} />
        <label className="rp-check">
          <input type="checkbox" checked={showTitle}
            onChange={e => setShowTitle(e.target.checked)} />Title overlay + colour bar
        </label>
        {showTitle && (
          <>
            <input className="rp-input" value={title} onChange={e => setTitle(e.target.value)}
              placeholder="Title" />
            <input className="rp-input" value={subtitle} onChange={e => setSubtitle(e.target.value)}
              placeholder="Subtitle (auto if blank)" />
          </>
        )}

        <div className="rp-render-action">
          <button
            className="rp-render-btn"
            disabled={renderState === 'rendering' || renderState === 'preparing' || chosen.size === 0}
            onClick={runRender}>
            {renderState === 'rendering' || renderState === 'preparing'
              ? `Rendering... ${Math.round(progress * 100)}%`
              : `Render ${width}×${height}`}
          </button>
          {statusMsg && <div className="rp-status">{statusMsg}</div>}
          {error && <div className="rp-error">{error}</div>}
          {(renderState === 'rendering' || renderState === 'preparing') && (
            <div className="rp-progress"><div style={{ width: `${progress * 100}%` }} /></div>
          )}
        </div>
      </aside>

      {/* ===== PREVIEW COLUMN ===== */}
      <main className="render-preview">
        <div className="rp-preview-toolbar">
          <span className="rp-prev-label">
            {renderBlobUrl
              ? `${width}×${height} · zoom ${(viewScale * 100).toFixed(0)}%`
              : 'No render yet — configure on the left, then press Render'}
          </span>
          <div className="rp-prev-btns">
            <button onClick={resetView}    disabled={!renderBlobUrl}>Fit</button>
            <button onClick={zoom100}      disabled={!renderBlobUrl}>1:1</button>
            <button onClick={() => setViewScale(s => s * 1.25)} disabled={!renderBlobUrl}>＋</button>
            <button onClick={() => setViewScale(s => s / 1.25)} disabled={!renderBlobUrl}>－</button>
            <button className="rp-save-btn" onClick={savePng} disabled={!renderBlobUrl}>Save PNG</button>
          </div>
        </div>
        <div
          ref={previewWrapRef}
          className="rp-preview-wrap"
          onMouseDown={onPreviewMouseDown}
          onMouseMove={onPreviewMouseMove}
          onMouseUp={onPreviewMouseUp}
          onMouseLeave={onPreviewMouseUp}
          onWheel={onPreviewWheel}>
          {renderBlobUrl && (
            <img
              src={renderBlobUrl}
              alt="render"
              draggable={false}
              style={{
                position: 'absolute',
                left: '50%', top: '50%',
                width:  width,
                height: height,
                transform: `translate(-50%, -50%) translate(${viewX}px, ${viewY}px) scale(${viewScale})`,
                transformOrigin: 'center',
                imageRendering: viewScale > 2 ? 'pixelated' : 'auto',
                userSelect: 'none', pointerEvents: 'none',
              }}
            />
          )}
          {!renderBlobUrl && renderTiles && (
            <div className="rp-tiles-panel">
              <div className="rp-tiles-head">
                <div className="rp-tiles-title">Render too large to preview</div>
                <div className="rp-tiles-sub">
                  Image was split into <b>{renderTiles.length}</b> tiles
                  ({width}×{height} total). Download each tile below, or grab
                  them all at once. Stitch with ImageMagick:
                  <code> magick montage -tile {Math.max(...renderTiles.map(t => t.col))}x
                  -geometry +0+0 *_tile_*.png stitched.png</code>
                </div>
                <button className="rp-save-btn" onClick={() => {
                  for (const t of renderTiles) {
                    const a = document.createElement('a');
                    a.href = t.url; a.download = t.name;
                    document.body.appendChild(a); a.click(); a.remove();
                  }
                }}>
                  Download all {renderTiles.length} tiles
                </button>
              </div>
              <div className="rp-tiles-grid"
                style={{ gridTemplateColumns: `repeat(${Math.max(...renderTiles.map(t => t.col))}, 1fr)` }}>
                {renderTiles.map(t => (
                  <a key={t.name} href={t.url} download={t.name} className="rp-tile-btn">
                    <div className="rp-tile-rc">r{t.row} · c{t.col}</div>
                    <div className="rp-tile-dim">{t.w}×{t.h}</div>
                    <div className="rp-tile-size">{(t.size / 1048576).toFixed(1)} MB</div>
                  </a>
                ))}
              </div>
            </div>
          )}
          {!renderBlobUrl && !renderTiles && renderState !== 'rendering' && renderState !== 'preparing' && (
            <div className="rp-empty">
              <div className="rp-empty-icon">🖼</div>
              <div>Select tracks &amp; press <b>Render</b> to generate a PNG</div>
            </div>
          )}
        </div>
      </main>
    </div>
  );
}

// ---- small UI bits ---------------------------------------------------------

function SectionHead({ children }) {
  return <div className="rp-section-head">{children}</div>;
}

function Slider({ label, value, min, max, step, onChange, fmt }) {
  return (
    <div className="rp-slider">
      <div className="rp-slider-head">
        <span>{label}</span>
        <span className="rp-slider-val">{fmt ? fmt(value) : value}</span>
      </div>
      <input type="range" min={min} max={max} step={step} value={value}
        onChange={e => onChange(parseFloat(e.target.value))} />
    </div>
  );
}

function paletteGradientCss(name) {
  const fn = PALETTES[name];
  const stops = [];
  for (let i = 0; i <= 6; i++) {
    const t = i / 6;
    const [r, g, b] = fn(t);
    stops.push(`rgb(${r},${g},${b}) ${(t*100).toFixed(0)}%`);
  }
  return `linear-gradient(90deg, ${stops.join(', ')})`;
}
