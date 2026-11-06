// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "nso_sha256.h"

#include <mbedtls/sha256.h>

namespace suyu::recomp::tool {

bool ComputeSha256(std::span<const std::uint8_t> source,
                   std::array<std::uint8_t, 0x20>& digest) {
    return mbedtls_sha256_ret(source.data(), source.size(), digest.data(), 0) == 0;
}

} // namespace suyu::recomp::tool
