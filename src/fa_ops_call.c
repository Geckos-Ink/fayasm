#include "fa_ops_internal.h"

/* Direct calls, indirect calls, returns, and table access operations. */

OP_RETURN_TYPE op_call(OP_ARGUMENTS) {
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

static bool runtime_function_signature_matches(const WasmModule* module, uint32_t function_index, uint32_t type_index) {
    if (!module || !module->functions || !module->types ||
        function_index >= module->num_functions || type_index >= module->num_types) {
        return false;
    }
    const uint32_t function_type_index = module->functions[function_index].type_index;
    if (function_type_index >= module->num_types) {
        return false;
    }
    if (function_type_index == type_index) {
        return true;
    }
    const WasmFunctionType* expected = &module->types[type_index];
    const WasmFunctionType* actual = &module->types[function_type_index];
    if (expected->num_params != actual->num_params || expected->num_results != actual->num_results) {
        return false;
    }
    if (expected->num_params > 0 &&
        memcmp(expected->param_types, actual->param_types, expected->num_params * sizeof(uint32_t)) != 0) {
        return false;
    }
    if (expected->num_results > 0 &&
        memcmp(expected->result_types, actual->result_types, expected->num_results * sizeof(uint32_t)) != 0) {
        return false;
    }
    return true;
}

OP_RETURN_TYPE op_call_indirect(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !runtime->module || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    uint32_t callee_slot = 0;
    if (pop_u32_checked(job, &callee_slot) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }

    u64 table_index = 0;
    if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    if (!table || table->elem_type != VALTYPE_FUNCREF) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (callee_slot >= table->size) {
        return FA_RUNTIME_ERR_TRAP;
    }

    u64 type_index = 0;
    if (pop_reg_u64_checked(job, &type_index) != FA_RUNTIME_OK || type_index > UINT32_MAX) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint32_t type_u32 = (uint32_t)type_index;

    const fa_ptr table_ref = table->data[callee_slot];
    uint32_t function_index = 0;
    if (runtime_decode_funcref_index(runtime, table_ref, &function_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime_function_signature_matches(runtime->module, function_index, type_u32)) {
        return FA_RUNTIME_ERR_TRAP;
    }

    job->instructionPointer = (fa_ptr)function_index;
    return FA_RUNTIME_OK;
}

OP_RETURN_TYPE op_return(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    job->instructionPointer = 0;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_table_get(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
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
    uint32_t index = 0;
    if (pop_u32_checked(job, &index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (index >= table->size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_ptr ref_value = table->data[index];
    if (runtime_validate_table_ref_value(runtime, table, ref_value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_ref_checked(job, ref_value);
}

static OP_RETURN_TYPE op_table_set(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
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
    if (runtime_validate_table_ref_value(runtime, table, ref_value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    table->data[index] = ref_value;
    return FA_RUNTIME_OK;
}

OP_RETURN_TYPE op_table(OP_ARGUMENTS) {
    static const Operation kTableHandlers[256] = {
        [0x25] = op_table_get,
        [0x26] = op_table_set
    };
    if (!runtime || !job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    Operation handler = kTableHandlers[descriptor->id];
    if (!handler) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return handler(runtime, job, descriptor);
}
