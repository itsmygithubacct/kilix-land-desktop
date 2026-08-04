#!/usr/bin/env python3
"""Validate assets/world/world.json against the desk world contract.

Mirrors the C-side checks in src/rooms.c so `make test` fails fast with a
readable report before the binary ever parses the file. Keep TARGETS and the
capacity constants in lockstep with src/kilix_land_desktop.h.
"""

import json
import os
import sys

LOGICAL_WIDTH = 480
LOGICAL_HEIGHT = 270
PLATE_WIDTH = 1280
PLATE_HEIGHT = 720
MAX_ROOMS = 16
MAX_OBJECTS = 12
MAX_DOORS = 4
MAX_OBSTACLES = 64
MAX_NPCS = 3
MAX_WALKBEHINDS = 15
MAX_ITEM_SPAWNS = 8
MAX_ITEM_STACK = 99
NPC_SPAWN_EXCLUSION = 24.0
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
# Keys parse_room() in src/rooms.c accepts; anything else is rejected there,
# so reject it here too (e.g. a stale pre-walk-behind "occluders" array).
ROOM_KEYS = {
    "id", "name", "plate", "outdoor", "walk", "obstacles", "doors",
    "objects", "npcs", "walkbehinds", "item_spawns",
}
ID_CAPACITY = 24
LABEL_CAPACITY = 40
PROMPT_CAPACITY = 48

# Names accepted by desk_target_from_string() in src/launcher.c.
TARGETS = {
    "terminal", "coding-agents", "files", "manuals", "models", "games",
    "music", "voice", "trash", "mailbox", "maintenance",
    "wardrobe", "bed", "status-board", "gate-locked", "walk-editor",
    "kettle", "browser",
}


def fail(message):
    print(f"world: {message}", file=sys.stderr)
    raise SystemExit(1)


def check_rect(rect, label, container=None):
    for key in ("x", "y", "w", "h"):
        if not isinstance(rect.get(key), (int, float)):
            fail(f"{label}: missing numeric '{key}'")
    if rect["w"] <= 0 or rect["h"] <= 0:
        fail(f"{label}: non-positive size")
    if rect["x"] < 0 or rect["y"] < 0:
        fail(f"{label}: negative origin")
    if rect["x"] + rect["w"] > LOGICAL_WIDTH:
        fail(f"{label}: exceeds logical width {LOGICAL_WIDTH}")
    if rect["y"] + rect["h"] > LOGICAL_HEIGHT:
        fail(f"{label}: exceeds logical height {LOGICAL_HEIGHT}")
    if container is not None:
        if (rect["x"] < container["x"] - 1e-6
                or rect["y"] < container["y"] - 1e-6
                or rect["x"] + rect["w"] > container["x"] + container["w"] + 1e-6
                or rect["y"] + rect["h"] > container["y"] + container["h"] + 1e-6):
            fail(f"{label}: outside its containing rect")


def check_string(value, label, capacity):
    if not isinstance(value, str) or not value:
        fail(f"{label}: missing or empty")
    if len(value.encode()) >= capacity:
        fail(f"{label}: longer than capacity {capacity - 1}")


def point_in_rect(x, y, rect):
    return (rect["x"] <= x <= rect["x"] + rect["w"]
            and rect["y"] <= y <= rect["y"] + rect["h"])


# Set by main() from the world.json location: <root>/assets/graphics/rooms.
ROOMS_DIR = None

SPAWN_ID_RE = None  # initialized lazily to keep the import list short
ITEM_ID_RE = None


