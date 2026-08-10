#!/usr/bin/env python3
"""Generate the per-style walk-behind masks (<room>-behind.png).

For every declared region this seeds a flood fill inside the furniture on
the style's room plate: a pixel joins the region when it is 4-connected to
a seed through pixels whose RGB is within the region's tolerance
(Euclidean) of ANY seed's color — and NOT within avoid_tolerance of any
declared avoid color (floor/wall swatches guarding bleed paths) — clipped
to a generous bbox. Enclosed holes (dark outlines, interior detail) are
then filled — any non-region pocket inside the bbox that cannot reach the
bbox border becomes region — specks below MIN_ISLAND pixels are dropped,
and the result dilates by `grow` pixels (default 2) to recover the dark
outline ring the color match cannot claim, then hole-fills once more.

Output masks are plate-sized (1280x720) 8-bit grayscale PNGs where pixel
value 0 = no walk-behind and 1..15 = the region id, written to
assets/graphics/rooms/<style>/<room>-behind.png. Region ids and their
baselines are authored in assets/world/world.json ("walkbehinds").

--review DIR additionally writes DIR/<room>-<style>.png with the plate
dimmed and each region tinted (id 1 red, id 2 cyan, ...) for visual
inspection, plus the bbox outlines.
"""

import argparse
import os
import sys
from collections import deque

from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOMS_DIR = os.path.join(REPO, "assets", "graphics", "rooms")
PLATE_W, PLATE_H = 1280, 720
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
MIN_ISLAND = 40

REVIEW_TINTS = {
    1: (255, 60, 60),
    2: (60, 220, 255),
}


def region(rid, bbox, tolerance, seeds, avoid=(), avoid_tolerance=25,
           grow=2, smooth=2, polys=()):
    """avoid: plate points whose colors repel the fill (floor/wall
    swatches next to the furniture); grow: outward dilation in px;
    smooth: morphological-opening radius that trims sliver bleed (thin
    plank-gap stripes) before the dilation; polys: hand-traced polygons
    (vertex lists) unioned into the region after the fill — for furniture
    whose colors match its background too closely to flood. A room+style
    may declare several entries with the same id — sub-fills union."""
    return {"id": rid, "bbox": bbox, "tolerance": tolerance,
            "seeds": seeds, "avoid": tuple(avoid),
            "avoid_tolerance": avoid_tolerance, "grow": grow,
            "smooth": smooth, "polys": tuple(polys)}


