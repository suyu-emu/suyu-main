// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_relocations.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace suyu::recomp {
namespace {

constexpr std::uint64_t NsoModuleAddressSpaceSize = std::uint64_t{1} << 32;

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool AddAddress(std::uint64_t base, std::uint64_t offset, std::uint64_t& result) {
    if (offset > std::numeric_limits<std::uint64_t>::max() - base) {
        return false;
    }
    result = base + offset;
    return true;
}

bool IsNoneRelocation(std::uint32_t type) {
    return type == NsoAarch64RelocationNone || type == NsoAarch64RelocationWithdrawnNone;
}

bool ValidateDecodedImage(const DecodedNso& image, std::uint64_t& image_size, std::string& error) {
    image_size = 0;
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        const NsoSegmentInfo& segment = image.info.segments[i];
        if (image.segments[i].size() != segment.decoded_size) {
            error = std::string{NsoSegmentName(static_cast<NsoSegmentId>(i))} +
                    " decoded buffer size does not match its NSO metadata";
            return false;
        }
        if (!RangeFits(segment.memory_offset, segment.decoded_size, NsoModuleAddressSpaceSize)) {
            error = std::string{NsoSegmentName(static_cast<NsoSegmentId>(i))} +
                    " decoded memory range overflows the 32-bit module image";
            return false;
        }
        image_size = std::max(image_size, static_cast<std::uint64_t>(segment.memory_offset) +
                                              segment.decoded_size);
    }
    const std::string layout_error = ValidateNsoExecutableLayout(image.info);
    if (!layout_error.empty()) {
        error = "decoded NSO executable layout is invalid: " + layout_error;
        return false;
    }
    const NsoSegmentInfo& data = image.info.segments[static_cast<std::size_t>(NsoSegmentId::Data)];
    const std::uint64_t data_end =
        static_cast<std::uint64_t>(data.memory_offset) + data.decoded_size;
    if (!RangeFits(data_end, image.info.bss_size, NsoModuleAddressSpaceSize)) {
        error = "decoded NSO data/BSS range overflows the 32-bit module image";
        return false;
    }
    image_size = std::max(image_size, data_end + image.info.bss_size);
    return true;
}

bool ValidateModuleMapping(std::uint64_t image_size, std::uint64_t module_base,
                           std::string& error) {
    if (image_size == 0 || image_size > NsoModuleAddressSpaceSize) {
        error = "module image size must be in the range [1, 2^32]";
        return false;
    }
    if (image_size - 1 > std::numeric_limits<std::uint64_t>::max() - module_base) {
        error = "mapped module image overflows the 64-bit address space";
        return false;
    }
    return true;
}

bool ValidateTableShape(const NsoRelaTable& table, NsoRelaTableKind expected_kind,
                        std::uint64_t image_size, std::uint64_t max_writes,
                        std::uint64_t& write_count, std::string& error) {
    if (table.kind != expected_kind) {
        error = std::string{NsoRelaTableKindName(expected_kind)} +
                " RELA table has the wrong table kind";
        return false;
    }
    if (table.entry_size != NsoElf64RelaEntrySize ||
        table.records.size() > std::numeric_limits<std::uint64_t>::max() / table.entry_size ||
        table.byte_size != table.records.size() * table.entry_size) {
        error = std::string{NsoRelaTableKindName(table.kind)} +
                " RELA table metadata does not match its decoded records";
        return false;
    }
    if ((table.address & 7) != 0 || !RangeFits(table.address, table.byte_size, image_size)) {
        error = std::string{NsoRelaTableKindName(table.kind)} +
                " RELA table extent is not 8-byte aligned or does not fit in the module image";
        return false;
    }
    for (std::size_t index = 0; index < table.records.size(); ++index) {
        const NsoRelaRecord& record = table.records[index];
        const std::uint64_t expected_info =
            (static_cast<std::uint64_t>(record.symbol_index) << 32) | record.type;
        if (record.info != expected_info) {
            error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                    std::to_string(index) + " has inconsistent decoded r_info fields";
            return false;
        }
        if (!IsNoneRelocation(record.type)) {
            if (write_count == max_writes) {
                error = "ELF64 writes exceed the configured relocation-plan limit";
                return false;
            }
            ++write_count;
        }
    }
    return true;
}

struct ResolvedSymbol {
    std::uint64_t value{};
    NsoRelocationValueSource source{NsoRelocationValueSource::NullSymbol};
};

