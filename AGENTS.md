# AI Reference

This document is a fast-access knowledge base for AI agents working on fayasm. Update it alongside `README.md` whenever you modify code or documentation so the next agent inherits the latest context.

## Collaboration Rules

- After every code edit, refresh both `README.md` and `AGENTS.md` with any relevant behavioural, architectural, or tooling changes.
- Keep `ROADMAP.md` updated when priorities shift or new directives are added.
- Log fresh research or experiments under `studies/` and cross-reference them here to avoid repeating the same investigations.
- Prefer incremental changes: keep commits small, document breaking changes, and run the available tests before yielding control.

## Naming Convention

- Public helpers follow scoped camelCase (e.g., prefer `fa_Runtime_bindHostFunctionFromLibrary` over `fa_Runtime_bind_host_function_from_library`). Keep docs/samples aligned whenever new API helpers land so future agents inherit the pattern.

## High Priority Directive

- Replace opcode switch-case towers with a microcode compilation path: macro-built micro-op sequences (pre-stacked function pointers) compiled just in time and gated by a RAM/CPU probe (defaults to >=64MB RAM and >=2 CPUs; override via `FAYASM_MICROCODE`).
- Keep `fa_jit` tied to microcode preparation and WASM-to-microcode conversions; feed decoded opcode streams into `fa_jit_prepare_program_from_opcodes` as the runtime executes.
- Maintain trap plus spill/load hooks for offload; harden spill formats so microcode and memory can persist across boots (avoid raw function pointers).
- Keep ESP32 configuration compile-time via `FAYASM_TARGET_ESP32`/`FAYASM_TARGET_*` macros (CMake/defines), not runtime discovery.

## Build & Test Checklist

- Configure and build with CMake (>= 3.10): `cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON`
- Invoke the provided helper: `./build.sh` (optionally builds `wasm_samples/` fixtures when `emcc` is available, cleans `build/`, regenerates, runs tests; skips terminal clear when `TERM` is unset/dumb).
- Run the harness: `build/bin/fayasm_test_main` or `ctest --output-on-failure` inside the build directory.
- Test filtering: `build/bin/fayasm_test_main --list` shows areas + hints; pass a substring filter to run a subset.
- JIT prescan toggles: `build/bin/fayasm_test_main --jit-prescan` or `--jit-prescan-force` (mirrors `FAYASM_JIT_PRESCAN_FORCE`).
- Ensure new tests land alongside new features; runtime code lacks extensive coverage, so favour regression tests around control flow and stack behaviour.

## Core Code Map

