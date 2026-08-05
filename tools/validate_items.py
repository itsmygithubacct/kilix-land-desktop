#!/usr/bin/env python3
"""Validate assets/world/items.json against the item catalog contract.

Mirrors the C-side checks in src/items.c so `make test` fails fast with a
readable report before the binary ever parses the file. Keep the
vocabularies and capacity constants in lockstep with src/items.h and
src/items.c: families, behaviors, tags, id syntax, and every numeric
range. Item data selects compiled behavior by name only — this validator
exists partly to guarantee no content file ever grows a command, path, or
callback field.
"""

import json
import re
import sys

MAX_DEFINITIONS = 63  # catalog slot 0 is the compiled missing item
MAX_RECEIVERS = 16
MAX_TASTES = 12
MAX_TASTE_ENTRIES = 8
ID_CAPACITY = 48
NAME_CAPACITY = 40
DESCRIPTION_CAPACITY = 96
RECEIVER_ID_CAPACITY = 24
SPRITE_COLUMNS = 8
MAX_STACK_LIMIT = 99
MAX_TICKS = 2160000
RESERVED_ID = "core:missing-item"

FAMILIES = ("portable", "consumable", "tool", "wearable", "placeable",
            "key")
BEHAVIORS = ("hold", "drink", "use-tool", "equip", "place", "unlock")
TAGS = ("drink", "food", "media", "tool", "wearable", "placeable", "decor",
        "key", "giftable", "quest", "light", "discardable",
        "receiver-input", "terminal")
TASTE_CASTS = ("legend", "chumrunner", "fantasy", "pleb-bound")

ITEM_KEYS = {"id", "name", "description", "family", "behavior", "sprite",
             "max_stack", "tags", "effect_ticks"}
RECEIVER_KEYS = {"id", "accept_any_tag", "accept_all_tags", "accept_item",
                 "accept_family", "consume", "processing_ticks", "result",
                 "output"}
TASTE_KEYS = {"cast", "actor", "love", "like", "dislike"}
ACCEPT_KEYS = ("accept_any_tag", "accept_all_tags", "accept_item",
               "accept_family")

ITEM_ID_RE = re.compile(r"^[a-z0-9_-]+:[a-z0-9_-]+(?:[/.][a-z0-9_-]+)*$")
RECEIVER_ID_RE = re.compile(r"^[a-z0-9-]+$")


def fail(message):
    print(f"items: {message}", file=sys.stderr)
    raise SystemExit(1)


def reject_duplicate_keys(pairs):
    seen = set()
    for key, _ in pairs:
        if key in seen:
            fail(f"duplicate key '{key}'")
        seen.add(key)
    return dict(pairs)


def check_c_parser_subset(text):
    """Reject JSON constructs the shared C reader does not accept, so a
    file that passes this validator can never be refused by the binary."""
    in_string = False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == "\\":
                if i + 1 >= len(text) or text[i + 1] not in '"\\/nt':
                    fail(f"offset {i}: escape \\{text[i + 1:i + 2]} is not "
                         "supported by the C parser")
                i += 2
                continue
            if ch == '"':
                in_string = False
        elif ch == '"':
            in_string = True
        elif ch in "eE" and i > 0 and (text[i - 1].isdigit()
                                       or text[i - 1] == "."):
            fail(f"offset {i}: exponent notation is not supported by "
                 "the C parser")
        i += 1


def check_string(value, label, capacity):
    if not isinstance(value, str) or not value:
        fail(f"{label}: missing or empty")
    if len(value.encode()) >= capacity:
        fail(f"{label}: longer than capacity {capacity - 1}")


def check_int(value, label, minimum, maximum):
    if not isinstance(value, int) or isinstance(value, bool):
        fail(f"{label}: must be an integer")
    if not minimum <= value <= maximum:
        fail(f"{label}: out of range {minimum}..{maximum}")


