/* FP differential driver for the static recompiler (see README.md).

   Implementations, all running the same instruction words of words.h:
     soft    the emitter's default (ABI 5) text, ops_soft.c
     fpx     the FPX1 text, ops_fpx.c                        (FPX_HAVE_FPX)
     nokeep  FPX1 without its keep test (negative control)   (FPX_HAVE_FPX)
     nomid   FPX1 without its midpoint test (negative control) (FPX_HAVE_FPX)
     nofz    FPX1 that ignores FPCR.FZ (negative control)    (FPX_HAVE_FPX)
     hw      the AArch64 host executing the word itself      (FPX_HAVE_HW)

   Every case compares the whole of q0, x0, NZCV and the final guest FPSR.

   usage: driver diff  [options]      compare every candidate with the reference
          driver hash  IMPL [options] print one FNV-1a hash per leg, word and FPCR
          driver check FILE [options] recompute hashes for soft (and fpx) and compare
          driver control NAME [options]  nokeep | nomid | nofz | mxcsr: must find mismatches
          driver env   [options]      poisoned host FP mode repaired by the host shim
          driver inhibit [options]    the host kill switch (fpcr bit 32) turns FPX1 off
          driver shadow [options]     the shadow instrumentation build against soft
   options: --cases N (L1 cases per word per FPCR), --legs 1234, --seed S,
            --shard I/N, --l4-step K, --word 0xXXXXXXXX, --adv N (L3 cases) */
#include "rt_soft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "words.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#define FPX_X86 1
#endif

typedef void (*FpxOpFn)(GuestContext*);
extern const FpxOpFn g_ops_soft[];
#ifdef FPX_HAVE_FPX
extern const FpxOpFn g_ops_fpx[];
extern const FpxOpFn g_ops_nokeep[];
extern const FpxOpFn g_ops_nomid[];
extern const FpxOpFn g_ops_nofz[];
extern const FpxOpFn g_ops_shadow[];
/* RECOMP_FPX_PROBE in ops_fpx.c: [1] fast path kept, [2] fell through. */
unsigned long long g_fpx_probe[3];
#endif
#ifdef FPX_HAVE_ENV
/* The host's FP-environment repair (fpx_env.cpp); returns 1 if it changed anything. */
int fpx_env_shim(void);
#endif
#ifdef FPX_HAVE_HW
typedef uint64_t (*FpxHwFn)(const uint64_t*, uint64_t*, uint64_t);
extern const FpxHwFn g_hw[];
#endif

enum { I_SOFT, I_FPX, I_NOKEEP, I_NOMID, I_NOFZ, I_SHADOW, I_HW, I_COUNT };
static const char* const kImplNames[I_COUNT] = {"soft", "fpx", "nokeep", "nomid", "nofz", "shadow", "hw"};

static int ImplAvailable(int impl) {
    switch (impl) {
    case I_SOFT: return 1;
#ifdef FPX_HAVE_FPX
    case I_FPX: case I_NOKEEP: case I_NOMID: case I_NOFZ: case I_SHADOW: return 1;
#endif
#ifdef FPX_HAVE_HW
    case I_HW: return 1;
#endif
    default: return 0;
    }
}

/* q0..q3 as lo/hi pairs, then x0, x1, NZCV in bits 31..28: the layout hw.c reads. */
typedef struct { uint64_t w[11]; } In;
typedef struct { uint64_t q0[2], x0, nzcv, fpsr, fpcr; } Out;

static GuestContext g_ctx[I_COUNT];

static void Run(int impl, unsigned op, const In* in, uint64_t fpcr, uint64_t fs0, Out* out) {
#ifdef FPX_HAVE_HW
    if (impl == I_HW) {
        uint64_t o[4];
        const uint64_t fs = g_hw[op](in->w, o, fpcr);
        out->q0[0] = o[0]; out->q0[1] = o[1]; out->x0 = o[2];
        out->nzcv = o[3] & 0xf0000000u;
        out->fpsr = fs0 | (fs & 0x9f);
        out->fpcr = fpcr;
        return;
    }
#endif
    GuestContext* c = &g_ctx[impl];
    FpxOpFn fn = g_ops_soft[op];
#ifdef FPX_HAVE_FPX
    if (impl == I_FPX) fn = g_ops_fpx[op];
    if (impl == I_NOKEEP) fn = g_ops_nokeep[op];
    if (impl == I_NOMID) fn = g_ops_nomid[op];
    if (impl == I_NOFZ) fn = g_ops_nofz[op];
    if (impl == I_SHADOW) fn = g_ops_shadow[op];
#endif
    memcpy(c->vreg[0], &in->w[0], 64);
    c->x[0] = in->w[8];
    c->x[1] = in->w[9];
    c->n = (uint8_t)((in->w[10] >> 31) & 1); c->z = (uint8_t)((in->w[10] >> 30) & 1);
    c->c = (uint8_t)((in->w[10] >> 29) & 1); c->v = (uint8_t)((in->w[10] >> 28) & 1);
    c->fpcr = fpcr;
    c->fpsr = fs0;
    fn(c);
    out->q0[0] = c->vreg[0][0]; out->q0[1] = c->vreg[0][1]; out->x0 = c->x[0];
    out->nzcv = ((uint64_t)(c->n & 1) << 31) | ((uint64_t)(c->z & 1) << 30) |
                ((uint64_t)(c->c & 1) << 29) | ((uint64_t)(c->v & 1) << 28);
    out->fpsr = c->fpsr;
    out->fpcr = c->fpcr;
}

static int SameValue(const Out* a, const Out* b) {
    return a->q0[0] == b->q0[0] && a->q0[1] == b->q0[1] && a->x0 == b->x0 && a->nzcv == b->nzcv;
}

/* ---- deterministic inputs ------------------------------------------------ */

static uint64_t rs;
static uint64_t Rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void Seed(uint64_t seed, uint32_t word, uint64_t fpcr, unsigned leg) {
    rs = seed * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)word << 20) ^ (fpcr << 3) ^ leg;
    rs = rs ? rs : 1;
    for (int i = 0; i < 8; ++i) Rnd();
}

static const uint32_t S_EDGE[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00001,
    0x7f800001, 0xff8a5a5a, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
    0x00800001, 0x7f7fffff, 0xff7fffff, 0x3f800000, 0xbf800000, 0x3f800001, 0x33800000, 0x0c000000,
    0x1f800000, 0x5f000000, 0x7f000000, 0x00400000, 0x3fffffff, 0x40000000};
static const uint64_t D_EDGE[] = {0, 0x8000000000000000ULL, 0x7ff0000000000000ULL,
    0xfff0000000000000ULL, 0x7ff8000000000000ULL, 0xfff8000000000001ULL, 0x7ff0000000000001ULL,
    0xfff4a5a5a5a5a5a5ULL, 1, 0x8000000000000001ULL, 0x000fffffffffffffULL, 0x0010000000000000ULL,
    0x8010000000000000ULL, 0x0010000000000001ULL, 0x7fefffffffffffffULL, 0xffefffffffffffffULL,
    0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x3ff0000000000001ULL, 0x3ca0000000000000ULL,
    0x2000000000000000ULL, 0x5ff0000000000000ULL, 0x7fe0000000000000ULL, 0x0008000000000000ULL,
    0x3fffffffffffffffULL};

