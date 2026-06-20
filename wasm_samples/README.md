# WASM Samples

This folder holds `.wasm` fixtures used by runtime smoke tests (`test_wasm_sample_*` in `test/main.c`).

## Layout

- `src/` - fixture sources (`.c` for Emscripten and `.rs` for Rust fallback).
- `build/` - Generated `.wasm` outputs (ignored by git).
- `build.sh` - One-shot build script for all fixtures.

## Prerequisites

- Preferred: Emscripten toolchain (`emcc`) available in `PATH`.
- Fallback: Rust toolchain (`rustc`) with `wasm32-unknown-unknown` target installed.
  - `rustup target add wasm32-unknown-unknown`

## Build

```bash
./wasm_samples/build.sh
```

`build.sh` tries `emcc` first, performs a sanity check, and auto-detects Homebrew Emscripten paths (`EMSDK_PYTHON`, `EM_LLVM_ROOT`, `EM_BINARYEN_ROOT`) when needed; if `emcc` still fails, it falls back to `rustc --target wasm32-unknown-unknown`.

Generated files:

- `wasm_samples/build/arithmetic.wasm`
- `wasm_samples/build/control_flow.wasm`
- `wasm_samples/build/advanced_runtime.wasm`
- `wasm_samples/build/typed_values.wasm` (parameterized exports + i64 return)
- `wasm_samples/build/floating_point.wasm` (f32/f64 arithmetic, int<->float conversions incl. `trunc_sat`)
- `wasm_samples/build/indirect_dispatch.wasm` (function-pointer table -> `call_indirect`; dense switch -> `br_table`)
- `wasm_samples/build/memory_ops.wasm` (array sort, data-segment lookup, `memcpy`/`memset` -> `memory.copy`/`memory.fill`; built with `-mbulk-memory`)

## Runtime Tests

`fayasm_test_main` includes optional smoke tests that load these files directly. If files are missing, those tests print `SKIP` and pass so normal CI/dev flows stay stable without Emscripten.