def check_tags(value, label):
    if not isinstance(value, list):
        fail(f"{label}: must be an array of tag names")
    seen = set()
    for tag in value:
        if not isinstance(tag, str) or tag not in TAGS:
            fail(f"{label}: unknown tag '{tag}'")
        if tag in seen:
            fail(f"{label}: duplicate tag '{tag}'")
        seen.add(tag)
    return seen


def check_definition(index, item, ids):
    label = f"definitions[{index}]"
    if not isinstance(item, dict):
        fail(f"{label}: must be an object")
    unknown = set(item) - ITEM_KEYS
    if unknown:
        fail(f"{label}: unknown keys {', '.join(sorted(unknown))}")
    for required in ("id", "name", "description", "family", "behavior",
                     "sprite", "max_stack"):
        if required not in item:
            fail(f"{label}: missing '{required}'")
    check_string(item["id"], f"{label}.id", ID_CAPACITY)
    item_id = item["id"]
    if not ITEM_ID_RE.match(item_id) or ".." in item_id:
        fail(f"{label}: invalid item id '{item_id}'")
    if item_id == RESERVED_ID:
        fail(f"{label}: reserved item id '{item_id}'")
    if item_id in ids:
        fail(f"duplicate item id '{item_id}'")
    ids.add(item_id)
    check_string(item["name"], f"{label}.name", NAME_CAPACITY)
    check_string(item["description"], f"{label}.description",
                 DESCRIPTION_CAPACITY)
    if item["family"] not in FAMILIES:
        fail(f"{label}: unknown family '{item['family']}'")
    if item["behavior"] not in BEHAVIORS:
        fail(f"{label}: unknown behavior '{item['behavior']}'")
    check_int(item["sprite"], f"{label}.sprite", 1, SPRITE_COLUMNS - 1)
    check_int(item["max_stack"], f"{label}.max_stack", 1, MAX_STACK_LIMIT)
    if "tags" in item:
        check_tags(item["tags"], f"{label}.tags")
    if "effect_ticks" in item:
        if item["behavior"] != "drink":
            fail(f"{label}: effect_ticks requires behavior 'drink'")
        check_int(item["effect_ticks"], f"{label}.effect_ticks", 1,
                  MAX_TICKS)


def check_receiver(index, receiver, ids, item_ids):
    label = f"receivers[{index}]"
    if not isinstance(receiver, dict):
        fail(f"{label}: must be an object")
    unknown = set(receiver) - RECEIVER_KEYS
    if unknown:
        fail(f"{label}: unknown keys {', '.join(sorted(unknown))}")
    check_string(receiver.get("id"), f"{label}.id", RECEIVER_ID_CAPACITY)
    receiver_id = receiver["id"]
    if not RECEIVER_ID_RE.match(receiver_id):
        fail(f"{label}: invalid receiver id '{receiver_id}'")
    if receiver_id in ids:
        fail(f"duplicate receiver id '{receiver_id}'")
    ids.add(receiver_id)
    accepts = [key for key in ACCEPT_KEYS if key in receiver]
    if len(accepts) == 0:
        fail(f"{label}: missing accept rule")
    if len(accepts) > 1:
        fail(f"{label}: more than one accept rule")
    accept = accepts[0]
    if accept in ("accept_any_tag", "accept_all_tags"):
        if not check_tags(receiver[accept], f"{label}.{accept}"):
            fail(f"{label}: empty accept tag list")
    elif accept == "accept_item":
        if receiver[accept] not in item_ids:
            fail(f"{label}: unknown item '{receiver[accept]}' in receiver")
    else:
        if receiver[accept] not in FAMILIES:
            fail(f"{label}: unknown family '{receiver[accept]}'")
    if "consume" not in receiver or not isinstance(receiver["consume"],
                                                   bool):
        fail(f"{label}: missing boolean 'consume'")
    if "processing_ticks" in receiver:
        check_int(receiver["processing_ticks"],
                  f"{label}.processing_ticks", 0, MAX_TICKS)
    if receiver.get("result") not in ("activate-fixture", "none"):
        fail(f"{label}: unknown result '{receiver.get('result')}'")
    if "output" in receiver:
        check_string(receiver["output"], f"{label}.output", ID_CAPACITY)
        if not receiver["consume"]:
            fail(f"{label}: output requires 'consume': true")
        if receiver["output"] not in item_ids:
            fail(f"{label}: unknown output item '{receiver['output']}' "
                 "in receiver")


