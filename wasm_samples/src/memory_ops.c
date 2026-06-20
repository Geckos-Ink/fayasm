#include <stdint.h>
#include <string.h>

/*
 * Memory-heavy smoke fixtures: in-place sorting (dense load/store + swaps),
 * memcpy/memset pipelines (which the toolchain may lower to memory.copy /
 * memory.fill bulk-memory ops), and a const lookup table read out of a data
 * segment. These drive linear-memory load/store, the 0xFC bulk-memory family,
 * and active data-segment initialization from real compiler output.
 */

/* Bubble-sort a local array, then weighted checksum of the sorted result.
 * Sorts to 1..8, so sum(i*(i)) for i in 1..8 == 1+4+9+...+64 == 204. */
int sample_sort_checksum(void) {
    int a[8] = { 5, 3, 8, 1, 9, 2, 7, 4 };
    /* Re-map the 9 -> 6 so the sorted sequence is exactly 1..8. */
    for (int i = 0; i < 8; ++i) {
        if (a[i] == 9) {
            a[i] = 6;
        }
    }
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 7 - i; ++j) {
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
    int sum = 0;
    for (int i = 0; i < 8; ++i) {
        sum += a[i] * (i + 1);
    }
    return sum;
}

/* Fill -> copy -> clear pipeline. With a runtime length the toolchain lowers
 * memcpy/memset to memory.copy / memory.fill (when built with -mbulk-memory),
 * exercising the 0xFC bulk-memory family from real compiler output. The
 * checksum keeps the traffic live. Deterministic for a given (seed, n). */
int sample_buffer_pipeline(int seed, int n) {
    uint8_t src[256];
    uint8_t dst[256];
    if (n < 0) {
        n = 0;
    }
    if (n > 256) {
        n = 256;
    }
    for (int i = 0; i < n; ++i) {
        src[i] = (uint8_t)((seed + i * 31) & 0xff);
    }
    memcpy(dst, src, (unsigned)n);   /* -> memory.copy */
    memset(src, 0, (unsigned)n);     /* -> memory.fill */
    /* Unsigned accumulator: well-defined wraparound keeps the result stable
     * across optimization levels (a signed accumulator would be overflow UB). */
    uint32_t checksum = 0;
    for (int i = 0; i < n; ++i) {
        checksum = (checksum * 131u + dst[i] + src[i]) & 0x7fffffffu;
    }
    return (int)checksum;
}

/* Read from a const lookup table parked in a data segment; indexing by a
 * runtime value forces an i32.load from initialized memory. lookup(5) == 25. */
int sample_table_lookup(int i) {
    static const int32_t k_squares[16] = {
        0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225
    };
    return k_squares[(unsigned)i & 15u];
}