def load_item_catalog(world_path):
    """Sibling items.json as {ids: {id: max_stack}, receivers: set}, or
    None when the catalog does not exist yet (a schema-1 world)."""
    import re
    global SPAWN_ID_RE, ITEM_ID_RE
    SPAWN_ID_RE = re.compile(r"^[a-z0-9-]+$")
    ITEM_ID_RE = re.compile(
        r"^[a-z0-9_-]+:[a-z0-9_-]+(?:[/.][a-z0-9_-]+)*$")
    path = os.path.join(os.path.dirname(os.path.abspath(world_path)),
                        "items.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as handle:
        catalog = json.load(handle)
    ids = {item["id"]: item["max_stack"]
           for item in catalog.get("definitions", [])}
    receivers = {receiver["id"]
                 for receiver in catalog.get("receivers", [])}
    return {"ids": ids, "receivers": receivers}


def check_item_spawn(room, index, spawn, seen_spawn_ids, item_catalog):
    rid = room["id"]
    label = f"{rid}.item_spawns[{index}]"
    unknown = set(spawn) - {"id", "item", "quantity", "x", "y"}
    if unknown:
        fail(f"{label}: unknown keys {', '.join(sorted(unknown))}")
    check_string(spawn.get("id"), f"{label}.id", ID_CAPACITY)
    if not SPAWN_ID_RE.match(spawn["id"]):
        fail(f"{label}: invalid spawn id '{spawn['id']}'")
    if spawn["id"] in seen_spawn_ids:
        fail(f"duplicate spawn id '{spawn['id']}'")
    seen_spawn_ids.add(spawn["id"])
    check_string(spawn.get("item"), f"{label}.item", 48)
    if not ITEM_ID_RE.match(spawn["item"]) or ".." in spawn["item"]:
        fail(f"{label}: invalid item id '{spawn['item']}'")
    quantity = spawn.get("quantity")
    if not isinstance(quantity, int) or isinstance(quantity, bool) \
            or not 1 <= quantity <= MAX_ITEM_STACK:
        fail(f"{label}: quantity out of range")
    x, y = spawn.get("x"), spawn.get("y")
    if not isinstance(x, (int, float)) or not isinstance(y, (int, float)):
        fail(f"{label}: missing position")
    if not point_in_rect(x, y, room["walk"]):
        fail(f"{label}: point outside walk rect")
    for j, rect in enumerate(room.get("obstacles", [])):
        if point_in_rect(x, y, rect):
            fail(f"{label}: point inside obstacle [{j}]")
    for j, door in enumerate(room.get("doors", [])):
        if point_in_rect(x, y, door.get("rect", {})):
            fail(f"{label}: point inside door [{j}]")
    for obj in room.get("objects", []):
        if point_in_rect(x, y, obj.get("rect", {})):
            fail(f"{label}: point inside object '{obj.get('id')}'")
    for npc in room.get("npcs", []):
        if ((x - npc["x"]) ** 2 + (y - npc["y"]) ** 2
                < NPC_SPAWN_EXCLUSION ** 2):
            fail(f"{label}: too close to npc actor {npc['actor']}")
    if item_catalog is not None:
        if spawn["item"] not in item_catalog["ids"]:
            fail(f"{label}: unknown item '{spawn['item']}'")
        if quantity > item_catalog["ids"][spawn["item"]]:
            fail(f"{label}: quantity {quantity} exceeds "
                 f"'{spawn['item']}' max stack")


def check_behind_masks(room, declared_ids):
    """Validate every style's <plate>-behind.png that exists: exactly
    plate-sized, 8-bit grayscale (the only mask form the engine's PNG
    decoder accepts), and every pixel value in {0} | declared ids."""
    if ROOMS_DIR is None:
        return
    rid = room["id"]
    allowed = {0} | declared_ids
    for style in STYLES:
        path = os.path.join(ROOMS_DIR, style,
                            f"{room['plate']}-behind.png")
        if not os.path.exists(path):
            continue
        from PIL import Image
        label = f"{rid}: {style}/{room['plate']}-behind.png"
        with Image.open(path) as image:
            if image.size != (PLATE_WIDTH, PLATE_HEIGHT):
                fail(f"{label}: size {image.size[0]}x{image.size[1]} is "
                     f"not plate-sized {PLATE_WIDTH}x{PLATE_HEIGHT}")
            if image.mode != "L":
                fail(f"{label}: mode '{image.mode}' is not 8-bit "
                     "grayscale 'L' (the engine decodes only grayscale "
                     "masks)")
            values = set(image.getdata())
        stray = values - allowed
        if stray:
            fail(f"{label}: mask values {sorted(stray)} are not in "
                 f"{{0}} | declared ids {sorted(declared_ids)}")


def check_c_parser_subset(text):
    """Reject JSON constructs src/rooms.c's parser does not accept, so a file
    that passes this validator can never be refused by the binary."""
    in_string = False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == "\\":
                if i + 1 >= len(text) or text[i + 1] not in '"\\/nt':
                    fail(f"offset {i}: escape \\{text[i + 1:i + 2]} is not "
                         "supported by the C parser")
                i += 2
                continue
            if ch == '"':
                in_string = False
        elif ch == '"':
            in_string = True
        elif ch in "eE" and i > 0 and (text[i - 1].isdigit()
                                       or text[i - 1] == "."):
            fail(f"offset {i}: exponent notation is not supported by "
                 "the C parser")
        i += 1


def main():
    global ROOMS_DIR
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/world/world.json"
    root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(path))))
    ROOMS_DIR = os.path.join(root, "assets", "graphics", "rooms")
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    check_c_parser_subset(text)
    world = json.loads(text)

    # Schema 1 = no item spawns or receivers; schema 2 adds them.
    if world.get("world") not in (1, 2):
        fail("unsupported schema version (want world: 1 or 2)")
    item_catalog = load_item_catalog(path)
    rooms = world.get("rooms")
    if not isinstance(rooms, list) or not rooms:
        fail("no rooms")
    if len(rooms) > MAX_ROOMS:
        fail(f"more than {MAX_ROOMS} rooms")

    ids = {}
    for index, room in enumerate(rooms):
        check_string(room.get("id"), f"room[{index}].id", ID_CAPACITY)
        if room["id"] in ids:
            fail(f"duplicate room id '{room['id']}'")
        ids[room["id"]] = index

    start = world.get("start")
    if start not in ids:
        fail(f"start room '{start}' does not exist")

    reachable = set()
    seen_spawn_ids = set()
    for room in rooms:
        rid = room["id"]
        unknown = set(room) - ROOM_KEYS
        if unknown:
            fail(f"{rid}: unknown keys {', '.join(sorted(unknown))}")
        check_string(room.get("name"), f"{rid}.name", LABEL_CAPACITY)
        check_string(room.get("plate"), f"{rid}.plate", LABEL_CAPACITY)
        walk = room.get("walk")
        if not isinstance(walk, dict):
            fail(f"{rid}: missing walk rect")
        check_rect(walk, f"{rid}.walk")

        obstacles = room.get("obstacles", [])
        if len(obstacles) > MAX_OBSTACLES:
            fail(f"{rid}: more than {MAX_OBSTACLES} obstacles")
        for i, rect in enumerate(obstacles):
            check_rect(rect, f"{rid}.obstacles[{i}]")

        doors = room.get("doors", [])
        if len(doors) > MAX_DOORS:
            fail(f"{rid}: more than {MAX_DOORS} doors")
        for i, door in enumerate(doors):
            check_rect(door.get("rect", {}), f"{rid}.doors[{i}].rect")
            to = door.get("to")
            if to not in ids:
                fail(f"{rid}.doors[{i}]: unknown destination '{to}'")
            if to == rid:
                fail(f"{rid}.doors[{i}]: door to its own room")
            spawn = door.get("spawn", {})
            sx, sy = spawn.get("x"), spawn.get("y")
            if not isinstance(sx, (int, float)) or not isinstance(sy, (int, float)):
                fail(f"{rid}.doors[{i}]: missing spawn point")
            dest = rooms[ids[to]]
            if not point_in_rect(sx, sy, dest["walk"]):
                fail(f"{rid}.doors[{i}]: spawn ({sx},{sy}) outside "
                     f"'{to}' walk rect")
            # Movement only ever accepts pre-validated feet boxes, so a
            # teleport target whose box overlaps solids strands the
            # player.
            if not feet_box_clear(dest, sx, sy):
                fail(f"{rid}.doors[{i}]: spawn ({sx},{sy}) feet box "
                     f"blocked in '{to}'")
            for j, rect in enumerate(dest.get("obstacles", [])):
                if point_in_rect(sx, sy, rect):
                    fail(f"{rid}.doors[{i}]: spawn inside "
                         f"'{to}' obstacle [{j}]")
            # Door triggers fire on position alone: a spawn inside any of the
            # destination's door rects would teleport an idle player.
            for j, dest_door in enumerate(dest.get("doors", [])):
                if point_in_rect(sx, sy, dest_door.get("rect", {})):
                    fail(f"{rid}.doors[{i}]: spawn inside "
                         f"'{to}' door [{j}]")
            reachable.add(to)

        objects = room.get("objects", [])
        if len(objects) > MAX_OBJECTS:
            fail(f"{rid}: more than {MAX_OBJECTS} objects")
        seen_objects = set()
        for i, obj in enumerate(objects):
            check_string(obj.get("id"), f"{rid}.objects[{i}].id", ID_CAPACITY)
            if obj["id"] in seen_objects:
                fail(f"{rid}: duplicate object id '{obj['id']}'")
            seen_objects.add(obj["id"])
            check_string(obj.get("prompt"), f"{rid}.objects[{i}].prompt",
                         PROMPT_CAPACITY)
            check_rect(obj.get("rect", {}), f"{rid}.objects[{i}].rect")
            target = obj.get("target")
            if target not in TARGETS:
                fail(f"{rid}.objects[{i}]: unknown target '{target}'")
            if "receiver" in obj:
                check_string(obj["receiver"], f"{rid}.objects[{i}].receiver",
                             ID_CAPACITY)
                if item_catalog is not None \
                        and obj["receiver"] not in item_catalog["receivers"]:
                    fail(f"{rid}.{obj['id']}: unknown receiver rule "
                         f"'{obj['receiver']}'")

        npcs = room.get("npcs", [])
        if len(npcs) > MAX_NPCS:
            fail(f"{rid}: more than {MAX_NPCS} npcs")
        seen_actors = set()
        for i, npc in enumerate(npcs):
            actor = npc.get("actor")
            if actor not in (1, 2, 3):
                fail(f"{rid}.npcs[{i}]: actor must be 1..3")
            if actor in seen_actors:
                fail(f"{rid}: duplicate npc actor {actor}")
            seen_actors.add(actor)
            x, y = npc.get("x"), npc.get("y")
            if not isinstance(x, (int, float)) or not isinstance(y, (int, float)):
                fail(f"{rid}.npcs[{i}]: missing position")
            if not (0 <= x <= LOGICAL_WIDTH and 0 <= y <= LOGICAL_HEIGHT):
                fail(f"{rid}.npcs[{i}]: position off the logical canvas")

        walkbehinds = room.get("walkbehinds", [])
        if len(walkbehinds) > MAX_WALKBEHINDS:
            fail(f"{rid}: more than {MAX_WALKBEHINDS} walkbehinds")
        seen_walkbehind_ids = set()
        for i, walkbehind in enumerate(walkbehinds):
            unknown = set(walkbehind) - {"id", "baseline"}
            if unknown:
                fail(f"{rid}.walkbehinds[{i}]: unknown keys "
                     f"{', '.join(sorted(unknown))}")
            wid = walkbehind.get("id")
            if not isinstance(wid, int) or isinstance(wid, bool) \
                    or not 1 <= wid <= MAX_WALKBEHINDS:
                fail(f"{rid}.walkbehinds[{i}]: id must be an integer "
                     f"1..{MAX_WALKBEHINDS}")
            if wid in seen_walkbehind_ids:
                fail(f"{rid}: duplicate walkbehind id {wid}")
            seen_walkbehind_ids.add(wid)
            baseline = walkbehind.get("baseline")
            if not isinstance(baseline, (int, float)) \
                    or isinstance(baseline, bool):
                fail(f"{rid}.walkbehinds[{i}]: baseline must be a number")
            if not 0 <= baseline <= LOGICAL_HEIGHT:
                fail(f"{rid}.walkbehinds[{i}]: baseline {baseline} off "
                     "the logical canvas")
        check_behind_masks(room, seen_walkbehind_ids)

        spawns = room.get("item_spawns", [])
        if len(spawns) > MAX_ITEM_SPAWNS:
            fail(f"{rid}: more than {MAX_ITEM_SPAWNS} item spawns")
        for i, spawn in enumerate(spawns):
            check_item_spawn(room, i, spawn, seen_spawn_ids, item_catalog)

    unreachable = [room["id"] for room in rooms
                   if room["id"] != start and room["id"] not in reachable]
    if unreachable:
        fail(f"rooms unreachable by any door: {', '.join(unreachable)}")

    for room in rooms:
        check_connectivity(room, rooms, ids, start)

    print(f"world: OK ({len(rooms)} rooms, start '{start}')")


