"""Compile and run the production package-policy helpers against synthetic trees."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def compiler():
    if os.name == 'nt' and shutil.which('cl'):
        return 'cl'
    for name in (os.environ.get('CXX'), 'c++', 'g++', 'clang++'):
        if name and shutil.which(name):
            return name
    return None


class PackagePolicyUnit(unittest.TestCase):
    def test_policy_helpers(self):
        cc = compiler()
        if not cc:
            self.skipTest('no C++20 compiler on PATH (on Windows run from a VS developer shell)')
        with tempfile.TemporaryDirectory(prefix='suyu-policy-unit-') as temp:
            temp = Path(temp)
            exe = temp / ('policy_unit.exe' if os.name == 'nt' else 'policy_unit')
            sources = [str(ROOT / 'tests/package_policy/policy_unit.cpp'),
                       str(ROOT / 'src/common/package_policy.cpp')]
            if cc == 'cl':
                command = ['cl', '/nologo', '/EHsc', '/std:c++20', '/W3', '/utf-8',
                           '/I' + str(ROOT / 'src'), *sources, '/Fe:' + str(exe),
                           '/Fo:' + str(temp) + os.sep]
            else:
                command = [cc, '-std=c++20', '-O1', '-Wall', '-I' + str(ROOT / 'src'), *sources,
                           '-o', str(exe)]
            build = subprocess.run(command, cwd=temp, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
            self.assertEqual(build.returncode, 0, build.stdout)
            run = subprocess.run([str(exe), str(temp / 'scratch')], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            print(run.stdout)
            self.assertEqual(run.returncode, 0, run.stdout)


if __name__ == '__main__':
    unittest.main()
