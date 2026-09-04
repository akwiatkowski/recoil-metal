#!/usr/bin/env python3
"""Compose a labelled contact sheet from a set of PNG captures, with Pillow.

    tools/hud_contact_sheet.py OUT.jpg TILE_WIDTH IMAGE...

Each image is scaled to TILE_WIDTH keeping its aspect, laid out four to a row, and labelled
with its file stem. Pillow rather than ImageMagick's `montage`: the latter shells out to
Ghostscript for text, which this machine does not have, and a contact sheet without labels
is a puzzle rather than evidence.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

COLUMNS = 4
GUTTER = 12
LABEL_HEIGHT = 26
BACKGROUND = (32, 36, 40)
INK = (199, 214, 220)


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(argv[1])
    tile_width = int(argv[2])
    sources = [Path(p) for p in argv[3:]]

    tiles: list[tuple[str, Image.Image]] = []
    for source in sources:
        if not source.exists():
            continue
        with Image.open(source) as image:
            scale = tile_width / image.width
            tile = image.convert("RGB").resize((tile_width, max(1, round(image.height * scale))))
        tiles.append((source.stem, tile))
    if not tiles:
        print("no images", file=sys.stderr)
        return 1

    tile_height = max(tile.height for _, tile in tiles)
    rows = (len(tiles) + COLUMNS - 1) // COLUMNS
    sheet = Image.new(
        "RGB",
        (COLUMNS * tile_width + (COLUMNS + 1) * GUTTER,
         rows * (tile_height + LABEL_HEIGHT) + (rows + 1) * GUTTER),
        BACKGROUND,
    )
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 14)
    except OSError:
        font = ImageFont.load_default()
    for index, (label, tile) in enumerate(tiles):
        col, row = index % COLUMNS, index // COLUMNS
        x = GUTTER + col * (tile_width + GUTTER)
        y = GUTTER + row * (tile_height + LABEL_HEIGHT + GUTTER)
        draw.text((x, y + 4), label, fill=INK, font=font)
        sheet.paste(tile, (x, y + LABEL_HEIGHT))
    sheet.save(out, quality=88)
    print(f"wrote {out} ({len(tiles)} tiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
