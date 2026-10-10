"""APT signatures stay inspected, bounded and hash-linked; no crypto trust claim."""
import base64
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/package_policy'))
import apt_signature
import scan_release as scan

FIXTURE = Path(__file__).parent / 'fixtures/libxau_1.0.9.orig.tar.gz.asc'
FIXTURE_SHA = 'af6104aaf3c5ede529e381237dd60f49640ec96593a84502fa493b86582b2f04'


def armor(packet):
    return b'-----BEGIN PGP SIGNATURE-----\n\n' + base64.b64encode(packet) + b'\n-----END PGP SIGNATURE-----\n'


class AptSignature(unittest.TestCase):
    def scan(self, data, digest=None, authentication='apt signed Sources index'):
        name = 'pkg_1.orig.tar.gz.asc'
        manifest = {'sources': [{'name': name, 'sha256': digest or hashlib.sha256(data).hexdigest(),
                                'authentication': authentication}]}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'sources.tar.gz'
            with tarfile.open(path, 'w:gz') as archive:
                for member, payload in ((name, data), ('MANIFEST.json', json.dumps(manifest).encode())):
                    info = tarfile.TarInfo('suyu-0.0.14-dependency-sources/' + member)
                    info.size = len(payload); archive.addfile(info, io.BytesIO(payload))
            return scan.scan_archive(path, 'dependency-sources', scan.Rules(scan.load_policy()))['findings']

    def test_actual_public_apt_signature(self):
        data = FIXTURE.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), FIXTURE_SHA)
        apt_signature.validate_detached_signature(data)
        self.assertEqual(self.scan(data), [])

    def test_multiple_document_signatures_bounded(self):
        body = bytes([4, 0, 1, 8, 0, 0, 0, 0, 0, 0, 0, 8, 128])
        packet = bytes([0xC2, len(body)]) + body
        apt_signature.validate_detached_signature(armor(packet * 2))
        with self.assertRaises(ValueError):
            apt_signature.validate_detached_signature(armor(packet * 9))

    def test_manifest_hash_and_authentication_required(self):
        for arguments in ({'digest': '0' * 64}, {'authentication': 'untrusted download'}):
            self.assertIn('source-provenance', {f['rule'] for f in self.scan(FIXTURE.read_bytes(), **arguments)})

    def test_arbitrary_secret_and_trailing_packet_rejected(self):
        data = FIXTURE.read_bytes()
        for payload in (b'arbitrary payload', b'-----BEGIN PGP PRIVATE KEY BLOCK-----\nsecret\n',
                        armor(b'master_key_00 = ' + b'01' * 16), armor(b'\xc5\x01\x04'),
                        data + b'trailing payload\n', b'A' * (apt_signature.MAX_SIGNATURE_BYTES + 1)):
            with self.subTest(payload_size=len(payload)):
                self.assertTrue(self.scan(payload))
        # Valid signature syntax does not exempt existing key-content scans.
        annotated = data + b'master_key_00 = ' + b'01' * 16 + b'\n'
        self.assertIn('key-text', {f['rule'] for f in self.scan(annotated)})

    def test_checksum_and_incomplete_packets_rejected(self):
        data = FIXTURE.read_bytes()
        checksum = data.split(b'\n=')[1][:4]
        tampered = data.replace(b'\n=' + checksum, b'\n=AAAA')
        for payload in (tampered, armor(b'\xc2\xff\x00\x00\x00\xff\x04')):
            with self.assertRaises(ValueError):
                apt_signature.validate_detached_signature(payload)


if __name__ == '__main__':
    unittest.main()
