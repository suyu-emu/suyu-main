#include "recomp_runtime.h"
#include "code.h"
#include <stdio.h>
#include <string.h>

#define MODULE(name) \
    extern unsigned recomp_image_abi_##name(void); \
    extern unsigned recomp_image_guard_v2_##name(unsigned); \
    extern void recomp_image_set_base_##name(uint64_t); \
    extern void recomp_image_run_slice_##name(GuestContext*); \
    extern BlockFn recomp_image_lookup_##name(uint64_t); \
    extern int recomp_image_index_##name(uint64_t*, uint64_t*, BlockFn**); \
    extern int g_recomp_guard_host_v2_##name
MODULE(smoke);
MODULE(second);
#ifdef RECOMP_FEATURE_FASTMEM_PT1
#define FASTMEM_MODULE(name) \
    extern unsigned recomp_image_features_##name(void); \
    extern unsigned recomp_image_fastmem_v1_##name(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t)
FASTMEM_MODULE(smoke);
FASTMEM_MODULE(second);
#endif
#ifdef RECOMP_FEATURE_GUARD_GEN1
extern uint32_t* recomp_image_guard_gen_v1_smoke(uint32_t, uint64_t*, uint64_t*, const uint64_t**);
extern uint32_t* recomp_image_guard_gen_v1_second(uint32_t, uint64_t*, uint64_t*, const uint64_t**);
extern uint32_t g_recomp_gg_word_smoke, g_recomp_gg_word_second;
extern uint64_t g_module_base_smoke, g_module_base_second;
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void context_at(GuestContext* c, uint64_t base, int budget) {
    memset(c, 0, sizeof(*c));
    c->mem = (uint8_t*)smoke_code;
    c->mem_size = sizeof(smoke_code);
    c->mem_base_vaddr = c->pc = base + 0x1000;
    c->chain_budget = budget;
    c->pending_svc = ~UINT64_C(0);
}

int main(void) {
    const uint64_t first_base = 0x10000, second_base = 0x20000;
    uint64_t first_lo, first_hi, second_lo, second_hi;
    BlockFn *first_index, *second_index;
    GuestContext first, second;
#ifdef RECOMP_FEATURE_FASTMEM_PT1
    CHECK(recomp_image_abi_smoke() == 6 && recomp_image_abi_second() == 6);
    CHECK(recomp_image_features_smoke() & recomp_image_features_second() & RECOMP_FEATURE_FASTMEM_PT1);
#ifdef RECOMP_FEATURE_GUARD_GEN1
    /* GG1 renames: each module owns its word, and its FM1 handshake waits for
       its own GG1 handshake, not the other module's. */
    {
        uint64_t lo, end;
        const uint64_t* base;
        CHECK(recomp_image_fastmem_v1_smoke(12, 5, ~(uint64_t)3, 872, 880) == 0);
        CHECK(recomp_image_guard_gen_v1_smoke(2, &lo, &end, &base) == &g_recomp_gg_word_smoke);
        CHECK(base == &g_module_base_smoke && lo == 0x1000);
        CHECK(recomp_image_fastmem_v1_second(12, 5, ~(uint64_t)3, 872, 880) == 0);
        CHECK(recomp_image_guard_gen_v1_second(2, &lo, &end, &base) == &g_recomp_gg_word_second);
        CHECK(base == &g_module_base_second);
        CHECK(&g_recomp_gg_word_smoke != &g_recomp_gg_word_second);
        CHECK(g_recomp_gg_word_second == RECOMP_GG_VERIFY_ALWAYS);
    }
#endif
    CHECK(recomp_image_fastmem_v1_smoke(12, 5, ~(uint64_t)3, 872, 880) == 1);
    CHECK(recomp_image_fastmem_v1_second(12, 5, ~(uint64_t)3, 872, 880) == 1);
    CHECK(recomp_image_fastmem_v1_second(12, 5, ~(uint64_t)3, 872, 872) == 0);
#else
    CHECK(recomp_image_abi_smoke() == 5 && recomp_image_abi_second() == 5);
#endif
    CHECK(g_recomp_guard_host_v2_smoke == 0 && g_recomp_guard_host_v2_second == 0);
    CHECK(recomp_image_guard_v2_smoke(2) == 2);
    CHECK(g_recomp_guard_host_v2_smoke == 2 && g_recomp_guard_host_v2_second == 0);
    CHECK(recomp_image_guard_v2_second(2) == 2);
    recomp_image_set_base_smoke(first_base);
    recomp_image_set_base_second(second_base);
    CHECK(recomp_image_index_smoke(&first_lo, &first_hi, &first_index));
    CHECK(recomp_image_index_second(&second_lo, &second_hi, &second_index));
    CHECK(first_lo == first_base + 0x1000 && second_lo == second_base + 0x1000);
    CHECK(first_hi - first_base == second_hi - second_base);
    CHECK(first_index != second_index);
    CHECK(first_index[0] == recomp_image_lookup_smoke(first_lo));
    CHECK(second_index[0] == recomp_image_lookup_second(second_lo));
    CHECK(first_index[0] != second_index[0]);
    CHECK(recomp_image_lookup_smoke(second_lo) == NULL);
    CHECK(recomp_image_lookup_second(first_lo) == NULL);
    CHECK(recomp_image_lookup_smoke(first_base + 0x1018) ==
          recomp_image_lookup_smoke(first_base + 0x1014));
    CHECK(recomp_image_lookup_second(second_base + 0x1018) ==
          recomp_image_lookup_second(second_base + 0x1014));
    context_at(&first, first_base, 3);
    context_at(&second, second_base, 5);
    recomp_image_run_slice_smoke(&first);
    recomp_image_run_slice_second(&second);
    CHECK(first.x[0] == 3 && first.pc == first_lo && first.chain_budget == 0);
    CHECK(second.x[0] == 5 && second.pc == second_lo && second.chain_budget == 0);
    /* Rebase one image after both indexes exist; the second must stay put. */
    recomp_image_set_base_smoke(0x30000);
    CHECK(recomp_image_lookup_smoke(first_lo) == NULL);
    CHECK(recomp_image_lookup_smoke(0x31000) == first_index[0]);
    CHECK(recomp_image_lookup_second(second_lo) == second_index[0]);
    context_at(&second, second_base, 1);
    recomp_image_run_slice_second(&second);
    CHECK(second.x[0] == 1 && second.pc == second_lo && second.chain_budget == 0);
    puts("PASS two static modules, isolated indexes/bases/guards, one shared runtime");
    return 0;
}
