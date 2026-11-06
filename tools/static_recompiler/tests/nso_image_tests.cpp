// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/npdm_info.h"
#include "core/recompiler/nso_image.h"
#include "nso_sha256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Byte = std::uint8_t;

void PutU16(std::vector<Byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<Byte>(value);
    bytes[offset + 1] = static_cast<Byte>(value >> 8);
}

void PutU32(std::vector<Byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<Byte>(value);
    bytes[offset + 1] = static_cast<Byte>(value >> 8);
    bytes[offset + 2] = static_cast<Byte>(value >> 16);
    bytes[offset + 3] = static_cast<Byte>(value >> 24);
}

void PutU64(std::vector<Byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
    }
}

std::vector<Byte> MakeNso() {
    std::vector<Byte> bytes(0x120);
    std::copy_n(reinterpret_cast<const Byte*>("NSO0"), 4, bytes.begin());

    // Three adjacent, uncompressed segments. Deliberately put nonsense in the
    // compressed-size fields: the format says those fields are only authoritative
    // when the matching flag is set.
    PutU32(bytes, 0x10, 0x100);
    PutU32(bytes, 0x14, 0x1000);
    PutU32(bytes, 0x18, 0x10);
    PutU32(bytes, 0x20, 0x110);
    PutU32(bytes, 0x24, 0x2000);
    PutU32(bytes, 0x28, 0x08);
    PutU32(bytes, 0x30, 0x118);
    PutU32(bytes, 0x34, 0x3000);
    PutU32(bytes, 0x38, 0x08);
    PutU32(bytes, 0x3C, 0x20);
    PutU32(bytes, 0x60, 1);
    PutU32(bytes, 0x64, 2);
    PutU32(bytes, 0x68, 3);

    for (std::size_t i = 0; i < 0x20; ++i) {
        bytes[0x40 + i] = static_cast<Byte>(i);
    }
    const Byte text[] = {0xA0, 0x00, 0x80, 0xD2, 0xE1, 0x00, 0x80, 0xD2,
                         0x02, 0x00, 0x01, 0x8B, 0x01, 0x00, 0x00, 0xD4};
    std::copy(std::begin(text), std::end(text), bytes.begin() + 0x100);
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[0x110 + i] = static_cast<Byte>(0x40 + i);
        bytes[0x118 + i] = static_cast<Byte>(0x80 + i);
    }
    return bytes;
}

std::vector<Byte> MakeLz4Nso() {
    const std::vector<Byte> uncompressed = MakeNso();
    std::vector<Byte> bytes(0x122);
    std::copy_n(uncompressed.begin(), 0x100, bytes.begin());
    PutU32(bytes, 0x0C, 1); // LZ4-compressed text only
    PutU32(bytes, 0x60, 18);
    PutU32(bytes, 0x20, 0x112);
    PutU32(bytes, 0x30, 0x11A);

    // A valid raw LZ4 block containing one 16-byte literal run and no match.
    bytes[0x100] = 0xF0;
    bytes[0x101] = 1;
    std::copy_n(uncompressed.begin() + 0x100, 16, bytes.begin() + 0x102);
    std::copy_n(uncompressed.begin() + 0x110, 8, bytes.begin() + 0x112);
    std::copy_n(uncompressed.begin() + 0x118, 8, bytes.begin() + 0x11A);
    return bytes;
}

