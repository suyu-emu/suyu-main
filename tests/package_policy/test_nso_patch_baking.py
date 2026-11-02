"""Compile the exporter flat-image patch adapter using synthetic NSO bytes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_package_policy_unit import ROOT, compiler


class NsoPatchBakingUnit(unittest.TestCase):
    def test_flat_image_patching(self):
        cc = compiler()
        if not cc:
            self.skipTest('no C++20 compiler on PATH')
        with tempfile.TemporaryDirectory(prefix='suyu-nso-baking-') as temporary:
            temp = Path(temporary)
            exe = temp / ('nso_baking.exe' if os.name == 'nt' else 'nso_baking')
            source = str(ROOT / 'tests/package_policy/nso_patch_baking_unit.cpp')
            if cc == 'cl':
                command = [cc, '/nologo', '/EHsc', '/std:c++20', '/W3',
                           '/I' + str(ROOT / 'src'), source, '/Fe:' + str(exe),
                           '/Fo:' + str(temp) + os.sep]
            else:
                command = [cc, '-std=c++20', '-O1', '-Wall', '-I' + str(ROOT / 'src'),
                           source, '-o', str(exe)]
            build = subprocess.run(command, cwd=temp, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            print(run.stdout)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
