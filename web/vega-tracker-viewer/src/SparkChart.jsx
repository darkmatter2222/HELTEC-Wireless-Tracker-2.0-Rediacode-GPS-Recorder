// Lightweight canvas sparkline used in the stats panel.
// Renders dose rate (or CPS) for all filtered points as a mini line chart.
import { useEffect, useRef } from 'react';
import { doseColor } from './colors.js';

export function SparkChart({ points, field, doseMin, doseMax, height = 60 }) {
  const ref = useRef();

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.offsetWidth;
    const H = height;
    canvas.width  = W * (window.devicePixelRatio || 1);
    canvas.height = H * (window.devicePixelRatio || 1);
    ctx.scale(window.devicePixelRatio || 1, window.devicePixelRatio || 1);

    ctx.clearRect(0, 0, W, H);

    if (!points || points.length < 2) {
      ctx.fillStyle = '#444';
      ctx.font = '11px sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('no data', W / 2, H / 2);
      return;
    }

    const vals = points.map(p => {
      if (field === 'cps') return p.cps ?? 0;
      if (field === 'speed') return p.spd ?? 0;
      return p.uSv ?? 0;
    });
    const lo = Math.min(...vals);
    const hi = Math.max(...vals);
    const range = Math.max(hi - lo, 1e-6);

    const pad = 8;
    const xScale = (W - pad * 2) / (vals.length - 1);

    // Draw area fill
    ctx.beginPath();
    ctx.moveTo(pad, H - pad);
    for (let i = 0; i < vals.length; i++) {
      const x = pad + i * xScale;
      const y = pad + (1 - (vals[i] - lo) / range) * (H - pad * 2);
      if (i === 0) ctx.lineTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.lineTo(pad + (vals.length - 1) * xScale, H - pad);
    ctx.closePath();
    ctx.fillStyle = 'rgba(0, 230, 118, 0.12)';
    ctx.fill();

    // Draw coloured line segments
    for (let i = 1; i < vals.length; i++) {
      const x0 = pad + (i - 1) * xScale;
      const y0 = pad + (1 - (vals[i - 1] - lo) / range) * (H - pad * 2);
      const x1 = pad + i * xScale;
      const y1 = pad + (1 - (vals[i] - lo) / range) * (H - pad * 2);
      ctx.beginPath();
      ctx.moveTo(x0, y0);
      ctx.lineTo(x1, y1);
      ctx.strokeStyle = field === 'cps'
        ? 'hsl(200,80%,60%)'
        : field === 'speed'
          ? 'hsl(25,85%,55%)'
          : doseColor(vals[i], doseMin, doseMax);
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }, [points, field, doseMin, doseMax, height]);

  return (
    <canvas ref={ref} style={{ width: '100%', height, display: 'block', borderRadius: 4 }} />
  );
}
