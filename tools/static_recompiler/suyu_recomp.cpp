// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/recompiler/arm64_to_c.h"
#include "core/recompiler/npdm_info.h"
#include "core/recompiler/nso_dynamic.h"
#include "core/recompiler/nso_image.h"
#include "core/recompiler/nso_relocations.h"
#include "core/recompiler/nso_symbols.h"
#include "nso_sha256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef SUYU_RECOMPILER_HAS_LZ4
#include <lz4.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off: shellapi.h requires declarations from windows.h.
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

namespace {

using suyu::recomp::u64;
using suyu::recomp::u8;

enum class Command {
    EmitRaw,
    EmitNso,
    InspectNso,
};

struct Options {
    Command command{Command::EmitRaw};
    std::filesystem::path text_path;
    std::filesystem::path npdm_path;
    std::filesystem::path output_path;
    std::string module = "main";
    std::string title;
    u64 base{};
    std::optional<u64> entry;
    std::vector<u64> extra_roots;
    bool force = false;
    bool json = false;
    bool assume_aarch64 = false;
    bool have_npdm = false;
};

struct MappedTextIdentity {
    std::size_t size{};
    std::array<u8, 0x20> sha256{};
};

void PrintUsage(std::ostream& out, std::string_view executable) {
    out << "suyu-recomp - experimental AArch64 static recompiler\n\n"
        << "Usage:\n"
        << "  " << executable
        << " emit-raw --input <text.bin> --base <address> --output <directory> "
           "[options]\n"
        << "  " << executable
        << " emit-nso --input <module.nso> --npdm <main.npdm> --output "
           "<directory> [options]\n"
        << "  " << executable << " inspect-nso --input <module.nso> [--npdm <main.npdm>] [--json]\n"
        << "  " << executable
        << " <text.bin> <address> <directory> [--source-only]  (legacy form)\n\n"
        << "Arguments:\n"
        << "  --input <path>       Raw .text for emit-raw, or a plaintext NSO\n"
        << "  --base <address>     emit-raw guest address, decimal or "
           "0x-prefixed hex\n"
        << "  --output <path>      Directory for an emitted CMake project\n"
        << "  --entry <address>    emit-raw initial guest PC (defaults to "
           "--base)\n"
        << "  --root <address>     emit-raw block-discovery root; may be "
           "repeated\n"
        << "  --module <name>      C identifier fragment (defaults to main)\n"
        << "  --title <text>       Display title embedded in the standalone "
           "runner\n"
        << "  --force              Allow writing into a non-empty output "
           "directory\n"
        << "  --json               Emit machine-readable inspect-nso output\n"
        << "  --npdm <path>        Read architecture from a structurally "
           "validated main.npdm\n"
        << "  --assume-aarch64     Explicitly bypass NPDM architecture "
           "detection\n"
        << "  -h, --help           Show this help\n\n"
        << "Commands accept plaintext NSO/NPDM files and never need title keys. "
           "emit-nso\n"
        << "requires main.npdm or an explicit --assume-aarch64 override.\n"
        << "Generated standalone programs use\n"
        << "stub services; real games require the hosted suyu ArmRecomp "
           "runtime.\n";
}

bool IsOption(std::string_view value) {
    return value.size() > 1 && value.front() == '-';
}

std::optional<u64> ParseAddress(std::string_view text) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty() || text.front() == '-') {
        return std::nullopt;
    }

    u64 result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

bool IsModuleName(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    for (const unsigned char ch : name) {
        if ((ch < 'a' || ch > 'z') && (ch < 'A' || ch > 'Z') && (ch < '0' || ch > '9') &&
            ch != '_') {
            return false;
        }
    }
    return true;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::optional<MappedTextIdentity> ComputeMappedTextIdentity(std::span<const u8> text,
                                                            std::string& error) {
    constexpr std::size_t TextPageSize = 0x1000;
    if (text.size() > std::numeric_limits<std::size_t>::max() - (TextPageSize - 1)) {
        error = "mapped NSO text size overflows this host";
        return std::nullopt;
    }

    MappedTextIdentity identity{
        .size = (text.size() + TextPageSize - 1) & ~(TextPageSize - 1),
    };
    std::vector<u8> mapped_text;
    try {
        mapped_text.assign(text.begin(), text.end());
        mapped_text.resize(identity.size, 0);
    } catch (const std::exception& exception) {
        error = std::string{"could not allocate the page-aligned NSO text image: "} +
                exception.what();
        return std::nullopt;
    }
    if (!suyu::recomp::tool::ComputeSha256(mapped_text, identity.sha256)) {
        error = "could not hash the page-aligned NSO text image";
        return std::nullopt;
    }
    return identity;
}

#ifdef _WIN32
std::optional<std::vector<std::string>> GetUtf8Arguments() {
    int wide_count = 0;
    wchar_t** wide_arguments = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (!wide_arguments) {
        return std::nullopt;
    }

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(wide_count));
    bool conversion_failed = false;
    for (int i = 0; i < wide_count; ++i) {
        const std::wstring_view wide = wide_arguments[i];
        if (wide.empty()) {
            arguments.emplace_back();
            continue;
        }
        const int size =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            conversion_failed = true;
            break;
        }
        std::string utf8(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), utf8.data(), size, nullptr,
                                nullptr) != size) {
            conversion_failed = true;
            break;
        }
        arguments.push_back(std::move(utf8));
    }
    LocalFree(wide_arguments);
    if (conversion_failed) {
        return std::nullopt;
    }
    return arguments;
}
#endif

std::optional<std::vector<u8>> ReadFile(const std::filesystem::path& path,
                                        std::string_view description,
                                        std::optional<std::uintmax_t> maximum_size = std::nullopt) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        std::cerr << "error: cannot open " << description << ": " << PathToUtf8(path) << '\n';
        return std::nullopt;
    }

    const std::streampos end = stream.tellg();
    if (end < 0) {
        std::cerr << "error: cannot determine the size of " << description << ": "
                  << PathToUtf8(path) << '\n';
        return std::nullopt;
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (maximum_size && size > *maximum_size) {
        std::cerr << "error: " << description << " exceeds maximum size of " << *maximum_size
                  << " bytes: " << PathToUtf8(path) << '\n';
        return std::nullopt;
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        std::cerr << "error: " << description << " is too large for this host\n";
        return std::nullopt;
    }

    std::vector<u8> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::exception& error) {
        std::cerr << "error: could not allocate space for " << description << ": " << error.what()
                  << '\n';
        return std::nullopt;
    }
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size()))) {
        std::cerr << "error: could not read all of " << description << ": " << PathToUtf8(path)
                  << '\n';
        return std::nullopt;
    }
    return bytes;
}

