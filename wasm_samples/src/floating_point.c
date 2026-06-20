#include <stdint.h>

/*
 * Floating-point smoke fixtures: the first wasm_samples coverage for f32/f64
 * arithmetic, int<->float conversions, and float comparisons through real
 * toolchain output. Exercises runtime paths (f64.add/mul/sub/div, f32.*,
 * f64.sqrt-free Newton iteration, i32.trunc_f64_s, f64.convert_i32_s,
 * f64.lt/gt) that the integer-only smoke samples never touched.
 *
 * libm is intentionally avoided (sqrt is open-coded) so the modules stay
 * standalone with no imports.
 */

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

/* Horner-form polynomial 3x^3 - 2x^2 + x - 5. f(2.0) == 13.0 exactly. */
double sample_f64_poly(double x) {
    return ((3.0 * x - 2.0) * x + 1.0) * x - 5.0;
}

/* Euclidean distance; hypot(3,4) == 5.0. Exercises f64.mul/add + sqrt loop. */
double sample_f64_hypot(double a, double b) {
    return fa_sqrt(a * a + b * b);
}

/* Linear interpolation in f32. lerp(0,10,0.5) == 5.0f exactly. */
float sample_f32_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/* Round half away from zero, returning i32 (f64->i32 conversion path). */
int sample_f64_round(double x) {
    if (x >= 0.0) {
        return (int)(x + 0.5);
    }
    return (int)(x - 0.5);
}

/* Accumulate 1/1 + 1/2 + ... contributions scaled to an exact f64.
 * Sums i*1.5 for i in [1..n] => 1.5 * n*(n+1)/2. For n=10 => 82.5. Mixes
 * i32.convert and f64 arithmetic inside a loop. */
double sample_f64_series(int n) {
    double total = 0.0;
    for (int i = 1; i <= n; ++i) {
        total += (double)i * 1.5;
    }
    return total;
}
