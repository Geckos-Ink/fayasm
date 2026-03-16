#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -n "${TERM:-}" && "${TERM}" != "dumb" ]]; then
    clear
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

DEBUG_MODE=false

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*"
}

debug_print() {
    if [[ "${DEBUG_MODE}" == "true" ]]; then
        echo -e "${BLUE}[DEBUG]${NC} $*"
    fi
}

usage() {
    cat <<'USAGE'
Usage: ./build.sh [options] [-- <extra-cmake-args...>]

General options:
  --target <native|esp32>       Build target preset (default: native)
  --build-dir <path>            Build directory (default: build or build-esp32)
  --generator <name>            CMake generator (example: Ninja)
  --build-type <type>           CMAKE_BUILD_TYPE (Debug/Release/RelWithDebInfo/MinSizeRel)
  --jobs <n>                    Parallel build jobs (default: auto)
  --clean / --no-clean          Remove build directory before configure (default: clean)
  --fixtures / --no-fixtures    Build wasm_samples fixtures first
  --run-tests / --no-run-tests  Execute fayasm_test_main after build
  --debug / --no-debug          Enable verbose debug lines

Fayasm CMake toggles:
  --shared <ON|OFF>             FAYASM_BUILD_SHARED
  --static <ON|OFF>             FAYASM_BUILD_STATIC
  --tests <ON|OFF>              FAYASM_BUILD_TESTS
  --tools <ON|OFF>              FAYASM_BUILD_TOOLS

ESP32 options:
  --esp-idf-path <path>         ESP-IDF root (fallbacks: IDF_PATH env, /Users/riccardo/esp/esp-idf)
  --esp-export-script <path>    export.sh path (default: <esp-idf-path>/export.sh)
  --esp-chip <name>             Toolchain suffix (default: esp32, e.g. esp32s3)
  --esp-toolchain-file <path>   Explicit CMake toolchain file
  --esp-ram-bytes <n>           Set FAYASM_TARGET_RAM_BYTES compile definition
  --esp-cpu-count <n>           Set FAYASM_TARGET_CPU_COUNT compile definition

Compatibility:
  -D...                         Forwarded to CMake
  --cmake-arg <arg>             Forwarded to CMake (repeatable)
  --help                        Show this help

Examples:
  ./build.sh
  ./build.sh --target esp32 --esp-idf-path /Users/riccardo/esp/esp-idf
  ./build.sh --target esp32 --esp-ram-bytes 262144 --esp-cpu-count 2 --cmake-arg -DFAYASM_BUILD_SHARED=OFF
USAGE
}

auto_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
        return
    fi
    if command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 1
        return
    fi
    echo 1
}

TARGET="native"
BUILD_DIR=""
EXPLICIT_BUILD_DIR=false
GENERATOR=""
BUILD_TYPE=""
JOBS="$(auto_jobs)"
CLEAN_BUILD=true
RUN_FIXTURES=true
RUN_TESTS_MODE="auto"

FAYASM_BUILD_SHARED="ON"
FAYASM_BUILD_STATIC="ON"
FAYASM_BUILD_TESTS="ON"
FAYASM_BUILD_TOOLS="ON"
EXPLICIT_SHARED=false
EXPLICIT_STATIC=false
EXPLICIT_TESTS=false
EXPLICIT_TOOLS=false

ESP_IDF_PATH="${IDF_PATH:-}"
ESP_EXPORT_SCRIPT=""
ESP_CHIP="esp32"
ESP_TOOLCHAIN_FILE=""
ESP_RAM_BYTES=""
ESP_CPU_COUNT=""

