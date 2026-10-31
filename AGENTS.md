# fayasm — AI Agent Reference

This is the fast-access operational reference for agents working on fayasm, an experimental C99 WebAssembly (WASM) parser and runtime built for transparent execution, runtime prototyping, and embedded experiments. The checked-out runtime implements a broad but incomplete WASM slice; it is not a production-hardened, fully validating, spec-complete engine, and its “JIT” layer currently prepares portable microcode rather than emitting native machine code.

## Read This First

Use this source-of-truth order:

1. [`LICENSE`](LICENSE) — legal terms; no code or documentation may contradict it.
2. Executable contracts and tests: public headers under [`src/`](src/), [`test/main.c`](test/main.c), and the CMake files — authoritative for ABI, behavior actually asserted, and build registration.
3. Current implementations under [`src/`](src/) and build/sample scripts — authoritative for checked-out behavior when not contradicted by a higher-order contract.
4. [`README.md`](README.md) and sample READMEs — user-facing commands, supported workflows, and examples.
5. [`ROADMAP.md`](ROADMAP.md) — planned priorities and recently completed work; a roadmap entry does not prove implementation.
6. [`studies/`](studies/) — research, prototypes, and benchmark evidence; these files inform work but are not production code.
7. Git history — historical context only.

When sources disagree, inspect the relevant code and focused test. Fix stale documentation in the same task when it is in scope; otherwise record the mismatch as a known gap. A test proves only the behavior it asserts, comments do not override executable behavior, and planned work must never be described as shipped.

## Collaboration and Maintenance Rules

- Read this file completely before editing and search for any nested `AGENTS.md` between the repository root and the target file. None are currently tracked below the root.
- Start with `git status --short --branch`; preserve unrelated user changes and ignored build outputs. Never clean a dirty tree merely to simplify the task.
- After every code edit, review both [`README.md`](README.md) and this handbook. Update every behavioral, architectural, API, ownership, command, or tooling fact made stale by the change; do not add cosmetic churn when no durable fact changed.
- Keep README `Quickstart`, command examples, performance claims, and `Project Direction` aligned with executable workflows and [`ROADMAP.md`](ROADMAP.md).
- Update [`ROADMAP.md`](ROADMAP.md) when priorities or status change. Keep only current operational priorities here and link the roadmap for chronology.
- Keep the central registry/dispatch comments in [`src/fa_ops.c`](src/fa_ops.c) and the macro-family comments in the focused `src/fa_ops_*.c` files synchronized with behavior; update them instead of deleting them during refactors.
- Add regression tests with runtime, parser, opcode, persistence, or ABI changes. Run the narrowest relevant filter, then the full harness when practical.
- Record new research or experiments under [`studies/`](studies/) and add them to the Research Archive subsection below. Consult existing studies before repeating an investigation.
- Treat `build/`, `build-esp32/`, `build-esp32-compile/`, `build-esp32-cmake/`, and `wasm_samples/build/` as generated output. Most are ignored; never commit the selected build directory even when a custom/default name is not yet covered by `.gitignore`. Change generators or sources, never generated artifacts.
- Keep changes incremental, document breaking changes, and review the complete diff before committing. A commit must describe the post-commit repository, not an aspirational state.
- Public helpers use scoped camelCase, for example `fa_Runtime_bindHostFunctionFromLibrary`; keep headers, samples, and docs aligned when APIs change.
- Do not maintain manual test-count claims in this file. Use `build/bin/fayasm_test_main --list` for the current registry.

## Essential Project Principles

### Transparency before opaque optimization

- Keep parsing, stack effects, control flow, traps, host bindings, and microcode preparation traceable in C99.
- Performance work must preserve inspectable execution paths and focused regression coverage; do not replace behavior with an untested black box.

### Microcode is the current compilation direction

- Replace remaining opcode switch towers incrementally with table-driven handlers and macro-built, pre-stacked micro-op sequences.
- Keep `fa_jit` responsible for resource decisions, opcode-to-microcode preparation, and prepared-program persistence. Feed decoded opcode streams to `fa_jit_prepare_program_from_opcodes`.
- Prepared records bind scalar local/global indices and constants after successful decode. Bindings are budgeted cache data: spill only opcodes and rebind after load. `jit_bytecode_executions` counts reuse per execution. Numeric register macros specialize exact typed operands and retain generic fallbacks; reacquire stack pointers after a pop because circular refill can move values.
- Extend microcode coverage only with measured benefit on the real-toolchain fixtures, especially `stats_suite`; use the controlled bytecode/backend comparisons in `studies/runtime/stats_benchmarks.md` and keep embedded claims separate from native timings.

### Portable low-resource operation

- Keep ESP32/embedded selection compile-time through `FAYASM_TARGET_ESP32` and related `FAYASM_TARGET_*` defines; do not add runtime platform discovery.
- Embedded builds must not depend on `dlopen`/`dlsym`. Dynamic-library binding returns `FA_RUNTIME_ERR_UNSUPPORTED` when loader support is unavailable.
- Persist reconstructible data, never process-local function pointers. Spill formats must remain portable across boots and architectures.

### Tested C99 boundaries

- Keep the library C99-compatible and maintain the local portability shims used by strict/newlib toolchains.
- Preserve allocator ownership, stack typing, bounds checks, and trap propagation across parser/runtime/opcode boundaries.

## Critical Implementation Contracts

