# fayasm 🔥

Faya pseudo-WASM runtime — an experimental, lightweight WebAssembly executor focused on transparency and portability over raw speed. The codebase is intentionally small and educational, showing how modules can be decoded and scheduled on top of a minimal runtime so it stays approachable on constrained systems.

## Current Status

- `fa_wasm.*` streams WebAssembly binaries from disk, validates headers, decodes sections (types, functions, exports, memory), and keeps the descriptors alive for the runtime.
- `fa_runtime.*` owns execution jobs, call frames, and memory management hooks; it already implements the operand stack, a tiny register-forwarding window, and helpers for LEB128 decoding.
- `fa_ops.*` enumerates the opcode catalogue and wires delegates for the instructions that are currently implemented. The tables double as a reference when expanding the interpreter.
- `fa_wasm_stream.*` offers a deterministic cursor API that the tests exercise to guarantee byte-accurate traversal.
- `test/` contains unit tests for the stream helpers and early parser branches; the harness builds alongside the library through CMake or the provided `build.sh`.

Plenty of operators still map to TODO handlers and host integration is minimal, but the runtime skeleton is stable enough to iterate on opcode support, validation, and scheduling behaviour.

## Repository Layout

- `src/` – core runtime sources
  - `fa_runtime.*` – job lifecycle, stack management, call frames
  - `fa_job.*` – operand stack and dataflow window primitives
  - `fa_ops.*` – opcode metadata and delegate lookup
  - `fa_wasm.*` – module loader that reads directly from file descriptors
  - `fa_wasm_stream.*` – byte stream utilities and tests’ flight recorder
  - `helpers/dynamic_list.h` – header-only pointer vector used by tooling
- `test/` – CMake-driven unit tests (`fayasm_test_main`) for stream and parser helpers
- `studies/` – research notes, prototypes, and reference material grouped by topic (`asm`, `runtime`, `wasm`, …)
- `build.sh` – convenience wrapper that regenerates `build/`, rebuilds the library (shared + static), and runs the test harness

## Building

The project relies on CMake (≥ 3.10) and a C99 compiler. Toggle the shared/static artefacts or tests with the `FAYASM_BUILD_*` options.

```bash
mkdir -p build
cd build
cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON
cmake --build .
```

Alternatively, `./build.sh` performs a clean rebuild and executes the test binary at `build/bin/fayasm_test_main`.

## Running Tests

Inside the build directory, use either the generated binary or CTest:

```bash
cd build
bin/fayasm_test_main
# or
ctest --output-on-failure
```

## Roadmap

1. Flesh out the opcode delegates with proper trapping semantics, bounds checks, and host hooks.
2. Expand the automated tests to cover arithmetic, memory, and control instructions end-to-end.
3. Introduce a module configuration API so the runtime can be embedded without touching the filesystem.

Contributions, experiments, and curious questions are welcome. The aim is for fayasm to remain approachable for anyone interested in the internals of WebAssembly execution.

_Inspired by WASM3 project_

**Credit: Riccardo Cecchini 2025 (License: MIT)**
