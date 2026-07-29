#!/usr/bin/env python3
"""Walkable-space debug editor for the room plates.

Shows a room plate on a 6-logical-pixel grid with the walkable area tinted
translucent white, plus context markers (doors blue, object rects amber
outlines, NPC anchors magenta, incoming door spawns green). Paint with the
mouse: left button paints walkable, right button paints blocked. Saving
decomposes the painted cells into the world.json model — one walk bounding
rect plus obstacle rects (greedy exact cover) — and rewrites the file, then
runs tools/validate_world.py so a spawn painted into a wall is caught
immediately.

Runs inside kitty (kitty graphics protocol + SGR pixel mouse). Keys:
  1-5 / n / p  switch room        t  cycle style        g  toggle grid
  s  save      u  undo            r  reload from json   q  quit

Headless modes: --render OUT.png [--room ID] [--style STYLE] composes one
frame; --selftest (pure stdlib, no PIL) proves rasterize -> decompose ->
rasterize is a fixpoint for every room and stays under the obstacle cap.
"""

import argparse
import base64
import fcntl
import io
import json
import os
import select
import struct
import subprocess
import sys
import termios
import tty

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGICAL_W, LOGICAL_H = 480, 270
CELL = 6
COLS, ROWS = LOGICAL_W // CELL, LOGICAL_H // CELL
MAX_OBSTACLES = 24
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
DISPLAY_W, DISPLAY_H = 960, 540
SCALE = DISPLAY_W // LOGICAL_W
IMAGE_ID = 77


def world_path():
    root = os.environ.get("KILIX_LAND_DESKTOP_ASSETS") or REPO
    return os.path.join(root, "assets/world/world.json")


def load_world():
    with open(world_path(), encoding="utf-8") as handle:
        return json.load(handle)


def rasterize(room):
    """Cell is walkable iff its center is inside walk and outside every
    obstacle — the same point test desk.c applies to the player."""
    grid = bytearray(COLS * ROWS)
    walk = room["walk"]
    obstacles = room.get("obstacles", [])
    for cy in range(ROWS):
        for cx in range(COLS):
            x = cx * CELL + CELL / 2
            y = cy * CELL + CELL / 2
            if not (walk["x"] <= x <= walk["x"] + walk["w"] and
                    walk["y"] <= y <= walk["y"] + walk["h"]):
                continue
            if any(o["x"] <= x <= o["x"] + o["w"] and
                   o["y"] <= y <= o["y"] + o["h"] for o in obstacles):
                continue
            grid[cy * COLS + cx] = 1
    return grid


def decompose(grid):
    """-> (walk rect dict, [obstacle rect dicts]) or (None, error). Exact:
    re-rasterizing the result reproduces the grid cell for cell."""
    cells = [(cx, cy) for cy in range(ROWS) for cx in range(COLS)
             if grid[cy * COLS + cx]]
    if not cells:
        return None, "no walkable cells painted"
    min_cx = min(c[0] for c in cells)
    max_cx = max(c[0] for c in cells)
    min_cy = min(c[1] for c in cells)
    max_cy = max(c[1] for c in cells)
    walk = {"x": min_cx * CELL, "y": min_cy * CELL,
            "w": (max_cx - min_cx + 1) * CELL,
            "h": (max_cy - min_cy + 1) * CELL}
    blocked = {(cx, cy)
               for cy in range(min_cy, max_cy + 1)
               for cx in range(min_cx, max_cx + 1)
               if not grid[cy * COLS + cx]}
    rects = []
    while blocked:
        cx, cy = min(blocked, key=lambda c: (c[1], c[0]))
        width = 1
        while (cx + width, cy) in blocked and cx + width <= max_cx:
            width += 1
        height = 1
        while all((cx + i, cy + height) in blocked for i in range(width)) \
                and cy + height <= max_cy:
            height += 1
        for dy in range(height):
            for dx in range(width):
                blocked.discard((cx + dx, cy + dy))
        rects.append({"x": cx * CELL, "y": cy * CELL,
                      "w": width * CELL, "h": height * CELL})
    if len(rects) > MAX_OBSTACLES:
        return None, (f"{len(rects)} obstacle rects exceed the cap of "
                      f"{MAX_OBSTACLES}; simplify the blocked shape")
    return {"walk": walk, "obstacles": rects}, None


