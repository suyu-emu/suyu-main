"""Synthetic relocation checks; no emulator build or copyrighted game input."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import shutil
import subprocess
import json
import hashlib

spec =importlib.util.spec_from_file_location('kit_package', Path(__file__).resolve().parents[2] / 'tools/export_build_kit/package.py')
kit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kit)


class PackageTests(unittest.TestCase):
    def write_compiler_metadata(self, build):
        metadata = build / 'CMakeFiles/fixture/CMakeCXXCompiler.cmake'
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text('set(CMAKE_CXX_COMPILER_VERSION "19.44.35207")\n')

    def synthetic_build(self, root, name='original-build', extra_objects=None, libraries='core.lib kernel32.lib'):
        """A fake Ninja tree whose host objects live under the generic host targets' .dir."""
        build = root / name
        build.mkdir()
        self.write_compiler_metadata(build)
        (build / 'core.lib').write_bytes(b'core.lib')
        manifest = ''
        for mode in ['strict', 'hybrid']:
            host_dir = build / 'CMakeFiles' / ('suyu-export-host-' + mode + '.dir')
            host_dir.mkdir(parents=True)
            objects = ['host.obj', 'registry_probe.c.obj'] + list((extra_objects or {}).get(mode, []))
            for obj in objects:
                (host_dir / obj).write_bytes((mode + obj).encode())
            listed = ' '.join('CMakeFiles/suyu-export-host-' + mode + '.dir/' + obj for obj in objects)
            manifest += ('build bin/suyu-export-host-' + mode + '.exe: CXX_EXECUTABLE_LINKER__Release ' + listed + ' | core.lib\n'
                         '  LINK_LIBRARIES = ' + libraries + '\n  LINK_FLAGS = /machine:x64 /ENTRY:mainCRTStartup\n')
        (build / 'build.ninja').write_text(manifest)
        return build

    def assert_manifest_matches(self, output):
        manifest = json.loads((output / 'manifest.json').read_text())
        policy = json.loads((Path(__file__).resolve().parents[2] / 'tools/package_policy/policy.json').read_text())
        self.assertEqual(manifest['policy_version'], policy['policy_version'])
        self.assertRegex(manifest['producer_source_revision'], '^[0-9a-f]{40,64}$')
        self.assertIn('do not grant permission', manifest['note'])
        actual = {p.relative_to(output).as_posix() for p in output.rglob('*') if p.is_file()}
        self.assertEqual(actual, set(manifest['files']) | {'manifest.json'})
        for relative, digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((output / relative).read_bytes()).hexdigest(), digest, relative)
        for relative, digest in manifest['inputs'].items():
            self.assertEqual(manifest['files'][relative], digest)
        return manifest

    def leftovers(self, parent):
        return [p.name for p in parent.iterdir() if '.staging-' in p.name or '.old-' in p.name]

    @unittest.skipUnless(shutil.which('cl'), 'Run from an x64 Visual Studio developer environment')
    def test_real_windows_link_after_producer_tree_removed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, build, output = root / 'producer with spaces', root / 'build with spaces', root / 'kit [relocated] folder'
            source.mkdir()
            (source / 'host.c').write_text('extern int game(void); int main(void) { return game(); }')
            (source / 'registry_probe.c').write_text('int game(void) { return 1; }')
            (source / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(fixture C CXX)\nforeach(mode strict hybrid)\nadd_executable(suyu-export-host-${mode} host.c registry_probe.c)\nendforeach()\n')
            def run(*args):
                result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode:
                    print(result.stdout.decode(errors='replace'), flush=True)
                    raise subprocess.CalledProcessError(result.returncode, args, output=result.stdout)
            run('cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
            run('cmake', '--build', str(build))
            kit.package(build, output, kit.REVISION)
            self.assertEqual(json.loads((output / 'manifest.json').read_text())['producer_compiler_version'],
                             kit.producer_compiler_version(build))
            self.assert_manifest_matches(output)
            # Rename both producer locations: none of their original paths exist.
            source.rename(root / 'hidden-source')
            build.rename(root / 'hidden-build')
            export = root / 'export with spaces [hybrid]'
            (export / 'main').mkdir(parents=True)
            for header in ['recomp_abi_v4.h', 'recomp_guard_v2.h', 'recomp_fastmem_v1.h', 'recomp_features_v1.h', 'recomp_guard_gen_v1.h', 'recomp_fpx_v1.h']:
                (export / header).write_text('/* synthetic fixture */')
            (export / 'recomp_modules.cmake').write_text('set(SUYU_RECOMP_MODULES main)')
            (export / 'recomp_registration.c').write_text('extern int fixture_module(void); int game(void) { return fixture_module(); }')
            (export / 'main/recomp_runtime.h').write_text('#define RECOMP_IMAGE_ABI 6\n/* tpidrro_el0 */')
            (export / 'main/module.c').write_text('int fixture_module(void) { return 0; }')
            (export / 'main/CMakeLists.txt').write_text('add_library(recomp_static_main STATIC module.c)\ntarget_compile_options(recomp_static_main PRIVATE /O2)')
            for mode in ['strict', 'hybrid']:
                consumer = root / ('consumer-' + mode)
                run('cmake', '-S', str(output), '-B', str(consumer), '-G', 'Ninja', '-DSUYU_CMD_RECOMP_DIR=' + str(export), '-DSUYU_EXPORT_BUILD_KIT_REVISION=' + kit.REVISION, '-DSUYU_RECOMP_HYBRID=' + ('ON' if mode == 'hybrid' else 'OFF'))
                self.assertIn('CMAKE_BUILD_TYPE:STRING=Release', (consumer / 'CMakeCache.txt').read_text())
                run('cmake', '--build', str(consumer))
                run(str(consumer / 'bin/suyu-cmd-static.exe'))

            def assert_configure_rejected(name, diagnostic, configuration='Release'):
                consumer = root / ('rejected-' + name)
                result = subprocess.run(
                    ['cmake', '-S', str(output), '-B', str(consumer), '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=' + configuration, '-DSUYU_CMD_RECOMP_DIR=' + str(export),
                     '-DSUYU_EXPORT_BUILD_KIT_REVISION=' + kit.REVISION],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(diagnostic, ' '.join(result.stdout.split()))
                self.assertFalse((consumer / 'build.ninja').exists(), result.stdout)
                self.assertFalse((consumer / 'bin/suyu-cmd-static.exe').exists())

            assert_configure_rejected('debug', 'use Release instead of Debug', 'Debug')
            manifest_path = output / 'manifest.json'
            original_manifest = manifest_path.read_text()
            producer_manifest = json.loads(original_manifest)
            producer_manifest['producer_compiler_version'] = '99.0.0'
            manifest_path.write_text(json.dumps(producer_manifest))
            try:
                assert_configure_rejected('old-compiler', 'Update MSVC Build Tools:')
            finally:
                manifest_path.write_text(original_manifest)
            del producer_manifest['producer_compiler_version']
            manifest_path.write_text(json.dumps(producer_manifest))
            try:
                assert_configure_rejected('missing-compiler-version', 'Build kit is missing a valid producer compiler version')
            finally:
                manifest_path.write_text(original_manifest)

            for name in ['recomp_fastmem_v1.h', 'recomp_guard_gen_v1.h', 'recomp_fpx_v1.h']:
                with self.subTest(missing_handshake=name):
                    header = export / name
                    original = header.read_bytes()
                    header.unlink()
                    try:
                        assert_configure_rejected(name, 'Missing matched ABI 6 handshake: ' + name)
                    finally:
                        header.write_bytes(original)

            unlisted = output / 'inputs/unlisted.obj'
            unlisted.write_bytes(b'not listed in the manifest')
            try:
                assert_configure_rejected('unlisted-input', 'Unlisted build-kit input: inputs/unlisted.obj')
            finally:
                unlisted.unlink()

            copied_input = next((output / 'inputs').iterdir())
            original = copied_input.read_bytes()
            copied_input.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            try:
                assert_configure_rejected('tampered-input', 'Corrupt or mixed build-kit input:')
            finally:
                copied_input.write_bytes(original)

    def test_relocation_excludes_probe_and_preserves_system_libraries(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = self.synthetic_build(root)
            output = root / 'relocated-kit'
            kit.package(build, output, kit.REVISION)
            frozen = (output / 'strict.cmake').read_text()
            self.assertNotIn(str(build), frozen)
            self.assertNotIn('registry_probe', frozen)
            self.assertIn('kernel32.lib', frozen)
            self.assertIn('${CMAKE_CURRENT_LIST_DIR}/inputs/', frozen)
            # One host object per mode plus the shared core.lib.
            self.assertEqual(len(list((output / 'inputs').iterdir())), 3)
            self.assert_manifest_matches(output)
            self.assertEqual(self.leftovers(root), [])

    def test_manifest_records_policy_source_revision_and_hashes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / 'kit'
            kit.package(self.synthetic_build(root), output, kit.REVISION)
            manifest = self.assert_manifest_matches(output)
            self.assertEqual(manifest['revision'], kit.REVISION)
            self.assertEqual(manifest['producer_compiler_version'], '19.44.35207')
            for relative in ['strict.cmake', 'hybrid.cmake', 'CMakeLists.txt', 'revision.txt',
                             'src/suyu_cmd/recomp_modules/CMakeLists.txt']:
                self.assertIn(relative, manifest['files'])
            self.assertNotIn('manifest.json', manifest['files'])

    def test_source_revision_falls_back_to_git_commit_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaisesRegex(ValueError, 'source revision'):
                kit.source_revision(root)
            (root / 'GIT-COMMIT').write_text('0123456789abcdef0123456789abcdef01234567\n')
            self.assertEqual(kit.source_revision(root), '0123456789abcdef0123456789abcdef01234567')

    def test_game_configured_build_tree_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = self.synthetic_build(root)
            (build / 'CMakeCache.txt').write_text('SUYU_CMD_RECOMP_DIR:PATH=C:/exports/some-game\n')
            with self.assertRaisesRegex(ValueError, 'SUYU_CMD_RECOMP_DIR'):
                kit.package(build, root / 'kit', kit.REVISION)
            self.assertFalse((root / 'kit').exists())
            self.assertEqual(self.leftovers(root), [])
            # An unset/empty entry is what a generic tree has.
            (build / 'CMakeCache.txt').write_text('SUYU_CMD_RECOMP_DIR:PATH=\nSUYU_CMD_RECOMP_PREBUILT_DIR:PATH=\n')
            kit.package(build, root / 'kit', kit.REVISION)
            self.assertTrue((root / 'kit/manifest.json').is_file())

    def test_game_specific_or_foreign_inputs_are_rejected(self):
        strict_dir = 'CMakeFiles/suyu-export-host-strict.dir/'
        cases = {
            'registration object': dict(extra_objects={'strict': ['recomp_registration.c.obj']}),
            'static module library': dict(libraries='core.lib recomp_static_main.lib'),
            'module directory library': dict(libraries='core.lib recomp/core2.lib'),
            'modules directory library': dict(libraries='core.lib recomp_modules/core2.lib'),
        }
        for name, options in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                build = self.synthetic_build(root, **options)
                for relative in ['recomp/core2.lib', 'recomp_modules/core2.lib', 'recomp_static_main.lib']:
                    (build / relative).parent.mkdir(exist_ok=True)
                    (build / relative).write_bytes(b'x')
                with self.assertRaisesRegex(ValueError, 'Game-specific'):
                    kit.package(build, root / 'kit', kit.REVISION)
                self.assertFalse((root / 'kit').exists())
                self.assertEqual(self.leftovers(root), [])
        with self.subTest('object outside the host target directory'), tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = self.synthetic_build(root)
            (build / 'stray.obj').write_bytes(b'x')
            text = (build / 'build.ninja').read_text().replace(strict_dir + 'host.obj', 'stray.obj')
            (build / 'build.ninja').write_text(text)
            with self.assertRaisesRegex(ValueError, 'not an output of the generic'):
                kit.package(build, root / 'kit', kit.REVISION)
        with self.subTest('object of another target'), tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = self.synthetic_build(root)
            other = build / 'CMakeFiles/other-target.dir'
            other.mkdir(parents=True)
            (other / 'x.obj').write_bytes(b'x')
            text = (build / 'build.ninja').read_text().replace(strict_dir + 'host.obj', 'CMakeFiles/other-target.dir/x.obj')
            (build / 'build.ninja').write_text(text)
            with self.assertRaisesRegex(ValueError, 'not an output of the generic'):
                kit.package(build, root / 'kit', kit.REVISION)
        with self.subTest('library outside the build tree'), tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            outside = root / 'elsewhere.lib'
            outside.write_bytes(b'x')
            build = self.synthetic_build(root, libraries='core.lib ' + outside.as_posix())
            with self.assertRaisesRegex(ValueError, 'outside the build tree'):
                kit.package(build, root / 'kit', kit.REVISION)
        with self.subTest('pinned CPM cache library is allowed'), tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            cached = root / 'work/.cache/cpm/sdl3/1.0/lib/SDL3.lib'
            cached.parent.mkdir(parents=True)
            cached.write_bytes(b'x')
            build = self.synthetic_build(root, libraries='core.lib ' + cached.as_posix())
            kit.package(build, root / 'kit', kit.REVISION)
            self.assertTrue(any(p.name.endswith('-SDL3.lib') for p in (root / 'kit/inputs').iterdir()))

    def test_dirty_destination_is_replaced_by_exactly_the_manifest_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = self.synthetic_build(root)
            output = root / 'bin/export-build-kit'
            kit.package(build, output, kit.REVISION)
            (output / 'inputs/stale.obj').write_bytes(b'stale')
            (output / 'extra.txt').write_text('extra')
            (output / 'nested').mkdir()
            (output / 'nested/leftover.bin').write_bytes(b'x')
            kit.package(build, output, kit.REVISION)
            for relative in ['inputs/stale.obj', 'extra.txt', 'nested/leftover.bin']:
                self.assertFalse((output / relative).exists(), relative)
            self.assert_manifest_matches(output)
            self.assertEqual(self.leftovers(output.parent), [])

    def test_empty_destination_is_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / 'kit'
            output.mkdir()
            kit.package(self.synthetic_build(root), output, kit.REVISION)
            self.assert_manifest_matches(output)

    def test_foreign_destination_is_refused_and_untouched(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / 'documents'
            output.mkdir()
            (output / 'notes.txt').write_text('keep me')
            with self.assertRaisesRegex(ValueError, 'not an export build kit'):
                kit.package(self.synthetic_build(root), output, kit.REVISION)
            self.assertEqual([p.name for p in output.iterdir()], ['notes.txt'])
            self.assertEqual((output / 'notes.txt').read_text(), 'keep me')
            self.assertEqual(self.leftovers(root), [])

    def test_failed_package_leaves_existing_kit_and_no_staging(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = self.synthetic_build(root)
            output = root / 'kit'
            kit.package(build, output, kit.REVISION)
            before = (output / 'manifest.json').read_bytes()
            (build / 'CMakeFiles/suyu-export-host-hybrid.dir/host.obj').unlink()
            with self.assertRaisesRegex(ValueError, 'Missing link input'):
                kit.package(build, output, kit.REVISION)
            self.assertEqual((output / 'manifest.json').read_bytes(), before)
            self.assertEqual(self.leftovers(root), [])

    def test_missing_non_system_input_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.write_compiler_metadata(root)
            (root / 'build.ninja').write_text('build bin/suyu-export-host-strict.exe: CXX_EXECUTABLE_LINKER__Release missing.obj registry_probe.c.obj\n')
            with self.assertRaisesRegex(ValueError, 'Missing link input'):
                kit.package(root, root / 'kit', kit.REVISION)

    def test_revision_mismatch_fails_closed(self):
        with self.assertRaisesRegex(ValueError, 'revision'):
            kit.package('.', '.', 'old')

    def test_missing_compiler_metadata_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, 'producer C\\+\\+ compiler version'):
                kit.package(temp, Path(temp) / 'kit', kit.REVISION)

    def test_ninja_escaped_windows_paths(self):
        self.assertEqual(kit.ninja_words('G$:/some$ folder/host.obj other.obj'), ['G:/some folder/host.obj', 'other.obj'])


if __name__ == '__main__':
    unittest.main()
