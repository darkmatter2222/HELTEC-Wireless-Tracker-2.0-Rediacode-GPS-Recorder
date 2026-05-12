#!/usr/bin/env python3
"""
render_screens.py — generate PNG mockups of the four TFT screens at 3× scale
(480 × 240 px, representing the 160 × 80 display).

Requires Pillow:
    pip install Pillow

Usage:
    python scripts/render_screens.py

Output: docs/screens/screen_{stats,gps,storage,picker}.png
"""

import os
import sys
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Display constants
# ---------------------------------------------------------------------------
SCALE   = 3
DW, DH  = 160, 80   # native display pixels
W, H    = DW * SCALE, DH * SCALE
HEADER_H = 12       # native header height

# ---------------------------------------------------------------------------
# BGR565 → RGB888
# The ST7735 on the HTIT-Tracker uses BGR bit order so R and B fields are
# swapped relative to standard RGB565.  Convert before handing to Pillow.
# ---------------------------------------------------------------------------
def bgr(c: int):
    r = ((c & 0x001F)         * 255 + 15) // 31
    g = (((c >> 5)  & 0x3F)   * 255 + 31) // 63
    b = (((c >> 11) & 0x1F)   * 255 + 15) // 31
    return (r, g, b)

COL_BG     = bgr(0x0000)   # black
COL_FG     = bgr(0xFFFF)   # white
COL_DIM    = bgr(0x8C71)   # medium gray
COL_GREEN  = bgr(0x07E0)   # green
COL_RED    = bgr(0x001F)   # red  (inverted blue field = red in BGR)
COL_AMBER  = bgr(0x053F)   # amber / orange
COL_HEADER = bgr(0x10A2)   # dark blue band
COL_PICK   = bgr(0x041F)   # selected-row highlight

# ---------------------------------------------------------------------------
# Fonts – try a few common monospace paths
# ---------------------------------------------------------------------------
_FONT_PATHS = [
    "C:/Windows/Fonts/cour.ttf",
    "C:/Windows/Fonts/lucon.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
]

def _load_font(px: int):
    for path in _FONT_PATHS:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, px)
            except Exception:
                pass
    # Fallback: PIL built-in bitmap font (will be tiny but functional)
    return ImageFont.load_default()

# Adafruit GFX uses a 5×8 bitmap character cell (6px wide with 1px gap).
# At SCALE=3 that is 18px per character wide, 24px tall.
# We pick TrueType sizes that approximate that char density.
fonts = {
    1: _load_font(15),   # text size 1: native 8px → 24px scaled
    2: _load_font(24),   # text size 2: native 16px → 48px scaled
    3: _load_font(38),   # text size 3: native 24px → 72px scaled
}

# ---------------------------------------------------------------------------
# Primitive helpers
# ---------------------------------------------------------------------------
def s(n):
    """Scale a native-pixel value."""
    return n * SCALE

def fillrect(draw: ImageDraw.Draw, x, y, w, h, color):
    draw.rectangle([s(x), s(y), s(x + w) - 1, s(y + h) - 1], fill=color)

def drawrect(draw: ImageDraw.Draw, x, y, w, h, color):
    draw.rectangle([s(x), s(y), s(x + w) - 1, s(y + h) - 1], outline=color)

def circle(draw: ImageDraw.Draw, cx, cy, r, fill=None, outline=None):
    draw.ellipse([s(cx) - s(r), s(cy) - s(r),
                  s(cx) + s(r) - 1, s(cy) + s(r) - 1],
                 fill=fill, outline=outline)

def field(draw: ImageDraw.Draw, x, y, w, h, text, fg, bg, sz=1):
    """
    Mirrors Ui::field(): clear a background rect then draw left-aligned text
    with a 1-native-pixel inset from the left edge.
    """
    fillrect(draw, x, y, w, h, bg)
    draw.text((s(x) + SCALE, s(y)), text, font=fonts[sz], fill=fg)