- **Funcref representation:** `0` means null and function index `n` is stored as `n + 1`. Use `fa_funcref_encode_u32` and `fa_funcref_decode_u32` from [`src/fa_types.h`](src/fa_types.h); never store a raw function index in a funcref carrier. `test_call_indirect_function_zero`, `test_call_indirect_null_trap`, and `test_ref_ops_basic` in [`test/main.c`](test/main.c) guard this.
- **Spill envelope:** every persisted JIT or memory blob starts with the fixed 16-byte little-endian `FA_SPILL_*` envelope from [`src/fa_jit.h`](src/fa_jit.h). JIT blobs contain opcode streams and rebuild microcode on load; raw `Operation` pointers must never be serialized. Memory blobs add the self-describing memory sub-header from [`src/fa_runtime.h`](src/fa_runtime.h). The `test_spill_envelope_*` regressions reject wrong magic, version, kind, size, and truncation.
- **Call-frame stack base:** after callee arguments are removed, the function control frame records the remaining caller stack height in `runtime_push_frame`; result unwinding must preserve operands below that height. `test_wasm_sample_nested_calls` protects this fixed regression.
- **Branch arity:** `runtime_branch_to_label` unwinds loop branches with loop parameter types when present and other branches with result types. `runtime_unwind_stack_to` must validate types before restoring values. The block/loop/branch tests in [`test/main.c`](test/main.c) enforce this.
- **Decode-path parity:** instruction immediates are interpreted in `runtime_decode_instruction`, skipped during block scanning in `runtime_skip_immediates`, and skipped during JIT prescan in `runtime_prescan_skip_immediates`. Any opcode or prefixed subopcode change must update all applicable paths. Scalar `0xFC` subopcodes `0..7` are immediate-free. Assemble signed LEB payload bits with unsigned shifts and then preserve the bit pattern when converting to the signed result; signed negative-mask shifts are undefined C.
- **Bounded job windows:** `fa_JobStack` defaults to keeping its newest `FA_JOB_REGISTER_WINDOW_SIZE` typed operands inline, spilling older values. With `FA_JOB_FIXED_REGISTERS=1`, the first N operands stay at fixed indices and newer values overflow instead. Both use retained contiguous storage and the same public helpers. `FA_JOB_STACK_MAX_DEPTH` is a hard safety cap. Decoded immediates use the separate fixed `FA_JOB_DATA_FLOW_WINDOW_SIZE` window; overflow must fail, never evict an older entry. Keep the immediate window at least five entries for multi-memory SIMD lane instructions, preserve LIFO decode/handler parity, and run the `job-window` plus `test_simd_multi_memory_load32_lane` regressions.
- **Budget-capped JIT preparation:** `runtime_jit_maybe_prepare` compares `prepared_source_count` with the capped `max_ops_per_chunk` target, not the uncapped recorded opcode count. Do not regress large bodies into per-instruction re-preparation; run `test_jit_large_body_prepare_once`.
- **Imported resource ownership:** imported memory/table buffers remain host-owned. Runtime reset, growth, deserialization, and rebind paths must honor `is_host`/`owns_data` and must not free or overwrite buffers the runtime does not own. Run the imported binding/rebind tests and memory spill tests.
- **Host-call type checking:** host callbacks consume arguments and set results only through the signature-aware `fa_RuntimeHostCall_*` helpers or equivalent checks. A callback returning mismatched result kinds must trap.
- **SIMD dispatch coverage:** `g_simd_dispatch` is built from `kSimdRanges`. A new SIMD subopcode belongs in its owning family handler and requires a matching range update or new range plus a direct regression.
- **Single dynamic-list implementation:** [`src/helpers/dynamic_list.h`](src/helpers/dynamic_list.h) is header-only and emits definitions when `LIST_IMPLEMENTATION` is set. Keep that define in exactly one linked translation unit, currently [`src/fa_runtime.c`](src/fa_runtime.c).

## Architecture and Data/Control Flow

Primary execution:

`host or fayasm_run` → `wasm_module_init*` and `wasm_load_*` → `fa_Runtime_attachModule` → `fa_Runtime_createJob` → `fa_Runtime_executeJob*` → `runtime_decode_instruction` → fixed immediate window → control handling or `fa_execute_op` → circular or fixed operand slots / bounded contiguous spill / memory / table → typed results on `fa_JobStack`

Microcode side path:

`decoded opcode` → per-function runtime cache → `runtime_jit_maybe_prepare` → `fa_jit_prepare_program_from_opcodes` → typed macro-built `Operation` steps + bound scalar immediates → `fa_jit_execute_prepared_op` (numeric/bound scalar paths bypass control-context setup)

Offload side path:

`trap or cache pressure` → runtime spill/load hook → `fa_jit_program_serialize` or `fa_Runtime_serializeMemory` → host-owned storage → matching deserialize function → validated, reconstructed runtime state

Fixture flow:

`wasm_samples/src/*.{c,rs}` → [`wasm_samples/build.sh`](wasm_samples/build.sh) → ignored `wasm_samples/build/*.wasm` → optional `test_wasm_sample_*` cases or `build/bin/fayasm_run`

The runtime owns jobs, internal memories/tables, globals, control frames, and JIT caches. `WasmModule` owns parsed module metadata and backing input. Host-provided imported buffers and external spill storage stay outside runtime ownership.

## Linked Source Tree and File Reference

### [`AGENTS.md`](AGENTS.md)

This operational map owns agent-facing invariants, ownership, pitfalls, and validation routes. It is not a changelog or a replacement for user documentation.

- **Update when:** durable behavior, symbols, ownership, interfaces, workflows, tests, generated paths, compatibility boundaries, or priorities change.
- **Common mistake:** copying roadmap history or manual test totals into current-state sections.

### [`LICENSE`](LICENSE)

Defines the repository’s MIT terms and required copyright/license notice. It outranks implementation and documentation preferences.

### [`.gitignore`](.gitignore)

Excludes native build products, compiler/linker artifacts, generated WASM fixtures, and the named ESP32 compile-check directories.

- **Common mistake:** assuming every `--build-dir` value or the wrapper’s `build-esp32/` default is ignored; verify with `git check-ignore` and never commit generated trees.

### [`README.md`](README.md)

Owns the public project explanation, supported quickstart, CLI examples, performance snapshot, and user-facing direction.

- **Depends on:** [`build.sh`](build.sh), [`CMakeLists.txt`](CMakeLists.txt), [`ROADMAP.md`](ROADMAP.md), and real fixture exports.
- **Common mistake:** documenting a command or status from memory without checking its script or registry.

### [`ROADMAP.md`](ROADMAP.md)

Owns active priorities and completed-work chronology. It does not establish shipped behavior by itself.

- **Related evidence:** [`studies/runtime/stats_benchmarks.md`](studies/runtime/stats_benchmarks.md).
- **Common mistake:** leaving completed work in a planned status or duplicating its long history into this handbook.

### [`CMakeLists.txt`](CMakeLists.txt)

Defines project version `0.1.0`, C99 mode, shared/static/test/tool options, platform defines, the `fayasm_run` target, test inclusion, and CPack metadata.

