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
- Runtime execution (`fa_runtime.*`) with call frames, locals/globals, branch stack semantics, multi-value returns, label arity checks, memory64/multi-memory behavior, and trap propagation.
- Reference operations and `call_indirect` with table lookup and signature validation, using encoded funcref storage (`null = 0`, index `n = n + 1`).
- Bulk memory and table operations, typed element expressions (`ref.func`, `ref.null`, `global.get`), and live imported memory/table rebind after attach.
- SIMD core + relaxed opcode coverage wired through `fa_ops.*` (with active regression tests).
- Host import bindings for functions, memories, and tables; dynamic-library bindings on supported desktop targets.
- JIT/microcode preparation scaffolding (`fa_jit.*`) with per-function opcode caches, optional prescan, and spill/load hooks for JIT programs and linear memory.

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
build/bin/fayasm_run ./my_module.wasm add i32:7 i32:5
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

## Architecture At a Glance

- `src/fa_runtime.*`: execution loop, frames, locals/globals, memory/table plumbing, trap + spill/load hooks, host bindings.
- `src/fa_ops.*`: opcode descriptors + dispatch, microcode-backed math/bit/select/float-special handlers, SIMD and ref ops.
- `src/fa_jit.*`: resource probe, budget/advantage scoring, opcode import/export, prepared-op execution.
- `src/fa_wasm.*`: parser/loader for module structure and function bodies.
- `src/fa_wasm_stream.*`: instruction cursor and immediate decoding helpers.
- `src/fa_job.*`: operand stack and register window.
- `test/`: `fayasm_test_main` harness with runtime regressions + optional wasm fixture smoke tests.

## Project Direction (Roadmap-Aligned)

### Near-term focus

- Standardize runtime-wide spill/load persistence conventions for versioned opcode + memory payloads.
- Expand smoke coverage using `wasm_samples/` modules.
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
- Update `README.md`, `AGENTS.md`, and `ROADMAP.md` when behavior or priorities change.
- Add/extend tests for runtime or opcode changes.
- Log new research under `studies/` and index it in `AGENTS.md`.

fayasm is intentionally experimental, but the direction is practical: clear runtime internals, solid regression coverage, and a path toward robust low-resource execution.

Inspired by WASM3.

Riccardo Cecchini, 2025. MIT License.
