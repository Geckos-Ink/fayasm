#include <stdint.h>

/*
 * Fixtures that exercise non-SIMD runtime paths not covered by the
 * zero-argument i32 smoke samples: argument transfer into callee locals
 * (i32 and i64 parameters) plus i64-typed returns.
 */

int sample_add_i32(int a, int b) {
    return a + b;
}

int sample_sum_to_n(int n) {
    int total = 0;
    for (int i = 1; i <= n; ++i) {
        total += i;
    }
    return total;
}

int64_t sample_scale_i64(int64_t x, int64_t y) {
    return (x * y) + 1;
}
