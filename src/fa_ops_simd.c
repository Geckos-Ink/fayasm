#include "fa_ops_internal.h"

/* Core and relaxed SIMD families plus the range-built subopcode router. */

/*
 * SIMD helpers reinterpret the same 128-bit payload through lane views.
 * This keeps per-opcode lane math explicit while preserving a single storage type.
 */
typedef union {
    fa_V128 v;
    uint8_t u8[16];
    int8_t i8[16];
    uint16_t u16[8];
    int16_t i16[8];
    uint32_t u32[4];
    int32_t i32[4];
    uint64_t u64[2];
    int64_t i64[2];
    f32 f32[4];
    f64 f64[2];
} fa_V128Lanes;

static void v128_to_lanes(const fa_V128* value, fa_V128Lanes* lanes) {
    if (!value || !lanes) {
        return;
    }
    memcpy(lanes, value, sizeof(*value));
}

static void lanes_to_v128(const fa_V128Lanes* lanes, fa_V128* value) {
    if (!value || !lanes) {
        return;
    }
    memcpy(value, lanes, sizeof(*value));
}

static int push_v128_lanes_checked(fa_Job* job, const fa_V128Lanes* lanes) {
    fa_V128 value = {0};
    lanes_to_v128(lanes, &value);
    return push_v128_checked(job, &value);
}

static bool pop_v128_value(fa_Job* job, fa_V128* out, fa_JobValue* raw_out) {
    if (!job || !out) {
        return false;
    }
    fa_JobValue value = {0};
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return false;
    }
    if (!job_value_to_v128(&value, out)) {
        restore_stack_value(job, &value);
        return false;
    }
    if (raw_out) {
        *raw_out = value;
    }
    return true;
}

static bool pop_two_v128_values(fa_Job* job,
                                fa_V128* lhs,
                                fa_V128* rhs,
                                fa_JobValue* lhs_raw,
                                fa_JobValue* rhs_raw) {
    if (!pop_v128_value(job, rhs, rhs_raw)) {
        return false;
    }
    if (!pop_v128_value(job, lhs, lhs_raw)) {
        if (rhs_raw) {
            restore_stack_value(job, rhs_raw);
        }
        return false;
    }
    return true;
}

