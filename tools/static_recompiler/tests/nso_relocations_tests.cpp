// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_relocations.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t ModuleBase = 0x1'0000'0000;
constexpr std::uint64_t ExternalAddress = 0xDEAD'BEEF;

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void CheckError(const suyu::recomp::NsoRelocationPlanResult& result, std::string_view text,
                std::string_view description) {
    Check(!result && !result.plan && result.error.find(text) != std::string::npos, description);
}

suyu::recomp::DecodedNso MakeImage() {
    suyu::recomp::DecodedNso image;
    image.info.segments[0].memory_offset = 0;
    image.info.segments[0].decoded_size = 0x1000;
    image.info.segments[1].memory_offset = 0x1000;
    image.info.segments[1].decoded_size = 0x1000;
    image.info.segments[2].memory_offset = 0x2000;
    image.info.segments[2].decoded_size = 0x100;
    image.info.bss_size = 0x100;
    for (std::size_t i = 0; i < suyu::recomp::NsoSegmentCount; ++i) {
        image.segments[i].resize(image.info.segments[i].decoded_size);
    }
    return image;
}

suyu::recomp::NsoDynamicSymbolInfo MakeSymbols() {
    suyu::recomp::NsoDynamicSymbolInfo info;
    info.symbols.resize(5);
    info.symbols[1] = {
        .index = 1,
        .info = static_cast<std::uint8_t>(suyu::recomp::NsoElfSymbolBindingGlobal << 4),
        .section_index = 1,
        .value = 0x80,
        .name = "module",
    };
    info.symbols[2] = {
        .index = 2,
        .info = static_cast<std::uint8_t>(suyu::recomp::NsoElfSymbolBindingGlobal << 4),
        .section_index = suyu::recomp::NsoElfSectionAbsolute,
        .value = 0x1234,
        .name = "absolute",
    };
    info.symbols[3] = {
        .index = 3,
        .info = static_cast<std::uint8_t>(suyu::recomp::NsoElfSymbolBindingWeak << 4),
        .section_index = suyu::recomp::NsoElfSectionUndefined,
        .name = "optional",
    };
    info.symbols[4] = {
        .index = 4,
        .info = static_cast<std::uint8_t>(suyu::recomp::NsoElfSymbolBindingGlobal << 4),
        .section_index = suyu::recomp::NsoElfSectionUndefined,
        .name = "required",
    };
    return info;
}

suyu::recomp::NsoRelaRecord Rela(std::uint64_t offset, std::uint32_t symbol_index,
                                 std::uint32_t type, std::int64_t addend) {
    return {
        .offset = offset,
        .info = (static_cast<std::uint64_t>(symbol_index) << 32) | type,
        .addend = addend,
        .symbol_index = symbol_index,
        .type = type,
    };
}

suyu::recomp::NsoRelaTable Table(suyu::recomp::NsoRelaTableKind kind,
                                 std::vector<suyu::recomp::NsoRelaRecord> records) {
    return {
        .kind = kind,
        .address = 0x1000,
        .byte_size = records.size() * suyu::recomp::NsoElf64RelaEntrySize,
        .entry_size = suyu::recomp::NsoElf64RelaEntrySize,
        .records = std::move(records),
    };
}

suyu::recomp::NsoDynamicInfo MakeDynamic() {
    suyu::recomp::NsoDynamicInfo info;
    info.rela = Table(suyu::recomp::NsoRelaTableKind::Dynamic,
                      {
                          Rela(0x2000, 0, suyu::recomp::NsoAarch64RelocationRelative, 0x40),
                          Rela(0x2008, 1, suyu::recomp::NsoAarch64RelocationAbs64, -8),
                          Rela(0x2010, 2, suyu::recomp::NsoAarch64RelocationGlobDat, 4),
                          Rela(0x2018, 0, suyu::recomp::NsoAarch64RelocationAbs64, -1),
                          Rela(0x2020, 4, suyu::recomp::NsoAarch64RelocationGlobDat, 8),
                      });
    info.plt_rela = Table(suyu::recomp::NsoRelaTableKind::ProcedureLinkage,
                          {Rela(0x2028, 3, suyu::recomp::NsoAarch64RelocationJumpSlot, 0)});
    return info;
}

struct ResolverState {
    std::size_t calls{};
};

