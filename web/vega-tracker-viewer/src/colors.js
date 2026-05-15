// Color helpers for Radiological Map viewer.
// Each function maps a value in [lo, hi] to a CSS color string.

// ---- generic gradient interpolation -----------------------------------

// Gradient stops: array of [fraction 0-1, hue, sat, light].
function gradientHsl(t, stops) {
  t = Math.max(0, Math.min(1, t));
  for (let i = 1; i < stops.length; i++) {
    const [f0, h0, s0, l0] = stops[i - 1];
    const [f1, h1, s1, l1] = stops[i];
    if (t <= f1) {
      const u = (t - f0) / (f1 - f0);
      const h = h0 + (h1 - h0) * u;
      const s = s0 + (s1 - s0) * u;
      const l = l0 + (l1 - l0) * u;
      return `hsl(${h.toFixed(0)},${s.toFixed(0)}%,${l.toFixed(0)}%)`;
    }
  }
  const last = stops[stops.length - 1];
  return `hsl(${last[1]},${last[2]}%,${last[3]}%)`;
}

function norm(val, lo, hi) {
  if (val == null || isNaN(val)) return null;
  return Math.max(0, Math.min(1, (val - lo) / Math.max(1e-9, hi - lo)));
}

// ---- dose rate: green -> amber -> red ----------------------------------
export function doseColor(uSv, lo, hi) {
  const t = norm(uSv, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 130, 85, 48],  // green
    [0.50,  55, 90, 50],  // amber
    [1.00,   0, 90, 48],  // red
  ]);
}

// ---- counts per second: blue -> cyan -> yellow -------------------------
export function cpsColor(cps, lo, hi) {
  const t = norm(cps, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 220, 80, 50],  // blue
    [0.50, 190, 80, 55],  // cyan
    [1.00,  55, 90, 52],  // yellow
  ]);
}

// ---- speed: cool blue (slow) -> warm orange (fast) ---------------------
export function speedColor(spd, lo, hi) {
  const t = norm(spd, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 200, 70, 55],  // steel blue
    [0.50, 160, 60, 50],  // teal
    [1.00,  25, 90, 55],  // orange
  ]);
}

// ---- altitude: sea blue -> land green -> peak white --------------------
export function altColor(alt, lo, hi) {
  const t = norm(alt, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 210, 60, 45],  // deep blue
    [0.40, 130, 55, 45],  // green
    [0.75,  55, 60, 60],  // sandy
    [1.00,   0,  0, 85],  // near white
  ]);
}

// ---- HDOP: green (good) -> red (bad) -----------------------------------
//  HDOP < 1 = excellent, 1-2 = good, 2-5 = moderate, >5 = bad
export function hdopColor(hdop, lo, hi) {
  const t = norm(hdop, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 130, 80, 48],  // green (excellent)
    [0.50,  55, 85, 52],  // amber (moderate)
    [1.00,   0, 85, 48],  // red (bad)
  ]);
}

// ---- Accuracy (m): same gradient as HDOP but in metres -----------------
//  <3m excellent, 3-10m good, 10-25m moderate, >25m bad
export function accColor(accM, lo, hi) {
  const t = norm(accM, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 130, 80, 48],
    [0.50,  55, 85, 52],
    [1.00,   0, 85, 48],
  ]);
}

// ---- session: rotating hue wheel ---------------------------------------
export function sessionColor(idx) {
  const hue = (idx * 47) % 360;
  return `hsl(${hue},70%,58%)`;
}

// ---- formatters --------------------------------------------------------

export function fmtTs(ms) {
  if (!ms) return '';
  return new Date(ms).toLocaleString();
}

export function fmtDose(uSv, nano) {
  if (uSv == null) return '-';
  if (nano) return `${(uSv * 1000).toFixed(1)} nSv/h`;
  return `${uSv.toFixed(3)} µSv/h`;
}
