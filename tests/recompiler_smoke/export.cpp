#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "core/arm/recomp/recomp_gaps.h"
#include "core/recompiler/arm64_to_c.h"
#include "code.h"
#include "loop_code.h"

static bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value && *value && *value != '0';
}

// --loop <dir> [gaps.json]: the coverage loop's module, seeded the way
// game_export.cpp seeds a real one - recorded offsets for this build ID only,
// inside .text and word aligned.
static int ExportLoop(const char* out_dir, const char* gaps_path) {
    std::vector<uint64_t> roots;
    if (gaps_path) {
        std::string error;
        const auto gaps = Core::RecompGaps::ReadFile(gaps_path, &error);
        if (!gaps) {
            std::fprintf(stderr, "cannot read %s: %s\n", gaps_path, error.c_str());
            return 1;
        }
        for (const uint64_t offset : Core::RecompGaps::RootsFor(*gaps, LOOP_BUILD_ID)) {
            if ((offset & 3) == 0 && offset >= LOOP_BASE &&
                offset - LOOP_BASE < sizeof(loop_code)) {
                roots.push_back(offset);
            }
        }
    }
    const auto stats = suyu::recomp::EmitProject(
        "loop", reinterpret_cast<const uint8_t*>(loop_code), sizeof(loop_code), LOOP_BASE,
        out_dir, true, nullptr, 0, nullptr, 0, 0, {}, &roots);
    std::printf("loop export: %zu root(s) used, %llu block(s)\n", roots.size(),
                static_cast<unsigned long long>(stats.blocks));
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--loop")) {
        return ExportLoop(argv[2], argc >= 4 ? argv[3] : nullptr);
    }
    if (argc != 2) return 2;
    // Same switches the nso_emit harness uses for the ABI 6 variants.
    suyu::recomp::g_emit_fastmem = EnvOn("SUYU_RECOMP_AB_FASTMEM");
    suyu::recomp::g_emit_guard_gen = EnvOn("SUYU_RECOMP_AB_GUARD_GEN");
    const char* fpx = std::getenv("SUYU_RECOMP_AB_FPX");
    suyu::recomp::g_emit_fpx = fpx && *fpx && *fpx != '0';
    suyu::recomp::g_emit_fpx_shadow = fpx && !std::strcmp(fpx, "shadow");
    const auto stats = suyu::recomp::EmitProject(
        "smoke", reinterpret_cast<const uint8_t*>(smoke_code), sizeof(smoke_code),
        0x1000, argv[1], true);
    const auto second = suyu::recomp::EmitProject(
        "second", reinterpret_cast<const uint8_t*>(smoke_code), sizeof(smoke_code),
        0x1000, std::string(argv[1]) + "/second", true);
    return stats.unhandled == 0 && stats.emitted == sizeof(smoke_code) / 4 &&
                   second.unhandled == 0 && second.emitted == sizeof(smoke_code) / 4
               ? 0
               : 1;
}
