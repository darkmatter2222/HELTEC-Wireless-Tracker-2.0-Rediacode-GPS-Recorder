// Horizontal timeline strip showing each session as a colored bar across a shared time axis.
import { useMemo } from 'react';
import { sessionColor, fmtTs } from './colors.js';

const MIN_VALID_TS_MS = 1577836800000;

function fmtAxisLabel(ms) {
  const d = new Date(ms);
  return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
}

export function TimelineView({ sessions, selected, onToggle }) {
  // Only sessions with valid timestamps.
  const valid = useMemo(() =>
    sessions.filter(s =>
      s.firstTsMs && s.firstTsMs >= MIN_VALID_TS_MS &&
      s.lastTsMs  && s.lastTsMs  >= MIN_VALID_TS_MS
    ).map((s, i) => ({ ...s, idx: i,  color: sessionColor(sessions.indexOf(s)) })),
    [sessions]
  );

  const { globalLo, globalHi, span } = useMemo(() => {
    if (!valid.length) return { globalLo: 0, globalHi: 1, span: 1 };
    const lo = Math.min(...valid.map(s => s.firstTsMs));
    const hi = Math.max(...valid.map(s => s.lastTsMs));
    return { globalLo: lo, globalHi: hi, span: Math.max(1, hi - lo) };
  }, [valid]);

  // 5 axis tick marks
  const ticks = useMemo(() => {
    const arr = [];
    for (let i = 0; i <= 4; i++) arr.push(globalLo + (span * i) / 4);
    return arr;
  }, [globalLo, span]);

  if (!valid.length) return null;

  return (
    <div className="timeline-view">
      <div className="timeline-header">Session Timeline</div>
      {/* Axis ticks */}
      <div className="timeline-axis">
        {ticks.map((t, i) => (
          <span key={i} style={{ left: `${(i / 4) * 100}%` }}>{fmtAxisLabel(t)}</span>
        ))}
      </div>
      {/* Session bars */}
      <div className="timeline-bars">
        {valid.map(s => {
          const left  = ((s.firstTsMs - globalLo) / span) * 100;
          const width = Math.max(0.5, ((s.lastTsMs - s.firstTsMs) / span) * 100);
          const isSel = selected.has(s.sessionId);
          return (
            <div key={s.sessionId} className="timeline-row"
              onClick={() => onToggle(s.sessionId)}
              title={`${s.displayName || s.sessionId}\n${fmtTs(s.firstTsMs)} → ${fmtTs(s.lastTsMs)}\n${s.samples ?? 0} pts`}
            >
              <div className="timeline-bar-track">
                <div
                  className={`timeline-bar ${isSel ? 'selected' : ''}`}
                  style={{ left: `${left}%`, width: `${width}%`, background: s.color }}
                />
              </div>
              <div className="timeline-label">{s.displayName || s.sessionId}</div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
