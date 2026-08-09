#!/usr/bin/env python3
"""Between this game's formats and kilix-mask's.

Both of this game's region formats stay exactly as they are, because both
are contracts with something that is not the editor:

  * world.json holds walkable space as one bounding rect plus obstacle
    rects, in logical units, and the engine's point-in-rect test is what
    "walkable" means to the player.
  * <style>/<plate>-behind.png is 8-bit greyscale at plate size with the
    region id in each pixel.  The engine loads it through a general PNG
    decoder and *takes the red channel*, so a palette image - which is
    what kilix-mask writes - would come back as palette colours instead
    of ids.  Greyscale is not incidental here; it is load-bearing.

So nothing on disk changes.  What changes is that the painting in between
is done by kilix-mask instead of by two thousand lines of bespoke editor.

The round trip is the whole safety argument, and it is checked against
every real room and every real mask by --selftest: convert out, convert
back, and the world.json rects and the mask bytes must be what they were.
"""

import json
import os

import land_mask
from land_mask import (COLS, LOGICAL_CELL, LOGICAL_H, LOGICAL_W, Mask,
                       PLATE_CELL, PLATE_H, PLATE_W, ROWS)

MAX_OBSTACLES = 64
MAX_REGIONS = 15
WALKABLE = 1
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def assets_root():
    return os.environ.get("KILIX_LAND_DESKTOP_ASSETS") or REPO


def world_path():
    return os.path.join(assets_root(), "assets/world/world.json")


def behind_path(style, plate):
    return os.path.join(assets_root(), "assets/graphics/rooms", style,
                        plate + "-behind.png")


def plate_path(style, plate):
    return os.path.join(assets_root(), "assets/graphics/rooms", style,
                        plate + ".png")


def logical_to_plate(value):
    """A logical coordinate on the 6-unit grid, in plate pixels."""
    scaled = value * PLATE_CELL
    if scaled % LOGICAL_CELL:
        raise ValueError(f"{value} is not on the {LOGICAL_CELL}-unit grid")
    return scaled // LOGICAL_CELL


def plate_to_logical(value):
    scaled = value * LOGICAL_CELL
    if scaled % PLATE_CELL:
        raise ValueError(f"{value} is not on a cell boundary")
    return scaled // PLATE_CELL


# A baseline is a free y, not a grid coordinate, so it converts by the
# plain plate/logical ratio and can land between logical units.  These
# round rather than refuse - refusing would make a baseline set by
# pointing at the picture, which is the only sensible way to set one,
# fail two times in three.
def logical_y_to_plate(y):
    return int(round(y * PLATE_H / LOGICAL_H))


def plate_y_to_logical(y):
    return int(round(y * LOGICAL_H / PLATE_H))


# ----------------------------- the walk layer -----------------------------

def walk_mask_from_room(room):
    """world.json's rects as a mask over the plate.

    The cell test is written out rather than expressed as
    "bounds minus holes", even though kmask_apply() would do that in one
    call, because *this* is the definition: desk.c decides whether the
    player may stand somewhere by testing a point against the rects, and
    the editor has to agree with the engine rather than with a convenient
    identity.  It also means a hand-edited world.json whose rects are off
    the grid converts to what the engine would actually do with it.
    """
    walk = room["walk"]
    obstacles = room.get("obstacles", [])
    mask = Mask.create(PLATE_W, PLATE_H, PLATE_CELL)
    mask.set_name(WALKABLE, "walkable")
    mask.set_color(WALKABLE, 0x60FF80)

    for cy in range(ROWS):
        y = cy * LOGICAL_CELL + LOGICAL_CELL / 2
        if not walk["y"] <= y <= walk["y"] + walk["h"]:
            continue
        for cx in range(COLS):
            x = cx * LOGICAL_CELL + LOGICAL_CELL / 2
            if not walk["x"] <= x <= walk["x"] + walk["w"]:
                continue
            if any(o["x"] <= x <= o["x"] + o["w"] and
                   o["y"] <= y <= o["y"] + o["h"] for o in obstacles):
                continue
            mask.set(cx, cy, WALKABLE)
    return mask


