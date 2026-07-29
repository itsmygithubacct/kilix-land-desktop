#!/usr/bin/env python3
"""kilix-land-desktop configuration — bind room objects to apps or folders.

The desktop's launcher registry gives every object a sensible default; this
tool records per-object overrides so the TV can open your video folder or the
dev rig can start a specific program. Overrides live in
`<config-home>/bindings.conf`, one per line:

    <room>.<object> = app <command and arguments>
    <room>.<object> = folder </absolute/path>

`app` values are split on whitespace into an argv — no shell, no quoting, no
expansion. `folder` values open in the file manager (kilix-file first). An
absent line means the built-in default. The desktop re-reads the file on
every activation, so edits here apply immediately.

Interactive mode wears the kilix-tui desktop's chrome (the real kilix_tui
core imported from the kilix-tui-utils checkout). `--check` validates the
bindings file with nothing but the stdlib and is wired into `make test`.
"""

import json
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KINDS = ("default", "app", "folder")
TARGET_LABELS = {
    "terminal": "Terminal", "coding-agents": "Coding agents",
    "files": "File manager", "manuals": "Manuals", "models": "Models",
    "games": "Games", "music": "Music player", "voice": "Voice",
    "trash": "Trash", "mailbox": "Mailbox", "maintenance": "Configuration",
    "wardrobe": "Wardrobe", "bed": "Bed", "status-board": "Status board",
    "gate-locked": "Locked gate",
}


def config_home():
    override = os.environ.get("KILIX_LAND_DESKTOP_CONFIG_HOME")
    if override:
        return override
    return os.path.join(os.path.expanduser("~"),
                        ".local/gpu_terminal/kilix-land-desktop")


def bindings_path():
    return os.path.join(config_home(), "bindings.conf")


def load_world():
    """[(room_id, room_name, [(object_id, target, prompt), ...]), ...]"""
    root = os.environ.get("KILIX_LAND_DESKTOP_ASSETS") or REPO
    path = os.path.join(root, "assets/world/world.json")
    with open(path, encoding="utf-8") as handle:
        world = json.load(handle)
    rooms = []
    for room in world["rooms"]:
        objects = [(o["id"], o["target"], o["prompt"])
                   for o in room.get("objects", [])]
        rooms.append((room["id"], room["name"], objects))
    return rooms


def parse_bindings(text):
    """-> (bindings dict key->(kind, value), [error strings])"""
    bindings = {}
    errors = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            errors.append(f"line {number}: missing '='")
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        parts = value.strip().split(None, 1)
        if "." not in key or not key.replace(".", "").replace("-", ""):
            errors.append(f"line {number}: bad key '{key}'")
            continue
        if not parts or parts[0] not in ("app", "folder"):
            errors.append(f"line {number}: kind must be 'app' or 'folder'")
            continue
        if len(parts) < 2 or not parts[1].strip():
            errors.append(f"line {number}: missing value")
            continue
        bindings[key] = (parts[0], parts[1].strip())
    return bindings, errors


def load_bindings():
    try:
        with open(bindings_path(), encoding="utf-8") as handle:
            return parse_bindings(handle.read())[0]
    except OSError:
        return {}


def save_bindings(bindings):
    directory = config_home()
    os.makedirs(directory, mode=0o700, exist_ok=True)
    lines = [
        "# kilix-land-desktop object bindings — edited by tools/land_config.py",
        "# <room>.<object> = app <command...> | folder </absolute/path>",
    ]
    for key in sorted(bindings):
        kind, value = bindings[key]
        lines.append(f"{key} = {kind} {value}")
    temporary = bindings_path() + ".tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    os.replace(temporary, bindings_path())


def validate_binding(kind, value):
    """-> error string or None."""
    if kind == "folder":
        if not value.startswith("/"):
            return "folder must be an absolute path"
        if not os.path.isdir(value):
            return "folder does not exist"
        return None
    program = value.split()[0]
    if program.startswith("/"):
        if not (os.path.isfile(program) and os.access(program, os.X_OK)):
            return "program is not an executable file"
    elif shutil.which(program) is None:
        return "program not found on PATH"
    return None


