#!/usr/bin/env python3
"""pty harness for tools/walk_editor.py — drives it exactly as kitty would.

Opens a pty whose slave reports the live kilix tab geometry (48x212 cells,
1908x960 px) via TIOCSWINSZ before spawn, runs the editor against a TEMP copy
of assets/world/world.json, and asserts the full interactive contract:

  startup   modes 1003/1006/1016 enabled (1003 = ANY-event, the hover
            superset of 1002), OSC 22 crosshair pointer requested, kitty
            graphics a=T transmitted, status line written
  hovering  motion-only reports (cb 35, final 'M', NO button) sweeping
            across cells retransmit the image (the drawn cursor tracks),
            report hover coords in the status line, and leave the grid
            byte-untouched: no paint/block, no dirty flag, walkable count
            unchanged — then a normal press+drag+release still paints
  painting  a left press + drag + release in SGR PIXEL coordinates (mode
            1016, the mode the live tab reports SET) flips the targeted grid
            cells, the editor retransmits the image (second a=T)
  saving    's' rewrites world.json so the painted cells rasterize walkable
            and the file still passes tools/validate_world.py; 'q' exits 0

Four scenarios, all must pass:
  clean          the whole mouse burst arrives in one read
  torture        the same bytes arrive in 3-byte fragments with 10ms gaps,
                 splitting every escape sequence across reads — what a pty
                 broker or a slow relay does in practice
  hover          the hover sweep (then press+drag+release) in one read
  hover-torture  the hover sweep through the same 3-byte fragmentation

Usage:  test_walk_editor.py            verbose report with log evidence
        test_walk_editor.py --quick    one line per check, nonzero on failure
"""

import argparse
import fcntl
import importlib.util
import json
import os
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
EDITOR = os.path.join(TOOLS, "walk_editor.py")
VALIDATOR = os.path.join(TOOLS, "validate_world.py")
WORLD_SRC = os.path.join(REPO, "assets/world/world.json")

# Live kilix tab geometry (TIOCGWINSZ probe: rows, cols, xpix, ypix).
ROWS, COLS, XPIX, YPIX = 48, 212, 1908, 960

# Cells painted walkable: inside bedroom's first obstacle (currently blocked),
# inside the existing walk bounding box, so decompose stays valid and small.
PAINT_CELLS = [(8, 31), (9, 31), (10, 31), (11, 31), (12, 31)]

APC_TRANSMIT = b"\x1b_Ga=T"


