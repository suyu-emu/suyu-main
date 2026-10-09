"""Final AppImage policy boundary; no image is executed or mounted."""
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/package_policy'))
import scan_release as scan

REV = 'a' * 40
POLICY = scan.load_policy()
PINS = {'runtime_sha256': POLICY['appimage']['runtime_sha256'],
        'runtime_size': POLICY['appimage']['runtime_size'], 'source_revision': REV, 'repository':'suyu-emu/suyu-main'}
PROVENANCE = POLICY['appimage']['provenance_path']
RUNTIME_LOCK, RUNTIME_LOCK_HASH = scan.appimage_runtime_lock(scan.Rules(POLICY))


def layout(extra=None, links=None):
    files = {name: b'placeholder\n' for name in POLICY['appimage']['required_files']}
    files.update({'AppRun': b'#!/bin/sh\nexec "$APPDIR/usr/bin/suyu" "$@"\n',
                  'usr/bin/suyu': b'\x7fELF\x02\x01', 'usr/bin/suyu-cmd': b'\x7fELF\x02\x01',
                  'usr/lib/libQt6Core.so.6': b'\x7fELF\x02\x01',
                  'usr/plugins/platforms/libqxcb.so': b'\x7fELF\x02\x01',
                  'usr/plugins/platforms/libqoffscreen.so': b'\x7fELF\x02\x01',
                  'usr/plugins/platforminputcontexts/libcomposeplatforminputcontextplugin.so': b'\x7fELF\x02\x01',
                  'apprun-hooks/linuxdeploy-plugin-qt-hook.sh': b'# generated hook\ncase \"${XDG_CURRENT_DESKTOP-}\" in GNOME) ;; esac\n',
                  'usr/share/doc/suyu/LICENSES/MIT.txt': b'MIT license\n',
                  'usr/share/doc/suyu/packaging-tool-licenses/runtime.txt': b'MIT license\n'})
    files.update(extra or {})
    receipt = dict(RUNTIME_LOCK, lock_sha256=RUNTIME_LOCK_HASH,
                   linker_map_sha256='0'*64, installed_package_db_sha256='1'*64,
                   contributed_archives=['/usr/lib/libc.a','/usr/lib/libfuse3.a','/usr/lib/libmimalloc.a',
                     '/usr/lib/libz.a','/usr/lib/libzstd.a','/usr/local/lib/libsquashfuse.a','/usr/local/lib/libsquashfuse_ll.a'])
    source_doc = {'schema':'suyu-appimage-sources-v1','runtime_build_receipt':receipt,
                  'runtime_components':RUNTIME_LOCK['components'],'sources':RUNTIME_LOCK['sources'],
                  'libraries':{n:{'sha256':hashlib.sha256(v).hexdigest(),'source_version':'fixture-1',
                     'copyright':'fixture-license','sources':['type2-runtime.tar.gz']}
                     for n,v in files.items() if n.startswith(('usr/lib/','usr/plugins/')) and '.so' in n}}
    files['usr/share/suyu/distribution-sources.json']=json.dumps(source_doc).encode()
    link_map = {'suyu.desktop': 'usr/share/applications/suyu.desktop',
                'suyu.svg': 'usr/share/icons/hicolor/scalable/apps/suyu.svg', '.DirIcon': 'suyu.svg'}
    link_map.update(links or {})
    provenance = {'schema': 'suyu-appimage-v1', 'policy_version': POLICY['policy_version'], **PINS,
                  'source_url': 'https://github.com/suyu-emu/suyu-main/tree/' + REV,
                  'runtime_source_url': RUNTIME_LOCK['source_url'],
                  'runtime_recipe_sha256':RUNTIME_LOCK['recipe_sha256'],'runtime_lock_sha256':RUNTIME_LOCK_HASH,
                  'files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
                  'links': link_map}
    files[PROVENANCE] = json.dumps(provenance).encode()
    return files, link_map


