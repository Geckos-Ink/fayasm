# Statistical Workload Benchmarks (stats_suite.wasm)

## Register-forwarding window comparison (2026-07-31)

A focused before/after comparison measured the hybrid operand forwarding
change against repository `HEAD` `bf76305` on the same Apple M1 Pro. Both
trees used AppleClang 21, a static Release build (`-O3 -DNDEBUG`), the existing
Emscripten `stats_suite.wasm`, and best-of-three whole-process wall time. These
numbers include parse/startup time; the comparison is still valid because the
binary, fixture, invocation, and build options differ only by the working-tree
stack/window implementation.

| `sample_stats_pipeline(100000)` | linked operand nodes | hybrid window | change |
| --- | ---: | ---: | ---: |
| descriptor dispatch (`FAYASM_MICROCODE=0`) | 3.43 s | 2.14 s | -37.6% |
| prepared dispatch (`FAYASM_MICROCODE=1`) | 3.47 s | 2.18 s | -37.2% |

Both implementations returned the identical f64 result
`17.215848559835482`. The focused regression also performs 200,000 shallow
push/pop cycles without creating spill storage. This establishes the intended
property: execution count alone does not grow stack memory; only live operand
depth beyond the inline window does. It does not establish the same speedup
for every workload or embedded target.

Implementation characteristics measured here:

- the top eight typed operands stay in a circular inline window;
- deeper live operands use one geometrically grown contiguous spill array,
  retained across job resets;
- decoded immediates use a separate fixed eight-entry inline window, with no
  per-immediate allocation and no silent eviction;
- live operand depth is capped by the overridable `FA_JOB_STACK_MAX_DEPTH`
  boundary (65,536 by default).

The broader full-suite benchmark below was recorded on 2026-07-04. Machine:
Apple M1 Pro, 16 GB RAM, macOS (arm64). Toolchains:
Emscripten 5.0.3 (`-O2 -s STANDALONE_WASM -mbulk-memory`), native reference
`cc -O2 -ffp-contract=off` (FP contraction disabled so native IEEE f64 results
match wasm semantics operation-for-operation).

## Methodology

- Fixture: `wasm_samples/src/stats_suite.c` (PRNG, Welford variance, linear
  regression, Pearson correlation, Monte Carlo pi, sieve of Eratosthenes,
  integer matrix multiply + i64 checksum).
- fayasm timing: wall time of `build/bin/fayasm_run` (module parse + execute),
  best of 3, minus a trivial-call startup baseline (~10-12 ms, measured with
  `sample_prime_count i32:2` best of 5).
- Native timing: same C file compiled natively, kernel-only timing via
  `clock_gettime(CLOCK_MONOTONIC)`, best of 5 in-process repetitions.
- Correctness cross-check: every export produces **bit-identical** f64/i64/i32
  results across fayasm (emcc build), fayasm (rustc fallback build), and the
  native reference.

## Results (post prepare-once fix)

| Workload | fayasm (µcode on) | fayasm (µcode off) | native -O2 | slowdown vs native |
| --- | ---: | ---: | ---: | ---: |
| mean(500k)            | 2285 ms | 2200 ms | 1.27 ms | ~1,800x |
| variance(500k)        | 4258 ms | 4128 ms | 3.66 ms | ~1,150x |
| linreg_slope(500k)    | 3664 ms | 3616 ms | 0.83 ms | ~4,400x |
| pearson_r(500k)       | 5835 ms | 5305 ms | 1.37 ms | ~4,000x |
| monte_carlo_pi(500k)  | 5451 ms | 5273 ms | 2.53 ms | ~2,100x |
| prime_count(100k)     |  718 ms |  803 ms | 0.26 ms | ~2,900x |
| matrix_checksum(40)   |  318 ms |  321 ms | 0.03 ms | ~11,500x |
| pipeline(100k)        | 5154 ms | 4780 ms | 1.88 ms | ~2,700x |

Interpreter throughput lands around **5-7 million wasm instructions/second**
on this machine (~150-200 ns/op). The worst ratios (linreg, matrix) are
kernels that native LLVM auto-vectorizes; the ratio there measures SIMD +
register allocation vs a structural interpreter, not just dispatch overhead.

## Findings

1. **Microcode currently does not pay off on real workloads.** With the
   dispatch already table-driven, prepared-op execution is within +/-5% of
   plain descriptor dispatch on every kernel (usually slightly slower: the
   per-instruction prepared-op lookup costs about what it saves). Any further
   microcode expansion should be justified by a measured win on these
   fixtures.
2. **Fixed: per-instruction re-preparation of large function bodies.** Before
   the fix, any function with more recorded opcodes than
   `max_ops_per_chunk` (512) — e.g. the inlined `sample_stats_pipeline` body —
   failed the "already prepared" check on every instruction and rebuilt the
   whole microcode program each time: pipeline(100k) took 44.3 s with
   microcode on vs 4.7 s off (~9.5x). After tracking the budget-capped
   prepared source count (`prepared_source_count`), it is 5.15 s (~8%
   overhead). Regression test: `test_jit_large_body_prepare_once` asserts a
   second execution of a >512-opcode body performs zero program rebuilds via
   the `jit_program_prepare_calls` counter.
3. **IEEE f64 semantics are exact.** All stats kernels (including dependent
   Welford chains and Newton sqrt loops) reproduce native results
   bit-identically when the native build disables FP contraction — a good
   ongoing invariant to test against: expected values in the smoke tests are
   native-derived constants under a tight 1e-9 tolerance.
4. **Startup is cheap.** Module parse + attach + trivial call is ~10-12 ms
   for the ~50 KB stats module; execution dominates everything else.

## Reproducing

```bash
./wasm_samples/build.sh                      # build fixtures (emcc or rustc)
build/bin/fayasm_test_main wasm_sample_stats # correctness
FAYASM_MICROCODE=1 build/bin/fayasm_run wasm_samples/build/stats_suite.wasm \
    sample_stats_pipeline i32:100000         # timing entry point
```
