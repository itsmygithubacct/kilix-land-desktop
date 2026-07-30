#!/usr/bin/env python3
"""Build Kilix Land's deterministic M4 audio bank.

The build is deliberately offline: sound effects come from the local
python_sound_generator checkout and music is decoded from reviewed MiniMax
masters already committed under ``assets/audio/source/minimax``.  This tool
never calls MiniMax or any other network service.
"""

from __future__ import annotations

import argparse
import array
import datetime as dt
import hashlib
import json
import math
import os
import random
import subprocess
import sys
import tempfile
import wave
from pathlib import Path
from typing import Any, Callable, Sequence

import audio_plan


ROOT = Path(__file__).resolve().parents[1]
SOURCE_PROVENANCE = Path("source/minimax/provenance.json")
Probe = Callable[[Path], dict[str, Any]]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.tmp.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        directory = os.open(
            path.parent, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY
        )
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def probe_master(path: Path) -> dict[str, Any]:
    """Read the retained master contract without decoding the full track."""
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=codec_name,sample_rate,bit_rate,channels",
            "-of",
            "json",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    payload = json.loads(result.stdout)
    streams = payload.get("streams")
    if not isinstance(streams, list) or len(streams) != 1:
        raise ValueError(f"{path}: expected exactly one audio stream")
    stream = streams[0]
    try:
        return {
            "codec": str(stream["codec_name"]),
            "sample_rate": int(stream["sample_rate"]),
            "bitrate": int(stream["bit_rate"]),
            "channels": int(stream["channels"]),
        }
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"{path}: incomplete ffprobe audio metadata") from error


def _validate_master_probe(master_id: str, metadata: dict[str, Any]) -> None:
    if metadata.get("codec") != "mp3":
        raise ValueError(f"{master_id}: retained master is not MP3")
    if metadata.get("sample_rate") != audio_plan.SAMPLE_RATE:
        raise ValueError(
            f"{master_id}: retained master is not "
            f"{audio_plan.SAMPLE_RATE} Hz"
        )
    bitrate = metadata.get("bitrate")
    if not isinstance(bitrate, int) or not 250_000 <= bitrate <= 262_000:
        raise ValueError(
            f"{master_id}: retained master is not approximately 256 kb/s"
        )
    if metadata.get("channels") not in (1, 2):
        raise ValueError(f"{master_id}: retained master has invalid channels")


def record_minimax_sources(
    audio_root: Path,
    generated_on: str,
    *,
    probe: Probe = probe_master,
) -> Path:
    """Hash reviewed masters and create their authoritative source ledger."""
    try:
        parsed_date = dt.date.fromisoformat(generated_on)
    except ValueError as error:
        raise ValueError("generated-on must be an ISO date (YYYY-MM-DD)") from error
    if parsed_date.isoformat() != generated_on:
        raise ValueError("generated-on must be an ISO date (YYYY-MM-DD)")

    masters: dict[str, Any] = {}
    for spec in audio_plan.MUSIC_SPECS:
        path = audio_root / spec.master_file
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"missing regular MiniMax master: {spec.master_file}")
        metadata = probe(path)
        _validate_master_probe(spec.master_id, metadata)
        masters[spec.master_id] = {
            "provider": "MiniMax Music",
            "model": audio_plan.MINIMAX_MODEL,
            "generated_on": generated_on,
            "file": spec.master_file,
            "sha256": sha256_file(path),
            "bytes": path.stat().st_size,
            "prompt": spec.prompt,
            "instrumental": True,
            "reference_audio_used": False,
            "vocals_requested": False,
            "audio": metadata,
        }
    payload = {
        "schema_version": 1,
        "project": "kilix-land-desktop",
        "policy": (
            "Original project-authored prompts, instrumental generation, no "
            "reference audio, and no commercial melody supplied."
        ),
        "masters": masters,
    }
    target = audio_root / SOURCE_PROVENANCE
    _atomic_json(target, payload)
    return target


