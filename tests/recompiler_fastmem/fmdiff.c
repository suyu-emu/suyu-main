/* Differential test of the ABI 6 (FM1) memory helpers.

   Three legs run the same random access sequence on identical memory:
     0  ABI 6 helpers with the fast path on (fm_limit set)
     1  ABI 6 helpers with fm_limit 0, i.e. only the recomp_*_slow path
     2  the pristine ABI 5 helpers, emitted with the option off
   Every return value, the final guest memory and the full host callback log
   (every call, in order, with its arguments) must be identical.

   Guest pages sit in host pages with an inaccessible page after each one, so
   a read or write past the end of a guest page faults instead of silently
   touching a neighbour. Page types, the address-space limit (page aligned or
   not) and the addresses are random, biased towards page ends, odd offsets,
   the limit, high and tagged bits, and wrap-around.

   usage: fmdiff [operations] [seed]   (defaults 20000000, 1) */
#include "recomp_runtime.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* The ABI 5 helpers read nothing in GuestContext past host_mem, and ABI 6
   only appends fields after chain_budget, so they can share one context. */
uint64_t abi5_recomp_load8(GuestContext*, uint64_t);
uint64_t abi5_recomp_load16(GuestContext*, uint64_t);
uint64_t abi5_recomp_load32(GuestContext*, uint64_t);
uint64_t abi5_recomp_load64(GuestContext*, uint64_t);
void abi5_recomp_store8(GuestContext*, uint64_t, uint64_t);
void abi5_recomp_store16(GuestContext*, uint64_t, uint64_t);
void abi5_recomp_store32(GuestContext*, uint64_t, uint64_t);
void abi5_recomp_store64(GuestContext*, uint64_t, uint64_t);
void abi5_recomp_ldp32(GuestContext*, uint64_t, uint64_t*, uint64_t*);
void abi5_recomp_ldp64(GuestContext*, uint64_t, uint64_t*, uint64_t*);
void abi5_recomp_stp32(GuestContext*, uint64_t, uint64_t, uint64_t);
void abi5_recomp_stp64(GuestContext*, uint64_t, uint64_t, uint64_t);

typedef struct {
    uint64_t (*load[4])(GuestContext*, uint64_t);
    void (*store[4])(GuestContext*, uint64_t, uint64_t);
    void (*ldp[2])(GuestContext*, uint64_t, uint64_t*, uint64_t*);
    void (*stp[2])(GuestContext*, uint64_t, uint64_t, uint64_t);
} Helpers;
static const Helpers abi6 = {
    {recomp_load8, recomp_load16, recomp_load32, recomp_load64},
    {recomp_store8, recomp_store16, recomp_store32, recomp_store64},
    {recomp_ldp32, recomp_ldp64},
    {recomp_stp32, recomp_stp64},
};
static const Helpers abi5 = {
    {abi5_recomp_load8, abi5_recomp_load16, abi5_recomp_load32, abi5_recomp_load64},
    {abi5_recomp_store8, abi5_recomp_store16, abi5_recomp_store32, abi5_recomp_store64},
    {abi5_recomp_ldp32, abi5_recomp_ldp64},
    {abi5_recomp_stp32, abi5_recomp_stp64},
};

enum { T_UNMAPPED = 0, T_MEMORY = 1, T_DEBUG = 2, T_CACHED = 3 };
typedef struct { uintptr_t ptr; uint64_t block, addr, pad; } PageEntry;
#define GUEST_PAGES 64
#define FIRST_PAGE 0x100
#define TABLE_PAGES (FIRST_PAGE + GUEST_PAGES)
#define OPS 4096
static PageEntry table[TABLE_PAGES];
static unsigned char* slot[GUEST_PAGES];
static unsigned char* backing[TABLE_PAGES];
static uint64_t snapshot[GUEST_PAGES][4096 / 8];
static unsigned char image[3][GUEST_PAGES][4096];

static uint64_t rng_state;
static uint64_t rnd(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(0x2545F4914F6CDD1D);
}

/* ---- the emulator side, as Memory::Read/Write behave ---- */
static unsigned char* model_byte(uint64_t va) {
    va &= UINT64_C(0xffffffffffff);
    return (va >> 12) < TABLE_PAGES && backing[va >> 12] ? backing[va >> 12] + (va & 0xfff)
                                                          : NULL;
}
typedef struct { uint64_t va, value; uint32_t size, kind; } Event;
static Event log_events[3][OPS * 2 + 8];
static unsigned log_count[3];
static int leg;
static void log_event(uint32_t kind, uint64_t va, uint32_t size, uint64_t value) {
    Event* e = &log_events[leg][log_count[leg]++];
    e->va = va;
    e->value = value;
    e->size = size;
    e->kind = kind;
}
static uint64_t host_load(void* user, uint64_t va, uint32_t size) {
    uint64_t v = 0;
    uint32_t i;
    (void)user;
    for (i = 0; i < size; ++i) {
        const unsigned char* p = model_byte(va + i);
        v |= (uint64_t)(p ? *p : 0) << (8 * i);
    }
    log_event(1, va, size, v);
    return v;
}
static void host_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    uint32_t i;
    (void)user;
    log_event(2, va, size, value);
    for (i = 0; i < size; ++i) {
        unsigned char* p = model_byte(va + i);
        if (p) *p = (unsigned char)(value >> (8 * i));
    }
}

