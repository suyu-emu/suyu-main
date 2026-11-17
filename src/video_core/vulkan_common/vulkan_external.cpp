// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <mutex>

#include "video_core/vulkan_common/vulkan_external.h"

namespace Vulkan::External {

namespace {

bool active = false;
Context context;

std::mutex lock_mutex;
std::function<void()> queue_lock;
std::function<void()> queue_unlock;

// The frontend may still be reading the last few images it was handed (its own
// frames in flight, and the one it repeats while nothing new arrives), so those
// slots are not drawn into again until newer ones have taken their place.
constexpr size_t HeldCount = 3;

std::mutex frame_mutex;
Frame pending{};
bool has_pending = false;
std::array<s64, HeldCount> held{-1, -1, -1};

} // Anonymous namespace

void SetActive(bool active_) {
    active = active_;
}

bool IsActive() {
    return active;
}

Context& GetContext() {
    return context;
}

void SetQueueLock(std::function<void()> lock, std::function<void()> unlock) {
    std::scoped_lock lk{lock_mutex};
    queue_lock = std::move(lock);
    queue_unlock = std::move(unlock);
}

void LockQueue() {
    std::function<void()> lock;
    {
        std::scoped_lock lk{lock_mutex};
        lock = queue_lock;
    }
    if (lock) {
        lock();
    }
}

void UnlockQueue() {
    std::function<void()> unlock;
    {
        std::scoped_lock lk{lock_mutex};
        unlock = queue_unlock;
    }
    if (unlock) {
        unlock();
    }
}

u32 PickSlot(u32 count) {
    std::scoped_lock lk{frame_mutex};
    static u32 next = 0;
    for (u32 tries = 0; tries < count; ++tries) {
        const u32 slot = next;
        next = (next + 1) % count;
        const bool is_held =
            std::find(held.begin(), held.end(), static_cast<s64>(slot)) != held.end();
        const bool is_pending = has_pending && pending.slot == slot;
        if (!is_held && !is_pending) {
            return slot;
        }
    }
    // Only with fewer slots than frames held; the oldest one is the least likely in use.
    return next;
}

void Publish(const Frame& frame) {
    std::scoped_lock lk{frame_mutex};
    pending = frame;
    has_pending = true;
}

bool Consume(Frame& frame) {
    std::scoped_lock lk{frame_mutex};
    if (!has_pending) {
        return false;
    }
    frame = pending;
    has_pending = false;
    return true;
}

void MarkShown(u32 slot) {
    std::scoped_lock lk{frame_mutex};
    std::rotate(held.rbegin(), held.rbegin() + 1, held.rend());
    held[0] = slot;
}

void ResetFrames() {
    std::scoped_lock lk{frame_mutex};
    has_pending = false;
    held.fill(-1);
}

} // namespace Vulkan::External