def load_minimax_sources(
    audio_root: Path,
    *,
    probe: Probe = probe_master,
) -> dict[str, Any]:
    """Verify the committed master ledger against files and the cue plan."""
    ledger_path = audio_root / SOURCE_PROVENANCE
    if ledger_path.is_symlink() or not ledger_path.is_file():
        raise ValueError(
            f"missing MiniMax source ledger: {SOURCE_PROVENANCE.as_posix()}"
        )
    try:
        payload = json.loads(ledger_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid MiniMax source ledger: {error}") from error
    if payload.get("schema_version") != 1:
        raise ValueError("unsupported MiniMax source-ledger schema")
    masters = payload.get("masters")
    if not isinstance(masters, dict):
        raise ValueError("MiniMax source ledger has no masters object")
    expected_ids = {spec.master_id for spec in audio_plan.MUSIC_SPECS}
    if set(masters) != expected_ids:
        raise ValueError("MiniMax source ledger inventory differs from plan")
    for spec in audio_plan.MUSIC_SPECS:
        row = masters[spec.master_id]
        if not isinstance(row, dict):
            raise ValueError(f"{spec.master_id}: invalid source-ledger row")
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
                raise ValueError(
                    f"{spec.master_id}: source ledger has wrong {key}"
                )
        try:
            dt.date.fromisoformat(str(row["generated_on"]))
        except (KeyError, ValueError) as error:
            raise ValueError(
                f"{spec.master_id}: invalid generation date"
            ) from error
        path = audio_root / spec.master_file
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"missing regular MiniMax master: {spec.master_file}")
        if row.get("bytes") != path.stat().st_size:
            raise ValueError(f"{spec.master_id}: source byte count differs")
        if row.get("sha256") != sha256_file(path):
            raise ValueError(f"{spec.master_id}: source checksum differs")
        metadata = probe(path)
        _validate_master_probe(spec.master_id, metadata)
        if row.get("audio") != metadata:
            raise ValueError(f"{spec.master_id}: source audio metadata differs")
    return payload


class SoundGenerator:
    """Thin project-owned adapter over approved soundgen providers."""

    def __init__(self, root: Path) -> None:
        self.root = root.expanduser().resolve()
        package = self.root / "python_sound_generator" / "__init__.py"
        if not package.is_file():
            raise ValueError(
                f"not a python_sound_generator checkout: {self.root}"
            )
        sys.path.insert(0, str(self.root))

        from python_sound_generator._audio import assets, dsp, loops, runtime
        from python_sound_generator.generators import (
            cyberpunk_sound_generator,
            door_sound_generator,
            footstep_generator,
            household_appliance_generator,
            quirky_rpg_sound_generator,
            rpg_sound_generator,
        )

        self.assets = assets
        self.dsp = dsp
        self.loops = loops
        self.runtime = runtime
        self.rpg = rpg_sound_generator
        self.cyber = cyberpunk_sound_generator
        self.quirky = quirky_rpg_sound_generator
        self.doors = door_sound_generator
        self.footsteps = footstep_generator
        self.appliances = household_appliance_generator

    def revision(self) -> str:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.root,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    @staticmethod
    def _samples(values: Sequence[float]) -> list[float]:
        return [float(value) for value in values]

    def render(
        self, spec: audio_plan.CueSpec, seed: int
    ) -> tuple[list[float], tuple[str, ...]]:
        options = dict(spec.options)
        if spec.provider == "rpg":
            result = self.rpg.generate_with_sources(
                spec.source_cue, seed=seed, **options
            )
            return self._samples(result.samples), tuple(result.sources)
        if spec.provider == "cyber":
            result = self.cyber.generate_with_sources(
                spec.source_cue, seed=seed, **options
            )
            return self._samples(result.samples), tuple(result.sources)
        if spec.provider == "quirky":
            result = self.quirky.generate_with_sources(
                spec.source_cue, seed=seed, **options
            )
            return self._samples(result.samples), tuple(result.sources)
        if spec.provider == "footsteps":
            audio = self.footsteps.generate_footsteps(
                surface=spec.source_cue,
                steps=1,
                tempo=112.0,
                stereo_width=0.0,
                sample_rate=audio_plan.SAMPLE_RATE,
                seed=seed,
                mono=True,
                **options,
            )
            return self._samples(audio.channels[0]), ()
        if spec.provider == "door":
            audio = self.doors.generate_door_sound(
                action=spec.source_cue,
                seed=seed,
                sample_rate=audio_plan.SAMPLE_RATE,
                stereo=False,
                **options,
            )
            return self._samples(audio.channels[0]), tuple(audio.sources)
        if spec.provider == "appliance":
            result = self.appliances.render(
                spec.source_cue,
                self.appliances.Options(**options),
                random.Random(seed),
            )
            return self._samples(result.samples), tuple(result.sources)
        raise ValueError(f"unknown provider {spec.provider!r}")

    def master(
        self, samples: list[float], peak_db: float, loop: bool
    ) -> list[float]:
        if loop:
            return self.loops.master(samples, peak_db, drive=1.12)
        return self.dsp.master(
            samples,
            peak_db,
            drive=1.12,
            fade_in_ms=0.5,
            fade_out_ms=24.0,
        )