/* ---- guest pages with a fault page after each ---- */
static int map_slots(void) {
    size_t host_page = 4096, i;
    unsigned char* base;
#ifdef _WIN32
    SYSTEM_INFO info;
    DWORD old;
    GetSystemInfo(&info);
    if (info.dwPageSize > host_page) host_page = info.dwPageSize;
    base = (unsigned char*)VirtualAlloc(NULL, host_page * (2 * GUEST_PAGES + 1),
                                        MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!base) return 0;
    for (i = 0; i < GUEST_PAGES; ++i) {
        if (!VirtualProtect(base + host_page * (2 * i + 1), host_page, PAGE_READWRITE, &old))
            return 0;
    }
#else
    const long size = sysconf(_SC_PAGESIZE);
    if (size > (long)host_page) host_page = (size_t)size;
    base = (unsigned char*)mmap(NULL, host_page * (2 * GUEST_PAGES + 1), PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return 0;
    for (i = 0; i < GUEST_PAGES; ++i) {
        if (mprotect(base + host_page * (2 * i + 1), host_page, PROT_READ | PROT_WRITE))
            return 0;
    }
#endif
    /* The 4 KiB guest page ends where the fault page begins. Guest pages are
       assigned to host slots in a scrambled order, so no two neighbouring
       guest pages are neighbours in host memory. */
    for (i = 0; i < GUEST_PAGES; ++i) {
        slot[i] = base + host_page * (2 * ((i * 37 + 11) % GUEST_PAGES) + 2) - 4096;
    }
    return 1;
}

static RecompHostMem bridge;
static GuestContext context;

typedef struct { unsigned kind; uint64_t va, v0, v1; } Op;
static Op ops[OPS];
static uint64_t results[3][OPS][2];

static void configure(void) {
    unsigned i;
    uint64_t asmax;
    memset(table, 0, sizeof(table));
    memset(backing, 0, sizeof(backing));
    for (i = 0; i < GUEST_PAGES; ++i) {
        const unsigned page = FIRST_PAGE + i;
        const uint64_t r = rnd() % 20;
        unsigned type = r < 16 ? T_MEMORY : r == 16 ? T_UNMAPPED : r == 17 ? T_DEBUG : T_CACHED;
        unsigned w;
        for (w = 0; w < 4096 / 8; ++w) snapshot[i][w] = rnd();
        if (type != T_UNMAPPED) backing[page] = slot[i];
        table[page].ptr = (type == T_MEMORY ? (uintptr_t)slot[i] - (uintptr_t)page * 0x1000 : 0) | type;
        table[page].block = rnd();
        table[page].addr = rnd();
    }
    switch (rnd() % 4) {
    case 0: asmax = (uint64_t)TABLE_PAGES << 12; break;
    case 1: asmax = (FIRST_PAGE + 1 + rnd() % GUEST_PAGES) << 12; break;
    default: asmax = (FIRST_PAGE << 12) + 1 + rnd() % ((uint64_t)GUEST_PAGES << 12); break;
    }
    bridge.address_space_max = asmax;
}

static uint64_t random_address(unsigned size) {
    const uint64_t asmax = bridge.address_space_max;
    const uint64_t pick = rnd() % 100;
    uint64_t page = FIRST_PAGE + rnd() % GUEST_PAGES, off, va;
    if (pick < 3) page = FIRST_PAGE - 1;
    else if (pick < 8) page = (asmax - 1) >> 12;
    switch (rnd() % 8) {
    case 0: case 1: case 2: off = 0xff0 + rnd() % 16; break;
    case 3: off = (rnd() % 4096) | 1; break;
    case 4: off = (rnd() % 4096) & ~(uint64_t)(size - 1); break;
    case 5: off = rnd() % 16; break;
    default: off = rnd() % 4096; break;
    }
    va = (page << 12) + off;
    switch (rnd() % 40) {
    case 0: va |= (rnd() & 0xff) << 56; break;             /* tagged top byte */
    case 1: va |= (rnd() & 0xffff) << 48; break;           /* any top 16 bits */
    case 2: va |= ((rnd() & 0x1ff) | 1) << 39; break;      /* bits 39-47 */
    case 3: va = 0 - 1 - rnd() % 24; break;                /* near 2^64 */
    case 4: va = asmax - 1 - rnd() % 24; break;            /* just below the limit */
    case 5: va = asmax + rnd() % 8; break;                 /* at or above it */
    case 6: va = ((asmax - 1) & ~UINT64_C(0xfff)) - rnd() % 24; break;
    default: break;
    }
    return va;
}

static void make_ops(void) {
    static const unsigned sizes[12] = {1, 2, 4, 8, 1, 2, 4, 8, 8, 16, 8, 16};
    unsigned i;
    for (i = 0; i < OPS; ++i) {
        ops[i].kind = (unsigned)(rnd() % 12);
        ops[i].va = random_address(sizes[ops[i].kind]);
        ops[i].v0 = rnd();
        ops[i].v1 = rnd();
    }
}

static void run_leg(int which, const Helpers* h, uint64_t fm_limit) {
    unsigned i;
    leg = which;
    log_count[which] = 0;
    for (i = 0; i < GUEST_PAGES; ++i) memcpy(slot[i], snapshot[i], 4096);
    context.fm_table = (const unsigned char*)table;
    context.fm_limit = fm_limit;
    for (i = 0; i < OPS; ++i) {
        const Op* op = &ops[i];
        uint64_t* r = results[which][i];
        r[0] = r[1] = 0;
        if (op->kind < 4) r[0] = h->load[op->kind](&context, op->va);
        else if (op->kind < 8) h->store[op->kind - 4](&context, op->va, op->v0);
        else if (op->kind < 10) h->ldp[op->kind - 8](&context, op->va, &r[0], &r[1]);
        else h->stp[op->kind - 10](&context, op->va, op->v0, op->v1);
    }
    for (i = 0; i < GUEST_PAGES; ++i) memcpy(image[which][i], slot[i], 4096);
}

static int compare(int a, int b, uint64_t iteration) {
    unsigned i;
    for (i = 0; i < OPS; ++i) {
        if (results[a][i][0] != results[b][i][0] || results[a][i][1] != results[b][i][1]) {
            fprintf(stderr, "MISMATCH iter %" PRIu64 " op %u kind %u va %#" PRIx64
                    ": leg %d %#" PRIx64 "/%#" PRIx64 " vs leg %d %#" PRIx64 "/%#" PRIx64 "\n",
                    iteration, i, ops[i].kind, ops[i].va, a, results[a][i][0], results[a][i][1],
                    b, results[b][i][0], results[b][i][1]);
            return 1;
        }
    }
    if (log_count[a] != log_count[b] ||
        memcmp(log_events[a], log_events[b], sizeof(Event) * log_count[a]) != 0) {
        fprintf(stderr, "MISMATCH iter %" PRIu64 ": callback log, leg %d %u events vs leg %d %u\n",
                iteration, a, log_count[a], b, log_count[b]);
        return 1;
    }
    if (memcmp(image[a], image[b], sizeof(image[a])) != 0) {
        fprintf(stderr, "MISMATCH iter %" PRIu64 ": memory image, leg %d vs %d\n", iteration, a, b);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    const uint64_t total = argc > 1 ? strtoull(argv[1], NULL, 0) : UINT64_C(20000000);
    uint64_t iteration, done = 0, fast_eligible = 0, callbacks = 0;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
    if (!rng_state) rng_state = 1;
    if (!map_slots()) {
        fprintf(stderr, "could not map guest pages\n");
        return 2;
    }
    if (RECOMP_FM_PAGE_BITS != 12 || RECOMP_FM_STRIDE_LOG2 != 5 || sizeof(PageEntry) != 32 ||
        RECOMP_FM_PTR_MASK != ~(uintptr_t)3) {
        fprintf(stderr, "FM1 constants do not match this test's table\n");
        return 2;
    }
    bridge.load = host_load;
    bridge.store = host_store;
    bridge.page_entries = table;
    bridge.page_entry_stride = sizeof(PageEntry);
    bridge.page_bits = 12;
    bridge.pointer_mask = ~(uint64_t)3;
    context.host_mem = &bridge;
    context.pending_svc = ~UINT64_C(0);
    for (iteration = 0; done < total; ++iteration) {
        unsigned i;
        uint64_t fm_limit;
        configure();
        make_ops();
        /* What the host sets: the limit rounded down to a page. */
        fm_limit = bridge.address_space_max & ~UINT64_C(0xfff);
        run_leg(0, &abi6, fm_limit);
        run_leg(1, &abi6, 0);
        run_leg(2, &abi5, 0);
        if (compare(0, 1, iteration) || compare(0, 2, iteration)) return 1;
        for (i = 0; i < OPS; ++i) {
            static const unsigned sizes[12] = {1, 2, 4, 8, 1, 2, 4, 8, 8, 16, 8, 16};
            const uint64_t va = ops[i].va;
            if (va < fm_limit && (va & 0xfff) + sizes[ops[i].kind] <= 0x1000 &&
                (table[va >> 12].ptr & ~(uintptr_t)3)) {
                ++fast_eligible;
            }
        }
        callbacks += log_count[0];
        done += OPS;
    }
    printf("fmdiff: %" PRIu64 " operations x 3 legs, %" PRIu64 " iterations, 0 mismatches; "
           "%" PRIu64 " served by the fast path, %" PRIu64 " host callbacks per leg\n",
           done, iteration, fast_eligible, callbacks);
    return 0;
}
