#include "fa_ops.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define OP_KEYWORD

static fa_WasmOp g_ops[256];
static bool g_ops_initialized = false;

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

static bool pop_stack_value(fa_Job* job, fa_JobValue* out) {
    if (!job) {
        return false;
    }
    return fa_JobStack_pop(&job->stack, out);
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

static bool pop_address_value(fa_Job* job, fa_ptr* out) {
    if (!job || !out) {
        return false;
    }
    fa_JobValue addr_value;
    if (pop_stack_value(job, &addr_value)) {
        u64 raw = 0;
        if (job_value_to_u64(&addr_value, &raw)) {
            *out = (fa_ptr)raw;
            return true;
        }
        restore_stack_value(job, &addr_value);
    }
    u64 raw = 0;
    if (!pop_reg_u64(job, &raw)) {
        return false;
    }
    *out = (fa_ptr)raw;
    return true;
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

static OP_RETURN_TYPE op_control(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }

    switch (descriptor->id) {
        case 0x00: /* unreachable */
            fa_JobStack_reset(&job->stack);
            job->instructionPointer = 0;
            break;
        case 0x01: /* nop */
            break;
        case 0x02: /* block */
        case 0x03: /* loop */
            discard_reg_arguments(job, descriptor->num_args);
            break;
        case 0x04: /* if */
        {
            discard_reg_arguments(job, descriptor->num_args);
            fa_JobValue cond;
            if (pop_stack_value(job, &cond)) {
                (void)job_value_truthy(&cond);
            }
            break;
        }
        case 0x05: /* else */
        case 0x0B: /* end */
            break;
        case 0x0C: /* br */
        {
            u64 label = 0;
            if (pop_reg_u64(job, &label)) {
                job->instructionPointer = (fa_ptr)label;
            }
            break;
        }
        case 0x0D: /* br_if */
        {
            fa_JobValue cond;
            if (!pop_stack_value(job, &cond)) {
                break;
            }
            const bool truthy = job_value_truthy(&cond);
            u64 label = 0;
            pop_reg_u64(job, &label);
            if (truthy) {
                job->instructionPointer = (fa_ptr)label;
            }
            break;
        }
        case 0x0E: /* br_table */
        {
            fa_JobValue index;
            if (pop_stack_value(job, &index)) {
                (void)job_value_truthy(&index);
            }
            u64 default_label = 0;
            pop_reg_u64(job, &default_label);
            job->instructionPointer = (fa_ptr)default_label;
            break;
        }
        case 0x0F: /* return */
            job->instructionPointer = 0;
            break;
        default:
            break;
    }
}

static OP_RETURN_TYPE op_load(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }

    u64 offset = 0;
    if (descriptor->num_args > 0) {
        pop_reg_u64(job, &offset);
    }
    if (descriptor->num_args > 1) {
        u64 align = 0;
        pop_reg_u64(job, &align);
    }

    fa_ptr base = 0;
    if (!pop_address_value(job, &base)) {
        return;
    }

    const fa_ptr addr = base + offset;
    size_t bits_to_read = descriptor->size_arg ? descriptor->size_arg : (descriptor->type.size ? descriptor->type.size * 8U : 64U);
    if (bits_to_read == 0U) {
        bits_to_read = 8U;
    }
    size_t bytes_to_read = (bits_to_read + 7U) / 8U;
    if (bytes_to_read > sizeof(u64)) {
        bytes_to_read = sizeof(u64);
    }

    u64 raw = 0;
    memcpy(&raw, (const void*)(uintptr_t)addr, bytes_to_read);

    if (descriptor->type.type == wt_float) {
        if (descriptor->type.size == 8) {
            f64 value = 0.0;
            memcpy(&value, &raw, sizeof(value));
            push_float_value(job, value, true);
        } else {
            f32 value = 0.0f;
            memcpy(&value, &raw, sizeof(value));
            push_float_value(job, value, false);
        }
        return;
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    if (is_signed) {
        raw = sign_extend_value(raw, (uint8_t)bits_to_read);
    } else {
        raw = mask_unsigned_value(raw, (uint8_t)bits_to_read);
    }
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : (uint8_t)bits_to_read;
    push_int_value(job, raw, result_bits, is_signed);
}

static OP_RETURN_TYPE op_store(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return;
    }

    u64 offset = 0;
    if (descriptor->num_args > 0) {
        pop_reg_u64(job, &offset);
    }
    if (descriptor->num_args > 1) {
        u64 align = 0;
        pop_reg_u64(job, &align);
    }

    fa_ptr base = 0;
    if (!pop_address_value(job, &base)) {
        restore_stack_value(job, &value);
        return;
    }
    const fa_ptr addr = base + offset;

    size_t bits_to_write = descriptor->size_arg ? descriptor->size_arg : (descriptor->type.size ? descriptor->type.size * 8U : 64U);
    if (bits_to_write == 0U) {
        bits_to_write = 8U;
    }
    size_t bytes_to_write = (bits_to_write + 7U) / 8U;
    if (bytes_to_write > sizeof(u64)) {
        bytes_to_write = sizeof(u64);
    }

    if (descriptor->type.type == wt_float) {
        if (descriptor->type.size == 8) {
            f64 data = 0.0;
            if (!job_value_to_f64(&value, &data)) {
                restore_stack_value(job, &value);
                return;
            }
            memcpy((void*)(uintptr_t)addr, &data, sizeof(data));
        } else {
            f32 data = 0.0f;
            if (!job_value_to_f32(&value, &data)) {
                restore_stack_value(job, &value);
                return;
            }
            memcpy((void*)(uintptr_t)addr, &data, sizeof(data));
        }
        return;
    }

    u64 raw = 0;
    if (!job_value_to_u64(&value, &raw)) {
        restore_stack_value(job, &value);
        return;
    }
    raw = mask_unsigned_value(raw, (uint8_t)bits_to_write);
    memcpy((void*)(uintptr_t)addr, &raw, bytes_to_write);
}