std::vector<Byte> MakeEmittableNso() {
    constexpr std::size_t TextSize = 0x68;
    std::vector<Byte> bytes(0x178);
    std::copy_n(reinterpret_cast<const Byte*>("NSO0"), 4, bytes.begin());
    PutU32(bytes, 0x10, 0x100);
    PutU32(bytes, 0x14, 0x1000);
    PutU32(bytes, 0x18, TextSize);
    PutU32(bytes, 0x20, 0x168);
    PutU32(bytes, 0x24, 0x3000);
    PutU32(bytes, 0x28, 8);
    PutU32(bytes, 0x30, 0x170);
    PutU32(bytes, 0x34, 0x5000);
    PutU32(bytes, 0x38, 8);
    PutU32(bytes, 0x3C, 0x10);
    for (std::size_t i = 0; i < 0x20; ++i) {
        bytes[0x40 + i] = static_cast<Byte>(0x80 + i);
    }

    // A conventional NSO entry stub and a minimum-sized MOD0 header.
    PutU32(bytes, 0x100, 0x14000010); // b text+0x40
    PutU32(bytes, 0x104, 8);
    std::copy_n(reinterpret_cast<const Byte*>("MOD0"), 4, bytes.begin() + 0x108);

    // Load 5 from rodata and 7 from data, add them, then prove that the zeroed
    // BSS tail is mapped by storing and reloading 9 at data+8. The standalone SVC
    // diagnostic prints x0-x3.
    constexpr std::array<std::uint32_t, 10> EntryInstructions{
        0xD0000000, // adrp x0, rodata@0x3000
        0xF9400000, // ldr  x0, [x0]
        0x90000021, // adrp x1, data@0x5000
        0xF9400021, // ldr  x1, [x1]
        0x8B010002, // add  x2, x0, x1
        0x90000023, // adrp x3, data@0x5000
        0xD2800124, // mov  x4, #9
        0xF9000464, // str  x4, [x3, #8] (BSS)
        0xF9400463, // ldr  x3, [x3, #8]
        0xD4000001, // svc  #0
    };
    for (std::size_t i = 0; i < EntryInstructions.size(); ++i) {
        PutU32(bytes, 0x140 + i * 4, EntryInstructions[i]);
    }
    bytes[0x168] = 5;
    bytes[0x170] = 7;
    return bytes;
}

