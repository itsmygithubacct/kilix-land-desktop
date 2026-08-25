"""Protocol-v1 surface of the compiled Kilix Land provider."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "kilix-land-desktop"


@unittest.skipUnless(BINARY.is_file(), "build the provider before this suite")
class ProviderContractTests(unittest.TestCase):
    def run_provider(self, *arguments: str, env: dict[str, str] | None = None):
        selected = dict(os.environ, KILIX_LAND_DESKTOP_ASSETS=str(ROOT))
        if env:
            selected.update(env)
        return subprocess.run(
            [str(BINARY), *arguments],
            cwd=ROOT,
            env=selected,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )

    def json_endpoint(self, *arguments: str) -> dict[str, object]:
        result = self.run_provider(*arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue(result.stdout.endswith("\n"))
        self.assertFalse(result.stdout.endswith("\n\n"))
        return json.loads(result.stdout)

    def test_describe_and_check(self):
        description = self.json_endpoint("provider", "describe", "--json")
        self.assertEqual(description["provider_id"], "kilix-land-desktop")
        self.assertEqual(description["contract_version"], 1)
        self.assertTrue(description["capabilities"]["headless_screenshot"])
        self.assertFalse(description["capabilities"]["mouse"]["available"])
        check = self.json_endpoint("provider", "check", "--json")
        self.assertEqual(check["status"], "ready")

    def test_config_reads_and_gated_mutations(self):
        schema = self.json_endpoint("provider", "config", "schema", "--json")
        self.assertEqual(
            schema["x-kilix-provider-id"], "kilix-land-desktop"
        )
        values = self.json_endpoint("provider", "config", "get", "--json")
        self.assertEqual(values["values"], {})
        for arguments in (
            ("provider", "config", "set", "style", "legend"),
            ("provider", "migrate", "--from", "0.1.0", "--dry-run"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_provider(*arguments)
                self.assertEqual(result.returncode, 4)
                self.assertEqual(result.stdout, "")
                self.assertIn("unavailable", result.stderr)

    def test_provider_screenshot_is_silent_and_writes_ppm(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "land.ppm"
            result = self.run_provider(
                "provider", "screenshot", str(output), "--room", "bedroom"
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "")
            self.assertTrue(output.read_bytes().startswith(b"P6\n"))

    def test_invalid_session_id_fails_closed(self):
        result = self.run_provider(
            "provider", "launch", "--session-id", "bad/session"
        )
        self.assertEqual(result.returncode, 3)
        self.assertEqual(result.stdout, "")

    def test_missing_authority_resolver_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            state = Path(temporary) / "state"
            marker = state / "kilix/desktops/migration-v1.json"
            marker.parent.mkdir(parents=True)
            marker.write_text("{}\n", encoding="utf-8")
            result = self.run_provider(
                "provider",
                "describe",
                "--json",
                env={
                    "KILIX_DESKTOP_CONTRACT_COMMAND": "",
                    "KILIX_DESKTOP_SDK_PREFIX": "",
                    "XDG_STATE_HOME": str(state),
                },
            )
        self.assertEqual(result.returncode, 4)
        self.assertEqual(result.stdout, "")
        self.assertIn("persistence resolver", result.stderr)


if __name__ == "__main__":
    unittest.main()
