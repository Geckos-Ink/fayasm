# Host Import Sample

This sample loads a tiny WASM module that imports `env.host_add`, binds it from a
shared library, and calls the exported `run` function.

## Build

1. Build fayasm (shared library required):
   ```bash
   mkdir -p build
   cd build
   cmake .. -DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON
   cmake --build .
   ```

2. Build the host library from this directory:
   - macOS:
     ```bash
     cc -shared -fPIC -I../../src host_add.c -L../../build/lib -lfayasm -o libfayasm_host_add.dylib
     ```
   - Linux:
     ```bash
     cc -shared -fPIC -I../../src host_add.c -L../../build/lib -lfayasm -o libfayasm_host_add.so
     ```

3. Build the runner:
   - macOS:
     ```bash
     cc -I../../src main.c -L../../build/lib -lfayasm -o host_import_demo
     ```
   - Linux:
     ```bash
     cc -I../../src main.c -L../../build/lib -lfayasm -o host_import_demo
     ```

## Run

- macOS:
  ```bash
  DYLD_LIBRARY_PATH=../../build/lib ./host_import_demo ./libfayasm_host_add.dylib
  ```
- Linux:
  ```bash
  LD_LIBRARY_PATH=../../build/lib ./host_import_demo ./libfayasm_host_add.so
  ```

The runner defaults to `./libfayasm_host_add` when no library path is provided.
