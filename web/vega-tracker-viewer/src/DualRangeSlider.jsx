// DualRangeSlider — a single gradient bar with two draggable handles.
// Shows the channel color gradient between the handles; solid endpoint
// color outside each handle ("clamping" the gradient at the edges).
//
// Props:
//   lo        — absolute minimum value for the track
//   hi        — absolute maximum value for the track
//   low       — current left-handle value
//   high      — current right-handle value
//   onLowChange(v)  — called while dragging left handle
//   onHighChange(v) — called while dragging right handle
//   colorFn(t)      — maps t ∈ [0,1] → CSS color string (the gradient preview)
//   label           — channel label shown above the slider
//   fmtVal(v)       — formats a numeric value for display (optional)
//   autoLabel       — label for the reset-to-auto button

import { useRef, useCallback } from 'react';

const HANDLE_W = 12; // px half-width of each handle for offset math
const STEPS = 200;   // gradient preview resolution

function buildGradient(colorFn) {
  const stops = [];
  for (let i = 0; i <= STEPS; i++) {
    stops.push(colorFn(i / STEPS));
  }
  return `linear-gradient(to right, ${stops.join(', ')})`;
}

export function DualRangeSlider({
  lo, hi,
  low, high,
  onLowChange, onHighChange,
  colorFn,
  label,
  fmtVal = v => v.toFixed(2),
  onAuto,
  autoLabel = '↺ Auto',
}) {
  const trackRef = useRef(null);

  // Converts a pixel x position inside the track to a value in [lo, hi]
  function pxToVal(clientX) {
    const rect = trackRef.current.getBoundingClientRect();
    const t = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
    return lo + t * (hi - lo);
  }

  // Drag handler factory
  const startDrag = useCallback((which) => (e) => {
    e.preventDefault();
    const isTouch = e.type === 'touchstart';

    function getX(ev) {
      return isTouch ? ev.touches[0].clientX : ev.clientX;
    }

    function onMove(ev) {
      const val = pxToVal(getX(ev));
      if (which === 'low') {
        onLowChange(Math.min(val, high - (hi - lo) * 0.001));
      } else {
        onHighChange(Math.max(val, low + (hi - lo) * 0.001));
      }
    }
    function onUp() {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      document.removeEventListener('touchmove', onMove);
      document.removeEventListener('touchend', onUp);
    }
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
    document.addEventListener('touchmove', onMove, { passive: false });
    document.addEventListener('touchend', onUp);
  }, [lo, hi, low, high, onLowChange, onHighChange]); // eslint-disable-line

  // Handle positions as % of track width
  const lowPct  = ((low  - lo) / Math.max(1e-9, hi - lo)) * 100;
  const highPct = ((high - lo) / Math.max(1e-9, hi - lo)) * 100;

  // Solid color at the endpoints
  const colorLow  = colorFn(0);
  const colorHigh = colorFn(1);
  const gradient  = buildGradient(colorFn);

  return (
    <div className="drs-wrap">
      <div className="drs-label-row">
        <span className="drs-label">{label}</span>
        {onAuto && (
          <button className="btn-sm drs-auto-btn" onClick={onAuto} title="Reset to auto-scale">
            {autoLabel}
          </button>
        )}
      </div>
      <div className="drs-values-row">
        <span className="drs-val">{fmtVal(low)}</span>
        <span className="drs-val drs-val-right">{fmtVal(high)}</span>
      </div>

      {/* Track */}
      <div className="drs-track-outer" ref={trackRef}>
        {/* Left solid zone */}
        <div className="drs-zone drs-zone-left"
          style={{ width: `${lowPct}%`, background: colorLow }} />

        {/* Gradient zone between handles */}
        <div className="drs-zone drs-zone-mid"
          style={{
            left: `${lowPct}%`,
            width: `${highPct - lowPct}%`,
            background: gradient,
            backgroundSize: `${100 / Math.max(0.001, (highPct - lowPct) / 100)}% 100%`,
          }} />

        {/* Right solid zone */}
        <div className="drs-zone drs-zone-right"
          style={{ left: `${highPct}%`, width: `${100 - highPct}%`, background: colorHigh }} />

        {/* Left handle */}
        <div className="drs-handle drs-handle-low"
          style={{ left: `${lowPct}%` }}
          onMouseDown={startDrag('low')}
          onTouchStart={startDrag('low')}
          title={fmtVal(low)}
        />

        {/* Right handle */}
        <div className="drs-handle drs-handle-high"
          style={{ left: `${highPct}%` }}
          onMouseDown={startDrag('high')}
          onTouchStart={startDrag('high')}
          title={fmtVal(high)}
        />
      </div>
    </div>
  );
}
