// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_image.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>

namespace suyu::recomp {
namespace {

constexpr std::uint32_t KnownFlagsMask = 0xFF;

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool RangesOverlap(std::uint64_t first_offset, std::uint64_t first_size,
                   std::uint64_t second_offset, std::uint64_t second_size) {
    if (first_size == 0 || second_size == 0) {
        return false;
    }
    return first_offset < second_offset + second_size && second_offset < first_offset + first_size;
}

std::string SegmentError(std::size_t index, const char* message) {
    std::ostringstream out;
    out << NsoSegmentName(static_cast<NsoSegmentId>(index)) << " segment " << message;
    return out.str();
}

bool ValidateRoExtent(const NsoRelativeExtent& extent, std::uint32_t rodata_size, const char* name,
                      std::string& error) {
    if (extent.size == 0) {
        return true;
    }
    if (!RangeFits(extent.offset, extent.size, rodata_size)) {
        error = std::string{name} + " extent lies outside the decoded rodata segment";
        return false;
    }
    return true;
}

std::uint32_t FindNsoAarch64EntryOffsetImpl(
    std::span<const std::uint8_t> text,
    const std::array<std::uint32_t, NsoSegmentCount>& memory_offsets,
    const std::array<std::span<const std::uint8_t>, NsoSegmentCount>& segments) {
    if (text.size() < 8) {
        return 0;
    }

    constexpr std::uint64_t MinimumMod0HeaderSize = 0x1C;
    constexpr std::uint32_t Mod0Magic = 0x30444F4D; // "MOD0", little-endian
    const std::uint32_t mod0_offset = ReadU32(text, 4);
    if ((mod0_offset & 3) != 0) {
        return 0;
    }
    const std::uint64_t mod0_address =
        static_cast<std::uint64_t>(memory_offsets[static_cast<std::size_t>(NsoSegmentId::Text)]) +
        mod0_offset;
    std::size_t mod0_segment = NsoSegmentCount;
    std::uint64_t mod0_local_offset = 0;
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        if (mod0_address < memory_offsets[i]) {
            continue;
        }
        const std::uint64_t local_offset =
            static_cast<std::uint64_t>(mod0_address) - memory_offsets[i];
        if (RangeFits(local_offset, MinimumMod0HeaderSize, segments[i].size()) &&
            ReadU32(segments[i], static_cast<std::size_t>(local_offset)) == Mod0Magic) {
            mod0_segment = i;
            mod0_local_offset = local_offset;
            break;
        }
    }
    if (mod0_segment == NsoSegmentCount) {
        return 0;
    }

    const std::uint32_t instruction = ReadU32(text, 0);
    if ((instruction & 0xFC000000) == 0x14000000) { // B imm26
        const std::int32_t immediate = static_cast<std::int32_t>(instruction << 6) >> 6;
        const std::int64_t target = static_cast<std::int64_t>(immediate) * 4;
        if (target < 8 || !RangeFits(static_cast<std::uint64_t>(target), 4, text.size())) {
            return 0;
        }
        if (mod0_segment == static_cast<std::size_t>(NsoSegmentId::Text) &&
            RangesOverlap(static_cast<std::uint64_t>(target), 4, mod0_local_offset,
                          MinimumMod0HeaderSize)) {
            return 0;
        }
        return static_cast<std::uint32_t>(target);
    }

    // Only the first loaded NSO is the process entry point. NintendoSDK leaves
    // word zero reserved in later modules (normally `main`, `subsdk*`, and
    // `sdk`) and records their initializer in ELF64 DT_INIT instead. Requiring
    // every module to begin with a branch therefore rejects ordinary retail
    // multi-NSO applications. Do not guess past arbitrary non-branch words:
    // accept precisely the reserved-zero form and validate its bounded,
    // terminated dynamic table before trusting DT_INIT.
    if (instruction != 0) {
        return 0;
    }

    constexpr std::uint64_t Elf64DynamicEntrySize = 0x10;
    constexpr std::uint64_t MaximumDynamicEntries = 4096;
    constexpr std::uint64_t DtNull = 0;
    constexpr std::uint64_t DtInit = 12;

    const auto read_module_u64 = [&](std::uint64_t address, std::size_t field_offset,
                                     std::uint64_t& value) {
        for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
            if (address < memory_offsets[i]) {
                continue;
            }
            const std::uint64_t local_offset = address - memory_offsets[i];
            if (RangeFits(local_offset, Elf64DynamicEntrySize, segments[i].size())) {
                value = ReadU64(segments[i], static_cast<std::size_t>(local_offset) + field_offset);
                return true;
            }
        }
        return false;
    };

