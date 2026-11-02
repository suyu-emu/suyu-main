// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Sealing for portable game exports.
//
// A portable export carries the user's game file and installed update NCAs exactly as they
// are (still in their original encryption), with one more AES-128-CTR layer on top. The key
// for that layer is derived from the sd_seed of the console the export was made with:
//
//   seal_key = HMAC-SHA256(key = sd_seed, msg = kSealContext || export_id)[0..16)
//
// Each sealed file has its own random 8-byte nonce, the high half of the CTR counter; the
// low half is the 16-byte block index (offset >> 4, big-endian), so any range of a sealed
// file can be read without reading what comes before it. A check value,
// HMAC-SHA256(seal_key, kCheckLabel)[0..16), tells keys from another console apart from a
// damaged file. Neither the seal key nor the sd_seed is ever stored.

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core::Crypto::PortableSeal {

inline constexpr std::string_view kSealContext = "suyu-portable-seal-v1";
inline constexpr std::string_view kCheckLabel = "check";
inline constexpr std::string_view kSealFormat = "suyu-portable-seal-1";

using Nonce = std::array<u8, 8>;
using CheckValue = std::array<u8, 16>;

/// The seal key for an export, or nullopt when HMAC fails.
std::optional<Key128> DeriveKey(const Key128& sd_seed, std::string_view export_id);
/// The check value stored beside the sealed files for @p seal_key.
std::optional<CheckValue> ComputeCheck(const Key128& seal_key);
/// Compares two check values in constant time.
bool CheckEquals(const CheckValue& a, const CheckValue& b);
/// A fresh random nonce for one sealed file.
std::optional<Nonce> RandomNonce();

/// Lower-case hex and back; FromHex returns false for anything but exactly 2*N hex digits.
std::string ToHex(const u8* data, std::size_t size);
bool FromHex(std::string_view text, u8* out, std::size_t size);

/// Seals (or, identically, unseals) consecutive chunks of one file while it is copied.
class Sealer {
public:
    Sealer(const Key128& seal_key, const Nonce& nonce);
    /// Transforms @p size bytes in place that sit at @p offset in the file. @p offset must be a
    /// multiple of 16.
    void Apply(u8* data, std::size_t size, u64 offset);

private:
    AESCipher<Key128> cipher;
    Nonce nonce;
};

/// A read-only view that presents the original bytes of a sealed file. Safe for concurrent
/// reads.
FileSys::VirtualFile OpenSealed(FileSys::VirtualFile sealed, const Key128& seal_key,
                                const Nonce& nonce);

} // namespace Core::Crypto::PortableSeal
