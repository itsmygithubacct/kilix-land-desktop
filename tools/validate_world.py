#!/usr/bin/env python3
"""Validate assets/world/world.json against the desk world contract.

Mirrors the C-side checks in src/rooms.c so `make test` fails fast with a
readable report before the binary ever parses the file. Keep TARGETS and the
capacity constants in lockstep with src/kilix_land_desktop.h.
"""

import json
import sys

LOGICAL_WIDTH = 480
LOGICAL_HEIGHT = 270
MAX_ROOMS = 16
MAX_OBJECTS = 12
MAX_DOORS = 4
MAX_OBSTACLES = 24
MAX_NPCS = 3
ID_CAPACITY = 24
LABEL_CAPACITY = 40
PROMPT_CAPACITY = 48

# Names accepted by desk_target_from_string() in src/launcher.c.
TARGETS = {
    "terminal", "coding-agents", "files", "manuals", "models", "games",
    "music", "voice", "trash", "mailbox", "maintenance",
    "wardrobe", "bed", "status-board", "gate-locked", "walk-editor",
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
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/world/world.json"
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    check_c_parser_subset(text)
    world = json.loads(text)

    if world.get("world") != 1:
        fail("unsupported schema version (want world: 1)")
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
    for room in rooms:
        rid = room["id"]
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

    unreachable = [room["id"] for room in rooms
                   if room["id"] != start and room["id"] not in reachable]
    if unreachable:
        fail(f"rooms unreachable by any door: {', '.join(unreachable)}")

    print(f"world: OK ({len(rooms)} rooms, start '{start}')")


if __name__ == "__main__":
    main()
