// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <nlohmann/json.hpp>
#include "common/package_policy.h"

namespace FileSys {

struct BakedPatchSelection {
    std::filesystem::path load_root;
    std::set<std::string> mods;
    std::string error;
};

// Digest is supplied by the host so validation also has a dependency-light
// synthetic test surface. No mod bytes are taken from the export package.
template <typename Digest>
BakedPatchSelection VerifyBakedPatches(const nlohmann::json& manifest,
                                      const std::filesystem::path& installed_load,
                                      const std::filesystem::path& package_dir,
                                      const std::string& title, Digest digest) {
    BakedPatchSelection result;
    if (!manifest.contains("baked_patches")) return result;
    const auto fail = [&](std::string message) {
        result.error = std::move(message);
        return result;
    };
    const auto component = [](const std::string& value) {
        return !value.empty() && value != "." && value != ".." &&
               value.find_first_of("/\\:") == std::string::npos;
    };
    const auto hash_text = [](const std::string& value) {
        return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    try {
        const auto& patches = manifest.at("baked_patches");
        const auto& mods = patches.at("mods");
        if (patches.at("schema") != 1 || !mods.is_array() || mods.empty() ||
            title.size() != 16 || title.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            return fail("Invalid baked-patch manifest. Re-export this game.");
        }
        const auto fingerprint = patches.at("fingerprint").get<std::string>();
        if (!hash_text(fingerprint) || digest(mods.dump()) != fingerprint) {
            return fail("Baked-patch manifest fingerprint differs. Re-export this game.");
        }
        const auto accepted = Common::PackagePolicy::AcceptInstalledRoot(package_dir, installed_load);
        if (!accepted) return fail("Baked patches require a mod folder outside this package.");
        result.load_root = *accepted;
        for (const auto& mod : mods) {
            const auto name = mod.at("name").get<std::string>();
            const auto& files = mod.at("files");
            if (!component(name) || !result.mods.insert(name).second || !files.is_array() || files.empty()) {
                return fail("Invalid or repeated baked mod name. Re-export this game.");
            }
            std::set<std::string> declared;
            const auto exefs = result.load_root / title / std::filesystem::u8path(name) / "exefs";
            for (const auto& file : files) {
                const auto path = file.at("path").get<std::string>();
                const auto leaf = path.starts_with("exefs/") ? path.substr(6) : std::string{};
                const auto extension = std::filesystem::u8path(leaf).extension().string();
                const auto build = file.at("target_build_id").get<std::string>();
                const auto hash = file.at("sha256").get<std::string>();
                if (!component(leaf) || (extension != ".ips" && extension != ".pchtxt") ||
                    !declared.insert(leaf).second || build.size() != 64 ||
                    build.find_first_not_of("0123456789ABCDEF") != std::string::npos || !hash_text(hash)) {
                    return fail("Invalid patch record for mod '" + name + "'. Re-export this game.");
                }
                const auto full = exefs / std::filesystem::u8path(leaf);
                if (!Common::PackagePolicy::AcceptInstalledRoot(package_dir, full)) {
                    return fail("Patch for mod '" + name + "' resolves inside this package.");
                }
                std::error_code ec;
                const auto size = std::filesystem::file_size(full, ec);
                if (ec || !file.at("size").is_number_unsigned() ||
                    size != file.at("size").get<std::uint64_t>() || size > (16u << 20)) {
                    return fail("Missing or changed patch for mod '" + name + "'. Restore it or re-export.");
                }
                std::ifstream input(full, std::ios::binary);
                std::string bytes(static_cast<std::size_t>(size), '\0');
                if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
                    digest(bytes) != file.at("sha256").get<std::string>()) {
                    return fail("Changed patch for mod '" + name + "'. Restore it or re-export.");
                }
            }
            // Prevent a new hook/module or patch in a baked folder from running
            // against code compiled for the recorded patch set.
            for (const auto& entry : std::filesystem::directory_iterator(exefs)) {
                const auto utf8_name = entry.path().filename().u8string();
                const std::string entry_name(utf8_name.begin(), utf8_name.end());
                if (!entry.is_regular_file() || !declared.contains(entry_name)) {
                    return fail("Unrecorded ExeFS file in mod '" + name + "'. Restore its baked files or re-export.");
                }
            }
        }
    } catch (const std::exception&) {
        return fail("Cannot read baked-patch metadata or installed mods. Restore them or re-export.");
    }
    return result;
}

} // namespace FileSys
