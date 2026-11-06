// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/recompiler/nso_relocations.h"

#include <cstdint>
#include <span>
#include <string>

namespace suyu::recomp {

using NsoRelocationMemoryReadCallback = bool (*)(void* user, std::uint64_t address,
                                                 std::span<std::uint8_t> destination);
using NsoRelocationMemoryWriteCallback = bool (*)(void* user, std::uint64_t address,
                                                  std::span<const std::uint8_t> source);

/// Checked absolute-address access supplied by the hosted runtime. A callback must return false
/// unless the complete span was transferred. It must not execute title code.
struct NsoRelocationMemory {
    void* user{};
    NsoRelocationMemoryReadCallback read{};
    NsoRelocationMemoryWriteCallback write{};
};

enum class NsoRelocationCommitState {
    /// No guest-memory write was attempted.
    Rejected,
    /// Every relocation and finalization write completed.
    Committed,
    /// A write failed, but every possibly changed byte was restored.
    RolledBack,
    /// A write failed and at least one restoration write also failed. Execution must stop.
    RollbackFailed,
};

struct NsoRelocationCommitResult {
    NsoRelocationCommitState state{NsoRelocationCommitState::Rejected};
    std::uint64_t relocation_writes{};
    std::uint64_t finalization_writes{};
    std::string error;

    explicit operator bool() const {
        return state == NsoRelocationCommitState::Committed && error.empty();
    }
};

/// Commits a previously validated relocation plan as one hosted-runtime transaction. The complete
/// plan and the DT_RELASZ/DT_PLTRELSZ value fields are snapshotted before the first write. Dynamic
/// size fields are cleared only after every relocation succeeds. Any failed write triggers a
/// reverse-order rollback, including the possibly partially written operation.
NsoRelocationCommitResult CommitNsoRelocationPlan(const NsoRelocationPlan& plan,
                                                  const NsoDynamicInfo& dynamic,
                                                  const NsoRelocationMemory& memory);

const char* NsoRelocationCommitStateName(NsoRelocationCommitState state);

} // namespace suyu::recomp