static OP_RETURN_TYPE op_const(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }

    const size_t target_bits = descriptor->type.size ? descriptor->type.size * 8U : (descriptor->size_arg ? descriptor->size_arg : (uint8_t)(sizeof(u64) * 8U));
    const size_t target_bytes = (target_bits + 7U) / 8U;

    u64 raw = 0;
    pop_reg_to_buffer(job, &raw, target_bytes > sizeof(raw) ? sizeof(raw) : target_bytes);

    if (descriptor->type.type == wt_float) {
        if (descriptor->type.size == 8) {
            f64 value = 0.0;
            memcpy(&value, &raw, sizeof(value));
            push_float_value(job, value, true);
        } else {
            f32 value = 0.0f;
            memcpy(&value, &raw, sizeof(value));
            push_float_value(job, value, false);
        }
        return;
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    u64 value = raw;
    if (is_signed) {
        value = sign_extend_value(raw, (uint8_t)target_bits);
    } else {
        value = mask_unsigned_value(raw, (uint8_t)target_bits);
    }
    push_int_value(job, value, (uint8_t)target_bits, is_signed);
}

static OP_RETURN_TYPE op_eqz(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue value;
    if (!pop_stack_value(job, &value)) {
        return;
    }
    push_bool_value(job, !job_value_truthy(&value));
}

static OP_RETURN_TYPE op_compare(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue rhs;
    if (!pop_stack_value(job, &rhs)) {
        return;
    }
    fa_JobValue lhs;
    if (!pop_stack_value(job, &lhs)) {
        restore_stack_value(job, &rhs);
        return;
    }

    bool result = false;
    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        switch (descriptor->op) {
            case wopt_eq:
                result = (left == right);
                break;
            case wopt_ne:
                result = (left != right);
                break;
            case wopt_lt:
                result = (left < right);
                break;
            case wopt_gt:
                result = (left > right);
                break;
            case wopt_le:
                result = (left <= right);
                break;
            case wopt_ge:
                result = (left >= right);
                break;
            default:
                break;
        }
    } else {
        const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
        if (is_signed) {
            i64 right = 0;
            i64 left = 0;
            if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
            }
            switch (descriptor->op) {
                case wopt_eq:
                    result = (left == right);
                    break;
                case wopt_ne:
                    result = (left != right);
                    break;
                case wopt_lt:
                    result = (left < right);
                    break;
                case wopt_gt:
                    result = (left > right);
                    break;
                case wopt_le:
                    result = (left <= right);
                    break;
                case wopt_ge:
                    result = (left >= right);
                    break;
                default:
                    break;
            }
        } else {
            u64 right = 0;
            u64 left = 0;
            if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
            }
            switch (descriptor->op) {
                case wopt_eq:
                    result = (left == right);
                    break;
                case wopt_ne:
                    result = (left != right);
                    break;
                case wopt_lt:
                    result = (left < right);
                    break;
                case wopt_gt:
                    result = (left > right);
                    break;
                case wopt_le:
                    result = (left <= right);
                    break;
                case wopt_ge:
                    result = (left >= right);
                    break;
                default:
                    break;
            }
        }
    }

    push_bool_value(job, result);
}

