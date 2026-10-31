#include "fa_ops_internal.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif

static fa_WasmOp g_ops[256];
static bool g_ops_initialized = false;
static const fa_Microcode* g_microcode[256];
static bool g_microcode_initialized = false;
static bool g_microcode_enabled = false;

/*
 * Central opcode registry and dispatch.
 * Handler implementations live in the focused fa_ops_* translation units;
 * keep descriptor metadata and microcode routing synchronized with them.
 * Numeric register microcode is generated from the owning family's macro
 * list; it overrides generic binary programs after this registry is built.
 */

/* Centralized descriptor constructor used by `fa_ops_defs_populate`. */
static void define_op(
    fa_WasmOp* ops,
    uint8_t opcode,
    const fa_WasmType* type,
    fa_WasmOp_type op_kind,
    uint8_t size_arg,
    uint8_t num_pull,
    uint8_t num_push,
    uint8_t num_args,
    Operation handler) {
    if (!ops) {
        return;
    }
    fa_WasmOp* dst = &ops[opcode];
    dst->id = opcode;
    if (type) {
        dst->type = *type;
    } else {
        dst->type.type = wt_void;
        dst->type.size = 0;
        dst->type.is_signed = false;
    }
    dst->op = op_kind;
    dst->size_arg = size_arg;
    dst->num_pull = num_pull;
    dst->num_push = num_push;
    dst->num_args = num_args;
    dst->operation = handler;
}

typedef struct {
    uint64_t ram_bytes;
    uint32_t cpu_count;
    bool ok;
} fa_SystemProbe;

#ifndef FA_MICROCODE_MIN_RAM_BYTES
#define FA_MICROCODE_MIN_RAM_BYTES (64ULL * 1024ULL * 1024ULL)
#endif

#ifndef FA_MICROCODE_MIN_CPU_COUNT
#define FA_MICROCODE_MIN_CPU_COUNT 2U
#endif

/*
 * Resource probe used only for default microcode enablement.
 * Embedded targets rely on compile-time hints (`FAYASM_TARGET_*`) while
 * desktop targets query host RAM/CPU.
 */
static fa_SystemProbe probe_system_resources(void) {
    fa_SystemProbe probe;
    memset(&probe, 0, sizeof(probe));
#if defined(FAYASM_TARGET_EMBEDDED)
    #if defined(FAYASM_TARGET_RAM_BYTES)
    probe.ram_bytes = (uint64_t)FAYASM_TARGET_RAM_BYTES;
    #endif
    #if defined(FAYASM_TARGET_CPU_COUNT)
    probe.cpu_count = (uint32_t)FAYASM_TARGET_CPU_COUNT;
    #endif
    probe.ok = (probe.ram_bytes > 0 && probe.cpu_count > 0);
    return probe;
#endif
#if defined(_WIN32)
    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        probe.ram_bytes = (uint64_t)memory.ullTotalPhys;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors > 0) {
        probe.cpu_count = (uint32_t)info.dwNumberOfProcessors;
    }
#elif defined(__APPLE__) || defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        probe.ram_bytes = (uint64_t)pages * (uint64_t)page_size;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) {
        cpus = sysconf(_SC_NPROCESSORS_CONF);
    }
    if (cpus > 0) {
        probe.cpu_count = (uint32_t)cpus;
    }
#if defined(__APPLE__)
    if (probe.ram_bytes == 0) {
        uint64_t memsize = 0;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0 && memsize > 0) {
            probe.ram_bytes = memsize;
        }
    }
    if (probe.cpu_count == 0) {
        uint32_t ncpu = 0;
        size_t len = sizeof(ncpu);
        if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
            probe.cpu_count = ncpu;
        }
    }
#endif
#endif
    probe.ok = (probe.ram_bytes > 0 && probe.cpu_count > 0);
    return probe;
}

static bool microcode_env_override(bool* enabled_out) {
    if (!enabled_out) {
        return false;
    }
    const char* env = getenv("FAYASM_MICROCODE");
    if (!env) {
        return false;
    }
    if (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 || strcmp(env, "on") == 0) {
        *enabled_out = true;
        return true;
    }
    if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0 || strcmp(env, "off") == 0) {
        *enabled_out = false;
        return true;
    }
    return false;
}

static bool microcode_should_enable(void) {
    bool enabled = false;
    if (microcode_env_override(&enabled)) {
        return enabled;
    }
    const fa_SystemProbe probe = probe_system_resources();
    if (!probe.ok) {
        return false;
    }
    if (probe.ram_bytes < FA_MICROCODE_MIN_RAM_BYTES) {
        return false;
    }
    if (probe.cpu_count < FA_MICROCODE_MIN_CPU_COUNT) {
        return false;
    }
    return true;
}

/*
 * Populates opcode -> microcode mapping.
 * Keep this table aligned with:
 * - handlers registered in `fa_ops_defs_populate`
 * - tests that assert microcode/dispatch behavior
 */
