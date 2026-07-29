#!/usr/bin/env python3
"""Validate the generated M4 bank, source pins, and WAV runtime contract."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import sys
import wave
from pathlib import Path
from typing import Any, Callable

import audio_plan
import generate_audio


ROOT = Path(__file__).resolve().parents[1]
HEX_256 = re.compile(r"[0-9a-f]{64}\Z")
Probe = Callable[[Path], dict[str, Any]]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_relative(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    path = Path(value)
    return (
        not path.is_absolute()
        and ".." not in path.parts
        and path.as_posix() == value
    )


def _load_json(path: Path, label: str, errors: list[str]) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        errors.append(f"missing regular {label}: {path.name}")
        return {}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        errors.append(f"invalid {label}: {error}")
        return {}
    if not isinstance(payload, dict):
        errors.append(f"{label} root is not an object")
        return {}
    return payload


def _wav_metadata(path: Path) -> dict[str, int]:
    with wave.open(str(path), "rb") as handle:
        return {
            "channels": handle.getnchannels(),
            "sample_rate": handle.getframerate(),
            "sample_width": handle.getsampwidth(),
            "frames": handle.getnframes(),
        }


def _validate_wav(
    root: Path,
    row: dict[str, Any],
    expected_file: str,
    errors: list[str],
) -> None:
    path = root / expected_file
    if path.is_symlink() or not path.is_file():
        errors.append(f"missing regular WAV: {expected_file}")
        return
    if row.get("bytes") != path.stat().st_size:
        errors.append(f"{expected_file}: byte count mismatch")
    digest = row.get("sha256")
    if not isinstance(digest, str) or not HEX_256.fullmatch(digest):
        errors.append(f"{expected_file}: invalid SHA-256")
    elif digest != _sha256(path):
        errors.append(f"{expected_file}: SHA-256 mismatch")
    try:
        metadata = _wav_metadata(path)
    except (OSError, EOFError, wave.Error) as error:
        errors.append(f"{expected_file}: invalid WAV ({error})")
        return
    expected = {
        "channels": audio_plan.CHANNELS,
        "sample_rate": audio_plan.SAMPLE_RATE,
        "sample_width": audio_plan.SAMPLE_WIDTH,
    }
    for key, value in expected.items():
        if metadata[key] != value:
            errors.append(
                f"{expected_file}: {key} is {metadata[key]}, want {value}"
            )
    if metadata["frames"] <= 0:
        errors.append(f"{expected_file}: WAV contains no frames")


def _artifact_plan() -> dict[str, tuple[audio_plan.CueSpec | None, int]]:
    result: dict[str, tuple[audio_plan.CueSpec | None, int]] = {}
    for spec in audio_plan.SFX_SPECS:
        for variation in range(1, spec.variations + 1):
            result[audio_plan.variation_file(spec, variation)] = (
                spec,
                variation,
            )
    for spec in audio_plan.MUSIC_SPECS:
        result[spec.output_file] = (None, 1)
    return result


def _validate_minimax(
    root: Path,
    provenance: dict[str, Any],
    errors: list[str],
    probe: Probe,
) -> None:
    source_path = root / generate_audio.SOURCE_PROVENANCE
    source = _load_json(source_path, "MiniMax source provenance", errors)
    if not source:
        return
    embedded = provenance.get("minimax")
    if embedded != source:
        errors.append("provenance.json does not embed the exact MiniMax ledger")
    masters = source.get("masters")
    if not isinstance(masters, dict):
        errors.append("MiniMax source provenance has no masters object")
        return
    expected_ids = {spec.master_id for spec in audio_plan.MUSIC_SPECS}
    if set(masters) != expected_ids:
        errors.append("MiniMax master inventory differs from the M4 plan")
        return
    for spec in audio_plan.MUSIC_SPECS:
        row = masters[spec.master_id]
        if not isinstance(row, dict):
            errors.append(f"{spec.master_id}: invalid MiniMax row")
            continue
        exact = {
            "provider": "MiniMax Music",
            "model": audio_plan.MINIMAX_MODEL,
            "file": spec.master_file,
            "prompt": spec.prompt,
            "instrumental": True,
            "reference_audio_used": False,
            "vocals_requested": False,
        }
        for key, value in exact.items():
            if row.get(key) != value:
                errors.append(f"{spec.master_id}: wrong MiniMax {key}")
        try:
            generated_on = str(row["generated_on"])
            if dt.date.fromisoformat(generated_on).isoformat() != generated_on:
                raise ValueError
        except (KeyError, ValueError):
            errors.append(f"{spec.master_id}: invalid generation date")
        path = root / spec.master_file
        if path.is_symlink() or not path.is_file():
            errors.append(f"missing regular MiniMax master: {spec.master_file}")
            continue
        if row.get("bytes") != path.stat().st_size:
            errors.append(f"{spec.master_id}: master byte count mismatch")
        if row.get("sha256") != _sha256(path):
            errors.append(f"{spec.master_id}: master SHA-256 mismatch")
        try:
            metadata = probe(path)
            generate_audio._validate_master_probe(spec.master_id, metadata)
        except (
            OSError,
            RuntimeError,
            ValueError,
            json.JSONDecodeError,
        ) as error:
            errors.append(f"{spec.master_id}: {error}")
            continue
        if row.get("audio") != metadata:
            errors.append(f"{spec.master_id}: master metadata mismatch")
    source_root = root / "source" / "minimax"
    actual_masters = {
        path.relative_to(root).as_posix()
        for path in source_root.glob("*.mp3")
        if path.is_file()
    }
    expected_masters = {
        spec.master_file for spec in audio_plan.MUSIC_SPECS
    }
    for relative in sorted(actual_masters - expected_masters):
        errors.append(f"unexpected MiniMax master: {relative}")


def validate(
    root: Path | None = None,
    *,
    probe: Probe = generate_audio.probe_master,
) -> list[str]:
    audio_root = (root or ROOT / "assets" / "audio").resolve()
    errors = [f"plan: {error}" for error in audio_plan.validate_plan()]
    manifest_path = audio_root / "manifest.json"
    provenance_path = audio_root / "provenance.json"
    manifest = _load_json(manifest_path, "manifest.json", errors)
    provenance = _load_json(provenance_path, "provenance.json", errors)
    if not manifest or not provenance:
        return errors

    if manifest.get("schema_version") != 1:
        errors.append("manifest.json: unsupported schema")
    if provenance.get("schema_version") != 1:
        errors.append("provenance.json: unsupported schema")
    if manifest.get("project") != "kilix-land-desktop":
        errors.append("manifest.json: wrong project")
    if provenance.get("project") != "kilix-land-desktop":
        errors.append("provenance.json: wrong project")
    expected_format = {
        "container": "RIFF/WAVE",
        "encoding": "signed PCM",
        "sample_rate": audio_plan.SAMPLE_RATE,
        "channels": audio_plan.CHANNELS,
        "bits_per_sample": audio_plan.SAMPLE_WIDTH * 8,
    }
    if manifest.get("format") != expected_format:
        errors.append("manifest.json: wrong runtime audio format")
    generator_row = manifest.get("generator")
    if not isinstance(generator_row, dict):
        errors.append("manifest.json: missing generator record")
    else:
        if generator_row.get("base_seed") != audio_plan.BASE_SEED:
            errors.append("manifest.json: wrong base seed")
        if generator_row.get("seed_scheme") != audio_plan.SEED_SCHEME:
            errors.append("manifest.json: wrong seed scheme")
        if generator_row.get("network_access") is not False:
            errors.append("manifest.json: build must be offline")

    provenance_pin = manifest.get("provenance")
    if not isinstance(provenance_pin, dict):
        errors.append("manifest.json: missing provenance pin")
    else:
        if provenance_pin.get("file") != "provenance.json":
            errors.append("manifest.json: wrong provenance file")
        if provenance_pin.get("sha256") != _sha256(provenance_path):
            errors.append("manifest.json: provenance SHA-256 mismatch")

    expected = _artifact_plan()
    artifact_rows = manifest.get("artifacts")
    if not isinstance(artifact_rows, list):
        errors.append("manifest.json: artifacts is not an array")
        artifact_rows = []
    seen: set[str] = set()
    for index, raw in enumerate(artifact_rows):
        if not isinstance(raw, dict):
            errors.append(f"artifact[{index}] is not an object")
            continue
        relative = raw.get("file")
        if not _safe_relative(relative):
            errors.append(f"artifact[{index}] has unsafe file path")
            continue
        if relative in seen:
            errors.append(f"duplicate artifact: {relative}")
            continue
        seen.add(relative)
        planned = expected.get(relative)
        if planned is None:
            errors.append(f"unexpected generated artifact: {relative}")
            continue
        spec, variation = planned
        if raw.get("variation") != variation:
            errors.append(f"{relative}: wrong variation number")
        if spec is None:
            music = next(
                item for item in audio_plan.MUSIC_SPECS
                if item.output_file == relative
            )
            if raw.get("cue_id") != music.cue_id:
                errors.append(f"{relative}: wrong music cue id")
            if raw.get("provider") != "minimax_music":
                errors.append(f"{relative}: wrong music provider")
            if raw.get("source_cue") != music.master_id:
                errors.append(f"{relative}: wrong music source")
            if raw.get("seed") is not None:
                errors.append(f"{relative}: music must not claim a seed")
            expected_loop = True
            expected_options = {
                "bpm": music.bpm,
                "start_bar": music.start_bar,
                "loop_bars": music.loop_bars,
                "fold_bars": music.fold_bars,
                "start_seconds": round(music.start_seconds, 6),
                "loop_seconds": round(music.loop_seconds, 6),
                "fold_seconds": round(music.fold_seconds, 6),
            }
        else:
            if raw.get("cue_id") != spec.cue_id:
                errors.append(f"{relative}: wrong cue id")
            if raw.get("provider") != spec.provider:
                errors.append(f"{relative}: wrong provider")
            if raw.get("source_cue") != spec.source_cue:
                errors.append(f"{relative}: wrong source cue")
            expected_seed = audio_plan.derive_seed(
                provider=spec.provider,
                cue=spec.source_cue,
                filename=spec.cue_id,
                variation=variation,
            )
            if raw.get("seed") != expected_seed:
                errors.append(f"{relative}: wrong stable-v2 seed")
            expected_loop = spec.loop
            expected_options = spec.options
        if raw.get("loop") is not expected_loop:
            errors.append(f"{relative}: wrong loop flag")
        if raw.get("options") != expected_options:
            errors.append(f"{relative}: render options differ from plan")
        if expected_loop:
            seam = raw.get("seam")
            if not isinstance(seam, dict) or seam.get("seamless") is not True:
                errors.append(f"{relative}: loop has no passing seam record")
        _validate_wav(audio_root, raw, relative, errors)
    missing = sorted(set(expected) - seen)
    for relative in missing:
        errors.append(f"missing generated artifact row: {relative}")

    generated_root = audio_root / "generated"
    actual_generated = {
        path.relative_to(audio_root).as_posix()
        for path in generated_root.rglob("*.wav")
    } if generated_root.is_dir() else set()
    for relative in sorted(set(expected) - actual_generated):
        errors.append(f"missing generated WAV: {relative}")
    for relative in sorted(actual_generated - set(expected)):
        errors.append(f"unexpected generated WAV: {relative}")

    expected_cues = {
        *(spec.cue_id for spec in audio_plan.SFX_SPECS),
        *(spec.cue_id for spec in audio_plan.MUSIC_SPECS),
    }
    cues = manifest.get("cues")
    if not isinstance(cues, dict) or set(cues) != expected_cues:
        errors.append("manifest.json: cue index differs from M4 plan")
    else:
        expected_cue_files = {
            spec.cue_id: [
                audio_plan.variation_file(spec, variation)
                for variation in range(1, spec.variations + 1)
            ]
            for spec in audio_plan.SFX_SPECS
        }
        expected_cue_files.update({
            spec.cue_id: [spec.output_file]
            for spec in audio_plan.MUSIC_SPECS
        })
        for cue_id, files in expected_cue_files.items():
            row = cues[cue_id]
            if not isinstance(row, dict) or row.get("files") != files:
                errors.append(f"{cue_id}: cue-index files differ from plan")

    parity_rows = manifest.get("parity_artifacts")
    if not isinstance(parity_rows, list):
        errors.append("manifest.json: parity_artifacts is not an array")
        parity_rows = []
    parity_seen: set[str] = set()
    for index, raw in enumerate(parity_rows):
        if not isinstance(raw, dict):
            errors.append(f"parity_artifact[{index}] is not an object")
            continue
        relative = raw.get("file")
        if relative not in audio_plan.PARITY_FILES:
            errors.append(f"unexpected parity artifact: {relative}")
            continue
        if relative in parity_seen:
            errors.append(f"duplicate parity artifact: {relative}")
            continue
        parity_seen.add(relative)
        _validate_wav(audio_root, raw, relative, errors)
    for relative in sorted(set(audio_plan.PARITY_FILES) - parity_seen):
        errors.append(f"missing parity artifact row: {relative}")

    parity_provenance = provenance.get("parity")
    if not isinstance(parity_provenance, dict):
        errors.append("provenance.json: missing parity record")
    elif parity_provenance.get("artifacts") != parity_rows:
        errors.append("parity manifest and provenance records differ")

    soundgen = provenance.get("soundgen")
    if not isinstance(soundgen, dict):
        errors.append("provenance.json: missing soundgen record")
    else:
        if soundgen.get("seed_scheme") != audio_plan.SEED_SCHEME:
            errors.append("provenance.json: wrong seed scheme")
        if soundgen.get("base_seed") != audio_plan.BASE_SEED:
            errors.append("provenance.json: wrong base seed")
        revision = soundgen.get("revision")
        if (
            not isinstance(revision, str)
            or not re.fullmatch(r"[0-9a-f]{40}", revision)
        ):
            errors.append("provenance.json: invalid soundgen revision")
        if (
            isinstance(generator_row, dict)
            and generator_row.get("soundgen_revision") != revision
        ):
            errors.append(
                "manifest and provenance soundgen revisions differ"
            )
        sources = soundgen.get("recording_sources")
        ledgers = soundgen.get("authoritative_ledgers")
        if not isinstance(sources, dict) or not isinstance(ledgers, dict):
            errors.append("provenance.json: invalid recording-source ledgers")
        else:
            for logical, row in sources.items():
                if not _safe_relative(logical) or not isinstance(row, dict):
                    errors.append(f"invalid recording source row: {logical}")
                    continue
                if not HEX_256.fullmatch(str(row.get("sha256", ""))):
                    errors.append(f"{logical}: invalid recording SHA-256")
                ledger = row.get("provenance_ledger")
                if ledger not in ledgers:
                    errors.append(f"{logical}: missing authoritative ledger")

    _validate_minimax(audio_root, provenance, errors, probe)
    return errors


def validate_source_brief(root: Path | None = None) -> list[str]:
    audio_root = (root or ROOT / "assets" / "audio").resolve()
    errors = audio_plan.validate_plan()
    readme = audio_root / "source" / "minimax" / "README.md"
    if not readme.is_file():
        errors.append("missing source/minimax/README.md")
        return errors
    text = readme.read_text(encoding="utf-8")
    if audio_plan.MINIMAX_MODEL not in text:
        errors.append("MiniMax README does not pin the model")
    for spec in audio_plan.MUSIC_SPECS:
        if spec.master_file not in text:
            errors.append(
                f"MiniMax README does not name {spec.master_file}"
            )
        if spec.prompt not in text:
            errors.append(
                f"MiniMax README does not contain verbatim {spec.master_id} prompt"
            )
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=ROOT / "assets" / "audio",
        help="audio root to validate",
    )
    parser.add_argument(
        "--plan",
        action="store_true",
        help="validate only the ungenerated cue/source plan",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    errors = (
        validate_source_brief(arguments.root)
        if arguments.plan
        else validate(arguments.root)
    )
    for error in errors:
        print(f"audio: {error}", file=sys.stderr)
    if errors:
        return 1
    if arguments.plan:
        print(
            "audio plan: OK "
            f"({len(audio_plan.expected_generated_files())} planned WAVs, "
            f"{len(audio_plan.MUSIC_SPECS)} retained masters)"
        )
    else:
        print(
            "audio: OK "
            f"({len(audio_plan.expected_generated_files())} generated WAVs, "
            f"{len(audio_plan.PARITY_FILES)} parity WAVs)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