- **Key subparts:** `FAYASM_BUILD_SHARED`, `FAYASM_BUILD_STATIC`, `FAYASM_BUILD_TESTS`, `FAYASM_BUILD_TOOLS`, `FAYASM_FIXED_REGISTERS`, and `FAYASM_TARGET_ESP32`.
- **Depends on:** [`src/CMakeLists.txt`](src/CMakeLists.txt) and [`test/CMakeLists.txt`](test/CMakeLists.txt).
- **Common mistake:** assuming Windows is supported; configuration explicitly warns that it may not work.

### [`build.sh`](build.sh)

Owns the target-aware native/ESP32 build orchestration, optional fixture build, generated-directory cleanup, CMake forwarding, and native test execution.

- **Key subparts:** `usage`, target preset resolution, ESP-IDF/toolchain resolution, `CMAKE_ARGS`, fixture invocation, configure/build, and optional harness run.
- **Generated outputs:** `build/` or `build-esp32/`; the default invocation removes and recreates its selected build directory. The native path is ignored, while the default ESP32 path currently requires manual care not to commit.
- **Common mistake:** enabling default ESP32 fixtures unintentionally; pass `--no-fixtures` for a static-only compile check.

### [`src/CMakeLists.txt`](src/CMakeLists.txt)

Builds `fa_*.c` into shared and/or static `fayasm` libraries, exports public headers, links threads/math and desktop loader dependencies, and installs the library.

- **Common mistake:** its second `file(GLOB FAYASM_HEADERS_EXTRA ...)` replaces the first glob rather than appending; verify installed-header behavior before relying on it.

### [`src/fa_types.h`](src/fa_types.h)

Owns scalar aliases, allocator callback types, reference carriers, and canonical funcref encoding.

- **Key symbols:** `fa_ptr`, `fa_funcref_encode_u32`, `fa_funcref_decode_u32`, `fa_Malloc`, `fa_Free`.
- **Used by:** every runtime subsystem.
- **Tests:** funcref and indirect-call cases in [`test/main.c`](test/main.c).

### [`src/fa_arch.h`](src/fa_arch.h)

Owns overridable pointer width, endianness, CPU-family detection, target selection, and embedded resource hints.

- **Key symbols:** `FA_ARCH_*`, `FAYASM_TARGET_*`, `FAYASM_TARGET_RAM_BYTES`, `FAYASM_TARGET_CPU_COUNT`.
- **Used by:** JIT/resource probes and compile-time platform paths.
- **Common mistake:** converting target selection into runtime discovery or assuming the default little-endian fallback proves an unknown target’s byte order.

### [`src/fa_job.h`](src/fa_job.h)

Defines typed operand values, compile-time circular/fixed operand backends, bounded contiguous overflow storage, the separate decoded-immediate window, and `fa_Job`.

- **Key types:** `fa_JobValue`, `fa_V128`, `fa_JobStack`, `fa_JobDataFlow`, `fa_Job`.
- **Contract:** value kind, signedness, and bit width travel with each payload. Shallow/steady operand traffic must not allocate; live depth beyond the inline window may grow the retained spill array geometrically up to `FA_JOB_STACK_MAX_DEPTH`.

### [`src/fa_job.c`](src/fa_job.c)

Implements circular forwarding or fixed-depth slots, contiguous overflow growth/reuse, the allocation-free decoded-immediate window, and legacy standalone `fa_JobDataFlow` list helpers.

- **Key functions:** `fa_JobStack_reset`, `fa_JobStack_push`, `fa_JobStack_pop`, `fa_JobStack_peek`, `fa_JobRegWindow_reset`, `fa_JobRegWindow_push`, `fa_JobRegWindow_pop`, `fa_Job_init`.
- **Called by:** [`src/fa_runtime.c`](src/fa_runtime.c) and [`src/fa_ops.c`](src/fa_ops.c).
- **Tests:** `job-window`, arithmetic, call, control-flow, stack-effect, and multi-memory SIMD lane cases in [`test/main.c`](test/main.c).
- **Common mistake:** bypassing these helpers, assuming `fa_JobStack_peek` remains stable across a mutation, clearing retained spill capacity on every execution, or silently dropping an immediate when the fixed window is full.

### [`src/fa_wasm.h`](src/fa_wasm.h)

Defines parsed module metadata for sections, signatures, functions, globals, memories, tables, element/data segments, exports, and disk/in-memory backing.

- **Key types:** `WasmModule`, `WasmFunctionType`, `WasmFunction`, `WasmMemory`, `WasmTable`, `WasmElementSegment`, `WasmDataSegment`.
- **Public flow:** `wasm_module_init*` followed by explicit `wasm_load_*` phases, then `wasm_module_free`.
- **Common mistake:** assuming `wasm_module_init*` parses every section; callers must invoke the load phases they need.

### [`src/fa_wasm.c`](src/fa_wasm.c)

Implements bounded disk/in-memory reads, LEB128 decoding, section parsing, typed element expressions, function-body loading, and module cleanup.

- **Key functions:** `wasm_module_init`, `wasm_module_init_from_memory`, `wasm_scan_sections`, `wasm_load_types`, `wasm_load_functions`, `wasm_load_exports`, `wasm_load_tables`, `wasm_load_memories`, `wasm_load_globals`, `wasm_load_elements`, `wasm_load_data`, `wasm_load_function_body`.
- **Portability landmark:** `wasm_strdup` replaces non-C99 `strdup`.
- **Called by:** runtime attachment, CLI/sample loaders, and tests.
- **Tests:** real fixtures plus element/global/table cases in [`test/main.c`](test/main.c).

### [`src/fa_wasm_stream.h`](src/fa_wasm_stream.h)

Declares the lazy single-function instruction stream and cursor/LEB128/address-mapping API.

- **Key type:** `WasmInstructionStream`.
- **Boundary:** the main runtime decodes loaded function bodies directly; this stream helper remains a separate public utility.

### [`src/fa_wasm_stream.c`](src/fa_wasm_stream.c)

Implements stream initialization, function load/unload, byte and LEB128 reads, PC movement, and global-offset mapping.

- **Key functions:** `wasm_instruction_stream_load_function`, `wasm_instruction_stream_read_uleb128`, `wasm_instruction_stream_read_sleb128`, `wasm_get_function_details_from_global_offset`.
- **Tests:** [`test/test_fa_wasm_stream.c`](test/test_fa_wasm_stream.c) and [`test/test_branches_fa_wasm_stream.c`](test/test_branches_fa_wasm_stream.c), currently not registered in CMake.

