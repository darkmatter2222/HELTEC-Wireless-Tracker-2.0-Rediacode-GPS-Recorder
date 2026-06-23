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

// ---- dose per count: violet (low) -> magenta -> amber (high) -----------
//  µSv/h divided by CPS; lower = more efficient / lower energy per count
export function dosePerCountColor(dpc, lo, hi) {
  const t = norm(dpc, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 270, 70, 55],  // violet
    [0.50, 320, 80, 55],  // magenta/pink
    [1.00,  35, 95, 52],  // amber
  ]);
}

// ---- session: rotating hue wheel ---------------------------------------
export function sessionColor(idx) {
  const hue = (idx * 47) % 360;
  return `hsl(${hue},70%,58%)`;
}

// ============================================================
// SPECTROGRAM COLOR CHANNELS — for spectrum-based hex binning
// Each maps a derived metric from the gamma energy spectrum
// channel counts to an HSL gradient color.
// ============================================================

// ---- total channel counts: dark teal -> bright cyan -> white ----------
// Total counting rate across all spectrum channels; indicator of overall
// detector activity. Higher = more radiation detected in the bin.
export function totalCountsColor(value, lo, hi) {
  const t = norm(value, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 170, 50, 25],   // dark teal
    [0.40, 180, 80, 45],   // medium cyan
    [0.70, 190, 90, 65],   // bright cyan
    [1.00, 200, 60, 90],   // near white (very active)
  ]);
}

// ---- peak channel index: purple (low/ambient) -> orange (high/gamma) --
// Which energy bin has the most counts; lower = more background radiation,
// higher = more energetic events detected.
export function peakChannelColor(value, lo, hi) {
  const t = norm(value, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 280, 60, 45],   // purple (low-energy dominant)
    [0.50, 310, 70, 55],   // magenta
    [1.00,  30, 90, 55],   // orange (high-energy events)
  ]);
}

// ---- low energy band intensity: navy -> green -------------------------
// Sum of counts in channels 0-24 (ambient/background radiation).
// Lower intensity = cleaner environment.
export function lowEnergyColor(value, lo, hi) {
  const t = norm(value, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 230, 60, 30],   // navy (minimal ambient)
    [0.50, 150, 70, 45],   // green
    [1.00, 100, 85, 55],   // lime-green (high ambient background)
  ]);
}

// ---- high energy band intensity: blue -> red --------------------------
// Sum of counts in tail channels (gamma radiation indicator).
// Higher = more penetrating radiation detected.
export function highEnergyColor(value, lo, hi) {
  const t = norm(value, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 220, 70, 45],   // blue (low gamma presence)
    [0.50,  30, 85, 55],   // orange
    [1.00,   0, 90, 50],   // red (elevated gamma radiation)
  ]);
}

// ---- spectral centroid: cool blue -> warm amber -----------------------
// Center of mass of the spectrum distribution; higher = broader/harder
// spectrum indicating more energetic events on average.
export function spectralCentroidColor(value, lo, hi) {
  const t = norm(value, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 210, 70, 50],   // blue (soft/low-energy spectrum)
    [0.50, 160, 60, 50],   // teal
    [1.00,  40, 90, 55],   // amber (hard/higher-energy spectrum)
  ]);
}

// ---- spectral entropy: indigo (uniform/noise) -> gold (peaked) --------
// Spread/complexity in bits; higher entropy = flatter distribution,
// lower entropy = narrow peaked spectrum. Useful for detecting anomalies.
export function spectralEntropyColor(value, lo, hi) {
  const t = norm(value, lo, hi);
  if (t === null) return '#555';
  return gradientHsl(t, [
    [0.00, 260, 60, 45],   // indigo
    [0.50, 180, 70, 55],   // spring green
    [1.00,  50, 95, 55],   // gold (high entropy / uniform spread)
  ]);
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