    const std::uint32_t dynamic_offset =
        ReadU32(segments[mod0_segment], static_cast<std::size_t>(mod0_local_offset) + 4);
    if (dynamic_offset < MinimumMod0HeaderSize ||
        dynamic_offset > std::numeric_limits<std::uint64_t>::max() - mod0_address) {
        return 0;
    }
    const std::uint64_t dynamic_address = mod0_address + dynamic_offset;
    if ((dynamic_address & 7) != 0) {
        return 0;
    }

    std::uint64_t init_address = 0;
    bool found_init = false;
    for (std::uint64_t index = 0; index < MaximumDynamicEntries; ++index) {
        if (index > (std::numeric_limits<std::uint64_t>::max() - dynamic_address) /
                        Elf64DynamicEntrySize) {
            return 0;
        }
        const std::uint64_t entry_address = dynamic_address + index * Elf64DynamicEntrySize;
        std::uint64_t tag = 0;
        std::uint64_t value = 0;
        if (!read_module_u64(entry_address, 0, tag) ||
            !read_module_u64(entry_address, sizeof(std::uint64_t), value)) {
            return 0;
        }
        if (tag == DtNull) {
            if (!found_init || init_address < memory_offsets[0]) {
                return 0;
            }
            const std::uint64_t target = init_address - memory_offsets[0];
            if ((target & 3) != 0 || !RangeFits(target, 4, text.size())) {
                return 0;
            }
            if (mod0_segment == static_cast<std::size_t>(NsoSegmentId::Text) &&
                RangesOverlap(target, 4, mod0_local_offset, MinimumMod0HeaderSize)) {
                return 0;
            }
            return static_cast<std::uint32_t>(target);
        }
        if (tag == DtInit) {
            if (found_init && init_address != value) {
                return 0;
            }
            init_address = value;
            found_init = true;
        }
    }
    return 0;
}

} // namespace

