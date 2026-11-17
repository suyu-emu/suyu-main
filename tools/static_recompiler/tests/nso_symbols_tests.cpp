// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_symbols.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Byte = std::uint8_t;

constexpr std::uint32_t RoDataAddress = 0x1000;
constexpr std::uint32_t DynamicAddress = 0x1020;
constexpr std::uint32_t RelaAddress = 0x1100;
constexpr std::uint32_t HashTableAddress = 0x1180;
constexpr std::uint32_t SymbolTableAddress = 0x1200;
constexpr std::uint32_t StringTableAddress = 0x1280;
constexpr std::string_view StringTable{"\0exported\0optional\0absolute\0", 28};

void PutU16(std::vector<Byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<Byte>(value);
    bytes[offset + 1] = static_cast<Byte>(value >> 8);
}

void PutU32(std::vector<Byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
    }
}

void PutU64(std::vector<Byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
    }
}

void PutI64(std::vector<Byte>& bytes, std::size_t offset, std::int64_t value) {
    PutU64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

std::size_t RoOffset(std::uint32_t address) {
    return address - RoDataAddress;
}

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool ReadImage(const void* user, std::uint64_t address, std::span<Byte> destination) {
    const auto& image = *static_cast<const suyu::recomp::DecodedNso*>(user);
    for (std::size_t i = 0; i < suyu::recomp::NsoSegmentCount; ++i) {
        const auto& segment = image.info.segments[i];
        if (address < segment.memory_offset) {
            continue;
        }
        const std::uint64_t offset = address - segment.memory_offset;
        if (!RangeFits(offset, destination.size(), image.segments[i].size())) {
            continue;
        }
        std::copy_n(image.segments[i].begin() + static_cast<std::size_t>(offset),
                    destination.size(), destination.begin());
        return true;
    }
    return destination.empty();
}

suyu::recomp::NsoModuleView MakeView(const suyu::recomp::DecodedNso& image) {
    return {
        .image_size = 0x2200,
        .text_address = 0,
        .user = &image,
        .read = ReadImage,
    };
}

void PutDynamic(std::vector<Byte>& rodata, std::size_t index, std::int64_t tag,
                std::uint64_t value) {
    const std::size_t offset = RoOffset(DynamicAddress) + index * 16;
    PutI64(rodata, offset, tag);
    PutU64(rodata, offset + 8, value);
}

void PutSymbol(std::vector<Byte>& rodata, std::size_t index, std::uint32_t name_offset,
               std::uint8_t binding, std::uint8_t type, std::uint8_t other,
               std::uint16_t section_index, std::uint64_t value, std::uint64_t size) {
    const std::size_t offset = RoOffset(SymbolTableAddress) + index * 24;
    PutU32(rodata, offset, name_offset);
    rodata[offset + 4] = static_cast<Byte>((binding << 4) | type);
    rodata[offset + 5] = other;
    PutU16(rodata, offset + 6, section_index);
    PutU64(rodata, offset + 8, value);
    PutU64(rodata, offset + 16, size);
}

suyu::recomp::DecodedNso MakeImage() {
    suyu::recomp::DecodedNso image;
    constexpr std::uint32_t TextAddress = 0;
    constexpr std::uint32_t TextSize = 0x100;
    constexpr std::uint32_t RoDataSize = 0x400;
    constexpr std::uint32_t DataAddress = 0x2000;
    constexpr std::uint32_t DataSize = 0x100;
    constexpr std::uint32_t SymbolCount = 4;

    image.info.segments[0].memory_offset = TextAddress;
    image.info.segments[0].decoded_size = TextSize;
    image.info.segments[1].memory_offset = RoDataAddress;
    image.info.segments[1].decoded_size = RoDataSize;
    image.info.segments[2].memory_offset = DataAddress;
    image.info.segments[2].decoded_size = DataSize;
    image.info.bss_size = 0x100;
    image.info.dynsym = {SymbolTableAddress - RoDataAddress, SymbolCount * 24};
    image.info.dynstr = {StringTableAddress - RoDataAddress,
                         static_cast<std::uint32_t>(StringTable.size())};
    image.segments[0].resize(TextSize);
    image.segments[1].resize(RoDataSize);
    image.segments[2].resize(DataSize);

    PutU32(image.segments[0], 4, RoDataAddress);
    auto& rodata = image.segments[1];
    std::copy_n(reinterpret_cast<const Byte*>("MOD0"), 4, rodata.begin());
    PutU32(rodata, 4, DynamicAddress - RoDataAddress);
    PutDynamic(rodata, 0, 7, RelaAddress);         // DT_RELA
    PutDynamic(rodata, 1, 8, 24);                  // DT_RELASZ
    PutDynamic(rodata, 2, 9, 24);                  // DT_RELAENT
    PutDynamic(rodata, 3, 4, HashTableAddress);    // DT_HASH
    PutDynamic(rodata, 4, 6, SymbolTableAddress);  // DT_SYMTAB
    PutDynamic(rodata, 5, 11, 24);                 // DT_SYMENT
    PutDynamic(rodata, 6, 5, StringTableAddress);  // DT_STRTAB
    PutDynamic(rodata, 7, 10, StringTable.size()); // DT_STRSZ
    PutDynamic(rodata, 8, 0, 0);                   // DT_NULL

    const std::size_t rela_offset = RoOffset(RelaAddress);
    PutU64(rodata, rela_offset, DataAddress);
    PutU64(rodata, rela_offset + 8, (std::uint64_t{2} << 32) | 0x401);
    PutI64(rodata, rela_offset + 16, 0);

    const std::size_t hash_offset = RoOffset(HashTableAddress);
    PutU32(rodata, hash_offset, 1);
    PutU32(rodata, hash_offset + 4, SymbolCount);
    PutU32(rodata, hash_offset + 8, 1);

    PutSymbol(rodata, 0, 0, 0, 0, 0, 0, 0, 0);
    PutSymbol(rodata, 1, 1, 1, 2, 0, 1, 0x40, 4);
    PutSymbol(rodata, 2, 10, 2, 2, 0, 0, 0, 0);
    PutSymbol(rodata, 3, 19, 1, 1, 0, 0xFFF1, 0, 0);
    std::copy(StringTable.begin(), StringTable.end(),
              rodata.begin() + RoOffset(StringTableAddress));
    return image;
}

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void CheckError(const suyu::recomp::NsoDynamicSymbolParseResult& result, std::string_view text,
                std::string_view description) {
    Check(!result && result.error.find(text) != std::string::npos, description);
}

void CheckError(const suyu::recomp::NsoDynamicParseResult& result, std::string_view text,
                std::string_view description) {
    Check(!result && result.error.find(text) != std::string::npos, description);
}

void RunTests() {
    const auto image = MakeImage();
    const auto dynamic = suyu::recomp::ParseNsoDynamic(image);
    Check(static_cast<bool>(dynamic), "dynamic metadata fixture parses");
    if (!dynamic) {
        return;
    }

    const auto parsed = suyu::recomp::ParseNsoDynamicSymbols(image, *dynamic.info);
    Check(static_cast<bool>(parsed), "bounded ELF64 dynamic symbols parse");
    if (!parsed) {
        return;
    }
    const auto& info = *parsed.info;
    Check(info.symbol_table_address == SymbolTableAddress &&
              info.symbol_table_byte_size == 4 * 24 &&
              info.string_table_address == StringTableAddress &&
              info.string_table_byte_size == StringTable.size(),
          "NSO dynamic symbol and string extents are exposed");
    Check(info.symbols.size() == 4, "exact NSO dynsym count is retained");
    Check(dynamic.info->sysv_hash && dynamic.info->sysv_hash->address == HashTableAddress &&
              dynamic.info->sysv_hash->byte_size == 28 &&
              dynamic.info->sysv_hash->bucket_count == 1 &&
              dynamic.info->sysv_hash->chain_count == 4,
          "bounded SysV DT_HASH metadata exposes the exact symbol count");
    Check(info.symbols[1].name == "exported" && info.symbols[1].Binding() == 1 &&
              info.symbols[1].Type() == 2 && info.symbols[1].value == 0x40 &&
              info.symbols[1].size == 4 && info.symbols[1].IsExternallyVisibleDefinition(),
          "defined global function metadata is decoded");
    Check(info.symbols[2].name == "optional" && info.symbols[2].IsUndefined() &&
              info.symbols[2].IsWeak() && !info.symbols[2].IsExternallyVisibleDefinition(),
          "undefined weak import metadata is preserved");
    Check(info.symbols[3].name == "absolute" && info.symbols[3].IsAbsolute() &&
              info.symbols[3].IsExternallyVisibleDefinition(),
          "absolute exported symbol is distinguished from a module-relative definition");

    const auto live_dynamic = suyu::recomp::ParseNsoDynamic(MakeView(image));
    Check(static_cast<bool>(live_dynamic), "live module dynamic metadata parses");
    if (live_dynamic) {
        const auto live = suyu::recomp::ParseNsoDynamicSymbols(MakeView(image), *live_dynamic.info);
        Check(live && live.info->symbols.size() == 4 &&
                  live.info->symbol_table_byte_size == 4 * 24 &&
                  live.info->symbols[1].name == "exported" &&
                  live.info->symbols[2].name == "optional",
              "DT_HASH drives exact symbol parsing from a live module view");

        auto missing_hash = *live_dynamic.info;
        missing_hash.sysv_hash.reset();
        CheckError(suyu::recomp::ParseNsoDynamicSymbols(MakeView(image), missing_hash),
                   "DT_HASH is absent", "live symbol parsing requires a proven symbol count");

        auto corrupted_hash = image;
        PutU32(corrupted_hash.segments[1], RoOffset(HashTableAddress) + 8, 4);
        CheckError(
            suyu::recomp::ParseNsoDynamicSymbols(MakeView(corrupted_hash), *live_dynamic.info),
            "index lies outside", "live symbol parsing validates SysV hash indices");

        CheckError(suyu::recomp::ParseNsoDynamicSymbols(MakeView(image), *live_dynamic.info, 4, 64,
                                                        StringTable.size() - 1),
                   "string table exceeds", "live string-table allocation is bounded");
    }

    auto malformed_hash = image;
    PutDynamic(malformed_hash.segments[1], 3, 4, HashTableAddress + 1);
    CheckError(suyu::recomp::ParseNsoDynamic(malformed_hash), "not 4-byte aligned",
               "misaligned DT_HASH metadata is rejected");

    malformed_hash = image;
    PutU32(malformed_hash.segments[1], RoOffset(HashTableAddress), 0);
    CheckError(suyu::recomp::ParseNsoDynamic(malformed_hash), "zero bucket or chain",
               "empty SysV hash dimensions are rejected");

    malformed_hash = image;
    PutU32(malformed_hash.segments[1], RoOffset(HashTableAddress),
           std::numeric_limits<std::uint32_t>::max());
    CheckError(suyu::recomp::ParseNsoDynamic(malformed_hash), "outside the module image",
               "oversized DT_HASH extents are rejected without overflow");

    malformed_hash = image;
    PutDynamic(malformed_hash.segments[1], 8, 4, HashTableAddress);
    PutDynamic(malformed_hash.segments[1], 9, 0, 0);
    CheckError(suyu::recomp::ParseNsoDynamic(malformed_hash), "duplicate DT_HASH",
               "duplicate DT_HASH entries are rejected");

    auto hidden_image = image;
    hidden_image.segments[1][RoOffset(SymbolTableAddress) + 24 + 5] = 2;
    const auto hidden = suyu::recomp::ParseNsoDynamicSymbols(hidden_image, *dynamic.info);
    Check(hidden && !hidden.info->symbols[1].IsExternallyVisibleDefinition(),
          "hidden definitions are not classified as cross-module exports");

    auto malformed = image;
    malformed.segments[1].pop_back();
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info),
               "buffer size does not match", "inconsistent decoded rodata is rejected");

    malformed = image;
    malformed.info.dynsym.offset = 0x3E0;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info),
               "outside the decoded rodata", "out-of-range NSO dynsym extent is rejected");

    malformed = image;
    malformed.info.dynsym.size -= 1;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info), "whole number",
               "partial ELF64 dynamic symbol is rejected");

    malformed = image;
    malformed.info.segments[1].memory_offset = 0xFFFFF000U;
    malformed.info.segments[1].decoded_size = 0x2000;
    malformed.segments[1].resize(0x2000);
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info),
               "overflows the 32-bit module image",
               "rodata outside the module address space is rejected");

    auto malformed_dynamic = *dynamic.info;
    malformed_dynamic.symbol_table_address = SymbolTableAddress + 24;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, malformed_dynamic),
               "DT_SYMTAB does not match", "mismatched DT_SYMTAB is rejected");

    malformed_dynamic = *dynamic.info;
    malformed_dynamic.string_table_address = StringTableAddress + 1;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, malformed_dynamic),
               "DT_STRTAB does not match", "mismatched DT_STRTAB is rejected");

    malformed_dynamic = *dynamic.info;
    malformed_dynamic.string_table_size = StringTable.size() - 1;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, malformed_dynamic),
               "DT_STRSZ does not match", "mismatched DT_STRSZ is rejected");

    malformed_dynamic = *dynamic.info;
    malformed_dynamic.symbol_table_address.reset();
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, malformed_dynamic),
               "DT_SYMTAB is absent", "missing DT_SYMTAB is rejected for nonempty dynsym");

    malformed_dynamic = *dynamic.info;
    --malformed_dynamic.sysv_hash->chain_count;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, malformed_dynamic),
               "chain count does not match", "DT_HASH is checked against the NSO dynsym extent");

    malformed = image;
    PutU32(malformed.segments[1], RoOffset(SymbolTableAddress) + 2 * 24,
           static_cast<std::uint32_t>(StringTable.size()));
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info),
               "name offset outside", "out-of-range dynamic symbol name is rejected");

    malformed = image;
    malformed.segments[1][RoOffset(StringTableAddress) + StringTable.size() - 1] = 'X';
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info), "no null terminator",
               "unterminated dynamic symbol name is rejected");

    malformed = image;
    malformed.segments[1][RoOffset(SymbolTableAddress) + 4] = 0x10;
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *dynamic.info),
               "required null symbol", "non-null dynamic symbol index zero is rejected");

    malformed = image;
    PutU64(malformed.segments[1], RoOffset(RelaAddress) + 8, (std::uint64_t{4} << 32) | 0x401);
    const auto out_of_range_dynamic = suyu::recomp::ParseNsoDynamic(malformed);
    Check(static_cast<bool>(out_of_range_dynamic),
          "metadata parser alone does not guess the true dynsym count");
    if (out_of_range_dynamic) {
        CheckError(suyu::recomp::ParseNsoDynamicSymbols(malformed, *out_of_range_dynamic.info),
                   "outside the 4-entry table",
                   "relocation symbol index is checked against the exact NSO dynsym extent");
    }

    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, *dynamic.info, 3), "symbol-entry limit",
               "dynamic symbol count limit is enforced");
    CheckError(suyu::recomp::ParseNsoDynamicSymbols(image, *dynamic.info, 4, 7), "names exceed",
               "aggregate decoded symbol-name limit is enforced");
}

} // namespace

int main() {
    RunTests();
    if (failures != 0) {
        std::cerr << failures << " NSO dynamic symbol test(s) failed\n";
        return 1;
    }
    std::cout << "NSO dynamic symbol tests passed\n";
    return 0;
}
