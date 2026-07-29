#!/usr/bin/env python3
"""Authoritative M4 cue map and MiniMax request plan.

This module is deliberately network-free.  ``tools/generate_audio.py`` owns
rendering and provenance, while this file owns stable cue ids, output paths,
provider choices, seeds, and the exact project-authored music prompts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


BASE_SEED = 0x4B494C49584C414E  # "KILIXLAN"; stable-v2 derives every take.
SAMPLE_RATE = 44_100
SAMPLE_WIDTH = 2
CHANNELS = 1
MINIMAX_MODEL = "music-2.5+"
MINIMAX_BITRATE = 256_000
MINIMAX_CLI = "~/minimax_asset_generation/minimax/minimax_music.py"
SEED_SCHEME = "stable-v2"
STYLES = ("legend", "chumrunner", "fantasy", "pleb-bound")
UI_ACTIONS = ("launch", "confirm", "deny", "toast")
FOOTSTEP_AREAS = ("home", "yard")
DOOR_ACTIONS = ("open", "close")


@dataclass(frozen=True, slots=True)
class CueSpec:
    cue_id: str
    file: str
    description: str
    provider: str
    source_cue: str
    options: dict[str, Any] = field(default_factory=dict)
    variations: int = 1
    loop: bool = False
    bus: str = "sfx"
    gain: float = 1.0
    peak_db: float = -8.0
    priority: int = 50
    max_instances: int = 4
    cooldown_ms: int = 0


@dataclass(frozen=True, slots=True)
class MusicSpec:
    master_id: str
    cue_id: str
    master_file: str
    output_file: str
    description: str
    bpm: float
    start_bar: int
    loop_bars: int
    fold_bars: int
    prompt: str
    gain: float = 0.58
    peak_db: float = -12.0

    @property
    def bar_seconds(self) -> float:
        return 240.0 / self.bpm

    @property
    def start_seconds(self) -> float:
        return self.start_bar * self.bar_seconds

    @property
    def loop_seconds(self) -> float:
        return self.loop_bars * self.bar_seconds

    @property
    def fold_seconds(self) -> float:
        return self.fold_bars * self.bar_seconds


def _cue(
    cue_id: str,
    file: str,
    description: str,
    provider: str,
    source_cue: str,
    *,
    options: dict[str, Any] | None = None,
    variations: int = 1,
    loop: bool = False,
    bus: str = "sfx",
    gain: float = 1.0,
    peak_db: float = -8.0,
    priority: int = 50,
    max_instances: int = 4,
    cooldown_ms: int = 0,
) -> CueSpec:
    return CueSpec(
        cue_id=cue_id,
        file=file,
        description=description,
        provider=provider,
        source_cue=source_cue,
        options=dict(options or {}),
        variations=variations,
        loop=loop,
        bus=bus,
        gain=gain,
        peak_db=peak_db,
        priority=priority,
        max_instances=max_instances,
        cooldown_ms=cooldown_ms,
    )


def _footstep_specs() -> tuple[CueSpec, ...]:
    profiles = {
        "legend": {
            "home": ("wood", "boot"),
            "yard": ("grass", "boot"),
        },
        "chumrunner": {
            "home": ("concrete", "sneaker"),
            "yard": ("gravel", "sneaker"),
        },
        "fantasy": {
            "home": ("wood", "hard_shoe"),
            "yard": ("dirt", "hard_shoe"),
        },
        "pleb-bound": {
            "home": ("carpet", "sneaker"),
            "yard": ("grass", "sneaker"),
        },
    }
    return tuple(
        _cue(
            f"footstep.{style}.{area}",
            f"generated/sfx/footsteps/{style}/{area}.wav",
            f"{style} {area} walking footfall.",
            "footsteps",
            surface,
            options={
                "footwear": footwear,
                "intensity": 0.64,
                "variation": 0.82,
            },
            variations=4,
            peak_db=-11.0,
            priority=45,
            max_instances=2,
            cooldown_ms=65,
        )
        for style in STYLES
        for area, (surface, footwear) in profiles[style].items()
    )


def _door_specs() -> tuple[CueSpec, ...]:
    profiles = {
        "legend": ("heavy_wood", 0.72),
        "chumrunner": ("metal", 0.38),
        "fantasy": ("heavy_wood", 0.84),
        "pleb-bound": ("hollow_wood", 0.42),
    }
    return tuple(
        _cue(
            f"door.{style}.{action}",
            f"generated/sfx/doors/{style}/{action}.wav",
            f"{action.title()} the {style} house door.",
            "door",
            action,
            options={
                "material": profiles[style][0],
                "intensity": 0.72 if action == "open" else 0.78,
                "age": profiles[style][1],
                "room": 0.08,
            },
            variations=2,
            peak_db=-8.0,
            priority=65,
            max_instances=2,
            cooldown_ms=180,
        )
        for style in STYLES
        for action in DOOR_ACTIONS
    )


def _ui_specs() -> tuple[CueSpec, ...]:
    profiles = {
        "legend": (
            "rpg",
            {"theme": "retro", "intensity": 0.72},
            {
                "launch": "world_shift",
                "confirm": "menu_confirm",
                "deny": "menu_error",
                "toast": "quest_accept",
            },
        ),
        "chumrunner": (
            "cyber",
            {"style": "noir", "intensity": 0.72},
            {
                "launch": "connection_open",
                "confirm": "terminal_confirm",
                "deny": "terminal_error",
                "toast": "objective_update",
            },
        ),
        "fantasy": (
            "rpg",
            {"theme": "fantasy", "intensity": 0.72},
            {
                "launch": "world_shift",
                "confirm": "menu_confirm",
                "deny": "menu_error",
                "toast": "quest_accept",
            },
        ),
        "pleb-bound": (
            "quirky",
            {"style": "suburban", "intensity": 0.72},
            {
                "launch": "door_transition",
                "confirm": "menu_confirm",
                "deny": "menu_error",
                "toast": "exclamation",
            },
        ),
    }
    rows = []
    for style in STYLES:
        provider, options, cues = profiles[style]
        for action in UI_ACTIONS:
            rows.append(
                _cue(
                    f"desktop.{style}.{action}",
                    f"generated/sfx/desktop/{style}/{action}.wav",
                    f"{style} feedback for desktop {action}.",
                    provider,
                    cues[action],
                    options=options,
                    peak_db=-10.0 if action == "toast" else -8.0,
                    priority=75 if action in ("confirm", "deny") else 65,
                    cooldown_ms=60 if action == "toast" else 0,
                )
            )
    return tuple(rows)


def _appliance_specs() -> tuple[CueSpec, ...]:
    return (
        _cue(
            "appliance.kitchen-hum",
            "generated/sfx/appliances/kitchen-hum.wav",
            "Quiet range-hood bed for the kitchen.",
            "appliance",
            "range_hood",
            options={"seconds": 5.0, "speed": 0.96, "intensity": 0.32},
            loop=True,
            bus="ambience",
            gain=0.34,
            peak_db=-18.0,
            priority=15,
            max_instances=1,
        ),
        _cue(
            "appliance.house-hum",
            "generated/sfx/appliances/house-hum.wav",
            "Quiet ventilation bed for indoor rooms.",
            "appliance",
            "air_conditioner",
            options={"seconds": 6.0, "speed": 0.98, "intensity": 0.28},
            loop=True,
            bus="ambience",
            gain=0.28,
            peak_db=-20.0,
            priority=10,
            max_instances=1,
        ),
    )


SFX_SPECS = (
    *_footstep_specs(),
    *_door_specs(),
    *_appliance_specs(),
    *_ui_specs(),
)


MUSIC_SPECS = (
    MusicSpec(
        "legend-home",
        "music.legend.home",
        "source/minimax/legend-home.mp3",
        "generated/music/legend-home.wav",
        "Hearthside cottage room ambience.",
        104.0,
        4,
        16,
        2,
        (
            "Original instrumental home ambience for a warm post-industrial "
            "pixel-art cottage. Psychedelic folk-electronica with plucked "
            "dulcimer, muted guitar, woody bass, hand percussion, and soft "
            "luminous analog synthesizer. Cozy, curious, lightly adventurous, "
            "and unobtrusive beneath dialogue. 104 BPM in 4/4 with phrase "
            "changes exactly every four bars and a stable sixteen-bar middle "
            "section suitable for looping. Immediate musical opening, steady "
            "energy, no long intro, no finale, no fade, no vocals, no lyrics, "
            "no sound effects, and no reference to any existing melody."
        ),
    ),
    MusicSpec(
        "chumrunner-home",
        "music.chumrunner.home",
        "source/minimax/chumrunner-home.mp3",
        "generated/music/chumrunner-home.wav",
        "Null safehouse room ambience.",
        112.0,
        4,
        16,
        2,
        (
            "Original instrumental cyberpunk safehouse ambience for a neon "
            "noir apartment. Restrained downtempo electro with dry drum "
            "machine, rounded sub bass, glassy FM keys, filtered arpeggios, "
            "and sparse noir electric piano. Alert but safe, technological "
            "without harshness, and unobtrusive beneath dialogue. 112 BPM in "
            "4/4 with phrase changes exactly every four bars and a stable "
            "sixteen-bar middle section suitable for looping. Immediate "
            "musical opening, steady energy, no long intro, no finale, no "
            "fade, no vocals, no lyrics, no sound effects, and no reference "
            "to any existing melody."
        ),
    ),
    MusicSpec(
        "fantasy-home",
        "music.fantasy.home",
        "source/minimax/fantasy-home.mp3",
        "generated/music/fantasy-home.wav",
        "Emberlight lodge room ambience.",
        96.0,
        4,
        16,
        2,
        (
            "Original instrumental fantasy lodge ambience with the clarity "
            "of a classic sixteen-bit role-playing game. Gentle lute, harp, "
            "celesta, warm strings, wooden flute, and restrained frame drum, "
            "with an original hopeful modal motif. Hearth-lit, magical, "
            "domestic, and unobtrusive beneath dialogue. 96 BPM in 4/4 with "
            "phrase changes exactly every four bars and a stable sixteen-bar "
            "middle section suitable for looping. Immediate musical opening, "
            "steady energy, no long intro, no finale, no fade, no vocals, no "
            "lyrics, no sound effects, and no reference to any existing "
            "melody."
        ),
    ),
    MusicSpec(
        "pleb-bound-home",
        "music.pleb-bound.home",
        "source/minimax/pleb-bound-home.mp3",
        "generated/music/pleb-bound-home.wav",
        "Maple suburban house ambience.",
        108.0,
        4,
        16,
        2,
        (
            "Original instrumental quirky suburban role-playing home theme. "
            "Friendly lo-fi electric piano, clean guitar, melodic bass, tiny "
            "toy percussion, brushed drums, and warm cartridge-like synth "
            "colors. Everyday, gently funny, sincere, and unobtrusive beneath "
            "dialogue. 108 BPM in 4/4 with phrase changes exactly every four "
            "bars and a stable sixteen-bar middle section suitable for "
            "looping. Immediate musical opening, steady energy, no long "
            "intro, no finale, no fade, no vocals, no lyrics, no sound "
            "effects, and no reference to any existing melody."
        ),
    ),
    MusicSpec(
        "shared-yard",
        "music.shared.yard",
        "source/minimax/shared-yard.mp3",
        "generated/music/shared-yard.wav",
        "Cross-style yard ambience.",
        100.0,
        4,
        16,
        2,
        (
            "Original instrumental outdoor home-yard theme that can bridge "
            "pixel folk, cyberpunk, fantasy, and quirky suburban art styles. "
            "Acoustic guitar harmonics, marimba, soft hand percussion, warm "
            "bass, airy flute, and subtle analog pads in a calm original "
            "motif. Open-air, neighborly, exploratory, and unobtrusive beneath "
            "dialogue. 100 BPM in 4/4 with phrase changes exactly every four "
            "bars and a stable sixteen-bar middle section suitable for "
            "looping. Immediate musical opening, steady energy, no long "
            "intro, no finale, no fade, no vocals, no lyrics, no sound "
            "effects, and no reference to any existing melody."
        ),
    ),
)


PARITY_FILES = tuple(
    f"{style}/{cue}.wav"
    for style in STYLES
    for cue in ("ui-move", "ui-confirm", "dialogue")
)


def variation_file(spec: CueSpec, variation: int) -> str:
    if not 1 <= variation <= spec.variations:
        raise ValueError("variation is outside the cue's declared range")
    if spec.variations == 1:
        return spec.file
    path = Path(spec.file)
    return (path.parent / f"{path.stem}_{variation:02d}{path.suffix}").as_posix()


def derive_seed(
    *,
    provider: str,
    cue: str,
    filename: str,
    variation: int,
    base_seed: int = BASE_SEED,
) -> int:
    """Independent copy of soundgen's stable-v2 public seed contract."""
    if variation < 1:
        raise ValueError("variation must be at least 1")
    identity = json.dumps(
        {
            "base_seed": int(base_seed),
            "cue": cue,
            "filename": Path(filename).as_posix(),
            "provider": provider,
            "variation": variation,
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    digest = hashlib.blake2b(
        identity, digest_size=8, person=b"sndgen-v2"
    ).digest()
    return int.from_bytes(digest, "big") & ((1 << 63) - 1)


def expected_generated_files() -> tuple[str, ...]:
    return tuple(
        variation_file(spec, variation)
        for spec in SFX_SPECS
        for variation in range(1, spec.variations + 1)
    ) + tuple(spec.output_file for spec in MUSIC_SPECS)


def minimax_request_payload() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "provider": "MiniMax Music",
        "model": MINIMAX_MODEL,
        "instrumental": True,
        "audio": {
            "format": "mp3",
            "sample_rate": SAMPLE_RATE,
            "bitrate": MINIMAX_BITRATE,
        },
        "requests": [
            {
                "master_id": spec.master_id,
                "source_file": spec.master_file,
                "output": f"assets/audio/{spec.master_file}",
                "prompt": spec.prompt,
                "command": [
                    "python3",
                    MINIMAX_CLI,
                    "generate",
                    "--instrumental",
                    "--model",
                    MINIMAX_MODEL,
                    "--audio-format",
                    "mp3",
                    "--sample-rate",
                    str(SAMPLE_RATE),
                    "--bitrate",
                    str(MINIMAX_BITRATE),
                    "--prompt",
                    spec.prompt,
                    "--output",
                    f"assets/audio/{spec.master_file}",
                ],
                "loop_window": {
                    "bpm": spec.bpm,
                    "start_bar": spec.start_bar,
                    "loop_bars": spec.loop_bars,
                    "fold_bars": spec.fold_bars,
                    "start_seconds": round(spec.start_seconds, 6),
                    "loop_seconds": round(spec.loop_seconds, 6),
                    "fold_seconds": round(spec.fold_seconds, 6),
                },
            }
            for spec in MUSIC_SPECS
        ],
    }


def _safe_relative(path: str) -> bool:
    candidate = Path(path)
    return (
        bool(path)
        and not candidate.is_absolute()
        and ".." not in candidate.parts
        and candidate.as_posix() == path
    )


def validate_plan() -> list[str]:
    errors: list[str] = []
    cue_ids = [spec.cue_id for spec in SFX_SPECS]
    cue_ids.extend(spec.cue_id for spec in MUSIC_SPECS)
    if len(cue_ids) != len(set(cue_ids)):
        errors.append("cue ids are not unique")

    files = list(expected_generated_files())
    if len(files) != len(set(files)):
        errors.append("generated output paths are not unique")
    for path in (*files, *PARITY_FILES):
        if not _safe_relative(path):
            errors.append(f"unsafe output path: {path}")
    for spec in SFX_SPECS:
        if spec.provider not in {
            "rpg", "cyber", "quirky", "footsteps", "door", "appliance"
        }:
            errors.append(
                f"{spec.cue_id}: unknown provider {spec.provider!r}"
            )
        if spec.variations < 1:
            errors.append(f"{spec.cue_id}: variations must be positive")
        if not spec.file.endswith(".wav"):
            errors.append(f"{spec.cue_id}: output is not WAV")

    required = {
        *(f"footstep.{style}.{area}"
          for style in STYLES for area in FOOTSTEP_AREAS),
        *(f"door.{style}.{action}"
          for style in STYLES for action in DOOR_ACTIONS),
        *(f"desktop.{style}.{action}"
          for style in STYLES for action in UI_ACTIONS),
        "appliance.kitchen-hum",
        "appliance.house-hum",
        *(f"music.{style}.home" for style in STYLES),
        "music.shared.yard",
    }
    missing = sorted(required - set(cue_ids))
    if missing:
        errors.append(f"required M4 cues are missing: {', '.join(missing)}")

    expected_ui_providers = {
        "legend": "rpg",
        "chumrunner": "cyber",
        "fantasy": "rpg",
        "pleb-bound": "quirky",
    }
    by_id = {spec.cue_id: spec for spec in SFX_SPECS}
    for style, provider in expected_ui_providers.items():
        for action in UI_ACTIONS:
            cue_id = f"desktop.{style}.{action}"
            if cue_id in by_id and by_id[cue_id].provider != provider:
                errors.append(
                    f"{cue_id}: provider must be {provider}"
                )

    master_ids = {spec.master_id for spec in MUSIC_SPECS}
    if master_ids != {
        "legend-home", "chumrunner-home", "fantasy-home",
        "pleb-bound-home", "shared-yard",
    }:
        errors.append("MiniMax plan must contain four style masters and yard")
    for spec in MUSIC_SPECS:
        if not _safe_relative(spec.master_file):
            errors.append(f"{spec.master_id}: unsafe master path")
        if not spec.master_file.endswith(".mp3"):
            errors.append(f"{spec.master_id}: master is not MP3")
        if spec.start_bar < 0 or spec.loop_bars < 4 or spec.fold_bars < 1:
            errors.append(f"{spec.master_id}: invalid phrase window")
        for phrase in (
            "Original instrumental",
            "no vocals",
            "no lyrics",
            "no sound effects",
            "no reference to any existing melody",
        ):
            if phrase not in spec.prompt:
                errors.append(
                    f"{spec.master_id}: prompt is missing {phrase!r}"
                )
    if len(PARITY_FILES) != 12 or len(set(PARITY_FILES)) != 12:
        errors.append("parity inventory must contain 12 unique cues")
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true", help="validate the static M4 plan"
    )
    parser.add_argument(
        "--json", action="store_true",
        help="print exact MiniMax request records (default)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    errors = validate_plan()
    if errors:
        for error in errors:
            print(f"audio plan: {error}")
        return 1
    if arguments.check:
        print(
            "audio plan: OK "
            f"({len(SFX_SPECS)} logical SFX cues, "
            f"{len(expected_generated_files())} generated WAVs, "
            f"{len(MUSIC_SPECS)} MiniMax masters)"
        )
        return 0
    print(json.dumps(minimax_request_payload(), indent=2) + "\n", end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
