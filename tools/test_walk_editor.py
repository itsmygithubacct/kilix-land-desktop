#!/usr/bin/env python3
"""pty harness for tools/walk_editor.py — drives it exactly as kitty would.

Opens a pty whose slave reports the live kilix tab geometry (48x212 cells,
1908x960 px) via TIOCSWINSZ before spawn, runs the editor against a TEMP copy
of assets/world/world.json, and asserts the full interactive contract:

  startup   modes 1003/1006/1016 enabled (1003 = ANY-event, the hover
            superset of 1002), OSC 22 crosshair pointer requested, kitty
            graphics a=T transmitted, status line written
  hovering  motion-only reports (cb 35, final 'M', NO button) sweeping
            across cells patch the displayed image with a=f frame edits
            (the drawn cursor tracks — no full retransmit, wire bytes
            under a hard budget), report hover coords in the status line,
            and leave the grid byte-untouched: no paint/block, no dirty
            flag, walkable count unchanged — then a normal
            press+drag+release still paints
  painting  a left press + drag + release in SGR PIXEL coordinates (mode
            1016, the mode the live tab reports SET) flips the targeted grid
            cells, the editor patches the image (a=f edits)
  saving    's' rewrites world.json so the painted cells rasterize walkable
            and the file still passes tools/validate_world.py; 'q' exits 0

Every scenario also parses the editor's full output stream and asserts the
presentation contract: full frames are raw zlib-deflated RGBA (f=32,o=z at
960x540, decoded and length-checked), the two image ids alternate, the
previous id is deleted only AFTER the replacing a=T — never between two
consecutive transmits, and nothing before the first frame; a=f edits are
partial rects (f=32,o=z,r=1 with x,y,s,v, payload length-checked) that
always target the image id currently on screen; and every present — full
or edit batch — sits inside a DEC 2026 synchronized update.

Eight scenarios, all must pass:
  clean          the whole mouse burst arrives in one read
  torture        the same bytes arrive in 3-byte fragments with 10ms gaps,
                 splitting every escape sequence across reads — what a pty
                 broker or a slow relay does in practice
  hover          the hover sweep (then press+drag+release) in one read
  hover-torture  the hover sweep through the same 3-byte fragmentation
  drag-cadence   a ~500ms continuous fragmented paint drag: presents during
                 the gesture stay throttled to the drag cadence while EVERY
                 swept cell still ends up painted in the saved world — and
                 the gesture is all a=f edits, never a mid-drag full frame
  toggle+brush   left click toggles, [/] sizes the brush footprint
  edit-coherence with KILIX_WALK_EDITOR_DEBUG_CHECKSUM=1 the editor
                 byte-compares its patched scene cache against a fresh
                 reference compose after every edit batch (paint + hover
                 sequence must report coherence=ok, never DRIFT), and
                 grid toggles / room switches still take the full-frame
                 double-buffer delete-after-place path
  behind-authoring  walk-behind mask mode ('o') against a synthetic
                 1280x720 plate with a distinct-colored blob written into
                 the temp asset tree: a magic-wand click floods exactly
                 the blob's pixels into region 1, 'b' sets the baseline
                 at the hovered y, 's' writes the plate-sized 8-bit
                 grayscale <room>-behind.png plus the world.json
                 "walkbehinds" declaration (validator green); a
                 right-drag erases a strip, x clears the region, and the
                 empty mask + dropped declaration round-trip too

Usage:  test_walk_editor.py            verbose report with log evidence
        test_walk_editor.py --quick    one line per check, nonzero on failure
"""

import argparse
import base64
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
import zlib

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
APC_EDIT = b"\x1b_Ga=f"          # frame-edit load (first chunk or m= chain)
SYNC_H = b"\x1b[?2026h"          # one bracket per present, full or edit
# Whole a=f escape tokens (payload is base64, never contains ESC).
EDIT_TOKEN = re.compile(rb"\x1b_Ga=f[^\x1b]*\x1b\\")

# One token per kitty graphics APC (control[;payload]) or DEC 2026 flip.
STREAM_TOKEN = re.compile(
    rb"\x1b_G([^;\x1b]*)(?:;([^\x1b]*))?\x1b\\|\x1b\[\?2026([hl])")


