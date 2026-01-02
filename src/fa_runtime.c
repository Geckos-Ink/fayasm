#define LIST_IMPLEMENTATION
#include "fa_runtime.h"
#include "fa_ops.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    uint32_t func_index;
    uint8_t* body;
    uint32_t body_size;
    uint32_t pc;
    uint32_t code_start;
    fa_JobValue* locals;
    uint32_t locals_count;
    struct fa_RuntimeControlFrame* control_stack;
    uint32_t control_depth;
    uint32_t control_capacity;
} fa_RuntimeCallFrame;

typedef enum {
    FA_CONTROL_BLOCK = 0,
    FA_CONTROL_LOOP,
    FA_CONTROL_IF
} fa_RuntimeControlType;

typedef struct fa_RuntimeControlFrame {
    fa_RuntimeControlType type;
    uint32_t start_pc;
    uint32_t else_pc;
    uint32_t end_pc;
} fa_RuntimeControlFrame;

static ptr fa_default_malloc(int size) {
    return malloc((size_t)size);
}

static void fa_default_free(ptr region) {
    free(region);
}

static void runtime_memory_reset(fa_Runtime* runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->memory.data) {
        runtime->free(runtime->memory.data);
        runtime->memory.data = NULL;
    }
    runtime->memory.size_bytes = 0;
    runtime->memory.max_size_bytes = 0;
    runtime->memory.has_max = false;
    runtime->memory.is_memory64 = false;
}

static void runtime_globals_reset(fa_Runtime* runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->globals) {
        free(runtime->globals);
        runtime->globals = NULL;
    }
    runtime->globals_count = 0;
}

static int runtime_init_value_from_valtype(fa_JobValue* out, uint32_t valtype);

