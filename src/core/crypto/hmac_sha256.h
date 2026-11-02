// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace Core::Crypto {

using HMACSHA256Hash = std::array<u8, 0x20>;

/// HMAC-SHA256 of @p data under @p key, written to @p out. Returns false when OpenSSL fails.
/// Safe to call from several threads at once.
bool CalculateHMACSHA256(HMACSHA256Hash& out, const void* key, std::size_t key_length,
                         const void* data, std::size_t data_length);

} // namespace Core::Crypto
