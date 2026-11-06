// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_dynamic.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>

namespace suyu::recomp {
namespace {

constexpr std::uint32_t Mod0Magic = 0x30444F4D; // "MOD0", little-endian
constexpr std::uint64_t MinimumMod0HeaderSize = 0x1C;

constexpr std::int64_t DtNull = 0;
constexpr std::int64_t DtPltRelSize = 2;
constexpr std::int64_t DtHash = 4;
constexpr std::int64_t DtStringTable = 5;
constexpr std::int64_t DtSymbolTable = 6;
constexpr std::int64_t DtRela = 7;
constexpr std::int64_t DtRelaSize = 8;
constexpr std::int64_t DtRelaEntry = 9;
constexpr std::int64_t DtStringTableSize = 10;
constexpr std::int64_t DtSymbolEntry = 11;
constexpr std::int64_t DtRel = 17;
constexpr std::int64_t DtRelSize = 18;
constexpr std::int64_t DtRelEntry = 19;
constexpr std::int64_t DtPltRel = 20;
constexpr std::int64_t DtJumpRel = 23;
constexpr std::int64_t DtRelrSize = 35;
constexpr std::int64_t DtRelr = 36;
constexpr std::int64_t DtRelrEntry = 37;

struct DynamicTags {
    std::optional<std::uint64_t> rela_address;
    std::optional<std::uint64_t> rela_size;
    std::optional<std::uint64_t> rela_entry_size;
    std::optional<std::uint64_t> jump_rela_address;
    std::optional<std::uint64_t> plt_rela_size;
    std::optional<std::uint64_t> plt_relocation_tag;
    std::optional<std::uint64_t> string_table_address;
    std::optional<std::uint64_t> string_table_size;
    std::optional<std::uint64_t> symbol_table_address;
    std::optional<std::uint64_t> symbol_entry_size;
    std::optional<std::uint64_t> hash_table_address;
    std::optional<std::uint64_t> rel_address;
    std::optional<std::uint64_t> rel_size;
    std::optional<std::uint64_t> rel_entry_size;
    std::optional<std::uint64_t> relr_address;
    std::optional<std::uint64_t> relr_size;
    std::optional<std::uint64_t> relr_entry_size;
};

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool RangesOverlap(std::uint64_t first_offset, std::uint64_t first_size,
                   std::uint64_t second_offset, std::uint64_t second_size) {
    if (first_size == 0 || second_size == 0) {
        return false;
    }
    return first_offset < second_offset + second_size && second_offset < first_offset + first_size;
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset = 0) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset = 0) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

std::int64_t ReadI64(std::span<const std::uint8_t> bytes, std::size_t offset = 0) {
    return std::bit_cast<std::int64_t>(ReadU64(bytes, offset));
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

bool ReadDecodedNso(const void* user, std::uint64_t address, std::span<std::uint8_t> destination) {
    const auto& image = *static_cast<const DecodedNso*>(user);
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        const std::uint64_t segment_address = image.info.segments[i].memory_offset;
        const auto& bytes = image.segments[i];
        if (address < segment_address) {
            continue;
        }
        const std::uint64_t local_offset = address - segment_address;
        if (!RangeFits(local_offset, destination.size(), bytes.size())) {
            continue;
        }
        std::copy_n(bytes.begin() + static_cast<std::size_t>(local_offset), destination.size(),
                    destination.begin());
        return true;
    }
    return false;
}

bool StoreUnique(std::optional<std::uint64_t>& destination, std::uint64_t value,
                 const char* tag_name, std::string& error) {
    if (destination.has_value()) {
        error = std::string{"duplicate "} + tag_name + " entry in the dynamic table";
        return false;
    }
    destination = value;
    return true;
}

bool CaptureDynamicTag(std::int64_t tag, std::uint64_t value, DynamicTags& tags,
                       std::string& error) {
    switch (tag) {
    case DtPltRelSize:
        return StoreUnique(tags.plt_rela_size, value, "DT_PLTRELSZ", error);
    case DtHash:
        return StoreUnique(tags.hash_table_address, value, "DT_HASH", error);
    case DtStringTable:
        return StoreUnique(tags.string_table_address, value, "DT_STRTAB", error);
    case DtSymbolTable:
        return StoreUnique(tags.symbol_table_address, value, "DT_SYMTAB", error);
    case DtRela:
        return StoreUnique(tags.rela_address, value, "DT_RELA", error);
    case DtRelaSize:
        return StoreUnique(tags.rela_size, value, "DT_RELASZ", error);
    case DtRelaEntry:
        return StoreUnique(tags.rela_entry_size, value, "DT_RELAENT", error);
    case DtStringTableSize:
        return StoreUnique(tags.string_table_size, value, "DT_STRSZ", error);
    case DtSymbolEntry:
        return StoreUnique(tags.symbol_entry_size, value, "DT_SYMENT", error);
    case DtRel:
        return StoreUnique(tags.rel_address, value, "DT_REL", error);
    case DtRelSize:
        return StoreUnique(tags.rel_size, value, "DT_RELSZ", error);
    case DtRelEntry:
        return StoreUnique(tags.rel_entry_size, value, "DT_RELENT", error);
    case DtPltRel:
        return StoreUnique(tags.plt_relocation_tag, value, "DT_PLTREL", error);
    case DtJumpRel:
        return StoreUnique(tags.jump_rela_address, value, "DT_JMPREL", error);
    case DtRelrSize:
        return StoreUnique(tags.relr_size, value, "DT_RELRSZ", error);
    case DtRelr:
        return StoreUnique(tags.relr_address, value, "DT_RELR", error);
    case DtRelrEntry:
        return StoreUnique(tags.relr_entry_size, value, "DT_RELRENT", error);
    default:
        return true;
    }
}

bool ValidateAddressSizePair(const NsoModuleView& module,
                             const std::optional<std::uint64_t>& address,
                             const std::optional<std::uint64_t>& size, const char* address_tag,
                             const char* size_tag, std::string& error) {
    if (address.has_value() && !size.has_value()) {
        error = std::string{address_tag} + " is present without " + size_tag;
        return false;
    }
    if (!address.has_value() && size.value_or(0) != 0) {
        error = std::string{size_tag} + " is nonzero without " + address_tag;
        return false;
    }
    if (address.has_value() && !RangeFits(*address, *size, module.image_size)) {
        error = std::string{address_tag} + '/' + size_tag + " extent lies outside the module image";
        return false;
    }
    return true;
}

bool ParseRelaTable(const NsoModuleView& module, const std::optional<std::uint64_t>& address,
                    const std::optional<std::uint64_t>& size, NsoRelaTableKind kind,
                    std::uint64_t entry_size, std::uint64_t dynamic_address,
                    std::uint64_t dynamic_size, std::uint64_t remaining_relocations,
                    std::optional<NsoRelaTable>& output, std::string& error) {
    const char* address_tag = kind == NsoRelaTableKind::Dynamic ? "DT_RELA" : "DT_JMPREL";
    const char* size_tag = kind == NsoRelaTableKind::Dynamic ? "DT_RELASZ" : "DT_PLTRELSZ";
    if (!ValidateAddressSizePair(module, address, size, address_tag, size_tag, error)) {
        return false;
    }
    if (!address.has_value()) {
        return true;
    }
    if ((*address & 7) != 0) {
        error = std::string{address_tag} + " is not 8-byte aligned";
        return false;
    }
    if ((*size % entry_size) != 0) {
        std::ostringstream message;
        message << size_tag << " is not a multiple of its " << entry_size
                << "-byte RELA entry size";
        error = message.str();
        return false;
    }
    const std::uint64_t count = *size / entry_size;
    if (count > remaining_relocations || count > std::numeric_limits<std::size_t>::max()) {
        error = "RELA tables exceed the configured relocation-entry limit";
        return false;
    }
    if (RangesOverlap(*address, *size, dynamic_address, dynamic_size)) {
        error = std::string{address_tag} + '/' + size_tag + " extent overlaps the dynamic table";
        return false;
    }

    NsoRelaTable table;
    table.kind = kind;
    table.address = *address;
    table.byte_size = *size;
    table.entry_size = entry_size;
    try {
        table.records.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            std::array<std::uint8_t, NsoElf64RelaEntrySize> bytes{};
            const std::uint64_t record_address = *address + index * entry_size;
            if (!ReadModule(module, record_address, bytes)) {
                error =
                    std::string{address_tag} + '/' + size_tag + " extent contains unreadable bytes";
                return false;
            }
            const std::uint64_t target = ReadU64(bytes);
            // AArch64 defines both 32-bit and 64-bit relocation types. Alignment and full write
            // extent are properties of a specific type, so the applier validates them once it
            // decides which types it supports. The format parser only requires an in-image start.
            if (!RangeFits(target, 1, module.image_size)) {
                std::ostringstream message;
                message << address_tag << " record " << index
                        << " destination lies outside the module image";
                error = message.str();
                return false;
            }
            const std::uint64_t info = ReadU64(bytes, 8);
            table.records.push_back({
                .offset = target,
                .info = info,
                .addend = ReadI64(bytes, 16),
                .symbol_index = static_cast<std::uint32_t>(info >> 32),
                .type = static_cast<std::uint32_t>(info),
            });
        }
    } catch (const std::bad_alloc&) {
        error = "could not allocate RELA records";
        return false;
    } catch (const std::length_error&) {
        error = "RELA record count is unsupported on this host";
        return false;
    }
    output = std::move(table);
    return true;
}

