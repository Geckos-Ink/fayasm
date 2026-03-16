#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${ROOT_DIR}/src"
OUT_DIR="${ROOT_DIR}/build"
EMCC_BIN="${EMCC:-emcc}"
RUSTC_BIN="${RUSTC:-rustc}"
RUST_TARGET="${RUST_TARGET:-wasm32-unknown-unknown}"
EMCC_REASON=""
EMCC_ENV=()

mkdir -p "${OUT_DIR}"

build_with_emcc() {
    local src_name="$1"
    local out_name="$2"
    shift 2

    env "${EMCC_ENV[@]}" "${EMCC_BIN}" "${SRC_DIR}/${src_name}" \
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

python_supports_emscripten() {
    local python_bin="$1"
    [ -n "${python_bin}" ] || return 1
    [ -x "${python_bin}" ] || return 1

    "${python_bin}" - <<'PY' >/dev/null 2>&1
import sys
raise SystemExit(0 if sys.version_info >= (3, 10) else 1)
PY
}

detect_homebrew_emscripten_root() {
    local emcc_path="$1"
    local prefix=""
    local root=""

    case "${emcc_path}" in
        /opt/homebrew/*)
            prefix="/opt/homebrew"
            ;;
        /usr/local/*)
            prefix="/usr/local"
            ;;
        *)
            return 1
            ;;
    esac

    root="$(ls -1d "${prefix}/Cellar/emscripten/"*/libexec 2>/dev/null | sort | tail -n 1 || true)"
    if [ -z "${root}" ] || [ ! -d "${root}" ]; then
        return 1
    fi

    printf '%s\n' "${root}"
}

prepare_emcc_env() {
    EMCC_ENV=()
    EMCC_REASON=""

    if ! command -v "${EMCC_BIN}" >/dev/null 2>&1; then
        EMCC_REASON="emcc not found in PATH"
        return 1
    fi

    local emcc_path
    emcc_path="$(command -v "${EMCC_BIN}")"

    local emscripten_root=""
    emscripten_root="$(detect_homebrew_emscripten_root "${emcc_path}" || true)"

    local emcc_python="${EMSDK_PYTHON:-}"
    if ! python_supports_emscripten "${emcc_python}"; then
        emcc_python=""
        local candidate
        for candidate in \
            "$(command -v python3 2>/dev/null || true)" \
            /opt/homebrew/bin/python3 \
            /usr/local/bin/python3; do
            if python_supports_emscripten "${candidate}"; then
                emcc_python="${candidate}"
                break
            fi
        done
    fi
    if [ -n "${emcc_python}" ]; then
        EMCC_ENV+=("EMSDK_PYTHON=${emcc_python}")
    fi

    local llvm_root="${EM_LLVM_ROOT:-}"
    local binaryen_root="${EM_BINARYEN_ROOT:-}"
    if [ -n "${emscripten_root}" ]; then
        if [ -z "${llvm_root}" ] && [ -x "${emscripten_root}/llvm/bin/clang" ]; then
            llvm_root="${emscripten_root}/llvm/bin"
        fi
        if [ -z "${binaryen_root}" ] && [ -x "${emscripten_root}/binaryen/bin/wasm-opt" ]; then
            binaryen_root="${emscripten_root}/binaryen"
        fi
    fi
    if [ -n "${llvm_root}" ]; then
        EMCC_ENV+=("EM_LLVM_ROOT=${llvm_root}")
    fi
    if [ -n "${binaryen_root}" ]; then
        EMCC_ENV+=("EM_BINARYEN_ROOT=${binaryen_root}")
    fi

    if [ -n "${llvm_root}" ] && [ ! -x "${llvm_root}/wasm-ld" ]; then
        EMCC_REASON="emcc LLVM root '${llvm_root}' does not provide wasm-ld"
        return 1
    fi

    if ! env "${EMCC_ENV[@]}" "${EMCC_BIN}" --version >/dev/null 2>&1; then
        EMCC_REASON="emcc sanity check failed (python/LLVM/Binaryen mismatch)"
        return 1
    fi

    return 0
}

build_all_with_emcc() {
    build_with_emcc arithmetic.c arithmetic.wasm \
        -Wl,--export=sample_const42 \
        -Wl,--export=sample_mul_add_const

    build_with_emcc control_flow.c control_flow.wasm \
        -Wl,--export=sample_loop_sum \
        -Wl,--export=sample_factorial_6

    build_with_emcc advanced_runtime.c advanced_runtime.wasm \
        -Wl,--export=sample_memory_mix \
        -Wl,--export=sample_call_chain
}

if command -v "${EMCC_BIN}" >/dev/null 2>&1; then
    if prepare_emcc_env && build_all_with_emcc; then
        toolchain="emcc"
    else
        if [ -n "${EMCC_REASON}" ]; then
            echo "warning: ${EMCC_REASON}. Falling back to rustc." >&2
        else
            echo "warning: emcc compilation failed. Falling back to rustc." >&2
        fi
    fi
fi

if [ -z "${toolchain:-}" ] && command -v "${RUSTC_BIN}" >/dev/null 2>&1; then
    if ! "${RUSTC_BIN}" --target "${RUST_TARGET}" --print cfg >/dev/null 2>&1; then
        echo "error: rust target '${RUST_TARGET}' is not installed." >&2
        echo "hint: run 'rustup target add ${RUST_TARGET}'." >&2
        exit 1
    fi

    build_with_rustc arithmetic.rs arithmetic.wasm
    build_with_rustc control_flow.rs control_flow.wasm
    build_with_rustc advanced_runtime.rs advanced_runtime.wasm
    toolchain="rustc (${RUST_TARGET}) fallback"
fi

if [ -z "${toolchain:-}" ]; then
    echo "error: no wasm toolchain available. Install Emscripten (emcc) or rustc with target '${RUST_TARGET}'." >&2
    exit 1
fi

echo "Built wasm samples with ${toolchain}:"
echo "  - ${OUT_DIR}/arithmetic.wasm"
echo "  - ${OUT_DIR}/control_flow.wasm"
echo "  - ${OUT_DIR}/advanced_runtime.wasm"
