#!/usr/bin/env python3
"""Cook a generated image into a runtime room plate.

Generator-agnostic (the kilix-cap prepare_visual.py shape): decode whatever
the generator produced (including JPEG bytes hiding in a .png filename),
center-fit with Lanczos to exactly 1280x720, and write a true PNG. The
runtime loader in src/graphics.c refuses any other size, so this is the only
door plates enter through.

Usage: prepare_plate.py SOURCE DEST.png
"""

import sys

from PIL import Image

PLATE_WIDTH = 1280
PLATE_HEIGHT = 720


def cook(source_path, destination_path):
    image = Image.open(source_path).convert("RGB")
    width, height = image.size
    if width < 640 or height < 360:
        raise SystemExit(f"source too small ({width}x{height}); regenerate "
                         "rather than upscale from below half size")
    target_ratio = PLATE_WIDTH / PLATE_HEIGHT
    ratio = width / height
    if ratio > target_ratio:
        crop_width = round(height * target_ratio)
        left = (width - crop_width) // 2
        image = image.crop((left, 0, left + crop_width, height))
    elif ratio < target_ratio:
        crop_height = round(width / target_ratio)
        top = (height - crop_height) // 2
        image = image.crop((0, top, width, top + crop_height))
    image = image.resize((PLATE_WIDTH, PLATE_HEIGHT), Image.LANCZOS)
    if image.size != (PLATE_WIDTH, PLATE_HEIGHT):
        raise SystemExit("resize failed to hit the plate contract")
    image.save(destination_path, format="PNG")
    print(f"plate: {destination_path} {PLATE_WIDTH}x{PLATE_HEIGHT} "
          f"(from {width}x{height})")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip())
    cook(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
