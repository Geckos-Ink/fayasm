# fayasm 🔥

Faya pseudo-WASM runtime — an experimental, lightweight WebAssembly executor designed to be dissectable. Rather than chasing peak throughput, the project focuses on showing how a WASM module can be decoded straight from disk, scheduled through a tiny runtime, and stepped instruction by instruction. The code stays intentionally compact so it can be used as classroom material, a sand-box for opcode experiments, or the seed for embedded interpreters.

## Goal & Current State

- fayasm can parse real `.wasm` binaries using a file-descriptor driven loader (`fa_wasm.*`) or in-memory buffers via `wasm_module_init_from_memory`. Sections for types, functions, exports, memories, tables, elements, and data segments are decoded, cached, and exposed through descriptors that the runtime consumes.
- The execution runtime (`fa_runtime.*`) owns job creation, call-frame allocation, operand stack reset, a small data-flow register window, and linear memory provisioning from the module memory section. It can stream a function body, decode immediates (LEB128 helpers, const payloads, locals/globals indices, memory operands with memory indices), run a control stack for `block`/`loop`/`if`/`br`/`br_table` with operand stack unwinding (loop labels use params when present, otherwise results), block result propagation, multi-value returns, and label arity checks, and propagate traps (divide-by-zero, out-of-bounds memory, conversion overflow/NaN).
- The runtime now exposes optional function traps plus spill/load hooks for JIT microcode programs and linear memory, enabling SD-backed offload on ESP32-class devices, and it can bind imported functions, memories, and tables to host callbacks/buffers or dynamic libraries (via `dlopen`/`dlsym`) with ABI helpers for parameters/results. Imported memory/table rebinds now propagate to already-attached modules without requiring detach/attach.
- Opcode metadata lives in `fa_ops.*`. The table contains size/signing metadata, stack effects, and function pointers. Integer bitcount, float unary/special/reinterpret, locals/globals, and basic control flow are wired. Per-op handlers for bitwise/bitcount/shift/rotate plus compare/arithmetic/convert use microcode functions (gated by a RAM/CPU probe, defaults to >=64MB RAM and >=2 CPUs; override via `FAYASM_MICROCODE`), and float unary/special/reinterpret/select now map into the microcode table via per-op macro handlers instead of opcode switch-case towers.
- `fa_jit.*` introduces scaffolding for microcode JIT planning: system probes, budget/advantage scoring, runtime-fed decoded opcode streams, optional function prescans (`FAYASM_JIT_PRESCAN` / `FAYASM_JIT_PRESCAN_FORCE`), a resource-aware precompile pass that prepares per-function microcode programs within the JIT budget, and opcode export/import helpers for stable spill formats.
- `fa_job.*` provides the doubly-linked operand stack plus a fixed-size “register window” (recent values slide through `fa_JobDataFlow`). The runtime resets and reuses jobs to amortize allocations.
- A deterministic instruction stream helper (`fa_wasm_stream.*`) sits between the module parser and the tests, making it easy to assert cursor positions and encoded immediates.
- Tests under `test/` exercise the streaming helpers, branch traversal, and module scaffolding; the harness now also covers interpreter stack effects, call-depth limits, locals, globals, branching semantics (including loop labels), multi-value returns, i64/f64 arithmetic, memory64/multi-memory usage, bulk memory copy/fill, table ops, element/data segments (including typed element-expression payloads with `ref.func`/`ref.null` and externref-table initialization), SIMD v128.const/splat plus v128 load/store, lane ops, basic arithmetic, trunc_sat conversions, host import bindings (functions/memories/tables), live rebind propagation for imported memories/tables, JIT opcode serialization roundtrips, repeated memory/JIT offload cycles (including trap-driven JIT reload + eviction), and trap paths (division by zero, memory bounds, conversion overflow/NaN, global type mismatches). Optional smoke tests can also load Emscripten-built fixture modules from `wasm_samples/build`.

The interpreter deliberately stops short of executing a full program: many opcodes have placeholders, traps are surfaced as error codes, and host integration is minimal. Even so, the scaffolding for job management, frame unwinding, and constant decoding is in place and stable for further opcode work.

## Architecture Overview