static void init_microcode_once(void) {
    if (g_microcode_initialized) {
        return;
    }
    memset(g_microcode, 0, sizeof(g_microcode));
    g_microcode_enabled = microcode_should_enable();
    if (g_microcode_enabled) {
        g_microcode[0x1B] = &mc_select;            // select
        g_microcode[0x67] = &mc_bitcount_clz;     // i32.clz
        g_microcode[0x68] = &mc_bitcount_ctz;     // i32.ctz
        g_microcode[0x69] = &mc_bitcount_popcnt;  // i32.popcnt
        g_microcode[0x71] = &mc_bitwise_and;      // i32.and
        g_microcode[0x72] = &mc_bitwise_or;       // i32.or
        g_microcode[0x73] = &mc_bitwise_xor;      // i32.xor
        g_microcode[0x74] = &mc_shift_left;       // i32.shl
        g_microcode[0x75] = &mc_shift_right_signed;   // i32.shr_s
        g_microcode[0x76] = &mc_shift_right_unsigned; // i32.shr_u
        g_microcode[0x77] = &mc_rotate_left;      // i32.rotl
        g_microcode[0x78] = &mc_rotate_right;     // i32.rotr
        g_microcode[0x79] = &mc_bitcount_clz;     // i64.clz
        g_microcode[0x7A] = &mc_bitcount_ctz;     // i64.ctz
        g_microcode[0x7B] = &mc_bitcount_popcnt;  // i64.popcnt
        g_microcode[0x83] = &mc_bitwise_and;      // i64.and
        g_microcode[0x84] = &mc_bitwise_or;       // i64.or
        g_microcode[0x85] = &mc_bitwise_xor;      // i64.xor
        g_microcode[0x86] = &mc_shift_left;       // i64.shl
        g_microcode[0x87] = &mc_shift_right_signed;   // i64.shr_s
        g_microcode[0x88] = &mc_shift_right_unsigned; // i64.shr_u
        g_microcode[0x89] = &mc_rotate_left;      // i64.rotl
        g_microcode[0x8A] = &mc_rotate_right;     // i64.rotr
        g_microcode[0x8B] = &mc_float_abs_f32;    // f32.abs
        g_microcode[0x8C] = &mc_float_neg_f32;    // f32.neg
        g_microcode[0x8D] = &mc_float_ceil_f32;   // f32.ceil
        g_microcode[0x8E] = &mc_float_floor_f32;  // f32.floor
        g_microcode[0x8F] = &mc_float_trunc_f32;  // f32.trunc
        g_microcode[0x90] = &mc_float_nearest_f32; // f32.nearest
        g_microcode[0x91] = &mc_float_sqrt_f32;   // f32.sqrt
        g_microcode[0x46] = &mc_compare_eq;       // i32.eq
        g_microcode[0x47] = &mc_compare_ne;       // i32.ne
        g_microcode[0x48] = &mc_compare_lt;       // i32.lt_s
        g_microcode[0x49] = &mc_compare_lt;       // i32.lt_u
        g_microcode[0x4A] = &mc_compare_gt;       // i32.gt_s
        g_microcode[0x4B] = &mc_compare_gt;       // i32.gt_u
        g_microcode[0x4C] = &mc_compare_le;       // i32.le_s
        g_microcode[0x4D] = &mc_compare_le;       // i32.le_u
        g_microcode[0x4E] = &mc_compare_ge;       // i32.ge_s
        g_microcode[0x4F] = &mc_compare_ge;       // i32.ge_u
        g_microcode[0x51] = &mc_compare_eq;       // i64.eq
        g_microcode[0x52] = &mc_compare_ne;       // i64.ne
        g_microcode[0x53] = &mc_compare_lt;       // i64.lt_s
        g_microcode[0x54] = &mc_compare_lt;       // i64.lt_u
        g_microcode[0x55] = &mc_compare_gt;       // i64.gt_s
        g_microcode[0x56] = &mc_compare_gt;       // i64.gt_u
        g_microcode[0x57] = &mc_compare_le;       // i64.le_s
        g_microcode[0x58] = &mc_compare_le;       // i64.le_u
        g_microcode[0x59] = &mc_compare_ge;       // i64.ge_s
        g_microcode[0x5A] = &mc_compare_ge;       // i64.ge_u
        g_microcode[0x5B] = &mc_compare_eq;       // f32.eq
        g_microcode[0x5C] = &mc_compare_ne;       // f32.ne
        g_microcode[0x5D] = &mc_compare_lt;       // f32.lt
        g_microcode[0x5E] = &mc_compare_gt;       // f32.gt
        g_microcode[0x5F] = &mc_compare_le;       // f32.le
        g_microcode[0x60] = &mc_compare_ge;       // f32.ge
        g_microcode[0x61] = &mc_compare_eq;       // f64.eq
        g_microcode[0x62] = &mc_compare_ne;       // f64.ne
        g_microcode[0x63] = &mc_compare_lt;       // f64.lt
        g_microcode[0x64] = &mc_compare_gt;       // f64.gt
        g_microcode[0x65] = &mc_compare_le;       // f64.le
        g_microcode[0x66] = &mc_compare_ge;       // f64.ge
        g_microcode[0x6A] = &mc_arith_add;        // i32.add
        g_microcode[0x6B] = &mc_arith_sub;        // i32.sub
        g_microcode[0x6C] = &mc_arith_mul;        // i32.mul
        g_microcode[0x6D] = &mc_arith_div;        // i32.div_s
        g_microcode[0x6E] = &mc_arith_div;        // i32.div_u
        g_microcode[0x6F] = &mc_arith_rem;        // i32.rem_s
        g_microcode[0x70] = &mc_arith_rem;        // i32.rem_u
        g_microcode[0x7C] = &mc_arith_add;        // i64.add
        g_microcode[0x7D] = &mc_arith_sub;        // i64.sub
        g_microcode[0x7E] = &mc_arith_mul;        // i64.mul
        g_microcode[0x7F] = &mc_arith_div;        // i64.div_s
        g_microcode[0x80] = &mc_arith_div;        // i64.div_u
        g_microcode[0x81] = &mc_arith_rem;        // i64.rem_s
        g_microcode[0x82] = &mc_arith_rem;        // i64.rem_u
        g_microcode[0x92] = &mc_arith_add;        // f32.add
        g_microcode[0x93] = &mc_arith_sub;        // f32.sub
        g_microcode[0x94] = &mc_arith_mul;        // f32.mul
        g_microcode[0x95] = &mc_arith_div;        // f32.div
        g_microcode[0x96] = &mc_float_min_f32;    // f32.min
        g_microcode[0x97] = &mc_float_max_f32;    // f32.max
        g_microcode[0x98] = &mc_float_copysign_f32; // f32.copysign
        g_microcode[0x99] = &mc_float_abs_f64;    // f64.abs
        g_microcode[0x9A] = &mc_float_neg_f64;    // f64.neg
        g_microcode[0x9B] = &mc_float_ceil_f64;   // f64.ceil
        g_microcode[0x9C] = &mc_float_floor_f64;  // f64.floor
        g_microcode[0x9D] = &mc_float_trunc_f64;  // f64.trunc
        g_microcode[0x9E] = &mc_float_nearest_f64; // f64.nearest
        g_microcode[0x9F] = &mc_float_sqrt_f64;   // f64.sqrt
        g_microcode[0xA0] = &mc_arith_add;        // f64.add
        g_microcode[0xA1] = &mc_arith_sub;        // f64.sub
        g_microcode[0xA2] = &mc_arith_mul;        // f64.mul
        g_microcode[0xA3] = &mc_arith_div;        // f64.div
        g_microcode[0xA4] = &mc_float_min_f64;    // f64.min
        g_microcode[0xA5] = &mc_float_max_f64;    // f64.max
        g_microcode[0xA6] = &mc_float_copysign_f64; // f64.copysign
        g_microcode[0xA7] = &mc_convert_i32_wrap_i64;      // i32.wrap_i64
        g_microcode[0xA8] = &mc_convert_i32_trunc_f32_s;   // i32.trunc_f32_s
        g_microcode[0xA9] = &mc_convert_i32_trunc_f32_u;   // i32.trunc_f32_u
        g_microcode[0xAA] = &mc_convert_i32_trunc_f64_s;   // i32.trunc_f64_s
        g_microcode[0xAB] = &mc_convert_i32_trunc_f64_u;   // i32.trunc_f64_u
        g_microcode[0xAC] = &mc_convert_i64_extend_i32_s;  // i64.extend_i32_s
        g_microcode[0xAD] = &mc_convert_i64_extend_i32_u;  // i64.extend_i32_u
        g_microcode[0xAE] = &mc_convert_i64_trunc_f32_s;   // i64.trunc_f32_s
        g_microcode[0xAF] = &mc_convert_i64_trunc_f32_u;   // i64.trunc_f32_u
        g_microcode[0xB0] = &mc_convert_i64_trunc_f64_s;   // i64.trunc_f64_s
        g_microcode[0xB1] = &mc_convert_i64_trunc_f64_u;   // i64.trunc_f64_u
        g_microcode[0xB2] = &mc_convert_f32_from_i32_s;    // f32.convert_i32_s
        g_microcode[0xB3] = &mc_convert_f32_from_i32_u;    // f32.convert_i32_u
        g_microcode[0xB4] = &mc_convert_f32_from_i64_s;    // f32.convert_i64_s
        g_microcode[0xB5] = &mc_convert_f32_from_i64_u;    // f32.convert_i64_u
        g_microcode[0xB6] = &mc_convert_f32_demote_f64;    // f32.demote_f64
        g_microcode[0xB7] = &mc_convert_f64_from_i32_s;    // f64.convert_i32_s
        g_microcode[0xB8] = &mc_convert_f64_from_i32_u;    // f64.convert_i32_u
        g_microcode[0xB9] = &mc_convert_f64_from_i64_s;    // f64.convert_i64_s
        g_microcode[0xBA] = &mc_convert_f64_from_i64_u;    // f64.convert_i64_u
        g_microcode[0xBB] = &mc_convert_f64_promote_f32;   // f64.promote_f32
        g_microcode[0xBC] = &mc_reinterpret_i32_from_f32;  // i32.reinterpret_f32
        g_microcode[0xBD] = &mc_reinterpret_i64_from_f64;  // i64.reinterpret_f64
        g_microcode[0xBE] = &mc_reinterpret_f32_from_i32;  // f32.reinterpret_i32
        g_microcode[0xBF] = &mc_reinterpret_f64_from_i64;  // f64.reinterpret_i64
        g_microcode[0xC0] = &mc_convert_i32_extend8_s;     // i32.extend8_s
        g_microcode[0xC1] = &mc_convert_i32_extend16_s;    // i32.extend16_s
        g_microcode[0xC2] = &mc_convert_i64_extend8_s;     // i64.extend8_s
        g_microcode[0xC3] = &mc_convert_i64_extend16_s;    // i64.extend16_s
        g_microcode[0xC4] = &mc_convert_i64_extend32_s;    // i64.extend32_s
        fa_ops_numeric_register_microcode(g_microcode);
    }
    g_microcode_initialized = true;
}

