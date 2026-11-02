// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <mutex>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "core/crypto/ctr_encryption_layer.h"
#include "core/crypto/hmac_sha256.h"
#include "core/crypto/portable_seal.h"
#include "core/file_sys/vfs/vfs.h"

namespace Core::Crypto::PortableSeal {

namespace {

std::array<u8, 16> CounterBlock(const Nonce& nonce, u64 offset) {
    std::array<u8, 16> iv{};
    std::memcpy(iv.data(), nonce.data(), nonce.size());
    u64 block = offset >> 4;
    for (std::size_t i = 0; i < 8; ++i) {
        iv[15 - i] = static_cast<u8>(block & 0xFF);
        block >>= 8;
    }
    return iv;
}

// CTREncryptionLayer keeps one cipher context, so concurrent reads are serialised here.
class SealedFile final : public EncryptionLayer {
public:
    SealedFile(FileSys::VirtualFile base_, const Key128& key, const Nonce& nonce)
        : EncryptionLayer(base_), layer(std::move(base_), key, 0) {
        layer.SetIV(CounterBlock(nonce, 0));
    }

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        std::scoped_lock lock{mutex};
        return layer.Read(data, length, offset);
    }

private:
    mutable std::mutex mutex;
    CTREncryptionLayer layer;
};

} // Anonymous namespace

std::optional<Key128> DeriveKey(const Key128& sd_seed, std::string_view export_id) {
    std::vector<u8> message(kSealContext.begin(), kSealContext.end());
    message.insert(message.end(), export_id.begin(), export_id.end());
    HMACSHA256Hash hash{};
    if (!CalculateHMACSHA256(hash, sd_seed.data(), sd_seed.size(), message.data(),
                             message.size())) {
        return std::nullopt;
    }
    Key128 key{};
    std::memcpy(key.data(), hash.data(), key.size());
    return key;
}

std::optional<CheckValue> ComputeCheck(const Key128& seal_key) {
    HMACSHA256Hash hash{};
    if (!CalculateHMACSHA256(hash, seal_key.data(), seal_key.size(), kCheckLabel.data(),
                             kCheckLabel.size())) {
        return std::nullopt;
    }
    CheckValue check{};
    std::memcpy(check.data(), hash.data(), check.size());
    return check;
}

bool CheckEquals(const CheckValue& a, const CheckValue& b) {
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::optional<Nonce> RandomNonce() {
    Nonce nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return std::nullopt;
    }
    return nonce;
}

std::string ToHex(const u8* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        text += kDigits[data[i] >> 4];
        text += kDigits[data[i] & 0xF];
    }
    return text;
}

bool FromHex(std::string_view text, u8* out, std::size_t size) {
    if (text.size() != size * 2) {
        return false;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < size; ++i) {
        const int high = digit(text[i * 2]);
        const int low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<u8>((high << 4) | low);
    }
    return true;
}

Sealer::Sealer(const Key128& seal_key, const Nonce& nonce_)
    : cipher(seal_key, Mode::CTR), nonce(nonce_) {}

void Sealer::Apply(u8* data, std::size_t size, u64 offset) {
    cipher.SetIV(CounterBlock(nonce, offset));
    cipher.Transcode(data, size, data, Op::Encrypt);
}

FileSys::VirtualFile OpenSealed(FileSys::VirtualFile sealed, const Key128& seal_key,
                                const Nonce& nonce) {
    if (sealed == nullptr) {
        return nullptr;
    }
    return std::make_shared<SealedFile>(std::move(sealed), seal_key, nonce);
}

} // namespace Core::Crypto::PortableSeal
