#!/usr/bin/env python3
"""Contract and failure-mode tests for tools/land_config.py."""

from __future__ import annotations

import contextlib
import errno
import io
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import land_config  # noqa: E402


class LandConfigTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="land-config-test.")
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir(mode=0o700)
        self.config = self.root / "config"
        self.environment = mock.patch.dict(
            os.environ,
            {
                "HOME": str(self.home),
                "KILIX_LAND_DESKTOP_CONFIG_HOME": str(self.config),
                "KILIX_LAND_DESKTOP_ASSETS": str(ROOT),
            },
        )
        self.environment.start()

    def tearDown(self) -> None:
        self.environment.stop()
        self.temporary.cleanup()

    def test_relative_override_matches_launcher_fallback(self) -> None:
        os.environ["KILIX_LAND_DESKTOP_CONFIG_HOME"] = "relative/config"
        self.assertEqual(
            land_config.config_home(),
            str(
                self.home
                / ".local"
                / "gpu_terminal"
                / "kilix-land-desktop"
            ),
        )

    def test_absolute_home_is_required_for_default(self) -> None:
        os.environ["KILIX_LAND_DESKTOP_CONFIG_HOME"] = ""
        os.environ["HOME"] = "relative-home"
        with self.assertRaisesRegex(
                land_config.BindingFileError, "HOME must be an absolute path"):
            land_config.config_home()

    def test_parser_accepts_canonical_entries(self) -> None:
        bindings, errors = land_config.parse_bindings(
            "# local choices\n"
            "bedroom.bed = app /usr/bin/true --version\n"
            "study.filing-cabinet = folder /tmp\n"
        )
        self.assertEqual(errors, [])
        self.assertEqual(
            bindings,
            {
                "bedroom.bed": ("app", "/usr/bin/true --version"),
                "study.filing-cabinet": ("folder", "/tmp"),
            },
        )

    def test_parser_rejects_ambiguous_or_noncanonical_entries(self) -> None:
        bindings, errors = land_config.parse_bindings(
            "bedroom.bed = app /usr/bin/true\n"
            "bedroom.bed = folder /tmp\n"
            "Bedroom.bed = app /usr/bin/true\n"
            "living.tv = app\t/usr/bin/true\n"
            "yard.shed = app /usr/bin/true \n"
            "study.computer = app /usr/bin/true\r\n"
        )
        self.assertEqual(
            bindings, {"bedroom.bed": ("app", "/usr/bin/true")}
        )
        joined = "\n".join(errors)
        self.assertIn("duplicate key 'bedroom.bed'", joined)
        self.assertIn("lowercase letters", joined)
        self.assertIn("kind must be 'app' or 'folder'", joined)
        self.assertIn("leading or trailing whitespace", joined)
        self.assertIn("contains a control character", joined)

    def test_binding_validation_matches_runtime_limits(self) -> None:
        self.assertIsNone(
            land_config.validate_binding("app", f"{sys.executable} -V")
        )
        self.assertEqual(
            land_config.validate_binding(
                "app", "program one two three four five six seven eight"
            ),
            "app command has more than 8 arguments",
        )
        self.assertEqual(
            land_config.validate_binding("app", "./local-program"),
            "program path must be absolute or found on PATH",
        )
        self.assertEqual(
            land_config.validate_binding("folder", "relative"),
            "folder must be an absolute path",
        )
        self.assertIsNone(
            land_config.validate_binding("folder", str(self.root))
        )

    def test_atomic_save_round_trips_with_private_modes(self) -> None:
        expected = {
            "study.computer": ("app", f"{sys.executable} -V"),
            "bedroom.bed": ("folder", str(self.root)),
        }
        land_config.save_bindings(expected)
        self.assertEqual(land_config.load_bindings(), expected)
        directory_mode = stat.S_IMODE(self.config.stat().st_mode)
        file_mode = stat.S_IMODE(
            (self.config / "bindings.conf").stat().st_mode
        )
        self.assertEqual(directory_mode, 0o700)
        self.assertEqual(file_mode, 0o600)
        self.assertEqual(
            list(self.config.glob(".bindings.conf.tmp.*")), []
        )

    def test_failed_replace_preserves_previous_file_and_cleans_temp(self) -> None:
        original = {"bedroom.bed": ("folder", str(self.root))}
        land_config.save_bindings(original)
        failure = OSError(errno.EIO, "fixture replace failure")
        with mock.patch.object(os, "replace", side_effect=failure):
            with self.assertRaisesRegex(
                    land_config.BindingFileError, "cannot save bindings.conf"):
                land_config.save_bindings(
                    {"study.computer": ("app", sys.executable)}
                )
        self.assertEqual(land_config.load_bindings(), original)
        self.assertEqual(
            list(self.config.glob(".bindings.conf.tmp.*")), []
        )

    def test_reader_refuses_symlinks_and_writable_files(self) -> None:
        self.config.mkdir(mode=0o700)
        outside = self.root / "outside.conf"
        outside.write_text(
            "bedroom.bed = app /usr/bin/true\n", encoding="utf-8"
        )
        (self.config / "bindings.conf").symlink_to(outside)
        with self.assertRaisesRegex(
                land_config.BindingFileError, "securely open bindings.conf"):
            land_config._read_bindings_text()

        (self.config / "bindings.conf").unlink()
        (self.config / "bindings.conf").write_text(
            "bedroom.bed = app /usr/bin/true\n", encoding="utf-8"
        )
        (self.config / "bindings.conf").chmod(0o666)
        with self.assertRaisesRegex(
                land_config.BindingFileError, "group/world writable"):
            land_config._read_bindings_text()

    def test_reader_refuses_nonprivate_directory(self) -> None:
        self.config.mkdir(mode=0o755)
        self.config.chmod(0o755)
        with self.assertRaisesRegex(
                land_config.BindingFileError, "permissions must be 0700"):
            land_config._read_bindings_text()

    def test_check_rejects_unknown_world_object(self) -> None:
        land_config.save_bindings(
            {"bedroom.not-an-object": ("app", sys.executable)}
        )
        standard_error = io.StringIO()
        with contextlib.redirect_stderr(standard_error):
            result = land_config.check()
        self.assertEqual(result, 1)
        self.assertIn(
            "bedroom.not-an-object: no such room object",
            standard_error.getvalue(),
        )

    def test_check_reports_missing_store_as_defaults(self) -> None:
        standard_output = io.StringIO()
        with contextlib.redirect_stdout(standard_output):
            result = land_config.check()
        self.assertEqual(result, 0)
        self.assertIn("defaults everywhere", standard_output.getvalue())

    def test_interactive_editor_uses_shared_kilix_shell(self) -> None:
        if land_config.locate_tui_core() is None:
            self.skipTest("kilix-tui-utils checkout is unavailable")

        screenshot = self.root / "land-config.txt"
        with mock.patch.object(
            sys,
            "argv",
            ["land_config.py", "--screenshot", str(screenshot)],
        ):
            self.assertEqual(land_config.run_interactive(), 0)

        lines = screenshot.read_text(encoding="utf-8").splitlines()
        self.assertIn("KILIX TUI", lines[0])
        self.assertIn("Kilix Land · Config", lines[0])
        self.assertIn("▶1 ", lines[1])
        self.assertTrue(lines[2].startswith("─"))
        self.assertIn("objects", lines[3])
        self.assertNotIn("KILIX LAND CONFIG", "\n".join(lines))
        self.assertNotIn(" // ", "\n".join(lines))

        from kilix_tui import app

        short_screenshot = self.root / "land-config-short.txt"
        render_to_text = app.render_to_text

        def render_short(render, state):
            state.mode = "kind"
            return render_to_text(render, state, height=6, width=80)

        with (
            mock.patch.object(
                sys,
                "argv",
                ["land_config.py", "--screenshot", str(short_screenshot)],
            ),
            mock.patch.object(
                app,
                "render_to_text",
                side_effect=render_short,
            ),
        ):
            self.assertEqual(land_config.run_interactive(), 0)

        short_lines = short_screenshot.read_text(
            encoding="utf-8"
        ).splitlines()
        self.assertIn("Bedroom · 2 objects", short_lines[3])
        self.assertNotIn("bind bedroom.bed", "\n".join(short_lines))


if __name__ == "__main__":
    unittest.main()
