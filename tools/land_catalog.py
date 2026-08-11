#!/usr/bin/env python3
"""Program catalog for a throwaway kilix-land-desktop tab.

The house is a walkable desktop, so it deliberately owns no list UI: the
study computer opens this in a tab the way the TV opens the games menu and
the shed opens the binding editor. What it lists:

  * the stack's own programs, when they are installed;
  * every application this machine advertises through XDG `.desktop`
    files, bucketed by category;
  * the user's own launchers from the desktop folder;
  * a run-a-command row for everything else.

A host-owned catalog wins when one exists — `src/launcher.c` prefers
`kilix-launcher` on PATH and only falls back here — so a box that installs
the shared launcher later gets it without changing the house.

Discovery is stdlib-only and read-only. Launching replaces this process
with the chosen program (`execvp`), so the tab becomes the program and
closing the tab closes it.
"""

import json
import os
import shlex
import shutil
import subprocess
import sys

XDG_FIELD_CODES = ("%f", "%F", "%u", "%U", "%d", "%D", "%n", "%N", "%i",
                   "%c", "%k", "%v", "%m")

# The stack's own programs, in the order a person is likely to want them.
# Each row is (label, argv). Rows whose first word is missing are dropped.
STACK_PROGRAMS = (
    ("Terminal", ["bash", "-l"]),
    ("PDF Conversion", ["@kilix", "app", "run", "kilix-pdf-conversion"]),
    ("Settings", ["kilix-settings"]),
    ("Files", ["kilix-file"]),
    ("System monitor", ["kilix-system"]),
    ("Thermals", ["kilix-temps"]),
    ("Memory", ["kilix-memory"]),
    ("Disks", ["kilix-disk"]),
    ("Packages", ["kilix-package"]),
    ("Music player", ["kilix-amp"]),
    ("Model store", ["kilix-bonsai"]),
    ("Read aloud", ["kilix-tts"]),
    ("Dictation", ["kilix-stt"]),
    ("Sessions", ["kilix-pty"]),
    ("Tmux manager", ["tmux-tui"]),
    ("Coding agents", ["kilix-rollout-resume"]),
    ("Text browser", ["cha"]),
)

CATEGORY_ORDER = (
    ("AudioVideo", "Sound and video"),
    ("Development", "Development"),
    ("Game", "Games"),
    ("Graphics", "Graphics"),
    ("Network", "Internet"),
    ("Office", "Office"),
    ("Settings", "Settings"),
    ("System", "System"),
    ("Utility", "Accessories"),
)


def data_directories():
    """XDG application directories, most specific first, deduplicated."""
    home = os.path.expanduser("~")
    data_home = os.environ.get("XDG_DATA_HOME") or os.path.join(
        home, ".local", "share")
    data_dirs = os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share"
    roots = [data_home] + [d for d in data_dirs.split(":") if d]
    seen = []
    for root in roots:
        path = os.path.join(os.path.abspath(os.path.expanduser(root)),
                            "applications")
        if path not in seen and os.path.isdir(path):
            seen.append(path)
    return seen


def desktop_folder():
    """The user's own launchers live wherever XDG_DESKTOP_DIR points."""
    configured = os.environ.get("XDG_DESKTOP_DIR")
    if configured:
        return os.path.abspath(os.path.expanduser(configured))
    return os.path.expanduser("~/Desktop")


def parse_desktop_file(path):
    """The [Desktop Entry] group as a dict, or None when unreadable.

    Deliberately small: keys are read literally, the first group wins, and
    locale variants are kept so the caller can prefer one.
    """
    entry = {}
    in_group = False
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("[") and line.endswith("]"):
                    if in_group:
                        break
                    in_group = line == "[Desktop Entry]"
                    continue
                if not in_group or not line or line.startswith("#"):
                    continue
                key, separator, value = line.partition("=")
                if not separator:
                    continue
                entry.setdefault(key.strip(), value.strip())
    except OSError:
        return None
    return entry or None


def localized(entry, key):
    """The Name/Comment for this locale, falling back to the plain key."""
    language = (os.environ.get("LC_MESSAGES") or os.environ.get("LANG") or
                "").split(".")[0]
    if language:
        for candidate in (f"{key}[{language}]",
                          f"{key}[{language.split('_')[0]}]"):
            if candidate in entry:
                return entry[candidate]
    return entry.get(key, "")


def entry_argv(entry):
    """Exec= as an argv, with field codes dropped.

    A launcher that needs a file or URL argument is offered without one:
    this is a menu, not a file manager.
    """
    exec_line = entry.get("Exec", "")
    if not exec_line:
        return None
    try:
        words = shlex.split(exec_line)
    except ValueError:
        return None
    argv = [word for word in words if word not in XDG_FIELD_CODES]
    # A %-code glued to a word (--file=%f) is dropped whole.
    argv = [word for word in argv if "%" not in word or word.startswith("%%")]
    if not argv:
        return None
    if entry.get("Terminal", "").strip().lower() == "true":
        # Already inside a terminal tab: run it directly rather than
        # asking a terminal emulator to open another window.
        pass
    return argv


def usable(entry):
    if entry.get("Type", "Application") != "Application":
        return False
    if entry.get("NoDisplay", "").strip().lower() == "true":
        return False
    if entry.get("Hidden", "").strip().lower() == "true":
        return False
    try_exec = entry.get("TryExec", "").strip()
    if try_exec and not shutil.which(try_exec) and not (
            os.path.isabs(try_exec) and os.access(try_exec, os.X_OK)):
        return False
    argv = entry_argv(entry)
    if not argv:
        return False
    program = argv[0]
    if os.path.isabs(program):
        return os.access(program, os.X_OK)
    return shutil.which(program) is not None