- **Runtime core (`fa_runtime.*`)**: Maintains a `fa_Runtime` handle with allocator hooks, job registry (`list_t` from `helpers/dynamic_list.h`), attached module, linear memory state, and lazily sized call-frame storage. Execution is performed by `fa_Runtime_executeJob`, which streams the target function, decodes each opcode, pushes immediates into the job register window, and dispatches either cached JIT-prepared microcode ops or `fa_execute_op`; loop labels unwind using params when present, otherwise results, and the decode loop feeds per-function opcode caches for microcode preparation. Optional function traps plus spill/load hooks allow SD-backed offload of JIT programs and linear memory, and imported functions/memories/tables can be routed to host callbacks/buffers or dynamic libraries (with `fa_RuntimeHostCall_*` helpers for ABI access); imported memory/table bindings can be updated live after attach.
- **Job abstraction (`fa_job.*`)**: A job packages the operand stack (`fa_JobStack`, doubly linked) and the sliding register window (`fa_JobDataFlow`). Helper routines manage push/pop, clamp window size (`FA_JOB_DATA_FLOW_WINDOW_SIZE`), and wipe state between invocations.
- **Opcode table (`fa_ops.*`)**: A 256-entry array of `fa_WasmOp` descriptors, initialised once. Each entry encodes type info, stack deltas, immediate width, and a handler. Microcode scaffolding now pre-stacks function pointer sequences for bitwise/bitcount/shift/rotate plus compare/arithmetic/convert ops, and per-op handlers use those microcode functions (gated by a RAM/CPU probe with `FAYASM_MICROCODE` override); float unary/special/reinterpret handlers now use per-op macro functions instead of opcode switch-case towers, while utility helpers (sign extension, masking, reg-window maintenance) support arithmetic op implementations.
- **JIT planning (`fa_jit.*`)**: Lightweight scaffolding that probes resources, computes budgets/advantage scores, optionally pre-scans functions for opcodes, precompiles per-function microcode sequences within the configured budget, executes prepared-op sequences for microcode preparation and WASM-to-microcode conversion, and supports opcode-level import/export for versioned spill formats.
- **Module loader (`fa_wasm.*`)**: Reads the binary straight from disk or memory, parsing headers, sections, type signatures, function metadata, exports, memories, tables, elements, and data segments. Element segments now accept both legacy function-index vectors and typed expression vectors (`ref.func`/`ref.null`) for funcref/externref tables. Function bodies can be reloaded on demand—`wasm_load_function_body` allocates a fresh buffer per call.
- **Instruction stream (`fa_wasm_stream.*`)**: Provides `wasm_instruction_stream_*` APIs for loading function bytecode lazily, peeking/advancing the program counter, and decoding immediate operands (ULEB128/SLEB128 from memory buffers). This is the primary surface the tests touch.
- **Helper utilities**: `helpers/dynamic_list.h` is a header-only pointer vector used for the runtime job registry. `build.sh` performs a clean CMake configure, builds shared/static artefacts, and runs the `fayasm_test_main` harness with colourful logging.

## Build & Test

- Requires CMake ≥ 3.10 and a C99 toolchain.
- Standard flow:
  ```bash
  mkdir -p build
  cd build
  cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON
  cmake --build .
  ```