# Per (room, style) region declarations. bbox = (x0, y0, x1, y1) exclusive
# right/bottom, plate pixels. Seeds are plate (x, y) points inside the
# furniture, spread across its distinct color patches.
REGIONS = {
    ("bedroom", "legend"): [
        region(1, (140, 350, 575, 650), 40, [
            (200, 470), (235, 472), (320, 500), (290, 530), (310, 560),
            (230, 380), (233, 440), (528, 470), (410, 500), (500, 500),
            (430, 535), (260, 505), (420, 555), (455, 560), (210, 540),
            (185, 520),
        ], avoid=[(170, 620), (300, 620), (350, 380), (450, 410),
                  (320, 410), (400, 415)], avoid_tolerance=18),
        # headboard rail + left legs (wood ~ floor color, so a tight
        # local patch instead of loosening the main fill)
        region(1, (145, 400, 250, 620), 34, [
            (175, 420), (205, 420), (160, 450), (230, 450), (165, 555),
            (160, 590),
        ], avoid=[(350, 380), (320, 410)], avoid_tolerance=18),
        # foot post shaft
        region(1, (505, 450, 560, 630), 34, [
            (528, 470), (530, 500), (535, 540), (533, 580), (530, 610),
        ], avoid=[(170, 620)], avoid_tolerance=16),
    ],
    ("bedroom", "chumrunner"): [
        region(1, (100, 275, 545, 545), 36, [
            (170, 310), (190, 330), (135, 380), (205, 415), (275, 415),
            (345, 415), (380, 415), (485, 415), (170, 450), (205, 450),
            (310, 520), (380, 520),
        ], avoid=[(450, 310), (135, 520)], avoid_tolerance=14,
            polys=[
                [(122, 286), (233, 286), (233, 408), (122, 408)],
                [(150, 398), (515, 402), (540, 435), (535, 470),
                 (470, 505), (465, 530), (295, 530), (200, 480),
                 (150, 470)],
            ]),
    ],
    ("bedroom", "fantasy"): [
        region(1, (45, 330, 475, 545), 40, [
            (110, 395), (140, 395), (170, 395), (200, 395), (260, 395),
            (290, 395), (230, 425), (260, 425), (290, 425), (320, 425),
            (320, 395), (350, 395), (290, 485), (320, 485), (80, 365),
            (110, 365), (140, 365), (380, 425), (410, 425), (200, 455),
            (110, 455), (140, 455), (170, 455),
        ], avoid=[(50, 515), (440, 515), (440, 335), (380, 335)],
            avoid_tolerance=14),
    ],
    ("bedroom", "pleb-bound"): [
        region(1, (15, 408, 468, 706), 40, [
            (295, 430), (330, 430), (365, 430), (400, 430), (225, 465),
            (260, 465), (295, 465), (330, 465), (365, 465), (295, 500),
            (330, 500), (365, 500), (225, 535), (260, 535), (330, 535),
            (435, 415), (50, 430), (85, 430), (120, 430),
            (190, 430), (50, 465), (85, 465), (120, 465), (155, 465),
            (50, 500), (85, 500), (155, 500), (50, 570),
            (120, 570), (190, 570), (260, 570), (85, 675), (155, 675),
            (225, 675), (295, 675),
        ], avoid=[(85, 605), (435, 605), (470, 640), (120, 395),
                  (50, 375), (15, 360), (120, 360)],
            avoid_tolerance=14),
    ],
    ("living", "legend"): [
        region(1, (555, 465, 740, 610), 38, [
            (630, 495), (660, 510), (645, 510), (585, 525), (645, 525),
            (690, 525), (705, 525), (645, 540),
        ], avoid=[(720, 540), (600, 480), (600, 465)],
            avoid_tolerance=18),
        # legs share the dark floor-row color, so a tight local patch
        region(1, (570, 535, 725, 605), 36, [
            (585, 570), (705, 555), (645, 545),
        ]),
    ],
    ("living", "chumrunner"): [
        region(1, (560, 495, 765, 610), 30, [
            (695, 530), (715, 530), (655, 550), (675, 550), (695, 550),
        ], avoid=[(575, 530), (575, 610)], avoid_tolerance=18,
            polys=[
                [(573, 517), (748, 505), (756, 548), (585, 560)],
                [(570, 550), (588, 550), (588, 606), (570, 606)],
                [(698, 535), (716, 535), (716, 600), (698, 600)],
                [(738, 528), (754, 528), (754, 585), (738, 585)],
            ]),
    ],
    ("living", "fantasy"): [
        # The table stands on a rug whose palette matches the tabletop
        # almost exactly, so this region is traced rather than flooded.
        region(1, (545, 448, 748, 585), 30, [], polys=[
            [(586, 458), (660, 452), (700, 460), (700, 505), (590, 507)],
            [(558, 505), (738, 496), (744, 532), (566, 543)],
            [(566, 540), (584, 542), (584, 580), (566, 580)],
            [(698, 532), (716, 530), (716, 572), (698, 572)],
        ]),
    ],
    ("living", "pleb-bound"): [
        region(1, (538, 405, 790, 610), 28, [
            (545, 510), (570, 510), (595, 510), (570, 535), (620, 535),
            (670, 535), (720, 535), (570, 460), (595, 460), (620, 460),
            (645, 485), (745, 485), (695, 510), (720, 510), (670, 485),
            (670, 510), (620, 435),
        ], avoid=[(620, 585), (545, 435), (795, 585)],
            avoid_tolerance=14,
            polys=[
                [(553, 545), (573, 545), (573, 605), (553, 605)],
                [(743, 538), (763, 538), (763, 598), (743, 598)],
            ]),
    ],
    ("kitchen", "legend"): [
        region(1, (640, 288, 960, 480), 40, [
            (660, 360), (735, 360), (835, 360), (905, 360),
            (785, 310), (860, 315), (910, 310), (785, 410),
            (860, 410), (735, 435),
        ], avoid=[(635, 335), (610, 385), (660, 335)],
            avoid_tolerance=20),
    ],
    ("kitchen", "chumrunner"): [
        region(1, (600, 310, 950, 480), 36, [
            (610, 325), (630, 325), (660, 355), (690, 355), (660, 415),
            (690, 415), (630, 415), (720, 445), (780, 325), (810, 325),
            (870, 355), (900, 355), (810, 385), (870, 415), (900, 445),
        ], avoid=[(630, 475)], avoid_tolerance=18),
    ],
    ("kitchen", "fantasy"): [
        region(1, (415, 285, 950, 510), 60, [
            (500, 400), (600, 430), (660, 380), (550, 360), (570, 455),
            (575, 480), (800, 430), (820, 390), (760, 495), (860, 495),
            (825, 335),
        ]),
    ],
    ("kitchen", "pleb-bound"): [
        region(1, (525, 280, 975, 520), 30, [
            (565, 360), (645, 360), (725, 360), (845, 360), (925, 360),
            (565, 440), (685, 440), (805, 440), (925, 440), (565, 480),
            (725, 480), (925, 480), (605, 320), (725, 320), (845, 320),
            (725, 280),
        ], avoid=[(845, 280)], avoid_tolerance=20),
    ],
    ("yard", "legend"): [
        region(1, (760, 85, 1085, 515), 34, [
            (855, 115), (855, 150), (890, 185), (925, 220), (925, 325),
            (1030, 325), (995, 395), (855, 290), (890, 360), (820, 395),
            (820, 430), (890, 430), (1065, 395),
        ], avoid=[(890, 80), (820, 500)], avoid_tolerance=26),
        region(2, (50, 258, 240, 470), 36, [
            (90, 305), (120, 305), (150, 305), (210, 305), (90, 285),
            (110, 265), (150, 265), (130, 325), (85, 325),
        ], avoid=[(50, 445), (150, 485), (190, 405), (130, 465),
                  (190, 445)], avoid_tolerance=18),
        # post shaft below the box
        region(2, (135, 340, 190, 515), 30, [
            (150, 370), (155, 410), (160, 450), (160, 490),
        ], avoid=[(190, 465), (150, 485)], avoid_tolerance=20),
    ],
    ("yard", "chumrunner"): [
        region(1, (770, 130, 1070, 520), 20, [
            (890, 285), (920, 285), (920, 345), (900, 405), (920, 435),
            (1040, 345), (950, 285), (950, 315), (830, 285), (830, 315),
            (980, 285), (1010, 315), (980, 345), (800, 405), (800, 435),
            (830, 405), (830, 435), (800, 150), (830, 165), (860, 135),
        ], avoid=[(980, 135), (1010, 135), (1040, 135), (1040, 165)],
            avoid_tolerance=12,
            polys=[
                [(778, 250), (860, 240), (862, 405), (782, 415)],
            ]),
        region(2, (50, 250, 205, 530), 26, [
            (115, 260), (95, 320), (175, 320), (95, 280), (115, 280),
            (75, 300), (55, 300), (135, 300), (155, 320), (135, 360),
            (140, 400),
        ], avoid=[(55, 380), (55, 440), (175, 460)], avoid_tolerance=16),
    ],
    ("yard", "fantasy"): [
        region(1, (812, 115, 1100, 500), 32, [
            (870, 330), (900, 330), (870, 360), (930, 360), (870, 390),
            (900, 420), (930, 420), (870, 450), (900, 450), (930, 450),
            (840, 240), (870, 240), (900, 210), (930, 240), (900, 270),
            (1020, 330), (1040, 345), (1080, 450), (1020, 480),
            (1080, 390),
        ], avoid=[(1110, 180), (810, 210), (840, 150), (840, 480),
                  (810, 480)], avoid_tolerance=22),
        region(2, (40, 230, 210, 700), 30, [
            (105, 335), (130, 335), (155, 335), (105, 360), (130, 360),
            (130, 385), (155, 385), (155, 360), (130, 285), (155, 285),
            (105, 285), (180, 285), (105, 235), (80, 260), (55, 310),
            (55, 360), (55, 410), (55, 510), (55, 610), (55, 660),
        ], avoid=[(30, 385), (130, 610), (155, 610), (155, 535)],
            avoid_tolerance=20),
    ],
    ("yard", "pleb-bound"): [
        region(1, (825, 35, 1165, 485), 28, [
            (890, 180), (890, 215), (890, 250), (890, 285), (925, 215),
            (960, 215), (960, 250), (890, 110), (925, 145), (960, 145),
            (855, 145), (1030, 390), (1065, 390), (1100, 390),
            (1030, 425), (1065, 425), (1100, 425), (1030, 215),
            (1065, 215), (1100, 215), (1065, 285), (1100, 285),
            (1135, 215), (1135, 285), (1135, 355), (995, 215),
            (995, 285), (995, 320), (1065, 355), (960, 110), (995, 110),
        ], avoid=[(925, 40), (1100, 75), (1135, 40), (890, 460),
                  (855, 425)], avoid_tolerance=14,
            polys=[
                [(826, 135), (1005, 45), (1163, 95), (1163, 160),
                 (1040, 190), (900, 155)],
                [(955, 175), (990, 175), (990, 470), (955, 470)],
            ]),
        region(2, (55, 225, 185, 455), 28, [
            (120, 240), (100, 260), (100, 280), (120, 280), (140, 280),
            (160, 280), (120, 300), (140, 300), (100, 300), (160, 260),
            (100, 320), (120, 340), (120, 360), (120, 380), (120, 400),
            (120, 420),
        ], avoid=[(140, 320), (140, 360), (140, 440), (160, 420),
                  (40, 420), (80, 240)], avoid_tolerance=16),
    ],
}


