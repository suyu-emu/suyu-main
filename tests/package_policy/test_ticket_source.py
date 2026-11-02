"""Exercise production ticket-source selection with synthetic paths only."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_package_policy_unit import ROOT, compiler


class TicketSourceUnit(unittest.TestCase):
    def test_ticket_source(self):
        cc = compiler()
        if not cc:
            self.skipTest('no C++20 compiler on PATH')
        with tempfile.TemporaryDirectory(prefix='suyu-ticket-source-') as temporary:
            temp = Path(temporary)
            exe = temp / ('ticket_source.exe' if os.name == 'nt' else 'ticket_source')
            sources = [str(ROOT / 'tests/package_policy/ticket_source_unit.cpp'),
                       str(ROOT / 'src/common/package_policy.cpp')]
            if cc == 'cl':
                command = [cc, '/nologo', '/EHsc', '/std:c++20', '/W3', '/utf-8',
                           '/I' + str(ROOT / 'src'), *sources, '/Fe:' + str(exe),
                           '/Fo:' + str(temp) + os.sep]
            else:
                command = [cc, '-std=c++20', '-O1', '-Wall', '-I' + str(ROOT / 'src'),
                           *sources, '-o', str(exe)]
            build = subprocess.run(command, cwd=temp, capture_output=True, text=True,
                                   creationflags=getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0))
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe), str(temp / 'synthetic')], capture_output=True, text=True)
            print(run.stdout)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