bool ResolveExternal(void* user, const suyu::recomp::NsoDynamicSymbol& symbol,
                     std::uint64_t& address) {
    auto& state = *static_cast<ResolverState*>(user);
    ++state.calls;
    if (symbol.name != "required") {
        return false;
    }
    address = ExternalAddress;
    return true;
}

suyu::recomp::NsoDynamicInfo OneRelocation(suyu::recomp::NsoRelaRecord record) {
    suyu::recomp::NsoDynamicInfo info;
    info.rela = Table(suyu::recomp::NsoRelaTableKind::Dynamic, {record});
    return info;
}

void RunSuccessTest() {
    const auto image = MakeImage();
    const auto symbols = MakeSymbols();
    const auto dynamic = MakeDynamic();
    ResolverState resolver;
    const auto result = suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase,
                                                         ResolveExternal, &resolver);
    Check(static_cast<bool>(result), "supported relocations produce a complete plan");
    if (!result) {
        return;
    }

    const auto& plan = *result.plan;
    Check(plan.module_base == ModuleBase && plan.module_image_size == 0x2200,
          "module base and segment-plus-BSS image size are retained");
    Check(plan.writes.size() == 6, "dynamic and PLT RELA records are planned in order");
    if (plan.writes.size() != 6) {
        return;
    }
    Check(plan.writes[0].address == ModuleBase + 0x2000 &&
              plan.writes[0].value == ModuleBase + 0x40 &&
              plan.writes[0].value_source == suyu::recomp::NsoRelocationValueSource::Relative,
          "R_AARCH64_RELATIVE uses the load base plus addend");
    Check(plan.writes[1].value == ModuleBase + 0x78 &&
              plan.writes[1].value_source ==
                  suyu::recomp::NsoRelocationValueSource::ModuleDefinition,
          "R_AARCH64_ABS64 bases a module definition and adds a signed addend modulo 2^64");
    Check(plan.writes[2].value == 0x1238 &&
              plan.writes[2].value_source ==
                  suyu::recomp::NsoRelocationValueSource::AbsoluteDefinition,
          "R_AARCH64_GLOB_DAT does not load-bias an SHN_ABS definition and includes its addend");
    Check(plan.writes[3].value == std::numeric_limits<std::uint64_t>::max() &&
              plan.writes[3].value_source == suyu::recomp::NsoRelocationValueSource::NullSymbol,
          "symbol zero contributes zero and a negative addend wraps modulo 2^64");
    Check(plan.writes[4].value == ExternalAddress + 8 &&
              plan.writes[4].value_source ==
                  suyu::recomp::NsoRelocationValueSource::ExternalDefinition,
          "an external definition is resolved before applying S+A");
    Check(plan.writes[5].table_kind == suyu::recomp::NsoRelaTableKind::ProcedureLinkage &&
              plan.writes[5].table_index == 0 && plan.writes[5].value == 0 &&
              plan.writes[5].value_source == suyu::recomp::NsoRelocationValueSource::UndefinedWeak,
          "an unresolved weak JUMP_SLOT resolves to zero without a synthetic stub");
    Check(resolver.calls == 2, "load-scope resolution is attempted for strong and weak imports");
    Check(std::string_view{suyu::recomp::NsoRelocationValueSourceName(
              suyu::recomp::NsoRelocationValueSource::UndefinedWeak)} == "undefined-weak",
          "relocation value sources have stable diagnostic names");
}

