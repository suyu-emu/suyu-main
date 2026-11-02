"""Compile runtime baked-patch validation against synthetic files and digests."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_package_policy_unit import ROOT, compiler


class BakedPatchManifestUnit(unittest.TestCase):
    def test_qt_fingerprint_interoperability(self):
        cc = compiler()
        cache = ROOT.parent / 'sxmb/CMakeCache.txt'
        if cc != 'cl' or not cache.is_file():
            self.skipTest('configured Windows Qt build required')
        match = re.search(r'^Qt6_DIR:[^=]*=(.*)/lib/cmake/Qt6$', cache.read_text(), re.M)
        if not match:
            self.skipTest('Qt installation unavailable')
        qt = Path(match.group(1))
        json_include = ROOT / '.cache/cpm/nlohmann_json/v3.12.0/include'
        with tempfile.TemporaryDirectory(prefix='suyu-patch-fingerprint-') as temporary:
            temp = Path(temporary)
            exe = temp / 'fingerprint.exe'
            command = [cc, '/nologo', '/EHsc', '/std:c++20', '/Zc:__cplusplus', '/W3', '/utf-8',
                       '/DQT_USE_QSTRINGBUILDER',
                       '/I' + str(qt / 'include'), '/I' + str(qt / 'include/QtCore'),
                       '/I' + str(json_include), str(ROOT / 'tests/package_policy/baked_patch_fingerprint_unit.cpp'),
                       '/Fe:' + str(exe), '/Fo:' + str(temp) + os.sep,
                       '/link', str(qt / 'lib/Qt6Core.lib')]
            build = subprocess.run(command, cwd=temp, capture_output=True, text=True,
                                   creationflags=getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0))
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            environment = os.environ.copy()
            environment['PATH'] = str(qt / 'bin') + os.pathsep + environment['PATH']
            run = subprocess.run([str(exe)], capture_output=True, text=True,
                                 encoding='utf-8', errors='replace', env=environment)
            print(run.stdout)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_manifest_validation(self):
        cc = compiler()
        if not cc:
            self.skipTest('no C++20 compiler on PATH')
        candidates = [ROOT / '.cache/cpm/nlohmann_json/v3.12.0/include',
                      ROOT / 'externals/nxdumptool/libs/borealis/library/include/borealis/extern']
        json_include = next((p for p in candidates if (p / 'nlohmann/json.hpp').is_file()), None)
        if not json_include:
            self.skipTest('nlohmann JSON headers unavailable')
        with tempfile.TemporaryDirectory(prefix='suyu-baked-patch-') as temporary:
            temp = Path(temporary)
            exe = temp / ('baked_patch.exe' if os.name == 'nt' else 'baked_patch')
            sources = [str(ROOT / 'tests/package_policy/baked_patch_manifest_unit.cpp'),
                       str(ROOT / 'src/common/package_policy.cpp')]
            if cc == 'cl':
                command = [cc, '/nologo', '/EHsc', '/std:c++20', '/W3', '/utf-8',
                           '/I' + str(ROOT / 'src'), '/I' + str(json_include), *sources,
                           '/Fe:' + str(exe), '/Fo:' + str(temp) + os.sep]
            else:
                command = [cc, '-std=c++20', '-O1', '-Wall', '-I' + str(ROOT / 'src'),
                           '-I' + str(json_include), *sources, '-o', str(exe)]
            build = subprocess.run(command, cwd=temp, capture_output=True, text=True,
                                   creationflags=getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0))
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe), str(temp / 'synthetic')], capture_output=True, text=True,
                                 encoding='utf-8', errors='replace')
            print(run.stdout)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
