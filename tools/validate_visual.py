#!/usr/bin/env python3
"""Validate the complete room-plate inventory and its provenance.

Checks the generator-independent contract promised by the implementation
document: exactly five 1280x720 RGB PNG plates for each of the four styles,
one manifest entry and one verbatim prompt record per plate, and matching
SHA-256 hashes.

Usage: validate_visual.py [ROOMS_DIRECTORY]
"""

from __future__ import annotations

import argparse
import binascii
import datetime
import hashlib
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any

import plate_prompts


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ROOMS_DIRECTORY = ROOT / "assets" / "graphics" / "rooms"
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
ROOMS = ("bedroom", "living", "study", "kitchen", "yard")
PLATE_WIDTH = 1280
PLATE_HEIGHT = 720
MANIFEST_FIELDS = {
    "file", "generated", "generator", "height", "sha256", "width",
}
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
PROMPT_HEADING = re.compile(
    r"^## ([a-z][a-z0-9-]*) / ([a-z][a-z0-9-]*)$",
    re.MULTILINE,
)
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class DuplicateJsonKey(ValueError):
    """Raised when JSON contains a key Python's normal decoder would hide."""


def expected_keys() -> tuple[str, ...]:
    return tuple(f"{style}/{room}" for style in STYLES for room in ROOMS)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKey(f"duplicate JSON key '{key}'")
        result[key] = value
    return result


def load_manifest(path: Path, errors: list[str]) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        errors.append(f"manifest.json cannot be read: {error}")
        return {}
    try:
        manifest = json.loads(text, object_pairs_hook=unique_json_object)
    except (json.JSONDecodeError, DuplicateJsonKey) as error:
        errors.append(f"manifest.json is invalid: {error}")
        return {}
    if not isinstance(manifest, dict):
        errors.append("manifest.json root must be an object")
        return {}
    return manifest


def png_info(path: Path) -> tuple[int, int, int, int] | str:
    """Return width, height, bit depth, color type or a validation error."""
    try:
        data = path.read_bytes()
    except OSError as error:
        return f"cannot be read: {error}"
    if not data.startswith(PNG_SIGNATURE):
        return "is not a PNG"

    offset = len(PNG_SIGNATURE)
    chunks: list[bytes] = []
    dimensions: tuple[int, int, int, int] | None = None
    saw_idat = False
    while offset < len(data):
        if len(data) - offset < 12:
            return "has a truncated chunk header"
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        chunk_type = data[offset + 4:offset + 8]
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            return f"has a truncated {chunk_type.decode('ascii', 'replace')} chunk"
        payload = data[offset + 8:offset + 8 + length]
        stored_crc = struct.unpack(">I", data[offset + 8 + length:chunk_end])[0]
        actual_crc = binascii.crc32(chunk_type + payload) & 0xFFFFFFFF
        if stored_crc != actual_crc:
            return f"has a bad {chunk_type.decode('ascii', 'replace')} CRC"

        chunks.append(chunk_type)
        if chunk_type == b"IHDR":
            if len(chunks) != 1 or dimensions is not None or length != 13:
                return "has an invalid IHDR chunk"
            width, height, bit_depth, color_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", payload)
            )
            if compression != 0 or filtering != 0 or interlace != 0:
                return "uses unsupported PNG compression, filtering, or interlace"
            dimensions = (width, height, bit_depth, color_type)
        elif chunk_type == b"IDAT":
            saw_idat = True
        elif chunk_type == b"IEND":
            if length != 0:
                return "has an invalid IEND chunk"
            if chunk_end != len(data):
                return "has bytes after IEND"
            break
        offset = chunk_end

    if not chunks or chunks[-1] != b"IEND":
        return "has no terminal IEND chunk"
    if dimensions is None:
        return "has no IHDR chunk"
    if not saw_idat:
        return "has no IDAT chunk"
    return dimensions


def validate_inventory(directory: Path, errors: list[str]) -> None:
    wanted = {"PROMPTS.md", "manifest.json"}
    wanted.update(f"{key}.png" for key in expected_keys())
    # Walk-behind masks are optional per room+style; their content contract
    # (plate-sized, grayscale, values in {0} | declared ids) is enforced by
    # tools/validate_world.py, which knows the declared region ids.
    optional = {f"{key}-behind.png" for key in expected_keys()}
    try:
        actual = {
            path.relative_to(directory).as_posix()
            for path in directory.rglob("*")
            if path.is_file()
        }
    except OSError as error:
        errors.append(f"room directory cannot be read: {error}")
        return
    for missing in sorted(wanted - actual):
        errors.append(f"missing file: {missing}")
    for unexpected in sorted(actual - wanted - optional):
        errors.append(f"unexpected file: {unexpected}")