NsoInspection InspectNso(std::span<const std::uint8_t> file) {
    NsoInspection result;
    if (file.size() < NsoHeaderSize) {
        result.error = "file is smaller than the 0x100-byte NSO0 header";
        return result;
    }
    if (file[0] != 'N' || file[1] != 'S' || file[2] != 'O' || file[3] != '0') {
        result.error = "invalid NSO0 magic";
        return result;
    }

    NsoInfo info;
    info.version = ReadU32(file, 0x04);
    info.reserved = ReadU32(file, 0x08);
    info.flags = ReadU32(file, 0x0C);
    info.execute_only = (info.flags & (1U << 6)) != 0;
    info.uses_zbic = (info.flags & (1U << 7)) != 0;
    std::copy_n(file.begin() + 0x40, info.build_id.size(), info.build_id.begin());

    info.module_name_offset = ReadU32(file, 0x1C);
    info.module_name_size = ReadU32(file, 0x2C);
    info.bss_size = ReadU32(file, 0x3C);
    info.api_info = {ReadU32(file, 0x88), ReadU32(file, 0x8C)};
    info.dynstr = {ReadU32(file, 0x90), ReadU32(file, 0x94)};
    info.dynsym = {ReadU32(file, 0x98), ReadU32(file, 0x9C)};

    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        const std::size_t header_offset = 0x10 + i * 0x10;
        NsoSegmentInfo& segment = info.segments[i];
        segment.file_offset = ReadU32(file, header_offset);
        segment.memory_offset = ReadU32(file, header_offset + 4);
        segment.decoded_size = ReadU32(file, header_offset + 8);
        segment.extra = ReadU32(file, header_offset + 12);
        segment.compressed_size_field = ReadU32(file, 0x60 + i * 4);
        const bool compressed = (info.flags & (1U << i)) != 0;
        segment.compression = compressed
                                  ? (info.uses_zbic ? NsoCompression::Zbic : NsoCompression::Lz4)
                                  : NsoCompression::None;
        // The compressed-size fields are not authoritative for uncompressed segments.
        segment.stored_size = compressed ? segment.compressed_size_field : segment.decoded_size;
        segment.hash_required = (info.flags & (1U << (i + 3))) != 0;
        std::copy_n(file.begin() + 0xA0 + i * 0x20, segment.expected_hash.size(),
                    segment.expected_hash.begin());

        if (segment.file_offset > file.size()) {
            result.error = SegmentError(i, "file offset lies beyond the end of the file");
            return result;
        }
        if (segment.decoded_size == 0 && segment.stored_size != 0) {
            result.error = SegmentError(i, "has stored bytes but a zero decoded size");
            return result;
        }
        if (segment.decoded_size != 0 && segment.stored_size == 0) {
            result.error = SegmentError(i, "has a nonzero decoded size but no stored bytes");
            return result;
        }
        if (segment.stored_size != 0) {
            if (segment.file_offset < NsoHeaderSize) {
                result.error = SegmentError(i, "overlaps the NSO0 header");
                return result;
            }
            if (!RangeFits(segment.file_offset, segment.stored_size, file.size())) {
                result.error = SegmentError(i, "extends beyond the end of the file");
                return result;
            }
        }
        constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32;
        if (!RangeFits(segment.memory_offset, segment.decoded_size, AddressSpaceSize)) {
            result.error = SegmentError(i, "memory range overflows the 32-bit module image");
            return result;
        }
    }

    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        for (std::size_t j = i + 1; j < NsoSegmentCount; ++j) {
            const auto& first = info.segments[i];
            const auto& second = info.segments[j];
            if (RangesOverlap(first.file_offset, first.stored_size, second.file_offset,
                              second.stored_size)) {
                result.error = SegmentError(j, "overlaps another segment in the file");
                return result;
            }
            if (RangesOverlap(first.memory_offset, first.decoded_size, second.memory_offset,
                              second.decoded_size)) {
                result.error = SegmentError(j, "overlaps another segment in memory");
                return result;
            }
        }
    }

    // A size of zero or one denotes no module name in existing NSO producers/loaders.
    if (info.module_name_size > 1) {
        if (info.module_name_offset < NsoHeaderSize ||
            !RangeFits(info.module_name_offset, info.module_name_size, file.size())) {
            result.error = "module-name range lies outside the file";
            return result;
        }
        for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
            const auto& segment = info.segments[i];
            if (RangesOverlap(info.module_name_offset, info.module_name_size, segment.file_offset,
                              segment.stored_size)) {
                result.error = "module-name range overlaps an NSO segment";
                return result;
            }
        }
    }

    const std::uint64_t data_end =
        static_cast<std::uint64_t>(info.segments[2].memory_offset) + info.segments[2].decoded_size;
    constexpr std::uint64_t AddressSpaceSize = std::uint64_t{1} << 32;
    if (!RangeFits(data_end, info.bss_size, AddressSpaceSize)) {
        result.error = "data and BSS ranges overflow the 32-bit module image";
        return result;
    }
    const std::uint64_t data_and_bss_size =
        static_cast<std::uint64_t>(info.segments[2].decoded_size) + info.bss_size;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& segment = info.segments[i];
        if (RangesOverlap(info.segments[2].memory_offset, data_and_bss_size, segment.memory_offset,
                          segment.decoded_size)) {
            result.error = "data/BSS memory range overlaps another segment";
            return result;
        }
    }

    if (!ValidateRoExtent(info.api_info, info.segments[1].decoded_size, "API-info", result.error) ||
        !ValidateRoExtent(info.dynstr, info.segments[1].decoded_size, "dynstr", result.error) ||
        !ValidateRoExtent(info.dynsym, info.segments[1].decoded_size, "dynsym", result.error)) {
        return result;
    }

    if (info.version != 0) {
        result.warnings.emplace_back("unrecognized nonzero NSO0 version");
    }
    if (info.reserved != 0) {
        result.warnings.emplace_back("reserved header word is nonzero");
    }
    if ((info.flags & ~KnownFlagsMask) != 0) {
        result.warnings.emplace_back("header contains unknown flag bits");
    }
    if (std::all_of(info.build_id.begin(), info.build_id.end(),
                    [](std::uint8_t value) { return value == 0; })) {
        result.warnings.emplace_back("build ID is all zeroes");
    }
    if (info.segments[0].memory_offset > info.segments[1].memory_offset ||
        info.segments[1].memory_offset > info.segments[2].memory_offset) {
        result.warnings.emplace_back("segment memory offsets are not in text/rodata/data order");
    }

    result.info = std::move(info);
    return result;
}

std::string ValidateNsoExecutableLayout(const NsoInfo& info) {
    constexpr std::uint32_t PageMask = 0xFFF;
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        if ((info.segments[i].memory_offset & PageMask) != 0) {
            return SegmentError(i, "memory offset is not 0x1000-aligned");
        }
    }

    const std::uint64_t text_end =
        static_cast<std::uint64_t>(info.segments[0].memory_offset) + info.segments[0].decoded_size;
    if (text_end > info.segments[1].memory_offset) {
        return "text segment ends after the rodata segment begins";
    }
    const std::uint64_t rodata_end =
        static_cast<std::uint64_t>(info.segments[1].memory_offset) + info.segments[1].decoded_size;
    if (rodata_end > info.segments[2].memory_offset) {
        return "rodata segment ends after the data segment begins";
    }
    return {};
}