bool ValidateDecodedImage(const DecodedNso& image, std::string& error) {
    constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32;
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        const auto& metadata = image.info.segments[i];
        if (image.segments[i].size() != metadata.decoded_size) {
            error = std::string{NsoSegmentName(static_cast<NsoSegmentId>(i))} +
                    " decoded buffer size does not match its NSO metadata";
            return false;
        }
        if (!RangeFits(metadata.memory_offset, metadata.decoded_size, AddressSpaceSize)) {
            error = std::string{NsoSegmentName(static_cast<NsoSegmentId>(i))} +
                    " decoded memory range overflows the 32-bit module image";
            return false;
        }
    }
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        for (std::size_t j = i + 1; j < NsoSegmentCount; ++j) {
            if (RangesOverlap(
                    image.info.segments[i].memory_offset, image.info.segments[i].decoded_size,
                    image.info.segments[j].memory_offset, image.info.segments[j].decoded_size)) {
                error = "decoded NSO segment memory ranges overlap";
                return false;
            }
        }
    }
    const auto& data = image.info.segments[static_cast<std::size_t>(NsoSegmentId::Data)];
    const std::uint64_t data_end =
        static_cast<std::uint64_t>(data.memory_offset) + data.decoded_size;
    if (!RangeFits(data_end, image.info.bss_size, AddressSpaceSize)) {
        error = "decoded NSO data/BSS range overflows the 32-bit module image";
        return false;
    }
    const std::uint64_t data_and_bss_size =
        static_cast<std::uint64_t>(data.decoded_size) + image.info.bss_size;
    for (std::size_t i = 0; i < static_cast<std::size_t>(NsoSegmentId::Data); ++i) {
        if (RangesOverlap(data.memory_offset, data_and_bss_size,
                          image.info.segments[i].memory_offset,
                          image.info.segments[i].decoded_size)) {
            error = "decoded NSO data/BSS range overlaps another segment";
            return false;
        }
    }
    if (const std::string layout_error = ValidateNsoExecutableLayout(image.info);
        !layout_error.empty()) {
        error = "decoded NSO executable layout is invalid: " + layout_error;
        return false;
    }
    return true;
}

