#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${ROOT_DIR}/src"
OUT_DIR="${ROOT_DIR}/build"
EMCC_BIN="${EMCC:-emcc}"
RUSTC_BIN="${RUSTC:-rustc}"
RUST_TARGET="${RUST_TARGET:-wasm32-unknown-unknown}"

mkdir -p "${OUT_DIR}"

build_with_emcc() {
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

build_with_rustc() {
    local src_name="$1"
    local out_name="$2"

    "${RUSTC_BIN}" \
        --target "${RUST_TARGET}" \
        -O \
        --crate-type=cdylib \
        -C panic=abort \
        "${SRC_DIR}/${src_name}" \
        -o "${OUT_DIR}/${out_name}"
}

if command -v "${EMCC_BIN}" >/dev/null 2>&1; then
    build_with_emcc arithmetic.c arithmetic.wasm \
        -Wl,--export=sample_const42 \
        -Wl,--export=sample_mul_add_const

    build_with_emcc control_flow.c control_flow.wasm \
        -Wl,--export=sample_loop_sum \
        -Wl,--export=sample_factorial_6
    toolchain="emcc"
elif command -v "${RUSTC_BIN}" >/dev/null 2>&1; then
    if ! "${RUSTC_BIN}" --target "${RUST_TARGET}" --print cfg >/dev/null 2>&1; then
        echo "error: rust target '${RUST_TARGET}' is not installed." >&2
        echo "hint: run 'rustup target add ${RUST_TARGET}'." >&2
        exit 1
    fi

    build_with_rustc arithmetic.rs arithmetic.wasm
    build_with_rustc control_flow.rs control_flow.wasm
    toolchain="rustc (${RUST_TARGET}) fallback"
else
    echo "error: no wasm toolchain available. Install Emscripten (emcc) or rustc with target '${RUST_TARGET}'." >&2
    exit 1
fi

echo "Built wasm samples with ${toolchain}:"
echo "  - ${OUT_DIR}/arithmetic.wasm"
echo "  - ${OUT_DIR}/control_flow.wasm"
