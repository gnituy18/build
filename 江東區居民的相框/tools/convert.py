#!/usr/bin/env python3
"""
Convert a photo to the binary the 3.6" Spectra 6 e-paper expects.

Usage:
    python3 convert.py input.jpg output.bin

The output is exactly 120,000 bytes (400x600 panel, 2 pixels packed per
byte: high nibble = left pixel, low nibble = right pixel). Each nibble
is one of the 6 panel color codes below.
"""

from PIL import Image
import sys

PANEL_W, PANEL_H = 400, 600

# (panel code, RGB) — the 6 colors the Spectra 6 actually emits.
# Order here is the palette index; we map back to panel codes at pack time.
PALETTE = [
    (0x0, (0,   0,   0)),    # BLACK
    (0x1, (255, 255, 255)),  # WHITE
    (0x2, (255, 255, 0)),    # YELLOW
    (0x3, (255, 0,   0)),    # RED
    (0x5, (0,   0,   255)),  # BLUE
    (0x6, (0,   255, 0)),    # GREEN
]


def make_palette_image():
    flat = []
    for _code, rgb in PALETTE:
        flat.extend(rgb)
    flat.extend([0] * (768 - len(flat)))  # pad to 256 RGB slots
    pal = Image.new("P", (1, 1))
    pal.putpalette(flat)
    return pal


def convert(in_path, out_path):
    img = Image.open(in_path).convert("RGB")

    # Auto-rotate landscape photos to fit the portrait panel
    if img.width > img.height:
        img = img.rotate(-90, expand=True)

    # Cover-fit: scale so the photo fully covers 400x600, then center-crop
    scale = max(PANEL_W / img.width, PANEL_H / img.height)
    new_size = (round(img.width * scale), round(img.height * scale))
    img = img.resize(new_size, Image.LANCZOS)
    left = (img.width - PANEL_W) // 2
    top = (img.height - PANEL_H) // 2
    img = img.crop((left, top, left + PANEL_W, top + PANEL_H))

    # Floyd-Steinberg dither onto our 6-color palette
    img = img.quantize(
        palette=make_palette_image(),
        dither=Image.Dither.FLOYDSTEINBERG,
    )

    # Palette index -> panel color code
    idx_to_code = [code for code, _ in PALETTE]

    pixels = img.load()
    out = bytearray(PANEL_W * PANEL_H // 2)
    i = 0
    for y in range(PANEL_H):
        for x in range(0, PANEL_W, 2):
            hi = idx_to_code[pixels[x, y]]
            lo = idx_to_code[pixels[x + 1, y]]
            out[i] = (hi << 4) | lo
            i += 1

    with open(out_path, "wb") as f:
        f.write(bytes(out))

    print(f"Wrote {out_path}: {len(out)} bytes ({PANEL_W}x{PANEL_H})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
