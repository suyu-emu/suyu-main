// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_symbols.h"

#include <algorithm>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>

namespace suyu::recomp {
namespace {

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

bool ValidateRelocationSymbolIndices(const std::optional<NsoRelaTable>& table,
                                     std::uint64_t symbol_count, std::string& error) {
    if (!table) {
        return true;
    }
    for (std::size_t index = 0; index < table->records.size(); ++index) {
        if (table->records[index].symbol_index != 0 &&
            table->records[index].symbol_index >= symbol_count) {
            error = std::string{NsoRelaTableKindName(table->kind)} + " RELA record " +
                    std::to_string(index) + " references dynamic symbol index " +
                    std::to_string(table->records[index].symbol_index) + " outside the " +
                    std::to_string(symbol_count) + "-entry table";
            return false;
        }
    }
    return true;
}

bool ReadModule(const NsoModuleView& module, std::uint64_t address,
                std::span<std::uint8_t> destination) {
    if (!RangeFits(address, destination.size(), module.image_size)) {
        return false;
    }
    if (destination.empty()) {
        return true;
    }
    return module.read != nullptr && module.read(module.user, address, destination);
}

NsoDynamicSymbolParseResult ParseSymbolBytes(
    std::span<const std::uint8_t> symbol_bytes, std::span<const std::uint8_t> string_bytes,
    std::uint64_t symbol_table_address, std::uint64_t string_table_address,
    const NsoDynamicInfo& dynamic, std::uint64_t max_symbols, std::uint64_t max_name_bytes) {
    NsoDynamicSymbolParseResult result;
    if ((symbol_bytes.size() % NsoElf64SymbolEntrySize) != 0) {
        result.error = "dynamic symbol extent is not a whole number of 24-byte ELF64 symbols";
        return result;
    }
    const std::uint64_t symbol_count = symbol_bytes.size() / NsoElf64SymbolEntrySize;
    if (symbol_count > max_symbols || symbol_count > std::numeric_limits<std::uint32_t>::max() ||
        symbol_count > std::numeric_limits<std::size_t>::max()) {
        result.error = "dynamic symbol table exceeds the configured symbol-entry limit";
        return result;
    }

    NsoDynamicSymbolInfo info{
        .symbol_table_address = symbol_table_address,
        .symbol_table_byte_size = symbol_bytes.size(),
        .string_table_address = string_table_address,
        .string_table_byte_size = string_bytes.size(),
    };
    try {
        info.symbols.reserve(static_cast<std::size_t>(symbol_count));
        std::uint64_t stored_name_bytes = 0;
        for (std::uint64_t index = 0; index < symbol_count; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index * NsoElf64SymbolEntrySize);
            NsoDynamicSymbol symbol{
                .index = static_cast<std::uint32_t>(index),
                .address = symbol_table_address + offset,
                .name_offset = ReadU32(symbol_bytes, offset),
                .info = symbol_bytes[offset + 4],
                .other = symbol_bytes[offset + 5],
                .section_index = ReadU16(symbol_bytes, offset + 6),
                .value = ReadU64(symbol_bytes, offset + 8),
                .size = ReadU64(symbol_bytes, offset + 16),
            };
            if (symbol.name_offset >= string_bytes.size()) {
                result.error = "dynamic symbol " + std::to_string(index) +
                               " has a name offset outside the dynamic string-table extent";
                return result;
            }
            if (stored_name_bytes > max_name_bytes) {
                result.error = "decoded dynamic symbol names exceed the configured byte limit";
                return result;
            }
            const auto name_begin = string_bytes.begin() + symbol.name_offset;
            const std::uint64_t bytes_available = string_bytes.end() - name_begin;
            const std::uint64_t name_budget = max_name_bytes - stored_name_bytes;
            const std::uint64_t scan_budget =
                name_budget == std::numeric_limits<std::uint64_t>::max() ? name_budget
                                                                         : name_budget + 1;
            const std::uint64_t bytes_to_scan = std::min(bytes_available, scan_budget);
            const auto scan_end = name_begin + static_cast<std::ptrdiff_t>(bytes_to_scan);
            const auto name_end = std::find(name_begin, scan_end, std::uint8_t{0});
            if (name_end == scan_end && scan_end != string_bytes.end()) {
                result.error = "decoded dynamic symbol names exceed the configured byte limit";
                return result;
            }
            if (name_end == string_bytes.end()) {
                result.error = "dynamic symbol " + std::to_string(index) +
                               " has no null terminator inside the dynamic string-table extent";
                return result;
            }
            const std::uint64_t name_size = name_end - name_begin;
            stored_name_bytes += name_size;
            symbol.name.assign(reinterpret_cast<const char*>(&*name_begin),
                               static_cast<std::size_t>(name_size));
            info.symbols.push_back(std::move(symbol));
        }
    } catch (const std::bad_alloc&) {
        result.error = "could not allocate dynamic symbols or their names";
        return result;
    } catch (const std::length_error&) {
        result.error = "dynamic symbol data is unsupported on this host";
        return result;
    }