CELL = 6
GRID_COLS = LOGICAL_WIDTH // CELL
GRID_ROWS = LOGICAL_HEIGHT // CELL

# The runtime collision volume (src/kilix_land_desktop.h): a 16x8 feet
# box anchored at the position, symmetric on the spine. Connectivity must
# be proven for the box, not the point — a lane the point threads but the
# box cannot is a sealed door in play.
FEET_HALF_WIDTH = 8.0
FEET_HEIGHT = 8.0


def feet_box_clear(room, x, y):
    walk = room["walk"]
    if (x - FEET_HALF_WIDTH < walk["x"]
            or x + FEET_HALF_WIDTH > walk["x"] + walk["w"]
            or y - FEET_HEIGHT < walk["y"]
            or y > walk["y"] + walk["h"]):
        return False
    for rect in room.get("obstacles", []):
        if (x - FEET_HALF_WIDTH < rect["x"] + rect["w"]
                and x + FEET_HALF_WIDTH > rect["x"]
                and y - FEET_HEIGHT < rect["y"] + rect["h"]
                and y > rect["y"]):
            return False
    for npc in room.get("npcs", []):
        if (x - FEET_HALF_WIDTH < npc["x"] + FEET_HALF_WIDTH
                and x + FEET_HALF_WIDTH > npc["x"] - FEET_HALF_WIDTH
                and y - FEET_HEIGHT < npc["y"]
                and y > npc["y"] - FEET_HEIGHT):
            return False
    return True


