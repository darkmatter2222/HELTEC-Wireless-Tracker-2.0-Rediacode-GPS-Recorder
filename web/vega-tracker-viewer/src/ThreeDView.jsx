/**
 * ThreeDView.jsx — WebGL 3D track viewer using Three.js.
 *
 * GPS tracks are rendered as 3D lines:
 *   X = East (lng offset)
 *   Y = Up   (altitude * exaggeration)
 *   Z = South (lat offset, negated so North = -Z)
 *
 * Any color channel (dose, CPS, speed, altitude, HDOP,…) can be used for
 * vertex coloring — altitude is always the Z-axis regardless of channel.
 *
 * Controls: left-drag = orbit, right-drag / middle-drag = pan, scroll = zoom.
 */

import React, { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import {
  doseColor, cpsColor, speedColor, altColor, hdopColor, accColor, sessionColor,
} from './colors.js';

const DEG2RAD = Math.PI / 180;

// Equirectangular approximation — good enough for city-to-region scale tracks.
function latM(dLat)         { return dLat * 111320; }
function lngM(dLng, cLat)   { return dLng * 111320 * Math.cos(cLat * DEG2RAD); }

// Map a data point to a CSS color string using the chosen channel.
function channelColor(p, channel, traceIdx, ranges) {
  switch (channel) {
    case 'cps':     return cpsColor(p.cps,   ranges.cpsMin,  ranges.cpsMax);
    case 'speed':   return speedColor(p.spd,  ranges.spdMin,  ranges.spdMax);
    case 'alt':     return altColor(p.alt,    ranges.altMin,  ranges.altMax);
    case 'hdop':    return hdopColor(p.hdop,  ranges.hdopMin, ranges.hdopMax);
    case 'accM':    return accColor(p.accM,   ranges.accMin,  ranges.accMax);
    case 'session': return sessionColor(traceIdx);
    default:        return doseColor(p.uSv,   ranges.doseMin, ranges.doseMax);
  }
}

// Build all Three.js geometry objects for the current data + settings.
// Returns an array of Object3D to add to the scene.
function buildTrackObjects(filteredTraces, colorChannel, ranges, cLat, cLng, minAlt, altExag, showDropLines) {
  const objects = [];
  const tmpCol = new THREE.Color();

  function toVec3(lat, lng, alt) {
    return new THREE.Vector3(
      lngM(lng - cLng, cLat),
      ((alt ?? minAlt) - minAlt) * altExag,
      -latM(lat - cLat),
    );
  }

  for (const t of filteredTraces) {
    if (!t.filtered || t.filtered.length < 2) continue;

    // Split track at GPS gaps (gapBefore flag set by firmware GPS_LOST events).
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

      // --- Track line (vertex-colored) --------------------------------
      const verts = new Float32Array(seg.length * 3);
      const cols  = new Float32Array(seg.length * 3);
      for (let i = 0; i < seg.length; i++) {
        const p = seg[i];
        const v = toVec3(p.lat, p.lng, p.alt);
        verts[i * 3]     = v.x;
        verts[i * 3 + 1] = v.y;
        verts[i * 3 + 2] = v.z;
        tmpCol.setStyle(channelColor(p, colorChannel, t.idx, ranges));
        cols[i * 3]     = tmpCol.r;
        cols[i * 3 + 1] = tmpCol.g;
        cols[i * 3 + 2] = tmpCol.b;
      }
      const geo = new THREE.BufferGeometry();
      geo.setAttribute('position', new THREE.BufferAttribute(verts, 3));
      geo.setAttribute('color',    new THREE.BufferAttribute(cols,  3));
      const mat = new THREE.LineBasicMaterial({ vertexColors: true });
      objects.push(new THREE.Line(geo, mat));

      // --- Vertical drop lines (each point → ground at y=0) -----------
      if (showDropLines) {
        // Two vertices per point: (x, y, z) and (x, 0, z)
        const dVerts = new Float32Array(seg.length * 6);
        const dCols  = new Float32Array(seg.length * 6);
        for (let i = 0; i < seg.length; i++) {
          const p = seg[i];
          const v = toVec3(p.lat, p.lng, p.alt);
          tmpCol.setStyle(channelColor(p, colorChannel, t.idx, ranges));
          // Top of drop line
          dVerts[i * 6]     = v.x; dVerts[i * 6 + 1] = v.y; dVerts[i * 6 + 2] = v.z;
          dCols [i * 6]     = tmpCol.r * 0.5; dCols[i * 6 + 1] = tmpCol.g * 0.5; dCols[i * 6 + 2] = tmpCol.b * 0.5;
          // Ground foot
          dVerts[i * 6 + 3] = v.x; dVerts[i * 6 + 4] = 0;   dVerts[i * 6 + 5] = v.z;
          dCols [i * 6 + 3] = 0.07; dCols[i * 6 + 4] = 0.09; dCols[i * 6 + 5] = 0.12;
        }
        const dGeo = new THREE.BufferGeometry();
        dGeo.setAttribute('position', new THREE.BufferAttribute(dVerts, 3));
        dGeo.setAttribute('color',    new THREE.BufferAttribute(dCols,  3));
        const dMat = new THREE.LineBasicMaterial({ vertexColors: true, transparent: true, opacity: 0.4 });
        objects.push(new THREE.LineSegments(dGeo, dMat));
      }
    }
  }
  return objects;
}