    if (!info.symbols.empty()) {
        const NsoDynamicSymbol& null_symbol = info.symbols.front();
        if (null_symbol.name_offset != 0 || null_symbol.info != 0 || null_symbol.other != 0 ||
            null_symbol.section_index != 0 || null_symbol.value != 0 || null_symbol.size != 0 ||
            !null_symbol.name.empty()) {
            result.error = "dynamic symbol index zero is not the required null symbol";
            return result;
        }
    }

    if (!ValidateRelocationSymbolIndices(dynamic.rela, symbol_count, result.error) ||
        !ValidateRelocationSymbolIndices(dynamic.plt_rela, symbol_count, result.error)) {
        return result;
    }
    result.info = std::move(info);
    return result;
}

} // namespace

NsoDynamicSymbolParseResult ParseNsoDynamicSymbols(const DecodedNso& image,
                                                   const NsoDynamicInfo& dynamic,
                                                   std::uint64_t max_symbols,
                                                   std::uint64_t max_name_bytes) {
    NsoDynamicSymbolParseResult result;
    const std::size_t rodata_index = static_cast<std::size_t>(NsoSegmentId::RoData);
    const NsoSegmentInfo& rodata_info = image.info.segments[rodata_index];
    const std::vector<std::uint8_t>& rodata = image.segments[rodata_index];
    if (rodata.size() != rodata_info.decoded_size) {
        result.error = "rodata decoded buffer size does not match its NSO metadata";
        return result;
    }
    constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32;
    if (!RangeFits(rodata_info.memory_offset, rodata_info.decoded_size, AddressSpaceSize)) {
        result.error = "decoded rodata range overflows the 32-bit module image";
        return result;
    }
    if (!RangeFits(image.info.dynsym.offset, image.info.dynsym.size, rodata.size())) {
        result.error = "NSO dynsym extent lies outside the decoded rodata segment";
        return result;
    }
    if (!RangeFits(image.info.dynstr.offset, image.info.dynstr.size, rodata.size())) {
        result.error = "NSO dynstr extent lies outside the decoded rodata segment";
        return result;
    }
    if ((image.info.dynsym.size % NsoElf64SymbolEntrySize) != 0) {
        result.error = "NSO dynsym extent is not a whole number of 24-byte ELF64 symbols";
        return result;
    }
    const std::uint64_t symbol_count = image.info.dynsym.size / NsoElf64SymbolEntrySize;

    const std::uint64_t expected_symbol_address =
        static_cast<std::uint64_t>(rodata_info.memory_offset) + image.info.dynsym.offset;
    const std::uint64_t expected_string_address =
        static_cast<std::uint64_t>(rodata_info.memory_offset) + image.info.dynstr.offset;
    if (dynamic.symbol_entry_size != NsoElf64SymbolEntrySize) {
        result.error = "dynamic symbol entry size is not the required 24-byte ELF64 size";
        return result;
    }
    if (symbol_count != 0) {
        if (!dynamic.symbol_table_address) {
            result.error = "NSO dynsym is nonempty but DT_SYMTAB is absent";
            return result;
        }
        if (*dynamic.symbol_table_address != expected_symbol_address) {
            result.error = "DT_SYMTAB does not match the NSO dynsym extent";
            return result;
        }
        if (!dynamic.string_table_address || !dynamic.string_table_size) {
            result.error = "NSO dynsym is nonempty but DT_STRTAB/DT_STRSZ is absent";
            return result;
        }
    } else if (dynamic.symbol_table_address) {
        result.error = "DT_SYMTAB is present but the NSO dynsym extent is empty";
        return result;
    }
    if (dynamic.string_table_address && *dynamic.string_table_address != expected_string_address) {
        result.error = "DT_STRTAB does not match the NSO dynstr extent";
        return result;
    }
    if (dynamic.string_table_size && *dynamic.string_table_size != image.info.dynstr.size) {
        result.error = "DT_STRSZ does not match the NSO dynstr extent";
        return result;
    }
    if (dynamic.sysv_hash && dynamic.sysv_hash->chain_count != symbol_count) {
        result.error = "DT_HASH chain count does not match the NSO dynsym extent";
        return result;
    }

    const std::span<const std::uint8_t> symbol_bytes =
        std::span<const std::uint8_t>{rodata}.subspan(image.info.dynsym.offset,
                                                      image.info.dynsym.size);
    const std::span<const std::uint8_t> string_bytes =
        std::span<const std::uint8_t>{rodata}.subspan(image.info.dynstr.offset,
                                                      image.info.dynstr.size);
    return ParseSymbolBytes(symbol_bytes, string_bytes, expected_symbol_address,
                            expected_string_address, dynamic, max_symbols, max_name_bytes);
}