def fold_music_samples(
    decoded: Sequence[float], frame_count: int, fold_frames: int
) -> list[float]:
    """Fold post-boundary audio over the head of a phrase-aligned loop."""
    if frame_count < 2:
        raise ValueError("music loop must contain at least two frames")
    if fold_frames < 2 or fold_frames >= frame_count:
        raise ValueError("music fold must be between 2 frames and loop length")
    required = frame_count + fold_frames
    if len(decoded) < required:
        raise ValueError(
            f"music window is short: wanted {required}, got {len(decoded)}"
        )
    output = [float(value) for value in decoded[:frame_count]]
    for index in range(fold_frames):
        ratio = index / (fold_frames - 1)
        output[index] = (
            output[index] * math.sqrt(ratio)
            + float(decoded[frame_count + index]) * math.sqrt(1.0 - ratio)
        )
    return output


def decode_music(spec: audio_plan.MusicSpec, audio_root: Path) -> list[float]:
    source = audio_root / spec.master_file
    frame_count = round(spec.loop_seconds * audio_plan.SAMPLE_RATE)
    fold_frames = round(spec.fold_seconds * audio_plan.SAMPLE_RATE)
    decode_seconds = spec.loop_seconds + spec.fold_seconds
    result = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-nostdin",
            "-i",
            str(source),
            "-ss",
            f"{spec.start_seconds:.6f}",
            "-t",
            f"{decode_seconds:.6f}",
            "-map",
            "0:a:0",
            "-ac",
            "1",
            "-ar",
            str(audio_plan.SAMPLE_RATE),
            "-f",
            "f32le",
            "-c:a",
            "pcm_f32le",
            "pipe:1",
        ],
        check=True,
        capture_output=True,
    )
    decoded = array.array("f")
    decoded.frombytes(result.stdout)
    if sys.byteorder != "little":
        decoded.byteswap()
    return fold_music_samples(decoded, frame_count, fold_frames)


def _seam(generator: SoundGenerator, samples: list[float]) -> dict[str, Any]:
    measured = generator.loops.seam(samples)
    return {
        "step_ratio": round(measured.step_ratio, 6),
        "level_ratio": round(measured.level_ratio, 6),
        "seamless": measured.is_seamless(),
    }


