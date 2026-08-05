#!/usr/bin/env python3
"""Numbered games menu for a throwaway kilix-land-desktop tab.

Preferred path: the host owns games. When this machine's `kilix` knows
`kilix games play GAME`, the menu hands the id over and the desktop needs
no other desktop's source tree — which is the whole point, because a box
that installed only Kilix Land has no Kilix 95 checkout to shell into.

Fallback path (today's reality on machines whose kilix predates that
verb): resolve a local Kilix 95 checkout, list its registry games, and
execv `python3 games.py GAME` so the tab becomes the chosen game. Catalog
discovery imports Kilix 95's games module under a temporary storage root —
mirroring kilix-cap's tools/kilix95_games.py — because that module creates
its config and data directories at import time; the launch itself runs
with the real environment restored so installs land where Kilix 95
expects them.
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT_ENV_VARS = ("KILIX95_PROJECT_HOME", "KILIX95_DIR", "KILIX95_HOME")


def resolve_root():
    for name in ROOT_ENV_VARS:
        value = os.environ.get(name)
        if value:
            return os.path.abspath(os.path.expanduser(value))
    source = os.environ.get("GPU_TERMINAL_SOURCE_HOME")
    if source:
        return os.path.abspath(
            os.path.join(os.path.expanduser(source), "kilix-desktops",
                         "kilix-95"))
    return os.path.expanduser(
        "~/.local/gpu_terminal/sources/kilix-desktops/kilix-95")


def wait_for_enter():
    try:
        input("\n[Enter to close]")
    except EOFError:
        pass


def resolve_kilix():
    """The kilix launcher, in the same order src/launcher.c uses:
    installed first, then the source checkout, then PATH."""
    candidates = []
    home = os.environ.get("KILIX_HOME")
    if home:
        candidates.append(os.path.join(home, "kilix"))
    source = os.environ.get("GPU_TERMINAL_SOURCE_HOME")
    if source:
        candidates.append(
            os.path.join(os.path.expanduser(source), "kilix", "kilix"))
    found = shutil.which("kilix")
    if found:
        candidates.append(found)
    for candidate in candidates:
        if os.access(candidate, os.X_OK):
            return candidate
    return None


def host_plays_games(kilix):
    """True when this kilix knows `games play`.

    Asked without a game id, which every version refuses — the answer is
    in *how* it refuses. A version that has the verb names it in its own
    usage line; a version that does not lists only the actions it has.
    Nothing is launched either way, so the probe is free of side effects.
    """
    try:
        probe = subprocess.run([kilix, "games", "play"],
                               stdin=subprocess.DEVNULL,
                               capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return False
    if probe.returncode == 0:
        return True
    message = (probe.stderr or "") + (probe.stdout or "")
    if "usage:" not in message.lower():
        return False
    return "play" in message


def load_catalog(root):
    """(game_id, label) rows from Kilix 95's registry, in menu order."""
    old_path = list(sys.path)
    old_cwd = os.getcwd()
    prior = os.environ.get("KILIX95_STORAGE_HOME")
    try:
        with tempfile.TemporaryDirectory(prefix="land-games-") as tmp:
            os.environ["KILIX95_STORAGE_HOME"] = tmp
            sys.path.insert(0, root)
            os.chdir(root)
            import games
            rows = []
            for game_id, meta in games.GAMES.items():
                label = meta.get("label") if isinstance(meta, dict) else None
                if not isinstance(label, str) or not label:
                    label = game_id
                rows.append((game_id, label))
            return rows
    finally:
        sys.path[:] = old_path
        os.chdir(old_cwd)
        if prior is None:
            os.environ.pop("KILIX95_STORAGE_HOME", None)
        else:
            os.environ["KILIX95_STORAGE_HOME"] = prior


def pick(rows):
    print("Kilix Land — Games\n")
    for index, (_game_id, label) in enumerate(rows, 1):
        print(f"  {index:2d}. {label}")
    print()
    while True:
        try:
            choice = input("game # (q to quit): ").strip()
        except EOFError:
            return None
        if choice in ("", "q", "Q"):
            return None
        if choice.isdigit() and 1 <= int(choice) <= len(rows):
            return rows[int(choice) - 1][0]
        print(f"enter 1..{len(rows)} or q")


def main():
    # Keep catalog discovery from dropping __pycache__ into the checkout.
    sys.dont_write_bytecode = True
    kilix = resolve_kilix()
    host_owned = kilix is not None and host_plays_games(kilix)
    root = resolve_root()
    games_py = os.path.join(root, "games.py")
    if host_owned and not os.path.isfile(games_py):
        # The host owns installs and launches, but not (yet) the catalog,
        # so there is nothing to list without a checkout. Hand the whole
        # menu over rather than printing an empty one.
        os.execv(kilix, [kilix, "games", "play"])
    if not os.path.isfile(games_py):
        print(f"Kilix 95 is not installed (no games.py under {root}).")
        print("Clone github.com/itsmygithubacct/kilix-95 to "
              "~/.local/gpu_terminal/sources/kilix-desktops/kilix-95 or set "
              "KILIX95_PROJECT_HOME.")
        print("A kilix with `kilix games play GAME` removes this "
              "requirement.")
        wait_for_enter()
        return 1
    try:
        rows = load_catalog(root)
    except Exception as error:
        print(f"land-games: cannot read the Kilix 95 catalog: {error}",
              file=sys.stderr)
        wait_for_enter()
        return 1
    if not rows:
        print("Kilix 95 lists no games.")
        wait_for_enter()
        return 0
    game = pick(rows)
    if game is None:
        return 0
    if host_owned:
        # No cross-checkout coupling: the host installs and boots it.
        os.execv(kilix, [kilix, "games", "play", game])
    os.chdir(root)
    os.execv(sys.executable, [sys.executable, games_py, game])


if __name__ == "__main__":
    raise SystemExit(main())
