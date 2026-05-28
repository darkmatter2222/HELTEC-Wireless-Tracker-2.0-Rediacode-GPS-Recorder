# Web Viewer Screenshots — Manifest & Regeneration Guide

This file is the authoritative record for every screenshot in this directory.
It also contains **step-by-step instructions** for regenerating all screenshots
so a future agent (human or AI) can update them automatically by following
this document alone.

---

## Screenshot Index

| # | Filename | Feature | What to look for when validating |
|---|----------|---------|----------------------------------|
| 01 | `01_explore_track.png` | Explore → Track mode | Green→yellow dose-rate coloured polyline on dark basemap; session list in sidebar with one row checked; timeline bar visible at bottom |
| 02 | `02_explore_dots.png` | Explore → Dots mode | Individual colour-coded circles instead of a line; Display panel open on left showing Dots active |
| 03 | `03_explore_hexbin.png` | Explore → Hex Bin mode | Orange–red flat-top hexagons with integer sample counts inside each bin; Hex button highlighted in Display panel |
| 04 | `04_explore_arrows.png` | Explore → Arrows mode | Small arrowheads along the route showing travel direction; Arrows button highlighted |
| 05 | `05_stats_panel.png` | Explore → Stats tab | Stats tab active in sidebar; POINTS / AVG DOSE / MAX DOSE / MIN DOSE / AVG CPS etc. stat cards visible; Dose rate / CPS / Speed sparkline charts below |
| 06 | `06_data_management.png` | Data Management tab | Two-column layout: left = session list + Rename/Delete/Merge/Export sub-tabs + daily samples/uploads spark charts; right = DATABASE panel with storage metrics + backup history |
| 07 | `07_render.png` | Render tab | Setup column on left (tracks picker, output size grid, render mode, palette, etc.); right panel shows an actual rendered preview image (not the empty "no render yet" state) |
| 08 | `08_export.png` | Export tab | Quick Range buttons; month picker list; Custom Range date inputs pre-filled; Export Format cards; Export Preview section |
| 09 | `09_collage.png` | 2×4 composite collage | All 8 panels tiled 4-across in 2 rows; green label bar under each cell; dark background |

---

## Capture Metadata

| Field | Value |
|-------|-------|
| Captured on date | 2026-05-28 |
| Git branch | `copilot/session-20260528-explorer-feature` |
| Git commit | `b4ec179` |
| App URL | `http://darkmatter2222:liquimatter@192.168.86.48:8031/tracker/` |
| Session used for screen captures | `2026-05-28` — 15,551 pts, 12.4 h, North Atlanta GA-400 corridor (Alpharetta → Cumming area) |
| Viewport size | 1280 × 664 (browser default at 100% zoom) |
| Color channel | Dose rate (default) |
| Map tile layer | CartoDB Dark (default) |
| Collage script | `scripts/build_screenshot_collage.py` |
| Collage size | 2,688 × 784 px (4 cols × 2 rows, 640×360 thumbnails, 32 px label bar, 8 px padding) |

---

## How Screenshots Are Gathered — Full Procedure

Follow every step in order. Do **not** skip steps — the order matters for the
map state carried between modes.

### Prerequisites

- Viewer running at `http://192.168.86.48:8031/tracker/` (or the current host in AGENTS.md)
- A browser page open to that URL with basic-auth credentials embedded:
  `http://darkmatter2222:liquimatter@192.168.86.48:8031/tracker/`
  (credentials stored in `web/vega-tracker-viewer/.env` as `TRACKER_USER` / `TRACKER_PASS`)
- `docs/screenshots/` directory exists (create if absent)

### Step 1 — Open the viewer and select exactly ONE session