def check():
    """--check: validate bindings.conf against the world. Exit status."""
    rooms = load_world()
    known = {f"{room_id}.{object_id}"
             for room_id, _, objects in rooms
             for object_id, _, _ in objects}
    try:
        with open(bindings_path(), encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        print("bindings: none configured (defaults everywhere)")
        return 0
    bindings, errors = parse_bindings(text)
    for key, (kind, value) in sorted(bindings.items()):
        if key not in known:
            errors.append(f"{key}: no such room object")
        problem = validate_binding(kind, value)
        if problem:
            errors.append(f"{key}: {problem}")
    for error in errors:
        print(f"bindings: {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"bindings: OK ({len(bindings)} override(s))")
    return 0


def locate_tui_core():
    for candidate in (
        os.environ.get("KILIX_TUI_UTILS_DIR"),
        os.path.join(os.environ.get("GPU_TERMINAL_SOURCE_HOME", ""),
                     "kilix-tui-utils"),
        os.path.expanduser("~/.local/gpu_terminal/sources/kilix-tui-utils"),
    ):
        if candidate and os.path.isdir(os.path.join(candidate, "src",
                                                    "kilix_tui")):
            return os.path.join(candidate, "src")
    return None


class State:
    def __init__(self):
        self.rooms = load_world()
        self.bindings = load_bindings()
        self.section = 0
        self.selected = 0
        self.mode = "list"          # list | kind | value
        self.kind_cursor = 0
        self.value = ""
        self.message = ""

    def objects(self):
        return self.rooms[self.section][2]

    def key(self):
        room_id = self.rooms[self.section][0]
        object_id = self.objects()[self.selected][0]
        return f"{room_id}.{object_id}"

    def describe(self, room_id, object_id, target):
        binding = self.bindings.get(f"{room_id}.{object_id}")
        if not binding:
            return f"default ({TARGET_LABELS.get(target, target)})"
        kind, value = binding
        return f"{kind}: {value}"


def run_interactive():
    core = locate_tui_core()
    if core is None:
        print("Kilix TUI utils is not installed; only --check is available.")
        try:
            input("Press Enter to close...")
        except EOFError:
            pass
        return 1
    sys.path.insert(0, core)
    import curses  # noqa: F401  (kilix_tui.app owns the wrapper)
    from kilix_tui import app, chrome, keys as keymap

    state = State()
    page = chrome.Page("KILIX LAND CONFIG",
                       [name for _, name, _ in state.rooms],
                       node="land")

    def render(surface, s):
        footer = {
            "list": "↑/↓ object · ←/→ or digits room · Enter edit · q quit",
            "kind": "←/→ choose · Enter next · Esc cancel",
            "value": "type value · Enter save · Esc cancel",
        }[s.mode]
        page.render(surface, s.section, footer=footer, status=s.message)
        top, left, height, width = page.content_box()
        if height < 3 or width < 20:
            return
        room_id, room_name, objects = s.rooms[s.section]
        row = top
        surface.addstr(row, left, f"{room_name} objects"[: width - 1])
        row += 2
        for index, (object_id, target, prompt) in enumerate(objects):
            if row >= top + height - 3:
                break
            marker = "▶" if (index == s.selected and s.mode == "list") else " "
            line = (f"{marker} {object_id:<14.14} "
                    f"{s.describe(room_id, object_id, target)}")
            surface.addstr(row, left, line[: width - 1])
            row += 1
        if s.mode == "kind":
            row = top + height - 3
            choices = "   ".join(
                (f"[{kind.upper()}]" if i == s.kind_cursor else f" {kind} ")
                for i, kind in enumerate(KINDS))
            surface.addstr(row, left, f"bind {s.key()}: {choices}"
                           [: width - 1])
        elif s.mode == "value":
            row = top + height - 3
            kind = KINDS[s.kind_cursor]
            surface.addstr(row, left,
                           f"{s.key()} = {kind} {s.value}_"[: width - 1])

    def commit(s):
        kind = KINDS[s.kind_cursor]
        problem = validate_binding(kind, s.value.strip())
        if problem:
            s.message = problem
            return
        s.bindings[s.key()] = (kind, s.value.strip())
        save_bindings(s.bindings)
        s.message = f"saved {s.key()}"
        s.mode = "list"

    def handle(key, s):
        s.message = ""
        if s.mode == "list":
            if keymap.is_quit(key):
                return False
            if step := keymap.direction(key):
                if s.objects():
                    s.selected = max(0, min(len(s.objects()) - 1,
                                            s.selected + step))
            elif key in keymap.LEFT or key == 9:
                s.section = (s.section - 1) % len(s.rooms)
                s.selected = 0
            elif key in keymap.RIGHT:
                s.section = (s.section + 1) % len(s.rooms)
                s.selected = 0
            elif ord("1") <= key <= ord("0") + len(s.rooms):
                s.section = key - ord("1")
                s.selected = 0
            elif key in keymap.SELECT and s.objects():
                s.mode = "kind"
                s.kind_cursor = 0
            return True
        if s.mode == "kind":
            if key == 27:
                s.mode = "list"
            elif key in keymap.LEFT:
                s.kind_cursor = (s.kind_cursor - 1) % len(KINDS)
            elif key in keymap.RIGHT:
                s.kind_cursor = (s.kind_cursor + 1) % len(KINDS)
            elif key in keymap.SELECT:
                if KINDS[s.kind_cursor] == "default":
                    s.bindings.pop(s.key(), None)
                    save_bindings(s.bindings)
                    s.message = f"{s.key()} back to default"
                    s.mode = "list"
                else:
                    existing = s.bindings.get(s.key())
                    s.value = existing[1] if existing else ""
                    s.mode = "value"
            return True
        # value entry
        if key == 27:
            s.mode = "list"
        elif key in (ord("\n"), ord("\r")):
            commit(s)
        elif key in (263, 127, 8):
            s.value = s.value[:-1]
        elif 32 <= key <= 126:
            s.value += chr(key)
        return True

    if path := app.screenshot_argv(sys.argv[1:]):
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(app.render_to_text(render, state) + "\n")
        return 0
    return app.run(render, state, handle=handle)


def main():
    if "--check" in sys.argv[1:]:
        return check()
    return run_interactive()


if __name__ == "__main__":
    raise SystemExit(main())
