// smoke_module_name_unit: naming loaded modules from the start of their
// read-only segment (Core::RecompGaps::ModuleNameFromRodata, used by
// Core::FindModules), which is the list the recompiled images are bound from.
//
// Regression: FindModules only knew the older layout, {u32 0, s32 length,
// path}. Newer SDKs (TOTK 1.4.3) put a 12-byte header {u32 1, u32 end of path,
// u32} in front of it, so every module was skipped, the base setter was never
// called, every image kept base 0, and the first block read a module-relative
// address as absolute. Synthetic bytes laid out like the real segments; no
// game data.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/arm/recomp/recomp_gaps.h"

namespace G = Core::RecompGaps;

#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

namespace {

void Put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void PutPath(std::vector<std::uint8_t>& out, const std::string& path) {
    Put32(out, 0);
    Put32(out, static_cast<std::uint32_t>(path.size()));
    out.insert(out.end(), path.begin(), path.end());
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
}

void PutMod0(std::vector<std::uint8_t>& out) {
    for (const char c : std::string{"MOD0"}) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    for (int i = 0; i < 6; ++i) {
        Put32(out, 0x1000u * (i + 1));
    }
}

/// Older SDKs (TOTK 1.0.0): the path struct starts rodata.
std::vector<std::uint8_t> OldRodata(const std::string& path) {
    std::vector<std::uint8_t> out;
    PutPath(out, path);
    PutMod0(out);
    return out;
}

/// Newer SDKs (TOTK 1.4.3): {1, end of path, ?} and then the same struct.
std::vector<std::uint8_t> NewRodata(const std::string& path, std::uint32_t third = 0x50) {
    std::vector<std::uint8_t> path_struct;
    PutPath(path_struct, path);
    std::vector<std::uint8_t> out;
    Put32(out, 1);
    Put32(out, static_cast<std::uint32_t>(12 + path_struct.size()));
    Put32(out, third);
    out.insert(out.end(), path_struct.begin(), path_struct.end());
    PutMod0(out);
    return out;
}

std::string Name(const std::vector<std::uint8_t>& rodata) {
    return G::ModuleNameFromRodata(rodata.data(), rodata.size());
}

const char* const kMainPath = "D:\\home\\Project\\EX-Game\\App\\Rom\\NX64\\EX-Game.nss";

int OldLayout() {
    CHECK(Name(OldRodata("nnrtld")) == "nnrtld");
    CHECK(Name(OldRodata(kMainPath)) == "EX-Game.nss");
    CHECK(Name(OldRodata("/sdk/nnSdk")) == "nnSdk");
    std::printf("PASS older rodata layout names its module\n");
    return 0;
}

int NewLayout() {
    // The bytes the 1.4.3 rtld starts with: 01000000 1c000000 50000000, then
    // {0, 6, "nnrtld"} and MOD0 at 0x1c.
    const auto rtld = NewRodata("nnrtld");
    CHECK(rtld.size() > 0x20 && rtld[0] == 1 && rtld[4] == 0x1c && rtld[12] == 0 && rtld[16] == 6);
    CHECK(std::memcmp(rtld.data() + 0x1c, "MOD0", 4) == 0);
    CHECK(Name(rtld) == "nnrtld");
    CHECK(Name(NewRodata(kMainPath, 0x94)) == "EX-Game.nss");
    CHECK(Name(NewRodata("multimedia", 0x54)) == "multimedia");
    CHECK(Name(NewRodata("nnSdk")) == "nnSdk");
    std::printf("PASS newer rodata layout names its module\n");
    return 0;
}

int Rejected() {
    CHECK(Name({}) == "");
    CHECK(Name({0, 0, 0}) == "");
    // A zero length, an unknown header version, and a path past its own end.
    CHECK(Name(OldRodata("")) == "");
    auto v2 = NewRodata("nnrtld");
    v2[0] = 2;
    CHECK(Name(v2) == "");
    auto short_end = NewRodata("nnrtld");
    short_end[4] = 0x18;
    CHECK(Name(short_end) == "");
    // A truncated read still names what fits.
    const auto rtld = OldRodata("nnrtld");
    CHECK(G::ModuleNameFromRodata(rtld.data(), 8 + 3) == "nnr");
    std::printf("PASS rodata without a module path names nothing\n");
    return 0;
}

int BindsEveryImage() {
    // The whole path the 1.4.3 run took: the loader notes each NSO with its
    // build ID; FindModules names each from its rodata; every image must bind.
    const std::string ids[] = {
        "a1000000000000000000000000000000000000000000000000000000000000a1",
        "b2000000000000000000000000000000000000000000000000000000000000b2",
        "c3000000000000000000000000000000000000000000000000000000000000c3",
        "d4000000000000000000000000000000000000000000000000000000000000d4"};
    const std::vector<std::vector<std::uint8_t>> rodata = {
        NewRodata("nnrtld"), NewRodata(kMainPath, 0x94), NewRodata("multimedia", 0x54),
        NewRodata("nnSdk")};
    const std::vector<G::ImageIdentity> images = {
        {"rtld", ids[0]}, {"main", ids[1]}, {"subsdk0", ids[2]}, {"sdk", ids[3]}};
    const char* const slots[] = {"rtld", "main", "subsdk0", "sdk"};
    G::SessionRecorder loader;
    loader.Begin(0x0100000000010000ULL, true);
    std::vector<std::string> named;
    for (std::size_t i = 0; i < rodata.size(); ++i) {
        loader.NoteModule(0x80000000ULL + 0x10000000ULL * i, 0x1000, slots[i], ids[i]);
        const std::string name = Name(rodata[i]);
        if (!name.empty()) {
            named.push_back(name);
        }
    }
    CHECK(named.size() == 4); // was 0: no setter call, no binding, no warning
    for (std::size_t i = 0; i < named.size(); ++i) {
        CHECK(G::MatchImage(images, i, named[i], ids[i]) == std::optional<std::size_t>{i});
        // What the bind check asks of an image left unbound.
        CHECK(loader.HasModule(images[i].build_id, images[i].name));
    }
    CHECK(!loader.HasModule("e5000000000000000000000000000000000000000000000000000000000000e5",
                            "subsdk1"));
    // Registrations without build IDs are matched by NSO slot name.
    CHECK(loader.HasModule("", "SubSdk0"));
    CHECK(!loader.HasModule("", "subsdk1"));
    std::printf("PASS every image binds on the newer layout\n");
    return 0;
}

} // namespace

int main() {
    return OldLayout() || NewLayout() || Rejected() || BindsEveryImage();
}
