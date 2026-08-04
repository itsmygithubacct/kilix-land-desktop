#!/usr/bin/env python3
"""Synchronize Kilix Land Desktop's cast subset with the four source games.

This is the trimmed sibling of kilix-land's tools/sync_source_parity.py: the
same four source games, the same committed-history discipline, but only the
cast atlases, portraits, and the three desktop UI cues per cast. No enemies,
no training backdrops, no combat cues.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def repository_root(path: Path) -> Path:
    for candidate in (path, *path.parents):
        if (candidate / ".git").exists():
            return candidate
    raise ValueError(f"no git repository contains {path}")


def git_output(root: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise ValueError(f"git {' '.join(arguments)} failed in {root}: {detail}")
    return result.stdout


def committed_bytes(path: Path) -> bytes:
    """Read a source game's committed content.

    The desktop pins itself to what each source game has committed, never to
    its working tree. A game mid-art-pass has files on disk that its own
    history has not accepted yet, and the desktop must not ship those.
    """
    root = repository_root(path)
    relative = path.resolve().relative_to(root.resolve()).as_posix()
    return git_output(root, "show", f"HEAD:{relative}")


def committed_text(path: Path) -> str:
    return committed_bytes(path).decode("utf-8")


def committed_commit(path: Path) -> str:
    root = repository_root(path)
    return git_output(root, "rev-parse", "HEAD").decode("ascii").strip()


def load_json(path: Path) -> dict[str, Any]:
    """Read one of the desktop's own files from disk."""
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def png_dimensions(data: bytes) -> tuple[int, int]:
    if (
        len(data) < 24
        or data[:8] != b"\x89PNG\r\n\x1a\n"
        or data[12:16] != b"IHDR"
    ):
        raise ValueError("copied graphics asset is not a valid PNG")
    return struct.unpack(">II", data[16:24])


def committed_atlas_grid(source: Path, data: bytes) -> dict[str, int]:
    """Return the source atlas grid from the source game's committed manifest."""
    root = repository_root(source)
    relative = source.resolve().relative_to(root.resolve()).as_posix()
    manifest_path = source.parents[1] / "manifest.json"
    manifest = json.loads(committed_text(manifest_path))
    for entry in manifest.get("atlases", []):
        if entry.get("path") != relative:
            continue
        grid = entry.get("grid")
        if not isinstance(grid, dict):
            break
        fields = ("columns", "rows", "width", "height",
                  "cell_width", "cell_height")
        if not all(
            isinstance(grid.get(field), int) and grid[field] > 0
            for field in fields
        ):
            break
        width, height = png_dimensions(data)
        expected = {field: grid[field] for field in fields}
        if (
            expected["width"] != width
            or expected["height"] != height
            or expected["columns"] * expected["cell_width"] != width
            or expected["rows"] * expected["cell_height"] != height
        ):
            raise ValueError(
                f"committed manifest grid does not match {relative}"
            )
        return expected
    raise ValueError(f"committed manifest has no valid grid for {relative}")