EXTRA_CMAKE_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --target)
            TARGET="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            EXPLICIT_BUILD_DIR=true
            shift 2
            ;;
        --generator)
            GENERATOR="$2"
            shift 2
            ;;
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --no-clean)
            CLEAN_BUILD=false
            shift
            ;;
        --fixtures)
            RUN_FIXTURES=true
            shift
            ;;
        --no-fixtures)
            RUN_FIXTURES=false
            shift
            ;;
        --run-tests)
            RUN_TESTS_MODE="on"
            shift
            ;;
        --no-run-tests)
            RUN_TESTS_MODE="off"
            shift
            ;;
        --debug)
            DEBUG_MODE=true
            shift
            ;;
        --no-debug)
            DEBUG_MODE=false
            shift
            ;;
        --shared)
            FAYASM_BUILD_SHARED="$2"
            EXPLICIT_SHARED=true
            shift 2
            ;;
        --static)
            FAYASM_BUILD_STATIC="$2"
            EXPLICIT_STATIC=true
            shift 2
            ;;
        --tests)
            FAYASM_BUILD_TESTS="$2"
            EXPLICIT_TESTS=true
            shift 2
            ;;
        --tools)
            FAYASM_BUILD_TOOLS="$2"
            EXPLICIT_TOOLS=true
            shift 2
            ;;
        --esp-idf-path)
            ESP_IDF_PATH="$2"
            shift 2
            ;;
        --esp-export-script)
            ESP_EXPORT_SCRIPT="$2"
            shift 2
            ;;
        --esp-chip)
            ESP_CHIP="$2"
            shift 2
            ;;
        --esp-toolchain-file)
            ESP_TOOLCHAIN_FILE="$2"
            shift 2
            ;;
        --esp-ram-bytes)
            ESP_RAM_BYTES="$2"
            shift 2
            ;;
        --esp-cpu-count)
            ESP_CPU_COUNT="$2"
            shift 2
            ;;
        --cmake-arg)
            EXTRA_CMAKE_ARGS+=("$2")
            shift 2
            ;;
        --)
            shift
            while [[ $# -gt 0 ]]; do
                EXTRA_CMAKE_ARGS+=("$1")
                shift
            done
            ;;
        -D*)
            EXTRA_CMAKE_ARGS+=("$1")
            shift
            ;;
        *)
            log_warn "Unknown argument '$1' forwarded to CMake"
            EXTRA_CMAKE_ARGS+=("$1")
            shift
            ;;
    esac
done

case "${TARGET}" in
    native|esp32)
        ;;
    *)
        log_error "Unsupported --target '${TARGET}'. Use 'native' or 'esp32'."
        exit 1
        ;;
esac

if [[ "${TARGET}" == "esp32" ]]; then
    if [[ "${EXPLICIT_SHARED}" == "false" ]]; then
        FAYASM_BUILD_SHARED="OFF"
    fi
    if [[ "${EXPLICIT_TESTS}" == "false" ]]; then
        FAYASM_BUILD_TESTS="OFF"
    fi
    if [[ "${EXPLICIT_TOOLS}" == "false" ]]; then
        FAYASM_BUILD_TOOLS="OFF"
    fi
    if [[ "${RUN_TESTS_MODE}" == "auto" ]]; then
        RUN_TESTS_MODE="off"
    fi
    if [[ "${RUN_FIXTURES}" == "true" ]]; then
        log_warn "Fixture build is enabled for ESP32 target; disable with --no-fixtures if unnecessary."
    fi
    if [[ "${EXPLICIT_BUILD_DIR}" == "false" ]]; then
        BUILD_DIR="${SCRIPT_DIR}/build-esp32"
    fi
else
    if [[ "${EXPLICIT_BUILD_DIR}" == "false" ]]; then
        BUILD_DIR="${SCRIPT_DIR}/build"
    fi
fi

if [[ -z "${BUILD_DIR}" ]]; then
    log_error "Build directory is empty"
    exit 1
fi

