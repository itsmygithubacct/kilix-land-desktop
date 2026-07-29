"""Structured generation prompts for the 20 room plates (5 rooms x 4 styles).

Follows kilix-land's assets/graphics/backgrounds/PROMPTS.md template: a
stylized-concept brief with environment-only subject, straight-on wide 16:9
framing, clear lower-third floor, and hard no-text/no-character constraints.
The per-cast style/palette/lighting language is carried over from kilix-land's
four approved social-room prompts so the desktop reads as the same world.

Composition lines bake in the shared hotspot layout from
assets/world/world.json: every style must put the same furniture in the same
logical regions so one hotspot map fits all four plate sets.
"""

CASTS = {
    "legend": {
        "world": "Legend of Kilix (Hearthside)",
        "style": ("polished hand-painted pixel art with crisp readable "
                  "clusters and a refined 32-bit adventure-game finish; "
                  "techno-folkloric frontier carpentry of weathered timber, "
                  "dark dressed stone, copper fittings, woven storage, and "
                  "small relay-coil gadgets"),
        "lighting": ("intimate orange hearth-and-lantern light balanced by a "
                     "small cool-blue signal glow; adventurous, communal, "
                     "slightly mechanical"),
        "palette": ("ember orange, oxidized copper, dark teal-blue, walnut "
                    "brown, soot black, muted cream"),
        "avoid": ("photorealism, 3D render, anime character art, extreme "
                  "perspective, blurry painterly edges, modern office "
                  "furniture, Japanese-specific landmarks, recognizable "
                  "copyrighted motifs"),
    },
    "chumrunner": {
        "world": "Chumrunner (Null Market)",
        "style": ("polished hand-painted pixel art with dense but controlled "
                  "cyberpunk detail; a converted maintenance space of "
                  "riveted dark metal, exposed cable bundles, worn polymer "
                  "cases, and cyan terminal glow"),
        "lighting": ("rain-night blue from outside, cyan screen glow, small "
                     "amber task lamps; secretive, practical, humane, "
                     "lived-in rather than sterile"),
        "palette": ("near-black navy, gunmetal, electric cyan, worn teal, "
                    "restrained amber, tiny red warning accents"),
        "avoid": ("photorealism, glossy 3D render, generic spaceship "
                  "bridge, pristine laboratory, excessive neon, extreme "
                  "perspective, blurry painterly edges"),
    },
    "fantasy": {
        "world": "Kilix Fantasy (Emberlight)",
        "style": ("polished hand-painted pixel art with crisp readable "
                  "clusters and a refined 32-bit JRPG interior aesthetic; "
                  "timber-and-stone lodge craft with hanging herbs, pinned "
                  "maps, and tiny magical lanterns"),
        "lighting": ("warm amber hearth and lantern light balanced against "
                     "cool blue twilight; friendly, intimate, gently "
                     "magical"),
        "palette": ("warm walnut, amber, teal, midnight blue, muted moss "
                    "green, small gold accents"),
        "avoid": ("photorealism, 3D render, extreme perspective, blurry "
                  "painterly edges, modern objects, large foreground "
                  "furniture"),
    },
    "pleb-bound": {
        "world": "Pleb Bound (Maple Loop)",
        "style": ("polished hand-painted pixel art with crisp readable "
                  "clusters and cozy illustrated adventure-game warmth; an "
                  "ordinary small-town home of warm wood paneling, painted "
                  "metal, pegboard, houseplants, and playful handcrafted "
                  "analog gadgets"),
        "lighting": ("warm domestic lamps and amber workshop light balanced "
                     "against cool violet-blue rainy dusk outside; safe, "
                     "funny, communal, quietly uncanny"),
        "palette": ("honey wood, warm cream, school-bus yellow accents, "
                    "faded coral, moss green, navy blue, rainy violet"),
        "avoid": ("photorealism, glossy 3D render, grand fantasy "
                  "architecture, futuristic cyberpunk, horror imagery, "
                  "extreme perspective, blurry painterly edges"),
    },
}