def rect_region(rid, bbox, base_y, tolerance=44, step=20, inset=8,
                avoid_tolerance=16):
    """A fixture-rect-guided region: grid seeds across the inset bbox
    (the fixture hotspot rects are authored to match the plate art) and
    floor swatches sampled just below the fixture's base as avoids. Used
    for the boundary-model coverage pass, where the exposed fixtures are
    known from world.json rather than hand-traced."""
    x0, y0, x1, y1 = bbox
    seeds = [(x, y) for x in range(x0 + inset, x1 - inset + 1, step)
             for y in range(y0 + inset, y1 - inset + 1, step)]
    center_x = (x0 + x1) // 2
    avoid = [(center_x - 44, base_y + 14), (center_x, base_y + 16),
             (center_x + 44, base_y + 14)]
    return region(rid, bbox, tolerance, seeds, avoid=avoid,
                  avoid_tolerance=avoid_tolerance)


# Fixtures whose wrong-overlap band is reachable by the runtime feet box
# (see BOUNDARIES-CLIPPING-RESEARCH): per (room, style) because each
# plate paints the furniture in its own spot — plate-space bbox measured
# by ruler, plate base row, region id. world.json declares the matching
# walkbehind baselines (study 1@198, yard 3@204).
_EXPOSED_FIXTURES = {
    ("study", "legend"): [((352, 296, 452, 516), 512, 1, 48, 16)],
    ("study", "chumrunner"): [((232, 322, 306, 522), 518, 1)],
    ("study", "fantasy"): [((250, 316, 334, 512), 505, 1, 48, 8),
                           ((172, 335, 250, 505), 500, 1, 48, 8)],
    ("study", "pleb-bound"): [((198, 276, 336, 528), 520, 1)],
    ("yard", "legend"): [((54, 298, 190, 556), 544, 3)],
    ("yard", "chumrunner"): [((52, 292, 142, 415), 544, 3)],
    ("yard", "fantasy"): [((48, 294, 210, 556), 544, 3)],
    ("yard", "pleb-bound"): [((36, 298, 116, 396), 544, 3)],
}
for (_room, _style), _entries in _EXPOSED_FIXTURES.items():
    REGIONS.setdefault((_room, _style), [])
    for _entry in _entries:
        _bbox, _base, _rid = _entry[0], _entry[1], _entry[2]
        _tol = _entry[3] if len(_entry) > 3 else 44
        _avoid_tol = _entry[4] if len(_entry) > 4 else 16
        REGIONS[(_room, _style)].append(
            rect_region(_rid, _bbox, _base, tolerance=_tol,
                        avoid_tolerance=_avoid_tol))


