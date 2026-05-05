/**
 * DualRangeSlider
 *
 * Gradient bar with two independent draggable handles.
 *
 * Uses a CSS linear-gradient background (not a canvas) so the gradient
 * fills perfectly to the rounded corners of the track with no clipping.
 * Uses refs for all mutable values so document-level drag handlers
 * always read current state — no stale-closure invisible walls.
 *
 * Props:
 *   lo / hi         — absolute track bounds (numbers)
 *   low / high      — current handle values  (lo <= low < high <= hi)
 *   onLowChange(v)  — called while dragging the left handle
 *   onHighChange(v) — called while dragging the right handle
 *   colorFn(t)      — t in [0,1] -> CSS color string
 *   label           — label text shown above the slider
 *   fmtVal(v)       — formats a value for display  (default: 2 dp)
 *   onAuto          — optional callback for the reset-to-auto button
 */

import { useRef } from 'react';

const GRADIENT_STEPS = 24;
const DIM_ALPHA = 0.62;       // how much gradient shows through in out-of-range zones

function buildGradientCSS(colorFn) {
  const stops = [];
  for (let i = 0; i <= GRADIENT_STEPS; i++) {
    const t = i / GRADIENT_STEPS;
    stops.push(`${colorFn(t)} ${(t * 100).toFixed(1)}%`);
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
}) {
  const trackRef = useRef(null);

  // --- Live refs -----------------------------------------------------------
  // Updated every render so document listeners always see current values.
  const loRef       = useRef(lo);
  const hiRef       = useRef(hi);
  const lowRef      = useRef(low);
  const highRef     = useRef(high);
  const onLowRef    = useRef(onLowChange);
  const onHighRef   = useRef(onHighChange);

  loRef.current     = lo;
  hiRef.current     = hi;
  lowRef.current    = low;
  highRef.current   = high;
  onLowRef.current  = onLowChange;
  onHighRef.current = onHighChange;
  // -------------------------------------------------------------------------

  const span       = Math.max(1e-9, hi - lo);
  const lowPct     = ((low  - lo) / span) * 100;
  const highPct    = ((high - lo) / span) * 100;
  const dimHiWidth = 100 - highPct;

  // CSS gradient fills within the rounded container — no canvas clipping issue
  const gradientCSS = buildGradientCSS(colorFn);
  const dimOpacity  = (1 - DIM_ALPHA).toFixed(2);

  // --- Drag logic ----------------------------------------------------------
  function makeDragStart(which) {
    return function onMouseDown(e) {
      e.preventDefault();
      e.stopPropagation();

      const track = trackRef.current;

      function pxToVal(clientX) {
        const rect = track.getBoundingClientRect();
        const t = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
        return loRef.current + t * (hiRef.current - loRef.current);
      }

      function onMove(ev) {
        const currentSpan = Math.max(1e-9, hiRef.current - loRef.current);
        const val = pxToVal(ev.clientX);
        if (which === 'low') {
          onLowRef.current(Math.min(val, highRef.current - currentSpan * 0.001));
        } else {
          onHighRef.current(Math.max(val, lowRef.current + currentSpan * 0.001));
        }
      }

      function onUp() {
        document.removeEventListener('mousemove', onMove);
        document.removeEventListener('mouseup', onUp);
        e.target.classList.remove('dragging');
      }

      e.target.classList.add('dragging');
      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseup', onUp);
    };
  }

  return (
    <div className="drs-wrap">
      <div className="drs-header">
        <span className="drs-label">{label}</span>
        {onAuto && (
          <button
            className="btn-sm"
            onClick={onAuto}
            style={{
              fontSize: '10px',
              padding: '1px 7px',
              color: 'var(--accent2)',
              borderColor: 'var(--accent2)',
              background: 'transparent',
            }}
          >
            ↺ Auto
          </button>
        )}
      </div>

      <div className="drs-values">
        <span className="drs-val">{fmtVal(low)}</span>
        <span className="drs-val">{fmtVal(high)}</span>
      </div>

      <div className="drs-track" ref={trackRef}>
        {/* CSS gradient fills exactly to the rounded corners — no canvas/clip issues */}
        <div className="drs-gradient" style={{ background: gradientCSS }} />

        {/* Dark overlay on left out-of-range zone */}
        {lowPct > 0 && (
          <div className="drs-dim drs-dim-lo"
            style={{ width: `${lowPct}%`, opacity: dimOpacity }} />
        )}
        {/* Dark overlay on right out-of-range zone */}
        {dimHiWidth > 0 && (
          <div className="drs-dim drs-dim-hi"
            style={{ width: `${dimHiWidth}%`, opacity: dimOpacity }} />
        )}

        {/* Low handle */}
        <div
          className="drs-handle"
          style={{ left: `${lowPct}%` }}
          onMouseDown={makeDragStart('low')}
          title={fmtVal(low)}
        />
        {/* High handle — rendered after so it stacks on top when handles overlap */}
        <div
          className="drs-handle"
          style={{ left: `${highPct}%` }}
          onMouseDown={makeDragStart('high')}
          title={fmtVal(high)}
        />
      </div>
    </div>
  );
}