std::optional<Options> ParseOptions(int argc, char** argv) {
    Options options;
    bool have_input = false;
    bool have_base = false;
    bool have_output = false;

    const bool emit_raw_command = argc > 1 && std::string_view{argv[1]} == "emit-raw";
    const bool emit_nso_command = argc > 1 && std::string_view{argv[1]} == "emit-nso";
    const bool inspect_nso_command = argc > 1 && std::string_view{argv[1]} == "inspect-nso";
    if (emit_nso_command) {
        options.command = Command::EmitNso;
    } else if (inspect_nso_command) {
        options.command = Command::InspectNso;
    }
    if (!emit_raw_command && !emit_nso_command && !inspect_nso_command && argc >= 4 &&
        !IsOption(argv[1]) && !IsOption(argv[2]) && !IsOption(argv[3])) {
        options.text_path = suyu::recomp::Utf8Path(argv[1]);
        const auto base = ParseAddress(argv[2]);
        if (!base) {
            std::cerr << "error: invalid base address: " << argv[2] << '\n';
            return std::nullopt;
        }
        options.base = *base;
        options.output_path = suyu::recomp::Utf8Path(argv[3]);
        have_input = have_base = have_output = true;
    }

    const int start =
        have_input ? 4 : ((emit_raw_command || emit_nso_command || inspect_nso_command) ? 2 : 1);
    for (int i = start; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto value_after = [&](std::string_view option) -> std::optional<std::string_view> {
            if (argument != option) {
                return std::nullopt;
            }
            if (++i >= argc) {
                std::cerr << "error: " << option << " requires a value\n";
                return std::string_view{};
            }
            return argv[i];
        };

        if (argument == "-h" || argument == "--help") {
            PrintUsage(std::cout, argv[0]);
            std::exit(0);
        }
        if (argument == "--source-only") {
            if (options.command != Command::EmitRaw) {
                std::cerr << "error: --source-only is only valid for emit-raw\n";
                return std::nullopt;
            }
            // Kept for compatibility with the original positional CLI. EmitProject
            // only emits source and has never invoked a compiler itself.
            continue;
        }
        if (argument == "--force") {
            if (options.command == Command::InspectNso) {
                std::cerr << "error: --force is not valid for inspect-nso\n";
                return std::nullopt;
            }
            options.force = true;
            continue;
        }
        if (argument == "--json") {
            if (options.command != Command::InspectNso) {
                std::cerr << "error: --json is only valid for inspect-nso\n";
                return std::nullopt;
            }
            options.json = true;
            continue;
        }
        if (argument == "--assume-aarch64") {
            if (options.command == Command::EmitRaw) {
                std::cerr << "error: --assume-aarch64 is only valid for NSO commands\n";
                return std::nullopt;
            }
            options.assume_aarch64 = true;
            continue;
        }
        if (const auto value = value_after("--input")) {
            if (value->empty()) {
                return std::nullopt;
            }
            options.text_path = suyu::recomp::Utf8Path(std::string{*value});
            have_input = true;
            continue;
        }
        if (const auto value = value_after("--npdm")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command == Command::EmitRaw) {
                std::cerr << "error: --npdm is only valid for NSO commands\n";
                return std::nullopt;
            }
            options.npdm_path = suyu::recomp::Utf8Path(std::string{*value});
            options.have_npdm = true;
            continue;
        }
        if (const auto value = value_after("--base")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command != Command::EmitRaw) {
                std::cerr << "error: --base is only valid for emit-raw\n";
                return std::nullopt;
            }
            const auto address = ParseAddress(*value);
            if (!address) {
                std::cerr << "error: invalid base address: " << *value << '\n';
                return std::nullopt;
            }
            options.base = *address;
            have_base = true;
            continue;
        }
        if (const auto value = value_after("--output")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command == Command::InspectNso) {
                std::cerr << "error: --output is not valid for inspect-nso\n";
                return std::nullopt;
            }
            options.output_path = suyu::recomp::Utf8Path(std::string{*value});
            have_output = true;
            continue;
        }
        if (const auto value = value_after("--entry")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command != Command::EmitRaw) {
                std::cerr << "error: --entry is only valid for emit-raw\n";
                return std::nullopt;
            }
            options.entry = ParseAddress(*value);
            if (!options.entry) {
                std::cerr << "error: invalid entry address: " << *value << '\n';
                return std::nullopt;
            }
            continue;
        }
        if (const auto value = value_after("--root")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command != Command::EmitRaw) {
                std::cerr << "error: --root is only valid for emit-raw\n";
                return std::nullopt;
            }
            const auto address = ParseAddress(*value);
            if (!address) {
                std::cerr << "error: invalid discovery root address: " << *value << '\n';
                return std::nullopt;
            }
            options.extra_roots.push_back(*address);
            continue;
        }
        if (const auto value = value_after("--module")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command == Command::InspectNso) {
                std::cerr << "error: --module is not valid for inspect-nso\n";
                return std::nullopt;
            }
            options.module = *value;
            continue;
        }
        if (const auto value = value_after("--title")) {
            if (value->empty()) {
                return std::nullopt;
            }
            if (options.command == Command::InspectNso) {
                std::cerr << "error: --title is not valid for inspect-nso\n";
                return std::nullopt;
            }
            options.title = *value;
            continue;
        }
        std::cerr << "error: unknown argument: " << argument << '\n';
        return std::nullopt;
    }

    if (options.command == Command::InspectNso) {
        if (!have_input) {
            std::cerr << "error: inspect-nso requires --input\n";
            return std::nullopt;
        }
        if (options.have_npdm && options.assume_aarch64) {
            std::cerr << "error: choose either --npdm or --assume-aarch64, not both\n";
            return std::nullopt;
        }
        return options;
    }

    if (options.command == Command::EmitNso) {
        if (!have_input || !have_output) {
            std::cerr << "error: emit-nso requires --input and --output\n";
            return std::nullopt;
        }
        if (options.have_npdm == options.assume_aarch64) {
            std::cerr << "error: emit-nso requires exactly one of --npdm or "
                         "--assume-aarch64\n";
            return std::nullopt;
        }
        if (!IsModuleName(options.module)) {
            std::cerr << "error: --module must contain 1-32 ASCII letters, digits, "
                         "or underscores\n";
            return std::nullopt;
        }
        return options;
    }

    if (!have_input || !have_base || !have_output) {
        std::cerr << "error: --input, --base, and --output are required\n";
        return std::nullopt;
    }
    if (!IsModuleName(options.module)) {
        std::cerr << "error: --module must contain 1-32 ASCII letters, digits, or "
                     "underscores\n";
        return std::nullopt;
    }
    if ((options.base & 3) != 0) {
        std::cerr << "error: --base must be 4-byte aligned\n";
        return std::nullopt;
    }

    return options;
}