### [`src/fa_ops.h`](src/fa_ops.h)

Defines opcode descriptors, operation callbacks, public descriptor lookup/execution, and microcode-step lookup.

- **Key types/functions:** `fa_WasmOp`, `Operation`, `fa_get_op`, `fa_execute_op`, `fa_ops_microcode_enabled`, `fa_ops_get_microcode_steps`.
- **Boundary:** descriptors describe operations; runtime decoding owns immediates and control context.

### [`src/fa_ops.c`](src/fa_ops.c)

Owns the canonical opcode descriptor registry, resource-gated microcode selection, one-time family initialization, and the public descriptor/microcode dispatch entry points.

- **Key subparts:** `g_ops`, `g_microcode`, `init_microcode_once`, `init_ops_once`, `fa_ops_defs_populate`, `fa_execute_op`.
- **Called by:** runtime descriptor dispatch and [`src/fa_jit.c`](src/fa_jit.c) preparation.
- **Common mistake:** changing a family handler without keeping descriptor metadata or microcode routing aligned.

### [`src/fa_ops_internal.h`](src/fa_ops_internal.h)

Declares private cross-translation-unit helpers, family entry points, and numeric microcode programs. It is not part of the public opcode API in [`src/fa_ops.h`](src/fa_ops.h).

- **Common mistake:** moving an implementation-only contract into `fa_ops.h` and accidentally expanding the supported ABI.

### [`src/fa_ops_helpers.c`](src/fa_ops_helpers.c)

Owns typed operand-stack conversions, decoded-immediate window consumers, bounds/resource lookup helpers, integer bit utilities, and shared trap/OOM normalization used by opcode families.

### [`src/fa_ops_core.c`](src/fa_ops_core.c)

Owns control, local/global, scalar load/store, constant, equality-zero, and reference handlers plus their delegate tables.

### [`src/fa_ops_numeric.c`](src/fa_ops_numeric.c)

Owns scalar integer/float arithmetic, comparison, conversion, reinterpretation, drop/select handlers, and macro-built microcode programs. `REGISTER_BINARY_OPS` generates typed binary handlers and their mapping together; integer add/sub/mul wrap through unsigned arithmetic.

- **Common mistake:** changing a generated handler without updating its macro-family explanation and the central microcode/descriptor mapping.

### [`src/fa_ops_call.c`](src/fa_ops_call.c)

Owns direct/indirect call, return, and table get/set execution.

### [`src/fa_ops_bulk.c`](src/fa_ops_bulk.c)

Owns `0xFC` non-trapping conversions, bulk memory/table operations, and memory/table size and growth behavior.

### [`src/fa_ops_simd.c`](src/fa_ops_simd.c)

Owns core and relaxed SIMD helpers, the 14 `op_simd_*` family handlers, `kSimdRanges`, and `g_simd_dispatch`.

- **Common mistake:** adding a SIMD handler without a matching range and direct regression.
- **Tests for all operation files:** arithmetic, conversion, memory, table, ref, `call_indirect`, SIMD, and microcode areas in [`test/main.c`](test/main.c).

### [`src/fa_jit.h`](src/fa_jit.h)

Defines resource probes/configuration, decisions and budgets, prepared operations/programs, opcode import/export, and the shared spill envelope.

- **Key contracts:** `FA_SPILL_MAGIC`, `FA_SPILL_VERSION`, `fa_SpillKind`, `fa_JitConfig`, `fa_JitProgram`.
- **Key APIs:** `fa_jit_decide`, `fa_jit_context_apply_env_overrides`, `fa_jit_prepare_program_from_opcodes`, `fa_jit_program_serialize`, `fa_jit_program_deserialize`.

### [`src/fa_jit.c`](src/fa_jit.c)

Implements system probing, resource/advantage decisions, environment overrides, microcode preparation, stable opcode persistence, and prepared-op execution.

- **Key functions:** `fa_jit_probe_system`, `fa_jit_default_config`, `fa_jit_compute_budget`, `fa_jit_score_advantage`, `fa_jit_prepare_op`, `fa_jit_prepare_program_from_opcodes`, `fa_spill_read_header`, `fa_spill_write_header`, `fa_jit_execute_prepared_op`.
- **Tests:** JIT cache, large-body, opcode round-trip, and spill-envelope cases in [`test/main.c`](test/main.c).
- **Common mistake:** calling this a native-code JIT or serializing the prepared `Operation` pointers.

### [`src/fa_runtime.h`](src/fa_runtime.h)

Declares runtime state, status codes, memory/table ownership, execution APIs, host import ABI, trap/spill hooks, and linear-memory serializers.

- **Key types:** `fa_Runtime`, `fa_RuntimeMemory`, `fa_RuntimeTable`, `fa_RuntimeHostCall`, `fa_RuntimeTrapHooks`, `fa_RuntimeSpillHooks`.
- **Primary API:** `fa_Runtime_attachModule`, `fa_Runtime_createJob`, `fa_Runtime_executeJob`, `fa_Runtime_executeJobWithArgs`, host/import binders, trap/spill controls, and memory serialization.
- **Common mistake:** mutating public runtime fields without preserving their ownership and lifecycle invariants.

### [`src/fa_runtime.c`](src/fa_runtime.c)

Owns module attachment, execution frames, locals/globals, control-flow unwinding, imports, runtime memories/tables, instruction decoding, JIT caches, traps, and offload.

- **Key phases:** `runtime_init_globals`/`runtime_memory_init`/`runtime_tables_init`/`runtime_segments_init`; `runtime_push_frame`; `runtime_decode_instruction`; `runtime_execute_control_op`; `runtime_execute_job_internal`.
- **JIT landmarks:** `runtime_jit_cache_prescan`, `runtime_jit_record_opcode`, `runtime_jit_maybe_prepare`, `runtime_jit_lookup_prepared`.
- **Boundary landmarks:** `runtime_call_imported`, `runtime_apply_attached_memory_rebind`, `runtime_apply_attached_table_rebind`, `fa_Runtime_serializeMemory`, `fa_Runtime_deserializeMemory`.
- **Tests:** most runtime areas in [`test/main.c`](test/main.c).
- **Common mistakes:** changing only one decoder/skip path, losing caller operands during frame setup/unwind, freeing host buffers, or enabling desktop loader code on embedded targets.

### [`src/helpers/dynamic_list.h`](src/helpers/dynamic_list.h)

