/**
 * LiveTrackingPanel.jsx
 *
 * Full-screen mobile-optimised live tracking overlay.
 * Shows:
 *   - A Leaflet map with the mission zone polygon highlighted
 *   - Existing radiation coverage (from pre-loaded session rows) as a
 *     lightweight dot overlay
 *   - A live GPS dot + trail updated via the browser watchPosition API
 *   - New dose readings polled from /explorer/live-samples every 10 s
 *   - A top bar (mission name + End button) and a bottom HUD
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  CircleMarker, MapContainer, Polygon, Polyline, TileLayer, Tooltip,
  useMap,
} from 'react-leaflet';
import { fetchLiveSamples } from './api.js';
import { doseColor } from './colors.js';

const POLL_INTERVAL_MS = 10_000;

// FitBounds helper rendered inside the map so it can access the Leaflet instance
function ZoneFit({ polygon }) {
  const map = useMap();
  useEffect(() => {
    if (!polygon) return;
    try {
      // polygon.coordinates[0] is array of [lng, lat]
      const bounds = polygon.coordinates[0].map(([lng, lat]) => [lat, lng]);
      map.fitBounds(bounds, { padding: [40, 40], maxZoom: 16 });
    } catch (_) {}
  }, [map, polygon]);
  return null;
}

function CenterOnUser({ position }) {
  const map = useMap();
  const centeredRef = useRef(false);
  useEffect(() => {
    if (position && !centeredRef.current) {
      map.setView(position, 16, { animate: true });
      centeredRef.current = true;
    }
  }, [map, position]);
  return null;
}

export function LiveTrackingPanel({ mission, allRows, onEnd }) {
  const [position, setPosition]   = useState(null);  // [lat, lng]
  const [trail, setTrail]         = useState([]);     // [[lat,lng], ...]
  const [accuracy, setAccuracy]   = useState(null);
  const [speed, setSpeed]         = useState(null);
  const [liveReadings, setLive]   = useState([]);     // newest-first from API
  const [gpsError, setGpsError]   = useState(null);
  const [sinceMs, setSinceMs]     = useState(() => Date.now() - 300_000); // 5 min history
  const watchRef  = useRef(null);
  const timerRef  = useRef(null);

  // GPS watchPosition
  useEffect(() => {
    if (!navigator.geolocation) {
      setGpsError('Geolocation not supported by this browser.');
      return;
    }
    watchRef.current = navigator.geolocation.watchPosition(
      pos => {
        const { latitude: lat, longitude: lng, accuracy: acc, speed: spd } = pos.coords;
        setPosition([lat, lng]);
        setAccuracy(acc != null ? Math.round(acc) : null);
        setSpeed(spd != null ? (spd * 3.6).toFixed(0) : null);  // m/s → km/h
        setTrail(prev => {
          const next = [...prev, [lat, lng]];
          return next.length > 500 ? next.slice(-500) : next;
        });
        setGpsError(null);
      },
      err => setGpsError(err.message),
      { enableHighAccuracy: true, maximumAge: 1000, timeout: 15_000 }
    );
    return () => {
      if (watchRef.current != null)
        navigator.geolocation.clearWatch(watchRef.current);
    };
  }, []);

  // Poll live samples from API
  const pollSamples = useCallback(async () => {
    try {
      const samples = await fetchLiveSamples({ sinceMs, limit: 50 });
      if (samples.length) {
        setLive(prev => {
          // Merge: avoid duplicates by timestampMs
          const existing = new Set(prev.map(s => s.timestampMs));
          const fresh = samples.filter(s => !existing.has(s.timestampMs));
          const merged = [...fresh, ...prev].slice(0, 200);
          return merged;
        });
        // Advance since_ms to the newest timestamp we've seen
        setSinceMs(samples[0].timestampMs + 1);
      }
    } catch (_) {}
  }, [sinceMs]);

  useEffect(() => {
    pollSamples();
    timerRef.current = setInterval(pollSamples, POLL_INTERVAL_MS);
    return () => clearInterval(timerRef.current);
  }, [pollSamples]);

  // Derive zone coverage % (how many live readings fall inside mission bbox)
  const coveragePct = (() => {
    if (!mission?.polygon || !liveReadings.length) return 0;
    const coords = mission.polygon.coordinates?.[0];
    if (!coords?.length) return 0;
    const lngs = coords.map(c => c[0]);
    const lats  = coords.map(c => c[1]);
    const minLng = Math.min(...lngs), maxLng = Math.max(...lngs);
    const minLat  = Math.min(...lats),  maxLat  = Math.max(...lats);
    const inside = liveReadings.filter(r =>
      r.latitude  >= minLat  && r.latitude  <= maxLat &&
      r.longitude >= minLng && r.longitude <= maxLng
    ).length;
    return Math.min(100, Math.round((inside / liveReadings.length) * 100));
  })();

  // Mission polygon for Leaflet (array of [lat, lng])
  const missionPolygon = (() => {
    const coords = mission?.polygon?.coordinates?.[0];
    if (!coords) return null;
    return coords.map(([lng, lat]) => [lat, lng]);
  })();

  // Default center: mission centroid or [39, -98] (US center)
  const defaultCenter = (() => {
    const c = mission?.centroid;
    if (c?.length === 2) return [c[1], c[0]];
    return [39, -98];
  })();

  const latestDose = liveReadings[0]?.uSvPerHour;

  return (
    <div className="live-tracking-overlay">
      {/* Top bar */}
      <div className="live-top-bar">
        <div className="live-mission-name">
          <span className="live-dot-indicator" />
          {mission?.name || 'Live Tracking'}
        </div>
        <button className="live-end-btn" onClick={onEnd}>✕ End Mission</button>
      </div>

      {/* Map */}
      <div className="live-map-wrap">
        <MapContainer
          center={defaultCenter}
          zoom={14}
          style={{ height: '100%', width: '100%' }}
          zoomControl={true}>
          <TileLayer
            url="https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png"
            attribution="&copy; OpenStreetMap contributors, &copy; CartoDB"
            maxNativeZoom={20}
            maxZoom={22}
          />

          {/* Fit to mission zone on mount */}
          {mission?.polygon && <ZoneFit polygon={mission.polygon} />}

          {/* Center on user once GPS arrives */}
          {position && <CenterOnUser position={position} />}

          {/* Mission zone polygon */}
          {missionPolygon && (
            <Polygon
              positions={missionPolygon}
              pathOptions={{
                color: '#00e676', weight: 2, opacity: 0.9,
                fillColor: '#00e676', fillOpacity: 0.07,
              }}>
              <Tooltip sticky>{mission.name}</Tooltip>
            </Polygon>
          )}

          {/* Historical coverage dots (thin) */}
          {(allRows || []).slice(0, 5000).filter(r => r.lat && r.lng).map((r, i) => (
            <CircleMarker
              key={i}
              center={[r.lat, r.lng]}
              radius={2}
              pathOptions={{
                color: doseColor(r.uSv, 0, 1), fillOpacity: 0.35, weight: 0,
              }} />
          ))}

          {/* Streaming live readings */}
          {liveReadings.filter(r => r.latitude && r.longitude).map((r, i) => (
            <CircleMarker
              key={`live-${r.timestampMs}`}
              center={[r.latitude, r.longitude]}
              radius={i === 0 ? 5 : 3}
              pathOptions={{
                color: doseColor(r.uSvPerHour, 0, 1),
                fillOpacity: i === 0 ? 0.9 : 0.55,
                weight: i === 0 ? 2 : 0,
              }}>
              {i === 0 && (
                <Tooltip permanent>
                  {r.uSvPerHour?.toFixed(3)} µSv/h
                </Tooltip>
              )}
            </CircleMarker>
          ))}

          {/* Live trail */}
          {trail.length > 1 && (
            <Polyline
              positions={trail}
              pathOptions={{ color: '#29b6f6', weight: 3, opacity: 0.8 }} />
          )}

          {/* Live GPS dot */}
          {position && (
            <CircleMarker
              center={position}
              radius={10}
              pathOptions={{
                color: '#fff', fillColor: '#29b6f6',
                fillOpacity: 1, weight: 3,
              }}>
              <Tooltip permanent>You</Tooltip>
            </CircleMarker>
          )}

          {/* Accuracy circle */}
          {position && accuracy && accuracy < 200 && (
            <CircleMarker
              center={position}
              radius={Math.min(accuracy * 0.5, 60)}
              pathOptions={{
                color: '#29b6f6', fillColor: '#29b6f6',
                fillOpacity: 0.08, weight: 1, dashArray: '4 4',
              }} />
          )}
        </MapContainer>
      </div>

      {/* Bottom HUD */}
      <div className="live-hud">
        <div className="live-hud-item">
          <div className="live-hud-label">GPS</div>
          {gpsError
            ? <div className="live-hud-value live-hud-warn">{gpsError.slice(0, 24)}</div>
            : <div className="live-hud-value">
                {position
                  ? (accuracy != null ? `±${accuracy}m` : 'OK')
                  : 'Acquiring…'}
              </div>}
        </div>
        <div className="live-hud-item">
          <div className="live-hud-label">Speed</div>
          <div className="live-hud-value">{speed != null ? `${speed} km/h` : '—'}</div>
        </div>
        <div className="live-hud-item">
          <div className="live-hud-label">Dose (latest)</div>
          <div className="live-hud-value live-hud-dose">
            {latestDose != null ? `${latestDose.toFixed(3)} µSv/h` : '—'}
          </div>
        </div>
        <div className="live-hud-item">
          <div className="live-hud-label">Zone coverage</div>
          <div className="live-hud-value">{coveragePct}%</div>
        </div>
        <div className="live-hud-item">
          <div className="live-hud-label">Live readings</div>
          <div className="live-hud-value">{liveReadings.length}</div>
        </div>
      </div>
    </div>
  );
}