// ---- Component ---------------------------------------------------------------

export function ThreeDView({ filteredTraces, colorChannel, ranges }) {
  const containerRef = useRef(null);
  const canvasRef    = useRef(null);
  const threeRef     = useRef(null); // persists renderer/camera/controls across geometry rebuilds
  const animFrameRef = useRef(null);
  const resetKeyRef  = useRef(null); // track which sessions were last camera-fitted

  const [altExag,      setAltExag]      = useState(5);
  const [showDropLines, setShowDropLines] = useState(false);
  const [showGround,   setShowGround]   = useState(true);
  const [noData,       setNoData]       = useState(false);
  const [pointCount,   setPointCount]   = useState(0);

  // ---- One-time Three.js setup (renderer, camera, controls, animation loop)
  useEffect(() => {
    const canvas    = canvasRef.current;
    const container = containerRef.current;
    if (!canvas || !container) return;

    const w = Math.max(container.clientWidth,  400);
    const h = Math.max(container.clientHeight, 300);

    const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, powerPreference: 'high-performance' });
    renderer.setSize(w, h);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));

    const camera = new THREE.PerspectiveCamera(55, w / h, 1, 10000000);
    camera.position.set(2000, 1500, 3000);

    const controls = new OrbitControls(camera, canvas);
    controls.enableDamping  = true;
    controls.dampingFactor  = 0.08;
    controls.maxPolarAngle  = Math.PI * 0.88;
    controls.zoomSpeed      = 1.2;
    controls.panSpeed       = 0.8;
    controls.mouseButtons   = {
      LEFT: THREE.MOUSE.ROTATE,
      MIDDLE: THREE.MOUSE.DOLLY,
      RIGHT: THREE.MOUSE.PAN,
    };

    threeRef.current = { renderer, camera, controls };

    function animate() {
      animFrameRef.current = requestAnimationFrame(animate);
      controls.update();
      // Scene is rebuilt by the geometry effect; just render whatever is current.
      if (threeRef.current?.scene) {
        renderer.render(threeRef.current.scene, camera);
      }
    }
    animate();

    // Responsive resize via ResizeObserver.
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

  // ---- Rebuild scene geometry when data or display params change
  useEffect(() => {
    const three = threeRef.current;
    if (!three) return;
    const { camera, controls } = three;

    // Collect all valid geo points across all visible traces.
    const allPts = [];
    for (const t of filteredTraces) {
      if (!t.filtered) continue;
      for (const p of t.filtered) {
        if (p.lat != null && p.lng != null && !(p.lat === 0 && p.lng === 0)) allPts.push(p);
      }
    }

    setPointCount(allPts.length);
    setNoData(allPts.length === 0);

    // Fresh scene for each rebuild.
    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x0d1117);
    // Subtle fog to give depth cues at extreme view distances.
    scene.fog = new THREE.FogExp2(0x0d1117, 0.000015);
    threeRef.current.scene = scene;

    if (allPts.length === 0) return;

    // ---- Geographic bounds & center --------------------------------
    let minLat = Infinity, maxLat = -Infinity;
    let minLng = Infinity, maxLng = -Infinity;
    let minAlt = Infinity, maxAlt = -Infinity;
    for (const p of allPts) {
      if (p.lat < minLat) minLat = p.lat;
      if (p.lat > maxLat) maxLat = p.lat;
      if (p.lng < minLng) minLng = p.lng;
      if (p.lng > maxLng) maxLng = p.lng;
      if (p.alt != null && isFinite(p.alt)) {
        if (p.alt < minAlt) minAlt = p.alt;
        if (p.alt > maxAlt) maxAlt = p.alt;
      }
    }
    if (!isFinite(minAlt)) { minAlt = 0; maxAlt = 0; }

    const cLat = (minLat + maxLat) / 2;
    const cLng = (minLng + maxLng) / 2;

    // When terrain is nearly flat (< 5 m variation), auto-boost exaggeration
    // so there is still *some* Z variation visible.
    const altRange  = maxAlt - minAlt;
    const adjExag   = altRange < 5 ? altExag * 20 : altExag;

    // ---- Build track geometry --------------------------------------
    const trackObjects = buildTrackObjects(
      filteredTraces, colorChannel, ranges, cLat, cLng, minAlt, adjExag, showDropLines,
    );
    for (const o of trackObjects) scene.add(o);

    // ---- Ground grid -----------------------------------------------
    if (showGround) {
      const spanX = lngM(maxLng - minLng, cLat) || 1000;
      const spanZ = latM(maxLat - minLat) || 1000;
      const size  = Math.max(spanX, spanZ) * 2.2;
      const divs  = Math.min(40, Math.max(6, Math.floor(size / 400)));
      // Coarse grid (major lines)
      const gridA = new THREE.GridHelper(size, divs, 0x1a3050, 0x0f1e2a);
      gridA.position.y = -1;
      scene.add(gridA);
      // Fine grid (minor lines)
      const gridB = new THREE.GridHelper(size, divs * 4, 0x0a1520, 0x0a1520);
      gridB.position.y = -2;
      scene.add(gridB);
    }

    // ---- Ambient light (makes any future 3D mesh objects look good) --
    scene.add(new THREE.AmbientLight(0xffffff, 0.6));

    // ---- Cardinal direction compass lines on the ground -------------
    // A thin cross shows N-S and E-W orientation at ground level.
    const spanX = lngM(maxLng - minLng, cLat) || 1000;
    const spanZ = latM(maxLat - minLat) || 1000;
    const compassSize = Math.max(spanX, spanZ) * 0.6;
    const compassGeo = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(-compassSize, 0, 0), new THREE.Vector3(compassSize, 0, 0), // E-W
      new THREE.Vector3(0, 0, -compassSize), new THREE.Vector3(0, 0, compassSize), // N-S
    ]);
    const compassMat = new THREE.LineBasicMaterial({ color: 0x1a4060, transparent: true, opacity: 0.6 });
    scene.add(new THREE.LineSegments(compassGeo, compassMat));

    // ---- Camera fit (only when the session selection changes) -------
    const sessionKey = filteredTraces.map(t => t.id).sort().join('|');
    if (resetKeyRef.current !== sessionKey) {
      resetKeyRef.current = sessionKey;
      const fitSpanX = spanX || 1000;
      const fitSpanZ = spanZ || 1000;
      const altSpan  = altRange * adjExag;
      const diameter = Math.max(fitSpanX, fitSpanZ, 200);
      // Isometric-ish initial angle: camera above and behind center.
      camera.position.set(diameter * 0.55, diameter * 0.5 + altSpan * 0.4, diameter * 0.85);
      controls.target.set(0, altSpan * 0.2, 0);
      controls.update();
    }
  }, [filteredTraces, colorChannel, ranges, altExag, showDropLines, showGround]);

  // ---- HUD ----------------------------------------------------------------
  return (
    <div ref={containerRef} className="three-d-container">
      <canvas ref={canvasRef} className="three-d-canvas" />

      {/* Empty-state overlay */}
      {noData && (
        <div className="three-d-empty">
          <div style={{ fontSize: 40, marginBottom: 12 }}>☢</div>
          <div>No track data visible</div>
          <div style={{ fontSize: 11, marginTop: 6, opacity: 0.5 }}>Select sessions in the Sessions tab to view tracks</div>
        </div>
      )}

      {/* HUD controls overlay */}
      <div className="three-d-hud">
        <div className="three-d-hud-title">
          <span style={{ color: 'var(--accent)', marginRight: 6 }}>▲</span>
          3D — Altitude as Z axis
        </div>

        {/* Altitude exaggeration */}
        <div className="three-d-hud-row">
          <span className="three-d-hud-label">Alt ×{altExag}</span>
          <input
            type="range" min="1" max="100" value={altExag}
            className="three-d-hud-slider"
            onChange={e => setAltExag(Number(e.target.value))}
          />
        </div>

        {/* Drop lines toggle */}
        <div className="three-d-hud-row">
          <label className="three-d-check">
            <input type="checkbox" checked={showDropLines}
              onChange={e => setShowDropLines(e.target.checked)} />
            <span>Drop lines</span>
          </label>
          <label className="three-d-check" style={{ marginLeft: 8 }}>
            <input type="checkbox" checked={showGround}
              onChange={e => setShowGround(e.target.checked)} />
            <span>Ground grid</span>
          </label>
        </div>

        {/* Point count */}
        {pointCount > 0 && (
          <div className="three-d-hud-count">{pointCount.toLocaleString()} points</div>
        )}

        {/* Compass legend */}
        <div className="three-d-hud-compass">
          <span style={{ color:'#4fc3f7' }}>N ↑</span>
          <span style={{ color:'#80cbc4' }}>E →</span>
          <span>Alt ↑ (Y)</span>
        </div>

        <div className="three-d-hud-hint">
          Drag orbit · Scroll zoom · Right-drag pan
        </div>
      </div>
    </div>
  );
}
