// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

// The host floating-point mode that FPX1 generated code relies on (see the FPX1
// block of the generated recomp_runtime.h). Its native fast paths keep a result
// only when it equals the architectural one, and that argument needs IEEE
// round-to-nearest without flushing of subnormal inputs or outputs:
// - x86-64: MXCSR & 0xFFC0 == 0x1F80: RC nearest, DAZ and FTZ clear, every
//   exception masked (the six sticky flags are ignored);
// - AArch64: FPCR with RMode nearest and no FZ, DN, AHP, FZ16, trap enables or
//   FEAT_AFP bits.
// Header-only and dependency-free so tests/recompiler_fpx runs the same check.
namespace Core::RecompFpEnv {

inline constexpr std::uint32_t kMxcsrMask = 0xFFC0u;
inline constexpr std::uint32_t kMxcsrRequired = 0x1F80u;
inline constexpr std::uint64_t kFpcrMask = 0x07C8FF07u;

/// True if this thread's FP mode is the one FPX1 code needs.
inline bool Conforms() {
#if defined(_M_X64) || defined(__x86_64__)
    return (_mm_getcsr() & kMxcsrMask) == kMxcsrRequired;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    return (fpcr & kFpcrMask) == 0;
#else
    // No FPX1 fast path is compiled for other hosts (RECOMP_FPX_HOST 0).
    return true;
#endif
}

/// Puts this thread's FP mode into the required state, keeping the sticky
/// flags. Returns true if it had to change anything.
inline bool Ensure() {
#if defined(_M_X64) || defined(__x86_64__)
    const unsigned csr = _mm_getcsr();
    if ((csr & kMxcsrMask) == kMxcsrRequired) {
        return false;
    }
    _mm_setcsr((csr & 0x3Fu) | kMxcsrRequired);
    return true;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if ((fpcr & kFpcrMask) == 0) {
        return false;
    }
    fpcr &= ~kFpcrMask;
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    return true;
#else
    return false;
#endif
}

} // namespace Core::RecompFpEnv
