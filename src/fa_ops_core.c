#include "fa_ops_internal.h"

/* Control, variable, scalar memory, constant, and reference operations. */

static OP_RETURN_TYPE op_control_unreachable(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobStack_reset(&job->stack);
    job->instructionPointer = 0;
    return FA_RUNTIME_ERR_TRAP;
}

static OP_RETURN_TYPE op_control_nop(OP_ARGUMENTS) {
    (void)runtime;
    (void)job;
    (void)descriptor;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_control_block(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    discard_reg_arguments(job, descriptor->num_args);
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_control_if(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    discard_reg_arguments(job, descriptor->num_args);
    fa_JobValue cond;
    if (pop_stack_checked(job, &cond) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    (void)job_value_truthy(&cond);
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_control_br(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 label = 0;
    if (pop_reg_u64_checked(job, &label) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    job->instructionPointer = (fa_ptr)label;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_control_br_if(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
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

static OP_RETURN_TYPE op_control_br_table(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
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

static OP_RETURN_TYPE op_control_return(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    job->instructionPointer = 0;
    return FA_RUNTIME_OK;
}

OP_RETURN_TYPE op_control(OP_ARGUMENTS) {
    static const Operation kControlHandlers[256] = {
        [0x00] = op_control_unreachable,
        [0x01] = op_control_nop,
        [0x02] = op_control_block,
        [0x03] = op_control_block,
        [0x04] = op_control_if,
        [0x05] = op_control_nop,
        [0x0B] = op_control_nop,
        [0x0C] = op_control_br,
        [0x0D] = op_control_br_if,
        [0x0E] = op_control_br_table,
        [0x0F] = op_control_return
    };
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    Operation handler = kControlHandlers[descriptor->id];
    if (!handler) {
        return FA_RUNTIME_OK;
    }
    return handler(runtime, job, descriptor);
}

static OP_RETURN_TYPE op_local_get(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->active_locals || index >= runtime->active_locals_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const fa_JobValue value = runtime->active_locals[index];
    return fa_JobStack_push(&job->stack, &value) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

static OP_RETURN_TYPE op_local_set(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->active_locals || index >= runtime->active_locals_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    runtime->active_locals[index] = value;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_local_tee(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->active_locals || index >= runtime->active_locals_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
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

OP_RETURN_TYPE op_local(OP_ARGUMENTS) {
    static const Operation kLocalHandlers[256] = {
        [0x20] = op_local_get,
        [0x21] = op_local_set,
        [0x22] = op_local_tee
    };
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    Operation handler = kLocalHandlers[descriptor->id];
    if (!handler) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return handler(runtime, job, descriptor);
}

static OP_RETURN_TYPE op_global_get(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job || !runtime->module || !runtime->module->globals) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->globals || index >= runtime->globals_count || index >= runtime->module->num_globals) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const WasmGlobal* global = &runtime->module->globals[index];
    const fa_JobValue value = runtime->globals[index];
    if (!job_value_matches_valtype(&value, global->valtype)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return fa_JobStack_push(&job->stack, &value) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
}

static OP_RETURN_TYPE op_global_set(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job || !runtime->module || !runtime->module->globals) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 index = 0;
    if (pop_reg_u64_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime->globals || index >= runtime->globals_count || index >= runtime->module->num_globals) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const WasmGlobal* global = &runtime->module->globals[index];
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!global->is_mutable || !job_value_matches_valtype(&value, global->valtype)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    runtime->globals[index] = value;
    return FA_RUNTIME_OK;
}

OP_RETURN_TYPE op_global(OP_ARGUMENTS) {
    static const Operation kGlobalHandlers[256] = {
        [0x23] = op_global_get,
        [0x24] = op_global_set
    };
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    Operation handler = kGlobalHandlers[descriptor->id];
    if (!handler) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return handler(runtime, job, descriptor);
}

OP_RETURN_TYPE op_load(OP_ARGUMENTS) {
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

    fa_RuntimeMemory* memory = NULL;
    int status = runtime_require_memory(runtime, mem_index, &memory);
    if (status != FA_RUNTIME_OK) {
        return status;
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

OP_RETURN_TYPE op_store(OP_ARGUMENTS) {
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

    fa_RuntimeMemory* memory = NULL;
    int status = runtime_require_memory(runtime, mem_index, &memory);
    if (status != FA_RUNTIME_OK) {
        restore_stack_value(job, &value);
        return status;
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

OP_RETURN_TYPE op_const(OP_ARGUMENTS) {
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

OP_RETURN_TYPE op_eqz(OP_ARGUMENTS) {
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

/*
 * Reference opcodes keep funcref encoding centralized:
 * - null is 0
 * - function index n is encoded as n + 1
 */
static OP_RETURN_TYPE op_ref_null(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 ref_type = 0;
    if (pop_reg_u64_checked(job, &ref_type) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (ref_type != VALTYPE_FUNCREF && ref_type != VALTYPE_EXTERNREF) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_ref_checked(job, (fa_ptr)0);
}

static OP_RETURN_TYPE op_ref_is_null(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_ptr value = 0;
    if (pop_ref_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_bool_checked(job, value == 0);
}

static OP_RETURN_TYPE op_ref_func(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job || !runtime->module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 func_index = 0;
    if (pop_reg_u64_checked(job, &func_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (func_index > UINT32_MAX || func_index >= runtime->module->num_functions) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_ptr encoded_ref = 0;
    if (!fa_funcref_encode_u32((uint32_t)func_index, &encoded_ref)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_ref_checked(job, encoded_ref);
}

OP_RETURN_TYPE op_ref(OP_ARGUMENTS) {
    static const Operation kRefHandlers[256] = {
        [0xD0] = op_ref_null,
        [0xD1] = op_ref_is_null,
        [0xD2] = op_ref_func
    };
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    Operation handler = kRefHandlers[descriptor->id];
    if (!handler) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return handler(runtime, job, descriptor);
}