def parse_presentation(log):
    """Tokenize the editor's output into ordered presentation events:
    ('sync', pos, 'h'|'l'), ('transmit', pos, {'id','keys','payload'}),
    ('edit', pos, {'id','keys','payload'}), ('delete', pos,
    {'id','mode'}).  A chunked a=T (m=1 ... m=0) collapses into one
    transmit event at the first chunk's position with the base64 payload
    joined across chunks; a chunked a=f collapses likewise (every a=f
    chunk repeats a=f — kitty routes action-less chunks to the add path —
    and only the first carries the rect geometry s=,v=,x=,y=)."""
    events = []
    open_transmit = None
    open_edit = None
    for match in STREAM_TOKEN.finditer(log):
        if match.group(3) is not None:
            events.append(("sync", match.start(), match.group(3).decode()))
            continue
        control = (match.group(1) or b"").decode()
        payload = match.group(2) or b""
        keys = dict(part.split("=", 1) for part in control.split(",")
                    if "=" in part)
        if keys.get("a") == "T":
            open_transmit = {"pos": match.start(), "id": int(keys["i"]),
                             "keys": keys, "payload": bytearray(payload)}
            if keys.get("m", "0") != "1":
                events.append(("transmit", open_transmit["pos"],
                               open_transmit))
                open_transmit = None
        elif keys.get("a") == "f":
            if "s" in keys:                 # first chunk: rect geometry
                open_edit = {"pos": match.start(),
                             "id": int(keys.get("i", 0)), "keys": keys,
                             "payload": bytearray(payload)}
            elif open_edit is not None:     # continuation chunk
                open_edit["payload"].extend(payload)
            else:
                continue
            if keys.get("m", "0") != "1":
                events.append(("edit", open_edit["pos"], open_edit))
                open_edit = None
        elif keys.get("a") == "d":
            events.append(("delete", match.start(),
                           {"id": int(keys.get("i", 0)),
                            "mode": keys.get("d", "")}))
        elif open_transmit is not None and "m" in keys:
            open_transmit["payload"].extend(payload)
            if keys["m"] != "1":
                events.append(("transmit", open_transmit["pos"],
                               open_transmit))
                open_transmit = None
    return events


def check_presentation(log, check):
    """Assert the flicker-free presentation contract on a session log."""
    events = parse_presentation(log)
    transmits = [e for e in events if e[0] == "transmit"]
    # In-frame deletes are a=d,d=i; the exit path's d=I teardown of both
    # ids is presentation cleanup, not part of the frame contract.
    deletes = [(pos, info) for kind, pos, info in events
               if kind == "delete" and info["mode"] == "i"]
    edits = [(pos, info) for kind, pos, info in events if kind == "edit"]
    syncs = [(pos, which) for kind, pos, which in events if kind == "sync"]
    check("presents at least one frame", bool(transmits))
    if not transmits:
        return
    keys = transmits[0][2]["keys"]
    frame_ok = (keys.get("f") == "32" and keys.get("o") == "z"
                and keys.get("s") == str(W.DISPLAY_W)
                and keys.get("v") == str(W.DISPLAY_H))
    check("frames are raw RGBA f=32,o=z at "
          f"{W.DISPLAY_W}x{W.DISPLAY_H}", frame_ok, f"keys={keys}")
    try:
        raw = zlib.decompress(
            base64.b64decode(bytes(transmits[0][2]["payload"])))
        expected = W.DISPLAY_W * W.DISPLAY_H * 4
        check("frame payload decodes to full RGBA byte count",
              len(raw) == expected, f"{len(raw)} != {expected}")
    except (ValueError, zlib.error) as error:
        check("frame payload decodes to full RGBA byte count", False,
              f"decode failed: {error}")
    ids = [t[2]["id"] for t in transmits]
    check("image ids alternate",
          all(a != b for a, b in zip(ids, ids[1:])), f"ids={ids[:16]}")
    check("no placement delete before the first frame",
          not any(pos < transmits[0][1] for pos, _ in deletes))
    order_ok, order_detail = True, ""
    for i in range(len(transmits) - 1):
        old_pos, old_id = transmits[i][1], transmits[i][2]["id"]
        new_pos = transmits[i + 1][1]
        stop = transmits[i + 2][1] if i + 2 < len(transmits) else len(log)
        if any(old_pos < pos < new_pos and info["id"] == old_id
               for pos, info in deletes):
            order_ok = False
            order_detail = (f"id {old_id} deleted BEFORE its replacement "
                            f"(transmit #{i + 1}) — blank-gap flicker")
            break
        if not any(new_pos < pos < stop and info["id"] == old_id
                   for pos, info in deletes):
            order_ok = False
            order_detail = (f"id {old_id} never deleted after replacement "
                            f"transmit #{i + 1}")
            break
    check("previous id deleted only AFTER the new a=T", order_ok,
          order_detail)
    sync_ok, sync_detail = True, ""
    for tpos in sorted([t[1] for t in transmits] + [e[0] for e in edits]):
        before = [which for pos, which in syncs if pos < tpos]
        after = [which for pos, which in syncs if pos > tpos]
        if not before or before[-1] != "h":
            sync_ok, sync_detail = \
                False, f"present at {tpos} not preceded by ?2026h"
            break
        if not after or after[0] != "l":
            sync_ok, sync_detail = \
                False, f"present at {tpos} not followed by ?2026l"
            break
    check("every present wrapped in DEC 2026 h/l", sync_ok, sync_detail)
    if edits:
        bad = [info["keys"] for _, info in edits
               if not (info["keys"].get("f") == "32"
                       and info["keys"].get("o") == "z"
                       and info["keys"].get("r") == "1"
                       and all(k in info["keys"] for k in "xysv"))]
        check("edits are partial-rect a=f (f=32,o=z,r=1 with x,y,s,v)",
              not bad, f"bad keys={bad[:1]}")
        info0 = edits[0][1]
        try:
            raw = zlib.decompress(
                base64.b64decode(bytes(info0["payload"])))
            want = int(info0["keys"].get("s", 0)) * \
                int(info0["keys"].get("v", 0)) * 4
            check("edit payload decodes to its rect byte count",
                  want > 0 and len(raw) == want, f"{len(raw)} != {want}")
        except (ValueError, zlib.error) as error:
            check("edit payload decodes to its rect byte count", False,
                  f"decode failed: {error}")

        def shown_at(pos):
            shown = [t[2]["id"] for t in transmits if t[1] < pos]
            return shown[-1] if shown else None

        check("edits target the displayed image id",
              all(info["id"] == shown_at(pos) for pos, info in edits),
              f"ids={[(pos, info['id']) for pos, info in edits[:4]]}")


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

