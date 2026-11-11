"""nup - reference CLI for the NUP (NX Package) format."""

from __future__ import annotations

import argparse
import os
import sys
import time
from typing import TYPE_CHECKING 

from . import format as F
from .access import app_meta, build_id, extract_tree, find_package, iter_children, \
    localized_titles, resolve_path
from . import edit as edit_cmd
from .pack import pack
from .read import NupReader
from .verify import verify

if TYPE_CHECKING:
    from .discover import FileObj
    from .read import NodeRef


def _parse_kv_flags(default: str | None, lang_flags: list[str], what: str
                    ) -> dict[str | None, str]:
    """Merge --name/--icon (default language) with --lang-name/--lang-icon LANG=VALUE."""
    out: dict[str | None, str] = {}
    if default:
        out[None] = default
    for item in lang_flags or []:
        lang, sep, value = item.partition("=")
        if not sep or not value:
            raise SystemExit(f"nup: --lang-{what} expects LANG=VALUE, got {item!r}")
        out[lang.lower()] = value
    return out


def _cmd_pack(args: argparse.Namespace) -> int:
    t0 = time.time()
    last = [0.0]

    def progress(done: int, total: int, fnode: NodeRef | FileObj) -> None:
        now = time.time()
        if now - last[0] >= 0.5 or done == total:
            last[0] = now
            pct = done * 100 // total if total else 100
            sys.stdout.write(f"\rpack: {done}/{total} files ({pct}%)")
            sys.stdout.flush()

    packages = pack(args.output, args.source, level=args.level,
                    keep_listings=args.keep_listings,
                    base_title_id=int(args.title_id, 16) if args.title_id else None,
                    display_version=args.display_version,
                    publisher=args.publisher,
                    names=_parse_kv_flags(args.name, args.lang_name, "name"),
                    icons=_parse_kv_flags(args.icon, args.lang_icon, "icon"),
                    split=args.split,
                    progress=None if args.quiet else progress)
    if not args.quiet:
        print()
    for p in packages:
        print(f"  {F.KIND_NAMES[p.kind]:<6} {p.title_id:016X} v{p.version} <- {p.src}")
    print(f"pack: wrote {args.output} in {time.time() - t0:.1f}s")
    return 0


def _print_tree(r: NupReader, dir_index: int, prefix: str, max_depth: int, depth: int = 0,
                show_files: bool = True) -> None:
    for child in iter_children(r, dir_index):
        label = child.name.decode("utf-8", "surrogateescape")
        if child.is_dir:
            print(f"{prefix}{label}/")
            if depth + 1 < max_depth:
                _print_tree(r, child.index, prefix + "  ", max_depth, depth + 1, show_files)
            elif any(c.is_dir for c in iter_children(r, child.index)):
                print(f"{prefix}  ...")
        elif show_files:
            name, size, sha, eoff, esz, codec = r.file_info(child.index)
            tag = "zstd" if codec == F.CODEC_ZSTD else ("raw" if size else "empty")
            print(f"{prefix}{label}  [{size} B, {tag}]")


def _cmd_info(args: argparse.Namespace) -> int:
    with NupReader(args.file) as r:
        s = r.super
        uuid = s["uuid"].hex()
        print(f"file:      {args.file}")
        print(f"version:   NUP v1 (header 0x{s['header_size']:x}, v{s['header_version']})")
        print(f"size:      {s['file_size']:,} bytes")
        print(f"uuid:      {uuid[:8]}-{uuid[8:12]}-{uuid[12:16]}")
        print(f"toc:       @{s['toc_offset']:#x} +{s['toc_size']:,} B")
        print(f"blobs:     @{s['blob_offset']:#x}")
        print(f"packages:  {len(r.packages)}  (dirs {r.dir_count}, files {r.file_count})")
        for p in r.packages:
            meta = app_meta(r, p)
            name = meta.get(F.TLV_TITLE_NAME, b"").decode("utf-8", "replace")
            pub = meta.get(F.TLV_PUBLISHER, b"").decode("utf-8", "replace")
            label = f" {name!r}" if name else ""
            if pub:
                label += f" by {pub!r}"
            if p.kind == F.KIND_UPDATE and (p.flags & F.PKG_FLAG_STANDALONE_UPDATE):
                label += " [standalone]"
            # Version reporting: the display version TLV is the truth. The raw
            # NCM value decodes to major.minor.micro only on Switch 1; Switch 2
            # titles store a per-title content revision counter there (one
            # 0x10000 step per released update), so label revision-aligned
            # values as revisions instead of pretending the decode is a version.
            disp = meta.get(F.TLV_DISPLAY_VERSION, b"").decode("utf-8", "replace")
            if disp:
                ver = f"display {disp} (NCM v{p.version:#x})"
            elif p.version == 0:
                ver = "no version stored"
            elif p.version & 0xFFFF == 0:
                ver = f"content revision {p.version >> 16} (NCM v{p.version:#x}; " \
                      f"S1-style decode {p.version_str}, display unknown)"
            else:
                ver = f"{p.version_str} (NCM v{p.version:#x})"
            print(f"  [{p.index}] {p.kind_name:<6} {p.title_id:016X} {ver}{label}")
            loc = localized_titles(r, p)
            if loc:
                langs = ", ".join(f"{l.decode()}={n.decode('utf-8', 'replace')!r}"
                                  for l, n in sorted(loc.items()))
                print(f"         localized: {langs}")
            bid = build_id(r, p)
            if bid:
                print(f"         build-id: {bid.rstrip(bytes(1)).hex().upper()}")
            if not args.tree:
                continue
            if p.exefs_dir != F.NO_DIR:
                print(f"      exefs/")
                _print_tree(r, p.exefs_dir, "        ", args.depth)
            if p.romfs_dir != F.NO_DIR:
                print(f"      romfs/  (top {args.depth} levels)")
                _print_tree(r, p.romfs_dir, "        ", args.depth, show_files=False)
    return 0


