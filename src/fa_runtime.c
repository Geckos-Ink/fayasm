#define LIST_IMPLEMENTATION
#include "fa_runtime.h"
#include "fa_ops.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

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
    uint8_t* param_types;
    uint32_t param_count;
    uint8_t* result_types;
    uint32_t result_count;
    bool preserve_stack;
    size_t stack_height;
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
    if (runtime->memories) {
        for (uint32_t i = 0; i < runtime->memories_count; ++i) {
            if (runtime->memories[i].data) {
                runtime->free(runtime->memories[i].data);
                runtime->memories[i].data = NULL;
            }
        }
        free(runtime->memories);
        runtime->memories = NULL;
    }
    runtime->memories_count = 0;
}

static void runtime_tables_reset(fa_Runtime* runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->tables) {
        for (uint32_t i = 0; i < runtime->tables_count; ++i) {
            free(runtime->tables[i].data);
            runtime->tables[i].data = NULL;
        }
        free(runtime->tables);
        runtime->tables = NULL;
    }
    runtime->tables_count = 0;
}

static void runtime_segments_reset(fa_Runtime* runtime) {
    if (!runtime) {
        return;
    }
    if (runtime->data_segments_dropped) {
        free(runtime->data_segments_dropped);
        runtime->data_segments_dropped = NULL;
    }
    if (runtime->elem_segments_dropped) {
        free(runtime->elem_segments_dropped);
        runtime->elem_segments_dropped = NULL;
    }
    runtime->data_segments_count = 0;
    runtime->elem_segments_count = 0;
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
static bool runtime_job_value_matches_valtype(const fa_JobValue* value, uint8_t valtype);

static int runtime_init_globals(fa_Runtime* runtime, const WasmModule* module) {
    if (!runtime || !module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue* imported_overrides = NULL;
    if (module->num_globals > 0 && runtime->globals && module->globals &&
        runtime->globals_count == module->num_globals) {
        imported_overrides = (fa_JobValue*)calloc(module->num_globals, sizeof(fa_JobValue));
        if (!imported_overrides) {
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0; i < module->num_globals; ++i) {
            const WasmGlobal* global = &module->globals[i];
            if (!global->is_imported) {
                continue;
            }
            if (!runtime_job_value_matches_valtype(&runtime->globals[i], global->valtype)) {
                continue;
            }
            imported_overrides[i] = runtime->globals[i];
        }
    }

    runtime_globals_reset(runtime);
    if (module->num_globals == 0) {
        free(imported_overrides);
        return FA_RUNTIME_OK;
    }
    if (!module->globals) {
        free(imported_overrides);
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue* globals = (fa_JobValue*)calloc(module->num_globals, sizeof(fa_JobValue));
    if (!globals) {
        free(imported_overrides);
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
            free(imported_overrides);
            return status;
        }
        if (global->is_imported) {
            if (imported_overrides && imported_overrides[i].kind != fa_job_value_invalid) {
                value = imported_overrides[i];
            }
        } else {
            switch (global->init_kind) {
                case WASM_GLOBAL_INIT_CONST:
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
                            free(imported_overrides);
                            return FA_RUNTIME_ERR_UNSUPPORTED;
                    }
                    break;
                case WASM_GLOBAL_INIT_GET:
                    if (global->init_index >= i) {
                        runtime_globals_reset(runtime);
                        free(imported_overrides);
                        return FA_RUNTIME_ERR_UNSUPPORTED;
                    }
                    if (module->globals[global->init_index].valtype != global->valtype) {
                        runtime_globals_reset(runtime);
                        free(imported_overrides);
                        return FA_RUNTIME_ERR_UNSUPPORTED;
                    }
                    value = runtime->globals[global->init_index];
                    break;
                case WASM_GLOBAL_INIT_NONE:
                default:
                    runtime_globals_reset(runtime);
                    free(imported_overrides);
                    return FA_RUNTIME_ERR_UNSUPPORTED;
            }
        }
        runtime->globals[i] = value;
    }
    free(imported_overrides);
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
    runtime->memories = (fa_RuntimeMemory*)calloc(module->num_memories, sizeof(fa_RuntimeMemory));
    if (!runtime->memories) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    runtime->memories_count = module->num_memories;

    int status = FA_RUNTIME_OK;
    for (uint32_t i = 0; i < module->num_memories; ++i) {
        const WasmMemory* memory = &module->memories[i];
        fa_RuntimeMemory* dst = &runtime->memories[i];
        dst->is_memory64 = memory->is_memory64;
        dst->has_max = memory->has_max;

        if (!memory->is_memory64) {
            if (memory->initial_size > UINT32_MAX) {
                status = FA_RUNTIME_ERR_UNSUPPORTED;
                goto cleanup;
            }
            if (memory->has_max && memory->maximum_size > UINT32_MAX) {
                status = FA_RUNTIME_ERR_UNSUPPORTED;
                goto cleanup;
            }
        }

        if (memory->has_max) {
            const uint64_t max_pages = memory->maximum_size;
            if (max_pages > (UINT64_MAX / FA_WASM_PAGE_SIZE)) {
                status = FA_RUNTIME_ERR_UNSUPPORTED;
                goto cleanup;
            }
            dst->max_size_bytes = max_pages * FA_WASM_PAGE_SIZE;
        }

        if (memory->initial_size == 0) {
            dst->size_bytes = 0;
            continue;
        }
        if (memory->initial_size > (UINT64_MAX / FA_WASM_PAGE_SIZE)) {
            status = FA_RUNTIME_ERR_UNSUPPORTED;
            goto cleanup;
        }
        const uint64_t size_bytes = memory->initial_size * FA_WASM_PAGE_SIZE;
        if (size_bytes > SIZE_MAX || size_bytes > (uint64_t)INT_MAX) {
            status = FA_RUNTIME_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        uint8_t* data = (uint8_t*)runtime->malloc((int)size_bytes);
        if (!data) {
            status = FA_RUNTIME_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        memset(data, 0, (size_t)size_bytes);
        dst->data = data;
        dst->size_bytes = size_bytes;
    }
    return FA_RUNTIME_OK;

cleanup:
    runtime_memory_reset(runtime);
    return status;
}

static int runtime_tables_init(fa_Runtime* runtime, const WasmModule* module) {
    if (!runtime || !module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    runtime_tables_reset(runtime);
    if (module->num_tables == 0 || !module->tables) {
        return FA_RUNTIME_OK;
    }
    runtime->tables = (fa_RuntimeTable*)calloc(module->num_tables, sizeof(fa_RuntimeTable));
    if (!runtime->tables) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    runtime->tables_count = module->num_tables;

    int status = FA_RUNTIME_OK;
    for (uint32_t i = 0; i < module->num_tables; ++i) {
        const WasmTable* table = &module->tables[i];
        fa_RuntimeTable* dst = &runtime->tables[i];
        dst->elem_type = table->elem_type;
        dst->has_max = table->has_max;
        dst->max_size = table->maximum_size;
        if (table->initial_size > UINT32_MAX) {
            status = FA_RUNTIME_ERR_UNSUPPORTED;
            goto cleanup;
        }
        if (table->has_max && table->maximum_size > UINT32_MAX) {
            status = FA_RUNTIME_ERR_UNSUPPORTED;
            goto cleanup;
        }
        dst->size = table->initial_size;
        if (dst->size > 0) {
            dst->data = (fa_ptr*)calloc(dst->size, sizeof(fa_ptr));
            if (!dst->data) {
                status = FA_RUNTIME_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
        }
    }
    return FA_RUNTIME_OK;

cleanup:
    runtime_tables_reset(runtime);
    return status;
}

static int runtime_memory_bounds_check(const fa_RuntimeMemory* memory, uint64_t offset, size_t size) {
    if (!memory || !memory->data) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (size == 0) {
        return FA_RUNTIME_OK;
    }
    if (offset > UINT64_MAX - size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (offset + size > memory->size_bytes) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}

static int runtime_segments_init(fa_Runtime* runtime, const WasmModule* module) {
    if (!runtime || !module) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    runtime_segments_reset(runtime);

    if (module->num_data_segments > 0 && module->data_segments) {
        runtime->data_segments_dropped = (bool*)calloc(module->num_data_segments, sizeof(bool));
        if (!runtime->data_segments_dropped) {
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        runtime->data_segments_count = module->num_data_segments;

        for (uint32_t i = 0; i < module->num_data_segments; ++i) {
            const WasmDataSegment* segment = &module->data_segments[i];
            if (segment->is_passive) {
                continue;
            }
            if (segment->memory_index >= runtime->memories_count) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            fa_RuntimeMemory* memory = &runtime->memories[segment->memory_index];
            if (segment->size > SIZE_MAX) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            const size_t length = (size_t)segment->size;
            if (runtime_memory_bounds_check(memory, segment->offset, length) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            memcpy(memory->data + (size_t)segment->offset, segment->data, length);
            runtime->data_segments_dropped[i] = true;
        }
    }

    if (module->num_elements > 0 && module->elements) {
        runtime->elem_segments_dropped = (bool*)calloc(module->num_elements, sizeof(bool));
        if (!runtime->elem_segments_dropped) {
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        runtime->elem_segments_count = module->num_elements;

        for (uint32_t i = 0; i < module->num_elements; ++i) {
            const WasmElementSegment* segment = &module->elements[i];
            if (segment->is_declarative) {
                runtime->elem_segments_dropped[i] = true;
                continue;
            }
            if (segment->is_passive) {
                continue;
            }
            if (segment->table_index >= runtime->tables_count) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            fa_RuntimeTable* table = &runtime->tables[segment->table_index];
            if (segment->elem_type != table->elem_type) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            if (segment->offset > UINT64_MAX - segment->element_count) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint64_t end = segment->offset + segment->element_count;
            if (end > table->size) {
                return FA_RUNTIME_ERR_TRAP;
            }
            for (uint32_t j = 0; j < segment->element_count; ++j) {
                table->data[(size_t)segment->offset + j] = (fa_ptr)segment->elements[j];
            }
            runtime->elem_segments_dropped[i] = true;
        }
    }

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
    const uint32_t previous_capacity = frame->control_capacity;
    uint32_t next_capacity = frame->control_capacity ? frame->control_capacity * 2U : 8U;
    while (next_capacity < count) {
        next_capacity *= 2U;
    }
    fa_RuntimeControlFrame* next = (fa_RuntimeControlFrame*)realloc(frame->control_stack, next_capacity * sizeof(fa_RuntimeControlFrame));
    if (!next) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    if (next_capacity > previous_capacity) {
        memset(next + previous_capacity, 0, (next_capacity - previous_capacity) * sizeof(fa_RuntimeControlFrame));
    }
    frame->control_stack = next;
    frame->control_capacity = next_capacity;
    return FA_RUNTIME_OK;
}

static void runtime_control_frame_clear(fa_RuntimeControlFrame* entry) {
    if (!entry) {
        return;
    }
    free(entry->param_types);
    free(entry->result_types);
    entry->param_types = NULL;
    entry->result_types = NULL;
    entry->param_count = 0;
    entry->result_count = 0;
}

static int runtime_control_push(fa_RuntimeCallFrame* frame,
                                fa_RuntimeControlType type,
                                uint32_t start_pc,
                                uint32_t else_pc,
                                uint32_t end_pc,
                                const uint32_t* param_types,
                                uint32_t param_count,
                                const uint32_t* result_types,
                                uint32_t result_count,
                                bool preserve_stack,
                                size_t stack_height) {
    if (!frame) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    const uint32_t next_depth = frame->control_depth + 1U;
    int status = runtime_control_reserve(frame, next_depth);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    fa_RuntimeControlFrame* entry = &frame->control_stack[frame->control_depth];
    runtime_control_frame_clear(entry);
    entry->type = type;
    entry->start_pc = start_pc;
    entry->else_pc = else_pc;
    entry->end_pc = end_pc;
    entry->param_count = param_count;
    entry->result_count = result_count;
    if (param_count > 0) {
        entry->param_types = (uint8_t*)malloc(param_count * sizeof(uint8_t));
        if (!entry->param_types) {
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0; i < param_count; ++i) {
            if (param_types[i] > UINT8_MAX) {
                runtime_control_frame_clear(entry);
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            entry->param_types[i] = (uint8_t)param_types[i];
        }
    }
    if (result_count > 0) {
        entry->result_types = (uint8_t*)malloc(result_count * sizeof(uint8_t));
        if (!entry->result_types) {
            runtime_control_frame_clear(entry);
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0; i < result_count; ++i) {
            if (result_types[i] > UINT8_MAX) {
                runtime_control_frame_clear(entry);
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            entry->result_types[i] = (uint8_t)result_types[i];
        }
    }
    entry->preserve_stack = preserve_stack;
    entry->stack_height = stack_height;
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
    const uint32_t new_depth = keep_target ? (target_index + 1U) : target_index;
    for (uint32_t i = new_depth; i < frame->control_depth; ++i) {
        runtime_control_frame_clear(&frame->control_stack[i]);
    }
    frame->control_depth = new_depth;
}

static void runtime_control_pop_one(fa_RuntimeCallFrame* frame) {
    if (!frame || frame->control_depth == 0) {
        return;
    }
    runtime_control_frame_clear(&frame->control_stack[frame->control_depth - 1U]);
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
        case VALTYPE_V128:
            out->kind = fa_job_value_v128;
            out->bit_width = 128U;
            out->is_signed = false;
            out->payload.v128_value.low = 0;
            out->payload.v128_value.high = 0;
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

typedef struct {
    const uint32_t* param_types;
    uint32_t param_count;
    const uint32_t* result_types;
    uint32_t result_count;
    uint32_t inline_result_type;
} fa_RuntimeBlockSignature;

static int runtime_decode_block_signature(const fa_Runtime* runtime,
                                          int64_t block_type,
                                          fa_RuntimeBlockSignature* sig) {
    if (!sig) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    sig->param_types = NULL;
    sig->param_count = 0;
    sig->result_types = NULL;
    sig->result_count = 0;
    sig->inline_result_type = 0;

    if (block_type == -64 || block_type == 0x40) {
        return FA_RUNTIME_OK;
    }
    if (block_type >= 0) {
        if (!runtime || !runtime->module || block_type > UINT32_MAX) {
            return FA_RUNTIME_ERR_UNSUPPORTED;
        }
        const uint32_t type_index = (uint32_t)block_type;
        if (type_index >= runtime->module->num_types) {
            return FA_RUNTIME_ERR_UNSUPPORTED;
        }
        const WasmFunctionType* type = &runtime->module->types[type_index];
        sig->param_types = type->param_types;
        sig->param_count = type->num_params;
        sig->result_types = type->result_types;
        sig->result_count = type->num_results;
        return FA_RUNTIME_OK;
    }

    switch (block_type) {
        case -1:
            sig->inline_result_type = VALTYPE_I32;
            sig->result_types = &sig->inline_result_type;
            sig->result_count = 1;
            return FA_RUNTIME_OK;
        case -2:
            sig->inline_result_type = VALTYPE_I64;
            sig->result_types = &sig->inline_result_type;
            sig->result_count = 1;
            return FA_RUNTIME_OK;
        case -3:
            sig->inline_result_type = VALTYPE_F32;
            sig->result_types = &sig->inline_result_type;
            sig->result_count = 1;
            return FA_RUNTIME_OK;
        case -4:
            sig->inline_result_type = VALTYPE_F64;
            sig->result_types = &sig->inline_result_type;
            sig->result_count = 1;
            return FA_RUNTIME_OK;
        case -5:
            sig->inline_result_type = VALTYPE_V128;
            sig->result_types = &sig->inline_result_type;
            sig->result_count = 1;
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
        for (uint32_t i = 0; i < frame->control_depth; ++i) {
            runtime_control_frame_clear(&frame->control_stack[i]);
        }
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

static int runtime_skip_immediates(const uint8_t* body,
                                   uint32_t body_size,
                                   const fa_Runtime* runtime,
                                   uint32_t* cursor,
                                   uint8_t opcode) {
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
            if (runtime && runtime->module && runtime->module->num_memories > 1) {
                int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
            }
            int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            return runtime_read_uleb128(body, body_size, cursor, &uleb);
        }
        case 0x25: /* table.get */
        case 0x26: /* table.set */
            return runtime_read_uleb128(body, body_size, cursor, &uleb);
        case 0xFC: /* bulk memory/table prefix */
        {
            int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            switch (uleb) {
                case 8: /* memory.init */
                    status = runtime_read_uleb128(body, body_size, cursor, &uleb);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 9: /* data.drop */
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 10: /* memory.copy */
                    status = runtime_read_uleb128(body, body_size, cursor, &uleb);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 11: /* memory.fill */
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 12: /* table.init */
                    status = runtime_read_uleb128(body, body_size, cursor, &uleb);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 13: /* elem.drop */
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 14: /* table.copy */
                    status = runtime_read_uleb128(body, body_size, cursor, &uleb);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                case 15: /* table.grow */
                case 16: /* table.size */
                case 17: /* table.fill */
                    return runtime_read_uleb128(body, body_size, cursor, &uleb);
                default:
                    return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
            }
        }
        case 0xFD: /* simd prefix */
        {
            int status = runtime_read_uleb128(body, body_size, cursor, &uleb);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            switch (uleb) {
                case 12: /* v128.const */
                    if (*cursor + 16U > body_size) {
                        return FA_RUNTIME_ERR_STREAM;
                    }
                    *cursor += 16U;
                    return FA_RUNTIME_OK;
                default:
                    return FA_RUNTIME_OK;
            }
        }
        default:
            return FA_RUNTIME_OK;
    }
}

static int runtime_scan_block(const uint8_t* body,
                              uint32_t body_size,
                              const fa_Runtime* runtime,
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
                int status = runtime_skip_immediates(body, body_size, runtime, &cursor, opcode);
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

static bool runtime_job_value_matches_valtype(const fa_JobValue* value, uint8_t valtype) {
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

static int runtime_stack_check_types(const fa_JobStack* stack, const uint32_t* types, uint32_t count) {
    if (!stack) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return FA_RUNTIME_OK;
    }
    if (!types) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (stack->size < count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const fa_JobValue* value = fa_JobStack_peek(stack, i);
        const uint32_t type_index = count - 1U - i;
        if (!value || types[type_index] > UINT8_MAX ||
            !runtime_job_value_matches_valtype(value, (uint8_t)types[type_index])) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    return FA_RUNTIME_OK;
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

    const WasmFunctionType* type = NULL;
    if (runtime->module) {
        const uint32_t type_index = runtime->module->functions[function_index].type_index;
        if (type_index >= runtime->module->num_types) {
            runtime_free_frame_resources(frame);
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;
        }
        type = &runtime->module->types[type_index];
        for (uint32_t i = 0; i < type->num_results; ++i) {
            fa_JobValue dummy;
            if (runtime_init_value_from_valtype(&dummy, type->result_types[i]) != FA_RUNTIME_OK) {
                runtime_free_frame_resources(frame);
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
        }
    }

    status = runtime_control_push(frame,
                                  FA_CONTROL_BLOCK,
                                  frame->code_start,
                                  0,
                                  frame->body_size,
                                  NULL,
                                  0,
                                  type ? type->result_types : NULL,
                                  type ? type->num_results : 0,
                                  false,
                                  0);
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
    runtime->memories = NULL;
    runtime->memories_count = 0;
    runtime->tables = NULL;
    runtime->tables_count = 0;
    runtime->data_segments_dropped = NULL;
    runtime->data_segments_count = 0;
    runtime->elem_segments_dropped = NULL;
    runtime->elem_segments_count = 0;
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
    status = runtime_tables_init(runtime, module);
    if (status != FA_RUNTIME_OK) {
        wasm_instruction_stream_free(runtime->stream);
        runtime->stream = NULL;
        runtime_memory_reset(runtime);
        runtime->module = NULL;
        return status;
    }
    status = runtime_segments_init(runtime, module);
    if (status != FA_RUNTIME_OK) {
        wasm_instruction_stream_free(runtime->stream);
        runtime->stream = NULL;
        runtime_segments_reset(runtime);
        runtime_tables_reset(runtime);
        runtime_memory_reset(runtime);
        runtime->module = NULL;
        return status;
    }
    status = runtime_init_globals(runtime, module);
    if (status != FA_RUNTIME_OK) {
        wasm_instruction_stream_free(runtime->stream);
        runtime->stream = NULL;
        runtime_globals_reset(runtime);
        runtime_segments_reset(runtime);
        runtime_tables_reset(runtime);
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
    runtime_segments_reset(runtime);
    runtime_tables_reset(runtime);
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

int fa_Runtime_set_imported_global(fa_Runtime* runtime, uint32_t global_index, const fa_JobValue* value) {
    if (!runtime || !value || !runtime->module || !runtime->globals) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (global_index >= runtime->globals_count || global_index >= runtime->module->num_globals) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    const WasmGlobal* global = &runtime->module->globals[global_index];
    if (!global->is_imported) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime_job_value_matches_valtype(value, global->valtype)) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    runtime->globals[global_index] = *value;
    return runtime_init_globals(runtime, runtime->module);
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
                                      fa_Runtime* runtime,
                                      fa_RuntimeCallFrame* frame,
                                      fa_Job* job,
                                      uint8_t opcode,
                                      fa_RuntimeInstructionContext* ctx) {
    if (!body || !frame || !job || !ctx || !runtime) {
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
            fa_RuntimeBlockSignature sig;
            status = runtime_decode_block_signature(runtime, block_type, &sig);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t else_pc = 0;
            uint32_t end_pc = 0;
            status = runtime_scan_block(body, body_size, runtime, frame->pc, &else_pc, &end_pc);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            status = runtime_stack_check_types(&job->stack, sig.param_types, sig.param_count);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            if (job->stack.size < sig.param_count) {
                return FA_RUNTIME_ERR_TRAP;
            }
            const fa_RuntimeControlType type = opcode == 0x03 ? FA_CONTROL_LOOP : FA_CONTROL_BLOCK;
            status = runtime_control_push(frame,
                                          type,
                                          frame->pc,
                                          0,
                                          end_pc,
                                          sig.param_types,
                                          sig.param_count,
                                          sig.result_types,
                                          sig.result_count,
                                          false,
                                          job->stack.size - sig.param_count);
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
            fa_RuntimeBlockSignature sig;
            status = runtime_decode_block_signature(runtime, block_type, &sig);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t else_pc = 0;
            uint32_t end_pc = 0;
            status = runtime_scan_block(body, body_size, runtime, frame->pc, &else_pc, &end_pc);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            if (sig.result_count > 0 && else_pc == 0) {
                return FA_RUNTIME_ERR_UNSUPPORTED;
            }
            status = runtime_control_push(frame,
                                          FA_CONTROL_IF,
                                          frame->pc,
                                          else_pc,
                                          end_pc,
                                          sig.param_types,
                                          sig.param_count,
                                          sig.result_types,
                                          sig.result_count,
                                          false,
                                          job->stack.size);
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
        case 0x25: // table.get
        case 0x26: // table.set
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
            uint64_t mem_index = 0;
            bool memory64 = false;
            if (runtime->module && runtime->module->num_memories > 1) {
                int status = runtime_read_uleb128(body, body_size, &frame->pc, &mem_index);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
                uint32_t mem_index_u32 = (uint32_t)mem_index;
                status = runtime_push_reg_value(job, &mem_index_u32, sizeof(mem_index_u32));
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
                if (mem_index_u32 < runtime->module->num_memories) {
                    memory64 = runtime->module->memories[mem_index_u32].is_memory64;
                }
            } else if (runtime->module && runtime->module->num_memories == 1) {
                memory64 = runtime->module->memories[0].is_memory64;
            }
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
            if (memory64) {
                return runtime_push_reg_value(job, &offset, sizeof(offset));
            }
            uint32_t offset32 = (uint32_t)offset;
            return runtime_push_reg_value(job, &offset32, sizeof(offset32));
        }
        case 0xFC: // bulk memory/table prefix
        {
            uint64_t subopcode = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &subopcode);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            uint32_t sub = (uint32_t)subopcode;
            switch (sub) {
                case 8: // memory.init
                {
                    uint64_t data_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &data_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t data_u32 = (uint32_t)data_index;
                    status = runtime_push_reg_value(job, &data_u32, sizeof(data_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint64_t mem_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &mem_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t mem_u32 = (uint32_t)mem_index;
                    status = runtime_push_reg_value(job, &mem_u32, sizeof(mem_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 9: // data.drop
                {
                    uint64_t data_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &data_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t data_u32 = (uint32_t)data_index;
                    status = runtime_push_reg_value(job, &data_u32, sizeof(data_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 10: // memory.copy
                {
                    uint64_t dst_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &dst_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t dst_u32 = (uint32_t)dst_index;
                    status = runtime_push_reg_value(job, &dst_u32, sizeof(dst_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint64_t src_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &src_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t src_u32 = (uint32_t)src_index;
                    status = runtime_push_reg_value(job, &src_u32, sizeof(src_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 11: // memory.fill
                {
                    uint64_t mem_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &mem_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t mem_u32 = (uint32_t)mem_index;
                    status = runtime_push_reg_value(job, &mem_u32, sizeof(mem_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 12: // table.init
                {
                    uint64_t table_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &table_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t table_u32 = (uint32_t)table_index;
                    status = runtime_push_reg_value(job, &table_u32, sizeof(table_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint64_t elem_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &elem_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t elem_u32 = (uint32_t)elem_index;
                    status = runtime_push_reg_value(job, &elem_u32, sizeof(elem_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 13: // elem.drop
                {
                    uint64_t elem_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &elem_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t elem_u32 = (uint32_t)elem_index;
                    status = runtime_push_reg_value(job, &elem_u32, sizeof(elem_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 14: // table.copy
                {
                    uint64_t dst_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &dst_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t dst_u32 = (uint32_t)dst_index;
                    status = runtime_push_reg_value(job, &dst_u32, sizeof(dst_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint64_t src_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &src_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t src_u32 = (uint32_t)src_index;
                    status = runtime_push_reg_value(job, &src_u32, sizeof(src_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                case 15: // table.grow
                case 16: // table.size
                case 17: // table.fill
                {
                    uint64_t table_index = 0;
                    status = runtime_read_uleb128(body, body_size, &frame->pc, &table_index);
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    uint32_t table_u32 = (uint32_t)table_index;
                    status = runtime_push_reg_value(job, &table_u32, sizeof(table_u32));
                    if (status != FA_RUNTIME_OK) {
                        return status;
                    }
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                default:
                    return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
            }
        }
        case 0xFD: // simd prefix
        {
            uint64_t subopcode = 0;
            int status = runtime_read_uleb128(body, body_size, &frame->pc, &subopcode);
            if (status != FA_RUNTIME_OK) {
                return status;
            }
            if (subopcode == 12) { // v128.const
                if (frame->pc + 16U > body_size) {
                    return FA_RUNTIME_ERR_STREAM;
                }
                status = runtime_push_reg_value(job, body + frame->pc, 16U);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
                frame->pc += 16U;
                uint32_t sub = (uint32_t)subopcode;
                return runtime_push_reg_value(job, &sub, sizeof(sub));
            }
            switch (subopcode) {
                case 15: // i8x16.splat
                case 16: // i16x8.splat
                case 17: // i32x4.splat
                case 18: // i64x2.splat
                case 19: // f32x4.splat
                case 20: // f64x2.splat
                {
                    uint32_t sub = (uint32_t)subopcode;
                    return runtime_push_reg_value(job, &sub, sizeof(sub));
                }
                default:
                    return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
            }
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

static int runtime_unwind_stack_to(fa_Job* job,
                                   size_t target_height,
                                   const uint8_t* types,
                                   uint32_t type_count) {
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (job->stack.size < target_height) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (type_count > 0) {
        if (!types) {
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;
        }
        if (job->stack.size < target_height + type_count) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }

    fa_JobValue* values = NULL;
    if (type_count > 0) {
        values = (fa_JobValue*)calloc(type_count, sizeof(fa_JobValue));
        if (!values) {
            return FA_RUNTIME_ERR_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0; i < type_count; ++i) {
            if (!fa_JobStack_pop(&job->stack, &values[i])) {
                free(values);
                return FA_RUNTIME_ERR_TRAP;
            }
            const uint32_t type_index = type_count - 1U - i;
            if (types[type_index] > UINT8_MAX ||
                !runtime_job_value_matches_valtype(&values[i], (uint8_t)types[type_index])) {
                free(values);
                return FA_RUNTIME_ERR_TRAP;
            }
        }
    }
    while (job->stack.size > target_height) {
        fa_JobValue discard;
        if (!fa_JobStack_pop(&job->stack, &discard)) {
            free(values);
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (values) {
        for (uint32_t i = type_count; i > 0; --i) {
            if (!fa_JobStack_push(&job->stack, &values[i - 1U])) {
                free(values);
                return FA_RUNTIME_ERR_OUT_OF_MEMORY;
            }
        }
        free(values);
    }
    return FA_RUNTIME_OK;
}

static int runtime_branch_to_label(fa_RuntimeCallFrame* frame, fa_Job* job, uint32_t label_index) {
    if (!frame || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_RuntimeControlFrame* target = runtime_control_peek(frame, label_index);
    if (!target) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const fa_RuntimeControlFrame target_copy = *target;
    if (!target_copy.preserve_stack) {
        size_t unwind_height = target_copy.stack_height;
        const uint8_t* types = target_copy.result_types;
        uint32_t type_count = target_copy.result_count;
        if (target_copy.type == FA_CONTROL_LOOP) {
            types = target_copy.param_types;
            type_count = target_copy.param_count;
            if (unwind_height < target_copy.param_count) {
                return FA_RUNTIME_ERR_TRAP;
            }
            unwind_height -= target_copy.param_count;
        }
        int status = runtime_unwind_stack_to(job, unwind_height, types, type_count);
        if (status != FA_RUNTIME_OK) {
            return status;
        }
    }
    if (target_copy.type == FA_CONTROL_LOOP) {
        frame->pc = target_copy.start_pc;
        runtime_control_pop_to(frame, label_index, true);
    } else {
        frame->pc = target_copy.end_pc;
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
            entry->stack_height = job->stack.size;
            if (entry->param_count > 0) {
                int status = runtime_stack_check_types(&job->stack, entry->param_types, entry->param_count);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
                if (entry->stack_height < entry->param_count) {
                    return FA_RUNTIME_ERR_TRAP;
                }
                entry->stack_height -= entry->param_count;
            }
            if (!truthy) {
                if (entry->else_pc != 0) {
                    frame->pc = entry->else_pc;
                } else {
                    return runtime_branch_to_label(frame, job, 0);
                }
            }
            return FA_RUNTIME_OK;
        }
        case 0x05: /* else */
        {
            return runtime_branch_to_label(frame, job, 0);
        }
        case 0x0B: /* end */
        {
            fa_RuntimeControlFrame* entry = runtime_control_peek(frame, 0);
            if (!entry) {
                return FA_RUNTIME_OK;
            }
            if (!entry->preserve_stack) {
                int status = runtime_unwind_stack_to(job,
                                                     entry->stack_height,
                                                     entry->result_types,
                                                     entry->result_count);
                if (status != FA_RUNTIME_OK) {
                    return status;
                }
            }
            runtime_control_pop_one(frame);
            return FA_RUNTIME_OK;
        }
        case 0x0C: /* br */
            return runtime_branch_to_label(frame, job, ctx->label_index);
        case 0x0D: /* br_if */
        {
            fa_JobValue cond;
            if (runtime_pop_stack_checked(job, &cond) != FA_RUNTIME_OK) {
                return FA_RUNTIME_ERR_TRAP;
            }
            if (runtime_job_value_truthy(&cond)) {
                return runtime_branch_to_label(frame, job, ctx->label_index);
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
            return runtime_branch_to_label(frame, job, label);
        }
        case 0x0F: /* return */
        {
            if (frame->control_depth == 0) {
                return FA_RUNTIME_OK;
            }
            uint32_t label_index = frame->control_depth - 1U;
            return runtime_branch_to_label(frame, job, label_index);
        }
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
        status = runtime_decode_instruction(body, frame->body_size, runtime, frame, job, opcode, &ctx);
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