std::string Hex(u64 value) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string result;
    do {
        result.push_back(Digits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    std::reverse(result.begin(), result.end());
    return "0x" + result;
}

std::string FixedHex(u64 value) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t i = result.size(); i > 0; --i) {
        result[i - 1] = Digits[value & 0xF];
        value >>= 4;
    }
    return result;
}

std::string JsonEscape(std::string_view value) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                result.push_back(Digits[ch >> 4]);
                result.push_back(Digits[ch & 0xF]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
        }
    }
    return result;
}

#ifdef SUYU_RECOMPILER_HAS_LZ4
bool DecompressLz4(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination) {
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        destination.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(source.data()), reinterpret_cast<char*>(destination.data()),
        static_cast<int>(source.size()), static_cast<int>(destination.size()));
    return size == static_cast<int>(destination.size());
}
#endif

struct LoadedNpdm {
    suyu::recomp::NpdmInfo info;
    std::vector<std::string> warnings;
};

struct DynamicSymbolSummary {
    std::size_t entries{};
    std::size_t imports{};
    std::size_t weak_imports{};
    std::size_t exports{};
    std::size_t absolute_definitions{};
};

struct RelocationPlanSummary {
    std::size_t writes{};
    std::size_t relative{};
    std::size_t null_symbols{};
    std::size_t module_definitions{};
    std::size_t absolute_definitions{};
    std::size_t external_definitions{};
    std::size_t undefined_weak{};
};

DynamicSymbolSummary SummarizeDynamicSymbols(const suyu::recomp::NsoDynamicSymbolInfo& info) {
    DynamicSymbolSummary summary{.entries = info.symbols.size()};
    for (const suyu::recomp::NsoDynamicSymbol& symbol : info.symbols) {
        if (symbol.IsUndefined() && !symbol.name.empty()) {
            ++summary.imports;
            if (symbol.IsWeak()) {
                ++summary.weak_imports;
            }
        }
        if (symbol.IsExternallyVisibleDefinition()) {
            ++summary.exports;
        }
        if (symbol.IsAbsolute() && !symbol.name.empty()) {
            ++summary.absolute_definitions;
        }
    }
    return summary;
}

RelocationPlanSummary SummarizeRelocationPlan(const suyu::recomp::NsoRelocationPlan& plan) {
    RelocationPlanSummary summary{.writes = plan.writes.size()};
    for (const suyu::recomp::NsoPlannedRelocation& write : plan.writes) {
        switch (write.value_source) {
        case suyu::recomp::NsoRelocationValueSource::Relative:
            ++summary.relative;
            break;
        case suyu::recomp::NsoRelocationValueSource::NullSymbol:
            ++summary.null_symbols;
            break;
        case suyu::recomp::NsoRelocationValueSource::ModuleDefinition:
            ++summary.module_definitions;
            break;
        case suyu::recomp::NsoRelocationValueSource::AbsoluteDefinition:
            ++summary.absolute_definitions;
            break;
        case suyu::recomp::NsoRelocationValueSource::ExternalDefinition:
            ++summary.external_definitions;
            break;
        case suyu::recomp::NsoRelocationValueSource::UndefinedWeak:
            ++summary.undefined_weak;
            break;
        }
    }
    return summary;
}

std::optional<std::vector<u64>> CollectDynamicExportRoots(
    const suyu::recomp::DecodedNso& image, std::string& error) {
    // A tiny synthetic or fully static module can legitimately omit both
    // tables. Once either NSO extent is present, however, require the complete
    // bounded ELF metadata rather than silently emitting an incomplete block
    // map from malformed symbol information.
    if (image.info.dynsym.size == 0 && image.info.dynstr.size == 0) {
        return std::vector<u64>{};
    }

    const auto dynamic = suyu::recomp::ParseNsoDynamic(image);
    if (!dynamic) {
        error = "could not parse ELF64 dynamic metadata for block roots: " + dynamic.error;
        return std::nullopt;
    }
    const auto symbols = suyu::recomp::ParseNsoDynamicSymbols(image, *dynamic.info);
    if (!symbols) {
        error = "could not parse dynamic symbols for block roots: " + symbols.error;
        return std::nullopt;
    }

    const u64 text_address = image.info.segments[0].memory_offset;
    const u64 text_size = image.segments[0].size();
    std::vector<u64> roots;
    try {
        roots.reserve(symbols.info->symbols.size());
        for (const suyu::recomp::NsoDynamicSymbol& symbol : symbols.info->symbols) {
            if (!symbol.IsExternallyVisibleDefinition() || (symbol.value & 3) != 0 ||
                symbol.value < text_address) {
                continue;
            }
            const u64 offset = symbol.value - text_address;
            if (offset <= text_size && 4 <= text_size - offset) {
                // Do not restrict this to STT_FUNC. Some retail SDKs publish
                // callable entry points with STT_NOTYPE; being externally
                // visible and instruction-aligned inside executable text is
                // the property the cross-module dispatcher actually needs.
                roots.push_back(symbol.value);
            }
        }
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    } catch (const std::bad_alloc&) {
        error = "could not allocate dynamic-symbol block roots";
        return std::nullopt;
    } catch (const std::length_error&) {
        error = "dynamic-symbol block roots are unsupported on this host";
        return std::nullopt;
    }
    return roots;
}

std::optional<LoadedNpdm> LoadNpdm(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path, "main.npdm", suyu::recomp::MaximumNpdmSize);
    if (!bytes) {
        return std::nullopt;
    }
    auto inspection = suyu::recomp::InspectNpdm(*bytes);
    if (!inspection) {
        std::cerr << "error: invalid main.npdm: " << inspection.error << '\n';
        return std::nullopt;
    }
    return LoadedNpdm{std::move(*inspection.info), std::move(inspection.warnings)};
}

