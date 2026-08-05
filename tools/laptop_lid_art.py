#!/usr/bin/env python3
"""Build the optional closed-lid sheet for the laptop item.

The laptop's open art is desktop-items column 7 (tools/laptop_item_art.py);
this tool builds its CLOSED counterpart: assets/graphics/items/
laptop-lid.png, one 32x32 cell per style row in the same row order. The
sheet is deliberately OPTIONAL and outside manifest.json — src/graphics.c
loads it like a room plate, present-and-valid or absent — so the art can
wait for review without breaking a tree, and a public clone without it
simply keeps drawing the open sprite.

Input is a directory of keyed RGBA images named <style>.png, one closed
laptop per style, generated with the Gemini image pipeline and
chroma-keyed offline. Pixel-art styles (legend, fantasy) reduce with
nearest so their pixels stay square; the painted styles use premultiplied
Lanczos, exactly like tools/laptop_item_art.py. A provenance record is
written beside the sheet (PROVENANCE-LID.json) and ships with it.
"""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
CELL = 32
PADDING = 1
SHEET_PATH = ROOT / "assets/graphics/items/laptop-lid.png"
PROVENANCE_PATH = ROOT / "assets/graphics/items/PROVENANCE-LID.json"

STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
PIXEL_STYLES = {"legend", "fantasy"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cells-dir",
        type=Path,
        required=True,
        help="directory holding keyed RGBA <style>.png closed-lid images",
    )
    parser.add_argument(
        "--generator",
        default="Gemini image generation (gemini-3-pro-image)",
        help="provenance source string recorded for the four cells",
    )
    return parser.parse_args()


def fitted_cell(source: Image.Image, pixel_art: bool) -> Image.Image:
    bounds = source.getchannel("A").getbbox()
    if bounds is None:
        raise SystemExit("closed-lid cell image has no visible subject")
    subject = source.crop(bounds)
    limit = CELL - 2 * PADDING
    scale = min(limit / subject.width, limit / subject.height)
    width = max(1, round(subject.width * scale))
    height = max(1, round(subject.height * scale))
    if pixel_art:
        resized = subject.resize((width, height), Image.Resampling.NEAREST)
    else:
        resized = subject.convert("RGBa").resize(
            (width, height), Image.Resampling.LANCZOS
        ).convert("RGBA")
    cell = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    cell.alpha_composite(resized, ((CELL - width) // 2,
                                   (CELL - height) // 2))
    return cell


def main() -> int:
    args = parse_args()
    sheet = Image.new("RGBA", (CELL, CELL * len(STYLES)), (0, 0, 0, 0))
    for row, style in enumerate(STYLES):
        source_path = args.cells_dir / f"{style}.png"
        if not source_path.exists():
            raise SystemExit(f"missing closed-lid cell image: {source_path}")
        with Image.open(source_path) as source:
            cell = fitted_cell(source.convert("RGBA"),
                               style in PIXEL_STYLES)
        sheet.alpha_composite(cell, (0, row * CELL))

    SHEET_PATH.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(SHEET_PATH)
    digest = hashlib.sha256(SHEET_PATH.read_bytes()).hexdigest()

    provenance = {
        "sheet": SHEET_PATH.name,
        "sha256": digest,
        "cell": CELL,
        "rows": list(STYLES),
        "cells": [
            {
                "row": row,
                "style": style,
                "source": args.generator,
                "notes": "original per-style CLOSED laptop lid; "
                         "chroma-keyed and fitted offline by "
                         "tools/laptop_lid_art.py",
            }
            for row, style in enumerate(STYLES)
        ],
    }
    PROVENANCE_PATH.write_text(json.dumps(provenance, indent=2) + "\n")

    print(f"laptop_lid_art: wrote {SHEET_PATH.name} "
          f"({CELL}x{CELL * len(STYLES)}) for {len(STYLES)} styles; "
          f"sha256 {digest[:12]}…")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
