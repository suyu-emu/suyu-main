#include <array>
#include <cstdint>
#include <iostream>
#include <vector>
#include "suyu/nso_patch_baking.h"

int main() {
    std::vector<std::uint8_t> header(0x100, 0);
    header[0] = 'N'; header[1] = 'S'; header[2] = 'O'; header[3] = '0';
    for (unsigned i = 0; i < 32; ++i) header[0x40 + i] = static_cast<std::uint8_t>(i + 1);
    const auto saved_header = header;
    const std::array<std::uint32_t, 3> locations{0x20, 0x100, 0x220};
    std::array<std::vector<std::uint8_t>, 3> segments{
        std::vector<std::uint8_t>(8, 0x11), std::vector<std::uint8_t>(12, 0x22),
        std::vector<std::uint8_t>(16, 0x33)};
    bool called = false;
    const auto patch = [&](const auto& image) {
        called = true;
        auto result = image;
        if (!std::equal(header.begin(), header.end(), image.begin()) || image.size() != 0x100 + 0x1000)
            result.clear();
        else {
            for (unsigned i = 0; i < 3; ++i) result[0x100 + locations[i] + 3] = static_cast<std::uint8_t>(0xa0 + i);
        }
        return result;
    };
    const bool ok = SuyuExport::BakeNsoPatches(header, locations, segments, 0x40, patch);
    const bool passed = ok && called && header == saved_header &&
                        segments[0][3] == 0xa0 && segments[1][3] == 0xa1 &&
                        segments[2][3] == 0xa2 && segments[0][4] == 0x11 &&
                        segments[1][4] == 0x22 && segments[2][4] == 0x33;
    std::cout << "flat NSO patch baking: " << (passed ? "PASS" : "FAIL") << '\n';
    if (!passed) return 1;
    auto unchanged = segments;
    if (SuyuExport::BakeNsoPatches(header, locations, segments, 0,
                                 [](const auto&) { return std::vector<std::uint8_t>{}; })) return 1;
    if (segments != unchanged) return 1;
    for (const auto offset : {0x100u + 0x10u, 0x100u + 0x80u,
                              0x100u + 0x230u, 0x100u + 0x500u}) {
        if (SuyuExport::BakeNsoPatches(header, locations, segments, 0x40,
                                      [&](const auto& image) {
                                          auto changed = image;
                                          changed[offset] = 0xff;
                                          return changed;
                                      })) return 1;
        if (segments != unchanged) return 1;
    }
    std::cout << "leading/intersegment gaps, BSS, and page padding changes: rejected\n";
    return 0;
}
