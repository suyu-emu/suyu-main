// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/recompiler/nso_image.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace suyu::recomp {

constexpr std::size_t NsoElf64DynamicEntrySize = 0x10;
constexpr std::size_t NsoElf64RelaEntrySize = 0x18;
constexpr std::size_t NsoElf64SymbolEntrySize = 0x18;
constexpr std::size_t DefaultNsoDynamicEntryLimit = 4096;
constexpr std::uint64_t DefaultNsoRelocationLimit = 4'000'000;

enum class NsoRelaTableKind {
    Dynamic,
    ProcedureLinkage,
};

/// A raw ELF64 .dynamic entry. Unknown tags are retained so callers can inspect metadata that this
/// parser does not interpret yet.
struct NsoDynamicEntry {
    /// Module-relative address of the entry and of its value field.
    std::uint64_t address{};
    std::uint64_t value_address{};
    std::int64_t tag{};
    std::uint64_t value{};
};

/// The minimum 0x1c-byte MOD0 header shared by all currently documented versions.
struct NsoMod0Header {
    std::uint64_t address{};
    std::uint32_t dynamic_offset{};
    std::uint32_t bss_start_offset{};
    std::uint32_t bss_end_offset{};
    std::uint32_t exception_info_start_offset{};
    std::uint32_t exception_info_end_offset{};
    std::uint32_t module_object_offset{};
};

struct NsoRelaRecord {
    std::uint64_t offset{};
    std::uint64_t info{};
    std::int64_t addend{};
    /// Decoded from r_info but not proven against the true dynsym count. A symbol consumer must
    /// first derive that count from DT_HASH/DT_GNU_HASH and reject indices outside it.
    std::uint32_t symbol_index{};
    std::uint32_t type{};
};

struct NsoRelaTable {
    NsoRelaTableKind kind{NsoRelaTableKind::Dynamic};
    /// Module-relative address from DT_RELA or DT_JMPREL.
    std::uint64_t address{};
    std::uint64_t byte_size{};
    std::uint64_t entry_size{NsoElf64RelaEntrySize};
    std::vector<NsoRelaRecord> records;
};

struct NsoSysvHashInfo {
    /// Module-relative address from DT_HASH.
    std::uint64_t address{};
    std::uint64_t byte_size{};
    std::uint32_t bucket_count{};
    /// The SysV hash chain count is the exact dynamic-symbol count.
    std::uint32_t chain_count{};
};

struct NsoDynamicInfo {
    NsoMod0Header mod0;
    std::uint64_t dynamic_address{};
    /// Includes the terminating DT_NULL entry.
    std::uint64_t dynamic_byte_size{};
    /// Excludes the terminating DT_NULL entry.
    std::vector<NsoDynamicEntry> entries;

    std::optional<std::uint64_t> string_table_address;
    std::optional<std::uint64_t> string_table_size;
    std::optional<std::uint64_t> symbol_table_address;
    std::optional<NsoSysvHashInfo> sysv_hash;
    std::uint64_t symbol_entry_size{NsoElf64SymbolEntrySize};
    std::uint64_t rela_entry_size{NsoElf64RelaEntrySize};
    /// Defaults to DT_RELA when omitted, matching the NSO AArch64 convention.
    std::uint64_t plt_relocation_tag{7};

    /// Value fields the hosted runtime may clear after a complete relocation commit.
    std::optional<std::uint64_t> rela_size_value_address;
    std::optional<std::uint64_t> plt_rela_size_value_address;

    std::optional<NsoRelaTable> rela;
    std::optional<NsoRelaTable> plt_rela;
};

struct NsoDynamicParseResult {
    std::optional<NsoDynamicInfo> info;
    std::vector<std::string> warnings;
    std::string error;

    explicit operator bool() const {
        return info.has_value() && error.empty();
    }
};

using NsoModuleReadCallback = bool (*)(const void* user, std::uint64_t module_address,
                                       std::span<std::uint8_t> destination);

/// A non-owning, module-relative reader. Valid addresses lie in [0, image_size); the callback must
/// return false for an unreadable range. It must fill the supplied destination synchronously;
/// parser code never retains spans across calls.
struct NsoModuleView {
    std::uint64_t image_size{};
    std::uint64_t text_address{};
    const void* user{};
    NsoModuleReadCallback read{};
};

/// Locates MOD0 through the module-header offset at text+4, walks the terminated ELF64 .dynamic
/// array, and decodes bounded DT_RELA and RELA-format DT_JMPREL tables. Addresses stored in dynamic
/// tags remain module-relative. Relocations are reported but never applied. A relocation target is
/// checked only as an in-image address; the eventual consumer must validate the supported type's
/// complete write extent and alignment before reading or writing it.
NsoDynamicParseResult ParseNsoDynamic(const NsoModuleView& module,
                                      std::size_t max_dynamic_entries = DefaultNsoDynamicEntryLimit,
                                      std::uint64_t max_relocations = DefaultNsoRelocationLimit);

/// Convenience overload using MakeDecodedNsoModuleView. It also rejects inconsistent decoded
/// segment metadata before parsing.
NsoDynamicParseResult ParseNsoDynamic(const DecodedNso& image,
                                      std::size_t max_dynamic_entries = DefaultNsoDynamicEntryLimit,
                                      std::uint64_t max_relocations = DefaultNsoRelocationLimit);

const char* NsoRelaTableKindName(NsoRelaTableKind kind);

} // namespace suyu::recomp