def check_taste_entries(value, label, item_ids):
    if not isinstance(value, list):
        fail(f"{label}: must be an array of taste entries")
    if len(value) > MAX_TASTE_ENTRIES:
        fail(f"{label}: more than {MAX_TASTE_ENTRIES} taste entries")
    seen = set()
    for entry in value:
        if not isinstance(entry, str):
            fail(f"{label}: taste entry must be a string")
        if entry in seen:
            fail(f"{label}: duplicate taste entry '{entry}'")
        seen.add(entry)
        if ":" in entry:
            if entry not in item_ids:
                fail(f"{label}: unknown taste entry '{entry}'")
        elif entry not in TAGS:
            fail(f"{label}: unknown taste entry '{entry}'")


def check_taste(index, taste, pairs, item_ids):
    label = f"tastes[{index}]"
    if not isinstance(taste, dict):
        fail(f"{label}: must be an object")
    unknown = set(taste) - TASTE_KEYS
    if unknown:
        fail(f"{label}: unknown keys {', '.join(sorted(unknown))}")
    if "cast" not in taste:
        fail(f"{label}: missing 'cast'")
    if "actor" not in taste:
        fail(f"{label}: missing 'actor'")
    cast = taste["cast"]
    if not isinstance(cast, str) or cast not in TASTE_CASTS:
        fail(f"{label}: unknown taste cast '{cast}'")
    check_int(taste["actor"], f"{label}.actor", 1, 3)
    pair = (cast, taste["actor"])
    if pair in pairs:
        fail(f"{label}: duplicate taste pair '{cast}' actor {taste['actor']}")
    pairs.add(pair)
    for tier in ("love", "like", "dislike"):
        if tier in taste:
            check_taste_entries(taste[tier], f"{label}.{tier}", item_ids)