/* kind: 0 edge, 1 raw bits, 2 moderate, 3 near/below min normal, 4 near overflow,
   5 short mantissas near 1, 6 near integers and halves up to 2^64 */
#define GEN_KINDS 7
static uint32_t Gen32(int kind) {
    uint32_t s = (uint32_t)Rnd() & 0x80000000u, e, m;
    switch (kind) {
    case 0: return S_EDGE[Rnd() % (sizeof S_EDGE / 4)];
    case 1: return (uint32_t)Rnd();
    case 2: e = (uint32_t)(Rnd() % 40) + 107; return s | (e << 23) | ((uint32_t)Rnd() & 0x7fffff);
    case 3: e = (uint32_t)(Rnd() % 60); return s | (e << 23) | ((uint32_t)Rnd() & 0x7fffff);
    case 4: e = (uint32_t)(Rnd() % 40) + 215; return s | (e << 23) | ((uint32_t)Rnd() & 0x7fffff);
    case 5: e = (uint32_t)(Rnd() % 8) + 124; return s | (e << 23) | ((uint32_t)Rnd() & 0x7ff000);
    default: {
        const unsigned up = (unsigned)(Rnd() % 67); /* 2^-2 .. 2^64 */
        e = 125 + up;
        m = (uint32_t)Rnd() & 0x7fffff;
        if (Rnd() & 1) {
            const unsigned keep = up + 2 < 23 ? up + 2 : 23; /* integers, halves, quarters */
            m &= ~((1u << (23 - keep)) - 1);
        }
        return s | (e << 23) | m;
    }
    }
}
static uint64_t Gen64(int kind) {
    uint64_t s = Rnd() & 0x8000000000000000ULL, e, m;
    switch (kind) {
    case 0: return D_EDGE[Rnd() % (sizeof D_EDGE / 8)];
    case 1: return Rnd();
    case 2: e = Rnd() % 80 + 983; return s | (e << 52) | (Rnd() & 0xfffffffffffffULL);
    case 3: e = Rnd() % 80; return s | (e << 52) | (Rnd() & 0xfffffffffffffULL);
    case 4: e = Rnd() % 80 + 1967; return s | (e << 52) | (Rnd() & 0xfffffffffffffULL);
    case 5: e = Rnd() % 8 + 1020; return s | (e << 52) | (Rnd() & 0xfffffff000000ULL);
    default: {
        const unsigned up = (unsigned)(Rnd() % 67);
        e = 1021 + up;
        m = Rnd() & 0xfffffffffffffULL;
        if (Rnd() & 1) {
            const unsigned keep = up + 2 < 52 ? up + 2 : 52;
            m &= ~((1ULL << (52 - keep)) - 1);
        }
        return s | (e << 52) | m;
    }
    }
}
/* Integers: small, near powers of two, around the 24/53-bit and signed limits, raw. */
static uint64_t GenInt(void) {
    const unsigned k = (unsigned)(Rnd() % 6);
    const unsigned b = (unsigned)(Rnd() % 64);
    const uint64_t neg = (Rnd() & 1) ? ~0ULL : 0;
    switch (k) {
    case 0: return (Rnd() % 1024) ^ neg;
    case 1: return ((1ULL << b) + (Rnd() % 5) - 2) ^ neg;
    case 2: return ((1ULL << (b % 8 + 23)) + (Rnd() % 9) - 4) ^ neg;
    case 3: return ((1ULL << (b % 8 + 52)) + (Rnd() % 9) - 4) ^ neg;
    case 4: { const uint64_t top = Rnd() & 1 ? 0x7fffffffULL : 0x7fffffffffffffffULL; return top + (Rnd() % 5) - 2; }
    default: { const unsigned shift = (unsigned)(Rnd() % 64); return Rnd() >> shift; }
    }
}

static int IsFpKind(char k) { return k == 'S' || k == 'D'; }

static void RandomInput(unsigned op, In* in) {
    const char kind = g_fpx_words[op].kind;
    const int gk = (int)(Rnd() % GEN_KINDS), mixed = (Rnd() % 4) == 0;
    for (int k = 0; k < 8; ++k) {
        const int kk = mixed ? (int)(Rnd() % GEN_KINDS) : gk;
        if (kind == 'D') in->w[k] = Gen64(kk);
        else if (kind == 'S') {
            const uint64_t lo = Gen32(kk);
            const int kh = mixed ? (int)(Rnd() % GEN_KINDS) : gk;
            in->w[k] = lo | (uint64_t)Gen32(kh) << 32;
        }
        else in->w[k] = Rnd();
    }
    if (kind == 's') for (int k = 2; k < 4; ++k) { const uint64_t lo = (uint32_t)GenInt(); in->w[k] = lo | GenInt() << 32; }
    if (kind == 'd') for (int k = 2; k < 4; ++k) in->w[k] = GenInt();
    /* Cancellation and exact cases: operand 2 = +-operand 1, low bits nudged. */
    if (IsFpKind(kind) && Rnd() % 8 == 0) {
        in->w[4] = in->w[2]; in->w[5] = in->w[3];
        uint32_t w32[4];
        memcpy(w32, &in->w[4], 16);
        for (int k = 0; k < 4; ++k) {
            if (kind == 'S' || (k & 1)) w32[k] ^= (Rnd() & 1) ? 0x80000000u : 0;
            w32[k] ^= (uint32_t)(Rnd() % 3);
        }
        memcpy(&in->w[4], w32, 16);
    }
    in->w[8] = Rnd();
    if (kind == 'W') { const uint64_t lo = (uint32_t)GenInt(); in->w[9] = lo | Rnd() << 32; }
    else in->w[9] = GenInt();
    in->w[10] = Rnd() & 0xf0000000u;
}

static uint64_t RandomFs0(void) {
    uint64_t fs0 = (Rnd() % 4) ? 0x10 : 0; /* guest IXC already set, or not */
    if (Rnd() % 16 == 0) fs0 |= Rnd() & 0x9f;
    return fs0;
}

/* ---- L2 special grid ----------------------------------------------------- */

static const uint32_t S_SPEC[64] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
    0x7fc12345, 0xffe00001, 0x7f800001, 0xff800001, 0x7fa5a5a5, 0xffbfffff, 0x00000001, 0x80000001,
    0x007fffff, 0x807fffff, 0x00400000, 0x80000003, 0x00800000, 0x80800000, 0x00800001, 0x80800001,
    0x007ffffe, 0x00ffffff, 0x7f7fffff, 0xff7fffff, 0x7f7ffffe, 0x7f000000, 0x3f800000, 0xbf800000,
    0x3f800001, 0x3f7fffff, 0x40000000, 0x40400000, 0x3f000000, 0x3fc00000, 0xbfc00000, 0x40200000,
    0xc0200000, 0x3f400000, 0x4b000000, 0x4b800000, 0x4b7fffff, 0x4effffff, 0x4f000000, 0xcf000000,
    0xcf000001, 0x4f800000, 0x4f7fffff, 0x5f000000, 0xdf000000, 0x5f7fffff, 0x5f800000, 0x33800000,
    0x34000000, 0x0c000000, 0x1f800000, 0x40490fdb, 0x3eaaaaab, 0x00800002, 0x3effffff, 0x3f000001,
    0x5e800000, 0x80400000};