def _write_wav_atomic(
    generator: SoundGenerator, path: Path, samples: list[float]
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(
        prefix=f".{path.name}.tmp.", dir=path.parent
    )
    os.close(descriptor)
    temporary = Path(name)
    try:
        generator.dsp.write_wav(temporary, samples)
        with temporary.open("rb") as handle:
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _artifact(
    generator: SoundGenerator,
    path: Path,
    relative: str,
    samples: list[float],
    *,
    cue_id: str,
    provider: str,
    source_cue: str,
    variation: int,
    seed: int | None,
    loop: bool,
    sources: Sequence[str],
    options: dict[str, Any],
    seam: dict[str, Any] | None = None,
) -> dict[str, Any]:
    stats = generator.runtime.audio_stats(
        samples, sample_rate=audio_plan.SAMPLE_RATE
    )
    row: dict[str, Any] = {
        "cue_id": cue_id,
        "file": relative,
        "provider": provider,
        "source_cue": source_cue,
        "variation": variation,
        "seed": seed,
        "loop": loop,
        "duration_seconds": round(stats.duration_seconds, 6),
        "peak": round(stats.peak, 8),
        "rms": round(stats.rms, 8),
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
        "recording_sources": list(dict.fromkeys(sources)),
        "options": options,
    }
    if seam is not None:
        row["seam"] = seam
    return row


def _normalise_source(provider: str, source: str) -> str | None:
    if source.startswith(
        ("generator:", "generated-space:", "suppression-balance:")
    ):
        return None
    if "/" in source:
        return source
    owners = {
        "rpg": "rpg_sound_generator",
        "cyber": "cyberpunk_sound_generator",
        "quirky": "quirky_rpg_sound_generator",
        "door": "door_sound_generator",
        "appliance": "household_appliance_generator",
    }
    owner = owners.get(provider)
    return f"{owner}/sources/{source}" if owner else None


def _recording_provenance(
    generator: SoundGenerator, used_sources: set[str]
) -> tuple[dict[str, Any], dict[str, Any]]:
    catalog = generator.assets.AssetCatalog.default()
    provider_root = (
        generator.root / "python_sound_generator" / "generators"
    )
    source_rows: dict[str, Any] = {}
    ledgers: dict[str, Any] = {}
    for logical in sorted(used_sources):
        relative = Path(logical)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or len(relative.parts) != 3
            or relative.parts[1] != "sources"
        ):
            raise ValueError(f"unsafe recording source: {logical}")
        alias = catalog.aliases.get(logical)
        owner = relative.parts[0]
        if alias is not None:
            physical = alias.path
            ledger = alias.provenance
            storage = "shared content-addressed asset catalog"
            if not ledger:
                raise ValueError(f"catalog source has no provenance: {logical}")
        else:
            physical = provider_root / relative
            if not physical.is_file():
                candidates = list(
                    provider_root.glob(f"*/sources/{relative.name}")
                )
                if len(candidates) != 1:
                    raise ValueError(
                        f"recording source is not uniquely owned: {logical}"
                    )
                physical = candidates[0]
                owner = physical.parent.parent.name
                storage = f"shared from {owner}/sources/{relative.name}"
            else:
                storage = "provider-owned source file"
            ledger = f"generators/{owner}/sources/provenance.json"
        if not physical.is_file():
            raise ValueError(f"missing recording source: {logical}")
        ledger_path = (
            generator.root / "python_sound_generator" / str(ledger)
        )
        if not ledger_path.is_file():
            raise ValueError(f"missing source ledger: {ledger}")
        ledgers.setdefault(
            str(ledger),
            json.loads(ledger_path.read_text(encoding="utf-8")),
        )
        source_rows[logical] = {
            "sha256": sha256_file(physical),
            "bytes": physical.stat().st_size,
            "storage": storage,
            "provenance_ledger": str(ledger),
        }
    return source_rows, ledgers


def _validate_parity_file(path: Path) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing regular parity cue: {path}")
    try:
        with wave.open(str(path), "rb") as handle:
            metadata = {
                "channels": handle.getnchannels(),
                "sample_rate": handle.getframerate(),
                "sample_width": handle.getsampwidth(),
                "frames": handle.getnframes(),
            }
    except (OSError, wave.Error) as error:
        raise ValueError(f"invalid parity WAV {path}: {error}") from error
    if metadata["frames"] <= 0:
        raise ValueError(f"empty parity WAV: {path}")
    return {
        "file": path.as_posix(),
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
        "audio": metadata,
    }