def fill_region(pixels, spec):
    """-> set of (x, y) plate pixels belonging to the region."""
    x0, y0, x1, y1 = spec["bbox"]
    tolerance_sq = spec["tolerance"] ** 2
    avoid_sq = spec["avoid_tolerance"] ** 2
    seed_colors = [pixels[seed][:3] for seed in spec["seeds"]]
    avoid_colors = [pixels[point][:3] for point in spec["avoid"]]

    def matches(point):
        r, g, b = pixels[point][:3]
        for ar, ag, ab in avoid_colors:
            if (r - ar) ** 2 + (g - ag) ** 2 + (b - ab) ** 2 <= avoid_sq:
                return False
        for sr, sg, sb in seed_colors:
            if (r - sr) ** 2 + (g - sg) ** 2 + (b - sb) ** 2 \
                    <= tolerance_sq:
                return True
        return False

    filled = set()
    queue = deque()
    for seed in spec["seeds"]:
        sx, sy = seed
        if not (x0 <= sx < x1 and y0 <= sy < y1):
            raise SystemExit(f"seed {seed} outside bbox {spec['bbox']}")
        if seed not in filled and matches(seed):
            filled.add(seed)
            queue.append(seed)
    while queue:
        cx, cy = queue.popleft()
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                       (cx, cy - 1)):
            if not (x0 <= nx < x1 and y0 <= ny < y1):
                continue
            if (nx, ny) in filled or not matches((nx, ny)):
                continue
            filled.add((nx, ny))
            queue.append((nx, ny))

    filled = fill_holes(filled, spec["bbox"])
    for _ in range(spec["smooth"]):
        filled = {(cx, cy) for cx, cy in filled
                  if (cx + 1, cy) in filled and (cx - 1, cy) in filled
                  and (cx, cy + 1) in filled and (cx, cy - 1) in filled}
    for _ in range(spec["smooth"]):
        filled |= {n for cx, cy in filled
                   for n in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                             (cx, cy - 1))
                   if x0 <= n[0] < x1 and y0 <= n[1] < y1}
    filled = drop_islands(filled)
    for _ in range(spec["grow"]):
        edge = set()
        for cx, cy in filled:
            for neighbor in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                             (cx, cy - 1)):
                nx, ny = neighbor
                if x0 <= nx < x1 and y0 <= ny < y1 and \
                        neighbor not in filled:
                    edge.add(neighbor)
        filled |= edge
    if spec["polys"]:
        from PIL import Image as PILImage, ImageDraw
        stencil = PILImage.new("1", (PLATE_W, PLATE_H), 0)
        drawer = ImageDraw.Draw(stencil)
        for poly in spec["polys"]:
            drawer.polygon([tuple(point) for point in poly], fill=1)
        stencil_pixels = stencil.load()
        filled |= {(x, y) for y in range(y0, y1) for x in range(x0, x1)
                   if stencil_pixels[x, y]}
    return fill_holes(filled, spec["bbox"])


