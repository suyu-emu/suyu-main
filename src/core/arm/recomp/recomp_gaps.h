// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Recorded AOT coverage gaps.
//
// A Hybrid run records where it had to leave recompiled code, so a later export
// of the same title can seed block discovery at those addresses and a player
// can tell whether a strict static export would run at all.
//
// Two formats, kept apart:
//  - GapData / recomp_gaps.json ("suyu-recomp-gaps") is this machine's local
//    diagnostic store. Besides IDs, offsets and counts it keeps the raw 32-bit
//    encodings of instructions the recompiler could not handle, for development.
//    It stays local.
//  - SharedCoverage ("suyu-shared-coverage") is what Export Coverage writes and
//    what is meant to be passed between players: execution metadata only -
//    title and build IDs, code offsets, counts and aggregate totals. It has no
//    raw instruction words, module names, paths or free text, and its reader
//    rejects any field the schema does not define. Legacy recomp_gaps files are
//    imported through the same conversion, so their opcodes never reach it.
//
// Standard library only: the recompiler smoke tests build this file on its own.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Core::RecompGaps {

inline constexpr std::string_view kSchemaName = "suyu-recomp-gaps";
inline constexpr std::uint64_t kSchemaVersion = 1;

// Bounds. A file past them keeps what it has and says "truncated".
inline constexpr std::size_t kMaxModules = 64;
inline constexpr std::size_t kMaxOffsetsPerModule = 65536;
inline constexpr std::size_t kMaxOpcodes = 4096;
inline constexpr std::size_t kMaxFileBytes = 64u << 20;

inline constexpr std::string_view kSharedSchemaName = "suyu-shared-coverage";
inline constexpr std::uint64_t kSharedSchemaVersion = 1;
inline constexpr std::string_view kSharedDescription =
    "Execution metadata; no raw instruction bytes included.";
/// Counts stay exact in any JSON reader.
inline constexpr std::uint64_t kMaxSharedCount = (1ull << 53) - 1;
/// Module-relative code offsets: instruction-aligned and below 4 GiB.
inline constexpr std::uint64_t kMaxSharedOffset = 1ull << 32;

struct ModuleGaps {
    std::string name;     ///< informational only; build_id is the key
    std::string build_id; ///< 64 lower-case hex digits
    std::uint64_t hits = 0;
    /// Module-relative offset -> times execution reached it with no block.
    std::map<std::uint64_t, std::uint64_t> offsets;
};

struct GapData {
    std::string title_id; ///< 16 upper-case hex digits, or empty
    std::uint64_t runs = 0;
    std::uint64_t hybrid_runs = 0;
    std::uint64_t strict_runs = 0;
    /// Runs that left recompiled code for no reason at all.
    std::uint64_t clean_runs = 0;
    bool last_run_strict = false;
    bool truncated = false;
    /// Misses at a PC in no module this run knew about.
    std::uint64_t unattributed_misses = 0;
    /// Misses inside a module that has a recompiled image, keyed by build ID.
    std::map<std::string, ModuleGaps> modules;
    /// Modules that executed without any recompiled image, keyed by build ID.
    std::map<std::string, ModuleGaps> no_image;
    /// Guest encoding -> times it forced a fallback. Local diagnostics only.
    std::map<std::uint32_t, std::uint64_t> unimplemented;
    /// Unsupported-instruction totals imported from shared coverage, which carries
    /// counts but no encodings, so they cannot be merged into `unimplemented`.
    std::uint64_t imported_unsupported_kinds = 0;
    std::uint64_t imported_unsupported_hits = 0;

    std::uint64_t GapOffsets() const;
    std::uint64_t Misses() const;
    /// Distinct unsupported instructions known: recorded here, or at least as many
    /// as an imported file reported. A lower bound when both are present.
    std::uint64_t UnsupportedInstructionKinds() const;
    /// Nothing left recompiled code: no miss of any kind and no opcode.
    bool Clean() const;
};

struct SharedModule {
    std::uint64_t hits = 0;
    /// Module-relative offset -> times execution reached it with no block.
    std::map<std::uint64_t, std::uint64_t> offsets;
};

