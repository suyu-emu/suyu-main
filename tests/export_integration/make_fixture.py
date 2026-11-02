#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate a synthetic, bootable extracted-ExeFS fixture for suyu (stdlib only).

Writes `main` (NSO), `main.npdm` and `romfs.bin` into a directory. Everything is
synthesised here: no Nintendo keys, firmware, code or game data are used.
The title/program ID 0x0100000000E57A00 is SYNTHETIC and belongs to no real title.

Usage: python make_fixture.py OUTDIR
"""
import hashlib
import struct
import sys
from pathlib import Path

TITLE_ID = 0x0100000000E57A00  # synthetic; not a real title
MARKER = "SUYU_EXPORT_FIXTURE_OK"
ROMFS_FILE_NAME = "fixture.txt"
ROMFS_FILE_DATA = b"suyu synthetic export fixture\n"
PAGE = 0x1000
FILES = ["main", "main.npdm", "romfs.bin"]

# Load layout (module-relative): .text @0, .rodata @0x1000, .data @0x2000.
TEXT_LOC, RODATA_LOC, DATA_LOC = 0x0000, 0x1000, 0x2000
MOD0_OFF = 0x8              # text[4] holds this; MOD0 header lives at text+8
CODE_OFF = MOD0_OFF + 0x1C  # first instruction after MOD0 (0x24)
MSG_OFF = 0x40              # the marker's offset in .rodata, after the module path


def _msg() -> bytes:
    return MARKER.encode() + b"\n"


def _code_words() -> list:
    assert len(_msg()) == 23
    # Each entry is one little-endian AArch64 instruction word (hand-assembled).
    return [
        0xB0000000,  # adrp x0, #0x1000         ; x0 = page(pc)+0x1000 -> .rodata
        0x91010000,  # add  x0, x0, #0x40       ; the message, after the module path
        0xD28002E1,  # movz x1, #23             ; length of "SUYU_EXPORT_FIXTURE_OK\n"
        0xD40004E1,  # svc  #0x27               ; svcOutputDebugString(x0=str, x1=len)
        0xD29C2000,  # movz x0, #0xe100         ; 100,000,000 ns = 0x05F5E100 (low half)
        0xF2A0BEA0,  # movk x0, #0x5f5, lsl 16  ; (high half)
        0xD4000161,  # svc  #0xb                ; svcSleepThread(100 ms): gives suyu's debug-string
                     #                          ; flusher thread time to start; the buffered
                     #                          ; marker is logged when the process shuts down.
                     #                          ; (Keep this < 250 ms: see README.)
        0xD40000E1,  # svc  #0x7                ; svcExitProcess()
        0x14000000,  # b    .                   ; unreachable safety net
    ]


def make_nso() -> bytes:
    # --- .text: b <code>; MOD0 offset; MOD0; code; zero pad to a page ---
    text = bytearray()
    text += struct.pack("<I", 0x14000000 | (CODE_OFF >> 2))  # b #CODE_OFF (entry at text+0)
    text += struct.pack("<I", MOD0_OFF)                       # offset of MOD0 (read by exporter)
    text += struct.pack(
        "<7I",
        0x30444F4D,                  # "MOD0"
        DATA_LOC - MOD0_OFF,         # dynamic_offset (rel. to MOD0): lone DT_NULL in .data
        DATA_LOC + 0x10 - MOD0_OFF,  # bss_start (empty bss)
        DATA_LOC + 0x10 - MOD0_OFF,  # bss_end
        0, 0,                        # eh_frame_hdr start/end (none)
        0)                           # runtime module object offset
    assert len(text) == CODE_OFF
    for w in _code_words():
        text += struct.pack("<I", w)
    text += b"\x00" * (PAGE - len(text))
    assert len(text) % 4 == 0 and len(text) % PAGE == 0

    # .rodata starts with the module path every NSO carries ({u32 0, s32 length,
    # path}). FindModules (src/core/arm/debug.cpp) needs it to see the module, and
    # recompiled images are bound to modules through that list. The message follows.
    path = b"synthetic/main.nss"
    rodata = bytearray(struct.pack("<Ii", 0, len(path)) + path)
    rodata += b"\x00" * (MSG_OFF - len(rodata))
    rodata += _msg()
    rodata += b"\x00" * (PAGE - len(rodata))
    data = bytearray(PAGE)  # zero-filled: .dynamic is a lone DT_NULL

    file_off = 0x100
    offs = [file_off, file_off + len(text), file_off + len(text) + len(rodata)]
    build_id = b"SYNTHETIC-FIXTURE".ljust(0x20, b"\x00")
    hdr = bytearray()
    hdr += b"NSO0"
    hdr += struct.pack("<III", 0, 0, 0)  # version, reserved, flags=0 (no LZ4, no hash check)
    # (file offset, location, segment, alignment / bss_size)
    for off, loc, seg, extra in ((offs[0], TEXT_LOC, text, 1),
                                 (offs[1], RODATA_LOC, rodata, 1),
                                 (offs[2], DATA_LOC, data, 0)):
        hdr += struct.pack("<IIII", off, loc, len(seg), extra)
    hdr += build_id
    hdr += struct.pack("<III", len(text), len(rodata), len(data))  # "compressed" == raw sizes
    hdr += b"\x00" * 0x1C
    hdr += b"\x00" * 24   # api_info / dynstr / dynsym extents
    hdr += b"\x00" * 96   # segment hashes (unused: hash-check flags clear)
    assert len(hdr) == 0x100, len(hdr)
    return bytes(hdr) + bytes(text) + bytes(rodata) + bytes(data)


def _svc_mask_cap(svcs) -> list:
    """SyscallMask caps: id bits[0:5]=0b01111, mask bits[5:29], index bits[29:32] (24 SVCs each)."""
    groups = {}
    for s in svcs:
        groups[s // 24] = groups.get(s // 24, 0) | (1 << (s % 24))
    return [0xF | (mask << 5) | (idx << 29) for idx, mask in sorted(groups.items())]


def make_npdm() -> bytes:
    caps = [
        0x030043F7,                          # CorePriority: cores 0-3, prio 16-63 (suyu default)
        *_svc_mask_cap([0x07, 0x0B, 0x27]),  # ExitProcess, SleepThread, OutputDebugString
        0x3FFF | (0 << 15) | (3 << 19),      # KernelVersion 3.0
        0x7FFF | (128 << 16),                # HandleTable size 128
        0xFFFF | (1 << 17),                  # DebugFlags: allow_debug
    ]
    kac = struct.pack("<%dI" % len(caps), *caps)

    fah = struct.pack("<B3xQ4I", 1, 0xFFFFFFFFFFFFFFFF, 0, 0, 0, 0)     # FileAccessHeader
    fac = struct.pack("<B3xQ", 1, 0xFFFFFFFFFFFFFFFF) + b"\x00" * 0x20  # FileAccessControl
    sac = b""

    aci_off = 0x80
    fah_o = 0x40
    sac_o = fah_o + len(fah)
    kac_o = sac_o + len(sac)
    aci_hdr = (b"ACI0" + b"\x00" * 0xC + struct.pack("<Q", TITLE_ID) + b"\x00" * 8 +
               struct.pack("<6I", fah_o, len(fah), sac_o, len(sac), kac_o, len(kac)) +
               b"\x00" * 8)
    assert len(aci_hdr) == 0x40, len(aci_hdr)
    aci = aci_hdr + fah + sac + kac
    aci += b"\x00" * (-len(aci) % 0x10)

    acid_off = aci_off + len(aci)
    fac_o = 0x240
    sac_o2 = fac_o + len(fac)
    kac_o2 = sac_o2 + len(sac)
    acid_hdr = (b"\x00" * 0x200 + b"ACID" + struct.pack("<I", 0) + b"\x00" * 4 +
                struct.pack("<I", 0) +  # flags: Application pool, non-production
                struct.pack("<QQ", TITLE_ID, TITLE_ID) +
                struct.pack("<6I", fac_o, len(fac), sac_o2, len(sac), kac_o2, len(kac)) +
                b"\x00" * 8)
    assert len(acid_hdr) == 0x240, len(acid_hdr)
    acid = acid_hdr + fac + sac + kac

    hdr = bytearray()
    hdr += b"META" + b"\x00" * 8
    hdr += bytes([0x07,   # flags: bit0 = 64-bit, bits1-3 = 3 (39-bit address space)
                  0,      # reserved
                  0x2C,   # main thread priority (within 16-63)
                  0])     # main thread core
    hdr += b"\x00" * 4
    hdr += struct.pack("<III", 0, 0, 0x100000)  # system resource size, category, stack size
    hdr += b"SyntheticFixture".ljust(0x10, b"\x00")
    hdr += b"\x00" * 0x40
    hdr += struct.pack("<4I", aci_off, len(aci), acid_off, len(acid))
    assert len(hdr) == 0x80, len(hdr)
    return bytes(hdr) + aci + acid


def _romfs_hash(parent: int, name: bytes) -> int:
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def make_romfs() -> bytes:
    empty = 0xFFFFFFFF
    name = ROMFS_FILE_NAME.encode()
    dir_hash = struct.pack("<I", 0)  # 1 bucket -> root dir at dir-meta offset 0
    root = struct.pack("<6I", 0, empty, empty, 0, _romfs_hash(0, b""), 0)  # child_file = offset 0
    file_hash = struct.pack("<I", 0)  # 1 bucket -> file at file-meta offset 0
    fent = struct.pack("<IIQQII", 0, empty, 0, len(ROMFS_FILE_DATA),
                       _romfs_hash(0, name), len(name))
    fent += name + b"\x00" * (-len(name) % 4)
    off = 0x50
    dh_o = off
    off += len(dir_hash)
    dm_o = off
    off += len(root)
    fh_o = off
    off += len(file_hash)
    fm_o = off
    off += len(fent)
    data_o = (off + 0x1FF) & ~0x1FF
    hdr = struct.pack("<Q8QQ", 0x50, dh_o, len(dir_hash), dm_o, len(root),
                      fh_o, len(file_hash), fm_o, len(fent), data_o)
    body = hdr + dir_hash + root + file_hash + fent
    body += b"\x00" * (data_o - len(body)) + ROMFS_FILE_DATA
    return body


def make_pfs0(entries) -> bytes:
    """A PFS0 (the NSP container format) holding the given (name, data) pairs."""
    names = b""
    offsets = []
    for name, _ in entries:
        offsets.append(len(names))
        names += name.encode() + b"\x00"
    names += b"\x00" * (-(0x10 + 0x18 * len(entries) + len(names)) % 0x20)
    header = b"PFS0" + struct.pack("<III", len(entries), len(names), 0)
    data = b""
    table = b""
    for (name, blob), name_offset in zip(entries, offsets):
        table += struct.pack("<QQII", len(data), len(blob), name_offset, 0)
        data += blob
    return header + table + names + data


def write_nsp(path) -> dict:
    """An extracted-type NSP: a PFS0 with the ExeFS files and the RomFS, no NCAs, no keys.

    suyu loads such a file like an extracted folder, so it boots the same synthetic program
    while being a single .nsp game file, as a portable export needs."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(make_pfs0([("main", make_nso()), ("main.npdm", make_npdm()),
                             ("fixture.romfs", make_romfs())]))
    return {"title_id": TITLE_ID, "marker": MARKER, "size": p.stat().st_size}


