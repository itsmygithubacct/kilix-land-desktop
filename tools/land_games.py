#!/usr/bin/env python3
"""Numbered games menu for a throwaway kilix-land-desktop tab.

Resolves a local Kilix 95 checkout, lists its registry games, and execv's
`python3 games.py GAME` so the tab becomes the chosen game. Catalog discovery
imports Kilix 95's games module under a temporary storage root — mirroring
kilix-cap's tools/kilix95_games.py — because that module creates its config
and data directories at import time; the launch itself runs with the real
environment restored so installs land where Kilix 95 expects them.
"""

import os
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
            os.path.join(os.path.expanduser(source), "kilix-95"))
    return os.path.expanduser("~/.local/gpu_terminal/sources/kilix-95")


def wait_for_enter():
    try:
        input("\n[Enter to close]")
    except EOFError:
        pass


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
    root = resolve_root()
    games_py = os.path.join(root, "games.py")
    if not os.path.isfile(games_py):
        print(f"Kilix 95 is not installed (no games.py under {root}).")
        print("Clone github.com/itsmygithubacct/kilix-95 to "
              "~/.local/gpu_terminal/sources/kilix-95 or set KILIX95_PROJECT_HOME.")
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
    os.chdir(root)
    os.execv(sys.executable, [sys.executable, games_py, game])


if __name__ == "__main__":
    raise SystemExit(main())
