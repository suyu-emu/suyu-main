// smoke_literal_unit: PC-relative literal loads (LDR/LDRSW literal, GPR and
// SIMD) must read from the module's run-time load base plus the literal's
// module offset, like ADR/ADRP.
//
// Regression: the emitter baked the module-relative address in as an absolute
// constant, so `ldr x28, <literal>` in TOTK main read 0x8635c0 instead of
// base+0x8635c0 and faulted. The rest of the smoke suite runs at base 0, where
// both forms agree, so only the emitted text can catch it.
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/recompiler/arm64_to_c.h"

#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

static std::string Emit(std::uint32_t insn, std::uint64_t pc) {
    std::string out;
    bool unhandled = false;
    if (!suyu::recomp::Translate(insn, pc, out, &unhandled) || unhandled) {
        return "<unhandled>";
    }
    return out;
}

static bool Has(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

int main() {
    // ldr x28, #+0x40 at 0x863580 (the TOTK main site).
    const std::string ldr_x = Emit(0x5800021cu, 0x863580);
    CHECK(Has(ldr_x, "recomp_load64(c,(g_module_base+0x8635c0ULL))"));
    CHECK(Has(ldr_x, "c->x[28]=_v"));

    // ldr w2, #-4: negative offsets stay module-relative too.
    const std::string ldr_w = Emit(0x18ffffe2u, 0x2000);
    CHECK(Has(ldr_w, "recomp_load32(c,(g_module_base+0x1ffcULL))"));

    // ldrsw x1, #+8: sign-extended 32-bit load.
    const std::string ldrsw = Emit(0x98000041u, 0x3000);
    CHECK(Has(ldrsw, "recomp_load32(c,(g_module_base+0x3008ULL))"));
    CHECK(Has(ldrsw, "-0x80000000ULL"));

    // ldr q0, #+8: both 64-bit halves are module-relative.
    const std::string ldr_q = Emit(0x9c000040u, 0x4000);
    CHECK(Has(ldr_q, "recomp_load64(c,(g_module_base+0x4008ULL))"));
    CHECK(Has(ldr_q, "recomp_load64(c,(g_module_base+0x4008ULL)+8)"));

    std::puts("smoke_literal_unit: ok");
    return 0;
}