AOC_TYPE = 0x82  # TitleType::AOC


def install_dlc(registered_dir, title_id, contents) -> list:
    """A synthetic DLC title the way suyu's NAND lists installed content: a plain CNMT in
    registered/yuzu_meta naming one <nca id>.nca file per content record.

    `contents` is a list of (record_type, size). The files hold made-up bytes, not NCAs; no
    keys decrypt them and suyu skips them when it looks for content metadata. They let the
    exporter find, copy and seal DLC without any Nintendo data. Returns
    [(record_type, path, data)]."""
    registered = Path(registered_dir)
    meta_dir = registered / "yuzu_meta"
    meta_dir.mkdir(parents=True, exist_ok=True)
    records = b""
    written = []
    for index, (record_type, size) in enumerate(contents):
        nca_id = struct.pack(">QQ", title_id, 0x5D1C0000 + index)
        pattern = b"synthetic dlc %016x %d " % (title_id, index)
        data = (pattern * (size // len(pattern) + 1))[:size]
        path = registered / (nca_id.hex() + ".nca")
        path.write_bytes(data)
        written.append((record_type, path, data))
        records += (b"\x00" * 0x20 + nca_id + size.to_bytes(6, "little") +
                    bytes([record_type, 0]))
    # CNMTHeader (0x20), then the optional header (0x10) the content records follow.
    header = struct.pack("<QIBBHHHB2sBI4x", title_id, 0, AOC_TYPE, 0, 0x10, len(contents),
                         0, 0, b"\x00\x00", 1, 0)
    assert len(header) == 0x20
    optional = struct.pack("<QQ", title_id & ~0x1FFF, 0)
    (meta_dir / ("%016x.cnmt" % title_id)).write_bytes(header + optional + records)
    return written


def make_ticket(rights_id: bytes, title_key: bytes, sig_type=0x10004) -> bytes:
    """A common ticket (RSA-2048 layout, 0x400 bytes) for `rights_id`. The signature is zeros
    and the title key is made up: it unlocks nothing and comes from no console or shop.
    suyu checks a ticket's layout, not Nintendo's signature."""
    assert len(rights_id) == 16 and len(title_key) == 16
    data = bytearray(0x2C0)
    issuer = b"Root-CA00000003-XS00000020"
    data[0:len(issuer)] = issuer
    data[0x40:0x50] = title_key        # title_key_block; a common ticket keeps the key here
    data[0x141] = 0                    # TitleKeyType::Common
    data[0x160:0x170] = rights_id
    return struct.pack("<I", sig_type) + bytes(0x100) + bytes(0x3C) + bytes(data)


def make_meta_nca(title_id: int, title_type: int, application_id: int) -> bytes:
    """A metadata NCA with a plaintext header, which suyu reads when the header does not
    decrypt with header_key: section 0 is an unencrypted PFS0 holding a CNMT with no content
    records. Opening it needs only some key_area_key_application_00 to be loaded."""
    header = struct.pack("<QIBBHHHB2sBI4x", title_id, 0, title_type, 0, 0x10, 0, 0, 0,
                         b"\x00\x00", 1, 0)
    cnmt = header + struct.pack("<QQ", application_id, 0)
    names = {0x80: "Application", 0x81: "Patch", 0x82: "AddOnContent"}
    pfs = make_pfs0([("%s_%016x.cnmt" % (names[title_type], title_id), cnmt)])
    assert len(pfs) <= 0x1000  # one hash block covers it
    hash_table = hashlib.sha256(pfs).digest()
    body = hash_table + bytes(0x200 - len(hash_table)) + pfs
    body += bytes(-len(body) % 0x200)
    section_start = 0xC00
    total = section_start + len(body)

    fs_header = bytearray(0x200)
    # version 2, PartitionFs, HierarchicalSha256, no encryption
    struct.pack_into("<HBBB", fs_header, 0, 2, 1, 2, 1)
    struct.pack_into("<32sii", fs_header, 0x8, hashlib.sha256(hash_table).digest(), 0x1000, 2)
    struct.pack_into("<qqqq", fs_header, 0x30, 0, len(hash_table), 0x200, len(pfs))

    nca = bytearray(0x400)
    nca[0x200:0x204] = b"NCA3"
    nca[0x204] = 0          # distribution: download
    nca[0x205] = 1          # content type: meta
    nca[0x206] = 0          # key generation
    nca[0x207] = 0          # key area index: application
    struct.pack_into("<QQII", nca, 0x208, total, title_id, 0, 0x000C1100)
    struct.pack_into("<II", nca, 0x240, section_start // 0x200, total // 0x200)
    nca[0x280:0x2A0] = hashlib.sha256(bytes(fs_header)).digest()
    return bytes(nca) + bytes(fs_header) + bytes(0x600) + body


def write_titlekey_dlc_nsp(path, title_id, application_id, title_key) -> dict:
    """An installable NSP laid out like eShop DLC: its metadata NCA, a ticket for its rights
    ID, and a second .tik that is not a ticket. Everything is synthetic."""
    rights_id = struct.pack(">Q", title_id) + bytes(7) + b"\x0a"
    ticket = make_ticket(rights_id, title_key)
    meta = make_meta_nca(title_id, AOC_TYPE, application_id)
    meta_id = hashlib.sha256(meta).hexdigest()[:32]
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(make_pfs0([(meta_id + ".cnmt.nca", meta), (rights_id.hex() + ".tik", ticket),
                             ("not-a-ticket.tik", b"\xff" * 16)]))
    return {"rights_id": rights_id.hex(), "ticket": ticket, "meta_id": meta_id}


def write_fixture(directory) -> dict:
    d = Path(directory)
    d.mkdir(parents=True, exist_ok=True)
    (d / "main").write_bytes(make_nso())
    (d / "main.npdm").write_bytes(make_npdm())
    (d / "romfs.bin").write_bytes(make_romfs())
    return {"title_id": TITLE_ID, "marker": MARKER, "files": list(FILES)}


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_fixture.py OUTDIR")
    info = write_fixture(sys.argv[1])
    for f in info["files"]:
        print(f, (Path(sys.argv[1]) / f).stat().st_size)
    print("title_id=0x%016X marker=%s" % (info["title_id"], info["marker"]))