Provides the header-only `list_t` pointer vector used for runtime jobs.

- **Key symbols:** `list_create`, `list_push`, `list_remove`, `list_destroy`; implementation is gated by `LIST_IMPLEMENTATION`.
- **Common mistake:** defining `LIST_IMPLEMENTATION` in more than one translation unit or assuming `list_destroy` frees pointed-to objects.

### [`src/fayasm.h`](src/fayasm.h)

Currently contains only basic includes and is not a complete umbrella for the public runtime API. Include the specific `fa_*.h` owner until this facade is intentionally expanded.

### [`test/CMakeLists.txt`](test/CMakeLists.txt)

Builds and registers only `test/main.c` as `fayasm_test_main`, adds the `check` target, and registers one CTest test.

- **Common mistake:** assuming the unused `TEST_SOURCES` glob causes every `test/*.c` file to compile.

### [`test/main.c`](test/main.c)

Owns the active regression harness, in-memory module builders, offload/host test doubles, optional real-WASM smoke tests, filters, and `kTestCases`.

- **Key subparts:** `ByteBuffer` module construction, `OffloadState`, `run_wasm_sample_export_checked`, `kTestCases`, `test_matches_filter`, `run_tests`.
- **Interface:** `--list`, `--jit-prescan`, `--jit-prescan-force`, and substring filtering by name, area, or hint.
- **Common mistake:** fixture tests skip successfully when generated `.wasm` files are absent; a green harness without fixtures does not prove real-toolchain smoke coverage ran.

### [`test/test_fa_wasm_stream.c`](test/test_fa_wasm_stream.c)

Standalone stream API tests with their own `main`. This file is tracked but is not built or registered by the current CMake test target.

### [`test/test_branches_fa_wasm_stream.c`](test/test_branches_fa_wasm_stream.c)

Standalone branch-oriented stream tests with their own `main`. This file is tracked but is not built or registered by the current CMake test target.

### [`samples/cli-runner/main.c`](samples/cli-runner/main.c)

Implements `fayasm_run`: loads all needed module sections, finds a function export, parses typed `i32/i64/f32/f64` arguments, validates the signature, executes it, and prints typed results.

- **Key functions:** `load_module_from_path`, `module_find_exported_function`, `parse_typed_argument`, `print_value`, `main`.
- **Tests:** exercised manually and indirectly by the same runtime paths as fixture tests.

### [`samples/cli-runner/README.md`](samples/cli-runner/README.md)

Owns the CLI-specific build and usage contract. Keep its accepted types and output path aligned with the target in [`CMakeLists.txt`](CMakeLists.txt).

### [`samples/host-import/main.c`](samples/host-import/main.c)

Builds an in-memory module importing `env.host_add`, loads a host shared library, binds the symbol, executes export `run`, and validates the result.

- **Boundary:** loading a shared library executes native code; use only trusted library paths.

### [`samples/host-import/host_add.c`](samples/host-import/host_add.c)

Exports `fayasm_host_add`, demonstrating the `fa_RuntimeHostCall_*` ABI and signature/result validation.

### [`samples/host-import/README.md`](samples/host-import/README.md)

Owns manual macOS/Linux compile and loader-path commands for the dynamic host-import demo.

### [`samples/esp32-trap/main.c`](samples/esp32-trap/main.c)

Demonstrates function traps plus SD-backed JIT and memory spill/load using shared serializers.

- **Key functions:** `jit_spill`, `jit_load`, `memory_spill`, `memory_load`, `function_trap`.
- **Runtime data:** `/sdcard/fayasm_jit_*.bin` and `/sdcard/fayasm_mem_*.bin` are external device state, not repository artifacts.
- **Common mistake:** treating this demonstration’s direct file writes as a complete durable-storage policy; checksum and atomic replacement remain host responsibilities.

### [`samples/esp32-trap/README.md`](samples/esp32-trap/README.md)

Owns ESP32 setup plus SD wear, performance, corruption, and retention guidance.

### [`wasm_samples/build.sh`](wasm_samples/build.sh)

Generates all smoke fixtures with Emscripten first and Rust `wasm32-unknown-unknown` as fallback.

- **Key subparts:** Homebrew Emscripten environment repair, sanity check, `build_all_with_emcc`, Rust target validation.
- **Generated output:** `wasm_samples/build/` is ignored and intentionally absent from version control.
- **Common mistake:** editing generated `.wasm` files or claiming both toolchains ran when the script selects one.

### [`wasm_samples/README.md`](wasm_samples/README.md)

Owns fixture prerequisites, generated outputs, and the rule that missing fixtures make smoke cases skip.

### [`wasm_samples/src/`](wasm_samples/src/)

Contains paired C and Rust sources with matching exported behavior. Keep both variants aligned when changing a fixture.

- [`arithmetic.c`](wasm_samples/src/arithmetic.c) / [`arithmetic.rs`](wasm_samples/src/arithmetic.rs) — constants and multiply/add.
- [`control_flow.c`](wasm_samples/src/control_flow.c) / [`control_flow.rs`](wasm_samples/src/control_flow.rs) — loops and factorial.
- [`advanced_runtime.c`](wasm_samples/src/advanced_runtime.c) / [`advanced_runtime.rs`](wasm_samples/src/advanced_runtime.rs) — memory mixing and call chains.
- [`typed_values.c`](wasm_samples/src/typed_values.c) / [`typed_values.rs`](wasm_samples/src/typed_values.rs) — parameter passing and i64 results.
- [`floating_point.c`](wasm_samples/src/floating_point.c) / [`floating_point.rs`](wasm_samples/src/floating_point.rs) — float arithmetic, conversions, and open-coded square root.
- [`indirect_dispatch.c`](wasm_samples/src/indirect_dispatch.c) / [`indirect_dispatch.rs`](wasm_samples/src/indirect_dispatch.rs) — funcref tables, `call_indirect`, and `br_table`.
- [`memory_ops.c`](wasm_samples/src/memory_ops.c) / [`memory_ops.rs`](wasm_samples/src/memory_ops.rs) — sorting, data lookup, and bulk memory.
- [`integer_ops.c`](wasm_samples/src/integer_ops.c) / [`integer_ops.rs`](wasm_samples/src/integer_ops.rs) — signed division/remainder, bit operations, and nested-call operand preservation.
- [`stats_suite.c`](wasm_samples/src/stats_suite.c) / [`stats_suite.rs`](wasm_samples/src/stats_suite.rs) — deterministic statistical/algorithmic smoke and benchmark workloads.

