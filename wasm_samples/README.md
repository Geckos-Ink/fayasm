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

`build.sh` tries `emcc` first, then falls back to `rustc --target wasm32-unknown-unknown`.

Generated files:

- `wasm_samples/build/arithmetic.wasm`
- `wasm_samples/build/control_flow.wasm`

## Runtime Tests

`fayasm_test_main` includes optional smoke tests that load these files directly. If files are missing, those tests print `SKIP` and pass so normal CI/dev flows stay stable without Emscripten.
