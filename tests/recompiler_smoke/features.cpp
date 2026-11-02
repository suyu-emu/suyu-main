// Host-side feature refusal (Core::RecompImageFeature) against a generated
// ABI 6 image: this host accepts exactly what this emitter produces, and a host
// that lacks any advertised feature refuses the image.
#include <cstdio>
#include "core/arm/recomp/recomp_image_features.h"

extern "C" unsigned recomp_image_features(void);

#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

namespace F = Core::RecompImageFeature;

// The assignments are shared with other branches; they must stay distinct.
static_assert(F::FastmemPT1 == 0x1u && F::GuardGen1 == 0x2u && F::ExactFpX1 == 0x4u);

int main() {
    CHECK(F::Unsupported(0) == 0);
    CHECK(F::Unsupported(F::FastmemPT1) == 0);
    // Anything outside what the host implements comes back, bit for bit.
    CHECK(F::Unsupported(F::FastmemPT1 | 0x80000000u) == 0x80000000u);
    CHECK(F::Unsupported(F::ExactFpX1) == 0);
    CHECK(F::Unsupported(0xFFFFFFFFu) == (0xFFFFFFFFu & ~F::kHostSupported));

    const unsigned features = recomp_image_features();
    CHECK(features & F::FastmemPT1);
    // This host implements every feature the emitter it was built with emits.
    CHECK(F::Unsupported(features) == 0);
    // A host implementing none of them, or only FM1, refuses what is missing.
    CHECK(F::Unsupported(features, 0) == features);
    CHECK(F::Unsupported(features, F::FastmemPT1) == (features & ~F::FastmemPT1));
    if (features & F::GuardGen1) {
        // The host before GG1 (FM1 only) refuses a generation-guard image;
        // this tree may also carry FPX1 (the "ggfpx" variant), which is an
        // independent bit refused the same way.
        CHECK(F::Unsupported(features, F::FastmemPT1) == (features & ~F::FastmemPT1));
        CHECK(F::Unsupported(features, F::FastmemPT1) & F::GuardGen1);
    }
    std::printf("PASS features 0x%x\n", features);
    return 0;
}
