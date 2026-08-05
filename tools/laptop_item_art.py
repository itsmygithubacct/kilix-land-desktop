#!/usr/bin/env python3
"""Paste the four generated laptop cells into desktop-items column 7.

The desktop-items atlas is composed per style row: columns 1..6 come from
the four source games at their committed HEAD (tools/sync_item_art.py);
column 7 is the laptop, an item none of the source games ship, generated
per style with the Gemini image pipeline and prepared offline (chroma-key
removal, trim, per-style resample). This tool takes a directory of four
prepared or raw keyed RGBA images named <style>.png, fits each into its
32x32 cell, writes the atlas, refreshes the manifest hash, and records the
column-7 provenance — the same commit-together contract as `make
items-art`.

Pixel-art styles (legend, fantasy) are reduced with nearest so their
pixels stay square; the painted styles use premultiplied Lanczos like
tools/sync_item_art.py.
"""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
CELL = 32
COLUMN = 7
PADDING = 1
ATLAS_PATH = ROOT / "assets/graphics/items/desktop-items.png"
PROVENANCE_PATH = ROOT / "assets/graphics/items/PROVENANCE.json"
MANIFEST_PATH = ROOT / "assets/graphics/manifest.json"

STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
PIXEL_STYLES = {"legend", "fantasy"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cells-dir",
        type=Path,
        required=True,
        help="directory holding keyed RGBA <style>.png laptop images",
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
        raise SystemExit("laptop cell image has no visible subject")
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
    with Image.open(ATLAS_PATH) as current:
        atlas = current.convert("RGBA")

    for row, style in enumerate(STYLES):
        source_path = args.cells_dir / f"{style}.png"
        if not source_path.exists():
            raise SystemExit(f"missing laptop cell image: {source_path}")
        with Image.open(source_path) as source:
            cell = fitted_cell(source.convert("RGBA"),
                               style in PIXEL_STYLES)
        corner = (COLUMN * CELL, row * CELL)
        atlas.paste((0, 0, 0, 0), (*corner, corner[0] + CELL,
                                   corner[1] + CELL))
        atlas.alpha_composite(cell, corner)

    atlas.save(ATLAS_PATH)
    digest = hashlib.sha256(ATLAS_PATH.read_bytes()).hexdigest()

    manifest = json.loads(MANIFEST_PATH.read_text())
    entry = next(item for item in manifest["atlases"]
                 if item["id"] == "desktop-items")
    entry["sha256"] = digest
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n")

    provenance = json.loads(PROVENANCE_PATH.read_text())
    provenance["cells"] = [
        cell for cell in provenance["cells"]
        if cell.get("column") != COLUMN
    ]
    for row, style in enumerate(STYLES):
        provenance["cells"].append({
            "row": row,
            "column": COLUMN,
            "style": style,
            "source": args.generator,
            "notes": "original per-style laptop item; chroma-keyed and "
                     "fitted offline by tools/laptop_item_art.py",
        })
    provenance["cells"].sort(key=lambda cell: (cell["row"],
                                               cell["column"]))
    PROVENANCE_PATH.write_text(json.dumps(provenance, indent=2) + "\n")

    print(f"laptop_item_art: wrote column {COLUMN} for {len(STYLES)} "
          f"styles; manifest sha256 {digest[:12]}…")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