def _cmd_list(args: argparse.Namespace) -> int:
    with NupReader(args.file) as r:
        for fi, (name, size, sha, eoff, esz, codec) in enumerate(r.files):
            tag = "zstd" if codec == F.CODEC_ZSTD else ("raw" if size else "empty")
            print(f"{fi:>8}  {size:>12}  {tag:<4}  {name.decode('utf-8', 'surrogateescape')}")
    return 0


def _cmd_extract(args: argparse.Namespace) -> int:
    with NupReader(args.file) as r:
        if args.flat:
            # Rescue mode: dump the file table directly, ignoring the directory
            # tree. Recovers content from containers whose children array is
            # broken (files present but not linked into any directory).
            os.makedirs(args.dest, exist_ok=True)
            n = 0
            for fi in range(r.file_count):
                name, size, sha, eoff, esz, codec = r.file_info(fi)
                target = os.path.join(args.dest, f"{fi:05d}_{name.decode('utf-8', 'surrogateescape')}")
                data = r.read_file(fi)
                with open(target, "wb") as fh:
                    fh.write(data)
                n += 1
            print(f"extract: {n} files -> {args.dest}")
            return 0
        pkg = find_package(r, kind=_kind_arg(args.package)) if args.package else r.packages[0]
        if pkg is None:
            print(f"extract: no package matches {args.package!r}", file=sys.stderr)
            return 1
        src_dir = pkg.root_dir
        if args.subtree:
            ref = resolve_path(r, pkg, args.subtree)
            if ref is None:
                print(f"extract: no such subtree {args.subtree!r}", file=sys.stderr)
                return 1
            if not ref.is_dir:
                print(f"extract: {args.subtree!r} is a file; use the tree root", file=sys.stderr)
                return 1
            src_dir = ref.index
        n = extract_tree(r, args.dest, src_dir)
        print(f"extract: {n} files -> {args.dest}")
    return 0


def _kind_arg(name: str) -> int | None:
    for k, v in F.KIND_NAMES.items():
        if v == name:
            return k
    try:
        return int(name, 0)
    except ValueError:
        return None


def _cmd_verify(args: argparse.Namespace) -> int:
    ok, problems = verify(args.file, deep=not args.fast)
    if ok:
        print(f"verify: OK ({'deep' if not args.fast else 'fast'}) - {args.file}")
        return 0
    print(f"verify: FAILED - {args.file}")
    for p in problems[:args.max_problems]:
        print(f"  - {p}")
    if len(problems) > args.max_problems:
        print(f"  ... and {len(problems) - args.max_problems} more")
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="nup", description="NUP (NX Package) container tool")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="pack a dump directory into an .nup")
    p.add_argument("source", help="dump dir (base/updates/dlc layout, or a content dir)")
    p.add_argument("output", help="output .nup path")
    p.add_argument("--level", type=int, default=3, help="zstd level (default 3)")
    p.add_argument("--title-id", help="override base title ID (16 hex digits)")
    p.add_argument("--display-version", help="store a display version string in meta")
    p.add_argument("--publisher", help="store a publisher string in meta (TLV 0x0006)")
    p.add_argument("--name", help="game title, default language (TLV 0x0005)")
    p.add_argument("--lang-name", action="append", metavar="LANG=NAME",
                   help="localized title, repeatable, BCP-47 tag (e.g. --lang-name ja=ゼルダ)")
    p.add_argument("--icon", metavar="PATH", help="icon image, default language (TLV 0x0003)")
    p.add_argument("--lang-icon", action="append", metavar="LANG=PATH",
                   help="localized icon, repeatable (e.g. --lang-icon ja=icon_ja.dat)")
    p.add_argument("--keep-listings", action="store_true", help="keep .listing artifacts")
    p.add_argument("--split", action="store_true",
                   help="write one .nup per package group instead of a single container: "
                        "base (boots alone), one per update (swap/downgrade by file), dlc")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(fn=_cmd_pack)

    edit_cmd.register(sub)

    p = sub.add_parser("info", help="show container info")
    p.add_argument("file")
    p.add_argument("--tree", action="store_true", help="also print trees")
    p.add_argument("--depth", type=int, default=2)
    p.set_defaults(fn=_cmd_info)

    p = sub.add_parser("list", help="list all file nodes")
    p.add_argument("file")
    p.set_defaults(fn=_cmd_list)

    p = sub.add_parser("extract", help="extract a package (or subtree) to a directory")
    p.add_argument("file")
    p.add_argument("dest")
    p.add_argument("--package", help="kind name (base/update/addon) or index")
    p.add_argument("--subtree", help="relative path from package root, e.g. romfs")
    p.add_argument("--flat", action="store_true",
                   help="rescue mode: dump the file table directly, ignoring the "
                        "directory tree (for containers with a broken tree)")
    p.set_defaults(fn=_cmd_extract)

    p = sub.add_parser("verify", help="verify integrity (structure + per-file SHA-256)")
    p.add_argument("file")
    p.add_argument("--fast", action="store_true", help="skip per-file SHA-256 recompute")
    p.add_argument("--max-problems", type=int, default=20)
    p.set_defaults(fn=_cmd_verify)

    args = ap.parse_args(argv)
    try:
        return int(args.fn(args))
    except F.NupError as e:
        print(f"nup: error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
