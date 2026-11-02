#!/usr/bin/env python3
"""Fix UltraCam 3.0 for Tears of the Kingdom 1.4.3.

UltraCam is by MaxLastBreath, licensed CC BY-NC 2.0
(https://creativecommons.org/licenses/by-nc/2.0/).
Source repository: https://github.com/MaxLastBreath/nx-optimizer
This script is an independent, non-commercial helper and is not affiliated
with the UltraCam author. It downloads nothing and ships none of the mod.

What it changes
---------------
UltraCam's built-in offsets cover TOTK up to 1.4.2; on 1.4.3 it finds its
hook sites by pattern (AOB) search. The "CameraControl" pattern, stored as a
text string in exefs/subsdk3 rodata (0x195C50), no longer matches 1.4.3's
main: two pattern bytes differ (an `ldr x0,[x0,#0x28]` immediate and a branch
displacement). The paired "CameraControlFix" pattern still matches, so the
game NOPs one call site without installing the replacement call and crashes.

The fix edits the pattern text: "08" -> "14" (pattern +0x35) and "fd" -> "fe"
(pattern +0x3D), i.e. three ASCII characters. The patched file is written as
an uncompressed NSO with consistent segment sizes and SHA-256 header fields.

Scope: only UltraCam 3.0 from nx-optimizer commit 9781528441 (exefs/subsdk3,
SHA-256 below) on TOTK 1.4.3. Any other file is refused and left untouched.

Usage:
  python ultracam_totk143_fix.py <subsdk3 or UltraCam exefs folder>
  python ultracam_totk143_fix.py <path> --check
  python ultracam_totk143_fix.py <path> --restore
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys

PRISTINE_SHA256 = "3273a1f6cf5af36a5135da74ced66e292e4655174807e883ab4408b73a636fb9"
PATCHED_SHA256 = "22b41025b4901654a63b8f5b9a797364da3a1ef904e8c5bda83655bb9f3a4d2d"
RODATA = 1  # segment index
# (offset within decompressed rodata, old, new): pattern text characters
PATCHES = [(0x2CCEF, b"0", b"1"), (0x2CCF0, b"8", b"4"), (0x2CD08, b"d", b"e")]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def lz4_block_decompress(src, out_len):
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]
        i += 1
        ln = tok >> 4
        if ln == 15:
            while True:
                b = src[i]
                i += 1
                ln += b
                if b != 255:
                    break
        dst += src[i:i + ln]
        i += ln
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if off == 0 or off > len(dst):
            raise ValueError("corrupt LZ4 stream")
        s = len(dst) - off
        for k in range(ml):
            dst.append(dst[s + k])
    if len(dst) != out_len:
        raise ValueError("LZ4 size mismatch")
    return bytes(dst)


def build_patched(data):
    if data[:4] != b"NSO0":
        raise ValueError("not an NSO")
    flags = struct.unpack_from("<I", data, 0xC)[0]
    segs = []
    for k in range(3):
        foff, memoff, size = struct.unpack_from("<III", data, 0x10 + k * 0x10)
        csize = struct.unpack_from("<I", data, 0x60 + k * 4)[0]
        raw = data[foff:foff + csize]
        if flags & (1 << k):
            raw = lz4_block_decompress(raw, size)
        segs.append((memoff, bytearray(raw)))
    ro = segs[RODATA][1]
    for off, old, new in PATCHES:
        if ro[off:off + 1] != old:
            raise ValueError("unexpected byte at rodata+0x%X" % off)
        ro[off:off + 1] = new
    hdr = bytearray(data[:0x100])
    struct.pack_into("<I", hdr, 0xC, 0)  # uncompressed, no segment hash check
    out = bytearray()
    foff = 0x100
    for k, (memoff, seg) in enumerate(segs):
        struct.pack_into("<III", hdr, 0x10 + k * 0x10, foff, memoff, len(seg))
        struct.pack_into("<I", hdr, 0x60 + k * 4, len(seg))
        hdr[0xA0 + k * 0x20:0xC0 + k * 0x20] = hashlib.sha256(bytes(seg)).digest()
        out += seg
        foff += len(seg)
    return bytes(hdr) + bytes(out)


def resolve(path):
    if os.path.isdir(path):
        for cand in (os.path.join(path, "subsdk3"), os.path.join(path, "exefs", "subsdk3")):
            if os.path.isfile(cand):
                return cand
        sys.exit("error: no subsdk3 found in %s" % path)
    if not os.path.isfile(path):
        sys.exit("error: %s does not exist" % path)
    return path


def main():
    ap = argparse.ArgumentParser(description="UltraCam 3.0 fix for TOTK 1.4.3")
    ap.add_argument("path", help="subsdk3 file or UltraCam exefs folder")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true", help="report status only")
    g.add_argument("--restore", action="store_true", help="restore subsdk3.orig")
    a = ap.parse_args()

    target = resolve(a.path)
    backup = target + ".orig"

    if a.restore:
        if not os.path.isfile(backup):
            sys.exit("error: no backup at %s" % backup)
        shutil.copyfile(backup, target)
        print("Restored %s from %s" % (target, backup))
        return

    with open(target, "rb") as f:
        cur = f.read()
    h = sha256(cur)
    if h == PATCHED_SHA256:
        print("Status: already fixed (matches the known-good patched file).")
        return
    if h != PRISTINE_SHA256:
        print("Status: not supported. This fix is only for UltraCam 3.0 from "
              "nx-optimizer 9781528441 (exefs/subsdk3, SHA-256 %s). Other "
              "versions are untouched." % PRISTINE_SHA256)
        print("Input SHA-256: %s" % h)
        sys.exit(1)
    if a.check:
        print("Status: pristine UltraCam 3.0 (9781528441); fix not yet applied.")
        return

    out = build_patched(cur)
    if sha256(out) != PATCHED_SHA256:
        sys.exit("FATAL: patched output hash %s != expected %s; nothing written."
                 % (sha256(out), PATCHED_SHA256))
    if os.path.exists(backup):
        print("Backup already exists, keeping it: %s" % backup)
    else:
        shutil.copyfile(target, backup)
        print("Backed up original to %s" % backup)
    with open(target, "wb") as f:
        f.write(out)
    with open(target, "rb") as f:
        if sha256(f.read()) != PATCHED_SHA256:
            sys.exit("FATAL: written file failed verification; run --restore.")
    print("Fixed %s (SHA-256 %s)." % (target, PATCHED_SHA256))


if __name__ == "__main__":
    main()