static int runtime_init_globals(fa_Runtime* runtime, const WasmModule* module) {
    if (!runtime || !module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    runtime_globals_reset(runtime);
    if (module->num_globals == 0) {
        return FA_RUNTIME_OK;
    }
    if (!module->globals) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue* globals = (fa_JobValue*)calloc(module->num_globals, sizeof(fa_JobValue));
    if (!globals) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    runtime->globals = globals;
    runtime->globals_count = module->num_globals;

    for (uint32_t i = 0; i < module->num_globals; ++i) {
        const WasmGlobal* global = &module->globals[i];
        fa_JobValue value;
        int status = runtime_init_value_from_valtype(&value, global->valtype);
        if (status != FA_RUNTIME_OK) {
            runtime_globals_reset(runtime);
            return status;
        }
        switch (global->valtype) {
            case VALTYPE_I32:
                value.payload.i32_value = (i32)global->init_raw;
                break;
            case VALTYPE_I64:
                value.payload.i64_value = (i64)global->init_raw;
                break;
            case VALTYPE_F32:
            {
                u32 raw = (u32)global->init_raw;
                memcpy(&value.payload.f32_value, &raw, sizeof(raw));
                break;
            }
            case VALTYPE_F64:
            {
                u64 raw = global->init_raw;
                memcpy(&value.payload.f64_value, &raw, sizeof(raw));
                break;
            }
            case VALTYPE_FUNCREF:
            case VALTYPE_EXTERNREF:
                value.payload.ref_value = (fa_ptr)global->init_raw;
                break;
            default:
                runtime_globals_reset(runtime);
                return FA_RUNTIME_ERR_UNSUPPORTED;
        }
        runtime->globals[i] = value;
    }
    return FA_RUNTIME_OK;
}

static int runtime_memory_init(fa_Runtime* runtime, const WasmModule* module) {
    if (!runtime || !module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    runtime_memory_reset(runtime);
    if (module->num_memories == 0 || !module->memories) {
        return FA_RUNTIME_OK;
    }
    const WasmMemory* memory = &module->memories[0];
    runtime->memory.is_memory64 = memory->is_memory64;
    runtime->memory.has_max = memory->has_max;
    if (memory->is_memory64) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    if (memory->initial_size > UINT32_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    if (memory->has_max && memory->maximum_size > UINT32_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    const uint64_t max_pages = memory->has_max ? memory->maximum_size : 0;
    if (memory->has_max) {
        if (max_pages > (UINT64_MAX / FA_WASM_PAGE_SIZE)) {
            return FA_RUNTIME_ERR_UNSUPPORTED;
        }
        runtime->memory.max_size_bytes = max_pages * FA_WASM_PAGE_SIZE;
    }

    if (memory->initial_size == 0) {
        runtime->memory.size_bytes = 0;
        return FA_RUNTIME_OK;
    }
    if (memory->initial_size > (UINT64_MAX / FA_WASM_PAGE_SIZE)) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    const uint64_t size_bytes = memory->initial_size * FA_WASM_PAGE_SIZE;
    if (size_bytes > SIZE_MAX || size_bytes > (uint64_t)INT_MAX) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    uint8_t* data = (uint8_t*)runtime->malloc((int)size_bytes);
    if (!data) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    memset(data, 0, (size_t)size_bytes);
    runtime->memory.data = data;
    runtime->memory.size_bytes = size_bytes;
    return FA_RUNTIME_OK;
}

static void runtime_job_reg_clear(fa_Job* job) {
    if (!job) {
        return;
    }
    fa_JobDataFlow* node = job->reg;
    while (node) {
        fa_JobDataFlow* prev = node->precede;
        free(node);
        node = prev;
    }
    job->reg = NULL;
}

static int runtime_control_reserve(fa_RuntimeCallFrame* frame, uint32_t count) {
    if (!frame) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (count <= frame->control_capacity) {
        return FA_RUNTIME_OK;
    }
    uint32_t next_capacity = frame->control_capacity ? frame->control_capacity * 2U : 8U;
    while (next_capacity < count) {
        next_capacity *= 2U;
    }
    fa_RuntimeControlFrame* next = (fa_RuntimeControlFrame*)realloc(frame->control_stack, next_capacity * sizeof(fa_RuntimeControlFrame));
    if (!next) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    frame->control_stack = next;
    frame->control_capacity = next_capacity;
    return FA_RUNTIME_OK;
}

static int runtime_control_push(fa_RuntimeCallFrame* frame, fa_RuntimeControlType type, uint32_t start_pc, uint32_t else_pc, uint32_t end_pc) {
    if (!frame) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    const uint32_t next_depth = frame->control_depth + 1U;
    int status = runtime_control_reserve(frame, next_depth);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    fa_RuntimeControlFrame* entry = &frame->control_stack[frame->control_depth];
    entry->type = type;
    entry->start_pc = start_pc;
    entry->else_pc = else_pc;
    entry->end_pc = end_pc;
    frame->control_depth = next_depth;
    return FA_RUNTIME_OK;
}

static fa_RuntimeControlFrame* runtime_control_peek(fa_RuntimeCallFrame* frame, uint32_t label_depth) {
    if (!frame || label_depth >= frame->control_depth) {
        return NULL;
    }
    return &frame->control_stack[frame->control_depth - 1U - label_depth];
}

static void runtime_control_pop_to(fa_RuntimeCallFrame* frame, uint32_t label_depth, bool keep_target) {
    if (!frame || label_depth >= frame->control_depth) {
        return;
    }
    const uint32_t target_index = frame->control_depth - 1U - label_depth;
    frame->control_depth = keep_target ? (target_index + 1U) : target_index;
}

static void runtime_control_pop_one(fa_RuntimeCallFrame* frame) {
    if (!frame || frame->control_depth == 0) {
        return;
    }
    frame->control_depth -= 1U;
}

static int runtime_init_value_from_valtype(fa_JobValue* out, uint32_t valtype) {
    if (!out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    switch (valtype) {
        case VALTYPE_I32:
            out->kind = fa_job_value_i32;
            out->bit_width = 32U;
            out->is_signed = true;
            out->payload.i32_value = 0;
            return FA_RUNTIME_OK;
        case VALTYPE_I64:
            out->kind = fa_job_value_i64;
            out->bit_width = 64U;
            out->is_signed = true;
            out->payload.i64_value = 0;
            return FA_RUNTIME_OK;
        case VALTYPE_F32:
            out->kind = fa_job_value_f32;
            out->bit_width = 32U;
            out->is_signed = false;
            out->payload.f32_value = 0.0f;
            return FA_RUNTIME_OK;
        case VALTYPE_F64:
            out->kind = fa_job_value_f64;
            out->bit_width = 64U;
            out->is_signed = false;
            out->payload.f64_value = 0.0;
            return FA_RUNTIME_OK;
        case VALTYPE_FUNCREF:
        case VALTYPE_EXTERNREF:
            out->kind = fa_job_value_ref;
            out->bit_width = (uint8_t)(sizeof(fa_ptr) * 8U);
            out->is_signed = false;
            out->payload.ref_value = 0;
            return FA_RUNTIME_OK;
        default:
            return FA_RUNTIME_ERR_UNSUPPORTED;
    }
}

static void fa_Runtime_reset_job_state(fa_Job* job) {
    if (!job) {
        return;
    }
    fa_JobStack_reset(&job->stack);
    runtime_job_reg_clear(job);
    job->instructionPointer = 0;
}

static fa_RuntimeCallFrame* runtime_alloc_frames(fa_Runtime* runtime) {
    if (!runtime) {
        return NULL;
    }
    const size_t capacity = runtime->max_call_depth ? runtime->max_call_depth : 64U;
    return (fa_RuntimeCallFrame*)calloc(capacity, sizeof(fa_RuntimeCallFrame));
}

static void runtime_free_frames(fa_Runtime* runtime, fa_RuntimeCallFrame* frames) {
    (void)runtime;
    free(frames);
}

static void runtime_free_frame_resources(fa_RuntimeCallFrame* frame) {
    if (!frame) {
        return;
    }
    if (frame->body) {
        free(frame->body);
        frame->body = NULL;
    }
    if (frame->locals) {
        free(frame->locals);
        frame->locals = NULL;
    }
    if (frame->control_stack) {
        free(frame->control_stack);
        frame->control_stack = NULL;
    }
    frame->body_size = 0;
    frame->pc = 0;
    frame->code_start = 0;
    frame->locals_count = 0;
    frame->control_depth = 0;
    frame->control_capacity = 0;
}

static int runtime_read_uleb128(const uint8_t* buffer, uint32_t buffer_size, uint32_t* cursor, uint64_t* out) {
    if (!buffer || !cursor || !out || *cursor >= buffer_size) {
        return FA_RUNTIME_ERR_STREAM;
    }
    uint64_t result = 0;
    uint32_t shift = 0;
    uint32_t index = *cursor;
    while (index < buffer_size) {
        uint8_t byte = buffer[index++];
        result |= ((uint64_t)(byte & 0x7F) << shift);
        if ((byte & 0x80U) == 0U) {
            *cursor = index;
            *out = result;
            return FA_RUNTIME_OK;
        }
        shift += 7U;
        if (shift >= 64U) {
            return FA_RUNTIME_ERR_STREAM;
        }
    }
    return FA_RUNTIME_ERR_STREAM;
}

static int runtime_read_sleb128(const uint8_t* buffer, uint32_t buffer_size, uint32_t* cursor, int64_t* out) {
    if (!buffer || !cursor || !out || *cursor >= buffer_size) {
        return FA_RUNTIME_ERR_STREAM;
    }
    int64_t result = 0;
    uint32_t shift = 0;
    uint32_t index = *cursor;
    uint8_t byte = 0;
    while (index < buffer_size) {
        byte = buffer[index++];
        result |= ((int64_t)(byte & 0x7F) << shift);
        shift += 7U;
        if ((byte & 0x80U) == 0U) {
            break;
        }
        if (shift >= 64U) {
            return FA_RUNTIME_ERR_STREAM;
        }
    }
    if (index > buffer_size) {
        return FA_RUNTIME_ERR_STREAM;
    }
    if ((shift < 64U) && (byte & 0x40U)) {
        result |= (~0LL) << shift;
    }
    *cursor = index;
    *out = result;
    return FA_RUNTIME_OK;
}

static int runtime_skip_immediates(const uint8_t* body, uint32_t body_size, uint32_t* cursor, uint8_t opcode) {
    if (!body || !cursor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    uint64_t uleb = 0;
    int64_t sleb = 0;
    switch (opcode) {
        case 0x0C: /* br */
        case 0x0D: /* br_if */
        case 0x10: /* call */
        case 0x20: /* local.get */
        case 0x21: /* local.set */
        case 0x22: /* local.tee */
        case 0x23: /* global.get */
        case 0x24: /* global.set */
        case 0x3F: /* memory.size */
        case 0x40: /* memory.grow */
            return runtime_read_uleb128(body, body_size, cursor, &uleb);
        case 0x0E: /* br_table */
        {
            int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            const uint64_t count = uleb;
            for (uint64_t i = 0; i < count; ++i) {
                status = runtime_read_uleb128(body, body_size, cursor, &uleb);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
            }
            return runtime_read_uleb128(body, body_size, cursor, &uleb);
        }
        case 0x11: /* call_indirect */
        {
            int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            return runtime_read_uleb128(body, body_size, cursor, &uleb);
        }
        case 0x41: /* i32.const */
            return runtime_read_sleb128(body, body_size, cursor, &sleb);
        case 0x42: /* i64.const */
            return runtime_read_sleb128(body, body_size, cursor, &sleb);
        case 0x43: /* f32.const */
            if (*cursor + 4U > body_size) {
                return FA_RUNTIME_ERR_STREAM;
            }
            *cursor += 4U;
            return FA_RUNTIME_OK;
        case 0x44: /* f64.const */
            if (*cursor + 8U > body_size) {
                return FA_RUNTIME_ERR_STREAM;
            }
            *cursor += 8U;
            return FA_RUNTIME_OK;
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E:
        {
            int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            return runtime_read_uleb128(body, body_size, cursor, &uleb);
        }
        default:
            return FA_RUNTIME_OK;
    }
}

static int runtime_scan_block(const uint8_t* body,
                              uint32_t body_size,
                              uint32_t start_pc,
                              uint32_t* else_pc_out,
                              uint32_t* end_pc_out) {
    if (!body || !else_pc_out || !end_pc_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    uint32_t cursor = start_pc;
    uint32_t depth = 1;
    uint32_t else_pc = 0;
    while (cursor < body_size) {
        uint8_t opcode = body[cursor++];
        switch (opcode) {
            case 0x02: /* block */
            case 0x03: /* loop */
            case 0x04: /* if */
            {
                int64_t block_type = 0;
                int status = runtime_read_sleb128(body, body_size, &cursor, &block_type);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
                depth += 1;
                break;
            }
            case 0x05: /* else */
                if (depth == 1 && else_pc == 0) {
                    else_pc = cursor;
                }
                break;
            case 0x0B: /* end */
                if (depth > 0) {
                    depth -= 1;
                }
                if (depth == 0) {
                    *else_pc_out = else_pc;
                    *end_pc_out = cursor;
                    return FA_RUNTIME_OK;
                }
                break;
            default:
            {
                int status = runtime_skip_immediates(body, body_size, &cursor, opcode);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
                break;
            }
        }
    }
    return FA_RUNTIME_ERR_STREAM;
}

static bool runtime_job_value_truthy(const fa_JobValue* value) {
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

static bool runtime_job_value_to_u64(const fa_JobValue* value, u64* out) {
    if (!value || !out) {
        return false;
    }
    switch (value->kind) {
        case fa_job_value_i32:
            *out = (u64)(u32)value->payload.i32_value;
            return true;
        case fa_job_value_i64:
            *out = (u64)value->payload.i64_value;
            return true;
        case fa_job_value_f32:
            *out = (u64)value->payload.f32_value;
            return true;
        case fa_job_value_f64:
            *out = (u64)value->payload.f64_value;
            return true;
        case fa_job_value_ref:
            *out = value->payload.ref_value;
            return true;
        default:
            return false;
    }
}

static int runtime_pop_stack_checked(fa_Job* job, fa_JobValue* out) {
    if (!job || !out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    return fa_JobStack_pop(&job->stack, out) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_TRAP;
}

static int runtime_push_reg_value(fa_Job* job, const void* data, size_t size) {
    if (!job || !data || size == 0 || size > FA_JOB_DATA_FLOW_MAX_SIZE) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobDataFlow* node = (fa_JobDataFlow*)calloc(1, sizeof(fa_JobDataFlow) + size);
    if (!node) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    node->size = (uint8_t)size;
    node->ptr = (void*)((uint8_t*)(node + 1));
    memcpy(node->ptr, data, size);

    node->follows = NULL;
    if (!job->reg) {
        node->precede = NULL;
        job->reg = node;
    } else {
        node->precede = job->reg;
        job->reg->follows = node;
        job->reg = node;
    }

    // enforce window size
    uint32_t count = 0;
    fa_JobDataFlow* head = NULL;
    for (fa_JobDataFlow* cur = job->reg; cur; cur = cur->precede) {
        head = cur;
        ++count;
    }
    while (count > FA_JOB_DATA_FLOW_WINDOW_SIZE && head) {
        fa_JobDataFlow* next = head->follows;
        if (next) {
            next->precede = NULL;
        } else {
            job->reg = NULL;
        }
        free(head);
        head = next;
        --count;
    }
    return FA_RUNTIME_OK;
}

static int runtime_parse_locals(fa_Runtime* runtime, fa_RuntimeCallFrame* frame) {
    if (!frame || !frame->body) {
        return FA_RUNTIME_ERR_STREAM;
    }
    uint32_t cursor = 0;
    uint64_t local_decl_count = 0;
    int status = runtime_read_uleb128(frame->body, frame->body_size, &cursor, &local_decl_count);
    if (status != FA_RUNTIME_OK) {
        return status;
    }

    uint64_t* decl_counts = NULL;
    uint8_t* decl_types = NULL;
    if (local_decl_count > 0) {
        decl_counts = (uint64_t*)calloc(local_decl_count, sizeof(uint64_t));
        decl_types = (uint8_t*)calloc(local_decl_count, sizeof(uint8_t));
        if (!decl_counts || !decl_types) {
            free(decl_counts);
            free(decl_types);
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
    }

    for (uint64_t i = 0; i < local_decl_count; ++i) {
        uint64_t repeat = 0;
        status = runtime_read_uleb128(frame->body, frame->body_size, &cursor, &repeat);
        if (status != FA_RUNTIME_OK) {
            goto cleanup_decl;
        }
        if (cursor >= frame->body_size) {
            status = FA_RUNTIME_ERR_STREAM;
            goto cleanup_decl;
        }
        uint8_t valtype = frame->body[cursor++];
        decl_counts[i] = repeat;
        decl_types[i] = valtype;
    }

    uint32_t param_count = 0;
    uint32_t type_index = 0;
    if (runtime && runtime->module) {
        if (frame->func_index >= runtime->module->num_functions) {
            status = FA_RUNTIME_ERR_INVALID_ARGUMENT;
            goto cleanup_decl;
        }
        type_index = runtime->module->functions[frame->func_index].type_index;
        if (type_index >= runtime->module->num_types) {
            status = FA_RUNTIME_ERR_INVALID_ARGUMENT;
            goto cleanup_decl;
        }
        param_count = runtime->module->types[type_index].num_params;
    }

    uint64_t total_locals = param_count;
    for (uint64_t i = 0; i < local_decl_count; ++i) {
        if (decl_counts[i] > UINT32_MAX - total_locals) {
            status = FA_RUNTIME_ERR_UNSUPPORTED;
            goto cleanup_decl;
        }
        total_locals += decl_counts[i];
    }
    if (total_locals > UINT32_MAX) {
        status = FA_RUNTIME_ERR_UNSUPPORTED;
        goto cleanup_decl;
    }

    fa_JobValue* locals = NULL;
    if (total_locals > 0) {
        locals = (fa_JobValue*)calloc((size_t)total_locals, sizeof(fa_JobValue));
        if (!locals) {
            status = FA_RUNTIME_ERR_OUT_OF_MEMORY;
            goto cleanup_decl;
        }
    }

    uint32_t local_index = 0;
    if (runtime && runtime->module) {
        for (uint32_t i = 0; i < param_count; ++i) {
            status = runtime_init_value_from_valtype(&locals[local_index], runtime->module->types[type_index].param_types[i]);
            if (status != FA_RUNTIME_OK) {
                free(locals);
                goto cleanup_decl;
            }
            local_index++;
        }
    }

    for (uint64_t i = 0; i < local_decl_count; ++i) {
        for (uint64_t j = 0; j < decl_counts[i]; ++j) {
            status = runtime_init_value_from_valtype(&locals[local_index], decl_types[i]);
            if (status != FA_RUNTIME_OK) {
                free(locals);
                goto cleanup_decl;
            }
            local_index++;
        }
    }

    frame->locals = locals;
    frame->locals_count = (uint32_t)total_locals;
    frame->code_start = cursor;
    frame->pc = cursor;
    status = FA_RUNTIME_OK;

cleanup_decl:
    free(decl_counts);
    free(decl_types);
    return status;
}

static int runtime_push_frame(fa_Runtime* runtime,
                              fa_RuntimeCallFrame* frames,
                              uint32_t* depth,
                              uint32_t function_index) {
    if (!runtime || !frames || !depth) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->module) {
        return FA_RUNTIME_ERR_NO_MODULE;
    }
    if (function_index >= runtime->module->num_functions) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    const uint32_t capacity = runtime->max_call_depth ? runtime->max_call_depth : 64U;
    if (*depth >= capacity) {
        return FA_RUNTIME_ERR_CALL_DEPTH_EXCEEDED;
    }

    uint8_t* body = wasm_load_function_body(runtime->module, function_index);
    if (!body) {
        return FA_RUNTIME_ERR_STREAM;
    }

    fa_RuntimeCallFrame* frame = &frames[*depth];
    memset(frame, 0, sizeof(*frame));
    frame->func_index = function_index;
    frame->body = body;
    frame->body_size = runtime->module->functions[function_index].body_size;

    int status = runtime_parse_locals(runtime, frame);
    if (status != FA_RUNTIME_OK) {
        runtime_free_frame_resources(frame);
        return status;
    }
    status = runtime_control_push(frame, FA_CONTROL_BLOCK, frame->code_start, 0, frame->body_size);
    if (status != FA_RUNTIME_OK) {
        runtime_free_frame_resources(frame);
        return status;
    }

    *depth += 1;
    return FA_RUNTIME_OK;
}

static void runtime_pop_frame(fa_RuntimeCallFrame* frames, uint32_t* depth) {
    if (!frames || !depth || *depth == 0) {
        return;
    }
    fa_RuntimeCallFrame* frame = &frames[*depth - 1];
    runtime_free_frame_resources(frame);
    *depth -= 1;
}

static bool runtime_is_function_end(const fa_RuntimeCallFrame* frame, uint8_t opcode) {
    if (!frame) {
        return false;
    }
    if (opcode != 0x0B) {
        return false;
    }
    return frame->control_depth == 1;
}

fa_Runtime* fa_Runtime_init(void) {
    fa_Runtime* runtime = (fa_Runtime*)calloc(1, sizeof(fa_Runtime));
    if (!runtime) {
        return NULL;
    }
    runtime->malloc = fa_default_malloc;
    runtime->free = fa_default_free;
    runtime->jobs = list_create(4);
    runtime->module = NULL;
    runtime->stream = NULL;
    runtime->next_job_id = 1;
    runtime->max_call_depth = 64;
    runtime->active_locals = NULL;
    runtime->active_locals_count = 0;
    runtime->globals = NULL;
    runtime->globals_count = 0;
    if (!runtime->jobs) {
        fa_Runtime_free(runtime);
        return NULL;
    }
    return runtime;
}

void fa_Runtime_free(fa_Runtime* runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->jobs) {
        while (list_count(runtime->jobs) > 0) {
            fa_Job* job = (fa_Job*)list_pop(runtime->jobs);
            if (job) {
                fa_JobStack_free(&job->stack);
                runtime_job_reg_clear(job);
                free(job);
            }
        }
        list_destroy(runtime->jobs);
        runtime->jobs = NULL;
    }
    fa_Runtime_detach_module(runtime);
    free(runtime);
}

int fa_Runtime_attach_module(fa_Runtime* runtime, WasmModule* module) {
    if (!runtime || !module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_Runtime_detach_module(runtime);
    runtime->module = module;
    runtime->stream = wasm_instruction_stream_init(module);
    if (!runtime->stream) {
        runtime->module = NULL;
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    int status = runtime_memory_init(runtime, module);
    if (status != FA_RUNTIME_OK) {
        wasm_instruction_stream_free(runtime->stream);
        runtime->stream = NULL;
        runtime->module = NULL;
        return status;
    }
    status = runtime_init_globals(runtime, module);
    if (status != FA_RUNTIME_OK) {
        wasm_instruction_stream_free(runtime->stream);
        runtime->stream = NULL;
        runtime_globals_reset(runtime);
        runtime_memory_reset(runtime);
        runtime->module = NULL;
        return status;
    }
    return FA_RUNTIME_OK;
}

void fa_Runtime_detach_module(fa_Runtime* runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->stream) {
        wasm_instruction_stream_free(runtime->stream);
        runtime->stream = NULL;
    }
    runtime_globals_reset(runtime);
    runtime_memory_reset(runtime);
    runtime->module = NULL;
    runtime->active_locals = NULL;
    runtime->active_locals_count = 0;
}

fa_Job* fa_Runtime_create_job(fa_Runtime* runtime) {
    if (!runtime || !runtime->jobs) {
        return NULL;
    }
    fa_Job* job = fa_Job_init();
    if (!job) {
        return NULL;
    }
    job->id = runtime->next_job_id++;
    if (list_push(runtime->jobs, job) != 0) {
        fa_JobStack_free(&job->stack);
        runtime_job_reg_clear(job);
        free(job);
        return NULL;
    }
    return job;
}

int fa_Runtime_destroy_job(fa_Runtime* runtime, fa_Job* job) {
    if (!runtime || !runtime->jobs || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < list_count(runtime->jobs); ++i) {
        if (list_get(runtime->jobs, i) == job) {
            (void)list_remove(runtime->jobs, i);
            fa_JobStack_free(&job->stack);
            runtime_job_reg_clear(job);
            free(job);
            return FA_RUNTIME_OK;
        }
    }
    return FA_RUNTIME_ERR_INVALID_ARGUMENT;
}

typedef enum {
    FA_CTRL_NONE = 0,
    FA_CTRL_BLOCK,
    FA_CTRL_LOOP,
    FA_CTRL_IF,
    FA_CTRL_ELSE,
    FA_CTRL_END,
    FA_CTRL_BR,
    FA_CTRL_BR_IF,
    FA_CTRL_BR_TABLE,
    FA_CTRL_UNREACHABLE,
    FA_CTRL_NOP,
    FA_CTRL_RETURN
} fa_RuntimeControlOp;

typedef struct {
    bool has_call;
    uint32_t call_target;
    bool request_return;
    bool request_end;
    fa_RuntimeControlOp control_op;
    uint32_t label_index;
    uint32_t* br_table_labels;
    uint32_t br_table_count;
    uint32_t br_table_default;
} fa_RuntimeInstructionContext;

static int runtime_decode_instruction(const uint8_t* body,
                                      uint32_t body_size,
                                      fa_RuntimeCallFrame* frame,
                                      fa_Job* job,
                                      uint8_t opcode,
                                      fa_RuntimeInstructionContext* ctx) {
    if (!body || !frame || !job || !ctx) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    memset(ctx, 0, sizeof(*ctx));

    switch (opcode) {
        case 0x02: /* block */
        case 0x03: /* loop */
        {
            int64_t block_type = 0;
            int status = runtime_read_sleb128(body, body_size, &frame->pc, &block_type);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t else_pc = 0;
            uint32_t end_pc = 0;
            status = runtime_scan_block(body, body_size, frame->pc, &else_pc, &end_pc);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            const fa_RuntimeControlType type = opcode == 0x03 ? FA_CONTROL_LOOP : FA_CONTROL_BLOCK;
            status = runtime_control_push(frame, type, frame->pc, 0, end_pc);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            ctx->control_op = opcode == 0x03 ? FA_CTRL_LOOP : FA_CTRL_BLOCK;
            return FA_RUNTIME_OK;
        }
        case 0x04: /* if */
        {
            int64_t block_type = 0;
            int status = runtime_read_sleb128(body, body_size, &frame->pc, &block_type);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t else_pc = 0;
            uint32_t end_pc = 0;
            status = runtime_scan_block(body, body_size, frame->pc, &else_pc, &end_pc);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            status = runtime_control_push(frame, FA_CONTROL_IF, frame->pc, else_pc, end_pc);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            ctx->control_op = FA_CTRL_IF;
            return FA_RUNTIME_OK;
        }
        case 0x05: /* else */
            ctx->control_op = FA_CTRL_ELSE;
            return FA_RUNTIME_OK;
        case 0x0C: /* br */
        case 0x0D: /* br_if */
        {
            uint64_t label = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &label);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            if (label > UINT32_MAX) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            ctx->label_index = (uint32_t)label;
            ctx->control_op = opcode == 0x0D ? FA_CTRL_BR_IF : FA_CTRL_BR;
            return FA_RUNTIME_OK;
        }
        case 0x0E: /* br_table */
        {
            uint64_t label_count = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &label_count);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            if (label_count > UINT32_MAX) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            if (label_count > 0) {
                ctx->br_table_labels = (uint32_t*)calloc((size_t)label_count, sizeof(uint32_t));
                if (!ctx->br_table_labels) {
                    return FA_RUNTIME_ERR_OUT_OF_MEMORY;
                }
            }
            ctx->br_table_count = (uint32_t)label_count;
            for (uint64_t i = 0; i < label_count; ++i) {
                uint64_t label = 0;
                status = runtime_read_uleb128(body, body_size, &frame->pc, &label);
                if (status != FA_RUNTIME_OK) {
                    free(ctx->br_table_labels);
                    ctx->br_table_labels = NULL;
                    ctx->br_table_count = 0;
                    return status;
                }
                if (label > UINT32_MAX) {
                    free(ctx->br_table_labels);
                    ctx->br_table_labels = NULL;
                    ctx->br_table_count = 0;
                    return FA_RUNTIME_ERR_UNSUPPORTED;
                }
                ctx->br_table_labels[i] = (uint32_t)label;
            }
            uint64_t default_label = 0;
            status = runtime_read_uleb128(body, body_size, &frame->pc, &default_label);
            if (status != FA_RUNTIME_OK) {
                free(ctx->br_table_labels);
                ctx->br_table_labels = NULL;
                ctx->br_table_count = 0;
                return status;
            }
            if (default_label > UINT32_MAX) {
                free(ctx->br_table_labels);
                ctx->br_table_labels = NULL;
                ctx->br_table_count = 0;
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            ctx->br_table_default = (uint32_t)default_label;
            ctx->control_op = FA_CTRL_BR_TABLE;
            return FA_RUNTIME_OK;
        }
        case 0x00: // unreachable
            ctx->control_op = FA_CTRL_UNREACHABLE;
            return FA_RUNTIME_OK;
        case 0x01: // nop
            ctx->control_op = FA_CTRL_NOP;
            return FA_RUNTIME_OK;
        case 0x0B: // end
            ctx->control_op = FA_CTRL_END;
            ctx->request_end = runtime_is_function_end(frame, opcode);
            return FA_RUNTIME_OK;
        case 0x0F: // return
            ctx->control_op = FA_CTRL_RETURN;
            ctx->request_return = true;
            return FA_RUNTIME_OK;
        case 0x10: // call
        {
            uint64_t func_index = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &func_index);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t u32_value = (uint32_t)func_index;
            status = runtime_push_reg_value(job, &u32_value, sizeof(u32_value));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            ctx->has_call = true;
            ctx->call_target = u32_value;
            return FA_RUNTIME_OK;
        }
        case 0x11: // call_indirect
            return FA_RUNTIME_ERR_UNSUPPORTED;
        case 0x20: // local.get
        case 0x21: // local.set
        case 0x22: // local.tee
        case 0x23: // global.get
        case 0x24: // global.set
        {
            uint64_t index = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &index);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t u32_value = (uint32_t)index;
            return runtime_push_reg_value(job, &u32_value, sizeof(u32_value));
        }
        case 0x41: // i32.const
        {
            int64_t value = 0;
            int status = runtime_read_sleb128(body, body_size, &frame->pc, &value);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            int32_t v32 = (int32_t)value;
            return runtime_push_reg_value(job, &v32, sizeof(v32));
        }
        case 0x42: // i64.const
        {
            int64_t value = 0;
            int status = runtime_read_sleb128(body, body_size, &frame->pc, &value);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            return runtime_push_reg_value(job, &value, sizeof(value));
        }
        case 0x43: // f32.const
        {
            if (frame->pc + 4U > body_size) {
                return FA_RUNTIME_ERR_STREAM;
            }
            float value = 0.0f;
            memcpy(&value, body + frame->pc, sizeof(value));
            frame->pc += 4U;
            return runtime_push_reg_value(job, &value, sizeof(value));
        }
        case 0x44: // f64.const
        {
            if (frame->pc + 8U > body_size) {
                return FA_RUNTIME_ERR_STREAM;
            }
            double value = 0.0;
            memcpy(&value, body + frame->pc, sizeof(value));
            frame->pc += 8U;
            return runtime_push_reg_value(job, &value, sizeof(value));
        }
        case 0x3F: // memory.size
        case 0x40: // memory.grow
        {
            uint64_t mem_index = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &mem_index);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t value = (uint32_t)mem_index;
            return runtime_push_reg_value(job, &value, sizeof(value));
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E:
        {
            uint64_t align = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &align);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t align32 = (uint32_t)align;
            status = runtime_push_reg_value(job, &align32, sizeof(align32));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint64_t offset = 0;
            status = runtime_read_uleb128(body, body_size, &frame->pc, &offset);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t offset32 = (uint32_t)offset;
            return runtime_push_reg_value(job, &offset32, sizeof(offset32));
        }
        default:
        {
            const fa_WasmOp* descriptor = fa_get_op(opcode);
            if (!descriptor || !descriptor->operation) {
                return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
            }
            if (descriptor->num_args == 0) {
                return FA_RUNTIME_OK;
            }
            return FA_RUNTIME_ERR_UNSUPPORTED;
        }
    }
}

static void runtime_instruction_context_free(fa_RuntimeInstructionContext* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->br_table_labels) {
        free(ctx->br_table_labels);
        ctx->br_table_labels = NULL;
    }
    ctx->br_table_count = 0;
    ctx->br_table_default = 0;
}

static int runtime_branch_to_label(fa_RuntimeCallFrame* frame, uint32_t label_index) {
    fa_RuntimeControlFrame* target = runtime_control_peek(frame, label_index);
    if (!target) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (target->type == FA_CONTROL_LOOP) {
        frame->pc = target->start_pc;
        runtime_control_pop_to(frame, label_index, true);
    } else {
        frame->pc = target->end_pc;
        runtime_control_pop_to(frame, label_index, false);
    }
    return FA_RUNTIME_OK;
}

static int runtime_execute_control_op(fa_Runtime* runtime,
                                      fa_RuntimeCallFrame* frame,
                                      fa_Job* job,
                                      fa_RuntimeInstructionContext* ctx,
                                      uint8_t opcode) {
    (void)runtime;
    if (!frame || !job || !ctx) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    switch (opcode) {
        case 0x00: /* unreachable */
            fa_JobStack_reset(&job->stack);
            return FA_RUNTIME_ERR_TRAP;
        case 0x01: /* nop */
            return FA_RUNTIME_OK;
        case 0x02: /* block */
        case 0x03: /* loop */
            return FA_RUNTIME_OK;
        case 0x04: /* if */
        {
            fa_JobValue cond;
            if (runtime_pop_stack_checked(job, &cond) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const bool truthy = runtime_job_value_truthy(&cond);
            fa_RuntimeControlFrame* entry = runtime_control_peek(frame, 0);
            if (!entry || entry->type != FA_CONTROL_IF) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (!truthy) {
                if (entry->else_pc != 0) {
                    frame->pc = entry->else_pc;
                } else {
                    frame->pc = entry->end_pc;
                    runtime_control_pop_one(frame);
                }
            }
            return FA_RUNTIME_OK;
        }
        case 0x05: /* else */
        {
            fa_RuntimeControlFrame* entry = runtime_control_peek(frame, 0);
            if (!entry || entry->type != FA_CONTROL_IF) {
                return FA_RUNTIME_ERR_TRAP;
            }
            frame->pc = entry->end_pc;
            runtime_control_pop_one(frame);
            return FA_RUNTIME_OK;
        }
        case 0x0B: /* end */
            if (frame->control_depth > 0) {
                runtime_control_pop_one(frame);
            }
            return FA_RUNTIME_OK;
        case 0x0C: /* br */
            return runtime_branch_to_label(frame, ctx->label_index);
        case 0x0D: /* br_if */
        {
            fa_JobValue cond;
            if (runtime_pop_stack_checked(job, &cond) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (runtime_job_value_truthy(&cond)) {
                return runtime_branch_to_label(frame, ctx->label_index);
            }
            return FA_RUNTIME_OK;
        }
        case 0x0E: /* br_table */
        {
            fa_JobValue index_value;
            if (runtime_pop_stack_checked(job, &index_value) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            u64 index = 0;
            if (!runtime_job_value_to_u64(&index_value, &index)) {
                return FA_RUNTIME_ERR_TRAP;
            }
            uint32_t label = ctx->br_table_default;
            if (index < ctx->br_table_count) {
                label = ctx->br_table_labels[index];
            }
            return runtime_branch_to_label(frame, label);
        }
        case 0x0F: /* return */
            return FA_RUNTIME_OK;
        default:
            return FA_RUNTIME_OK;
    }
}

int fa_Runtime_execute_job(fa_Runtime* runtime, fa_Job* job, uint32_t function_index) {
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->module || !runtime->module->functions) {
        return FA_RUNTIME_ERR_NO_MODULE;
    }
    if (function_index >= runtime->module->num_functions) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    fa_Runtime_reset_job_state(job);

    fa_RuntimeCallFrame* frames = runtime_alloc_frames(runtime);
    if (!frames) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }

    uint32_t depth = 0;
    int status = runtime_push_frame(runtime, frames, &depth, function_index);
    if (status != FA_RUNTIME_OK) {
        runtime_free_frames(runtime, frames);
        return status;
    }

    while (status == FA_RUNTIME_OK && depth > 0) {
        fa_RuntimeCallFrame* frame = &frames[depth - 1];
        if (frame->pc >= frame->body_size) {
            runtime_pop_frame(frames, &depth);
            continue;
        }
        runtime->active_locals = frame->locals;
        runtime->active_locals_count = frame->locals_count;

        const uint8_t* body = frame->body;
        uint8_t opcode = body[frame->pc++];

        fa_RuntimeInstructionContext ctx;
        status = runtime_decode_instruction(body, frame->body_size, frame, job, opcode, &ctx);
        if (status != FA_RUNTIME_OK) {
            runtime_instruction_context_free(&ctx);
            break;
        }

        if (ctx.control_op != FA_CTRL_NONE) {
            status = runtime_execute_control_op(runtime, frame, job, &ctx, opcode);
            runtime_instruction_context_free(&ctx);
            if (status != FA_RUNTIME_OK) {
                break;
            }
            if (ctx.request_return || ctx.request_end) {
                runtime_pop_frame(frames, &depth);
                continue;
            }
            continue;
        }

        const fa_WasmOp* descriptor = fa_get_op(opcode);
        if (!descriptor || !descriptor->operation) {
            status = FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
            runtime_instruction_context_free(&ctx);
            break;
        }

        status = fa_execute_op(opcode, runtime, job);
        if (status != FA_RUNTIME_OK) {
            runtime_instruction_context_free(&ctx);
            break;
        }

        if (ctx.has_call) {
            job->instructionPointer = 0;
            status = runtime_push_frame(runtime, frames, &depth, ctx.call_target);
            runtime_instruction_context_free(&ctx);
            if (status != FA_RUNTIME_OK) {
                break;
            }
            continue;
        }

        if (ctx.request_return || ctx.request_end) {
            runtime_pop_frame(frames, &depth);
            runtime_instruction_context_free(&ctx);
            continue;
        }

        runtime_instruction_context_free(&ctx);
    }

    while (depth > 0) {
        runtime_pop_frame(frames, &depth);
    }
    runtime->active_locals = NULL;
    runtime->active_locals_count = 0;
    runtime_free_frames(runtime, frames);
    return status;
}
