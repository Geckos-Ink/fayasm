# fayasm

Faya pseudo-WASM runtime — an experimental, lightweight WebAssembly executor focused on transparency over raw speed. The codebase is intentionally small and educational, showcasing how modules can be decoded and scheduled on top of a minimal runtime.

## Overview

- **Single-pass loader** – `fa_wasm.c` progressively parses the binary format (magic, sections, tables) without needing a heavyweight engine.
- **Job-based execution** – `fa_job.*` models the state of a running invocation. Jobs track the instruction pointer, a streaming register window, and a dynamically allocated value stack so the runtime only pays for the data it actually touches.
- **Opcode catalog** – `fa_ops_defs.c` describes all core WASM instructions, while `fa_ops.c` wires descriptors to the concrete handlers that operate on a `fa_Job`.
- **Stream helpers** – `fa_wasm_stream.*` exposes a cursor API that makes stepping through bytecode deterministic and testable.

The project is in active incubation; many operators are still stubs, but the skeleton is in place for a full interpreter.

## Runtime Data Structures

- `fa_JobDataFlow` – a tiny doubly linked list used as the scratchpad/forwarding window for intermediate values produced by recently executed instructions.
- `fa_JobStack` – a linked structure mirroring the WASM operand stack. Each `fa_JobStackValue` is heap-backed, so the stack grows and shrinks without a static allocation footprint.
- `fa_WasmOp` – metadata describing each opcode (type, arity, bytecode arguments) and an optional delegate function pointer for handlers implemented in C.

These pieces are assembled in `fa_Job`, which can be handed to op delegates to mutate the execution state safely.

## Building

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

The repository ships with a small C test harness under `test/` to exercise the wasm stream loader. You can enable it by configuring CMake with `-DFA_ENABLE_TESTS=ON` (flag to be wired up once the harness is extended).

## Current Focus

1. Flesh out the opcode delegates in `fa_ops.c` and link them to the descriptors defined in `fa_ops_defs.c`.
2. Harden the runtime with additional validation (bounds checks, trap semantics, memory abstraction).
3. Expand the parser tests so every WASM section has deterministic coverage.

Contributions, experiments, and curious questions are welcome. The aim is for fayasm to remain approachable for anyone interested in the internals of WebAssembly execution.
