// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm/recomp/recomp_gap_session.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "core/arm/recomp/recomp_gaps.h"

namespace Core::RecompGaps {

namespace {

SessionRecorder g_recorder;
std::atomic<bool> g_active{false};

std::mutex g_flush_lock;
bool g_written = false;
std::optional<GapData> g_log_base;
std::optional<GapData> g_store_base;
std::chrono::steady_clock::time_point g_last_flush{};

std::mutex g_store_lock;
bool g_store_overridden = false;
std::filesystem::path g_store_dir;

constexpr auto kFlushInterval = std::chrono::seconds(30);

/// The file as it stood before this session wrote to it, if it is for this title.
GapData LoadBase(const std::filesystem::path& path, const std::string& title) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    std::string error;
    auto data = ReadFile(path, &error);
    if (!data) {
        LOG_WARNING(Core_ARM, "recomp gaps: {} is unreadable ({}); starting it again",
                    Common::FS::PathToUTF8String(path), error);
        return {};
    }
    if (data->title_id != title) {
        // The log copy follows whichever title ran last; the store is per title.
        return {};
    }
    return std::move(*data);
}

bool WriteMerged(const std::filesystem::path& path, const GapData& base, const GapData& run) {
    GapData merged = base;
    Merge(merged, run);
    std::string error;
    if (!WriteFile(path, merged, &error)) {
        LOG_WARNING(Core_ARM, "recomp gaps: could not write {}: {}",
                    Common::FS::PathToUTF8String(path), error);
        return false;
    }
    return true;
}

} // namespace

void BeginSession(u64 title_id, bool strict) {
    std::scoped_lock lk{g_flush_lock};
    g_recorder.Begin(title_id, strict);
    g_written = false;
    g_log_base.reset();
    g_store_base.reset();
    g_last_flush = std::chrono::steady_clock::now();
    g_active.store(true, std::memory_order_release);
}

void NoteModule(u64 base, u64 size, std::string_view name, const std::array<u8, 0x20>& build_id) {
    g_recorder.NoteModule(base, size, name, BuildIdHex(build_id.data(), build_id.size()));
}

void ForgetModule(u64 base) {
    g_recorder.ForgetModule(base);
}

void NoteImage(u64 base, std::string_view image_name) {
    g_recorder.NoteImage(base, image_name);
}

std::string ModuleBuildId(u64 address) {
    return g_recorder.BuildIdAt(address);
}

bool HasLoadedModule(std::string_view build_id, std::string_view name) {
    return g_recorder.HasModule(build_id, name);
}

void RecordMiss(u64 pc) {
    if (g_active.load(std::memory_order_acquire)) {
        g_recorder.RecordMiss(pc);
    }
}

void RecordUnimplemented(u32 insn) {
    if (g_active.load(std::memory_order_acquire)) {
        g_recorder.RecordUnimplemented(insn);
    }
}

void Flush(bool ran, bool force) {
    if (!g_active.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk{g_flush_lock, std::defer_lock};
    if (force) {
        lk.lock();
    } else if (!lk.try_lock()) {
        return; // another guest thread is already writing
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_last_flush < kFlushInterval) {
        return;
    }
    g_last_flush = now;
    const GapData run = g_recorder.Snapshot();
    if (!ran && run.Misses() == 0 && run.unimplemented.empty()) {
        return; // the backend never ran: not a run worth counting
    }
    if (!g_recorder.TakeDirty() && g_written) {
        return;
    }

    const auto log_path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir) /
                          "recomp_gaps.json";
    if (!g_log_base) {
        g_log_base = LoadBase(log_path, run.title_id);
    }
    const bool log_ok = WriteMerged(log_path, *g_log_base, run);

    const u64 title_id = g_recorder.TitleId();
    const auto store_file = SharedStoreFile(title_id);
    bool store_ok = false;
    if (!store_file.empty()) {
        std::error_code ec;
        const auto dir = store_file.parent_path();
        // Only ever create recomp/gaps inside a suyu user folder that exists.
        if (std::filesystem::is_directory(dir.parent_path().parent_path(), ec)) {
            std::filesystem::create_directories(dir, ec);
        }
        if (std::filesystem::is_directory(dir, ec)) {
            if (!g_store_base) {
                g_store_base = LoadBase(store_file, run.title_id);
            }
            store_ok = WriteMerged(store_file, *g_store_base, run);
        }
    }
    if (!g_written || force) {
        LOG_INFO(Core_ARM,
                 "recomp gaps: {} offset(s) in {} module(s), {} module(s) without an image, {} "
                 "opcode(s), {} unattributed miss(es); log {} store {}",
                 run.GapOffsets(), run.modules.size(), run.no_image.size(),
                 run.unimplemented.size(), run.unattributed_misses,
                 log_ok ? Common::FS::PathToUTF8String(log_path) : std::string{"not written"},
                 store_ok ? Common::FS::PathToUTF8String(store_file) : std::string{"not written"});
    }
    g_written = true;
}

void EndSession(bool ran) {
    Flush(ran, true);
    g_active.store(false, std::memory_order_release);
}

void SetSharedStoreDir(const std::filesystem::path& dir) {
    std::scoped_lock lk{g_store_lock};
    g_store_overridden = true;
    g_store_dir = dir;
}

std::filesystem::path SharedStoreDir() {
    std::scoped_lock lk{g_store_lock};
    if (g_store_overridden) {
        return g_store_dir;
    }
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::EdenDir) / "recomp" / "gaps";
}

std::filesystem::path SharedStoreFile(u64 title_id) {
    const auto dir = SharedStoreDir();
    if (title_id == 0 || dir.empty()) {
        return {};
    }
    return dir / (TitleIdHex(title_id) + ".json");
}

} // namespace Core::RecompGaps