# ---------------------------------------------------------------------------
# Header (12 native px), shared by all screens
# ---------------------------------------------------------------------------
def draw_header(draw, rc_state, rc_col, has_fix, bat_pct, recording):
    fillrect(draw, 0, 0, DW, HEADER_H, COL_HEADER)

    # RC state badge  (x=2, w=30)
    field(draw, 2, 2, 30, 8, rc_state, rc_col, COL_HEADER)

    # GPS fix badge   (x=36, w=44)
    fix_text = "GPS 3D" if has_fix else "GPS NO"
    fix_col  = COL_GREEN if has_fix else COL_RED
    field(draw, 36, 2, 44, 8, fix_text, fix_col, COL_HEADER)

    # Battery         (x=84, w=54)
    bat_text = f"BAT {bat_pct:3d}%" if bat_pct >= 0 else "BAT --% "
    if bat_pct < 0:     bat_col = COL_DIM
    elif bat_pct < 20:  bat_col = COL_RED
    elif bat_pct < 40:  bat_col = COL_AMBER
    else:               bat_col = COL_GREEN
    field(draw, 84, 2, 54, 8, bat_text, bat_col, COL_HEADER)

    # Recording dot   (cx=149, cy=6, r=4)
    if recording:
        circle(draw, 149, 6, 4, fill=COL_RED)
    else:
        circle(draw, 149, 6, 4, outline=COL_DIM)

# ---------------------------------------------------------------------------
# STATS screen — dose rate + CPS
# ---------------------------------------------------------------------------
def render_stats():
    img  = Image.new("RGB", (W, H), COL_BG)
    draw = ImageDraw.Draw(img)
    draw_header(draw, "OK  ", COL_GREEN, has_fix=True, bat_pct=87, recording=True)

    field(draw, 4,  14, 60, 8, "DOSE nSv/h", COL_DIM,   COL_BG)
    field(draw, 4,  22, 110, 22, " 1250",     COL_GREEN, COL_BG, sz=3)
    field(draw, 116, 26, 42, 8,  "+/- 8%",   COL_DIM,   COL_BG)

    field(draw, 4,  46, 30, 8,  "CPS",        COL_DIM,   COL_BG)
    field(draw, 36, 46, 80, 16, " 15.3",      COL_FG,    COL_BG, sz=2)
    field(draw, 116, 50, 42, 8, "+/-12%",     COL_DIM,   COL_BG)

    # Footer: GPS accuracy (shown when RC connected + fix)
    field(draw, 4, 66, 156, 8, "+/- 3.0m  hdop 1.0", COL_GREEN, COL_BG)
    return img

# ---------------------------------------------------------------------------
# GPS screen — fix quality + coordinates
# ---------------------------------------------------------------------------
def render_gps():
    img  = Image.new("RGB", (W, H), COL_BG)
    draw = ImageDraw.Draw(img)
    draw_header(draw, "OK  ", COL_GREEN, has_fix=True, bat_pct=87, recording=True)

    field(draw, 4,  14, 76, 8, "FIX 3D",   COL_GREEN, COL_BG)
    field(draw, 4,  26, 76, 8, "Sats 9",   COL_GREEN, COL_BG)
    field(draw, 4,  38, 76, 8, "HDOP 1.0", COL_GREEN, COL_BG)
    field(draw, 4,  50, 76, 8, "+/- 3.0m", COL_GREEN, COL_BG)

    field(draw, 84, 14, 76, 8, "47.60621",  COL_FG, COL_BG)
    field(draw, 84, 26, 76, 8, "-122.3321", COL_FG, COL_BG)
    field(draw, 84, 38, 76, 8, "12m",       COL_FG, COL_BG)
    field(draw, 84, 50, 76, 8, "48.2kph",   COL_FG, COL_BG)

    field(draw, 4, 66, 156, 8, "Hdg 267 deg", COL_DIM, COL_BG)
    return img