### [`studies/`](studies/)

Research and prototypes are non-production references. Read the relevant study before repeating work:

- [`studies/runtime/stats_benchmarks.md`](studies/runtime/stats_benchmarks.md) — current bytecode/backend benchmark method and measurements, cache/ABI footprint, large-body JIT regression analysis, and historical window comparisons. [`benchmark_stats.py`](studies/runtime/benchmark_stats.py) interleaves existing builds, checks identical outputs, and records raw timings.
- [`studies/runtime/jit/runtimeCompiledFunctions.md`](studies/runtime/jit/runtimeCompiledFunctions.md) and [`studies/runtime/jit/inRuntimeExec.c`](studies/runtime/jit/inRuntimeExec.c) — runtime-compiled-function concepts and prototype code.
- [`studies/runtime/prompt.md`](studies/runtime/prompt.md), [`studies/runtime/justInTime.c`](studies/runtime/justInTime.c), and [`studies/runtime/test.c`](studies/runtime/test.c) — hybrid engine design and harness prototypes.
- [`studies/asm/`](studies/asm/) — AArch64/x86 JIT and register-allocation notes.
- [`studies/wasm/`](studies/wasm/) — WASM encoding, section, branch, and streaming-decoder notes.
- [`studies/external_ideas/`](studies/external_ideas/) — borrowed/prototype compiler, opcode, logging, and WASM material; do not compile it into the runtime without a deliberate port and license/behavior review.
- [`studies/code_samples/callbacks.c`](studies/code_samples/callbacks.c) — callback structure example.

## Features and Recurring Development Pitfalls

### Module parsing and execution — Shipped, experimental runtime

- **Behavior:** loads modules from files or memory and executes implemented exports with typed arguments/results, locals/globals, nested calls, control flow, memory, tables, refs, bulk operations, and multi-value returns.
- **Flow and owners:** [`src/fa_wasm.c`](src/fa_wasm.c) → [`src/fa_runtime.c`](src/fa_runtime.c) → [`src/fa_ops.c`](src/fa_ops.c) → [`src/fa_job.c`](src/fa_job.c).
- **Constraints:** full WASM validation and reference-type block signatures are incomplete; do not market execution of untrusted modules as sandbox-hardened.
- **Tests and gaps:** active harness areas in [`test/main.c`](test/main.c); broaden non-SIMD language/toolchain coverage.

### Selectable operand storage and immediate windows — Shipped, bounded optimization

- **Default behavior:** the newest eight typed operands are forwarded through an inline circular window. Only older simultaneously live values spill to a contiguous geometric buffer, whose capacity is retained across resets. Decoder immediates use a separate allocation-free eight-entry LIFO window.
- **Configuration:** `FA_JOB_REGISTER_WINDOW_SIZE`, `FA_JOB_STACK_MAX_DEPTH`, and `FA_JOB_DATA_FLOW_WINDOW_SIZE` are compile-time overrides; the immediate window cannot be configured below five with current decode requirements.
- **Fixed alternative:** CMake `FAYASM_FIXED_REGISTERS=ON` publishes `FA_JOB_FIXED_REGISTERS=1` to the library and consumers. The first N operands stay in fixed virtual slots and newer operands overflow contiguously, avoiding rotation/refill. The default remains circular forwarding. Use helpers instead of assuming either storage order; these are virtual, not reserved CPU registers.
- **Safety boundary:** a push beyond the operand-depth cap or immediate-window capacity fails without mutating or evicting existing values. Long execution at stable shallow depth performs no stack/window allocations.
- **Tests and evidence:** `job-window`, `test_simd_multi_memory_load32_lane`, the full runtime harness, the compact four-slot/five-immediate sanitizer configuration, and current/historical comparisons in [`studies/runtime/stats_benchmarks.md`](studies/runtime/stats_benchmarks.md).

### Table, reference, bulk-memory, and SIMD families — Shipped subset

- **Behavior:** supports the documented table/ref/bulk operations, scalar non-trapping truncations, core SIMD, and relaxed-SIMD families currently mapped by descriptors and dispatch tables.
- **Flow and owners:** runtime immediate decode → `kBulkHandlers` or `g_simd_dispatch` in [`src/fa_ops.c`](src/fa_ops.c).
- **Constraints:** new proposal opcodes require explicit decode, handler, descriptor/range, comments, and tests.
- **Tests and gaps:** `call-indirect`, `table`, `bulk-memory`, `convert`, and `simd` filters; deeper relaxed edge-case coverage remains open.

### Microcode preparation and cache — Experimental/scaffold

- **Behavior:** resource-gated descriptor-to-microcode preparation, optional function prescan, per-function caches, budgeted eviction, typed binary microcode, and bound scalar bytecode. Bound local/global indices and constants skip repeated immediate decode; immediate-free numeric instructions skip control-context setup.
- **Configuration:** `FAYASM_MICROCODE`, `FAYASM_JIT_PRESCAN`, `FAYASM_JIT_PRESCAN_FORCE`; default microcode probe requires at least 64 MiB RAM and two CPUs unless overridden.
- **Constraints:** this is not native code generation. Preserve `prepared_source_count` semantics and benchmark changes.
- **Tests and gaps:** `jit` filter and [`studies/runtime/stats_benchmarks.md`](studies/runtime/stats_benchmarks.md); native results are workload/toolchain-specific; fixed-slot performance and cache budgets still need embedded hardware validation.

### Host imports and resource rebinding — Shipped with platform boundary

- **Behavior:** binds imported functions through callbacks or desktop dynamic libraries and binds imported memories/tables before or after attach.
- **Constraints:** embedded targets reject dynamic-library binding; host buffers remain host-owned; host callback results are signature-checked.
- **Tests and gaps:** host import and imported memory/table binding/rebind cases; validation of mismatched limits needs further expansion.

### Versioned spill/load and ESP32 sample — Shipped API, experimental deployment

- **Behavior:** portable JIT opcode and linear-memory blobs plus trap/spill/load callbacks; the sample stores them on SD.
- **Constraints:** envelope validation covers structure, not durability, freshness, authenticity, or checksums. Hosts own atomicity and wear policy.
- **Tests and gaps:** `offload` filter; tables/globals persistence and long-run low-RAM/wear validation are planned.

