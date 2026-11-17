// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/arm64_to_c.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void CheckNoShiftBy64(std::uint32_t instruction, std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output.find("1ULL << 64") == std::string::npos,
          "bitfield translation never emits a 64-bit C shift count");
}

void CheckPrefetchNoop(std::uint32_t instruction, std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output == "    /* prfum */\n",
          "PRFUM emits a no-op without unused address temporaries");
}

void CheckDiscardedLoad(std::uint32_t instruction, std::string_view load,
                        std::string_view description) {
    std::string output;
    Check(suyu::recomp::Translate(instruction, 0x1000, output), description);
    Check(output.find(load) != std::string::npos,
          "loads into the zero register still perform the memory access");
}

void CheckUnhandled(std::uint32_t instruction, std::string_view description) {
    std::string output;
    bool known_unhandled = false;
    Check(suyu::recomp::Translate(instruction, 0x1000, output, &known_unhandled), description);
    Check(output.find("recomp_unhandled") != std::string::npos,
          "unallocated load encoding is handed to the fallback engine");
    Check(known_unhandled, "explicit fallback is included in static coverage");
}

void CheckCoverageReport() {
    const suyu::recomp::RecompileStats empty_stats{};
    Check(empty_stats.KnownTranslatedInstructions() == 0 &&
              empty_stats.KnownTranslatedFraction() == 0.0 &&
              empty_stats.KnownUnhandledFraction() == 0.0,
          "empty coverage has no fabricated percentage");

    bool known_unhandled = true;
    std::string translated;
    Check(suyu::recomp::Translate(0xD503201F, 0x1000, translated, &known_unhandled),
          "NOP translates for the coverage flag reset test");
    Check(!known_unhandled, "a translated instruction clears the coverage fallback flag");

    constexpr std::array<std::uint32_t, 3> text{
        0xB9C0019F, // Unallocated load encoding.
        0xF8C01120, // Unallocated PRFUM encoding with a distinct signature.
        0xD65F03C0, // RET
    };
    const auto unique_id = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto output_dir = std::filesystem::temp_directory_path() /
                            ("suyu_arm64_to_c_coverage_test_" + std::to_string(unique_id));
    std::error_code error;
    std::filesystem::remove_all(output_dir, error);
    Check(!error, "old static coverage test output can be removed");

    try {
        const auto stats = suyu::recomp::EmitProject(
            "coverage", reinterpret_cast<const std::uint8_t*>(text.data()), sizeof(text), 0x1000,
            output_dir.string(), true, nullptr, 0, nullptr, 0, 0x1000);
        Check(stats.visited_instructions == 3, "coverage counts every visited instruction");
        Check(stats.known_unhandled_instructions == 2,
              "coverage counts explicit runtime fallbacks");
        Check(stats.KnownTranslatedInstructions() == 1,
              "coverage derives the known translated instruction count");
        Check(stats.known_unhandled_by_op0.at(0xC) == 2,
              "coverage groups unknown instructions by op0");
        const auto& signature = stats.known_unhandled_by_signature.at(0xB9C00000U);
        Check(signature.count == 1 && signature.example_instruction == 0xB9C0019FU &&
                  signature.example_pc == 0x1000,
              "coverage records a stable signature example and guest PC");

        std::ifstream report(output_dir / "recomp_static_coverage.json", std::ios::binary);
        const std::string json{std::istreambuf_iterator<char>{report},
                               std::istreambuf_iterator<char>{}};
        Check(static_cast<bool>(report), "coverage report can be read");
        Check(json.find("\"module\": \"coverage\"") != std::string::npos,
              "coverage JSON identifies its module");
        Check(json.find("\"known_unhandled_instructions\": 2") != std::string::npos,
              "coverage JSON reports the fallback count");
        const auto first_signature = json.find("\"signature\": \"0xB9C00000\"");
        const auto second_signature = json.find("\"signature\": \"0xF8C00000\"");
        Check(first_signature != std::string::npos && second_signature != std::string::npos,
              "coverage JSON reports both encoding signatures");
        Check(first_signature < second_signature,
              "equally frequent signatures use deterministic numeric ordering");
        Check(json.find("\"example_pc\": \"0x1000\"") != std::string::npos,
              "coverage JSON reports the example guest PC");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: coverage report generation threw: " << exception.what() << '\n';
        ++failures;
    }

    error.clear();
    std::filesystem::remove_all(output_dir, error);
    Check(!error, "static coverage test output can be removed");
}

} // namespace

int main() {
    // SBFIZ X8, X1, #33, #31: its sign bit lands at bit 63, so there are no upper bits to
    // extend. Emitting a nominal extension mask used to generate the undefined `1ULL << 64`.
    CheckNoShiftBy64(0x935F7828, "64-bit SBFIZ translates");

    // SBFM X0, X1, #0, #63 is a full-width move and likewise needs no extension mask.
    CheckNoShiftBy64(0x9340FC20, "full-width SBFM translates");

    // Real SDK instances with positive and negative unscaled offsets. PRFUM is a cache hint,
    // so declaring address temporaries for it only creates compiler warnings in generated C.
    CheckPrefetchNoop(0xF8801120, "positive-offset PRFUM translates");
    CheckPrefetchNoop(0xF89C0100, "negative-offset PRFUM translates");

    // Odyssey uses these LDUR WZR forms while probing memory. The destination is discarded,
    // but the load itself must still happen (and the generated address temporaries must be used).
    CheckDiscardedLoad(0xB85D019F, "(void)recomp_load32", "negative-offset LDUR WZR translates");
    CheckDiscardedLoad(0xB85E819F, "(void)recomp_load32", "second LDUR WZR translates");

    // size=10/opc=11 is unallocated, rather than a 32-bit sign-extending load.
    CheckUnhandled(0xB9C0019F, "unsigned-offset unallocated load remains a fallback");
    CheckUnhandled(0xB8C0019F, "unscaled unallocated load remains a fallback");
    CheckUnhandled(0xB8ED699F, "register-offset unallocated load remains a fallback");
    CheckUnhandled(0xF8C01120, "unallocated PRFUM opc remains a fallback");
    CheckUnhandled(0xF8801520, "unallocated PRFUM writeback remains a fallback");
    CheckUnhandled(0x88DFFD9F, "LDAR WZR retains acquire semantics through fallback");
    CheckCoverageReport();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "arm64_to_c tests passed\n";
    return 0;
}