static const uint64_t D_SPEC[64] = {0, 0x8000000000000000ULL, 0x7ff0000000000000ULL,
    0xfff0000000000000ULL, 0x7ff8000000000000ULL, 0xfff8000000000000ULL, 0x7ff8123456789abcULL,
    0xfffc000000000001ULL, 0x7ff0000000000001ULL, 0xfff0000000000001ULL, 0x7ff4a5a5a5a5a5a5ULL,
    0xfff7ffffffffffffULL, 1, 0x8000000000000001ULL, 0x000fffffffffffffULL, 0x800fffffffffffffULL,
    0x0008000000000000ULL, 0x8000000000000003ULL, 0x0010000000000000ULL, 0x8010000000000000ULL,
    0x0010000000000001ULL, 0x8010000000000001ULL, 0x000ffffffffffffeULL, 0x001fffffffffffffULL,
    0x7fefffffffffffffULL, 0xffefffffffffffffULL, 0x7feffffffffffffeULL, 0x7fe0000000000000ULL,
    0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x3ff0000000000001ULL, 0x3fefffffffffffffULL,
    0x4000000000000000ULL, 0x4008000000000000ULL, 0x3fe0000000000000ULL, 0x3ff8000000000000ULL,
    0xbff8000000000000ULL, 0x4004000000000000ULL, 0xc004000000000000ULL, 0x3fe8000000000000ULL,
    0x4330000000000000ULL, 0x4340000000000000ULL, 0x433fffffffffffffULL, 0x41dfffffffc00000ULL,
    0x41e0000000000000ULL, 0xc1e0000000000000ULL, 0xc1e0000000200000ULL, 0x41f0000000000000ULL,
    0x41efffffffe00000ULL, 0x43e0000000000000ULL, 0xc3e0000000000000ULL, 0x43efffffffffffffULL,
    0x43f0000000000000ULL, 0x3ca0000000000000ULL, 0x3cb0000000000000ULL, 0x2000000000000000ULL,
    0x5ff0000000000000ULL, 0x400921fb54442d18ULL, 0x3fd5555555555555ULL, 0x0010000000000002ULL,
    0x3fdfffffffffffffULL, 0x3fe0000000000001ULL, 0x47efffffe0000000ULL, 0x36a0000000000000ULL};
static const uint64_t I_SPEC[64] = {0, 1, 2, 3, 0xffffffffffffffffULL, 0xfffffffffffffffeULL,
    0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0xfffffffe, 0x7fffffffffffffffULL,
    0x8000000000000000ULL, 0x8000000000000001ULL, 0x00ffffff, 0x01000000, 0x01000001, 0x01000002,
    0x01000003, 0x02000005, 0xff000001, 0xfeffffff, 0x001fffffffffffffULL, 0x0020000000000000ULL,
    0x0020000000000001ULL, 0x0020000000000003ULL, 0xffe0000000000001ULL, 0x7ffffffffffffe00ULL,
    0x7ffffffffffffc00ULL, 0xfffffffffffff800ULL, 0x00000000ffffff80ULL, 0x7fffffc0, 0x7fffff80,
    0x80000040, 0xffffff7f, 0xffffff80, 0x10000000000ULL, 0x123456789abcdefULL,
    0xfedcba9876543210ULL, 0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL, 0x100, 0xffffff00ULL,
    0x0000000080000080ULL, 0x40000001, 0xc0000001, 0x3ffffffffffffffULL, 0x4000000000000001ULL,
    0xc000000000000001ULL, 0x00000000fffffffdULL, 0xfffffffffffffffdULL, 0x7fffff, 0x800001,
    0xffffffff007fffffULL, 0x1fffffe, 0x3fffffe, 0x7ffffff, 0xfffffff0, 0xfffffff000000000ULL,
    0x0fffffffffffffffULL, 0xffffffff80000000ULL, 0x00000001ffffffffULL, 0x2a, 0xdeadbeef};

static void GridInput(unsigned op, unsigned a, unsigned b, unsigned z, unsigned variant, In* in) {
    const char kind = g_fpx_words[op].kind;
    memset(in, 0, sizeof *in);
    if (kind == 'S') {
        const uint64_t va = S_SPEC[a], vb = S_SPEC[b], vz = S_SPEC[z];
        in->w[0] = in->w[1] = in->w[6] = in->w[7] = vz | vz << 32;
        in->w[2] = in->w[3] = va | vb << 32;
        in->w[4] = in->w[5] = vb | va << 32;
    } else if (kind == 'D') {
        in->w[0] = in->w[1] = in->w[6] = in->w[7] = D_SPEC[z];
        in->w[2] = D_SPEC[a]; in->w[3] = D_SPEC[b];
        in->w[4] = D_SPEC[b]; in->w[5] = D_SPEC[a];
    } else if (kind == 's') {
        in->w[2] = (uint32_t)I_SPEC[a] | I_SPEC[b] << 32;
        in->w[3] = (uint32_t)I_SPEC[b] | I_SPEC[a] << 32;
    } else if (kind == 'd') {
        in->w[2] = I_SPEC[a]; in->w[3] = I_SPEC[b];
    }
    in->w[8] = 0x0123456789abcdefULL;
    in->w[9] = I_SPEC[a];
    in->w[10] = (uint64_t)((variant * 0x5u + a) & 15) << 28;
}

/* ---- L3 adversarial ------------------------------------------------------ */

static uint32_t Pack32(unsigned neg, int exp_unbiased, uint32_t mant24) {
    return (neg ? 0x80000000u : 0) | ((uint32_t)(exp_unbiased + 127) << 23) | (mant24 & 0x7fffff);
}
static uint64_t Pack64(unsigned neg, int exp_unbiased, uint64_t mant53) {
    return (neg ? 0x8000000000000000ULL : 0) | ((uint64_t)(exp_unbiased + 1023) << 52) |
           (mant53 & 0xfffffffffffffULL);
}
/* Integers A, B in [2^23, 2^24) with A*B = 2^47 + r, 0 < |r| < 2^18: the exact
   product is a power of two plus a sliver far below binary64 precision. */
static void NearPow2Product(uint32_t* A, uint32_t* B) {
    for (;;) {
        const uint64_t b = (1u << 23) | ((uint32_t)Rnd() & 0x7fffff);
        const uint64_t a = ((1ULL << 47) + b / 2) / b;
        const int64_t r = (int64_t)(a * b) - (int64_t)(1ULL << 47);
        if (a >= (1u << 23) && a < (1u << 24) && r != 0 && r > -(1 << 18) && r < (1 << 18)) {
            *A = (uint32_t)a; *B = (uint32_t)b;
            return;
        }
    }
}

