#!/usr/bin/env python3
"""Synthetic tests for the offline M4 audio/provenance pipeline."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import audio_plan  # noqa: E402
import generate_audio  # noqa: E402
import validate_audio  # noqa: E402


MASTER_METADATA = {
    "codec": "mp3",
    "sample_rate": 44_100,
    "bitrate": 256_000,
    "channels": 2,
}


def fake_probe(_path: Path) -> dict[str, int | str]:
    return dict(MASTER_METADATA)


def write_wav(
    path: Path,
    *,
    channels: int = 1,
    sample_rate: int = 44_100,
    sample_width: int = 2,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frame = struct.pack("<h", 1200) * channels
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(sample_width)
        handle.setframerate(sample_rate)
        handle.writeframes(frame * 32)


def artifact_row(
    root: Path,
    relative: str,
    *,
    cue_id: str,
    provider: str,
    source_cue: str,
    variation: int,
    seed: int | None,
    loop: bool,
    options: dict[str, object],
) -> dict[str, object]:
    path = root / relative
    row: dict[str, object] = {
        "cue_id": cue_id,
        "file": relative,
        "provider": provider,
        "source_cue": source_cue,
        "variation": variation,
        "seed": seed,
        "loop": loop,
        "options": options,
        "bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }
    if loop:
        row["seam"] = {
            "step_ratio": 0.0,
            "level_ratio": 1.0,
            "seamless": True,
        }
    return row


class AudioFixture:
    def __init__(self, root: Path):
        self.root = root
        for spec in audio_plan.MUSIC_SPECS:
            path = root / spec.master_file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"synthetic {spec.master_id}".encode())
        self.source_ledger_path = generate_audio.record_minimax_sources(
            root, "2026-07-29", probe=fake_probe
        )
        self.source_ledger = json.loads(
            self.source_ledger_path.read_text(encoding="utf-8")
        )

        self.artifacts: list[dict[str, object]] = []
        self.cues: dict[str, object] = {}
        for spec in audio_plan.SFX_SPECS:
            files = []
            for variation in range(1, spec.variations + 1):
                relative = audio_plan.variation_file(spec, variation)
                write_wav(root / relative)
                seed = audio_plan.derive_seed(
                    provider=spec.provider,
                    cue=spec.source_cue,
                    filename=spec.cue_id,
                    variation=variation,
                )
                self.artifacts.append(
                    artifact_row(
                        root,
                        relative,
                        cue_id=spec.cue_id,
                        provider=spec.provider,
                        source_cue=spec.source_cue,
                        variation=variation,
                        seed=seed,
                        loop=spec.loop,
                        options=spec.options,
                    )
                )
                files.append(relative)
            self.cues[spec.cue_id] = {
                "description": spec.description,
                "files": files,
            }
        for spec in audio_plan.MUSIC_SPECS:
            write_wav(root / spec.output_file)
            window: dict[str, object] = {
                "bpm": spec.bpm,
                "start_bar": spec.start_bar,
                "loop_bars": spec.loop_bars,
                "fold_bars": spec.fold_bars,
                "start_seconds": round(spec.start_seconds, 6),
                "loop_seconds": round(spec.loop_seconds, 6),
                "fold_seconds": round(spec.fold_seconds, 6),
            }
            self.artifacts.append(
                artifact_row(
                    root,
                    spec.output_file,
                    cue_id=spec.cue_id,
                    provider="minimax_music",
                    source_cue=spec.master_id,
                    variation=1,
                    seed=None,
                    loop=True,
                    options=window,
                )
            )
            self.cues[spec.cue_id] = {
                "description": spec.description,
                "files": [spec.output_file],
            }

        self.parity: list[dict[str, object]] = []
        for relative in audio_plan.PARITY_FILES:
            write_wav(root / relative)
            path = root / relative
            self.parity.append(
                {
                    "file": relative,
                    "bytes": path.stat().st_size,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                }
            )
        self.write_indexes()

    def write_indexes(self) -> None:
        provenance = {
            "schema_version": 1,
            "project": "kilix-land-desktop",
            "soundgen": {
                "revision": "a" * 40,
                "base_seed": audio_plan.BASE_SEED,
                "seed_scheme": audio_plan.SEED_SCHEME,
                "recording_sources": {},
                "authoritative_ledgers": {},
            },
            "minimax": self.source_ledger,
            "parity": {
                "policy": "synthetic fixture",
                "artifacts": self.parity,
            },
        }
        provenance_path = self.root / "provenance.json"
        provenance_path.write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        manifest = {
            "schema_version": 1,
            "project": "kilix-land-desktop",
            "generator": {
                "soundgen_revision": "a" * 40,
                "base_seed": audio_plan.BASE_SEED,
                "seed_scheme": audio_plan.SEED_SCHEME,
                "network_access": False,
            },
            "format": {
                "container": "RIFF/WAVE",
                "encoding": "signed PCM",
                "sample_rate": 44_100,
                "channels": 1,
                "bits_per_sample": 16,
            },
            "cues": self.cues,
            "artifacts": self.artifacts,
            "parity_artifacts": self.parity,
            "provenance": {
                "file": "provenance.json",
                "sha256": hashlib.sha256(
                    provenance_path.read_bytes()
                ).hexdigest(),
            },
        }
        (self.root / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )


class AudioPlanTests(unittest.TestCase):
    def test_plan_and_verbatim_source_brief_pass(self) -> None:
        self.assertEqual(audio_plan.validate_plan(), [])
        self.assertEqual(
            validate_audio.validate_source_brief(ROOT / "assets" / "audio"),
            [],
        )
        self.assertEqual(len(audio_plan.MUSIC_SPECS), 5)
        self.assertEqual(len(audio_plan.PARITY_FILES), 12)
        self.assertEqual(len(audio_plan.expected_generated_files()), 71)
        requests = audio_plan.minimax_request_payload()["requests"]
        self.assertTrue(
            all(
                row["output"].startswith("assets/audio/source/minimax/")
                for row in requests
            )
        )

    def test_music_fold_uses_post_boundary_audio_at_the_head(self) -> None:
        decoded = [float(value) for value in range(12)]
        folded = generate_audio.fold_music_samples(decoded, 8, 4)
        self.assertEqual(len(folded), 8)
        self.assertEqual(folded[0], 8.0)
        self.assertEqual(folded[3], 3.0)
        self.assertEqual(folded[4:], decoded[4:8])

    def test_stable_v2_seed_contract_is_pinned(self) -> None:
        self.assertEqual(
            audio_plan.derive_seed(
                provider="footsteps",
                cue="wood",
                filename="footstep.legend.home",
                variation=1,
            ),
            5_672_410_434_095_811_290,
        )

    def test_recorded_master_ledger_pins_exact_inputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="master-ledger-test.") as raw:
            root = Path(raw)
            for spec in audio_plan.MUSIC_SPECS:
                path = root / spec.master_file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(spec.master_id.encode())
            target = generate_audio.record_minimax_sources(
                root, "2026-07-29", probe=fake_probe
            )
            payload = json.loads(target.read_text(encoding="utf-8"))
            legend = payload["masters"]["legend-home"]
            self.assertEqual(legend["model"], "music-2.5+")
            self.assertEqual(
                legend["prompt"], audio_plan.MUSIC_SPECS[0].prompt
            )
            self.assertFalse(legend["reference_audio_used"])
            self.assertEqual(legend["audio"], MASTER_METADATA)
            self.assertEqual(
                generate_audio.load_minimax_sources(
                    root, probe=fake_probe
                ),
                payload,
            )

    def test_recording_rejects_wrong_source_format(self) -> None:
        with tempfile.TemporaryDirectory(prefix="master-format-test.") as raw:
            root = Path(raw)
            for spec in audio_plan.MUSIC_SPECS:
                path = root / spec.master_file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(spec.master_id.encode())

            def bad_probe(_path: Path) -> dict[str, int | str]:
                return {**MASTER_METADATA, "sample_rate": 48_000}

            with self.assertRaisesRegex(ValueError, "not 44100 Hz"):
                generate_audio.record_minimax_sources(
                    root, "2026-07-29", probe=bad_probe
                )


class AudioValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="audio-validation-test."
        )
        self.root = Path(self.temporary.name)
        self.fixture = AudioFixture(self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def errors(self) -> list[str]:
        return validate_audio.validate(self.root, probe=fake_probe)

    def test_complete_synthetic_bank_passes(self) -> None:
        self.assertEqual(self.errors(), [])

    def test_missing_artifact_row_is_reported(self) -> None:
        removed = self.fixture.artifacts.pop()
        self.fixture.write_indexes()
        self.assertIn(
            f"missing generated artifact row: {removed['file']}",
            self.errors(),
        )

    def test_generated_hash_drift_is_rejected(self) -> None:
        relative = audio_plan.expected_generated_files()[0]
        with (self.root / relative).open("ab") as handle:
            handle.write(b"drift")
        self.assertIn(f"{relative}: byte count mismatch", self.errors())
        self.assertIn(f"{relative}: SHA-256 mismatch", self.errors())

    def test_wrong_runtime_wav_contract_is_rejected(self) -> None:
        row = self.fixture.artifacts[0]
        relative = str(row["file"])
        write_wav(self.root / relative, channels=2)
        path = self.root / relative
        row["bytes"] = path.stat().st_size
        row["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.fixture.write_indexes()
        self.assertIn(f"{relative}: channels is 2, want 1", self.errors())

    def test_master_hash_drift_is_rejected(self) -> None:
        spec = audio_plan.MUSIC_SPECS[0]
        with (self.root / spec.master_file).open("ab") as handle:
            handle.write(b"drift")
        errors = self.errors()
        self.assertIn(
            f"{spec.master_id}: master byte count mismatch", errors
        )
        self.assertIn(
            f"{spec.master_id}: master SHA-256 mismatch", errors
        )

    def test_prompt_or_model_drift_is_rejected(self) -> None:
        source = json.loads(
            self.fixture.source_ledger_path.read_text(encoding="utf-8")
        )
        source["masters"]["legend-home"]["prompt"] = "changed prompt"
        self.fixture.source_ledger_path.write_text(
            json.dumps(source, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        errors = self.errors()
        self.assertIn("legend-home: wrong MiniMax prompt", errors)
        self.assertIn(
            "provenance.json does not embed the exact MiniMax ledger", errors
        )


if __name__ == "__main__":
    unittest.main()