std::vector<Byte> MakeDynamicNso() {
    constexpr std::uint32_t TextFile = 0x100;
    constexpr std::uint32_t TextAddress = 0x1000;
    constexpr std::uint32_t TextSize = 0x20;
    constexpr std::uint32_t RoDataFile = TextFile + TextSize;
    constexpr std::uint32_t RoDataAddress = 0x6000;
    constexpr std::uint32_t RoDataSize = 0x200;
    constexpr std::uint32_t DataFile = RoDataFile + RoDataSize;
    constexpr std::uint32_t DataAddress = 0x8000;
    constexpr std::uint32_t DataSize = 8;
    constexpr std::uint32_t DynamicAddress = RoDataAddress + 0x20;
    constexpr std::uint32_t RelaAddress = RoDataAddress + 0x100;
    constexpr std::uint32_t PltRelaAddress = RelaAddress + 0x18;
    constexpr std::uint32_t SymbolTableAddress = RoDataAddress + 0x180;
    constexpr std::uint32_t StringTableAddress = RoDataAddress + 0x1E0;
    constexpr std::string_view DynamicStrings{"\0imported\0exported\0absolute\0", 28};

    std::vector<Byte> bytes(DataFile + DataSize);
    std::copy_n(reinterpret_cast<const Byte*>("NSO0"), 4, bytes.begin());
    PutU32(bytes, 0x10, TextFile);
    PutU32(bytes, 0x14, TextAddress);
    PutU32(bytes, 0x18, TextSize);
    PutU32(bytes, 0x20, RoDataFile);
    PutU32(bytes, 0x24, RoDataAddress);
    PutU32(bytes, 0x28, RoDataSize);
    PutU32(bytes, 0x30, DataFile);
    PutU32(bytes, 0x34, DataAddress);
    PutU32(bytes, 0x38, DataSize);
    PutU32(bytes, 0x3C, 0x20); // BSS
    PutU32(bytes, 0x60, TextSize);
    PutU32(bytes, 0x64, RoDataSize);
    PutU32(bytes, 0x68, DataSize);
    PutU32(bytes, 0x90, StringTableAddress - RoDataAddress);
    PutU32(bytes, 0x94, static_cast<std::uint32_t>(DynamicStrings.size()));
    PutU32(bytes, 0x98, SymbolTableAddress - RoDataAddress);
    PutU32(bytes, 0x9C, 4 * 24);
    for (std::size_t i = 0; i < 0x20; ++i) {
        bytes[0x40 + i] = static_cast<Byte>(0x40 + i);
    }

    // A normal non-entry NintendoSDK module: word zero is reserved, MOD0 is
    // still located through text+4, and its module initializer comes from
    // ELF64 DT_INIT rather than a process-entry branch.
    PutU32(bytes, TextFile, 0);
    PutU32(bytes, TextFile + 4, RoDataAddress - TextAddress);
    PutU32(bytes, TextFile + 0x10, 0xD503201F); // nop (DT_INIT)
    PutU32(bytes, TextFile + 0x14, 0xD65F03C0); // ret (exported function)

    const auto ro_file_offset = [](std::uint32_t address) {
        return static_cast<std::size_t>(RoDataFile + address - RoDataAddress);
    };
    std::copy_n(reinterpret_cast<const Byte*>("MOD0"), 4,
                bytes.begin() + ro_file_offset(RoDataAddress));
    PutU32(bytes, ro_file_offset(RoDataAddress) + 4, DynamicAddress - RoDataAddress);
    PutU32(bytes, ro_file_offset(RoDataAddress) + 8, DataAddress - RoDataAddress + DataSize);
    PutU32(bytes, ro_file_offset(RoDataAddress) + 12,
           DataAddress - RoDataAddress + DataSize + 0x20);
    PutU32(bytes, ro_file_offset(RoDataAddress) + 24, DataAddress - RoDataAddress);

    const auto put_dynamic = [&](std::size_t index, std::int64_t tag, std::uint64_t value) {
        const std::size_t offset = ro_file_offset(DynamicAddress) + index * 16;
        PutU64(bytes, offset, static_cast<std::uint64_t>(tag));
        PutU64(bytes, offset + 8, value);
    };
    put_dynamic(0, 7, RelaAddress);            // DT_RELA
    put_dynamic(1, 8, 24);                     // DT_RELASZ
    put_dynamic(2, 9, 24);                     // DT_RELAENT
    put_dynamic(3, 23, PltRelaAddress);        // DT_JMPREL
    put_dynamic(4, 2, 24);                     // DT_PLTRELSZ
    put_dynamic(5, 20, 7);                     // DT_PLTREL = DT_RELA
    put_dynamic(6, 6, SymbolTableAddress);     // DT_SYMTAB
    put_dynamic(7, 11, 24);                    // DT_SYMENT
    put_dynamic(8, 5, StringTableAddress);     // DT_STRTAB
    put_dynamic(9, 10, DynamicStrings.size()); // DT_STRSZ
    put_dynamic(10, 12, TextAddress + 0x10);   // DT_INIT
    put_dynamic(11, 0, 0);                     // DT_NULL

    const auto put_rela = [&](std::uint32_t address, std::uint64_t target, std::uint32_t symbol,
                              std::uint32_t type, std::int64_t addend) {
        const std::size_t offset = ro_file_offset(address);
        PutU64(bytes, offset, target);
        PutU64(bytes, offset + 8, (static_cast<std::uint64_t>(symbol) << 32) | type);
        PutU64(bytes, offset + 16, static_cast<std::uint64_t>(addend));
    };
    put_rela(RelaAddress, DataAddress, 0, 0x403, TextAddress + 0x10);
    put_rela(PltRelaAddress, DataAddress + DataSize, 1, 0x402, 0);

    const std::size_t symbol_table = ro_file_offset(SymbolTableAddress);
    // Symbol zero remains the required all-zero null entry.
    PutU32(bytes, symbol_table + 24, 1);
    bytes[symbol_table + 24 + 4] = 0x22; // STB_WEAK | STT_FUNC
    PutU32(bytes, symbol_table + 48, 10);
    bytes[symbol_table + 48 + 4] = 0x12; // STB_GLOBAL | STT_FUNC
    PutU16(bytes, symbol_table + 48 + 6, 1);
    PutU64(bytes, symbol_table + 48 + 8, TextAddress + 0x14);
    PutU64(bytes, symbol_table + 48 + 16, 4);
    PutU32(bytes, symbol_table + 72, 19);
    bytes[symbol_table + 72 + 4] = 0x11;          // STB_GLOBAL | STT_OBJECT
    PutU16(bytes, symbol_table + 72 + 6, 0xFFF1); // SHN_ABS
    PutU64(bytes, symbol_table + 72 + 16, 8);
    std::copy(DynamicStrings.begin(), DynamicStrings.end(),
              bytes.begin() + ro_file_offset(StringTableAddress));

    return bytes;
}

std::vector<Byte> MakeNpdm(bool aarch64, std::uint8_t address_space = 3) {
    constexpr std::uint64_t ProgramId = 0x0100000000010000ULL;
    std::vector<Byte> bytes(0x300);
    std::copy_n(reinterpret_cast<const Byte*>("META"), 4, bytes.begin());
    bytes[0x0C] = static_cast<Byte>((aarch64 ? 1 : 0) | (address_space << 1));
    bytes[0x0E] = 0x2C;
    PutU32(bytes, 0x1C, 0x100000);
    PutU32(bytes, 0x70, 0x2C0);
    PutU32(bytes, 0x74, 0x40);
    PutU32(bytes, 0x78, 0x80);
    PutU32(bytes, 0x7C, 0x240);

    std::copy_n(reinterpret_cast<const Byte*>("ACID"), 4, bytes.begin() + 0x280);
    for (const std::size_t offset : {0x2A0, 0x2A8, 0x2B0}) {
        PutU32(bytes, offset, 0x240);
    }
    std::copy_n(reinterpret_cast<const Byte*>("ACI0"), 4, bytes.begin() + 0x2C0);
    PutU64(bytes, 0x2D0, ProgramId);
    for (const std::size_t offset : {0x2E0, 0x2E8, 0x2F0}) {
        PutU32(bytes, offset, 0x40);
    }
    return bytes;
}