def fill_holes(filled, bbox):
    """Add every non-region pocket that cannot reach the bbox border:
    outlines and interior details enclosed by the fill become region."""
    x0, y0, x1, y1 = bbox
    outside = set()
    queue = deque()
    for x in range(x0, x1):
        for y in (y0, y1 - 1):
            if (x, y) not in filled and (x, y) not in outside:
                outside.add((x, y))
                queue.append((x, y))
    for y in range(y0, y1):
        for x in (x0, x1 - 1):
            if (x, y) not in filled and (x, y) not in outside:
                outside.add((x, y))
                queue.append((x, y))
    while queue:
        cx, cy = queue.popleft()
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                       (cx, cy - 1)):
            if not (x0 <= nx < x1 and y0 <= ny < y1):
                continue
            if (nx, ny) in filled or (nx, ny) in outside:
                continue
            outside.add((nx, ny))
            queue.append((nx, ny))
    total = (x1 - x0) * (y1 - y0)
    return filled | {(x, y) for y in range(y0, y1) for x in range(x0, x1)
                     if (x, y) not in filled and (x, y) not in outside} \
        if len(filled) + len(outside) < total else set(filled)


def drop_islands(filled):
    """Remove 4-connected components smaller than MIN_ISLAND pixels."""
    remaining = set(filled)
    kept = set()
    while remaining:
        start = next(iter(remaining))
        component = {start}
        queue = deque([start])
        remaining.discard(start)
        while queue:
            cx, cy = queue.popleft()
            for neighbor in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                             (cx, cy - 1)):
                if neighbor in remaining:
                    remaining.discard(neighbor)
                    component.add(neighbor)
                    queue.append(neighbor)
        if len(component) >= MIN_ISLAND:
            kept |= component
    return kept


