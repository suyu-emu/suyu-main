// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "core/crypto/aes_util.h"

namespace Core::Crypto {
namespace {

TEST_CASE("AESCipher initializes valid all-zero AES keys", "[core][crypto]") {
    using TestKey128 = std::array<u8, 16>;
    constexpr TestKey128 zero_key{};
    constexpr std::array<u8, 16> zero_block{};
    constexpr std::array<u8, 16> encrypted_zero_block{
        0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c, 0x3b,
        0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34, 0x2b, 0x2e,
    };

    SECTION("ECB") {
        AESCipher<TestKey128> cipher{zero_key, Mode::ECB};
        std::array<u8, 16> encrypted{};
        std::array<u8, 16> decrypted{};

        cipher.Transcode(zero_block.data(), zero_block.size(), encrypted.data(), Op::Encrypt);
        cipher.Transcode(encrypted.data(), encrypted.size(), decrypted.data(), Op::Decrypt);

        REQUIRE(encrypted == encrypted_zero_block);
        REQUIRE(decrypted == zero_block);
    }

    SECTION("CTR") {
        AESCipher<TestKey128> cipher{zero_key, Mode::CTR};
        cipher.SetIV(zero_block);
        std::array<u8, 16> encrypted{};
        std::array<u8, 16> decrypted{};

        cipher.Transcode(zero_block.data(), zero_block.size(), encrypted.data(), Op::Encrypt);
        cipher.Transcode(encrypted.data(), encrypted.size(), decrypted.data(), Op::Decrypt);

        REQUIRE(encrypted == encrypted_zero_block);
        REQUIRE(decrypted == zero_block);
    }
}

} // namespace
} // namespace Core::Crypto
