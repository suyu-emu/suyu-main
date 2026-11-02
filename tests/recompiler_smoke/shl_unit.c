/* smoke_shl_unit: runs the emitted SHL by immediate (shl_ops.c, from
   smoke_shl_gen) against a reference written from the architecture's
   pseudocode: esize = 8 << HighestSetBit(immh), shift = immh:immb - esize,
   every element shifted left and truncated, the rest of a 64-bit vector
   destination zeroed, and nothing else in the guest context touched. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_runtime.h"
#include "shl_words.h"

typedef void (*ShlOpFn)(GuestContext*);
extern const ShlOpFn g_shl_ops[];

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint64_t Next(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

static void Reference(unsigned w, GuestContext* c) {
    const unsigned scalar = (w >> 28) & 1, q = (w >> 30) & 1;
    const unsigned immh = (w >> 19) & 15, immb = (w >> 16) & 7;
    const unsigned rn = (w >> 5) & 31, rd = w & 31;
    const unsigned hs = (immh & 8) ? 3 : (immh & 4) ? 2 : (immh & 2) ? 1 : 0;
    const unsigned esize = 8u << hs, shift = ((immh << 3) | immb) - esize;
    const unsigned datasize = scalar ? esize : (q ? 128 : 64);
    const uint64_t mask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint8_t src[16], dst[16] = {0};
    memcpy(src, c->vreg[rn], 16);
    for (unsigned e = 0; e < datasize / esize; ++e) {
        uint64_t v = 0;
        for (unsigned b = 0; b < esize / 8; ++b)
            v |= (uint64_t)src[e * esize / 8 + b] << (8 * b);
        v = (v << shift) & mask;
        for (unsigned b = 0; b < esize / 8; ++b)
            dst[e * esize / 8 + b] = (uint8_t)(v >> (8 * b));
    }
    memcpy(c->vreg[rd], dst, 16);
}

int main(void) {
    unsigned checked = 0;
    for (unsigned k = 0; k < kShlWordCount; ++k) {
        for (unsigned trial = 0; trial < 2000; ++trial) {
            GuestContext got, want;
            uint8_t* raw = (uint8_t*)&got;
            for (size_t b = 0; b < sizeof got; ++b) raw[b] = (uint8_t)Next();
            if (trial == 0) memset(got.vreg, 0xFF, sizeof got.vreg);
            if (trial == 1) memset(got.vreg, 0x80, sizeof got.vreg);
            memcpy(&want, &got, sizeof got);
            Reference(kShlWords[k], &want);
            g_shl_ops[k](&got);
            if (memcmp(&got, &want, sizeof got) != 0) {
                const unsigned rd = kShlWords[k] & 31;
                fprintf(stderr,
                        "FAIL %08x trial %u: v%u got %016llx:%016llx want %016llx:%016llx\n",
                        kShlWords[k], trial, rd, (unsigned long long)got.vreg[rd][1],
                        (unsigned long long)got.vreg[rd][0], (unsigned long long)want.vreg[rd][1],
                        (unsigned long long)want.vreg[rd][0]);
                return 1;
            }
            ++checked;
        }
    }
    printf("smoke_shl_unit: ok (%u words, %u cases)\n", (unsigned)kShlWordCount, checked);
    return 0;
}
