# fayasm 🔥

fayasm is an experimental WebAssembly runtime in C99 built for people who want to understand execution internals, not hide them.

Instead of presenting a black box, fayasm keeps parsing, stack handling, control flow, traps, and host bindings visible and hackable. It is useful for learning, runtime prototyping, and embedded-focused experiments where you want clear control over tradeoffs.

## Why This Project Exists

Most runtimes optimize for performance and spec completeness first. fayasm takes a different path:

- Keep code paths understandable so new contributors can trace behavior quickly.
- Make runtime internals easy to test and evolve.
- Support practical experiments: host imports, table/memory operations, microcode preparation, and offload hooks for low-RAM targets.

## What Works Today

fayasm already supports a substantial runtime slice:

- Real `.wasm` parsing from disk or memory (`fa_wasm.*`) including types, functions, exports, globals, memories, tables, element segments, and data segments.
- Runtime execution (`fa_runtime.*`) with call frames, locals/globals, branch stack semantics, nested-call operand preservation, multi-value returns, label arity checks, memory64/multi-memory behavior, and trap propagation.
- Reference operations and `call_indirect` with table lookup and signature validation, using encoded funcref storage (`null = 0`, index `n = n + 1`).
- Bulk memory and table operations, typed element expressions (`ref.func`, `ref.null`, `global.get`), and live imported memory/table rebind after attach.
- Scalar integer<->float conversions including the non-trapping saturating truncations (`i32`/`i64.trunc_sat_f32`/`f64_s`/`_u`, `0xFC 0x00`–`0x07`) that toolchains emit by default for `(int)`/`(long)` casts of floats.
- SIMD core + relaxed opcode coverage wired through `fa_ops.*` (with active regression tests).
- Host import bindings for functions, memories, and tables; dynamic-library bindings on supported desktop targets.
- JIT/microcode preparation scaffolding (`fa_jit.*`) with per-function opcode caches, optional prescan, and spill/load hooks for JIT programs and linear memory, plus a runtime-wide versioned spill envelope (`FA_SPILL_*`) for portable, endianness-stable persistence of JIT programs and memory.
- `fa_ops.*` now routes control/local/global/ref/table plus `0xFC` bulk-memory/table families through prebuilt delegate tables, and the `0xFD` SIMD/relaxed-SIMD prefix dispatches through a prebuilt family-handler table (`g_simd_dispatch`) instead of a 347-case switch tower, reducing per-call dispatch to a single indexed lookup.

## Quickstart (Native, Recommended)

### 1) Build everything and run the test harness

```bash
./build.sh
```

Default native behavior:

- Cleans and configures `build/`
- Builds shared + static libraries
- Builds tools (including `build/bin/fayasm_run`)
- Builds fixtures from `wasm_samples/` when toolchains are available
- Runs `build/bin/fayasm_test_main`

### 2) Explore and filter tests

```bash
build/bin/fayasm_test_main --list
build/bin/fayasm_test_main call_indirect
build/bin/fayasm_test_main wasm_sample
```

### 3) Run a WASM export from the CLI runner

```bash
build/bin/fayasm_run wasm_samples/build/arithmetic.wasm sample_const42
# result[0] (i32): 42

build/bin/fayasm_run wasm_samples/build/control_flow.wasm sample_factorial_6
# result[0] (i32): 720
```

If your module has parameters, pass typed args:

```bash
build/bin/fayasm_run wasm_samples/build/typed_values.wasm sample_add_i32 i32:7 i32:5
# result[0] (i32): 12

build/bin/fayasm_run wasm_samples/build/typed_values.wasm sample_scale_i64 i64:100000 i64:100000
# result[0] (i64): 10000000001
```

The advanced fixtures exercise floating point, `call_indirect`/`br_table`, and bulk memory:

```bash
build/bin/fayasm_run wasm_samples/build/floating_point.wasm sample_f64_hypot f64:3 f64:4
# result[0] (f64): 5

build/bin/fayasm_run wasm_samples/build/indirect_dispatch.wasm sample_dispatch i32:2 i32:6 i32:7
# result[0] (i32): 42   (call_indirect selects multiply)

build/bin/fayasm_run wasm_samples/build/memory_ops.wasm sample_buffer_pipeline i32:7 i32:64
# result[0] (i32): 1380176480   (memcpy/memset -> memory.copy/memory.fill)

build/bin/fayasm_run wasm_samples/build/integer_ops.wasm sample_signed_divrem i32:-100 i32:7
# result[0] (i32): -436   (signed div/rem with truncation toward zero)
```

