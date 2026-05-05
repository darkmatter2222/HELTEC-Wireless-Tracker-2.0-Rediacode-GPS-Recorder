/**
 * DualRangeSlider
 *
 * Gradient bar with two independent draggable handles.
 * Uses refs for all mutable values so the document-level drag handler
 * always reads current state — no stale-closure invisible walls.
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

import { useRef, useEffect } from 'react';

const CANVAS_W = 400;
const CANVAS_H = 10;
const GRADIENT_STEPS = 40;   // color-stop resolution for full-bar gradient
const DIM_ALPHA = 0.18;       // opacity of the out-of-range overlay

// Draw the full-spectrum gradient across the whole bar, then darken the
// regions outside [lowPct, highPct] so the active zone is clearly lit.
function drawGradient(canvas, colorFn, lowPct, highPct) {
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  // 1. Full gradient, wall to wall
  const grd = ctx.createLinearGradient(0, 0, W, 0);
  for (let i = 0; i <= GRADIENT_STEPS; i++) {
    grd.addColorStop(i / GRADIENT_STEPS, colorFn(i / GRADIENT_STEPS));
  }
  ctx.fillStyle = grd;
  ctx.fillRect(0, 0, W, H);

  const x0 = (lowPct  / 100) * W;
  const x1 = (highPct / 100) * W;

  // 2. Dark overlay on the out-of-range zones
  ctx.fillStyle = '#0a0c0f';   // same as --bg
  ctx.globalAlpha = 1 - DIM_ALPHA;
  if (x0 > 0)    ctx.fillRect(0,  0, x0,     H);
  if (x1 < W)    ctx.fillRect(x1, 0, W - x1, H);
  ctx.globalAlpha = 1;
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
  const trackRef  = useRef(null);
  const canvasRef = useRef(null);

  // --- Live refs -----------------------------------------------------------
  // Updated every render so document listeners always see current values.
  const loRef         = useRef(lo);
  const hiRef         = useRef(hi);
  const lowRef        = useRef(low);
  const highRef       = useRef(high);
  const onLowRef      = useRef(onLowChange);
  const onHighRef     = useRef(onHighChange);
  const colorFnRef    = useRef(colorFn);

  loRef.current      = lo;
  hiRef.current      = hi;
  lowRef.current     = low;
  highRef.current    = high;
  onLowRef.current   = onLowChange;
  onHighRef.current  = onHighChange;
  colorFnRef.current = colorFn;
  // -------------------------------------------------------------------------

  const span    = Math.max(1e-9, hi - lo);
  const lowPct  = ((low  - lo) / span) * 100;
  const highPct = ((high - lo) / span) * 100;

  // Redraw canvas on every render (cheap — just canvas 2d operations)
  useEffect(() => {
    if (canvasRef.current) {
      drawGradient(canvasRef.current, colorFn, lowPct, highPct);
    }
  });

  // --- Drag logic ----------------------------------------------------------
  // makeDragStart returns a mousedown handler.
  // onMove reads ONLY from refs — never from the closure snapshot.
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
        <canvas
          ref={canvasRef}
          className="drs-canvas"
          width={CANVAS_W}
          height={CANVAS_H}
        />
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