bool IdentityDecompress(std::span<const Byte> source, std::span<Byte> destination) {
    if (source.size() != destination.size()) {
        return false;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    return true;
}

bool FixtureLz4Decompress(std::span<const Byte> source, std::span<Byte> destination) {
    if (source.size() != destination.size() + 2 || source[0] != 0xF0 || source[1] != 1) {
        return false;
    }
    std::copy(source.begin() + 2, source.end(), destination.begin());
    return true;
}

std::array<Byte, 0x20> MakeTestDigest(std::span<const Byte> source) {
    std::array<Byte, 0x20> digest{};
    std::uint32_t state = 2166136261U;
    for (const Byte value : source) {
        state ^= value;
        state *= 16777619U;
    }
    state ^= static_cast<std::uint32_t>(source.size());
    for (std::size_t i = 0; i < digest.size(); ++i) {
        state ^= state >> 13;
        state *= 0x5BD1E995U;
        digest[i] = static_cast<Byte>(state >> ((i & 3) * 8));
    }
    return digest;
}

void PutTestDigest(std::vector<Byte>& bytes, std::size_t segment,
                   std::span<const Byte> decoded) {
    const auto digest = MakeTestDigest(decoded);
    std::copy(digest.begin(), digest.end(), bytes.begin() + 0xA0 + segment * digest.size());
}

std::size_t hash_call_count = 0;
std::array<std::size_t, suyu::recomp::NsoSegmentCount> hashed_sizes{};

void ResetHashCalls() {
    hash_call_count = 0;
    hashed_sizes.fill(0);
}

bool TestSha256(std::span<const Byte> source, std::array<Byte, 0x20>& digest) {
    if (hash_call_count < hashed_sizes.size()) {
        hashed_sizes[hash_call_count] = source.size();
    }
    ++hash_call_count;
    digest = MakeTestDigest(source);
    return true;
}

bool FailingSha256(std::span<const Byte>, std::array<Byte, 0x20>&) {
    return false;
}

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void RunTests() {
    constexpr std::array<Byte, 3> Sha256Input{'a', 'b', 'c'};
    constexpr std::array<Byte, 0x20> ExpectedSha256{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    std::array<Byte, 0x20> sha256_digest{};
    Check(suyu::recomp::tool::ComputeSha256(Sha256Input, sha256_digest) &&
              sha256_digest == ExpectedSha256,
          "bundled SHA-256 backend matches the standard abc vector");

    const std::vector<Byte> valid = MakeNso();
    const auto inspection = suyu::recomp::InspectNso(valid);
    Check(static_cast<bool>(inspection), "valid NSO inspects successfully");
    if (inspection) {
        const auto& info = *inspection.info;
        Check(suyu::recomp::ValidateNsoExecutableLayout(info).empty(),
              "valid NSO has an executable load layout");
        Check(info.segments[0].stored_size == 0x10,
              "uncompressed text uses decoded size rather than compressed-size "
              "field");
        Check(info.segments[1].stored_size == 0x08, "rodata stored size is decoded size");
        Check(info.bss_size == 0x20, "data extra word is exposed as BSS size");
        Check(suyu::recomp::NsoBuildIdToHex(info.build_id) ==
                  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
              "build ID has stable lowercase hexadecimal formatting");
    }

    const auto decoded = suyu::recomp::DecodeNso(valid);
    Check(static_cast<bool>(decoded), "uncompressed NSO decodes without a callback");
    if (decoded) {
        Check(decoded.image->segments[0].size() == 0x10, "decoded text has expected size");
        Check(decoded.image->segments[0][0] == 0xA0, "decoded text has expected contents");
    }

    auto bad_magic = valid;
    bad_magic[0] = 'X';
    Check(!suyu::recomp::InspectNso(bad_magic), "bad magic is rejected");
    Check(!suyu::recomp::InspectNso(std::span<const Byte>{valid}.first(0xFF)),
          "truncated header is rejected");

    auto outside_file = valid;
    PutU32(outside_file, 0x30, 0x1000);
    Check(!suyu::recomp::InspectNso(outside_file), "segment outside file is rejected");

    auto file_overlap = valid;
    PutU32(file_overlap, 0x20, 0x108);
    Check(!suyu::recomp::InspectNso(file_overlap), "overlapping file segments are rejected");

    auto memory_overlap = valid;
    PutU32(memory_overlap, 0x24, 0x1008);
    Check(!suyu::recomp::InspectNso(memory_overlap), "overlapping memory segments are rejected");

    auto misaligned_layout = valid;
    PutU32(misaligned_layout, 0x14, 0x1004);
    const auto misaligned_inspection = suyu::recomp::InspectNso(misaligned_layout);
    Check(misaligned_inspection &&
              !suyu::recomp::ValidateNsoExecutableLayout(*misaligned_inspection.info).empty(),
          "executable layout rejects a non-page-aligned destination");

    auto reversed_layout = valid;
    PutU32(reversed_layout, 0x24, 0x4000);
    PutU32(reversed_layout, 0x34, 0x3000);
    const auto reversed_inspection = suyu::recomp::InspectNso(reversed_layout);
    Check(reversed_inspection &&
              !suyu::recomp::ValidateNsoExecutableLayout(*reversed_inspection.info).empty(),
          "executable layout rejects reversed rodata and data destinations");

    auto bad_extent = valid;
    PutU32(bad_extent, 0x88, 8);
    PutU32(bad_extent, 0x8C, 1);
    Check(!suyu::recomp::InspectNso(bad_extent), "out-of-range rodata extent is rejected");

    auto empty_extent = valid;
    PutU32(empty_extent, 0x88, 0xFFFFFFFF);
    Check(static_cast<bool>(suyu::recomp::InspectNso(empty_extent)),
          "zero-sized extent ignores its sentinel offset");

    auto empty_module_name = valid;
    PutU32(empty_module_name, 0x1C, 0);
    PutU32(empty_module_name, 0x2C, 1);
    Check(static_cast<bool>(suyu::recomp::InspectNso(empty_module_name)),
          "one-byte module-name sentinel is accepted");

    auto overlapping_module_name = valid;
    PutU32(overlapping_module_name, 0x1C, 0x100);
    PutU32(overlapping_module_name, 0x2C, 4);
    Check(!suyu::recomp::InspectNso(overlapping_module_name),
          "module-name range may not overlap a segment");

    auto overlapping_bss = valid;
    PutU32(overlapping_bss, 0x34, 0x1FF0);
    Check(!suyu::recomp::InspectNso(overlapping_bss), "data BSS tail may not overlap rodata");

    auto zero_length_bad_offset = valid;
    PutU32(zero_length_bad_offset, 0x28, 0);
    PutU32(zero_length_bad_offset, 0x20, 0xFFFFFFFF);
    Check(!suyu::recomp::InspectNso(zero_length_bad_offset),
          "zero-length segment offset beyond the file is rejected");

    auto compressed = valid;
    PutU32(compressed, 0x0C, 1);
    PutU32(compressed, 0x60, 0x10);
    const auto missing_decompressor = suyu::recomp::DecodeNso(compressed);
    Check(!missing_decompressor &&
              missing_decompressor.error.find("requires an LZ4") != std::string::npos,
          "compressed segment clearly requests an LZ4 backend");
    Check(static_cast<bool>(suyu::recomp::DecodeNso(compressed, IdentityDecompress)),
          "injected decompressor decodes compressed segment");

    auto compressed_without_bytes = compressed;
    PutU32(compressed_without_bytes, 0x60, 0);
    Check(!suyu::recomp::InspectNso(compressed_without_bytes),
          "nonempty compressed segment with no stored bytes is rejected");

    auto zbic = compressed;
    PutU32(zbic, 0x0C, 1U | (1U << 7));
    const auto zbic_result = suyu::recomp::DecodeNso(zbic, IdentityDecompress);
    Check(!zbic_result && zbic_result.error.find("ZBIC") != std::string::npos,
          "ZBIC is recognized and rejected explicitly");

    auto size_bomb = valid;
    PutU32(size_bomb, 0x0C, 1);
    PutU32(size_bomb, 0x14, 0);
    PutU32(size_bomb, 0x18, 0x40000001);
    PutU32(size_bomb, 0x60, 1);
    PutU32(size_bomb, 0x20, 0);
    PutU32(size_bomb, 0x24, 0);
    PutU32(size_bomb, 0x28, 0);
    PutU32(size_bomb, 0x30, 0);
    PutU32(size_bomb, 0x34, 0);
    PutU32(size_bomb, 0x38, 0);
    PutU32(size_bomb, 0x3C, 0);
    const auto bomb_result = suyu::recomp::DecodeNso(size_bomb, IdentityDecompress);
    Check(!bomb_result && bomb_result.error.find("configured limit") != std::string::npos,
          "decoded-size bomb is rejected before allocation");

    std::vector<Byte> fake_mod0(0x40);
    PutU32(fake_mod0, 4, 8);
    PutU32(fake_mod0, 0, 0x14000009); // b +0x24
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0,
          "entry heuristic rejects a missing MOD0 magic");
    fake_mod0[8] = 'M';
    fake_mod0[9] = 'O';
    fake_mod0[10] = 'D';
    fake_mod0[11] = '0';
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0x24,
          "entry probe accepts a bounded MOD0 header and AArch64 branch stub");
    PutU32(fake_mod0, 0, 0x14000002); // b text+8, into MOD0 metadata
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0,
          "entry probe rejects a branch into the MOD0 header");

    const auto library_module = suyu::recomp::DecodeNso(MakeDynamicNso());
    Check(library_module &&
              suyu::recomp::FindNsoAarch64EntryOffset(*library_module.image) == 0x10,
          "entry probe accepts a reserved-zero library module with bounded DT_INIT");
    if (library_module) {
        auto missing_init = *library_module.image;
        PutU64(missing_init.segments[1], 0x20 + 10 * 16, 0); // DT_NULL replaces DT_INIT
        Check(suyu::recomp::FindNsoAarch64EntryOffset(missing_init) == 0,
              "reserved-zero module requires DT_INIT before DT_NULL");

        auto invalid_init = *library_module.image;
        PutU64(invalid_init.segments[1], 0x20 + 10 * 16 + 8, 0x1001);
        Check(suyu::recomp::FindNsoAarch64EntryOffset(invalid_init) == 0,
              "reserved-zero module rejects a misaligned DT_INIT target");
    }

    suyu::recomp::DecodedNso split_mod0{};
    split_mod0.info.segments[0].memory_offset = 0x1000;
    split_mod0.info.segments[1].memory_offset = 0x3000;
    split_mod0.info.segments[2].memory_offset = 0x4000;
    split_mod0.segments[0].resize(0x10);
    split_mod0.segments[1].resize(0x1C);
    PutU32(split_mod0.segments[0], 0, 0x14000002); // b text+8
    PutU32(split_mod0.segments[0], 4, 0x2000);
    std::copy_n(reinterpret_cast<const Byte*>("MOD0"), 4, split_mod0.segments[1].begin());
    Check(suyu::recomp::FindNsoAarch64EntryOffset(split_mod0) == 8,
          "entry probe resolves a module-relative MOD0 pointer into rodata");

    // HOS 19+ extends MOD0 from 0x1c to 0x34 bytes. Nonzero extension fields must
    // not be mistaken for code; the entry-stub branch is the authoritative
    // target.
    PutU32(fake_mod0, 0, 0x1400000F); // b +0x3c
    for (std::size_t offset = 0x24; offset < 0x3C; offset += 4) {
        PutU32(fake_mod0, offset, static_cast<std::uint32_t>(offset));
    }
    PutU32(fake_mod0, 0x3C, 0xD4000001);
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0x3C,
          "entry probe follows the branch over an extended MOD0 header");

    std::vector<Byte> truncated_mod0(0x0C);
    PutU32(truncated_mod0, 0, 0x14000002);
    PutU32(truncated_mod0, 4, 8);
    truncated_mod0[8] = 'M';
    truncated_mod0[9] = 'O';
    truncated_mod0[10] = 'D';
    truncated_mod0[11] = '0';
    Check(suyu::recomp::FindNsoAarch64EntryOffset(truncated_mod0) == 0,
          "entry probe rejects a truncated MOD0 header");

    fake_mod0.resize(0x3E);
    Check(suyu::recomp::FindNsoAarch64EntryOffset(fake_mod0) == 0,
          "entry probe requires a complete instruction at the branch target");

    auto hash_required = valid;
    PutU32(hash_required, 0x0C, 1U << 3);
    const auto unverified = suyu::recomp::DecodeNso(hash_required);
    Check(unverified && !unverified.image->required_hashes_verified,
          "hash-required image is marked unverified");
    Check(unverified && std::any_of(unverified.warnings.begin(), unverified.warnings.end(),
                                    [](const std::string& warning) {
                                        return warning.find("not verified") != std::string::npos;
                                    }),
          "hash-required image emits an integrity warning");

    auto verified_hashes = valid;
    PutU32(verified_hashes, 0x0C, (1U << 3) | (1U << 5));
    PutTestDigest(verified_hashes, 0, std::span<const Byte>{valid}.subspan(0x100, 0x10));
    PutTestDigest(verified_hashes, 2, std::span<const Byte>{valid}.subspan(0x118, 0x08));
    ResetHashCalls();
    const auto verified = suyu::recomp::DecodeNso(
        verified_hashes, nullptr, suyu::recomp::DefaultNsoDecodeLimit, TestSha256);
    Check(verified && verified.image->required_hashes_verified,
          "matching required hashes are marked verified");
    Check(verified &&
              std::none_of(verified.warnings.begin(), verified.warnings.end(),
                           [](const std::string& warning) {
                               return warning.find("not verified") != std::string::npos;
                           }),
          "successful hash verification does not emit an integrity warning");
    Check(hash_call_count == 2 && hashed_sizes[0] == 0x10 && hashed_sizes[1] == 0x08,
          "only hash-required nonempty segments are passed to the hash callback");

    auto mismatched_hash = verified_hashes;
    mismatched_hash[0xA0] ^= 0xFF;
    const auto mismatch = suyu::recomp::DecodeNso(
        mismatched_hash, nullptr, suyu::recomp::DefaultNsoDecodeLimit, TestSha256);
    Check(!mismatch && mismatch.error.find("does not match") != std::string::npos,
          "a required segment hash mismatch rejects the image");

    const auto hash_failure = suyu::recomp::DecodeNso(
        verified_hashes, nullptr, suyu::recomp::DefaultNsoDecodeLimit, FailingSha256);
    Check(!hash_failure && hash_failure.error.find("could not be computed") != std::string::npos,
          "a hash-backend failure rejects the image with a clear error");

    auto empty_hash_required = valid;
    PutU32(empty_hash_required, 0x0C, 1U << 4);
    PutU32(empty_hash_required, 0x28, 0);
    PutTestDigest(empty_hash_required, 1, std::span<const Byte>{});
    ResetHashCalls();
    const auto empty_verified = suyu::recomp::DecodeNso(
        empty_hash_required, nullptr, suyu::recomp::DefaultNsoDecodeLimit, TestSha256);
    Check(empty_verified && empty_verified.image->required_hashes_verified,
          "a required empty-segment hash is verified");
    Check(hash_call_count == 1 && hashed_sizes[0] == 0,
          "the hash callback receives required empty segments");

    auto compressed_hash_required = MakeLz4Nso();
    PutU32(compressed_hash_required, 0x0C, 1U | (1U << 3));
    PutTestDigest(compressed_hash_required, 0,
                  std::span<const Byte>{valid}.subspan(0x100, 0x10));
    const auto compressed_verified = suyu::recomp::DecodeNso(
        compressed_hash_required, FixtureLz4Decompress, suyu::recomp::DefaultNsoDecodeLimit,
        TestSha256);
    Check(compressed_verified && compressed_verified.image->required_hashes_verified,
          "required hashes cover decoded rather than compressed segment bytes");

    ResetHashCalls();
    Check(static_cast<bool>(suyu::recomp::DecodeNso(
              valid, nullptr, suyu::recomp::DefaultNsoDecodeLimit, TestSha256)) &&
              hash_call_count == 0,
          "segments without hash-required flags do not invoke the hash callback");

    const std::vector<Byte> npdm64 = MakeNpdm(true);
    const auto npdm64_result = suyu::recomp::InspectNpdm(npdm64);
    Check(static_cast<bool>(npdm64_result), "valid AArch64 NPDM inspects successfully");
    if (npdm64_result) {
        Check(npdm64_result.info->architecture == suyu::recomp::NpdmArchitecture::Aarch64,
              "NPDM bit zero selects AArch64");
        Check(npdm64_result.info->address_space ==
                  suyu::recomp::NpdmAddressSpace::AddressSpace64Bit,
              "NPDM address-space bits are decoded independently");
        Check(npdm64_result.info->program_id == 0x0100000000010000ULL,
              "NPDM ACI0 program ID is decoded");
    }
    const auto npdm32_result = suyu::recomp::InspectNpdm(MakeNpdm(false, 2));
    Check(npdm32_result &&
              npdm32_result.info->architecture == suyu::recomp::NpdmArchitecture::Aarch32,
          "NPDM bit zero selects AArch32");

    auto modern_flags = npdm64;
    modern_flags[0x0C] |= 0xF0;
    Check(static_cast<bool>(suyu::recomp::InspectNpdm(modern_flags)),
          "modern NPDM feature flags are accepted");
    const auto unknown_address_space = suyu::recomp::InspectNpdm(MakeNpdm(true, 4));
    Check(unknown_address_space && !unknown_address_space.info->address_space,
          "unknown NPDM address spaces remain explicit");

    auto bad_npdm = npdm64;
    bad_npdm[0] = 'X';
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "bad META magic is rejected");
    Check(!suyu::recomp::InspectNpdm(std::span<const Byte>{npdm64}.first(0x80)),
          "META-only file cannot supply a trusted architecture");
    bad_npdm = npdm64;
    bad_npdm[0x2C0] = 'X';
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "bad ACI0 magic is rejected");
    bad_npdm = npdm64;
    bad_npdm[0x280] = 'X';
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "bad ACID magic is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x70, 0x40);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "ACI0 may not overlap META");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x74, 0x20);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "undersized ACI0 is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x70, 0x100);
    std::copy_n(reinterpret_cast<const Byte*>("ACI0"), 4, bad_npdm.begin() + 0x100);
    for (const std::size_t offset : {0x120, 0x128, 0x130}) {
        PutU32(bad_npdm, offset, 0x40);
    }
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "overlapping ACI0 and ACID are rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x78, 0xFFFFFFFF);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "overflowing ACID range is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x2A0, 0x23F);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "ACID child before its header is rejected");
    bad_npdm = npdm64;
    PutU32(bad_npdm, 0x2E0, 0x40);
    PutU32(bad_npdm, 0x2E4, 1);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "ACI0 child beyond its region is rejected");
    bad_npdm = npdm64;
    bad_npdm.resize(suyu::recomp::MaximumNpdmSize + 1);
    Check(!suyu::recomp::InspectNpdm(bad_npdm), "oversized NPDM input is rejected");
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<Byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    return output &&
           output.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())) &&
           output.flush().good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view{argv[1]}.starts_with("--write-")) {
        const std::string_view path_bytes = argv[2];
        const auto path = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(path_bytes.data()), path_bytes.size()));
        const std::string_view operation = argv[1];
        std::optional<std::vector<Byte>> fixture;
        if (operation == "--write-fixture") {
            fixture = MakeNso();
        } else if (operation == "--write-lz4-fixture") {
            fixture = MakeLz4Nso();
        } else if (operation == "--write-emittable-fixture") {
            fixture = MakeEmittableNso();
        } else if (operation == "--write-dynamic-fixture") {
            fixture = MakeDynamicNso();
        } else if (operation == "--write-misaligned-emittable-fixture") {
            fixture = MakeEmittableNso();
            PutU32(*fixture, 0x14, 0x1004);
        } else if (operation == "--write-reversed-emittable-fixture") {
            fixture = MakeEmittableNso();
            PutU32(*fixture, 0x24, 0x6000);
        } else if (operation == "--write-npdm") {
            fixture = MakeNpdm(true);
        } else if (operation == "--write-oversized-npdm") {
            fixture = MakeNpdm(true);
            fixture->resize(suyu::recomp::MaximumNpdmSize + 1);
        } else if (operation == "--write-aarch32-npdm") {
            fixture = MakeNpdm(false, 2);
        } else if (operation == "--write-unknown-address-npdm") {
            fixture = MakeNpdm(true, 4);
        }
        if (!fixture) {
            std::cerr << "unknown fixture type\n";
            return 2;
        }
        if (!WriteBytes(path, *fixture)) {
            std::cerr << "could not write synthetic fixture\n";
            return 1;
        }
        return 0;
    }
    if (argc != 1) {
        std::cerr << "unexpected test-helper arguments\n";
        return 2;
    }

    RunTests();
    if (failures != 0) {
        std::cerr << failures << " NSO image test(s) failed\n";
        return 1;
    }
    std::cout << "NSO image tests passed\n";
    return 0;
}
