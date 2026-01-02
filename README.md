# fayasm 🔥

Faya pseudo-WASM runtime — an experimental, lightweight WebAssembly executor designed to be dissectable. Rather than chasing peak throughput, the project focuses on showing how a WASM module can be decoded straight from disk, scheduled through a tiny runtime, and stepped instruction by instruction. The code stays intentionally compact so it can be used as classroom material, a sand-box for opcode experiments, or the seed for embedded interpreters.

## Goal & Current State

- fayasm can parse real `.wasm` binaries using a file-descriptor driven loader (`fa_wasm.*`) or in-memory buffers via `wasm_module_init_from_memory`. Sections for types, functions, exports, and memories are decoded, cached, and exposed through descriptors that the runtime consumes.
- The execution runtime (`fa_runtime.*`) owns job creation, call-frame allocation, operand stack reset, a small data-flow register window, and linear memory provisioning from the module memory section. It can stream a function body, decode immediates (LEB128 helpers, const payloads, locals/globals indices, memory operands), dispatch into the opcode table, and propagate traps (divide-by-zero, out-of-bounds memory, conversion overflow/NaN).
- Opcode metadata lives in `fa_ops.*`. The table contains size/signing metadata, stack effects, and function pointers. Integer bitcount, float unary ops, locals, and basic control flow are wired; unsupported ones surface as `FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE`, keeping the interpreter honest.
- `fa_job.*` provides the doubly-linked operand stack plus a fixed-size “register window” (recent values slide through `fa_JobDataFlow`). The runtime resets and reuses jobs to amortize allocations.
- A deterministic instruction stream helper (`fa_wasm_stream.*`) sits between the module parser and the tests, making it easy to assert cursor positions and encoded immediates.
- Tests under `test/` exercise the streaming helpers, branch traversal, and module scaffolding; the harness now also covers interpreter stack effects, call-depth limits, locals, i64/f64 arithmetic, and trap paths (division by zero, memory bounds, conversion overflow/NaN).

The interpreter deliberately stops short of executing a full program: many opcodes have placeholders, traps are surfaced as error codes, and host integration is minimal. Even so, the scaffolding for job management, frame unwinding, and constant decoding is in place and stable for further opcode work.

## Architecture Overview

- **Runtime core (`fa_runtime.*`)**: Maintains a `fa_Runtime` handle with allocator hooks, job registry (`list_t` from `helpers/dynamic_list.h`), attached module, linear memory state, and lazily sized call-frame storage. Execution is performed by `fa_Runtime_execute_job`, which streams the target function, decodes each opcode, pushes immediates into the job register window, delegates to `fa_execute_op`, and surfaces trap errors.
- **Job abstraction (`fa_job.*`)**: A job packages the operand stack (`fa_JobStack`, doubly linked) and the sliding register window (`fa_JobDataFlow`). Helper routines manage push/pop, clamp window size (`FA_JOB_DATA_FLOW_WINDOW_SIZE`), and wipe state between invocations.
- **Opcode table (`fa_ops.*`)**: A 256-entry array of `fa_WasmOp` descriptors, initialised once. Each entry encodes type info, stack deltas, immediate width, and a handler. Utility helpers (sign extension, masking, reg-window maintenance) support arithmetic op implementations.
- **Module loader (`fa_wasm.*`)**: Reads the binary straight from disk or memory, parsing headers, sections, type signatures, function metadata, exports, and memories. Function bodies can be reloaded on demand—`wasm_load_function_body` allocates a fresh buffer per call.
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
- Tests live under `test/` and cover cursor behaviour in `fa_wasm_stream`, branch navigation scenarios, plus interpreter regressions (stack arithmetic, call depth, traps). Run them with `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside `build/`.

## Repository Layout

- `src/`
  - `fa_runtime.*` – runtime handle, job lifecycle, call-frame execution loop, LEB128 helpers.
  - `fa_job.*` – operand stack (linked list) and register window primitives.
  - `fa_ops.*` – opcode descriptors, stack effect helpers, delegating dispatcher.
  - `fa_wasm.*` – on-disk module loader with section scanners and function-body fetch.
  - `fa_wasm_stream.*` – bytecode cursor helpers used by tests and future interpreter work.
  - `helpers/dynamic_list.h` – header-only dynamic array for `void*` (runtime job registry).
- `test/` – CMake-driven harness (`fayasm_test_main`) with stream navigation and parser coverage.
- `studies/` – research archive covering JIT experiments, WASM decoding notes, and runtime prototypes; cross-reference entries when reusing ideas.
- `build.sh` – clean rebuild + test runner script; keep its CMake flags in sync with the docs.

## Known Gaps & Next Steps

- Large portions of the opcode table still return `FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE` (tables, bulk memory, SIMD). Globals are stubbed and structured control flow (`if`/`br`/`br_table`) lacks real branch execution.
- Memory64 and multi-memory are not supported yet; `memory.size`/`memory.grow` and loads/stores currently target memory index 0 only.
- Trap semantics cover divide-by-zero, linear-memory bounds, and float-to-int conversion traps; structured control flow and global semantics still need real execution.
- Runtime tests now cover stack effects, call depth, locals, i64/f64 arithmetic, and conversion traps; add coverage for globals, branching semantics, and multi-value returns as new opcodes land.

Contributions, experiments, and curious questions are welcome. The ambition is for fayasm to remain an approachable deep dive into WebAssembly execution internals while leaving room for JIT experiments or host integration research.

_Inspired by WASM3 project_

**Credit: Riccardo Cecchini 2025 (License: MIT)**
