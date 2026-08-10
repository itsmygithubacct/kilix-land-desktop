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


if __name__ == "__main__":
    unittest.main()
