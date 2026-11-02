// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/interface/exclusive_monitor.h"

#include <algorithm>

#include "common/assert.h"

namespace Dynarmic {

namespace {

// Reservations may be published without the lock (see ReadAndMark).
VAddr LoadAddress(const VAddr& address) {
#if DYNARMIC_LOCKFREE_EXCLUSIVE_MARK
    return __atomic_load_n(&address, __ATOMIC_SEQ_CST);
#else
    return address;
#endif
}

void StoreAddress(VAddr& address, VAddr value) {
#if DYNARMIC_LOCKFREE_EXCLUSIVE_MARK
    __atomic_store_n(&address, value, __ATOMIC_SEQ_CST);
#else
    address = value;
#endif
}

void ClearIfEqual(VAddr& address, VAddr expected, VAddr invalid) {
#if DYNARMIC_LOCKFREE_EXCLUSIVE_MARK
    __atomic_compare_exchange_n(&address, &expected, invalid, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#else
    if (address == expected) {
        address = invalid;
    }
#endif
}

}  // namespace

ExclusiveMonitor::ExclusiveMonitor(std::size_t processor_count)
        : exclusive_addresses(processor_count, INVALID_EXCLUSIVE_ADDRESS), exclusive_values(processor_count) {}

size_t ExclusiveMonitor::GetProcessorCount() const {
    return exclusive_addresses.size();
}

void ExclusiveMonitor::Lock() {
    lock.Lock();
}

void ExclusiveMonitor::Unlock() {
    lock.Unlock();
}

bool ExclusiveMonitor::CheckAndClear(std::size_t processor_id, VAddr address) {
    const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

    Lock();
    if (LoadAddress(exclusive_addresses[processor_id]) != masked_address) {
        Unlock();
        return false;
    }

    for (VAddr& other_address : exclusive_addresses) {
        ClearIfEqual(other_address, masked_address, INVALID_EXCLUSIVE_ADDRESS);
    }
    return true;
}

void ExclusiveMonitor::Clear() {
    Lock();
    for (VAddr& address : exclusive_addresses) {
        StoreAddress(address, INVALID_EXCLUSIVE_ADDRESS);
    }
    Unlock();
}

void ExclusiveMonitor::ClearProcessor(std::size_t processor_id) {
    Lock();
    StoreAddress(exclusive_addresses[processor_id], INVALID_EXCLUSIVE_ADDRESS);
    Unlock();
}

}  // namespace Dynarmic
