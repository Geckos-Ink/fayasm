# fayasm 🔥

Faya pseudo-WASM runtime — an experimental, lightweight WebAssembly executor designed to be dissectable. Rather than chasing peak throughput, the project focuses on showing how a WASM module can be decoded straight from disk, scheduled through a tiny runtime, and stepped instruction by instruction. The code stays intentionally compact so it can be used as classroom material, a sand-box for opcode experiments, or the seed for embedded interpreters.

## Goal & Current State

- fayasm can parse real `.wasm` binaries using a file-descriptor driven loader (`fa_wasm.*`) or in-memory buffers via `wasm_module_init_from_memory`. Sections for types, functions, exports, memories, tables, elements, and data segments are decoded, cached, and exposed through descriptors that the runtime consumes.
- The execution runtime (`fa_runtime.*`) owns job creation, call-frame allocation, operand stack reset, a small data-flow register window, and linear memory provisioning from the module memory section. It can stream a function body, decode immediates (LEB128 helpers, const payloads, locals/globals indices, memory operands with memory indices), run a control stack for `block`/`loop`/`if`/`br`/`br_table` with operand stack unwinding (loop labels use params when present, otherwise results), block result propagation, multi-value returns, and label arity checks, and propagate traps (divide-by-zero, out-of-bounds memory, conversion overflow/NaN).
- Opcode metadata lives in `fa_ops.*`. The table contains size/signing metadata, stack effects, and function pointers. Integer bitcount, float unary ops, locals/globals, and basic control flow are wired; per-op handlers for bitwise/bitcount/shift/rotate plus compare/arithmetic/convert now use microcode functions, gated by a RAM/CPU probe (defaults to >=64MB RAM and >=2 CPUs; override via `FAYASM_MICROCODE`).
- `fa_jit.*` introduces scaffolding for microcode JIT planning: system probes, budget/advantage scoring, and preparation of microcode programs from decoded opcodes.
- `fa_job.*` provides the doubly-linked operand stack plus a fixed-size “register window” (recent values slide through `fa_JobDataFlow`). The runtime resets and reuses jobs to amortize allocations.
- A deterministic instruction stream helper (`fa_wasm_stream.*`) sits between the module parser and the tests, making it easy to assert cursor positions and encoded immediates.
- Tests under `test/` exercise the streaming helpers, branch traversal, and module scaffolding; the harness now also covers interpreter stack effects, call-depth limits, locals, globals, branching semantics (including loop labels), multi-value returns, i64/f64 arithmetic, memory64/multi-memory usage, bulk memory copy/fill, table ops, element/data segments, SIMD v128.const/splat, and trap paths (division by zero, memory bounds, conversion overflow/NaN, global type mismatches).

The interpreter deliberately stops short of executing a full program: many opcodes have placeholders, traps are surfaced as error codes, and host integration is minimal. Even so, the scaffolding for job management, frame unwinding, and constant decoding is in place and stable for further opcode work.

## Architecture Overview