def check_art(items_path, definitions):
    """The committed desktop-items artifacts: atlas geometry, manifest
    hash, provenance record, and a sprite column for every definition."""
    import hashlib
    import os
    root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(items_path))))
    atlas_path = os.path.join(root, "assets", "graphics", "items",
                              "desktop-items.png")
    provenance_path = os.path.join(root, "assets", "graphics", "items",
                                   "PROVENANCE.json")
    manifest_path = os.path.join(root, "assets", "graphics",
                                 "manifest.json")
    for required in (atlas_path, provenance_path):
        if not os.path.exists(required):
            fail(f"missing {os.path.relpath(required, root)} "
                 "(run `make items-art`)")
    from PIL import Image
    with Image.open(atlas_path) as image:
        if image.size != (32 * SPRITE_COLUMNS, 32 * 4):
            fail(f"desktop-items.png is {image.size[0]}x{image.size[1]}, "
                 f"want {32 * SPRITE_COLUMNS}x{32 * 4}")
        if image.mode != "RGBA":
            fail(f"desktop-items.png mode '{image.mode}' is not RGBA")
    with open(manifest_path, encoding="utf-8") as handle:
        manifest = json.load(handle)
    entry = next((atlas for atlas in manifest.get("atlases", [])
                  if atlas.get("id") == "desktop-items"), None)
    if entry is None:
        fail("manifest.json has no desktop-items atlas entry")
    with open(atlas_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    if entry.get("sha256") != digest:
        fail("manifest hash is stale for desktop-items "
             "(run `make items-art`)")
    grid = entry.get("grid", {})
    if (grid.get("columns"), grid.get("rows")) != (SPRITE_COLUMNS, 4) \
            or (grid.get("cell_width"), grid.get("cell_height")) != (32, 32):
        fail("manifest grid is wrong for desktop-items")
    with open(provenance_path, encoding="utf-8") as handle:
        provenance = json.load(handle)
    covered = {(cell.get("row"), cell.get("column"))
               for cell in provenance.get("cells", [])}
    for item in definitions:
        for row in range(4):
            if (row, item["sprite"]) not in covered:
                fail(f"provenance covers no cell for sprite column "
                     f"{item['sprite']} row {row} ('{item['id']}')")

    actions_path = os.path.join(root, "assets", "graphics", "items",
                                "desktop-item-actions.png")
    actions_provenance = os.path.join(root, "assets", "graphics", "items",
                                      "PROVENANCE-ACTIONS.json")
    for required in (actions_path, actions_provenance):
        if not os.path.exists(required):
            fail(f"missing {os.path.relpath(required, root)} "
                 "(run `make actions-art`)")
    with Image.open(actions_path) as image:
        if image.size != (128, 384):
            fail(f"desktop-item-actions.png is "
                 f"{image.size[0]}x{image.size[1]}, want 128x384")
        if image.mode != "RGBA":
            fail(f"desktop-item-actions.png mode '{image.mode}' is "
                 "not RGBA")
    entry = next((atlas for atlas in manifest.get("atlases", [])
                  if atlas.get("id") == "desktop-item-actions"), None)
    if entry is None:
        fail("manifest.json has no desktop-item-actions atlas entry")
    with open(actions_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    if entry.get("sha256") != digest:
        fail("manifest hash is stale for desktop-item-actions "
             "(run `make actions-art`)")
    grid = entry.get("grid", {})
    if (grid.get("columns"), grid.get("rows")) != (4, 12) \
            or (grid.get("cell_width"), grid.get("cell_height")) != (32, 32):
        fail("manifest grid is wrong for desktop-item-actions")
    with open(actions_provenance, encoding="utf-8") as handle:
        action_cells = {(cell.get("row"), cell.get("column"))
                        for cell in json.load(handle).get("cells", [])}
    for row in range(12):
        for column in range(4):
            if (row, column) not in action_cells:
                fail(f"action provenance covers no cell ({row}, {column})")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "assets/world/items.json"
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    check_c_parser_subset(text)
    catalog = json.loads(text, object_pairs_hook=reject_duplicate_keys)

    unknown = set(catalog) - {"items", "definitions", "receivers",
                              "tastes"}
    if unknown:
        fail(f"unknown keys {', '.join(sorted(unknown))} at top level")
    if catalog.get("items") != 1:
        fail("unsupported schema version (want items: 1)")
    definitions = catalog.get("definitions")
    if not isinstance(definitions, list):
        fail("missing key 'definitions'")
    if len(definitions) > MAX_DEFINITIONS:
        fail(f"more than {MAX_DEFINITIONS} items")

    item_ids = set()
    for index, item in enumerate(definitions):
        check_definition(index, item, item_ids)

    tastes = catalog.get("tastes", [])
    if not isinstance(tastes, list):
        fail("tastes must be an array")
    if len(tastes) > MAX_TASTES:
        fail(f"more than {MAX_TASTES} tastes")
    taste_pairs = set()
    for index, taste in enumerate(tastes):
        check_taste(index, taste, taste_pairs, item_ids)

    receivers = catalog.get("receivers", [])
    if not isinstance(receivers, list):
        fail("receivers must be an array")
    if len(receivers) > MAX_RECEIVERS:
        fail(f"more than {MAX_RECEIVERS} receivers")
    receiver_ids = set()
    for index, receiver in enumerate(receivers):
        check_receiver(index, receiver, receiver_ids, item_ids)

    check_art(path, definitions)

    print(f"items: OK ({len(definitions)} definitions, "
          f"{len(receivers)} receivers, {len(tastes)} tastes, "
          "art verified)")


if __name__ == "__main__":
    main()