def inspect(files, links):
    members = [(n, '-rwxr-xr-x' if n in ('AppRun', 'usr/bin/suyu', 'usr/bin/suyu-cmd') else '-rw-r--r--', len(v), None)
               for n, v in files.items()]
    members += [(n, 'lrwxrwxrwx', len(t), t) for n, t in links.items()]
    scanner = scan.Scanner(scan.Rules(POLICY), 'linux-appimage')
    scanner.appimage_pins = PINS
    @contextlib.contextmanager
    def reader(name, size):
        yield io.BytesIO(files[name])
    try:
        scan.scan_appdir(scanner, members, reader)
    except (ValueError, json.JSONDecodeError):
        scanner.add('appimage',PROVENANCE,'invalid embedded provenance')
    return scanner.findings


class AppImagePolicy(unittest.TestCase):
    def test_realistic_inventory_clean(self):
        self.assertEqual(inspect(*layout()), [])

    def test_secret_game_export_userdata_unexpected(self):
        for name, data, rule in [
            ('usr/lib/prod.keys', b'placeholder', 'key-name'),
            ('usr/lib/libhidden.so', b'master_key_00 = ' + b'01' * 16, 'key-text'),
            ('usr/share/suyu/game_source.txt', b'fixture', 'export-marker'),
            ('usr/share/suyu/game.nsp', b'fixture', 'game-container'),
            ('user/config/settings.ini', b'fixture', 'user-data'),
            ('usr/share/suyu/unexpected.txt', b'fixture', 'unexpected')]:
            with self.subTest(name=name):
                self.assertIn(rule, {f['rule'] for f in inspect(*layout({name:data}))})

    def test_nested_archive_not_hidden_by_library_name(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('prod.keys', b'placeholder')
        findings = inspect(*layout({'usr/lib/libhidden.so':buf.getvalue()}))
        self.assertIn('nested-archive', {f['rule'] for f in findings})
        self.assertIn('key-name', {f['rule'] for f in findings})

    def test_escaping_absolute_dangling_and_cycle_links(self):
        for target in ('../../outside', '/etc/passwd', 'C:outside', 'missing', '.DirIcon'):
            with self.subTest(target=target):
                self.assertIn('link', {f['rule'] for f in inspect(*layout(links={'.DirIcon':target}))})

    def test_path_never_reaches_reader(self):
        files, links = layout({'../outside':b'bad'})
        findings = inspect(files, links)
        self.assertIn('path-unsafe', {f['rule'] for f in findings})

    def test_inventory_and_provenance_pins_fail_closed(self):
        files, links = layout()
        files['usr/bin/suyu'] += b'changed'
        self.assertIn('appimage', {f['rule'] for f in inspect(files, links)})
        files, links = layout()
        p=json.loads(files[PROVENANCE]);p['source_revision']='b'*40
        files[PROVENANCE]=json.dumps(p).encode()
        self.assertIn('appimage', {f['rule'] for f in inspect(files, links)})

    def test_recipe_and_source_closure_cannot_be_self_asserted(self):
        files,links=layout();p=json.loads(files[PROVENANCE]);p['source_url']='https://example.invalid/'+REV;files[PROVENANCE]=json.dumps(p).encode()
        self.assertIn('appimage',{f['rule'] for f in inspect(files,links)})
        for field,value in [('runtime_recipe_sha256','0'*64),('runtime_lock_sha256','0'*64)]:
            files,links=layout();p=json.loads(files[PROVENANCE]);p[field]=value;files[PROVENANCE]=json.dumps(p).encode()
            self.assertIn('appimage',{f['rule'] for f in inspect(files,links)})
        files,links=layout();name='usr/share/suyu/distribution-sources.json';doc=json.loads(files[name])
        doc['runtime_complete']=True;doc['runtime_components']=[]
        files[name]=json.dumps(doc).encode();p=json.loads(files[PROVENANCE]);p['files'][name]=hashlib.sha256(files[name]).hexdigest();files[PROVENANCE]=json.dumps(p).encode()
        self.assertIn('source-provenance',{f['rule'] for f in inspect(files,links)})

    def test_listing_special_truncated_ambiguous(self):
        good=b'-rwxr-xr-x 0/0 6 2026-10-09 00:00 squashfs-root/AppRun\n'
        self.assertEqual(scan.squashfs_listing(good)[0][0], 'AppRun')
        for value in (b'bad', good.replace(b'-rwx',b'brwx'), good.replace(b'AppRun',b'AppRun -> confusing')):
            with self.assertRaises(ValueError):scan.squashfs_listing(value)

    def test_runtime_pin_type_and_truncation(self):
        policy=copy.deepcopy(POLICY)
        runtime=bytearray(128);runtime[:6]=b'\x7fELF\x02\x01';runtime[8:11]=b'AI\x02';runtime[18:20]=b'\x3e\x00'
        digest=hashlib.sha256(runtime).hexdigest()
        policy['appimage'].update(runtime_sha256=digest,runtime_size=128)
        rules=scan.Rules(policy)
        header=bytearray(96);header[:4]=b'hsqs';struct.pack_into('<HH',header,28,4,0);struct.pack_into('<Q',header,40,96)
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'fixture.AppImage';path.write_bytes(runtime+header)
            self.assertEqual(scan.appimage_offset(path,rules,digest,128),128)
            for payload in (runtime+header[:-1],runtime+header+b'junk',b'bad'+header):
                path.write_bytes(payload)
                with self.assertRaises(ValueError):scan.appimage_offset(path,rules,digest,128)
            path.write_bytes(runtime+header)
            with self.assertRaises(ValueError):scan.appimage_offset(path,rules,'0'*64,128)

    def test_linux_layout_unchanged_and_appimage_scoped(self):
        self.assertEqual(POLICY['release_layouts']['linux'],
                         ['suyu','suyu-cmd',r'LICENSE\.txt',r'LICENSES/[^/]+\.txt',r'THIRD-PARTY-NOTICES\.txt'])
        self.assertIn('linux-appimage',scan.KINDS)
        self.assertNotIn('ELF',json.dumps(POLICY.get('approved_exceptions',[])))



class DebianSourceRoles(unittest.TestCase):
    def test_patch_bounded_inspected_and_hash_linked(self):
        import gzip, tarfile
        from unittest.mock import patch as mock_patch
        def run(patch, digest=None):
            files={'pkg_1.dsc':b'Format: 3.0 (quilt)\n', 'pkg_1.diff.gz':gzip.compress(patch)}
            files['MANIFEST.json']=json.dumps({'sources':[{'name':n,'sha256':digest or hashlib.sha256(v).hexdigest()} for n,v in files.items()]}).encode()
            with tempfile.TemporaryDirectory() as tmp:
                path=Path(tmp)/'sources.tar.gz'
                with tarfile.open(path,'w:gz') as archive:
                    for n,v in files.items():
                        info=tarfile.TarInfo('suyu-0.0.14-dependency-sources/'+n);info.size=len(v);archive.addfile(info,io.BytesIO(v))
                return scan.scan_archive(path,'dependency-sources',scan.Rules(POLICY))['findings']
        self.assertEqual(run(b'--- source\n+++ source\n+ harmless patch\n'),[])
        # A real temporary file exposes w+b; gzip must still decompress, not write.
        with mock_patch.object(scan, 'SPOOL_BYTES', 1):
            self.assertEqual(run(b'--- source\n+++ source\n+ harmless patch\n'), [])
            self.assertIn('source-provenance', {f['rule'] for f in run(b'--- source\n', '0'*64)})
            self.assertIn('key-text', {f['rule'] for f in run(b'master_key_00 = '+b'01'*16)})
        self.assertIn('source-provenance',{f['rule'] for f in run(b'--- source\n','0'*64)})
        self.assertIn('key-text',{f['rule'] for f in run(b'master_key_00 = '+b'01'*16)})
        self.assertIn('unreadable',{f['rule'] for f in run(b'\x00binary')})


@unittest.skipUnless(sys.platform.startswith('linux'), 'real SquashFS tooling requires Linux')
class RealSquashFS(unittest.TestCase):
    def test_final_image_roundtrip_and_negative_payloads(self):
        import shutil, subprocess
        if not shutil.which('mksquashfs') or not shutil.which('unsquashfs'):
            self.fail('Linux AppImage gate requires squashfs-tools')
        for extra,links,rule in [({}, {}, None),({'usr/lib/prod.keys':b'placeholder'}, {}, 'key-name'),
                ({'usr/share/suyu/game.nsp':b'placeholder'}, {}, 'game-container'),
                ({'usr/share/suyu/game_source.txt':b'placeholder'}, {}, 'export-marker'),
                ({}, {'.DirIcon':'../../escape'}, 'link'),
                ({'unexpected.txt':b'placeholder'}, {}, 'unexpected')]:
            with self.subTest(rule=rule),tempfile.TemporaryDirectory() as tmp:
                folder=Path(tmp);app=folder/'AppDir';app.mkdir()
                files,syms=layout(extra,links)
                for name,data in files.items():
                    p=app/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
                    if name in ('AppRun','usr/bin/suyu','usr/bin/suyu-cmd'):p.chmod(0o755)
                for name,target in syms.items():(app/name).symlink_to(target)
                fs=folder/'payload.sqfs'
                subprocess.run(['mksquashfs',str(app),str(fs),'-noappend','-processors','1','-quiet'],check=True,stdout=subprocess.DEVNULL)
                runtime=bytearray(128);runtime[:6]=b'\x7fELF\x02\x01';runtime[8:11]=b'AI\x02';runtime[18:20]=b'\x3e\x00'
                pin=hashlib.sha256(runtime).hexdigest();policy=copy.deepcopy(POLICY);policy['appimage'].update(runtime_sha256=pin,runtime_size=128)
                test_lock=copy.deepcopy(RUNTIME_LOCK);test_lock.update(sha256=pin,size=128)
                doc=json.loads(files['usr/share/suyu/distribution-sources.json']);doc['runtime_build_receipt'].update(sha256=pin,size=128)
                (app/'usr/share/suyu/distribution-sources.json').write_text(json.dumps(doc))
                prov=json.loads(files[PROVENANCE]);prov.update(runtime_sha256=pin,runtime_size=128)
                prov['files']['usr/share/suyu/distribution-sources.json']=hashlib.sha256((app/'usr/share/suyu/distribution-sources.json').read_bytes()).hexdigest()
                (app/PROVENANCE).write_text(json.dumps(prov));fs.unlink()
                subprocess.run(['mksquashfs',str(app),str(fs),'-noappend','-processors','1','-quiet'],check=True,stdout=subprocess.DEVNULL)
                image=folder/'final.AppImage';image.write_bytes(runtime+fs.read_bytes())
                from unittest.mock import patch
                with patch.object(scan,'appimage_runtime_lock',return_value=(test_lock,RUNTIME_LOCK_HASH)):
                    found=scan.scan_archive(image,'linux-appimage',scan.Rules(policy),{'runtime_sha256':pin,'runtime_size':128,'source_revision':REV,'repository':'suyu-emu/suyu-main'})['findings']
                if rule:self.assertIn(rule,{f['rule'] for f in found},found)
                else:self.assertEqual(found,[])
                used=struct.unpack_from('<Q',fs.read_bytes(),40)[0]
                image.write_bytes(image.read_bytes()[:128+used-1])
                self.assertIn('appimage',{f['rule'] for f in scan.scan_archive(image,'linux-appimage',scan.Rules(policy),{'runtime_sha256':pin,'runtime_size':128,'source_revision':REV,'repository':'suyu-emu/suyu-main'})['findings']})

if __name__=='__main__':unittest.main()
