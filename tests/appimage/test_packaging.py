import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import json
import os
import subprocess
import hashlib
from unittest import mock

TOOLS = Path(__file__).resolve().parents[2] / 'tools/appimage'
sys.path.insert(0, str(TOOLS))
import runtime
import build
from build import inventory, fetch, sha, apprun_script, deployment_env, retain_optional_translations
from collect_sources import validate


class Packaging(unittest.TestCase):
    def test_extracted_qt_plugin_does_not_collide_with_discovery_wrapper(self):
        class DeploymentReached(Exception):
            pass
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); package = root / 'package'; package.mkdir()
            for name in ('suyu', 'suyu-cmd', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt'):
                (package / name).write_bytes(b'fixture')
            (package / 'LICENSES').mkdir()
            work = root / 'work'
            extractions = []

            def fetch_fixture(url, target, expected):
                target.write_bytes(b'mocked pinned tool or license')
                return target

            def tool_fixture(command, **kwargs):
                if command[-1] == '--appimage-extract':
                    directory = kwargs['cwd']; extractions.append(directory)
                    extracted = directory / 'squashfs-root'; extracted.mkdir()
                    (extracted / 'AppRun').write_text('fixture extracted entry point')
                    return
                self.assertEqual(command[-2:], ['--plugin', 'qt'])
                wrapper = work / 'tools/linuxdeploy-plugin-qt'
                self.assertTrue(wrapper.is_file())
                plugin = next(p for p in extractions if p.name == 'linuxdeploy-plugin-qt')
                self.assertTrue(plugin.is_dir())
                self.assertNotEqual(wrapper, plugin)
                self.assertIn(str(plugin / 'squashfs-root/AppRun'), wrapper.read_text())
                self.assertEqual(kwargs['env']['PATH'].split(os.pathsep)[0], str(work / 'tools'))
                raise DeploymentReached

            with mock.patch.object(build, 'fetch', side_effect=fetch_fixture), \
                 mock.patch.object(build.subprocess, 'run', side_effect=tool_fixture), \
                 mock.patch.object(build.platform, 'freedesktop_os_release',
                                   return_value={'ID': 'ubuntu', 'VERSION_ID': '24.04'}), \
                 mock.patch.object(build.platform, 'machine', return_value='x86_64'), \
                 mock.patch.dict(os.environ, {'SOURCE_DATE_EPOCH': '1'}), \
                 mock.patch.object(sys, 'argv', ['build.py', '--package-dir', str(package),
                       '--work-dir', str(work), '--runtime-dir', str(root / 'runtime'),
                       '--source-revision', 'a' * 40, '--repository', 'fixture/repository',
                       '--release-name', 'v0.0.14']):
                with self.assertRaises(DeploymentReached):
                    build.main()

    @unittest.skipIf(os.name == 'nt' or not hasattr(os, 'geteuid') or os.geteuid() == 0,
                     'requires a non-root POSIX user for directory permission enforcement')
    def test_runtime_handoff_reads_without_mutating_source_parent(self):
        payload = b'verified tiny runtime fixture\n'
        recipe_bytes = b'fixture recipe; Docker execution is mocked\n'
        download = b'verified download fixture\n'
        digest = lambda data: hashlib.sha256(data).hexdigest()
        lock = json.loads((TOOLS / 'runtime.lock.json').read_text())
        lock.update(sha256=digest(payload), size=len(payload),
                    recipe_sha256=digest(recipe_bytes))
        for entry in lock['sources'] + lock['apks']:
            entry['sha256'] = digest(download)
        archives = ['/usr/lib/libc.a', '/usr/lib/libfuse3.a', '/usr/lib/libmimalloc.a',
                    '/usr/lib/libz.a', '/usr/lib/libzstd.a',
                    '/usr/local/lib/libsquashfuse.a', '/usr/local/lib/libsquashfuse_ll.a']
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); fixture_tools = root / 'tools'; fixture_tools.mkdir()
            (fixture_tools / 'runtime-build.sh').write_bytes(recipe_bytes)
            (fixture_tools / 'runtime.lock.json').write_text(json.dumps(lock))
            work = root / 'work'; result_dir = work / 'result'

            def fetch_fixture(url, target, expected):
                target.write_bytes(download)
                self.assertEqual(sha(target), expected)

            def docker_fixture(command, **kwargs):
                self.assertEqual(command[:4], ['fixture-docker', 'run', '--rm', '--cpus'])
                self.assertTrue(kwargs['check'])
                result_dir.mkdir()
                binary = result_dir / 'runtime-x86_64'; binary.write_bytes(payload)
                binary.chmod(0o755)
                (result_dir / 'runtime.map').write_text('\n'.join(a + '(fixture.o)' for a in archives))
                records = ['P:' + item['package'] + '\nV:' + item['version'] + '\nc:' + item['commit']
                           for item in lock['aports_build_commits'].values()]
                (result_dir / 'apk-installed-db.txt').write_text('\n\n'.join(records))
                # Model a Docker-owned0755 parent: the host user can read its
                # files but cannot remove entries. chmod0555 enforces the same
                # handoff constraint without requiring privileged chown.
                result_dir.chmod(0o555)
                self.assertFalse(os.access(result_dir, os.W_OK))
                with self.assertRaises(PermissionError):
                    binary.rename(work / 'forbidden-rename')

            try:
                with mock.patch.object(runtime, 'HERE', fixture_tools), \
                     mock.patch.object(runtime, 'fetch', side_effect=fetch_fixture), \
                     mock.patch.object(runtime.subprocess, 'run', side_effect=docker_fixture), \
                     mock.patch.object(sys, 'argv', ['runtime.py', '--work-dir', str(work),
                                                    '--docker', 'fixture-docker']):
                    runtime.main()
                self.assertEqual((work / 'runtime-x86_64').read_bytes(), payload)
                self.assertEqual(sha(work / 'runtime-x86_64'), lock['sha256'])
                self.assertEqual((work / 'runtime-x86_64').stat().st_size, lock['size'])
                self.assertEqual((result_dir / 'runtime-x86_64').read_bytes(), payload)
                self.assertEqual(result_dir.stat().st_mode & 0o777, 0o555)
                receipt = json.loads((work / 'runtime-receipt.json').read_text())
                self.assertTrue(receipt['reproducibility_pin_verified'])
                self.assertEqual(receipt['contributed_archives'], archives)
            finally:
                if result_dir.exists():
                    result_dir.chmod(0o755)

    def test_offscreen_plugin_explicitly_deployed(self):
        env = deployment_env('private-qmake')
        self.assertEqual(env['EXTRAPLATFORM_PLUGINS'], 'libqoffscreen.so')
        self.assertEqual(env['QMAKE'], 'private-qmake')
        self.assertEqual(env['DISABLE_COPYRIGHT_FILES_DEPLOYMENT'], '1')

    def test_optional_translation_bytes_retained_outside_release(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d); app = work / 'AppDir'
            translations = app / 'usr/translations'; translations.mkdir(parents=True)
            original = translations / 'qtbase_fr.qm'; original.write_bytes(b'fixture-translation')
            digest = sha(original)
            self.assertEqual(retain_optional_translations(app, work), {'qtbase_fr.qm': digest})
            self.assertFalse(translations.exists())
            self.assertEqual(sha(work / 'omitted-qt-translations/qtbase_fr.qm'), digest)

    def test_unexpected_translation_subtree_preserved_and_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d); app = work / 'AppDir'
            translations = app / 'usr/translations'; translations.mkdir(parents=True)
            unexpected = translations / 'unexpected.txt'; unexpected.write_text('preserve')
            with self.assertRaisesRegex(ValueError, 'unexpected file'):
                retain_optional_translations(app, work)
            self.assertTrue(unexpected.is_file())

    @unittest.skipIf(os.name == 'nt', 'actual POSIX launcher runs in Linux CI')
    def test_actual_root_hook_and_missing_desktop_environment(self):
        with tempfile.TemporaryDirectory() as d:
            app = Path(d)
            (app / 'apprun-hooks').mkdir(); (app / 'usr/bin').mkdir(parents=True)
            (app / 'apprun-hooks/linuxdeploy-plugin-qt-hook.sh').write_text(
                'test "$XDG_CURRENT_DESKTOP" = ""\nexport FIXTURE_QT_HOOK=root-hook\n')
            for command in ('suyu', 'suyu-cmd'):
                executable = app / 'usr/bin' / command
                executable.write_text('#!/bin/sh\nprintf "%s:%s" "$FIXTURE_QT_HOOK" "$*"\n')
                executable.chmod(0o755)
            run = app / 'AppRun'; run.write_text(apprun_script()); run.chmod(0o755)
            env = dict(os.environ); env.pop('XDG_CURRENT_DESKTOP', None)
            result = subprocess.run([str(run), '--suyu-cmd', '--help'], env=env,
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout, 'root-hook:--help')

    def test_uncovered_library_rejected(self):
        runtime = json.loads((TOOLS / 'runtime.lock.json').read_text())
        m = dict(schema='suyu-appimage-sources-v1', runtime_complete=True,
                 runtime_sha256=runtime['sha256'],
                 runtime_build_receipt=dict(runtime, lock_sha256=sha(TOOLS / 'runtime.lock.json')),
                 runtime_components=runtime['components'], sources=runtime['sources'], libraries={})
        with self.assertRaisesRegex(ValueError, 'coverage'):
            validate(m, {'usr/lib/libQt6Core.so.6': 'a' * 64})

    def test_runtime_closure_required(self):
        with self.assertRaisesRegex(ValueError, 'runtime'):
            validate(dict(schema='suyu-appimage-sources-v1'), {})

    def test_wrong_cached_download_never_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'tool'; f.write_bytes(b'wrong')
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                fetch('https://example.invalid/tool', f, '0' * 64)

    def test_inventory_excludes_self_and_hashes_payload(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); meta = root / 'usr/share/suyu'; meta.mkdir(parents=True)
            (meta / 'appimage-provenance.json').write_text('self')
            (root / 'AppRun').write_text('payload')
            files, links = inventory(root)
            self.assertEqual(list(files), ['AppRun'])
            self.assertEqual(len(files['AppRun']), 64)
            self.assertEqual(links, {})


if __name__ == '__main__': unittest.main()