def source_digest(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(committed_bytes(path))
    return digest.hexdigest()


def c_define_number(path: Path, name: str) -> float:
    pattern = re.compile(
        rf"^#define\s+{re.escape(name)}\s+"
        r"([0-9]+(?:\.[0-9]+)?)f?\s*$",
        re.MULTILINE,
    )
    match = pattern.search(committed_text(path))
    if not match:
        raise ValueError(f"missing numeric C define {name} in {path}")
    return float(match.group(1))


def generated_header(games_root: Path) -> str:
    legend_game_path = games_root / "legend-of-kilix/src/game.c"

    legend_player_speed = c_define_number(
        legend_game_path, "LOK_PLAYER_SPEED"
    )

    digest = source_digest([legend_game_path])
    legend_commit = committed_commit(legend_game_path)[:12]
    chum_commit = committed_commit(games_root / "chumrunner")[:12]
    fantasy_commit = committed_commit(games_root / "kilix-fantasy")[:12]
    pleb_commit = committed_commit(games_root / "pleb-bound")[:12]

    # The per-cast accent colors live in kilix-land's hand-written
    # CAST_PROFILES table (src/game.c); they are not derived from the source
    # games, so no parity define exists for them here either.
    return f"""\
/* Generated by tools/sync_source_parity.py; do not edit by hand.
 * Every value below is read from the source games' committed history, never
 * from their working trees, so an in-flight art or balance pass in another
 * repository cannot reach Kilix Land Desktop until that repository commits
 * it.
 * Source content digest: {digest}
 */
#ifndef KILIX_LAND_DESKTOP_SOURCE_PARITY_H
#define KILIX_LAND_DESKTOP_SOURCE_PARITY_H

#define DESK_PARITY_LEGEND_SOURCE_COMMIT "{legend_commit}"
#define DESK_PARITY_CHUM_SOURCE_COMMIT "{chum_commit}"
#define DESK_PARITY_FANTASY_SOURCE_COMMIT "{fantasy_commit}"
#define DESK_PARITY_PLEB_SOURCE_COMMIT "{pleb_commit}"

#define DESK_PARITY_LEGEND_PLAYER_SPEED {legend_player_speed:.1f}f

#endif
"""


def asset_mappings(games_root: Path) -> list[tuple[str, Path, Path]]:
    legend = games_root / "legend-of-kilix/assets/graphics"
    chum = games_root / "chumrunner/assets/graphics"
    fantasy = games_root / "kilix-fantasy/assets/graphics"
    pleb = games_root / "pleb-bound/assets/graphics"
    return [
        ("legend-player", legend / "atlases/player.png",
         ROOT / "assets/graphics/casts/legend-player.png"),
        ("legend-npcs", legend / "atlases/npcs.png",
         ROOT / "assets/graphics/casts/legend-npcs.png"),
        ("legend-portraits", legend / "portraits/dialogue-portraits.png",
         ROOT / "assets/graphics/casts/legend-portraits.png"),
        ("chumrunner-characters", chum / "atlases/characters.png",
         ROOT / "assets/graphics/casts/chumrunner-characters.png"),
        ("chumrunner-portraits", chum / "atlases/portraits.png",
         ROOT / "assets/graphics/casts/chumrunner-portraits.png"),
        ("fantasy-characters", fantasy / "atlases/characters.png",
         ROOT / "assets/graphics/casts/fantasy-characters.png"),
        ("fantasy-portraits", fantasy / "atlases/portraits.png",
         ROOT / "assets/graphics/casts/fantasy-portraits.png"),
        ("pleb-bound-characters", pleb / "atlases/characters.png",
         ROOT / "assets/graphics/casts/pleb-bound-characters.png"),
        ("pleb-bound-portraits", pleb / "atlases/portraits.png",
         ROOT / "assets/graphics/casts/pleb-bound-portraits.png"),
    ]


def audio_mappings(games_root: Path) -> list[tuple[Path, Path]]:
    legend = games_root / "legend-of-kilix/assets/audio"
    chum = games_root / "chumrunner/assets/audio/core/noir"
    fantasy = games_root / "kilix-fantasy/assets/audio/core/fantasy"
    pleb = games_root / "pleb-bound/assets/audio/core/suburban"

    def mapped(
        root: Path, cast: str, items: list[tuple[str, str]]
    ) -> list[tuple[Path, Path]]:
        return [
            (root / source, ROOT / "assets/audio" / cast / destination)
            for source, destination in items
        ]

    # kilix-land substitutes a committed cue for the uncommitted
    # enemy_attack cues in Kilix Fantasy and Pleb Bound. The desktop takes
    # only ui-move, ui-confirm, and dialogue, none of which need a
    # substitution: every cue below is a direct committed copy.
    return (
        mapped(
            legend,
            "legend",
            [
                ("sfx/ui/cursor_move_01.wav", "ui-move.wav"),
                ("sfx/ui/confirm_01.wav", "ui-confirm.wav"),
                ("sfx/dialogue/text_kilix_01.wav", "dialogue.wav"),
            ],
        )
        + mapped(
            chum,
            "chumrunner",
            [
                ("interface/cursor_move.wav", "ui-move.wav"),
                ("interface/terminal_confirm.wav", "ui-confirm.wav"),
                ("interface/dialogue_blip.wav", "dialogue.wav"),
            ],
        )
        + mapped(
            fantasy,
            "fantasy",
            [
                ("interface/cursor_move.wav", "ui-move.wav"),
                ("interface/menu_confirm.wav", "ui-confirm.wav"),
                ("interface/text_blip.wav", "dialogue.wav"),
            ],
        )
        + mapped(
            pleb,
            "pleb-bound",
            [
                ("interface/cursor_move.wav", "ui-move.wav"),
                ("interface/menu_confirm.wav", "ui-confirm.wav"),
                ("dialogue/text_blip.wav", "dialogue.wav"),
            ],
        )
    )


SOURCE_GAMES = (
    "legend-of-kilix",
    "chumrunner",
    "kilix-fantasy",
    "pleb-bound",
)


def absent_games(games_root: Path) -> list[str]:
    """Source games that are not checked out beside this repository.

    Parity reads each game's *committed* history, so a directory only counts as
    present when it is a git repository.
    """
    return [
        game for game in SOURCE_GAMES
        if not (games_root / game / ".git").exists()
    ]


def under(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def synchronize(
    games_root: Path, write: bool, skip_absent: bool = False
) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    absent = absent_games(games_root) if skip_absent else []
    skipped = [f"{game}: not checked out at {games_root / game}"
               for game in absent]

    def present(source: Path) -> bool:
        return not any(under(source, games_root / game) for game in absent)

    header_path = ROOT / "src/source_parity.h"
    if absent:
        # The generated contract embeds a commit from every source game, so it
        # cannot be recomputed while any of them is missing.
        skipped.append(
            "src/source_parity.h: needs all four source games to recompute"
        )
    else:
        expected_header = generated_header(games_root)
        if write:
            header_path.parent.mkdir(parents=True, exist_ok=True)
            header_path.write_text(expected_header, encoding="utf-8")
        elif not header_path.exists() or header_path.read_text(
            encoding="utf-8"
        ) != expected_header:
            errors.append("src/source_parity.h is stale")

    manifest_path = ROOT / "assets/graphics/manifest.json"
    manifest = load_json(manifest_path)
    entries = {item["id"]: item for item in manifest["atlases"]}
    manifest_changed = False
    for asset_id, source, destination in asset_mappings(games_root):
        if not present(source):
            continue
        try:
            source_data = committed_bytes(source)
        except ValueError as error:
            errors.append(f"uncommitted source asset: {source} ({error})")
            continue
        if write:
            if not destination.is_file() or destination.read_bytes() != source_data:
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source_data)
        elif not destination.is_file() or destination.read_bytes() != source_data:
            errors.append(f"stale copied asset: {destination.relative_to(ROOT)}")
        expected_hash = sha256_bytes(source_data)
        entry = entries.get(asset_id)
        if not entry:
            errors.append(f"manifest is missing {asset_id}")
            continue
        if entry.get("sha256") != expected_hash:
            if write:
                entry["sha256"] = expected_hash
                manifest_changed = True
            else:
                errors.append(f"manifest hash is stale for {asset_id}")
        expected_grid = committed_atlas_grid(source, source_data)
        grid = entry.get("grid")
        if grid != expected_grid:
            if write:
                entry["grid"] = expected_grid
                manifest_changed = True
            else:
                errors.append(f"manifest grid is stale for {asset_id}")
    for source, destination in audio_mappings(games_root):
        if not present(source):
            continue
        try:
            source_data = committed_bytes(source)
        except ValueError as error:
            errors.append(f"uncommitted source audio: {source} ({error})")
            continue
        if write:
            if not destination.is_file() or destination.read_bytes() != source_data:
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source_data)
        elif not destination.is_file() or destination.read_bytes() != source_data:
            errors.append(f"stale copied audio: {destination.relative_to(ROOT)}")
    if write and manifest_changed:
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
    return errors, skipped


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--games-root",
        type=Path,
        default=ROOT.parents[1] / "games",
        help="directory containing the four source game repositories",
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="rewrite the generated contract and copied source assets",
    )
    parser.add_argument(
        "--skip-absent",
        action="store_true",
        help=(
            "skip parity for source games that are not checked out instead of "
            "failing; each game's checks switch back on by itself once that "
            "game is available"
        ),
    )
    args = parser.parse_args()
    games_root = args.games_root.resolve()
    if args.write and args.skip_absent:
        print(
            "source parity error: --write cannot be combined with "
            "--skip-absent; syncing requires every source game",
            file=sys.stderr,
        )
        return 1
    if not args.skip_absent:
        absent = absent_games(games_root)
        if absent:
            print(
                "source parity error: source games not checked out under "
                f"{games_root}: {', '.join(absent)}\n"
                "  pass --skip-absent (or use `make parity-check`) to verify "
                "only the games you have",
                file=sys.stderr,
            )
            return 1
    try:
        errors, skipped = synchronize(games_root, args.write, args.skip_absent)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"source parity error: {error}", file=sys.stderr)
        return 1
    for note in skipped:
        print(f"SKIP source parity {note}")
    if errors:
        for error in errors:
            print(f"source parity error: {error}", file=sys.stderr)
        print(
            "run `make parity-sync` to update Kilix Land Desktop",
            file=sys.stderr,
        )
        return 1
    mode = "synchronized" if args.write else "verified"
    if skipped:
        checked = [g for g in SOURCE_GAMES if g not in absent_games(games_root)]
        print(
            f"PASS source parity {mode} for "
            f"{len(checked)}/{len(SOURCE_GAMES)} source games "
            f"({', '.join(checked) if checked else 'none'}): "
            f"games={games_root}"
        )
    else:
        print(f"PASS source parity {mode}: games={games_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