/* One lane triple (a, b, z) for a single-precision op. */
static void Adv32(uint32_t* a, uint32_t* b, uint32_t* z) {
    const unsigned scenario = (unsigned)(Rnd() % 11);
    const unsigned sa = Rnd() & 1, sb = Rnd() & 1, sz = Rnd() & 1;
    const int E = (int)(Rnd() % 200) - 100;
    uint32_t A, B;
    switch (scenario) {
    case 0: /* a*b+z within 2^-60 relative of a binary32 midpoint (double rounding bait) */
    case 1: {
        NearPow2Product(&A, &B);
        const int scale = 24 + (int)(Rnd() % 2);          /* product 2^(E-24) or 2^(E-25) */
        const int ea = (int)(Rnd() % 20) - 10;
        const int eb = E - scale - 47 - ea;                /* a = A*2^ea, b = B*2^eb */
        *a = Pack32(sa, ea + 23, A);
        *b = Pack32(sb, eb + 23, B);
        *z = Pack32(sz, E, (uint32_t)(scenario ? Rnd() & 0x7fffff : 0));
        if (Rnd() & 1) { uint32_t t = *a; *a = *b; *b = t; }
        return;
    }
    case 2: /* step bait: a*b = 2^-24 or 2^-23 times (1 + r*2^-47), both signs */
        NearPow2Product(&A, &B);
        {
            const int scale = (Rnd() & 1) ? -24 : -23;
            const int ea = (int)(Rnd() % 20) - 10;
            *a = Pack32(sa, ea + 23, A);
            *b = Pack32(sb, scale - 47 - ea + 23, B);
            *z = Pack32(sz, 1, 0);
        }
        return;
    case 3: /* exact ties: product exactly half an ulp of z */
        *z = Pack32(sz, E, (uint32_t)Rnd());
        *a = Pack32(sa, (int)(Rnd() % 20) - 10, 0);
        *b = Pack32(sb, E - 24 - (int)((*a >> 23) & 255) + 127, 0);
        return;
    case 4: { /* results at and around the smallest normal (tininess boundary) */
        const uint32_t m = (uint32_t)Rnd() & 0x7fffff;
        const int ea = (int)(Rnd() % 60) - 30;
        *a = Pack32(sa, ea, m);
        /* b ~ 2^-126 / a, nudged by a few ulps. */
        *b = Pack32(sb, -126 - ea - (m ? 1 : 0), (uint32_t)(0x7fffff - (m >> 1)) + (uint32_t)(Rnd() % 7) - 3);
        *z = (Rnd() & 1) ? (uint32_t)(sz << 31) | (0x00800000u + (uint32_t)(Rnd() % 5) - 2)
                         : (uint32_t)(sz << 31) | ((uint32_t)Rnd() % 0x10);
        return;
    }
    case 5: { /* around FLT_MAX + ulp/2 */
        const uint32_t m = (uint32_t)Rnd() & 0x7fffff;
        const int ea = (int)(Rnd() % 60);
        *a = Pack32(sa, ea, m);
        *b = Pack32(sb, 127 - ea - (m ? 1 : 0), (uint32_t)(0x7fffff - (m >> 1)) + (uint32_t)(Rnd() % 7) - 3);
        *z = (uint32_t)(sz << 31) | (0x7f7fffffu - (uint32_t)(Rnd() % 4));
        return;
    }
    case 6: /* subnormal inputs with normal results (the DAZ hazard) */
        *a = (uint32_t)(sa << 31) | (((uint32_t)Rnd() & 0x7fffff) | 1);
        {
            const int eb = 24 + (int)(Rnd() % 100);
            *b = Pack32(sb, eb, (uint32_t)Rnd());
            if (Rnd() & 1) *z = (uint32_t)(sz << 31) | ((uint32_t)Rnd() & 0x7fffff);
            else { const int ez = (int)(Rnd() % 40) - 110; *z = Pack32(sz, ez, (uint32_t)Rnd()); }
        }
        if (Rnd() & 1) { uint32_t t = *a; *a = *b; *b = t; }
        return;
    case 7: /* inf * 0, and zero products with any addend */
        *a = (Rnd() & 1) ? (uint32_t)(sa << 31) | 0x7f800000u : (uint32_t)(sa << 31);
        *b = (Rnd() & 1) ? (uint32_t)(sb << 31) : (uint32_t)(sb << 31) | 0x7f800000u;
        *z = Gen32((int)(Rnd() % GEN_KINDS));
        return;
    case 8: /* tiny exact sums and differences */
        *a = (uint32_t)(sa << 31) | ((uint32_t)Rnd() % 0x01000000u);
        *b = (uint32_t)(sb << 31) | ((uint32_t)Rnd() % 0x01000000u);
        *z = (uint32_t)(sz << 31) | ((uint32_t)Rnd() % 0x01000000u);
        return;
    case 9: { /* normal operands whose sum or difference lands at, just above or
                 just below the smallest normal, or in the subnormal range (FZ) */
        float fx, fd, fy;
        const uint32_t x = Pack32(0, -126 + (int)(Rnd() % 6), (uint32_t)Rnd());
        const uint32_t d = (Rnd() & 1) ? 0x00800000u + (uint32_t)(Rnd() % 9) - 4
                                       : (uint32_t)Rnd() & 0x00ffffffu;
        uint32_t y;
        memcpy(&fx, &x, 4); memcpy(&fd, &d, 4);
        fy = (float)((double)fx - (double)fd); /* x - y is d, or within an ulp of y of it */
        memcpy(&y, &fy, 4);
        y &= 0x7fffffffu;
        *a = (uint32_t)(sa << 31) | x;
        *b = (uint32_t)(sb << 31) | (y >= 0x00800000u ? y : x);
        if (Rnd() & 1) { uint32_t t = *a; *a = *b; *b = t; }
        *z = (uint32_t)(sz << 31) | (Rnd() & 1 ? x : 0x00800000u);
        return;
    }
    default: { /* a*b+z cancelling into the subnormal range or onto the smallest
                  normal from either side, with normal operands (FZ) */
        float fa, fb, fz;
        const int ea = -1 - (int)(Rnd() % 50), ep = -126 + (int)(Rnd() % 6) - 1;
        *a = Pack32(sa, ea, (uint32_t)Rnd());
        *b = Pack32(sb, ep - ea, (uint32_t)Rnd());
        memcpy(&fa, a, 4); memcpy(&fb, b, 4);
        const double p = (double)fa * (double)fb; /* exact */
        const unsigned k = (unsigned)(Rnd() % 3);
        const double d = k == 0 ? 0.0 : ldexp(1.0 + ((double)(Rnd() % 9) - 4) * ldexp(1.0, -23), -126);
        fz = (float)(-p + (k == 2 ? -d : d));
        memcpy(z, &fz, 4);
        if (Rnd() & 1) *z += (uint32_t)(Rnd() % 5) - 2;
        if ((*z & 0x7f800000u) == 0 && (*z & 0x7fffffu)) *z = (*z & 0x80000000u) | 0x00800000u;
        return;
    }
    }
}
static void Adv64(uint64_t* a, uint64_t* b, uint64_t* z) {
    const unsigned scenario = (unsigned)(Rnd() % 8);
    const unsigned sa = Rnd() & 1, sb = Rnd() & 1, sz = Rnd() & 1;
    const uint64_t m = Rnd() & 0xfffffffffffffULL;
    const int ea = (int)(Rnd() % 400) - 200;
    switch (scenario) {
    case 0: /* smallest-normal boundary */
        *a = Pack64(sa, ea, m);
        *b = Pack64(sb, -1022 - ea - (m ? 1 : 0), (0xfffffffffffffULL - (m >> 1)) + Rnd() % 7 - 3);
        *z = (uint64_t)sz << 63 | (0x0010000000000000ULL + Rnd() % 5 - 2);
        return;
    case 1: /* overflow boundary */
        *a = Pack64(sa, ea < 0 ? -ea : ea, m);
        *b = Pack64(sb, 1023 - (ea < 0 ? -ea : ea) - (m ? 1 : 0), (0xfffffffffffffULL - (m >> 1)) + Rnd() % 7 - 3);
        *z = (uint64_t)sz << 63 | (0x7fefffffffffffffULL - Rnd() % 4);
        return;
    case 2: /* subnormal inputs, normal results */
        *a = (uint64_t)sa << 63 | (m | 1);
        { const int eb = 53 + (int)(Rnd() % 900); *b = Pack64(sb, eb, Rnd()); }
        *z = Gen64((int)(Rnd() % GEN_KINDS));
        return;
    case 3: /* inf * 0 */
        *a = (Rnd() & 1) ? (uint64_t)sa << 63 | 0x7ff0000000000000ULL : (uint64_t)sa << 63;
        *b = (Rnd() & 1) ? (uint64_t)sb << 63 : (uint64_t)sb << 63 | 0x7ff0000000000000ULL;
        *z = Gen64((int)(Rnd() % GEN_KINDS));
        return;
    case 4: /* exact ties against z */
        *z = Pack64(sz, ea, Rnd());
        *a = Pack64(sa, (int)(Rnd() % 20) - 10, 0);
        *b = Pack64(sb, ea - 53 - (int)((*a >> 52) & 2047) + 1023, 0);
        return;
    case 5: /* tiny exact sums */
        *a = (uint64_t)sa << 63 | (Rnd() % 0x0020000000000000ULL);
        *b = (uint64_t)sb << 63 | (Rnd() % 0x0020000000000000ULL);
        *z = (uint64_t)sz << 63 | (Rnd() % 0x0020000000000000ULL);
        return;
    case 6: { /* normal operands whose sum or difference lands around the smallest
                 normal or in the subnormal range (FZ) */
        double fx, fd, fy;
        const uint64_t x = Pack64(0, -1022 + (int)(Rnd() % 6), Rnd());
        const uint64_t d = (Rnd() & 1) ? 0x0010000000000000ULL + Rnd() % 9 - 4
                                       : Rnd() & 0x001fffffffffffffULL;
        uint64_t y;
        memcpy(&fx, &x, 8); memcpy(&fd, &d, 8);
        fy = fx - fd; /* x - y is d, or within an ulp of y of it */
        memcpy(&y, &fy, 8);
        y &= 0x7fffffffffffffffULL;
        *a = (uint64_t)sa << 63 | x;
        *b = (uint64_t)sb << 63 | (y >= 0x0010000000000000ULL ? y : x);
        if (Rnd() & 1) { const uint64_t t = *a; *a = *b; *b = t; }
        *z = (uint64_t)sz << 63 | (Rnd() & 1 ? x : 0x0010000000000000ULL);
        return;
    }
    default: { /* a*b+z cancelling into the subnormal range or onto the smallest
                  normal from either side, with normal operands (FZ) */
        double fa, fb, fz;
        const int eb = -1 - (int)(Rnd() % 200), ep = -1022 + (int)(Rnd() % 6) - 1;
        *a = Pack64(sa, ep - eb, Rnd());
        *b = Pack64(sb, eb, Rnd());
        memcpy(&fa, a, 8); memcpy(&fb, b, 8);
        const unsigned k = (unsigned)(Rnd() % 3);
        const double d = k == 0 ? 0.0 : ldexp(1.0 + ((double)(Rnd() % 9) - 4) * ldexp(1.0, -52), -1022);
        fz = -(fa * fb) + (k == 2 ? -d : d);
        memcpy(z, &fz, 8);
        if (Rnd() & 1) *z += Rnd() % 5 - 2;
        if ((*z & 0x7ff0000000000000ULL) == 0 && (*z & 0xfffffffffffffULL))
            *z = (*z & 0x8000000000000000ULL) | 0x0010000000000000ULL;
        return;
    }
    }
}