def load_editor_module():
    spec = importlib.util.spec_from_file_location("walk_editor", EDITOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


W = load_editor_module()


def image_box():
    """Replicate Terminal.__init__'s image_cols/image_rows math exactly."""
    cell_w, cell_h = XPIX / COLS, YPIX / ROWS
    image_rows = max(4, ROWS - 3)
    image_cols = COLS
    want_ratio = W.DISPLAY_W / W.DISPLAY_H
    box_w = image_cols * cell_w
    box_h = image_rows * cell_h
    if box_w / box_h > want_ratio:
        image_cols = max(4, int(box_h * want_ratio / cell_w))
    else:
        image_rows = max(4, int(box_w / want_ratio / cell_h))
    return image_cols * cell_w, image_rows * cell_h


def pixel_for_cell(cx, cy):
    """SGR-1016 pixel coords whose editor-side mapping lands in (cx, cy)."""
    box_w, box_h = image_box()
    lx, ly = cx * W.CELL + W.CELL / 2, cy * W.CELL + W.CELL / 2
    px = int(round(lx / W.LOGICAL_W * box_w))
    py = int(round(ly / W.LOGICAL_H * box_h))
    x = px / box_w * W.LOGICAL_W                 # editor's inverse mapping
    y = py / box_h * W.LOGICAL_H
    if int(x // W.CELL) != cx or int(y // W.CELL) != cy:
        raise AssertionError(f"pixel mapping drifted for cell ({cx},{cy})")
    return px, py


def mouse_burst():
    """Press + drags + release exactly as kitty emits them in mode 1016."""
    cells = PAINT_CELLS
    px, py = pixel_for_cell(*cells[0])
    out = [f"\x1b[<0;{px};{py}M"]                       # left press
    for cell in cells[1:]:
        px, py = pixel_for_cell(*cell)
        out.append(f"\x1b[<32;{px};{py}M")              # drag motion
    out.append(f"\x1b[<0;{px};{py}m")                   # release
    return "".join(out).encode()


# Motion-only hover sweep: row 20, cells 6..15 (disjoint from PAINT_CELLS).
HOVER_CELLS = [(cx, 20) for cx in range(6, 16)]


def hover_burst():
    """ANY-event (1003) hover exactly as kitty emits it: cb 3 (no button)
    | 32 (motion) = 35, final 'M' — never a press, never a release."""
    out = []
    for cell in HOVER_CELLS:
        px, py = pixel_for_cell(*cell)
        out.append(f"\x1b[<35;{px};{py}M")
    return "".join(out).encode()


class Session:
    """One editor run on a pty with the live winsize, temp assets tree."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="walk_editor_test_")
        world_dir = os.path.join(self.tmp, "assets/world")
        os.makedirs(world_dir)
        self.world = os.path.join(world_dir, "world.json")
        shutil.copy(WORLD_SRC, self.world)
        with open(self.world, "rb") as handle:
            self.original = handle.read()
        self.log = bytearray()
        self.lock = threading.Lock()
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, XPIX, YPIX))
        env = dict(os.environ,
                   KILIX_LAND_DESKTOP_ASSETS=self.tmp,
                   TERM="xterm-kitty")
        self.child = subprocess.Popen(
            [sys.executable, EDITOR], stdin=slave, stdout=slave,
            stderr=slave, env=env, start_new_session=True)
        os.close(slave)
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()

    def _drain(self):
        while True:
            try:
                ready, _, _ = select.select([self.master], [], [], 0.2)
                if not ready:
                    continue
                chunk = os.read(self.master, 65536)
            except OSError:
                return
            if not chunk:
                return
            with self.lock:
                self.log.extend(chunk)

    def snapshot(self):
        with self.lock:
            return bytes(self.log)

    def wait_for(self, needle, timeout, start=0):
        """-> offset of needle at/after start, or -1 on timeout."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            index = self.snapshot().find(needle, start)
            if index != -1:
                return index
            time.sleep(0.05)
        return -1

    def send(self, data):
        os.write(self.master, data)

    def close(self):
        try:
            if self.child.poll() is None:
                self.send(b"q")
                time.sleep(0.4)
                self.send(b"q")
            self.child.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            self.child.kill()
            self.child.wait()
        try:
            os.close(self.master)
        except OSError:
            pass
        self.reader.join(timeout=1)

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


def status_rooms(log):
    """Room ids the editor ever showed in its status line, in order."""
    seen = []
    for match in re.finditer(rb"walk-editor  (\S+) \(", log):
        room = match.group(1).decode()
        if not seen or seen[-1] != room:
            seen.append(room)
    return seen


def send_burst(session, burst, fragmented):
    if fragmented:
        for i in range(0, len(burst), 3):
            session.send(burst[i:i + 3])
            time.sleep(0.010)
    else:
        session.send(burst)


def run_scenario(name, fragmented, verbose, hover=False):
    checks = []

    def check(label, ok, detail=""):
        checks.append((label, bool(ok), detail))

    session = Session()
    try:
        # (a) mouse modes, (b) first graphics transmit, (c) status line
        for mode in (b"\x1b[?1003h", b"\x1b[?1006h", b"\x1b[?1016h"):
            check(f"enables {mode[3:-1].decode()}",
                  session.wait_for(mode, 10) != -1)
        check("requests crosshair pointer (OSC 22)",
              session.wait_for(b"\x1b]22;crosshair\x07", 5) != -1)
        first_t = session.wait_for(APC_TRANSMIT, 15)
        check("transmits kitty image (a=T)", first_t != -1)
        check("writes status line",
              session.wait_for(b"walk-editor  ", 5) != -1)

        if hover:
            # Motion-only sweep with NO button press: the drawn cursor
            # must track (image retransmit + hover status), the grid must
            # stay byte-identical (no paint/block notes, no dirty flag,
            # walkable count unchanged) — and painting must still work
            # afterwards (the shared paint phase below runs regardless).
            time.sleep(0.2)              # let startup repaints settle
            hover_mark = len(session.snapshot())
            pre_counts = re.findall(rb"walkable (\d+)/",
                                    session.snapshot()[:hover_mark])
            send_burst(session, hover_burst(), fragmented)
            hover_t = session.wait_for(APC_TRANSMIT, 6, start=hover_mark)
            check("retransmits image on hover sweep (cursor tracks)",
                  hover_t != -1)
            last_cell = "cell {},{}".format(*HOVER_CELLS[-1]).encode()
            check("status reports final hover cell",
                  session.wait_for(last_cell, 6, start=hover_mark) != -1)
            time.sleep(0.3)              # trailing repaint + status settle
            hover_log = session.snapshot()[hover_mark:]
            check("hover never paints (no paint/block mouse notes)",
                  not re.search(rb"cell \d+,\d+ (paint|block)", hover_log))
            check("hover leaves grid clean (no dirty flag)",
                  b")*  walkable" not in hover_log)
            counts = set(re.findall(rb"walkable (\d+)/", hover_log))
            check("walkable count unchanged by hover",
                  bool(pre_counts) and counts <= {pre_counts[-1]},
                  f"before={pre_counts[-1:]} during/after={sorted(counts)}")

        mark = len(session.snapshot())
        send_burst(session, mouse_burst(), fragmented)

        second_t = session.wait_for(APC_TRANSMIT, 6, start=mark)
        check("retransmits image after painting (2nd a=T)", second_t != -1)

        time.sleep(0.5)                      # let the input queue drain
        save_mark = len(session.snapshot())
        session.send(b"s")
        saved_ok = session.wait_for(b"validator OK", 15, start=save_mark)
        log = session.snapshot()
        save_error = (b"SAVED BUT INVALID" in log[save_mark:]
                      or b"exceed the cap" in log[save_mark:]
                      or b"no walkable cells" in log[save_mark:])
        check("save reports 'validator OK'", saved_ok != -1,
              "editor reported a save error" if save_error else
              "no save acknowledgement seen")
        session.send(b"q")
        exited = True
        try:
            session.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            exited = False
            session.send(b"q")
        check("quits on q", exited)
    finally:
        session.close()
        log = session.snapshot()

    with open(session.world, "rb") as handle:
        final = handle.read()
    check("world.json rewritten", final != session.original)

    verdict = subprocess.run([sys.executable, VALIDATOR, session.world],
                             capture_output=True, text=True)
    check("saved file passes validate_world.py", verdict.returncode == 0,
          verdict.stderr.strip()[-120:])

    try:
        world = json.loads(final)
        room = next(r for r in world["rooms"] if r["id"] == "bedroom")
        grid = W.rasterize(room)
        missing = [c for c in PAINT_CELLS
                   if not grid[c[1] * W.COLS + c[0]]]
    except (ValueError, StopIteration, KeyError) as error:
        missing = [f"unreadable: {error}"]
    check("painted cells are walkable in saved world", not missing,
          f"cells never painted: {missing}")

    crashed = b"Traceback" in log
    check("no traceback", not crashed)
    session.cleanup()

    ok = all(c[1] for c in checks)
    label = name + (" (3-byte fragments)" if fragmented else "")
    print(f"[{'PASS' if ok else 'FAIL'}] scenario: {label}")
    for label_, good, detail in checks:
        if verbose or not good:
            extra = f"  -- {detail}" if detail and not good else ""
            print(f"    {'ok  ' if good else 'FAIL'} {label_}{extra}")
    if verbose or not ok:
        rooms = status_rooms(log)
        print(f"    evidence: a=T count={log.count(APC_TRANSMIT)}, "
              f"status rooms seen={rooms}")
        if crashed:
            tail = log[log.find(b"Traceback"):][:600]
            print("    " + tail.decode(errors="replace"))
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true",
                        help="terse output; exit nonzero on any failure")
    arguments = parser.parse_args()
    verbose = not arguments.quick
    ok = run_scenario("clean", False, verbose)
    ok &= run_scenario("torture", True, verbose)
    ok &= run_scenario("hover", False, verbose, hover=True)
    ok &= run_scenario("hover-torture", True, verbose, hover=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
