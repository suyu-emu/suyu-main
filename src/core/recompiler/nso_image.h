// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace suyu::recomp {

constexpr std::size_t NsoHeaderSize = 0x100;
constexpr std::size_t NsoSegmentCount = 3;
constexpr std::uint64_t DefaultNsoDecodeLimit = std::uint64_t{1} << 30;

enum class NsoSegmentId : std::size_t {
    Text = 0,
    RoData = 1,
    Data = 2,
};

enum class NsoCompression {
    None,
    Lz4,
    Zbic,
};

struct NsoRelativeExtent {
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct NsoSegmentInfo {
    std::uint32_t file_offset{};
    std::uint32_t memory_offset{};
    std::uint32_t decoded_size{};
    std::uint32_t compressed_size_field{};
    std::uint32_t stored_size{};
    std::uint32_t extra{};
    NsoCompression compression{NsoCompression::None};
    bool hash_required{};
    std::array<std::uint8_t, 0x20> expected_hash{};
};

struct NsoInfo {
    std::uint32_t version{};
    std::uint32_t reserved{};
    std::uint32_t flags{};
    std::array<std::uint8_t, 0x20> build_id{};
    std::array<NsoSegmentInfo, NsoSegmentCount> segments{};
    std::uint32_t module_name_offset{};
    std::uint32_t module_name_size{};
    std::uint32_t bss_size{};
    NsoRelativeExtent api_info{};
    NsoRelativeExtent dynstr{};
    NsoRelativeExtent dynsym{};
    bool execute_only{};
    bool uses_zbic{};
};

struct NsoInspection {
    std::optional<NsoInfo> info;
    std::vector<std::string> warnings;
    std::string error;

    explicit operator bool() const {
        return info.has_value() && error.empty();
    }
};

using NsoDecompressor = bool (*)(std::span<const std::uint8_t> source,
                                 std::span<std::uint8_t> destination);
using NsoSha256Hasher = bool (*)(std::span<const std::uint8_t> source,
                                 std::array<std::uint8_t, 0x20>& digest);

struct DecodedNso {
    NsoInfo info;
    std::array<std::vector<std::uint8_t>, NsoSegmentCount> segments;
    /// True when every segment whose NSO0 flag requires SHA-256 checking was verified, or when no
    /// segment requested hash checking.
    bool required_hashes_verified{};
};

struct NsoDecodeResult {
    std::optional<DecodedNso> image;
    std::vector<std::string> warnings;
    std::string error;

    explicit operator bool() const {
        return image.has_value() && error.empty();
    }
};

/// Inspects and structurally validates an NSO0 image without decoding its segments.
NsoInspection InspectNso(std::span<const std::uint8_t> file);

/// Validates the page-aligned, ordered destination layout required to load an executable NSO.
/// Returns an empty string when the layout is valid, otherwise a user-facing error.
std::string ValidateNsoExecutableLayout(const NsoInfo& info);

/// Decodes every segment in a structurally valid NSO0 image. The decompressor is needed only when
/// a segment uses LZ4. When supplied, the SHA-256 callback verifies every segment whose NSO0 flag
/// requires it after decompression. ZBIC decoding is intentionally not implemented yet.
NsoDecodeResult DecodeNso(std::span<const std::uint8_t> file,
                          NsoDecompressor lz4_decompressor = nullptr,
                          std::uint64_t max_decoded_bytes = DefaultNsoDecodeLimit,
                          NsoSha256Hasher sha256_hasher = nullptr);

/// Returns the target of the conventional AArch64 `b` process-entry stub when text+4 points to a
/// valid MOD0 header. For a later NintendoSDK module whose reserved first word is zero, returns its
/// validated ELF64 DT_INIT target instead. Returns zero when neither layout can be identified.
/// Calling this is an explicit architecture assumption; NSO0 itself has no ISA field.
std::uint32_t FindNsoAarch64EntryOffset(std::span<const std::uint8_t> text);

/// As above, but resolves the module-relative MOD0 pointer across every decoded NSO segment.
/// Retail modules commonly place MOD0 in rodata rather than beside the entry stub in text.
std::uint32_t FindNsoAarch64EntryOffset(const DecodedNso& image);

std::string NsoBuildIdToHex(const std::array<std::uint8_t, 0x20>& build_id);
const char* NsoSegmentName(NsoSegmentId segment);
const char* NsoCompressionName(NsoCompression compression);

} // namespace suyu::recomp
