#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${ROOT_DIR}/src"
OUT_DIR="${ROOT_DIR}/build"
EMCC_BIN="${EMCC:-emcc}"

if ! command -v "${EMCC_BIN}" >/dev/null 2>&1; then
    echo "error: emcc not found. Install/activate Emscripten first (emsdk_env.sh)." >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

build_sample() {
    local src_name="$1"
    local out_name="$2"
    shift 2

    "${EMCC_BIN}" "${SRC_DIR}/${src_name}" \
        -O2 \
        -s STANDALONE_WASM=1 \
        -Wl,--no-entry \
        "$@" \
        -o "${OUT_DIR}/${out_name}"
}

build_sample arithmetic.c arithmetic.wasm \
    -Wl,--export=sample_const42 \
    -Wl,--export=sample_mul_add_const

build_sample control_flow.c control_flow.wasm \
    -Wl,--export=sample_loop_sum \
    -Wl,--export=sample_factorial_6

echo "Built wasm samples:"
echo "  - ${OUT_DIR}/arithmetic.wasm"
echo "  - ${OUT_DIR}/control_flow.wasm"
