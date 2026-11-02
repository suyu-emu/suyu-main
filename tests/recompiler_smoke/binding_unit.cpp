// smoke_binding_unit: which recompiled image each loaded module is given
// (Core::RecompGaps::MatchImage, used by suyu-cmd's base setter).
//
// Regression: images used to be bound by position - the loader's Nth module
// got the registration's Nth image. A Hybrid export that leaves main to the
// JIT registers [rtld, subsdk0, sdk], so subsdk0's image was given main's base
// and sdk's image multimedia's, and nnSdk got none. Synthetic IDs and names
// only; no game data.
#include <cstddef>
#include <cstdio>
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

const std::string kRtld = "a1000000000000000000000000000000000000000000000000000000000000a1";
const std::string kMain = "b2000000000000000000000000000000000000000000000000000000000000b2";
const std::string kSub0 = "c3000000000000000000000000000000000000000000000000000000000000c3";
const std::string kSdk = "d4000000000000000000000000000000000000000000000000000000000000d4";
const std::string kSub1 = "e5000000000000000000000000000000000000000000000000000000000000e5";

struct Loaded {
    const char* name;
    std::string build_id;
};

// What the loader reports, in load order: names as the kernel has them.
const std::vector<Loaded> kLoaded = {
    {"nnrtld", kRtld}, {"EX-Game.nss", kMain}, {"multimedia", kSub0}, {"nnSdk", kSdk}};

constexpr std::optional<std::size_t> kNone = std::nullopt;

/// Image index bound to each loaded module.
std::vector<std::optional<std::size_t>> Bind(const std::vector<G::ImageIdentity>& images) {
    std::vector<std::optional<std::size_t>> out;
    for (std::size_t i = 0; i < kLoaded.size(); ++i) {
        out.push_back(G::MatchImage(images, i, kLoaded[i].name, kLoaded[i].build_id));
    }
    return out;
}

int HybridOmitsMain() {
    // fallback_modules ["main"]: the registration has no image for main.
    const std::vector<G::ImageIdentity> images = {
        {"rtld", kRtld}, {"subsdk0", kSub0}, {"sdk", kSdk}};
    const auto b = Bind(images);
    CHECK(b[0] == std::optional<std::size_t>{0}); // nnrtld -> rtld
    CHECK(b[1] == kNone);                         // main runs on the JIT
    CHECK(b[2] == std::optional<std::size_t>{1}); // multimedia -> subsdk0, not main's base
    CHECK(b[3] == std::optional<std::size_t>{2}); // nnSdk -> sdk, not multimedia's base
    std::printf("PASS hybrid registration without main binds by build ID\n");
    return 0;
}

int ImageWithoutModule() {
    // An image whose module is not loaded is never bound to anything, and IDs
    // compare without regard to case.
    std::string upper_sdk = kSdk;
    for (char& c : upper_sdk) {
        c = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    const std::vector<G::ImageIdentity> images = {{"rtld", kRtld},   {"main", kMain},
                                                  {"subsdk0", kSub0}, {"subsdk1", kSub1},
                                                  {"sdk", upper_sdk}};
    const auto b = Bind(images);
    CHECK(b[0] == std::optional<std::size_t>{0});
    CHECK(b[1] == std::optional<std::size_t>{1});
    CHECK(b[2] == std::optional<std::size_t>{2});
    CHECK(b[3] == std::optional<std::size_t>{4}); // not subsdk1 (#3), whose module is absent
    // A module the loader gave no build ID for matches nothing once IDs are in use.
    CHECK(G::MatchImage(images, 1, "EX-Game.nss", "") == kNone);
    CHECK(G::MatchImage(images, 1, "EX-Game.nss", std::string(64, '0')) == kNone);
    std::printf("PASS unloaded image stays unbound\n");
    return 0;
}

int LegacyRegistration() {
    // Registrations from before build IDs: bound by NSO slot (name, or the slot
    // the load position implies), which is still independent of image positions.
    const std::vector<G::ImageIdentity> images = {{"rtld", ""}, {"subsdk0", ""}, {"sdk", ""}};
    const auto b = Bind(images);
    CHECK(b[0] == std::optional<std::size_t>{0});
    CHECK(b[1] == kNone);
    CHECK(b[2] == std::optional<std::size_t>{1});
    CHECK(b[3] == std::optional<std::size_t>{2});
    std::printf("PASS legacy registration binds by NSO slot\n");
    return 0;
}

} // namespace

int main() {
    return HybridOmitsMain() || ImageWithoutModule() || LegacyRegistration();
}
