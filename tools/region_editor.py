#!/usr/bin/env python3
"""Edit a room's walkable space or walk-behind mask with kilix-mask.

What this replaces is the *painting*: the viewport, the brush, the wand,
the undo stack, the cursor, the damage-rect presentation.  All of that is
now a published module with its own tests, instead of two thousand lines
living next to a game that is not about painting.

What stays here is everything that is about this game - world.json, the
walk-behind file format the engine reads, the obstacle cap, and running
the validator afterwards so a spawn painted into a wall is caught at once.

The conversion in both directions is exact, and --selftest proves it
against every real room and every real mask rather than against a
synthetic one: convert out, convert back, and nothing has moved.

  tools/region_editor.py --room bedroom --style chumrunner
  tools/region_editor.py --room yard --behind
  tools/region_editor.py --selftest
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import land_mask
import land_regions as lr


def run_editor(mask_path, plate, cap=None):
    """Hand the mask to kilix-mask and wait.  Returns False when the
    editor could not run at all, which is different from the operator
    quitting without saving."""
    if not os.path.exists(land_mask.COMMAND):
        raise land_mask.MaskLibraryMissing(
            f"{land_mask.COMMAND} is missing; run:\n"
            f"  git submodule update --init --recursive\n"
            f"  make -C third_party/kilix-mask")
    command = [land_mask.COMMAND, "--image", plate]
    if cap:
        command += ["--cap", str(cap)]
    command.append(mask_path)
    return subprocess.call(command) == 0


def validate():
    """The reason a save is worth trusting: a door or item spawn painted
    into a wall is caught now rather than when somebody walks into it."""
    script = os.path.join(lr.REPO, "tools", "validate_world.py")
    if not os.path.exists(script):
        return True
    return subprocess.call([sys.executable, script]) == 0


def edit_walk(room_id, style, dry_run=False):
    world = lr.load_world()
    room = lr.room_by_id(world, room_id)
    plate = lr.plate_path(style, room["plate"])
    if not os.path.exists(plate):
        print(f"no plate at {plate}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, f"{room_id}-walk.mask.png")
        with lr.walk_mask_from_room(room) as mask:
            mask.save(path)
        if not dry_run and not run_editor(path, plate, lr.MAX_OBSTACLES):
            return 1
        with lr.Mask.load(path) as mask:
            result, error = lr.room_from_walk_mask(mask)

    if error:
        print(f"not saved: {error}", file=sys.stderr)
        return 1
    if result["walk"] == room["walk"] and \
            result["obstacles"] == room["obstacles"]:
        print("unchanged")
        return 0
    if dry_run:
        print(f"{room_id}: would write walk {result['walk']} and "
              f"{len(result['obstacles'])} obstacles")
        return 0
    room["walk"] = result["walk"]
    room["obstacles"] = result["obstacles"]
    lr.save_world(world)
    print(f"{room_id}: walk {result['walk']}, "
          f"{len(result['obstacles'])} obstacles "
          f"(cap {lr.MAX_OBSTACLES})")
    return 0 if validate() else 1


def edit_behind(room_id, style, dry_run=False):
    world = lr.load_world()
    room = lr.room_by_id(world, room_id)
    plate_name = room["plate"]
    plate = lr.plate_path(style, plate_name)
    if not os.path.exists(plate):
        print(f"no plate at {plate}", file=sys.stderr)
        return 1

    original_baselines = {int(e["id"]): int(e["baseline"])
                          for e in room["walkbehinds"]}

    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, f"{room_id}-behind.mask.png")
        with lr.behind_mask_from_disk(style, plate_name,
                                      room["walkbehinds"]) as mask:
            mask.save(path)
        if not dry_run and not run_editor(path, plate):
            return 1
        with lr.Mask.load(path) as mask:
            values = mask.expand()
            baselines = {}
            for region in range(1, lr.MAX_REGIONS + 1):
                text = mask.attr(region, "baseline")
                if text is None:
                    continue
                plate_y = int(text)
                declared = original_baselines.get(region)
                if declared is not None and \
                        lr.logical_y_to_plate(declared) == plate_y:
                    # Untouched: keep the logical value world.json already
                    # holds rather than round-tripping it through plate
                    # pixels, which would move baselines nobody edited.
                    baselines[region] = declared
                else:
                    baselines[region] = lr.plate_y_to_logical(plate_y)

    if dry_run:
        # Converting out and back is the whole point of a dry run; writing
        # is what it exists to avoid.
        print(f"{room_id} ({style}): {len(lr.behind_ids_present(values))} "
              f"region(s), mask unchanged on disk")
        return 0
    lr.write_behind_bytes(style, plate_name, values)

    # Ids and baselines are shared across styles by contract, so the
    # declaration list is the union over every style's mask - editing one
    # style must not drop a region another style still uses.
    per_style = [values if other == style
                 else lr.read_behind_bytes(other, plate_name)
                 for other in lr.STYLES]
    existing = {int(e["id"]): int(e["baseline"]) for e in room["walkbehinds"]}
    existing.update(baselines)
    room["walkbehinds"] = lr.walkbehinds_from_masks(per_style, existing)
    lr.save_world(world)
    print(f"{room_id} ({style}): {room['walkbehinds']}")
    return 0 if validate() else 1


def selftest():
    """Convert every room and every mask out and back, and require that
    nothing moved.

    Against the real assets on purpose.  A synthetic room would prove the
    conversion is self-consistent; only the real ones prove it agrees
    with what is already on disk and in the engine.
    """
    failures = 0
    world = lr.load_world()

    for room in world["rooms"]:
        with lr.walk_mask_from_room(room) as mask:
            result, error = lr.room_from_walk_mask(mask)
        if error:
            print(f"not ok walk {room['id']}: {error}")
            failures += 1
            continue
        if result["walk"] != room["walk"]:
            print(f"not ok walk {room['id']}: bounds moved")
            failures += 1
        elif result["obstacles"] != room["obstacles"]:
            print(f"not ok walk {room['id']}: "
                  f"{len(result['obstacles'])} obstacles, "
                  f"was {len(room['obstacles'])}")
            failures += 1
        else:
            print(f"ok walk {room['id']} "
                  f"({len(result['obstacles'])} obstacles)")
        if len(result["obstacles"]) > lr.MAX_OBSTACLES:
            print(f"not ok walk {room['id']}: over the obstacle cap")
            failures += 1

    try:
        import PIL  # noqa: F401
    except ImportError:
        print("skip behind masks (no PIL)")
        return 1 if failures else 0

    for style in lr.STYLES:
        for room in world["rooms"]:
            plate = room["plate"]
            original = lr.read_behind_bytes(style, plate)
            with lr.behind_mask_from_disk(style, plate,
                                          room["walkbehinds"]) as mask:
                back = mask.expand()
                carried = {r: mask.attr(r, "baseline")
                           for r in range(1, lr.MAX_REGIONS + 1)
                           if mask.attr(r, "baseline") is not None}
            if back != original:
                print(f"not ok behind {style}/{plate}: mask bytes changed")
                failures += 1
                continue
            # Compared in plate pixels, the space the attribute is in;
            # world.json states them in logical units.
            declared = {int(e["id"]): str(lr.logical_y_to_plate(
                            int(e["baseline"])))
                        for e in room["walkbehinds"]}
            if any(carried.get(k) != v for k, v in declared.items()):
                print(f"not ok behind {style}/{plate}: baselines changed "
                      f"({carried} vs {declared})")
                failures += 1
                continue
            # And back again without drift, which is what the editor does
            # on save for a baseline nobody moved.
            returned = {k: lr.plate_y_to_logical(int(v))
                        for k, v in carried.items()}
            original = {int(e["id"]): int(e["baseline"])
                        for e in room["walkbehinds"]}
            if returned != original:
                print(f"not ok behind {style}/{plate}: baseline round trip "
                      f"{returned} vs {original}")
                failures += 1
                continue
            print(f"ok behind {style}/{plate}")

    print(f"{'FAILED' if failures else 'selftest passed'}"
          f"{f' ({failures})' if failures else ''}")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--room", default="bedroom")
    parser.add_argument("--style", default="chumrunner", choices=lr.STYLES)
    parser.add_argument("--behind", action="store_true",
                        help="edit the walk-behind mask instead of the "
                             "walkable space")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--dry-run", action="store_true",
                        help="convert out and back without opening the "
                             "editor and without writing anything")
    options = parser.parse_args()

    if options.selftest:
        return selftest()
    try:
        if options.behind:
            return edit_behind(options.room, options.style, options.dry_run)
        return edit_walk(options.room, options.style, options.dry_run)
    except land_mask.MaskLibraryMissing as problem:
        print(problem, file=sys.stderr)
        return 1
    except KeyError as missing:
        print(f"no room {missing}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
