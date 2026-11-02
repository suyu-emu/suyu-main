#include "recomp_runtime.h"
#include "code.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned recomp_image_abi(void);
extern unsigned recomp_image_guard_v2(unsigned);
extern void recomp_image_set_base(uint64_t);
extern void recomp_image_run_slice(GuestContext*);
extern BlockFn recomp_image_lookup(uint64_t);
#ifdef RECOMP_FEATURE_FASTMEM_PT1
extern unsigned recomp_image_features(void);
extern unsigned recomp_image_fastmem_v1(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t);
#endif
#ifdef RECOMP_FEATURE_GUARD_GEN1
extern uint32_t* recomp_image_guard_gen_v1(uint32_t, uint64_t*, uint64_t*, const uint64_t**);
#endif
#ifdef RECOMP_FEATURE_FPX1
extern unsigned recomp_image_fpx_v1(uint32_t, uint32_t, uint64_t);
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Shaped like Common::PageTable's entries: 32 bytes, a pointer part of
   (host page - guest page) with the page type in the low two bits. */
enum { T_UNMAPPED = 0, T_MEMORY = 1, T_DEBUG = 2, T_CACHED = 3 };
typedef struct { uintptr_t ptr; uint64_t block, addr, pad; } PageEntry;
#define PAGE_COUNT 256 /* address_space_max 0x100000 */
static PageEntry table[PAGE_COUNT];
/* What the page is actually backed by, whatever its entry says; the callbacks
   use it the way Memory::Read/Write do. Null means unmapped. */
static unsigned char* backing[PAGE_COUNT];

static union { unsigned char bytes[8192]; uint64_t align; } code_memory;
#define memory code_memory.bytes
static unsigned reads;
static uintptr_t stack_min = UINTPTR_MAX, stack_max;
static int unmapped;
static uint64_t unmapped_from = UINT64_MAX;
static GuestContext context;
static uint64_t abort_pc;
static int mutate_after_guard;

static void set_page(unsigned page, unsigned char* host, unsigned type) {
    backing[page] = host;
    table[page].ptr = (type == T_MEMORY ? (uintptr_t)host - (uintptr_t)page * 0x1000 : 0) | type;
}

/* Guest memory as the emulator sees it: 48-bit addresses, split per byte. */
static unsigned char* model_byte(uint64_t va) {
    va &= UINT64_C(0xffffffffffff);
    return va >> 12 < PAGE_COUNT && backing[va >> 12] ? backing[va >> 12] + (va & 0xfff) : NULL;
}
static uint64_t model_read(uint64_t va, unsigned size) {
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < size; ++i) {
        const unsigned char* p = model_byte(va + i);
        v |= (uint64_t)(p ? *p : 0) << (8 * i);
    }
    return v;
}
static void model_write(uint64_t va, unsigned size, uint64_t v) {
    unsigned i;
    for (i = 0; i < size; ++i) {
        unsigned char* p = model_byte(va + i);
        if (p) *p = (unsigned char)(v >> (8 * i));
    }
}

/* Every data callback is logged in order, so two runs can be compared. */
typedef struct { char kind; uint64_t va, value; uint32_t size; } Event;
static Event events[256];
static unsigned event_count, plain_events;
static uint64_t excl_addr;
static int excl_valid;
static void log_event(char kind, uint64_t va, uint32_t size, uint64_t value) {
    if (event_count < 256) {
        Event e;
        memset(&e, 0, sizeof(e)); /* padding too: logs are compared bytewise */
        e.kind = kind; e.va = va; e.size = size; e.value = value;
        events[event_count] = e;
    }
    ++event_count;
    if (kind == 'L' || kind == 'S') ++plain_events;
}

