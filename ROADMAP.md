# Roadmap

This file captures near-term and medium-term priorities for fayasm. Update it alongside `AI_REFERENCE.md` and `README.md` whenever plans change.

## Near-Term

- Harden JIT microcode spill formats with versioning for cross-boot reuse (avoid raw function pointers).
- Add tests for function traps and spill/load hooks (memory reloads, JIT cache eviction paths).
- Expand `samples/esp32-trap` to use the hardened spill format and document SD wear/perf considerations.

## Medium-Term

- Expand element/data segment support to ref.func expressions and externref tables.
- Add SIMD edge-case tests (saturating arithmetic, lane load/store traps, NaN handling) plus coverage for additional table bounds scenarios.

## Recently Completed

- Implemented core + relaxed SIMD opcodes (v128 load/store, shuffle/swizzle, lane ops, comparisons, arithmetic, conversions, relaxed swizzle/trunc/madd/nmadd/laneselect/min/max/q15mulr).
- Extended microcode coverage to float unary/special/reinterpret/select ops and added a resource-aware JIT precompile pass for per-function sequences.
- Wired imported functions to host callbacks or dynamic-library bindings (`dlopen`/`dlsym` scaffolding).
- Extended host import bindings to imported memories/tables, added `fa_RuntimeHostCall_*` ABI helpers, and shipped a dynamic-library host import sample.
- Added JIT cache eviction/spill hooks plus memory spill/load hooks for ESP32-class offload.
- Added prescan force toggles (`--jit-prescan-force`, `FAYASM_JIT_PRESCAN_FORCE`).
- Added compile-time target selection (`FAYASM_TARGET_ESP32`, `FAYASM_TARGET_*`).

## Long-Term

- Integrate background offload/prefetch for JIT/memory spill with wear-aware storage strategies.
- Validate embedded resource heuristics on additional targets and tune `FAYASM_TARGET_*` defaults.
