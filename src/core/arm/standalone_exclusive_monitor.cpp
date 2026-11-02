// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "core/arm/standalone_exclusive_monitor.h"
#include "core/memory.h"

namespace Core {

StandaloneExclusiveMonitor::StandaloneExclusiveMonitor(Memory::Memory& memory_,
                                                       std::size_t core_count_)
    : exclusive_addresses(core_count_, kInvalidAddress), exclusive_values(core_count_),
      memory{memory_} {}

StandaloneExclusiveMonitor::~StandaloneExclusiveMonitor() = default;

template <typename T, typename Function>
T StandaloneExclusiveMonitor::ReadAndMark(std::size_t core_index, VAddr addr, Function op) {
    static_assert(std::is_trivially_copyable_v<T>);
    lock.lock();
    exclusive_addresses[core_index] = addr;
    const T value = op();
    std::memcpy(exclusive_values[core_index].data(), &value, sizeof(T));
    lock.unlock();
    return value;
}

bool StandaloneExclusiveMonitor::CheckAndClear(std::size_t core_index, VAddr addr) {
    lock.lock();
    if (exclusive_addresses[core_index] != addr) {
        lock.unlock();
        return false;
    }
    // Every core reserving this address loses its reservation, not just the one
    // storing: that is what makes a competing store-exclusive fail rather than
    // both succeeding.
    for (VAddr& other : exclusive_addresses) {
        if (other == addr) {
            other = kInvalidAddress;
        }
    }
    // Returns holding the lock. The caller unlocks once its store is done.
    return true;
}

template <typename T, typename Function>
bool StandaloneExclusiveMonitor::DoExclusiveOperation(std::size_t core_index, VAddr addr,
                                                      Function op) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!CheckAndClear(core_index, addr)) {
        return false;
    }
    T expected;
    std::memcpy(&expected, exclusive_values[core_index].data(), sizeof(T));
    const bool result = op(expected);
    lock.unlock();
    return result;
}

u8 StandaloneExclusiveMonitor::ExclusiveRead8(std::size_t core_index, VAddr addr) {
    return ReadAndMark<u8>(core_index, addr, [&]() -> u8 { return memory.Read8(addr); });
}

u16 StandaloneExclusiveMonitor::ExclusiveRead16(std::size_t core_index, VAddr addr) {
    return ReadAndMark<u16>(core_index, addr, [&]() -> u16 { return memory.Read16(addr); });
}

u32 StandaloneExclusiveMonitor::ExclusiveRead32(std::size_t core_index, VAddr addr) {
    return ReadAndMark<u32>(core_index, addr, [&]() -> u32 { return memory.Read32(addr); });
}

u64 StandaloneExclusiveMonitor::ExclusiveRead64(std::size_t core_index, VAddr addr) {
    return ReadAndMark<u64>(core_index, addr, [&]() -> u64 { return memory.Read64(addr); });
}

u128 StandaloneExclusiveMonitor::ExclusiveRead128(std::size_t core_index, VAddr addr) {
    return ReadAndMark<u128>(core_index, addr, [&]() -> u128 {
        u128 result;
        result[0] = memory.Read64(addr);
        result[1] = memory.Read64(addr + 8);
        return result;
    });
}

void StandaloneExclusiveMonitor::ClearExclusive(std::size_t core_index) {
    std::scoped_lock guard{lock};
    exclusive_addresses[core_index] = kInvalidAddress;
}

bool StandaloneExclusiveMonitor::ExclusiveWrite8(std::size_t core_index, VAddr vaddr, u8 value) {
    return DoExclusiveOperation<u8>(core_index, vaddr, [&](u8 expected) -> bool {
        return memory.WriteExclusive8(vaddr, value, expected);
    });
}

bool StandaloneExclusiveMonitor::ExclusiveWrite16(std::size_t core_index, VAddr vaddr, u16 value) {
    return DoExclusiveOperation<u16>(core_index, vaddr, [&](u16 expected) -> bool {
        return memory.WriteExclusive16(vaddr, value, expected);
    });
}

bool StandaloneExclusiveMonitor::ExclusiveWrite32(std::size_t core_index, VAddr vaddr, u32 value) {
    return DoExclusiveOperation<u32>(core_index, vaddr, [&](u32 expected) -> bool {
        return memory.WriteExclusive32(vaddr, value, expected);
    });
}

bool StandaloneExclusiveMonitor::ExclusiveWrite64(std::size_t core_index, VAddr vaddr, u64 value) {
    return DoExclusiveOperation<u64>(core_index, vaddr, [&](u64 expected) -> bool {
        return memory.WriteExclusive64(vaddr, value, expected);
    });
}

bool StandaloneExclusiveMonitor::ExclusiveWrite128(std::size_t core_index, VAddr vaddr, u128 value) {
    return DoExclusiveOperation<u128>(core_index, vaddr, [&](u128 expected) -> bool {
        return memory.WriteExclusive128(vaddr, value, expected);
    });
}

} // namespace Core
