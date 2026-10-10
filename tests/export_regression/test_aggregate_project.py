#!/usr/bin/env python3
"""Titles must not become CMake languages or commands in Source exports."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")
TITLES = ("SUPER MARIO ODYSSEY", "マリオ ゲーム",
          "title;message(FATAL_ERROR injected)", 'title${CMAKE_VERSION}"\\\n)#')


def emitted_project(title):
    source = (ROOT / "src/suyu/game_export.cpp").read_text(encoding="utf-8")
    region = source.split("// Top-level CMakeLists.txt:", 1)[1].split("// ── Single", 1)[0]
    if '"project(" << game_name << "_recompiled C)' in region:
        return "project(" + title + "_recompiled C)\n"
    match = re.search(r'"(project\([^"\n]+\))\\n', region)
    if not match:
        raise AssertionError("actual emitted project literal not found")
    return match.group(1) + "\n"


@unittest.skipUnless(CMAKE, "CMake is required")
class AggregateProject(unittest.TestCase):
    def configure(self, declaration):
        with tempfile.TemporaryDirectory(prefix="suyu-aggregate-project-") as temp:
            root = Path(temp)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.13)\n" + declaration, encoding="utf-8")
            # Syntax-only configure: an existing executable and forced compiler
            # identity prevent compiler detection/try_compile or native builds.
            result = subprocess.run(
                [CMAKE, "-G", "Ninja", "-S", str(root), "-B", str(root / "build"),
                 "-DCMAKE_C_COMPILER=" + Path(sys.executable).as_posix(),
                 "-DCMAKE_C_COMPILER_ID_RUN=TRUE", "-DCMAKE_C_COMPILER_ID=GNU",
                 "-DCMAKE_C_COMPILER_VERSION=12.0", "-DCMAKE_C_COMPILER_FORCED=TRUE"],
                capture_output=True, text=True, timeout=30)
            cache = root / "build/CMakeCache.txt"
            return result.returncode, result.stdout + result.stderr, (
                cache.read_text(encoding="utf-8") if cache.exists() else "")

    def test_titles_cannot_change_project_syntax(self):
        for title in TITLES:
            with self.subTest(title=title):
                before, _, _ = self.configure("project(" + title + "_recompiled C)\n")
                self.assertNotEqual(before, 0, "pre-fix declaration unexpectedly configured")
                declaration = emitted_project(title)
                after, log, cache = self.configure(declaration)
                self.assertEqual(after, 0, log)
                self.assertEqual(declaration, "project(suyu_recompiled LANGUAGES C)\n")
                self.assertIn("CMAKE_PROJECT_NAME:STATIC=suyu_recompiled", cache)
                self.assertNotIn("CMAKE_CXX_COMPILER:", cache)


if __name__ == "__main__":
    unittest.main()