static OP_RETURN_TYPE op_arithmetic(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue rhs;
    if (!pop_stack_value(job, &rhs)) {
        return;
    }
    fa_JobValue lhs;
    if (!pop_stack_value(job, &lhs)) {
        restore_stack_value(job, &rhs);
        return;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        double result = 0.0;
        switch (descriptor->op) {
            case wopt_add:
                result = left + right;
                break;
            case wopt_sub:
                result = left - right;
                break;
            case wopt_mul:
                result = left * right;
                break;
            case wopt_div:
                result = right == 0.0 ? 0.0 : left / right;
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
        }
        push_float_value(job, result, descriptor->type.size == 8);
        return;
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;

    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        i64 outcome = 0;
        switch (descriptor->op) {
            case wopt_add:
                outcome = left + right;
                break;
            case wopt_sub:
                outcome = left - right;
                break;
            case wopt_mul:
                outcome = left * right;
                break;
            case wopt_div:
                if (right == 0) {
                    restore_stack_value(job, &lhs);
                    restore_stack_value(job, &rhs);
                    return;
                }
                outcome = left / right;
                break;
            case wopt_rem:
                if (right == 0) {
                    restore_stack_value(job, &lhs);
                    restore_stack_value(job, &rhs);
                    return;
                }
                outcome = left % right;
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
        }
        push_int_value(job, (u64)outcome, result_bits, true);
    } else {
        u64 right = 0;
        u64 left = 0;
        if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        u64 outcome = 0;
        switch (descriptor->op) {
            case wopt_add:
                outcome = left + right;
                break;
            case wopt_sub:
                outcome = left - right;
                break;
            case wopt_mul:
                outcome = left * right;
                break;
            case wopt_div:
                if (right == 0) {
                    restore_stack_value(job, &lhs);
                    restore_stack_value(job, &rhs);
                    return;
                }
                outcome = left / right;
                break;
            case wopt_rem:
                if (right == 0) {
                    restore_stack_value(job, &lhs);
                    restore_stack_value(job, &rhs);
                    return;
                }
                outcome = left % right;
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
        }
        push_int_value(job, outcome, result_bits, false);
    }
}

static OP_RETURN_TYPE op_bitwise(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue rhs;
    if (!pop_stack_value(job, &rhs)) {
        return;
    }
    fa_JobValue lhs;
    if (!pop_stack_value(job, &lhs)) {
        restore_stack_value(job, &rhs);
        return;
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return;
    }

    u64 outcome = 0;
    switch (descriptor->op) {
        case wopt_and:
            outcome = left & right;
            break;
        case wopt_or:
            outcome = left | right;
            break;
        case wopt_xor:
            outcome = left ^ right;
            break;
        default:
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
    }
    const uint8_t bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    push_int_value(job, mask_unsigned_value(outcome, bits), bits, is_signed);
}

static OP_RETURN_TYPE op_shift(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue rhs;
    if (!pop_stack_value(job, &rhs)) {
        return;
    }
    fa_JobValue lhs;
    if (!pop_stack_value(job, &lhs)) {
        restore_stack_value(job, &rhs);
        return;
    }

    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return;
    }

    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;

    if (width <= 32U) {
        i64 left_signed = 0;
        u64 left_unsigned = 0;
        if (is_signed) {
            if (!job_value_to_i64(&lhs, &left_signed)) {
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
            }
        } else {
            if (!job_value_to_u64(&lhs, &left_unsigned)) {
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
            }
        }
        const uint8_t mask = 31U;
        const uint8_t amount = (uint8_t)(amount_raw & mask);
        u32 result = 0;
        if (descriptor->op == wopt_shl) {
            const u32 base = is_signed ? (u32)(i32)left_signed : (u32)left_unsigned;
            result = base << amount;
        } else if (is_signed) {
            const i32 base = (i32)left_signed;
            result = (u32)(base >> amount);
        } else {
            const u32 base = (u32)left_unsigned;
            result = base >> amount;
        }
        push_int_value(job, result, width, is_signed);
    } else {
        i64 left_signed = 0;
        u64 left_unsigned = 0;
        if (is_signed) {
            if (!job_value_to_i64(&lhs, &left_signed)) {
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
            }
        } else {
            if (!job_value_to_u64(&lhs, &left_unsigned)) {
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
            }
        }
        const uint8_t mask = 63U;
        const uint8_t amount = (uint8_t)(amount_raw & mask);
        u64 result = 0;
        if (descriptor->op == wopt_shl) {
            const u64 base = is_signed ? (u64)left_signed : left_unsigned;
            result = base << amount;
        } else if (is_signed) {
            result = (u64)(left_signed >> amount);
        } else {
            result = left_unsigned >> amount;
        }
        push_int_value(job, result, width, is_signed);
    }
}

