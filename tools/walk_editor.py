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
import re
import select
import struct
import subprocess
import sys
import termios
import time
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


_PLATE_CACHE = {}
# grid byte 0 -> alpha 0, any nonzero -> the walkable tint alpha (78)
_WALK_ALPHA = bytes(0 if value == 0 else 78 for value in range(256))


def load_plate(style, plate):
    """Plate already resized to display size, cached per (root, style,
    plate) so a repaint never re-reads and re-LANCZOSes the PNG."""
    from PIL import Image
    root = os.environ.get("KILIX_LAND_DESKTOP_ASSETS") or REPO
    key = (root, style, plate)
    cached = _PLATE_CACHE.get(key)
    if cached is None:
        path = os.path.join(root, "assets/graphics/rooms", style,
                            plate + ".png")
        try:
            cached = Image.open(path).convert("RGB").resize(
                (DISPLAY_W, DISPLAY_H), Image.LANCZOS).convert("RGBA")
        except OSError:
            cached = Image.new("RGBA", (DISPLAY_W, DISPLAY_H),
                               (28, 30, 38, 255))
        _PLATE_CACHE[key] = cached
    return cached


def compose(world, room, grid, style, show_grid=True):
    """-> PNG bytes of plate + overlay at 960x540 (PIL only here).

    The walkable tint is one COLSxROWS alpha mask scaled with NEAREST and
    alpha_composited once — not one rectangle per cell — so repainting
    mid-drag stays well under 100ms."""
    from PIL import Image, ImageDraw
    base = load_plate(style, room["plate"])
    walk_alpha = Image.frombytes(
        "L", (COLS, ROWS), bytes(grid).translate(_WALK_ALPHA)).resize(
        (DISPLAY_W, DISPLAY_H), Image.NEAREST)
    walk_tint = Image.new("RGBA", (DISPLAY_W, DISPLAY_H), (255, 255, 255, 0))
    walk_tint.putalpha(walk_alpha)
    frame = Image.alpha_composite(base, walk_tint)
    overlay = Image.new("RGBA", (DISPLAY_W, DISPLAY_H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    cell_px = CELL * SCALE
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
    frame = Image.alpha_composite(frame, overlay).convert("RGB")
    buffer = io.BytesIO()
    frame.save(buffer, format="PNG", compress_level=1)
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
        # DECRQM: did the terminal actually grant pixel reports?  The reply
        # (\x1b[?1016;Ps$y, Ps 1 or 3 = set) is consumed by parse_events in
        # the event loop; until it says otherwise we assume pixel coords,
        # which is what kitty/kilix grants.  A terminal that answers "reset"
        # drops us to SGR cell coordinates (1006) transparently.
        self.write("\x1b[?1016$p")
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
        """Mode 1016: kitty reports 0-based pixels relative to the cell
        content area — the same box TIOCGWINSZ's xpix/ypix describes, so
        window padding cancels out and no offset correction is needed
        (1-based would be an xterm-ism kitty does not follow)."""
        x = px / (self.image_cols * self.cell_w) * LOGICAL_W
        y = py / (self.image_rows * self.cell_h) * LOGICAL_H
        if 0 <= x < LOGICAL_W and 0 <= y < LOGICAL_H:
            return x, y
        return None

    def logical_from_cells(self, col, row):
        """Mode 1006 fallback: 1-based cell coordinates; use the center of
        the reported cell so painting still lands where the cursor is."""
        return self.logical_from_pixels((col - 0.5) * self.cell_w,
                                        (row - 0.5) * self.cell_h)


MODE_REPORT = re.compile(rb"\x1b\[\?(\d+);(\d+)\$y")
STRING_INTRODUCERS = (0x5d, 0x50, 0x5f, 0x5e, 0x58)  # OSC DCS APC PM SOS
RUNAWAY = 4096              # unterminated-sequence cap before we drop it


def parse_events(data):
    """-> (mouse events [(button, pressed_or_drag, px, py)], key list,
    mode reports [(mode, status)], unconsumed tail bytes).

    Never drops a split escape sequence: any incomplete trailing sequence
    (bare ESC, partial CSI like \\x1b[<0;17, unterminated OSC/DCS/APC) is
    returned as the tail, which the caller MUST prepend to its next read.
    The pty broker delivers relay-sized packets, so 1016's per-pixel drag
    reports routinely split mid-sequence — the old parser threw the prefix
    away and the orphaned coordinate digits leaked into the key stream,
    where '1'-'5' switched rooms and wiped unsaved painting.  Non-mouse
    escape sequences (including the DECRQM reply) are consumed whole so
    their bytes can never reach the key path."""
    mice, keys, reports = [], [], []
    index, length = 0, len(data)
    while index < length:
        if data[index] != 0x1b:
            keys.append(data[index])
            index += 1
            continue
        if index + 1 >= length:
            break                                   # bare trailing ESC
        kind = data[index + 1]
        if kind == 0x5b:                            # CSI
            scan = index + 2
            while scan < length and 0x20 <= data[scan] <= 0x3f:
                scan += 1
            if scan >= length:                      # split mid-sequence
                if length - index > RUNAWAY:
                    index = length                  # runaway; drop it
                break
            if not 0x40 <= data[scan] <= 0x7e:      # malformed; resync
                index = scan
                continue
            sequence = data[index:scan + 1]
            index = scan + 1
            if sequence.startswith(b"\x1b[<") and sequence[-1:] in (b"M",
                                                                    b"m"):
                try:
                    button, px, py = (int(part) for part in
                                      sequence[3:-1].split(b";"))
                except ValueError:
                    continue
                mice.append((button, sequence[-1:] == b"M", px, py))
            else:
                match = MODE_REPORT.fullmatch(sequence)
                if match:
                    reports.append((int(match.group(1)),
                                    int(match.group(2))))
        elif kind in STRING_INTRODUCERS:            # terminated by ST
            stop = data.find(b"\x1b\\", index + 2)
            if kind == 0x5d:                        # OSC: BEL also ends it
                bel = data.find(b"\x07", index + 2)
                if bel != -1 and (stop == -1 or bel < stop):
                    index = bel + 1
                    continue
            if stop == -1:
                if length - index > RUNAWAY:
                    index = length                  # runaway; drop it
                break
            index = stop + 2
        elif kind == 0x4f:                          # SS3: one final byte
            if index + 2 >= length:
                break
            index += 3
        else:                                       # two-byte escape
            index += 2
    return mice, keys, reports, data[index:]


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
    pixel_mouse = True          # until a DECRQM reply says otherwise
    mouse_note = "mouse: no input yet"

    with Terminal() as terminal:
        def status_lines():
            room = rooms[room_index]
            state_flag = "*" if dirty else " "
            unit = "" if pixel_mouse else " [cell coords]"
            return (
                f"walk-editor  {room['id']} ({STYLES[style_index]})"
                f"{state_flag}  walkable {sum(grid)}/{COLS * ROWS} cells"
                f"   {mouse_note}{unit}",
                "1-5/n/p room · t style · g grid · s save · u undo · "
                f"r reload · q quit   {message}")

        def repaint():
            terminal.show_image(compose(world, rooms[room_index], grid,
                                        STYLES[style_index], show_grid))
            terminal.status(*status_lines())

        def switch_room(new_index):
            nonlocal room_index, grid, dirty, undo_stack
            room_index = new_index % len(rooms)
            grid = rasterize(rooms[room_index])
            undo_stack = []
            dirty = False

        repaint()
        last_draw = time.monotonic()
        pending = b""               # incomplete escape tail, carried over
        need_image = need_status = False
        while True:
            timeout = 0.05 if (need_image or need_status) else 0.5
            ready, _, _ = select.select([terminal.fd], [], [], timeout)
            if ready:
                try:
                    data = os.read(terminal.fd, 65536)
                except OSError:
                    return 0        # pty gone
                if not data:
                    return 0        # EOF
                mice, keys, reports, pending = parse_events(pending + data)
            else:
                mice, keys, reports = [], [], []
            for mode, value in reports:
                if mode == 1016:
                    pixel_mouse = value in (1, 3)
                    need_status = True
            for button, active, px, py in mice:
                need_status = True
                if button & 256:            # kitty window-leave (cb 288)
                    mouse_note = f"mouse: {px},{py} leave"
                    continue
                if button & (64 | 128):     # wheel / extra buttons
                    mouse_note = f"mouse: {px},{py} wheel/aux"
                    continue
                if not active:              # release ends the gesture
                    paint_value = None
                    mouse_note = f"mouse: {px},{py} release"
                    continue
                base_button = button & 3
                motion = bool(button & 32)
                point = (terminal.logical_from_pixels(px, py) if pixel_mouse
                         else terminal.logical_from_cells(px, py))
                if point is None:
                    mouse_note = f"mouse: {px},{py} outside image"
                    continue
                cx, cy = int(point[0] // CELL), int(point[1] // CELL)
                if not (0 <= cx < COLS and 0 <= cy < ROWS):
                    mouse_note = f"mouse: {px},{py} outside grid"
                    continue
                if base_button in (0, 2) and (not motion
                                              or paint_value is None):
                    # A press starts a gesture.  A drag whose press never
                    # arrived still carries the held button in its low
                    # bits, so adopt it: one lost press must not kill the
                    # whole gesture.
                    if paint_value is None:
                        undo_stack.append(bytes(grid))
                        del undo_stack[:-64]
                    paint_value = 1 if base_button == 0 else 0
                if paint_value is None:
                    mouse_note = f"mouse: {px},{py} cell {cx},{cy} idle"
                    continue
                action = "paint" if paint_value else "block"
                mouse_note = f"mouse: {px},{py} cell {cx},{cy} {action}"
                if grid[cy * COLS + cx] != paint_value:
                    grid[cy * COLS + cx] = paint_value
                    dirty = True
                    need_image = True
            for key in keys:
                char = chr(key) if 32 <= key < 127 else ""
                if char == "q":
                    if dirty and message != "unsaved changes - q again":
                        message = "unsaved changes - q again"
                        need_image = True
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
                need_image = True
            # Coalesce repaints: during a 1016 drag kitty reports every
            # pixel of motion, so redraw at most ~30fps while input is
            # still streaming and flush immediately once it pauses.
            now = time.monotonic()
            if (need_image or need_status) and (not ready
                                                or now - last_draw >= 0.03):
                if need_image:
                    repaint()
                else:
                    terminal.status(*status_lines())
                last_draw = now
                need_image = need_status = False


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
