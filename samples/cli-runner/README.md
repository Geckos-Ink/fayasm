# fayasm_run

`fayasm_run` is a small standalone CLI for executing an exported function from a `.wasm` module outside the test harness.

## Build

The target is built by default via `./build.sh` and produced at:

- `build/bin/fayasm_run`

You can disable it with:

- `-DFAYASM_BUILD_TOOLS=OFF`

## Usage

```bash
build/bin/fayasm_run <module.wasm> <export_name>
```

Notes:

- The selected export must be a function.
- The current runner supports zero-argument exports.
- Results are printed with their WASM value type.
