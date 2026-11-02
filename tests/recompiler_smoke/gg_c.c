/* C half of smoke_gg_host: owns the generated GG1 module's context and code
   memory, and exposes a few plain functions to the C++ driver (gg_host.cpp),
   which plays the emulator side through Core::RecompGuardGen. */
#include "recomp_runtime.h"
#include "code.h"
/* The module's only block unit, included rather than compiled on its own so
   this file can read the unit-private seen words (recomp_gg_seen). */
#include "src/recompiled_smoke_0.c"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define GG_TLS __declspec(thread)
#else
#define GG_TLS _Thread_local
#endif

extern unsigned recomp_image_guard_v2(unsigned);
extern void recomp_image_set_base(uint64_t);
extern void recomp_image_run_slice(GuestContext*);
extern uint32_t* recomp_image_guard_gen_v1(uint32_t, uint64_t*, uint64_t*, const uint64_t**);

/* Guest code at 0x1000; the guard reads it word by word through `load` size 0
   (page_entries stays null), so every verification is visible as reads. */
static union { unsigned char bytes[8192]; uint64_t align; } code_memory;
static uint64_t code_base = 0x1000;
static RecompHostMem bridge;
static GG_TLS unsigned long thread_reads;
static GG_TLS GuestContext* thread_context;
static volatile uint64_t abort_pc;

/* A page table shaped like Common::PageTable's (32-byte entries, pointer part
   host page - guest page, type in the low bits, watch word at +24). Page 0 and
   the data page are ordinary memory; pages 1-2 are the code. Used only while a
   store block runs; the guard keeps reading code through `load` otherwise. */
typedef struct { uintptr_t ptr; uint64_t block, addr, watch; } PageEntry;
#define GG_PAGES 256
static PageEntry table[GG_PAGES];
static unsigned char page0[0x1000];
static union { unsigned char bytes[0x1000]; uint64_t align; } data_page;
#define GG_DATA_PAGE 0x10
/* Called after every host store lands, as Core::Memory does after a write. */
static void (*on_host_write)(uint64_t va, uint32_t size);

static unsigned char* backing(uint64_t va) {
    va &= UINT64_C(0xffffffffffff);
    if (va >> 12 == 0) return page0 + (va & 0xfff);
    if (va >= code_base && va - code_base < sizeof(code_memory.bytes))
        return code_memory.bytes + (va - code_base);
    if (va >> 12 == GG_DATA_PAGE) return data_page.bytes + (va & 0xfff);
    return NULL;
}
static uint64_t plain_load(uint64_t va, uint32_t size) {
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < size; ++i) {
        const unsigned char* b = backing(va + i);
        v |= (uint64_t)(b ? *b : 0) << (8 * i);
    }
    return v;
}
static void host_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    uint32_t i;
    (void)user;
    for (i = 0; i < size; ++i) {
        unsigned char* b = backing(va + i);
        if (b) *b = (unsigned char)(value >> (8 * i));
    }
    if (on_host_write) on_host_write(va & UINT64_C(0xffffffffffff), size);
}
static uint64_t host_excl_load(void* user, uint64_t va, uint32_t size) {
    (void)user;
    return plain_load(va, size);
}
static uint32_t host_excl_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    host_store(user, va, size, value);
    return 0;
}
static void map_page(unsigned page, unsigned char* host) {
    table[page].ptr = ((uintptr_t)host - (uintptr_t)page * 0x1000) | 1u;
}

static uint64_t checked_load(void* user, uint64_t va, uint32_t size) {
    uint64_t word = 0;
    (void)user;
    if (size != 0) return 0;
    ++thread_reads;
    if (va < code_base || va - code_base > sizeof(code_memory.bytes) - 4) return 0;
    memcpy(&word, (const unsigned char*)code_memory.bytes + (va - code_base), 4);
    return word | UINT64_C(0x100000000);
}

static void guard_aborted(int sig) {
    (void)sig;
    /* The guard names the rejected word before any guest effect of the block. */
    _Exit(thread_context && thread_context->pc == abort_pc && thread_context->x[0] == 99 ? 86
                                                                                        : 87);
}

