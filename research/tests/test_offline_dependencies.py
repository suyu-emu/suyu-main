#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the real CMake offline/local-source hooks with small local fixtures."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which('cmake')

@unittest.skipUnless(CMAKE, 'CMake is required')
class OfflineDependencies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='suyu-offline-')
        self.root = Path(self.tmp.name).resolve()
        self.mod = self.root/'CMakeModules'; self.mod.mkdir()
        for name in ['CPM.cmake', 'CPMUtil.cmake', 'LocalDependencies.cmake']:
            shutil.copyfile(ROOT/'CMakeModules'/name, self.mod/name)
    def tearDown(self): self.tmp.cleanup()
    def configure(self, body, extra=(), success=True):
        (self.root/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.31)\nproject(offline_probe LANGUAGES C)\nlist(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")\ninclude(CPMUtil)\n'+body)
        result = subprocess.run([CMAKE, '-S', str(self.root), '-B', str(self.root/'build'),
                                 '-DCPMUTIL_OFFLINE=ON', *extra], capture_output=True, text=True, timeout=20)
        text = result.stdout + result.stderr
        self.assertEqual(result.returncode == 0, success, text)
        return text
    def check_top_level_cache(self, explicit):
        # Execute the production top-level declaration, then the production CPM
        # path helper. A normal set() would shadow a caller's -D cache entry.
        import re
        source = (ROOT/'CMakeLists.txt').read_text()
        match = re.search(r'^set\(CPM_SOURCE_CACHE\b[^)]*\)', source, re.MULTILINE)
        self.assertIsNotNone(match, 'Top-level CPM cache declaration is missing')
        selected = self.root/'separate writable cache'
        output = self.root/'cache-result.txt'
        script = self.root/'cache-probe.cmake'
        script.write_text('cmake_minimum_required(VERSION 3.31)\n' +
            match.group(0) + '\n' +
            'list(PREPEND CMAKE_MODULE_PATH "' + self.mod.as_posix() + '")\n' +
            'include(CPMUtil)\n' +
            'get_cache_path(fmt 12.1.0 result)\n' +
            'file(WRITE "' + output.as_posix() + '" "${result}")\n')
        args = [CMAKE]
        if explicit:
            args.append('-DCPM_SOURCE_CACHE:PATH=' + str(selected))
        result = subprocess.run(args + ['-P', str(script)], cwd=self.root,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected = selected if explicit else self.root/'.cache/cpm'
        self.assertEqual(Path(output.read_text()), expected/'fmt/12.1.0')

    def test_top_level_honors_explicit_source_cache(self):
        self.check_top_level_cache(True)

    def test_top_level_default_source_cache(self):
        self.check_top_level_cache(False)

    def test_local_package_build_and_unverified_provenance(self):
        dep=self.root/'local dependency'; dep.mkdir()
        (dep/'CMakeLists.txt').write_text('add_library(testdep STATIC testdep.c)\n')
        (dep/'testdep.c').write_text('int value(void){return 19;}\n')
        body='''AddPackage(NAME testdep VERSION 1.0 URL https://example.invalid/never-fetch.tar.gz HASH 00)
get_property(revisions GLOBAL PROPERTY CPM_PACKAGE_SHAS)
file(WRITE "${CMAKE_BINARY_DIR}/revisions.txt" "${revisions}")
'''
        self.configure(body, [f'-Dtestdep_CUSTOM_DIR={dep}'])
        result=subprocess.run([CMAKE, '--build', str(self.root/'build'), '--target', 'testdep'],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('revision unverified',(self.root/'build/revisions.txt').read_text())
    def test_missing_package_fails_without_retries(self):
        start=time.monotonic()
        text=self.configure('AddPackage(NAME missing VERSION 1.0 URL https://example.invalid/no.tar.gz HASH 00)\n',success=False)
        self.assertIn('Offline dependency missing: missing',text)
        self.assertIn('No download was attempted',' '.join(text.split()))
        self.assertNotIn('Trying again',text)
        self.assertLess(time.monotonic()-start,10)
    def test_bad_override_is_not_silently_downloaded(self):
        text=self.configure('AddPackage(NAME bad VERSION 1.0 URL https://example.invalid/no.tar.gz HASH 00)\n',[f'-Dbad_CUSTOM_DIR={self.root}/absent'],success=False)
        self.assertIn('is not a directory',text)
    def test_low_level_download_is_blocked(self):
        text=self.configure('cpm_download("https://example.invalid/no.tar.gz" "${CMAKE_BINARY_DIR}/never")\n',success=False)
        self.assertIn('Offline mode forbids downloading',text)
    def test_dynamic_map_nested_sources_and_overrides(self):
        packages={'new-package':{'repo':'example/new-package','version':'v1'},
                  'nested':{'repo':'example/nested','version':'v1'},
                  'keep':{'repo':'example/keep','version':'v1'},
                  'binary':{'repo':'example/binary','ci':True,'version':'v1'}}
        (self.root/'cpmfile.json').write_text(json.dumps(packages))
        for path in ['externals/new-package','externals/dynarmic/externals/nested','externals/keep','externals/binary','explicit']:
            d=self.root/path;d.mkdir(parents=True);(d/'CMakeLists.txt').write_text('# local fixture\n')
        self.configure('''include(LocalDependencies)
if(NOT new-package_CUSTOM_DIR STREQUAL "${CMAKE_SOURCE_DIR}/externals/new-package")
 message(FATAL_ERROR "new package was not discovered")
endif()
if(NOT nested_CUSTOM_DIR STREQUAL "${CMAKE_SOURCE_DIR}/externals/dynarmic/externals/nested")
 message(FATAL_ERROR "nested package was not discovered")
endif()
if(NOT keep_CUSTOM_DIR STREQUAL "${CMAKE_SOURCE_DIR}/explicit")
 message(FATAL_ERROR "explicit override was changed")
endif()
if(DEFINED binary_CUSTOM_DIR)
 message(FATAL_ERROR "CI artifact was incorrectly treated as source")
endif()
''',[f'-Dkeep_CUSTOM_DIR={self.root}/explicit'])
    def test_system_unknown_version_is_not_bundled_pin(self):
        (self.mod/'Findtestsystem.cmake').write_text('set(testsystem_FOUND TRUE)\n')
        text=self.configure('AddPackage(NAME testsystem VERSION 999.0 URL https://example.invalid/no.tar.gz HASH 00)\n')
        self.assertIn('Using system package testsystem@unknown',text)
        self.assertNotIn('Using system package testsystem@999.0',text)

if __name__=='__main__': unittest.main(verbosity=2)
