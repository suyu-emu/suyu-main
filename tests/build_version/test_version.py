"""Exercise release identity through the real CMake SCM generator."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("git") and shutil.which("cmake"), "Git and CMake required")
class BuildVersion(unittest.TestCase):
    def git(self, root, *args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Version probe", "-c",
             "user.email=version-probe@example.invalid", *args],
            cwd=root, text=True, stderr=subprocess.STDOUT).strip()

    def generate(self, root):
        shutil.copy2(ROOT / "src/common/scm_rev.cpp.in", root / "scm_rev.cpp.in")
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(version_probe NONE)\n'
            f'list(APPEND CMAKE_MODULE_PATH "{ROOT.as_posix()}/externals/cmake-modules")\n'
            f'include("{ROOT.as_posix()}/CMakeModules/GenerateSCMRev.cmake")\n',
            encoding="utf-8")
        subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                       capture_output=True, text=True, check=True, timeout=30)
        return (root / "build/scm_rev.cpp").read_text(encoding="utf-8")

    def test_exact_tag_and_later_development(self):
        with tempfile.TemporaryDirectory(prefix="suyu-version-") as temp:
            root = Path(temp)
            self.git(root, "init", "-b", "version-check")
            self.git(root, "commit", "--allow-empty", "-m",
                     "Provide a release head for version verification")
            self.git(root, "tag", "-a", "v0.0.14", "-m", "Identify the version probe release")
            release = self.generate(root)
            self.assertIn('#define BUILD_FULLNAME "suyu v0.0.14"', release)
            self.assertIn('#define BUILD_VERSION "v0.0.14"', release)
            self.assertIn('#define IS_DEV_BUILD false', release)
            self.git(root, "commit", "--allow-empty", "-m",
                     "Represent development after the release for verification")
            revision = self.git(root, "rev-parse", "HEAD")[:10]
            development = self.generate(root)
            self.assertIn(f'#define BUILD_FULLNAME "suyu {revision}-version-check"', development)
            self.assertIn('#define IS_DEV_BUILD true', development)

    def test_release_source_archive_without_git(self):
        with tempfile.TemporaryDirectory(prefix="suyu-version-archive-") as temp:
            root = Path(temp)
            for name, value in {"GIT-COMMIT": "a" * 40, "GIT-TAG": "v0.0.14",
                                "GIT-REFSPEC": "v0.0.14", "GIT-RELEASE": "v0.0.14"}.items():
                (root / name).write_text(value + "\n", encoding="utf-8")
            archive = self.generate(root)
            self.assertIn('#define BUILD_FULLNAME "suyu v0.0.14"', archive)
            self.assertIn('#define IS_DEV_BUILD false', archive)


if __name__ == "__main__":
    unittest.main()