/// Shareable execution metadata. See the top of this file.
struct SharedCoverage {
    std::string title_id; ///< 16 upper-case hex digits
    std::uint64_t runs = 0;
    std::uint64_t hybrid_runs = 0;
    std::uint64_t strict_runs = 0;
    std::uint64_t clean_runs = 0;
    bool truncated = false;
    std::uint64_t unattributed_misses = 0;
    std::uint64_t unsupported_instruction_kinds = 0;
    std::uint64_t unsupported_instruction_hits = 0;
    /// Build ID -> offsets inside modules that have a recompiled image.
    std::map<std::string, SharedModule> modules;
    /// Build ID -> misses in modules that ran without any recompiled image.
    std::map<std::string, std::uint64_t> modules_without_image;
};

std::string TitleIdHex(std::uint64_t title_id);
/// 64 lower-case hex digits from raw build ID bytes (zero-padded to 32 bytes).
std::string BuildIdHex(const std::uint8_t* bytes, std::size_t size);
/// Lower-cases and zero-pads to 64 digits. Empty for anything that is not hex,
/// too long, or all zero: none of those identifies a module.
std::string NormalizeBuildId(std::string_view text);
/// Exact match after normalization; an invalid ID matches nothing.
bool BuildIdMatches(std::string_view a, std::string_view b);
/// A module name with any directories removed and only [A-Za-z0-9._-] kept.
std::string SanitizeName(std::string_view name);

/// What a recompiled image says about the module it was built from: its export
/// name ("rtld", "main", "subsdk0", "sdk") and that module's build ID, which is
/// empty for registrations that predate build IDs.
struct ImageIdentity {
    std::string name;
    std::string build_id;
};

/// The image that belongs to a loaded module, or nullopt for none. `load_index`
/// is the module's position in load order, `module_name` the kernel's name for
/// it ("nnrtld", "multimedia", "nnSdk") and `module_build_id` its build ID.
///
/// When any image carries a build ID, the build ID alone decides: an export
/// may omit modules (Hybrid runs them on the JIT), so an image's position in
/// the registration says nothing about which loaded module it belongs to.
/// Registrations without build IDs fall back to the NSO slot: the module's own
/// name (without the kernel's "nn" prefix, ignoring case), then the slot its
/// load position implies (rtld, main, subsdk0..9, sdk).
std::optional<std::size_t> MatchImage(const std::vector<ImageIdentity>& images,
                                      std::size_t load_index, std::string_view module_name,
                                      std::string_view module_build_id);

/// Bytes ModuleNameFromRodata may look at: the newer header plus the path struct.
inline constexpr std::size_t kRodataModuleNameBytes = 12 + 8 + 0x200;

/// The kernel's name for a module ("nnrtld", "EX-King.nss"): the module path
/// at the start of its read-only segment with any directories removed, or
/// empty when the segment starts with neither layout. Older SDKs start rodata
/// with {u32 0, s32 length, char path[length]}; newer ones (TOTK 1.4.3) put
/// {u32 1, u32 end of path, u32} in front of that same struct. `size` may be
/// smaller than kRodataModuleNameBytes.
std::string ModuleNameFromRodata(const std::uint8_t* rodata, std::size_t size);

/// One byte of guest memory at an absolute address (0 where unmapped).
using GuestRead8 = std::function<std::uint8_t(std::uint64_t)>;

/// Absolute address of a loaded module's MOD0 header, or 0 when it has none.
/// The module's second word (base + 4) is MOD0's offset from the base. Older
/// SDKs (TOTK 1.0.0) put MOD0 right after it, at +8; newer ones (TOTK 1.4.3
/// main, subsdk0 and sdk) put it deep in rodata, megabytes in, where a scan
/// of the first pages never reaches. That scan stays as the fallback for a
/// module whose word at +4 does not lead to the magic.
std::uint64_t FindMod0(std::uint64_t module_base, const GuestRead8& read8);

/// One .dynsym entry (Elf64_Sym) with its .dynstr name.
struct DynSymbol {
    std::string name;
    std::uint64_t value = 0;
    bool defined = false; ///< st_shndx != SHN_UNDEF
    bool weak = false;    ///< STB_WEAK
};
DynSymbol ReadDynSymbol(std::uint64_t symtab_va, std::uint64_t strtab_va, std::uint32_t index,
                        const GuestRead8& read8);

