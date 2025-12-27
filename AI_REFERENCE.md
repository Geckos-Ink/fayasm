# AI Reference

This document is a fast-access knowledge base for AI agents working on fayasm. Update it alongside `README.md` whenever you modify code or documentation so the next agent inherits the latest context.

## Collaboration Rules

- After every code edit, refresh both `README.md` and `AI_REFERENCE.md` with any relevant behavioural, architectural, or tooling changes.
- Log fresh research or experiments under `studies/` and cross-reference them here to avoid repeating the same investigations.
- Prefer incremental changes: keep commits small, document breaking changes, and run the available tests before yielding control.

## Build & Test Checklist

- Configure and build with CMake (>= 3.10): `cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON`
- Invoke the provided helper: `./build.sh` (cleans `build/`, regenerates, runs tests).
- Run the harness: `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside the build directory.
- Ensure new tests land alongside new features; runtime code lacks extensive coverage, so favour regression tests around control flow and stack behaviour.

## Core Code Map

- `src/fa_runtime.*` – execution entry points, allocator hooks, call-frame management, operand stack reset.
- `src/fa_job.*` – linked-list operand stack (`fa_JobStack`) and register window (`fa_JobDataFlow`).
- `src/fa_ops.*` – opcode descriptors plus the delegate table; unimplemented handlers are clearly marked for future work.
- `src/fa_wasm.*` – file-descriptor driven parser for module sections (types, functions, exports, memories).
- `src/fa_wasm_stream.*` – cursor helpers used in the tests to exercise streaming reads.
- `src/helpers/dynamic_list.h` – pointer vector used by ancillary tools.
- `test/` – CMake target `fayasm_test_main` with coverage for wasm stream navigation and parser branches.
- `build.sh` – one-shot rebuild + test script; keep options in sync with documented build flags.

### Gaps Worth Watching

- Large portions of the opcode table still return `FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE`. Arithmetic/memory handlers need fuller stack interaction and trap semantics.
- The runtime expects to read modules from disk; embedding scenarios will require an in-memory loader or VFS shim.
- Memory/trap semantics are bare bones: grow/size instructions just push immediates, and the runtime lacks bounds-checked linear memory operations.
- Tests currently cover the instruction stream; interpreter behaviours (stack effects, call depth, trapping) need dedicated regression suites as functionality lands.

## Research Archive (studies/)

Use these references before re-running the same explorations:

- `studies/asm/aarch64_C_JIT.md` – Italian write-up on building a minimal AArch64 JIT in C with executable buffers.
- `studies/asm/cJitRegisters.md` – Register allocation strategies for an AArch64 micro-JIT, including full sample code.
- `studies/asm/ita_justInTime.md` – Introductory x86-64 JIT example in C covering executable memory management.
- `studies/asm/registersNoStoreLoad.md` – Inline assembly approach for leveraging physical registers without load/store round-trips (AArch64 focus).
- `studies/code_samples/callbacks.c` – Callback handler example showing function pointers embedded in structs.
- `studies/external_ideas/esp_log.h` – ESP-IDF logging shim for potential embedded integrations.
- `studies/external_ideas/fa_compile.c` / `fa_compile.h` – Prototype front-end for compiling instructions; review before revisiting compiler scaffolding.
- `studies/external_ideas/for_cycle.wasm` – Sample WASM module for parser experiments.
- `studies/external_ideas/operations_references.c` – Collection of operation reference implementations.
- `studies/runtime/justInTime.c` – Hybrid bytecode engine prototype combining interpretation with native chunks and trampolines.
- `studies/runtime/prompt.md` – Design summary for the hybrid engine (superoperators, trampolines, native chunks).
- `studies/runtime/test.c` – Companion C harness for the hybrid runtime concepts.
- `studies/wasm/branchExamples.md` – Bytecode snippets illustrating branching and control flow in WASM.
- `studies/wasm/functionsSections.md` – Notes on parsing WASM function/type sections.
- `studies/wasm/justInTimeParser.c` – Streaming parser experiment for WASM binaries.
- `studies/wasm/magic.md` – Explanation of WASM magic/version headers.
- `studies/wasm/notes.md` – General-purpose WASM decoding notes and observations.
- `studies/wasm/ops_reading.md` – Reference guide for interpreting opcode encodings.
- `studies/wasm/sectionsVsFunctions.md` – Comparison of section ordering versus function declarations in WASM modules.

Keep this index synchronized when new material lands in `studies/`.

## When Starting a New Task

- Skim outstanding TODOs in source files (search for `TODO` or `FIXME`).
- Validate whether relevant studies already cover the topic; if not, add a new entry both under `studies/` and above.
- Outline expected tests; if the suite lacks coverage, note the gap here so the next agent can prioritise it.

## Contact & Credits

- Project owner: Riccardo Cecchini (MIT License, 2025).
- Inspiration: WASM3 project.

If any workflow rule changes, reflect it here immediately so human collaborators and AI agents remain aligned.
