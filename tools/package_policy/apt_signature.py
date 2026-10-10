"""Bounded detached OpenPGP signature syntax, not a cryptographic trust check.

Authentication still comes from the signed APT Sources index and exact SHA256.
Only bounded version4 document-signature packets are admitted; key/literal packets
and trailing payloads are rejected, even inside signature-looking armor.
"""
import base64
import re

MAX_SIGNATURE_BYTES = 65536


def validate_detached_signature(data):
    if not 0 < len(data) <= MAX_SIGNATURE_BYTES:
        raise ValueError('detached signature exceeds bounded size')
    try:
        lines = data.decode('ascii').strip('\r\n').splitlines()
    except UnicodeError as error:
        raise ValueError('detached signature must be ASCII armor') from error
    if not lines or lines[0] != '-----BEGIN PGP SIGNATURE-----' or lines[-1] != '-----END PGP SIGNATURE-----':
        raise ValueError('detached signature armor required')
    interior = lines[1:-1]
    while interior and interior[0]:
        if not re.fullmatch(r'Version: (GnuPG|GPG) v?[0-9][A-Za-z0-9 .()_-]{0,80}', interior.pop(0)):
            raise ValueError('invalid detached signature armor header')
    if not interior or interior.pop(0) != '':
        raise ValueError('detached signature armor separator missing')
    checksum = None
    if interior and interior[-1].startswith('='):
        checksum = base64.b64decode(interior.pop()[1:], validate=True)
    if not interior or any(not re.fullmatch(r'[A-Za-z0-9+/]{1,76}={0,2}', line) for line in interior):
        raise ValueError('invalid detached signature armor body')
    packet = base64.b64decode(''.join(interior), validate=True)
    if checksum is not None:
        crc = 0xB704CE
        for byte in packet:
            crc ^= byte << 16
            for _ in range(8):
                crc <<= 1
                if crc & 0x1000000:
                    crc ^= 0x1864CFB
        if checksum != (crc & 0xFFFFFF).to_bytes(3, 'big'):
            raise ValueError('detached signature armor checksum mismatch')
    count = 0
    while packet:
        count += 1
        if count > 8:
            raise ValueError('too many detached signature packets')
        consumed = _validate_packet(packet)
        packet = packet[consumed:]
    if not count:
        raise ValueError('detached signature packet missing')


def _validate_packet(packet):
    if len(packet) < 2 or not packet[0] & 0x80:
        raise ValueError('invalid detached signature packet')
    header = packet[0]
    if header & 0x40:
        tag = header & 0x3F; first = packet[1]; start = 2
        if first < 192:
            length = first
        elif first < 224 and len(packet) >= 3:
            length = ((first - 192) << 8) + packet[2] + 192; start = 3
        elif first == 255 and len(packet) >= 6:
            length = int.from_bytes(packet[2:6], 'big'); start = 6
        else:
            raise ValueError('partial signature packets are not admitted')
    else:
        tag = (header >> 2) & 15; width = (1, 2, 4, 0)[header & 3]
        if not width or len(packet) < 1 + width:
            raise ValueError('indeterminate signature packets are not admitted')
        start = 1 + width; length = int.from_bytes(packet[1:start], 'big')
    if tag != 2 or start + length > len(packet):
        raise ValueError('only complete detached signature packets admitted')
    body = packet[start:start + length]
    if len(body) < 13 or body[0] != 4 or body[1] not in (0, 1):
        raise ValueError('version4 document signature required')
    if body[2] not in (1, 3, 17, 19, 22) or body[3] not in (1, 2, 8, 9, 10, 11):
        raise ValueError('unsupported document signature algorithm')
    position = 4
    for _ in range(2):
        if position + 2 > len(body):
            raise ValueError('truncated signature subpacket length')
        count = int.from_bytes(body[position:position + 2], 'big')
        position += 2 + count
        if position > len(body):
            raise ValueError('truncated signature subpacket region')
    position += 2  # Digest prefix.
    for _ in range(1 if body[2] in (1, 3) else 2):
        if position + 2 > len(body):
            raise ValueError('truncated signature MPI')
        bits = int.from_bytes(body[position:position + 2], 'big')
        position += 2 + (bits + 7) // 8
        if not bits or position > len(body):
            raise ValueError('invalid signature MPI')
    if position != len(body):
        raise ValueError('trailing data in detached signature')
    return start + length
