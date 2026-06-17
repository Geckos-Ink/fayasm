# Roadmap

This file captures near-term and medium-term priorities for fayasm. Update it alongside `AI_REFERENCE.md` and `README.md` whenever plans change.

## Near-Term

- Standardize runtime-wide spill/load persistence conventions around versioned opcode/memory payloads.
- Continue replacing remaining `src/fa_ops.c` switch/subopcode towers; the prefix families (control/local/global/ref/table, `0xFC`, and now `0xFD` SIMD) are table-driven. Remaining work is folding per-family operator selection into microcode where it pays off.
- Expand runtime smoke coverage using `wasm_samples/` modules (Emscripten primary toolchain, Rust fallback fixtures available).
- Validate offload behavior under repeated spill/load cycles on low-RAM targets.

## Medium-Term

- Expand runtime smoke coverage using `wasm_samples/` modules with focus on non-SIMD language/toolchain outputs.
- Add low-RAM/runtime-footprint validation passes for ESP32-class targets (tables, call depth, spill/load cycles).

## Recently Completed

- Expanded runtime smoke coverage toward non-SIMD toolchain outputs: added the `typed_values.{c,rs}` fixture and `test_wasm_sample_typed_add_i32`/`test_wasm_sample_typed_sum_to_n`/`test_wasm_sample_typed_scale_i64`, the first smoke tests to drive parameterized exports (i32/i64 args) and i64-typed returns through `fa_Runtime_executeJobWithArgs` with real compiled output (suite is 83 tests).
- Closed the last SIMD family-handler test gap: added direct regression tests for `op_simd_memlane` (`test_simd_v128_load32_lane`, `test_simd_v128_store32_lane` — both validate lane selection against non-zero lanes) and `op_simd_f64x2` (`test_simd_f64x2_add`), so all 14 `g_simd_dispatch` family handlers now have direct coverage (suite is 80 tests, all passing under CTest).
- Replaced the 347-case `0xFD` SIMD/relaxed-SIMD switch tower in `op_simd` with 14 contiguous-range family handlers reached through a prebuilt dispatch table (`g_simd_dispatch`, built once from `[lo, hi]` ranges). Added SIMD regression tests covering the family handlers (`op_simd_bitwise`, `op_simd_cmp`, `op_simd_i16x8`, `op_simd_i32x4`, `op_simd_i64x2`, `op_simd_f32x4`, `op_simd_relaxed`, `op_simd_memlane`, `op_simd_f64x2`, plus the previously covered mem/build/lane/i8x16/convert paths) and registered the test harness with CTest (`add_test`).
- Replaced shared `fa_ops.c` switch towers for control/local/global/ref/table and `0xFC` bulk-memory/table families with prebuilt delegate tables.
- Implemented `call_indirect` with table lookup, signature validation, and trap paths (null slot, OOB index, type mismatch).
- Resolved funcref representation so `null` and function index `0` are unambiguous (`null` = `0`, function index `n` = `n + 1`) across `ref.*`, `table.*`, and `call_indirect`.
- Added typed element-segment expression decoding (`flags 4..7`) for `ref.func`/`ref.null`/`global.get` with runtime resolution against globals during active/passive table initialization.
- Added baseline reference opcode execution (`ref.null`, `ref.is_null`, `ref.func`) plus regression coverage.
- Implemented core + relaxed SIMD opcodes (v128 load/store, shuffle/swizzle, lane ops, comparisons, arithmetic, conversions, relaxed swizzle/trunc/madd/nmadd/laneselect/min/max/q15mulr).
- Extended microcode coverage to float unary/special/reinterpret/select ops and added a resource-aware JIT precompile pass for per-function sequences.
- Wired imported functions to host callbacks or dynamic-library bindings (`dlopen`/`dlsym` scaffolding).
- Extended host import bindings to imported memories/tables, added `fa_RuntimeHostCall_*` ABI helpers, and shipped a dynamic-library host import sample.
- Added live rebind propagation for imported memories/tables on already-attached modules.
- Added JIT opcode import/export helpers and switched `samples/esp32-trap` to a versioned opcode spill format.
- Added targeted offload tests for repeated memory spill/load cycles and JIT eviction + trap-driven reload cycles.
- Added SD wear/perf + retention guidance to `samples/esp32-trap/README.md`.
- Added `wasm_samples/` (Emscripten fixture sources + build script) and optional runtime smoke tests that consume generated modules.
- Added Rust `wasm32-unknown-unknown` fallback fixture sources/build path for `wasm_samples/` so smoke modules can be generated without `emcc`.
- Hardened `wasm_samples/build.sh` with `emcc` sanity checks plus Homebrew Python/LLVM/Binaryen auto-detection before Rust fallback, so fixture generation remains reliable when `emcc` is installed but partially configured.
- Expanded wasm sample smoke coverage with additional exported-function checks (`sample_mul_add_const`, `sample_factorial_6`) and a new `advanced_runtime.wasm` fixture (`sample_memory_mix`, `sample_call_chain`).
- Added standalone CLI runner target `fayasm_run` for executing exported zero-argument functions from arbitrary `.wasm` modules outside the test harness.
- Extended `fayasm_run` to accept typed CLI arguments (`i32/i64/f32/f64`) and validate them against export signatures for parameterized exported-function execution.
- Wired argument transfer from operand stack into callee locals for wasm-to-wasm calls, enabling nested calls with parameters in real-world fixture modules.
- Added `fa_Runtime_executeJobWithArgs` so host code can invoke parameterized exports directly through the runtime API.
- Added JIT cache eviction/spill hooks plus memory spill/load hooks for ESP32-class offload.
- Added prescan force toggles (`--jit-prescan-force`, `FAYASM_JIT_PRESCAN_FORCE`).
- Added compile-time target selection (`FAYASM_TARGET_ESP32`, `FAYASM_TARGET_*`).
- Hardened ESP32 portability by removing embedded `dlopen`/`dlsym` compile dependencies and replacing `strdup` usage in `fa_wasm` with a C99-safe local duplicate helper.
- Reworked `build.sh` into a target-aware build orchestrator with native/ESP32 presets, ESP-IDF/toolchain personalization flags, pass-through CMake args, and configurable fixture/test execution.

## Long-Term

- Integrate background offload/prefetch for JIT/memory spill with wear-aware storage strategies.
- Validate embedded resource heuristics on additional targets and tune `FAYASM_TARGET_*` defaults.