```javascript
// Open (or reuse) a browser page at the app URL
// Wait for the session list to load — look for the first date-group heading

// Ensure no sessions are pre-selected
await page.locator('button:has-text("None")').first().click();
await page.waitForTimeout(500);

// Check ONLY the first (most recent) session in the list
const firstCheckbox = page.locator('.session-item input[type="checkbox"]').first();
await firstCheckbox.click();
await page.waitForTimeout(600);

// Zoom the map to fit the selected track
await page.locator('button:has-text("Fit")').first().click();
await page.waitForTimeout(2000);  // allow map tiles + polyline to render
```

> **CRITICAL**: only ONE session must be selected at all times. Selecting more
> than ~5 large sessions simultaneously can crash or stall the browser tab.
> Always start from "None" and check only the first visible session.

### Step 2 — Capture 01_explore_track.png (Track mode, Sessions tab)

The Sessions tab and Track mode are the default state after loading. No extra
navigation needed after Step 1.

```javascript
await page.screenshot({
  path: 'docs/screenshots/01_explore_track.png',
  fullPage: false
});
```

### Step 3 — Open the Display tab

```javascript
await page.locator('text=DISPLAY').first().click();
await page.waitForTimeout(600);
```

### Step 4 — Capture 02_explore_dots.png (Dots mode)

```javascript
await page.locator('button:has-text("Dots")').first().click();
await page.waitForTimeout(1500);
await page.screenshot({ path: 'docs/screenshots/02_explore_dots.png', fullPage: false });
```

### Step 5 — Capture 03_explore_hexbin.png (Hex Bin mode)

```javascript
await page.locator('button:has-text("Hex")').first().click();
await page.waitForTimeout(2000);
await page.screenshot({ path: 'docs/screenshots/03_explore_hexbin.png', fullPage: false });
```

### Step 6 — Capture 04_explore_arrows.png (Arrows mode)

```javascript
await page.locator('button:has-text("Arrows")').first().click();
await page.waitForTimeout(2000);
await page.screenshot({ path: 'docs/screenshots/04_explore_arrows.png', fullPage: false });
```

### Step 7 — Capture 05_stats_panel.png (Stats tab)

Switch back to Track mode first so the map looks clean, then open Stats.

```javascript
await page.locator('button:has-text("Track")').first().click();
await page.waitForTimeout(1000);
await page.locator('text=STATS').first().click();
await page.waitForTimeout(1500);
await page.screenshot({ path: 'docs/screenshots/05_stats_panel.png', fullPage: false });
```

### Step 8 — Capture 06_data_management.png (Data Management tab)

```javascript
await page.locator('text=Data Management').first().click();
await page.waitForTimeout(2000);
await page.screenshot({ path: 'docs/screenshots/06_data_management.png', fullPage: false });
```

### Step 9 — Capture 07_render.png (Render tab, with actual render output)

Selecting and rendering ONE session at HD 1080p takes ~5 s and is safe.

```javascript
await page.locator('text=Render').first().click();
await page.waitForTimeout(1500);

// Set date filter to the same session date, e.g. 2026-05-28
const dateInputs = await page.locator('input[type="date"]').all();
await dateInputs[0].fill('2026-05-28');
await dateInputs[1].fill('2026-05-28');
await page.waitForTimeout(400);

// Select the one visible track
await page.locator('button').filter({ hasText: /Select visible/ }).first().click();
await page.waitForTimeout(600);

// Choose HD 1080p so the render is fast
await page.locator('button:has-text("HD 1080p")').first().click();
await page.waitForTimeout(400);

// Click the Render button (the big bottom one — not "Select visible")
await page.locator('button').filter({ hasText: /Render \d+×\d+/ }).last().click();
await page.waitForTimeout(6000);  // wait for rAF-loop render to finish

await page.screenshot({ path: 'docs/screenshots/07_render.png', fullPage: false });
```

### Step 10 — Capture 08_export.png (Export tab)

```javascript
await page.locator('text=Export').first().click();
await page.waitForTimeout(2000);
await page.screenshot({ path: 'docs/screenshots/08_export.png', fullPage: false });
```

### Step 11 — Regenerate the collage