def walkable_cells(room):
    """Cell centers where the runtime feet box fits — the same solidity
    desk.c enforces, including solid housemates."""
    cells = set()
    for cy in range(GRID_ROWS):
        for cx in range(GRID_COLS):
            x, y = cx * CELL + CELL / 2, cy * CELL + CELL / 2
            if feet_box_clear(room, x, y):
                cells.add((cx, cy))
    return cells


def check_connectivity(room, rooms, ids, start):
    """Every door of a room must be reachable by walking from where the
    player can appear in it (incoming door spawns; for the start room also
    the wizard drop near the walk center) — a painted wall that seals a
    door is a broken world even though every rect is individually valid."""
    rid = room["id"]
    cells = walkable_cells(room)
    if not cells:
        fail(f"{rid}: no walkable cells at all")
    seeds = set()
    for other in rooms:
        for door in other.get("doors", []):
            if door.get("to") == rid:
                spawn = door["spawn"]
                seeds.add((int(spawn["x"] // CELL),
                           int(spawn["y"] // CELL)))
    if rid == start:
        walk = room["walk"]
        center = (walk["x"] + walk["w"] / 2, walk["y"] + walk["h"] / 2)
        seeds.add(min(cells, key=lambda c: (
            (c[0] * CELL + 3 - center[0]) ** 2 +
            (c[1] * CELL + 3 - center[1]) ** 2)))
    seeds &= cells
    if not seeds:
        fail(f"{rid}: no spawn lands on a walkable cell")
    frontier = list(seeds)
    reached = set(seeds)
    while frontier:
        cx, cy = frontier.pop()
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                       (cx, cy - 1)):
            if (nx, ny) in cells and (nx, ny) not in reached:
                reached.add((nx, ny))
                frontier.append((nx, ny))
    walk = room["walk"]
    legal_x0 = walk["x"] + FEET_HALF_WIDTH
    legal_x1 = walk["x"] + walk["w"] - FEET_HALF_WIDTH
    legal_y0 = walk["y"] + FEET_HEIGHT
    legal_y1 = walk["y"] + walk["h"]
    for i, door in enumerate(room.get("doors", [])):
        rect = door["rect"]
        # Door triggers fire on the position point, whose legal band is
        # the walk rect inset by the feet box; a top-edge door is reached
        # exactly at that band's boundary row, which cell centers miss.
        dx0 = max(rect["x"], legal_x0)
        dx1 = min(rect["x"] + rect["w"], legal_x1)
        dy0 = max(rect["y"], legal_y0)
        dy1 = min(rect["y"] + rect["h"], legal_y1)
        reachable = dx0 <= dx1 and dy0 <= dy1 and any(
            dx0 <= (cx + 1) * CELL and dx1 >= cx * CELL
            and dy0 <= (cy + 1) * CELL and dy1 >= cy * CELL
            for cx, cy in reached)
        if not reachable:
            fail(f"{rid}.doors[{i}] (to '{door['to']}'): unreachable — "
                 "walkable space does not connect the spawns to this door")


if __name__ == "__main__":
    main()
