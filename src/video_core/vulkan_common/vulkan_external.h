// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

#include "common/common_types.h"
// The core Vulkan types only: the libretro core includes this, and the platform headers
// video_core/vulkan_common/vulkan.h pulls in (Xlib) define macros like Success and None
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

// A Vulkan context owned by a frontend the renderer runs inside of (the libretro core):
// the renderer uses the frontend's instance and physical device, creates its device
// through the frontend (which adds what it needs to present), takes the frontend's queue
// lock around its submits, and hands each finished frame over as an image instead of
// reading it back. Inactive unless the frontend sets it up before the renderer is made.
namespace Vulkan::External {

struct Context {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    /// The frontend's vkCreateDevice; only valid while the renderer is being constructed.
    std::function<VkDevice(VkPhysicalDevice, const VkDeviceCreateInfo*)> create_device;

    /// Filled in by the renderer once its device exists.
    VkDevice device{};
    VkQueue queue{};
    u32 queue_family{};
};

/// A frame the renderer finished, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
struct Frame {
    VkImage image{};
    VkImageView view{};
    VkFormat format{};
    u32 width{};
    u32 height{};
    u32 slot{};
};

void SetActive(bool active);
bool IsActive();
Context& GetContext();

/// The frontend's queue lock, taken around every submit to the shared queue.
void SetQueueLock(std::function<void()> lock, std::function<void()> unlock);
void LockQueue();
void UnlockQueue();

/// Renderer side: a slot of `count` that the frontend is not holding on to.
u32 PickSlot(u32 count);
/// Renderer side: the newest finished frame; replaces one the frontend did not take yet.
void Publish(const Frame& frame);

/// Frontend side: the newest finished frame, if any arrived since the last call.
bool Consume(Frame& frame);
/// Frontend side: slot of the frame just handed to the frontend's presentation.
void MarkShown(u32 slot);
/// Clears the frame handoff, for a new session.
void ResetFrames();

} // namespace Vulkan::External
