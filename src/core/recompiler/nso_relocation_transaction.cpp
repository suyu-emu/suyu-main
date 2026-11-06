// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/nso_relocation_transaction.h"

#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace suyu::recomp {
namespace {

constexpr std::uint64_t ModuleAddressSpaceSize = std::uint64_t{1} << 32;
constexpr std::size_t WriteSize = sizeof(std::uint64_t);

enum class WriteKind {
    Relocation,
    RelaSize,
    PltRelaSize,
};

struct WriteAction {
    std::uint64_t address{};
    std::array<std::uint8_t, WriteSize> replacement{};
    std::array<std::uint8_t, WriteSize> original{};
    WriteKind kind{WriteKind::Relocation};
    std::uint64_t table_index{};
};

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

bool AddAddress(std::uint64_t base, std::uint64_t offset, std::uint64_t& result) {
    if (offset > std::numeric_limits<std::uint64_t>::max() - base) {
        return false;
    }
    result = base + offset;
    return true;
}

bool IsNoneRelocation(std::uint32_t type) {
    return type == NsoAarch64RelocationNone || type == NsoAarch64RelocationWithdrawnNone;
}

void EncodeU64(std::uint64_t value, std::array<std::uint8_t, WriteSize>& bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint64_t DecodeU64(const std::array<std::uint8_t, WriteSize>& bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return value;
}

bool Read(const NsoRelocationMemory& memory, std::uint64_t address,
          std::array<std::uint8_t, WriteSize>& bytes) {
    try {
        return memory.read(memory.user, address, bytes);
    } catch (...) {
        return false;
    }
}

bool Write(const NsoRelocationMemory& memory, const WriteAction& action, bool restore) {
    const std::span<const std::uint8_t> bytes = restore ? action.original : action.replacement;
    try {
        return memory.write(memory.user, action.address, bytes);
    } catch (...) {
        return false;
    }
}

std::string ActionName(const WriteAction& action) {
    switch (action.kind) {
    case WriteKind::Relocation:
        return "relocation " + std::to_string(action.table_index);
    case WriteKind::RelaSize:
        return "DT_RELASZ finalization";
    case WriteKind::PltRelaSize:
        return "DT_PLTRELSZ finalization";
    }
    return "unknown relocation operation";
}

bool ValidateTable(const NsoRelaTable& table, NsoRelaTableKind expected_kind, std::string& error) {
    if (table.kind != expected_kind) {
        error = std::string{NsoRelaTableKindName(expected_kind)} +
                " RELA table has the wrong table kind";
        return false;
    }
    if (table.entry_size != NsoElf64RelaEntrySize ||
        table.records.size() > std::numeric_limits<std::uint64_t>::max() / table.entry_size ||
        table.byte_size != table.records.size() * table.entry_size) {
        error = std::string{NsoRelaTableKindName(table.kind)} +
                " RELA table metadata does not match its decoded records";
        return false;
    }
    return true;
}

bool ValidatePlanTable(const std::optional<NsoRelaTable>& optional_table,
                       NsoRelaTableKind expected_kind, const NsoRelocationPlan& plan,
                       std::size_t& plan_index, std::string& error) {
    if (!optional_table) {
        return true;
    }
    const NsoRelaTable& table = *optional_table;
    if (!ValidateTable(table, expected_kind, error)) {
        return false;
    }
    for (std::size_t table_index = 0; table_index < table.records.size(); ++table_index) {
        const NsoRelaRecord& record = table.records[table_index];
        if (IsNoneRelocation(record.type)) {
            continue;
        }
        if (plan_index == plan.writes.size()) {
            error = "relocation plan has fewer writes than its dynamic metadata";
            return false;
        }
        const NsoPlannedRelocation& write = plan.writes[plan_index++];
        if (write.table_kind != table.kind || write.table_index != table_index ||
            write.module_offset != record.offset || write.addend != record.addend ||
            write.type != record.type || write.symbol_index != record.symbol_index) {
            error = "relocation plan does not match its dynamic metadata";
            return false;
        }
    }
    return true;
}

bool ValidatePlan(const NsoRelocationPlan& plan, const NsoDynamicInfo& dynamic,
                  std::string& error) {
    if (plan.module_image_size == 0 || plan.module_image_size > ModuleAddressSpaceSize ||
        plan.module_image_size - 1 > std::numeric_limits<std::uint64_t>::max() - plan.module_base) {
        error = "relocation plan has an invalid module mapping";
        return false;
    }
    std::size_t plan_index = 0;
    if (!ValidatePlanTable(dynamic.rela, NsoRelaTableKind::Dynamic, plan, plan_index, error) ||
        !ValidatePlanTable(dynamic.plt_rela, NsoRelaTableKind::ProcedureLinkage, plan, plan_index,
                           error)) {
        return false;
    }
    if (plan_index != plan.writes.size()) {
        error = "relocation plan has more writes than its dynamic metadata";
        return false;
    }
    for (const NsoPlannedRelocation& write : plan.writes) {
        std::uint64_t expected_address = 0;
        if ((write.module_offset & (WriteSize - 1)) != 0 ||
            !RangeFits(write.module_offset, WriteSize, plan.module_image_size) ||
            !AddAddress(plan.module_base, write.module_offset, expected_address) ||
            write.address != expected_address || (write.address & (WriteSize - 1)) != 0) {
            error = "relocation plan contains an invalid destination mapping";
            return false;
        }
    }
    return true;
}

bool AddFinalizationAction(const NsoRelocationPlan& plan, const NsoDynamicInfo& dynamic,
                           const std::optional<NsoRelaTable>& table,
                           const std::optional<std::uint64_t>& value_address, WriteKind kind,
                           std::vector<WriteAction>& actions, std::string& error) {
    if (!table || table->byte_size == 0) {
        return true;
    }
    const char* tag = kind == WriteKind::RelaSize ? "DT_RELASZ" : "DT_PLTRELSZ";
    if (!value_address) {
        error = std::string{tag} + " value address is absent for a nonempty RELA table";
        return false;
    }
    if (*value_address < dynamic.dynamic_address ||
        !RangeFits(*value_address - dynamic.dynamic_address, WriteSize,
                   dynamic.dynamic_byte_size)) {
        error = std::string{tag} + " value field lies outside the parsed dynamic table";
        return false;
    }
    std::uint64_t address = 0;
    if ((*value_address & (WriteSize - 1)) != 0 ||
        !RangeFits(*value_address, WriteSize, plan.module_image_size) ||
        !AddAddress(plan.module_base, *value_address, address) ||
        (address & (WriteSize - 1)) != 0) {
        error = std::string{tag} + " value field has an invalid destination mapping";
        return false;
    }
    for (const WriteAction& action : actions) {
        if (action.address == address) {
            error = std::string{tag} + " value field overlaps another transaction write";
            return false;
        }
    }
    WriteAction action{
        .address = address,
        .kind = kind,
    };
    EncodeU64(0, action.replacement);
    actions.push_back(action);
    return true;
}

} // namespace

NsoRelocationCommitResult CommitNsoRelocationPlan(const NsoRelocationPlan& plan,
                                                  const NsoDynamicInfo& dynamic,
                                                  const NsoRelocationMemory& memory) {
    NsoRelocationCommitResult result;
    if (memory.read == nullptr || memory.write == nullptr) {
        result.error = "relocation transaction requires read and write callbacks";
        return result;
    }
    if (!ValidatePlan(plan, dynamic, result.error)) {
        return result;
    }
    if (plan.writes.size() > std::numeric_limits<std::size_t>::max() - 2) {
        result.error = "relocation transaction is unsupported on this host";
        return result;
    }

    std::vector<WriteAction> actions;
    try {
        actions.reserve(plan.writes.size() + 2);
        for (const NsoPlannedRelocation& write : plan.writes) {
            WriteAction action{
                .address = write.address,
                .kind = WriteKind::Relocation,
                .table_index = write.table_index,
            };
            EncodeU64(write.value, action.replacement);
            actions.push_back(action);
        }
        if (!AddFinalizationAction(plan, dynamic, dynamic.rela, dynamic.rela_size_value_address,
                                   WriteKind::RelaSize, actions, result.error) ||
            !AddFinalizationAction(plan, dynamic, dynamic.plt_rela,
                                   dynamic.plt_rela_size_value_address, WriteKind::PltRelaSize,
                                   actions, result.error)) {
            return result;
        }

        for (WriteAction& action : actions) {
            if (!Read(memory, action.address, action.original)) {
                result.error = "could not snapshot " + ActionName(action);
                return result;
            }
            if (action.kind == WriteKind::RelaSize &&
                DecodeU64(action.original) != dynamic.rela->byte_size) {
                result.error = "DT_RELASZ changed after the relocation plan was parsed";
                return result;
            }
            if (action.kind == WriteKind::PltRelaSize &&
                DecodeU64(action.original) != dynamic.plt_rela->byte_size) {
                result.error = "DT_PLTRELSZ changed after the relocation plan was parsed";
                return result;
            }
        }
    } catch (const std::bad_alloc&) {
        result.error = "could not allocate the relocation transaction snapshot";
        return result;
    } catch (const std::length_error&) {
        result.error = "relocation transaction is unsupported on this host";
        return result;
    }

    for (std::size_t index = 0; index < actions.size(); ++index) {
        if (Write(memory, actions[index], false)) {
            continue;
        }
        bool rollback_succeeded = true;
        for (std::size_t rollback = index + 1; rollback != 0;) {
            --rollback;
            rollback_succeeded &= Write(memory, actions[rollback], true);
        }
        result.state = rollback_succeeded ? NsoRelocationCommitState::RolledBack
                                          : NsoRelocationCommitState::RollbackFailed;
        result.error = "could not commit " + ActionName(actions[index]);
        if (rollback_succeeded) {
            result.error += "; all transaction writes were rolled back";
        } else {
            result.error += "; rollback also failed and title execution is unsafe";
        }
        return result;
    }

    result.state = NsoRelocationCommitState::Committed;
    result.relocation_writes = plan.writes.size();
    result.finalization_writes = actions.size() - plan.writes.size();
    return result;
}

const char* NsoRelocationCommitStateName(NsoRelocationCommitState state) {
    switch (state) {
    case NsoRelocationCommitState::Rejected:
        return "rejected";
    case NsoRelocationCommitState::Committed:
        return "committed";
    case NsoRelocationCommitState::RolledBack:
        return "rolled-back";
    case NsoRelocationCommitState::RollbackFailed:
        return "rollback-failed";
    }
    return "unknown";
}

} // namespace suyu::recomp