static int AdvApplies(unsigned op) {
    return IsFpKind(g_fpx_words[op].kind) && g_fpx_words[op].arity >= 2;
}

static void AdvInput(unsigned op, In* in) {
    uint32_t a32[4], b32[4], z32[4];
    uint64_t a64[2], b64[2], z64[2];
    RandomInput(op, in);
    if (g_fpx_words[op].kind == 'S') {
        for (int l = 0; l < 4; ++l) Adv32(&a32[l], &b32[l], &z32[l]);
        /* By-element forms read v2.s[1] for every lane: keep lane 1 of b with lane 0 of a. */
        if (Rnd() & 1) { b32[1] = b32[0]; }
        memcpy(&in->w[2], a32, 16);
        memcpy(&in->w[4], b32, 16);
        memcpy(&in->w[0], z32, 16);
        memcpy(&in->w[6], z32, 16);
    } else {
        for (int l = 0; l < 2; ++l) Adv64(&a64[l], &b64[l], &z64[l]);
        if (Rnd() & 1) { b64[1] = b64[0]; }
        memcpy(&in->w[2], a64, 16);
        memcpy(&in->w[4], b64, 16);
        memcpy(&in->w[0], z64, 16);
        memcpy(&in->w[6], z64, 16);
    }
}

/* ---- L4 exhaustive unary ------------------------------------------------- */

static int L4Applies(unsigned op) {
    static const char* const names[] = {"fsqrt s0, s1", "fcvt d0, s1", "fcvtzs w0, s1",
                                        "fcvtzu w0, s1", "scvtf s0, w1", "ucvtf s0, w1"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (!strcmp(g_fpx_words[op].text, names[i])) return 1;
    }
    return 0;
}
static void L4Input(unsigned op, uint32_t x, In* in) {
    memset(in, 0, sizeof *in);
    in->w[2] = x;
    in->w[9] = x | 0xa5a5a5a500000000ULL; /* the W forms must ignore the top half */
    (void)op;
}
static uint64_t L4Fs0(uint32_t x) { return ((x * 0x9E3779B1u) >> 30) ? 0x10 : 0; }

/* ---- options and legs ---------------------------------------------------- */

/* FZ (bit 24) alone, with DN and with each directed rounding mode. */
#define FPX_FZ (1u << 24)
static const uint64_t FPCRS[] = {0, FPX_FZ, 1u << 25, 1u << 22, 2u << 22, 3u << 22, FPX_FZ | (1u << 25),
                                 FPX_FZ | (1u << 22), FPX_FZ | (2u << 22), FPX_FZ | (3u << 22)};
#define NFPCR (sizeof FPCRS / sizeof FPCRS[0])

