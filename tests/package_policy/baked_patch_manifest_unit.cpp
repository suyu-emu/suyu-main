#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "core/file_sys/baked_patch_manifest.h"

namespace fs = std::filesystem;
using nlohmann::json;
// Deterministic test digest; production uses EVP SHA256, not this placeholder.
static std::string Digest(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    const auto part = out.str();
    return part + part + part + part;
}
static void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = fs::absolute(argv[1]);
    const auto package = root / "package";
    const auto load = root / "installed" / "load";
    const std::string title = "0100F2C0115B6000";
    const auto patch = load / title / "Static FPS" / "exefs" / "main.pchtxt";
    Write(patch, "patch bytes");
    fs::create_directories(package);
    json base = {{"baked_patches", {{"schema", 1}, {"mods", json::array({
        {{"name", "Static FPS"}, {"files", json::array({
            {{"path", "exefs/main.pchtxt"}, {"size", std::uint64_t{11}},
             {"sha256", Digest("patch bytes")}, {"target_build_id", std::string(64, 'A')}}})}}
    })}}}};
    const auto sign = [](json& value) {
        value["baked_patches"]["fingerprint"] = Digest(value["baked_patches"]["mods"].dump());
    };
    sign(base);
    int checks = 0, failures = 0, skipped = 0;
    const auto check = [&](const std::string& name, const json& value,
                           const fs::path& source, bool success) {
        ++checks;
        const auto result = FileSys::VerifyBakedPatches(value, source, package, title, Digest);
        if (result.error.empty() != success) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << result.error << '\n';
        }
    };
    check("unchanged", base, load, true);
    const auto matched = FileSys::VerifyBakedPatches(base, load, package, title, Digest);
    if (matched.load_root != load || matched.mods != std::set<std::string>{"Static FPS"}) ++failures;
    check("no baked metadata", json::object(), load, true);
    fs::rename(patch, patch.string() + ".missing");
    check("missing patch", base, load, false);
    fs::rename(patch.string() + ".missing", patch);
    Write(patch, "other bytes");
    check("same-size drift", base, load, false);
    Write(patch, "short");
    check("size drift", base, load, false);
    Write(patch, "patch bytes");
    auto value = base;
    value["baked_patches"]["fingerprint"] = "bad";
    check("fingerprint drift", value, load, false);
    for (const auto& bad_hash : {std::string{}, std::string("abcd"), std::string(64, 'G'), std::string(64, 'A')}) {
        value = base; value["baked_patches"]["fingerprint"] = bad_hash;
        check("malformed fingerprint", value, load, false);
        value = base; value["baked_patches"]["mods"][0]["files"][0]["sha256"] = bad_hash; sign(value);
        check("malformed patch hash", value, load, false);
    }
    ++checks;
    if (FileSys::VerifyBakedPatches(base, load, package, title,
                                  [](const auto&) { return std::string{}; }).error.empty()) ++failures;
    value = base; value["baked_patches"]["schema"] = 2;
    check("unknown schema", value, load, false);
    for (const auto* name : {"..", ".", "../outside", "outside\\bad", "C:outside", ""}) {
        value = base; value["baked_patches"]["mods"][0]["name"] = name; sign(value);
        check(std::string("bad name ") + name, value, load, false);
    }
    for (const auto* path : {"exefs/../main.pchtxt", "exefs/..\\main.pchtxt", "exefs/C:main.ips",
                             "other/main.ips", "exefs/main.npdm", "exefs/subsdk3"}) {
        value = base; value["baked_patches"]["mods"][0]["files"][0]["path"] = path; sign(value);
        check(std::string("bad path ") + path, value, load, false);
    }
    value = base;
    value["baked_patches"]["mods"].push_back(value["baked_patches"]["mods"][0]); sign(value);
    check("duplicate mod", value, load, false);
    value = base;
    auto& files = value["baked_patches"]["mods"][0]["files"];
    files.push_back(files[0]); sign(value);
    check("duplicate file", value, load, false);
    check("package load root", base, package / "load", false);
    Write(patch.parent_path() / "subsdk3", "synthetic hook");
    check("unexpected hook", base, load, false);
    fs::remove(patch.parent_path() / "subsdk3");
    Write(patch.parent_path() / "another.ips", "synthetic patch");
    check("unrecorded patch", base, load, false);
    fs::remove(patch.parent_path() / "another.ips");
    fs::create_directory(patch.parent_path() / "nested");
    check("unexpected directory", base, load, false);
    fs::remove(patch.parent_path() / "nested");
    std::error_code ec;
    fs::create_directory_symlink(package, root / "package-link", ec);
    if (ec) { ++skipped; std::cout << "SKIP symlink: " << ec.message() << '\n'; }
    else check("symlink load aliases package", base, root / "package-link" / "load", false);
    const auto unicode_name = std::string(reinterpret_cast<const char*>(u8"静的 FPS"));
    const auto unicode_leaf = std::string(reinterpret_cast<const char*>(u8"帧率.pchtxt"));
    Write(load / title / fs::u8path(unicode_name) / "exefs" / fs::u8path(unicode_leaf), "patch bytes");
    value = base;
    value["baked_patches"]["mods"][0]["name"] = unicode_name;
    value["baked_patches"]["mods"][0]["files"][0]["path"] = "exefs/" + unicode_leaf;
    sign(value);
    check("Unicode names", value, load, true);
    std::cout << checks << " baked-patch checks, " << failures << " failures, " << skipped << " skips\n";
    return failures ? 1 : 0;
}