void ggc_init(void) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    memcpy(code_memory.bytes, smoke_code, sizeof(smoke_code));
    memset(data_page.bytes, 0x5a, sizeof(data_page.bytes));
    map_page(0, page0);
    map_page(1, code_memory.bytes);
    map_page(2, code_memory.bytes + 0x1000);
    map_page(GG_DATA_PAGE, data_page.bytes);
    bridge.load = checked_load;
    bridge.store = host_store;
    bridge.excl_load = host_excl_load;
    bridge.excl_store = host_excl_store;
    bridge.page_entry_stride = sizeof(PageEntry);
    bridge.page_bits = 12;
    bridge.pointer_mask = ~(uint64_t)3;
    bridge.address_space_max = GG_PAGES * 0x1000;
    bridge.page_entries = NULL;
    recomp_image_guard_v2(2);
    recomp_image_set_base(0);
    signal(SIGABRT, guard_aborted);
}

uint32_t* ggc_handshake(uint32_t version, uint64_t* lo, uint64_t* end, const uint64_t** base) {
    return recomp_image_guard_gen_v1(version, lo, end, base);
}

void* ggc_new_context(void) {
    return calloc(1, sizeof(GuestContext));
}

/* One entry into the block at `pc` (add x0,x0,#1; b pc), with x0 preset.
   Returns how many code words this entry's guard read (0 = skipped). */
unsigned long ggc_enter(void* ctx, uint64_t pc, uint64_t x0) {
    GuestContext* c = (GuestContext*)ctx;
    const unsigned long before = thread_reads;
    memset(c, 0, sizeof(*c));
    c->pc = pc;
    c->x[0] = x0;
    c->pending_svc = ~UINT64_C(0);
    c->chain_budget = 1;
    c->host_mem = &bridge;
    thread_context = c;
    recomp_image_run_slice(c);
    if (c->x[0] != x0 + 1) {
        fprintf(stderr, "block did not run: x0=%llu\n", (unsigned long long)c->x[0]);
        _Exit(88);
    }
    return thread_reads - before;
}

void ggc_expect_abort_at(uint64_t pc) {
    abort_pc = pc;
}

/* Flip one bit of the first word of the block at 0x1000. */
void ggc_mutate(void) {
    volatile unsigned char* p = code_memory.bytes;
    p[0] ^= 1;
}

/* Guest code now lives at `base`: the same bytes, moved. */
void ggc_move_code(uint64_t base) {
    code_base = base;
    recomp_image_set_base(base - 0x1000);
}

const void* ggc_table(void) {
    return table;
}
uint64_t ggc_table_limit(void) {
    return GG_PAGES * 0x1000;
}
void ggc_set_host_write_hook(void (*hook)(uint64_t, uint32_t)) {
    on_host_write = hook;
}
/* What the host's watch callback does: set the watch word of every page. */
void ggc_watch(uint64_t va, uint64_t size) {
    uint64_t page;
    for (page = va >> 12; page <= (va + size - 1) >> 12 && page < GG_PAGES; ++page)
        RECOMP_GG_STORE_RELEASE(*(uint32_t*)&table[page].watch, 1u);
}

/* Runs the smoke memory block (loads from x5 in the data page, stores to x6)
   through the page table, with the FM1 fast path on or off. */
void ggc_run_stores(uint64_t x6, int fastmem) {
    GuestContext* c = (GuestContext*)ggc_new_context();
    c->pc = SMOKE_MEM_BLOCK;
    c->x[5] = (uint64_t)GG_DATA_PAGE << 12 | 0x10;
    c->x[6] = x6;
    c->pending_svc = ~UINT64_C(0);
    c->chain_budget = 1;
    c->host_mem = &bridge;
    c->fm_table = (const unsigned char*)table;
    c->fm_limit = fastmem ? GG_PAGES * 0x1000 : 0;
    thread_context = c;
    bridge.page_entries = table;
    recomp_image_run_slice(c);
    bridge.page_entries = NULL;
    if (c->halted != RECOMP_HALT_BREAKPOINT) {
        fprintf(stderr, "store block did not finish: halted=%d\n", c->halted);
        _Exit(89);
    }
    free(c);
}

/* A write the host makes itself (an HLE service, the loader, a cheat), as
   Core::Memory::WriteBlock does: bytes first, then the report. */
void ggc_host_write(uint64_t va, uint32_t value) {
    host_store(NULL, va, 4, value);
}

uint32_t ggc_seen(unsigned index) {
    return RECOMP_GG_LOAD(recomp_gg_seen[index]);
}
