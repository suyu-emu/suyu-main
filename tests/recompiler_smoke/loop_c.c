/* Drives the coverage loop's generated module (loop_code.h) for loop_host.cpp,
   with guest memory and host callbacks shaped as in run.c. */
#include "recomp_runtime.h"
#include "loop_code.h"
#include <string.h>

extern void recomp_image_set_base(uint64_t);
extern void recomp_image_run_slice(GuestContext*);
extern BlockFn recomp_image_lookup(uint64_t);
extern unsigned recomp_image_guard_v2(unsigned);

enum { T_UNMAPPED = 0, T_MEMORY = 1 };
typedef struct { uintptr_t ptr; uint64_t block, addr, pad; } PageEntry;
#define PAGE_COUNT 256
#define DATA_PAGE 0x10
static PageEntry table[PAGE_COUNT];
static unsigned char* backing[PAGE_COUNT];
static union { unsigned char bytes[4096]; uint64_t align; } code_page, data_page;
static GuestContext context;
static RecompHostMem bridge;

static void set_page(unsigned page, unsigned char* host) {
    backing[page] = host;
    table[page].ptr = ((uintptr_t)host - (uintptr_t)page * 0x1000) | T_MEMORY;
}
static unsigned char* model_byte(uint64_t va) {
    va &= UINT64_C(0xffffffffffff);
    return va >> 12 < PAGE_COUNT && backing[va >> 12] ? backing[va >> 12] + (va & 0xfff) : NULL;
}
static uint64_t host_load(void* user, uint64_t va, uint32_t size) {
    uint64_t v = 0;
    uint32_t i;
    (void)user;
    if (size == 0) { /* a code-guard word: flagged as present */
        const unsigned char* p = model_byte(va);
        if (!p) return 0;
        memcpy(&v, p, 4);
        return (v & 0xffffffffu) | UINT64_C(0x100000000);
    }
    for (i = 0; i < size; ++i) {
        const unsigned char* p = model_byte(va + i);
        v |= (uint64_t)(p ? *p : 0) << (8 * i);
    }
    return v;
}
static void host_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    uint32_t i;
    (void)user;
    for (i = 0; i < size; ++i) {
        unsigned char* p = model_byte(va + i);
        if (p) *p = (unsigned char)(value >> (8 * i));
    }
}
static uint64_t host_excl_load(void* user, uint64_t va, uint32_t size) {
    return host_load(user, va, size);
}
static uint32_t host_excl_store(void* user, uint64_t va, uint32_t size, uint64_t value) {
    host_store(user, va, size, value);
    return 0;
}

void loopc_init(void) {
    const uint64_t callee = LOOP_CALLEE;
    memcpy(code_page.bytes, loop_code, sizeof(loop_code));
    memcpy(data_page.bytes, &callee, sizeof(callee));
    set_page(LOOP_BASE >> 12, code_page.bytes);
    set_page(DATA_PAGE, data_page.bytes);
    bridge.load = host_load;
    bridge.store = host_store;
    bridge.excl_load = host_excl_load;
    bridge.excl_store = host_excl_store;
    bridge.page_entries = table;
    bridge.page_entry_stride = sizeof(PageEntry);
    bridge.page_bits = 12;
    bridge.pointer_mask = ~(uint64_t)3;
    bridge.address_space_max = 0x100000;
    recomp_image_guard_v2(2);
    /* Module base 0: offsets and vaddrs coincide, as for an NSO. */
    recomp_image_set_base(0);
}

int loopc_has_block(uint64_t pc) {
    return recomp_image_lookup(pc) != NULL;
}

/* Calls the first function. Returns the halt reason; *pc is where it stopped. */
int loopc_run(uint64_t* pc, uint64_t* x30) {
    memset(&context, 0, sizeof(context));
    context.pc = LOOP_BASE;
    context.mem = code_page.bytes;
    context.mem_size = sizeof(code_page.bytes);
    context.mem_base_vaddr = LOOP_BASE;
    context.pending_svc = ~UINT64_C(0);
    context.chain_budget = 64;
    context.host_mem = &bridge;
    context.x[5] = (uint64_t)DATA_PAGE << 12;
    recomp_image_run_slice(&context);
    *pc = context.pc;
    *x30 = context.x[30];
    return context.halted;
}

uint32_t loopc_word(uint64_t pc) {
    return (uint32_t)host_load(NULL, pc, 4);
}