def bucket_for(entry):
    categories = {c for c in entry.get("Categories", "").split(";") if c}
    for key, label in CATEGORY_ORDER:
        if key in categories:
            return label
    return "Other"


def discover_applications():
    """(bucket, label, argv) rows from the XDG directories, deduplicated
    by desktop-file id so a user override hides the system copy."""
    rows = []
    claimed = set()
    for directory in data_directories():
        try:
            names = sorted(os.listdir(directory))
        except OSError:
            continue
        for name in names:
            if not name.endswith(".desktop"):
                continue
            if name in claimed:
                continue
            claimed.add(name)
            entry = parse_desktop_file(os.path.join(directory, name))
            if not entry or not usable(entry):
                continue
            label = localized(entry, "Name") or name[:-len(".desktop")]
            rows.append((bucket_for(entry), label, entry_argv(entry)))
    return rows


def discover_user_launchers():
    """The desktop folder's own .desktop files: launchers the user made."""
    rows = []
    folder = desktop_folder()
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        return rows
    for name in names:
        if not name.endswith(".desktop"):
            continue
        entry = parse_desktop_file(os.path.join(folder, name))
        if not entry or not usable(entry):
            continue
        label = localized(entry, "Name") or name[:-len(".desktop")]
        rows.append(("Your launchers", label, entry_argv(entry)))
    return rows


def kilix_command():
    """Resolve the host launcher without trusting the child tab's PATH."""
    home = os.environ.get("KILIX_HOME")
    if home:
        candidate = os.path.join(home, "kilix")
        if os.access(candidate, os.X_OK):
            return candidate
    source = os.environ.get("GPU_TERMINAL_SOURCE_HOME")
    if source:
        candidate = os.path.join(source, "kilix", "kilix")
        if os.access(candidate, os.X_OK):
            return candidate
    return shutil.which("kilix")


def discover_stack():
    rows = []
    for label, argv in STACK_PROGRAMS:
        command = list(argv)
        if command[0] == "@kilix":
            resolved = kilix_command()
            if not resolved:
                continue
            command[0] = resolved
        elif not shutil.which(command[0]):
            continue
        rows.append(("The stack", label, command))
    return rows


def discover_catalog_apps():
    """Application rows from the host's catalog, never a Land-owned ID list."""
    kilix = kilix_command()
    if not kilix:
        return []
    try:
        result = subprocess.run(
            [kilix, "install", "--json"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        payload = json.loads(result.stdout) if result.returncode == 0 else []
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    if not isinstance(payload, list):
        return []
    rows = []
    for record in payload:
        if not isinstance(record, dict) or record.get("kind") != "app":
            continue
        content_id = record.get("id")
        label = record.get("label")
        if not isinstance(content_id, str) or not content_id:
            continue
        if not isinstance(label, str) or not label or any(
                ord(character) < 32 for character in label):
            continue
        rows.append((
            "Kilix applications",
            label,
            [kilix, "app", "run", content_id],
        ))
    return rows


def build_catalog():
    """All rows, grouped, with the group order fixed so the menu does not
    reshuffle itself between runs."""
    rows = (
        discover_stack()
        + discover_catalog_apps()
        + discover_user_launchers()
        + discover_applications()
    )
    order = (
        ["The stack", "Kilix applications", "Your launchers"]
        + [label for _key, label in CATEGORY_ORDER]
        + ["Other"]
    )
    groups = []
    for bucket in order:
        members = sorted((label, argv) for group, label, argv in rows
                         if group == bucket)
        if members:
            groups.append((bucket, members))
    return groups


def wait_for_enter():
    try:
        input("\n[Enter to close]")
    except EOFError:
        pass


def run(argv):
    """Become the chosen program; the tab is its window."""
    try:
        os.execvp(argv[0], argv)
    except OSError as error:
        print(f"land-catalog: cannot run {argv[0]}: {error}",
              file=sys.stderr)
        wait_for_enter()
        return 1
    return 0


def prompt_command():
    try:
        line = input("command: ").strip()
    except EOFError:
        return None
    if not line:
        return None
    try:
        argv = shlex.split(line)
    except ValueError:
        print("that command has an unbalanced quote")
        return None
    return argv or None


def main():
    sys.dont_write_bytecode = True
    groups = build_catalog()
    numbered = []
    print("Kilix Land — Programs\n")
    for bucket, members in groups:
        print(f"  {bucket}")
        for label, argv in members:
            numbered.append(argv)
            print(f"  {len(numbered):3d}. {label}")
        print()
    if not numbered:
        print("  (nothing discovered on this machine)\n")
    print("    r. run a command")
    print("    q. close\n")
    while True:
        try:
            choice = input("program # (r to run, q to quit): ").strip()
        except EOFError:
            return 0
        if choice in ("", "q", "Q"):
            return 0
        if choice in ("r", "R"):
            argv = prompt_command()
            if argv:
                return run(argv)
            continue
        if choice.isdigit() and 1 <= int(choice) <= len(numbered):
            return run(numbered[int(choice) - 1])
        print(f"enter 1..{len(numbered)}, r, or q")


if __name__ == "__main__":
    raise SystemExit(main())