bool ValidateOutputDirectory(const Options& options) {
    std::error_code error;
    const bool exists = std::filesystem::exists(options.output_path, error);
    if (error) {
        std::cerr << "error: cannot inspect output path: " << error.message() << '\n';
        return false;
    }
    bool has_entries = false;
    if (exists) {
        const bool is_directory = std::filesystem::is_directory(options.output_path, error);
        if (error) {
            std::cerr << "error: cannot inspect output path: " << error.message() << '\n';
            return false;
        }
        if (!is_directory) {
            std::cerr << "error: output path exists and is not a directory: "
                      << PathToUtf8(options.output_path) << '\n';
            return false;
        }
        const std::filesystem::directory_iterator first(options.output_path, error);
        if (error) {
            std::cerr << "error: cannot inspect output directory: " << error.message() << '\n';
            return false;
        }
        has_entries = first != std::filesystem::directory_iterator{};
    }
    if (has_entries && !options.force) {
        std::cerr << "error: output directory is not empty; pass --force to "
                     "overwrite the "
                     "generated files\n";
        return false;
    }
    return true;
}

bool ValidateGeneratedProject(const Options& options, bool expect_rodata, bool expect_data) {
    std::vector<std::filesystem::path> required_files{
        "CMakeLists.txt",
        "main.c",
        "recomp_export.c",
        "recomp_runtime.c",
        "recomp_runtime.h",
        "recomp_static_coverage.json",
        "data/text.bin",
        std::filesystem::path{"src"} / ("recompiled_" + options.module + ".c"),
        std::filesystem::path{"src"} / ("recompiled_" + options.module + "_0.c"),
    };
    if (expect_rodata) {
        required_files.emplace_back("data/rodata.bin");
    }
    if (expect_data) {
        required_files.emplace_back("data/data.bin");
    }
    for (const auto& relative_path : required_files) {
        const std::filesystem::path generated_path = options.output_path / relative_path;
        if (!std::filesystem::is_regular_file(generated_path)) {
            std::cerr << "error: generation did not produce " << PathToUtf8(generated_path) << '\n';
            return false;
        }
    }
    return true;
}

void PrintGenerationResult(const Options& options, const suyu::recomp::RecompileStats& stats) {
    std::cout << "Generated " << stats.blocks << " blocks from " << stats.instructions
              << " AArch64 instructions (" << stats.translated_terminators
              << " translated terminators).\n";
    if (stats.visited_instructions == 0) {
        std::cout << "Known static decoder coverage: N/A (no emitted text words).\n";
    } else {
        std::cout << "Known static decoder coverage (emitted text words): "
                  << stats.KnownTranslatedInstructions() << '/' << stats.visited_instructions
                  << " instructions (" << std::fixed << std::setprecision(2)
                  << stats.KnownTranslatedFraction() * 100.0 << "% translated, "
                  << stats.KnownUnhandledFraction() * 100.0 << "% explicit fallback).\n"
                  << std::defaultfloat;
    }
    std::cout << "Coverage report: "
              << PathToUtf8(options.output_path / "recomp_static_coverage.json")
              << "\nOutput: " << PathToUtf8(options.output_path) << '\n';
}