void RunFailureTests() {
    const auto image = MakeImage();
    const auto symbols = MakeSymbols();

    CheckError(suyu::recomp::PlanNsoRelocations(image, MakeDynamic(), symbols, ModuleBase),
               "unresolved strong", "unresolved strong symbols reject the entire plan");

    auto dynamic = OneRelocation(Rela(0x2001, 0, suyu::recomp::NsoAarch64RelocationRelative, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "not 8-byte aligned", "unaligned ELF64 destinations are rejected");

    auto short_image = image;
    short_image.info.bss_size = 0x104;
    dynamic = OneRelocation(Rela(0x2200, 0, suyu::recomp::NsoAarch64RelocationRelative, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(short_image, dynamic, symbols, ModuleBase),
               "outside the module image", "the complete 8-byte write must fit the image");

    dynamic = OneRelocation(Rela(0x2000, 0, 0xDEAD, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "unsupported AArch64 relocation type", "unknown relocation types are rejected");

    dynamic = OneRelocation(Rela(0x2000, 0, suyu::recomp::NsoAarch64RelocationIRelative, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "executing a guest resolver", "IRELATIVE is rejected instead of executed");

    dynamic = OneRelocation(Rela(0x2000, 1, suyu::recomp::NsoAarch64RelocationRelative, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "nonzero symbol index", "RELATIVE must use dynamic symbol zero");

    dynamic = OneRelocation(Rela(0x2000, 5, suyu::recomp::NsoAarch64RelocationGlobDat, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "outside the 5-entry table", "symbol indices are checked again at planning time");

    dynamic = OneRelocation(Rela(0x2000, 0, suyu::recomp::NsoAarch64RelocationRelative, 0));
    const std::uint64_t near_end_base = std::numeric_limits<std::uint64_t>::max() - 0x2003;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, near_end_base),
               "mapped module image overflows", "the complete mapped image extent cannot wrap");

    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, 1),
               "absolute destination is not 8-byte aligned",
               "an aligned module offset cannot hide a misaligned load base");

    auto overflowing_symbols = symbols;
    overflowing_symbols.symbols[1].value = 0x2200;
    dynamic = OneRelocation(Rela(0x2000, 1, suyu::recomp::NsoAarch64RelocationAbs64, 0));
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, overflowing_symbols,
                                                std::numeric_limits<std::uint64_t>::max() - 0x21FF),
               "symbol 'module' overflows", "module-relative symbol addresses cannot wrap");

    dynamic = MakeDynamic();
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase,
                                                ResolveExternal, nullptr, 5),
               "relocation-plan limit", "the configured total write limit is enforced");

    dynamic = OneRelocation(Rela(0x2000, 0, suyu::recomp::NsoAarch64RelocationRelative, 0));
    dynamic.rela->byte_size -= 1;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "metadata does not match", "malformed RELA table metadata is rejected");

    auto malformed_image = image;
    malformed_image.segments[2].pop_back();
    CheckError(suyu::recomp::PlanNsoRelocations(malformed_image, dynamic, symbols, ModuleBase),
               "buffer size does not match", "inconsistent decoded segment buffers are rejected");
}

void RunDefensiveMetadataTests() {
    const auto image = MakeImage();
    const auto symbols = MakeSymbols();

    auto dynamic = OneRelocation(Rela(0x2001, 4, suyu::recomp::NsoAarch64RelocationNone, 123));
    const auto none =
        suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase, nullptr, nullptr, 0);
    Check(none && none.plan->writes.empty(),
          "R_AARCH64_NONE is a no-op and does not consume the write limit");

    dynamic = OneRelocation(Rela(0x2001, 99, suyu::recomp::NsoAarch64RelocationWithdrawnNone, 123));
    const auto withdrawn_none =
        suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase, nullptr, nullptr, 0);
    Check(withdrawn_none && withdrawn_none.plan->writes.empty(),
          "withdrawn AArch64 relocation code 256 is accepted as R_AARCH64_NONE");

    dynamic = OneRelocation(Rela(0x2000, 1, suyu::recomp::NsoAarch64RelocationGlobDat, 0));
    auto ifunc_symbols = symbols;
    ifunc_symbols.symbols[1].info = static_cast<std::uint8_t>(
        (suyu::recomp::NsoElfSymbolBindingGlobal << 4) | suyu::recomp::NsoElfSymbolTypeGnuIfunc);
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, ifunc_symbols, ModuleBase),
               "GNU IFUNC", "a defined GNU IFUNC is not mistaken for its resolver result");

    auto common_symbols = symbols;
    common_symbols.symbols[1].section_index = suyu::recomp::NsoElfSectionCommon;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, common_symbols, ModuleBase),
               "allocating loader storage", "COMMON symbols are deferred to a real loader");

    auto reserved_symbols = symbols;
    reserved_symbols.symbols[1].section_index = 0xFFFF;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, reserved_symbols, ModuleBase),
               "reserved ELF section index", "unsupported special section indices are rejected");

    auto outside_symbols = symbols;
    outside_symbols.symbols[1].value = 0x2201;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, outside_symbols, ModuleBase),
               "outside the module image", "module-relative definitions must fit the mapping");

    auto end_symbols = symbols;
    end_symbols.symbols[1].value = 0x2200;
    const auto end_symbol =
        suyu::recomp::PlanNsoRelocations(image, dynamic, end_symbols, ModuleBase);
    Check(end_symbol && end_symbol.plan->writes[0].value == ModuleBase + 0x2200,
          "a module definition may denote its one-past-image boundary");

    auto wrong_index_symbols = symbols;
    wrong_index_symbols.symbols[1].index = 2;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, wrong_index_symbols, ModuleBase),
               "inconsistent decoded index", "fabricated dynamic symbol indices are rejected");

    dynamic.rela->kind = suyu::recomp::NsoRelaTableKind::ProcedureLinkage;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "wrong table kind", "a RELA table cannot masquerade as the other table");

    dynamic = OneRelocation(Rela(0x2000, 1, suyu::recomp::NsoAarch64RelocationGlobDat, 0));
    dynamic.rela->records[0].info = 0;
    CheckError(suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase),
               "inconsistent decoded r_info", "split relocation fields must match raw r_info");

    dynamic = OneRelocation(Rela(0x2000, 0, suyu::recomp::NsoAarch64RelocationRelative, 0x2200));
    const auto one_past = suyu::recomp::PlanNsoRelocations(image, dynamic, symbols, ModuleBase);
    Check(one_past && one_past.plan->writes[0].value == ModuleBase + 0x2200,
          "a relocation value may legitimately point one byte past the module image");

    CheckError(suyu::recomp::PlanNsoRelocations(0, dynamic, symbols, ModuleBase), "range [1, 2^32]",
               "an empty hosted module mapping is rejected");
    CheckError(suyu::recomp::PlanNsoRelocations((std::uint64_t{1} << 32) + 1, dynamic, symbols,
                                                ModuleBase),
               "range [1, 2^32]", "a hosted NSO mapping cannot exceed its 32-bit image space");
}