typedef struct {
    unsigned long cases, adv, l4_step;
    uint64_t seed;
    unsigned shard, shards;
    int legs[5];
    uint32_t only_word;
} Opts;

static int Selected(const Opts* o, unsigned op) {
    if (o->only_word && g_fpx_words[op].word != o->only_word) return 0;
    return op % o->shards == o->shard;
}

static void ParseOpts(int argc, char** argv, int first, Opts* o) {
    memset(o, 0, sizeof *o);
    o->cases = 20000; o->adv = 20000; o->l4_step = 4099; o->seed = 1; o->shards = 1;
    o->legs[1] = o->legs[2] = o->legs[3] = o->legs[4] = 1;
    for (int i = first; i < argc; ++i) {
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "--cases")) { o->cases = strtoul(v, 0, 0); ++i; }
        else if (!strcmp(argv[i], "--adv")) { o->adv = strtoul(v, 0, 0); ++i; }
        else if (!strcmp(argv[i], "--l4-step")) { o->l4_step = strtoul(v, 0, 0); ++i; }
        else if (!strcmp(argv[i], "--seed")) { o->seed = strtoull(v, 0, 0); ++i; }
        else if (!strcmp(argv[i], "--word")) { o->only_word = (uint32_t)strtoul(v, 0, 16); ++i; }
        else if (!strcmp(argv[i], "--shard")) { sscanf(v, "%u/%u", &o->shard, &o->shards); ++i; }
        else if (!strcmp(argv[i], "--legs")) {
            memset(o->legs, 0, sizeof o->legs);
            for (const char* p = v; *p; ++p) if (*p >= '1' && *p <= '4') o->legs[*p - '0'] = 1;
            ++i;
        } else { fprintf(stderr, "unknown option %s\n", argv[i]); exit(2); }
    }
    if (!o->shards || o->shard >= o->shards || !o->l4_step) { fprintf(stderr, "bad options\n"); exit(2); }
}

/* Calls fn(op, fpcr, leg, in, fs0, user) for every case of the selected legs. */
typedef void (*CaseFn)(unsigned op, uint64_t fpcr, unsigned leg, const In* in, uint64_t fs0, void* user);
static void ForEachCase(const Opts* o, unsigned op, unsigned f, CaseFn fn, void* user) {
    const uint64_t fpcr = FPCRS[f];
    const uint32_t word = g_fpx_words[op].word;
    In in;
    if (o->legs[1]) {
        Seed(o->seed, word, fpcr, 1);
        for (unsigned long t = 0; t < o->cases; ++t) {
            RandomInput(op, &in);
            fn(op, fpcr, 1, &in, RandomFs0(), user);
        }
    }
    if (o->legs[2]) {
        const unsigned arity = g_fpx_words[op].arity;
        const unsigned na = 64, nb = arity >= 2 ? 64 : 1, nz = arity >= 3 ? 64 : 1;
        for (unsigned a = 0; a < na; ++a)
            for (unsigned b = 0; b < nb; ++b)
                for (unsigned z = 0; z < nz; ++z)
                    for (unsigned variant = 0; variant < 2; ++variant) {
                        GridInput(op, a, b, z, variant, &in);
                        fn(op, fpcr, 2, &in, variant ? 0x10 : 0, user);
                    }
    }
    if (o->legs[3] && AdvApplies(op) && (f == 0 || (FPCRS[f] & FPX_FZ))) {
        Seed(o->seed, word, fpcr, 3);
        for (unsigned long t = 0; t < o->adv; ++t) {
            AdvInput(op, &in);
            fn(op, fpcr, 3, &in, (Rnd() % 4) ? 0x10 : 0, user);
        }
    }
    if (o->legs[4] && L4Applies(op) && (f == 0 || f == 1)) {
        for (uint64_t x = 0; x <= 0xffffffffULL; x += o->l4_step) {
            L4Input(op, (uint32_t)x, &in);
            fn(op, fpcr, 4, &in, L4Fs0((uint32_t)x), user);
        }
    }
}

/* ---- diff ---------------------------------------------------------------- */

typedef struct {
    int ref, cand[I_COUNT], ncand;
    unsigned long long cases[5], bad_v[I_COUNT][5], bad_s[I_COUNT][5];
    unsigned long long hit, miss;   /* fpx probe at FPCR 0 with IXC set, L1 */
    unsigned long long hit_fz, miss_fz; /* the same at FPCR FZ, L1 and L3 */
    unsigned shown;
    int poison;                     /* 1: poison the host FP mode around every fpx call */
} DiffState;

static void PoisonHostFp(int on);

static void DiffCase(unsigned op, uint64_t fpcr, unsigned leg, const In* in, uint64_t fs0, void* user) {
    DiffState* s = (DiffState*)user;
    Out ref, got;
    Run(s->ref, op, in, fpcr, fs0, &ref);
    ++s->cases[leg];
    for (int k = 0; k < s->ncand; ++k) {
        const int impl = s->cand[k];
#ifdef FPX_HAVE_FPX
        const unsigned long long h0 = g_fpx_probe[1], m0 = g_fpx_probe[2];
        if (s->poison) PoisonHostFp(1);
#endif
        Run(impl, op, in, fpcr, fs0, &got);
#ifdef FPX_HAVE_FPX
        if (s->poison) PoisonHostFp(0);
        if (impl == I_FPX && leg == 1 && fpcr == 0 && (fs0 & 0x10)) {
            s->hit += g_fpx_probe[1] - h0;
            s->miss += g_fpx_probe[2] - m0;
        }
        if (impl == I_FPX && (leg == 1 || leg == 3) && fpcr == FPX_FZ && (fs0 & 0x10)) {
            s->hit_fz += g_fpx_probe[1] - h0;
            s->miss_fz += g_fpx_probe[2] - m0;
        }
#endif
        const int bv = !SameValue(&ref, &got), bs = ref.fpsr != got.fpsr;
        s->bad_v[impl][leg] += bv;
        s->bad_s[impl][leg] += bs;
        if ((bv || bs) && s->shown < 4) {
            ++s->shown;
            printf("  %s L%u %-26s fpcr=%08llx fs0=%02llx in q1=%016llx:%016llx q2=%016llx:%016llx"
                   " q0=%016llx:%016llx x1=%016llx nzcv=%llx\n"
                   "     %-6s %016llx:%016llx x0=%016llx nzcv=%llx fpsr=%02llx | %-6s %016llx:%016llx"
                   " x0=%016llx nzcv=%llx fpsr=%02llx\n",
                   kImplNames[impl], leg, g_fpx_words[op].text, (unsigned long long)fpcr,
                   (unsigned long long)fs0, (unsigned long long)in->w[3], (unsigned long long)in->w[2],
                   (unsigned long long)in->w[5], (unsigned long long)in->w[4],
                   (unsigned long long)in->w[1], (unsigned long long)in->w[0],
                   (unsigned long long)in->w[9], (unsigned long long)in->w[10] >> 28,
                   kImplNames[impl], (unsigned long long)got.q0[1], (unsigned long long)got.q0[0],
                   (unsigned long long)got.x0, (unsigned long long)got.nzcv >> 28,
                   (unsigned long long)got.fpsr, kImplNames[s->ref], (unsigned long long)ref.q0[1],
                   (unsigned long long)ref.q0[0], (unsigned long long)ref.x0,
                   (unsigned long long)ref.nzcv >> 28, (unsigned long long)ref.fpsr);
        }
    }
}

