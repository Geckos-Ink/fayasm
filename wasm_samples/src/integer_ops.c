#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define FA_NOINLINE __attribute__((noinline))
#else
#define FA_NOINLINE
#endif

/*
 * Non-SIMD integer fixture emitted by a real C toolchain. This deliberately
 * combines signed division/remainder with the WebAssembly bit-count and rotate
 * instructions, including their i64 variants.
 */

int32_t sample_signed_divrem(int32_t value, int32_t divisor) {
    return ((value / divisor) * 31) + (value % divisor);
}

uint32_t sample_i32_bit_mix(uint32_t value) {
    const uint32_t rotated = (value << 7) | (value >> 25);
    const uint32_t population = (uint32_t)__builtin_popcount(value);
    const uint32_t leading = (uint32_t)__builtin_clz(value | 1U);
    return rotated ^ (population << 24) ^ leading;
}

uint64_t sample_i64_bit_mix(uint64_t value) {
    const uint64_t rotated = (value >> 13) | (value << 51);
    return rotated ^ (uint64_t)__builtin_popcountll(value);
}

FA_NOINLINE static int32_t sample_nested_helper(int32_t lhs, int32_t rhs) {
    return (lhs * 3) + (rhs * 5);
}

int32_t sample_nested_calls(int32_t a, int32_t b, int32_t c, int32_t d) {
    return sample_nested_helper(a, b) + sample_nested_helper(c, d);
}