int InspectNso(const Options& options) {
    const auto bytes = ReadFile(options.text_path, "NSO image");
    if (!bytes) {
        return 1;
    }

    const auto inspection = suyu::recomp::InspectNso(*bytes);
    if (!inspection) {
        std::cerr << "error: invalid NSO image: " << inspection.error << '\n';
        return 1;
    }
    const auto& info = *inspection.info;
    std::optional<LoadedNpdm> npdm;
    if (options.have_npdm) {
        npdm = LoadNpdm(options.npdm_path);
        if (!npdm) {
            return 1;
        }
    }

#ifdef SUYU_RECOMPILER_HAS_LZ4
    const auto decoded = suyu::recomp::DecodeNso(
        *bytes, DecompressLz4, suyu::recomp::DefaultNsoDecodeLimit,
        suyu::recomp::tool::ComputeSha256);
#else
    const auto decoded = suyu::recomp::DecodeNso(
        *bytes, nullptr, suyu::recomp::DefaultNsoDecodeLimit,
        suyu::recomp::tool::ComputeSha256);
#endif
    const bool decode_unavailable = !decoded && (info.uses_zbic ||
#ifdef SUYU_RECOMPILER_HAS_LZ4
                                                 false
#else
                                                 std::any_of(
                                                     info.segments.begin(), info.segments.end(),
                                                     [](const auto& segment) {
                                                         return segment.compression ==
                                                                suyu::recomp::NsoCompression::Lz4;
                                                     })
#endif
                                                );
    if (!decoded && !decode_unavailable) {
        std::cerr << "error: NSO segment decoding failed: " << decoded.error << '\n';
        return 1;
    }
    std::optional<MappedTextIdentity> mapped_text_identity;
    if (decoded) {
        std::string identity_error;
        mapped_text_identity =
            ComputeMappedTextIdentity(decoded.image->segments[0], identity_error);
        if (!mapped_text_identity) {
            std::cerr << "error: " << identity_error << '\n';
            return 1;
        }
    }
    std::vector<std::string> warnings = decoded.warnings;
    if (npdm) {
        warnings.insert(warnings.end(), npdm->warnings.begin(), npdm->warnings.end());
    }

    std::optional<u64> aarch64_entry;
    std::vector<suyu::recomp::Block> aarch64_blocks;
    std::string aarch64_error;
    const bool analyze_aarch64 =
        options.assume_aarch64 ||
        (npdm && npdm->info.architecture == suyu::recomp::NpdmArchitecture::Aarch64);

    std::optional<suyu::recomp::NsoDynamicInfo> dynamic_info;
    std::optional<DynamicSymbolSummary> dynamic_symbol_summary;
    std::optional<RelocationPlanSummary> relocation_plan_summary;
    std::string dynamic_error;
    std::string dynamic_symbol_error;
    std::string relocation_plan_error;
    if (analyze_aarch64) {
        if (!decoded) {
            dynamic_error = decoded.error;
        } else {
            auto dynamic = suyu::recomp::ParseNsoDynamic(*decoded.image);
            warnings.insert(warnings.end(), dynamic.warnings.begin(), dynamic.warnings.end());
            if (dynamic) {
                dynamic_info = std::move(*dynamic.info);
            } else {
                dynamic_error = std::move(dynamic.error);
            }
        }
    }
    if (dynamic_info) {
        auto symbols = suyu::recomp::ParseNsoDynamicSymbols(*decoded.image, *dynamic_info);
        if (symbols) {
            dynamic_symbol_summary = SummarizeDynamicSymbols(*symbols.info);
            auto plan =
                suyu::recomp::PlanNsoRelocations(*decoded.image, *dynamic_info, *symbols.info, 0);
            if (plan) {
                relocation_plan_summary = SummarizeRelocationPlan(*plan.plan);
            } else {
                relocation_plan_error = std::move(plan.error);
            }
        } else {
            dynamic_symbol_error = std::move(symbols.error);
            relocation_plan_error =
                "dynamic symbol analysis is unavailable: " + dynamic_symbol_error;
        }
    }

    if (analyze_aarch64) {
        if (!decoded) {
            aarch64_error = decoded.error;
        } else {
            const auto& text = decoded.image->segments[0];
            const u64 base = info.segments[0].memory_offset;
            if (text.empty() || (text.size() & 3) != 0 || (base & 3) != 0) {
                aarch64_error = "text must be non-empty, four-byte aligned, and word-sized";
            } else {
                const std::uint32_t entry_offset =
                    suyu::recomp::FindNsoAarch64EntryOffset(*decoded.image);
                if (entry_offset == 0) {
                    aarch64_error =
                        "could not validate the conventional AArch64 entry stub and MOD0 header";
                } else {
                    aarch64_entry = base + entry_offset;
                    std::string dynamic_roots_error;
                    const auto dynamic_roots =
                        CollectDynamicExportRoots(*decoded.image, dynamic_roots_error);
                    if (!dynamic_roots) {
                        aarch64_error = std::move(dynamic_roots_error);
                    } else {
                        aarch64_blocks = suyu::recomp::DiscoverBlocks(
                            text.data(), text.size(), base, *aarch64_entry,
                            dynamic_roots->empty() ? nullptr : &*dynamic_roots);
                    }
                }
            }
        }
    }

    const std::string build_id = suyu::recomp::NsoBuildIdToHex(info.build_id);
    if (!options.json) {
        std::cout << "Input: " << PathToUtf8(options.text_path) << '\n'
                  << "Format: NSO0\n"
                  << "Version: " << info.version << '\n'
                  << "Flags: " << Hex(info.flags) << '\n'
                  << "Architecture: "
                  << (options.assume_aarch64 ? "AArch64 (explicit assumption)"
                      : npdm                 ? std::string{suyu::recomp::NpdmArchitectureName(
                                                   npdm->info.architecture)} +
                                                   " (main.npdm)"
                                             : "unknown (main.npdm required)")
                  << '\n';
        if (npdm) {
            std::cout << "NPDM address space: "
                      << (npdm->info.address_space
                              ? suyu::recomp::NpdmAddressSpaceName(*npdm->info.address_space)
                              : "unknown")
                      << '\n'
                      << "Program ID: " << FixedHex(npdm->info.program_id) << '\n';
        }
        std::cout << "Build ID: " << build_id << '\n'
                  << "Execute-only text: " << (info.execute_only ? "yes" : "no") << '\n'
                  << "Segments:\n";
        for (std::size_t i = 0; i < info.segments.size(); ++i) {
            const auto& segment = info.segments[i];
            std::cout << "  "
                      << suyu::recomp::NsoSegmentName(static_cast<suyu::recomp::NsoSegmentId>(i))
                      << ": file=" << Hex(segment.file_offset)
                      << " memory=" << Hex(segment.memory_offset)
                      << " decoded=" << segment.decoded_size << " stored=" << segment.stored_size
                      << " compression=" << suyu::recomp::NsoCompressionName(segment.compression)
                      << " hash-required=" << (segment.hash_required ? "yes" : "no");
            if (segment.hash_required) {
                std::cout << " expected-sha256="
                          << suyu::recomp::NsoBuildIdToHex(segment.expected_hash);
            }
            std::cout << '\n';
        }
        std::cout << "Module name: offset=" << Hex(info.module_name_offset)
                  << " size=" << info.module_name_size << '\n'
                  << "BSS size: " << info.bss_size << '\n'
                  << "Rodata extents: api-info=" << Hex(info.api_info.offset) << "+"
                  << info.api_info.size << " dynstr=" << Hex(info.dynstr.offset) << "+"
                  << info.dynstr.size << " dynsym=" << Hex(info.dynsym.offset) << "+"
                  << info.dynsym.size << '\n';
        if (decoded) {
            std::cout << "Segments decoded: yes\n";
            std::cout << "Required hashes verified: "
                      << (decoded.image->required_hashes_verified ? "yes" : "no") << '\n'
                      << "Mapped text size: " << mapped_text_identity->size << '\n'
                      << "Mapped text SHA-256: "
                      << suyu::recomp::NsoBuildIdToHex(mapped_text_identity->sha256) << '\n';
        } else {
            std::cout << "Segments decoded: no (" << decoded.error << ")\n";
            std::cout << "Required hashes verified: unavailable\n"
                      << "Mapped text identity: unavailable\n";
        }
        if (analyze_aarch64) {
            if (aarch64_error.empty()) {
                std::cout << "AArch64 entry: " << Hex(*aarch64_entry) << '\n'
                          << "AArch64 blocks: " << aarch64_blocks.size() << '\n'
                          << "AArch64 instruction words: " << decoded.image->segments[0].size() / 4
                          << '\n';
            } else {
                std::cout << "AArch64 analysis: unavailable (" << aarch64_error << ")\n";
            }

            if (dynamic_info) {
                std::cout << "ELF64 dynamic: MOD0=" << Hex(dynamic_info->mod0.address)
                          << " table=" << Hex(dynamic_info->dynamic_address)
                          << " non-null-entries=" << dynamic_info->entries.size()
                          << " bytes=" << dynamic_info->dynamic_byte_size << '\n';
                if (dynamic_info->string_table_address && dynamic_info->string_table_size) {
                    std::cout << "Dynamic strings: address="
                              << Hex(*dynamic_info->string_table_address)
                              << " bytes=" << *dynamic_info->string_table_size << '\n';
                } else {
                    std::cout << "Dynamic strings: none\n";
                }
                if (dynamic_info->symbol_table_address) {
                    std::cout << "Dynamic symbols: address="
                              << Hex(*dynamic_info->symbol_table_address)
                              << " entry-size=" << dynamic_info->symbol_entry_size << '\n';
                } else {
                    std::cout << "Dynamic symbols: none\n";
                }
                const auto print_rela = [](std::string_view label,
                                           const std::optional<suyu::recomp::NsoRelaTable>& table) {
                    std::cout << label << ": ";
                    if (!table) {
                        std::cout << "none\n";
                        return;
                    }
                    std::cout << "address=" << Hex(table->address) << " bytes=" << table->byte_size
                              << " entries=" << table->records.size()
                              << " entry-size=" << table->entry_size << '\n';
                };
                print_rela("Dynamic RELA", dynamic_info->rela);
                print_rela("PLT RELA", dynamic_info->plt_rela);
                if (dynamic_symbol_summary) {
                    std::cout << "ELF64 symbols: entries=" << dynamic_symbol_summary->entries
                              << " imports=" << dynamic_symbol_summary->imports
                              << " weak-imports=" << dynamic_symbol_summary->weak_imports
                              << " exports=" << dynamic_symbol_summary->exports
                              << " absolute-definitions="
                              << dynamic_symbol_summary->absolute_definitions
                              << " relocation-indices=verified\n";
                } else {
                    std::cout << "ELF64 symbol analysis: unavailable (" << dynamic_symbol_error
                              << ")\n";
                }
                if (relocation_plan_summary) {
                    std::cout
                        << "ELF64 relocation plan: writes=" << relocation_plan_summary->writes
                        << " relative=" << relocation_plan_summary->relative
                        << " null-symbol=" << relocation_plan_summary->null_symbols
                        << " module-definitions=" << relocation_plan_summary->module_definitions
                        << " absolute-definitions=" << relocation_plan_summary->absolute_definitions
                        << " external-definitions=" << relocation_plan_summary->external_definitions
                        << " undefined-weak=" << relocation_plan_summary->undefined_weak << '\n';
                } else {
                    std::cout << "ELF64 relocation plan: unavailable (" << relocation_plan_error
                              << ")\n";
                }
            } else {
                std::cout << "ELF64 dynamic analysis: unavailable (" << dynamic_error << ")\n";
            }
        }
        for (const std::string& warning : warnings) {
            std::cout << "Warning: " << warning << '\n';
        }
        return 0;
    }

    const char* architecture_json = "null";
    if (options.assume_aarch64) {
        architecture_json = "\"aarch64-assumed\"";
    } else if (npdm) {
        architecture_json = npdm->info.architecture == suyu::recomp::NpdmArchitecture::Aarch64
                                ? "\"aarch64\""
                                : "\"aarch32\"";
    }
    std::cout << "{\n"
              << "  \"format\": \"NSO0\",\n"
              << "  \"input\": \"" << JsonEscape(PathToUtf8(options.text_path)) << "\",\n"
              << "  \"version\": " << info.version << ",\n"
              << "  \"flags\": \"" << Hex(info.flags) << "\",\n"
              << "  \"architecture\": " << architecture_json << ",\n"
              << "  \"architecture_source\": "
              << (options.assume_aarch64 ? "\"assumption\""
                  : npdm                 ? "\"npdm\""
                                         : "null")
              << ",\n"
              << "  \"program_id\": ";
    if (npdm) {
        std::cout << "\"" << FixedHex(npdm->info.program_id) << "\",\n";
    } else {
        std::cout << "null,\n";
    }
    std::cout << "  \"build_id\": \"" << build_id << "\",\n"
              << "  \"execute_only\": " << (info.execute_only ? "true" : "false") << ",\n"
              << "  \"bss_size\": " << info.bss_size << ",\n"
              << "  \"module_name\": {\"offset\": \"" << Hex(info.module_name_offset)
              << "\", \"size\": " << info.module_name_size << "},\n"
              << "  \"rodata_extents\": {\n"
              << "    \"api_info\": {\"offset\": \"" << Hex(info.api_info.offset)
              << "\", \"size\": " << info.api_info.size << "},\n"
              << "    \"dynstr\": {\"offset\": \"" << Hex(info.dynstr.offset)
              << "\", \"size\": " << info.dynstr.size << "},\n"
              << "    \"dynsym\": {\"offset\": \"" << Hex(info.dynsym.offset)
              << "\", \"size\": " << info.dynsym.size << "}\n"
              << "  },\n"
              << "  \"segments\": [\n";
    for (std::size_t i = 0; i < info.segments.size(); ++i) {
        const auto& segment = info.segments[i];
        std::cout << "    {\"name\": \""
                  << suyu::recomp::NsoSegmentName(static_cast<suyu::recomp::NsoSegmentId>(i))
                  << "\", \"file_offset\": \"" << Hex(segment.file_offset)
                  << "\", \"memory_offset\": \"" << Hex(segment.memory_offset)
                  << "\", \"decoded_size\": " << segment.decoded_size
                  << ", \"stored_size\": " << segment.stored_size << ", \"compression\": \""
                  << suyu::recomp::NsoCompressionName(segment.compression)
                  << "\", \"hash_required\": " << (segment.hash_required ? "true" : "false")
                  << ", \"expected_sha256\": \""
                  << suyu::recomp::NsoBuildIdToHex(segment.expected_hash) << "\"}"
                  << (i + 1 == info.segments.size() ? "\n" : ",\n");
    }
    std::cout << "  ],\n"
              << "  \"segments_decoded\": " << (decoded ? "true" : "false") << ",\n"
              << "  \"required_hashes_verified\": ";
    if (decoded) {
        std::cout << (decoded.image->required_hashes_verified ? "true" : "false") << ",\n";
    } else {
        std::cout << "null,\n";
    }
    std::cout << "  \"decode_error\": ";
    if (decoded) {
        std::cout << "null,\n";
    } else {
        std::cout << "\"" << JsonEscape(decoded.error) << "\",\n";
    }
    std::cout << "  \"mapped_text_size\": ";
    if (mapped_text_identity) {
        std::cout << mapped_text_identity->size << ",\n";
    } else {
        std::cout << "null,\n";
    }
    std::cout << "  \"mapped_text_sha256\": ";
    if (mapped_text_identity) {
        std::cout << "\"" << suyu::recomp::NsoBuildIdToHex(mapped_text_identity->sha256)
                  << "\",\n";
    } else {
        std::cout << "null,\n";
    }
    std::cout << "  \"aarch64_analysis\": ";
    if (!analyze_aarch64) {
        std::cout << "null,\n";
    } else if (!aarch64_error.empty()) {
        std::cout << "{\"error\": \"" << JsonEscape(aarch64_error) << "\"},\n";
    } else {
        std::cout << "{\"entry\": \"" << Hex(*aarch64_entry)
                  << "\", \"blocks\": " << aarch64_blocks.size()
                  << ", \"instruction_words\": " << decoded.image->segments[0].size() / 4 << "},\n";
    }
    std::cout << "  \"dynamic_analysis\": ";
    if (!analyze_aarch64) {
        std::cout << "null,\n";
    } else if (!dynamic_info) {
        std::cout << "{\"error\": \"" << JsonEscape(dynamic_error) << "\"},\n";
    } else {
        const auto print_optional_table =
            [](const std::optional<suyu::recomp::NsoRelaTable>& table) {
                if (!table) {
                    std::cout << "null";
                    return;
                }
                std::cout << "{\"address\": \"" << Hex(table->address)
                          << "\", \"byte_size\": " << table->byte_size
                          << ", \"entry_size\": " << table->entry_size
                          << ", \"entries\": " << table->records.size() << '}';
            };
        std::cout << "{\"format\": \"ELF64\", \"mod0\": {\"address\": \""
                  << Hex(dynamic_info->mod0.address) << "\", \"dynamic_offset\": \""
                  << Hex(dynamic_info->mod0.dynamic_offset) << "\", \"bss_start_offset\": \""
                  << Hex(dynamic_info->mod0.bss_start_offset) << "\", \"bss_end_offset\": \""
                  << Hex(dynamic_info->mod0.bss_end_offset)
                  << "\", \"exception_info_start_offset\": \""
                  << Hex(dynamic_info->mod0.exception_info_start_offset)
                  << "\", \"exception_info_end_offset\": \""
                  << Hex(dynamic_info->mod0.exception_info_end_offset)
                  << "\", \"module_object_offset\": \""
                  << Hex(dynamic_info->mod0.module_object_offset)
                  << "\"}, \"dynamic_table\": {\"address\": \""
                  << Hex(dynamic_info->dynamic_address)
                  << "\", \"byte_size\": " << dynamic_info->dynamic_byte_size
                  << ", \"non_null_entries\": " << dynamic_info->entries.size()
                  << "}, \"string_table\": ";
        if (dynamic_info->string_table_address && dynamic_info->string_table_size) {
            std::cout << "{\"address\": \"" << Hex(*dynamic_info->string_table_address)
                      << "\", \"byte_size\": " << *dynamic_info->string_table_size << '}';
        } else {
            std::cout << "null";
        }
        std::cout << ", \"symbol_table\": ";
        if (dynamic_info->symbol_table_address) {
            std::cout << "{\"address\": \"" << Hex(*dynamic_info->symbol_table_address)
                      << "\", \"entry_size\": " << dynamic_info->symbol_entry_size << '}';
        } else {
            std::cout << "null";
        }
        std::cout << ", \"symbol_analysis\": ";
        if (dynamic_symbol_summary) {
            std::cout << "{\"entries\": " << dynamic_symbol_summary->entries
                      << ", \"imports\": " << dynamic_symbol_summary->imports
                      << ", \"weak_imports\": " << dynamic_symbol_summary->weak_imports
                      << ", \"exports\": " << dynamic_symbol_summary->exports
                      << ", \"absolute_definitions\": "
                      << dynamic_symbol_summary->absolute_definitions
                      << ", \"relocation_indices_verified\": true}";
        } else {
            std::cout << "{\"error\": \"" << JsonEscape(dynamic_symbol_error) << "\"}";
        }
        std::cout << ", \"relocation_plan\": ";
        if (relocation_plan_summary) {
            std::cout << "{\"writes\": " << relocation_plan_summary->writes
                      << ", \"relative\": " << relocation_plan_summary->relative
                      << ", \"null_symbols\": " << relocation_plan_summary->null_symbols
                      << ", \"module_definitions\": " << relocation_plan_summary->module_definitions
                      << ", \"absolute_definitions\": "
                      << relocation_plan_summary->absolute_definitions
                      << ", \"external_definitions\": "
                      << relocation_plan_summary->external_definitions
                      << ", \"undefined_weak\": " << relocation_plan_summary->undefined_weak << '}';
        } else {
            std::cout << "{\"error\": \"" << JsonEscape(relocation_plan_error) << "\"}";
        }
        std::cout << ", \"rela\": ";
        print_optional_table(dynamic_info->rela);
        std::cout << ", \"plt_rela\": ";
        print_optional_table(dynamic_info->plt_rela);
        std::cout << "},\n";
    }
    std::cout << "  \"warnings\": [";
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << "\"" << JsonEscape(warnings[i]) << "\"";
    }
    std::cout << "]\n}\n";
    return 0;
}

