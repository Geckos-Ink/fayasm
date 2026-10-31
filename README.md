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
- Portable prepared bytecode/microcode (`fa_jit.*`) with macro-generated typed binary handlers, cached scalar immediates, per-function opcode caches, optional prescan, and spill/load hooks for JIT programs and linear memory, plus a runtime-wide versioned spill envelope (`FA_SPILL_*`) for portable, endianness-stable persistence of JIT programs and memory.
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
- `FA_JOB_REGISTER_WINDOW_SIZE` controls inline operand slots for either backend (default 8).
- CMake `-DFAYASM_FIXED_REGISTERS=ON` selects fixed virtual operand slots instead of circular forwarding. The first eight operands stay at stable indices; deeper values use retained contiguous storage. This portable C99 alternative does not reserve physical CPU registers. The default remains the circular window.
- `FA_JOB_STACK_MAX_DEPTH` caps live operands before a push fails safely (default 65,536; lower it for constrained builds).
- `FA_JOB_DATA_FLOW_WINDOW_SIZE` controls decoded-immediate forwarding (default 8, minimum 5 for current SIMD/multi-memory decoding).

For a smaller fixed-slot profile (the window sizes also affect public struct layouts):

```bash
cmake -S . -B build/fixed -DCMAKE_BUILD_TYPE=Release \
  -DFAYASM_FIXED_REGISTERS=ON \
  '-DCMAKE_C_FLAGS=-DFA_JOB_REGISTER_WINDOW_SIZE=4 -DFA_JOB_DATA_FLOW_WINDOW_SIZE=5 -DFA_JOB_STACK_MAX_DEPTH=2048'
cmake --build build/fixed
build/fixed/bin/fayasm_test_main
```

Both backends retain overflow capacity across resets and fail safely at the
configured depth cap. The fixed backend avoids rotation/refill; the circular
backend keeps the newest operands inline at greater depths. Measure both on
the target processor. Microcode table enablement and JIT resource gates remain
separate: forcing `FAYASM_MICROCODE=1` does not remove the JIT cache budget or
RAM/CPU requirements.

## Performance Snapshot

Measured **2026-10-31** on Apple M1 Pro (16 GiB), AppleClang 21, static
Release builds. Every runner uses the same Rust 1.98.1 `stats_suite.wasm`
fixture. Values are median-of-three whole-process wall time after an excluded
warmup, including parsing, startup, preparation, and execution.
**Microcode is enabled in every column**; prescan is disabled.

“Before” is revision `8db9510`, which already used the circular window.
“Fixed 4” also reduces immediate slots to five and caps operand depth at 2,048.

| Workload | Before (window 8) | New window 8 | New fixed 8 | New fixed 4 |
| --- | ---: | ---: | ---: | ---: |
| Mean, 500k samples | 0.964 s | 0.888 s | 0.880 s | 0.882 s |
| Welford variance, 500k | 1.754 s | 1.619 s | 1.607 s | 1.602 s |
| Pearson correlation, 500k | 2.556 s | 2.343 s | 2.317 s | 2.323 s |
| Monte Carlo pi, 500k | 2.262 s | 2.101 s | 2.075 s | 2.084 s |
| Sieve, n=100k | 0.323 s | 0.307 s | 0.307 s | 0.303 s |
| 40×40 matrix multiply | 0.110 s | 0.104 s | 0.103 s | 0.103 s |
| Combined pipeline, n=100k | 2.175 s | 2.044 s | 2.028 s | 2.018 s |

The new circular microcode path takes **5.2–8.3% less time** across the eight
nontrivial kernels; fixed-eight takes **5.2–9.3% less** than pre-change
microcode. Differences between the new backends are about 0–1.2% here, so the
circular backend remains the default. Descriptor-only controls stay close to
the baseline, with small regressions on some kernels. All configurations
return identical outputs, also matching the native C reference printed to
17 significant digits.

The compact profile reduces `sizeof(fa_Job)` from **648 to 408 bytes** on this
ARM64 ABI by reducing slot counts. Fixed storage alone does not shrink the
struct. Embedded speed and 32-bit footprint still require hardware measurements.

See the [full benchmark study](studies/runtime/stats_benchmarks.md) for all
kernels, microcode-off controls, cache/code-size costs, validation, and
reproduction commands; [raw samples](studies/runtime/stats_2026-10-31.json)
and the [comparison script](studies/runtime/benchmark_stats.py) are included.
July's Emscripten/native ratios and linked-stack comparisons are preserved as
historical results there and are not mixed with these measurements.

## Architecture At a Glance

- `src/fa_runtime.*`: execution loop, frames, locals/globals, memory/table plumbing, trap + spill/load hooks, host bindings.
- `src/fa_ops.c` / `src/fa_ops_*.c`: the central opcode registry and public dispatch, with focused translation units for shared typed-value helpers, control/scalar memory/ref ops, numeric microcode, calls/tables, `0xFC` bulk operations, and `0xFD` SIMD/relaxed-SIMD families.
- `src/fa_jit.*`: resource probe, budget/advantage scoring, opcode import/export, prepared-op execution. Prepared records bind scalar local/global and constant immediates after their first decode; loops reuse them. Macro-generated typed binary handlers replace two operands with one using a single pop. Bound immediates count toward cache budgets and are rebuilt after spill/load.
- `src/fa_wasm.*`: parser/loader for module structure and function bodies.
- `src/fa_wasm_stream.*`: instruction cursor and immediate decoding helpers.
- `src/fa_job.*`: selectable circular forwarding or fixed operand slots, with bounded retained contiguous overflow storage and a separate allocation-free decoded-immediate window. Shallow/steady computations do not allocate per operation.
- `test/`: `fayasm_test_main` harness with runtime regressions + optional wasm fixture smoke tests.

`fa_JobStack`, `fa_Job`, `fa_JitPreparedOp`, and `fa_Runtime` are public C
struct layouts in this experimental API. Prepared-bytecode fields change layout for precompiled consumers;
rebuild applications with the matching fayasm headers and use the stack/window
helpers instead of addressing storage fields directly.

## Project Direction (Roadmap-Aligned)

### Near-term focus

- Spill/load persistence is now standardized around a runtime-wide versioned envelope (`FA_SPILL_*`) for JIT programs and linear memory; extend the same convention to any further persisted runtime state as it lands.
- Extend typed microcode, scalar bytecode binding, and bounded instruction fusion only where `stats_suite` measurements justify their code/cache footprint. Calls, control flow, memory, and prefixed operations retain the common decoder.
- Expand smoke coverage using `wasm_samples/` modules; real-toolchain integer div/rem, bit-count, and rotate coverage now complements the floating-point, indirect-call, and bulk-memory fixtures.
- Validate repeated offload cycles, fixed versus circular operand storage, and prepared-cache footprint on low-RAM hardware before changing defaults.

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
