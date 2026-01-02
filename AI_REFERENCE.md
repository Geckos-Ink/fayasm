# AI Reference

This document is a fast-access knowledge base for AI agents working on fayasm. Update it alongside `README.md` whenever you modify code or documentation so the next agent inherits the latest context.

## Collaboration Rules

- After every code edit, refresh both `README.md` and `AI_REFERENCE.md` with any relevant behavioural, architectural, or tooling changes.
- Log fresh research or experiments under `studies/` and cross-reference them here to avoid repeating the same investigations.
- Prefer incremental changes: keep commits small, document breaking changes, and run the available tests before yielding control.

## Build & Test Checklist

- Configure and build with CMake (>= 3.10): `cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON`
- Invoke the provided helper: `./build.sh` (cleans `build/`, regenerates, runs tests; skips terminal clear when `TERM` is unset/dumb).
- Run the harness: `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside the build directory.
- Ensure new tests land alongside new features; runtime code lacks extensive coverage, so favour regression tests around control flow and stack behaviour.

## Core Code Map

- `src/fa_runtime.*` – execution entry points, allocator hooks, call-frame management, operand stack reset, locals initialization, linear memory provisioning (multi-memory/memory64), multi-value returns, label arity checks, and imported-global overrides via `fa_Runtime_set_imported_global`.
- `src/fa_job.*` – linked-list operand stack (`fa_JobStack`) and register window (`fa_JobDataFlow`).
- `src/fa_ops.*` – opcode descriptors plus the delegate table; numeric bitcount and float unary handlers are now wired alongside arithmetic.
- `src/fa_wasm.*` – disk or in-memory parser for module sections (types, functions, exports, globals, memories, tables, elements, data segments).
- `src/fa_wasm_stream.*` – cursor helpers used in the tests to exercise streaming reads.
- `src/helpers/dynamic_list.h` – pointer vector used by ancillary tools.
- `src/fa_arch.h` – architecture macros with override hooks (pointer width, endianness, CPU family).
- `test/` – CMake target `fayasm_test_main` with wasm stream coverage plus runtime regression checks (stack effects, call depth, locals/globals, branching semantics incl. loop labels, multi-value returns, memory64/multi-memory, bulk memory copy/fill, table ops, element/data segments, SIMD v128.const/splat, conversion traps, block unwinding, global type mismatch traps).
- `build.sh` – one-shot rebuild + test script; keep options in sync with documented build flags.

### Gaps Worth Watching

- Multi-value returns and label arity checks are enforced; reference-type block signatures and full validation remain open.
- Memory64 and multi-memory are supported; loads/stores and memory.size/grow honor memory indices and 64-bit addressing.
- Table/bulk memory execution now covers memory.init/data.drop/memory.copy/fill and table.get/set/init/copy/grow/size/fill; SIMD is still partial (v128.const + splats wired).
- Interpreter tests now cover stack effects, call depth, locals/globals, branching semantics, multi-value returns, memory64/multi-memory, table ops, element/data segments, SIMD v128.const/splat, conversion traps, stack unwinding, and imported-global overrides.

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

## Next steps
1. Implement remaining SIMD opcodes (loads/stores, shuffles, lane ops, comparisons, arithmetic).
2. Expand element/data segment support to ref.func expressions and externref tables.
3. Add lane-focused SIMD tests plus coverage for additional table bounds scenarios.

### General next steps
1. Improve traps to allow real time write and move of volatile data on another storage system
2. Implement macros for handling compilation on different architecture (x86, x86_64, ESP32, ..)

## Contact & Credits

- Project owner: Riccardo Cecchini (MIT License, 2025).
- Inspiration: WASM3 project.

If any workflow rule changes, reflect it here immediately so human collaborators and AI agents remain aligned.
