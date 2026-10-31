# Statistical Workload Benchmarks (stats_suite.wasm)

## Bytecode and operand backends (2026-10-31)

This is a controlled comparison against pre-change revision `8db95103c625580f1388645169b86fc71c38dfb1`, which already has the hybrid operand window. It isolates this task's bytecode/macro work and fixed-slot alternative; it is not a comparison with the old linked stack.

### Method

- Apple M1 Pro, 16 GiB RAM, macOS/ARM64; AppleClang 21.0.0.21000401.
- All four runners: CMake Release, static library, C99, `-O3 -DNDEBUG`, no LTO or fast-math. Only the implementation/backend configuration differs.
- One identical fixture for all builds: Rust 1.98.1 (`wasm32-unknown-unknown`, `-O`, `panic=abort`) through `wasm_samples/build.sh`. Emscripten is unavailable on this machine for this run. SHA-256: `183619c8b6d183d1e4e6b9834b6f7cffaee03a7a3df2e729fe32c1effd801814`.
- `baseline`: pre-change eight-slot circular window. `window`: new bytecode/macros with the same window. `fixed`: new bytecode/macros with eight fixed slots. `compact`: four fixed slots, five immediate slots, depth cap 2,048 (other variants: eight immediate slots, cap 65,536).
- `mc0` / `mc1`: `FAYASM_MICROCODE=0` / `1`; both prescan flags explicitly disabled. Resource/advantage gates and the default 512-source-op preparation cap remain active. Results measure the combined change, not isolated contributions from each optimization.
- Median of three fresh-process wall times, after one excluded warmup per workload/configuration. Variant order rotates between repetitions. Includes process startup, module parsing, attach, cache preparation, and execution; no baseline subtraction. Timings are end-to-end, not warmed in-process execution.
- Every stdout result agrees exactly across all eight variants. The eight large-input results also match the same C kernels compiled with `cc -O2 -ffp-contract=off`, printed to 17 significant digits. Regression tests separately cover small fixture inputs. No new native slowdown ratios are inferred from July's timings.
- Script: [`benchmark_stats.py`](benchmark_stats.py). All individual measurements and outputs: [`stats_2026-10-31.json`](stats_2026-10-31.json). An earlier exploratory run overlapped compilation; only the final run, with no concurrent builds, is reported here.

### Results

Microcode enabled; median whole-process time in milliseconds.

| Workload | Before (window 8) | New window 8 | New fixed 8 | New fixed 4 |
| --- | ---: | ---: | ---: | ---: |
| mean(500k) | 964.16 | 887.68 | 880.30 | 882.07 |
| variance(500k) | 1753.65 | 1619.28 | 1606.81 | 1602.32 |
| linreg(500k) | 1734.35 | 1625.20 | 1622.49 | 1614.13 |
| pearson(500k) | 2555.97 | 2342.80 | 2317.29 | 2322.86 |
| pi(500k) | 2262.40 | 2100.73 | 2074.61 | 2083.84 |
| sieve(100k) | 323.45 | 306.62 | 306.63 | 303.12 |
| matrix(40) | 109.62 | 103.79 | 102.98 | 103.29 |
| pipeline(100k) | 2175.22 | 2044.36 | 2027.94 | 2017.64 |
| startup | 5.00 | 5.43 | 4.96 | 4.90 |

Descriptor-only controls; median whole-process time in milliseconds.

| Workload | Before (window 8) | New window 8 | New fixed 8 | New fixed 4 |
| --- | ---: | ---: | ---: | ---: |
| mean(500k) | 955.95 | 966.98 | 963.00 | 956.99 |
| variance(500k) | 1732.71 | 1720.19 | 1702.92 | 1706.85 |
| linreg(500k) | 1714.68 | 1711.53 | 1711.41 | 1702.95 |
| pearson(500k) | 2524.73 | 2504.13 | 2491.14 | 2490.71 |
| pi(500k) | 2233.56 | 2263.80 | 2256.18 | 2245.48 |
| sieve(100k) | 322.08 | 319.96 | 319.02 | 316.52 |
| matrix(40) | 108.55 | 108.95 | 109.26 | 107.95 |
| pipeline(100k) | 2145.86 | 2151.24 | 2134.93 | 2133.92 |
| startup | 5.10 | 5.45 | 4.74 | 4.85 |

Across the eight nontrivial kernels, the new circular microcode path reduces time by **5.2–8.3%** versus pre-change microcode; fixed-eight by **5.2–9.3%**. Fixed-eight differs from new circular by about **0–1.2%**, too small to infer a general backend win from three repetitions. Descriptor-only timings are close to the control, with small regressions on some workloads (up to about 1.4%). The improvement is primarily in the enabled microcode/bytecode path. Startup is roughly 5 ms here and is sensitive to process-launch noise.

### What changed and what remains

