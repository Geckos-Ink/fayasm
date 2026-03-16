#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define FA_NOINLINE __attribute__((noinline))
#else
#define FA_NOINLINE
#endif

FA_NOINLINE static int sample_mix_helper(int lhs, int rhs) {
    return (lhs * 3) + (rhs * 5);
}

int sample_memory_mix(void) {
    uint32_t buffer[8] = {0};
    for (uint32_t i = 0; i < 8; ++i) {
        buffer[i] = (i * 3U) + 1U;
    }

    uint32_t checksum = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        checksum ^= (buffer[i] << (i & 7U));
    }

    return (int)checksum;
}

int sample_call_chain(void) {
    int acc = 0;
    for (int i = 0; i < 6; ++i) {
        acc += sample_mix_helper(i, 6 - i);
    }
    return acc;
}
