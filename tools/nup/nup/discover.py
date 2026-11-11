"""NUP dump discovery: source layout conventions and the in-memory tree model.

Recognizes dump directory layouts (base/updates/dlc bundles, update-only
dumps, or a single content dir), resolves each package's identity from its
ExeFS (NPDM program ID, NSO/NRO build ID), and walks source trees into the
DirObj/FileObj/Tree model the packer serializes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import os
import re
import struct
from typing import Final

from . import format as F

# SPEC §6.3 reserved artifact names
ARTIFACT_NAMES: Final = {b".listing"}
# child-slot marker: slot >= FILE_TAG references files[]
FILE_TAG: Final = 0x8000_0000
_DEC: Final = re.compile(r"^\d+$")
_HEX: Final = re.compile(r"^[0-9a-fA-F]{16}$")


def parse_npdm_program_id(data: bytes) -> int | None:
    """Extract the program ID from an NPDM (any known header revision).

    ACI0 layout: magic 'ACI0' (4), reserved (0xC), program id (u64 LE) at +0x10.
    """
    off = data.find(b"ACI0")
    if off < 0 or off + 0x18 > len(data):
        return None
    return int(struct.unpack_from("<Q", data, off + 0x10)[0])


@dataclass(slots=True)
class DirObj:
    name: bytes
    children: list[DirObj | FileObj] = field(default_factory=list)
    id: int = -1
    first_child: int = 0
    child_count: int = 0


@dataclass(slots=True)
class FileObj:
    name: bytes
    src_path: str
    size: int = 0
    sha256: bytes = b"\0" * 32
    extent_offset: int = 0
    extent_size: int = 0
    codec: F.Codec = F.CODEC_RAW
    codec_flags: int = 0


@dataclass(slots=True)
class PackInput:
    """One package to store: kind, ids, and the source directory of its tree."""

    kind: F.Kind
    src: str
    title_id: int = 0
    version: int = 0
    flags: int = 0
    npdm: bytes | None = None
    build_id: bytes | None = None


def _find_npdm(src: str) -> str | None:
    for cand in ("exefs/main.npdm", "main.npdm"):
        p = os.path.join(src, cand)
        if os.path.isfile(p):
            return p
    return None


def _read_build_id(src: str) -> bytes | None:
    """ModuleId: bytes 0x40..0x60 of exefs/main (NSO0/NRO0 build-id field).

    Returns None when main is absent, shorter than 0x60 bytes, or has an
    unknown magic - the TLV is optional metadata.
    """
    for cand in ("exefs/main", "main"):
        p = os.path.join(src, cand)
        if os.path.isfile(p):
            with open(p, "rb") as fh:
                head = fh.read(0x60)
            if len(head) == 0x60 and (head[:4] == b"NSO0" or head[0x10:0x14] == b"NRO0"):
                return head[0x40:0x60]
            return None
    return None


def _resolve_title_id(pkg: PackInput, override: int | None) -> None:
    npdm_path = _find_npdm(pkg.src)
    if npdm_path:
        with open(npdm_path, "rb") as fh:
            pkg.npdm = fh.read()
        pid = parse_npdm_program_id(pkg.npdm)
        if pid is not None and pid != 0:
            pkg.title_id = pid
    pkg.build_id = _read_build_id(pkg.src)
    if override is not None:
        pkg.title_id = override


def _looks_like_content(d: str) -> bool:
    return os.path.isdir(os.path.join(d, "exefs")) or os.path.isdir(os.path.join(d, "romfs"))


def _has_files(d: str) -> bool:
    """True when a package dir contains at least one packable file. Dump-tool
    artifacts don't count, and neither do folders that only hold empty dirs -
    an exefs/ folder with no files is not a usable ExeFS, and a package whose
    record points at one misleads consumers (they treat it as the ExeFS)."""
    for _, _, filenames in os.walk(d):
        for name in filenames:
            if name != ".listing":
                return True
    return False


def _collect_updates(upd_dir: str, base_title_id: int, flags: int) -> list[PackInput]:
    """updates/<decimal version>/ content dirs; flags propagates standalone_update."""
    out: list[PackInput] = []
    for name in sorted(os.listdir(upd_dir)):
        p = os.path.join(upd_dir, name)
        if not _DEC.match(name) or not os.path.isdir(p) or not _has_files(p):
            continue
        upd = PackInput(F.KIND_UPDATE, p, version=int(name, 10), flags=flags)
        _resolve_title_id(upd, None)
        if upd.title_id == 0:
            upd.title_id = base_title_id
        out.append(upd)
    return out


def _collect_dlcs(dlc_dir: str) -> list[PackInput]:
    """dlc/<16-hex title id>/<index>/ content dirs (SPEC §3.1.2 addon ids)."""
    out: list[PackInput] = []
    for name in sorted(os.listdir(dlc_dir)):
        tdir = os.path.join(dlc_dir, name)
        if not _HEX.match(name) or not os.path.isdir(tdir):
            continue
        for idx in sorted(os.listdir(tdir)):
            cdir = os.path.join(tdir, idx)
            if os.path.isdir(cdir) and _has_files(cdir):
                out.append(PackInput(F.KIND_ADDON, cdir, title_id=int(name, 16)))
    return out


def discover(src: str, base_title_id: int | None) -> list[PackInput]:
    """Discover packages in a dump directory (SPEC §3.1.3 conventions)."""
    packages: list[PackInput] = []
    upd_dir = os.path.join(src, "updates")
    has_updates = os.path.isdir(upd_dir)

    if os.path.isdir(os.path.join(src, "base")):
        base = PackInput(F.KIND_BASE, os.path.join(src, "base"))
        _resolve_title_id(base, base_title_id)
        if _has_files(base.src):
            packages.append(base)
            if has_updates:
                packages.extend(_collect_updates(upd_dir, base.title_id, flags=0))
        elif has_updates:
            # an empty base/ is not a game: drop it so a real update can boot
            # as the standalone program instead of hiding behind a dead base
            packages.extend(_collect_updates(upd_dir, base_title_id or 0,
                                             flags=F.PKG_FLAG_STANDALONE_UPDATE))
        dlc_dir = os.path.join(src, "dlc")
        if os.path.isdir(dlc_dir):
            packages.extend(_collect_dlcs(dlc_dir))
    elif has_updates:
        # Update-only dump:
        # every update package carries standalone_update (SPEC §3.1.2, flags
        # bit 0 at offset 0x19). DLC may still ride along in dlc/.
        packages.extend(_collect_updates(upd_dir, base_title_id or 0,
                                         flags=F.PKG_FLAG_STANDALONE_UPDATE))
        dlc_dir = os.path.join(src, "dlc")
        if os.path.isdir(dlc_dir):
            packages.extend(_collect_dlcs(dlc_dir))
    elif _looks_like_content(src):
        base = PackInput(F.KIND_BASE, src)
        _resolve_title_id(base, base_title_id)
        packages.append(base)
    else:
        raise SystemExit(
            f"pack: {src!r} is neither a dump bundle (base/updates/dlc), an update-only "
            f"dump (updates/), nor a content directory (exefs/ or romfs/)")
    if not packages:
        raise SystemExit(f"pack: no packages found in {src!r}")
    return packages


@dataclass(slots=True)
class Tree:
    """Node store for one pack run (global across packages)."""

    artifacts: set[bytes]
    dirs: list[DirObj] = field(default_factory=list)
    files: list[FileObj] = field(default_factory=list)
    # serial ids; see assign()
    children: list[int] = field(default_factory=list)
    pool: dict[bytes, int] = field(default_factory=dict)

    @property
    def pool_size(self) -> int:
        return sum(map(len, self.pool))

    def intern(self, name: bytes) -> int:
        off = self.pool.get(name)
        if off is None:
            off = self.pool_size
            self.pool[name] = off
        return off

    def add_tree(self, src: str) -> DirObj:
        return self._walk(src, b"")

    def _walk(self, src: str, name: bytes) -> DirObj:
        node = DirObj(name)
        try:
            entries = sorted(os.scandir(src),
                             key=lambda e: e.name.encode("utf-8", "surrogateescape"))
        except OSError as exc:
            raise SystemExit(f"pack: cannot read {src}: {exc}")
        for entry in entries:
            ename = entry.name.encode("utf-8", "surrogateescape")
            if entry.is_dir(follow_symlinks=False):
                node.children.append(self._walk(entry.path, ename))
            elif ename not in self.artifacts:
                node.children.append(FileObj(ename, entry.path))
        return node

    def assign(self, roots: list[DirObj]) -> None:
        """Assign serial order: dirs pre-order, files in first-reference order,
        contiguous child slots per directory."""
        def rec(d: DirObj) -> int:
            did = len(self.dirs)
            d.id = did
            self.dirs.append(d)
            slot_base = len(self.children)
            self.children.extend([0] * len(d.children))
            for i, child in enumerate(d.children):
                if isinstance(child, FileObj):
                    self.children[slot_base + i] = FILE_TAG + len(self.files)
                    self.files.append(child)
            for i, child in enumerate(d.children):
                if isinstance(child, DirObj):
                    self.children[slot_base + i] = rec(child)
            d.first_child = slot_base
            d.child_count = len(d.children)
            return did

        for root in roots:
            rec(root)