/* Returns the total value+FPSR mismatch count over all candidates. */
static unsigned long long Diff(const Opts* o, int ref, const int* cands, int ncand, int poison,
                               unsigned long long* hits_out, unsigned long long* miss_out) {
    unsigned long long total_cases = 0, total_bad = 0, hits = 0, misses = 0, hits_fz = 0, misses_fz = 0;
    unsigned long long leg_cases[5] = {0}, leg_bad_v[I_COUNT][5], leg_bad_s[I_COUNT][5];
    memset(leg_bad_v, 0, sizeof leg_bad_v);
    memset(leg_bad_s, 0, sizeof leg_bad_s);
    for (unsigned op = 0; op < FPX_NWORDS; ++op) {
        if (!Selected(o, op)) continue;
        DiffState s;
        memset(&s, 0, sizeof s);
        s.ref = ref; s.ncand = ncand; s.poison = poison;
        memcpy(s.cand, cands, sizeof(int) * (size_t)ncand);
        for (unsigned f = 0; f < NFPCR; ++f) ForEachCase(o, op, f, DiffCase, &s);
        unsigned long long op_cases = 0;
        for (int leg = 1; leg <= 4; ++leg) { op_cases += s.cases[leg]; leg_cases[leg] += s.cases[leg]; }
        total_cases += op_cases;
        for (int k = 0; k < ncand; ++k) {
            const int impl = cands[k];
            unsigned long long bv = 0, bs = 0;
            for (int leg = 1; leg <= 4; ++leg) {
                bv += s.bad_v[impl][leg]; bs += s.bad_s[impl][leg];
                leg_bad_v[impl][leg] += s.bad_v[impl][leg]; leg_bad_s[impl][leg] += s.bad_s[impl][leg];
            }
            total_bad += bv + bs;
            if (bv || bs)
                printf("%08x %-26s %s vs %s: value %llu fpsr %llu of %llu\n", g_fpx_words[op].word,
                       g_fpx_words[op].text, kImplNames[impl], kImplNames[ref], bv, bs, op_cases);
        }
        if (s.hit + s.miss)
            printf("%08x %-26s fpx hit %.2f%% (%llu of %llu lane ops, L1 FPCR 0, IXC set)\n",
                   g_fpx_words[op].word, g_fpx_words[op].text, 100.0 * (double)s.hit / (double)(s.hit + s.miss),
                   s.hit, s.hit + s.miss);
        if (s.hit_fz + s.miss_fz)
            printf("%08x %-26s fpx hit FZ %.2f%% (%llu of %llu lane ops, L1+L3 FPCR FZ, IXC set)\n",
                   g_fpx_words[op].word, g_fpx_words[op].text,
                   100.0 * (double)s.hit_fz / (double)(s.hit_fz + s.miss_fz), s.hit_fz, s.hit_fz + s.miss_fz);
        hits += s.hit; misses += s.miss; hits_fz += s.hit_fz; misses_fz += s.miss_fz;
        fflush(stdout);
    }
    for (int k = 0; k < ncand; ++k)
        for (int leg = 1; leg <= 4; ++leg)
            if (leg_cases[leg])
                printf("LEG L%d %s vs %s: cases %llu value-mismatch %llu fpsr-mismatch %llu\n", leg,
                       kImplNames[cands[k]], kImplNames[ref], leg_cases[leg], leg_bad_v[cands[k]][leg],
                       leg_bad_s[cands[k]][leg]);
    printf("TOTAL cases %llu mismatches %llu\n", total_cases, total_bad);
    if (hits + misses + hits_fz + misses_fz)
        printf("HITS fpcr0 %llu %llu fz %llu %llu\n", hits, hits + misses, hits_fz, hits_fz + misses_fz);
    if (hits_out) *hits_out = hits;
    if (miss_out) *miss_out = misses;
    return total_bad;
}

/* ---- hashes (L5) --------------------------------------------------------- */

typedef struct { int impl; uint64_t h[5]; } HashState;
static void Fnv(uint64_t* h, uint64_t v) {
    for (int i = 0; i < 8; ++i) { *h ^= (v >> (8 * i)) & 0xff; *h *= 0x100000001b3ULL; }
}
static void HashCase(unsigned op, uint64_t fpcr, unsigned leg, const In* in, uint64_t fs0, void* user) {
    HashState* s = (HashState*)user;
    Out out;
    Run(s->impl, op, in, fpcr, fs0, &out);
    Fnv(&s->h[leg], out.q0[0]); Fnv(&s->h[leg], out.q0[1]); Fnv(&s->h[leg], out.x0);
    Fnv(&s->h[leg], out.nzcv | out.fpsr);
}
/* Writes "L<leg> <word> <fpcr> <hash>" lines for legs 1, 2 and 4 (L3 is host-built input). */
static void Hashes(const Opts* o, int impl, FILE* out) {
    Opts h = *o;
    h.legs[3] = 0;
    for (unsigned op = 0; op < FPX_NWORDS; ++op) {
        if (!Selected(&h, op)) continue;
        for (unsigned f = 0; f < NFPCR; ++f) {
            HashState s;
            s.impl = impl;
            for (int leg = 0; leg < 5; ++leg) s.h[leg] = 0xcbf29ce484222325ULL;
            ForEachCase(&h, op, f, HashCase, &s);
            for (int leg = 1; leg <= 4; ++leg) {
                if (!h.legs[leg] || leg == 3 || (leg == 4 && !(L4Applies(op) && f < 2))) continue;
                fprintf(out, "L%d %08x %08llx %016llx\n", leg, g_fpx_words[op].word,
                        (unsigned long long)FPCRS[f], (unsigned long long)s.h[leg]);
            }
        }
        fflush(out);
    }
}

static int Check(const Opts* o, const char* path) {
    FILE* golden = fopen(path, "r");
    if (!golden) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    char line[128];
    unsigned long long lines = 0, bad = 0;
    static char mine[2][1 << 22];
    for (int k = 0; k < 2; ++k) {
        const int impl = k ? I_FPX : I_SOFT;
        FILE* mem = tmpfile();
        if (!ImplAvailable(impl)) { mine[k][0] = 0; if (mem) fclose(mem); continue; }
        if (!mem) { fprintf(stderr, "tmpfile failed\n"); return 2; }
        Hashes(o, impl, mem);
        rewind(mem);
        const size_t n = fread(mine[k], 1, sizeof mine[k] - 1, mem);
        mine[k][n] = 0;
        fclose(mem);
    }
    unsigned long long present = 0;
    while (fgets(line, sizeof line, golden)) {
        unsigned leg; unsigned word; unsigned long long fpcr, hash;
        if (sscanf(line, "L%u %x %llx %llx", &leg, &word, &fpcr, &hash) != 4) continue;
        unsigned op = 0;
        while (op < FPX_NWORDS && g_fpx_words[op].word != word) ++op;
        if (op == FPX_NWORDS || !Selected(o, op) || !o->legs[leg]) continue;
        ++lines;
        for (int k = 0; k < 2; ++k) {
            if (!mine[k][0]) continue;
            char key[64];
            snprintf(key, sizeof key, "L%u %08x %08llx ", leg, word, fpcr);
            const char* at = strstr(mine[k], key);
            ++present;
            if (!at || strtoull(at + strlen(key), 0, 16) != hash) {
                ++bad;
                printf("MISMATCH %s L%u %08x %-26s fpcr=%08llx\n", k ? "fpx" : "soft", leg, word,
                       g_fpx_words[op].text, fpcr);
            }
        }
    }
    fclose(golden);
    printf("CHECK %llu golden lines, %llu comparisons, %llu mismatches\n", lines, present, bad);
    return bad || !lines ? 1 : 0;
}

