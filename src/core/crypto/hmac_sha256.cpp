// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <openssl/evp.h>

#include "core/crypto/hmac_sha256.h"

namespace Core::Crypto {

bool CalculateHMACSHA256(HMACSHA256Hash& out, const void* key, std::size_t key_length,
                         const void* data, std::size_t data_length) {
    std::size_t out_length = 0;
    const unsigned char* result =
        EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key, key_length,
                  static_cast<const unsigned char*>(data), data_length, out.data(), out.size(),
                  &out_length);
    return result != nullptr && out_length == out.size();
}

} // namespace Core::Crypto
