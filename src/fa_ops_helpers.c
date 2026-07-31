#include "fa_ops_internal.h"

/* Shared typed-value, operand-stack, register-window, and resource helpers. */

size_t op_value_width(const fa_WasmOp* op) {
    if (!op) {
        return 0;
    }
    if (op->size_arg != 0) {
        size_t bytes = op->size_arg / 8;
        if (bytes == 0 && (op->size_arg % 8) != 0) {
            bytes = (op->size_arg + 7) / 8;
        }
        if (bytes > 0) {
            if (bytes > FA_JOB_DATA_FLOW_MAX_SIZE) {
                bytes = FA_JOB_DATA_FLOW_MAX_SIZE;
            }
            return bytes;
        }
    }
    if (op->type.size != 0) {
        size_t bytes = op->type.size;
        if (bytes > FA_JOB_DATA_FLOW_MAX_SIZE) {
            bytes = FA_JOB_DATA_FLOW_MAX_SIZE;
        }
        return bytes;
    }
    return sizeof(fa_ptr);
}

u64 sign_extend_value(u64 value, uint8_t bits) {
    if (bits == 0 || bits >= 64) {
        return value;
    }
    const u64 mask = (1ULL << bits) - 1ULL;
    value &= mask;
    const u64 sign_bit = 1ULL << (bits - 1U);
    if (value & sign_bit) {
        value |= ~mask;
    }
    return value;
}

u64 mask_unsigned_value(u64 value, uint8_t bits) {
    if (bits == 0 || bits >= 64) {
        return value;
    }
    const u64 mask = (1ULL << bits) - 1ULL;
    return value & mask;
}

bool push_int_value(fa_Job* job, u64 value, uint8_t bit_width, bool is_signed) {
    if (!job) {
        return false;
    }
    if (bit_width == 0) {
        bit_width = is_signed ? 32U : 32U;
    }
    if (bit_width > 64U) {
        bit_width = 64U;
    }
    fa_JobValue v = {0};
    v.is_signed = is_signed;
    v.bit_width = bit_width;
    if (bit_width <= 32U) {
        v.kind = fa_job_value_i32;
        if (is_signed) {
            v.payload.i32_value = (i32)value;
        } else {
            v.payload.i32_value = (i32)(uint32_t)value;
        }
    } else {
        v.kind = fa_job_value_i64;
        if (is_signed) {
            v.payload.i64_value = (i64)value;
        } else {
            v.payload.i64_value = (i64)value;
        }
    }
    return fa_JobStack_push(&job->stack, &v);
}

bool push_float_value(fa_Job* job, double value, bool is_64) {
    if (!job) {
        return false;
    }
    fa_JobValue v = {0};
    v.kind = is_64 ? fa_job_value_f64 : fa_job_value_f32;
    v.bit_width = is_64 ? 64U : 32U;
    v.is_signed = false;
    if (is_64) {
        v.payload.f64_value = value;
    } else {
        v.payload.f32_value = (f32)value;
    }
    return fa_JobStack_push(&job->stack, &v);
}

bool push_bool_value(fa_Job* job, bool truthy) {
    return push_int_value(job, truthy ? 1U : 0U, 32U, false);
}

