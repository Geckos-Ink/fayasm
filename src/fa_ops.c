#include "fa_ops.h"
#include "fa_runtime.h"
#include "fa_arch.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif

#define OP_KEYWORD

typedef struct {
    const Operation* steps;
    uint8_t step_count;
} fa_Microcode;

static fa_WasmOp g_ops[256];
static bool g_ops_initialized = false;
static const fa_Microcode* g_microcode[256];
static bool g_microcode_initialized = false;
static bool g_microcode_enabled = false;

static size_t op_value_width(const fa_WasmOp* op) {
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

static fa_JobDataFlow* job_node_from_value(const void* data, size_t size) {
    if (!data || size == 0 || size > FA_JOB_DATA_FLOW_MAX_SIZE) {
        return NULL;
    }
    fa_JobDataFlow* node = calloc(1, sizeof(fa_JobDataFlow) + size);
    if (!node) {
        return NULL;
    }
    node->size = (uint8_t)size;
    node->ptr = (void*)((uint8_t*)(node + 1));
    memcpy(node->ptr, data, size);
    return node;
}

static void job_node_destroy(fa_JobDataFlow* node) {
    if (!node) {
        return;
    }
    free(node);
}

static uint8_t job_reg_count_and_head(const fa_Job* job, fa_JobDataFlow** head_out) {
    uint8_t count = 0;
    fa_JobDataFlow* head = NULL;
    for (fa_JobDataFlow* cur = job ? job->reg : NULL; cur; cur = cur->precede) {
        head = cur;
        ++count;
    }
    if (head_out) {
        *head_out = head;
    }
    return count;
}

static void job_reg_enforce_limit(fa_Job* job) {
    if (!job) {
        return;
    }

    fa_JobDataFlow* head = NULL;
    uint8_t count = job_reg_count_and_head(job, &head);
    while (count > FA_JOB_DATA_FLOW_WINDOW_SIZE && head) {
        fa_JobDataFlow* next = head->follows;
        if (next) {
            next->precede = NULL;
        } else {
            job->reg = NULL;
        }
        job_node_destroy(head);
        head = next;
        --count;
    }
}

static void job_reg_push_node(fa_Job* job, fa_JobDataFlow* node) {
    if (!job || !node) {
        return;
    }
    node->follows = NULL;
    if (!job->reg) {
        node->precede = NULL;
        job->reg = node;
    } else {
        node->precede = job->reg;
        job->reg->follows = node;
        job->reg = node;
    }
    job_reg_enforce_limit(job);
}

static fa_JobDataFlow* job_reg_pop_tail(fa_Job* job) {
    if (!job) {
        return NULL;
    }
    fa_JobDataFlow* tail = job->reg;
    if (!tail) {
        return NULL;
    }
    fa_JobDataFlow* new_tail = tail->precede;
    if (new_tail) {
        new_tail->follows = NULL;
    }
    job->reg = new_tail;
    tail->precede = NULL;
    tail->follows = NULL;
    return tail;
}

static bool job_reg_push_value(fa_Job* job, const void* data, size_t size) {
    fa_JobDataFlow* node = job_node_from_value(data, size);
    if (!node) {
        return false;
    }
    job_reg_push_node(job, node);
    return true;
}

static u64 sign_extend_value(u64 value, uint8_t bits) {
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

static u64 mask_unsigned_value(u64 value, uint8_t bits) {
    if (bits == 0 || bits >= 64) {
        return value;
    }
    const u64 mask = (1ULL << bits) - 1ULL;
    return value & mask;
}

static bool push_int_value(fa_Job* job, u64 value, uint8_t bit_width, bool is_signed) {
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

static bool push_float_value(fa_Job* job, double value, bool is_64) {
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

static bool push_bool_value(fa_Job* job, bool truthy) {
    return push_int_value(job, truthy ? 1U : 0U, 32U, false);
}

static int push_int_checked(fa_Job* job, u64 value, uint8_t bit_width, bool is_signed) {
    return push_int_value(job, value, bit_width, is_signed) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

static int push_float_checked(fa_Job* job, double value, bool is_64) {
    return push_float_value(job, value, is_64) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

static int push_bool_checked(fa_Job* job, bool truthy) {
    return push_bool_value(job, truthy) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

static bool pop_stack_value(fa_Job* job, fa_JobValue* out) {
    if (!job) {
        return false;
    }
    return fa_JobStack_pop(&job->stack, out);
}

static int pop_stack_checked(fa_Job* job, fa_JobValue* out) {
    return pop_stack_value(job, out) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_TRAP;
}

static void restore_stack_value(fa_Job* job, const fa_JobValue* value) {
    if (!job || !value) {
        return;
    }
    fa_JobStack_push(&job->stack, value);
}

static bool job_value_to_u64(const fa_JobValue* value, u64* out) {
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

static bool job_value_to_i64(const fa_JobValue* value, i64* out) {
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

static bool job_value_to_f32(const fa_JobValue* value, f32* out) {
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

static bool job_value_to_f64(const fa_JobValue* value, f64* out) {
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

static bool job_value_truthy(const fa_JobValue* value) {
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

static bool job_value_matches_valtype(const fa_JobValue* value, uint8_t valtype) {
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

static bool trunc_f64_to_i32(double value, bool is_signed, u64* out) {
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

static bool trunc_f64_to_i64(double value, bool is_signed, u64* out) {
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

static bool pop_reg_to_buffer(fa_Job* job, void* buffer, size_t size) {
    if (!job || !buffer || size == 0) {
        return false;
    }
    fa_JobDataFlow* node = job_reg_pop_tail(job);
    if (!node) {
        memset(buffer, 0, size);
        return false;
    }
    size_t copy = node->size;
    if (copy > size) {
        copy = size;
    }
    memcpy(buffer, node->ptr, copy);
    if (copy < size) {
        memset((uint8_t*)buffer + copy, 0, size - copy);
    }
    job_node_destroy(node);
    return true;
}

static bool pop_reg_u64(fa_Job* job, u64* out) {
    if (!out) {
        return false;
    }
    u64 value = 0;
    const bool ok = pop_reg_to_buffer(job, &value, sizeof(value));
    *out = value;
    return ok;
}

static int pop_reg_u64_checked(fa_Job* job, u64* out) {
    return pop_reg_u64(job, out) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_TRAP;
}

static void discard_reg_arguments(fa_Job* job, uint8_t count) {
    if (!job) {
        return;
    }
    for (uint8_t i = 0; i < count; ++i) {
        fa_JobDataFlow* node = job_reg_pop_tail(job);
        if (!node) {
            break;
        }
        job_node_destroy(node);
    }
}

static int pop_address_checked_typed(fa_Job* job, u64* out, bool memory64) {
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

static int pop_index_checked(fa_Job* job, bool memory64, u64* out) {
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

static int pop_u32_checked(fa_Job* job, uint32_t* out) {
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

static int pop_ref_checked(fa_Job* job, fa_ptr* out) {
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

static int push_ref_checked(fa_Job* job, fa_ptr value) {
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

static int push_v128_checked(fa_Job* job, const fa_V128* value) {
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

static int memory_bounds_check(const fa_RuntimeMemory* memory, u64 addr, size_t size) {
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

static fa_RuntimeMemory* runtime_get_memory(fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->memories) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->memories_count) {
        return NULL;
    }
    return &runtime->memories[(uint32_t)index];
}

static fa_RuntimeTable* runtime_get_table(fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->tables) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->tables_count) {
        return NULL;
    }
    return &runtime->tables[(uint32_t)index];
}

static const WasmDataSegment* runtime_get_data_segment(const fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->module || !runtime->module->data_segments) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->module->num_data_segments) {
        return NULL;
    }
    return &runtime->module->data_segments[(uint32_t)index];
}

static const WasmElementSegment* runtime_get_element_segment(const fa_Runtime* runtime, u64 index) {
    if (!runtime || !runtime->module || !runtime->module->elements) {
        return NULL;
    }
    if (index > UINT32_MAX || index >= runtime->module->num_elements) {
        return NULL;
    }
    return &runtime->module->elements[(uint32_t)index];
}

static int pop_length_checked(fa_Job* job, bool memory64, u64* out) {
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

static int pop_byte_value_checked(fa_Job* job, u8* out) {
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

static u32 rotl32(u32 value, uint8_t amount) {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    return (value << amount) | (value >> (32U - amount));
}

static u32 rotr32(u32 value, uint8_t amount) {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    return (value >> amount) | (value << (32U - amount));
}

static u64 rotl64(u64 value, uint8_t amount) {
    amount &= 63U;
    if (amount == 0U) {
        return value;
    }
    return (value << amount) | (value >> (64U - amount));
}

static u64 rotr64(u64 value, uint8_t amount) {
    amount &= 63U;
    if (amount == 0U) {
        return value;
    }
    return (value >> amount) | (value << (64U - amount));
}

static u32 clz32(u32 value) {
    if (value == 0U) {
        return 32U;
    }
    u32 count = 0;
    for (u32 mask = 0x80000000U; (value & mask) == 0U; mask >>= 1U) {
        ++count;
    }
    return count;
}

static u32 ctz32(u32 value) {
    if (value == 0U) {
        return 32U;
    }
    u32 count = 0;
    for (u32 mask = 1U; (value & mask) == 0U; mask <<= 1U) {
        ++count;
    }
    return count;
}

static u32 popcnt32(u32 value) {
    u32 count = 0;
    while (value) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static u64 clz64(u64 value) {
    if (value == 0ULL) {
        return 64ULL;
    }
    u64 count = 0;
    for (u64 mask = 0x8000000000000000ULL; (value & mask) == 0ULL; mask >>= 1U) {
        ++count;
    }
    return count;
}

static u64 ctz64(u64 value) {
    if (value == 0ULL) {
        return 64ULL;
    }
    u64 count = 0;
    for (u64 mask = 1ULL; (value & mask) == 0ULL; mask <<= 1U) {
        ++count;
    }
    return count;
}

static u64 popcnt64(u64 value) {
    u64 count = 0;
    while (value) {
        count += value & 1ULL;
        value >>= 1U;
    }
    return count;
}

static OP_RETURN_TYPE op_control(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    switch (descriptor->id) {
        case 0x00: /* unreachable */
            fa_JobStack_reset(&job->stack);
            job->instructionPointer = 0;
            return FA_RUNTIME_ERR_TRAP;
        case 0x01: /* nop */
            return FA_RUNTIME_OK;
        case 0x02: /* block */
        case 0x03: /* loop */
            discard_reg_arguments(job, descriptor->num_args);
            return FA_RUNTIME_OK;
        case 0x04: /* if */
        {
            discard_reg_arguments(job, descriptor->num_args);
            fa_JobValue cond;
            if (pop_stack_checked(job, &cond) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            (void)job_value_truthy(&cond);
            return FA_RUNTIME_OK;
        }
        case 0x05: /* else */
        case 0x0B: /* end */
            return FA_RUNTIME_OK;
        case 0x0C: /* br */
        {
            u64 label = 0;
            if (pop_reg_u64_checked(job, &label) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            job->instructionPointer = (fa_ptr)label;
            return FA_RUNTIME_OK;
        }
        case 0x0D: /* br_if */
        {
            fa_JobValue cond;
            if (pop_stack_checked(job, &cond) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const bool truthy = job_value_truthy(&cond);
            u64 label = 0;
            if (pop_reg_u64_checked(job, &label) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (truthy) {
                job->instructionPointer = (fa_ptr)label;
            }
            return FA_RUNTIME_OK;
        }
        case 0x0E: /* br_table */
        {
            fa_JobValue index;
            if (pop_stack_checked(job, &index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 default_label = 0;
            if (pop_reg_u64_checked(job, &default_label) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            job->instructionPointer = (fa_ptr)default_label;
            return FA_RUNTIME_OK;
        }
        case 0x0F: /* return */
            job->instructionPointer = 0;
            return FA_RUNTIME_OK;
        default:
            return FA_RUNTIME_OK;
    }
}

static OP_RETURN_TYPE op_local(OP_ARGUMENTS) {
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->active_locals || index >= runtime->active_locals_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    switch (descriptor->id) {
        case 0x20: /* local.get */
        {
            const fa_JobValue value = runtime->active_locals[index];
            return fa_JobStack_push(&job->stack, &value) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        case 0x21: /* local.set */
        {
            fa_JobValue value;
            if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            runtime->active_locals[index] = value;
            return FA_RUNTIME_OK;
        }
        case 0x22: /* local.tee */
        {
            fa_JobValue value;
            if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const fa_JobValue previous = runtime->active_locals[index];
            runtime->active_locals[index] = value;
            if (!fa_JobStack_push(&job->stack, &value)) {
                runtime->active_locals[index] = previous;
                return FA_RUNTIME_ERR_OUT_OF_MEMORY;
            }
            return FA_RUNTIME_OK;
        }
        default:
            return FA_RUNTIME_ERR_TRAP;
    }
}

static OP_RETURN_TYPE op_global(OP_ARGUMENTS) {
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->globals || index >= runtime->globals_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->module || !runtime->module->globals || index >= runtime->module->num_globals) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const WasmGlobal* global = &runtime->module->globals[index];
    switch (descriptor->id) {
        case 0x23: /* global.get */
        {
            const fa_JobValue value = runtime->globals[index];
            if (!job_value_matches_valtype(&value, global->valtype)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            return fa_JobStack_push(&job->stack, &value) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        case 0x24: /* global.set */
        {
            fa_JobValue value;
            if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!global->is_mutable) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!job_value_matches_valtype(&value, global->valtype)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            runtime->globals[index] = value;
            return FA_RUNTIME_OK;
        }
        default:
            return FA_RUNTIME_ERR_TRAP;
    }
}

static OP_RETURN_TYPE op_load(OP_ARGUMENTS) {
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    u64 mem_index = 0;
    u64 offset = 0;
    if (descriptor->num_args > 0) {
        if (pop_reg_u64_checked(job, &offset) != FA_RUNTIME_OK) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (descriptor->num_args > 1) {
        u64 align = 0;
        if (pop_reg_u64_checked(job, &align) != FA_RUNTIME_OK) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (runtime->memories_count > 1) {
        if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }

    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 base = 0;
    if (pop_address_checked_typed(job, &base, memory->is_memory64) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }

    u64 addr = base;
    if (offset > UINT64_MAX - addr) {
        return FA_RUNTIME_ERR_TRAP;
    }
    addr += offset;
    size_t bits_to_read = descriptor->size_arg ? descriptor->size_arg : (descriptor->type.size ? descriptor->type.size * 8U : 64U);
    if (bits_to_read == 0U) {
        bits_to_read = 8U;
    }
    size_t bytes_to_read = (bits_to_read + 7U) / 8U;
    if (bytes_to_read > sizeof(u64)) {
        bytes_to_read = sizeof(u64);
    }

    if (memory_bounds_check(memory, addr, bytes_to_read) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }

    u64 raw = 0;
    memcpy(&raw, memory->data + (size_t)addr, bytes_to_read);

    if (descriptor->type.type == wt_float) {
        if (descriptor->type.size == 8) {
            f64 value = 0.0;
            memcpy(&value, &raw, sizeof(value));
            return push_float_checked(job, value, true);
        } else {
            f32 value = 0.0f;
            memcpy(&value, &raw, sizeof(value));
            return push_float_checked(job, value, false);
        }
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    if (is_signed) {
        raw = sign_extend_value(raw, (uint8_t)bits_to_read);
    } else {
        raw = mask_unsigned_value(raw, (uint8_t)bits_to_read);
    }
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : (uint8_t)bits_to_read;
    return push_int_checked(job, raw, result_bits, is_signed);
}

static OP_RETURN_TYPE op_store(OP_ARGUMENTS) {
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }

    u64 mem_index = 0;
    u64 offset = 0;
    if (descriptor->num_args > 0) {
        if (pop_reg_u64_checked(job, &offset) != FA_RUNTIME_OK) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (descriptor->num_args > 1) {
        u64 align = 0;
        if (pop_reg_u64_checked(job, &align) != FA_RUNTIME_OK) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (runtime->memories_count > 1) {
        if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
    }

    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 base = 0;
    if (pop_address_checked_typed(job, &base, memory->is_memory64) != FA_RUNTIME_OK) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 addr = base;
    if (offset > UINT64_MAX - addr) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    addr += offset;

    size_t bits_to_write = descriptor->size_arg ? descriptor->size_arg : (descriptor->type.size ? descriptor->type.size * 8U : 64U);
    if (bits_to_write == 0U) {
        bits_to_write = 8U;
    }
    size_t bytes_to_write = (bits_to_write + 7U) / 8U;
    if (bytes_to_write > sizeof(u64)) {
        bytes_to_write = sizeof(u64);
    }

    if (memory_bounds_check(memory, addr, bytes_to_write) != FA_RUNTIME_OK) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        if (descriptor->type.size == 8) {
            f64 data = 0.0;
            if (!job_value_to_f64(&value, &data)) {
                restore_stack_value(job, &value);
                return FA_RUNTIME_ERR_TRAP;
            }
            memcpy(memory->data + (size_t)addr, &data, sizeof(data));
        } else {
            f32 data = 0.0f;
            if (!job_value_to_f32(&value, &data)) {
                restore_stack_value(job, &value);
                return FA_RUNTIME_ERR_TRAP;
            }
            memcpy(memory->data + (size_t)addr, &data, sizeof(data));
        }
        return FA_RUNTIME_OK;
    }

    u64 raw = 0;
    if (!job_value_to_u64(&value, &raw)) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    raw = mask_unsigned_value(raw, (uint8_t)bits_to_write);
    memcpy(memory->data + (size_t)addr, &raw, bytes_to_write);
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_const(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    const size_t target_bits = descriptor->type.size ? descriptor->type.size * 8U : (descriptor->size_arg ? descriptor->size_arg : (uint8_t)(sizeof(u64) * 8U));
    const size_t target_bytes = (target_bits + 7U) / 8U;

    u64 raw = 0;
    if (!pop_reg_to_buffer(job, &raw, target_bytes > sizeof(raw) ? sizeof(raw) : target_bytes)) {
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        if (descriptor->type.size == 8) {
            f64 value = 0.0;
            memcpy(&value, &raw, sizeof(value));
            return push_float_checked(job, value, true);
        } else {
            f32 value = 0.0f;
            memcpy(&value, &raw, sizeof(value));
            return push_float_checked(job, value, false);
        }
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    u64 value = raw;
    if (is_signed) {
        value = sign_extend_value(raw, (uint8_t)target_bits);
    } else {
        value = mask_unsigned_value(raw, (uint8_t)target_bits);
    }
    return push_int_checked(job, value, (uint8_t)target_bits, is_signed);
}

static OP_RETURN_TYPE op_eqz(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_bool_checked(job, !job_value_truthy(&value));
}

#define DEFINE_BITWISE_OP(name, expr)                                            \
    static OP_RETURN_TYPE name(OP_ARGUMENTS) {                                   \
        (void)runtime;                                                           \
        if (!job || !descriptor) {                                               \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                              \
        }                                                                        \
        fa_JobValue rhs;                                                         \
        if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {                     \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        fa_JobValue lhs;                                                         \
        if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {                     \
            restore_stack_value(job, &rhs);                                      \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        u64 right = 0;                                                           \
        u64 left = 0;                                                            \
        if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) { \
            restore_stack_value(job, &lhs);                                      \
            restore_stack_value(job, &rhs);                                      \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        u64 outcome = (expr);                                                    \
        const uint8_t bits = descriptor->type.size ? descriptor->type.size * 8U : 32U; \
        const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed; \
        return push_int_checked(job, mask_unsigned_value(outcome, bits), bits, is_signed); \
    }

DEFINE_BITWISE_OP(op_bitwise_and_mc, left & right)
DEFINE_BITWISE_OP(op_bitwise_or_mc, left | right)
DEFINE_BITWISE_OP(op_bitwise_xor_mc, left ^ right)

#define DEFINE_BITCOUNT_OP(name, op32, op64)                                     \
    static OP_RETURN_TYPE name(OP_ARGUMENTS) {                                   \
        (void)runtime;                                                           \
        if (!job || !descriptor) {                                               \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                              \
        }                                                                        \
        fa_JobValue value;                                                       \
        if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {                   \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        u64 raw = 0;                                                             \
        if (!job_value_to_u64(&value, &raw)) {                                   \
            restore_stack_value(job, &value);                                    \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U; \
        u64 result = 0;                                                          \
        if (width <= 32U) {                                                      \
            result = (u64)(op32((u32)raw));                                      \
        } else {                                                                 \
            result = (u64)(op64(raw));                                           \
        }                                                                        \
        return push_int_checked(job, result, width, false);                      \
    }

DEFINE_BITCOUNT_OP(op_bitcount_clz_mc, clz32, clz64)
DEFINE_BITCOUNT_OP(op_bitcount_ctz_mc, ctz32, ctz64)
DEFINE_BITCOUNT_OP(op_bitcount_popcnt_mc, popcnt32, popcnt64)

static OP_RETURN_TYPE op_shift_left_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 left_raw = 0;
    if (!job_value_to_u64(&lhs, &left_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 base = (u32)left_raw;
        outcome = (u64)(base << amount);
    } else {
        const u64 base = left_raw;
        outcome = base << amount;
    }
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    return push_int_checked(job, outcome, width, is_signed);
}

static OP_RETURN_TYPE op_shift_right_signed_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 left_signed = 0;
    if (!job_value_to_i64(&lhs, &left_signed)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const i32 base = (i32)left_signed;
        outcome = (u64)(i32)(base >> amount);
    } else {
        const i64 base = left_signed;
        outcome = (u64)(base >> amount);
    }
    return push_int_checked(job, outcome, width, true);
}

static OP_RETURN_TYPE op_shift_right_unsigned_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 left_unsigned = 0;
    if (!job_value_to_u64(&lhs, &left_unsigned)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 base = (u32)left_unsigned;
        outcome = (u64)(base >> amount);
    } else {
        const u64 base = left_unsigned;
        outcome = base >> amount;
    }
    return push_int_checked(job, outcome, width, false);
}

static OP_RETURN_TYPE op_rotate_left_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 value_raw = 0;
    if (!job_value_to_u64(&lhs, &value_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 value = (u32)value_raw;
        outcome = (u64)rotl32(value, amount);
    } else {
        const u64 value = value_raw;
        outcome = rotl64(value, amount);
    }
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    return push_int_checked(job, outcome, width, is_signed);
}

static OP_RETURN_TYPE op_rotate_right_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 value_raw = 0;
    if (!job_value_to_u64(&lhs, &value_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 value = (u32)value_raw;
        outcome = (u64)rotr32(value, amount);
    } else {
        const u64 value = value_raw;
        outcome = rotr64(value, amount);
    }
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    return push_int_checked(job, outcome, width, is_signed);
}

#define DEFINE_COMPARE_OP(name, cmp)                                            \
    static OP_RETURN_TYPE name(OP_ARGUMENTS) {                                  \
        (void)runtime;                                                          \
        if (!job || !descriptor) {                                              \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                             \
        }                                                                       \
        fa_JobValue rhs;                                                        \
        if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {                    \
            return FA_RUNTIME_ERR_TRAP;                                         \
        }                                                                       \
        fa_JobValue lhs;                                                        \
        if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {                    \
            restore_stack_value(job, &rhs);                                     \
            return FA_RUNTIME_ERR_TRAP;                                         \
        }                                                                       \
        bool result = false;                                                    \
        if (descriptor->type.type == wt_float) {                                \
            f64 right = 0.0;                                                    \
            f64 left = 0.0;                                                     \
            if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) { \
                restore_stack_value(job, &lhs);                                 \
                restore_stack_value(job, &rhs);                                 \
                return FA_RUNTIME_ERR_TRAP;                                     \
            }                                                                   \
            result = (left cmp right);                                          \
        } else {                                                                \
            const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed; \
            if (is_signed) {                                                    \
                i64 right = 0;                                                  \
                i64 left = 0;                                                   \
                if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) { \
                    restore_stack_value(job, &lhs);                             \
                    restore_stack_value(job, &rhs);                             \
                    return FA_RUNTIME_ERR_TRAP;                                 \
                }                                                               \
                result = (left cmp right);                                      \
            } else {                                                            \
                u64 right = 0;                                                  \
                u64 left = 0;                                                   \
                if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) { \
                    restore_stack_value(job, &lhs);                             \
                    restore_stack_value(job, &rhs);                             \
                    return FA_RUNTIME_ERR_TRAP;                                 \
                }                                                               \
                result = (left cmp right);                                      \
            }                                                                   \
        }                                                                       \
        return push_bool_checked(job, result);                                  \
    }

DEFINE_COMPARE_OP(op_compare_eq_mc, ==)
DEFINE_COMPARE_OP(op_compare_ne_mc, !=)
DEFINE_COMPARE_OP(op_compare_lt_mc, <)
DEFINE_COMPARE_OP(op_compare_gt_mc, >)
DEFINE_COMPARE_OP(op_compare_le_mc, <=)
DEFINE_COMPARE_OP(op_compare_ge_mc, >=)

static OP_RETURN_TYPE op_arith_add_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left + right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left + right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left + right;
    return push_int_checked(job, outcome, result_bits, false);
}

static OP_RETURN_TYPE op_arith_sub_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left - right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left - right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left - right;
    return push_int_checked(job, outcome, result_bits, false);
}

static OP_RETURN_TYPE op_arith_mul_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left * right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left * right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left * right;
    return push_int_checked(job, outcome, result_bits, false);
}

static OP_RETURN_TYPE op_arith_div_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left / right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        if (right == 0) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        if ((result_bits == 32U && left == (i64)INT32_MIN && right == -1) ||
            (result_bits == 64U && left == (i64)INT64_MIN && right == -1)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left / right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    if (right == 0) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left / right;
    return push_int_checked(job, outcome, result_bits, false);
}

static OP_RETURN_TYPE op_arith_rem_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        if (right == 0) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left % right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    if (right == 0) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left % right;
    return push_int_checked(job, outcome, result_bits, false);
}

#define DEFINE_CONVERT_OP(name, body)                                            \
    static OP_RETURN_TYPE name(OP_ARGUMENTS) {                                   \
        (void)runtime;                                                           \
        if (!job || !descriptor) {                                               \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                              \
        }                                                                        \
        fa_JobValue source;                                                      \
        if (pop_stack_checked(job, &source) != FA_RUNTIME_OK) {                  \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        body                                                                     \
    }

DEFINE_CONVERT_OP(op_convert_i32_wrap_i64_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, (u32)value, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f32_s_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32((double)value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f32_u_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32((double)value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, false);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f64_s_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32(value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f64_u_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32(value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_extend_i32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, sign_extend_value((u64)value, 32U), 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend_i32_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, mask_unsigned_value(value, 32U), 64U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f32_s_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64((double)value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f32_u_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64((double)value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f64_s_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64(value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f64_u_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64(value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i32_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i64_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i64_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_demote_f64_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i32_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i64_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i64_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_promote_f32_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_i32_extend8_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i32 extended = (i32)(int8_t)(value & 0xFF);
    return push_int_checked(job, (u32)extended, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_extend16_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i32 extended = (i32)(int16_t)(value & 0xFFFF);
    return push_int_checked(job, (u32)extended, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend8_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 extended = (i64)(int8_t)(value & 0xFF);
    return push_int_checked(job, (u64)extended, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend16_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 extended = (i64)(int16_t)(value & 0xFFFF);
    return push_int_checked(job, (u64)extended, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 extended = (i64)(int32_t)(value & 0xFFFFFFFFULL);
    return push_int_checked(job, (u64)extended, 64U, true);
})

#define DEFINE_MICROCODE(name, ...)                                            \
    static const Operation name##_steps[] = { __VA_ARGS__ };                   \
    static const fa_Microcode name = {                                         \
        name##_steps,                                                          \
        (uint8_t)(sizeof(name##_steps) / sizeof(name##_steps[0]))              \
    };

DEFINE_MICROCODE(mc_bitwise_and, op_bitwise_and_mc)
DEFINE_MICROCODE(mc_bitwise_or, op_bitwise_or_mc)
DEFINE_MICROCODE(mc_bitwise_xor, op_bitwise_xor_mc)
DEFINE_MICROCODE(mc_bitcount_clz, op_bitcount_clz_mc)
DEFINE_MICROCODE(mc_bitcount_ctz, op_bitcount_ctz_mc)
DEFINE_MICROCODE(mc_bitcount_popcnt, op_bitcount_popcnt_mc)
DEFINE_MICROCODE(mc_shift_left, op_shift_left_mc)
DEFINE_MICROCODE(mc_shift_right_signed, op_shift_right_signed_mc)
DEFINE_MICROCODE(mc_shift_right_unsigned, op_shift_right_unsigned_mc)
DEFINE_MICROCODE(mc_rotate_left, op_rotate_left_mc)
DEFINE_MICROCODE(mc_rotate_right, op_rotate_right_mc)
DEFINE_MICROCODE(mc_compare_eq, op_compare_eq_mc)
DEFINE_MICROCODE(mc_compare_ne, op_compare_ne_mc)
DEFINE_MICROCODE(mc_compare_lt, op_compare_lt_mc)
DEFINE_MICROCODE(mc_compare_gt, op_compare_gt_mc)
DEFINE_MICROCODE(mc_compare_le, op_compare_le_mc)
DEFINE_MICROCODE(mc_compare_ge, op_compare_ge_mc)
DEFINE_MICROCODE(mc_arith_add, op_arith_add_mc)
DEFINE_MICROCODE(mc_arith_sub, op_arith_sub_mc)
DEFINE_MICROCODE(mc_arith_mul, op_arith_mul_mc)
DEFINE_MICROCODE(mc_arith_div, op_arith_div_mc)
DEFINE_MICROCODE(mc_arith_rem, op_arith_rem_mc)
DEFINE_MICROCODE(mc_convert_i32_wrap_i64, op_convert_i32_wrap_i64_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f32_s, op_convert_i32_trunc_f32_s_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f32_u, op_convert_i32_trunc_f32_u_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f64_s, op_convert_i32_trunc_f64_s_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f64_u, op_convert_i32_trunc_f64_u_mc)
DEFINE_MICROCODE(mc_convert_i64_extend_i32_s, op_convert_i64_extend_i32_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend_i32_u, op_convert_i64_extend_i32_u_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f32_s, op_convert_i64_trunc_f32_s_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f32_u, op_convert_i64_trunc_f32_u_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f64_s, op_convert_i64_trunc_f64_s_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f64_u, op_convert_i64_trunc_f64_u_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i32_s, op_convert_f32_from_i32_s_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i32_u, op_convert_f32_from_i32_u_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i64_s, op_convert_f32_from_i64_s_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i64_u, op_convert_f32_from_i64_u_mc)
DEFINE_MICROCODE(mc_convert_f32_demote_f64, op_convert_f32_demote_f64_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i32_s, op_convert_f64_from_i32_s_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i32_u, op_convert_f64_from_i32_u_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i64_s, op_convert_f64_from_i64_s_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i64_u, op_convert_f64_from_i64_u_mc)
DEFINE_MICROCODE(mc_convert_f64_promote_f32, op_convert_f64_promote_f32_mc)
DEFINE_MICROCODE(mc_convert_i32_extend8_s, op_convert_i32_extend8_s_mc)
DEFINE_MICROCODE(mc_convert_i32_extend16_s, op_convert_i32_extend16_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend8_s, op_convert_i64_extend8_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend16_s, op_convert_i64_extend16_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend32_s, op_convert_i64_extend32_s_mc)

#undef DEFINE_MICROCODE
#undef DEFINE_BITCOUNT_OP
#undef DEFINE_BITWISE_OP
#undef DEFINE_COMPARE_OP
#undef DEFINE_CONVERT_OP

static OP_RETURN_TYPE op_float_unary(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.size == 8) {
        f64 source = 0.0;
        if (!job_value_to_f64(&value, &source)) {
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
        }
        f64 result = 0.0;
        switch (descriptor->id) {
            case 0x99: /* f64.abs */
                result = fabs(source);
                break;
            case 0x9A: /* f64.neg */
                result = -source;
                break;
            case 0x9B: /* f64.ceil */
                result = ceil(source);
                break;
            case 0x9C: /* f64.floor */
                result = floor(source);
                break;
            case 0x9D: /* f64.trunc */
                result = trunc(source);
                break;
            case 0x9E: /* f64.nearest */
                result = nearbyint(source);
                break;
            case 0x9F: /* f64.sqrt */
                result = sqrt(source);
                break;
            default:
                restore_stack_value(job, &value);
                return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, result, true);
    }

    f32 source = 0.0f;
    if (!job_value_to_f32(&value, &source)) {
        restore_stack_value(job, &value);
        return FA_RUNTIME_ERR_TRAP;
    }
    f32 result = 0.0f;
    switch (descriptor->id) {
        case 0x8B: /* f32.abs */
            result = fabsf(source);
            break;
        case 0x8C: /* f32.neg */
            result = -source;
            break;
        case 0x8D: /* f32.ceil */
            result = ceilf(source);
            break;
        case 0x8E: /* f32.floor */
            result = floorf(source);
            break;
        case 0x8F: /* f32.trunc */
            result = truncf(source);
            break;
        case 0x90: /* f32.nearest */
            result = nearbyintf(source);
            break;
        case 0x91: /* f32.sqrt */
            result = sqrtf(source);
            break;
        default:
            restore_stack_value(job, &value);
            return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, result, false);
}

static OP_RETURN_TYPE op_float_binary_special(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.size == 8) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        f64 result = 0.0;
        switch (descriptor->id) {
            case 0xA4: /* f64.min */
                result = fmin(left, right);
                break;
            case 0xA5: /* f64.max */
                result = fmax(left, right);
                break;
            case 0xA6: /* f64.copysign */
                result = copysign(left, right);
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, result, true);
    } else {
        f32 right = 0.0f;
        f32 left = 0.0f;
        if (!job_value_to_f32(&rhs, &right) || !job_value_to_f32(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        f32 result = 0.0f;
        switch (descriptor->id) {
            case 0x96: /* f32.min */
                result = fminf(left, right);
                break;
            case 0x97: /* f32.max */
                result = fmaxf(left, right);
                break;
            case 0x98: /* f32.copysign */
                result = copysignf(left, right);
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, result, false);
    }
}

static OP_RETURN_TYPE op_reinterpret(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue source;
    if (pop_stack_checked(job, &source) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }

    switch (descriptor->id) {
        case 0xBC: /* i32.reinterpret_f32 */
        {
            u32 raw = 0;
            if (source.kind == fa_job_value_f32) {
                memcpy(&raw, &source.payload.f32_value, sizeof(raw));
            } else {
                f32 value = 0.0f;
                if (!job_value_to_f32(&source, &value)) {
                    restore_stack_value(job, &source);
                    return FA_RUNTIME_ERR_TRAP;
                }
                memcpy(&raw, &value, sizeof(raw));
            }
            return push_int_checked(job, raw, 32U, true);
        }
        case 0xBD: /* i64.reinterpret_f64 */
        {
            u64 raw = 0;
            if (source.kind == fa_job_value_f64) {
                memcpy(&raw, &source.payload.f64_value, sizeof(raw));
            } else {
                f64 value = 0.0;
                if (!job_value_to_f64(&source, &value)) {
                    restore_stack_value(job, &source);
                    return FA_RUNTIME_ERR_TRAP;
                }
                memcpy(&raw, &value, sizeof(raw));
            }
            return push_int_checked(job, raw, 64U, true);
        }
        case 0xBE: /* f32.reinterpret_i32 */
        {
            u64 raw64 = 0;
            if (!job_value_to_u64(&source, &raw64)) {
                restore_stack_value(job, &source);
                return FA_RUNTIME_ERR_TRAP;
            }
            const u32 raw = (u32)raw64;
            f32 value = 0.0f;
            memcpy(&value, &raw, sizeof(value));
            return push_float_checked(job, value, false);
        }
        case 0xBF: /* f64.reinterpret_i64 */
        {
            u64 raw = 0;
            if (!job_value_to_u64(&source, &raw)) {
                restore_stack_value(job, &source);
                return FA_RUNTIME_ERR_TRAP;
            }
            f64 value = 0.0;
            memcpy(&value, &raw, sizeof(value));
            return push_float_checked(job, value, true);
        }
        default:
            restore_stack_value(job, &source);
            return FA_RUNTIME_ERR_TRAP;
    }
}

static OP_RETURN_TYPE op_drop(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_select(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue condition;
    if (pop_stack_checked(job, &condition) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue value_false;
    if (pop_stack_checked(job, &value_false) != FA_RUNTIME_OK) {
        restore_stack_value(job, &condition);
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue value_true;
    if (pop_stack_checked(job, &value_true) != FA_RUNTIME_OK) {
        restore_stack_value(job, &value_false);
        restore_stack_value(job, &condition);
        return FA_RUNTIME_ERR_TRAP;
    }
    const bool truthy = job_value_truthy(&condition);
    const fa_JobValue* chosen = truthy ? &value_true : &value_false;
    if (!fa_JobStack_push(&job->stack, chosen)) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_call(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 func_index = 0;
    if (pop_reg_u64_checked(job, &func_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    job->instructionPointer = (fa_ptr)func_index;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_call_indirect(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue table_index;
    if (pop_stack_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 ignored = 0;
    (void)job_value_to_u64(&table_index, &ignored);
    u64 type_index = 0;
    if (pop_reg_u64_checked(job, &type_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 reserved = 0;
    if (pop_reg_u64_checked(job, &reserved) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    job->instructionPointer = (fa_ptr)type_index;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_return(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    job->instructionPointer = 0;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_table(OP_ARGUMENTS) {
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 table_index = 0;
    if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    if (!table) {
        return FA_RUNTIME_ERR_TRAP;
    }
    switch (descriptor->id) {
        case 0x25: /* table.get */
        {
            uint32_t index = 0;
            if (pop_u32_checked(job, &index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (index >= table->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            return push_ref_checked(job, table->data[index]);
        }
        case 0x26: /* table.set */
        {
            fa_ptr ref_value = 0;
            if (pop_ref_checked(job, &ref_value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t index = 0;
            if (pop_u32_checked(job, &index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (index >= table->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            table->data[index] = ref_value;
            return FA_RUNTIME_OK;
        }
        default:
            return FA_RUNTIME_ERR_TRAP;
    }
}

static int runtime_table_grow(fa_Runtime* runtime, u64 table_index, u64 delta, fa_ptr init_value, u64* prev_size_out, bool* grew_out);

static OP_RETURN_TYPE op_bulk_memory(OP_ARGUMENTS) {
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 subopcode = 0;
    if (pop_reg_u64_checked(job, &subopcode) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    switch (subopcode) {
        case 8: /* memory.init */
        {
            u64 mem_index = 0;
            u64 data_index = 0;
            if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK ||
                pop_reg_u64_checked(job, &data_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
            if (!memory) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const WasmDataSegment* segment = runtime_get_data_segment(runtime, data_index);
            if (!segment || !runtime->data_segments_dropped ||
                data_index >= runtime->data_segments_count ||
                runtime->data_segments_dropped[data_index]) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 length = 0;
            if (pop_index_checked(job, memory->is_memory64, &length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 src_offset = 0;
            if (pop_index_checked(job, memory->is_memory64, &src_offset) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 dst_addr = 0;
            if (pop_address_checked_typed(job, &dst_addr, memory->is_memory64) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (length > SIZE_MAX || src_offset > UINT64_MAX - length) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!segment->data && length > 0) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint64_t segment_end = src_offset + length;
            if (segment_end > segment->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const size_t len = (size_t)length;
            if (memory_bounds_check(memory, dst_addr, len) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            memcpy(memory->data + (size_t)dst_addr, segment->data + (size_t)src_offset, len);
            return FA_RUNTIME_OK;
        }
        case 9: /* data.drop */
        {
            u64 data_index = 0;
            if (pop_reg_u64_checked(job, &data_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!runtime_get_data_segment(runtime, data_index) ||
                !runtime->data_segments_dropped ||
                data_index >= runtime->data_segments_count) {
                return FA_RUNTIME_ERR_TRAP;
            }
            runtime->data_segments_dropped[data_index] = true;
            return FA_RUNTIME_OK;
        }
        case 10: /* memory.copy */
        {
            u64 src_index = 0;
            u64 dst_index = 0;
            if (pop_reg_u64_checked(job, &src_index) != FA_RUNTIME_OK ||
                pop_reg_u64_checked(job, &dst_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeMemory* src_memory = runtime_get_memory(runtime, src_index);
            fa_RuntimeMemory* dst_memory = runtime_get_memory(runtime, dst_index);
            if (!src_memory || !dst_memory) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const bool length_is_64 = src_memory->is_memory64 || dst_memory->is_memory64;
            u64 length = 0;
            if (pop_length_checked(job, length_is_64, &length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 src_addr = 0;
            if (pop_address_checked_typed(job, &src_addr, src_memory->is_memory64) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 dst_addr = 0;
            if (pop_address_checked_typed(job, &dst_addr, dst_memory->is_memory64) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (length > SIZE_MAX) {
                return FA_RUNTIME_ERR_TRAP;
            }
            size_t len = (size_t)length;
            if (memory_bounds_check(src_memory, src_addr, len) != FA_RUNTIME_OK ||
                memory_bounds_check(dst_memory, dst_addr, len) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            memmove(dst_memory->data + (size_t)dst_addr, src_memory->data + (size_t)src_addr, len);
            return FA_RUNTIME_OK;
        }
        case 11: /* memory.fill */
        {
            u64 mem_index = 0;
            if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
            if (!memory) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 length = 0;
            if (pop_length_checked(job, memory->is_memory64, &length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u8 byte_value = 0;
            if (pop_byte_value_checked(job, &byte_value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 dst_addr = 0;
            if (pop_address_checked_typed(job, &dst_addr, memory->is_memory64) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (length > SIZE_MAX) {
                return FA_RUNTIME_ERR_TRAP;
            }
            size_t len = (size_t)length;
            if (memory_bounds_check(memory, dst_addr, len) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            memset(memory->data + (size_t)dst_addr, byte_value, len);
            return FA_RUNTIME_OK;
        }
        case 12: /* table.init */
        {
            u64 elem_index = 0;
            u64 table_index = 0;
            if (pop_reg_u64_checked(job, &elem_index) != FA_RUNTIME_OK ||
                pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
            const WasmElementSegment* segment = runtime_get_element_segment(runtime, elem_index);
            if (!table || !segment || !runtime->elem_segments_dropped ||
                elem_index >= runtime->elem_segments_count ||
                runtime->elem_segments_dropped[elem_index] || !segment->is_passive) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (segment->elem_type != table->elem_type) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t length = 0;
            if (pop_u32_checked(job, &length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t src = 0;
            if (pop_u32_checked(job, &src) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t dst = 0;
            if (pop_u32_checked(job, &dst) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if ((uint64_t)src + length > segment->element_count ||
                (uint64_t)dst + length > table->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!segment->elements && length > 0) {
                return FA_RUNTIME_ERR_TRAP;
            }
            for (uint32_t i = 0; i < length; ++i) {
                table->data[dst + i] = (fa_ptr)segment->elements[src + i];
            }
            return FA_RUNTIME_OK;
        }
        case 13: /* elem.drop */
        {
            u64 elem_index = 0;
            if (pop_reg_u64_checked(job, &elem_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!runtime_get_element_segment(runtime, elem_index) ||
                !runtime->elem_segments_dropped ||
                elem_index >= runtime->elem_segments_count) {
                return FA_RUNTIME_ERR_TRAP;
            }
            runtime->elem_segments_dropped[elem_index] = true;
            return FA_RUNTIME_OK;
        }
        case 14: /* table.copy */
        {
            u64 src_index = 0;
            u64 dst_index = 0;
            if (pop_reg_u64_checked(job, &src_index) != FA_RUNTIME_OK ||
                pop_reg_u64_checked(job, &dst_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeTable* src_table = runtime_get_table(runtime, src_index);
            fa_RuntimeTable* dst_table = runtime_get_table(runtime, dst_index);
            if (!src_table || !dst_table) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t length = 0;
            if (pop_u32_checked(job, &length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t src = 0;
            if (pop_u32_checked(job, &src) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t dst = 0;
            if (pop_u32_checked(job, &dst) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if ((uint64_t)src + length > src_table->size ||
                (uint64_t)dst + length > dst_table->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (length > 0) {
                memmove(dst_table->data + dst, src_table->data + src, length * sizeof(fa_ptr));
            }
            return FA_RUNTIME_OK;
        }
        case 15: /* table.grow */
        {
            u64 table_index = 0;
            if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t delta = 0;
            if (pop_u32_checked(job, &delta) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_ptr init_value = 0;
            if (pop_ref_checked(job, &init_value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 prev_size = 0;
            bool grew = false;
            const int status = runtime_table_grow(runtime, table_index, delta, init_value, &prev_size, &grew);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            if (!grew) {
                return push_int_checked(job, (u32)UINT32_MAX, 32U, true);
            }
            return push_int_checked(job, prev_size, 32U, true);
        }
        case 16: /* table.size */
        {
            u64 table_index = 0;
            if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
            if (!table) {
                return FA_RUNTIME_ERR_TRAP;
            }
            return push_int_checked(job, table->size, 32U, true);
        }
        case 17: /* table.fill */
        {
            u64 table_index = 0;
            if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
            if (!table) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t length = 0;
            if (pop_u32_checked(job, &length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            fa_ptr value = 0;
            if (pop_ref_checked(job, &value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t start = 0;
            if (pop_u32_checked(job, &start) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if ((uint64_t)start + length > table->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            for (uint32_t i = 0; i < length; ++i) {
                table->data[start + i] = value;
            }
            return FA_RUNTIME_OK;
        }
        default:
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
}

static OP_RETURN_TYPE op_simd(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 subopcode = 0;
    if (pop_reg_u64_checked(job, &subopcode) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    switch (subopcode) {
        case 12: /* v128.const */
        {
            fa_V128 value = {0};
            if (!pop_reg_to_buffer(job, &value, sizeof(value))) {
                return FA_RUNTIME_ERR_TRAP;
            }
            return push_v128_checked(job, &value);
        }
        case 15: /* i8x16.splat */
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
        case 16: /* i16x8.splat */
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
        case 17: /* i32x4.splat */
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
        case 18: /* i64x2.splat */
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
        case 19: /* f32x4.splat */
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
        case 20: /* f64x2.splat */
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

static int runtime_table_grow(fa_Runtime* runtime, u64 table_index, u64 delta, fa_ptr init_value, u64* prev_size_out, bool* grew_out) {
    if (!runtime || !prev_size_out || !grew_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    if (!table) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    *prev_size_out = table->size;
    *grew_out = false;
    if (delta == 0) {
        *grew_out = true;
        return FA_RUNTIME_OK;
    }
    if (delta > UINT32_MAX - table->size) {
        return FA_RUNTIME_OK;
    }
    const uint32_t new_size = table->size + (uint32_t)delta;
    if (table->has_max && new_size > table->max_size) {
        return FA_RUNTIME_OK;
    }
    if (new_size > SIZE_MAX / sizeof(fa_ptr)) {
        return FA_RUNTIME_OK;
    }
    fa_ptr* new_data = (fa_ptr*)realloc(table->data, new_size * sizeof(fa_ptr));
    if (!new_data) {
        return FA_RUNTIME_OK;
    }
    for (uint32_t i = table->size; i < new_size; ++i) {
        new_data[i] = init_value;
    }
    table->data = new_data;
    table->size = new_size;
    *grew_out = true;
    return FA_RUNTIME_OK;
}

static int runtime_memory_grow(fa_Runtime* runtime, u64 mem_index, u64 delta_pages, u64* prev_pages_out, bool* grew_out) {
    if (!runtime || !prev_pages_out || !grew_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    const uint64_t prev_pages = memory->size_bytes / FA_WASM_PAGE_SIZE;
    if (!memory->is_memory64 && prev_pages > UINT32_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    *prev_pages_out = prev_pages;
    *grew_out = false;
    if (delta_pages == 0) {
        *grew_out = true;
        return FA_RUNTIME_OK;
    }
    const uint64_t new_pages = prev_pages + delta_pages;
    if (new_pages < prev_pages) {
        return FA_RUNTIME_OK;
    }
    if (memory->has_max) {
        const uint64_t max_pages = memory->max_size_bytes / FA_WASM_PAGE_SIZE;
        if (new_pages > max_pages) {
            return FA_RUNTIME_OK;
        }
    }
    if (new_pages > (UINT64_MAX / FA_WASM_PAGE_SIZE)) {
        return FA_RUNTIME_OK;
    }
    const uint64_t new_size_bytes = new_pages * FA_WASM_PAGE_SIZE;
    if (new_size_bytes > SIZE_MAX || new_size_bytes > (uint64_t)INT_MAX) {
        return FA_RUNTIME_OK;
    }
    uint8_t* new_data = (uint8_t*)runtime->malloc((int)new_size_bytes);
    if (!new_data) {
        return FA_RUNTIME_OK;
    }
    if (memory->data && memory->size_bytes > 0) {
        memcpy(new_data, memory->data, (size_t)memory->size_bytes);
    }
    if (new_size_bytes > memory->size_bytes) {
        memset(new_data + memory->size_bytes, 0, (size_t)(new_size_bytes - memory->size_bytes));
    }
    if (memory->data) {
        runtime->free(memory->data);
    }
    memory->data = new_data;
    memory->size_bytes = new_size_bytes;
    *grew_out = true;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_memory_size(OP_ARGUMENTS) {
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->module || runtime->module->num_memories == 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 mem_index = 0;
    if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint64_t pages = memory->size_bytes / FA_WASM_PAGE_SIZE;
    const uint8_t bits = memory->is_memory64 ? 64U : 32U;
    const bool is_signed = true;
    if (!memory->is_memory64 && pages > UINT32_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    return push_int_checked(job, pages, bits, is_signed);
}

static OP_RETURN_TYPE op_memory_grow(OP_ARGUMENTS) {
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->module || runtime->module->num_memories == 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 mem_index = 0;
    if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue delta;
    if (pop_stack_checked(job, &delta) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 delta_pages_raw = 0;
    if (memory->is_memory64) {
        if (delta.kind != fa_job_value_i64) {
            return FA_RUNTIME_ERR_TRAP;
        }
    } else {
        if (delta.kind != fa_job_value_i32) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (!job_value_to_u64(&delta, &delta_pages_raw)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!memory->is_memory64 && delta_pages_raw > UINT32_MAX) {
        return push_int_checked(job, (u32)UINT32_MAX, 32U, true);
    }
    u64 prev_pages = 0;
    bool grew = false;
    const int status = runtime_memory_grow(runtime, mem_index, delta_pages_raw, &prev_pages, &grew);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    if (!grew) {
        if (memory->is_memory64) {
            return push_int_checked(job, UINT64_MAX, 64U, true);
        }
        return push_int_checked(job, (u32)UINT32_MAX, 32U, true);
    }
    return push_int_checked(job, prev_pages, memory->is_memory64 ? 64U : 32U, true);
}

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

static fa_SystemProbe probe_system_resources(void) {
    fa_SystemProbe probe;
    memset(&probe, 0, sizeof(probe));
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

static void init_microcode_once(void) {
    if (g_microcode_initialized) {
        return;
    }
    memset(g_microcode, 0, sizeof(g_microcode));
    g_microcode_enabled = microcode_should_enable();
    if (g_microcode_enabled) {
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
        g_microcode[0xA0] = &mc_arith_add;        // f64.add
        g_microcode[0xA1] = &mc_arith_sub;        // f64.sub
        g_microcode[0xA2] = &mc_arith_mul;        // f64.mul
        g_microcode[0xA3] = &mc_arith_div;        // f64.div
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
        g_microcode[0xC0] = &mc_convert_i32_extend8_s;     // i32.extend8_s
        g_microcode[0xC1] = &mc_convert_i32_extend16_s;    // i32.extend16_s
        g_microcode[0xC2] = &mc_convert_i64_extend8_s;     // i64.extend8_s
        g_microcode[0xC3] = &mc_convert_i64_extend16_s;    // i64.extend16_s
        g_microcode[0xC4] = &mc_convert_i64_extend32_s;    // i64.extend32_s
    }
    g_microcode_initialized = true;
}

static void init_ops_once(void) {
    if (g_ops_initialized) {
        return;
    }
    fa_ops_defs_populate(g_ops);
    init_microcode_once();
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
    define_op(ops, 0x8B, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.abs
    define_op(ops, 0x8C, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.neg
    define_op(ops, 0x8D, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.ceil
    define_op(ops, 0x8E, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.floor
    define_op(ops, 0x8F, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.trunc
    define_op(ops, 0x90, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.nearest
    define_op(ops, 0x91, &type_f32, wopt_unique, 0, 1, 1, 0, op_float_unary); // f32.sqrt
    define_op(ops, 0x92, &type_f32, wopt_add, 0, 2, 1, 0, op_arith_add_mc); // f32.add
    define_op(ops, 0x93, &type_f32, wopt_sub, 0, 2, 1, 0, op_arith_sub_mc); // f32.sub
    define_op(ops, 0x94, &type_f32, wopt_mul, 0, 2, 1, 0, op_arith_mul_mc); // f32.mul
    define_op(ops, 0x95, &type_f32, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // f32.div
    define_op(ops, 0x96, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_binary_special); // f32.min
    define_op(ops, 0x97, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_binary_special); // f32.max
    define_op(ops, 0x98, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_binary_special); // f32.copysign
    define_op(ops, 0x99, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.abs
    define_op(ops, 0x9A, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.neg
    define_op(ops, 0x9B, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.ceil
    define_op(ops, 0x9C, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.floor
    define_op(ops, 0x9D, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.trunc
    define_op(ops, 0x9E, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.nearest
    define_op(ops, 0x9F, &type_f64, wopt_unique, 0, 1, 1, 0, op_float_unary); // f64.sqrt
    define_op(ops, 0xA0, &type_f64, wopt_add, 0, 2, 1, 0, op_arith_add_mc); // f64.add
    define_op(ops, 0xA1, &type_f64, wopt_sub, 0, 2, 1, 0, op_arith_sub_mc); // f64.sub
    define_op(ops, 0xA2, &type_f64, wopt_mul, 0, 2, 1, 0, op_arith_mul_mc); // f64.mul
    define_op(ops, 0xA3, &type_f64, wopt_div, 0, 2, 1, 0, op_arith_div_mc); // f64.div
    define_op(ops, 0xA4, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_binary_special); // f64.min
    define_op(ops, 0xA5, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_binary_special); // f64.max
    define_op(ops, 0xA6, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_binary_special); // f64.copysign
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
    define_op(ops, 0xBC, &type_i32, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret); // i32.reinterpret_f32
    define_op(ops, 0xBD, &type_i64, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret); // i64.reinterpret_f64
    define_op(ops, 0xBE, &type_f32, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret); // f32.reinterpret_i32
    define_op(ops, 0xBF, &type_f64, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret); // f64.reinterpret_i64
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
