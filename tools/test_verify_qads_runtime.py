"""Exercise the artifact omissions and stale-template cases CI must reject."""

from pathlib import Path
import tempfile
import unittest

from verify_qads_runtime import verify_runtime


class QadsRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        project = self.root / "Ksword5.1" / "Ksword5.1"
        (project / "lib").mkdir(parents=True)
        (project / "include" / "ads").mkdir(parents=True)
        self.reference = project / "lib" / "qtadvanceddocking.dll"
        self.reference.write_bytes(b"current QADS library")
        (project / "include" / "ads" / "ads_version.h").write_text(
            "#define ADS_VERSION_MAJOR 5\n"
            "#define ADS_VERSION_MINOR 1\n"
            "#define ADS_VERSION_PATCH 1\n", encoding="utf-8"
        )
        self.runtime = self.root / "Release"
        self.runtime.mkdir()
        self.dll = self.runtime / self.reference.name

    def test_current_runtime_passes(self):
        self.dll.write_bytes(self.reference.read_bytes())
        self.assertIn("QADS 5.1.1 verified", verify_runtime(self.runtime, self.root))

    def test_missing_runtime_fails(self):
        with self.assertRaisesRegex(ValueError, "missing or empty"):
            verify_runtime(self.runtime, self.root)

    def test_empty_runtime_fails(self):
        self.dll.touch()
        with self.assertRaisesRegex(ValueError, "missing or empty"):
            verify_runtime(self.runtime, self.root)

    def test_stale_template_runtime_fails(self):
        self.dll.write_bytes(b"old QADS from manual release")
        with self.assertRaisesRegex(ValueError, "differs from the release commit"):
            verify_runtime(self.runtime, self.root)

    def test_checked_out_commit_controls_expected_library(self):
        self.dll.write_bytes(self.reference.read_bytes())
        self.reference.write_bytes(b"next QADS library")
        with self.assertRaisesRegex(ValueError, "differs from the release commit"):
            verify_runtime(self.runtime, self.root)


if __name__ == "__main__":
    unittest.main()