- `src/fa_runtime.*` - execution entry points, allocator hooks, call-frame management, operand stack reset, locals initialization, linear memory provisioning (multi-memory/memory64), multi-value returns, loop label unwinding (params when present, otherwise results), label arity checks, imported-global overrides via `fa_Runtime_setImportedGlobal`, host import bindings for functions + imported memories/tables (callbacks/buffers or dynamic libraries) with `fa_RuntimeHostCall_*` ABI helpers, live rebind propagation for imported memories/tables after module attach, optional per-function trap hooks, spill/load hooks for JIT programs and memory, JIT cache eviction bookkeeping, plus per-function JIT opcode caches (optionally pre-scanned with `FAYASM_JIT_PRESCAN` or forced with `FAYASM_JIT_PRESCAN_FORCE`) that feed microcode preparation, precompile passes, and dispatch.
- `src/fa_job.*` - linked-list operand stack (`fa_JobStack`) and register window (`fa_JobDataFlow`).
- `src/fa_ops.*` - opcode descriptors plus the delegate table; microcode scaffolding now pre-stacks function pointer sequences for bitwise/bitcount/shift/rotate plus compare/arithmetic/convert and float unary/special/reinterpret/select ops, and per-op handlers now use those microcode functions instead of switch-case towers (gated by a RAM/CPU probe; override via `FAYASM_MICROCODE`).
- `src/fa_jit.*` - JIT planning scaffolding (resource probe, budget/advantage scoring, microcode program preparation from decoded opcodes) used for microcode preparation and WASM-to-microcode conversions, plus prepared-op execution helpers, opcode import/export helpers for stable spill formats, optional prescan configuration (`FAYASM_JIT_PRESCAN`/`FAYASM_JIT_PRESCAN_FORCE`), and `fa_jit_context_apply_env_overrides`.
- `src/fa_wasm.*` - disk or in-memory parser for module sections (types, functions, exports, globals, memories, tables, elements, data segments).
- `src/fa_wasm_stream.*` - cursor helpers used in the tests to exercise streaming reads.
- `src/helpers/dynamic_list.h` - pointer vector used by ancillary tools.
- `src/fa_arch.h` - architecture macros with override hooks (pointer width, endianness, CPU family), plus `FAYASM_TARGET_*` selection and embedded resource hints (`FAYASM_TARGET_RAM_BYTES`, `FAYASM_TARGET_CPU_COUNT`).
- `ROADMAP.md` - prioritized roadmap with near-term and medium-term planning directives.
- `test/` - CMake target `fayasm_test_main` with wasm stream coverage plus runtime regression checks (stack effects, call depth, locals/globals, branching semantics incl. loop labels, multi-value returns, memory64/multi-memory, bulk memory copy/fill, table ops, element/data segments, SIMD v128.const/splat plus v128 load/store, lane ops, arithmetic, trunc_sat conversions, conversion traps, block unwinding, global type mismatch traps, function trap allow/block, imported memory/table rebind-after-attach, JIT opcode serialization roundtrip, repeated memory spill/load cycles, JIT eviction + trap-driven reload cycles, optional wasm-sample smoke tests). The runner accepts `--list` and substring filters to locate tests and hints for relevant source files.
- `samples/esp32-trap` - ESP32 sample wiring trap hooks plus SD-backed spill/load for JIT microcode and linear memory, with a versioned opcode spill format for JIT persistence.
- `samples/host-import` - dynamic-library host import demo that binds `env.host_add` via `fa_Runtime_bindHostFunctionFromLibrary`.
- `wasm_samples/` - Emscripten fixture sources (`src/`) plus `wasm_samples/build.sh` that generates standalone `.wasm` modules for runtime smoke tests.
- `build.sh` - one-shot rebuild + test script; keep options in sync with documented build flags.

### Gaps Worth Watching

- Multi-value returns and label arity checks are enforced; reference-type block signatures and full validation remain open.
- Memory64 and multi-memory are supported; loads/stores and memory.size/grow honor memory indices and 64-bit addressing.
- Table/bulk memory execution now covers memory.init/data.drop/memory.copy/fill and table.get/set/init/copy/grow/size/fill; SIMD core opcodes plus relaxed SIMD are wired (v128 load/store, shuffle/swizzle, lane ops, integer/float arithmetic, conversions, relaxed swizzle/trunc/madd/nmadd/laneselect/min/max/q15mulr). Remaining SIMD gaps are any future extension proposals plus relaxed-edge-case test coverage.
- Interpreter tests now cover stack effects, call depth, locals/globals, branching semantics, multi-value returns, memory64/multi-memory, table ops, element/data segments, SIMD v128.const/splat plus load/store, lane ops, arithmetic, trunc_sat conversions, conversion traps, stack unwinding, imported-global overrides, and host import bindings (functions/memories/tables).
- Runtime now propagates imported memory/table rebinds on already-attached modules; keep coverage for mismatched sizes/limits as import validation evolves.
- JIT spill/load hooks now have opcode import/export helpers; tests now cover eviction + trap-driven reload and repeated offload cycles, while broader runtime-wide persistence conventions and long-run wear/perf validation are still open.

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
1. Expand element/data segment support to ref.func expressions and externref tables.
2. Add relaxed SIMD coverage tests (relaxed swizzle, laneselect, madd/nmadd, relaxed min/max, relaxed trunc, relaxed q15mulr).
3. Add SIMD edge-case tests (saturating arithmetic, lane load/store traps, NaN propagation).
4. Expand runtime smoke coverage using `wasm_samples/` modules and include fixture-build guidance in CI/docs.

### General next steps
1. Standardize runtime-wide spill/load persistence conventions around versioned opcode/memory payloads.
2. Validate offload behavior under repeated spill/load cycles on low-RAM targets.
3. Add integrity checks (e.g., CRC) and atomic replacement strategy examples to spill/load sample code.

## Contact & Credits

- Project owner: Riccardo Cecchini (MIT License, 2025).
- Inspiration: WASM3 project.

If any workflow rule changes, reflect it here immediately so human collaborators and AI agents remain aligned.
