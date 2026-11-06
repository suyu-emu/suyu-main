// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/recompiler/nso_symbols.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace suyu::recomp {

constexpr std::uint32_t NsoAarch64RelocationNone = 0;
constexpr std::uint32_t NsoAarch64RelocationWithdrawnNone = 0x100;
constexpr std::uint32_t NsoAarch64RelocationAbs64 = 0x101;
constexpr std::uint32_t NsoAarch64RelocationGlobDat = 0x401;
constexpr std::uint32_t NsoAarch64RelocationJumpSlot = 0x402;
constexpr std::uint32_t NsoAarch64RelocationRelative = 0x403;
constexpr std::uint32_t NsoAarch64RelocationIRelative = 0x408;

enum class NsoRelocationValueSource {
    Relative,
    NullSymbol,
    ModuleDefinition,
    AbsoluteDefinition,
    ExternalDefinition,
    UndefinedWeak,
};

struct NsoPlannedRelocation {
    NsoRelaTableKind table_kind{NsoRelaTableKind::Dynamic};
    std::uint64_t table_index{};
    std::uint64_t module_offset{};
    std::uint64_t address{};
    std::uint64_t value{};
    std::int64_t addend{};
    std::uint32_t type{};
    std::uint32_t symbol_index{};
    NsoRelocationValueSource value_source{NsoRelocationValueSource::Relative};
};

struct NsoRelocationPlan {
    std::uint64_t module_base{};
    std::uint64_t module_image_size{};
    std::vector<NsoPlannedRelocation> writes;
};

struct NsoRelocationPlanResult {
    std::optional<NsoRelocationPlan> plan;
    std::string error;

    explicit operator bool() const {
        return plan.has_value() && error.empty();
    }
};

/// Returns true and writes an absolute address when an undefined symbol is available in the
/// caller's load scope. Returning false means not found; an unfound weak reference resolves to
/// zero, while an unfound strong reference rejects the plan.
using NsoExternalSymbolResolver = bool (*)(void* user, const NsoDynamicSymbol& symbol,
                                           std::uint64_t& address);

/// Produces a plan from an already-proven contiguous module mapping. This overload is suitable for
/// a hosted runtime that parsed metadata through NsoModuleView and knows the exact mapped extent.
/// The resolver must return the final address for an undefined symbol; it must not execute title
/// code. Defined symbols bind to this module; ELF definition preemption is outside this planner's
/// scope. R_AARCH64_NONE records are validated but intentionally produce no write.
NsoRelocationPlanResult PlanNsoRelocations(std::uint64_t module_image_size,
                                           const NsoDynamicInfo& dynamic,
                                           const NsoDynamicSymbolInfo& symbols,
                                           std::uint64_t module_base,
                                           NsoExternalSymbolResolver external_resolver = nullptr,
                                           void* resolver_user = nullptr,
                                           std::uint64_t max_writes = DefaultNsoRelocationLimit);

/// Produces an all-or-nothing list of aligned, in-image ELF64 writes for the supported AArch64
/// dynamic relocation types. This function never mutates `image` or guest memory. `dynamic` and
/// `symbols` must be successful parse results for the same `image`. In addition to planning, this
/// overload revalidates decoded segment sizes and derives the complete segment-plus-BSS extent.
NsoRelocationPlanResult PlanNsoRelocations(const DecodedNso& image, const NsoDynamicInfo& dynamic,
                                           const NsoDynamicSymbolInfo& symbols,
                                           std::uint64_t module_base,
                                           NsoExternalSymbolResolver external_resolver = nullptr,
                                           void* resolver_user = nullptr,
                                           std::uint64_t max_writes = DefaultNsoRelocationLimit);

const char* NsoRelocationValueSourceName(NsoRelocationValueSource source);

} // namespace suyu::recomp
