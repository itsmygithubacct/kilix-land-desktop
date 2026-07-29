#!/usr/bin/env python3
"""Focused failure-mode tests for tools/validate_visual.py."""

from __future__ import annotations

import binascii
import hashlib
import json
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import validate_visual  # noqa: E402


def chunk(kind: bytes, payload: bytes) -> bytes:
    crc = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def rgb_png(width: int = 1280, height: int = 720) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    row = b"\0" + b"\0" * (width * 3)
    pixels = zlib.compress(row * height, 9)
    return (
        validate_visual.PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", pixels)
        + chunk(b"IEND", b"")
    )


class VisualFixture:
    def __init__(self, root: Path):
        self.root = root
        self.plate = rgb_png()
        digest = hashlib.sha256(self.plate).hexdigest()
        self.manifest = {}
        for key in validate_visual.expected_keys():
            path = root / f"{key}.png"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.plate)
            self.manifest[key] = {
                "file": f"{key}.png",
                "generated": "2026-07-29",
                "generator": "fixture test-generator",
                "height": 720,
                "sha256": digest,
                "width": 1280,
            }
        self.write_manifest()
        shutil.copyfile(
            ROOT / "assets" / "graphics" / "rooms" / "PROMPTS.md",
            root / "PROMPTS.md",
        )

    def write_manifest(self) -> None:
        (self.root / "manifest.json").write_text(
            json.dumps(self.manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )


class ValidateVisualTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="visual-test.")
        self.directory = Path(self.temporary.name)
        self.fixture = VisualFixture(self.directory)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def errors(self) -> list[str]:
        return validate_visual.validate(self.directory)

    def test_complete_fixture_passes(self) -> None:
        self.assertEqual(self.errors(), [])

    def test_inventory_reports_missing_and_unexpected_files(self) -> None:
        (self.directory / "legend" / "bedroom.png").unlink()
        (self.directory / "stray.jpg").write_bytes(b"stray")
        errors = self.errors()
        self.assertIn("missing file: legend/bedroom.png", errors)
        self.assertIn("unexpected file: stray.jpg", errors)

    def test_png_contract_and_hash_are_both_checked(self) -> None:
        key = "fantasy/study"
        (self.directory / f"{key}.png").write_bytes(rgb_png(640, 360))
        errors = self.errors()
        self.assertTrue(
            any(
                error.startswith(
                    "fantasy/study.png: dimensions are 640x360"
                )
                for error in errors
            )
        )
        self.assertTrue(
            any(
                error.startswith("fantasy/study.png: sha256 mismatch")
                for error in errors
            )
        )

    def test_manifest_hash_drift_is_rejected(self) -> None:
        self.fixture.manifest["chumrunner/yard"]["sha256"] = "0" * 64
        self.fixture.write_manifest()
        self.assertTrue(
            any(
                error.startswith("chumrunner/yard.png: sha256 mismatch")
                for error in self.errors()
            )
        )

    def test_prompt_must_remain_verbatim(self) -> None:
        path = self.directory / "PROMPTS.md"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace(
                "Create an original the resident's small private bedroom",
                "Create a changed resident bedroom",
                1,
            ),
            encoding="utf-8",
        )
        self.assertIn(
            "PROMPTS.md legend/bedroom: prompt differs from "
            "tools/plate_prompts.py",
            self.errors(),
        )

    def test_corrupt_png_is_rejected(self) -> None:
        (self.directory / "pleb-bound" / "kitchen.png").write_bytes(b"not png")
        self.assertIn(
            "pleb-bound/kitchen.png: is not a PNG",
            self.errors(),
        )


if __name__ == "__main__":
    unittest.main()