- `./build.sh` automates the rebuild: it optionally builds `wasm_samples/` fixtures when `emcc` is available, nukes `build/`, configures with the same flags, builds shared + static libraries, compiles `fayasm_test_main`, and executes it.
- `build.sh` skips terminal clear when `TERM` is unset/dumb, so it can run in non-interactive shells.
- Tests live under `test/` and cover cursor behaviour in `fa_wasm_stream`, branch navigation scenarios, plus interpreter regressions (stack arithmetic, call depth, traps, table ops, element/data segments including `ref.func`/`ref.null` expressions and externref tables, SIMD v128.const/splat plus load/store, lane ops, arithmetic, trunc_sat conversions). Run them with `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside `build/`; pass `--list` or a substring filter to focus on a specific area and see source hints.
- `fayasm_test_main` also accepts `--jit-prescan` and `--jit-prescan-force` to toggle JIT prescan without code changes.
- Optional real-module fixtures can be generated with Emscripten via `./wasm_samples/build.sh` (`emcc` required). Missing fixture files are reported as `SKIP` in sample-specific tests.

## Repository Layout

- `src/`
  - `fa_runtime.*` – runtime handle, job lifecycle, call-frame execution loop, LEB128 helpers.
  - `fa_job.*` – operand stack (linked list) and register window primitives.
  - `fa_ops.*` – opcode descriptors, stack effect helpers, per-op handlers for microcode-backed ops, delegating dispatcher.
  - `fa_jit.*` – JIT planning utilities (probes, budgets, optional function prescan, per-function microcode program preparation + execution from runtime-fed opcode streams).
  - `fa_wasm.*` – on-disk module loader with section scanners and function-body fetch (incl. tables/elements/data).
  - `fa_wasm_stream.*` – bytecode cursor helpers used by tests and future interpreter work.
  - `fa_arch.h` – architecture-size/endianness/cpu-family macros with override hooks.
  - `helpers/dynamic_list.h` – header-only dynamic array for `void*` (runtime job registry).
- `ROADMAP.md` – near-term and medium-term priorities for runtime, JIT, and microcode work.
- `test/` – CMake-driven harness (`fayasm_test_main`) with stream navigation and parser coverage; supports `--list` and substring filters to focus on a specific area.
- `samples/esp32-trap` - ESP32 trap/offload sample with SD-backed microcode and memory spill hooks.
- `samples/host-import` - host import demo using `fa_Runtime_bindHostFunctionFromLibrary` with a shared library.
- `wasm_samples/` - Emscripten fixture sources + build script for standalone `.wasm` samples consumed by runtime smoke tests.
- `studies/` – research archive covering JIT experiments, WASM decoding notes, and runtime prototypes; cross-reference entries when reusing ideas.
- `build.sh` – clean rebuild + test runner script; keep its CMake flags in sync with the docs.

## Known Gaps & Next Steps

- SIMD core + relaxed opcodes are now wired (v128 load/store, shuffle/swizzle, lane ops, integer/float arithmetic, conversions, relaxed swizzle/trunc/madd/nmadd/laneselect/min/max/q15mulr). Remaining SIMD gaps are any future extension proposals and relaxed-edge-case test coverage.
- Memory64 and multi-memory are supported; `memory.size`/`memory.grow` and loads/stores honor memory indices and 64-bit addressing, and table/data/element segment operations now execute for active/passive segments including typed element expressions (`ref.func`/`ref.null`) for funcref/externref tables.
- Element-segment const-expression parsing currently supports `ref.func` and `ref.null`; `global.get`-based reference initializers remain a follow-up.
- Trap semantics cover divide-by-zero, linear-memory bounds, and float-to-int conversion traps; global initializers accept const and `global.get` expressions, and imported globals can be overridden via `fa_Runtime_setImportedGlobal` (defaults remain zero if unset).
- Runtime tests now cover stack effects, call depth, locals, globals, branching semantics, multi-value returns, i64/f64 arithmetic, memory64/multi-memory behavior, table ops, element/data segments (legacy + typed expressions), SIMD v128.const/splat plus load/store, lane ops, arithmetic, trunc_sat conversions, conversion traps, function traps, repeated spill/load cycles (memory + JIT eviction/reload), and imported-global overrides.
- Microcode compilation now spans bit/compare/arithmetic/convert plus float unary/special/reinterpret/select ops; the runtime caches per-function decoded opcodes, can precompile sequences within the JIT budget, and dispatches prepared microcode ops, but control flow remains interpreter-driven and the engine does not yet execute fully precompiled microcode streams end-to-end.
- Host import binding now covers callbacks/`dlopen` plus imported memories/tables and `fa_RuntimeHostCall_*` ABI helpers, and memory/table rebinds now update already-attached modules.
- JIT cache eviction and spill/load hooks are in place for ESP32-class offload; opcode import/export helpers and the ESP32 sample now use a versioned opcode spill format, and the sample docs now include SD wear/perf + retention guidance. Broader runtime-level persistence standardization remains open.
- ESP32 targeting is compile-time via `FAYASM_TARGET_ESP32`; tune embedded probes with `FAYASM_TARGET_RAM_BYTES` and `FAYASM_TARGET_CPU_COUNT` as needed.

Contributions, experiments, and curious questions are welcome. The ambition is for fayasm to remain an approachable deep dive into WebAssembly execution internals while leaving room for JIT experiments or host integration research.

_Inspired by WASM3 project_

**Credit: Riccardo Cecchini 2025 (License: MIT)**