NsoModuleView MakeDecodedNsoModuleViewUnchecked(const DecodedNso& image) {
    std::uint64_t image_size = 0;
    for (const auto& segment : image.info.segments) {
        image_size = std::max(image_size, static_cast<std::uint64_t>(segment.memory_offset) +
                                              segment.decoded_size);
    }
    const auto& data = image.info.segments[static_cast<std::size_t>(NsoSegmentId::Data)];
    image_size = std::max(image_size, static_cast<std::uint64_t>(data.memory_offset) +
                                          data.decoded_size + image.info.bss_size);
    return {
        .image_size = image_size,
        .text_address =
            image.info.segments[static_cast<std::size_t>(NsoSegmentId::Text)].memory_offset,
        .user = &image,
        .read = ReadDecodedNso,
    };
}

bool DecodedRangeIsReadable(const DecodedNso& image, std::uint64_t address, std::uint64_t size) {
    if (size == 0) {
        return true;
    }
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        const std::uint64_t segment_address = image.info.segments[i].memory_offset;
        if (address >= segment_address &&
            RangeFits(address - segment_address, size, image.segments[i].size())) {
            return true;
        }
    }
    return false;
}

} // namespace

NsoDynamicParseResult ParseNsoDynamic(const NsoModuleView& module, std::size_t max_dynamic_entries,
                                      std::uint64_t max_relocations) {
    NsoDynamicParseResult result;
    if (module.read == nullptr) {
        result.error = "module view has no read callback";
        return result;
    }

    std::array<std::uint8_t, 8> module_header_location{};
    if (!ReadModule(module, module.text_address, module_header_location)) {
        result.error = "text segment does not contain the 8-byte module-header location";
        return result;
    }
    const std::uint32_t mod0_offset = ReadU32(module_header_location, 4);
    if ((mod0_offset & 3) != 0 ||
        mod0_offset > std::numeric_limits<std::uint64_t>::max() - module.text_address) {
        result.error = "MOD0 offset at text+4 is invalid";
        return result;
    }
    const std::uint64_t mod0_address = module.text_address + mod0_offset;

    std::array<std::uint8_t, MinimumMod0HeaderSize> mod0_bytes{};
    if (!ReadModule(module, mod0_address, mod0_bytes)) {
        result.error = "MOD0 header lies outside readable module bytes";
        return result;
    }
    if (ReadU32(mod0_bytes) != Mod0Magic) {
        result.error = "module-header offset does not point to MOD0 magic";
        return result;
    }

    NsoDynamicInfo info;
    info.mod0 = {
        .address = mod0_address,
        .dynamic_offset = ReadU32(mod0_bytes, 4),
        .bss_start_offset = ReadU32(mod0_bytes, 8),
        .bss_end_offset = ReadU32(mod0_bytes, 12),
        .exception_info_start_offset = ReadU32(mod0_bytes, 16),
        .exception_info_end_offset = ReadU32(mod0_bytes, 20),
        .module_object_offset = ReadU32(mod0_bytes, 24),
    };
    if (info.mod0.dynamic_offset < MinimumMod0HeaderSize ||
        info.mod0.dynamic_offset > std::numeric_limits<std::uint64_t>::max() - mod0_address) {
        result.error = "MOD0 dynamic offset overlaps its minimum header or overflows";
        return result;
    }
    info.dynamic_address = mod0_address + info.mod0.dynamic_offset;
    if ((info.dynamic_address & 7) != 0) {
        result.error = "MOD0 dynamic table is not 8-byte aligned";
        return result;
    }

    DynamicTags tags;
    std::uint64_t entry_address = info.dynamic_address;
    try {
        info.entries.reserve(std::min<std::size_t>(max_dynamic_entries, 64));
        while (true) {
            std::array<std::uint8_t, NsoElf64DynamicEntrySize> entry_bytes{};
            if (!ReadModule(module, entry_address, entry_bytes)) {
                result.error =
                    "dynamic table is not DT_NULL-terminated within readable module bytes";
                return result;
            }
            const std::int64_t tag = ReadI64(entry_bytes);
            if (tag == DtNull) {
                info.dynamic_byte_size =
                    entry_address - info.dynamic_address + NsoElf64DynamicEntrySize;
                break;
            }
            if (info.entries.size() >= max_dynamic_entries) {
                result.error = "dynamic table exceeds the configured entry limit";
                return result;
            }
            const std::uint64_t value = ReadU64(entry_bytes, 8);
            info.entries.push_back({
                .address = entry_address,
                .value_address = entry_address + sizeof(std::uint64_t),
                .tag = tag,
                .value = value,
            });
            if (!CaptureDynamicTag(tag, value, tags, result.error)) {
                return result;
            }
            if (tag == DtRelaSize) {
                info.rela_size_value_address = entry_address + sizeof(std::uint64_t);
            } else if (tag == DtPltRelSize) {
                info.plt_rela_size_value_address = entry_address + sizeof(std::uint64_t);
            }
            if (entry_address >
                std::numeric_limits<std::uint64_t>::max() - NsoElf64DynamicEntrySize) {
                result.error = "dynamic table address overflows";
                return result;
            }
            entry_address += NsoElf64DynamicEntrySize;
        }
    } catch (const std::bad_alloc&) {
        result.error = "could not allocate dynamic entries";
        return result;
    } catch (const std::length_error&) {
        result.error = "dynamic entry count is unsupported on this host";
        return result;
    }

    info.rela_entry_size = tags.rela_entry_size.value_or(NsoElf64RelaEntrySize);
    if (info.rela_entry_size != NsoElf64RelaEntrySize) {
        result.error = "DT_RELAENT is not the required 24-byte ELF64 RELA entry size";
        return result;
    }
    info.symbol_entry_size = tags.symbol_entry_size.value_or(NsoElf64SymbolEntrySize);
    if (info.symbol_entry_size != NsoElf64SymbolEntrySize) {
        result.error = "DT_SYMENT is not the required 24-byte ELF64 symbol entry size";
        return result;
    }
    info.plt_relocation_tag = tags.plt_relocation_tag.value_or(DtRela);
    if (info.plt_relocation_tag != static_cast<std::uint64_t>(DtRela) &&
        info.plt_relocation_tag != static_cast<std::uint64_t>(DtRel)) {
        result.error = "DT_PLTREL is neither DT_RELA nor DT_REL";
        return result;
    }

    if (tags.hash_table_address) {
        const std::uint64_t address = *tags.hash_table_address;
        if ((address & 3) != 0) {
            result.error = "DT_HASH is not 4-byte aligned";
            return result;
        }
        std::array<std::uint8_t, 8> header{};
        if (!ReadModule(module, address, header)) {
            result.error = "DT_HASH does not point to a complete readable header";
            return result;
        }
        const std::uint32_t bucket_count = ReadU32(header);
        const std::uint32_t chain_count = ReadU32(header, 4);
        if (bucket_count == 0 || chain_count == 0) {
            result.error = "DT_HASH has a zero bucket or chain count";
            return result;
        }
        const std::uint64_t word_count =
            std::uint64_t{2} + bucket_count + static_cast<std::uint64_t>(chain_count);
        const std::uint64_t byte_size = word_count * sizeof(std::uint32_t);
        if (!RangeFits(address, byte_size, module.image_size)) {
            result.error = "DT_HASH extent lies outside the module image";
            return result;
        }
        if (RangesOverlap(address, byte_size, info.dynamic_address, info.dynamic_byte_size)) {
            result.error = "DT_HASH extent overlaps the dynamic table";
            return result;
        }
        info.sysv_hash = NsoSysvHashInfo{
            .address = address,
            .byte_size = byte_size,
            .bucket_count = bucket_count,
            .chain_count = chain_count,
        };
    }

    info.string_table_address = tags.string_table_address;
    info.string_table_size = tags.string_table_size;
    if (!ValidateAddressSizePair(module, tags.string_table_address, tags.string_table_size,
                                 "DT_STRTAB", "DT_STRSZ", result.error)) {
        return result;
    }
    info.symbol_table_address = tags.symbol_table_address;
    if (info.symbol_table_address.has_value()) {
        if ((*info.symbol_table_address & 7) != 0) {
            result.error = "DT_SYMTAB is not 8-byte aligned";
            return result;
        }
        std::array<std::uint8_t, NsoElf64SymbolEntrySize> null_symbol{};
        if (!ReadModule(module, *info.symbol_table_address, null_symbol)) {
            result.error = "DT_SYMTAB does not point to a complete readable ELF64 symbol entry";
            return result;
        }
    }

    if (tags.rel_address.has_value() || tags.rel_size.value_or(0) != 0 ||
        tags.rel_entry_size.has_value()) {
        result.warnings.emplace_back("DT_REL metadata is present but REL tables are not parsed");
    }
    if (tags.relr_address.has_value() || tags.relr_size.value_or(0) != 0 ||
        tags.relr_entry_size.has_value()) {
        result.warnings.emplace_back("DT_RELR metadata is present but RELR tables are not parsed");
    }

    if (!ParseRelaTable(module, tags.rela_address, tags.rela_size, NsoRelaTableKind::Dynamic,
                        info.rela_entry_size, info.dynamic_address, info.dynamic_byte_size,
                        max_relocations, info.rela, result.error)) {
        return result;
    }
    const std::uint64_t parsed_relocations = info.rela ? info.rela->records.size() : 0;
    if (info.plt_relocation_tag == static_cast<std::uint64_t>(DtRel) &&
        (tags.jump_rela_address.has_value() || tags.plt_rela_size.value_or(0) != 0)) {
        result.error = "DT_JMPREL uses unsupported DT_REL entries rather than RELA";
        return result;
    }
    if (!ParseRelaTable(module, tags.jump_rela_address, tags.plt_rela_size,
                        NsoRelaTableKind::ProcedureLinkage, info.rela_entry_size,
                        info.dynamic_address, info.dynamic_byte_size,
                        max_relocations - parsed_relocations, info.plt_rela, result.error)) {
        return result;
    }
    if (info.rela && info.plt_rela &&
        RangesOverlap(info.rela->address, info.rela->byte_size, info.plt_rela->address,
                      info.plt_rela->byte_size)) {
        result.error = "DT_RELA and DT_JMPREL table extents overlap";
        return result;
    }

    const auto has_symbol_relocation = [](const std::optional<NsoRelaTable>& table) {
        return table &&
               std::any_of(table->records.begin(), table->records.end(),
                           [](const NsoRelaRecord& record) { return record.symbol_index != 0; });
    };
    if (!info.symbol_table_address &&
        (has_symbol_relocation(info.rela) || has_symbol_relocation(info.plt_rela))) {
        result.error = "a RELA record references a symbol but DT_SYMTAB is absent";
        return result;
    }
    if (info.symbol_table_address) {
        const auto validate_symbol_references = [&](const std::optional<NsoRelaTable>& table) {
            if (!table) {
                return true;
            }
            for (const NsoRelaRecord& record : table->records) {
                if (record.symbol_index == 0) {
                    continue;
                }
                const std::uint64_t symbol_offset =
                    static_cast<std::uint64_t>(record.symbol_index) * info.symbol_entry_size;
                if (!RangeFits(*info.symbol_table_address, symbol_offset, module.image_size)) {
                    result.error = "a RELA record's symbol index overflows the module image";
                    return false;
                }
                const std::uint64_t symbol_address = *info.symbol_table_address + symbol_offset;
                std::array<std::uint8_t, NsoElf64SymbolEntrySize> symbol_bytes{};
                if (!ReadModule(module, symbol_address, symbol_bytes)) {
                    result.error =
                        "a RELA record's symbol index lies outside readable symbol-table bytes";
                    return false;
                }
            }
            return true;
        };
        if (!validate_symbol_references(info.rela) || !validate_symbol_references(info.plt_rela)) {
            return result;
        }
    }

    result.info = std::move(info);
    return result;
}

