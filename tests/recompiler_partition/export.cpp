#include "core/recompiler/arm64_to_c.h"
#include "../recompiler_smoke/code.h"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    suyu::recomp::g_emit_fastmem = true;
    suyu::recomp::g_emit_guard_gen = true;
    suyu::recomp::g_emit_fpx = true;
    for (const char* mode : {"baseline", "partitioned"}) {
        const size_t limit = std::string_view(mode) == "baseline" ? 32u << 20 : 4096;
        for (const char* module : {"smoke", "second"}) {
            const std::string output = std::string(argv[1]) + "/" + mode +
                (std::string_view(module) == "second" ? "/second" : "");
            const auto stats = suyu::recomp::EmitProject(
                module, reinterpret_cast<const uint8_t*>(smoke_code), sizeof(smoke_code),
                0x1000, output, true, nullptr, 0, nullptr, 0, 0, {}, nullptr, limit);
            if (stats.unhandled || stats.emitted != sizeof(smoke_code) / 4) return 1;
        }
    }
    // The byte budget supplements the existing block ceiling.
    const std::vector<uint32_t> returns(20001, 0xd65f03c0U);
    const auto counted = suyu::recomp::EmitProject(
        "counted", reinterpret_cast<const uint8_t*>(returns.data()),
        returns.size() * sizeof(uint32_t), 0x1000,
        std::string(argv[1]) + "/counted", true);
    return counted.blocks == returns.size() && counted.emitted == returns.size() ? 0 : 1;
}
