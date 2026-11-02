// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include "common/common_types.h"

// A complete audit trail of the guest's lock-related supervisor calls.
//
// A static-recompiled title deadlocks with guest threads parked in
// svcArbitrateLock on a word that reads zero. That is a lost wakeup, but it has
// two very different causes and the thread dump alone cannot tell them apart:
//
//   - the owner never issued svcArbitrateUnlock at all, because the waiters bit
//     never reached the lock word, or
//   - the owner did issue it, but the waiter had queued itself on a different
//     thread's waiter list, because the handle in the lock word named the wrong
//     owner. KConditionVariable::SignalToAddress only searches the *calling*
//     thread's list, so that waiter is orphaned and the word is written zero.
//
// Recording every lock call with its outcome separates the two. The whole run
// issues well under 65536 supervisor calls, so the ring holds the entire
// history rather than a sample and nothing of interest can age out.
//
// Off unless SUYU_LOCK_TRACE is set, so a normal run pays nothing.

namespace Kernel::LockTrace {

enum class Ev : u32 {
    LockEnter,   // svcArbitrateLock entry.       a=owner handle, b=own tag, c=word now
    LockExit,    // svcArbitrateLock return.      a=result
    Unlock,      // svcArbitrateUnlock.           a=word before, b=word written, c=next waiter tid
    Await,       // svcWaitForAddress entry.      a=arb type, b=value, c=word now
    AwaitExit,   // svcWaitForAddress return.     a=result
    Signal,      // svcSignalToAddress.           a=signal type, b=value, c=count
};

bool Enabled();

void Record(Ev kind, u32 tid, u64 addr, u32 a = 0, u32 b = 0, u32 c = 0);

/// Every recorded event whose address is `addr`, oldest first.
std::string DumpForAddress(u64 addr);

/// Counts per event kind, and the last `tail` events regardless of address.
std::string DumpSummary(std::size_t tail);

} // namespace Kernel::LockTrace
