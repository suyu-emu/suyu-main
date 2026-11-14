// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace Common {

/// A mutex that polls for a moment before it sleeps. For a lock that is held for microseconds at
/// a time but handed over thousands of times a second, sleeping and being woken through the
/// kernel costs both sides more than the wait itself.
template <typename Mutex>
class AdaptiveMutex {
public:
    void lock() {
        if (mutex.try_lock()) {
            Acquired();
            return;
        }
        for (int i = 0; i < POLL_COUNT; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
            // Look before trying, so that waiting doesn't keep taking the holder's cache line
            if (!busy.load(std::memory_order_relaxed) && mutex.try_lock()) {
                Acquired();
                return;
            }
        }
        mutex.lock();
        Acquired();
    }

    bool try_lock() {
        if (!mutex.try_lock()) {
            return false;
        }
        Acquired();
        return true;
    }

    void unlock() {
        if (--depth == 0) {
            busy.store(false, std::memory_order_relaxed);
        }
        mutex.unlock();
    }

private:
    static constexpr int POLL_COUNT = 2000;

    void Acquired() {
        if (depth++ == 0) {
            busy.store(true, std::memory_order_relaxed);
        }
    }

    Mutex mutex;
    std::atomic<bool> busy{false};
    /// Only touched while the mutex is held
    int depth{0};
};

} // namespace Common
