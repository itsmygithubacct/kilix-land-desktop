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

Interactive mode wears the shared Kilix text shell (the real ``kilix_tui``
core imported from the kilix-tui-utils checkout). `--check` validates the
bindings file with nothing but the stdlib and is wired into `make test`.
"""

import json
import os
import re
import secrets
import shutil
import stat
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KINDS = ("default", "app", "folder")
ID_CAPACITY = 24
LAUNCH_BINDING_CAPACITY = 256
LAUNCH_COMMAND_MAX = 8
BINDINGS_FILE_MAX = 64 * 1024
ID_PATTERN = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
TARGET_LABELS = {
    "terminal": "Terminal", "coding-agents": "Coding agents",
    "files": "File manager", "manuals": "Manuals", "models": "Models",
    "games": "Games", "music": "Music player", "voice": "Voice",
    "trash": "Trash", "mailbox": "Mailbox", "maintenance": "Configuration",
    "wardrobe": "Wardrobe", "bed": "Bed", "status-board": "Status board",
    "gate-locked": "Locked gate",
}


class BindingFileError(ValueError):
    """A binding store failed the user-owned configuration contract."""


def config_home():
    override = os.environ.get("KILIX_LAND_DESKTOP_CONFIG_HOME")
    # Match the C launcher: relative overrides are ignored, so the editor and
    # runtime can never silently read different binding files.
    if override and os.path.isabs(override):
        return override
    home = os.environ.get("HOME")
    if not home or not os.path.isabs(home):
        raise BindingFileError("HOME must be an absolute path")
    return os.path.join(home, ".local/gpu_terminal/kilix-land-desktop")


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


def validate_key(key):
    """Return a binding-key problem or None."""
    if not isinstance(key, str):
        return "key must be text"
    parts = key.split(".")
    if len(parts) != 2 or not all(parts):
        return "key must be '<room>.<object>'"
    for label, value in zip(("room", "object"), parts):
        if not ID_PATTERN.fullmatch(value):
            return f"{label} id must use lowercase letters, digits, or '-'"
        if len(value.encode("utf-8")) >= ID_CAPACITY:
            return f"{label} id is longer than {ID_CAPACITY - 1} bytes"
    return None


def _binding_shape_error(kind, value):
    if kind not in ("app", "folder"):
        return "kind must be 'app' or 'folder'"
    if not isinstance(value, str) or not value:
        return "missing value"
    if value != value.strip(" \t"):
        return "value has leading or trailing whitespace"
    for character in value:
        codepoint = ord(character)
        if codepoint == 0 or codepoint == 127 or (
                codepoint < 32 and not (kind == "app" and character == "\t")):
            return "value contains a control character"
        if character.isspace() and character not in (" ", "\t"):
            return "value contains unsupported whitespace"
    if len(value.encode("utf-8")) >= LAUNCH_BINDING_CAPACITY:
        return (
            "value is longer than "
            f"{LAUNCH_BINDING_CAPACITY - 1} bytes"
        )
    if kind == "app":
        arguments = [part for part in re.split(r"[ \t]+", value) if part]
        if len(arguments) > LAUNCH_COMMAND_MAX:
            return f"app command has more than {LAUNCH_COMMAND_MAX} arguments"
    return None


def parse_bindings(text):
    """-> (bindings dict key->(kind, value), [error strings])"""
    bindings = {}
    errors = []
    for number, raw in enumerate(text.split("\n"), 1):
        if "\r" in raw or "\0" in raw:
            errors.append(f"line {number}: contains a control character")
            continue
        line = raw.lstrip(" \t")
        if not line or not line.strip(" \t") or line.startswith("#"):
            continue
        if "=" not in line:
            errors.append(f"line {number}: missing '='")
            continue
        key, _, value = line.partition("=")
        key = key.strip(" \t")
        problem = validate_key(key)
        if problem:
            errors.append(f"line {number}: bad key '{key}': {problem}")
            continue
        if key in bindings:
            errors.append(f"line {number}: duplicate key '{key}'")
            continue
        entry = value.lstrip(" \t")
        if entry.startswith("app "):
            kind, binding_value = "app", entry[4:]
        elif entry.startswith("folder "):
            kind, binding_value = "folder", entry[7:]
        else:
            errors.append(f"line {number}: kind must be 'app' or 'folder'")
            continue
        problem = _binding_shape_error(kind, binding_value)
        if problem:
            errors.append(f"line {number}: {problem}")
            continue
        bindings[key] = (kind, binding_value)
    return bindings, errors


def _open_config_directory(create):
    directory = config_home()
    if directory == os.path.sep:
        raise BindingFileError("configuration home cannot be '/'")
    if create:
        try:
            os.makedirs(directory, mode=0o700, exist_ok=True)
        except OSError as error:
            raise BindingFileError(
                f"cannot create configuration directory: {error.strerror}"
            ) from error
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW
    try:
        descriptor = os.open(directory, flags)
    except FileNotFoundError:
        raise
    except OSError as error:
        raise BindingFileError(
            f"cannot securely open configuration directory: {error.strerror}"
        ) from error
    try:
        status = os.fstat(descriptor)
        if not stat.S_ISDIR(status.st_mode):
            raise BindingFileError("configuration home is not a directory")
        if status.st_uid != os.geteuid():
            raise BindingFileError(
                "configuration directory is not owned by the current user"
            )
        if create:
            os.fchmod(descriptor, 0o700)
        elif status.st_mode & 0o077:
            raise BindingFileError(
                "configuration directory permissions must be 0700"
            )
        return descriptor
    except Exception:
        os.close(descriptor)
        raise


def _read_bindings_text():
    try:
        directory = _open_config_directory(False)
    except FileNotFoundError:
        return None
    try:
        flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
        try:
            descriptor = os.open("bindings.conf", flags, dir_fd=directory)
        except FileNotFoundError:
            return None
        except OSError as error:
            raise BindingFileError(
                f"cannot securely open bindings.conf: {error.strerror}"
            ) from error
        try:
            status = os.fstat(descriptor)
            if not stat.S_ISREG(status.st_mode):
                raise BindingFileError("bindings.conf is not a regular file")
            if status.st_uid != os.geteuid():
                raise BindingFileError(
                    "bindings.conf is not owned by the current user"
                )
            if status.st_mode & 0o022:
                raise BindingFileError(
                    "bindings.conf must not be group/world writable"
                )
            chunks = []
            remaining = BINDINGS_FILE_MAX + 1
            while remaining:
                chunk = os.read(descriptor, min(16 * 1024, remaining))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            contents = b"".join(chunks)
            if len(contents) > BINDINGS_FILE_MAX:
                raise BindingFileError(
                    f"bindings.conf exceeds {BINDINGS_FILE_MAX} bytes"
                )
            if b"\0" in contents:
                raise BindingFileError("bindings.conf contains a NUL byte")
            try:
                return contents.decode("utf-8")
            except UnicodeDecodeError as error:
                raise BindingFileError(
                    "bindings.conf is not valid UTF-8"
                ) from error
        finally:
            os.close(descriptor)
    finally:
        os.close(directory)


def load_bindings():
    try:
        text = _read_bindings_text()
        return parse_bindings(text)[0] if text is not None else {}
    except (BindingFileError, OSError):
        return {}


def _write_all(descriptor, contents):
    offset = 0
    while offset < len(contents):
        written = os.write(descriptor, contents[offset:])
        if written <= 0:
            raise OSError("short write to bindings.conf")
        offset += written


def save_bindings(bindings):
    lines = [
        "# kilix-land-desktop object bindings — edited by tools/land_config.py",
        "# <room>.<object> = app <command...> | folder </absolute/path>",
    ]
    for key in sorted(bindings):
        kind, value = bindings[key]
        key_problem = validate_key(key)
        value_problem = _binding_shape_error(kind, value)
        if key_problem:
            raise BindingFileError(f"bad key '{key}': {key_problem}")
        if value_problem:
            raise BindingFileError(f"{key}: {value_problem}")
        lines.append(f"{key} = {kind} {value}")
    contents = ("\n".join(lines) + "\n").encode("utf-8")
    if len(contents) > BINDINGS_FILE_MAX:
        raise BindingFileError(
            f"serialized bindings exceed {BINDINGS_FILE_MAX} bytes"
        )
    directory = _open_config_directory(True)
    descriptor = -1
    temporary = ""
    try:
        for _ in range(16):
            temporary = (
                f".bindings.conf.tmp.{os.getpid()}."
                f"{secrets.token_hex(6)}"
            )
            try:
                descriptor = os.open(
                    temporary,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                    os.O_CLOEXEC | os.O_NOFOLLOW,
                    0o600,
                    dir_fd=directory,
                )
                break
            except FileExistsError:
                continue
        if descriptor < 0:
            raise BindingFileError("cannot allocate a binding temporary file")
        try:
            _write_all(descriptor, contents)
            os.fchmod(descriptor, 0o600)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
            descriptor = -1
        os.replace(
            temporary,
            "bindings.conf",
            src_dir_fd=directory,
            dst_dir_fd=directory,
        )
        temporary = ""
        os.fsync(directory)
    except OSError as error:
        raise BindingFileError(
            f"cannot save bindings.conf: {error.strerror or error}"
        ) from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary:
            try:
                os.unlink(temporary, dir_fd=directory)
            except FileNotFoundError:
                pass
        os.close(directory)


def validate_binding(kind, value):
    """-> error string or None."""
    problem = _binding_shape_error(kind, value)
    if problem:
        return problem
    if kind == "folder":
        if not os.path.isabs(value):
            return "folder must be an absolute path"
        if not os.path.isdir(value):
            return "folder does not exist"
        return None
    program = re.split(r"[ \t]+", value, maxsplit=1)[0]
    if "/" in program and not os.path.isabs(program):
        return "program path must be absolute or found on PATH"
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
        text = _read_bindings_text()
    except (BindingFileError, OSError) as error:
        print(f"bindings: {error}", file=sys.stderr)
        return 1
    if text is None:
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
                     "kilix-desktops", "kilix-tui-utils"),
        os.path.expanduser(
            "~/.local/gpu_terminal/sources/kilix-desktops/kilix-tui-utils"),
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
    from kilix_tui import app, keys as keymap, shell

    state = State()

    def render(surface, s):
        footer = {
            "list": "↑/↓ object · ←/→ or digits room · Enter edit · q quit",
            "kind": "←/→ choose · Enter next · Esc cancel",
            "value": "type value · Enter save · Esc cancel",
        }[s.mode]
        room_id, room_name, objects = s.rooms[s.section]
        summary = f"{room_name} · {len(objects)} objects"
        if s.message:
            summary += f" · {s.message}"
        body = shell.draw(
            surface,
            title="Kilix Land · Config",
            sections=[name for _, name, _ in s.rooms],
            active=s.section,
            summary=summary,
            footer=footer,
            summary_role="accent" if s.message else "muted",
        )
        if body.height < 3 or body.width < 20:
            return
        row = body.top
        for index, (object_id, target, prompt) in enumerate(objects):
            if row >= body.bottom - 2:
                break
            marker = "▶" if (index == s.selected and s.mode == "list") else " "
            line = (f"{marker} {object_id:<14.14} "
                    f"{s.describe(room_id, object_id, target)}")
            shell.put(
                surface,
                row,
                body.left,
                line.ljust(body.width)[:body.width],
                shell.tango.attr("selected")
                if index == s.selected and s.mode == "list" else 0,
            )
            row += 1
        if s.mode == "kind":
            row = body.bottom - 2
            choices = "   ".join(
                (f"[{kind.upper()}]" if i == s.kind_cursor else f" {kind} ")
                for i, kind in enumerate(KINDS))
            shell.put(
                surface,
                row,
                body.left,
                f"bind {s.key()}: {choices}"[:body.width],
                shell.tango.attr("accent"),
            )
        elif s.mode == "value":
            row = body.bottom - 2
            kind = KINDS[s.kind_cursor]
            shell.put(
                surface,
                row,
                body.left,
                f"{s.key()} = {kind} {s.value}_"[:body.width],
                shell.tango.attr("accent"),
            )

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