### Real-toolchain fixtures and CLI — Shipped developer tooling

- **Behavior:** paired C/Rust modules exercise runtime paths, and `fayasm_run` invokes exported functions with typed scalar arguments.
- **Constraints:** fixture outputs are generated and optional; the Rust fallback must remain behaviorally aligned with C.
- **Tests and gaps:** `wasm-sample` filter; skipped fixtures must be reported separately from executed smoke coverage.

### Decoder drift across three paths

- **Symptom / wrong assumption:** direct execution works while block scanning or forced JIT prescan fails, misaligns the cursor, or records immediates as opcodes.
- **Cause and invariant:** decode, block-skip, and prescan-skip logic are separate in [`src/fa_runtime.c`](src/fa_runtime.c).
- **Safe pattern / regression check:** update all three applicable functions and test both default and `--jit-prescan-force` execution.
- **Status:** fixed regression risk.

### Immediate context exceeds the forwarding window

- **Symptom / wrong assumption:** a complex prefixed opcode traps or addresses memory zero because an early immediate disappeared.
- **Cause and invariant:** the decoder must never silently evict a live immediate. Multi-memory SIMD lane operations currently require five entries including the subopcode.
- **Safe pattern / regression check:** keep `FA_JOB_DATA_FLOW_WINDOW_SIZE >= 5`, return failure on overflow, and run `test_job_immediate_window_bounds` plus `test_simd_multi_memory_load32_lane`.
- **Status:** fixed regression risk.

### Function index zero confused with null

- **Symptom / wrong assumption:** `call_indirect` traps on a valid table entry for function zero.
- **Cause and invariant:** raw zero cannot represent both null and index zero; encoded refs use `n + 1`.
- **Safe pattern / regression check:** use the helpers in [`src/fa_types.h`](src/fa_types.h); run `test_call_indirect_function_zero`.
- **Status:** fixed regression risk.

### Callee unwinding destroys caller operands

- **Symptom / wrong assumption:** nested expressions return the callee result but lose values evaluated before the call.
- **Cause and invariant:** the function frame must record the stack height after popping only its arguments.
- **Safe pattern / regression check:** preserve the base passed by `runtime_push_frame`; run `test_wasm_sample_nested_calls`.
- **Status:** fixed regression risk.

### Large bodies repeatedly rebuild microcode

- **Symptom / wrong assumption:** a body larger than `max_ops_per_chunk` rebuilds on every instruction and becomes dramatically slower.
- **Cause and invariant:** readiness is compared with the capped source target via `prepared_source_count`.
- **Safe pattern / regression check:** keep the capped comparison in `runtime_jit_maybe_prepare`; run `test_jit_large_body_prepare_once`.
- **Status:** fixed regression risk.

### Optional smoke tests hide missing fixtures

- **Symptom / wrong assumption:** the full harness passes even though no compiled WASM fixture was executed.
- **Cause and invariant:** `resolve_wasm_sample_path` turns missing files into `SKIP`.
- **Safe pattern / regression check:** run [`wasm_samples/build.sh`](wasm_samples/build.sh), then the `wasm-sample` filter and inspect output for skips.
- **Status:** deliberate limitation.

## Interface Ownership Map

- Public parser/module API → [`src/fa_wasm.h`](src/fa_wasm.h), implemented by [`src/fa_wasm.c`](src/fa_wasm.c).
- Public instruction-stream API → [`src/fa_wasm_stream.h`](src/fa_wasm_stream.h), implemented by [`src/fa_wasm_stream.c`](src/fa_wasm_stream.c).
- Public runtime/job/host/trap/offload API → [`src/fa_runtime.h`](src/fa_runtime.h), [`src/fa_job.h`](src/fa_job.h), implemented primarily by [`src/fa_runtime.c`](src/fa_runtime.c) and [`src/fa_job.c`](src/fa_job.c).
- Opcode descriptor/microcode API → [`src/fa_ops.h`](src/fa_ops.h) and [`src/fa_jit.h`](src/fa_jit.h).
- CLI `fayasm_run` → [`samples/cli-runner/main.c`](samples/cli-runner/main.c).
- Test CLI and registry → `kTestCases` and `main` in [`test/main.c`](test/main.c).
- Host function callback ABI → `fa_RuntimeHostFunction` and `fa_RuntimeHostCall_*` in [`src/fa_runtime.h`](src/fa_runtime.h).
- Trap and spill extension hooks → `fa_RuntimeTrapHooks` and `fa_RuntimeSpillHooks` in [`src/fa_runtime.h`](src/fa_runtime.h).
- Build/package options → [`CMakeLists.txt`](CMakeLists.txt); target-aware wrapper → [`build.sh`](build.sh).

## Build, Run, Debug, and Release

Prerequisites: CMake 3.10 or newer and a C99 compiler. Native fixture generation additionally needs Emscripten or Rust with `wasm32-unknown-unknown`. ESP32 builds require ESP-IDF and a matching toolchain file.

```sh
# Default native clean build, fixtures when possible, and full active harness
./build.sh

# Manual native configure/build/test
cmake -S . -B build \
  -DFAYASM_BUILD_TESTS=ON \
  -DFAYASM_BUILD_SHARED=ON \
  -DFAYASM_BUILD_STATIC=ON \
  -DFAYASM_BUILD_TOOLS=ON
cmake --build build
ctest --test-dir build --output-on-failure

# Inspect and run focused tests
build/bin/fayasm_test_main --list
build/bin/fayasm_test_main call_indirect
build/bin/fayasm_test_main --jit-prescan-force jit

# Rebuild generated real-toolchain fixtures, then ensure smoke cases execute
./wasm_samples/build.sh
build/bin/fayasm_test_main wasm-sample

# Run an export
build/bin/fayasm_run wasm_samples/build/typed_values.wasm sample_add_i32 i32:7 i32:5

# ESP32 compile-oriented preset; provide the real local ESP-IDF path
./build.sh --target esp32 --esp-idf-path /path/to/esp-idf --no-fixtures

# Build CPack packages from an already configured tree
cmake --build build --target package
```

