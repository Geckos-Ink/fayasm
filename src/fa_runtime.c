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
    uint32_t block_depth;
    fa_JobValue* locals;
    uint32_t locals_count;
} fa_RuntimeCallFrame;

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
    frame->body_size = 0;
    frame->pc = 0;
    frame->code_start = 0;
    frame->block_depth = 0;
    frame->locals_count = 0;
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
    if (frame->block_depth > 0) {
        return false;
    }
    return true;
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

typedef struct {
    bool has_call;
    uint32_t call_target;
    bool request_return;
    bool request_end;
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
            int32_t value = (int32_t)block_type;
            status = runtime_push_reg_value(job, &value, sizeof(value));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            frame->block_depth += 1;
            return FA_RUNTIME_OK;
        }
        case 0x04: /* if */
        {
            int64_t block_type = 0;
            int status = runtime_read_sleb128(body, body_size, &frame->pc, &block_type);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            int32_t value = (int32_t)block_type;
            status = runtime_push_reg_value(job, &value, sizeof(value));
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            frame->block_depth += 1;
            return FA_RUNTIME_OK;
        }
        case 0x05: /* else */
            return FA_RUNTIME_OK;
        case 0x0C: /* br */
        case 0x0D: /* br_if */
        {
            uint64_t label = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &label);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t u32_value = (uint32_t)label;
            return runtime_push_reg_value(job, &u32_value, sizeof(u32_value));
        }
        case 0x0E: /* br_table */
        {
            uint64_t label_count = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &label_count);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            for (uint64_t i = 0; i < label_count; ++i) {
                uint64_t label = 0;
                status = runtime_read_uleb128(body, body_size, &frame->pc, &label);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
            }
            uint64_t default_label = 0;
            status = runtime_read_uleb128(body, body_size, &frame->pc, &default_label);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t u32_value = (uint32_t)default_label;
            return runtime_push_reg_value(job, &u32_value, sizeof(u32_value));
        }
        case 0x00: // unreachable
            return FA_RUNTIME_OK;
        case 0x01: // nop
            return FA_RUNTIME_OK;
        case 0x0B: // end
            ctx->request_end = runtime_is_function_end(frame, opcode);
            if (frame->block_depth > 0) {
                frame->block_depth -= 1;
            }
            return FA_RUNTIME_OK;
        case 0x0F: // return
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
            break;
        }

        const fa_WasmOp* descriptor = fa_get_op(opcode);
        if (!descriptor || !descriptor->operation) {
            status = FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
            break;
        }

        status = fa_execute_op(opcode, runtime, job);
        if (status != FA_RUNTIME_OK) {
            break;
        }

        if (ctx.has_call) {
            job->instructionPointer = 0;
            status = runtime_push_frame(runtime, frames, &depth, ctx.call_target);
            if (status != FA_RUNTIME_OK) {
                break;
            }
            continue;
        }

        if (ctx.request_return || ctx.request_end) {
            runtime_pop_frame(frames, &depth);
            continue;
        }
    }

    while (depth > 0) {
        runtime_pop_frame(frames, &depth);
    }
    runtime->active_locals = NULL;
    runtime->active_locals_count = 0;
    runtime_free_frames(runtime, frames);
    return status;
}
