# Roadmap

This file captures near-term and medium-term priorities for fayasm. Update it alongside `AI_REFERENCE.md` and `README.md` whenever plans change.

## Near-Term

- Standardize runtime-wide spill/load persistence conventions around versioned opcode/memory payloads.
- Expand runtime smoke coverage using Emscripten-built `wasm_samples/` modules.
- Validate offload behavior under repeated spill/load cycles on low-RAM targets.

## Medium-Term

- Expand element/data segment support to ref.func expressions and externref tables.
- Add SIMD edge-case tests (saturating arithmetic, lane load/store traps, NaN handling) plus coverage for additional table bounds scenarios.

## Recently Completed

- Implemented core + relaxed SIMD opcodes (v128 load/store, shuffle/swizzle, lane ops, comparisons, arithmetic, conversions, relaxed swizzle/trunc/madd/nmadd/laneselect/min/max/q15mulr).
- Extended microcode coverage to float unary/special/reinterpret/select ops and added a resource-aware JIT precompile pass for per-function sequences.
- Wired imported functions to host callbacks or dynamic-library bindings (`dlopen`/`dlsym` scaffolding).
- Extended host import bindings to imported memories/tables, added `fa_RuntimeHostCall_*` ABI helpers, and shipped a dynamic-library host import sample.
- Added live rebind propagation for imported memories/tables on already-attached modules.
- Added JIT opcode import/export helpers and switched `samples/esp32-trap` to a versioned opcode spill format.
- Added targeted offload tests for repeated memory spill/load cycles and JIT eviction + trap-driven reload cycles.
- Added SD wear/perf + retention guidance to `samples/esp32-trap/README.md`.
- Added `wasm_samples/` (Emscripten fixture sources + build script) and optional runtime smoke tests that consume generated modules.
- Added JIT cache eviction/spill hooks plus memory spill/load hooks for ESP32-class offload.
- Added prescan force toggles (`--jit-prescan-force`, `FAYASM_JIT_PRESCAN_FORCE`).
- Added compile-time target selection (`FAYASM_TARGET_ESP32`, `FAYASM_TARGET_*`).

## Long-Term

- Integrate background offload/prefetch for JIT/memory spill with wear-aware storage strategies.
- Validate embedded resource heuristics on additional targets and tune `FAYASM_TARGET_*` defaults.