# Drag-cadence sweep: snake through two full rows of bedroom's first
# obstacle (x 24..120, y 180..214 → cells cx 4..19, cy 30..35), all
# currently blocked and inside the walk bbox, so every report paints a
# fresh cell — 32 paint actions spread across the ~500ms gesture.
DRAG_SWEEP_CELLS = ([(cx, 32) for cx in range(4, 20)] +
                    [(cx, 33) for cx in reversed(range(4, 20))])


def drag_sweep_burst():
    """-> (press+drag bytes across DRAG_SWEEP_CELLS, release bytes)."""
    px, py = pixel_for_cell(*DRAG_SWEEP_CELLS[0])
    out = [f"\x1b[<0;{px};{py}M"]                       # left press
    for cell in DRAG_SWEEP_CELLS[1:]:
        px, py = pixel_for_cell(*cell)
        out.append(f"\x1b[<32;{px};{py}M")              # drag motion
    return "".join(out).encode(), f"\x1b[<0;{px};{py}m".encode()


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

    def __init__(self, env_extra=None, setup=None):
        self.tmp = tempfile.mkdtemp(prefix="walk_editor_test_")
        world_dir = os.path.join(self.tmp, "assets/world")
        os.makedirs(world_dir)
        self.world = os.path.join(world_dir, "world.json")
        shutil.copy(WORLD_SRC, self.world)
        with open(self.world, "rb") as handle:
            self.original = handle.read()
        if setup:
            setup(self.tmp)
        self.log = bytearray()
        self.lock = threading.Lock()
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, XPIX, YPIX))
        env = dict(os.environ,
                   KILIX_LAND_DESKTOP_ASSETS=self.tmp,
                   TERM="xterm-kitty")
        env.update(env_extra or {})
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


