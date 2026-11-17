"""NUP metadata editor: rewrite a container's meta blocks in place.

Meta blocks are the trailing region of a .nup, so an edit rebuilds only
that tail: everything before it (superblock, TOC, and all blob extents)
is preserved byte-for-byte, per-package meta_offset/meta_size are patched
in the TOC, the TOC is re-hashed and the superblock rewritten. The UUID
is unaffected (SPEC §2.1 covers only title_id/version/kind). Edits merge
into the existing metadata: unspecified TLVs are preserved, and package
level TLVs (NPDM, build ID, source dump) are always kept.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import time
from typing import Final

from . import format as F
from . import meta as M
from .read import NupReader, PackageInfo

APP_TLVS: Final = (F.TLV_TITLE_NAME, F.TLV_PUBLISHER, F.TLV_DISPLAY_VERSION,
                   F.TLV_ICON, F.TLV_LOCALIZED_TITLES, F.TLV_LOCALIZED_ICONS)
_COPY_CHUNK: Final = 4 << 20


def _old_block(r: NupReader, pkg: PackageInfo) -> bytes:
    if not pkg.meta_size:
        return b""
    return bytes(r._mm[pkg.meta_offset:pkg.meta_offset + pkg.meta_size])


def _resolve_package(r: NupReader, package: str | None) -> PackageInfo:
    """Explicit kind name / index, else the package already carrying app
    metadata, else base, else the first package (SPEC §5.1)."""
    if package is not None:
        if package.isdigit() and package not in F.KIND_NAMES.values():
            idx = int(package)
            if idx >= len(r.packages):
                raise SystemExit(f"edit: package index {idx} out of range "
                                 f"({len(r.packages)} packages)")
            return r.packages[idx]
        for kind, name in F.KIND_NAMES.items():
            if name == package:
                found = next((p for p in r.packages if p.kind == kind), None)
                if found is None:
                    raise SystemExit(f"edit: no {name} package in container")
                return found
        raise SystemExit(f"edit: unknown package {package!r} "
                         f"(kind name or index expected)")
    for p in r.packages:
        if p.meta_size and any(t in M.parse_block(_old_block(r, p)) for t in APP_TLVS):
            return p
    return next((p for p in r.packages if p.kind == F.KIND_BASE), r.packages[0])


def _replace_retry(tmp: str, path: str, attempts: int = 6) -> None:
    """os.replace can transiently fail with PermissionError on Windows when
    another process (antivirus, indexer, cloud sync) still holds the
    destination - back off and retry instead of failing the whole edit."""
    for attempt in range(attempts):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.25 * (attempt + 1))


def edit(path: str, *, package: str | None = None, name: str | None = None,
         publisher: str | None = None, display_version: str | None = None,
         icon: str | None = None, lang_names: dict[str, str] | None = None,
         lang_icons: dict[str, str] | None = None, clear_name: bool = False,
         clear_publisher: bool = False, clear_display_version: bool = False,
         clear_icon: bool = False) -> PackageInfo:
    """Edit one package's meta block in place. Returns the edited package."""
    tmp = path + ".tmp"
    try:
        r = NupReader(path)  # full validation before anything is written
        try:
            target = _resolve_package(r, package)
            records = [M.parse_block(_old_block(r, p)) for p in r.packages]
            idx = r.packages.index(target)
            records[idx] = M.merge_app_meta(
                records[idx], name=name, publisher=publisher,
                display_version=display_version, icon=icon, lang_names=lang_names,
                lang_icons=lang_icons, clear_name=clear_name,
                clear_publisher=clear_publisher,
                clear_display_version=clear_display_version, clear_icon=clear_icon)

            serialized = [M.serialize_block(recs) for recs in records]
            new_tail = b"".join(serialized)

            # end of the blob region = first meta block (only extents before it)
            meta_starts = [p.meta_offset for p in r.packages if p.meta_size]
            blob_end = min(meta_starts) if meta_starts else r.super["file_size"]

            new_offsets: list[tuple[int, int]] = []
            cursor = blob_end
            for block in serialized:
                if block:
                    new_offsets.append((cursor, len(block)))
                    cursor += len(block)
                else:
                    new_offsets.append((0, 0))  # no meta block for this package
            new_file_size = cursor

            s = r.super
            toc = bytearray(r._mm[s["toc_offset"]:s["toc_offset"] + s["toc_size"]])
            pos = F._toc_head_s.size
            for i, p in enumerate(r.packages):
                mo, ms = new_offsets[i]
                F._pkg_s.pack_into(toc, pos, p.title_id, p.root_dir, p.exefs_dir,
                                   p.romfs_dir, p.version, p.kind, p.flags, 0, 0, mo, ms)
                pos += F.PKG_SIZE

            toc_sha = hashlib.sha256(toc).digest()
            superblock = F.pack_super(F.MAGIC, s["header_size"], s["header_version"],
                                      s["toc_offset"], s["toc_size"], toc_sha,
                                      s["blob_offset"], new_file_size,
                                      len(r.packages), s["flags"], s["uuid"])

            with open(tmp, "wb") as out:
                out.write(superblock)
                out.write(toc)
                copy_pos = s["toc_offset"] + s["toc_size"]
                while copy_pos < blob_end:
                    chunk = r._mm[copy_pos:min(copy_pos + _COPY_CHUNK, blob_end)]
                    out.write(chunk)
                    copy_pos += len(chunk)
                out.write(new_tail)
        finally:
            # Release the original (file handle and mmap) before replacing it:
            # on Windows a rename over a still-open file fails with
            # PermissionError (WinError 5).
            r.close()

        with NupReader(tmp):  # structural validation of the result
            pass
        _replace_retry(tmp, path)
        return target
    except BaseException:
        try:
            os.unlink(tmp)  # keep the original, drop the partial result
        except OSError:
            pass
        raise