static void init_ops_once(void) {
    if (g_ops_initialized) {
        return;
    }
    fa_ops_defs_populate(g_ops);
    init_microcode_once();
    fa_ops_simd_init_dispatch();
    g_ops_initialized = true;
}

const fa_WasmOp* fa_instance_ops(void) {
    init_ops_once();
    return g_ops;
}

const fa_WasmOp* fa_get_op(uint8_t opcode) {
    init_ops_once();
    return &g_ops[opcode];
}

bool fa_ops_microcode_enabled(void) {
    init_ops_once();
    return g_microcode_enabled;
}

bool fa_ops_get_microcode_steps(uint8_t opcode, const Operation** steps_out, uint8_t* step_count_out) {
    init_ops_once();
    if (!g_microcode_enabled) {
        return false;
    }
    const fa_Microcode* microcode = g_microcode[opcode];
    if (!microcode) {
        return false;
    }
    if (steps_out) {
        *steps_out = microcode->steps;
    }
    if (step_count_out) {
        *step_count_out = microcode->step_count;
    }
    return true;
}

/* Executes one prepared microcode sequence, stopping on first trap/error. */
static OP_RETURN_TYPE execute_microcode(const fa_Microcode* microcode,
                                        fa_Runtime* runtime,
                                        fa_Job* job,
                                        const fa_WasmOp* descriptor) {
    if (!microcode || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    for (uint8_t i = 0; i < microcode->step_count; ++i) {
        Operation step = microcode->steps[i];
        if (!step) {
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
        }
        const int status = step(runtime, job, descriptor);
        if (status != FA_RUNTIME_OK) {
            return status;
        }
    }
    return FA_RUNTIME_OK;
}

/*
 * Single opcode dispatch entry point used by runtime/JIT-prepared paths.
 * Microcode is attempted first when enabled and available; otherwise the
 * descriptor handler is executed directly.
 */
OP_RETURN_TYPE fa_execute_op(uint8_t opcode, fa_Runtime* runtime, fa_Job* job) {
    const fa_WasmOp* op = fa_get_op(opcode);
    if (!op || !op->operation) {
        return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
    if (g_microcode_enabled) {
        const fa_Microcode* microcode = g_microcode[opcode];
        if (microcode) {
            return execute_microcode(microcode, runtime, job, op);
        }
    }
    return op->operation(runtime, job, op);
}

/*
 * Defines the canonical 0..255 opcode table.
 * Keep metadata (`num_pull`, `num_push`, `num_args`, type/signedness) and
 * handlers synchronized whenever an opcode implementation changes.
 */
void fa_ops_defs_populate(fa_WasmOp* ops) {
    if (!ops) {
        return;
    }
    memset(ops, 0, sizeof(fa_WasmOp) * 256);

    const fa_WasmType type_void = {wt_void, 0, false};
    const fa_WasmType type_i32 = {wt_integer, 4, true};
    const fa_WasmType type_u32 = {wt_unsigned_integer, 4, false};
    const fa_WasmType type_i64 = {wt_integer, 8, true};
    const fa_WasmType type_u64 = {wt_unsigned_integer, 8, false};
    const fa_WasmType type_f32 = {wt_float, 4, false};
    const fa_WasmType type_f64 = {wt_float, 8, false};

    define_op(ops, 0x00, &type_void, wopt_unique, 0, 0, 0, 0, op_control); // unreachable
    define_op(ops, 0x01, &type_void, wopt_unique, 0, 0, 0, 0, op_control); // nop
    define_op(ops, 0x02, &type_void, wopt_unique, 0, 0, 0, 1, op_control); // block
    define_op(ops, 0x03, &type_void, wopt_unique, 0, 0, 0, 1, op_control); // loop
    define_op(ops, 0x04, &type_void, wopt_unique, 0, 1, 0, 1, op_control); // if
    define_op(ops, 0x05, &type_void, wopt_unique, 0, 0, 0, 0, op_control); // else
    define_op(ops, 0x0B, &type_void, wopt_unique, 0, 0, 0, 0, op_control); // end
    define_op(ops, 0x0C, &type_void, wopt_unique, 0, 0, 0, 1, op_control); // br
    define_op(ops, 0x0D, &type_void, wopt_unique, 0, 1, 0, 1, op_control); // br_if
    define_op(ops, 0x0E, &type_void, wopt_unique, 0, 1, 0, 1, op_control); // br_table
    define_op(ops, 0x0F, &type_void, wopt_return, 0, 0, 0, 0, op_return); // return
    define_op(ops, 0x10, &type_void, wopt_call, 0, 0, 0, 1, op_call); // call
    define_op(ops, 0x11, &type_void, wopt_call, 0, 1, 0, 2, op_call_indirect); // call_indirect
    define_op(ops, 0x1A, &type_void, wopt_drop, 0, 1, 0, 0, op_drop); // drop
    define_op(ops, 0x1B, &type_void, wopt_select, 0, 3, 1, 0, op_select); // select
    define_op(ops, 0x20, &type_void, wopt_unique, 0, 0, 1, 1, op_local); // local.get
    define_op(ops, 0x21, &type_void, wopt_unique, 0, 1, 0, 1, op_local); // local.set
    define_op(ops, 0x22, &type_void, wopt_unique, 0, 1, 1, 1, op_local); // local.tee
    define_op(ops, 0x23, &type_void, wopt_unique, 0, 0, 1, 1, op_global); // global.get
    define_op(ops, 0x24, &type_void, wopt_unique, 0, 1, 0, 1, op_global); // global.set
    define_op(ops, 0xD0, &type_void, wopt_unique, 0, 0, 1, 1, op_ref); // ref.null
    define_op(ops, 0xD1, &type_i32, wopt_unique, 0, 1, 1, 0, op_ref); // ref.is_null
    define_op(ops, 0xD2, &type_void, wopt_unique, 0, 0, 1, 1, op_ref); // ref.func
    define_op(ops, 0x25, &type_void, wopt_unique, 0, 0, 0, 1, op_table); // table.get
    define_op(ops, 0x26, &type_void, wopt_unique, 0, 1, 0, 1, op_table); // table.set
    define_op(ops, 0x28, &type_i32, wopt_load, 32, 1, 1, 2, op_load); // i32.load
    define_op(ops, 0x29, &type_i64, wopt_load, 64, 1, 1, 2, op_load); // i64.load
    define_op(ops, 0x2A, &type_f32, wopt_load, 32, 1, 1, 2, op_load); // f32.load
    define_op(ops, 0x2B, &type_f64, wopt_load, 64, 1, 1, 2, op_load); // f64.load
    define_op(ops, 0x2C, &type_i32, wopt_load, 8, 1, 1, 2, op_load); // i32.load8_s
    define_op(ops, 0x2D, &type_u32, wopt_load, 8, 1, 1, 2, op_load); // i32.load8_u
    define_op(ops, 0x2E, &type_i32, wopt_load, 16, 1, 1, 2, op_load); // i32.load16_s
    define_op(ops, 0x2F, &type_u32, wopt_load, 16, 1, 1, 2, op_load); // i32.load16_u
    define_op(ops, 0x30, &type_i64, wopt_load, 8, 1, 1, 2, op_load); // i64.load8_s
    define_op(ops, 0x31, &type_u64, wopt_load, 8, 1, 1, 2, op_load); // i64.load8_u
    define_op(ops, 0x32, &type_i64, wopt_load, 16, 1, 1, 2, op_load); // i64.load16_s
    define_op(ops, 0x33, &type_u64, wopt_load, 16, 1, 1, 2, op_load); // i64.load16_u
    define_op(ops, 0x34, &type_i64, wopt_load, 32, 1, 1, 2, op_load); // i64.load32_s
    define_op(ops, 0x35, &type_u64, wopt_load, 32, 1, 1, 2, op_load); // i64.load32_u
    define_op(ops, 0x36, &type_i32, wopt_store, 32, 2, 0, 2, op_store); // i32.store
    define_op(ops, 0x37, &type_i64, wopt_store, 64, 2, 0, 2, op_store); // i64.store
    define_op(ops, 0x38, &type_f32, wopt_store, 32, 2, 0, 2, op_store); // f32.store
    define_op(ops, 0x39, &type_f64, wopt_store, 64, 2, 0, 2, op_store); // f64.store
    define_op(ops, 0x3A, &type_i32, wopt_store, 8, 2, 0, 2, op_store); // i32.store8
    define_op(ops, 0x3B, &type_i32, wopt_store, 16, 2, 0, 2, op_store); // i32.store16
    define_op(ops, 0x3C, &type_i64, wopt_store, 8, 2, 0, 2, op_store); // i64.store8
    define_op(ops, 0x3D, &type_i64, wopt_store, 16, 2, 0, 2, op_store); // i64.store16
    define_op(ops, 0x3E, &type_i64, wopt_store, 32, 2, 0, 2, op_store); // i64.store32
    define_op(ops, 0x41, &type_i32, wopt_const, 32, 0, 1, 1, op_const); // i32.const
    define_op(ops, 0x42, &type_i64, wopt_const, 64, 0, 1, 1, op_const); // i64.const
    define_op(ops, 0x43, &type_f32, wopt_const, 32, 0, 1, 1, op_const); // f32.const
    define_op(ops, 0x44, &type_f64, wopt_const, 64, 0, 1, 1, op_const); // f64.const
    define_op(ops, 0x45, &type_i32, wopt_eqz, 0, 1, 1, 0, op_eqz); // i32.eqz
    define_op(ops, 0x50, &type_i64, wopt_eqz, 0, 1, 1, 0, op_eqz); // i64.eqz
    define_op(ops, 0x46, &type_i32, wopt_eq, 0, 2, 1, 0, op_compare_eq_mc); // i32.eq
    define_op(ops, 0x47, &type_i32, wopt_ne, 0, 2, 1, 0, op_compare_ne_mc); // i32.ne
    define_op(ops, 0x48, &type_i32, wopt_lt, 0, 2, 1, 0, op_compare_lt_mc); // i32.lt_s
    define_op(ops, 0x49, &type_u32, wopt_lt, 0, 2, 1, 0, op_compare_lt_mc); // i32.lt_u
    define_op(ops, 0x4A, &type_i32, wopt_gt, 0, 2, 1, 0, op_compare_gt_mc); // i32.gt_s
    define_op(ops, 0x4B, &type_u32, wopt_gt, 0, 2, 1, 0, op_compare_gt_mc); // i32.gt_u
    define_op(ops, 0x4C, &type_i32, wopt_le, 0, 2, 1, 0, op_compare_le_mc); // i32.le_s
    define_op(ops, 0x4D, &type_u32, wopt_le, 0, 2, 1, 0, op_compare_le_mc); // i32.le_u
    define_op(ops, 0x4E, &type_i32, wopt_ge, 0, 2, 1, 0, op_compare_ge_mc); // i32.ge_s
    define_op(ops, 0x4F, &type_u32, wopt_ge, 0, 2, 1, 0, op_compare_ge_mc); // i32.ge_u
    define_op(ops, 0x51, &type_i64, wopt_eq, 0, 2, 1, 0, op_compare_eq_mc); // i64.eq
    define_op(ops, 0x52, &type_i64, wopt_ne, 0, 2, 1, 0, op_compare_ne_mc); // i64.ne
    define_op(ops, 0x53, &type_i64, wopt_lt, 0, 2, 1, 0, op_compare_lt_mc); // i64.lt_s
    define_op(ops, 0x54, &type_u64, wopt_lt, 0, 2, 1, 0, op_compare_lt_mc); // i64.lt_u
    define_op(ops, 0x55, &type_i64, wopt_gt, 0, 2, 1, 0, op_compare_gt_mc); // i64.gt_s
    define_op(ops, 0x56, &type_u64, wopt_gt, 0, 2, 1, 0, op_compare_gt_mc); // i64.gt_u
    define_op(ops, 0x57, &type_i64, wopt_le, 0, 2, 1, 0, op_compare_le_mc); // i64.le_s
    define_op(ops, 0x58, &type_u64, wopt_le, 0, 2, 1, 0, op_compare_le_mc); // i64.le_u
    define_op(ops, 0x59, &type_i64, wopt_ge, 0, 2, 1, 0, op_compare_ge_mc); // i64.ge_s
    define_op(ops, 0x5A, &type_u64, wopt_ge, 0, 2, 1, 0, op_compare_ge_mc); // i64.ge_u
    define_op(ops, 0x5B, &type_f32, wopt_eq, 0, 2, 1, 0, op_compare_eq_mc); // f32.eq
    define_op(ops, 0x5C, &type_f32, wopt_ne, 0, 2, 1, 0, op_compare_ne_mc); // f32.ne
    define_op(ops, 0x5D, &type_f32, wopt_lt, 0, 2, 1, 0, op_compare_lt_mc); // f32.lt
    define_op(ops, 0x5E, &type_f32, wopt_gt, 0, 2, 1, 0, op_compare_gt_mc); // f32.gt
    define_op(ops, 0x5F, &type_f32, wopt_le, 0, 2, 1, 0, op_compare_le_mc); // f32.le
    define_op(ops, 0x60, &type_f32, wopt_ge, 0, 2, 1, 0, op_compare_ge_mc); // f32.ge
    define_op(ops, 0x61, &type_f64, wopt_eq, 0, 2, 1, 0, op_compare_eq_mc); // f64.eq
    define_op(ops, 0x62, &type_f64, wopt_ne, 0, 2, 1, 0, op_compare_ne_mc); // f64.ne
    define_op(ops, 0x63, &type_f64, wopt_lt, 0, 2, 1, 0, op_compare_lt_mc); // f64.lt
    define_op(ops, 0x64, &type_f64, wopt_gt, 0, 2, 1, 0, op_compare_gt_mc); // f64.gt
    define_op(ops, 0x65, &type_f64, wopt_le, 0, 2, 1, 0, op_compare_le_mc); // f64.le
    define_op(ops, 0x66, &type_f64, wopt_ge, 0, 2, 1, 0, op_compare_ge_mc); // f64.ge
    define_op(ops, 0x67, &type_i32, wopt_clz, 0, 1, 1, 0, op_bitcount_clz_mc); // i32.clz
    define_op(ops, 0x68, &type_i32, wopt_ctz, 0, 1, 1, 0, op_bitcount_ctz_mc); // i32.ctz
    define_op(ops, 0x69, &type_i32, wopt_popcnt, 0, 1, 1, 0, op_bitcount_popcnt_mc); // i32.popcnt
    define_op(ops, 0x6A, &type_i32, wopt_add, 0, 2, 1, 0, op_arith_add_mc); // i32.add
    define_op(ops, 0x6B, &type_i32, wopt_sub, 0, 2, 1, 0, op_arith_sub_mc); // i32.sub
    define_op(ops, 0x6C, &type_i32, wopt_mul, 0, 2, 1, 0, op_arith_mul_mc); // i32.mul
    define_op(ops, 0x6D, &type_i32, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // i32.div_s
    define_op(ops, 0x6E, &type_u32, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // i32.div_u
    define_op(ops, 0x6F, &type_i32, wopt_rem, 0, 2, 1, 0, op_arith_rem_mc); // i32.rem_s
    define_op(ops, 0x70, &type_u32, wopt_rem, 0, 2, 1, 0, op_arith_rem_mc); // i32.rem_u
    define_op(ops, 0x71, &type_i32, wopt_and, 0, 2, 1, 0, op_bitwise_and_mc); // i32.and
    define_op(ops, 0x72, &type_i32, wopt_or, 0, 2, 1, 0, op_bitwise_or_mc); // i32.or
    define_op(ops, 0x73, &type_i32, wopt_xor, 0, 2, 1, 0, op_bitwise_xor_mc); // i32.xor
    define_op(ops, 0x74, &type_i32, wopt_shl, 0, 2, 1, 0, op_shift_left_mc); // i32.shl
    define_op(ops, 0x75, &type_i32, wopt_shr, 0, 2, 1, 0, op_shift_right_signed_mc); // i32.shr_s
    define_op(ops, 0x76, &type_u32, wopt_shr, 0, 2, 1, 0, op_shift_right_unsigned_mc); // i32.shr_u
    define_op(ops, 0x77, &type_i32, wopt_rotl, 0, 2, 1, 0, op_rotate_left_mc); // i32.rotl
    define_op(ops, 0x78, &type_i32, wopt_rotr, 0, 2, 1, 0, op_rotate_right_mc); // i32.rotr
    define_op(ops, 0x79, &type_i64, wopt_clz, 0, 1, 1, 0, op_bitcount_clz_mc); // i64.clz
    define_op(ops, 0x7A, &type_i64, wopt_ctz, 0, 1, 1, 0, op_bitcount_ctz_mc); // i64.ctz
    define_op(ops, 0x7B, &type_i64, wopt_popcnt, 0, 1, 1, 0, op_bitcount_popcnt_mc); // i64.popcnt
    define_op(ops, 0x7C, &type_i64, wopt_add, 0, 2, 1, 0, op_arith_add_mc); // i64.add
    define_op(ops, 0x7D, &type_i64, wopt_sub, 0, 2, 1, 0, op_arith_sub_mc); // i64.sub
    define_op(ops, 0x7E, &type_i64, wopt_mul, 0, 2, 1, 0, op_arith_mul_mc); // i64.mul
    define_op(ops, 0x7F, &type_i64, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // i64.div_s
    define_op(ops, 0x80, &type_u64, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // i64.div_u
    define_op(ops, 0x81, &type_i64, wopt_rem, 0, 2, 1, 0, op_arith_rem_mc); // i64.rem_s
    define_op(ops, 0x82, &type_u64, wopt_rem, 0, 2, 1, 0, op_arith_rem_mc); // i64.rem_u
    define_op(ops, 0x83, &type_i64, wopt_and, 0, 2, 1, 0, op_bitwise_and_mc); // i64.and
    define_op(ops, 0x84, &type_i64, wopt_or, 0, 2, 1, 0, op_bitwise_or_mc); // i64.or
    define_op(ops, 0x85, &type_i64, wopt_xor, 0, 2, 1, 0, op_bitwise_xor_mc); // i64.xor
    define_op(ops, 0x86, &type_i64, wopt_shl, 0, 2, 1, 0, op_shift_left_mc); // i64.shl
    define_op(ops, 0x87, &type_i64, wopt_shr, 0, 2, 1, 0, op_shift_right_signed_mc); // i64.shr_s
    define_op(ops, 0x88, &type_u64, wopt_shr, 0, 2, 1, 0, op_shift_right_unsigned_mc); // i64.shr_u
    define_op(ops, 0x89, &type_i64, wopt_rotl, 0, 2, 1, 0, op_rotate_left_mc); // i64.rotl
    define_op(ops, 0x8A, &type_i64, wopt_rotr, 0, 2, 1, 0, op_rotate_right_mc); // i64.rotr
    define_op(ops, 0x8B, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_abs_f32_mc); // f32.abs
    define_op(ops, 0x8C, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_neg_f32_mc); // f32.neg
    define_op(ops, 0x8D, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_ceil_f32_mc); // f32.ceil
    define_op(ops, 0x8E, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_floor_f32_mc); // f32.floor
    define_op(ops, 0x8F, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_trunc_f32_mc); // f32.trunc
    define_op(ops, 0x90, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_nearest_f32_mc); // f32.nearest
    define_op(ops, 0x91, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_sqrt_f32_mc); // f32.sqrt
    define_op(ops, 0x92, &type_f32, wopt_add, 0, 2, 1, 0, op_arith_add_mc); // f32.add
    define_op(ops, 0x93, &type_f32, wopt_sub, 0, 2, 1, 0, op_arith_sub_mc); // f32.sub
    define_op(ops, 0x94, &type_f32, wopt_mul, 0, 2, 1, 0, op_arith_mul_mc); // f32.mul
    define_op(ops, 0x95, &type_f32, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // f32.div
    define_op(ops, 0x96, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_min_f32_mc); // f32.min
    define_op(ops, 0x97, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_max_f32_mc); // f32.max
    define_op(ops, 0x98, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_copysign_f32_mc); // f32.copysign
    define_op(ops, 0x99, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_abs_f64_mc); // f64.abs
    define_op(ops, 0x9A, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_neg_f64_mc); // f64.neg
    define_op(ops, 0x9B, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_ceil_f64_mc); // f64.ceil
    define_op(ops, 0x9C, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_floor_f64_mc); // f64.floor
    define_op(ops, 0x9D, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_trunc_f64_mc); // f64.trunc
    define_op(ops, 0x9E, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_nearest_f64_mc); // f64.nearest
    define_op(ops, 0x9F, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_sqrt_f64_mc); // f64.sqrt
    define_op(ops, 0xA0, &type_f64, wopt_add, 0, 2, 1, 0, op_arith_add_mc); // f64.add
    define_op(ops, 0xA1, &type_f64, wopt_sub, 0, 2, 1, 0, op_arith_sub_mc); // f64.sub
    define_op(ops, 0xA2, &type_f64, wopt_mul, 0, 2, 1, 0, op_arith_mul_mc); // f64.mul
    define_op(ops, 0xA3, &type_f64, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // f64.div
    define_op(ops, 0xA4, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_min_f64_mc); // f64.min
    define_op(ops, 0xA5, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_max_f64_mc); // f64.max
    define_op(ops, 0xA6, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_copysign_f64_mc); // f64.copysign
    define_op(ops, 0xA7, &type_i32, wopt_wrap, 0, 1, 1, 0, op_convert_i32_wrap_i64_mc); // i32.wrap_i64
    define_op(ops, 0xA8, &type_i32, wopt_trunc, 0, 1, 1, 0, op_convert_i32_trunc_f32_s_mc); // i32.trunc_f32_s
    define_op(ops, 0xA9, &type_u32, wopt_trunc, 0, 1, 1, 0, op_convert_i32_trunc_f32_u_mc); // i32.trunc_f32_u
    define_op(ops, 0xAA, &type_i32, wopt_trunc, 0, 1, 1, 0, op_convert_i32_trunc_f64_s_mc); // i32.trunc_f64_s
    define_op(ops, 0xAB, &type_u32, wopt_trunc, 0, 1, 1, 0, op_convert_i32_trunc_f64_u_mc); // i32.trunc_f64_u
    define_op(ops, 0xAC, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert_i64_extend_i32_s_mc); // i64.extend_i32_s
    define_op(ops, 0xAD, &type_u64, wopt_extend, 0, 1, 1, 0, op_convert_i64_extend_i32_u_mc); // i64.extend_i32_u
    define_op(ops, 0xAE, &type_i64, wopt_trunc, 0, 1, 1, 0, op_convert_i64_trunc_f32_s_mc); // i64.trunc_f32_s
    define_op(ops, 0xAF, &type_u64, wopt_trunc, 0, 1, 1, 0, op_convert_i64_trunc_f32_u_mc); // i64.trunc_f32_u
    define_op(ops, 0xB0, &type_i64, wopt_trunc, 0, 1, 1, 0, op_convert_i64_trunc_f64_s_mc); // i64.trunc_f64_s
    define_op(ops, 0xB1, &type_u64, wopt_trunc, 0, 1, 1, 0, op_convert_i64_trunc_f64_u_mc); // i64.trunc_f64_u
    define_op(ops, 0xB2, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert_f32_from_i32_s_mc); // f32.convert_i32_s
    define_op(ops, 0xB3, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert_f32_from_i32_u_mc); // f32.convert_i32_u
    define_op(ops, 0xB4, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert_f32_from_i64_s_mc); // f32.convert_i64_s
    define_op(ops, 0xB5, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert_f32_from_i64_u_mc); // f32.convert_i64_u
    define_op(ops, 0xB6, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert_f32_demote_f64_mc); // f32.demote_f64
    define_op(ops, 0xB7, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert_f64_from_i32_s_mc); // f64.convert_i32_s
    define_op(ops, 0xB8, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert_f64_from_i32_u_mc); // f64.convert_i32_u
    define_op(ops, 0xB9, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert_f64_from_i64_s_mc); // f64.convert_i64_s
    define_op(ops, 0xBA, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert_f64_from_i64_u_mc); // f64.convert_i64_u
    define_op(ops, 0xBB, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert_f64_promote_f32_mc); // f64.promote_f32
    define_op(ops, 0xBC, &type_i32, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret_i32_from_f32_mc); // i32.reinterpret_f32
    define_op(ops, 0xBD, &type_i64, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret_i64_from_f64_mc); // i64.reinterpret_f64
    define_op(ops, 0xBE, &type_f32, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret_f32_from_i32_mc); // f32.reinterpret_i32
    define_op(ops, 0xBF, &type_f64, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret_f64_from_i64_mc); // f64.reinterpret_i64
    define_op(ops, 0xC0, &type_i32, wopt_extend, 0, 1, 1, 0, op_convert_i32_extend8_s_mc); // i32.extend8_s
    define_op(ops, 0xC1, &type_i32, wopt_extend, 0, 1, 1, 0, op_convert_i32_extend16_s_mc); // i32.extend16_s
    define_op(ops, 0xC2, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert_i64_extend8_s_mc); // i64.extend8_s
    define_op(ops, 0xC3, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert_i64_extend16_s_mc); // i64.extend16_s
    define_op(ops, 0xC4, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert_i64_extend32_s_mc); // i64.extend32_s
    define_op(ops, 0x3F, &type_i32, wopt_unique, 0, 0, 1, 1, op_memory_size); // memory.size
    define_op(ops, 0x40, &type_i32, wopt_unique, 0, 1, 1, 1, op_memory_grow); // memory.grow
    define_op(ops, 0xFC, &type_void, wopt_unique, 0, 0, 0, 1, op_bulk_memory); // bulk memory/table prefix
    define_op(ops, 0xFD, &type_void, wopt_unique, 0, 0, 0, 1, op_simd); // simd prefix
}