/// Adds every defined, named .dynsym symbol of the module at `module_base` to
/// `out` as name -> absolute address; a name already present is kept. .dynsym
/// and .dynstr are back to back, so the gap between them is the entry count.
void IndexModuleExports(std::uint64_t module_base, std::uint64_t symtab_va,
                        std::uint64_t strtab_va, const GuestRead8& read8,
                        std::unordered_map<std::string, std::uint64_t>& out);

/// What an import slot holds when no loaded module exports its symbol: 0 for
/// a weak one, as the ABI and the guest's rtld have it, else `trap`. A weak
/// import that some module does define resolves to that definition instead,
/// exactly like a strong one.
inline std::uint64_t UndefinedImportValue(bool weak, std::uint64_t trap) {
    return weak ? 0 : trap;
}

std::string Serialize(const GapData& data);
std::optional<GapData> Parse(std::string_view json, std::string* error = nullptr);
/// Adds `from` into `into`: run counts and hits add, offsets and opcodes dedupe.
void Merge(GapData& into, const GapData& from);

/// Recorded offsets for the module with exactly this build ID, ascending.
std::vector<std::uint64_t> RootsFor(const GapData& data, std::string_view build_id);
/// Stable over the (build ID, offset) set only, so extra runs or hits do not
/// change it. Empty when there are no offsets.
std::string Fingerprint(const GapData& data);

/// The shareable subset of local diagnostics: names, raw encodings and invalid
/// offsets are dropped; unsupported instructions become totals.
SharedCoverage ToShared(const GapData& data);
std::string SerializeShared(const SharedCoverage& data);
/// Strict: unknown or duplicate fields, wrong types and out-of-range values are errors.
std::optional<SharedCoverage> ParseShared(std::string_view json, std::string* error = nullptr);
/// Reads a shared file, or a legacy recomp_gaps file converted through ToShared.
std::optional<SharedCoverage> ParseImport(std::string_view json, std::string* error = nullptr,
                                          bool* legacy = nullptr);
/// Adds imported coverage into local diagnostics without touching their opcodes.
void MergeShared(GapData& into, const SharedCoverage& from);

std::optional<GapData> ReadFile(const std::filesystem::path& path, std::string* error = nullptr);
std::optional<SharedCoverage> ReadImportFile(const std::filesystem::path& path,
                                             std::string* error = nullptr,
                                             bool* legacy = nullptr);
bool WriteSharedFile(const std::filesystem::path& path, const SharedCoverage& data,
                     std::string* error = nullptr);
/// Writes through a temporary file and a rename, so a reader never sees half.
bool WriteFile(const std::filesystem::path& path, const GapData& data,
               std::string* error = nullptr);

/// One run's gaps, classified as they are recorded. Thread-safe; misses are
/// already the slow path, so a mutex and a map lookup cost nothing measurable.
class SessionRecorder {
public:
    void Begin(std::uint64_t title_id, bool strict);
    /// A module the loader placed at [base, base + size).
    void NoteModule(std::uint64_t base, std::uint64_t size, std::string_view name,
                    std::string build_id);
    void ForgetModule(std::uint64_t base);
    /// The module at `base` runs from a recompiled image named `image_name`.
    void NoteImage(std::uint64_t base, std::string_view image_name);
    /// Build ID of the noted module containing `address`; empty if none.
    std::string BuildIdAt(std::uint64_t address) const;
    /// Whether a noted module has this build ID or, when `build_id` is empty,
    /// this name (ignoring case).
    bool HasModule(std::string_view build_id, std::string_view name) const;
    void RecordMiss(std::uint64_t pc);
    void RecordUnimplemented(std::uint32_t insn);
    /// This run as a one-run GapData.
    GapData Snapshot() const;
    std::uint64_t TitleId() const;
    /// True once, after anything was recorded since the last call.
    bool TakeDirty();

private:
    struct Module {
        std::uint64_t size = 0;
        std::string name;
        std::string build_id;
        bool has_image = false;
    };
    mutable std::mutex lock;
    std::uint64_t title_id = 0;
    bool strict = false;
    bool dirty = true;
    std::map<std::uint64_t, Module> loaded;
    GapData run;
};

} // namespace Core::RecompGaps