static uint64_t checked_load(void* user, uint64_t va, uint32_t size) {
    uint64_t word = 0;
    (void)user;
    if (size != 0) {
        word = model_read(va, size);
        log_event('L', va, size, word);
        return word;
    }
    if ((uintptr_t)&word < stack_min) stack_min = (uintptr_t)&word;
    if ((uintptr_t)&word > stack_max) stack_max = (uintptr_t)&word;
    ++reads;
    if (unmapped || va >= unmapped_from || va < 0x1000 || va - 0x1000 > sizeof(memory) - 4) return 0;
    memcpy(&word, memory + va - 0x1000, 4);
    if (mutate_after_guard && va == 0x1004) {
        /* Finish the caller's verification with its original branch, then
           change its direct-chain target before that target is entered. */
        mutate_after_guard = 0;
        memory[0] ^= 1;
#ifdef RECOMP_FEATURE_GUARD_GEN1
        ++g_recomp_gg_word;
#endif
    }
    return word | UINT64_C(0x100000000);
}
static void host_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    (void)user;
    log_event('S', va, size, value);
    model_write(va, size, value);
}
static uint64_t host_excl_load(void* user, uint64_t va, uint32_t size) {
    const uint64_t v = model_read(va, size);
    (void)user;
    log_event('X', va, size, v);
    excl_addr = va;
    excl_valid = 1;
    return v;
}
static uint32_t host_excl_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    const int ok = excl_valid && excl_addr == va;
    (void)user;
    log_event('Y', va, size, value);
    excl_valid = 0;
    if (ok) model_write(va, size, value);
    return ok ? 0u : 1u;
}
static RecompHostMem bridge;
static void reset(uint64_t pc, int budget) {
    memset(&context, 0, sizeof(context));
    context.pc = pc;
    context.mem = memory;
    context.mem_size = sizeof(memory);
    context.mem_base_vaddr = 0x1000;
    context.pending_svc = ~UINT64_C(0);
    context.chain_budget = budget;
    context.host_mem = &bridge;
}
static void guard_aborted(int sig) {
    (void)sig;
    /* Prove this is the intended guard failure before any guest effect. */
    _Exit(context.pc == abort_pc && context.x[0] == 99 ? 86 : 87);
}

/* ---- Memory modes: one block of loads from x5 and stores to x6. ---- */

/* Data pages 0x10-0x1f live in one host buffer, but no two neighbouring guest
   pages are neighbours in it, so a page crossing served from one entry reads
   the wrong bytes instead of the right ones. */
#define DATA_PAGE 0x10
static unsigned char* arena;
static unsigned char* data_page(unsigned page) {
    return arena + (size_t)(((page - DATA_PAGE) * 5 + 3) % 32) * 0x1000;
}
static void map_data(void) {
    unsigned page, i;
    for (page = DATA_PAGE; page < DATA_PAGE + 16; ++page) set_page(page, data_page(page), T_MEMORY);
    for (i = 0; i < 32 * 0x1000; ++i) arena[i] = (unsigned char)(i * 131u + (i >> 12) * 7u + 1u);
}

typedef struct {
    uint64_t x[32], v0[2];
    Event events[256];
    unsigned event_count, plain_events;
    unsigned char image[32 * 0x1000];
    int halted;
} Outcome;
static Outcome outcomes[2];
static unsigned char snapshot[32 * 0x1000];

/* Runs the block once. fm_limit 0 is the ABI 5 path; otherwise the host
   enables FM1 exactly as arm_recomp does. */
static void run_block(uint64_t x5, uint64_t x6, uint64_t fm_limit, Outcome* out) {
    memcpy(arena, snapshot, sizeof(snapshot));
    event_count = plain_events = 0;
    excl_valid = 0;
    reset(SMOKE_MEM_BLOCK, 1);
    context.x[5] = x5;
    context.x[6] = x6;
#ifdef RECOMP_FEATURE_FASTMEM_PT1
    context.fm_table = (const unsigned char*)table;
    context.fm_limit = fm_limit;
#else
    (void)fm_limit;
#endif
    recomp_image_run_slice(&context);
    memcpy(out->x, context.x, sizeof(out->x));
    out->v0[0] = context.vreg[0][0];
    out->v0[1] = context.vreg[0][1];
    memcpy(out->events, events, sizeof(events));
    out->event_count = event_count;
    out->plain_events = plain_events;
    memcpy(out->image, arena, sizeof(snapshot));
    out->halted = context.halted;
}

