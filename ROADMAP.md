# Roadmap

This file captures near-term and medium-term priorities for fayasm. Update it alongside `AI_REFERENCE.md` and `README.md` whenever plans change.

## Near-Term

- Add a small eviction/spill policy stub for the JIT cache to align with the RAM load/offload directive (targeting ESP32-class devices without PSRAM).
- Add a CLI/env toggle to force JIT prescan at runtime for profiling (without modifying code).
- Ensure ESP32 configuration is selected at compile time (CMake/defines), not via runtime probes.

## Medium-Term

- Extend microcode coverage to float unary/special ops plus reinterpret/select, and fold in a resource-aware JIT precompile pass for per-function sequences.
- Implement remaining SIMD opcodes (loads/stores, shuffles, lane ops, comparisons, arithmetic).
- Expand element/data segment support to ref.func expressions and externref tables.
- Add lane-focused SIMD tests plus coverage for additional table bounds scenarios.

## Long-Term

- Improve traps to allow real-time write and move of volatile data on another storage system.
- Implement macros for handling compilation on different architecture targets (x86, x86_64, ESP32, ..).
