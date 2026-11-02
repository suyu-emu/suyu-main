// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "common/common_types.h"

// The emulator's side of recomp_gaps.json: one recorder per ArmRecomp session.
// ArmRecomp begins and ends the session and reports misses and opcodes; the
// loaders say which module sits where with which build ID; the frontends say
// which of those modules run from a recompiled image.
//
// The run is merged into <log dir>/recomp_gaps.json and into the installed
// suyu's shared store, <suyu user dir>/recomp/gaps/<TITLEID>.json. Both are
// rewritten from the file as it was when the session first wrote plus this run,
// so writing again later in the same session never counts the run twice.
namespace Core::RecompGaps {

void BeginSession(u64 title_id, bool strict);
void NoteModule(u64 base, u64 size, std::string_view name, const std::array<u8, 0x20>& build_id);
void ForgetModule(u64 base);
void NoteImage(u64 base, std::string_view image_name);
/// Build ID (64 lower-case hex digits) of the loaded module containing
/// `address`, as the loader noted it; empty if no noted module contains it.
std::string ModuleBuildId(u64 address);
/// Whether the loader noted a module with this build ID (or, when it is
/// empty, this NSO name) in the current session.
bool HasLoadedModule(std::string_view build_id, std::string_view name);
void RecordMiss(u64 pc);
void RecordUnimplemented(u32 insn);

/// Writes now if anything changed; at most every 30 s unless `force`.
/// `ran` says the session executed recompiled code at all.
void Flush(bool ran, bool force);
void EndSession(bool ran);

/// The shared store directory. Defaults to <suyu user dir>/recomp/gaps. An
/// exported package points it at the installed suyu's instead, or passes an
/// empty path when none was found, which turns the shared copy off.
void SetSharedStoreDir(const std::filesystem::path& dir);
std::filesystem::path SharedStoreDir();
std::filesystem::path SharedStoreFile(u64 title_id);

} // namespace Core::RecompGaps