# Per-room briefs. {home} is the cast's home flavor word; layout lines place
# furniture in the shared hotspot regions (fractions of the frame width).
ROOMS = {
    "bedroom": {
        "request": "the resident's small private bedroom",
        "scene": ("a snug personal bedroom: a single bed with layered "
                  "covers along the LEFT quarter of the room, a tall "
                  "wardrobe or garment cabinet standing at roughly "
                  "three-quarters of the way across (right of center), a "
                  "clearly framed doorway in the far RIGHT side wall, and a "
                  "small window with a view that fits the world outside; "
                  "modest keepsakes on shelves high on the back wall"),
        "outdoor": False,
    },
    "living": {
        "request": "the shared living room at the heart of the house",
        "scene": ("a common living room with a clearly framed open doorway "
                  "in the far LEFT side wall and another in the far RIGHT "
                  "side wall, plus a wide open doorway at back-center "
                  "leading deeper into the house; an entertainment screen "
                  "or television cabinet on the back wall left of center "
                  "(about one third across), a music player or record "
                  "cabinet right of center (about two thirds across), and "
                  "a wall-mounted handset telephone near the far right; a "
                  "low table sits center-room but small and low"),
        "outdoor": False,
    },
    "study": {
        "request": "the resident's study and workroom",
        "scene": ("a focused study with a clearly framed doorway in the far "
                  "LEFT side wall; along the back wall from left to right: "
                  "a compact filing cabinet at the far left, a tall full "
                  "bookshelf left of center, a glass-front display shelf of "
                  "small models and curios at center, a desk with a "
                  "computer terminal and glowing screen at the right third, "
                  "and a second smaller equipment rig at the far right"),
        "outdoor": False,
    },
    "kitchen": {
        "request": "the small kitchen",
        "scene": ("a compact kitchen whose exit is toward the viewer (no "
                  "visible door in the back wall): counters and a stove "
                  "along the back wall, a pinned notice board mounted at "
                  "back-center with only blank unreadable scraps, a small "
                  "waste bin at the far left, cookware and staples that fit "
                  "the world, and a small window above the counter"),
        "outdoor": False,
    },
    "yard": {
        "request": "the yard just outside the house",
        "scene": ("the outdoor yard seen from outside the home: the house "
                  "facade forms the back of the scene with a clearly "
                  "framed front door at back-center, a post-mounted "
                  "mailbox near the far left, a small storage shed or "
                  "outbuilding filling the right third, and a closed "
                  "gate in the far RIGHT edge leading to the street; "
                  "ground cover, plantings, and sky that fit the world"),
        "outdoor": True,
    },
}

TEMPLATE = """Use case: stylized-concept
Asset type: full-screen 16:9 game room background for a terminal-native pixel-art desktop scene
Primary request: Create an original {request} for the {world} hero's home.
Scene/backdrop: {scene}.
Subject: environment only; absolutely no people, animals, creatures, portraits, silhouettes, mannequins, or character-shaped decorations.
Style/medium: {style}; original design.
Composition/framing: exact wide 16:9 straight-on game backdrop; {framing_wall} and all furniture in the upper two thirds or against the sides; broad clean uncluttered {floor} across the lower third for one moving hero; keep the center foreground clear; avoid a steep top-down angle and avoid large foreground furniture.
Lighting/mood: {lighting}.
Color palette: {palette}.
Constraints: no text, letters, numbers, signage, readable screens, UI, dialogue boxes, borders, logos, watermark, or baked-in characters; environment only; clean sprite-readable floor plane; visual detail must not compete with characters.
Avoid: {avoid}, copied screen composition."""


def plate_prompt(style, room):
    cast = CASTS[style]
    brief = ROOMS[room]
    outdoor = brief["outdoor"]
    return TEMPLATE.format(
        request=brief["request"],
        world=cast["world"],
        scene=brief["scene"],
        style=cast["style"],
        framing_wall=("horizon, facade, and plantings"
                      if outdoor else "back wall and set dressing"),
        floor=("ground plane" if outdoor else "floor"),
        lighting=cast["lighting"],
        palette=cast["palette"],
        avoid=cast["avoid"],
    )


def all_plates():
    for style in ("legend", "chumrunner", "fantasy", "pleb-bound"):
        for room in ("bedroom", "living", "study", "kitchen", "yard"):
            yield style, room, plate_prompt(style, room)


if __name__ == "__main__":
    for style, room, prompt in all_plates():
        print(f"=== {style}/{room} ===")
        print(prompt)
        print()