static int simd_pop_lane_index(fa_Job* job, uint8_t max_lane, uint8_t* lane_out) {
    if (!lane_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 lane = 0;
    if (pop_reg_u64_checked(job, &lane) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (lane > max_lane) {
        return FA_RUNTIME_ERR_TRAP;
    }
    *lane_out = (uint8_t)lane;
    return FA_RUNTIME_OK;
}

static int simd_pop_memarg(fa_Runtime* runtime, fa_Job* job, u64* offset_out, fa_RuntimeMemory** memory_out) {
    if (!runtime || !job || !offset_out || !memory_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 offset = 0;
    if (pop_reg_u64_checked(job, &offset) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 align = 0;
    if (pop_reg_u64_checked(job, &align) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 mem_index = 0;
    if (runtime->memories_count > 1) {
        if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    fa_RuntimeMemory* memory = NULL;
    int status = runtime_require_memory(runtime, mem_index, &memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    *offset_out = offset;
    *memory_out = memory;
    return FA_RUNTIME_OK;
}

static int simd_resolve_memarg_address(fa_Runtime* runtime, fa_Job* job, u64* addr_out, fa_RuntimeMemory** memory_out) {
    if (!addr_out || !memory_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 offset = 0;
    fa_RuntimeMemory* memory = NULL;
    int status = simd_pop_memarg(runtime, job, &offset, &memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    u64 base = 0;
    if (pop_address_checked_typed(job, &base, memory->is_memory64) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (offset > UINT64_MAX - base) {
        return FA_RUNTIME_ERR_TRAP;
    }
    *addr_out = base + offset;
    *memory_out = memory;
    return FA_RUNTIME_OK;
}

static int simd_load_bytes(fa_RuntimeMemory* memory, u64 addr, void* out, size_t size) {
    if (!memory || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (memory_bounds_check(memory, addr, size) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    memcpy(out, memory->data + (size_t)addr, size);
    return FA_RUNTIME_OK;
}

static int simd_store_bytes(fa_RuntimeMemory* memory, u64 addr, const void* data, size_t size) {
    if (!memory || !data) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (memory_bounds_check(memory, addr, size) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    memcpy(memory->data + (size_t)addr, data, size);
    return FA_RUNTIME_OK;
}

static int8_t simd_saturate_i8(int32_t value) {
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static uint8_t simd_saturate_u8_from_u16(uint16_t value) {
    if (value > UINT8_MAX) {
        return UINT8_MAX;
    }
    return (uint8_t)value;
}

static uint8_t simd_saturate_u8_from_i32(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > UINT8_MAX) {
        return UINT8_MAX;
    }
    return (uint8_t)value;
}

static int16_t simd_saturate_i16(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static uint16_t simd_saturate_u16_from_u32(uint32_t value) {
    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

static uint16_t simd_saturate_u16_from_i32(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

static int32_t simd_trunc_sat_f32_to_i32(f32 value) {
    if (isnan(value)) {
        return 0;
    }
    if (!isfinite(value)) {
        return value < 0.0f ? INT32_MIN : INT32_MAX;
    }
    double truncated = trunc((double)value);
    if (truncated < (double)INT32_MIN) {
        return INT32_MIN;
    }
    if (truncated > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)truncated;
}

static uint32_t simd_trunc_sat_f32_to_u32(f32 value) {
    if (isnan(value)) {
        return 0U;
    }
    if (!isfinite(value)) {
        return value < 0.0f ? 0U : UINT32_MAX;
    }
    double truncated = trunc((double)value);
    if (truncated <= 0.0) {
        return 0U;
    }
    if (truncated > (double)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)truncated;
}

static int32_t simd_trunc_sat_f64_to_i32(f64 value) {
    if (isnan(value)) {
        return 0;
    }
    if (!isfinite(value)) {
        return value < 0.0 ? INT32_MIN : INT32_MAX;
    }
    double truncated = trunc(value);
    if (truncated < (double)INT32_MIN) {
        return INT32_MIN;
    }
    if (truncated > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)truncated;
}

static uint32_t simd_trunc_sat_f64_to_u32(f64 value) {
    if (isnan(value)) {
        return 0U;
    }
    if (!isfinite(value)) {
        return value < 0.0 ? 0U : UINT32_MAX;
    }
    double truncated = trunc(value);
    if (truncated <= 0.0) {
        return 0U;
    }
    if (truncated > (double)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)truncated;
}

static f32 simd_pmin_f32(f32 left, f32 right) {
    if (isnan(left) || isnan(right)) {
        return NAN;
    }
    return fminf(left, right);
}

static f32 simd_pmax_f32(f32 left, f32 right) {
    if (isnan(left) || isnan(right)) {
        return NAN;
    }
    return fmaxf(left, right);
}

static f64 simd_pmin_f64(f64 left, f64 right) {
    if (isnan(left) || isnan(right)) {
        return NAN;
    }
    return fmin(left, right);
}

static f64 simd_pmax_f64(f64 left, f64 right) {
    if (isnan(left) || isnan(right)) {
        return NAN;
    }
    return fmax(left, right);
}

/*
 * SIMD family handlers.
 * Each handler owns a contiguous run of 0xFD subopcodes and is reached through
 * the prebuilt dispatch table at the bottom of this section. The subopcode is
 * passed in so families that share a body (compares, arithmetic, shifts, ...)
 * can still select the exact lane operation.
 * Convention used throughout:
 * - decode immediates from reg window
 * - pop lane/vector operands from stack
 * - push one result or trap
 */
static OP_RETURN_TYPE op_simd_mem(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x00: /* v128.load */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            status = simd_load_bytes(memory, addr, &value, sizeof(value));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            return push_v128_checked(job, &value);
        }
        case 0x01: /* v128.load8x8_s */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint8_t raw[8] = {0};
            status = simd_load_bytes(memory, addr, raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 8; ++i) {
                out.i16[i] = (int16_t)(int8_t)raw[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x02: /* v128.load8x8_u */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint8_t raw[8] = {0};
            status = simd_load_bytes(memory, addr, raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 8; ++i) {
                out.u16[i] = raw[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x03: /* v128.load16x4_s */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint16_t raw[4] = {0};
            status = simd_load_bytes(memory, addr, raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 4; ++i) {
                out.i32[i] = (int32_t)(int16_t)raw[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x04: /* v128.load16x4_u */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint16_t raw[4] = {0};
            status = simd_load_bytes(memory, addr, raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 4; ++i) {
                out.u32[i] = raw[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x05: /* v128.load32x2_s */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t raw[2] = {0};
            status = simd_load_bytes(memory, addr, raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 2; ++i) {
                out.i64[i] = (int64_t)(int32_t)raw[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x06: /* v128.load32x2_u */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t raw[2] = {0};
            status = simd_load_bytes(memory, addr, raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            out.u64[0] = raw[0];
            out.u64[1] = raw[1];
            return push_v128_lanes_checked(job, &out);
        }
        case 0x07: /* v128.load8_splat */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint8_t raw = 0;
            status = simd_load_bytes(memory, addr, &raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            memset(&value, raw, sizeof(value));
            return push_v128_checked(job, &value);
        }
        case 0x08: /* v128.load16_splat */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint16_t raw = 0;
            status = simd_load_bytes(memory, addr, &raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 8; ++i) {
                out.u16[i] = raw;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x09: /* v128.load32_splat */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t raw = 0;
            status = simd_load_bytes(memory, addr, &raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            for (size_t i = 0; i < 4; ++i) {
                out.u32[i] = raw;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x0a: /* v128.load64_splat */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint64_t raw = 0;
            status = simd_load_bytes(memory, addr, &raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            out.u64[0] = raw;
            out.u64[1] = raw;
            return push_v128_lanes_checked(job, &out);
        }
        case 0x0b: /* v128.store */
        {
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            return simd_store_bytes(memory, addr, &value, sizeof(value));
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_build(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x0c: /* v128.const */
        {
            fa_V128 value = {0};
            if (!pop_reg_to_buffer(job, &value, sizeof(value))) {
                return FA_RUNTIME_ERR_TRAP;
            }
            return push_v128_checked(job, &value);
        }
        case 0x0d: /* i8x16.shuffle */
        {
            uint8_t lanes_bytes[16] = {0};
            if (!pop_reg_to_buffer(job, lanes_bytes, sizeof(lanes_bytes))) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 16; ++i) {
                uint8_t lane = lanes_bytes[i];
                if (lane < 16) {
                    out.u8[i] = left.u8[lane];
                } else if (lane < 32) {
                    out.u8[i] = right.u8[lane - 16];
                } else {
                    out.u8[i] = 0;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x0e: /* i8x16.swizzle */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 16; ++i) {
                uint8_t lane = right.u8[i];
                out.u8[i] = lane < 16 ? left.u8[lane] : 0;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x0f: /* i8x16.splat */
        {
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i32) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint8_t lane = (uint8_t)scalar.payload.i32_value;
            fa_V128 value = {0};
            memset(&value, lane, sizeof(value));
            return push_v128_checked(job, &value);
        }
        case 0x10: /* i16x8.splat */
        {
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i32) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint16_t lane = (uint16_t)scalar.payload.i32_value;
            uint16_t lanes[8];
            for (size_t i = 0; i < 8; ++i) {
                lanes[i] = lane;
            }
            fa_V128 value = {0};
            memcpy(&value, lanes, sizeof(lanes));
            return push_v128_checked(job, &value);
        }
        case 0x11: /* i32x4.splat */
        {
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i32) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint32_t lane = (uint32_t)scalar.payload.i32_value;
            uint32_t lanes[4];
            for (size_t i = 0; i < 4; ++i) {
                lanes[i] = lane;
            }
            fa_V128 value = {0};
            memcpy(&value, lanes, sizeof(lanes));
            return push_v128_checked(job, &value);
        }
        case 0x12: /* i64x2.splat */
        {
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i64) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint64_t lane = (uint64_t)scalar.payload.i64_value;
            uint64_t lanes[2];
            lanes[0] = lane;
            lanes[1] = lane;
            fa_V128 value = {0};
            memcpy(&value, lanes, sizeof(lanes));
            return push_v128_checked(job, &value);
        }
        case 0x13: /* f32x4.splat */
        {
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_f32) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const f32 lane = scalar.payload.f32_value;
            f32 lanes[4];
            for (size_t i = 0; i < 4; ++i) {
                lanes[i] = lane;
            }
            fa_V128 value = {0};
            memcpy(&value, lanes, sizeof(lanes));
            return push_v128_checked(job, &value);
        }
        case 0x14: /* f64x2.splat */
        {
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_f64) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const f64 lane = scalar.payload.f64_value;
            f64 lanes[2];
            lanes[0] = lane;
            lanes[1] = lane;
            fa_V128 value = {0};
            memcpy(&value, lanes, sizeof(lanes));
            return push_v128_checked(job, &value);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_lane(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x15: /* i8x16.extract_lane_s */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 15, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            int32_t lane_value = (int32_t)lanes.i8[lane];
            return push_int_checked(job, (u64)lane_value, 32U, true);
        }
        case 0x16: /* i8x16.extract_lane_u */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 15, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            return push_int_checked(job, lanes.u8[lane], 32U, false);
        }
        case 0x17: /* i8x16.replace_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 15, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i32) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u8[lane] = (uint8_t)scalar.payload.i32_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x18: /* i16x8.extract_lane_s */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 7, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            int32_t lane_value = (int32_t)lanes.i16[lane];
            return push_int_checked(job, (u64)lane_value, 32U, true);
        }
        case 0x19: /* i16x8.extract_lane_u */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 7, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            return push_int_checked(job, lanes.u16[lane], 32U, false);
        }
        case 0x1a: /* i16x8.replace_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 7, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i32) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u16[lane] = (uint16_t)scalar.payload.i32_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x1b: /* i32x4.extract_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 3, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            int32_t lane_value = lanes.i32[lane];
            return push_int_checked(job, (u64)lane_value, 32U, true);
        }
        case 0x1c: /* i32x4.replace_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 3, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i32) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u32[lane] = (uint32_t)scalar.payload.i32_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x1d: /* i64x2.extract_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 1, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            int64_t lane_value = lanes.i64[lane];
            return push_int_checked(job, (u64)lane_value, 64U, true);
        }
        case 0x1e: /* i64x2.replace_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 1, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_i64) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u64[lane] = (uint64_t)scalar.payload.i64_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x1f: /* f32x4.extract_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 3, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            return push_float_checked(job, lanes.f32[lane], false);
        }
        case 0x20: /* f32x4.replace_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 3, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_f32) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.f32[lane] = scalar.payload.f32_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x21: /* f64x2.extract_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 1, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            return push_float_checked(job, lanes.f64[lane], true);
        }
        case 0x22: /* f64x2.replace_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 1, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_JobValue scalar = {0};
            if (pop_stack_checked(job, &scalar) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (scalar.kind != fa_job_value_f64) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &scalar);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.f64[lane] = scalar.payload.f64_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_cmp(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x23: /* i8x16.eq */
        case 0x24: /* i8x16.ne */
        case 0x25: /* i8x16.lt_s */
        case 0x26: /* i8x16.lt_u */
        case 0x27: /* i8x16.gt_s */
        case 0x28: /* i8x16.gt_u */
        case 0x29: /* i8x16.le_s */
        case 0x2a: /* i8x16.le_u */
        case 0x2b: /* i8x16.ge_s */
        case 0x2c: /* i8x16.ge_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 16; ++i) {
                bool result = false;
                switch (subopcode) {
                    case 0x23: result = left.u8[i] == right.u8[i]; break;
                    case 0x24: result = left.u8[i] != right.u8[i]; break;
                    case 0x25: result = left.i8[i] < right.i8[i]; break;
                    case 0x26: result = left.u8[i] < right.u8[i]; break;
                    case 0x27: result = left.i8[i] > right.i8[i]; break;
                    case 0x28: result = left.u8[i] > right.u8[i]; break;
                    case 0x29: result = left.i8[i] <= right.i8[i]; break;
                    case 0x2a: result = left.u8[i] <= right.u8[i]; break;
                    case 0x2b: result = left.i8[i] >= right.i8[i]; break;
                    case 0x2c: result = left.u8[i] >= right.u8[i]; break;
                    default: result = false; break;
                }
                out.u8[i] = result ? 0xFFU : 0x00U;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x2d: /* i16x8.eq */
        case 0x2e: /* i16x8.ne */
        case 0x2f: /* i16x8.lt_s */
        case 0x30: /* i16x8.lt_u */
        case 0x31: /* i16x8.gt_s */
        case 0x32: /* i16x8.gt_u */
        case 0x33: /* i16x8.le_s */
        case 0x34: /* i16x8.le_u */
        case 0x35: /* i16x8.ge_s */
        case 0x36: /* i16x8.ge_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 8; ++i) {
                bool result = false;
                switch (subopcode) {
                    case 0x2d: result = left.u16[i] == right.u16[i]; break;
                    case 0x2e: result = left.u16[i] != right.u16[i]; break;
                    case 0x2f: result = left.i16[i] < right.i16[i]; break;
                    case 0x30: result = left.u16[i] < right.u16[i]; break;
                    case 0x31: result = left.i16[i] > right.i16[i]; break;
                    case 0x32: result = left.u16[i] > right.u16[i]; break;
                    case 0x33: result = left.i16[i] <= right.i16[i]; break;
                    case 0x34: result = left.u16[i] <= right.u16[i]; break;
                    case 0x35: result = left.i16[i] >= right.i16[i]; break;
                    case 0x36: result = left.u16[i] >= right.u16[i]; break;
                    default: result = false; break;
                }
                out.u16[i] = result ? 0xFFFFU : 0x0000U;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x37: /* i32x4.eq */
        case 0x38: /* i32x4.ne */
        case 0x39: /* i32x4.lt_s */
        case 0x3a: /* i32x4.lt_u */
        case 0x3b: /* i32x4.gt_s */
        case 0x3c: /* i32x4.gt_u */
        case 0x3d: /* i32x4.le_s */
        case 0x3e: /* i32x4.le_u */
        case 0x3f: /* i32x4.ge_s */
        case 0x40: /* i32x4.ge_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                bool result = false;
                switch (subopcode) {
                    case 0x37: result = left.u32[i] == right.u32[i]; break;
                    case 0x38: result = left.u32[i] != right.u32[i]; break;
                    case 0x39: result = left.i32[i] < right.i32[i]; break;
                    case 0x3a: result = left.u32[i] < right.u32[i]; break;
                    case 0x3b: result = left.i32[i] > right.i32[i]; break;
                    case 0x3c: result = left.u32[i] > right.u32[i]; break;
                    case 0x3d: result = left.i32[i] <= right.i32[i]; break;
                    case 0x3e: result = left.u32[i] <= right.u32[i]; break;
                    case 0x3f: result = left.i32[i] >= right.i32[i]; break;
                    case 0x40: result = left.u32[i] >= right.u32[i]; break;
                    default: result = false; break;
                }
                out.u32[i] = result ? 0xFFFFFFFFU : 0x00000000U;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x41: /* f32x4.eq */
        case 0x42: /* f32x4.ne */
        case 0x43: /* f32x4.lt */
        case 0x44: /* f32x4.gt */
        case 0x45: /* f32x4.le */
        case 0x46: /* f32x4.ge */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                bool result = false;
                const f32 a = left.f32[i];
                const f32 b = right.f32[i];
                const bool a_nan = isnan(a);
                const bool b_nan = isnan(b);
                switch (subopcode) {
                    case 0x41: result = (!a_nan && !b_nan && a == b); break;
                    case 0x42: result = (a_nan || b_nan || a != b); break;
                    case 0x43: result = (!a_nan && !b_nan && a < b); break;
                    case 0x44: result = (!a_nan && !b_nan && a > b); break;
                    case 0x45: result = (!a_nan && !b_nan && a <= b); break;
                    case 0x46: result = (!a_nan && !b_nan && a >= b); break;
                    default: result = false; break;
                }
                out.u32[i] = result ? 0xFFFFFFFFU : 0x00000000U;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x47: /* f64x2.eq */
        case 0x48: /* f64x2.ne */
        case 0x49: /* f64x2.lt */
        case 0x4a: /* f64x2.gt */
        case 0x4b: /* f64x2.le */
        case 0x4c: /* f64x2.ge */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 2; ++i) {
                bool result = false;
                const f64 a = left.f64[i];
                const f64 b = right.f64[i];
                const bool a_nan = isnan(a);
                const bool b_nan = isnan(b);
                switch (subopcode) {
                    case 0x47: result = (!a_nan && !b_nan && a == b); break;
                    case 0x48: result = (a_nan || b_nan || a != b); break;
                    case 0x49: result = (!a_nan && !b_nan && a < b); break;
                    case 0x4a: result = (!a_nan && !b_nan && a > b); break;
                    case 0x4b: result = (!a_nan && !b_nan && a <= b); break;
                    case 0x4c: result = (!a_nan && !b_nan && a >= b); break;
                    default: result = false; break;
                }
                out.u64[i] = result ? 0xFFFFFFFFFFFFFFFFULL : 0x0000000000000000ULL;
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_bitwise(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x4d: /* v128.not */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            value.low = ~value.low;
            value.high = ~value.high;
            return push_v128_checked(job, &value);
        }
        case 0x4e: /* v128.and */
        case 0x4f: /* v128.andnot */
        case 0x50: /* v128.or */
        case 0x51: /* v128.xor */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 out = {0};
            switch (subopcode) {
                case 0x4e:
                    out.low = lhs.low & rhs.low;
                    out.high = lhs.high & rhs.high;
                    break;
                case 0x4f:
                    out.low = lhs.low & ~rhs.low;
                    out.high = lhs.high & ~rhs.high;
                    break;
                case 0x50:
                    out.low = lhs.low | rhs.low;
                    out.high = lhs.high | rhs.high;
                    break;
                case 0x51:
                    out.low = lhs.low ^ rhs.low;
                    out.high = lhs.high ^ rhs.high;
                    break;
                default:
                    return FA_RUNTIME_ERR_TRAP;
            }
            return push_v128_checked(job, &out);
        }
        case 0x52: /* v128.bitselect */
        {
            fa_V128 mask = {0};
            fa_V128 rhs = {0};
            fa_V128 lhs = {0};
            fa_JobValue mask_raw = {0};
            fa_JobValue rhs_raw = {0};
            fa_JobValue lhs_raw = {0};
            if (!pop_v128_value(job, &mask, &mask_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &rhs, &rhs_raw)) {
                restore_stack_value(job, &mask_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &lhs, &lhs_raw)) {
                restore_stack_value(job, &rhs_raw);
                restore_stack_value(job, &mask_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 out = {0};
            out.low = (lhs.low & mask.low) | (rhs.low & ~mask.low);
            out.high = (lhs.high & mask.high) | (rhs.high & ~mask.high);
            return push_v128_checked(job, &out);
        }
        case 0x53: /* v128.any_true */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            return push_bool_checked(job, (value.low != 0 || value.high != 0));
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_memlane(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x54: /* v128.load8_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 15, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            uint8_t byte = 0;
            status = simd_load_bytes(memory, addr, &byte, sizeof(byte));
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u8[lane] = byte;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x55: /* v128.load16_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 7, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            uint16_t lane_value = 0;
            status = simd_load_bytes(memory, addr, &lane_value, sizeof(lane_value));
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u16[lane] = lane_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x56: /* v128.load32_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 3, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            uint32_t lane_value = 0;
            status = simd_load_bytes(memory, addr, &lane_value, sizeof(lane_value));
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u32[lane] = lane_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x57: /* v128.load64_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 1, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            uint64_t lane_value = 0;
            status = simd_load_bytes(memory, addr, &lane_value, sizeof(lane_value));
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            lanes.u64[lane] = lane_value;
            return push_v128_lanes_checked(job, &lanes);
        }
        case 0x58: /* v128.store8_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 15, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            const uint8_t lane_value = lanes.u8[lane];
            return simd_store_bytes(memory, addr, &lane_value, sizeof(lane_value));
        }
        case 0x59: /* v128.store16_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 7, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            const uint16_t lane_value = lanes.u16[lane];
            return simd_store_bytes(memory, addr, &lane_value, sizeof(lane_value));
        }
        case 0x5a: /* v128.store32_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 3, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            const uint32_t lane_value = lanes.u32[lane];
            return simd_store_bytes(memory, addr, &lane_value, sizeof(lane_value));
        }
        case 0x5b: /* v128.store64_lane */
        {
            uint8_t lane = 0;
            int status = simd_pop_lane_index(job, 1, &lane);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                restore_stack_value(job, &value_raw);
                return status;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            const uint64_t lane_value = lanes.u64[lane];
            return simd_store_bytes(memory, addr, &lane_value, sizeof(lane_value));
        }
        case 0x5c: /* v128.load32_zero */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t raw = 0;
            status = simd_load_bytes(memory, addr, &raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            out.u32[0] = raw;
            return push_v128_lanes_checked(job, &out);
        }
        case 0x5d: /* v128.load64_zero */
        {
            u64 addr = 0;
            fa_RuntimeMemory* memory = NULL;
            int status = simd_resolve_memarg_address(runtime, job, &addr, &memory);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint64_t raw = 0;
            status = simd_load_bytes(memory, addr, &raw, sizeof(raw));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            fa_V128Lanes out = {0};
            out.u64[0] = raw;
            return push_v128_lanes_checked(job, &out);
        }
        case 0x5e: /* f32x4.demote_f64x2_zero */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            fa_V128Lanes out = {0};
            out.f32[0] = (f32)lanes.f64[0];
            out.f32[1] = (f32)lanes.f64[1];
            out.f32[2] = 0.0f;
            out.f32[3] = 0.0f;
            return push_v128_lanes_checked(job, &out);
        }
        case 0x5f: /* f64x2.promote_low_f32x4 */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            fa_V128Lanes out = {0};
            out.f64[0] = (f64)lanes.f32[0];
            out.f64[1] = (f64)lanes.f32[1];
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_i8x16(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x60: /* i8x16.abs */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 16; ++i) {
                uint8_t raw = (uint8_t)lanes.i8[i];
                if (lanes.i8[i] < 0) {
                    raw = (uint8_t)(0U - raw);
                }
                out.i8[i] = (int8_t)raw;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x61: /* i8x16.neg */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 16; ++i) {
                out.u8[i] = (uint8_t)(0U - lanes.u8[i]);
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x62: /* i8x16.popcnt */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 16; ++i) {
                out.u8[i] = (uint8_t)popcnt32((uint32_t)lanes.u8[i]);
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x63: /* i8x16.all_true */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            bool all_true = true;
            for (size_t i = 0; i < 16; ++i) {
                if (lanes.u8[i] == 0) {
                    all_true = false;
                    break;
                }
            }
            return push_bool_checked(job, all_true);
        }
        case 0x64: /* i8x16.bitmask */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            uint32_t mask = 0;
            for (size_t i = 0; i < 16; ++i) {
                if (lanes.u8[i] & 0x80U) {
                    mask |= (1U << i);
                }
            }
            return push_int_checked(job, mask, 32U, false);
        }
        case 0x65: /* i8x16.narrow_i16x8_s */
        case 0x66: /* i8x16.narrow_i16x8_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 8; ++i) {
                if (subopcode == 0x65) {
                    out.i8[i] = simd_saturate_i8((int32_t)left.i16[i]);
                    out.i8[i + 8] = simd_saturate_i8((int32_t)right.i16[i]);
                } else {
                    out.u8[i] = simd_saturate_u8_from_u16(left.u16[i]);
                    out.u8[i + 8] = simd_saturate_u8_from_u16(right.u16[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x67: /* i8x16.shl */
        case 0x68: /* i8x16.shr_s */
        case 0x69: /* i8x16.shr_u */
        {
            fa_JobValue shift = {0};
            if (pop_stack_checked(job, &shift) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (shift.kind != fa_job_value_i32) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint8_t amount = (uint8_t)shift.payload.i32_value & 7U;
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 16; ++i) {
                if (subopcode == 0x67) {
                    out.u8[i] = (uint8_t)(lanes.u8[i] << amount);
                } else if (subopcode == 0x68) {
                    out.i8[i] = (int8_t)(lanes.i8[i] >> amount);
                } else {
                    out.u8[i] = (uint8_t)(lanes.u8[i] >> amount);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x6a: /* i8x16.add */
        case 0x6b: /* i8x16.add_sat_s */
        case 0x6c: /* i8x16.add_sat_u */
        case 0x6d: /* i8x16.sub */
        case 0x6e: /* i8x16.sub_sat_s */
        case 0x6f: /* i8x16.sub_sat_u */
        case 0x70: /* i8x16.min_s */
        case 0x71: /* i8x16.min_u */
        case 0x72: /* i8x16.max_s */
        case 0x73: /* i8x16.max_u */
        case 0x74: /* i8x16.avgr_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 16; ++i) {
                switch (subopcode) {
                    case 0x6a:
                        out.u8[i] = (uint8_t)(left.u8[i] + right.u8[i]);
                        break;
                    case 0x6b:
                        out.i8[i] = simd_saturate_i8((int32_t)left.i8[i] + (int32_t)right.i8[i]);
                        break;
                    case 0x6c:
                        out.u8[i] = simd_saturate_u8_from_u16((uint16_t)left.u8[i] + (uint16_t)right.u8[i]);
                        break;
                    case 0x6d:
                        out.u8[i] = (uint8_t)(left.u8[i] - right.u8[i]);
                        break;
                    case 0x6e:
                        out.i8[i] = simd_saturate_i8((int32_t)left.i8[i] - (int32_t)right.i8[i]);
                        break;
                    case 0x6f:
                        out.u8[i] = simd_saturate_u8_from_i32((int32_t)left.u8[i] - (int32_t)right.u8[i]);
                        break;
                    case 0x70:
                        out.i8[i] = (left.i8[i] < right.i8[i]) ? left.i8[i] : right.i8[i];
                        break;
                    case 0x71:
                        out.u8[i] = (left.u8[i] < right.u8[i]) ? left.u8[i] : right.u8[i];
                        break;
                    case 0x72:
                        out.i8[i] = (left.i8[i] > right.i8[i]) ? left.i8[i] : right.i8[i];
                        break;
                    case 0x73:
                        out.u8[i] = (left.u8[i] > right.u8[i]) ? left.u8[i] : right.u8[i];
                        break;
                    case 0x74:
                        out.u8[i] = (uint8_t)(((uint16_t)left.u8[i] + (uint16_t)right.u8[i] + 1U) >> 1);
                        break;
                    default:
                        out.u8[i] = 0;
                        break;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x75: /* i16x8.extadd_pairwise_i8x16_s */
        case 0x76: /* i16x8.extadd_pairwise_i8x16_u */
        case 0x77: /* i32x4.extadd_pairwise_i16x8_s */
        case 0x78: /* i32x4.extadd_pairwise_i16x8_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            if (subopcode == 0x75) {
                for (size_t i = 0; i < 8; ++i) {
                    int32_t sum = (int32_t)lanes.i8[i * 2] + (int32_t)lanes.i8[i * 2 + 1];
                    out.i16[i] = (int16_t)sum;
                }
            } else if (subopcode == 0x76) {
                for (size_t i = 0; i < 8; ++i) {
                    uint16_t sum = (uint16_t)lanes.u8[i * 2] + (uint16_t)lanes.u8[i * 2 + 1];
                    out.u16[i] = sum;
                }
            } else if (subopcode == 0x77) {
                for (size_t i = 0; i < 4; ++i) {
                    int32_t sum = (int32_t)lanes.i16[i * 2] + (int32_t)lanes.i16[i * 2 + 1];
                    out.i32[i] = sum;
                }
            } else {
                for (size_t i = 0; i < 4; ++i) {
                    uint32_t sum = (uint32_t)lanes.u16[i * 2] + (uint32_t)lanes.u16[i * 2 + 1];
                    out.u32[i] = sum;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_i16x8(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x79: /* i16x8.abs */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 8; ++i) {
                uint16_t raw = (uint16_t)lanes.i16[i];
                if (lanes.i16[i] < 0) {
                    raw = (uint16_t)(0U - raw);
                }
                out.i16[i] = (int16_t)raw;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x7a: /* i16x8.neg */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 8; ++i) {
                out.u16[i] = (uint16_t)(0U - lanes.u16[i]);
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x7b: /* i16x8.q15mulr_sat_s */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 8; ++i) {
                int32_t prod = (int32_t)left.i16[i] * (int32_t)right.i16[i];
                int32_t rounded = (prod + 0x4000) >> 15;
                if (rounded > INT16_MAX) {
                    out.i16[i] = INT16_MAX;
                } else if (rounded < INT16_MIN) {
                    out.i16[i] = INT16_MIN;
                } else {
                    out.i16[i] = (int16_t)rounded;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x7c: /* i16x8.all_true */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            bool all_true = true;
            for (size_t i = 0; i < 8; ++i) {
                if (lanes.u16[i] == 0) {
                    all_true = false;
                    break;
                }
            }
            return push_bool_checked(job, all_true);
        }
        case 0x7d: /* i16x8.bitmask */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            uint32_t mask = 0;
            for (size_t i = 0; i < 8; ++i) {
                if (lanes.u16[i] & 0x8000U) {
                    mask |= (1U << i);
                }
            }
            return push_int_checked(job, mask, 32U, false);
        }
        case 0x7e: /* i16x8.narrow_i32x4_s */
        case 0x7f: /* i16x8.narrow_i32x4_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0x7e) {
                    out.i16[i] = simd_saturate_i16(left.i32[i]);
                    out.i16[i + 4] = simd_saturate_i16(right.i32[i]);
                } else {
                    out.u16[i] = simd_saturate_u16_from_u32(left.u32[i]);
                    out.u16[i + 4] = simd_saturate_u16_from_u32(right.u32[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x80: /* i16x8.extend_low_i8x16_s */
        case 0x81: /* i16x8.extend_high_i8x16_s */
        case 0x82: /* i16x8.extend_low_i8x16_u */
        case 0x83: /* i16x8.extend_high_i8x16_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            const size_t base = (subopcode == 0x81 || subopcode == 0x83) ? 8 : 0;
            for (size_t i = 0; i < 8; ++i) {
                if (subopcode == 0x80 || subopcode == 0x81) {
                    out.i16[i] = (int16_t)lanes.i8[base + i];
                } else {
                    out.u16[i] = (uint16_t)lanes.u8[base + i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x84: /* i16x8.shl */
        case 0x85: /* i16x8.shr_s */
        case 0x86: /* i16x8.shr_u */
        {
            fa_JobValue shift = {0};
            if (pop_stack_checked(job, &shift) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (shift.kind != fa_job_value_i32) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint8_t amount = (uint8_t)shift.payload.i32_value & 15U;
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 8; ++i) {
                if (subopcode == 0x84) {
                    out.u16[i] = (uint16_t)(lanes.u16[i] << amount);
                } else if (subopcode == 0x85) {
                    out.i16[i] = (int16_t)(lanes.i16[i] >> amount);
                } else {
                    out.u16[i] = (uint16_t)(lanes.u16[i] >> amount);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x87: /* i16x8.add */
        case 0x88: /* i16x8.add_sat_s */
        case 0x89: /* i16x8.add_sat_u */
        case 0x8a: /* i16x8.sub */
        case 0x8b: /* i16x8.sub_sat_s */
        case 0x8c: /* i16x8.sub_sat_u */
        case 0x8d: /* i16x8.mul */
        case 0x8e: /* i16x8.min_s */
        case 0x8f: /* i16x8.min_u */
        case 0x90: /* i16x8.max_s */
        case 0x91: /* i16x8.max_u */
        case 0x92: /* i16x8.avgr_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 8; ++i) {
                switch (subopcode) {
                    case 0x87:
                        out.u16[i] = (uint16_t)(left.u16[i] + right.u16[i]);
                        break;
                    case 0x88:
                        out.i16[i] = simd_saturate_i16((int32_t)left.i16[i] + (int32_t)right.i16[i]);
                        break;
                    case 0x89:
                        out.u16[i] = simd_saturate_u16_from_u32((uint32_t)left.u16[i] + (uint32_t)right.u16[i]);
                        break;
                    case 0x8a:
                        out.u16[i] = (uint16_t)(left.u16[i] - right.u16[i]);
                        break;
                    case 0x8b:
                        out.i16[i] = simd_saturate_i16((int32_t)left.i16[i] - (int32_t)right.i16[i]);
                        break;
                    case 0x8c:
                        out.u16[i] = simd_saturate_u16_from_i32((int32_t)left.u16[i] - (int32_t)right.u16[i]);
                        break;
                    case 0x8d:
                        out.u16[i] = (uint16_t)(left.u16[i] * right.u16[i]);
                        break;
                    case 0x8e:
                        out.i16[i] = (left.i16[i] < right.i16[i]) ? left.i16[i] : right.i16[i];
                        break;
                    case 0x8f:
                        out.u16[i] = (left.u16[i] < right.u16[i]) ? left.u16[i] : right.u16[i];
                        break;
                    case 0x90:
                        out.i16[i] = (left.i16[i] > right.i16[i]) ? left.i16[i] : right.i16[i];
                        break;
                    case 0x91:
                        out.u16[i] = (left.u16[i] > right.u16[i]) ? left.u16[i] : right.u16[i];
                        break;
                    case 0x92:
                        out.u16[i] = (uint16_t)(((uint32_t)left.u16[i] + (uint32_t)right.u16[i] + 1U) >> 1);
                        break;
                    default:
                        out.u16[i] = 0;
                        break;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x93: /* i16x8.extmul_low_i8x16_s */
        case 0x94: /* i16x8.extmul_high_i8x16_s */
        case 0x95: /* i16x8.extmul_low_i8x16_u */
        case 0x96: /* i16x8.extmul_high_i8x16_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            const size_t base = (subopcode == 0x94 || subopcode == 0x96) ? 8 : 0;
            for (size_t i = 0; i < 8; ++i) {
                if (subopcode == 0x93 || subopcode == 0x94) {
                    out.i16[i] = (int16_t)((int32_t)left.i8[base + i] * (int32_t)right.i8[base + i]);
                } else {
                    out.u16[i] = (uint16_t)((uint32_t)left.u8[base + i] * (uint32_t)right.u8[base + i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_i32x4(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x97: /* i32x4.abs */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                uint32_t raw = (uint32_t)lanes.i32[i];
                if (lanes.i32[i] < 0) {
                    raw = 0U - raw;
                }
                out.i32[i] = (int32_t)raw;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x98: /* i32x4.neg */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                out.u32[i] = 0U - lanes.u32[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x99: /* i32x4.all_true */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            bool all_true = true;
            for (size_t i = 0; i < 4; ++i) {
                if (lanes.u32[i] == 0) {
                    all_true = false;
                    break;
                }
            }
            return push_bool_checked(job, all_true);
        }
        case 0x9a: /* i32x4.bitmask */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            uint32_t mask = 0;
            for (size_t i = 0; i < 4; ++i) {
                if (lanes.u32[i] & 0x80000000U) {
                    mask |= (1U << i);
                }
            }
            return push_int_checked(job, mask, 32U, false);
        }
        case 0x9b: /* i32x4.extend_low_i16x8_s */
        case 0x9c: /* i32x4.extend_high_i16x8_s */
        case 0x9d: /* i32x4.extend_low_i16x8_u */
        case 0x9e: /* i32x4.extend_high_i16x8_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            const size_t base = (subopcode == 0x9c || subopcode == 0x9e) ? 4 : 0;
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0x9b || subopcode == 0x9c) {
                    out.i32[i] = (int32_t)lanes.i16[base + i];
                } else {
                    out.u32[i] = (uint32_t)lanes.u16[base + i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x9f: /* i32x4.shl */
        case 0xa0: /* i32x4.shr_s */
        case 0xa1: /* i32x4.shr_u */
        {
            fa_JobValue shift = {0};
            if (pop_stack_checked(job, &shift) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (shift.kind != fa_job_value_i32) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint8_t amount = (uint8_t)shift.payload.i32_value & 31U;
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0x9f) {
                    out.u32[i] = lanes.u32[i] << amount;
                } else if (subopcode == 0xa0) {
                    out.i32[i] = lanes.i32[i] >> amount;
                } else {
                    out.u32[i] = lanes.u32[i] >> amount;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xa2: /* i32x4.add */
        case 0xa3: /* i32x4.sub */
        case 0xa4: /* i32x4.mul */
        case 0xa5: /* i32x4.min_s */
        case 0xa6: /* i32x4.min_u */
        case 0xa7: /* i32x4.max_s */
        case 0xa8: /* i32x4.max_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                switch (subopcode) {
                    case 0xa2:
                        out.u32[i] = left.u32[i] + right.u32[i];
                        break;
                    case 0xa3:
                        out.u32[i] = left.u32[i] - right.u32[i];
                        break;
                    case 0xa4:
                        out.u32[i] = left.u32[i] * right.u32[i];
                        break;
                    case 0xa5:
                        out.i32[i] = (left.i32[i] < right.i32[i]) ? left.i32[i] : right.i32[i];
                        break;
                    case 0xa6:
                        out.u32[i] = (left.u32[i] < right.u32[i]) ? left.u32[i] : right.u32[i];
                        break;
                    case 0xa7:
                        out.i32[i] = (left.i32[i] > right.i32[i]) ? left.i32[i] : right.i32[i];
                        break;
                    case 0xa8:
                        out.u32[i] = (left.u32[i] > right.u32[i]) ? left.u32[i] : right.u32[i];
                        break;
                    default:
                        out.u32[i] = 0;
                        break;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xa9: /* i32x4.dot_i16x8_s */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                int64_t sum = (int64_t)left.i16[i * 2] * (int64_t)right.i16[i * 2];
                sum += (int64_t)left.i16[i * 2 + 1] * (int64_t)right.i16[i * 2 + 1];
                out.i32[i] = (int32_t)sum;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xaa: /* i32x4.extmul_low_i16x8_s */
        case 0xab: /* i32x4.extmul_high_i16x8_s */
        case 0xac: /* i32x4.extmul_low_i16x8_u */
        case 0xad: /* i32x4.extmul_high_i16x8_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            const size_t base = (subopcode == 0xab || subopcode == 0xad) ? 4 : 0;
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0xaa || subopcode == 0xab) {
                    out.i32[i] = (int32_t)left.i16[base + i] * (int32_t)right.i16[base + i];
                } else {
                    out.u32[i] = (uint32_t)left.u16[base + i] * (uint32_t)right.u16[base + i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_i64x2(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0xae: /* i64x2.abs */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 2; ++i) {
                uint64_t raw = (uint64_t)lanes.i64[i];
                if (lanes.i64[i] < 0) {
                    raw = 0ULL - raw;
                }
                out.i64[i] = (int64_t)raw;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xaf: /* i64x2.neg */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 2; ++i) {
                out.u64[i] = 0ULL - lanes.u64[i];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xb0: /* i64x2.all_true */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            bool all_true = (lanes.u64[0] != 0 && lanes.u64[1] != 0);
            return push_bool_checked(job, all_true);
        }
        case 0xb1: /* i64x2.bitmask */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            v128_to_lanes(&value, &lanes);
            uint32_t mask = 0;
            for (size_t i = 0; i < 2; ++i) {
                if (lanes.u64[i] & 0x8000000000000000ULL) {
                    mask |= (1U << i);
                }
            }
            return push_int_checked(job, mask, 32U, false);
        }
        case 0xb2: /* i64x2.extend_low_i32x4_s */
        case 0xb3: /* i64x2.extend_high_i32x4_s */
        case 0xb4: /* i64x2.extend_low_i32x4_u */
        case 0xb5: /* i64x2.extend_high_i32x4_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            const size_t base = (subopcode == 0xb3 || subopcode == 0xb5) ? 2 : 0;
            for (size_t i = 0; i < 2; ++i) {
                if (subopcode == 0xb2 || subopcode == 0xb3) {
                    out.i64[i] = (int64_t)lanes.i32[base + i];
                } else {
                    out.u64[i] = (uint64_t)lanes.u32[base + i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xb6: /* i64x2.shl */
        case 0xb7: /* i64x2.shr_s */
        case 0xb8: /* i64x2.shr_u */
        {
            fa_JobValue shift = {0};
            if (pop_stack_checked(job, &shift) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (shift.kind != fa_job_value_i32) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128 value = {0};
            fa_JobValue value_raw = {0};
            if (!pop_v128_value(job, &value, &value_raw)) {
                restore_stack_value(job, &shift);
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint8_t amount = (uint8_t)shift.payload.i32_value & 63U;
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 2; ++i) {
                if (subopcode == 0xb6) {
                    out.u64[i] = lanes.u64[i] << amount;
                } else if (subopcode == 0xb7) {
                    out.i64[i] = lanes.i64[i] >> amount;
                } else {
                    out.u64[i] = lanes.u64[i] >> amount;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xb9: /* i64x2.add */
        case 0xba: /* i64x2.sub */
        case 0xbb: /* i64x2.mul */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 2; ++i) {
                switch (subopcode) {
                    case 0xb9:
                        out.u64[i] = left.u64[i] + right.u64[i];
                        break;
                    case 0xba:
                        out.u64[i] = left.u64[i] - right.u64[i];
                        break;
                    case 0xbb:
                        out.u64[i] = left.u64[i] * right.u64[i];
                        break;
                    default:
                        out.u64[i] = 0ULL;
                        break;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xbc: /* i64x2.eq */
        case 0xbd: /* i64x2.ne */
        case 0xbe: /* i64x2.lt_s */
        case 0xbf: /* i64x2.gt_s */
        case 0xc0: /* i64x2.le_s */
        case 0xc1: /* i64x2.ge_s */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 2; ++i) {
                bool result = false;
                switch (subopcode) {
                    case 0xbc: result = left.i64[i] == right.i64[i]; break;
                    case 0xbd: result = left.i64[i] != right.i64[i]; break;
                    case 0xbe: result = left.i64[i] < right.i64[i]; break;
                    case 0xbf: result = left.i64[i] > right.i64[i]; break;
                    case 0xc0: result = left.i64[i] <= right.i64[i]; break;
                    case 0xc1: result = left.i64[i] >= right.i64[i]; break;
                    default: result = false; break;
                }
                out.u64[i] = result ? UINT64_MAX : 0ULL;
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xc2: /* i64x2.extmul_low_i32x4_s */
        case 0xc3: /* i64x2.extmul_high_i32x4_s */
        case 0xc4: /* i64x2.extmul_low_i32x4_u */
        case 0xc5: /* i64x2.extmul_high_i32x4_u */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            const size_t base = (subopcode == 0xc3 || subopcode == 0xc5) ? 2 : 0;
            for (size_t i = 0; i < 2; ++i) {
                if (subopcode == 0xc2 || subopcode == 0xc3) {
                    out.i64[i] = (int64_t)left.i32[base + i] * (int64_t)right.i32[base + i];
                } else {
                    out.u64[i] = (uint64_t)left.u32[base + i] * (uint64_t)right.u32[base + i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_f32x4(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0xc6: /* f32x4.abs */
        case 0xc7: /* f32x4.neg */
        case 0xc8: /* f32x4.sqrt */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0xc6) {
                    out.f32[i] = fabsf(lanes.f32[i]);
                } else if (subopcode == 0xc7) {
                    out.f32[i] = -lanes.f32[i];
                } else {
                    out.f32[i] = sqrtf(lanes.f32[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xc9: /* f32x4.add */
        case 0xca: /* f32x4.sub */
        case 0xcb: /* f32x4.mul */
        case 0xcc: /* f32x4.div */
        case 0xcd: /* f32x4.min */
        case 0xce: /* f32x4.max */
        case 0xcf: /* f32x4.pmin */
        case 0xd0: /* f32x4.pmax */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                switch (subopcode) {
                    case 0xc9:
                        out.f32[i] = left.f32[i] + right.f32[i];
                        break;
                    case 0xca:
                        out.f32[i] = left.f32[i] - right.f32[i];
                        break;
                    case 0xcb:
                        out.f32[i] = left.f32[i] * right.f32[i];
                        break;
                    case 0xcc:
                        out.f32[i] = left.f32[i] / right.f32[i];
                        break;
                    case 0xcd:
                        out.f32[i] = fminf(left.f32[i], right.f32[i]);
                        break;
                    case 0xce:
                        out.f32[i] = fmaxf(left.f32[i], right.f32[i]);
                        break;
                    case 0xcf:
                        out.f32[i] = simd_pmin_f32(left.f32[i], right.f32[i]);
                        break;
                    case 0xd0:
                        out.f32[i] = simd_pmax_f32(left.f32[i], right.f32[i]);
                        break;
                    default:
                        out.f32[i] = 0.0f;
                        break;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_f64x2(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0xd1: /* f64x2.abs */
        case 0xd2: /* f64x2.neg */
        case 0xd3: /* f64x2.sqrt */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 2; ++i) {
                if (subopcode == 0xd1) {
                    out.f64[i] = fabs(lanes.f64[i]);
                } else if (subopcode == 0xd2) {
                    out.f64[i] = -lanes.f64[i];
                } else {
                    out.f64[i] = sqrt(lanes.f64[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xd4: /* f64x2.add */
        case 0xd5: /* f64x2.sub */
        case 0xd6: /* f64x2.mul */
        case 0xd7: /* f64x2.div */
        case 0xd8: /* f64x2.min */
        case 0xd9: /* f64x2.max */
        case 0xda: /* f64x2.pmin */
        case 0xdb: /* f64x2.pmax */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 2; ++i) {
                switch (subopcode) {
                    case 0xd4:
                        out.f64[i] = left.f64[i] + right.f64[i];
                        break;
                    case 0xd5:
                        out.f64[i] = left.f64[i] - right.f64[i];
                        break;
                    case 0xd6:
                        out.f64[i] = left.f64[i] * right.f64[i];
                        break;
                    case 0xd7:
                        out.f64[i] = left.f64[i] / right.f64[i];
                        break;
                    case 0xd8:
                        out.f64[i] = fmin(left.f64[i], right.f64[i]);
                        break;
                    case 0xd9:
                        out.f64[i] = fmax(left.f64[i], right.f64[i]);
                        break;
                    case 0xda:
                        out.f64[i] = simd_pmin_f64(left.f64[i], right.f64[i]);
                        break;
                    case 0xdb:
                        out.f64[i] = simd_pmax_f64(left.f64[i], right.f64[i]);
                        break;
                    default:
                        out.f64[i] = 0.0;
                        break;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_convert(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0xdc: /* i32x4.trunc_sat_f32x4_s */
        case 0xdd: /* i32x4.trunc_sat_f32x4_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0xdc) {
                    out.i32[i] = simd_trunc_sat_f32_to_i32(lanes.f32[i]);
                } else {
                    out.u32[i] = simd_trunc_sat_f32_to_u32(lanes.f32[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xde: /* f32x4.convert_i32x4_s */
        case 0xdf: /* f32x4.convert_i32x4_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0xde) {
                    out.f32[i] = (f32)lanes.i32[i];
                } else {
                    out.f32[i] = (f32)lanes.u32[i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0xe0: /* i32x4.trunc_sat_f64x2_s_zero */
        case 0xe1: /* i32x4.trunc_sat_f64x2_u_zero */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            if (subopcode == 0xe0) {
                out.i32[0] = simd_trunc_sat_f64_to_i32(lanes.f64[0]);
                out.i32[1] = simd_trunc_sat_f64_to_i32(lanes.f64[1]);
            } else {
                out.u32[0] = simd_trunc_sat_f64_to_u32(lanes.f64[0]);
                out.u32[1] = simd_trunc_sat_f64_to_u32(lanes.f64[1]);
            }
            out.u32[2] = 0U;
            out.u32[3] = 0U;
            return push_v128_lanes_checked(job, &out);
        }
        case 0xe2: /* f64x2.convert_low_i32x4_s */
        case 0xe3: /* f64x2.convert_low_i32x4_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            if (subopcode == 0xe2) {
                out.f64[0] = (f64)lanes.i32[0];
                out.f64[1] = (f64)lanes.i32[1];
            } else {
                out.f64[0] = (f64)lanes.u32[0];
                out.f64[1] = (f64)lanes.u32[1];
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd_relaxed(OP_ARGUMENTS, u64 subopcode) {
    (void)runtime;
    (void)descriptor;
    switch (subopcode) {
        case 0x100: /* i8x16.relaxed_swizzle */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 16; ++i) {
                const uint8_t lane = (uint8_t)(right.u8[i] & 0x0FU);
                out.u8[i] = left.u8[lane];
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x101: /* i32x4.relaxed_trunc_f32x4_s */
        case 0x102: /* i32x4.relaxed_trunc_f32x4_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0x101) {
                    out.i32[i] = simd_trunc_sat_f32_to_i32(lanes.f32[i]);
                } else {
                    out.u32[i] = simd_trunc_sat_f32_to_u32(lanes.f32[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x103: /* i32x4.relaxed_trunc_f64x2_s */
        case 0x104: /* i32x4.relaxed_trunc_f64x2_u */
        {
            fa_V128 value = {0};
            if (!pop_v128_value(job, &value, NULL)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes lanes = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&value, &lanes);
            if (subopcode == 0x103) {
                out.i32[0] = simd_trunc_sat_f64_to_i32(lanes.f64[0]);
                out.i32[1] = simd_trunc_sat_f64_to_i32(lanes.f64[1]);
            } else {
                out.u32[0] = simd_trunc_sat_f64_to_u32(lanes.f64[0]);
                out.u32[1] = simd_trunc_sat_f64_to_u32(lanes.f64[1]);
            }
            out.u32[2] = 0U;
            out.u32[3] = 0U;
            return push_v128_lanes_checked(job, &out);
        }
        case 0x105: /* f32x4.relaxed_madd */
        case 0x106: /* f32x4.relaxed_nmadd */
        {
            fa_V128 acc = {0};
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue acc_raw = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_v128_value(job, &acc, &acc_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &rhs, &rhs_raw)) {
                restore_stack_value(job, &acc_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &lhs, &lhs_raw)) {
                restore_stack_value(job, &rhs_raw);
                restore_stack_value(job, &acc_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes accum = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            v128_to_lanes(&acc, &accum);
            for (size_t i = 0; i < 4; ++i) {
                const f32 prod = left.f32[i] * right.f32[i];
                out.f32[i] = (subopcode == 0x105) ? (prod + accum.f32[i]) : (-prod + accum.f32[i]);
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x107: /* f64x2.relaxed_madd */
        case 0x108: /* f64x2.relaxed_nmadd */
        {
            fa_V128 acc = {0};
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue acc_raw = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_v128_value(job, &acc, &acc_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &rhs, &rhs_raw)) {
                restore_stack_value(job, &acc_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &lhs, &lhs_raw)) {
                restore_stack_value(job, &rhs_raw);
                restore_stack_value(job, &acc_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes accum = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            v128_to_lanes(&acc, &accum);
            for (size_t i = 0; i < 2; ++i) {
                const f64 prod = left.f64[i] * right.f64[i];
                out.f64[i] = (subopcode == 0x107) ? (prod + accum.f64[i]) : (-prod + accum.f64[i]);
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x109: /* i8x16.relaxed_laneselect */
        case 0x10a: /* i16x8.relaxed_laneselect */
        case 0x10b: /* i32x4.relaxed_laneselect */
        case 0x10c: /* i64x2.relaxed_laneselect */
        {
            fa_V128 mask = {0};
            fa_V128 rhs = {0};
            fa_V128 lhs = {0};
            fa_JobValue mask_raw = {0};
            fa_JobValue rhs_raw = {0};
            fa_JobValue lhs_raw = {0};
            if (!pop_v128_value(job, &mask, &mask_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &rhs, &rhs_raw)) {
                restore_stack_value(job, &mask_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!pop_v128_value(job, &lhs, &lhs_raw)) {
                restore_stack_value(job, &rhs_raw);
                restore_stack_value(job, &mask_raw);
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes sel = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            v128_to_lanes(&mask, &sel);
            if (subopcode == 0x109) {
                for (size_t i = 0; i < 16; ++i) {
                    const bool choose_left = (sel.u8[i] & 0x80U) != 0U;
                    out.u8[i] = choose_left ? left.u8[i] : right.u8[i];
                }
            } else if (subopcode == 0x10a) {
                for (size_t i = 0; i < 8; ++i) {
                    const bool choose_left = (sel.u16[i] & 0x8000U) != 0U;
                    out.u16[i] = choose_left ? left.u16[i] : right.u16[i];
                }
            } else if (subopcode == 0x10b) {
                for (size_t i = 0; i < 4; ++i) {
                    const bool choose_left = (sel.u32[i] & 0x80000000U) != 0U;
                    out.u32[i] = choose_left ? left.u32[i] : right.u32[i];
                }
            } else {
                for (size_t i = 0; i < 2; ++i) {
                    const bool choose_left = (sel.u64[i] & 0x8000000000000000ULL) != 0ULL;
                    out.u64[i] = choose_left ? left.u64[i] : right.u64[i];
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x10d: /* f32x4.relaxed_min */
        case 0x10e: /* f32x4.relaxed_max */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 4; ++i) {
                if (subopcode == 0x10d) {
                    out.f32[i] = fminf(left.f32[i], right.f32[i]);
                } else {
                    out.f32[i] = fmaxf(left.f32[i], right.f32[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x10f: /* f64x2.relaxed_min */
        case 0x110: /* f64x2.relaxed_max */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 2; ++i) {
                if (subopcode == 0x10f) {
                    out.f64[i] = fmin(left.f64[i], right.f64[i]);
                } else {
                    out.f64[i] = fmax(left.f64[i], right.f64[i]);
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        case 0x111: /* i16x8.relaxed_q15mulr_s */
        {
            fa_V128 lhs = {0};
            fa_V128 rhs = {0};
            fa_JobValue lhs_raw = {0};
            fa_JobValue rhs_raw = {0};
            if (!pop_two_v128_values(job, &lhs, &rhs, &lhs_raw, &rhs_raw)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_V128Lanes left = {0};
            fa_V128Lanes right = {0};
            fa_V128Lanes out = {0};
            v128_to_lanes(&lhs, &left);
            v128_to_lanes(&rhs, &right);
            for (size_t i = 0; i < 8; ++i) {
                int32_t prod = (int32_t)left.i16[i] * (int32_t)right.i16[i];
                int32_t rounded = (prod + 0x4000) >> 15;
                if (rounded > INT16_MAX) {
                    out.i16[i] = INT16_MAX;
                } else if (rounded < INT16_MIN) {
                    out.i16[i] = INT16_MIN;
                } else {
                    out.i16[i] = (int16_t)rounded;
                }
            }
            return push_v128_lanes_checked(job, &out);
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

/*
 * SIMD dispatch table.
 * Each 0xFD subopcode maps to the family handler that owns it. The mapping is
 * built once from compact [lo, hi] ranges so the hot path is a single indexed
 * lookup instead of the former 347-case switch tower.
 */
typedef OP_RETURN_TYPE (*SimdHandler)(OP_ARGUMENTS, u64 subopcode);

#define FA_SIMD_DISPATCH_SIZE 0x112u /* covers subopcodes 0x00..0x111 */

static SimdHandler g_simd_dispatch[FA_SIMD_DISPATCH_SIZE];
static bool g_simd_dispatch_initialized = false;

void fa_ops_simd_init_dispatch(void) {
    if (g_simd_dispatch_initialized) {
        return;
    }
    static const struct {
        uint32_t lo;
        uint32_t hi;
        SimdHandler handler;
    } kSimdRanges[] = {
        {0x00, 0x0b, op_simd_mem},      /* v128 load/store */
        {0x0c, 0x14, op_simd_build},    /* const/shuffle/swizzle/splat */
        {0x15, 0x22, op_simd_lane},     /* extract/replace lane */
        {0x23, 0x4c, op_simd_cmp},      /* integer + float compares */
        {0x4d, 0x53, op_simd_bitwise},  /* not/and/or/xor/bitselect/any_true */
        {0x54, 0x5f, op_simd_memlane},  /* load/store lane, load_zero, demote/promote */
        {0x60, 0x78, op_simd_i8x16},    /* i8x16 family + extadd_pairwise */
        {0x79, 0x96, op_simd_i16x8},    /* i16x8 family */
        {0x97, 0xad, op_simd_i32x4},    /* i32x4 family */
        {0xae, 0xc5, op_simd_i64x2},    /* i64x2 family */
        {0xc6, 0xd0, op_simd_f32x4},    /* f32x4 family */
        {0xd1, 0xdb, op_simd_f64x2},    /* f64x2 family */
        {0xdc, 0xe3, op_simd_convert},  /* trunc_sat / convert */
        {0x100, 0x111, op_simd_relaxed} /* relaxed SIMD */
    };
    memset(g_simd_dispatch, 0, sizeof(g_simd_dispatch));
    for (size_t r = 0; r < sizeof(kSimdRanges) / sizeof(kSimdRanges[0]); ++r) {
        for (uint32_t s = kSimdRanges[r].lo; s <= kSimdRanges[r].hi; ++s) {
            g_simd_dispatch[s] = kSimdRanges[r].handler;
        }
    }
    g_simd_dispatch_initialized = true;
}

/*
 * Handles 0xFD-prefixed SIMD and relaxed-SIMD subopcodes.
 * Decodes the subopcode immediate, then routes to the owning family handler
 * through the prebuilt dispatch table.
 */
OP_RETURN_TYPE op_simd(OP_ARGUMENTS) {
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 subopcode = 0;
    if (pop_reg_u64_checked(job, &subopcode) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (subopcode >= FA_SIMD_DISPATCH_SIZE) {
        return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
    SimdHandler handler = g_simd_dispatch[subopcode];
    if (!handler) {
        return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
    return handler(runtime, job, descriptor, subopcode);
}
