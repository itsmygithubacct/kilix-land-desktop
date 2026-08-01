#!/usr/bin/env python3
"""Compose the desktop-item-actions atlas: held-item animation cells.

For each house style, three action clips of four 32x32 frames each —
use-tool (toolbox swing), drink (cup tilt with steam), give (offered
gift with sparkle) — composed deterministically from that style's own
cells in the committed desktop-items atlas, plus hand-placed effect
pixels in the style accent. No new source art: the same provenance chain
as tools/sync_item_art.py, one derivation step longer.

Atlas layout: 4 columns (frames 0..3), 12 rows (style-major: rows 0-2
legend use-tool/drink/give, rows 3-5 chumrunner, 6-8 fantasy, 9-11
pleb-bound). `--preview DIR` also writes one labeled, upscaled 4x3 sheet
per style for review. This is an authoring tool (`make actions-art`),
not a test; validate_items.py owns the committed artifact checks.
"""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
CELL = 32
FRAMES = 4
CLIPS = ("use-tool", "drink", "give")
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
ITEMS_ATLAS = ROOT / "assets/graphics/items/desktop-items.png"
ATLAS_PATH = ROOT / "assets/graphics/items/desktop-item-actions.png"
PROVENANCE_PATH = ROOT / "assets/graphics/items/PROVENANCE-ACTIONS.json"
MANIFEST_PATH = ROOT / "assets/graphics/manifest.json"
ATLAS_ID = "desktop-item-actions"

STYLE_ACCENTS = {
    "legend": (255, 163, 60),
    "chumrunner": (85, 214, 208),
    "fantasy": (158, 226, 122),
    "pleb-bound": (255, 193, 92),
}

# Which desktop-items column each clip animates: the toolbox, the cup,
# and the gift.
CLIP_SOURCE_COLUMN = {"use-tool": 3, "drink": 2, "give": 5}

# Per-frame item transform: (rotate degrees CCW, dx, dy, scale).
CLIP_POSES = {
    "use-tool": ((-18, -3, 2, 0.94), (-42, -5, -4, 1.0),
                 (26, 4, 3, 1.0), (10, 2, 4, 0.94)),
    "drink": ((0, 0, 2, 0.94), (-14, -1, 0, 0.97),
              (-38, -2, -2, 1.0), (0, 0, 1, 0.94)),
    "give": ((0, 0, 4, 0.82), (-6, 0, 0, 0.9),
             (0, 0, -3, 1.0), (0, 0, -2, 0.94)),
}

WHITE = (248, 244, 232)
STEAM = (226, 226, 222)
DROP = (150, 208, 236)


def sparkle(draw, x, y, color, radius=2):
    draw.line([(x - radius, y), (x + radius, y)], fill=color)
    draw.line([(x, y - radius), (x, y + radius)], fill=color)


def star_burst(draw, x, y, color, radius=5):
    sparkle(draw, x, y, color, radius)
    for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        draw.line([(x + dx * 2, y + dy * 2),
                   (x + dx * (radius - 1), y + dy * (radius - 1))],
                  fill=color)
    draw.point((x, y), fill=WHITE)


def arc_trail(draw, accent):
    for x, y in ((22, 6), (26, 10), (28, 15)):
        draw.point((x, y), fill=accent)
        draw.point((x - 1, y + 1), fill=accent + (150,))


def steam_wisps(draw, x, tall):
    for step in range(tall):
        wiggle = 1 if step % 2 == 0 else -1
        draw.point((x + wiggle, 8 - step * 3), fill=STEAM)
        draw.point((x + wiggle, 7 - step * 3), fill=STEAM + (140,))


def effect_layer(clip, frame, accent):
    layer = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    if clip == "use-tool":
        if frame == 1:
            arc_trail(draw, accent)
        elif frame == 2:
            star_burst(draw, 24, 22, accent)
        elif frame == 3:
            sparkle(draw, 25, 23, accent + (150,), 1)
    elif clip == "drink":
        if frame == 0:
            steam_wisps(draw, 16, 1)
        elif frame == 1:
            steam_wisps(draw, 15, 2)
        elif frame == 2:
            draw.point((11, 20), fill=DROP)
            draw.point((12, 22), fill=DROP + (170,))
        else:
            steam_wisps(draw, 14, 2)
            steam_wisps(draw, 19, 1)
    else:  # give
        if frame == 1:
            sparkle(draw, 24, 8, accent, 1)
        elif frame == 2:
            sparkle(draw, 6, 7, accent, 2)
            sparkle(draw, 25, 5, WHITE, 2)
            sparkle(draw, 26, 18, accent, 1)
        elif frame == 3:
            sparkle(draw, 7, 8, accent + (150,), 1)
            sparkle(draw, 25, 6, accent + (120,), 1)
    return layer