The statistical suite (`stats_suite.wasm`) doubles as a benchmark workload:

```bash
build/bin/fayasm_run wasm_samples/build/stats_suite.wasm sample_monte_carlo_pi i32:10000 i32:42
# result[0] (f64): 3.1551999999999998

build/bin/fayasm_run wasm_samples/build/stats_suite.wasm sample_prime_count i32:10000
# result[0] (i32): 1229

build/bin/fayasm_run wasm_samples/build/stats_suite.wasm sample_stats_pipeline i32:2000
# result[0] (f64): 7.9224987970405749   (all kernels in one call)
```

Supported CLI arg types:

- `i32:<value>`
- `i64:<value>`
- `f32:<value>`
- `f64:<value>`

## Manual Build (CMake)

```bash
mkdir -p build
cd build
cmake .. \
  -DFAYASM_BUILD_TESTS=ON \
  -DFAYASM_BUILD_SHARED=ON \
  -DFAYASM_BUILD_STATIC=ON
cmake --build .
ctest --output-on-failure
```

## Build Fixtures Only

```bash
./wasm_samples/build.sh
```

The script prefers Emscripten and falls back to Rust (`wasm32-unknown-unknown`) when needed.

## ESP32 / Embedded Flow

Use the target-aware wrapper:

```bash
./build.sh --target esp32 --esp-idf-path /Users/riccardo/esp/esp-idf --no-fixtures
```

Useful overrides:

```bash
./build.sh --target esp32 \
  --esp-idf-path /Users/riccardo/esp/esp-idf \
  --esp-ram-bytes 262144 \
  --esp-cpu-count 2 \
  --cmake-arg -DFAYASM_BUILD_SHARED=OFF
```

Notes:

- ESP32 targeting is compile-time (`FAYASM_TARGET_ESP32`, `FAYASM_TARGET_*`).
- Embedded builds intentionally avoid `dlopen`/`dlsym`; dynamic-library host binding returns `FA_RUNTIME_ERR_UNSUPPORTED`.

## Runtime Tuning Knobs

- `FAYASM_MICROCODE=1|0` to force-enable/disable microcode tables (otherwise resource-gated: RAM/CPU probe).
- `FAYASM_JIT_PRESCAN=1` to enable per-function prescan.
- `FAYASM_JIT_PRESCAN_FORCE=1` to force prescan mode.
- `FAYASM_TARGET_RAM_BYTES` / `FAYASM_TARGET_CPU_COUNT` compile-time hints for embedded probes.

## Performance Snapshot

Measured 2026-07 on an Apple M1 Pro (16 GB) with the `stats_suite.wasm`
fixture (Emscripten 5.0.3, `-O2`), best-of-3 wall time through
`fayasm_run` with the ~10 ms startup baseline subtracted, against the same C
compiled natively (`cc -O2 -ffp-contract=off`). Full data and methodology:
`studies/runtime/stats_benchmarks.md`.

| Workload | fayasm | native -O2 | slowdown |
| --- | ---: | ---: | ---: |
| Welford variance, 500k samples | 4.3 s | 3.7 ms | ~1,150x |
| Monte Carlo pi, 500k points | 5.5 s | 2.5 ms | ~2,100x |
| Sieve of Eratosthenes, n=100k | 0.7 s | 0.26 ms | ~2,900x |
| 40x40 int matrix multiply | 0.32 s | 0.03 ms | ~11,500x |
| Combined stats pipeline, n=100k | 5.2 s | 1.9 ms | ~2,700x |

What the numbers say:

- The structural interpreter executes roughly **5-7 million wasm
  instructions/second** (~150-200 ns/op) on this class of hardware. fayasm
  optimizes for clarity and hackability, not raw speed; the worst ratios are
  kernels native LLVM auto-vectorizes.