bool ResolveSymbol(const NsoDynamicSymbolInfo& symbols, std::uint32_t index,
                   std::uint64_t module_base, std::uint64_t module_image_size,
                   NsoExternalSymbolResolver external_resolver, void* resolver_user,
                   ResolvedSymbol& resolved, std::string& error) {
    if (index == 0) {
        resolved = {};
        return true;
    }
    if (index >= symbols.symbols.size()) {
        error = "relocation references dynamic symbol index " + std::to_string(index) +
                " outside the " + std::to_string(symbols.symbols.size()) + "-entry table";
        return false;
    }

    const NsoDynamicSymbol& symbol = symbols.symbols[index];
    if (symbol.index != index) {
        error = "dynamic symbol table entry " + std::to_string(index) +
                " has an inconsistent decoded index";
        return false;
    }
    if (symbol.IsUndefined()) {
        std::uint64_t external_address = 0;
        if (external_resolver != nullptr &&
            external_resolver(resolver_user, symbol, external_address)) {
            resolved = {external_address, NsoRelocationValueSource::ExternalDefinition};
            return true;
        }
        if (symbol.IsWeak()) {
            resolved = {0, NsoRelocationValueSource::UndefinedWeak};
            return true;
        }
        error = "unresolved strong dynamic symbol '" +
                (symbol.name.empty() ? std::string{"<unnamed>"} : symbol.name) + "'";
        return false;
    }
    if (symbol.Type() == NsoElfSymbolTypeGnuIfunc) {
        error = "defined GNU IFUNC dynamic symbol '" +
                (symbol.name.empty() ? std::string{"<unnamed>"} : symbol.name) +
                "' requires executing a guest resolver";
        return false;
    }
    if (symbol.section_index == NsoElfSectionCommon) {
        error = "COMMON dynamic symbol '" +
                (symbol.name.empty() ? std::string{"<unnamed>"} : symbol.name) +
                "' requires allocating loader storage";
        return false;
    }
    if (symbol.IsAbsolute()) {
        resolved = {symbol.value, NsoRelocationValueSource::AbsoluteDefinition};
        return true;
    }
    if (symbol.section_index >= NsoElfSectionLowReserved) {
        error = "dynamic symbol '" +
                (symbol.name.empty() ? std::string{"<unnamed>"} : symbol.name) +
                "' uses an unsupported reserved ELF section index";
        return false;
    }
    if (symbol.value > module_image_size) {
        error = "module-relative dynamic symbol '" +
                (symbol.name.empty() ? std::string{"<unnamed>"} : symbol.name) +
                "' lies outside the module image";
        return false;
    }
    std::uint64_t address = 0;
    if (!AddAddress(module_base, symbol.value, address)) {
        error = "module-relative dynamic symbol '" +
                (symbol.name.empty() ? std::string{"<unnamed>"} : symbol.name) +
                "' overflows the 64-bit address space";
        return false;
    }
    resolved = {address, NsoRelocationValueSource::ModuleDefinition};
    return true;
}

