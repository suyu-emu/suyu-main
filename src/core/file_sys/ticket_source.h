// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include "common/package_policy.h"

namespace FileSys {

inline bool CanReadInstalledTicketPath(const std::filesystem::path& path,
                                      const std::filesystem::path& package_dir) {
    return package_dir.empty() ||
           Common::PackagePolicy::AcceptInstalledRoot(package_dir, path).has_value();
}

inline std::filesystem::path SelectTicketSource(const std::filesystem::path& configured_nand,
                                               const std::filesystem::path& fallback_nand,
                                               const std::filesystem::path& package_dir,
                                               bool using_fallback_content) {
    const auto& source = using_fallback_content ? fallback_nand : configured_nand;
    if (!package_dir.empty() &&
        !Common::PackagePolicy::AcceptInstalledRoot(package_dir, source)) {
        return {};
    }
    return source;
}

} // namespace FileSys