int EmitNso(const Options& options) {
    std::optional<LoadedNpdm> npdm;
    if (options.have_npdm) {
        npdm = LoadNpdm(options.npdm_path);
        if (!npdm) {
            return 1;
        }
        if (!npdm->info.address_space) {
            std::cerr << "error: main.npdm uses an unsupported process address-space "
                         "value\n";
            return 1;
        }
        if (npdm->info.architecture != suyu::recomp::NpdmArchitecture::Aarch64) {
            std::cerr << "error: main.npdm declares AArch32; emit-nso currently "
                         "supports only "
                         "AArch64\n";
            return 1;
        }
    } else {
        std::cerr << "warning: treating NSO instructions as AArch64 without main.npdm\n";
    }

    const auto bytes = ReadFile(options.text_path, "NSO image");
    if (!bytes) {
        return 1;
    }
#ifdef SUYU_RECOMPILER_HAS_LZ4
    auto decoded = suyu::recomp::DecodeNso(*bytes, DecompressLz4,
                                           suyu::recomp::DefaultNsoDecodeLimit,
                                           suyu::recomp::tool::ComputeSha256);
#else
    auto decoded = suyu::recomp::DecodeNso(*bytes, nullptr,
                                           suyu::recomp::DefaultNsoDecodeLimit,
                                           suyu::recomp::tool::ComputeSha256);
#endif
    if (!decoded) {
        std::cerr << "error: NSO segment decoding failed: " << decoded.error << '\n';
        return 1;
    }
    if (!decoded.image->required_hashes_verified) {
        std::cerr << "error: NSO requires segment hashes that were not verified\n";
        return 1;
    }

    const auto& info = decoded.image->info;
    const std::string layout_error = suyu::recomp::ValidateNsoExecutableLayout(info);
    if (!layout_error.empty()) {
        std::cerr << "error: NSO executable layout is invalid: " << layout_error << '\n';
        return 1;
    }
    const auto& text = decoded.image->segments[0];
    const auto& rodata = decoded.image->segments[1];
    const auto& data = decoded.image->segments[2];
    const u64 base = info.segments[0].memory_offset;
    if (text.empty() || (text.size() & 3) != 0 || (base & 3) != 0) {
        std::cerr << "error: decoded NSO text must be non-empty, word-sized, and "
                     "aligned\n";
        return 1;
    }
    const std::uint32_t entry_offset = suyu::recomp::FindNsoAarch64EntryOffset(*decoded.image);
    if (entry_offset == 0) {
        std::cerr << "error: could not validate the conventional AArch64 NSO entry "
                     "stub and MOD0 "
                     "header\n";
        return 1;
    }
    const u64 entry = base + entry_offset;
    std::string dynamic_roots_error;
    const auto dynamic_roots = CollectDynamicExportRoots(*decoded.image, dynamic_roots_error);
    if (!dynamic_roots) {
        std::cerr << "error: " << dynamic_roots_error << '\n';
        return 1;
    }
    std::string identity_error;
    const auto mapped_text_identity = ComputeMappedTextIdentity(text, identity_error);
    if (!mapped_text_identity) {
        std::cerr << "error: " << identity_error << '\n';
        return 1;
    }
    if (!ValidateOutputDirectory(options)) {
        return 1;
    }

    try {
        const auto stats = suyu::recomp::EmitProject(
            options.module, text.data(), text.size(), base, PathToUtf8(options.output_path), true,
            rodata.empty() ? nullptr : rodata.data(), rodata.size(),
            data.empty() ? nullptr : data.data(), data.size(), entry, options.title,
            dynamic_roots->empty() ? nullptr : &*dynamic_roots,
            suyu::recomp::RecompileImageLayout{info.segments[1].memory_offset,
                                               info.segments[2].memory_offset, info.bss_size},
            &info.build_id, &mapped_text_identity->sha256,
            static_cast<u64>(mapped_text_identity->size));
        if (!ValidateGeneratedProject(options, !rodata.empty(), !data.empty())) {
            return 1;
        }
        PrintGenerationResult(options, stats);
        std::cout << "NSO build ID: " << suyu::recomp::NsoBuildIdToHex(info.build_id) << '\n'
                  << "Architecture: AArch64 (" << (npdm ? "main.npdm" : "explicit assumption")
                  << ")\n"
                  << "Entry: " << Hex(entry) << '\n'
                  << "Dynamic export roots: " << dynamic_roots->size() << '\n';
        for (const std::string& warning : decoded.warnings) {
            std::cerr << "warning: " << warning << '\n';
        }
        if (npdm) {
            for (const std::string& warning : npdm->warnings) {
                std::cerr << "warning: " << warning << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "error: generation failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

int Run(int argc, char** argv) {
    if (argc == 1) {
        PrintUsage(std::cerr, argv[0]);
        return 2;
    }

    const auto parsed = ParseOptions(argc, argv);
    if (!parsed) {
        std::cerr << "Run with --help for usage.\n";
        return 2;
    }
    const Options& options = *parsed;

    if (options.command == Command::InspectNso) {
        return InspectNso(options);
    }
    if (options.command == Command::EmitNso) {
        return EmitNso(options);
    }

    const auto text = ReadFile(options.text_path, "text segment");
    if (!text) {
        return 1;
    }
    if (text->empty() || (text->size() & 3) != 0) {
        std::cerr << "error: the text segment must be non-empty and a multiple of "
                     "four bytes\n";
        return 1;
    }
    if (text->size() > std::numeric_limits<u64>::max() - options.base) {
        std::cerr << "error: text segment address range overflows 64 bits\n";
        return 1;
    }

    const u64 entry = options.entry.value_or(options.base);
    if ((entry & 3) != 0 || entry < options.base || entry >= options.base + text->size()) {
        std::cerr << "error: --entry must be aligned and inside the text segment\n";
        return 1;
    }

    for (const u64 root : options.extra_roots) {
        if ((root & 3) != 0 || root < options.base || root >= options.base + text->size()) {
            std::cerr << "error: every --root must be aligned and inside the text "
                         "segment\n";
            return 1;
        }
    }

    std::error_code output_error;
    const bool output_exists = std::filesystem::exists(options.output_path, output_error);
    if (output_error) {
        std::cerr << "error: cannot inspect output path: " << output_error.message() << '\n';
        return 1;
    }
    bool output_has_entries = false;
    if (output_exists) {
        const bool output_is_directory =
            std::filesystem::is_directory(options.output_path, output_error);
        if (output_error) {
            std::cerr << "error: cannot inspect output path: " << output_error.message() << '\n';
            return 1;
        }
        if (!output_is_directory) {
            std::cerr << "error: output path exists and is not a directory: "
                      << PathToUtf8(options.output_path) << '\n';
            return 1;
        }
        const std::filesystem::directory_iterator first(options.output_path, output_error);
        if (output_error) {
            std::cerr << "error: cannot inspect output directory: " << output_error.message()
                      << '\n';
            return 1;
        }
        output_has_entries = first != std::filesystem::directory_iterator{};
    }
    if (output_has_entries && !options.force) {
        std::cerr << "error: output directory is not empty; pass --force to "
                     "overwrite the "
                     "generated files\n";
        return 1;
    }

    try {
        const std::string output = PathToUtf8(options.output_path);
        const auto stats =
            suyu::recomp::EmitProject(options.module, text->data(), text->size(), options.base,
                                      output, true, nullptr, 0, nullptr, 0, entry, options.title,
                                      options.extra_roots.empty() ? nullptr : &options.extra_roots);

        const std::vector<std::filesystem::path> required_files{
            "CMakeLists.txt",
            "main.c",
            "recomp_export.c",
            "recomp_runtime.c",
            "recomp_runtime.h",
            "recomp_static_coverage.json",
            "data/text.bin",
            std::filesystem::path{"src"} / ("recompiled_" + options.module + ".c"),
            std::filesystem::path{"src"} / ("recompiled_" + options.module + "_0.c"),
        };
        for (const auto& relative_path : required_files) {
            const std::filesystem::path generated_path = options.output_path / relative_path;
            if (!std::filesystem::is_regular_file(generated_path)) {
                std::cerr << "error: generation did not produce " << PathToUtf8(generated_path)
                          << '\n';
                return 1;
            }
        }

        PrintGenerationResult(options, stats);
    } catch (const std::exception& error) {
        std::cerr << "error: generation failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    auto utf8_arguments = GetUtf8Arguments();
    if (!utf8_arguments) {
        std::cerr << "error: could not convert the Windows command line to UTF-8\n";
        return 1;
    }
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(utf8_arguments->size());
    for (std::string& argument : *utf8_arguments) {
        argument_pointers.push_back(argument.data());
    }
    return Run(static_cast<int>(argument_pointers.size()), argument_pointers.data());
#else
    return Run(argc, argv);
#endif
}
