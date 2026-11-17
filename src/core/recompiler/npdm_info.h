// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace suyu::recomp {

constexpr std::size_t NpdmMetaHeaderSize = 0x80;
constexpr std::size_t MaximumNpdmSize = 0x8000;

enum class NpdmArchitecture {
    Aarch32,
    Aarch64,
};

enum class NpdmAddressSpace : std::uint8_t {
    AddressSpace32Bit = 0,
    AddressSpace64BitOld = 1,
    AddressSpace32BitNoReserved = 2,
    AddressSpace64Bit = 3,
};

struct NpdmInfo {
    std::uint8_t flags{};
    NpdmArchitecture architecture{NpdmArchitecture::Aarch32};
    std::uint8_t address_space_value{};
    std::optional<NpdmAddressSpace> address_space;
    bool optimize_memory_allocation{};
    bool disable_device_address_space_merge{};
    bool enable_alias_region_extra_size{};
    bool prevent_code_reads{};
    std::uint8_t main_thread_priority{};
    std::uint8_t main_thread_core{};
    std::uint32_t version{};
    std::uint32_t main_thread_stack_size{};
    std::uint32_t aci_offset{};
    std::uint32_t aci_size{};
    std::uint32_t acid_offset{};
    std::uint32_t acid_size{};
    std::uint64_t program_id{};
};

struct NpdmInspection {
    std::optional<NpdmInfo> info;
    std::vector<std::string> warnings;
    std::string error;

    explicit operator bool() const {
        return info.has_value() && error.empty();
    }
};

/// Parses the META header and validates the top-level ACI0 and ACID regions before exposing the
/// architecture and ACI0 program ID. Signature and permission verification are outside this
/// lightweight probe.
NpdmInspection InspectNpdm(std::span<const std::uint8_t> file);

const char* NpdmArchitectureName(NpdmArchitecture architecture);
const char* NpdmAddressSpaceName(NpdmAddressSpace address_space);

} // namespace suyu::recomp