if [[ -n "${BUILD_TYPE}" ]]; then
    EXTRA_CMAKE_ARGS+=("-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
fi

CMAKE_ARGS=(
    "-DFAYASM_BUILD_TESTS=${FAYASM_BUILD_TESTS}"
    "-DFAYASM_BUILD_SHARED=${FAYASM_BUILD_SHARED}"
    "-DFAYASM_BUILD_STATIC=${FAYASM_BUILD_STATIC}"
    "-DFAYASM_BUILD_TOOLS=${FAYASM_BUILD_TOOLS}"
)

if [[ "${TARGET}" == "esp32" ]]; then
    if [[ -z "${ESP_IDF_PATH}" && -d "/Users/riccardo/esp/esp-idf" ]]; then
        ESP_IDF_PATH="/Users/riccardo/esp/esp-idf"
    fi
    if [[ -n "${ESP_IDF_PATH}" && -z "${ESP_EXPORT_SCRIPT}" ]]; then
        ESP_EXPORT_SCRIPT="${ESP_IDF_PATH}/export.sh"
    fi
    if [[ -n "${ESP_EXPORT_SCRIPT}" ]]; then
        if [[ ! -f "${ESP_EXPORT_SCRIPT}" ]]; then
            log_error "ESP-IDF export script not found: ${ESP_EXPORT_SCRIPT}"
            exit 1
        fi
        # shellcheck disable=SC1090
        source "${ESP_EXPORT_SCRIPT}" >/dev/null
        debug_print "Sourced ESP-IDF export script: ${ESP_EXPORT_SCRIPT}"
        if [[ -z "${ESP_IDF_PATH}" && -n "${IDF_PATH:-}" ]]; then
            ESP_IDF_PATH="${IDF_PATH}"
        fi
    fi
    if [[ -z "${ESP_IDF_PATH}" ]]; then
        log_error "ESP-IDF path not set. Use --esp-idf-path or export IDF_PATH."
        exit 1
    fi
    if [[ -z "${ESP_TOOLCHAIN_FILE}" ]]; then
        ESP_TOOLCHAIN_FILE="${ESP_IDF_PATH}/tools/cmake/toolchain-${ESP_CHIP}.cmake"
    fi
    if [[ ! -f "${ESP_TOOLCHAIN_FILE}" ]]; then
        log_error "ESP toolchain file not found: ${ESP_TOOLCHAIN_FILE}"
        exit 1
    fi
    CMAKE_ARGS+=("-DFAYASM_TARGET_ESP32=ON")
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${ESP_TOOLCHAIN_FILE}")
    if [[ -n "${ESP_RAM_BYTES}" ]]; then
        CMAKE_ARGS+=("-DFAYASM_TARGET_RAM_BYTES=${ESP_RAM_BYTES}")
    fi
    if [[ -n "${ESP_CPU_COUNT}" ]]; then
        CMAKE_ARGS+=("-DFAYASM_TARGET_CPU_COUNT=${ESP_CPU_COUNT}")
    fi
fi

GENERATOR_ARGS=()
if [[ -n "${GENERATOR}" ]]; then
    GENERATOR_ARGS+=("-G" "${GENERATOR}")
elif command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS+=("-G" "Ninja")
fi

RUN_TESTS=false
if [[ "${RUN_TESTS_MODE}" == "on" ]]; then
    RUN_TESTS=true
elif [[ "${RUN_TESTS_MODE}" == "off" ]]; then
    RUN_TESTS=false
elif [[ "${TARGET}" == "native" && "${FAYASM_BUILD_TESTS}" == "ON" ]]; then
    RUN_TESTS=true
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}        FAYASM BUILD SCRIPT            ${NC}"
echo -e "${BLUE}========================================${NC}"
log_info "Target: ${TARGET}"
log_info "Build dir: ${BUILD_DIR}"
log_info "Jobs: ${JOBS}"
if [[ "${TARGET}" == "esp32" ]]; then
    log_info "ESP-IDF: ${ESP_IDF_PATH}"
    log_info "ESP toolchain: ${ESP_TOOLCHAIN_FILE}"
fi

if [[ "${RUN_FIXTURES}" == "true" ]]; then
    log_info "Building wasm_samples fixtures (emcc preferred, rust fallback)..."
    if "${SCRIPT_DIR}/wasm_samples/build.sh"; then
        debug_print "WASM fixtures built in wasm_samples/build/"
    else
        log_warn "No WASM fixture toolchain available; fixture smoke tests may SKIP."
    fi
else
    debug_print "Fixture build skipped"
fi

if [[ "${CLEAN_BUILD}" == "true" && -d "${BUILD_DIR}" ]]; then
    log_info "Cleaning build directory"
    cmake -E remove_directory "${BUILD_DIR}"
fi
cmake -E make_directory "${BUILD_DIR}"

log_info "Configuring CMake"
cmake_args_display="${CMAKE_ARGS[*]}"
if (( ${#EXTRA_CMAKE_ARGS[@]} > 0 )); then
    cmake_args_display+=" ${EXTRA_CMAKE_ARGS[*]}"
fi
log_info "CMake args: ${cmake_args_display}"

CONFIGURE_CMD=(cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}")
if (( ${#GENERATOR_ARGS[@]} > 0 )); then
    CONFIGURE_CMD+=("${GENERATOR_ARGS[@]}")
fi
CONFIGURE_CMD+=("${CMAKE_ARGS[@]}")
if (( ${#EXTRA_CMAKE_ARGS[@]} > 0 )); then
    CONFIGURE_CMD+=("${EXTRA_CMAKE_ARGS[@]}")
fi
"${CONFIGURE_CMD[@]}"

log_info "Building targets"
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

if [[ "${RUN_TESTS}" == "true" ]]; then
    TEST_BIN="${BUILD_DIR}/bin/fayasm_test_main"
    if [[ -x "${TEST_BIN}" ]]; then
        log_info "Running test harness"
        "${TEST_BIN}"
    else
        log_error "Test binary not found: ${TEST_BIN}"
        exit 1
    fi
else
    debug_print "Test execution skipped"
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Build completed successfully${NC}"
echo -e "${BLUE}========================================${NC}"
log_info "Artifacts available in ${BUILD_DIR}"
if [[ -x "${BUILD_DIR}/bin/fayasm_test_main" ]]; then
    log_info "Test binary: ${BUILD_DIR}/bin/fayasm_test_main"
fi
if [[ -x "${BUILD_DIR}/bin/fayasm_run" ]]; then
    log_info "CLI runner: ${BUILD_DIR}/bin/fayasm_run"
fi