- **Runtime core (`fa_runtime.*`)**: Maintains a `fa_Runtime` handle with allocator hooks, job registry (`list_t` from `helpers/dynamic_list.h`), attached module, linear memory state, and lazily sized call-frame storage. Execution is performed by `fa_Runtime_execute_job`, which streams the target function, decodes each opcode, pushes immediates into the job register window, delegates to `fa_execute_op`, and surfaces trap errors; loop labels unwind using params when present, otherwise results.
- **Job abstraction (`fa_job.*`)**: A job packages the operand stack (`fa_JobStack`, doubly linked) and the sliding register window (`fa_JobDataFlow`). Helper routines manage push/pop, clamp window size (`FA_JOB_DATA_FLOW_WINDOW_SIZE`), and wipe state between invocations.
- **Opcode table (`fa_ops.*`)**: A 256-entry array of `fa_WasmOp` descriptors, initialised once. Each entry encodes type info, stack deltas, immediate width, and a handler. Microcode scaffolding now pre-stacks function pointer sequences for bitwise/bitcount/shift/rotate plus compare/arithmetic/convert ops, and per-op handlers use those microcode functions (gated by a RAM/CPU probe with `FAYASM_MICROCODE` override), while utility helpers (sign extension, masking, reg-window maintenance) support arithmetic op implementations.
- **JIT planning (`fa_jit.*`)**: Lightweight scaffolding that probes resources, computes budgets/advantage scores, and prepares microcode programs from decoded opcode streams.
- **Module loader (`fa_wasm.*`)**: Reads the binary straight from disk or memory, parsing headers, sections, type signatures, function metadata, exports, memories, tables, elements, and data segments. Function bodies can be reloaded on demand—`wasm_load_function_body` allocates a fresh buffer per call.
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
- `./build.sh` automates the rebuild: it nukes `build/`, configures with the same flags, builds shared + static libraries, compiles `fayasm_test_main`, and executes it.
- `build.sh` skips terminal clear when `TERM` is unset/dumb, so it can run in non-interactive shells.
- Tests live under `test/` and cover cursor behaviour in `fa_wasm_stream`, branch navigation scenarios, plus interpreter regressions (stack arithmetic, call depth, traps, table ops, element/data segments, SIMD v128.const/splat). Run them with `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside `build/`; pass `--list` or a substring filter to focus on a specific area and see source hints.

## Repository Layout

- `src/`
  - `fa_runtime.*` – runtime handle, job lifecycle, call-frame execution loop, LEB128 helpers.
  - `fa_job.*` – operand stack (linked list) and register window primitives.
  - `fa_ops.*` – opcode descriptors, stack effect helpers, per-op handlers for microcode-backed ops, delegating dispatcher.
  - `fa_jit.*` – JIT planning utilities (probes, budgets, microcode program preparation).
  - `fa_wasm.*` – on-disk module loader with section scanners and function-body fetch (incl. tables/elements/data).
  - `fa_wasm_stream.*` – bytecode cursor helpers used by tests and future interpreter work.
  - `fa_arch.h` – architecture-size/endianness/cpu-family macros with override hooks.
  - `helpers/dynamic_list.h` – header-only dynamic array for `void*` (runtime job registry).
- `test/` – CMake-driven harness (`fayasm_test_main`) with stream navigation and parser coverage; supports `--list` and substring filters to focus on a specific area.
- `studies/` – research archive covering JIT experiments, WASM decoding notes, and runtime prototypes; cross-reference entries when reusing ideas.
- `build.sh` – clean rebuild + test runner script; keep its CMake flags in sync with the docs.

## Known Gaps & Next Steps

- Large portions of the opcode table still return `FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE` (SIMD). Table/bulk memory ops are now wired (memory.init/data.drop/memory.copy/fill, table.get/set/init/copy/grow/size/fill), but most SIMD lanes/arithmetic remain stubbed.
- Memory64 and multi-memory are supported; `memory.size`/`memory.grow` and loads/stores honor memory indices and 64-bit addressing, and table/data/element segment operations now execute for active/passive segments (ref.func element expressions remain missing).
- Trap semantics cover divide-by-zero, linear-memory bounds, and float-to-int conversion traps; global initializers accept const and `global.get` expressions, and imported globals can be overridden via `fa_Runtime_set_imported_global` (defaults remain zero if unset).
- Runtime tests now cover stack effects, call depth, locals, globals, branching semantics, multi-value returns, i64/f64 arithmetic, memory64/multi-memory behavior, table ops, element/data segments, SIMD v128.const/splat, and conversion traps; they now specify function result types and exercise imported-global overrides.
- Microcode compilation is now scaffolded for select bit/compare/arithmetic/convert ops with per-op handlers, but many control/memory/table ops still use switch-case dispatch and the engine does not yet precompile full functions.

Contributions, experiments, and curious questions are welcome. The ambition is for fayasm to remain an approachable deep dive into WebAssembly execution internals while leaving room for JIT experiments or host integration research.

_Inspired by WASM3 project_

**Credit: Riccardo Cecchini 2025 (License: MIT)**