def build(
    generator_root: Path,
    audio_root: Path,
    base_seed: int,
    *,
    verbose: bool = True,
) -> None:
    plan_errors = audio_plan.validate_plan()
    if plan_errors:
        raise ValueError("; ".join(plan_errors))
    audio_root = audio_root.expanduser().resolve()
    source_ledger = load_minimax_sources(audio_root)
    generator = SoundGenerator(generator_root)
    artifacts: list[dict[str, Any]] = []
    cue_index: dict[str, Any] = {}
    used_sources: set[str] = set()
    expected_paths: set[Path] = set()

    total = len(audio_plan.expected_generated_files())
    completed = 0
    for spec in audio_plan.SFX_SPECS:
        files = []
        for variation in range(1, spec.variations + 1):
            relative = audio_plan.variation_file(spec, variation)
            target = audio_root / relative
            expected_paths.add(target)
            seed = generator.runtime.derive_seed(
                base_seed,
                provider=spec.provider,
                cue=spec.source_cue,
                filename=spec.cue_id,
                variation=variation,
            )
            expected_seed = audio_plan.derive_seed(
                base_seed=base_seed,
                provider=spec.provider,
                cue=spec.source_cue,
                filename=spec.cue_id,
                variation=variation,
            )
            if seed != expected_seed:
                raise ValueError(
                    "python_sound_generator stable-v2 seed contract changed"
                )
            samples, raw_sources = generator.render(spec, seed)
            samples = generator.master(samples, spec.peak_db, spec.loop)
            seam = _seam(generator, samples) if spec.loop else None
            if seam is not None and not seam["seamless"]:
                raise ValueError(
                    f"generated loop failed seam check: {spec.cue_id}"
                )
            normalised = [
                logical
                for source in raw_sources
                if (
                    logical := _normalise_source(spec.provider, source)
                ) is not None
            ]
            used_sources.update(normalised)
            _write_wav_atomic(generator, target, samples)
            artifacts.append(
                _artifact(
                    generator,
                    target,
                    relative,
                    samples,
                    cue_id=spec.cue_id,
                    provider=spec.provider,
                    source_cue=spec.source_cue,
                    variation=variation,
                    seed=seed,
                    loop=spec.loop,
                    sources=normalised,
                    options=spec.options,
                    seam=seam,
                )
            )
            files.append(relative)
            completed += 1
            if verbose:
                print(f"[{completed:02d}/{total:02d}] {relative}", flush=True)
        cue_index[spec.cue_id] = {
            "description": spec.description,
            "files": files,
            "loop": spec.loop,
            "bus": spec.bus,
            "gain": spec.gain,
            "priority": spec.priority,
            "max_instances": spec.max_instances,
            "cooldown_ms": spec.cooldown_ms,
        }

    for spec in audio_plan.MUSIC_SPECS:
        target = audio_root / spec.output_file
        expected_paths.add(target)
        samples = decode_music(spec, audio_root)
        samples = generator.master(samples, spec.peak_db, True)
        seam = _seam(generator, samples)
        if not seam["seamless"]:
            raise ValueError(
                f"generated music loop failed seam check: {spec.cue_id}"
            )
        _write_wav_atomic(generator, target, samples)
        window = {
            "bpm": spec.bpm,
            "start_bar": spec.start_bar,
            "loop_bars": spec.loop_bars,
            "fold_bars": spec.fold_bars,
            "start_seconds": round(spec.start_seconds, 6),
            "loop_seconds": round(spec.loop_seconds, 6),
            "fold_seconds": round(spec.fold_seconds, 6),
        }
        artifacts.append(
            _artifact(
                generator,
                target,
                spec.output_file,
                samples,
                cue_id=spec.cue_id,
                provider="minimax_music",
                source_cue=spec.master_id,
                variation=1,
                seed=None,
                loop=True,
                sources=(),
                options=window,
                seam=seam,
            )
        )
        cue_index[spec.cue_id] = {
            "description": spec.description,
            "files": [spec.output_file],
            "loop": True,
            "bus": "music",
            "gain": spec.gain,
            "priority": 5,
            "max_instances": 1,
            "cooldown_ms": 0,
        }
        completed += 1
        if verbose:
            print(
                f"[{completed:02d}/{total:02d}] {spec.output_file}",
                flush=True,
            )

    generated_root = audio_root / "generated"
    if generated_root.is_dir():
        for stale in generated_root.rglob("*.wav"):
            if stale not in expected_paths:
                stale.unlink()

    recording_sources, authoritative_ledgers = _recording_provenance(
        generator, used_sources
    )
    parity = [
        _validate_parity_file(audio_root / relative)
        for relative in audio_plan.PARITY_FILES
    ]
    for row, relative in zip(parity, audio_plan.PARITY_FILES):
        row["file"] = relative

    revision = generator.revision()
    provenance = {
        "schema_version": 1,
        "project": "kilix-land-desktop",
        "policy": (
            "Production audio uses original procedural synthesis, approved "
            "CC0/public-domain recording inputs, project-authored instrumental "
            "MiniMax masters with no reference audio, and parity-managed cast "
            "cues. No commercial reference-game audio is a synthesis input."
        ),
        "soundgen": {
            "repository": (
                "https://github.com/itsmygithubacct/python-sound-generator"
            ),
            "revision": revision,
            "base_seed": base_seed,
            "seed_scheme": audio_plan.SEED_SCHEME,
            "recording_sources": recording_sources,
            "authoritative_ledgers": authoritative_ledgers,
        },
        "minimax": source_ledger,
        "parity": {
            "policy": (
                "Byte-identical committed-history copies maintained by "
                "tools/sync_source_parity.py."
            ),
            "artifacts": parity,
        },
    }
    provenance_path = audio_root / "provenance.json"
    _atomic_json(provenance_path, provenance)

    manifest = {
        "schema_version": 1,
        "project": "kilix-land-desktop",
        "generator": {
            "name": "tools/generate_audio.py",
            "soundgen_revision": revision,
            "base_seed": base_seed,
            "seed_scheme": audio_plan.SEED_SCHEME,
            "rebuild": "python3 tools/generate_audio.py build",
            "network_access": False,
        },
        "format": {
            "container": "RIFF/WAVE",
            "encoding": "signed PCM",
            "sample_rate": audio_plan.SAMPLE_RATE,
            "channels": audio_plan.CHANNELS,
            "bits_per_sample": audio_plan.SAMPLE_WIDTH * 8,
        },
        "counts": {
            "logical_generated_cues": len(cue_index),
            "generated_wav_files": len(artifacts),
            "parity_wav_files": len(parity),
            "minimax_masters": len(audio_plan.MUSIC_SPECS),
            "recording_inputs": len(recording_sources),
        },
        "buses": {
            "music": {"default_gain": 0.58, "duck_on_external_focus": True},
            "ambience": {"default_gain": 0.44},
            "sfx": {"default_gain": 1.0},
        },
        "cues": cue_index,
        "artifacts": artifacts,
        "parity_artifacts": parity,
        "provenance": {
            "file": "provenance.json",
            "sha256": sha256_file(provenance_path),
        },
    }
    manifest_path = audio_root / "manifest.json"
    _atomic_json(manifest_path, manifest)
    print(f"sha256 {sha256_file(manifest_path)}  manifest.json")
    print(f"sha256 {sha256_file(provenance_path)}  provenance.json")
    print(
        f"Generated {len(artifacts)} WAV files for {len(cue_index)} "
        f"logical cues; retained {len(parity)} parity cues."
    )


