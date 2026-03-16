# fayasm_run

`fayasm_run` is a small standalone CLI for executing an exported function from a `.wasm` module outside the test harness.

## Build

The target is built by default via `./build.sh` and produced at:

- `build/bin/fayasm_run`

You can disable it with:

- `-DFAYASM_BUILD_TOOLS=OFF`

## Usage

```bash
build/bin/fayasm_run <module.wasm> <export_name> [typed-arg ...]
```

Notes:

- The selected export must be a function.
- Typed args must match the export signature and use:
  - `i32:<value>`
  - `i64:<value>`
  - `f32:<value>`
  - `f64:<value>`
- Results are printed with their WASM value type.

Example:

```bash
build/bin/fayasm_run module.wasm add i32:7 i32:5
```
