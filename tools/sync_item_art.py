#!/usr/bin/env python3
"""Compose the desktop-items atlas from the four source games' item art.

One 32x32 cell per (house style row, item sprite column), read from each
style's own game at its committed HEAD — the same committed-history
discipline as tools/sync_source_parity.py, so an in-flight art pass in a
game repository cannot reach the desktop until that repository commits it.
Column 0 is the drawn missing-item icon; columns past the mapped sprites
stay transparent and reserved.

This is an authoring tool, not a test: run `make items-art` after
changing the mapping, review the result by eye, and commit the PNG,
PROVENANCE.json, and refreshed manifest hash together.
tools/validate_items.py verifies the committed artifacts in `make test`.
"""

import hashlib
import json
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
GAMES_ROOT = ROOT.parents[1] / "games"
CELL = 32
COLUMNS = 8
ROWS = 4  # one per house style, in desk_cast order
ATLAS_ID = "desktop-items"
ATLAS_PATH = ROOT / "assets/graphics/items/desktop-items.png"
PROVENANCE_PATH = ROOT / "assets/graphics/items/PROVENANCE.json"
MANIFEST_PATH = ROOT / "assets/graphics/manifest.json"

STYLE_ACCENTS = {
    "legend": (255, 163, 60),
    "chumrunner": (85, 214, 208),
    "fantasy": (158, 226, 122),
    "pleb-bound": (255, 193, 92),
}

SOURCES = {
    # game, sheet path, cell width, cell height, crop inset. The painted
    # pleb-bound sheet lets art overflow its cell borders, so its crops
    # pull in from the edges to shed neighbor-cell slivers.
    "legend": ("legend-of-kilix", "assets/graphics/atlases/items.png",
               32, 32, 0),
    "chumrunner": ("chumrunner",
                   "assets/graphics/atlases/items-effects.png",
                   222, 222, 4),
    "fantasy": ("kilix-fantasy",
                "assets/graphics/atlases/items-effects.png", 24, 24, 0),
    "pleb-bound": ("pleb-bound",
                   "assets/graphics/atlases/items-effects.png",
                   192, 256, 22),
}

# (style row, sprite column) -> (source row, source col) in that style's
# sheet. Sprite columns match assets/world/items.json: 1 record, 2 coffee,
# 3 toolbox, 4 houseplant, 5 postcard. The thematic reading per style is
# deliberate (a data card is chumrunner's postcard).
CELL_MAP = {
    "legend": {1: (3, 5), 2: (1, 2), 3: (2, 7), 4: (3, 4), 5: (3, 2)},
    "chumrunner": {1: (0, 5), 2: (0, 2), 3: (0, 7), 4: (0, 6), 5: (0, 4)},
    "fantasy": {1: (1, 7), 2: (0, 0), 3: (3, 6), 4: (0, 2), 5: (0, 3)},
    "pleb-bound": {1: (1, 0), 2: (0, 1), 3: (3, 4), 4: (1, 2), 5: (0, 6)},
}

STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")

QUESTION_MARK = (
    "01110",
    "10001",
    "00001",
    "00010",
    "00100",
    "00000",
    "00100",
)


def committed_bytes(repo: Path, relative: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), "show", f"HEAD:{relative}"],
        capture_output=True, check=True)
    return result.stdout


def committed_commit(repo: Path) -> str:
    result = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                            capture_output=True, check=True)
    return result.stdout.decode("ascii").strip()[:12]


def fit_cell(cell: Image.Image) -> Image.Image:
    """Scale a source cell into 32x32: small pixel art is centered
    unscaled, painted art is LANCZOS-fit preserving aspect."""
    out = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    width, height = cell.size
    if width <= CELL and height <= CELL:
        out.paste(cell, ((CELL - width) // 2, (CELL - height) // 2), cell)
        return out
    scale = min(CELL / width, CELL / height)
    new_size = (max(1, round(width * scale)), max(1, round(height * scale)))
    scaled = cell.resize(new_size, Image.LANCZOS)
    out.paste(scaled, ((CELL - new_size[0]) // 2,
                       (CELL - new_size[1]) // 2), scaled)
    return out


def missing_icon(accent) -> Image.Image:
    """Dashed box plus a hand-drawn question mark in the style accent."""
    out = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    draw = ImageDraw.Draw(out)
    draw.rectangle((2, 2, CELL - 3, CELL - 3), fill=(20, 22, 30, 210))
    on = accent + (255,)
    for i in range(2, CELL - 2):
        if (i // 3) % 2 == 0:
            draw.point((i, 2), fill=on)
            draw.point((i, CELL - 3), fill=on)
            draw.point((2, i), fill=on)
            draw.point((CELL - 3, i), fill=on)
    glyph_w, glyph_h, scale = 5, 7, 3
    ox = (CELL - glyph_w * scale) // 2
    oy = (CELL - glyph_h * scale) // 2
    for row, bits in enumerate(QUESTION_MARK):
        for col, bit in enumerate(bits):
            if bit == "1":
                draw.rectangle((ox + col * scale, oy + row * scale,
                                ox + col * scale + scale - 1,
                                oy + row * scale + scale - 1), fill=on)
    return out


def main() -> int:
    atlas = Image.new("RGBA", (COLUMNS * CELL, ROWS * CELL), (0, 0, 0, 0))
    provenance = {
        "tool": "tools/sync_item_art.py",
        "atlas": ATLAS_ID,
        "grid": {"columns": COLUMNS, "rows": ROWS,
                 "cell_width": CELL, "cell_height": CELL},
        "cells": [],
    }
    for row, style in enumerate(STYLES):
        game, relative, cw, ch, inset = SOURCES[style]
        repo = GAMES_ROOT / game
        commit = committed_commit(repo)
        sheet = Image.open(
            __import__("io").BytesIO(committed_bytes(repo, relative))
        ).convert("RGBA")
        atlas.paste(missing_icon(STYLE_ACCENTS[style]), (0, row * CELL))
        provenance["cells"].append({
            "row": row, "column": 0, "style": style,
            "source": "drawn missing-item icon",
        })
        for column, (sr, sc) in sorted(CELL_MAP[style].items()):
            cell = sheet.crop((sc * cw + inset, sr * ch + inset,
                               (sc + 1) * cw - inset,
                               (sr + 1) * ch - inset))
            atlas.paste(fit_cell(cell), (column * CELL, row * CELL))
            provenance["cells"].append({
                "row": row, "column": column, "style": style,
                "game": game, "path": relative, "cell": [sr, sc],
                "commit": commit,
            })
    ATLAS_PATH.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(ATLAS_PATH)
    PROVENANCE_PATH.write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8")

    digest = hashlib.sha256(ATLAS_PATH.read_bytes()).hexdigest()
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    entry = {
        "id": ATLAS_ID,
        "path": "assets/graphics/items/desktop-items.png",
        "sha256": digest,
        "alpha_required": True,
        "grid": {"columns": COLUMNS, "rows": ROWS,
                 "width": COLUMNS * CELL, "height": ROWS * CELL,
                 "cell_width": CELL, "cell_height": CELL},
    }
    atlases = [item for item in manifest["atlases"]
               if item.get("id") != ATLAS_ID]
    atlases.append(entry)
    manifest["atlases"] = atlases
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8")
    print(f"PASS item art written: {ATLAS_PATH.relative_to(ROOT)} "
          f"sha256={digest[:12]}…")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
