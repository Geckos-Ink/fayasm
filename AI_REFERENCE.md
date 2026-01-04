# AI Reference

This document is a fast-access knowledge base for AI agents working on fayasm. Update it alongside `README.md` whenever you modify code or documentation so the next agent inherits the latest context.

## Collaboration Rules

- After every code edit, refresh both `README.md` and `AI_REFERENCE.md` with any relevant behavioural, architectural, or tooling changes.
- Keep `ROADMAP.md` updated when priorities shift or new directives are added.
- Log fresh research or experiments under `studies/` and cross-reference them here to avoid repeating the same investigations.
- Prefer incremental changes: keep commits small, document breaking changes, and run the available tests before yielding control.

## High Priority Directive

- Replace opcode switch-case towers with a microcode compilation path: macro-built micro-op sequences (pre-stacked function pointers) compiled just in time and gated by a RAM/CPU probe (defaults to >=64MB RAM and >=2 CPUs; override via `FAYASM_MICROCODE`).
- Keep `fa_jit` tied to microcode preparation and WASM-to-microcode conversions; feed decoded opcode streams into `fa_jit_prepare_program_from_opcodes` as the runtime executes.
- Maintain trap plus spill/load hooks for offload; harden spill formats so microcode and memory can persist across boots (avoid raw function pointers).
- Keep ESP32 configuration compile-time via `FAYASM_TARGET_ESP32`/`FAYASM_TARGET_*` macros (CMake/defines), not runtime discovery.

## Build & Test Checklist

- Configure and build with CMake (>= 3.10): `cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON`
- Invoke the provided helper: `./build.sh` (cleans `build/`, regenerates, runs tests; skips terminal clear when `TERM` is unset/dumb).
- Run the harness: `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside the build directory.
- Test filtering: `build/bin/fayasm_test_main --list` shows areas + hints; pass a substring filter to run a subset.
- JIT prescan toggles: `build/bin/fayasm_test_main --jit-prescan` or `--jit-prescan-force` (mirrors `FAYASM_JIT_PRESCAN_FORCE`).
- Ensure new tests land alongside new features; runtime code lacks extensive coverage, so favour regression tests around control flow and stack behaviour.

## Core Code Map

- `src/fa_runtime.*` - execution entry points, allocator hooks, call-frame management, operand stack reset, locals initialization, linear memory provisioning (multi-memory/memory64), multi-value returns, loop label unwinding (params when present, otherwise results), label arity checks, imported-global overrides via `fa_Runtime_set_imported_global`, optional per-function trap hooks, spill/load hooks for JIT programs and memory, JIT cache eviction bookkeeping, plus per-function JIT opcode caches (optionally pre-scanned with `FAYASM_JIT_PRESCAN` or forced with `FAYASM_JIT_PRESCAN_FORCE`) that feed microcode preparation and dispatch.
- `src/fa_job.*` - linked-list operand stack (`fa_JobStack`) and register window (`fa_JobDataFlow`).
- `src/fa_ops.*` - opcode descriptors plus the delegate table; microcode scaffolding now pre-stacks function pointer sequences for bitwise/bitcount/shift/rotate plus compare/arithmetic/convert ops, and per-op handlers now use those microcode functions instead of switch-case towers (gated by a RAM/CPU probe; override via `FAYASM_MICROCODE`).
- `src/fa_jit.*` - JIT planning scaffolding (resource probe, budget/advantage scoring, microcode program preparation from decoded opcodes) used for microcode preparation and WASM-to-microcode conversions, plus prepared-op execution helpers, optional prescan configuration (`FAYASM_JIT_PRESCAN`/`FAYASM_JIT_PRESCAN_FORCE`), and `fa_jit_context_apply_env_overrides`.
- `src/fa_wasm.*` - disk or in-memory parser for module sections (types, functions, exports, globals, memories, tables, elements, data segments).
- `src/fa_wasm_stream.*` - cursor helpers used in the tests to exercise streaming reads.
- `src/helpers/dynamic_list.h` - pointer vector used by ancillary tools.
- `src/fa_arch.h` - architecture macros with override hooks (pointer width, endianness, CPU family), plus `FAYASM_TARGET_*` selection and embedded resource hints (`FAYASM_TARGET_RAM_BYTES`, `FAYASM_TARGET_CPU_COUNT`).
- `ROADMAP.md` - prioritized roadmap with near-term and medium-term planning directives.
- `test/` - CMake target `fayasm_test_main` with wasm stream coverage plus runtime regression checks (stack effects, call depth, locals/globals, branching semantics incl. loop labels, multi-value returns, memory64/multi-memory, bulk memory copy/fill, table ops, element/data segments, SIMD v128.const/splat, conversion traps, block unwinding, global type mismatch traps, function trap allow/block). The runner accepts `--list` and substring filters to locate tests and hints for relevant source files.
- `samples/esp32-trap` - ESP32 sample wiring trap hooks plus SD-backed spill/load for JIT microcode and linear memory.
- `build.sh` - one-shot rebuild + test script; keep options in sync with documented build flags.

### Gaps Worth Watching

- Multi-value returns and label arity checks are enforced; reference-type block signatures and full validation remain open.
- Memory64 and multi-memory are supported; loads/stores and memory.size/grow honor memory indices and 64-bit addressing.
- Table/bulk memory execution now covers memory.init/data.drop/memory.copy/fill and table.get/set/init/copy/grow/size/fill; SIMD is still partial (v128.const + splats wired).
- Interpreter tests now cover stack effects, call depth, locals/globals, branching semantics, multi-value returns, memory64/multi-memory, table ops, element/data segments, SIMD v128.const/splat, conversion traps, stack unwinding, and imported-global overrides.
- JIT spill/load hooks currently persist pointer-based microcode; a stable, versioned format is needed for cross-boot reuse and broader testing.

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
1. Extend microcode coverage to float unary/special ops plus reinterpret/select, and fold in a resource-aware JIT precompile pass for per-function sequences.
2. Implement remaining SIMD opcodes (loads/stores, shuffles, lane ops, comparisons, arithmetic).
3. Expand element/data segment support to ref.func expressions and externref tables.
4. Add lane-focused SIMD tests plus coverage for additional table bounds scenarios.

### General next steps
1. Harden JIT spill/load formats to remove raw function pointers and add versioning for persistence across boots.
2. Add tests for function traps and spill/load hooks (memory reloads, JIT cache eviction paths).
3. Expand the ESP32 trap/offload sample to use the hardened spill format and document SD wear/perf tradeoffs.

## Contact & Credits

- Project owner: Riccardo Cecchini (MIT License, 2025).
- Inspiration: WASM3 project.

If any workflow rule changes, reflect it here immediately so human collaborators and AI agents remain aligned.
