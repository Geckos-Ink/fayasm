# fayasm 🔥

Faya pseudo-WASM runtime — an experimental, lightweight WebAssembly executor designed to be dissectable. Rather than chasing peak throughput, the project focuses on showing how a WASM module can be decoded straight from disk, scheduled through a tiny runtime, and stepped instruction by instruction. The code stays intentionally compact so it can be used as classroom material, a sand-box for opcode experiments, or the seed for embedded interpreters.

## Goal & Current State

- fayasm can parse real `.wasm` binaries using a file-descriptor driven loader (`fa_wasm.*`). Sections for types, functions, exports, and memories are decoded, cached, and exposed through descriptors that the runtime consumes.
- The execution runtime (`fa_runtime.*`) already owns job creation, call-frame allocation, operand stack reset, and a small data-flow register window. It can stream a function body, decode immediates (LEB128 helpers, const payloads, memory operands), and dispatch into the opcode table.
- Opcode metadata lives in `fa_ops.*`. The table contains size/signing metadata, stack effects, and function pointers. Arithmetic and control opcodes are progressively being implemented; unsupported ones surface as `FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE`, keeping the interpreter honest.
- `fa_job.*` provides the doubly-linked operand stack plus a fixed-size “register window” (recent values slide through `fa_JobDataFlow`). The runtime resets and reuses jobs to amortize allocations.
- A deterministic instruction stream helper (`fa_wasm_stream.*`) sits between the module parser and the tests, making it easy to assert cursor positions and encoded immediates.
- Tests under `test/` exercise the streaming helpers, branch traversal, and module scaffolding. They currently rely on helper shims such as `wasm_exec_stream.h` to model a control-flow loop around the instruction reader.

The interpreter deliberately stops short of executing a full program: many opcodes have placeholders, traps are surfaced as error codes, and host integration is minimal. Even so, the scaffolding for job management, frame unwinding, and constant decoding is in place and stable for further opcode work.

## Architecture Overview

- **Runtime core (`fa_runtime.*`)**: Maintains a `fa_Runtime` handle with allocator hooks, job registry (`list_t` from `helpers/dynamic_list.h`), attached module, and lazily sized call-frame storage. Execution is performed by `fa_Runtime_execute_job`, which streams the target function, decodes each opcode, pushes immediates into the job register window, and delegates to `fa_execute_op`.
- **Job abstraction (`fa_job.*`)**: A job packages the operand stack (`fa_JobStack`, doubly linked) and the sliding register window (`fa_JobDataFlow`). Helper routines manage push/pop, clamp window size (`FA_JOB_DATA_FLOW_WINDOW_SIZE`), and wipe state between invocations.
- **Opcode table (`fa_ops.*`)**: A 256-entry array of `fa_WasmOp` descriptors, initialised once. Each entry encodes type info, stack deltas, immediate width, and a handler. Utility helpers (sign extension, masking, reg-window maintenance) support arithmetic op implementations.
- **Module loader (`fa_wasm.*`)**: Reads the binary straight from disk, parsing headers, sections, type signatures, function metadata, exports, and memories. Function bodies can be reloaded on demand—`wasm_load_function_body` allocates a fresh buffer per call.
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
- Tests live under `test/` and currently focus on cursor behaviour in `fa_wasm_stream` and branch navigation scenarios. Run them with `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside `build/`.

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

- Large portions of the opcode table still return `FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE`. Arithmetic/memory handlers need fuller stack interaction and trap semantics.
- The runtime expects to read modules from disk; embedding scenarios will require an in-memory loader or VFS shim.
- Memory/trap semantics are bare bones: grow/size instructions just push immediates, and the runtime lacks bounds-checked linear memory operations.
- Tests currently cover the instruction stream; interpreter behaviours (stack effects, call depth, trapping) need dedicated regression suites as functionality lands.

Contributions, experiments, and curious questions are welcome. The ambition is for fayasm to remain an approachable deep dive into WebAssembly execution internals while leaving room for JIT experiments or host integration research.

_Inspired by WASM3 project_

**Credit: Riccardo Cecchini 2025 (License: MIT)**