NsoRelocationPlanResult PlanNsoRelocationsImpl(std::uint64_t module_image_size,
                                               const NsoDynamicInfo& dynamic,
                                               const NsoDynamicSymbolInfo& symbols,
                                               std::uint64_t module_base,
                                               NsoExternalSymbolResolver external_resolver,
                                               void* resolver_user, std::uint64_t max_writes) {
    NsoRelocationPlanResult result;
    NsoRelocationPlan plan{
        .module_base = module_base,
        .module_image_size = module_image_size,
    };
    if (!ValidateModuleMapping(module_image_size, module_base, result.error)) {
        return result;
    }

    std::uint64_t write_count = 0;
    if ((dynamic.rela &&
         !ValidateTableShape(*dynamic.rela, NsoRelaTableKind::Dynamic, module_image_size,
                             max_writes, write_count, result.error)) ||
        (dynamic.plt_rela &&
         !ValidateTableShape(*dynamic.plt_rela, NsoRelaTableKind::ProcedureLinkage,
                             module_image_size, max_writes, write_count, result.error)) ||
        write_count > std::numeric_limits<std::size_t>::max()) {
        if (result.error.empty()) {
            result.error = "NSO relocation plan is unsupported on this host";
        }
        return result;
    }

    try {
        plan.writes.reserve(static_cast<std::size_t>(write_count));
        const auto append_table = [&](const std::optional<NsoRelaTable>& optional_table) {
            if (!optional_table) {
                return true;
            }
            const NsoRelaTable& table = *optional_table;
            for (std::size_t index = 0; index < table.records.size(); ++index) {
                const NsoRelaRecord& record = table.records[index];
                if (IsNoneRelocation(record.type)) {
                    continue;
                }
                if (record.type == NsoAarch64RelocationIRelative) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) +
                                   " uses R_AARCH64_IRELATIVE, which requires executing a guest "
                                   "resolver";
                    return false;
                }
                if (record.type != NsoAarch64RelocationRelative &&
                    record.type != NsoAarch64RelocationAbs64 &&
                    record.type != NsoAarch64RelocationGlobDat &&
                    record.type != NsoAarch64RelocationJumpSlot) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) +
                                   " uses unsupported AArch64 relocation type " +
                                   std::to_string(record.type);
                    return false;
                }
                if ((record.offset & 7) != 0) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) + " destination is not 8-byte aligned";
                    return false;
                }
                if (!RangeFits(record.offset, sizeof(std::uint64_t), module_image_size)) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) +
                                   " 8-byte destination lies outside the module image";
                    return false;
                }
                std::uint64_t address = 0;
                if (!AddAddress(module_base, record.offset, address)) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) +
                                   " destination overflows the 64-bit address space";
                    return false;
                }
                if ((address & 7) != 0) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) +
                                   " absolute destination is not 8-byte aligned";
                    return false;
                }
                if (sizeof(std::uint64_t) - 1 >
                    std::numeric_limits<std::uint64_t>::max() - address) {
                    result.error = std::string{NsoRelaTableKindName(table.kind)} + " RELA record " +
                                   std::to_string(index) +
                                   " destination end overflows the 64-bit address space";
                    return false;
                }

                NsoPlannedRelocation write{
                    .table_kind = table.kind,
                    .table_index = index,
                    .module_offset = record.offset,
                    .address = address,
                    .addend = record.addend,
                    .type = record.type,
                    .symbol_index = record.symbol_index,
                };
                if (record.type == NsoAarch64RelocationRelative) {
                    if (record.symbol_index != 0) {
                        result.error = std::string{NsoRelaTableKindName(table.kind)} +
                                       " RELA record " + std::to_string(index) +
                                       " uses R_AARCH64_RELATIVE with a nonzero symbol index";
                        return false;
                    }
                    write.value = module_base + std::bit_cast<std::uint64_t>(record.addend);
                    write.value_source = NsoRelocationValueSource::Relative;
                } else {
                    ResolvedSymbol symbol;
                    if (!ResolveSymbol(symbols, record.symbol_index, module_base, module_image_size,
                                       external_resolver, resolver_user, symbol, result.error)) {
                        return false;
                    }
                    write.value = symbol.value + std::bit_cast<std::uint64_t>(record.addend);
                    write.value_source = symbol.source;
                }
                plan.writes.push_back(std::move(write));
            }
            return true;
        };
        if (!append_table(dynamic.rela) || !append_table(dynamic.plt_rela)) {
            return result;
        }
    } catch (const std::bad_alloc&) {
        result.error = "could not allocate the NSO relocation plan";
        return result;
    } catch (const std::length_error&) {
        result.error = "NSO relocation plan is unsupported on this host";
        return result;
    }

    result.plan = std::move(plan);
    return result;
}

} // namespace

NsoRelocationPlanResult PlanNsoRelocations(std::uint64_t module_image_size,
                                           const NsoDynamicInfo& dynamic,
                                           const NsoDynamicSymbolInfo& symbols,
                                           std::uint64_t module_base,
                                           NsoExternalSymbolResolver external_resolver,
                                           void* resolver_user, std::uint64_t max_writes) {
    return PlanNsoRelocationsImpl(module_image_size, dynamic, symbols, module_base,
                                  external_resolver, resolver_user, max_writes);
}

NsoRelocationPlanResult PlanNsoRelocations(const DecodedNso& image, const NsoDynamicInfo& dynamic,
                                           const NsoDynamicSymbolInfo& symbols,
                                           std::uint64_t module_base,
                                           NsoExternalSymbolResolver external_resolver,
                                           void* resolver_user, std::uint64_t max_writes) {
    NsoRelocationPlanResult result;
    std::uint64_t module_image_size = 0;
    if (!ValidateDecodedImage(image, module_image_size, result.error)) {
        return result;
    }
    return PlanNsoRelocationsImpl(module_image_size, dynamic, symbols, module_base,
                                  external_resolver, resolver_user, max_writes);
}

const char* NsoRelocationValueSourceName(NsoRelocationValueSource source) {
    switch (source) {
    case NsoRelocationValueSource::Relative:
        return "relative";
    case NsoRelocationValueSource::NullSymbol:
        return "null-symbol";
    case NsoRelocationValueSource::ModuleDefinition:
        return "module-definition";
    case NsoRelocationValueSource::AbsoluteDefinition:
        return "absolute-definition";
    case NsoRelocationValueSource::ExternalDefinition:
        return "external-definition";
    case NsoRelocationValueSource::UndefinedWeak:
        return "undefined-weak";
    }
    return "unknown";
}

} // namespace suyu::recomp
