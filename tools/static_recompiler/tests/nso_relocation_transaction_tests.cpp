// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_relocation_transaction.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Byte = std::uint8_t;

struct MemoryHarness {
    std::uint64_t base{};
    std::vector<Byte> bytes;
    std::vector<std::size_t> failed_write_attempts;
    std::vector<std::uint64_t> write_addresses;
    std::uint64_t failed_read_address{~std::uint64_t{0}};
    std::size_t write_attempts{};

    static bool Read(void* user, std::uint64_t address, std::span<Byte> destination) {
        auto& memory = *static_cast<MemoryHarness*>(user);
        if (address == memory.failed_read_address || address < memory.base) {
            return false;
        }
        const std::uint64_t offset = address - memory.base;
        if (offset > memory.bytes.size() || destination.size() > memory.bytes.size() - offset) {
            return false;
        }
        std::copy_n(memory.bytes.begin() + static_cast<std::size_t>(offset), destination.size(),
                    destination.begin());
        return true;
    }

    static bool Write(void* user, std::uint64_t address, std::span<const Byte> source) {
        auto& memory = *static_cast<MemoryHarness*>(user);
        if (address < memory.base) {
            return false;
        }
        const std::uint64_t offset = address - memory.base;
        if (offset > memory.bytes.size() || source.size() > memory.bytes.size() - offset) {
            return false;
        }
        const std::size_t attempt = memory.write_attempts++;
        memory.write_addresses.push_back(address);
        if (std::find(memory.failed_write_attempts.begin(), memory.failed_write_attempts.end(),
                      attempt) != memory.failed_write_attempts.end()) {
            std::copy_n(source.begin(), source.size() / 2,
                        memory.bytes.begin() + static_cast<std::size_t>(offset));
            return false;
        }
        std::copy(source.begin(), source.end(),
                  memory.bytes.begin() + static_cast<std::size_t>(offset));
        return true;
    }

    suyu::recomp::NsoRelocationMemory Access() {
        return {
            .user = this,
            .read = Read,
            .write = Write,
        };
    }
};

void PutU64(std::vector<Byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<Byte>(value >> (index * 8));
    }
}

