"""
Build a 2×4 collage of web viewer screenshots for the README.

Also auto-updates the capture metadata table in docs/screenshots/SCREENSHOTS.md
with the current date, active git branch, and latest git commit hash so the
manifest stays in sync without manual editing.

Usage:
    python scripts/build_screenshot_collage.py

Outputs:  docs/screenshots/09_collage.png
Requires: Pillow  (pip install Pillow)
"""

import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def _git(args: list[str]) -> str:
    """Run a git command and return stdout, or '' on error."""
    try:
        return subprocess.check_output(
            ["git", *args], cwd=Path(__file__).parent.parent, text=True
        ).strip()
    except Exception:
        return ""


def _update_screenshots_md(manifest: Path) -> None:
    """Patch the Capture Metadata table in SCREENSHOTS.md with live values."""
    if not manifest.exists():
        return

    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    branch = _git(["branch", "--show-current"]) or "unknown"
    commit = _git(["rev-parse", "--short", "HEAD"]) or "unknown"

    text = manifest.read_text(encoding="utf-8")

    # Replace the three metadata cells in the table row that follows "| Item |"
    replacements = [
        (r"(\|\s*Captured on date\s*\|)[^\|]+(\|)", rf"\g<1> {today} \g<2>"),
        (r"(\|\s*Git branch\s*\|)[^\|]+(\|)", rf"\g<1> `{branch}` \g<2>"),
        (r"(\|\s*Git commit\s*\|)[^\|]+(\|)", rf"\g<1> `{commit}` \g<2>"),
    ]
    for pattern, replacement in replacements:
        text = re.sub(pattern, replacement, text)

    manifest.write_text(text, encoding="utf-8")
    print(f"Updated SCREENSHOTS.md  (branch={branch}, commit={commit}, date={today})")

REPO_ROOT = Path(__file__).parent.parent
SCREENSHOTS_DIR = REPO_ROOT / "docs" / "screenshots"
OUTPUT_PATH = SCREENSHOTS_DIR / "09_collage.png"

# Order: top-row left→right, bottom-row left→right
IMAGES = [
    ("01_explore_track.png",   "Explore — Track"),
    ("02_explore_dots.png",    "Explore — Dots"),
    ("03_explore_hexbin.png",  "Explore — Hex Bin"),
    ("05_stats_panel.png",     "Stats Panel"),
    ("06_data_management.png", "Data Management"),
    ("07_render.png",          "Render (PNG output)"),
    ("08_export.png",          "Export"),
    ("04_explore_arrows.png",  "Explore — Arrows"),
]

COLS = 4
ROWS = 2
THUMB_W = 640
THUMB_H = 360
LABEL_H = 32
PADDING = 8
BG_COLOR = (18, 18, 24)
LABEL_BG = (30, 30, 40)
LABEL_FG = (0, 230, 118)   # green accent matching the app

canvas_w = COLS * THUMB_W + (COLS + 1) * PADDING
canvas_h = ROWS * (THUMB_H + LABEL_H) + (ROWS + 1) * PADDING

canvas = Image.new("RGB", (canvas_w, canvas_h), BG_COLOR)
draw = ImageDraw.Draw(canvas)

# Try to load a small font; fall back to default if not available
try:
    font = ImageFont.truetype("arial.ttf", 14)
except Exception:
    font = ImageFont.load_default()

for idx, (filename, label) in enumerate(IMAGES):
    col = idx % COLS
    row = idx // COLS

    x = PADDING + col * (THUMB_W + PADDING)
    y = PADDING + row * (THUMB_H + LABEL_H + PADDING)

    img_path = SCREENSHOTS_DIR / filename
    if not img_path.exists():
        print(f"  MISSING: {filename}")
        continue

    img = Image.open(img_path).convert("RGB")
    img.thumbnail((THUMB_W, THUMB_H), Image.LANCZOS)

    # Paste thumbnail (centred horizontally within the cell)
    paste_x = x + (THUMB_W - img.width) // 2
    paste_y = y
    canvas.paste(img, (paste_x, paste_y))

    # Label bar below the thumbnail
    label_y = y + THUMB_H
    draw.rectangle([x, label_y, x + THUMB_W, label_y + LABEL_H], fill=LABEL_BG)
    # Centre the text in the label bar
    bbox = draw.textbbox((0, 0), label, font=font)
    text_w = bbox[2] - bbox[0]
    text_h = bbox[3] - bbox[1]
    draw.text(
        (x + (THUMB_W - text_w) // 2, label_y + (LABEL_H - text_h) // 2),
        label,
        fill=LABEL_FG,
        font=font,
    )

canvas.save(OUTPUT_PATH, "PNG", optimize=True)
print(f"Saved: {OUTPUT_PATH}  ({OUTPUT_PATH.stat().st_size // 1024} KB)")

_update_screenshots_md(SCREENSHOTS_DIR / "SCREENSHOTS.md")