int push_int_checked(fa_Job* job, u64 value, uint8_t bit_width, bool is_signed) {
    return push_int_value(job, value, bit_width, is_signed) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

int push_float_checked(fa_Job* job, double value, bool is_64) {
    return push_float_value(job, value, is_64) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

int push_bool_checked(fa_Job* job, bool truthy) {
    return push_bool_value(job, truthy) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

/* Operand-stack pop/push wrappers that normalize trap-vs-OOM reporting. */
bool pop_stack_value(fa_Job* job, fa_JobValue* out) {
    if (!job) {
        return false;
    }
    return fa_JobStack_pop(&job->stack, out);
}

int pop_stack_checked(fa_Job* job, fa_JobValue* out) {
    return pop_stack_value(job, out) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_TRAP;
}

void restore_stack_value(fa_Job* job, const fa_JobValue* value) {
    if (!job || !value) {
        return;
    }
    fa_JobStack_push(&job->stack, value);
}

/*
 * Value conversion helpers used by handlers and macro-generated operators.
 * They intentionally accept a few cross-kind casts because WASM op semantics
 * vary by opcode and are validated by the surrounding handler.
 */
bool job_value_to_u64(const fa_JobValue* value, u64* out) {
    if (!value || !out) {
        return false;
    }
    switch (value->kind) {
        case fa_job_value_i32:
            if (value->is_signed) {
                *out = (u64)(u32)value->payload.i32_value;
            } else {
                *out = (u64)(uint32_t)value->payload.i32_value;
            }
            return true;
        case fa_job_value_i64:
            if (value->is_signed) {
                *out = (u64)value->payload.i64_value;
            } else {
                *out = (u64)value->payload.i64_value;
            }
            return true;
        case fa_job_value_f32:
            *out = (u64)(value->payload.f32_value);
            return true;
        case fa_job_value_f64:
            *out = (u64)(value->payload.f64_value);
            return true;
        case fa_job_value_ref:
            *out = value->payload.ref_value;
            return true;
        default:
            return false;
    }
}

bool job_value_to_i64(const fa_JobValue* value, i64* out) {
    if (!value || !out) {
        return false;
    }
    switch (value->kind) {
        case fa_job_value_i32:
            *out = value->payload.i32_value;
            return true;
        case fa_job_value_i64:
            *out = value->payload.i64_value;
            return true;
        case fa_job_value_f32:
            *out = (i64)value->payload.f32_value;
            return true;
        case fa_job_value_f64:
            *out = (i64)value->payload.f64_value;
            return true;
        case fa_job_value_ref:
            *out = (i64)value->payload.ref_value;
            return true;
        default:
            return false;
    }
}

bool job_value_to_f32(const fa_JobValue* value, f32* out) {
    if (!value || !out) {
        return false;
    }
    switch (value->kind) {
        case fa_job_value_f32:
            *out = value->payload.f32_value;
            return true;
        case fa_job_value_f64:
            *out = (f32)value->payload.f64_value;
            return true;
        case fa_job_value_i32:
            *out = (f32)value->payload.i32_value;
            return true;
        case fa_job_value_i64:
            *out = (f32)value->payload.i64_value;
            return true;
        default:
            return false;
    }
}

bool job_value_to_f64(const fa_JobValue* value, f64* out) {
    if (!value || !out) {
        return false;
    }
    switch (value->kind) {
        case fa_job_value_f32:
            *out = value->payload.f32_value;
            return true;
        case fa_job_value_f64:
            *out = value->payload.f64_value;
            return true;
        case fa_job_value_i32:
            *out = (f64)value->payload.i32_value;
            return true;
        case fa_job_value_i64:
            *out = (f64)value->payload.i64_value;
            return true;
        default:
            return false;
    }
}

bool job_value_to_v128(const fa_JobValue* value, fa_V128* out) {
    if (!value || !out) {
        return false;
    }
    if (value->kind != fa_job_value_v128) {
        return false;
    }
    *out = value->payload.v128_value;
    return true;
}

bool job_value_truthy(const fa_JobValue* value) {
    if (!value) {
        return false;
    }
    switch (value->kind) {
        case fa_job_value_i32:
            return value->payload.i32_value != 0;
        case fa_job_value_i64:
            return value->payload.i64_value != 0;
        case fa_job_value_f32:
            return value->payload.f32_value != 0.0f;
        case fa_job_value_f64:
            return value->payload.f64_value != 0.0;
        case fa_job_value_ref:
            return value->payload.ref_value != 0;
        default:
            return false;
    }
}

bool job_value_matches_valtype(const fa_JobValue* value, uint8_t valtype) {
    if (!value) {
        return false;
    }
    switch (valtype) {
        case VALTYPE_I32:
            return value->kind == fa_job_value_i32;
        case VALTYPE_I64:
            return value->kind == fa_job_value_i64;
        case VALTYPE_F32:
            return value->kind == fa_job_value_f32;
        case VALTYPE_F64:
            return value->kind == fa_job_value_f64;
        case VALTYPE_V128:
            return value->kind == fa_job_value_v128;
        case VALTYPE_FUNCREF:
        case VALTYPE_EXTERNREF:
            return value->kind == fa_job_value_ref;
        default:
            return false;
    }
}

bool trunc_f64_to_i32(double value, bool is_signed, u64* out) {
    if (!out || isnan(value) || !isfinite(value)) {
        return false;
    }
    const double min = is_signed ? (double)INT32_MIN : 0.0;
    const double max = is_signed ? (double)INT32_MAX : (double)UINT32_MAX;
    if (value < min || value > max) {
        return false;
    }
    const double truncated = trunc(value);
    if (is_signed) {
        *out = (u64)(i32)truncated;
    } else {
        *out = (u64)(u32)truncated;
    }
    return true;
}

bool trunc_f64_to_i64(double value, bool is_signed, u64* out) {
    if (!out || isnan(value) || !isfinite(value)) {
        return false;
    }
    const double min = is_signed ? (double)INT64_MIN : 0.0;
    const double max = is_signed ? (double)INT64_MAX : (double)UINT64_MAX;
    if (value < min || value > max) {
        return false;
    }
    const double truncated = trunc(value);
    if (is_signed) {
        *out = (u64)(i64)truncated;
    } else {
        *out = (u64)truncated;
    }
    return true;
}

/*
 * Non-trapping saturating truncation (the 0xFC 0x00..0x07 trunc_sat family).
 * Unlike the trapping conversions above these never fail: NaN maps to 0 and
 * out-of-range inputs clamp to the destination type's min/max. Returns the
 * raw bit pattern ready for push_int_checked at the given width/signedness.
 */
u64 trunc_sat_f64_to_i32(double value, bool is_signed) {
    if (isnan(value)) {
        return 0;
    }
    const double truncated = isfinite(value) ? trunc(value) : value;
    if (is_signed) {
        if (truncated < (double)INT32_MIN) {
            return (u64)(i32)INT32_MIN;
        }
        if (truncated > (double)INT32_MAX) {
            return (u64)(i32)INT32_MAX;
        }
        return (u64)(i32)truncated;
    }
    if (truncated <= 0.0) {
        return 0;
    }
    if (truncated > (double)UINT32_MAX) {
        return (u64)UINT32_MAX;
    }
    return (u64)(u32)truncated;
}

u64 trunc_sat_f64_to_i64(double value, bool is_signed) {
    if (isnan(value)) {
        return 0;
    }
    const double truncated = isfinite(value) ? trunc(value) : value;
    if (is_signed) {
        if (truncated < (double)INT64_MIN) {
            return (u64)INT64_MIN;
        }
        /* (double)INT64_MAX rounds up to 2^63, so saturate at >=. */
        if (truncated >= (double)INT64_MAX) {
            return (u64)INT64_MAX;
        }
        return (u64)(i64)truncated;
    }
    if (truncated <= 0.0) {
        return 0;
    }
    /* (double)UINT64_MAX rounds up to 2^64, so saturate at >=. */
    if (truncated >= (double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (u64)truncated;
}

/*
 * Immediates/subopcodes are read from the job's allocation-free forwarding
 * window. A missing entry is a decode/handler parity failure and traps.
 */
bool pop_reg_to_buffer(fa_Job* job, void* buffer, size_t size) {
    if (!job || !buffer || size == 0) {
        return false;
    }
    return fa_JobRegWindow_pop(job, buffer, size);
}

bool pop_reg_u64(fa_Job* job, u64* out) {
    if (!out) {
        return false;
    }
    u64 value = 0;
    const bool ok = pop_reg_to_buffer(job, &value, sizeof(value));
    *out = value;
    return ok;
}

int pop_reg_u64_checked(fa_Job* job, u64* out) {
    return pop_reg_u64(job, out) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_TRAP;
}

void discard_reg_arguments(fa_Job* job, uint8_t count) {
    if (!job) {
        return;
    }
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t discarded[FA_JOB_DATA_FLOW_MAX_SIZE];
        if (!fa_JobRegWindow_pop(job, discarded, sizeof(discarded))) {
            break;
        }
    }
}

int pop_address_checked_typed(fa_Job* job, u64* out, bool memory64) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue addr_value;
    if (!pop_stack_value(job, &addr_value)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (memory64) {
        if (addr_value.kind != fa_job_value_i64) {
            restore_stack_value(job, &addr_value);
            return FA_RUNTIME_ERR_TRAP;
        }
    } else {
        if (addr_value.kind != fa_job_value_i32) {
            restore_stack_value(job, &addr_value);
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    u64 raw = 0;
    if (!job_value_to_u64(&addr_value, &raw)) {
        restore_stack_value(job, &addr_value);
        return FA_RUNTIME_ERR_TRAP;
    }
    *out = raw;
    return FA_RUNTIME_OK;
}

int pop_index_checked(fa_Job* job, bool memory64, u64* out) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (memory64) {
        if (value.kind != fa_job_value_i64) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    } else {
        if (value.kind != fa_job_value_i32) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    u64 raw = 0;
    if (!job_value_to_u64(&value, &raw)) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    *out = raw;
    return FA_RUNTIME_OK;
}

int pop_u32_checked(fa_Job* job, uint32_t* out) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (value.kind != fa_job_value_i32) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 raw = 0;
    if (!job_value_to_u64(&value, &raw) || raw > UINT32_MAX) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    *out = (uint32_t)raw;
    return FA_RUNTIME_OK;
}

int pop_ref_checked(fa_Job* job, fa_ptr* out) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (value.kind != fa_job_value_ref) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    *out = value.payload.ref_value;
    return FA_RUNTIME_OK;
}

int push_ref_checked(fa_Job* job, fa_ptr value) {
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue v = {0};
    v.kind = fa_job_value_ref;
    v.bit_width = (uint8_t)(sizeof(fa_ptr) * 8U);
    v.is_signed = false;
    v.payload.ref_value = value;
    return fa_JobStack_push(&job->stack, &v) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

int push_v128_checked(fa_Job* job, const fa_V128* value) {
    if (!job || !value) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue v = {0};
    v.kind = fa_job_value_v128;
    v.bit_width = 128U;
    v.is_signed = false;
    v.payload.v128_value = *value;
    return fa_JobStack_push(&job->stack, &v) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

/* Common runtime guards for memory/table/segment lookup and validation. */
int memory_bounds_check(const fa_RuntimeMemory* memory, u64 addr, size_t size) {
    if (!memory || !memory->data) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (size == 0) {
        return FA_RUNTIME_OK;
    }
    if (addr > UINT64_MAX - size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (addr + size > memory->size_bytes) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}

fa_RuntimeMemory* runtime_get_memory(fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->memories) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->memories_count) {
        return NULL;
    }
    return &runtime->memories[(uint32_t)index];
}

int runtime_require_memory(fa_Runtime* runtime, u64 index, fa_RuntimeMemory** memory_out) {
    if (!runtime || !memory_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_RuntimeMemory* memory = runtime_get_memory(runtime, index);
    if (!memory) {
        return FA_RUNTIME_ERR_TRAP;
    }
    int status = fa_Runtime_ensureMemoryLoaded(runtime, (uint32_t)index);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    *memory_out = memory;
    return FA_RUNTIME_OK;
}

fa_RuntimeTable* runtime_get_table(fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->tables) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->tables_count) {
        return NULL;
    }
    return &runtime->tables[(uint32_t)index];
}

int runtime_decode_funcref_index(const fa_Runtime* runtime, fa_ptr encoded_ref, uint32_t* out_function_index) {
    if (!runtime || !runtime->module || !out_function_index) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!fa_funcref_decode_u32(encoded_ref, out_function_index)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (*out_function_index >= runtime->module->num_functions) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}

int runtime_validate_table_ref_value(const fa_Runtime* runtime,
                                            const fa_RuntimeTable* table,
                                            fa_ptr ref_value) {
    if (!table) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (table->elem_type != VALTYPE_FUNCREF || ref_value == 0) {
        return FA_RUNTIME_OK;
    }
    uint32_t function_index = 0;
    return runtime_decode_funcref_index(runtime, ref_value, &function_index);
}

const WasmDataSegment* runtime_get_data_segment(const fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->module || !runtime->module->data_segments) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->module->num_data_segments) {
        return NULL;
    }
    return &runtime->module->data_segments[(uint32_t)index];
}

const WasmElementSegment* runtime_get_element_segment(const fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->module || !runtime->module->elements) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->module->num_elements) {
        return NULL;
    }
    return &runtime->module->elements[(uint32_t)index];
}