def validate_manifest(directory: Path, errors: list[str]) -> None:
    manifest = load_manifest(directory / "manifest.json", errors)
    wanted = set(expected_keys())
    actual = set(manifest)
    for missing in sorted(wanted - actual):
        errors.append(f"manifest missing entry: {missing}")
    for unexpected in sorted(actual - wanted):
        errors.append(f"manifest has unexpected entry: {unexpected}")

    for key in sorted(wanted & actual):
        entry = manifest[key]
        if not isinstance(entry, dict):
            errors.append(f"manifest {key}: entry must be an object")
            continue
        fields = set(entry)
        if fields != MANIFEST_FIELDS:
            missing = ", ".join(sorted(MANIFEST_FIELDS - fields))
            extra = ", ".join(sorted(fields - MANIFEST_FIELDS))
            detail = []
            if missing:
                detail.append(f"missing fields {missing}")
            if extra:
                detail.append(f"unexpected fields {extra}")
            errors.append(f"manifest {key}: {'; '.join(detail)}")
            continue

        expected_file = f"{key}.png"
        if entry["file"] != expected_file:
            errors.append(
                f"manifest {key}: file must be '{expected_file}'"
            )
        if type(entry["width"]) is not int or entry["width"] != PLATE_WIDTH:
            errors.append(
                f"manifest {key}: width must be {PLATE_WIDTH}"
            )
        if type(entry["height"]) is not int or entry["height"] != PLATE_HEIGHT:
            errors.append(
                f"manifest {key}: height must be {PLATE_HEIGHT}"
            )
        if not isinstance(entry["generator"], str) or not entry["generator"].strip():
            errors.append(f"manifest {key}: generator must be a non-empty string")
        if not isinstance(entry["generated"], str):
            errors.append(f"manifest {key}: generated must be an ISO date")
        else:
            try:
                datetime.date.fromisoformat(entry["generated"])
            except ValueError:
                errors.append(f"manifest {key}: generated must be an ISO date")
        if (
            not isinstance(entry["sha256"], str)
            or SHA256_PATTERN.fullmatch(entry["sha256"]) is None
        ):
            errors.append(f"manifest {key}: sha256 must be 64 lowercase hex digits")

        plate = directory / expected_file
        if not plate.is_file():
            continue
        info = png_info(plate)
        if isinstance(info, str):
            errors.append(f"{expected_file}: {info}")
        else:
            width, height, bit_depth, color_type = info
            if (width, height) != (PLATE_WIDTH, PLATE_HEIGHT):
                errors.append(
                    f"{expected_file}: dimensions are {width}x{height}, "
                    f"want {PLATE_WIDTH}x{PLATE_HEIGHT}"
                )
            if (bit_depth, color_type) != (8, 2):
                errors.append(
                    f"{expected_file}: want 8-bit RGB PNG, got "
                    f"bit-depth {bit_depth}, color-type {color_type}"
                )
        if (
            isinstance(entry["sha256"], str)
            and SHA256_PATTERN.fullmatch(entry["sha256"]) is not None
        ):
            actual_hash = sha256_file(plate)
            if actual_hash != entry["sha256"]:
                errors.append(
                    f"{expected_file}: sha256 mismatch "
                    f"(manifest {entry['sha256']}, actual {actual_hash})"
                )


def authoritative_prompts() -> dict[str, str]:
    prompts: dict[str, str] = {}
    for style, room, prompt in plate_prompts.all_plates():
        key = f"{style}/{room}"
        if key in prompts:
            raise ValueError(f"plate_prompts.py repeats {key}")
        prompts[key] = prompt
    return prompts


def validate_prompts(directory: Path, errors: list[str]) -> None:
    path = directory / "PROMPTS.md"
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        errors.append(f"PROMPTS.md cannot be read: {error}")
        return
    matches = list(PROMPT_HEADING.finditer(text))
    found: dict[str, str] = {}
    for index, match in enumerate(matches):
        key = f"{match.group(1)}/{match.group(2)}"
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        section = text[match.end():end]
        fenced = re.fullmatch(r"\s*```text\n(.*?)\n```\s*", section, re.DOTALL)
        if fenced is None:
            errors.append(f"PROMPTS.md {key}: expected one fenced text prompt")
            continue
        if key in found:
            errors.append(f"PROMPTS.md repeats heading: {key}")
            continue
        found[key] = fenced.group(1)

    wanted = set(expected_keys())
    actual = set(found)
    for missing in sorted(wanted - actual):
        errors.append(f"PROMPTS.md missing prompt: {missing}")
    for unexpected in sorted(actual - wanted):
        errors.append(f"PROMPTS.md has unexpected prompt: {unexpected}")

    try:
        source = authoritative_prompts()
    except ValueError as error:
        errors.append(str(error))
        return
    if set(source) != wanted:
        missing = ", ".join(sorted(wanted - set(source)))
        extra = ", ".join(sorted(set(source) - wanted))
        errors.append(
            "plate_prompts.py inventory differs from the contract"
            f" (missing: {missing or 'none'}; unexpected: {extra or 'none'})"
        )
    for key in sorted(wanted & actual & set(source)):
        if found[key] != source[key]:
            errors.append(
                f"PROMPTS.md {key}: prompt differs from tools/plate_prompts.py"
            )


def validate(directory: Path) -> list[str]:
    errors: list[str] = []
    if not directory.is_dir():
        return [f"room directory does not exist: {directory}"]
    validate_inventory(directory, errors)
    validate_manifest(directory, errors)
    validate_prompts(directory, errors)
    return errors


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "directory",
        nargs="?",
        type=Path,
        default=DEFAULT_ROOMS_DIRECTORY,
        help="room-plate directory (default: assets/graphics/rooms)",
    )
    arguments = parser.parse_args()
    errors = validate(arguments.directory.resolve())
    if errors:
        for error in errors:
            print(f"visual: {error}", file=sys.stderr)
        raise SystemExit(1)
    print(
        f"visual: OK ({len(expected_keys())} plates, "
        f"{PLATE_WIDTH}x{PLATE_HEIGHT}, prompts+hashes verified)"
    )


if __name__ == "__main__":
    main()
