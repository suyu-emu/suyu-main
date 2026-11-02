// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "core/arm/exclusive_monitor.h"

namespace Core::Memory {
class Memory;
}

namespace Core {

/// The exclusive monitor, without dynarmic behind it.
///
/// suyu's only implementation of this interface wraps Dynarmic::ExclusiveMonitor,
/// and every process builds one whether or not a JIT ever runs - which is why
/// dynarmic shows up in the profile of a run that never enters it, and why the
/// library cannot be unlinked while that is the only implementation.
///
/// The semantics are dynarmic's, deliberately: a reservation is one address per
/// core, a successful store clears that address on *every* core holding it, and
/// the lock is held across the compare-and-write so two cores cannot both win
/// the same reservation. The lock being held between CheckAndClear returning
/// true and the store completing is load-bearing, not an oversight.
class StandaloneExclusiveMonitor final : public ExclusiveMonitor {
public:
    explicit StandaloneExclusiveMonitor(Memory::Memory& memory_, std::size_t core_count_);
    ~StandaloneExclusiveMonitor() override;

    u8 ExclusiveRead8(std::size_t core_index, VAddr addr) override;
    u16 ExclusiveRead16(std::size_t core_index, VAddr addr) override;
    u32 ExclusiveRead32(std::size_t core_index, VAddr addr) override;
    u64 ExclusiveRead64(std::size_t core_index, VAddr addr) override;
    u128 ExclusiveRead128(std::size_t core_index, VAddr addr) override;
    void ClearExclusive(std::size_t core_index) override;

    bool ExclusiveWrite8(std::size_t core_index, VAddr vaddr, u8 value) override;
    bool ExclusiveWrite16(std::size_t core_index, VAddr vaddr, u16 value) override;
    bool ExclusiveWrite32(std::size_t core_index, VAddr vaddr, u32 value) override;
    bool ExclusiveWrite64(std::size_t core_index, VAddr vaddr, u64 value) override;
    bool ExclusiveWrite128(std::size_t core_index, VAddr vaddr, u128 value) override;

private:
    /// Reserve `addr` for `core_index` and read through `op`.
    template <typename T, typename Function>
    T ReadAndMark(std::size_t core_index, VAddr addr, Function op);

    /// True with the lock still held when `core_index` still owns `addr`; the
    /// caller must complete its store and then unlock. False with the lock
    /// released.
    bool CheckAndClear(std::size_t core_index, VAddr addr);

    template <typename T, typename Function>
    bool DoExclusiveOperation(std::size_t core_index, VAddr addr, Function op);

    static constexpr VAddr kInvalidAddress = 0xDEADDEADDEADDEADULL;

    std::mutex lock;
    std::vector<VAddr> exclusive_addresses;
    std::vector<std::array<u64, 2>> exclusive_values;
    Memory::Memory& memory;
};

} // namespace Core
