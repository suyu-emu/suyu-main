// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/npdm_info.h"

#include <cstdint>
#include <string>
#include <utility>

namespace suyu::recomp {
namespace {

constexpr std::size_t MinimumAciSize = 0x40;
constexpr std::size_t MinimumAcidSize = 0x240;

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(ReadU32(bytes, offset)) |
           (static_cast<std::uint64_t>(ReadU32(bytes, offset + 4)) << 32);
}

bool HasMagic(std::span<const std::uint8_t> bytes, std::size_t offset, const char (&magic)[5]) {
    return offset <= bytes.size() && 4 <= bytes.size() - offset && bytes[offset] == magic[0] &&
           bytes[offset + 1] == magic[1] && bytes[offset + 2] == magic[2] &&
           bytes[offset + 3] == magic[3];
}

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool RangesOverlap(std::uint64_t first_offset, std::uint64_t first_size,
                   std::uint64_t second_offset, std::uint64_t second_size) {
    return first_size != 0 && second_size != 0 && first_offset < second_offset + second_size &&
           second_offset < first_offset + first_size;
}

bool ValidateRegion(std::span<const std::uint8_t> file, std::uint32_t offset, std::uint32_t size,
                    std::size_t minimum_size, const char* name, std::string& error) {
    if (offset < NpdmMetaHeaderSize) {
        error = std::string{name} + " region overlaps the META header";
        return false;
    }
    if (size < minimum_size) {
        error = std::string{name} + " region is smaller than its fixed header";
        return false;
    }
    if (!RangeFits(offset, size, file.size())) {
        error = std::string{name} + " region extends beyond the end of the file";
        return false;
    }
    return true;
}

bool ValidateSubregion(std::span<const std::uint8_t> file, std::uint32_t region_offset,
                       std::uint32_t region_size, std::size_t fields_offset,
                       std::size_t fixed_header_size, const char* name, std::string& error) {
    const std::size_t fields = static_cast<std::size_t>(region_offset) + fields_offset;
    const std::uint32_t offset = ReadU32(file, fields);
    const std::uint32_t size = ReadU32(file, fields + 4);
    if (offset < fixed_header_size || !RangeFits(offset, size, region_size)) {
        error = std::string{name} + " subregion lies outside its component";
        return false;
    }
    return true;
}

} // namespace

NpdmInspection InspectNpdm(std::span<const std::uint8_t> file) {
    NpdmInspection result;
    if (file.size() < NpdmMetaHeaderSize) {
        result.error = "file is smaller than the 0x80-byte META header";
        return result;
    }
    if (file.size() > MaximumNpdmSize) {
        result.error = "file exceeds the 0x8000-byte NPDM limit";
        return result;
    }
    if (!HasMagic(file, 0, "META")) {
        result.error = "invalid META magic";
        return result;
    }

    NpdmInfo info;
    info.flags = file[0x0C];
    info.architecture =
        (info.flags & 1) != 0 ? NpdmArchitecture::Aarch64 : NpdmArchitecture::Aarch32;
    info.address_space_value = static_cast<std::uint8_t>((info.flags >> 1) & 7);
    if (info.address_space_value <=
        static_cast<std::uint8_t>(NpdmAddressSpace::AddressSpace64Bit)) {
        info.address_space = static_cast<NpdmAddressSpace>(info.address_space_value);
    }
    info.optimize_memory_allocation = (info.flags & (1U << 4)) != 0;
    info.disable_device_address_space_merge = (info.flags & (1U << 5)) != 0;
    info.enable_alias_region_extra_size = (info.flags & (1U << 6)) != 0;
    info.prevent_code_reads = (info.flags & (1U << 7)) != 0;
    info.main_thread_priority = file[0x0E];
    info.main_thread_core = file[0x0F];
    info.version = ReadU32(file, 0x18);
    info.main_thread_stack_size = ReadU32(file, 0x1C);
    info.aci_offset = ReadU32(file, 0x70);
    info.aci_size = ReadU32(file, 0x74);
    info.acid_offset = ReadU32(file, 0x78);
    info.acid_size = ReadU32(file, 0x7C);

    if (!ValidateRegion(file, info.aci_offset, info.aci_size, MinimumAciSize, "ACI0",
                        result.error) ||
        !ValidateRegion(file, info.acid_offset, info.acid_size, MinimumAcidSize, "ACID",
                        result.error)) {
        return result;
    }
    if (RangesOverlap(info.aci_offset, info.aci_size, info.acid_offset, info.acid_size)) {
        result.error = "ACI0 and ACID regions overlap";
        return result;
    }
    if (!HasMagic(file, info.aci_offset, "ACI0")) {
        result.error = "invalid ACI0 magic";
        return result;
    }
    info.program_id = ReadU64(file, static_cast<std::size_t>(info.aci_offset) + 0x10);
    if (!HasMagic(file, static_cast<std::size_t>(info.acid_offset) + 0x200, "ACID")) {
        result.error = "invalid ACID magic";
        return result;
    }
    for (const auto& [offset, name] : {std::pair{std::size_t{0x20}, "ACI0 file-access"},
                                      std::pair{std::size_t{0x28}, "ACI0 service-access"},
                                      std::pair{std::size_t{0x30}, "ACI0 kernel-access"}}) {
        if (!ValidateSubregion(file, info.aci_offset, info.aci_size, offset, MinimumAciSize, name,
                               result.error)) {
            return result;
        }
    }
    for (const auto& [offset, name] : {std::pair{std::size_t{0x220}, "ACID file-access"},
                                      std::pair{std::size_t{0x228}, "ACID service-access"},
                                      std::pair{std::size_t{0x230}, "ACID kernel-access"}}) {
        if (!ValidateSubregion(file, info.acid_offset, info.acid_size, offset, MinimumAcidSize,
                               name, result.error)) {
            return result;
        }
    }

    if (!info.address_space) {
        result.warnings.emplace_back("NPDM uses an unknown process address-space value");
    }
    if (info.main_thread_priority > 0x3F) {
        result.warnings.emplace_back("NPDM main-thread priority is outside the documented range");
    }
    if (info.main_thread_stack_size != 0 && (info.main_thread_stack_size & 0xFFF) != 0) {
        result.warnings.emplace_back("NPDM main-thread stack size is not page-aligned");
    }

    result.info = info;
    return result;
}

const char* NpdmArchitectureName(NpdmArchitecture architecture) {
    switch (architecture) {
    case NpdmArchitecture::Aarch32:
        return "AArch32";
    case NpdmArchitecture::Aarch64:
        return "AArch64";
    }
    return "unknown";
}

const char* NpdmAddressSpaceName(NpdmAddressSpace address_space) {
    switch (address_space) {
    case NpdmAddressSpace::AddressSpace32Bit:
        return "32-bit";
    case NpdmAddressSpace::AddressSpace64BitOld:
        return "64-bit (36-bit address space)";
    case NpdmAddressSpace::AddressSpace32BitNoReserved:
        return "32-bit (no reserved region)";
    case NpdmAddressSpace::AddressSpace64Bit:
        return "64-bit (39-bit address space)";
    }
    return "unknown";
}

} // namespace suyu::recomp
