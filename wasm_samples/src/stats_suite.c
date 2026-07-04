#include <stdint.h>
#include <string.h>

/*
 * Statistical/algorithmic smoke + benchmark fixtures: the heaviest wasm_samples
 * module, meant both for correctness smoke tests (small n) and for runtime
 * performance measurement (large n via the CLI runner). Every kernel is
 * deterministic for a given (n, seed) so results are reproducible across
 * toolchains and native reference builds.
 *
 * Kernels: xorshift32 PRNG + uniform doubles, one-pass Welford variance,
 * least-squares linear regression, Pearson correlation (open-coded sqrt),
 * Monte Carlo pi estimation, sieve of Eratosthenes, and an integer matrix
 * multiply with an i64 checksum. Together they drive tight f64 loops,
 * int<->float conversions, i64 arithmetic, dense linear-memory traffic, and
 * memset lowered to memory.fill (built with -mbulk-memory).
 *
 * libm is intentionally avoided (sqrt is open-coded) so the module stays
 * standalone with no imports.
 */

/* xorshift32: 3 shifts + 3 xors per draw; period 2^32-1, never yields 0. */
static uint32_t xs32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Uniform double in [0,1) from the top 24 bits (exact in f64, portable). */
static double u01(uint32_t* state) {
    return (double)(xs32(state) >> 8) * (1.0 / 16777216.0);
}

/* Open-coded sqrt via Newton's method so we depend on no host import. */
static double fa_sqrt(double x) {
    if (x <= 0.0) {
        return 0.0;
    }
    double guess = x;
    for (int i = 0; i < 60; ++i) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

/* Mean of n uniform draws from a fixed seed; converges toward 0.5. */
double sample_stats_mean(int n) {
    if (n <= 0) {
        return 0.0;
    }
    uint32_t state = 0x12345678u;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += u01(&state);
    }
    return sum / (double)n;
}

/* One-pass Welford variance (sample variance, n-1 denominator) over the same
 * uniform stream; converges toward 1/12. Exercises dependent f64 div/mul/sub
 * chains that cannot be reordered. */
double sample_stats_variance(int n) {
    if (n <= 1) {
        return 0.0;
    }
    uint32_t state = 0x12345678u;
    double mean = 0.0;
    double m2 = 0.0;
    for (int i = 1; i <= n; ++i) {
        double x = u01(&state);
        double delta = x - mean;
        mean += delta / (double)i;
        m2 += delta * (x - mean);
    }
    return m2 / (double)(n - 1);
}

/* Least-squares slope of y = 3x + 7 + deterministic integer noise in [-5,5].
 * The noise is zero-mean over any window of 11 samples, so the slope stays
 * very close to 3. Mixes i32 modular arithmetic with f64 accumulation. */
double sample_linreg_slope(int n) {
    if (n <= 1) {
        return 0.0;
    }
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; ++i) {
        double x = (double)i;
        double y = 3.0 * x + 7.0 + (double)((i * 37) % 11 - 5);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double denom = (double)n * sxx - sx * sx;
    if (denom == 0.0) {
        return 0.0;
    }
    return ((double)n * sxy - sx * sy) / denom;
}

/* Pearson correlation of a noisy linear relation; approaches 1 as n grows.
 * Drives the Newton sqrt loop twice per call on top of the accumulations. */
double sample_pearson_r(int n) {
    if (n <= 1) {
        return 0.0;
    }
    uint32_t state = 0x9e3779b9u;
    double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (int i = 0; i < n; ++i) {
        double x = (double)i / (double)n;
        double y = 2.0 * x + (u01(&state) - 0.5);
        sx += x;
        sy += y;
        sxx += x * x;
        syy += y * y;
        sxy += x * y;
    }
    double inv_n = 1.0 / (double)n;
    double cov = sxy - sx * sy * inv_n;
    double vx = sxx - sx * sx * inv_n;
    double vy = syy - sy * sy * inv_n;
    if (vx <= 0.0 || vy <= 0.0) {
        return 0.0;
    }
    return cov / (fa_sqrt(vx) * fa_sqrt(vy));
}

/* Monte Carlo pi: fraction of PRNG points inside the unit quarter-circle.
 * Deterministic for a given (n, seed); the hit count is integer-exact. */
double sample_monte_carlo_pi(int n, int seed) {
    if (n <= 0) {
        return 0.0;
    }
    uint32_t state = (uint32_t)seed;
    if (state == 0u) {
        state = 1u;
    }
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        double x = u01(&state);
        double y = u01(&state);
        if (x * x + y * y <= 1.0) {
            ++hits;
        }
    }
    return 4.0 * (double)hits / (double)n;
}

/* Sieve of Eratosthenes over a static buffer (memset -> memory.fill with
 * -mbulk-memory). pi(10000) == 1229, pi(100000) == 9592. */
#define FA_SIEVE_MAX 100000
int sample_prime_count(int limit) {
    static uint8_t composite[FA_SIEVE_MAX + 1];
    if (limit < 2) {
        return 0;
    }
    if (limit > FA_SIEVE_MAX) {
        limit = FA_SIEVE_MAX;
    }
    memset(composite, 0, (unsigned)(limit + 1));
    int count = 0;
    for (int i = 2; i <= limit; ++i) {
        if (!composite[i]) {
            ++count;
            for (int64_t j = (int64_t)i * i; j <= limit; j += i) {
                composite[j] = 1;
            }
        }
    }
    return count;
}

/* n x n integer matrix multiply with a position-weighted i64 checksum.
 * Deterministic small-range entries keep every product in i32 while the
 * checksum exercises i64 widening and accumulation. */
#define FA_MAT_MAX 40
long long sample_matrix_checksum(int n) {
    static int32_t a[FA_MAT_MAX * FA_MAT_MAX];
    static int32_t b[FA_MAT_MAX * FA_MAT_MAX];
    static int32_t c[FA_MAT_MAX * FA_MAT_MAX];
    if (n < 1) {
        n = 1;
    }
    if (n > FA_MAT_MAX) {
        n = FA_MAT_MAX;
    }
    for (int i = 0; i < n * n; ++i) {
        a[i] = (i * 7 + 3) % 13 - 6;
        b[i] = (i * 5 + 1) % 11 - 5;
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            int32_t acc = 0;
            for (int k = 0; k < n; ++k) {
                acc += a[i * n + k] * b[k * n + j];
            }
            c[i * n + j] = acc;
        }
    }
    long long checksum = 0;
    for (int i = 0; i < n * n; ++i) {
        checksum += (long long)c[i] * (long long)(i + 1);
    }
    return checksum;
}

/* Single-call benchmark entry combining every kernel; useful for one-command
 * timing runs through the CLI runner. Deterministic for a given n. */
double sample_stats_pipeline(int n) {
    double acc = 0.0;
    acc += sample_stats_mean(n);
    acc += sample_stats_variance(n);
    acc += sample_linreg_slope(n);
    acc += sample_pearson_r(n);
    acc += sample_monte_carlo_pi(n, 424242);
    acc += (double)sample_prime_count(n) * 1e-3;
    acc += (double)sample_matrix_checksum(n % FA_MAT_MAX + 1) * 1e-6;
    return acc;
}