def apply_decomposition(room, result):
    room["walk"] = result["walk"]
    room["obstacles"] = result["obstacles"]


def incoming_spawns(world, room_id):
    points = []
    for room in world["rooms"]:
        for door in room.get("doors", []):
            if door.get("to") == room_id:
                spawn = door.get("spawn", {})
                points.append((spawn.get("x", 0), spawn.get("y", 0)))
    return points


def selftest():
    world = load_world()
    for room in world["rooms"]:
        grid = rasterize(room)
        result, error = decompose(grid)
        if error:
            print(f"walk-editor: {room['id']}: {error}", file=sys.stderr)
            return 1
        replay = dict(room)
        apply_decomposition(replay, result)
        if rasterize(replay) != grid:
            print(f"walk-editor: {room['id']}: decomposition is not a "
                  "fixpoint", file=sys.stderr)
            return 1
        for value in (result["walk"], *result["obstacles"]):
            if any(not isinstance(value[k], int) for k in ("x", "y", "w",
                                                           "h")):
                print(f"walk-editor: {room['id']}: non-integer rect",
                      file=sys.stderr)
                return 1
    print(f"walk-editor: OK (fixpoint over {len(world['rooms'])} rooms, "
          f"cell {CELL}px, cap {MAX_OBSTACLES})")
    return 0