std::uint64_t ReadU64(const std::vector<Byte>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

struct Fixture {
    static constexpr std::uint64_t Base = 0x1000;
    static constexpr std::uint64_t ImageSize = 0x200;
    static constexpr std::uint64_t RelaSizeAddress = 0x108;
    static constexpr std::uint64_t PltRelaSizeAddress = 0x118;

    suyu::recomp::NsoDynamicInfo dynamic;
    suyu::recomp::NsoRelocationPlan plan;
    MemoryHarness memory;

    Fixture() {
        plan.module_base = Base;
        plan.module_image_size = ImageSize;
        memory.base = Base;
        memory.bytes.resize(ImageSize);
        dynamic.dynamic_address = 0x100;
        dynamic.dynamic_byte_size = 0x30;
        dynamic.rela_size_value_address = RelaSizeAddress;
        dynamic.plt_rela_size_value_address = PltRelaSizeAddress;
        dynamic.rela = suyu::recomp::NsoRelaTable{
            .kind = suyu::recomp::NsoRelaTableKind::Dynamic,
            .address = 0x40,
            .byte_size = 48,
            .entry_size = 24,
            .records = {{.offset = 0x20,
                         .info = suyu::recomp::NsoAarch64RelocationRelative,
                         .addend = 1,
                         .symbol_index = 0,
                         .type = suyu::recomp::NsoAarch64RelocationRelative},
                        {.offset = 0x28,
                         .info =
                             (std::uint64_t{2} << 32) | suyu::recomp::NsoAarch64RelocationGlobDat,
                         .addend = 2,
                         .symbol_index = 2,
                         .type = suyu::recomp::NsoAarch64RelocationGlobDat}},
        };
        dynamic.plt_rela = suyu::recomp::NsoRelaTable{
            .kind = suyu::recomp::NsoRelaTableKind::ProcedureLinkage,
            .address = 0x70,
            .byte_size = 24,
            .entry_size = 24,
            .records = {{.offset = 0x30,
                         .info =
                             (std::uint64_t{3} << 32) | suyu::recomp::NsoAarch64RelocationJumpSlot,
                         .addend = 0,
                         .symbol_index = 3,
                         .type = suyu::recomp::NsoAarch64RelocationJumpSlot}},
        };
        plan.writes = {
            {.table_kind = suyu::recomp::NsoRelaTableKind::Dynamic,
             .table_index = 0,
             .module_offset = 0x20,
             .address = Base + 0x20,
             .value = 0x1111111111111111,
             .addend = 1,
             .type = suyu::recomp::NsoAarch64RelocationRelative,
             .symbol_index = 0},
            {.table_kind = suyu::recomp::NsoRelaTableKind::Dynamic,
             .table_index = 1,
             .module_offset = 0x28,
             .address = Base + 0x28,
             .value = 0x2222222222222222,
             .addend = 2,
             .type = suyu::recomp::NsoAarch64RelocationGlobDat,
             .symbol_index = 2},
            {.table_kind = suyu::recomp::NsoRelaTableKind::ProcedureLinkage,
             .table_index = 0,
             .module_offset = 0x30,
             .address = Base + 0x30,
             .value = 0x3333333333333333,
             .addend = 0,
             .type = suyu::recomp::NsoAarch64RelocationJumpSlot,
             .symbol_index = 3},
        };
        PutU64(memory.bytes, 0x20, 0xAAAAAAAAAAAAAAAA);
        PutU64(memory.bytes, 0x28, 0xBBBBBBBBBBBBBBBB);
        PutU64(memory.bytes, 0x30, 0xCCCCCCCCCCCCCCCC);
        PutU64(memory.bytes, RelaSizeAddress, dynamic.rela->byte_size);
        PutU64(memory.bytes, PltRelaSizeAddress, dynamic.plt_rela->byte_size);
    }
};

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void RunBasicTests() {
    Fixture fixture;
    const auto committed = suyu::recomp::CommitNsoRelocationPlan(fixture.plan, fixture.dynamic,
                                                                 fixture.memory.Access());
    Check(committed && committed.state == suyu::recomp::NsoRelocationCommitState::Committed,
          "complete relocation transaction commits");
    Check(committed.relocation_writes == 3 && committed.finalization_writes == 2,
          "commit reports relocation and finalization counts separately");
    Check(ReadU64(fixture.memory.bytes, 0x20) == 0x1111111111111111 &&
              ReadU64(fixture.memory.bytes, 0x28) == 0x2222222222222222 &&
              ReadU64(fixture.memory.bytes, 0x30) == 0x3333333333333333,
          "planned values are written in table order");
    Check(ReadU64(fixture.memory.bytes, Fixture::RelaSizeAddress) == 0 &&
              ReadU64(fixture.memory.bytes, Fixture::PltRelaSizeAddress) == 0,
          "dynamic relocation sizes are cleared only after relocation writes");
    Check(fixture.memory.write_addresses.size() == 5 &&
              fixture.memory.write_addresses[3] == Fixture::Base + Fixture::RelaSizeAddress &&
              fixture.memory.write_addresses[4] == Fixture::Base + Fixture::PltRelaSizeAddress,
          "finalization barriers are the transaction's last writes");

    fixture = Fixture{};
    fixture.memory.failed_read_address = Fixture::Base + 0x28;
    const auto unreadable = suyu::recomp::CommitNsoRelocationPlan(fixture.plan, fixture.dynamic,
                                                                  fixture.memory.Access());
    Check(unreadable.state == suyu::recomp::NsoRelocationCommitState::Rejected &&
              fixture.memory.write_attempts == 0,
          "snapshot failure rejects the transaction before any write");

    fixture = Fixture{};
    PutU64(fixture.memory.bytes, Fixture::RelaSizeAddress, 47);
    const auto changed = suyu::recomp::CommitNsoRelocationPlan(fixture.plan, fixture.dynamic,
                                                               fixture.memory.Access());
    Check(changed.state == suyu::recomp::NsoRelocationCommitState::Rejected &&
              changed.error.find("changed") != std::string::npos &&
              fixture.memory.write_attempts == 0,
          "changed relocation metadata is rejected before any write");

    fixture = Fixture{};
    fixture.plan.writes[1].type = suyu::recomp::NsoAarch64RelocationAbs64;
    const auto mismatched = suyu::recomp::CommitNsoRelocationPlan(fixture.plan, fixture.dynamic,
                                                                  fixture.memory.Access());
    Check(mismatched.state == suyu::recomp::NsoRelocationCommitState::Rejected &&
              fixture.memory.write_attempts == 0,
          "plan and dynamic metadata mismatch is rejected before memory access");
}

void RunRollbackTests() {
    Fixture fixture;
    const auto original = fixture.memory.bytes;
    fixture.memory.failed_write_attempts = {1};
    const auto relocation_failure = suyu::recomp::CommitNsoRelocationPlan(
        fixture.plan, fixture.dynamic, fixture.memory.Access());
    Check(relocation_failure.state == suyu::recomp::NsoRelocationCommitState::RolledBack &&
              fixture.memory.bytes == original,
          "partial relocation write failure restores the complete original image");

    fixture = Fixture{};
    const auto barrier_original = fixture.memory.bytes;
    fixture.memory.failed_write_attempts = {3};
    const auto barrier_failure = suyu::recomp::CommitNsoRelocationPlan(
        fixture.plan, fixture.dynamic, fixture.memory.Access());
    Check(barrier_failure.state == suyu::recomp::NsoRelocationCommitState::RolledBack &&
              fixture.memory.bytes == barrier_original,
          "finalization failure restores both relocations and dynamic metadata");

    fixture = Fixture{};
    fixture.memory.failed_write_attempts = {1, 2};
    const auto rollback_failure = suyu::recomp::CommitNsoRelocationPlan(
        fixture.plan, fixture.dynamic, fixture.memory.Access());
    Check(rollback_failure.state == suyu::recomp::NsoRelocationCommitState::RollbackFailed &&
              rollback_failure.error.find("execution is unsafe") != std::string::npos,
          "failed restoration is surfaced as a fatal transaction result");

    fixture = Fixture{};
    fixture.dynamic.rela->records[0].offset = Fixture::RelaSizeAddress;
    fixture.plan.writes[0].module_offset = Fixture::RelaSizeAddress;
    fixture.plan.writes[0].address = Fixture::Base + Fixture::RelaSizeAddress;
    const auto overlap = suyu::recomp::CommitNsoRelocationPlan(fixture.plan, fixture.dynamic,
                                                               fixture.memory.Access());
    Check(overlap.state == suyu::recomp::NsoRelocationCommitState::Rejected &&
              overlap.error.find("overlaps") != std::string::npos &&
              fixture.memory.write_attempts == 0,
          "relocation writes cannot alias the finalization barrier");
}

void RunQlaunchScaleTest() {
    constexpr std::size_t QlaunchWrites = 41'116;
    constexpr std::uint64_t Base = 0x10000000;
    const std::uint64_t destination_bytes = QlaunchWrites * sizeof(std::uint64_t);
    const std::uint64_t rela_address = destination_bytes + 0x100;
    const std::uint64_t rela_bytes = QlaunchWrites * suyu::recomp::NsoElf64RelaEntrySize;
    const std::uint64_t dynamic_address = rela_address + rela_bytes + 0x100;
    const std::uint64_t image_size = dynamic_address + 0x20;

    suyu::recomp::NsoDynamicInfo dynamic;
    dynamic.dynamic_address = dynamic_address;
    dynamic.dynamic_byte_size = 0x20;
    dynamic.rela_size_value_address = dynamic_address + 8;
    dynamic.rela.emplace();
    dynamic.rela->kind = suyu::recomp::NsoRelaTableKind::Dynamic;
    dynamic.rela->address = rela_address;
    dynamic.rela->byte_size = rela_bytes;
    dynamic.rela->entry_size = suyu::recomp::NsoElf64RelaEntrySize;

    suyu::recomp::NsoRelocationPlan plan;
    plan.module_base = Base;
    plan.module_image_size = image_size;
    dynamic.rela->records.reserve(QlaunchWrites);
    plan.writes.reserve(QlaunchWrites);
    for (std::size_t index = 0; index < QlaunchWrites; ++index) {
        const std::uint64_t offset = index * sizeof(std::uint64_t);
        dynamic.rela->records.push_back({
            .offset = offset,
            .info = suyu::recomp::NsoAarch64RelocationRelative,
            .addend = static_cast<std::int64_t>(index),
            .symbol_index = 0,
            .type = suyu::recomp::NsoAarch64RelocationRelative,
        });
        plan.writes.push_back({
            .table_kind = suyu::recomp::NsoRelaTableKind::Dynamic,
            .table_index = index,
            .module_offset = offset,
            .address = Base + offset,
            .value = Base + index,
            .addend = static_cast<std::int64_t>(index),
            .type = suyu::recomp::NsoAarch64RelocationRelative,
            .symbol_index = 0,
        });
    }

    MemoryHarness memory;
    memory.base = Base;
    memory.bytes.resize(image_size);
    PutU64(memory.bytes, static_cast<std::size_t>(*dynamic.rela_size_value_address),
           dynamic.rela->byte_size);
    const auto committed = suyu::recomp::CommitNsoRelocationPlan(plan, dynamic, memory.Access());
    Check(committed && committed.relocation_writes == QlaunchWrites &&
              committed.finalization_writes == 1,
          "qlaunch-scale transaction commits exactly 41,116 relocation writes");
    Check(ReadU64(memory.bytes, 0) == Base &&
              ReadU64(memory.bytes, (QlaunchWrites - 1) * sizeof(std::uint64_t)) ==
                  Base + QlaunchWrites - 1 &&
              ReadU64(memory.bytes, static_cast<std::size_t>(*dynamic.rela_size_value_address)) ==
                  0,
          "qlaunch-scale transaction writes both endpoints before finalization");
}

} // namespace

int main() {
    RunBasicTests();
    RunRollbackTests();
    RunQlaunchScaleTest();
    if (failures != 0) {
        std::cerr << failures << " NSO relocation transaction test(s) failed\n";
        return 1;
    }
    std::cout << "NSO relocation transaction tests passed\n";
    return 0;
}