def room_from_walk_mask(mask):
    """-> ({"walk": rect, "obstacles": [rect]}, None) or (None, reason).

    The obstacle cap is reported, not enforced by refusing to compute:
    the caller decides what to do about a room that is over, and it can
    only decide if it is told the number.
    """
    bounds, holes = mask.decompose(WALKABLE)
    if bounds is None:
        return None, "no walkable cells painted"
    if len(holes) > MAX_OBSTACLES:
        return None, (f"{len(holes)} obstacle rects exceed the cap of "
                      f"{MAX_OBSTACLES}; simplify the blocked shape")
    def logical(rect):
        return {"x": plate_to_logical(rect.x), "y": plate_to_logical(rect.y),
                "w": plate_to_logical(rect.w), "h": plate_to_logical(rect.h)}

    return {"walk": logical(bounds),
            "obstacles": [logical(hole) for hole in holes]}, None


# ---------------------------- the behind layer ----------------------------

def read_behind_bytes(style, plate):
    """Plate-sized region ids, or all zero when the file is missing or the
    wrong shape - the same rule the engine applies."""
    from PIL import Image
    try:
        with Image.open(behind_path(style, plate)) as image:
            if image.size != (PLATE_W, PLATE_H) or image.mode != "L":
                return bytes(PLATE_W * PLATE_H)
            data = image.tobytes()
    except OSError:
        return bytes(PLATE_W * PLATE_H)
    table = bytes(v if v <= MAX_REGIONS else 0 for v in range(256))
    return data.translate(table)


def write_behind_bytes(style, plate, values):
    """The 8-bit greyscale PNG the engine decodes; -> the path written."""
    from PIL import Image
    path = behind_path(style, plate)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.frombytes("L", (PLATE_W, PLATE_H), bytes(values)).save(path)
    return path


def behind_mask_from_disk(style, plate, walkbehinds=()):
    """The style's mask as a per-pixel kilix-mask, baselines attached.

    Baselines live in world.json rather than in the image, and are shared
    across styles by contract, so they are carried in as region
    attributes: that keeps the number beside the shape it describes while
    it is being edited, without inventing a second place to store it.
    """
    mask = Mask.create(PLATE_W, PLATE_H, 1)
    mask.import_bytes(read_behind_bytes(style, plate))
    # In plate pixels, because that is the space the mask is painted in
    # and the editor draws the marker at whatever the attribute says.
    # world.json keeps them in logical units.
    for entry in walkbehinds:
        region = int(entry["id"])
        if 1 <= region <= MAX_REGIONS:
            mask.set_attr(region, "baseline",
                          logical_y_to_plate(int(entry["baseline"])))
    for region in range(1, MAX_REGIONS + 1):
        mask.set_name(region, f"behind{region}")
    return mask


def behind_ids_present(values):
    return {v for v in set(values) if 1 <= v <= MAX_REGIONS}


def walkbehinds_from_masks(per_style_values, baselines):
    """The room's declarations, following the existing rules exactly.

    Ids appearing in no style's mask are dropped; an id that appears but
    has no baseline is declared inert at 0, which is AGS's unset-baseline
    behaviour and what this game already does.
    """
    present = set()
    for values in per_style_values:
        present |= behind_ids_present(values)
    return [{"id": region, "baseline": int(baselines.get(region, 0))}
            for region in sorted(present)]


# --------------------------------- world ---------------------------------

def load_world():
    with open(world_path(), encoding="utf-8") as handle:
        return json.load(handle)


def save_world(world):
    """Rewritten whole, with the file's existing shape: two-space indent
    and a trailing newline, so a save that changes one rect produces a
    one-rect diff rather than reformatting the file."""
    with open(world_path(), "w", encoding="utf-8") as handle:
        json.dump(world, handle, indent=2)
        handle.write("\n")


def room_by_id(world, room_id):
    for room in world["rooms"]:
        if room["id"] == room_id:
            return room
    raise KeyError(room_id)