NsoDynamicParseResult ParseNsoDynamic(const DecodedNso& image, std::size_t max_dynamic_entries,
                                      std::uint64_t max_relocations) {
    NsoDynamicParseResult result;
    if (!ValidateDecodedImage(image, result.error)) {
        return result;
    }
    result = ParseNsoDynamic(MakeDecodedNsoModuleViewUnchecked(image), max_dynamic_entries,
                             max_relocations);
    if (result && result.info->string_table_address && result.info->string_table_size &&
        !DecodedRangeIsReadable(image, *result.info->string_table_address,
                                *result.info->string_table_size)) {
        result.info.reset();
        result.error = "DT_STRTAB/DT_STRSZ extent contains unreadable decoded NSO bytes";
    }
    if (result && result.info->sysv_hash &&
        !DecodedRangeIsReadable(image, result.info->sysv_hash->address,
                                result.info->sysv_hash->byte_size)) {
        result.info.reset();
        result.error = "DT_HASH extent contains unreadable decoded NSO bytes";
    }
    return result;
}

const char* NsoRelaTableKindName(NsoRelaTableKind kind) {
    switch (kind) {
    case NsoRelaTableKind::Dynamic:
        return "dynamic";
    case NsoRelaTableKind::ProcedureLinkage:
        return "procedure-linkage";
    }
    return "unknown";
}

} // namespace suyu::recomp