/* The expected result, from the byte model alone. */
static int check_against_model(const Outcome* o, uint64_t x5, uint64_t x6) {
    static unsigned char expect[32 * 0x1000];
    memcpy(arena, snapshot, sizeof(snapshot));
    CHECK(o->halted == RECOMP_HALT_BREAKPOINT);
    CHECK(o->x[4] == model_read(x5, 8));
    CHECK(o->x[7] == model_read(x5 + 4, 4));
    CHECK(o->x[8] == model_read(x5 + 1, 2));
    CHECK(o->x[9] == model_read(x5 + 3, 1));
    CHECK(o->x[10] == model_read(x5, 8) && o->x[11] == model_read(x5 + 8, 8));
    CHECK(o->x[15] == model_read(x5 + 8, 4) && o->x[16] == model_read(x5 + 12, 4));
    CHECK(o->v0[0] == model_read(x5, 8) && o->v0[1] == model_read(x5 + 8, 8));
    CHECK(o->x[12] == model_read(x5, 8) && o->x[13] == model_read(x5, 8));
    CHECK(o->x[14] == 0); /* the reservation was taken on the same address */
    model_write(x6, 8, o->x[4]);
    model_write(x6 + 1, 1, o->x[9]);
    model_write(x6 + 3, 2, o->x[8]);
    model_write(x6 + 5, 4, o->x[7]);
    model_write(x6, 8, o->x[10]);
    model_write(x6 + 8, 8, o->x[11]);
    model_write(x6 + 8, 4, o->x[15]);
    model_write(x6 + 12, 4, o->x[16]);
    model_write(x6, 8, o->v0[0]);
    model_write(x6 + 8, 8, o->v0[1]);
    model_write(x6, 8, o->x[12]);
    memcpy(expect, arena, sizeof(expect));
    CHECK(memcmp(expect, o->image, sizeof(expect)) == 0);
    return 0;
}

/* One case: the ABI 5 path, the byte model, and (ABI 6) the fast path, which
   must produce the same values, memory and callback sequence. Returns the
   number of plain load/store callbacks through *plain. */
static int mem_case(uint64_t x5, uint64_t x6, unsigned* plain) {
    run_block(x5, x6, 0, &outcomes[0]);
    if (check_against_model(&outcomes[0], x5, x6)) return 1;
#ifdef RECOMP_FEATURE_FASTMEM_PT1
    run_block(x5, x6, bridge.address_space_max & ~UINT64_C(0xfff), &outcomes[1]);
    CHECK(memcmp(outcomes[0].x, outcomes[1].x, sizeof(outcomes[0].x)) == 0);
    CHECK(memcmp(outcomes[0].v0, outcomes[1].v0, sizeof(outcomes[0].v0)) == 0);
    CHECK(outcomes[0].event_count == outcomes[1].event_count);
    CHECK(memcmp(outcomes[0].events, outcomes[1].events,
                 sizeof(Event) * (outcomes[0].event_count < 256 ? outcomes[0].event_count : 256)) == 0);
    CHECK(memcmp(outcomes[0].image, outcomes[1].image, sizeof(snapshot)) == 0);
#endif
    *plain = outcomes[0].plain_events;
    return 0;
}