int runtime_resolve_element_ref(const fa_Runtime* runtime,
                                       const WasmElementSegment* segment,
                                       uint32_t element_index,
                                       fa_ptr* out) {
    if (!runtime || !segment || !out || !segment->elements || element_index >= segment->element_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const WasmElementInit* init = &segment->elements[element_index];
    switch (init->kind) {
        case WASM_ELEMENT_INIT_REF_VALUE:
            *out = init->value;
            break;
        case WASM_ELEMENT_INIT_GLOBAL_GET:
        {
            if (!runtime->module || !runtime->module->globals ||
                init->global_index >= runtime->module->num_globals ||
                runtime->module->globals[init->global_index].valtype != segment->elem_type ||
                init->global_index >= runtime->globals_count || !runtime->globals) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const fa_JobValue* value = &runtime->globals[init->global_index];
            if (value->kind != fa_job_value_ref) {
                return FA_RUNTIME_ERR_TRAP;
            }
            *out = value->payload.ref_value;
            break;
        }
        default:
            return FA_RUNTIME_ERR_TRAP;
    }
    if (segment->elem_type == VALTYPE_FUNCREF && *out != 0) {
        uint32_t function_index = 0;
        if (runtime_decode_funcref_index(runtime, *out, &function_index) != FA_RUNTIME_OK) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    return FA_RUNTIME_OK;
}

int pop_length_checked(fa_Job* job, bool memory64, u64* out) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (memory64) {
        if (value.kind != fa_job_value_i64) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    } else {
        if (value.kind != fa_job_value_i32) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    u64 raw = 0;
    if (!job_value_to_u64(&value, &raw)) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    *out = raw;
    return FA_RUNTIME_OK;
}

int pop_byte_value_checked(fa_Job* job, u8* out) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (value.kind != fa_job_value_i32) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 raw = 0;
    if (!job_value_to_u64(&value, &raw)) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    *out = (u8)(raw & 0xFFU);
    return FA_RUNTIME_OK;
}

u32 rotl32(u32 value, uint8_t amount) {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    return (value << amount) | (value >> (32U - amount));
}

u32 rotr32(u32 value, uint8_t amount) {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    return (value >> amount) | (value << (32U - amount));
}

u64 rotl64(u64 value, uint8_t amount) {
    amount &= 63U;
    if (amount == 0U) {
        return value;
    }
    return (value << amount) | (value >> (64U - amount));
}

u64 rotr64(u64 value, uint8_t amount) {
    amount &= 63U;
    if (amount == 0U) {
        return value;
    }
    return (value >> amount) | (value << (64U - amount));
}

u32 clz32(u32 value) {
    if (value == 0U) {
        return 32U;
    }
    u32 count = 0;
    for (u32 mask = 0x80000000U; (value & mask) == 0U; mask >>= 1U) {
        ++count;
    }
    return count;
}

u32 ctz32(u32 value) {
    if (value == 0U) {
        return 32U;
    }
    u32 count = 0;
    for (u32 mask = 1U; (value & mask) == 0U; mask <<= 1U) {
        ++count;
    }
    return count;
}

u32 popcnt32(u32 value) {
    u32 count = 0;
    while (value) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

u64 clz64(u64 value) {
    if (value == 0ULL) {
        return 64ULL;
    }
    u64 count = 0;
    for (u64 mask = 0x8000000000000000ULL; (value & mask) == 0ULL; mask >>= 1U) {
        ++count;
    }
    return count;
}

u64 ctz64(u64 value) {
    if (value == 0ULL) {
        return 64ULL;
    }
    u64 count = 0;
    for (u64 mask = 1ULL; (value & mask) == 0ULL; mask <<= 1U) {
        ++count;
    }
    return count;
}

u64 popcnt64(u64 value) {
    u64 count = 0;
    while (value) {
        count += value & 1ULL;
        value >>= 1U;
    }
    return count;
}
