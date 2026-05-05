/**
 * DualRangeSlider
 *
 * A gradient bar with two independent draggable handles.
 *
 * Props:
 *   lo / hi         — absolute track bounds
 *   low / high      — current handle values (lo <= low < high <= hi)
 *   onLowChange(v)  — fired while dragging the left handle
 *   onHighChange(v) — fired while dragging the right handle
 *   colorFn(t)      — t in [0,1] maps to a CSS color string for the gradient preview
 *   label           — text shown above the slider
 *   fmtVal(v)       — formats a value for display (default: 2 decimals)
 *   onAuto          — optional callback for the reset-to-auto button
 */

import { useRef, useEffect } from 'react';

const CANVAS_W = 300;
const CANVAS_H = 10;

function drawGradient(canvas, colorFn, lowPct, highPct) {
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  const x0 = (lowPct  / 100) * W;
  const x1 = (highPct / 100) * W;

  // Left dimmed zone
  if (x0 > 0) {
    ctx.globalAlpha = 0.25;
    ctx.fillStyle = colorFn(0);
    ctx.fillRect(0, 0, x0, H);
  }

  // Active gradient zone
  ctx.globalAlpha = 1;
  if (x1 > x0) {
    const grd = ctx.createLinearGradient(x0, 0, x1, 0);
    const STEPS = 24;
    for (let i = 0; i <= STEPS; i++) {
      const t = i / STEPS;
      const globalT = (lowPct + t * (highPct - lowPct)) / 100;
      grd.addColorStop(t, colorFn(globalT));
    }
    ctx.fillStyle = grd;
    ctx.fillRect(x0, 0, x1 - x0, H);
  }

  // Right dimmed zone
  if (x1 < W) {
    ctx.globalAlpha = 0.25;
    ctx.fillStyle = colorFn(1);
    ctx.fillRect(x1, 0, W - x1, H);
  }
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

  const span    = Math.max(1e-9, hi - lo);
  const lowPct  = ((low  - lo) / span) * 100;
  const highPct = ((high - lo) / span) * 100;

  useEffect(() => {
    if (canvasRef.current) {
      drawGradient(canvasRef.current, colorFn, lowPct, highPct);
    }
  });

  function makeDragStart(which) {
    return function onMouseDown(e) {
      e.preventDefault();
      e.stopPropagation();

      const track = trackRef.current;

      function pxToVal(clientX) {
        const rect = track.getBoundingClientRect();
        const t = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
        return lo + t * (hi - lo);
      }

      function onMove(ev) {
        const val = pxToVal(ev.clientX);
        if (which === 'low') {
          onLowChange(Math.min(val, high - span * 0.001));
        } else {
          onHighChange(Math.max(val, low + span * 0.001));
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
        <div
          className="drs-handle"
          style={{ left: `${lowPct}%` }}
          onMouseDown={makeDragStart('low')}
          title={fmtVal(low)}
        />
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