static int run_memory_mode(const char* mode) {
    unsigned plain = 0;
    arena = (unsigned char*)malloc(32 * 0x1000);
    CHECK(arena != NULL);
    map_data();
    memcpy(snapshot, arena, sizeof(snapshot));
    CHECK(recomp_image_lookup(SMOKE_MEM_BLOCK) != NULL);
    if (!strcmp(mode, "mem-ordinary")) {
        CHECK(mem_case(0x10010, 0x11020, &plain) == 0);
        CHECK(plain == 0);
        CHECK(outcomes[0].event_count == 2); /* LDXR and STXR only */
        CHECK(outcomes[0].events[0].kind == 'X' && outcomes[0].events[1].kind == 'Y');
#ifdef RECOMP_FEATURE_FASTMEM_PT1
        /* Prove it is the fast path serving these, not the ABI 5 walk: hide
           the two data pages from the ABI 5 table only. With the fast path on,
           nothing may reach the callback. */
        {
            static PageEntry abi5_view[PAGE_COUNT];
            memcpy(abi5_view, table, sizeof(table));
            abi5_view[0x10].ptr = abi5_view[0x11].ptr = T_CACHED;
            bridge.page_entries = abi5_view;
            run_block(0x10010, 0x11020, bridge.address_space_max, &outcomes[1]);
            CHECK(outcomes[1].plain_events == 0);
            CHECK(check_against_model(&outcomes[1], 0x10010, 0x11020) == 0);
            run_block(0x10010, 0x11020, 0, &outcomes[1]);
            CHECK(outcomes[1].plain_events == 22);
            bridge.page_entries = table;
        }
#endif
    } else if (!strcmp(mode, "mem-unmapped")) {
        set_page(0x12, NULL, T_UNMAPPED);
        set_page(0x13, NULL, T_UNMAPPED);
        CHECK(mem_case(0x12010, 0x13020, &plain) == 0);
        /* 11 load and 11 store callbacks: pairs and Q split into two. */
        CHECK(plain == 22);
        CHECK(outcomes[0].x[4] == 0 && outcomes[0].v0[1] == 0);
    } else if (!strcmp(mode, "mem-special")) {
        set_page(0x12, data_page(0x12), T_CACHED);
        set_page(0x13, data_page(0x13), T_DEBUG);
        CHECK(mem_case(0x12010, 0x13020, &plain) == 0);
        CHECK(plain == 22);
        CHECK(outcomes[0].x[4] != 0);
        /* Swapped: debug loads, GPU-tracked stores. */
        set_page(0x12, data_page(0x12), T_DEBUG);
        set_page(0x13, data_page(0x13), T_CACHED);
        CHECK(mem_case(0x12010, 0x13020, &plain) == 0);
        CHECK(plain == 22);
    } else if (!strcmp(mode, "mem-cross")) {
        /* Loads: ldr x, the first half of ldp x, ldr q and ldar cross.
           Stores: str x, sturh, the first half of stp x, str q and stlr. */
        CHECK(mem_case(0x14ffc, 0x16ffc, &plain) == 0);
        CHECK(plain == 9);
        /* Crossing into an unmapped page, and into a GPU-tracked one. */
        set_page(0x15, NULL, T_UNMAPPED);
        set_page(0x17, data_page(0x17), T_CACHED);
        CHECK(mem_case(0x14ffc, 0x16ffc, &plain) == 0);
    } else if (!strcmp(mode, "mem-unaligned")) {
        CHECK(mem_case(0x17003, 0x18005, &plain) == 0);
        CHECK(plain == 0);
        CHECK(mem_case(0x1700f, 0x18fe1, &plain) == 0);
        CHECK(plain == 0);
    } else if (!strcmp(mode, "mem-limit")) {
        static const uint64_t cases[][2] = {
            /* Just below a limit that is not page aligned (SLOWPATH_ABOVE):
               ABI 5 serves, the fast path declines; and the last page below
               fm_limit, which the fast path serves. */
            {0x1a7c0, 0x19fe0},
            /* Straddling address_space_max itself. */
            {0x1a7fc, 0x1a7f8},
            /* Bits 39-47 set. */
            {UINT64_C(0x0000008000010010), UINT64_C(0x0000ff8000011020)},
            /* Tagged top bits: ABI 5 masks them and serves from the table. */
            {UINT64_C(0xab00000000010010), UINT64_C(0x5600000000011020)},
            /* Near 2^64, wrapping for the later offsets. */
            {UINT64_C(0xfffffffffffffff8), UINT64_C(0xfffffffffffffffc)},
        };
        unsigned i;
        bridge.address_space_max = 0x1a800;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            CHECK(mem_case(cases[i][0], cases[i][1], &plain) == 0);
            if (i == 0 || i == 3) CHECK(plain == 0);
        }
        bridge.address_space_max = 0x100000;
    } else if (!strcmp(mode, "mem-protected")) {
        /* Guest permissions are not in the page table: a page the guest
           mapped read-only is still an ordinary entry, and stores to it are
           served directly by both paths, exactly as ABI 5 does. */
        CHECK(mem_case(0x1b100, 0x1b200, &plain) == 0);
        CHECK(plain == 0);
    } else {
        CHECK(0);
    }
    printf("PASS %s\n", mode);
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "slice";
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    memcpy(memory, smoke_code, sizeof(smoke_code));
    bridge.load = checked_load;
    bridge.store = host_store;
    bridge.excl_load = host_excl_load;
    bridge.excl_store = host_excl_store;
    bridge.page_entries = table;
    bridge.page_entry_stride = sizeof(PageEntry);
    bridge.page_bits = 12;
    bridge.pointer_mask = ~(uint64_t)3;
    bridge.address_space_max = 0x100000;
    set_page(1, memory, T_MEMORY);
    set_page(2, memory + 0x1000, T_MEMORY);