- **Numerical results are bit-identical to native IEEE semantics**: every
  stats kernel (dependent Welford chains, Newton sqrt loops, PRNG-driven
  Monte Carlo) reproduces the native reference exactly, under both the emcc
  and rustc fixture builds.
- Microcode-prepared dispatch is currently within +/-5% of plain descriptor
  dispatch on real workloads — extensions to microcode coverage should be
  justified by measured wins on these fixtures.
- Benchmarking exposed (and fixed) a real defect: function bodies larger than
  the JIT budget cap (512 recorded opcodes) were re-preparing their microcode
  program on **every executed instruction** (~9.5x slowdown on the inlined
  pipeline body). The runtime now tracks the budget-capped prepared count
  (`test_jit_large_body_prepare_once` guards the fix).

## Architecture At a Glance

- `src/fa_runtime.*`: execution loop, frames, locals/globals, memory/table plumbing, trap + spill/load hooks, host bindings.
- `src/fa_ops.c` / `src/fa_ops_*.c`: the central opcode registry and public dispatch, with focused translation units for shared typed-value helpers, control/scalar memory/ref ops, numeric microcode, calls/tables, `0xFC` bulk operations, and `0xFD` SIMD/relaxed-SIMD families.
- `src/fa_jit.*`: resource probe, budget/advantage scoring, opcode import/export, prepared-op execution.
- `src/fa_wasm.*`: parser/loader for module structure and function bodies.
- `src/fa_wasm_stream.*`: instruction cursor and immediate decoding helpers.
- `src/fa_job.*`: operand stack and register window.
- `test/`: `fayasm_test_main` harness with runtime regressions + optional wasm fixture smoke tests.

## Project Direction (Roadmap-Aligned)

### Near-term focus

- Spill/load persistence is now standardized around a runtime-wide versioned envelope (`FA_SPILL_*`) for JIT programs and linear memory; extend the same convention to any further persisted runtime state as it lands.
- Continue replacing remaining per-family switch/subopcode towers under `src/fa_ops_*.c`; the prefix families (control/local/global/ref/table, `0xFC`, `0xFD` SIMD) are now table-driven, leaving per-family operator selection as the incremental microcode target.
- Expand smoke coverage using `wasm_samples/` modules; real-toolchain integer div/rem, bit-count, and rotate coverage now complements the floating-point, indirect-call, and bulk-memory fixtures.
- Validate repeated offload cycles on low-RAM targets.

### Medium-term focus

- Broaden smoke coverage toward non-SIMD language/toolchain outputs.
- Add low-footprint runtime validation passes for ESP32-class targets (tables, call depth, spill/load cycles).

### Long-term direction

- Explore background offload/prefetch with wear-aware storage strategies.
- Validate and tune embedded resource heuristics (`FAYASM_TARGET_*`) across more targets.

## Repository Layout

- `src/` - runtime, parser, opcode, JIT, and architecture code.
- `test/` - regression and smoke harness (`fayasm_test_main`).
- `samples/cli-runner` - standalone CLI executor (`fayasm_run`).
- `samples/host-import` - dynamic-library host import example.
- `samples/esp32-trap` - trap + SD-backed offload example.
- `wasm_samples/` - fixture sources and builder script.
- `studies/` - research archive for runtime/JIT/WASM investigations.
- `ROADMAP.md` - active planning priorities.
- `AGENTS.md` - AI collaborator reference and workflow rules.

## Contributing

- Keep changes incremental and test-backed.
- Treat `AGENTS.md` as the operational ownership/contract map: review it after every code change and update it, `README.md`, and `ROADMAP.md` whenever their documented behavior, commands, or priorities change.
- Add/extend tests for runtime or opcode changes.
- When editing the `src/fa_ops*.c` family, keep the central dispatch comments and the numeric/SIMD macro-family comments updated with the code paths they describe.
- Log new research under `studies/` and index it in `AGENTS.md`.

fayasm is intentionally experimental, but the direction is practical: clear runtime internals, solid regression coverage, and a path toward robust low-resource execution.

**Inspired by [WASM3](https://github.com/wasm3/wasm3).**

Riccardo Cecchini, 2025. MIT License.
