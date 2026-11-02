// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

// Feature bits an ABI 6 recompiled image reports from recomp_image_features().
//
// A feature bit means the image relies on something from the host. So a host
// must refuse any image that reports a bit it does not implement: loading it
// anyway would run code whose contract nobody is keeping. Every loader checks
// Unsupported() before any other handshake.
//
// Bit assignments, shared by every branch that adds a feature. Take the next
// free bit; never reuse one.
//   bit 0 (0x1)  FM1   page-table fastmem (RECOMP_FEATURE_FASTMEM_PT1)
//   bit 1 (0x2)  GG1   generation code guard (RECOMP_FEATURE_GUARD_GEN1)
//   bit 2 (0x4)  FPX1  exact native floating point (RECOMP_FEATURE_FPX1)
//   bits 3-31          free
//
// Deliberately dependency-free so tests/recompiler_smoke can include it.
namespace Core::RecompImageFeature {

inline constexpr std::uint32_t FastmemPT1 = 1u << 0;
inline constexpr std::uint32_t GuardGen1 = 1u << 1;
inline constexpr std::uint32_t ExactFpX1 = 1u << 2;

/// Features this host implements.
inline constexpr std::uint32_t kHostSupported = FastmemPT1 | GuardGen1 | ExactFpX1;

/// The bits of `features` a host implementing `supported` does not know; any
/// nonzero result means the image must be refused.
constexpr std::uint32_t Unsupported(std::uint32_t features,
                                    std::uint32_t supported = kHostSupported) {
    return features & ~supported;
}

} // namespace Core::RecompImageFeature