1. `REGISTER_BINARY_OPS` emits 24 typed handlers and their microcode mappings from one list: i32/i64 add/sub/mul, and/or/xor, left/logical-right shifts, and f32/f64 add/sub/mul/div. Exact typed operands are read before mutation; one pop and an in-place result replace two pops, generic conversion, and a push. Integer arithmetic wraps through unsigned operations, including the generic signed add/sub/mul fallback. Underflow and cross-kind calls retain the original checked handlers. The destination is reacquired after pop because circular refill can move it. These handlers serve both descriptor microcode dispatch and prepared programs, including instructions beyond a prepared chunk's capped prefix.
2. Prepared records bind local/global indices and scalar constants after successful common decode. Loops replay the immediate bytes and next PC; bound scalar and immediate-free numeric instructions bypass control-context setup. Memory, calls, branches, and prefixed instructions retain the existing decoder. This is bounded portable bytecode preparation, not native machine-code generation or whole-function fusion. Rebuilding/evicting a program discards bindings; loading its opcode-only spill reconstructs unbound records that bind again.
3. The fixed alternative maps operand depths `0..N-1` to stable inline virtual slots; newer values overflow to retained contiguous storage. It needs no circular arithmetic, rotation, or refill. At depths greater than N, its hottest operands are in overflow storage, whereas the circular backend keeps the newest N inline. Both preserve typing, stack order, cap failures, call-frame bases, and allocation-free shallow operation. They do not reserve physical CPU registers.
4. Keep the circular backend as the default. Backend differences on this native suite are small and workload-dependent. The compact profile is an option for testing footprint/performance tradeoffs on small processors, not evidence of ESP32 speed. Actual embedded execution, flash/code size, repeated low-RAM offload, and hardware register allocation remain unmeasured.

### Footprint and compatibility

On this ARM64 build, `sizeof(fa_JobValue) = 24`; default `fa_JobStack = 232` and `fa_Job = 648` bytes for either backend. The compact four-slot/five-immediate profile gives `fa_JobStack = 136`, `fa_Job = 408` bytes (240 bytes / 37.0% less per job before any overflow allocation). The savings come from reducing slot counts, not changing the backend alone; a circular build with the same counts has the same struct sizes. These are host ABI sizes, not measurements on a 32-bit MCU.

`fa_JitPreparedOp` grows from 48 to 56 bytes on ARM64. At the default 512-op cap that is 4 KiB more per full prepared chunk, charged to the existing program budget. Scalar bindings are inline and allocate no additional per-instruction buffers. The Mach-O runner's page-rounded `__TEXT` segment grows from 180,224 to 196,608 bytes with typed microcode; eight-slot circular and fixed runners have the same rounded size. These segment sizes are not embedded flash estimates.

Rebuild consumers: `fa_JitPreparedOp` and `fa_Runtime` public struct layouts changed. CMake propagates the fixed-backend define to linked consumers; slot/depth compile flags must also agree with the library. The persisted `FA_SPILL_VERSION` stays at 1 because the opcode and memory envelopes/layouts did not change.

### Validation

- Native shared/default and static circular/fixed builds pass the full harness with generated Rust fixtures present (no fixture skips).
- Compact fixed configuration passes the full harness, including forced-prescan invocation and five-immediate multi-memory SIMD lane handling.
- AddressSanitizer + UndefinedBehaviorSanitizer pass the full harness for default circular and compact fixed configurations. LeakSanitizer is unavailable on this macOS host (`detect_leaks=0`); UBSan runs with `halt_on_error=1`.
- Focused regressions cover all typed macro handlers against descriptor execution across inline/overflow boundaries, integer wraparound, masked shifts, signed zero/infinity, underflow/type-failure retention, four scalar constant kinds, changing local arguments, cache counter reset, and opcode restoration without bound bytes. Existing offload/reload and large-body prepare-once regressions continue to pass.

### Reproduce the current variants

Run from the repository root, leaving fixtures unchanged throughout the comparison:

```sh
./wasm_samples/build.sh
cmake -S . -B build/perf-window -DCMAKE_BUILD_TYPE=Release -DFAYASM_BUILD_SHARED=OFF
cmake --build build/perf-window
cmake -S . -B build/perf-fixed -DCMAKE_BUILD_TYPE=Release -DFAYASM_BUILD_SHARED=OFF -DFAYASM_FIXED_REGISTERS=ON
cmake --build build/perf-fixed
cmake -S . -B build/perf-compact -DCMAKE_BUILD_TYPE=Release -DFAYASM_BUILD_SHARED=OFF -DFAYASM_FIXED_REGISTERS=ON \
  '-DCMAKE_C_FLAGS=-DFA_JOB_REGISTER_WINDOW_SIZE=4 -DFA_JOB_DATA_FLOW_WINDOW_SIZE=5 -DFA_JOB_STACK_MAX_DEPTH=2048'
cmake --build build/perf-compact
python3 studies/runtime/benchmark_stats.py \
  --variant window=build/perf-window/bin/fayasm_run \
  --variant fixed=build/perf-fixed/bin/fayasm_run \
  --variant compact=build/perf-compact/bin/fayasm_run \
  --output /tmp/fayasm-stats.json
```

To include the historical control without switching the working tree, export revision `8db95103c625580f1388645169b86fc71c38dfb1` into a separate temporary source directory with `git archive`, configure/build it with the same Release/static options, and add `--variant baseline=/path/to/that/build/bin/fayasm_run` to the script. All variants use the fixture path supplied to the script, not any fixture in the control source tree. Omit concurrent builds and other heavy work while timing. `--repeats 5` provides a longer run; `--microcode 1` restricts the comparison to the enabled path.

## Historical register-forwarding window comparison (2026-07-31)

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

## Historical pre-window methodology (2026-07-04)

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

## Historical pre-window results (post prepare-once fix)

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

## Historical findings

1. **Microcode currently does not pay off on real workloads.** With the
   dispatch already table-driven, prepared-op execution was generally near
   plain descriptor dispatch, with overhead varying by kernel and reaching
   about 10% in this table (the per-instruction prepared-op lookup costs
   about what it saves). Any further
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

## Reproducing the historical workload

```bash
./wasm_samples/build.sh                      # build fixtures (emcc or rustc)
build/bin/fayasm_test_main wasm_sample_stats # correctness
FAYASM_MICROCODE=1 build/bin/fayasm_run wasm_samples/build/stats_suite.wasm \
    sample_stats_pipeline i32:100000         # timing entry point
```
