#!/usr/bin/env python3
"""Check the real CMake selection against retained Hybrid fallback sources."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")
if not CMAKE:
    for line in (ROOT / "build-win-fixed/CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_COMMAND:"):
            CMAKE = line.partition("=")[2]
            break


class ModuleSelection(unittest.TestCase):
    def select(self, selection):
        source = (ROOT / "src/suyu_cmd/CMakeLists.txt").read_text()
        start = source.index('        set(_recomp_dir_escaped ')
        end = source.index('        # SUYU_CMD_RECOMP_PREBUILT_DIR:', start)
        with tempfile.TemporaryDirectory(prefix="suyu-selection-") as temp:
            root = Path(temp) / "TOTK [Hybrid export]"
            root.mkdir()
            for module in ("main", "rtld", "sdk", "subsdk0"):
                (root / module).mkdir()
            if selection is not None:
                (root / "recomp_modules.cmake").write_text(
                    "set(SUYU_RECOMP_MODULES " + " ".join(selection) + ")\n"
                )
            result = Path(temp) / "result.txt"
            script = Path(temp) / "select.cmake"
            script.write_text(
                f'set(SUYU_CMD_RECOMP_DIR "{root.as_posix()}")\n'
                + source[start:end]
                + f'file(WRITE "{result.as_posix()}" "${{_recomp_children}}")\n'
            )
            subprocess.run([CMAKE, "-P", str(script)], check=True, capture_output=True)
            return set(filter(None, result.read_text().split(";")))

    def test_fallback_sources_are_excluded(self):
        self.assertEqual(self.select(["rtld", "sdk", "subsdk0"]),
                         {"rtld", "sdk", "subsdk0"})

    def test_legacy_exports_discover_modules(self):
        self.assertEqual(self.select(None), {"main", "rtld", "sdk", "subsdk0"})

    def test_empty_selection_does_not_restore_modules(self):
        self.assertEqual(self.select([]), set())


if __name__ == "__main__":
    unittest.main()