def compose(world, room, grid, style, show_grid=True):
    """-> PNG bytes of plate + overlay at 960x540 (PIL only here)."""
    from PIL import Image, ImageDraw
    root = os.environ.get("KILIX_LAND_DESKTOP_ASSETS") or REPO
    plate_path = os.path.join(root, "assets/graphics/rooms", style,
                              room["plate"] + ".png")
    try:
        base = Image.open(plate_path).convert("RGB").resize(
            (DISPLAY_W, DISPLAY_H), Image.LANCZOS)
    except OSError:
        base = Image.new("RGB", (DISPLAY_W, DISPLAY_H), (28, 30, 38))
    base = base.convert("RGBA")
    overlay = Image.new("RGBA", (DISPLAY_W, DISPLAY_H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    cell_px = CELL * SCALE
    for cy in range(ROWS):
        for cx in range(COLS):
            if grid[cy * COLS + cx]:
                draw.rectangle([cx * cell_px, cy * cell_px,
                                (cx + 1) * cell_px - 1,
                                (cy + 1) * cell_px - 1],
                               fill=(255, 255, 255, 78))
    if show_grid:
        for cx in range(COLS + 1):
            draw.line([cx * cell_px, 0, cx * cell_px, DISPLAY_H],
                      fill=(0, 0, 0, 60))
        for cy in range(ROWS + 1):
            draw.line([0, cy * cell_px, DISPLAY_W, cy * cell_px],
                      fill=(0, 0, 0, 60))

    def scaled(rect):
        return [rect["x"] * SCALE, rect["y"] * SCALE,
                (rect["x"] + rect["w"]) * SCALE,
                (rect["y"] + rect["h"]) * SCALE]

    for door in room.get("doors", []):
        draw.rectangle(scaled(door["rect"]), fill=(80, 140, 255, 90),
                       outline=(120, 170, 255, 200))
    for entry in room.get("objects", []):
        draw.rectangle(scaled(entry["rect"]), outline=(255, 210, 80, 200),
                       width=2)
    for npc in room.get("npcs", []):
        x, y = npc["x"] * SCALE, npc["y"] * SCALE
        draw.ellipse([x - 5, y - 5, x + 5, y + 5], fill=(240, 80, 220, 220))
    for x, y in incoming_spawns(world, room["id"]):
        x, y = x * SCALE, y * SCALE
        draw.ellipse([x - 5, y - 5, x + 5, y + 5], fill=(90, 230, 120, 230))
    frame = Image.alpha_composite(base, overlay).convert("RGB")
    buffer = io.BytesIO()
    frame.save(buffer, format="PNG")
    return buffer.getvalue()


class Terminal:
    """Raw tty + kitty graphics + SGR pixel mouse, restored on exit."""

    def __init__(self):
        self.fd = sys.stdin.fileno()
        self.saved = termios.tcgetattr(self.fd)
        size = struct.unpack("HHHH", fcntl.ioctl(
            1, termios.TIOCGWINSZ, b"\0" * 8))
        self.rows, self.cols = size[0], size[1]
        self.xpix, self.ypix = size[2], size[3]
        if self.xpix == 0 or self.ypix == 0:
            raise SystemExit("terminal does not report pixel size; "
                             "run inside kitty/kilix")
        self.cell_w = self.xpix / self.cols
        self.cell_h = self.ypix / self.rows
        image_rows = max(4, self.rows - 3)
        image_cols = self.cols
        want_ratio = DISPLAY_W / DISPLAY_H
        box_w = image_cols * self.cell_w
        box_h = image_rows * self.cell_h
        if box_w / box_h > want_ratio:
            image_cols = max(4, int(box_h * want_ratio / self.cell_w))
        else:
            image_rows = max(4, int(box_w / want_ratio / self.cell_h))
        self.image_cols = image_cols
        self.image_rows = image_rows

    def __enter__(self):
        tty.setcbreak(self.fd)
        self.write("\x1b[?1049h\x1b[?25l\x1b[?1002h\x1b[?1006h\x1b[?1016h")
        return self

    def __exit__(self, *_):
        self.write(f"\x1b_Ga=d,d=i,i={IMAGE_ID},q=2\x1b\\")
        self.write("\x1b[?1016l\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[?1049l")
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)

    def write(self, text):
        os.write(1, text.encode())

    def show_image(self, png_bytes):
        self.write(f"\x1b_Ga=d,d=i,i={IMAGE_ID},q=2\x1b\\")
        self.write("\x1b[H")
        payload = base64.standard_b64encode(png_bytes).decode()
        first = True
        while payload:
            chunk, payload = payload[:4096], payload[4096:]
            more = 1 if payload else 0
            if first:
                self.write(f"\x1b_Ga=T,f=100,i={IMAGE_ID},"
                           f"c={self.image_cols},r={self.image_rows},"
                           f"q=2,m={more};{chunk}\x1b\\")
                first = False
            else:
                self.write(f"\x1b_Gm={more};{chunk}\x1b\\")

    def status(self, *lines):
        for offset, line in enumerate(lines):
            row = self.rows - len(lines) + offset + 1
            self.write(f"\x1b[{row};1H\x1b[2K" + line[: self.cols - 1])

    def logical_from_pixels(self, px, py):
        x = (px - 1) / (self.image_cols * self.cell_w) * LOGICAL_W
        y = (py - 1) / (self.image_rows * self.cell_h) * LOGICAL_H
        if 0 <= x < LOGICAL_W and 0 <= y < LOGICAL_H:
            return x, y
        return None


def parse_events(data):
    """-> (mouse events [(button, pressed_or_drag, px, py)], key list)."""
    mice, keys = [], []
    index = 0
    while index < len(data):
        if data.startswith(b"\x1b[<", index):
            end_m = data.find(b"M", index)
            end_r = data.find(b"m", index)
            candidates = [e for e in (end_m, end_r) if e != -1]
            if not candidates:
                break
            end = min(candidates)
            body = data[index + 3:end]
            try:
                button, px, py = (int(part) for part in body.split(b";"))
            except ValueError:
                index = end + 1
                continue
            mice.append((button, data[end:end + 1] == b"M", px, py))
            index = end + 1
        elif data[index] == 0x1b:
            index += 1
        else:
            keys.append(data[index])
            index += 1
    return mice, keys


def run_editor(initial_room, initial_style):
    world = load_world()
    rooms = world["rooms"]
    room_index = next((i for i, r in enumerate(rooms)
                       if r["id"] == initial_room), 0)
    style_index = STYLES.index(initial_style) if initial_style in STYLES \
        else 0
    grid = rasterize(rooms[room_index])
    undo_stack = []
    dirty = False
    show_grid = True
    message = "left paint · right block"
    paint_value = None

    with Terminal() as terminal:
        def repaint():
            room = rooms[room_index]
            terminal.show_image(compose(world, room, grid,
                                        STYLES[style_index], show_grid))
            state_flag = "*" if dirty else " "
            terminal.status(
                f"walk-editor  {room['id']} ({STYLES[style_index]}){state_flag}"
                f"  walkable {sum(grid)}/{COLS * ROWS} cells",
                "1-5/n/p room · t style · g grid · s save · u undo · "
                f"r reload · q quit   {message}")

        def switch_room(new_index):
            nonlocal room_index, grid, dirty, undo_stack
            room_index = new_index % len(rooms)
            grid = rasterize(rooms[room_index])
            undo_stack = []
            dirty = False

        repaint()
        while True:
            ready, _, _ = select.select([terminal.fd], [], [], 0.5)
            if not ready:
                continue
            data = os.read(terminal.fd, 65536)
            mice, keys = parse_events(data)
            changed = False
            for button, active, px, py in mice:
                if not active:
                    paint_value = None
                    continue
                base_button = button & 3
                motion = bool(button & 32)
                point = terminal.logical_from_pixels(px, py)
                if point is None:
                    continue
                cx, cy = int(point[0] // CELL), int(point[1] // CELL)
                if not (0 <= cx < COLS and 0 <= cy < ROWS):
                    continue
                if not motion:
                    undo_stack.append(bytes(grid))
                    del undo_stack[:-64]
                    paint_value = 1 if base_button == 0 else 0
                if paint_value is None:
                    continue
                if grid[cy * COLS + cx] != paint_value:
                    grid[cy * COLS + cx] = paint_value
                    dirty = True
                    changed = True
            for key in keys:
                char = chr(key) if 32 <= key < 127 else ""
                if char == "q":
                    if dirty and message != "unsaved changes - q again":
                        message = "unsaved changes - q again"
                        changed = True
                        break
                    return 0
                if char == "s":
                    result, error = decompose(grid)
                    if error:
                        message = error
                    else:
                        apply_decomposition(rooms[room_index], result)
                        with open(world_path(), "w",
                                  encoding="utf-8") as handle:
                            json.dump(world, handle, indent=2)
                            handle.write("\n")
                        verdict = subprocess.run(
                            [sys.executable,
                             os.path.join(REPO, "tools/validate_world.py"),
                             world_path()],
                            capture_output=True, text=True)
                        if verdict.returncode == 0:
                            dirty = False
                            message = (f"saved: walk + "
                                       f"{len(result['obstacles'])} "
                                       "obstacles, validator OK")
                        else:
                            message = ("SAVED BUT INVALID: " +
                                       verdict.stderr.strip()[-90:])
                elif char == "u" and undo_stack:
                    grid = bytearray(undo_stack.pop())
                    dirty = True
                    message = "undo"
                elif char == "g":
                    show_grid = not show_grid
                elif char == "t":
                    style_index = (style_index + 1) % len(STYLES)
                elif char == "n":
                    switch_room(room_index + 1)
                elif char == "p":
                    switch_room(room_index - 1)
                elif char in "123456789":
                    target = int(char) - 1
                    if target < len(rooms):
                        switch_room(target)
                elif char == "r":
                    world = load_world()
                    rooms = world["rooms"]
                    switch_room(room_index)
                    message = "reloaded"
                changed = True
            if changed:
                repaint()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--room", default="bedroom")
    parser.add_argument("--style", default="legend")
    parser.add_argument("--render", metavar="OUT")
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    if arguments.selftest:
        return selftest()
    if arguments.render:
        world = load_world()
        room = next((r for r in world["rooms"]
                     if r["id"] == arguments.room), world["rooms"][0])
        png = compose(world, room, rasterize(room), arguments.style)
        with open(arguments.render, "wb") as handle:
            handle.write(png)
        print(f"walk-editor: rendered {room['id']} ({arguments.style}) "
              f"to {arguments.render}")
        return 0
    return run_editor(arguments.room, arguments.style)


if __name__ == "__main__":
    raise SystemExit(main())
