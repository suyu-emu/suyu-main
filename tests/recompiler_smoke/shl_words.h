/* SHL by immediate words for smoke_shl_gen and smoke_shl_unit. The first four
   are the TOTK 1.4.3 Hybrid transition leaders; the rest cover every
   arrangement with the smallest and largest shift, rd == rn, and scalar D. */
#ifndef SMOKE_SHL_WORDS_H
#define SMOKE_SHL_WORDS_H

static const unsigned kShlWords[] = {
    0x4F235425u, /* shl v5.4s, v1.4s, #3 */
    0x4F3D5421u, /* shl v1.4s, v1.4s, #29 */
    0x4F3F5421u, /* shl v1.4s, v1.4s, #31 */
    0x4F375442u, /* shl v2.4s, v2.4s, #23 */
    0x0F085464u, /* shl v4.8b, v3.8b, #0 */
    0x4F0F5464u, /* shl v4.16b, v3.16b, #7 */
    0x0F115464u, /* shl v4.4h, v3.4h, #1 */
    0x4F1F5464u, /* shl v4.8h, v3.8h, #15 */
    0x0F255464u, /* shl v4.2s, v3.2s, #5 */
    0x4F405464u, /* shl v4.2d, v3.2d, #0 */
    0x4F7F5464u, /* shl v4.2d, v3.2d, #63 */
    0x5F475464u, /* shl d4, d3, #7 */
    0x5F7F5464u, /* shl d4, d3, #63 */
};
#define kShlWordCount (sizeof kShlWords / sizeof kShlWords[0])

#endif