def build_mask(room, style):
    plate_path = os.path.join(ROOMS_DIR, style, f"{room}.png")
    plate = Image.open(plate_path).convert("RGB")
    if plate.size != (PLATE_W, PLATE_H):
        raise SystemExit(f"{plate_path}: not {PLATE_W}x{PLATE_H}")
    pixels = plate.load()
    mask = Image.new("L", (PLATE_W, PLATE_H), 0)
    mask_pixels = mask.load()
    counts = {}
    for spec in REGIONS[(room, style)]:
        filled = fill_region(pixels, spec)
        counts[spec["id"]] = counts.get(spec["id"], 0) + len(filled)
        for x, y in filled:
            mask_pixels[x, y] = spec["id"]
    return plate, mask, counts


def review_image(plate, mask, specs):
    dimmed = Image.eval(plate, lambda value: value * 45 // 100)
    out = dimmed.convert("RGB")
    out_pixels = out.load()
    mask_pixels = mask.load()
    plate_pixels = plate.load()
    for y in range(PLATE_H):
        for x in range(PLATE_W):
            value = mask_pixels[x, y]
            if value:
                tint = REVIEW_TINTS.get(value, (255, 255, 0))
                pr, pg, pb = plate_pixels[x, y]
                out_pixels[x, y] = ((pr + tint[0] * 2) // 3,
                                    (pg + tint[1] * 2) // 3,
                                    (pb + tint[2] * 2) // 3)
    for spec in specs:
        x0, y0, x1, y1 = spec["bbox"]
        for x in range(x0, x1):
            out_pixels[x, y0] = (255, 255, 255)
            out_pixels[x, y1 - 1] = (255, 255, 255)
        for y in range(y0, y1):
            out_pixels[x0, y] = (255, 255, 255)
            out_pixels[x1 - 1, y] = (255, 255, 255)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--review", metavar="DIR",
                        help="also write mask-over-plate review PNGs here")
    parser.add_argument("--room", help="only this room")
    parser.add_argument("--style", help="only this style")
    arguments = parser.parse_args()
    if arguments.review:
        os.makedirs(arguments.review, exist_ok=True)
    rooms = sorted({room for room, _ in REGIONS})
    for room in rooms:
        if arguments.room and room != arguments.room:
            continue
        for style in STYLES:
            if arguments.style and style != arguments.style:
                continue
            plate, mask, counts = build_mask(room, style)
            out_path = os.path.join(ROOMS_DIR, style, f"{room}-behind.png")
            mask.save(out_path)
            summary = " ".join(f"id{rid}={count}px"
                               for rid, count in sorted(counts.items()))
            print(f"{style}/{room}-behind.png  {summary}")
            if arguments.review:
                review = review_image(plate, mask, REGIONS[(room, style)])
                review.save(os.path.join(arguments.review,
                                         f"{room}-{style}.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
