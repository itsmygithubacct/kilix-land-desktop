"""Land's local Programs fallback keeps catalog apps in the current tab."""

from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import land_catalog  # noqa: E402


class StackProgramsTests(unittest.TestCase):
    def test_pdf_conversion_uses_the_shared_host_application_verb(self):
        def which(command):
            return f"/usr/bin/{command}" if command == "bash" else None

        with mock.patch.object(
                land_catalog, "kilix_command",
                return_value="/opt/kilix/kilix"), \
             mock.patch.object(land_catalog.shutil, "which", side_effect=which):
            rows = land_catalog.discover_stack()
        pdf = next(argv for _group, label, argv in rows
                   if label == "PDF Conversion")
        self.assertEqual(
            pdf,
            [
                "/opt/kilix/kilix",
                "app",
                "run",
                "kilix-pdf-conversion",
            ],
        )

    def test_host_launcher_prefers_kilix_home(self):
        with mock.patch.dict(
                land_catalog.os.environ,
                {"KILIX_HOME": "/opt/kilix"}, clear=True), \
             mock.patch.object(land_catalog.os, "access", return_value=True):
            self.assertEqual(land_catalog.kilix_command(), "/opt/kilix/kilix")

    def test_every_host_catalog_app_becomes_a_current_tab_launch(self):
        response = mock.Mock(
            returncode=0,
            stdout='[{"id":"kilix-file","label":"File Manager","kind":"app"},'
                   '{"id":"doom","label":"Doom","kind":"game"}]',
        )
        with mock.patch.object(land_catalog, "kilix_command",
                               return_value="/opt/kilix/kilix"), \
             mock.patch.object(land_catalog.subprocess, "run",
                               return_value=response) as run:
            rows = land_catalog.discover_catalog_apps()
        run.assert_called_once_with(
            ["/opt/kilix/kilix", "install", "--json"],
            capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(rows, [(
            "Kilix applications",
            "File Manager",
            ["/opt/kilix/kilix", "app", "run", "kilix-file"],
        )])


if __name__ == "__main__":
    unittest.main()
