// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace SuyuExport {

// PatchNSO consumes an NSO header followed by the loader's flat virtual image,
// not three concatenated decompressed segments or the compressed on-disk NSO.
template <typename Patch>
bool BakeNsoPatches(std::span<const std::uint8_t> header,
                    const std::array<std::uint32_t, 3>& locations,
                    std::array<std::vector<std::uint8_t>, 3>& segments,
                    std::uint32_t bss_size, Patch&& patch) {
    // The loader pads the image (including BSS) to a 4 KiB page before patching.
    std::uint64_t extent = std::uint64_t{locations[2]} + segments[2].size() + bss_size;
    extent = (extent + 0xfff) & ~std::uint64_t{0xfff};
    if (extent > std::numeric_limits<std::uint32_t>::max()) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (std::uint64_t{locations[i]} + segments[i].size() > extent) return false;
    }
    std::vector<std::uint8_t> image(header.size() + static_cast<std::size_t>(extent));
    std::copy(header.begin(), header.end(), image.begin());
    for (unsigned i = 0; i < 3; ++i) {
        std::copy(segments[i].begin(), segments[i].end(), image.begin() + header.size() + locations[i]);
    }
    auto patched = patch(image);
    if (patched.size() != image.size() ||
        !std::equal(header.begin(), header.end(), patched.begin())) return false;
    // Only original segment bytes are represented in the recompiled image.
    // Changes in gaps, BSS, or page padding would be lost when splitting it.
    std::array<std::pair<std::size_t, std::size_t>, 3> ranges;
    for (unsigned i = 0; i < 3; ++i) {
        ranges[i] = {header.size() + locations[i], header.size() + locations[i] + segments[i].size()};
    }
    std::sort(ranges.begin(), ranges.end());
    std::size_t cursor = header.size();
    for (const auto& [begin, end] : ranges) {
        if (begin < cursor || !std::equal(image.begin() + cursor, image.begin() + begin,
                                         patched.begin() + cursor)) return false;
        cursor = end;
    }
    if (!std::equal(image.begin() + cursor, image.end(), patched.begin() + cursor)) return false;
    for (unsigned i = 0; i < 3; ++i) {
        std::copy_n(patched.begin() + header.size() + locations[i], segments[i].size(), segments[i].begin());
    }
    return true;
}

} // namespace SuyuExport
