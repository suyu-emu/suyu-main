#pragma once
#include <stdint.h>
/* Synthetic instructions only: no executable or cartridge input. */
static const uint32_t smoke_code[] = {
    0x91000400, 0x17ffffff, /* 1000: add x0,x0,#1; b 1000 */
    0xd40000e1,             /* 1008: svc #7 */
    0x91000421, 0xd4200000, /* 100c: add x1,x1,#1; brk */
    0xd2800022, 0xd503201f, 0xd65f03c0, /* 1014: mov x2,#1; nop; ret */
    0xd63f03c0,             /* 1020: blr x30 */
    0xd4200000,             /* 1024: return address */
    0x91000463, 0xd4200000, /* 1028: add x3,x3,#1; brk */
    /* 1030: one block of guest memory accesses, loads from x5 then stores to
       x6, ending in brk. Every width and pair form the helpers implement. */
    0xf94000a4, /* ldr   x4,[x5]         */
    0xb94004a7, /* ldr   w7,[x5,#4]      */
    0x784010a8, /* ldurh w8,[x5,#1]      unaligned */
    0x39400ca9, /* ldrb  w9,[x5,#3]      */
    0xa9402caa, /* ldp   x10,x11,[x5]    */
    0x294140af, /* ldp   w15,w16,[x5,#8] */
    0x3dc000a0, /* ldr   q0,[x5]         */
    0xc8dffcac, /* ldar  x12,[x5]        */
    0xc85f7cad, /* ldxr  x13,[x5]        */
    0xc80e7cad, /* stxr  w14,x13,[x5]    */
    0xf90000c4, /* str   x4,[x6]         */
    0x390004c9, /* strb  w9,[x6,#1]      */
    0x780030c8, /* sturh w8,[x6,#3]      unaligned */
    0xb80050c7, /* stur  w7,[x6,#5]      unaligned */
    0xa9002cca, /* stp   x10,x11,[x6]    */
    0x290140cf, /* stp   w15,w16,[x6,#8] */
    0x3d8000c0, /* str   q0,[x6]         */
    0xc89ffccc, /* stlr  x12,[x6]        */
    0xd4200000  /* brk */
};
#define SMOKE_MEM_BLOCK 0x1030
