/**
 * ThreeDView.jsx — WebGL 3D track viewer using Three.js.
 *
 * GPS tracks are rendered as 3D lines:
 *   X = East  (lng offset in metres from bbox centre)
 *   Y = Up     (altitude × exaggeration)
 *   Z = South  (lat offset, negated so North = −Z)
 *
 * When a slippy-map tile URL template is supplied, tiles are fetched and
 * rendered as textured PlaneGeometry meshes on the ground plane (Y = −1).
 * This gives the same basemap appearance as the 2D Leaflet map underneath.
 *
 * Controls: left-drag = orbit, right-drag = pan, scroll = zoom.
 */

import React, { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import {
  doseColor, cpsColor, speedColor, altColor, hdopColor, accColor, sessionColor,
} from './colors.js';

const DEG2RAD = Math.PI / 180;

// ---- Geo helpers ------------------------------------------------------------

// Equirectangular approximation — accurate enough for city-to-region scale.
function latM(dLat)       { return dLat * 111320; }
function lngM(dLng, cLat) { return dLng * 111320 * Math.cos(cLat * DEG2RAD); }

// Slippy tile index for a given lat/lng at zoom z.
function latLng2tile(lat, lng, z) {
  const n   = Math.pow(2, z);
  const tx  = Math.floor((lng + 180) / 360 * n);
  const latR = lat * DEG2RAD;
  const ty  = Math.floor((1 - Math.log(Math.tan(latR) + 1 / Math.cos(latR)) / Math.PI) / 2 * n);
  return { tx: Math.max(0, Math.min(n - 1, tx)), ty: Math.max(0, Math.min(n - 1, ty)) };
}

// NW corner lat/lng of a tile.
function tile2latLng(tx, ty, z) {
  const n    = Math.pow(2, z);
  const lng  = tx / n * 360 - 180;
  const latR = Math.atan(Math.sinh(Math.PI * (1 - 2 * ty / n)));
  return { lat: latR / DEG2RAD, lng };
}

// Pick the coarsest zoom level where the bbox spans ≤ maxTiles on each axis.
function chooseTileZoom(minLat, maxLat, minLng, maxLng, maxTiles = 8) {
  for (let z = 16; z >= 1; z--) {
    const nw = latLng2tile(maxLat, minLng, z);
    const se = latLng2tile(minLat, maxLng, z);
    if ((se.tx - nw.tx + 1) <= maxTiles && (se.ty - nw.ty + 1) <= maxTiles) return z;
  }
  return 2;
}

// Fill {s}/{z}/{x}/{y}/{r} tokens in a tile URL template.
function buildTileUrl(template, z, tx, ty) {
  const s = ['a', 'b', 'c'][(tx + ty) % 3];
  return template
    .replace('{s}', s)
    .replace('{z}', String(z))
    .replace('{x}', String(tx))
    .replace('{y}', String(ty))
    .replace('{r}', '');
}

// ---- Terrain elevation -------------------------------------------------------

// 32×32 = 1 024 quad faces per tile — enough detail without melting the GPU.
const TERRAIN_SEGS = 32;

// Fetch one AWS Terrarium tile and decode per-pixel elevation values.
// Terrarium encoding: elev_m = R×256 + G + B/256 − 32768
// Returns a Float32Array of 256×256 elevation values (metres, absolute).
async function decodeTerrariumTile(tx, ty, z, signal) {
  const url = `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/${z}/${tx}/${ty}.png`;
  const img = new Image();
  img.crossOrigin = 'anonymous';
  await new Promise((res, rej) => {
    if (signal?.aborted) return rej(new DOMException('aborted', 'AbortError'));
    img.onload  = res;
    img.onerror = () => rej(new Error('terrain fetch failed'));
    img.src = url;
  });
  if (signal?.aborted) throw new DOMException('aborted', 'AbortError');
  const cv  = document.createElement('canvas');
  cv.width  = cv.height = 256;
  const ctx = cv.getContext('2d');
  ctx.drawImage(img, 0, 0);
  const px   = ctx.getImageData(0, 0, 256, 256).data;
  const elev = new Float32Array(256 * 256);
  for (let i = 0; i < 256 * 256; i++)
    elev[i] = px[i * 4] * 256 + px[i * 4 + 1] + px[i * 4 + 2] / 256 - 32768;
  return elev;
}

// ---- Color helpers ----------------------------------------------------------

function channelColor(p, channel, traceIdx, ranges) {
  switch (channel) {
    case 'cps':     return cpsColor(p.cps,  ranges.cpsMin,  ranges.cpsMax);
    case 'speed':   return speedColor(p.spd, ranges.spdMin,  ranges.spdMax);
    case 'alt':     return altColor(p.alt,   ranges.altMin,  ranges.altMax);
    case 'hdop':    return hdopColor(p.hdop, ranges.hdopMin, ranges.hdopMax);
    case 'accM':    return accColor(p.accM,  ranges.accMin,  ranges.accMax);
    case 'session': return sessionColor(traceIdx);
    default:        return doseColor(p.uSv,  ranges.doseMin, ranges.doseMax);
  }
}

// ---- Track geometry builder -------------------------------------------------

function buildTrackObjects(filteredTraces, colorChannel, ranges, cLat, cLng, minAlt, altExag, showDropLines) {
  const objects = [];
  const tmpCol  = new THREE.Color();

  function toVec3(lat, lng, alt) {
    return new THREE.Vector3(
      lngM(lng - cLng, cLat),
      ((alt ?? minAlt) - minAlt) * altExag,
      -latM(lat - cLat),
    );
  }

  for (const t of filteredTraces) {
    if (!t.filtered || t.filtered.length < 2) continue;

    // Split at GPS gaps.
    const segs = [];
    let cur = [];
    for (const p of t.filtered) {
      if (p.lat == null || p.lng == null || (p.lat === 0 && p.lng === 0)) continue;
      if (p.gapBefore && cur.length > 0) { segs.push(cur); cur = []; }
      cur.push(p);
    }
    if (cur.length) segs.push(cur);

    for (const seg of segs) {
      if (seg.length < 2) continue;

      // Track line (vertex-colored).
      const verts = new Float32Array(seg.length * 3);
      const cols  = new Float32Array(seg.length * 3);
      for (let i = 0; i < seg.length; i++) {
        const p = seg[i];
        const v = toVec3(p.lat, p.lng, p.alt);
        verts[i * 3] = v.x; verts[i * 3 + 1] = v.y; verts[i * 3 + 2] = v.z;
        tmpCol.setStyle(channelColor(p, colorChannel, t.idx, ranges));
        cols[i * 3] = tmpCol.r; cols[i * 3 + 1] = tmpCol.g; cols[i * 3 + 2] = tmpCol.b;
      }
      const geo = new THREE.BufferGeometry();
      geo.setAttribute('position', new THREE.BufferAttribute(verts, 3));
      geo.setAttribute('color',    new THREE.BufferAttribute(cols,  3));
      objects.push(new THREE.Line(geo, new THREE.LineBasicMaterial({ vertexColors: true })));

      // Vertical drop lines (segment top → ground at Y = 0).
      if (showDropLines) {
        const dVerts = new Float32Array(seg.length * 6);
        const dCols  = new Float32Array(seg.length * 6);
        for (let i = 0; i < seg.length; i++) {
          const p = seg[i];
          const v = toVec3(p.lat, p.lng, p.alt);
          tmpCol.setStyle(channelColor(p, colorChannel, t.idx, ranges));
          dVerts[i*6]   = v.x; dVerts[i*6+1] = v.y; dVerts[i*6+2] = v.z;
          dCols [i*6]   = tmpCol.r * 0.5; dCols[i*6+1] = tmpCol.g * 0.5; dCols[i*6+2] = tmpCol.b * 0.5;
          dVerts[i*6+3] = v.x; dVerts[i*6+4] = 0;   dVerts[i*6+5] = v.z;
          dCols [i*6+3] = 0.07; dCols[i*6+4] = 0.09; dCols[i*6+5] = 0.12;
        }
        const dGeo = new THREE.BufferGeometry();
        dGeo.setAttribute('position', new THREE.BufferAttribute(dVerts, 3));
        dGeo.setAttribute('color',    new THREE.BufferAttribute(dCols,  3));
        objects.push(new THREE.LineSegments(dGeo,
          new THREE.LineBasicMaterial({ vertexColors: true, transparent: true, opacity: 0.4 })));
      }
    }
  }
  return objects;
}

// ---- Component --------------------------------------------------------------

export function ThreeDView({ filteredTraces, colorChannel, ranges, tileUrl }) {
  const containerRef = useRef(null);
  const canvasRef    = useRef(null);
  const threeRef     = useRef(null);
  const animFrameRef = useRef(null);
  const resetKeyRef  = useRef(null);

  const [altExag,       setAltExag]       = useState(5);
  const [showDropLines, setShowDropLines] = useState(false);
  const [showTiles,     setShowTiles]     = useState(true);
  const [showTerrain,   setShowTerrain]   = useState(true);
  const [showGrid,      setShowGrid]      = useState(false);
  const [noData,        setNoData]        = useState(false);
  const [pointCount,    setPointCount]    = useState(0);
  const [tileStatus,    setTileStatus]    = useState('');  // '' | 'loading' | 'N/M loaded'
  const [terrainStatus, setTerrainStatus] = useState('');  // '' | 'loading' | 'N/M loaded'

  // ---- One-time Three.js setup --------------------------------------------
  useEffect(() => {
    const canvas    = canvasRef.current;
    const container = containerRef.current;
    if (!canvas || !container) return;

    const w = Math.max(container.clientWidth,  400);
    const h = Math.max(container.clientHeight, 300);

    const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, powerPreference: 'high-performance' });
    renderer.setSize(w, h);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));

    const camera = new THREE.PerspectiveCamera(55, w / h, 1, 50_000_000);
    camera.position.set(2000, 1500, 3000);

    const controls = new OrbitControls(camera, canvas);
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.maxPolarAngle = Math.PI * 0.88;
    controls.zoomSpeed     = 1.2;
    controls.panSpeed      = 0.8;
    controls.mouseButtons  = {
      LEFT: THREE.MOUSE.ROTATE, MIDDLE: THREE.MOUSE.DOLLY, RIGHT: THREE.MOUSE.PAN,
    };

    threeRef.current = { renderer, camera, controls, scene: null };

    function animate() {
      animFrameRef.current = requestAnimationFrame(animate);
      controls.update();
      if (threeRef.current?.scene) renderer.render(threeRef.current.scene, camera);
    }
    animate();

    const ro = new ResizeObserver(() => {
      const w2 = container.clientWidth;
      const h2 = container.clientHeight;
      if (!w2 || !h2) return;
      renderer.setSize(w2, h2);
      camera.aspect = w2 / h2;
      camera.updateProjectionMatrix();
    });
    ro.observe(container);

    return () => {
      cancelAnimationFrame(animFrameRef.current);
      ro.disconnect();
      controls.dispose();
      renderer.dispose();
      threeRef.current = null;
    };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  // ---- Rebuild scene when data or display params change -------------------
  useEffect(() => {
    const three = threeRef.current;
    if (!three) return;
    const { camera, controls } = three;

    // Fresh abort controller for this render's tile fetches.
    const abort = new AbortController();

    // Collect all valid geo points.
    const allPts = [];
    for (const t of filteredTraces) {
      if (!t.filtered) continue;
      for (const p of t.filtered) {
        if (p.lat != null && p.lng != null && !(p.lat === 0 && p.lng === 0)) allPts.push(p);
      }
    }

    setPointCount(allPts.length);
    setNoData(allPts.length === 0);
    setTileStatus('');
    setTerrainStatus('');

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x0d1117);
    scene.fog = new THREE.FogExp2(0x0d1117, 0.0000015);
    three.scene = scene;

    if (allPts.length === 0) return () => abort.abort();

    // Geographic bounds & centre.
    let minLat = Infinity, maxLat = -Infinity;
    let minLng = Infinity, maxLng = -Infinity;
    let minAlt = Infinity, maxAlt = -Infinity;
    for (const p of allPts) {
      if (p.lat < minLat) minLat = p.lat; if (p.lat > maxLat) maxLat = p.lat;
      if (p.lng < minLng) minLng = p.lng; if (p.lng > maxLng) maxLng = p.lng;
      if (p.alt != null && isFinite(p.alt)) {
        if (p.alt < minAlt) minAlt = p.alt; if (p.alt > maxAlt) maxAlt = p.alt;
      }
    }
    if (!isFinite(minAlt)) { minAlt = 0; maxAlt = 0; }

    const cLat     = (minLat + maxLat) / 2;
    const cLng     = (minLng + maxLng) / 2;
    const altRange = maxAlt - minAlt;
    const adjExag  = altRange < 5 ? altExag * 20 : altExag;
    const spanX    = lngM(maxLng - minLng, cLat) || 1000;
    const spanZ    = latM(maxLat - minLat)        || 1000;

    // ---- Slippy-map tile ground plane -----------------------------------
    if (showTiles && tileUrl) {
      const PAD    = 0.35;
      const padLat = (maxLat - minLat) * PAD || 0.01;
      const padLng = (maxLng - minLng) * PAD || 0.01;
      const padMinLat = minLat - padLat, padMaxLat = maxLat + padLat;
      const padMinLng = minLng - padLng, padMaxLng = maxLng + padLng;

      const tileZ  = chooseTileZoom(padMinLat, padMaxLat, padMinLng, padMaxLng, 8);
      const nwTile = latLng2tile(padMaxLat, padMinLng, tileZ);
      const seTile = latLng2tile(padMinLat, padMaxLng, tileZ);

      const loader = new THREE.TextureLoader();
      const total  = (seTile.tx - nwTile.tx + 1) * (seTile.ty - nwTile.ty + 1);
      let loaded = 0;
      let terrainLoaded = 0;
      setTileStatus('loading');
      if (showTerrain) setTerrainStatus('loading');

      for (let tx = nwTile.tx; tx <= seTile.tx; tx++) {
        for (let ty = nwTile.ty; ty <= seTile.ty; ty++) {
          // NW & SE corners of this tile in world metres.
          const nw = tile2latLng(tx,     ty,     tileZ);
          const se = tile2latLng(tx + 1, ty + 1, tileZ);
          const tileW = Math.abs(lngM(se.lng - nw.lng, cLat));
          const tileH = Math.abs(latM(nw.lat - se.lat));
          // World-space centre of tile (X = East, Z = South).
          const wx =  lngM((nw.lng + se.lng) / 2 - cLng, cLat);
          const wz = -latM((nw.lat + se.lat) / 2 - cLat);

          const segs = showTerrain ? TERRAIN_SEGS : 1;
          const planeGeo = new THREE.PlaneGeometry(tileW, tileH, segs, segs);
          // PhongMaterial responds to the DirectionalLight → hillshading on displaced terrain.
          // BasicMaterial ignores all lights — used when terrain is off (flat plane).
          const mat = showTerrain
            ? new THREE.MeshPhongMaterial({
                color: new THREE.Color(0x111824),
                transparent: true,
                opacity: 0.90,
                side: THREE.FrontSide,
                shininess: 5,
              })
            : new THREE.MeshBasicMaterial({
                color: new THREE.Color(0x111824),
                transparent: true,
                opacity: 0.90,
                side: THREE.FrontSide,
              });
          const mesh = new THREE.Mesh(planeGeo, mat);
          mesh.rotation.x = -Math.PI / 2; // rotate to lie flat in the XZ ground plane
          mesh.position.set(wx, -1, wz);  // Y = −1: tiles sit just below the track lines
          scene.add(mesh);

          // Async: fetch Terrarium elevation tile and displace PlaneGeometry vertices.
          // PlaneGeometry is in the XY plane; after rotation.x=-PI/2, local Z→world Y,
          // so setZ() on a vertex moves it upward in world space.
          if (showTerrain) {
            const capGeo = planeGeo; // captured for async closure
            decodeTerrariumTile(tx, ty, tileZ, abort.signal).then(elevData => {
              if (abort.signal.aborted) return;
              const pos = capGeo.attributes.position;
              for (let vi = 0; vi < pos.count; vi++) {
                // PlaneGeometry vertex layout: row-major, row 0 = north (+y / -z after rotation).
                const col = vi % (TERRAIN_SEGS + 1);
                const row = Math.floor(vi / (TERRAIN_SEGS + 1));
                // u=0→west, v=0→north — matches Terrarium tile pixel layout (col 0=west, row 0=north).
                const px = Math.min(255, Math.floor((col / TERRAIN_SEGS) * 255.999));
                const py = Math.min(255, Math.floor((row / TERRAIN_SEGS) * 255.999));
                const elevM = elevData[py * 256 + px];
                pos.setZ(vi, (elevM - minAlt) * adjExag);
              }
              pos.needsUpdate = true;
              capGeo.computeVertexNormals();
              terrainLoaded++;
              if (!abort.signal.aborted) setTerrainStatus(`${terrainLoaded}/${total}`);
            }).catch(() => {
              // Count failures so the status counter still reaches N/N.
              terrainLoaded++;
              if (!abort.signal.aborted) setTerrainStatus(`${terrainLoaded}/${total}`);
            });
          }

          // Fetch the tile texture — crossOrigin is set to 'anonymous' by TextureLoader.
          const url = buildTileUrl(tileUrl, tileZ, tx, ty);
          loader.load(
            url,
            (tex) => {
              if (abort.signal.aborted) return;
              tex.colorSpace = THREE.SRGBColorSpace;
              mat.map = tex;
              mat.color.setHex(0xffffff); // white = no tint, show true tile colours
              mat.needsUpdate = true;
              loaded++;
              setTileStatus(`${loaded}/${total}`);
            },
            undefined, // onProgress
            () => {
              // On error (CORS, 404, etc.) leave the placeholder tile visible.
              if (abort.signal.aborted) return;
              loaded++;
              setTileStatus(`${loaded}/${total}`);
            },
          );
        }
      }
    } else if (showGrid) {
      // Fallback wire grid when tiles are disabled.
      const size = Math.max(spanX, spanZ) * 2.5;
      const divs = Math.min(40, Math.max(6, Math.floor(size / 400)));
      const gridA = new THREE.GridHelper(size, divs, 0x1a3050, 0x0f1e2a);
      gridA.position.y = -1;
      scene.add(gridA);
      const gridB = new THREE.GridHelper(size, divs * 4, 0x0a1520, 0x0a1520);
      gridB.position.y = -2;
      scene.add(gridB);
    }

    // ---- Track geometry -------------------------------------------------
    const trackObjects = buildTrackObjects(
      filteredTraces, colorChannel, ranges, cLat, cLng, minAlt, adjExag, showDropLines,
    );
    for (const o of trackObjects) scene.add(o);

    // Ambient light + directional sun for hillshading when terrain is active.
    // MeshBasicMaterial ignores lights, so the DirectionalLight only has visible
    // effect on tiles rendered with MeshPhongMaterial (i.e. when showTerrain=true).
    scene.add(new THREE.AmbientLight(0xffffff, 0.5));
    if (showTerrain && showTiles) {
      const sun = new THREE.DirectionalLight(0xffffff, 0.8);
      sun.position.set(-1, 2, 0.5); // NW + slight south: classic oblique hillshade angle
      scene.add(sun);
    }

    // N–S / E–W compass cross at ground level.
    const cs = Math.max(spanX, spanZ) * 0.65;
    const compassGeo = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(-cs, 0, 0), new THREE.Vector3(cs, 0, 0),
      new THREE.Vector3(0, 0, -cs), new THREE.Vector3(0, 0, cs),
    ]);
    scene.add(new THREE.LineSegments(compassGeo,
      new THREE.LineBasicMaterial({ color: 0x1a4060, transparent: true, opacity: 0.5 })));

    // ---- Camera fit (only when session selection changes) ---------------
    const sessionKey = filteredTraces.map(t => t.id).sort().join('|');
    if (resetKeyRef.current !== sessionKey) {
      resetKeyRef.current = sessionKey;
      const altSpan  = altRange * adjExag;
      const diameter = Math.max(spanX, spanZ, 200);
      camera.position.set(diameter * 0.55, diameter * 0.5 + altSpan * 0.4, diameter * 0.85);
      controls.target.set(0, altSpan * 0.2, 0);
      controls.update();
    }

    // Abort any in-flight tile fetches when the effect re-runs or component unmounts.
    return () => abort.abort();

  }, [filteredTraces, colorChannel, ranges, altExag, showDropLines, showTiles, showTerrain, showGrid, tileUrl]);

  // ---- HUD ----------------------------------------------------------------
  return (
    <div ref={containerRef} className="three-d-container">
      <canvas ref={canvasRef} className="three-d-canvas" />

      {noData && (
        <div className="three-d-empty">
          <div style={{ fontSize: 40, marginBottom: 12 }}>☢</div>
          <div>No track data visible</div>
          <div style={{ fontSize: 11, marginTop: 6, opacity: 0.5 }}>
            Select sessions in the Sessions tab to view tracks
          </div>
        </div>
      )}

      <div className="three-d-hud">
        <div className="three-d-hud-title">
          <span style={{ color: 'var(--accent)', marginRight: 6 }}>▲</span>
          3D — Altitude as Z axis
        </div>

        <div className="three-d-hud-row">
          <span className="three-d-hud-label">Alt ×{altExag}</span>
          <input type="range" min="1" max="100" value={altExag}
            className="three-d-hud-slider"
            onChange={e => setAltExag(Number(e.target.value))} />
        </div>

        <div className="three-d-hud-row">
          <label className="three-d-check">
            <input type="checkbox" checked={showDropLines}
              onChange={e => setShowDropLines(e.target.checked)} />
            <span>Drop lines</span>
          </label>
          <label className="three-d-check" style={{ marginLeft: 8 }}>
            <input type="checkbox" checked={showTiles}
              onChange={e => setShowTiles(e.target.checked)} />
            <span>Map tiles</span>
          </label>
          <label className="three-d-check" style={{ marginLeft: 8 }}>
            <input type="checkbox" checked={showTerrain} disabled={!showTiles}
              onChange={e => setShowTerrain(e.target.checked)} />
            <span style={{ opacity: showTiles ? 1 : 0.4 }}>Terrain</span>
          </label>
          <label className="three-d-check" style={{ marginLeft: 8 }}>
            <input type="checkbox" checked={showGrid}
              onChange={e => setShowGrid(e.target.checked)} />
            <span>Grid</span>
          </label>
        </div>

        {tileStatus && (
          <div className="three-d-hud-count"
            style={{ color: tileStatus === 'loading' ? 'var(--accent)' : '#4fc3f7' }}>
            {tileStatus === 'loading' ? 'Fetching tiles…' : `Tiles: ${tileStatus} loaded`}
          </div>
        )}
        {terrainStatus && (
          <div className="three-d-hud-count"
            style={{ color: terrainStatus === 'loading' ? '#ffb74d' : '#a5d6a7' }}>
            {terrainStatus === 'loading' ? 'Fetching terrain…' : `Terrain: ${terrainStatus} loaded`}
          </div>
        )}

        {pointCount > 0 && (
          <div className="three-d-hud-count">{pointCount.toLocaleString()} pts</div>
        )}

        <div className="three-d-hud-compass">
          <span style={{ color: '#4fc3f7' }}>N ↑</span>
          <span style={{ color: '#80cbc4' }}>E →</span>
          <span>Alt ↑ (Y)</span>
        </div>

        <div className="three-d-hud-hint">
          Drag orbit · Scroll zoom · Right-drag pan
        </div>
      </div>
    </div>
  );
}