def pose_item(item, pose):
    degrees, dx, dy, scale = pose
    cell = item
    if scale != 1.0:
        size = max(1, round(CELL * scale))
        cell = item.resize((size, size), Image.NEAREST)
    rotated = cell.rotate(degrees, resample=Image.NEAREST, expand=False)
    out = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    out.paste(rotated, ((CELL - rotated.width) // 2 + dx,
                        (CELL - rotated.height) // 2 + dy), rotated)
    return out


def compose_frame(items_atlas, style_row, clip, frame, accent):
    column = CLIP_SOURCE_COLUMN[clip]
    item = items_atlas.crop((column * CELL, style_row * CELL,
                             (column + 1) * CELL, (style_row + 1) * CELL))
    cell = pose_item(item, CLIP_POSES[clip][frame])
    cell.alpha_composite(effect_layer(clip, frame, accent))
    return cell


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", type=Path,
                        help="also write labeled per-style review sheets")
    parser.add_argument("--no-manifest", action="store_true",
                        help="compose only; leave manifest.json untouched")
    args = parser.parse_args()

    items_atlas = Image.open(ITEMS_ATLAS).convert("RGBA")
    atlas = Image.new("RGBA", (FRAMES * CELL, len(STYLES) * len(CLIPS) *
                               CELL), (0, 0, 0, 0))
    provenance = {
        "tool": "tools/sync_action_art.py",
        "atlas": ATLAS_ID,
        "source": "assets/graphics/items/desktop-items.png",
        "grid": {"columns": FRAMES, "rows": len(STYLES) * len(CLIPS),
                 "cell_width": CELL, "cell_height": CELL},
        "cells": [],
    }
    for style_index, style in enumerate(STYLES):
        for clip_index, clip in enumerate(CLIPS):
            row = style_index * len(CLIPS) + clip_index
            for frame in range(FRAMES):
                cell = compose_frame(items_atlas, style_index, clip,
                                     frame, STYLE_ACCENTS[style])
                atlas.paste(cell, (frame * CELL, row * CELL))
                provenance["cells"].append({
                    "row": row, "column": frame, "style": style,
                    "clip": clip,
                    "item_column": CLIP_SOURCE_COLUMN[clip],
                    "pose": list(CLIP_POSES[clip][frame]),
                })
    ATLAS_PATH.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(ATLAS_PATH)
    PROVENANCE_PATH.write_text(json.dumps(provenance, indent=2) + "\n",
                               encoding="utf-8")

    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
        scale = 4
        pad = 10
        for style_index, style in enumerate(STYLES):
            width = FRAMES * CELL * scale + pad * (FRAMES + 1)
            height = len(CLIPS) * (CELL * scale + 16) + pad * 2 + 18
            sheet = Image.new("RGBA", (width, height), (22, 24, 34, 255))
            draw = ImageDraw.Draw(sheet)
            draw.text((pad, 4), f"{style} - action cells (frames 0-3)",
                      fill=(240, 230, 200, 255))
            for clip_index, clip in enumerate(CLIPS):
                row = style_index * len(CLIPS) + clip_index
                top = 22 + clip_index * (CELL * scale + 16)
                draw.text((pad, top + CELL * scale + 1), clip,
                          fill=STYLE_ACCENTS[style] + (255,))
                for frame in range(FRAMES):
                    cell = atlas.crop((frame * CELL, row * CELL,
                                       (frame + 1) * CELL,
                                       (row + 1) * CELL))
                    cell = cell.resize((CELL * scale, CELL * scale),
                                       Image.NEAREST)
                    left = pad + frame * (CELL * scale + pad)
                    sheet.paste(cell, (left, top), cell)
            sheet.save(args.preview / f"actions-{style}.png")

    if not args.no_manifest:
        digest = hashlib.sha256(ATLAS_PATH.read_bytes()).hexdigest()
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        entry = {
            "id": ATLAS_ID,
            "path": "assets/graphics/items/desktop-item-actions.png",
            "sha256": digest,
            "alpha_required": True,
            "grid": {"columns": FRAMES,
                     "rows": len(STYLES) * len(CLIPS),
                     "width": FRAMES * CELL,
                     "height": len(STYLES) * len(CLIPS) * CELL,
                     "cell_width": CELL, "cell_height": CELL},
        }
        atlases = [item for item in manifest["atlases"]
                   if item.get("id") != ATLAS_ID]
        atlases.append(entry)
        manifest["atlases"] = atlases
        MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n",
                                 encoding="utf-8")
    print(f"PASS action art written: {ATLAS_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