# ---------------------------------------------------------------------------
# STORAGE screen — recording status + upload pipeline
# ---------------------------------------------------------------------------
def render_storage():
    img  = Image.new("RGB", (W, H), COL_BG)
    draw = ImageDraw.Draw(img)
    draw_header(draw, "OK  ", COL_GREEN, has_fix=True, bat_pct=87, recording=True)

    # Row 1: non-overlapping layout (v0.4.9)
    # field(30) x=4  w=24   "REC"
    # field(31) x=30 w=50   auto-mode label
    # field(32) x=82 w=74   "Samp N"
    field(draw, 4,  14, 24, 8, "REC",      COL_DIM,   COL_BG)
    field(draw, 30, 14, 50, 8, "AUTO ok",  COL_GREEN, COL_BG)
    field(draw, 82, 14, 74, 8, "Samp 1250", COL_FG,   COL_BG)

    # Day
    field(draw, 4, 26, 156, 8, "Day 2026-05-11", COL_FG, COL_BG)

    # Disk usage
    field(draw, 4, 38, 156, 8, "Disk 12%  623/5376K", COL_DIM, COL_BG)

    # Disk bar  (barX=4, barY=50, barW=152, barH=6)
    bx, by, bw, bh = 4, 50, DW - 8, 6
    pct = 0.12
    drawrect(draw, bx,     by,     bw,     bh,     COL_DIM)
    fillrect(draw, bx + 1, by + 1, bw - 2, bh - 2, COL_BG)
    fill_w = max(1, int((bw - 2) * pct))
    fillrect(draw, bx + 1, by + 1, fill_w, bh - 2, COL_GREEN)

    # Pending files (green = 0 pending)
    field(draw, 4, 58, 156, 8, "Pending: 0", COL_GREEN, COL_BG)

    # Wi-Fi countdown
    field(draw, 4, 70, 156, 8, "Next sync: 55s", COL_DIM, COL_BG)
    return img


def render_storage_uploading():
    """Variant: mid-upload with 2 pending files."""
    img  = Image.new("RGB", (W, H), COL_BG)
    draw = ImageDraw.Draw(img)
    draw_header(draw, "OK  ", COL_GREEN, has_fix=True, bat_pct=87, recording=True)

    field(draw, 4,  14, 24, 8, "REC",      COL_DIM,   COL_BG)
    field(draw, 30, 14, 50, 8, "AUTO ok",  COL_GREEN, COL_BG)
    field(draw, 82, 14, 74, 8, "Samp 38",  COL_FG,    COL_BG)

    field(draw, 4, 26, 156, 8, "Day 2026-05-11", COL_FG, COL_BG)
    field(draw, 4, 38, 156, 8, "Disk 11%  586/5376K", COL_DIM, COL_BG)

    bx, by, bw, bh = 4, 50, DW - 8, 6
    pct = 0.11
    drawrect(draw, bx,     by,     bw,     bh,     COL_DIM)
    fillrect(draw, bx + 1, by + 1, bw - 2, bh - 2, COL_BG)
    fill_w = max(1, int((bw - 2) * pct))
    fillrect(draw, bx + 1, by + 1, fill_w, bh - 2, COL_GREEN)

    field(draw, 4, 58, 156, 8, "Pending: 2", COL_AMBER, COL_BG)
    field(draw, 4, 70, 156, 8, "Wi-Fi: uploading...", COL_GREEN, COL_BG)
    return img

# ---------------------------------------------------------------------------
# PICKER screen — BLE device list
# ---------------------------------------------------------------------------
def render_picker():
    img  = Image.new("RGB", (W, H), COL_BG)
    draw = ImageDraw.Draw(img)
    draw_header(draw, "SCAN", COL_AMBER, has_fix=True, bat_pct=87, recording=False)

    entries = [
        ("RadiaCode-5243",  "-62dB"),
        ("RadiaCode-0024",  "-71dB"),
        ("MyRC-110",        "-78dB"),
    ]

    for i, (name, rssi) in enumerate(entries):
        y   = 14 + i * 10
        bg  = COL_PICK if i == 0 else COL_BG
        label = f"{name:<14} {rssi}"
        field(draw, 2, y, 156, 9, label, COL_FG, bg)

    # Empty rows to fill the list area
    field(draw, 2, 44, 156, 9, "", COL_DIM, COL_BG)
    field(draw, 2, 54, 156, 9, "", COL_DIM, COL_BG)

    # Cancel row
    field(draw, 2, 64, 156, 9, "Cancel", COL_DIM, COL_BG)

    # Hints
    field(draw, 4, 72, 156, 8, "Short=scroll  Long=connect", COL_DIM, COL_BG)
    return img

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    repo_root = os.path.join(os.path.dirname(__file__), "..")
    out_dir   = os.path.join(repo_root, "docs", "screens")
    os.makedirs(out_dir, exist_ok=True)

    screens = {
        "stats":             render_stats(),
        "gps":               render_gps(),
        "storage":           render_storage(),
        "storage_uploading": render_storage_uploading(),
        "picker":            render_picker(),
    }

    for name, img in screens.items():
        path = os.path.join(out_dir, f"screen_{name}.png")
        img.save(path)
        print(f"  {path}")

    print(f"\nDone — {len(screens)} PNG files in docs/screens/")

if __name__ == "__main__":
    main()