void RunQlaunchScaleTest() {
    constexpr std::size_t DynamicRelocationCount = 41'115;
    constexpr std::uint64_t QlaunchImageSize = 0x426e000;
    std::vector<suyu::recomp::NsoRelaRecord> records;
    records.reserve(DynamicRelocationCount);
    for (std::size_t i = 0; i < DynamicRelocationCount; ++i) {
        records.push_back(Rela(0x2000, 0, suyu::recomp::NsoAarch64RelocationRelative,
                               i + 1 == DynamicRelocationCount ? QlaunchImageSize : 0x40));
    }
    suyu::recomp::NsoDynamicInfo dynamic;
    dynamic.rela = Table(suyu::recomp::NsoRelaTableKind::Dynamic, std::move(records));
    dynamic.rela->address = 0;
    dynamic.plt_rela = Table(suyu::recomp::NsoRelaTableKind::ProcedureLinkage,
                             {Rela(0x2008, 3, suyu::recomp::NsoAarch64RelocationJumpSlot, 0)});
    dynamic.plt_rela->address = 0xF0E88;

    const auto symbols = MakeSymbols();
    const auto result = suyu::recomp::PlanNsoRelocations(QlaunchImageSize, dynamic, symbols,
                                                         ModuleBase, nullptr, nullptr, 41'116);
    Check(result && result.plan->writes.size() == 41'116,
          "the exact qlaunch relocation scale fits the planner and its inclusive limit");
    if (result) {
        Check(result.plan->writes.front().table_kind == suyu::recomp::NsoRelaTableKind::Dynamic &&
                  result.plan->writes[DynamicRelocationCount - 1].value ==
                      ModuleBase + QlaunchImageSize &&
                  result.plan->writes.back().table_kind ==
                      suyu::recomp::NsoRelaTableKind::ProcedureLinkage,
              "qlaunch-scale ordering and its one-past-image sentinel value are preserved");
    }
    CheckError(suyu::recomp::PlanNsoRelocations(QlaunchImageSize, dynamic, symbols, ModuleBase,
                                                nullptr, nullptr, 41'115),
               "write", "one fewer allowed write rejects the qlaunch-scale plan transaction");
}

} // namespace

int main() {
    RunSuccessTest();
    RunFailureTests();
    RunDefensiveMetadataTests();
    RunQlaunchScaleTest();
    if (failures != 0) {
        std::cerr << failures << " NSO relocation planner test(s) failed\n";
        return 1;
    }
    std::cout << "NSO relocation planner tests passed\n";
    return 0;
}
