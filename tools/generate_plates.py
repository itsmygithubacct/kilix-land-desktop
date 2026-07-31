#!/usr/bin/env python3
"""Generate the room plates through the gemini image pipeline.

Runs the gemini-v7 helper from a scratch working directory (it drops a stray
images/last_image.png in its cwd), cooks each result with prepare_plate.py,
installs into assets/graphics/rooms/<style>/<room>.png, and records
provenance (verbatim prompt + sha256) into PROMPTS.md and manifest.json
under assets/graphics/rooms/.

Generated plates are NOT committed by this tool; review them by eye first.

Usage: generate_plates.py [--only STYLE[/ROOM]] [--force]
"""

import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import plate_prompts  # noqa: E402
import prepare_plate  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[1]
ROOMS_DIR = REPO / "assets" / "graphics" / "rooms"
GENERATOR = pathlib.Path.home() / (
    "image-generation/providers/gemini-v7/gemini_image/gemini_image.sh")
MODEL = "gemini-3-pro-image"
ATTEMPTS = 3


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def generate_one(prompt, destination, scratch):
    raw = scratch / (destination.stem + "-raw.png")
    command = [
        str(GENERATOR), prompt,
        "--model", MODEL,
        "--aspect-ratio", "16:9",
        "--image-only",
        "--output", str(raw),
    ]
    for attempt in range(1, ATTEMPTS + 1):
        result = subprocess.run(command, cwd=scratch, capture_output=True,
                                text=True, timeout=420)
        if result.returncode == 0 and raw.exists() and raw.stat().st_size:
            try:
                prepare_plate.cook(str(raw), str(destination))
                return True
            except SystemExit as error:
                print(f"  cook rejected ({error}); retrying", flush=True)
        else:
            tail = (result.stderr or result.stdout or "").strip()
            print(f"  attempt {attempt} failed: {tail[-200:]}", flush=True)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="STYLE or STYLE/ROOM filter")
    parser.add_argument("--force", action="store_true",
                        help="regenerate plates that already exist")
    arguments = parser.parse_args()

    if not GENERATOR.exists():
        raise SystemExit(f"generator missing: {GENERATOR}")
    ROOMS_DIR.mkdir(parents=True, exist_ok=True)
    manifest_path = ROOMS_DIR / "manifest.json"
    manifest = {}
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())

    todo = []
    for style, room, prompt in plate_prompts.all_plates():
        if arguments.only:
            want = arguments.only.split("/")
            if want[0] != style or (len(want) > 1 and want[1] != room):
                continue
        todo.append((style, room, prompt))

    failures = []
    with tempfile.TemporaryDirectory(prefix="land-plates.") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        for style, room, prompt in todo:
            destination = ROOMS_DIR / style / f"{room}.png"
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists() and not arguments.force:
                print(f"keep {style}/{room} (exists)", flush=True)
                continue
            print(f"generate {style}/{room} ...", flush=True)
            if not generate_one(prompt, destination, scratch):
                failures.append(f"{style}/{room}")
                continue
            manifest[f"{style}/{room}"] = {
                "file": f"{style}/{room}.png",
                "sha256": sha256_file(destination),
                "width": prepare_plate.PLATE_WIDTH,
                "height": prepare_plate.PLATE_HEIGHT,
                "generator": f"gemini-v7 {MODEL}",
                "generated": datetime.date.today().isoformat(),
            }
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    write_prompts_md()
    if failures:
        raise SystemExit("failed plates: " + ", ".join(failures))
    print("all requested plates present; review by eye before committing")


def write_prompts_md():
    lines = [
        "# Room plate prompts",
        "",
        "Generated with the gemini image pipeline "
        f"(`providers/gemini-v7`, model `{MODEL}`), cooked to 1280x720 by "
        "`tools/prepare_plate.py` (center-fit Lanczos). Hashes in "
        "`manifest.json`. Prompts are emitted verbatim from "
        "`tools/plate_prompts.py`, the authoritative source.",
        "",
    ]
    for style, room, prompt in plate_prompts.all_plates():
        lines.append(f"## {style} / {room}")
        lines.append("")
        lines.append("```text")
        lines.append(prompt)
        lines.append("```")
        lines.append("")
    (ROOMS_DIR / "PROMPTS.md").write_text("\n".join(lines))


if __name__ == "__main__":
    main()
