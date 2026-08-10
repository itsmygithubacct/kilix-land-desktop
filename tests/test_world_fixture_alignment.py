#!/usr/bin/env python3
"""Regression coverage for authored room fixtures and walk-behind data."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
WORLD = ROOT / "assets" / "world" / "world.json"
ROOMS = ROOT / "assets" / "graphics" / "rooms"
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")


class WorldFixtureAlignmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        world = json.loads(WORLD.read_text(encoding="utf-8"))
        cls.rooms = {room["id"]: room for room in world["rooms"]}

    def object_rect(self, room: str, object_id: str) -> dict[str, int]:
        objects = {
            item["id"]: item for item in self.rooms[room].get("objects", [])
        }
        return objects[object_id]["rect"]

    def test_phone_prompt_matches_the_right_wall_phone(self) -> None:
        self.assertEqual(
            self.object_rect("living", "phone"),
            {"x": 454, "y": 158, "w": 22, "h": 34},
        )

    def test_filing_cabinet_prompt_matches_the_cabinet(self) -> None:
        self.assertEqual(
            self.object_rect("study", "filing-cabinet"),
            {"x": 78, "y": 152, "w": 30, "h": 46},
        )

    def test_living_masks_have_no_unreachable_floor_line_regions(self) -> None:
        self.assertEqual(
            self.rooms["living"]["walkbehinds"],
            [{"id": 1, "baseline": 216}],
        )
        for style in STYLES:
            with self.subTest(style=style):
                with Image.open(ROOMS / style / "living-behind.png") as mask:
                    self.assertEqual(set(mask.getdata()), {0, 1})


if __name__ == "__main__":
    unittest.main()