#ifdef RECOMP_FEATURE_GUARD_GEN1
    /* GG1: the FM1 handshake completes only after the GG1 one, so a host
       that knows FM1 but not GG1 refuses the image. Until a host takes the
       generation word over it stays VERIFY_ALWAYS, and every mode below then
       runs exactly as on ABI 5. */
    {
        uint64_t lo = 0, end = 0;
        const uint64_t* base = NULL;
        /* This tree may also carry FPX1 (the "ggfpx" variant). GG1's own
           handshake sequence below does not depend on it, so the expected
           feature set just includes FPX1 when it is built in. */
#ifdef RECOMP_FEATURE_FPX1
        CHECK(recomp_image_features() ==
              (RECOMP_FEATURE_FASTMEM_PT1 | RECOMP_FEATURE_GUARD_GEN1 | RECOMP_FEATURE_FPX1));
#else
        CHECK(recomp_image_features() ==
              (RECOMP_FEATURE_FASTMEM_PT1 | RECOMP_FEATURE_GUARD_GEN1));
#endif
        CHECK(recomp_image_fastmem_v1(12, 5, ~(uint64_t)3, 872, 880) == 0);
        CHECK(recomp_image_guard_gen_v1(0, &lo, &end, &base) == NULL);
        CHECK(recomp_image_guard_gen_v1(1, &lo, &end, &base) == NULL);
        CHECK(recomp_image_fastmem_v1(12, 5, ~(uint64_t)3, 872, 880) == 0);
        CHECK(recomp_image_guard_gen_v1(2, &lo, &end, &base) == &g_recomp_gg_word);
        CHECK(lo == 0x1000 && end == 0x1000 + sizeof(smoke_code) && base == &g_module_base);
        CHECK(g_recomp_gg_word == RECOMP_GG_VERIFY_ALWAYS);
    }
#endif
#ifdef RECOMP_FEATURE_FASTMEM_PT1
    CHECK(recomp_image_abi() == 6);
    CHECK(recomp_image_features() & RECOMP_FEATURE_FASTMEM_PT1);
    CHECK(recomp_image_fastmem_v1(12, 5, ~(uint64_t)3, 872, 880) == 1);
    CHECK(recomp_image_fastmem_v1(13, 5, ~(uint64_t)3, 872, 880) == 0);
    CHECK(recomp_image_fastmem_v1(12, 4, ~(uint64_t)3, 872, 880) == 0);
    CHECK(recomp_image_fastmem_v1(12, 5, ~(uint64_t)7, 872, 880) == 0);
    CHECK(recomp_image_fastmem_v1(12, 5, ~(uint64_t)3, 864, 880) == 0);
    CHECK(recomp_image_fastmem_v1(12, 5, ~(uint64_t)3, 872, 888) == 0);
    CHECK(sizeof(PageEntry) == 32);
#ifdef RECOMP_FEATURE_FPX1
    CHECK(recomp_image_features() & RECOMP_FEATURE_FPX1);
    CHECK((recomp_image_fpx_v1(848, 856, (uint64_t)1 << 32) & 0x1ffu) == (0x100u | RECOMP_FPX_HOST));
    CHECK(recomp_image_fpx_v1(856, 856, (uint64_t)1 << 32) == 0);
    CHECK(recomp_image_fpx_v1(848, 848, (uint64_t)1 << 32) == 0);
    CHECK(recomp_image_fpx_v1(848, 856, (uint64_t)1 << 31) == 0);
