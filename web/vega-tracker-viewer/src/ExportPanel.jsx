// ExportPanel — time-range data export with automatic multi-file ZIP for large datasets.
import { useState } from 'react';
import { fetchExportPreview, exportTimeRange } from './api.js';

const MAX_BYTES_PER_FILE = 10 * 1024 * 1024; // 10 MB hard cap per file

// ---- helpers ---------------------------------------------------------------

function todayDateStr() {
  return new Date().toISOString().slice(0, 10);
}

/** Midnight UTC for a YYYY-MM-DD string. */
function dayStartMs(dateStr) {
  return new Date(dateStr + 'T00:00:00Z').getTime();
}

/** 23:59:59.999 UTC for a YYYY-MM-DD string. */
function dayEndMs(dateStr) {
  return new Date(dateStr + 'T23:59:59.999Z').getTime();
}

function formatBytes(bytes) {
  if (bytes < 1024)              return `${bytes} B`;
  if (bytes < 1024 * 1024)      return `${(bytes / 1024).toFixed(0)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

// Build quick-select presets relative to today (UTC dates).
function buildPresets() {
  const now  = new Date();
  const yr   = now.getUTCFullYear();
  const mo   = now.getUTCMonth();  // 0-based
  const day  = now.getUTCDate();

  const fmt = (d) => d.toISOString().slice(0, 10);

  const today     = fmt(now);
  const yesterday = fmt(new Date(Date.UTC(yr, mo, day - 1)));

  // This week: Monday → today.
  const dow            = now.getUTCDay() === 0 ? 7 : now.getUTCDay(); // 1=Mon…7=Sun
  const thisWeekStart  = fmt(new Date(Date.UTC(yr, mo, day - (dow - 1))));

  // Last week: Mon → Sun.
  const lastWeekEnd    = fmt(new Date(Date.UTC(yr, mo, day - dow)));
  const lastWeekStart  = fmt(new Date(Date.UTC(yr, mo, day - dow - 6)));

  // This month.
  const thisMonthStart = fmt(new Date(Date.UTC(yr, mo, 1)));

  // Last month.
  const lastMonthStart = fmt(new Date(Date.UTC(yr, mo - 1, 1)));
  const lastMonthEnd   = fmt(new Date(Date.UTC(yr, mo, 0)));  // last day of prev month

  return [
    { label: 'Today',       start: today,          end: today },
    { label: 'Yesterday',   start: yesterday,       end: yesterday },
    { label: 'This Week',   start: thisWeekStart,   end: today },
    { label: 'Last Week',   start: lastWeekStart,   end: lastWeekEnd },
    { label: 'This Month',  start: thisMonthStart,  end: today },
    { label: 'Last Month',  start: lastMonthStart,  end: lastMonthEnd },
  ];
}

// Generate month options going back 3 years from today.
function buildMonthOptions() {
  const now = new Date();
  let yr    = now.getUTCFullYear();
  let mo    = now.getUTCMonth();  // 0-based
  const out = [];
  for (let i = 0; i < 36; i++) {
    const start = new Date(Date.UTC(yr, mo, 1));
    const end   = new Date(Date.UTC(yr, mo + 1, 0)); // last day of month
    out.push({
      label: start.toLocaleString('default', { month: 'long', year: 'numeric', timeZone: 'UTC' }),
      start: start.toISOString().slice(0, 10),
      end:   end.toISOString().slice(0, 10),
    });
    mo--;
    if (mo < 0) { mo = 11; yr--; }
  }
  return out;
}

const EXPORT_FORMATS = [
  { key: 'radiacode_txt', label: 'RadiaCode (.txt)', ext: 'txt', desc: 'Native RadiaCode app format — tab-separated with Windows FILETIME timestamps' },
  { key: 'radiacode',     label: 'RadiaCode CSV',    ext: 'csv', desc: 'Comma-separated RadiaCode variant with running total-dose column' },
  { key: 'internal',      label: 'Internal CSV',     ext: 'csv', desc: 'Full firmware schema — 12 columns including event markers and accuracy' },
];

const PRESETS   = buildPresets();
const MONTHS    = buildMonthOptions();

// ---- component -------------------------------------------------------------

export function ExportPanel() {
  const [startDate,    setStartDate]    = useState(todayDateStr());
  const [endDate,      setEndDate]      = useState(todayDateStr());
  const [format,       setFormat]       = useState('radiacode_txt');
  const [selectedLabel, setSelectedLabel] = useState(null); // which preset/month is active
  const [gpsOnly,      setGpsOnly]      = useState(true);  // filter to GPS-locked samples only

  const [preview,        setPreview]        = useState(null);
  const [previewLoading, setPreviewLoading] = useState(false);
  const [exporting,      setExporting]      = useState(false);
  const [error,          setError]          = useState('');
  const [status,         setStatus]         = useState('');

  function applyRange(start, end, label = null) {
    setStartDate(start);
    setEndDate(end);
    setSelectedLabel(label);
    setPreview(null);
    setError('');
    setStatus('');
  }

  async function handleCheckRange() {
    setError('');
    setStatus('');
    setPreview(null);

    if (startDate > endDate) {
      setError('Start date must not be after end date.');
      return;
    }

    setPreviewLoading(true);
    try {
      const data = await fetchExportPreview(dayStartMs(startDate), dayEndMs(endDate), gpsOnly);
      setPreview(data);
    } catch (e) {
      setError(`Preview failed: ${e.message}`);
    } finally {
      setPreviewLoading(false);
    }
  }

  async function handleExport() {
    setError('');
    setStatus('');

    if (startDate > endDate) {
      setError('Start date must not be after end date.');
      return;
    }

    setExporting(true);
    try {
      const { filename, sizeBytes } = await exportTimeRange(
        dayStartMs(startDate),
        dayEndMs(endDate),
        format,
        MAX_BYTES_PER_FILE,
        selectedLabel || '',
        gpsOnly,
      );
      setStatus(`Downloaded: ${filename}  (${formatBytes(sizeBytes)})`);
    } catch (e) {
      setError(`Export failed: ${e.message}`);
    } finally {
      setExporting(false);
    }
  }

  const fmtInfo = EXPORT_FORMATS.find(f => f.key === format);

  return (
    <div className="export-panel">
      <div className="export-layout">

        {/* ===== LEFT: configuration ===== */}
        <div className="export-config">

          {/* Quick presets */}
          <div className="export-section">
            <div className="export-section-title">Quick Range</div>
            <div className="preset-grid">
              {PRESETS.map(p => (
                <button key={p.label}
                  className={`preset-btn${selectedLabel === p.label ? ' active' : ''}`}
                  onClick={() => applyRange(p.start, p.end, p.label)}>
                  {p.label}
                </button>
              ))}
            </div>
          </div>

          {/* Month picker */}
          <div className="export-section">
            <div className="export-section-title">Select Month</div>
            <div className="month-scroll">
              {MONTHS.map(m => (
                <button key={m.label}
                  className={`month-btn${selectedLabel === m.label ? ' active' : ''}`}
                  onClick={() => applyRange(m.start, m.end, m.label)}>
                  {m.label}
                </button>
              ))}
            </div>
          </div>

          {/* Custom date range */}
          <div className="export-section">
            <div className="export-section-title">
              Custom Range
              {selectedLabel && (
                <span className="range-badge">{selectedLabel}</span>
              )}
            </div>
            <div className="date-row">
              <span className="date-label">From</span>
              <input type="date" className="date-input" value={startDate}
                onChange={e => { setStartDate(e.target.value); setSelectedLabel(null); setPreview(null); }} />
            </div>
            <div className="date-row">
              <span className="date-label">To</span>
              <input type="date" className="date-input" value={endDate}
                onChange={e => { setEndDate(e.target.value); setSelectedLabel(null); setPreview(null); }} />
            </div>
          </div>

          {/* Format picker */}
          <div className="export-section">
            <div className="export-section-title">Export Format</div>
            <div className="format-list">
              {EXPORT_FORMATS.map(f => (
                <button key={f.key}
                  className={`format-btn${format === f.key ? ' active' : ''}`}
                  onClick={() => setFormat(f.key)}>
                  <span className="format-label">{f.label}</span>
                  <span className="format-desc">{f.desc}</span>
                </button>
              ))}
            </div>
          </div>

          {/* GPS filter toggle */}
          <div className="export-section">
            <div className="export-section-title">Filter</div>
            <label className="gps-toggle-row">
              <span className="gps-toggle-label">GPS samples only</span>
              <input
                type="checkbox"
                className="gps-toggle-check"
                checked={gpsOnly}
                onChange={e => { setGpsOnly(e.target.checked); setPreview(null); }}
              />
              <span className={`gps-toggle-pill${gpsOnly ? ' on' : ''}`} />
            </label>
            <div className="gps-toggle-desc">
              {gpsOnly
                ? 'Only samples with a valid GPS fix — required for RadiaCode format'
                : 'Include all samples — rows missing coordinates are exported as blanks'}
            </div>
          </div>

          {/* Check range button */}
          <div className="export-section">
            <button className="check-btn" onClick={handleCheckRange}
              disabled={previewLoading}>
              {previewLoading ? 'Checking...' : 'Check Range'}
            </button>
          </div>

        </div>{/* /export-config */}

        {/* ===== RIGHT: preview + download ===== */}
        <div className="export-right">
          <div className="export-right-header">Export Preview</div>

          {error  && <div className="export-error">{error}</div>}
          {status && <div className="export-status">{status}</div>}

          {preview ? (
            <div className="preview-card">
              <div className="preview-stat big">
                <span className="preview-stat-label">Data points</span>
                <span className="preview-stat-val accent">
                  {preview.rowCount.toLocaleString()}
                </span>
              </div>

              <div className="preview-divider" />

              <div className="preview-stat">
                <span className="preview-stat-label">Estimated size</span>
                <span className="preview-stat-val">{preview.estimatedMB} MB</span>
              </div>
              <div className="preview-stat">
                <span className="preview-stat-label">Output</span>
                <span className="preview-stat-val">
                  {preview.estimatedFiles === 1
                    ? '1 file — direct download'
                    : `${preview.estimatedFiles} files — packaged as .zip`}
                </span>
              </div>
              <div className="preview-stat">
                <span className="preview-stat-label">Date range</span>
                <span className="preview-stat-val muted">
                  {selectedLabel ? selectedLabel : `${startDate} → ${endDate}`}
                </span>
              </div>
              <div className="preview-stat">
                <span className="preview-stat-label">Format</span>
                <span className="preview-stat-val muted">{fmtInfo?.label}</span>
              </div>

              {preview.rowCount === 0 ? (
                <div className="preview-empty">No data found in this time range.</div>
              ) : (
                <button className="export-btn" onClick={handleExport} disabled={exporting}>
                  {exporting
                    ? 'Preparing download...'
                    : `Export ${preview.rowCount.toLocaleString()} rows`}
                </button>
              )}
            </div>
          ) : (
            !error && (
              <div className="preview-hint">
                Select a date range and click <strong>Check Range</strong> to see
                how many data points are available, then click <strong>Export</strong>
                to download.
              </div>
            )
          )}

          <div className="export-notes">
            <div className="export-notes-title">Notes</div>
            <ul>
              <li>Files larger than 10 MB are automatically split and packaged as a <code>.zip</code>.</li>
              <li><strong>RadiaCode (.txt)</strong> matches the native app export format exactly, including Windows FILETIME timestamps — compatible with RadiaCode upload sites.</li>
              <li>All timestamps are UTC.</li>
              <li>GPS event rows (GPS_LOST / GPS_REGAINED) are omitted from RadiaCode .txt exports.</li>
            </ul>
          </div>
        </div>{/* /export-right */}

      </div>{/* /export-layout */}
    </div>
  );
}
