// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/recompiler/nso_dynamic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace suyu::recomp {

constexpr std::uint64_t DefaultNsoDynamicSymbolLimit = 1'000'000;
constexpr std::uint64_t DefaultNsoDynamicSymbolNameBytesLimit = std::uint64_t{64} << 20;
constexpr std::uint64_t DefaultNsoDynamicStringTableLimit = std::uint64_t{64} << 20;

constexpr std::uint8_t NsoElfSymbolBindingLocal = 0;
constexpr std::uint8_t NsoElfSymbolBindingGlobal = 1;
constexpr std::uint8_t NsoElfSymbolBindingWeak = 2;
constexpr std::uint8_t NsoElfSymbolTypeGnuIfunc = 10;
constexpr std::uint8_t NsoElfSymbolVisibilityDefault = 0;
constexpr std::uint8_t NsoElfSymbolVisibilityInternal = 1;
constexpr std::uint8_t NsoElfSymbolVisibilityHidden = 2;
constexpr std::uint8_t NsoElfSymbolVisibilityProtected = 3;
constexpr std::uint16_t NsoElfSectionUndefined = 0;
constexpr std::uint16_t NsoElfSectionLowReserved = 0xFF00;
constexpr std::uint16_t NsoElfSectionAbsolute = 0xFFF1;
constexpr std::uint16_t NsoElfSectionCommon = 0xFFF2;

/// One bounded Elf64_Sym record and its name from proven dynsym/dynstr extents.
struct NsoDynamicSymbol {
    std::uint32_t index{};
    std::uint64_t address{};
    std::uint32_t name_offset{};
    std::uint8_t info{};
    std::uint8_t other{};
    std::uint16_t section_index{};
    std::uint64_t value{};
    std::uint64_t size{};
    std::string name;

    std::uint8_t Binding() const {
        return info >> 4;
    }

    std::uint8_t Type() const {
        return info & 0xF;
    }

    std::uint8_t Visibility() const {
        return other & 0x3;
    }

    bool IsUndefined() const {
        return section_index == NsoElfSectionUndefined;
    }

    bool IsAbsolute() const {
        return section_index == NsoElfSectionAbsolute;
    }

    bool IsWeak() const {
        return Binding() == NsoElfSymbolBindingWeak;
    }

    bool IsExternallyVisibleDefinition() const {
        const std::uint8_t binding = Binding();
        const std::uint8_t visibility = Visibility();
        return !IsUndefined() && !name.empty() &&
               (binding == NsoElfSymbolBindingGlobal || binding == NsoElfSymbolBindingWeak) &&
               (visibility == NsoElfSymbolVisibilityDefault ||
                visibility == NsoElfSymbolVisibilityProtected);
    }
};

struct NsoDynamicSymbolInfo {
    std::uint64_t symbol_table_address{};
    std::uint64_t symbol_table_byte_size{};
    std::uint64_t string_table_address{};
    std::uint64_t string_table_byte_size{};
    std::vector<NsoDynamicSymbol> symbols;
};

struct NsoDynamicSymbolParseResult {
    std::optional<NsoDynamicSymbolInfo> info;
    std::string error;

    explicit operator bool() const {
        return info.has_value() && error.empty();
    }
};

/// Parses the exact dynamic symbol and string extents recorded in an already-decoded NSO header,
/// cross-checks them against the ELF64 dynamic table, and proves every nonzero relocation symbol
/// index is within the resulting table. `dynamic` must be a successful ParseNsoDynamic result for
/// the same `image`. No symbol is resolved and no relocation is applied.
NsoDynamicSymbolParseResult ParseNsoDynamicSymbols(
    const DecodedNso& image, const NsoDynamicInfo& dynamic,
    std::uint64_t max_symbols = DefaultNsoDynamicSymbolLimit,
    std::uint64_t max_name_bytes = DefaultNsoDynamicSymbolNameBytesLimit);

/// Parses a live module's exact dynamic-symbol count from its validated SysV DT_HASH metadata.
/// The hash, symbol, and string extents must all be readable through `module`; no guest code is
/// executed. This is the hosted-runtime counterpart to the DecodedNso overload.
NsoDynamicSymbolParseResult ParseNsoDynamicSymbols(
    const NsoModuleView& module, const NsoDynamicInfo& dynamic,
    std::uint64_t max_symbols = DefaultNsoDynamicSymbolLimit,
    std::uint64_t max_name_bytes = DefaultNsoDynamicSymbolNameBytesLimit,
    std::uint64_t max_string_table_bytes = DefaultNsoDynamicStringTableLimit);

} // namespace suyu::recomp
