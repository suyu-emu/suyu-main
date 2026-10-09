import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import json
import os
import subprocess

TOOLS = Path(__file__).resolve().parents[2] / 'tools/appimage'
sys.path.insert(0, str(TOOLS))
from build import inventory, fetch, sha, apprun_script, deployment_env, retain_optional_translations
from collect_sources import validate


class Packaging(unittest.TestCase):
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