def wait_for_any(session, needles, timeout, start=0):
    """-> earliest offset of any needle at/after start, or -1 on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        snapshot = session.snapshot()
        hits = [i for i in (snapshot.find(needle, start)
                            for needle in needles) if i != -1]
        if hits:
            return min(hits)
        time.sleep(0.05)
    return -1


IMAGE_UPDATE = (APC_TRANSMIT, APC_EDIT)   # either proves a screen update


def send_burst(session, burst, fragmented):
    if fragmented:
        for i in range(0, len(burst), 3):
            session.send(burst[i:i + 3])
            time.sleep(0.010)
    else:
        session.send(burst)


def report(label, checks, log, verbose):
    """Print one scenario's verdict + failing (or all) checks. -> ok."""
    ok = all(c[1] for c in checks)
    print(f"[{'PASS' if ok else 'FAIL'}] scenario: {label}")
    for label_, good, detail in checks:
        if verbose or not good:
            extra = f"  -- {detail}" if detail and not good else ""
            print(f"    {'ok  ' if good else 'FAIL'} {label_}{extra}")
    if verbose or not ok:
        rooms = status_rooms(log)
        print(f"    evidence: a=T count={log.count(APC_TRANSMIT)}, "
              f"a=f chunk count={log.count(APC_EDIT)}, "
              f"status rooms seen={rooms}")
        if b"Traceback" in log:
            tail = log[log.find(b"Traceback"):][:600]
            print("    " + tail.decode(errors="replace"))
    return ok


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
            # must track (a=f image patches + hover status), the grid must
            # stay byte-identical (no paint/block notes, no dirty flag,
            # walkable count unchanged) — and painting must still work
            # afterwards (the shared paint phase below runs regardless).
            time.sleep(0.2)              # let startup repaints settle
            hover_mark = len(session.snapshot())
            pre_counts = re.findall(rb"walkable (\d+)/",
                                    session.snapshot()[:hover_mark])
            send_burst(session, hover_burst(), fragmented)
            hover_t = wait_for_any(session, IMAGE_UPDATE, 6,
                                   start=hover_mark)
            check("updates image on hover sweep (cursor tracks)",
                  hover_t != -1)
            last_cell = "cell {},{}".format(*HOVER_CELLS[-1]).encode()
            check("status reports final hover cell",
                  session.wait_for(last_cell, 6, start=hover_mark) != -1)
            time.sleep(0.3)              # trailing repaint + status settle
            hover_log = session.snapshot()[hover_mark:]
            check("hover sweep is a=f edits only (no full a=T)",
                  APC_EDIT in hover_log
                  and APC_TRANSMIT not in hover_log)
            wire = sum(match.end() - match.start()
                       for match in EDIT_TOKEN.finditer(hover_log))
            budget = 500_000 if fragmented else 100_000
            check(f"hover sweep wire under {budget // 1000}KB "
                  "(was ~1.5MB per cell crossed)",
                  0 < wire <= budget, f"a=f wire bytes={wire}")
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

        second_t = wait_for_any(session, IMAGE_UPDATE, 6, start=mark)
        check("updates image after painting (a=f edit or a=T)",
              second_t != -1)
        check("paint drag is patched via a=f edits",
              session.wait_for(APC_EDIT, 6, start=mark) != -1)

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

    check_presentation(log, check)
    check("no traceback", b"Traceback" not in log)
    session.cleanup()

    label = name + (" (3-byte fragments)" if fragmented else "")
    return report(label, checks, log, verbose)


def run_drag_scenario(verbose):
    """~500ms continuous fragmented paint drag: presents throttled to the
    drag cadence (<= duration/cadence + 2 during the gesture) and carried
    entirely by a=f edits, yet every swept cell ends up painted in the
    saved world — proof that frame encoding never starves input
    processing."""
    checks = []

    def check(label, ok, detail=""):
        checks.append((label, bool(ok), detail))

    session = Session()
    try:
        check("transmits kitty image (a=T)",
              session.wait_for(APC_TRANSMIT, 15) != -1)
        check("writes status line",
              session.wait_for(b"walk-editor  ", 5) != -1)
        time.sleep(0.3)                  # startup repaints settle
        burst, release = drag_sweep_burst()
        fragments = [burst[i:i + 3] for i in range(0, len(burst), 3)]
        pace = 0.5 / len(fragments)
        mark = len(session.snapshot())
        started = time.monotonic()
        for fragment in fragments:       # continuous fragmented gesture
            session.send(fragment)
            time.sleep(pace)
        duration = time.monotonic() - started
        during = session.snapshot()[mark:]
        presents = during.count(SYNC_H)
        cadence = W.DRAG_REPAINT_INTERVAL
        budget = int(duration / cadence) + 2
        check(f"drag presents throttled to {cadence * 1000:.0f}ms cadence "
              f"(<= {budget} in {duration * 1000:.0f}ms)",
              presents <= budget, f"saw {presents} presents")
        check("still presents during the drag", presents >= 1)
        check("drag gesture is a=f edits only (no mid-drag full a=T)",
              APC_EDIT in during and APC_TRANSMIT not in during)
        end_mark = mark + len(during)
        session.send(release)
        check("trailing repaint after release",
              wait_for_any(session, IMAGE_UPDATE, 3, start=end_mark) != -1)
        time.sleep(0.4)                  # let the input queue drain
        save_mark = len(session.snapshot())
        session.send(b"s")
        check("save reports 'validator OK'",
              session.wait_for(b"validator OK", 15, start=save_mark) != -1)
        session.send(b"q")
        exited = True
        try:
            session.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            exited = False
        check("quits on q", exited)
    finally:
        session.close()
        log = session.snapshot()

    try:
        with open(session.world, "rb") as handle:
            world = json.loads(handle.read())
        room = next(r for r in world["rooms"] if r["id"] == "bedroom")
        grid = W.rasterize(room)
        missing = [c for c in DRAG_SWEEP_CELLS
                   if not grid[c[1] * W.COLS + c[0]]]
    except (OSError, ValueError, StopIteration, KeyError) as error:
        missing = [f"unreadable: {error}"]
    check("every swept cell painted (input never starved)", not missing,
          f"{len(missing)} cells never painted: {missing[:6]}")

    check_presentation(log, check)
    check("no traceback", b"Traceback" not in log)
    session.cleanup()
    return report("drag-cadence (~500ms fragmented)", checks, log, verbose)