NsoDynamicSymbolParseResult ParseNsoDynamicSymbols(const NsoModuleView& module,
                                                   const NsoDynamicInfo& dynamic,
                                                   std::uint64_t max_symbols,
                                                   std::uint64_t max_name_bytes,
                                                   std::uint64_t max_string_table_bytes) {
    NsoDynamicSymbolParseResult result;
    constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32;
    if (module.read == nullptr) {
        result.error = "module view has no read callback";
        return result;
    }
    if (module.image_size == 0 || module.image_size > AddressSpaceSize) {
        result.error = "module image size must be in the range [1, 2^32]";
        return result;
    }
    if (dynamic.symbol_entry_size != NsoElf64SymbolEntrySize) {
        result.error = "dynamic symbol entry size is not the required 24-byte ELF64 size";
        return result;
    }
    if (!dynamic.symbol_table_address) {
        if (dynamic.sysv_hash) {
            result.error = "DT_HASH is present but DT_SYMTAB is absent";
            return result;
        }
        if (!ValidateRelocationSymbolIndices(dynamic.rela, 0, result.error) ||
            !ValidateRelocationSymbolIndices(dynamic.plt_rela, 0, result.error)) {
            return result;
        }
        result.info = NsoDynamicSymbolInfo{};
        return result;
    }
    if (!dynamic.sysv_hash) {
        result.error = "DT_SYMTAB is present but DT_HASH is absent; symbol count cannot be proven";
        return result;
    }
    if (!dynamic.string_table_address || !dynamic.string_table_size) {
        result.error = "DT_SYMTAB is present but DT_STRTAB/DT_STRSZ is absent";
        return result;
    }

    const NsoSysvHashInfo& hash = *dynamic.sysv_hash;
    const std::uint64_t expected_hash_size =
        (std::uint64_t{2} + hash.bucket_count + static_cast<std::uint64_t>(hash.chain_count)) *
        sizeof(std::uint32_t);
    if (hash.bucket_count == 0 || hash.chain_count == 0 || hash.byte_size != expected_hash_size ||
        !RangeFits(hash.address, hash.byte_size, module.image_size)) {
        result.error = "DT_HASH metadata is inconsistent with the module mapping";
        return result;
    }
    if (hash.bucket_count > max_symbols || hash.chain_count > max_symbols) {
        result.error = "DT_HASH exceeds the configured symbol-entry limit";
        return result;
    }
    const std::uint64_t symbol_byte_size =
        static_cast<std::uint64_t>(hash.chain_count) * NsoElf64SymbolEntrySize;
    if (!RangeFits(*dynamic.symbol_table_address, symbol_byte_size, module.image_size)) {
        result.error = "DT_SYMTAB extent derived from DT_HASH lies outside the module image";
        return result;
    }
    if (*dynamic.string_table_size > max_string_table_bytes ||
        *dynamic.string_table_size > std::numeric_limits<std::size_t>::max()) {
        result.error = "dynamic string table exceeds the configured byte limit";
        return result;
    }

    try {
        std::vector<std::uint8_t> hash_bytes(static_cast<std::size_t>(hash.byte_size));
        std::vector<std::uint8_t> symbol_bytes(static_cast<std::size_t>(symbol_byte_size));
        std::vector<std::uint8_t> string_bytes(
            static_cast<std::size_t>(*dynamic.string_table_size));
        if (!ReadModule(module, hash.address, hash_bytes)) {
            result.error = "DT_HASH extent contains unreadable module bytes";
            return result;
        }
        if (ReadU32(hash_bytes, 0) != hash.bucket_count ||
            ReadU32(hash_bytes, 4) != hash.chain_count) {
            result.error = "DT_HASH header changed after dynamic metadata was parsed";
            return result;
        }
        const std::size_t hash_index_count =
            static_cast<std::size_t>(hash.bucket_count) + hash.chain_count;
        for (std::size_t index = 0; index < hash_index_count; ++index) {
            if (ReadU32(hash_bytes, (index + 2) * sizeof(std::uint32_t)) >= hash.chain_count) {
                result.error = "DT_HASH bucket or chain index lies outside the dynamic symbols";
                return result;
            }
        }
        if (!ReadModule(module, *dynamic.symbol_table_address, symbol_bytes)) {
            result.error = "DT_SYMTAB extent contains unreadable module bytes";
            return result;
        }
        if (!ReadModule(module, *dynamic.string_table_address, string_bytes)) {
            result.error = "DT_STRTAB/DT_STRSZ extent contains unreadable module bytes";
            return result;
        }
        return ParseSymbolBytes(symbol_bytes, string_bytes, *dynamic.symbol_table_address,
                                *dynamic.string_table_address, dynamic, max_symbols,
                                max_name_bytes);
    } catch (const std::bad_alloc&) {
        result.error = "could not allocate live dynamic symbol metadata";
        return result;
    } catch (const std::length_error&) {
        result.error = "live dynamic symbol metadata is unsupported on this host";
        return result;
    }
}

} // namespace suyu::recomp
