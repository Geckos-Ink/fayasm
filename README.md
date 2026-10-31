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
- Portable prepared bytecode/microcode (`fa_jit.*`) with serial pre-stacked instruction sequences, macro-generated scalar/memory delegates, cached decoded arguments, per-function opcode caches, optional prescan, and spill/load hooks for JIT programs and linear memory, plus a runtime-wide versioned spill envelope (`FA_SPILL_*`) for portable, endianness-stable persistence of JIT programs and memory.
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
- `FAYASM_JIT_SEQUENCES=0` disables serial prepared execution for comparisons (enabled by default). Bound sequences use a temporary flat register bank, synchronize at control/call/host boundaries, and restore the public stack on every exit. Resource probes occur at context initialization, not on each statistics update.
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
Release builds, using one unchanged Rust 1.98.1 `stats_suite.wasm` fixture.
Median of three fresh-process wall times after an excluded warmup, including
startup, parsing, preparation, and execution. Microcode is enabled; prescan is off.

“Before” is `bd7b632`. The middle column uses current code with
`FAYASM_JIT_SEQUENCES=0`, isolating probe reuse from serial execution.

| Workload | Before | Current, sequences off | Serial circular | Speedup vs before |
| --- | ---: | ---: | ---: | ---: |
| Mean, 500k | 891.7 ms | 404.1 ms | 72.1 ms | 12.4× |
| Welford variance, 500k | 1724.7 ms | 761.6 ms | 120.3 ms | 14.3× |
| Linear regression, 500k | 1663.5 ms | 773.1 ms | 124.1 ms | 13.4× |
| Pearson correlation, 500k | 2361.3 ms | 1109.3 ms | 170.5 ms | 13.8× |
| Monte Carlo pi, 500k | 2111.5 ms | 972.5 ms | 155.7 ms | 13.6× |
| Sieve, n=100k | 306.3 ms | 166.8 ms | 50.3 ms | 6.1× |
| 40×40 matrix multiply | 104.5 ms | 59.5 ms | 14.9 ms | 7.0× |
| Combined pipeline, n=100k | 2091.1 ms | 994.6 ms | 380.3 ms | 5.5× |

The combined change is **5.5–14.3× faster** on these workloads;
serial execution adds **2.6–6.5×** beyond the cached-probe control.
All outputs agree exactly across configurations and with the recorded native C
reference values. These are portable precompiled C delegates sharing temporary
registers, not native machine-code generation. Cold instructions, calls/control,
and operations beyond the capped prepared prefix retain interpreter overhead.

Fixed-four-slot results are close to circular, so circular remains the default.
The compact profile keeps `fa_Job` at **408 bytes** versus **648 bytes** by
default on ARM64. Prepared records now cost **120 bytes** (60 KiB per full
512-op chunk), and the register bank adds C-stack use. Generated native code
also grows; embedded speed and footprint still need hardware measurements.

See the [current benchmark study](studies/runtime/sequence_benchmarks.md) for
fixed-backend results, costs, validation, and reproduction commands;
[raw samples](studies/runtime/sequence_2026-10-31.json) and the
[comparison script](studies/runtime/benchmark_stats.py) are included.
[Earlier results](studies/runtime/stats_benchmarks.md) remain archived separately.

## Architecture At a Glance

- `src/fa_runtime.*`: execution loop, frames, locals/globals, memory/table plumbing, trap + spill/load hooks, host bindings.
- `src/fa_ops.c` / `src/fa_ops_*.c`: the serial sequence executor and central opcode registry and public dispatch, with focused translation units for shared typed-value helpers, control/scalar memory/ref ops, numeric microcode, calls/tables, `0xFC` bulk operations, and `0xFD` SIMD/relaxed-SIMD families.
- `src/fa_jit.*`: resource probe, budget/advantage scoring, opcode import/export, prepared-op execution. Prepared records bind decoded arguments and links once; a direct sequence of precompiled C delegates executes with shared temporary registers. All scalar numeric and scalar memory opcodes have macro-generated typed delegates. Generic operations synchronize registers and stop at the host boundary. Binding metadata counts toward cache budgets and is rebuilt after spill/load.
- `src/fa_wasm.*`: parser/loader for module structure and function bodies.
- `src/fa_wasm_stream.*`: instruction cursor and immediate decoding helpers.
- `src/fa_job.*`: selectable circular forwarding or fixed operand slots, with bounded retained contiguous overflow storage and a separate allocation-free decoded-immediate window. Shallow/steady computations do not allocate per operation.
- `test/`: `fayasm_test_main` harness with runtime regressions + optional wasm fixture smoke tests.

`fa_JobStack`, `fa_Job`, `fa_JitPreparedOp`, `fa_JitConfig`, and `fa_Runtime` are public C
struct layouts in this experimental API. Serial-bytecode fields change layout for precompiled consumers;
rebuild applications with the matching fayasm headers and use the stack/window
helpers instead of addressing storage fields directly.

## Project Direction (Roadmap-Aligned)

### Near-term focus

- Spill/load persistence is now standardized around a runtime-wide versioned envelope (`FA_SPILL_*`) for JIT programs and linear memory; extend the same convention to any further persisted runtime state as it lands.
- Extend typed microcode, scalar bytecode binding, and bounded instruction fusion only where `stats_suite` measurements justify their code/cache footprint. Calls/control remain synchronization boundaries; cold operations, generic adapters, and instructions beyond the capped prepared prefix retain ordinary dispatch.
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
