/* Link-only sentinel. Its object MUST NEVER be distributed in the build kit. */
#include <stdint.h>
#include <stddef.h>
const void* suyu_recomp_static_modules_v4(unsigned* count) { *count = 0; return NULL; }
int suyu_recomp_static_guard_v2(unsigned version) { (void)version; return 0; }
int suyu_recomp_static_fastmem_v1(uint32_t a, uint32_t b, uint64_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
unsigned suyu_recomp_static_features_v1(void) { return 0; }
unsigned suyu_recomp_static_guard_gen_v1(uint32_t version, void* out, unsigned max) {
    (void)version; (void)out; (void)max; return 0;
}
unsigned suyu_recomp_static_fpx_v1(uint32_t a, uint32_t b, uint64_t c) {
    (void)a; (void)b; (void)c; return 0;
}