- `./build.sh` deletes and recreates its selected generated build directory by default; use `--no-clean` to retain it.
- Fixture generation writes ignored files under `wasm_samples/build/`.
- The ESP32 command sources ESP-IDF’s export script and requires a local toolchain; it does not flash hardware.
- The host-import demo has separate trusted-library commands in [`samples/host-import/README.md`](samples/host-import/README.md).
- There is no repository CI, formatter, linter, static-analysis, deployment, or automated release workflow currently tracked. Use compiler warnings, focused tests, full tests, and `git diff --check`.

## Test Ownership Map

- Both operand backends, mixed-type overflow/refill ordering/reuse, depth caps, and decoded-immediate bounds → `job-window`; five-entry multi-memory SIMD forwarding → `test_simd_multi_memory_load32_lane`.
- Operand stack, scalar arithmetic, conversions, and traps → `arith`/`conversion`/`convert` filters in [`test/main.c`](test/main.c).
- Calls, frames, branch arity, loops, multi-value results, and call depth → `control` plus `test_wasm_sample_nested_calls`.
- Parser/module integration → real `wasm-sample` cases and element/global/table module builders.
- Memories, memory64, multi-memory, bulk memory, and ownership → `memory`, `memory64`, `bulk-memory`, and imported-memory cases.
- Tables, refs, element expressions, and `call_indirect` → `table`, `refs`, and `call-indirect`.
- SIMD family dispatch → `simd`; each of the 14 family handlers has a direct case in the active registry.
- JIT/microcode/cache preparation, bound scalar reuse, and typed binary parity/overflow across spill boundaries → `jit`.
- Spill envelope, eviction, trap reload, and repeated memory cycles → `offload`.
- Host imports and live resource rebind → `test_host_import_call` and imported memory/table cases.
- Stream cursor/LEB128 utilities → the two standalone stream test files; **known gap:** current CMake does not build or run them.
- ESP32 hardware, SD wear, retention, power-loss behavior, and low-RAM footprint → **known gap:** no automated hardware test.

## Data, Security, Privacy, and Compatibility Boundaries

- WASM files and in-memory buffers are untrusted parser input in API shape, but the runtime lacks full module validation. Keep bounds/type/trap checks and do not claim a production security sandbox.
- Imported function callbacks and dynamic libraries execute with host-process authority. Dynamic-library paths and symbols must be trusted; never treat WASM module names as authorization.
- Host memory/table bindings remain host-owned. Runtime-owned buffers may be freed or replaced only when `owns_data` permits it.
- Spill blobs are portable little-endian caches/state containers with magic/version/kind/length validation. They are not authenticated, encrypted, checksummed, transactionally committed, or automatically migrated.
- Generated `.wasm`, build trees, packages, and device spill files are derived data. Tracked C/Rust fixture sources, C sources/headers, CMake, scripts, and docs are canonical.
- `FA_SPILL_VERSION` is the persisted-format compatibility boundary. Bump it deliberately for incompatible layout changes and update serializers, samples, tests, and migration/invalidation guidance together.
- Public C helpers follow scoped camelCase. Treat public struct layouts and status values as compatibility-sensitive even though no formal stability guarantee is published. Bound bytecode adds fields to `fa_JitPreparedOp` and a per-execution counter to `fa_Runtime`; rebuild consumers with matching headers/configuration. These fields do not change the persisted spill format.
- No secrets, credentials, telemetry, remote services, or user-data stores are part of the repository. Do not add real credentials to examples or research logs.

## Current Status and Known Gaps

### Shipped

- Disk/in-memory module parsing and the documented experimental runtime feature slice.
- Typed host execution, nested calls, control flow, memory64/multi-memory, multi-value returns, tables/refs, bulk memory, scalar conversions, and core/relaxed SIMD handlers covered by the active harness.
- Selectable circular/fixed operand storage and allocation-free decoded-immediate forwarding, including retained contiguous overflow storage and compile-time depth/window limits.
- Callback host imports, desktop dynamic-library imports, imported memories/tables, and live rebind.
- Resource-gated microcode preparation, macro-generated typed binary handlers, bound scalar bytecode, opcode caches, prescan controls, and stable JIT/memory spill envelopes.
- Native build wrapper, CLI runner, paired C/Rust fixtures, active regression harness, and CPack configuration.

### Experimental / Scaffold

- The runtime as a whole remains experimental and incomplete relative to the full WASM specification.
- Microcode preparation is infrastructure for a compilation path, not a demonstrated native JIT or universal performance win.
- ESP32 SD offload is a sample integration, not a production durability layer.

### Known Gaps

- Full WASM validation and reference-type block signatures remain incomplete.
- Future extension proposals and deeper relaxed-SIMD edge cases are not covered.
- Imported resource limit/mismatch validation needs broader coverage.
- Tables and globals do not use the shared persisted-state envelope.
- Standalone stream tests are not registered in CMake/CTest.
- Low-RAM, repeated offload, SD wear/power-loss, and broader embedded-target behavior need hardware validation.
- Calls, control flow, memory, and prefixed opcodes still use the common decoder/context path. Scalar bytecode caching remains capped by the prepared-op budget; broader predecode/fusion and embedded backend tuning need measurements.

### Near-Term Priorities

1. Extend the versioned persistence convention when tables/globals persistence lands.
2. Replace remaining per-family operator selection only where `stats_suite` measurements justify it.
3. Expand real-toolchain smoke coverage, especially non-SIMD outputs.
4. Validate repeated offload and runtime footprint on ESP32-class hardware.
5. Extend bytecode binding/fusion only after measuring real workloads, and validate fixed/circular sizing and prepared-cache footprint on embedded hardware.

See [`ROADMAP.md`](ROADMAP.md) for detailed status and longer-term direction.

## Task Start and Handoff Checklist

1. Read all applicable agent instructions, run `git status --short --branch`, and preserve unrelated changes.
2. Search `TODO|FIXME`, inspect the owning file subsections above, and consult the relevant [`studies/`](studies/) material.
3. Trace the affected public entry point through parser/runtime/opcode/persistence ownership and identify focused tests before editing.
4. Check funcref, stack/frame, ownership, spill-format, decode-parity, platform, and compatibility contracts as applicable.
5. Update code, comments, tests, README/sample docs, roadmap, and this handbook in the same coherent batch when their facts change.
6. Run focused tests, then the full harness when practical; distinguish executed fixture tests from skips.
7. Validate touched commands and all local Markdown links, run `git diff --check`, and review the final diff for stale paths or planned/shipped confusion.
8. Report tests run, tests not run, generated artifacts, and remaining gaps accurately.
