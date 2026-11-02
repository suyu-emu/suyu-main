#pragma once
#include <stdint.h>
/* Synthetic instructions only: the coverage loop's module (recomp_gaps.json).
   The first function calls through a pointer it loads from memory, so nothing
   in the image names the callee. Block discovery indexes every nonzero word
   (an indirect branch may enter any of them), so the only code it leaves out
   is a zero word straight after an unconditional branch or return, which it
   takes for padding. The callee starts on exactly such a word. */
static const uint32_t loop_code[] = {
    0xf94000a8, /* 1000: ldr x8,[x5]  the callee's address */
    0xd63f0100, /* 1004: blr x8 */
    0xd4200000, /* 1008: brk #0       return address */
    0xd65f03c0, /* 100c: ret          end of an unrelated function */
    0x00000000, /* 1010: the callee: udf #0 behind a ret, left out */
};
#define LOOP_BASE 0x1000
#define LOOP_CALLEE 0x1010
/* Stands in for an NSO header's build ID; any nonzero 32 bytes. */
#define LOOP_BUILD_ID "5eedc0de0123456789abcdef0123456789abcdef000000000000000000000000"
#define LOOP_TITLE_ID UINT64_C(0x0100000000c0ffee)
