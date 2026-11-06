// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_dynamic.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Byte = std::uint8_t;

constexpr std::uint64_t DynamicAddress = 0x1020;

struct ModuleBytes {
  std::vector<Byte> bytes;
  std::uint64_t unreadable_start{};
  std::uint64_t unreadable_end{};
};

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
  return offset <= limit && size <= limit - offset;
}

void PutU32(std::vector<Byte> &bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
  }
}

void PutU64(std::vector<Byte> &bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
  }
}

void PutI64(std::vector<Byte> &bytes, std::size_t offset, std::int64_t value) {
  PutU64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

void PutDynamicEntry(ModuleBytes &module, std::size_t index, std::int64_t tag,
                     std::uint64_t value) {
  const std::size_t offset =
      static_cast<std::size_t>(DynamicAddress) + index * 16;
  PutI64(module.bytes, offset, tag);
  PutU64(module.bytes, offset + 8, value);
}

void PutRela(ModuleBytes &module, std::size_t offset, std::uint64_t target,
             std::uint32_t symbol, std::uint32_t type, std::int64_t addend) {
  PutU64(module.bytes, offset, target);
  PutU64(module.bytes, offset + 8,
         (static_cast<std::uint64_t>(symbol) << 32) | type);
  PutI64(module.bytes, offset + 16, addend);
}

bool ReadModuleBytes(const void *user, std::uint64_t address,
                     std::span<Byte> destination) {
  const auto &module = *static_cast<const ModuleBytes *>(user);
  if (!RangeFits(address, destination.size(), module.bytes.size())) {
    return false;
  }
  const std::uint64_t end = address + destination.size();
  if (destination.size() != 0 &&
      module.unreadable_start < module.unreadable_end &&
      address < module.unreadable_end && module.unreadable_start < end) {
    return false;
  }
  std::copy_n(module.bytes.begin() + static_cast<std::size_t>(address),
              destination.size(), destination.begin());
  return true;
}

ModuleBytes MakeModule() {
  ModuleBytes module{std::vector<Byte>(0x3000)};
  PutU32(module.bytes, 4, 0x1000);
  std::copy_n(reinterpret_cast<const Byte *>("MOD0"), 4,
              module.bytes.begin() + 0x1000);
  PutU32(module.bytes, 0x1004, 0x20);
  PutU32(module.bytes, 0x1008, 0x2800);
  PutU32(module.bytes, 0x100C, 0x2900);
  PutU32(module.bytes, 0x1010, 0x300);
  PutU32(module.bytes, 0x1014, 0x380);
  PutU32(module.bytes, 0x1018, 0x2800);

  PutDynamicEntry(module, 0, 7, 0x2000);          // DT_RELA
  PutDynamicEntry(module, 1, 8, 48);              // DT_RELASZ
  PutDynamicEntry(module, 2, 9, 24);              // DT_RELAENT
  PutDynamicEntry(module, 3, 23, 0x2040);         // DT_JMPREL
  PutDynamicEntry(module, 4, 2, 24);              // DT_PLTRELSZ
  PutDynamicEntry(module, 5, 20, 7);              // DT_PLTREL = DT_RELA
  PutDynamicEntry(module, 6, 6, 0x2100);          // DT_SYMTAB
  PutDynamicEntry(module, 7, 11, 24);             // DT_SYMENT
  PutDynamicEntry(module, 8, 5, 0x2200);          // DT_STRTAB
  PutDynamicEntry(module, 9, 10, 0x20);           // DT_STRSZ
  PutDynamicEntry(module, 10, 0x60000001, 0xABC); // Retained unknown tag
  PutDynamicEntry(module, 11, 0, 0);              // DT_NULL

  PutRela(module, 0x2000, 0x2810, 0, 0x403, 0x44);
  PutRela(module, 0x2018, 0x2820, 3, 0x101, -8);
  PutRela(module, 0x2040, 0x2830, 4, 0x402, 0);
  module.bytes[0x2200] = 0;
  return module;
}

suyu::recomp::NsoModuleView MakeView(const ModuleBytes &module) {
  return {
      .image_size = module.bytes.size(),
      .text_address = 0,
      .user = &module,
      .read = ReadModuleBytes,
  };
}

suyu::recomp::DecodedNso MakeDecodedNso(const ModuleBytes &module) {
  suyu::recomp::DecodedNso image;
  constexpr std::array<std::uint32_t, 3> Addresses{0, 0x1000, 0x2000};
  constexpr std::array<std::uint32_t, 3> Sizes{0x100, 0x300, 0x300};
  for (std::size_t i = 0; i < Addresses.size(); ++i) {
    image.info.segments[i].memory_offset = Addresses[i];
    image.info.segments[i].decoded_size = Sizes[i];
    image.segments[i].assign(module.bytes.begin() + Addresses[i],
                             module.bytes.begin() + Addresses[i] + Sizes[i]);
  }
  image.info.bss_size = 0xD00;
  return image;
}

int failures = 0;

void Check(bool condition, std::string_view description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

void CheckError(const suyu::recomp::NsoDynamicParseResult &result,
                std::string_view text, std::string_view description) {
  Check(!result && result.error.find(text) != std::string::npos, description);
}

void RunSuccessTests() {
  const ModuleBytes module = MakeModule();
  const auto parsed = suyu::recomp::ParseNsoDynamic(MakeView(module));
  Check(static_cast<bool>(parsed), "valid MOD0 and RELA metadata parses");
  if (!parsed) {
    return;
  }
  const auto &info = *parsed.info;
  Check(info.mod0.address == 0x1000 && info.mod0.dynamic_offset == 0x20,
        "MOD0 is located through the text+4 offset");
  Check(info.mod0.bss_start_offset == 0x2800 &&
            info.mod0.bss_end_offset == 0x2900 &&
            info.mod0.exception_info_start_offset == 0x300 &&
            info.mod0.exception_info_end_offset == 0x380 &&
            info.mod0.module_object_offset == 0x2800,
        "minimum MOD0 fields are exposed");
  Check(info.dynamic_address == DynamicAddress &&
            info.dynamic_byte_size == 12 * 16,
        "terminated dynamic-table extent is exposed");
  Check(info.entries.size() == 11 && info.entries.back().tag == 0x60000001 &&
            info.entries.back().value == 0xABC &&
            info.entries.front().address == DynamicAddress &&
            info.entries.front().value_address == DynamicAddress + 8,
        "unknown dynamic tags are retained and DT_NULL is excluded");
  Check(info.rela_size_value_address == DynamicAddress + 16 + 8 &&
            info.plt_rela_size_value_address == DynamicAddress + 4 * 16 + 8,
        "dynamic relocation-size value fields retain their module-relative "
        "addresses");
  Check(info.string_table_address == 0x2200 && info.string_table_size == 0x20 &&
            info.symbol_table_address == 0x2100 &&
            info.symbol_entry_size == 24 && info.rela_entry_size == 24 &&
            info.plt_relocation_tag == 7,
        "validated dynamic addresses, extents, and entry sizes are exposed");

  Check(info.rela &&
            info.rela->kind == suyu::recomp::NsoRelaTableKind::Dynamic &&
            info.rela->address == 0x2000 && info.rela->byte_size == 48 &&
            info.rela->records.size() == 2,
        "DT_RELA extent and count are exposed");
  if (info.rela && info.rela->records.size() == 2) {
    Check(info.rela->records[0].offset == 0x2810 &&
              info.rela->records[0].type == 0x403 &&
              info.rela->records[0].symbol_index == 0 &&
              info.rela->records[0].addend == 0x44,
          "relative relocation fields are decoded");
    Check(info.rela->records[1].type == 0x101 &&
              info.rela->records[1].symbol_index == 3 &&
              info.rela->records[1].addend == -8,
          "symbol index, type, and signed addend are split from ELF64 RELA");
  }
  Check(info.plt_rela &&
            info.plt_rela->kind ==
                suyu::recomp::NsoRelaTableKind::ProcedureLinkage &&
            info.plt_rela->records.size() == 1 &&
            info.plt_rela->records[0].type == 0x402 &&
            info.plt_rela->records[0].symbol_index == 4,
        "RELA-format procedure-linkage relocations are exposed separately");
  Check(std::string_view{suyu::recomp::NsoRelaTableKindName(
            suyu::recomp::NsoRelaTableKind::ProcedureLinkage)} ==
            "procedure-linkage",
        "relocation-table kind has stable display text");

  const auto decoded = MakeDecodedNso(module);
  const auto decoded_parsed = suyu::recomp::ParseNsoDynamic(decoded);
  Check(decoded_parsed && decoded_parsed.info->rela &&
            decoded_parsed.info->rela->records.size() == 2,
        "DecodedNso adapter resolves MOD0 and tables across separate segments");

  auto shifted_text = module;
  PutU32(shifted_text.bytes, 0x104, 0xF00);
  auto shifted_view = MakeView(shifted_text);
  shifted_view.text_address = 0x100;
  const auto shifted_parsed = suyu::recomp::ParseNsoDynamic(shifted_view);
  Check(shifted_parsed && shifted_parsed.info->mod0.address == 0x1000,
        "MOD0 offsets are resolved relative to a nonzero text address");

  auto near_end_32 = module;
  PutRela(near_end_32, 0x2000, 0x2FFF, 0, 0x102, 0);
  Check(
      static_cast<bool>(suyu::recomp::ParseNsoDynamic(MakeView(near_end_32))),
      "metadata parsing defers a known 32-bit relocation's write-extent check");

  auto near_end_64 = module;
  PutRela(near_end_64, 0x2000, 0x2FFF, 0, 0x101, 0);
  Check(
      static_cast<bool>(suyu::recomp::ParseNsoDynamic(MakeView(near_end_64))),
      "metadata parsing defers a known 64-bit relocation's write-extent check");

  auto empty_rela = module;
  PutDynamicEntry(empty_rela, 1, 8, 0);
  const auto empty_parsed = suyu::recomp::ParseNsoDynamic(MakeView(empty_rela));
  Check(empty_parsed && empty_parsed.info->rela &&
            empty_parsed.info->rela->records.empty(),
        "a present zero-sized RELA table is exposed as empty");

  auto rel_metadata = module;
  PutDynamicEntry(rel_metadata, 10, 17, 0x2300);
  const auto rel_warning =
      suyu::recomp::ParseNsoDynamic(MakeView(rel_metadata));
  Check(rel_warning && rel_warning.warnings.size() == 1 &&
            rel_warning.warnings[0].find("DT_REL") != std::string::npos,
        "out-of-scope REL metadata is retained with an explicit warning");
}

void RunFailureTests() {
  ModuleBytes module = MakeModule();
  auto view = MakeView(module);
  view.read = nullptr;
  CheckError(suyu::recomp::ParseNsoDynamic(view), "no read callback",
             "missing module reader is rejected");

  view = MakeView(module);
  view.image_size = 0x1008;
  CheckError(
      suyu::recomp::ParseNsoDynamic(view), "outside readable",
      "the half-open module image bound is enforced before callback reads");

  auto malformed = module;
  PutU32(malformed.bytes, 4, 0x1002);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "offset at text+4", "misaligned MOD0 offset is rejected");

  malformed = module;
  malformed.bytes[0x1000] = 'X';
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)), "MOD0 magic",
             "bad MOD0 magic is rejected");

  malformed = module;
  PutU32(malformed.bytes, 0x1004, 0x10);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "overlaps its minimum",
             "dynamic table may not overlap the minimum MOD0 header");

  malformed = module;
  PutU32(malformed.bytes, 0x1004, 0x1C);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "not 8-byte aligned", "misaligned dynamic table is rejected");

  view = MakeView(module);
  view.image_size = DynamicAddress + 16;
  CheckError(suyu::recomp::ParseNsoDynamic(view), "not DT_NULL-terminated",
             "unterminated dynamic table is rejected at the module bound");
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(module), 1), "entry limit",
             "dynamic entry limit is enforced");

  malformed = module;
  PutDynamicEntry(malformed, 1, 7, 0x2000);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "duplicate DT_RELA",
             "duplicate singleton dynamic tags are rejected");

  malformed = module;
  PutDynamicEntry(malformed, 2, 9, 16);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)), "DT_RELAENT",
             "non-ELF64 RELA entry size is rejected");

  malformed = module;
  PutDynamicEntry(malformed, 7, 11, 16);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)), "DT_SYMENT",
             "non-ELF64 symbol entry size is rejected");

  malformed = module;
  PutDynamicEntry(malformed, 1, 0x60000002, 48);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "without DT_RELASZ", "RELA address without size is rejected");

  malformed = module;
  PutDynamicEntry(malformed, 0, 0x60000002, 0x2000);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "without DT_RELA",
             "nonzero RELA size without address is rejected");

  malformed = module;
  PutDynamicEntry(malformed, 1, 8, 25);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "not a multiple", "partial RELA record is rejected");

  malformed = module;
  PutDynamicEntry(malformed, 0, 7, 0x2FF0);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "outside the module image",
             "out-of-range RELA extent is rejected");

  malformed = module;
  malformed.unreadable_start = 0x2018;
  malformed.unreadable_end = 0x2030;
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "unreadable bytes",
             "reader-rejected bytes inside a RELA extent are rejected");

  malformed = module;
  PutRela(malformed, 0x2000, 0x2814, 0, 0x102, 0);
  Check(
      static_cast<bool>(suyu::recomp::ParseNsoDynamic(MakeView(malformed))),
      "parser retains a 32-bit relocation at a four-byte-aligned destination");

  malformed = module;
  PutU64(malformed.bytes, 0x2000, 0x3000);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "outside the module image",
             "RELA destination extent must fit inside the module image");

  malformed = module;
  PutDynamicEntry(malformed, 0, 7, DynamicAddress);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "overlaps the dynamic table",
             "RELA extent may not overlap the dynamic table");

  malformed = module;
  PutDynamicEntry(malformed, 3, 23, 0x2018);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "table extents overlap",
             "dynamic and procedure-linkage RELA extents may not overlap");

  malformed = module;
  PutDynamicEntry(malformed, 5, 20, 17);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "unsupported DT_REL",
             "REL-format procedure-linkage table is rejected explicitly");

  CheckError(
      suyu::recomp::ParseNsoDynamic(
          MakeView(module), suyu::recomp::DefaultNsoDynamicEntryLimit, 2),
      "relocation-entry limit", "combined RELA entry limit is enforced");

  malformed = module;
  PutDynamicEntry(malformed, 6, 0x60000002, 0x2100);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "DT_SYMTAB is absent",
             "symbol-indexed relocation requires DT_SYMTAB");

  malformed = module;
  PutRela(malformed, 0x2018, 0x2820, 0xFFFFFFFFU, 0x101, 0);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)), "symbol index",
             "out-of-range relocation symbol index is rejected");

  malformed = module;
  PutDynamicEntry(malformed, 9, 0x60000002, 0x20);
  CheckError(suyu::recomp::ParseNsoDynamic(MakeView(malformed)),
             "without DT_STRSZ",
             "string-table address without extent is rejected");

  auto decoded = MakeDecodedNso(module);
  decoded.segments[1].pop_back();
  CheckError(suyu::recomp::ParseNsoDynamic(decoded),
             "buffer size does not match",
             "DecodedNso adapter rejects inconsistent segment metadata");

  decoded = MakeDecodedNso(module);
  decoded.info.segments[1].memory_offset = 0;
  CheckError(suyu::recomp::ParseNsoDynamic(decoded), "ranges overlap",
             "DecodedNso adapter rejects overlapping segment memory ranges");

  decoded = MakeDecodedNso(module);
  decoded.info.segments[1].memory_offset = 0x2000;
  decoded.info.segments[2].memory_offset = 0x1000;
  decoded.info.bss_size = 0xD01;
  CheckError(suyu::recomp::ParseNsoDynamic(decoded), "data/BSS range overlaps",
             "DecodedNso adapter rejects BSS overlapping another segment");

  decoded = MakeDecodedNso(module);
  decoded.info.segments[1].memory_offset = 0x1001;
  CheckError(suyu::recomp::ParseNsoDynamic(decoded), "not 0x1000-aligned",
             "DecodedNso adapter rejects a misaligned executable layout");

  decoded = MakeDecodedNso(module);
  decoded.info.segments[1].memory_offset = 0x2000;
  decoded.info.segments[2].memory_offset = 0x1000;
  decoded.info.bss_size = 0;
  CheckError(suyu::recomp::ParseNsoDynamic(decoded),
             "rodata segment ends after",
             "DecodedNso adapter rejects reversed rodata and data segments");

  decoded = MakeDecodedNso(module);
  decoded.info.segments[2].memory_offset = 0xFFFFF000U;
  decoded.info.segments[2].decoded_size = 0x2000;
  decoded.segments[2].resize(0x2000);
  CheckError(
      suyu::recomp::ParseNsoDynamic(decoded),
      "overflows the 32-bit module image",
      "DecodedNso adapter rejects segment ranges past the address space");

  auto string_table_gap = module;
  PutDynamicEntry(string_table_gap, 8, 5, 0x1800);
  decoded = MakeDecodedNso(string_table_gap);
  CheckError(
      suyu::recomp::ParseNsoDynamic(decoded), "unreadable decoded NSO bytes",
      "DecodedNso adapter rejects a string table extent in a segment gap");
}

} // namespace

int main() {
  RunSuccessTests();
  RunFailureTests();
  if (failures != 0) {
    std::cerr << failures << " NSO dynamic metadata test(s) failed\n";
    return 1;
  }
  std::cout << "NSO dynamic metadata tests passed\n";
  return 0;
}