#endif
#else
    CHECK(recomp_image_abi() == 5);
#endif
    CHECK(recomp_image_guard_v2(2) == 2);
    /* Exercise the interval lookup before the flat index is constructed. */
    CHECK(recomp_image_lookup(0x1018) == recomp_image_lookup(0x1014));
    CHECK(recomp_image_lookup(0x1002) == NULL);
    recomp_image_set_base(0);
    if (!strncmp(mode, "mem-", 4)) {
        return run_memory_mode(mode);
    } else if (!strcmp(mode, "slice")) {
        const int budgets[] = {1, 3, 4096};
        unsigned i;
        for (i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
            int spent, counted;
            reset(0x1000, budgets[i]);
            recomp_image_run_slice(&context);
            CHECK(context.x[0] == (uint64_t)budgets[i]);
            CHECK(context.chain_budget == 0 && context.pc == 0x1000);
            /* Mirrors the host: first block already counted; exhausted final
               decrement parks a PC without executing an extra block. */
            spent = budgets[i] - context.chain_budget;
            counted = 1 + spent - (context.chain_budget == 0 ? 1 : 0);
            CHECK(counted == (int)context.x[0]);
        }
        reset(0x1008, 3);
        recomp_image_run_slice(&context);
        CHECK(context.pending_svc == 7 && context.pc == 0x100c);
        CHECK(context.x[1] == 0 && context.chain_budget == 3);
        reset(0x100c, 3);
        recomp_image_run_slice(&context);
        CHECK(context.halted == RECOMP_HALT_BREAKPOINT && context.x[1] == 1);
        CHECK(context.chain_budget == 3 && context.pc == 0x1010);
        reset(0x1018, 3);
        context.x[2] = 99;
        context.x[30] = 0x3000;
        recomp_image_run_slice(&context);
        CHECK(context.x[2] == 99 && context.pc == 0x3000 && !context.halted);
        CHECK(context.chain_budget == 3);
        reset(0x1020, 3);
        context.x[30] = 0x1028;
        recomp_image_run_slice(&context);
        CHECK(context.x[30] == 0x1024 && context.x[3] == 1);
        CHECK(context.pc == 0x102c && context.halted == RECOMP_HALT_BREAKPOINT);
        CHECK(context.chain_budget == 2);
        CHECK(reads == 0); /* All guards used the ordinary same-page path. */
        /* Force callbacks to observe stack depth through a long module slice. */
        bridge.page_entries = NULL;
        reset(0x1000, 4096);
        recomp_image_run_slice(&context);
        CHECK(context.x[0] == 4096 && context.chain_budget == 0);
        CHECK(reads == 8192 && stack_max - stack_min < 4096);
    } else if (!strcmp(mode, "hybrid-middle-entry")) {
#ifdef RECOMP_FEATURE_GUARD_GEN1
        g_recomp_gg_word = 1;
#endif
        reset(0x1018, 3); /* Enter after the block's mov x2,#1. */
        context.x[0] = 99;
        context.x[2] = 77;
        context.x[30] = 0x1028;
        memory[0x1c] ^= 1; /* Reject the later RET before any native effect. */
        recomp_image_lookup(context.pc)(&context);
        CHECK(context.halted == RECOMP_HALT_UNHANDLED && context.pc == 0x1018);
        CHECK(context.pending_svc == UINT64_MAX && context.x[0] == 99);
        CHECK(context.x[2] == 77 && context.x[30] == 0x1028);
    } else if (!strcmp(mode, "hybrid-direct-chain")) {
#ifdef RECOMP_FEATURE_GUARD_GEN1
        g_recomp_gg_word = 1;
#endif
        /* The 1000 ADD/B block branches directly back to itself through the
           module slice. The final checked guard read changes the next entry. */
        bridge.page_entries = NULL;
        mutate_after_guard = 1;
        reset(0x1000, 3);
        context.x[0] = 99;
        recomp_image_run_slice(&context);
        CHECK(context.halted == RECOMP_HALT_UNHANDLED && context.pc == 0x1000);
        CHECK(context.pending_svc == UINT64_MAX && context.x[0] == 100);
        CHECK(context.chain_budget == 2 && reads == 3);
        /* Re-enter the rejected target: the completed caller remains once,
           and a failed GG check must not cache this changed generation. */
        context.halted = 0;
        context.chain_budget = 3;
        recomp_image_lookup(context.pc)(&context);
        CHECK(context.halted == RECOMP_HALT_UNHANDLED && context.pc == 0x1000);
        CHECK(context.x[0] == 100 && context.chain_budget == 3 && reads == 4);
    } else if (!strcmp(mode, "hybrid-mutated-entry") ||
               !strcmp(mode, "hybrid-unmapped-entry")) {
#ifdef RECOMP_FEATURE_GUARD_GEN1
        g_recomp_gg_word = 1; /* Enable caching to detect a rejected generation being recorded. */
#endif
        reset(0x1000, 3);
        context.x[0] = 99;
        if (!strcmp(mode, "hybrid-mutated-entry")) {
            /* Reject a later instruction before the first instruction's effect. */
            memory[4] ^= 1;
        } else {
            set_page(1, NULL, T_UNMAPPED);
            unmapped = 1;
        }
        recomp_image_run_slice(&context);
        CHECK(context.halted == RECOMP_HALT_UNHANDLED && context.pc == 0x1000);
        CHECK(context.pending_svc == UINT64_MAX && context.x[0] == 99);
        /* A failed GG verification must never mark changed code verified. */
        reset(0x1000, 3);
        context.x[0] = 99;
        recomp_image_run_slice(&context);
        CHECK(context.halted == RECOMP_HALT_UNHANDLED && context.pc == 0x1000);
        CHECK(context.x[0] == 99);
    } else if (!strcmp(mode, "mutated-entry")) {
        reset(0x1000, 3);
        context.x[0] = 99;
        memory[0] ^= 1;
        abort_pc = 0x1000;
        signal(SIGABRT, guard_aborted);
        recomp_image_run_slice(&context);
        CHECK(0); /* No generated effect may precede guard rejection. */
    } else {
        uint32_t expected[] = {0xd503201f, 0};
        const int cross = !strncmp(mode, "cross-page", 10);
        uint64_t pc = cross ? 0x1ffc : 0x1080;
        memcpy(memory + pc - 0x1000, expected, sizeof(expected));
        reset(pc, 3);
        context.x[0] = 99;
        if (!strcmp(mode, "mutated") || !strcmp(mode, "cross-page-mutated")) {
            /* For cross-page, the changed word is the one on the second page. */
            memory[pc - 0x1000 + 4] ^= 1;
            abort_pc = pc + 4;
            signal(SIGABRT, guard_aborted);
        } else if (!strcmp(mode, "unmapped-zero")) {
            pc += 4;
            expected[0] = 0;
            set_page(1, NULL, T_UNMAPPED);
            unmapped = 1;
            abort_pc = pc;
            signal(SIGABRT, guard_aborted);
        } else if (!strcmp(mode, "cross-page-unmapped")) {
            /* The second page is gone, though its expected word (0) matches. */
            set_page(2, NULL, T_UNMAPPED);
            unmapped_from = 0x2000;
            abort_pc = pc + 4;
            signal(SIGABRT, guard_aborted);
        } else if (!strcmp(mode, "special-page")) {
            /* A nonzero mapping tag without an ordinary backing pointer. */
            set_page(1, memory, T_CACHED);
        }
        recomp_code_guard(&context, pc, expected,
                          !strcmp(mode, "unmapped-zero") ? 1 : 2, 2);
        CHECK(strcmp(mode, "mutated") && strcmp(mode, "unmapped-zero") &&
              strcmp(mode, "cross-page-mutated") && strcmp(mode, "cross-page-unmapped"));
        /* A page crossing is compared per page without callbacks; only the
           special mapping needs the checked loop. */
        CHECK(reads == (!strcmp(mode, "special-page") ? 2u : 0u));
        CHECK(context.x[0] == 99);
    }
    printf("PASS %s\n", mode);
    return 0;
}