# Toggle/brush cells: (40, 38) is walkable in the shipped bedroom (in the
# walk band, outside both obstacles); (9, 31) is blocked (inside the bed
# obstacle) with its whole 3x3 neighborhood (8..10, 30..32) also blocked.
TOGGLE_CELL = (40, 38)
BRUSH_CENTER = (9, 31)
BRUSH_FOOTPRINT = [(cx, cy) for cy in (30, 31, 32) for cx in (8, 9, 10)]


def click_burst(cell):
    px, py = pixel_for_cell(*cell)
    return f"\x1b[<0;{px};{py}M\x1b[<0;{px};{py}m".encode()


def last_walkable(log):
    matches = re.findall(rb"walkable (\d+)/", log)
    return int(matches[-1]) if matches else -1


def wait_for_walkable(session, value, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if last_walkable(session.snapshot()) == value:
            return True
        time.sleep(0.05)
    return False


def run_toggle_brush_scenario(verbose):
    """Left click TOGGLES the tile under the cursor; [/] size the brush and
    a click paints the whole footprint with the toggled value."""
    checks = []

    def check(label, ok, detail=""):
        checks.append((label, bool(ok), detail))

    session = Session()
    try:
        check("transmits kitty image (a=T)",
              session.wait_for(APC_TRANSMIT, 15) != -1)
        check("writes status line",
              session.wait_for(b"walk-editor  ", 5) != -1)
        time.sleep(0.3)
        count0 = last_walkable(session.snapshot())
        check("initial walkable count visible", count0 > 0)
        session.send(click_burst(TOGGLE_CELL))
        check("click on a walkable tile clears it (toggle off)",
              wait_for_walkable(session, count0 - 1, 5),
              f"count stayed {last_walkable(session.snapshot())}")
        session.send(click_burst(TOGGLE_CELL))
        check("second click restores it (toggle on)",
              wait_for_walkable(session, count0, 5),
              f"count stayed {last_walkable(session.snapshot())}")
        mark = len(session.snapshot())
        session.send(b"]]")
        check("]] grows the brush to 3x3",
              session.wait_for(b"brush 3x3", 5, start=mark) != -1)
        session.send(click_burst(BRUSH_CENTER))
        check("3x3 click paints the full footprint (+9 walkable)",
              wait_for_walkable(session, count0 + 9, 5),
              f"count is {last_walkable(session.snapshot())}, "
              f"wanted {count0 + 9}")
        save_mark = len(session.snapshot())
        session.send(b"s")
        check("save reports 'validator OK'",
              session.wait_for(b"validator OK", 15, start=save_mark) != -1)
        session.send(b"q")
        exited = True
        try:
            session.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            exited = False
        check("quits on q", exited)
    finally:
        session.close()
        log = session.snapshot()

    try:
        with open(session.world, "rb") as handle:
            world = json.loads(handle.read())
        room = next(r for r in world["rooms"] if r["id"] == "bedroom")
        grid = W.rasterize(room)
        missing = [c for c in BRUSH_FOOTPRINT
                   if not grid[c[1] * W.COLS + c[0]]]
        toggled = grid[TOGGLE_CELL[1] * W.COLS + TOGGLE_CELL[0]]
    except (OSError, ValueError, StopIteration, KeyError) as error:
        missing = [f"unreadable: {error}"]
        toggled = 0
    check("saved world has the 3x3 footprint walkable", not missing,
          f"missing {missing}")
    check("saved world kept the double-toggled tile walkable", toggled == 1)

    check_presentation(log, check)
    check("no traceback", b"Traceback" not in log)
    session.cleanup()
    return report("toggle+brush", checks, log, verbose)


def run_coherence_scenario(verbose):
    """Screen-vs-cache coherence: with KILIX_WALK_EDITOR_DEBUG_CHECKSUM=1
    every edit-mode present byte-compares the incrementally patched scene
    cache against a fresh reference compose_scene and reports
    coherence=ok or coherence=DRIFT in an APC the harness greps — so any
    drift introduced by the a=f patching is caught, not painted over.
    Also proves whole-scene changes still take the full-frame
    double-buffer path: grid toggles ('g' twice) and a room switch ('n')
    must each produce an a=T (check_presentation then holds their
    alternating ids to the delete-after-place contract), and edits after
    those full presents must still be coherent."""
    checks = []

    def check(label, ok, detail=""):
        checks.append((label, bool(ok), detail))

    session = Session(env_extra={"KILIX_WALK_EDITOR_DEBUG_CHECKSUM": "1"})
    try:
        check("transmits kitty image (a=T)",
              session.wait_for(APC_TRANSMIT, 15) != -1)
        check("writes status line",
              session.wait_for(b"walk-editor  ", 5) != -1)
        time.sleep(0.3)                  # startup repaints settle
        sweep_mark = len(session.snapshot())
        send_burst(session, hover_burst(), False)
        check("hover sweep patches via a=f",
              session.wait_for(APC_EDIT, 6, start=sweep_mark) != -1)
        paint_mark = len(session.snapshot())
        send_burst(session, mouse_burst(), False)
        check("paint drag patches via a=f",
              session.wait_for(APC_EDIT, 6, start=paint_mark) != -1)
        time.sleep(0.5)                  # trailing flush + audits settle
        edit_log = session.snapshot()[sweep_mark:]
        check("edit presents ran the coherence audit",
              b"coherence=ok" in edit_log)
        check("paint+hover edits kept cache == reference compose",
              b"coherence=DRIFT" not in edit_log)
        check("no full a=T during the paint+hover edit sequence",
              APC_TRANSMIT not in edit_log)
        toggle_mark = len(session.snapshot())
        session.send(b"g")
        check("grid toggle off takes the full-frame a=T path",
              session.wait_for(APC_TRANSMIT, 5, start=toggle_mark) != -1)
        time.sleep(0.2)
        toggle2_mark = len(session.snapshot())
        session.send(b"g")
        check("grid toggle back on takes the full-frame a=T path too",
              session.wait_for(APC_TRANSMIT, 5, start=toggle2_mark) != -1)
        time.sleep(0.2)
        rehover_mark = len(session.snapshot())
        send_burst(session, hover_burst(), False)
        check("edits resume after the full presents",
              session.wait_for(APC_EDIT, 6, start=rehover_mark) != -1)
        time.sleep(0.4)
        rehover_log = session.snapshot()[rehover_mark:]
        check("post-toggle edits still coherent (audit ok, no drift)",
              b"coherence=ok" in rehover_log
              and b"coherence=DRIFT" not in rehover_log)
        switch_mark = len(session.snapshot())
        session.send(b"n")
        check("room switch takes the full-frame a=T path",
              session.wait_for(APC_TRANSMIT, 5, start=switch_mark) != -1)
        check("status line shows the switched room",
              session.wait_for(b"walk-editor  ", 5,
                               start=switch_mark) != -1)
        session.send(b"q")               # room switch cleared the dirty flag
        exited = True
        try:
            session.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            exited = False
        check("quits on q", exited)
    finally:
        session.close()
        log = session.snapshot()

    rooms = status_rooms(log)
    check("room actually switched before quitting",
          len(rooms) >= 2 and rooms[-1] != "bedroom", f"rooms={rooms}")
    check("no cache drift anywhere in the session",
          b"coherence=DRIFT" not in log)
    check_presentation(log, check)
    check("no traceback", b"Traceback" not in log)
    session.cleanup()
    return report("edit-coherence (checksum audit)", checks, log, verbose)


# Walk-behind authoring scenario: a synthetic bedroom plate with one
# distinct-colored blob, written into the temp asset tree before spawn.
BLOB_RECT = (400, 300, 560, 420)          # plate px, exclusive
BLOB_COLOR = (196, 64, 60)
PLATE_BG = (44, 46, 54)
BLOB_AREA = ((BLOB_RECT[2] - BLOB_RECT[0])
             * (BLOB_RECT[3] - BLOB_RECT[1]))
WAND_CELL = (30, 22)                      # cell center -> plate (488, 360)
BASELINE_CELL = (30, 33)                  # hover row -> baseline y ~201
BASELINE_Y = 201
# 1x1-brush erase drag across these cells zeroes this plate strip (each
# cell is 16 plate px), fully inside the blob.
ERASE_CELLS = [(28, 22), (29, 22), (30, 22), (31, 22), (32, 22)]
ERASE_PLATE_RECT = (448, 352, 528, 368)
ERASE_AREA = ((ERASE_PLATE_RECT[2] - ERASE_PLATE_RECT[0])
              * (ERASE_PLATE_RECT[3] - ERASE_PLATE_RECT[1]))


def make_synthetic_plate(tree):
    """Write the wandable synthetic legend/bedroom plate into `tree`."""
    from PIL import Image
    room_dir = os.path.join(tree, "assets/graphics/rooms/legend")
    os.makedirs(room_dir, exist_ok=True)
    plate = Image.new("RGB", (W.PLATE_W, W.PLATE_H), PLATE_BG)
    blob = Image.new("RGB", (BLOB_RECT[2] - BLOB_RECT[0],
                             BLOB_RECT[3] - BLOB_RECT[1]), BLOB_COLOR)
    plate.paste(blob, BLOB_RECT[:2])
    plate.save(os.path.join(room_dir, "bedroom.png"))


def right_drag_burst(cells):
    """Right press + drags + release exactly as kitty emits them."""
    px, py = pixel_for_cell(*cells[0])
    out = [f"\x1b[<2;{px};{py}M"]
    for cell in cells[1:]:
        px, py = pixel_for_cell(*cell)
        out.append(f"\x1b[<34;{px};{py}M")
    out.append(f"\x1b[<2;{px};{py}m")
    return "".join(out).encode()


def hover_report(cell):
    px, py = pixel_for_cell(*cell)
    return f"\x1b[<35;{px};{py}M".encode()


def read_mask(session):
    """-> (mode, size, flat pixel list) of the saved legend bedroom mask,
    or (None, None, None) when the file does not exist."""
    from PIL import Image
    path = os.path.join(session.tmp,
                        "assets/graphics/rooms/legend/bedroom-behind.png")
    if not os.path.exists(path):
        return None, None, None
    with Image.open(path) as image:
        return image.mode, image.size, list(image.getdata())


def bedroom_walkbehinds(session):
    with open(session.world, "rb") as handle:
        world = json.loads(handle.read())
    room = next(r for r in world["rooms"] if r["id"] == "bedroom")
    return room.get("walkbehinds")


def run_behind_scenario(verbose):
    """Walk-behind authoring: 'o' enters mask mode, a magic-wand click
    floods the synthetic blob into region 1, 'b' sets the baseline at
    the hovered y, 's' writes the mask PNG + world.json declaration
    (validator green); a right-drag erases a strip, 'x' clears the rest,
    and the empty mask + dropped declaration save cleanly too."""
    checks = []

    def check(label, ok, detail=""):
        checks.append((label, bool(ok), detail))

    session = Session(setup=make_synthetic_plate)
    try:
        check("transmits kitty image (a=T)",
              session.wait_for(APC_TRANSMIT, 15) != -1)
        check("writes status line",
              session.wait_for(b"walk-editor  ", 5) != -1)
        time.sleep(0.3)                  # startup repaints settle
        mode_mark = len(session.snapshot())
        session.send(b"o")
        check("'o' enters behind mode (status shows the region id)",
              session.wait_for(b"behind id 1", 5, start=mode_mark) != -1)
        time.sleep(0.2)
        wand_mark = len(session.snapshot())
        session.send(click_burst(WAND_CELL))
        check("magic wand floods exactly the blob into region 1",
              session.wait_for(f"wand +{BLOB_AREA}px id 1".encode(), 6,
                               start=wand_mark) != -1)
        check("wand updates the image",
              wait_for_any(session, IMAGE_UPDATE, 6, start=wand_mark)
              != -1)
        session.send(hover_report(BASELINE_CELL))
        check("hover reaches the baseline row",
              session.wait_for(b"cell 30,33", 5, start=wand_mark) != -1)
        session.send(b"b")
        check("'b' sets the selected region's baseline",
              session.wait_for(b"baseline id 1 = ", 5,
                               start=wand_mark) != -1)
        time.sleep(0.2)
        save1 = len(session.snapshot())
        session.send(b"s")
        check("save 1 reports 'validator OK'",
              session.wait_for(b"validator OK", 15, start=save1) != -1)

        mask_mode, size, data = read_mask(session)
        check("mask PNG written plate-sized 8-bit grayscale",
              mask_mode == "L" and size == (W.PLATE_W, W.PLATE_H),
              f"mode={mask_mode} size={size}")
        blob_ok = (data is not None and set(data) == {0, 1}
                   and data.count(1) == BLOB_AREA
                   and data[BLOB_RECT[1] * W.PLATE_W + BLOB_RECT[0]] == 1
                   and data[(BLOB_RECT[3] - 1) * W.PLATE_W
                            + BLOB_RECT[2] - 1] == 1
                   and data[BLOB_RECT[1] * W.PLATE_W
                            + BLOB_RECT[0] - 1] == 0
                   and data[(BLOB_RECT[1] - 1) * W.PLATE_W
                            + BLOB_RECT[0]] == 0)
        check("mask contains exactly the blob's pixels as region 1",
              blob_ok,
              "" if data is None else
              f"values={sorted(set(data))} count1={data.count(1)}")
        declared = bedroom_walkbehinds(session)
        check("world.json declares id 1 with the authored baseline",
              isinstance(declared, list) and len(declared) == 1
              and declared[0].get("id") == 1
              and abs(declared[0].get("baseline", -99)
                      - BASELINE_Y) <= 1,
              f"walkbehinds={declared}")

        erase_mark = len(session.snapshot())
        session.send(right_drag_burst(ERASE_CELLS))
        remaining = BLOB_AREA - ERASE_AREA
        check("right-drag erase drops the region px count in the status",
              session.wait_for(f"({remaining}px)".encode(), 5,
                               start=erase_mark) != -1)
        time.sleep(0.4)                  # trailing flush + queue drain
        save2 = len(session.snapshot())
        session.send(b"s")
        check("save 2 reports 'validator OK'",
              session.wait_for(b"validator OK", 15, start=save2) != -1)
        _, _, data = read_mask(session)
        strip_ok = (data is not None and all(
            data[y * W.PLATE_W + x] == 0
            for y in range(ERASE_PLATE_RECT[1], ERASE_PLATE_RECT[3])
            for x in range(ERASE_PLATE_RECT[0], ERASE_PLATE_RECT[2])))
        check("erased strip is zero in the saved mask", strip_ok)
        check("unerased blob pixels survive as region 1",
              data is not None
              and data.count(1) == BLOB_AREA - ERASE_AREA
              and data[305 * W.PLATE_W + 405] == 1,
              "" if data is None else f"count1={data.count(1)}")
        declared = bedroom_walkbehinds(session)
        check("declaration kept while mask pixels remain",
              isinstance(declared, list)
              and [wb["id"] for wb in declared] == [1],
              f"walkbehinds={declared}")

        clear_mark = len(session.snapshot())
        session.send(b"x")
        check("'x' clears the selected region",
              session.wait_for(b"cleared id 1", 5, start=clear_mark)
              != -1)
        save3 = len(session.snapshot())
        session.send(b"s")
        check("save 3 (empty mask) reports 'validator OK'",
              session.wait_for(b"validator OK", 15, start=save3) != -1)
        _, _, data = read_mask(session)
        check("empty mask saved as all-zero",
              data is not None and set(data) == {0},
              "" if data is None else f"values={sorted(set(data))}")
        check("empty mask drops the world.json declaration",
              bedroom_walkbehinds(session) in (None, []),
              f"walkbehinds={bedroom_walkbehinds(session)}")
        session.send(b"q")
        exited = True
        try:
            session.child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            exited = False
        check("quits on q", exited)
    finally:
        session.close()
        log = session.snapshot()

    verdict = subprocess.run([sys.executable, VALIDATOR, session.world],
                             capture_output=True, text=True)
    check("final world passes validate_world.py",
          verdict.returncode == 0, verdict.stderr.strip()[-120:])
    check_presentation(log, check)
    check("no traceback", b"Traceback" not in log)
    session.cleanup()
    return report("behind-authoring (magic wand)", checks, log, verbose)


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
    ok &= run_drag_scenario(verbose)
    ok &= run_toggle_brush_scenario(verbose)
    ok &= run_coherence_scenario(verbose)
    ok &= run_behind_scenario(verbose)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