def _default_generator_root() -> Path:
    source_home = os.environ.get("GPU_TERMINAL_SOURCE_HOME")
    if source_home:
        return (
            Path(source_home).expanduser()
            / "kilix-apps"
            / "python_sound_generator"
        )
    return (
        Path.home()
        / "gpu_terminal"
        / "kilix-apps"
        / "python_sound_generator"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action",
        nargs="?",
        choices=("build", "requests", "record-minimax"),
        default="build",
    )
    parser.add_argument(
        "--generator-root",
        type=Path,
        default=_default_generator_root(),
        help="python_sound_generator checkout",
    )
    parser.add_argument(
        "--audio-root",
        type=Path,
        default=ROOT / "assets" / "audio",
        help="project audio root",
    )
    parser.add_argument("--seed", type=int, default=audio_plan.BASE_SEED)
    parser.add_argument(
        "--generated-on",
        help="ISO generation date required by record-minimax",
    )
    parser.add_argument(
        "--quiet", action="store_true", help="suppress per-file progress"
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    try:
        if arguments.action == "requests":
            print(
                json.dumps(audio_plan.minimax_request_payload(), indent=2)
                + "\n",
                end="",
            )
        elif arguments.action == "record-minimax":
            if not arguments.generated_on:
                raise ValueError(
                    "record-minimax requires --generated-on YYYY-MM-DD"
                )
            target = record_minimax_sources(
                arguments.audio_root, arguments.generated_on
            )
            print(f"Recorded reviewed MiniMax masters in {target}")
        else:
            build(
                arguments.generator_root,
                arguments.audio_root,
                arguments.seed,
                verbose=not arguments.quiet,
            )
    except (
        ImportError,
        OSError,
        RuntimeError,
        ValueError,
        json.JSONDecodeError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"generate_audio.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