def _parse_langs(items: list[str] | None, what: str) -> dict[str, str]:
    """--lang-* items: LANG=VALUE sets, LANG= (empty value) removes."""
    out: dict[str, str] = {}
    for item in items or []:
        lang, sep, value = item.partition("=")
        if not sep:
            raise SystemExit(f"nup: --lang-{what} expects LANG=VALUE, got {item!r}")
        out[lang.lower()] = value
    return out


def cmd(args: argparse.Namespace) -> int:
    lang_names = _parse_langs(args.lang_name, "name")
    lang_icons = _parse_langs(args.lang_icon, "icon")
    touched = [args.name, args.publisher, args.display_version, args.icon,
               bool(lang_names), bool(lang_icons), args.clear_name,
               args.clear_publisher, args.clear_display_version, args.clear_icon]
    if not any(touched):
        raise SystemExit("edit: nothing to do - provide at least one metadata flag")
    target = edit(args.file, package=args.package, name=args.name,
                  publisher=args.publisher, display_version=args.display_version,
                  icon=args.icon, lang_names=lang_names, lang_icons=lang_icons,
                  clear_name=args.clear_name, clear_publisher=args.clear_publisher,
                  clear_display_version=args.clear_display_version,
                  clear_icon=args.clear_icon)
    print(f"edit: updated {target.kind_name} package {target.title_id:016X} in {args.file}")
    return 0


def register(sub: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    """Wire the `edit` subcommand into the CLI."""
    p = sub.add_parser("edit", help="edit package metadata in an existing .nup")
    p.add_argument("file")
    p.add_argument("--package", help="target package: kind name or index "
                                     "(default: where app metadata lives, else base)")
    p.add_argument("--name", help="set title name, default language (TLV 0x0005)")
    p.add_argument("--publisher", help="set publisher (TLV 0x0006)")
    p.add_argument("--display-version", help="set display version (TLV 0x0004)")
    p.add_argument("--icon", metavar="PATH", help="set icon image, default language (TLV 0x0003)")
    p.add_argument("--lang-name", action="append", metavar="LANG=NAME",
                   help="set localized title; LANG= (empty value) removes it")
    p.add_argument("--lang-icon", action="append", metavar="LANG=PATH",
                   help="set localized icon; LANG= (empty value) removes it")
    p.add_argument("--clear-name", action="store_true", help="remove the title name TLV")
    p.add_argument("--clear-publisher", action="store_true", help="remove the publisher TLV")
    p.add_argument("--clear-display-version", action="store_true",
                   help="remove the display version TLV")
    p.add_argument("--clear-icon", action="store_true", help="remove the icon TLV")
    p.set_defaults(fn=cmd)