Run the collage script from the repo root:

```powershell
python scripts\build_screenshot_collage.py
```

Expected output:
```
Saved: docs\screenshots\09_collage.png  (NNN KB)
```

### Step 12 — Validate all files exist and are non-empty

```powershell
Get-ChildItem docs\screenshots\*.png | Select-Object Name, Length
```

All 9 files must have `Length > 10000` bytes. Any zero-byte or missing file
means a step failed; re-run that step individually.

### Step 13 — Commit

```powershell
git add docs/screenshots
git add README.md          # if README table was also updated
git commit -m "docs: refresh web viewer screenshots"
git push
```

---

## Collage Script Reference

**Script:** `scripts/build_screenshot_collage.py`

**Usage:**
```powershell
python scripts\build_screenshot_collage.py
```

**What it does:**
- Loads the 8 individual PNGs listed in the `IMAGES` list (hardcoded order)
- Thumbnails each to 640×360 px using LANCZOS resampling
- Arranges them 4 columns × 2 rows on a dark `(18, 18, 24)` background
- Draws a `(30, 30, 40)` label bar (32 px tall) under each thumbnail with a green label
- Saves result to `docs/screenshots/09_collage.png`

**Customising the layout:**
- Change `COLS`, `ROWS`, `THUMB_W`, `THUMB_H` at the top of the script
- Change the `IMAGES` list to reorder panels or swap in new screenshots
- `LABEL_FG = (0, 230, 118)` — green matching the app's `#00E676` brand colour

**Adding a new screenshot** (e.g. `10_new_feature.png`):
1. Capture it following the procedure above
2. Add its filename + label to the `IMAGES` list in `build_screenshot_collage.py`
3. Update `COLS`/`ROWS` if you're going from 8→9 panels (e.g. 3×3 or 5+4)
4. Run the script — collage regenerates automatically
5. Update the table in this file and in `README.md`

---

## README Integration

The screenshots are embedded in `README.md` under the `## Web Viewer` section
using GitHub raw-content URLs:

```
https://raw.githubusercontent.com/darkmatter2222/HELTEC-Wireless-Tracker-2.0-Rediacode-GPS-Recorder/<branch>/docs/screenshots/<filename>

> **Note**: replace `<branch>` with the active git branch (e.g. `main` after merge, or the
> session branch while screenshots are still on a feature branch). The README must use the
> branch where the PNG files actually live, not always `main`.
```

> **Note:** Raw URLs resolve from the `main` branch. Screenshots committed only
> to a session branch will not appear in the README until the branch is merged
> to `main`.

To update the README table after adding or renaming a screenshot, find the
`## Web Viewer` section in `README.md` and edit the `|` table rows to match
this index.

---

## Future Agent Instructions

If you are an AI agent asked to **update the screenshots**, do the following:

1. Read this file completely before touching anything.
2. Check `AGENTS.md` for the current server IP, credentials, and any infra changes.
3. Verify the viewer is running:
   ```powershell
   ssh -i ~/.ssh/id_rsa darkmatter2222@192.168.86.48 "curl -s -o /dev/null -w '%{http_code}' http://localhost:8031/tracker/"
   ```
   Expected: `200` or `401`. If `000` or `502`, start the container first.
4. Open a browser page to `http://darkmatter2222:liquimatter@192.168.86.48:8031/tracker/`
5. Follow Steps 1–13 above **in order**.
6. After capturing, use `view_image` on each PNG to confirm it looks correct and
   contains no PII (no street addresses, no full names — city/town names are fine).
7. Run the collage script.
8. Commit on the active session branch and push.
9. Update the `Captured on date`, `Git branch`, and `Git commit` fields in this
   file's **Capture Metadata** table to reflect the new capture.
10. If any new viewer feature was added since the last capture, add a new row
    to the **Screenshot Index** table, capture the new screenshot, add it to
    `build_screenshot_collage.py`'s `IMAGES` list, and update `README.md`.
