# WASM Samples (Emscripten)

This folder holds real-world `.wasm` fixtures compiled with Emscripten and used by runtime smoke tests (`test_wasm_sample_*` in `test/main.c`).

## Layout

- `src/` - C sources compiled to standalone WASM modules.
- `build/` - Generated `.wasm` outputs (ignored by git).
- `build.sh` - One-shot build script for all fixtures.

## Prerequisites

- Emscripten toolchain (`emcc`) available in `PATH`.
- Recommended activation:
  - `source /path/to/emsdk/emsdk_env.sh`

## Build

```bash
./wasm_samples/build.sh
```

Generated files:

- `wasm_samples/build/arithmetic.wasm`
- `wasm_samples/build/control_flow.wasm`

## Runtime Tests

`fayasm_test_main` includes optional smoke tests that load these files directly. If files are missing, those tests print `SKIP` and pass so normal CI/dev flows stay stable without Emscripten.