static OP_RETURN_TYPE op_rotate(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue rhs;
    if (!pop_stack_value(job, &rhs)) {
        return;
    }
    fa_JobValue lhs;
    if (!pop_stack_value(job, &lhs)) {
        restore_stack_value(job, &rhs);
        return;
    }

    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return;
    }

    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (width <= 32U) {
        u64 value_raw = 0;
        if (!job_value_to_u64(&lhs, &value_raw)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        const u32 value = (u32)value_raw;
        const uint8_t amount = (uint8_t)(amount_raw & 31U);
        u32 result = descriptor->op == wopt_rotl ? rotl32(value, amount) : rotr32(value, amount);
        const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
        push_int_value(job, result, width, is_signed);
    } else {
        u64 value = 0;
        if (!job_value_to_u64(&lhs, &value)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        const uint8_t amount = (uint8_t)(amount_raw & 63U);
        u64 result = descriptor->op == wopt_rotl ? rotl64(value, amount) : rotr64(value, amount);
        const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
        push_int_value(job, result, width, is_signed);
    }
}

static OP_RETURN_TYPE op_float_binary_special(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue rhs;
    if (!pop_stack_value(job, &rhs)) {
        return;
    }
    fa_JobValue lhs;
    if (!pop_stack_value(job, &lhs)) {
        restore_stack_value(job, &rhs);
        return;
    }

    if (descriptor->type.size == 8) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        f64 result = 0.0;
        switch (descriptor->id) {
            case 0x95: /* f64.min */
                result = fmin(left, right);
                break;
            case 0x96: /* f64.max */
                result = fmax(left, right);
                break;
            case 0x97: /* f64.copysign */
                result = copysign(left, right);
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
        }
        push_float_value(job, result, true);
    } else {
        f32 right = 0.0f;
        f32 left = 0.0f;
        if (!job_value_to_f32(&rhs, &right) || !job_value_to_f32(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return;
        }
        f32 result = 0.0f;
        switch (descriptor->id) {
            case 0x8E: /* f32.min */
                result = fminf(left, right);
                break;
            case 0x8F: /* f32.max */
                result = fmaxf(left, right);
                break;
            case 0x90: /* f32.copysign */
                result = copysignf(left, right);
                break;
            default:
                restore_stack_value(job, &lhs);
                restore_stack_value(job, &rhs);
                return;
        }
        push_float_value(job, result, false);
    }
}

static OP_RETURN_TYPE op_convert(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue source;
    if (!pop_stack_value(job, &source)) {
        return;
    }

    switch (descriptor->id) {
        case 0x98: /* i32.wrap_i64 */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, (u32)value, 32U, true);
            break;
        }
        case 0x99: /* i32.trunc_f32_s */
        {
            f32 value = 0.0f;
            if (!job_value_to_f32(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, (i32)value, 32U, true);
            break;
        }
        case 0x9A: /* i32.trunc_f32_u */
        {
            f32 value = 0.0f;
            if (!job_value_to_f32(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            u32 truncated = value < 0.0f ? 0U : (u32)value;
            push_int_value(job, truncated, 32U, false);
            break;
        }
        case 0x9B: /* i32.trunc_f64_s */
        {
            f64 value = 0.0;
            if (!job_value_to_f64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, (i32)value, 32U, true);
            break;
        }
        case 0x9C: /* i32.trunc_f64_u */
        {
            f64 value = 0.0;
            if (!job_value_to_f64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            u32 truncated = value < 0.0 ? 0U : (u32)value;
            push_int_value(job, truncated, 32U, false);
            break;
        }
        case 0x9D: /* i64.extend_i32_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, sign_extend_value((u64)value, 32U), 64U, true);
            break;
        }
        case 0x9E: /* i64.extend_i32_u */
        {
            u64 value = 0;
            if (!job_value_to_u64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, mask_unsigned_value(value, 32U), 64U, false);
            break;
        }
        case 0x9F: /* i64.trunc_f32_s */
        {
            f32 value = 0.0f;
            if (!job_value_to_f32(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, (i64)value, 64U, true);
            break;
        }
        case 0xA0: /* i64.trunc_f32_u */
        {
            f32 value = 0.0f;
            if (!job_value_to_f32(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            u64 truncated = value < 0.0f ? 0ULL : (u64)value;
            push_int_value(job, truncated, 64U, false);
            break;
        }
        case 0xA1: /* i64.trunc_f64_s */
        {
            f64 value = 0.0;
            if (!job_value_to_f64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_int_value(job, (i64)value, 64U, true);
            break;
        }
        case 0xA2: /* i64.trunc_f64_u */
        {
            f64 value = 0.0;
            if (!job_value_to_f64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            u64 truncated = value < 0.0 ? 0ULL : (u64)value;
            push_int_value(job, truncated, 64U, false);
            break;
        }
        case 0xA3: /* f32.convert_i32_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f32)value, false);
            break;
        }
        case 0xA4: /* f32.convert_i32_u */
        {
            u64 value = 0;
            if (!job_value_to_u64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f32)value, false);
            break;
        }
        case 0xA5: /* f32.convert_i64_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f32)value, false);
            break;
        }
        case 0xA6: /* f32.convert_i64_u */
        {
            u64 value = 0;
            if (!job_value_to_u64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f32)value, false);
            break;
        }
        case 0xA7: /* f32.demote_f64 */
        {
            f64 value = 0.0;
            if (!job_value_to_f64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f32)value, false);
            break;
        }
        case 0xA8: /* f64.convert_i32_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f64)value, true);
            break;
        }
        case 0xA9: /* f64.convert_i32_u */
        {
            u64 value = 0;
            if (!job_value_to_u64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f64)value, true);
            break;
        }
        case 0xAA: /* f64.convert_i64_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f64)value, true);
            break;
        }
        case 0xAB: /* f64.convert_i64_u */
        {
            u64 value = 0;
            if (!job_value_to_u64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f64)value, true);
            break;
        }
        case 0xAC: /* f64.promote_f32 */
        {
            f32 value = 0.0f;
            if (!job_value_to_f32(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            push_float_value(job, (f64)value, true);
            break;
        }
        case 0xC0: /* i32.extend8_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            i32 extended = (i32)(int8_t)(value & 0xFF);
            push_int_value(job, (u32)extended, 32U, true);
            break;
        }
        case 0xC1: /* i32.extend16_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            i32 extended = (i32)(int16_t)(value & 0xFFFF);
            push_int_value(job, (u32)extended, 32U, true);
            break;
        }
        case 0xC2: /* i64.extend8_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            i64 extended = (i64)(int8_t)(value & 0xFF);
            push_int_value(job, (u64)extended, 64U, true);
            break;
        }
        case 0xC3: /* i64.extend16_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            i64 extended = (i64)(int16_t)(value & 0xFFFF);
            push_int_value(job, (u64)extended, 64U, true);
            break;
        }
        case 0xC4: /* i64.extend32_s */
        {
            i64 value = 0;
            if (!job_value_to_i64(&source, &value)) {
                restore_stack_value(job, &source);
                return;
            }
            i64 extended = (i64)(int32_t)(value & 0xFFFFFFFFULL);
            push_int_value(job, (u64)extended, 64U, true);
            break;
        }
        default:
            restore_stack_value(job, &source);
            break;
    }
}

static OP_RETURN_TYPE op_reinterpret(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }
    fa_JobValue source;
    if (!pop_stack_value(job, &source)) {
        return;
    }

    switch (descriptor->id) {
        case 0xAD: /* i32.reinterpret_f32 */
        {
            u32 raw = 0;
            if (source.kind == fa_job_value_f32) {
                memcpy(&raw, &source.payload.f32_value, sizeof(raw));
            } else {
                f32 value = 0.0f;
                if (!job_value_to_f32(&source, &value)) {
                    restore_stack_value(job, &source);
                    return;
                }
                memcpy(&raw, &value, sizeof(raw));
            }
            push_int_value(job, raw, 32U, true);
            break;
        }
        case 0xAE: /* i64.reinterpret_f64 */
        {
            u64 raw = 0;
            if (source.kind == fa_job_value_f64) {
                memcpy(&raw, &source.payload.f64_value, sizeof(raw));
            } else {
                f64 value = 0.0;
                if (!job_value_to_f64(&source, &value)) {
                    restore_stack_value(job, &source);
                    return;
                }
                memcpy(&raw, &value, sizeof(raw));
            }
            push_int_value(job, raw, 64U, true);
            break;
        }
        case 0xAF: /* f32.reinterpret_i32 */
        {
            u64 raw64 = 0;
            if (!job_value_to_u64(&source, &raw64)) {
                restore_stack_value(job, &source);
                return;
            }
            const u32 raw = (u32)raw64;
            f32 value = 0.0f;
            memcpy(&value, &raw, sizeof(value));
            push_float_value(job, value, false);
            break;
        }
        case 0xB0: /* f64.reinterpret_i64 */
        {
            u64 raw = 0;
            if (!job_value_to_u64(&source, &raw)) {
                restore_stack_value(job, &source);
                return;
            }
            f64 value = 0.0;
            memcpy(&value, &raw, sizeof(value));
            push_float_value(job, value, true);
            break;
        }
        default:
            restore_stack_value(job, &source);
            break;
    }
}

static OP_RETURN_TYPE op_drop(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    fa_JobValue value;
    pop_stack_value(job, &value);
}

static OP_RETURN_TYPE op_select(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    fa_JobValue condition;
    if (!pop_stack_value(job, &condition)) {
        return;
    }
    fa_JobValue value_false;
    if (!pop_stack_value(job, &value_false)) {
        restore_stack_value(job, &condition);
        return;
    }
    fa_JobValue value_true;
    if (!pop_stack_value(job, &value_true)) {
        restore_stack_value(job, &value_false);
        restore_stack_value(job, &condition);
        return;
    }
    const bool truthy = job_value_truthy(&condition);
    const fa_JobValue* chosen = truthy ? &value_true : &value_false;
    fa_JobStack_push(&job->stack, chosen);
}

static OP_RETURN_TYPE op_call(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    u64 func_index = 0;
    pop_reg_u64(job, &func_index);
    job->instructionPointer = (fa_ptr)func_index;
}

static OP_RETURN_TYPE op_call_indirect(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    fa_JobValue table_index;
    if (pop_stack_value(job, &table_index)) {
        u64 ignored = 0;
        (void)job_value_to_u64(&table_index, &ignored);
    }
    u64 type_index = 0;
    pop_reg_u64(job, &type_index);
    u64 reserved = 0;
    pop_reg_u64(job, &reserved);
    job->instructionPointer = (fa_ptr)type_index;
}

static OP_RETURN_TYPE op_return(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    job->instructionPointer = 0;
}

static OP_RETURN_TYPE op_memory_size(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    push_int_value(job, 0, 32U, false);
}

static OP_RETURN_TYPE op_memory_grow(OP_ARGUMENTS) {
    if (!job) {
        return;
    }
    fa_JobValue delta;
    pop_stack_value(job, &delta);
    push_int_value(job, (u64)UINT32_MAX, 32U, true);
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

static void init_ops_once(void) {
    if (g_ops_initialized) {
        return;
    }
    fa_ops_defs_populate(g_ops);
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

OP_RETURN_TYPE fa_execute_op(uint8_t opcode, fa_Job* job) {
    const fa_WasmOp* op = fa_get_op(opcode);
    if (!op || !op->operation) {
        return;
    }
    op->operation(job, op);
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

    define_op(ops, 0x00, &type_void, wopt_unique, 0, 0, 0, 0, op_control);
    define_op(ops, 0x01, &type_void, wopt_unique, 0, 0, 0, 0, op_control);
    define_op(ops, 0x02, &type_void, wopt_unique, 0, 0, 0, 1, op_control);
    define_op(ops, 0x03, &type_void, wopt_unique, 0, 0, 0, 1, op_control);
    define_op(ops, 0x04, &type_void, wopt_unique, 0, 1, 0, 1, op_control);
    define_op(ops, 0x05, &type_void, wopt_unique, 0, 0, 0, 0, op_control);
    define_op(ops, 0x0B, &type_void, wopt_unique, 0, 0, 0, 0, op_control);
    define_op(ops, 0x0C, &type_void, wopt_unique, 0, 0, 0, 1, op_control);
    define_op(ops, 0x0D, &type_void, wopt_unique, 0, 1, 0, 1, op_control);
    define_op(ops, 0x0E, &type_void, wopt_unique, 0, 1, 0, 1, op_control);
    define_op(ops, 0x0F, &type_void, wopt_return, 0, 0, 0, 0, op_return);
    define_op(ops, 0x10, &type_void, wopt_call, 0, 0, 0, 1, op_call);
    define_op(ops, 0x11, &type_void, wopt_call, 0, 1, 0, 2, op_call_indirect);
    define_op(ops, 0x1A, &type_void, wopt_drop, 0, 1, 0, 0, op_drop);
    define_op(ops, 0x1B, &type_void, wopt_select, 0, 3, 1, 0, op_select);
    define_op(ops, 0x28, &type_i32, wopt_load, 32, 1, 1, 2, op_load);
    define_op(ops, 0x29, &type_i64, wopt_load, 64, 1, 1, 2, op_load);
    define_op(ops, 0x2A, &type_f32, wopt_load, 32, 1, 1, 2, op_load);
    define_op(ops, 0x2B, &type_f64, wopt_load, 64, 1, 1, 2, op_load);
    define_op(ops, 0x2C, &type_i32, wopt_load, 8, 1, 1, 2, op_load);
    define_op(ops, 0x2D, &type_u32, wopt_load, 8, 1, 1, 2, op_load);
    define_op(ops, 0x2E, &type_i32, wopt_load, 16, 1, 1, 2, op_load);
    define_op(ops, 0x2F, &type_u32, wopt_load, 16, 1, 1, 2, op_load);
    define_op(ops, 0x30, &type_i64, wopt_load, 8, 1, 1, 2, op_load);
    define_op(ops, 0x31, &type_u64, wopt_load, 8, 1, 1, 2, op_load);
    define_op(ops, 0x32, &type_i64, wopt_load, 16, 1, 1, 2, op_load);
    define_op(ops, 0x33, &type_u64, wopt_load, 16, 1, 1, 2, op_load);
    define_op(ops, 0x34, &type_i64, wopt_load, 32, 1, 1, 2, op_load);
    define_op(ops, 0x35, &type_u64, wopt_load, 32, 1, 1, 2, op_load);
    define_op(ops, 0x36, &type_i32, wopt_store, 32, 2, 0, 2, op_store);
    define_op(ops, 0x37, &type_i64, wopt_store, 64, 2, 0, 2, op_store);
    define_op(ops, 0x38, &type_f32, wopt_store, 32, 2, 0, 2, op_store);
    define_op(ops, 0x39, &type_f64, wopt_store, 64, 2, 0, 2, op_store);
    define_op(ops, 0x3A, &type_i32, wopt_store, 8, 2, 0, 2, op_store);
    define_op(ops, 0x3B, &type_i32, wopt_store, 16, 2, 0, 2, op_store);
    define_op(ops, 0x3C, &type_i64, wopt_store, 8, 2, 0, 2, op_store);
    define_op(ops, 0x3D, &type_i64, wopt_store, 16, 2, 0, 2, op_store);
    define_op(ops, 0x3E, &type_i64, wopt_store, 32, 2, 0, 2, op_store);
    define_op(ops, 0x41, &type_i32, wopt_const, 32, 0, 1, 1, op_const);
    define_op(ops, 0x42, &type_i64, wopt_const, 64, 0, 1, 1, op_const);
    define_op(ops, 0x43, &type_f32, wopt_const, 32, 0, 1, 1, op_const);
    define_op(ops, 0x44, &type_f64, wopt_const, 64, 0, 1, 1, op_const);
    define_op(ops, 0x45, &type_i32, wopt_eqz, 0, 1, 1, 0, op_eqz);
    define_op(ops, 0x50, &type_i64, wopt_eqz, 0, 1, 1, 0, op_eqz);
    define_op(ops, 0x46, &type_i32, wopt_eq, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x47, &type_i32, wopt_ne, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x48, &type_i32, wopt_lt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x49, &type_u32, wopt_lt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x4A, &type_i32, wopt_gt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x4B, &type_u32, wopt_gt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x4C, &type_i32, wopt_le, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x4D, &type_u32, wopt_le, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x4E, &type_i32, wopt_ge, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x4F, &type_u32, wopt_ge, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x51, &type_i64, wopt_eq, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x52, &type_i64, wopt_ne, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x53, &type_i64, wopt_lt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x54, &type_u64, wopt_lt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x55, &type_i64, wopt_gt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x56, &type_u64, wopt_gt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x57, &type_i64, wopt_le, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x58, &type_u64, wopt_le, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x59, &type_i64, wopt_ge, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x5A, &type_u64, wopt_ge, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x5B, &type_f32, wopt_eq, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x5C, &type_f32, wopt_ne, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x5D, &type_f32, wopt_lt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x5E, &type_f32, wopt_gt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x5F, &type_f32, wopt_le, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x60, &type_f32, wopt_ge, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x61, &type_f64, wopt_eq, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x62, &type_f64, wopt_ne, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x63, &type_f64, wopt_lt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x64, &type_f64, wopt_gt, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x65, &type_f64, wopt_le, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x66, &type_f64, wopt_ge, 0, 2, 1, 0, op_compare);
    define_op(ops, 0x6A, &type_i32, wopt_add, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x6B, &type_i32, wopt_sub, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x6C, &type_i32, wopt_mul, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x6D, &type_i32, wopt_div, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x6E, &type_u32, wopt_div, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x6F, &type_i32, wopt_rem, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x70, &type_u32, wopt_rem, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x79, &type_i64, wopt_add, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x7A, &type_i64, wopt_sub, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x7B, &type_i64, wopt_mul, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x7C, &type_i64, wopt_div, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x7D, &type_u64, wopt_div, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x7E, &type_i64, wopt_rem, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x7F, &type_u64, wopt_rem, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x8A, &type_f32, wopt_add, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x8B, &type_f32, wopt_sub, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x8C, &type_f32, wopt_mul, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x8D, &type_f32, wopt_div, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x91, &type_f64, wopt_add, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x92, &type_f64, wopt_sub, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x93, &type_f64, wopt_mul, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x94, &type_f64, wopt_div, 0, 2, 1, 0, op_arithmetic);
    define_op(ops, 0x71, &type_i32, wopt_and, 0, 2, 1, 0, op_bitwise);
    define_op(ops, 0x72, &type_i32, wopt_or, 0, 2, 1, 0, op_bitwise);
    define_op(ops, 0x73, &type_i32, wopt_xor, 0, 2, 1, 0, op_bitwise);
    define_op(ops, 0x80, &type_i64, wopt_and, 0, 2, 1, 0, op_bitwise);
    define_op(ops, 0x81, &type_i64, wopt_or, 0, 2, 1, 0, op_bitwise);
    define_op(ops, 0x82, &type_i64, wopt_xor, 0, 2, 1, 0, op_bitwise);
    define_op(ops, 0x74, &type_i32, wopt_shl, 0, 2, 1, 0, op_shift);
    define_op(ops, 0x75, &type_i32, wopt_shr, 0, 2, 1, 0, op_shift);
    define_op(ops, 0x76, &type_u32, wopt_shr, 0, 2, 1, 0, op_shift);
    define_op(ops, 0x83, &type_i64, wopt_shl, 0, 2, 1, 0, op_shift);
    define_op(ops, 0x84, &type_i64, wopt_shr, 0, 2, 1, 0, op_shift);
    define_op(ops, 0x85, &type_u64, wopt_shr, 0, 2, 1, 0, op_shift);
    define_op(ops, 0x77, &type_i32, wopt_rotl, 0, 2, 1, 0, op_rotate);
    define_op(ops, 0x78, &type_i32, wopt_rotr, 0, 2, 1, 0, op_rotate);
    define_op(ops, 0x86, &type_i64, wopt_rotl, 0, 2, 1, 0, op_rotate);
    define_op(ops, 0x87, &type_i64, wopt_rotr, 0, 2, 1, 0, op_rotate);
    define_op(ops, 0x8E, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_binary_special);
    define_op(ops, 0x8F, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_binary_special);
    define_op(ops, 0x90, &type_f32, wopt_unique, 0, 2, 1, 0, op_float_binary_special);
    define_op(ops, 0x95, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_binary_special);
    define_op(ops, 0x96, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_binary_special);
    define_op(ops, 0x97, &type_f64, wopt_unique, 0, 2, 1, 0, op_float_binary_special);
    define_op(ops, 0x98, &type_i32, wopt_wrap, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x99, &type_i32, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x9A, &type_u32, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x9B, &type_i32, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x9C, &type_u32, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x9D, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x9E, &type_u64, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0x9F, &type_i64, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA0, &type_u64, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA1, &type_i64, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA2, &type_u64, wopt_trunc, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA3, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA4, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA5, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA6, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA7, &type_f32, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA8, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xA9, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xAA, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xAB, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xAC, &type_f64, wopt_convert, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xC0, &type_i32, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xC1, &type_i32, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xC2, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xC3, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xC4, &type_i64, wopt_extend, 0, 1, 1, 0, op_convert);
    define_op(ops, 0xAD, &type_i32, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret);
    define_op(ops, 0xAE, &type_i64, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret);
    define_op(ops, 0xAF, &type_f32, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret);
    define_op(ops, 0xB0, &type_f64, wopt_reinterpret, 0, 1, 1, 0, op_reinterpret);
    define_op(ops, 0x3F, &type_i32, wopt_unique, 0, 0, 1, 1, op_memory_size);
    define_op(ops, 0x40, &type_i32, wopt_unique, 0, 1, 1, 1, op_memory_grow);
}