NsoDecodeResult DecodeNso(std::span<const std::uint8_t> file, NsoDecompressor lz4_decompressor,
                          std::uint64_t max_decoded_bytes, NsoSha256Hasher sha256_hasher) {
    NsoDecodeResult result;
    NsoInspection inspection = InspectNso(file);
    result.warnings = std::move(inspection.warnings);
    if (!inspection) {
        result.error = std::move(inspection.error);
        return result;
    }

    DecodedNso decoded;
    decoded.info = std::move(*inspection.info);
    std::uint64_t total_decoded_bytes = 0;
    bool requires_hash_verification = false;
    for (const NsoSegmentInfo& segment : decoded.info.segments) {
        total_decoded_bytes += segment.decoded_size;
        requires_hash_verification |= segment.hash_required;
    }
    if (total_decoded_bytes > max_decoded_bytes ||
        total_decoded_bytes > std::numeric_limits<std::size_t>::max()) {
        std::ostringstream error;
        error << "decoded segments require " << total_decoded_bytes
              << " bytes, exceeding the configured limit of " << max_decoded_bytes;
        result.error = error.str();
        return result;
    }
    decoded.required_hashes_verified = !requires_hash_verification;
    if (requires_hash_verification && sha256_hasher == nullptr) {
        result.warnings.emplace_back(
            "required segment hashes were not verified by this decoder build");
    }

    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        const NsoSegmentInfo& segment = decoded.info.segments[i];
        std::vector<std::uint8_t>& destination = decoded.segments[i];
        if (segment.decoded_size != 0) {
            const auto source = file.subspan(segment.file_offset, segment.stored_size);
            try {
                destination.resize(segment.decoded_size);
            } catch (const std::bad_alloc&) {
                result.error = SegmentError(i, "could not allocate its decoded buffer");
                return result;
            } catch (const std::length_error&) {
                result.error = SegmentError(i, "decoded size is unsupported on this host");
                return result;
            }

            switch (segment.compression) {
            case NsoCompression::None:
                std::copy(source.begin(), source.end(), destination.begin());
                break;
            case NsoCompression::Lz4:
                if (lz4_decompressor == nullptr) {
                    result.error = SegmentError(i, "requires an LZ4 decompressor");
                    return result;
                }
                if (!lz4_decompressor(source, destination)) {
                    result.error = SegmentError(i, "could not be decompressed with LZ4");
                    return result;
                }
                break;
            case NsoCompression::Zbic:
                result.error = SegmentError(i, "uses unsupported ZBIC compression");
                return result;
            }
        }

        if (segment.hash_required && sha256_hasher != nullptr) {
            std::array<std::uint8_t, 0x20> actual_hash{};
            if (!sha256_hasher(std::span<const std::uint8_t>{destination}, actual_hash)) {
                result.error = SegmentError(i, "SHA-256 hash could not be computed");
                return result;
            }
            if (actual_hash != segment.expected_hash) {
                result.error = SegmentError(i, "SHA-256 hash does not match the NSO0 header");
                return result;
            }
        }
    }

    if (requires_hash_verification && sha256_hasher != nullptr) {
        decoded.required_hashes_verified = true;
    }

    result.image = std::move(decoded);
    return result;
}

std::uint32_t FindNsoAarch64EntryOffset(std::span<const std::uint8_t> text) {
    const std::array<std::uint32_t, NsoSegmentCount> memory_offsets{};
    const std::array<std::span<const std::uint8_t>, NsoSegmentCount> segments{
        text, std::span<const std::uint8_t>{}, std::span<const std::uint8_t>{}};
    return FindNsoAarch64EntryOffsetImpl(text, memory_offsets, segments);
}

std::uint32_t FindNsoAarch64EntryOffset(const DecodedNso& image) {
    std::array<std::uint32_t, NsoSegmentCount> memory_offsets{};
    std::array<std::span<const std::uint8_t>, NsoSegmentCount> segments{};
    for (std::size_t i = 0; i < NsoSegmentCount; ++i) {
        memory_offsets[i] = image.info.segments[i].memory_offset;
        segments[i] = image.segments[i];
    }
    return FindNsoAarch64EntryOffsetImpl(segments[static_cast<std::size_t>(NsoSegmentId::Text)],
                                         memory_offsets, segments);
}

std::string NsoBuildIdToHex(const std::array<std::uint8_t, 0x20>& build_id) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string result(build_id.size() * 2, '0');
    for (std::size_t i = 0; i < build_id.size(); ++i) {
        result[i * 2] = Digits[build_id[i] >> 4];
        result[i * 2 + 1] = Digits[build_id[i] & 0xF];
    }
    return result;
}

const char* NsoSegmentName(NsoSegmentId segment) {
    switch (segment) {
    case NsoSegmentId::Text:
        return "text";
    case NsoSegmentId::RoData:
        return "rodata";
    case NsoSegmentId::Data:
        return "data";
    }
    return "unknown";
}

const char* NsoCompressionName(NsoCompression compression) {
    switch (compression) {
    case NsoCompression::None:
        return "none";
    case NsoCompression::Lz4:
        return "lz4";
    case NsoCompression::Zbic:
        return "zbic";
    }
    return "unknown";
}

} // namespace suyu::recomp