/* ---- host FP mode -------------------------------------------------------- */

static void PoisonHostFp(int on) {
#if defined(FPX_X86)
    /* FTZ, DAZ and round-toward-zero, every exception masked. */
    _mm_setcsr(on ? 0x1F80u | 0x8000u | 0x0040u | 0x6000u : 0x1F80u);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    const uint64_t v = on ? (1u << 24) | (3u << 22) : 0;
    __asm__ volatile("msr fpcr,%0" ::"r"(v));
#else
    (void)on;
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: see driver.c\n"); return 2; }
    Opts o;
    const char* mode = argv[1];
    if (!strcmp(mode, "hash")) {
        if (argc < 3) return 2;
        int impl = -1;
        for (int k = 0; k < I_COUNT; ++k) if (!strcmp(argv[2], kImplNames[k])) impl = k;
        if (impl < 0 || !ImplAvailable(impl)) { fprintf(stderr, "implementation unavailable\n"); return 2; }
        ParseOpts(argc, argv, 3, &o);
        Hashes(&o, impl, stdout);
        return 0;
    }
    if (!strcmp(mode, "check")) {
        if (argc < 3) return 2;
        ParseOpts(argc, argv, 3, &o);
        return Check(&o, argv[2]);
    }
    if (!strcmp(mode, "diff")) {
        ParseOpts(argc, argv, 2, &o);
        const int ref = ImplAvailable(I_HW) ? I_HW : I_SOFT;
        int cands[2], n = 0;
        if (ref == I_HW) cands[n++] = I_SOFT;
        if (ImplAvailable(I_FPX)) cands[n++] = I_FPX;
        if (!n) { printf("nothing to compare: soft is the only implementation here\n"); return 0; }
        unsigned long long hits = 0, misses = 0;
        const unsigned long long bad = Diff(&o, ref, cands, n, 0, &hits, &misses);
        if (hits + misses)
            printf("FPX hit rate %.2f%% (L1, FPCR 0, IXC set)\n", 100.0 * (double)hits / (double)(hits + misses));
        return bad ? 1 : 0;
    }
#ifdef FPX_HAVE_FPX
    if (!strcmp(mode, "control")) {
        if (argc < 3) return 2;
        ParseOpts(argc, argv, 3, &o);
        const char* name = argv[2];
        unsigned long long bad;
        if (!strcmp(name, "nokeep")) { const int c[] = {I_NOKEEP}; bad = Diff(&o, I_SOFT, c, 1, 0, 0, 0); }
        else if (!strcmp(name, "nomid")) { const int c[] = {I_NOMID}; bad = Diff(&o, I_SOFT, c, 1, 0, 0, 0); }
        else if (!strcmp(name, "nofz")) { const int c[] = {I_NOFZ}; bad = Diff(&o, I_SOFT, c, 1, 0, 0, 0); }
        else if (!strcmp(name, "mxcsr")) { const int c[] = {I_FPX}; bad = Diff(&o, I_SOFT, c, 1, 1, 0, 0); }
        else { fprintf(stderr, "unknown control\n"); return 2; }
        /* The verdict needs every shard's count; run.py adds them up. */
        printf("CONTROL %s: %llu mismatches\n", name, bad);
        return 0;
    }
    if (!strcmp(mode, "shadow")) {
        /* Shadow results are the exact body's by construction; its own
           comparison goes to SUYU_RECOMP_FPX_SHADOW_LOG(.sum) at exit. */
        ParseOpts(argc, argv, 2, &o);
        const int c[] = {I_SHADOW};
        return Diff(&o, I_SOFT, c, 1, 0, 0, 0) ? 1 : 0;
    }
    if (!strcmp(mode, "inhibit")) {
        /* With bit 32 of fpcr set every op must take the exact body: same
           result and FPSR as soft at FPCR 0, no fast-path hit, the bit kept,
           and MRS/MSR FPCR must neither expose nor clear it. */
        const uint64_t inhibit = 1ULL << 32;
        unsigned long long cases = 0, bad = 0;
        ParseOpts(argc, argv, 2, &o);
        for (unsigned op = 0; op < FPX_NWORDS; ++op) {
            if (!Selected(&o, op)) continue;
            In in;
            Out ref, got;
            Seed(o.seed, g_fpx_words[op].word, 0, 9);
            for (unsigned long t = 0; t < o.cases; ++t) {
                RandomInput(op, &in);
                const uint64_t fs0 = RandomFs0() | 0x10;
                const unsigned long long hits = g_fpx_probe[1];
                Run(I_SOFT, op, &in, 0, fs0, &ref);
                Run(I_FPX, op, &in, inhibit, fs0, &got);
                int ok = SameValue(&ref, &got) && ref.fpsr == got.fpsr && g_fpx_probe[1] == hits &&
                         (got.fpcr & inhibit);
                if (!strncmp(g_fpx_words[op].text, "msr fpcr", 8))
                    ok = ok && got.fpcr == ((in.w[9] & 0xffffffffULL) | inhibit);
                if (!strncmp(g_fpx_words[op].text, "mrs", 3)) ok = ok && got.x0 == 0;
                ++cases;
                if (!ok && bad++ < 4)
                    printf("  inhibit %s: x0 %016llx fpcr %016llx\n", g_fpx_words[op].text,
                           (unsigned long long)got.x0, (unsigned long long)got.fpcr);
            }
        }
        printf("INHIBIT cases %llu mismatches %llu\n", cases, bad);
        return bad ? 1 : 0;
    }
#endif
#ifdef FPX_HAVE_ENV
    if (!strcmp(mode, "env")) {
        /* L6(d): poison the host mode, let the host's check repair it, then run. */
        ParseOpts(argc, argv, 2, &o);
        PoisonHostFp(1);
        const int repaired = fpx_env_shim();
        const int again = fpx_env_shim();
        const int c[] = {I_FPX};
        const unsigned long long bad = Diff(&o, I_SOFT, c, 1, 0, 0, 0);
        printf("ENV repaired=%d second-call=%d mismatches=%llu\n", repaired, again, bad);
        PoisonHostFp(0);
        return (repaired == 1 && again == 0 && !bad) ? 0 : 1;
    }
#endif
    fprintf(stderr, "unknown or unavailable mode %s\n", mode);
    return 2;
}
