#include "fa_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct {
    const uint8_t* param_types;
    uint32_t param_count;
    const uint8_t* result_types;
    uint32_t result_count;
} TypeSpec;

static const uint8_t kResultI32[] = { VALTYPE_I32 };
static const uint8_t kResultI64[] = { VALTYPE_I64 };
static const uint8_t kResultF32[] = { VALTYPE_F32 };
static const uint8_t kResultF64[] = { VALTYPE_F64 };

static void bb_free(ByteBuffer* buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static void bb_reset(ByteBuffer* buffer) {
    if (!buffer) {
        return;
    }
    buffer->size = 0;
}

static int bb_reserve(ByteBuffer* buffer, size_t extra) {
    if (!buffer) {
        return 0;
    }
    if (buffer->size + extra <= buffer->capacity) {
        return 1;
    }
    size_t new_capacity = buffer->capacity ? buffer->capacity * 2U : 64U;
    while (new_capacity < buffer->size + extra) {
        new_capacity *= 2U;
    }
    uint8_t* next = (uint8_t*)realloc(buffer->data, new_capacity);
    if (!next) {
        return 0;
    }
    buffer->data = next;
    buffer->capacity = new_capacity;
    return 1;
}

static int bb_write_byte(ByteBuffer* buffer, uint8_t value) {
    if (!bb_reserve(buffer, 1)) {
        return 0;
    }
    buffer->data[buffer->size++] = value;
    return 1;
}

static int bb_write_bytes(ByteBuffer* buffer, const uint8_t* data, size_t length) {
    if (!buffer || (!data && length > 0)) {
        return 0;
    }
    if (!bb_reserve(buffer, length)) {
        return 0;
    }
    memcpy(buffer->data + buffer->size, data, length);
    buffer->size += length;
    return 1;
}

static int bb_write_uleb(ByteBuffer* buffer, uint32_t value);

static int bb_write_string(ByteBuffer* buffer, const char* value) {
    if (!buffer || !value) {
        return 0;
    }
    const size_t len = strlen(value);
    if (!bb_write_uleb(buffer, (uint32_t)len)) {
        return 0;
    }
    return bb_write_bytes(buffer, (const uint8_t*)value, len);
}

static int bb_write_uleb(ByteBuffer* buffer, uint32_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        if (!bb_write_byte(buffer, byte)) {
            return 0;
        }
    } while (value != 0);
    return 1;
}

static int bb_write_uleb64(ByteBuffer* buffer, uint64_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        if (!bb_write_byte(buffer, byte)) {
            return 0;
        }
    } while (value != 0);
    return 1;
}

static int bb_write_f32(ByteBuffer* buffer, float value) {
    uint8_t bytes[sizeof(value)];
    memcpy(bytes, &value, sizeof(value));
    return bb_write_bytes(buffer, bytes, sizeof(bytes));
}

static int bb_write_f64(ByteBuffer* buffer, double value) {
    uint8_t bytes[sizeof(value)];
    memcpy(bytes, &value, sizeof(value));
    return bb_write_bytes(buffer, bytes, sizeof(bytes));
}

static int bb_write_sleb32(ByteBuffer* buffer, int32_t value) {
    int more = 1;
    while (more) {
        uint8_t byte = (uint8_t)(value & 0x7F);
        const int32_t sign = value >> 31;
        value >>= 7;
        if ((value == sign) && ((byte & 0x40) == (sign & 0x40))) {
            more = 0;
        } else {
            byte |= 0x80;
        }
        if (!bb_write_byte(buffer, byte)) {
            return 0;
        }
    }
    return 1;
}

static int append_section(ByteBuffer* module, uint8_t section_id, const ByteBuffer* payload) {
    if (!module || !payload) {
        return 0;
    }
    return bb_write_byte(module, section_id) &&
        bb_write_uleb(module, (uint32_t)payload->size) &&
        bb_write_bytes(module, payload->data, payload->size);
}

static int build_module(ByteBuffer* module,
                        const uint8_t* const* bodies,
                        const size_t* body_sizes,
                        size_t func_count,
                        int with_memory,
                        uint32_t memory_initial_pages,
                        int memory_has_max,
                        uint32_t memory_max_pages,
                        const uint8_t* result_types,
                        uint32_t result_count,
                        const uint8_t* param_types,
                        uint32_t param_count) {
    if (!module || !bodies || !body_sizes || func_count == 0) {
        return 0;
    }
    if (result_count > 0 && !result_types) {
        return 0;
    }
    if (param_count > 0 && !param_types) {
        return 0;
    }
    bb_reset(module);
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (!bb_write_bytes(module, header, sizeof(header))) {
        return 0;
    }

    ByteBuffer payload = {0};

    // Type section: one empty function signature.
    bb_write_uleb(&payload, 1);
    bb_write_byte(&payload, 0x60);
    bb_write_uleb(&payload, param_count);
    for (uint32_t i = 0; i < param_count; ++i) {
        bb_write_byte(&payload, param_types[i]);
    }
    bb_write_uleb(&payload, result_count);
    for (uint32_t i = 0; i < result_count; ++i) {
        bb_write_byte(&payload, result_types[i]);
    }
    if (!append_section(module, SECTION_TYPE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    // Function section: all functions use type 0.
    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        bb_write_uleb(&payload, 0);
    }
    if (!append_section(module, SECTION_FUNCTION, &payload)) {
        bb_free(&payload);
        return 0;
    }

    if (with_memory) {
        bb_reset(&payload);
        bb_write_uleb(&payload, 1);
        uint8_t flags = memory_has_max ? 0x01 : 0x00;
        bb_write_byte(&payload, flags);
        bb_write_uleb(&payload, memory_initial_pages);
        if (memory_has_max) {
            bb_write_uleb(&payload, memory_max_pages);
        }
        if (!append_section(module, SECTION_MEMORY, &payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    // Code section.
    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        const uint32_t body_size = (uint32_t)(body_sizes[i] + 1U);
        bb_write_uleb(&payload, body_size);
        bb_write_uleb(&payload, 0);
        bb_write_bytes(&payload, bodies[i], body_sizes[i]);
    }
    if (!append_section(module, SECTION_CODE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    bb_free(&payload);
    return 1;
}

static int build_module_with_locals(ByteBuffer* module,
                                    const uint8_t* const* bodies,
                                    const size_t* body_sizes,
                                    const uint8_t* const* locals,
                                    const size_t* locals_sizes,
                                    size_t func_count,
                                    const ByteBuffer* imports_payload,
                                    const ByteBuffer* globals_payload,
                                    int with_memory,
                                    uint32_t memory_initial_pages,
                                    int memory_has_max,
                                    uint32_t memory_max_pages,
                                    const uint8_t* result_types,
                                    uint32_t result_count,
                                    const uint8_t* param_types,
                                    uint32_t param_count) {
    if (!module || !bodies || !body_sizes || func_count == 0) {
        return 0;
    }
    if (result_count > 0 && !result_types) {
        return 0;
    }
    if (param_count > 0 && !param_types) {
        return 0;
    }
    bb_reset(module);
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (!bb_write_bytes(module, header, sizeof(header))) {
        return 0;
    }

    ByteBuffer payload = {0};

    // Type section: one empty function signature.
    bb_write_uleb(&payload, 1);
    bb_write_byte(&payload, 0x60);
    bb_write_uleb(&payload, param_count);
    for (uint32_t i = 0; i < param_count; ++i) {
        bb_write_byte(&payload, param_types[i]);
    }
    bb_write_uleb(&payload, result_count);
    for (uint32_t i = 0; i < result_count; ++i) {
        bb_write_byte(&payload, result_types[i]);
    }
    if (!append_section(module, SECTION_TYPE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    if (imports_payload && imports_payload->size > 0) {
        if (!append_section(module, SECTION_IMPORT, imports_payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    // Function section: all functions use type 0.
    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        bb_write_uleb(&payload, 0);
    }
    if (!append_section(module, SECTION_FUNCTION, &payload)) {
        bb_free(&payload);
        return 0;
    }

    if (with_memory) {
        bb_reset(&payload);
        bb_write_uleb(&payload, 1);
        uint8_t flags = memory_has_max ? 0x01 : 0x00;
        bb_write_byte(&payload, flags);
        bb_write_uleb(&payload, memory_initial_pages);
        if (memory_has_max) {
            bb_write_uleb(&payload, memory_max_pages);
        }
        if (!append_section(module, SECTION_MEMORY, &payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    if (globals_payload && globals_payload->size > 0) {
        if (!append_section(module, SECTION_GLOBAL, globals_payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    // Code section.
    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    const uint8_t empty_locals = 0;
    for (size_t i = 0; i < func_count; ++i) {
        const uint8_t* locals_bytes = (locals && locals_sizes && locals_sizes[i] > 0) ? locals[i] : &empty_locals;
        const size_t locals_size = (locals && locals_sizes && locals_sizes[i] > 0) ? locals_sizes[i] : 1U;
        const uint32_t body_size = (uint32_t)(locals_size + body_sizes[i]);
        bb_write_uleb(&payload, body_size);
        bb_write_bytes(&payload, locals_bytes, locals_size);
        bb_write_bytes(&payload, bodies[i], body_sizes[i]);
    }
    if (!append_section(module, SECTION_CODE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    bb_free(&payload);
    return 1;
}

static int build_module_with_custom_memory(ByteBuffer* module,
                                           const uint8_t* const* bodies,
                                           const size_t* body_sizes,
                                           size_t func_count,
                                           const ByteBuffer* memory_payload,
                                           const uint8_t* result_types,
                                           uint32_t result_count,
                                           const uint8_t* param_types,
                                           uint32_t param_count) {
    if (!module || !bodies || !body_sizes || func_count == 0) {
        return 0;
    }
    if (result_count > 0 && !result_types) {
        return 0;
    }
    if (param_count > 0 && !param_types) {
        return 0;
    }
    bb_reset(module);
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (!bb_write_bytes(module, header, sizeof(header))) {
        return 0;
    }

    ByteBuffer payload = {0};

    // Type section: one function signature.
    bb_write_uleb(&payload, 1);
    bb_write_byte(&payload, 0x60);
    bb_write_uleb(&payload, param_count);
    for (uint32_t i = 0; i < param_count; ++i) {
        bb_write_byte(&payload, param_types[i]);
    }
    bb_write_uleb(&payload, result_count);
    for (uint32_t i = 0; i < result_count; ++i) {
        bb_write_byte(&payload, result_types[i]);
    }
    if (!append_section(module, SECTION_TYPE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    // Function section: all functions use type 0.
    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        bb_write_uleb(&payload, 0);
    }
    if (!append_section(module, SECTION_FUNCTION, &payload)) {
        bb_free(&payload);
        return 0;
    }

    if (memory_payload && memory_payload->size > 0) {
        if (!append_section(module, SECTION_MEMORY, memory_payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    // Code section.
    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        const uint32_t body_size = (uint32_t)(body_sizes[i] + 1U);
        bb_write_uleb(&payload, body_size);
        bb_write_uleb(&payload, 0);
        bb_write_bytes(&payload, bodies[i], body_sizes[i]);
    }
    if (!append_section(module, SECTION_CODE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    bb_free(&payload);
    return 1;
}

static int build_module_with_types(ByteBuffer* module,
                                   const uint8_t* const* bodies,
                                   const size_t* body_sizes,
                                   size_t func_count,
                                   const TypeSpec* types,
                                   size_t type_count) {
    if (!module || !bodies || !body_sizes || func_count == 0 || !types || type_count == 0) {
        return 0;
    }
    for (size_t i = 0; i < type_count; ++i) {
        if (types[i].param_count > 0 && !types[i].param_types) {
            return 0;
        }
        if (types[i].result_count > 0 && !types[i].result_types) {
            return 0;
        }
    }

    bb_reset(module);
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (!bb_write_bytes(module, header, sizeof(header))) {
        return 0;
    }

    ByteBuffer payload = {0};

    bb_write_uleb(&payload, (uint32_t)type_count);
    for (size_t i = 0; i < type_count; ++i) {
        bb_write_byte(&payload, 0x60);
        bb_write_uleb(&payload, types[i].param_count);
        for (uint32_t j = 0; j < types[i].param_count; ++j) {
            bb_write_byte(&payload, types[i].param_types[j]);
        }
        bb_write_uleb(&payload, types[i].result_count);
        for (uint32_t j = 0; j < types[i].result_count; ++j) {
            bb_write_byte(&payload, types[i].result_types[j]);
        }
    }
    if (!append_section(module, SECTION_TYPE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        bb_write_uleb(&payload, 0);
    }
    if (!append_section(module, SECTION_FUNCTION, &payload)) {
        bb_free(&payload);
        return 0;
    }

    bb_reset(&payload);
    bb_write_uleb(&payload, (uint32_t)func_count);
    for (size_t i = 0; i < func_count; ++i) {
        const uint32_t body_size = (uint32_t)(body_sizes[i] + 1U);
        bb_write_uleb(&payload, body_size);
        bb_write_uleb(&payload, 0);
        bb_write_bytes(&payload, bodies[i], body_sizes[i]);
    }
    if (!append_section(module, SECTION_CODE, &payload)) {
        bb_free(&payload);
        return 0;
    }

    bb_free(&payload);
    return 1;
}

static WasmModule* load_module_from_bytes(const uint8_t* bytes, size_t size) {
    WasmModule* module = wasm_module_init_from_memory(bytes, size);
    if (!module) {
        return NULL;
    }
    if (wasm_load_header(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    if (wasm_scan_sections(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    if (wasm_load_types(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    if (wasm_load_functions(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    if (wasm_load_memories(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    if (wasm_load_globals(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    return module;
}

static int run_job(ByteBuffer* module_bytes, fa_Runtime** runtime_out, fa_Job** job_out, WasmModule** module_out) {
    WasmModule* module = load_module_from_bytes(module_bytes->data, module_bytes->size);
    if (!module) {
        return 0;
    }
    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        return 0;
    }
    if (fa_Runtime_attach_module(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 0;
    }
    fa_Job* job = fa_Runtime_create_job(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 0;
    }
    *runtime_out = runtime;
    *job_out = job;
    *module_out = module;
    return 1;
}

static void cleanup_job(fa_Runtime* runtime, fa_Job* job, WasmModule* module, ByteBuffer* module_bytes, ByteBuffer* instructions) {
    if (runtime && job) {
        (void)fa_Runtime_destroy_job(runtime, job);
    }
    if (runtime) {
        fa_Runtime_free(runtime);
    }
    if (module) {
        wasm_module_free(module);
    }
    if (module_bytes) {
        bb_free(module_bytes);
    }
    if (instructions) {
        bb_free(instructions);
    }
}

static int test_stack_arithmetic(void) {
    ByteBuffer instructions = {0};
    if (!bb_write_byte(&instructions, 0x41) || !bb_write_sleb32(&instructions, 7)) {
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x41) || !bb_write_sleb32(&instructions, 5)) {
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x6A) || !bb_write_byte(&instructions, 0x0B)) {
        bb_free(&instructions);
        return 1;
    }

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 12) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_div_by_zero_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 4);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x6D);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_multi_value_return(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x42);
    bb_write_sleb32(&instructions, 9);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_I32, VALTYPE_I64 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, result_types, 2, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* top = fa_JobStack_peek(&job->stack, 0);
    if (!top || top->kind != fa_job_value_i64 || top->payload.i64_value != 9) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* next = fa_JobStack_peek(&job->stack, 1);
    if (!next || next->kind != fa_job_value_i32 || next->payload.i32_value != 7) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 2) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_call_depth_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x10);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, NULL, 0, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    runtime->max_call_depth = 4;

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_CALL_DEPTH_EXCEEDED ? 0 : 1;
}

static int test_memory_oob_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 65536);
    bb_write_byte(&instructions, 0x28);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_memory_grow_failure(void) {
    ByteBuffer locals = {0};
    bb_write_uleb(&locals, 1);
    bb_write_uleb(&locals, 2);
    bb_write_byte(&locals, VALTYPE_I32);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x3F);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x21);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x40);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x21);
    bb_write_uleb(&instructions, 1);
    bb_write_byte(&instructions, 0x3F);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x20);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x6B);
    bb_write_byte(&instructions, 0x20);
    bb_write_uleb(&instructions, 1);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x6A);
    bb_write_byte(&instructions, 0x6A);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t* locals_list[] = { locals.data };
    const size_t locals_sizes[] = { locals.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  locals_list,
                                  locals_sizes,
                                  1,
                                  NULL,
                                  NULL,
                                  1,
                                  1,
                                  1,
                                  1,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&locals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&locals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* top = fa_JobStack_peek(&job->stack, 0);
    if (!top || top->kind != fa_job_value_i32 || top->payload.i32_value != 0) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_memory64_grow_size(void) {
    ByteBuffer memory_payload = {0};
    bb_write_uleb(&memory_payload, 1);
    bb_write_byte(&memory_payload, 0x05);
    bb_write_uleb64(&memory_payload, 1);
    bb_write_uleb64(&memory_payload, 2);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x42);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x40);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x3F);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_I64, VALTYPE_I64 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_custom_memory(&module_bytes, bodies, sizes, 1, &memory_payload, result_types, 2, NULL, 0)) {
        bb_free(&memory_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&memory_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* size = fa_JobStack_peek(&job->stack, 0);
    if (!size || size->kind != fa_job_value_i64 || size->payload.i64_value != 2) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* prev = fa_JobStack_peek(&job->stack, 1);
    if (!prev || prev->kind != fa_job_value_i64 || prev->payload.i64_value != 1) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 2) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_multi_memory_memarg(void) {
    ByteBuffer memory_payload = {0};
    bb_write_uleb(&memory_payload, 2);
    bb_write_byte(&memory_payload, 0x00);
    bb_write_uleb(&memory_payload, 1);
    bb_write_byte(&memory_payload, 0x00);
    bb_write_uleb(&memory_payload, 1);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 42);
    bb_write_byte(&instructions, 0x36);
    bb_write_uleb(&instructions, 1);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x28);
    bb_write_uleb(&instructions, 1);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_custom_memory(&module_bytes, bodies, sizes, 1, &memory_payload, kResultI32, 1, NULL, 0)) {
        bb_free(&memory_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&memory_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 42) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_bulk_memory_copy_fill(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0x11);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 4);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 11);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 8);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 4);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 10);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 8);
    bb_write_byte(&instructions, 0x28);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 0x11111111) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_table_get_unimplemented(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, NULL, 0, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE ? 0 : 1;
}

static int test_simd_unimplemented(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0);
    for (int i = 0; i < 16; ++i) {
        bb_write_byte(&instructions, 0);
    }
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, NULL, 0, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE ? 0 : 1;
}

static int test_i32_clz(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x67);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 31) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_f32_abs(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x43);
    bb_write_f32(&instructions, -1.5f);
    bb_write_byte(&instructions, 0x8B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultF32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_f32 || fabsf(value->payload.f32_value - 1.5f) > 0.0001f) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_local_get_set(void) {
    ByteBuffer locals = {0};
    bb_write_uleb(&locals, 1);
    bb_write_uleb(&locals, 1);
    bb_write_byte(&locals, VALTYPE_I32);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x21);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x20);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t* locals_list[] = { locals.data };
    const size_t locals_sizes[] = { locals.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  locals_list,
                                  locals_sizes,
                                  1,
                                  NULL,
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&locals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&locals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 7) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_local_tee(void) {
    ByteBuffer locals = {0};
    bb_write_uleb(&locals, 1);
    bb_write_uleb(&locals, 1);
    bb_write_byte(&locals, VALTYPE_I32);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 9);
    bb_write_byte(&instructions, 0x22);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x6A);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t* locals_list[] = { locals.data };
    const size_t locals_sizes[] = { locals.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  locals_list,
                                  locals_sizes,
                                  1,
                                  NULL,
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&locals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&locals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 10) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_br_if_stack_effect(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x02);
    bb_write_byte(&instructions, 0x7F);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x0D);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 7) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_i64_add(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x42);
    bb_write_sleb32(&instructions, 10);
    bb_write_byte(&instructions, 0x42);
    bb_write_sleb32(&instructions, 32);
    bb_write_byte(&instructions, 0x7C);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI64, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i64 || value->payload.i64_value != 42) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_f64_mul(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x44);
    bb_write_f64(&instructions, 2.5);
    bb_write_byte(&instructions, 0x44);
    bb_write_f64(&instructions, 4.0);
    bb_write_byte(&instructions, 0xA2);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultF64, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_f64 || fabs(value->payload.f64_value - 10.0) > 0.0001) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_trunc_f32_nan_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x43);
    bb_write_f32(&instructions, NAN);
    bb_write_byte(&instructions, 0xA8);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_trunc_f32_overflow_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x43);
    bb_write_f32(&instructions, 2147483648.0f);
    bb_write_byte(&instructions, 0xA8);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_trunc_f64_overflow_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x44);
    bb_write_f64(&instructions, (double)INT64_MAX * 2.0);
    bb_write_byte(&instructions, 0xB0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI64, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_if_else_false(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x04);
    bb_write_byte(&instructions, 0x7F);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x05);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 2);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 2) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_block_result_br(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x02);
    bb_write_byte(&instructions, 0x7F);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 3);
    bb_write_byte(&instructions, 0x0C);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 9);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 3) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_global_get_initializer(void) {
    ByteBuffer globals = {0};
    bb_write_uleb(&globals, 2);
    bb_write_byte(&globals, VALTYPE_I32);
    bb_write_byte(&globals, 0);
    bb_write_byte(&globals, 0x41);
    bb_write_sleb32(&globals, 5);
    bb_write_byte(&globals, 0x0B);
    bb_write_byte(&globals, VALTYPE_I32);
    bb_write_byte(&globals, 0);
    bb_write_byte(&globals, 0x23);
    bb_write_uleb(&globals, 0);
    bb_write_byte(&globals, 0x0B);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x23);
    bb_write_uleb(&instructions, 1);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  NULL,
                                  NULL,
                                  1,
                                  NULL,
                                  &globals,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&globals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&globals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 5) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_global_import_initializer(void) {
    ByteBuffer imports = {0};
    bb_write_uleb(&imports, 1);
    bb_write_string(&imports, "env");
    bb_write_string(&imports, "g0");
    bb_write_byte(&imports, 0x03);
    bb_write_byte(&imports, VALTYPE_I32);
    bb_write_byte(&imports, 0);

    ByteBuffer globals = {0};
    bb_write_uleb(&globals, 1);
    bb_write_byte(&globals, VALTYPE_I32);
    bb_write_byte(&globals, 0);
    bb_write_byte(&globals, 0x23);
    bb_write_uleb(&globals, 0);
    bb_write_byte(&globals, 0x0B);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x23);
    bb_write_uleb(&instructions, 1);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  NULL,
                                  NULL,
                                  1,
                                  &imports,
                                  &globals,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&imports);
        bb_free(&globals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&imports);
    bb_free(&globals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    fa_JobValue import_value = {0};
    import_value.kind = fa_job_value_i32;
    import_value.is_signed = true;
    import_value.bit_width = 32U;
    import_value.payload.i32_value = 11;
    if (fa_Runtime_set_imported_global(runtime, 0, &import_value) != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 11) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_block_result_arity_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x02);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_I32, VALTYPE_I64 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, result_types, 2, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_br_to_end(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x02);
    bb_write_byte(&instructions, 0x7F);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x0C);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 2);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 1) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_br_table_branch(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x02);
    bb_write_byte(&instructions, 0x7F);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x0E);
    bb_write_uleb(&instructions, 1);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 5);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 7) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_loop_label_result(void) {
    ByteBuffer locals = {0};
    bb_write_uleb(&locals, 1);
    bb_write_uleb(&locals, 1);
    bb_write_byte(&locals, VALTYPE_I32);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 5);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x21);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x03);
    bb_write_byte(&instructions, 0x7F);
    bb_write_byte(&instructions, 0x20);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x45);
    bb_write_byte(&instructions, 0x04);
    bb_write_byte(&instructions, 0x40);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x21);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x0C);
    bb_write_uleb(&instructions, 1);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x6A);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t* locals_list[] = { locals.data };
    const size_t locals_sizes[] = { locals.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  locals_list,
                                  locals_sizes,
                                  1,
                                  NULL,
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&locals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&locals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 12) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_loop_label_type_mismatch_trap(void) {
    const uint8_t loop_params[] = { VALTYPE_I32 };
    TypeSpec types[2] = {0};
    types[0].param_types = NULL;
    types[0].param_count = 0;
    types[0].result_types = NULL;
    types[0].result_count = 0;
    types[1].param_types = loop_params;
    types[1].param_count = 1;
    types[1].result_types = NULL;
    types[1].result_count = 0;

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x03);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x43);
    bb_write_f32(&instructions, 0.0f);
    bb_write_byte(&instructions, 0x0C);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_types(&module_bytes, bodies, sizes, 1, types, 2)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_global_get_set(void) {
    ByteBuffer globals = {0};
    bb_write_uleb(&globals, 1);
    bb_write_byte(&globals, VALTYPE_I32);
    bb_write_byte(&globals, 1);
    bb_write_byte(&globals, 0x41);
    bb_write_sleb32(&globals, 4);
    bb_write_byte(&globals, 0x0B);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x23);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 6);
    bb_write_byte(&instructions, 0x6A);
    bb_write_byte(&instructions, 0x24);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x23);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  NULL,
                                  NULL,
                                  1,
                                  NULL,
                                  &globals,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&globals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&globals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 10) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_global_set_immutable_trap(void) {
    ByteBuffer globals = {0};
    bb_write_uleb(&globals, 1);
    bb_write_byte(&globals, VALTYPE_I32);
    bb_write_byte(&globals, 0);
    bb_write_byte(&globals, 0x41);
    bb_write_sleb32(&globals, 1);
    bb_write_byte(&globals, 0x0B);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x24);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  NULL,
                                  NULL,
                                  1,
                                  NULL,
                                  &globals,
                                  0,
                                  0,
                                  0,
                                  0,
                                  NULL,
                                  0,
                                  NULL,
                                  0)) {
        bb_free(&globals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&globals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_global_set_type_mismatch_trap(void) {
    ByteBuffer globals = {0};
    bb_write_uleb(&globals, 1);
    bb_write_byte(&globals, VALTYPE_I32);
    bb_write_byte(&globals, 1);
    bb_write_byte(&globals, 0x41);
    bb_write_sleb32(&globals, 0);
    bb_write_byte(&globals, 0x0B);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x43);
    bb_write_f32(&instructions, 1.0f);
    bb_write_byte(&instructions, 0x24);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  NULL,
                                  NULL,
                                  1,
                                  NULL,
                                  &globals,
                                  0,
                                  0,
                                  0,
                                  0,
                                  NULL,
                                  0,
                                  NULL,
                                  0)) {
        bb_free(&globals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&globals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_local_f32_default(void) {
    ByteBuffer locals = {0};
    bb_write_uleb(&locals, 1);
    bb_write_uleb(&locals, 1);
    bb_write_byte(&locals, VALTYPE_F32);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x20);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t* locals_list[] = { locals.data };
    const size_t locals_sizes[] = { locals.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  locals_list,
                                  locals_sizes,
                                  1,
                                  NULL,
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultF32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&locals);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&locals);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_f32 || fabsf(value->payload.f32_value) > 0.0001f) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

int main(void) {
    int failures = 0;

    if (test_stack_arithmetic() != 0) {
        printf("FAIL: test_stack_arithmetic\n");
        failures++;
    }
    if (test_div_by_zero_trap() != 0) {
        printf("FAIL: test_div_by_zero_trap\n");
        failures++;
    }
    if (test_multi_value_return() != 0) {
        printf("FAIL: test_multi_value_return\n");
        failures++;
    }
    if (test_call_depth_trap() != 0) {
        printf("FAIL: test_call_depth_trap\n");
        failures++;
    }
    if (test_memory_oob_trap() != 0) {
        printf("FAIL: test_memory_oob_trap\n");
        failures++;
    }
    if (test_memory_grow_failure() != 0) {
        printf("FAIL: test_memory_grow_failure\n");
        failures++;
    }
    if (test_memory64_grow_size() != 0) {
        printf("FAIL: test_memory64_grow_size\n");
        failures++;
    }
    if (test_multi_memory_memarg() != 0) {
        printf("FAIL: test_multi_memory_memarg\n");
        failures++;
    }
    if (test_bulk_memory_copy_fill() != 0) {
        printf("FAIL: test_bulk_memory_copy_fill\n");
        failures++;
    }
    if (test_table_get_unimplemented() != 0) {
        printf("FAIL: test_table_get_unimplemented\n");
        failures++;
    }
    if (test_simd_unimplemented() != 0) {
        printf("FAIL: test_simd_unimplemented\n");
        failures++;
    }
    if (test_i32_clz() != 0) {
        printf("FAIL: test_i32_clz\n");
        failures++;
    }
    if (test_f32_abs() != 0) {
        printf("FAIL: test_f32_abs\n");
        failures++;
    }
    if (test_local_get_set() != 0) {
        printf("FAIL: test_local_get_set\n");
        failures++;
    }
    if (test_local_tee() != 0) {
        printf("FAIL: test_local_tee\n");
        failures++;
    }
    if (test_br_if_stack_effect() != 0) {
        printf("FAIL: test_br_if_stack_effect\n");
        failures++;
    }
    if (test_i64_add() != 0) {
        printf("FAIL: test_i64_add\n");
        failures++;
    }
    if (test_f64_mul() != 0) {
        printf("FAIL: test_f64_mul\n");
        failures++;
    }
    if (test_trunc_f32_nan_trap() != 0) {
        printf("FAIL: test_trunc_f32_nan_trap\n");
        failures++;
    }
    if (test_trunc_f32_overflow_trap() != 0) {
        printf("FAIL: test_trunc_f32_overflow_trap\n");
        failures++;
    }
    if (test_trunc_f64_overflow_trap() != 0) {
        printf("FAIL: test_trunc_f64_overflow_trap\n");
        failures++;
    }
    if (test_if_else_false() != 0) {
        printf("FAIL: test_if_else_false\n");
        failures++;
    }
    if (test_block_result_br() != 0) {
        printf("FAIL: test_block_result_br\n");
        failures++;
    }
    if (test_block_result_arity_trap() != 0) {
        printf("FAIL: test_block_result_arity_trap\n");
        failures++;
    }
    if (test_br_to_end() != 0) {
        printf("FAIL: test_br_to_end\n");
        failures++;
    }
    if (test_br_table_branch() != 0) {
        printf("FAIL: test_br_table_branch\n");
        failures++;
    }
    if (test_loop_label_result() != 0) {
        printf("FAIL: test_loop_label_result\n");
        failures++;
    }
    if (test_loop_label_type_mismatch_trap() != 0) {
        printf("FAIL: test_loop_label_type_mismatch_trap\n");
        failures++;
    }
    if (test_global_get_set() != 0) {
        printf("FAIL: test_global_get_set\n");
        failures++;
    }
    if (test_global_get_initializer() != 0) {
        printf("FAIL: test_global_get_initializer\n");
        failures++;
    }
    if (test_global_import_initializer() != 0) {
        printf("FAIL: test_global_import_initializer\n");
        failures++;
    }
    if (test_global_set_immutable_trap() != 0) {
        printf("FAIL: test_global_set_immutable_trap\n");
        failures++;
    }
    if (test_global_set_type_mismatch_trap() != 0) {
        printf("FAIL: test_global_set_type_mismatch_trap\n");
        failures++;
    }
    if (test_local_f32_default() != 0) {
        printf("FAIL: test_local_f32_default\n");
        failures++;
    }

    if (failures == 0) {
        printf("All tests passed.\n");
    } else {
        printf("%d test(s) failed.\n", failures);
    }
    return failures ? 1 : 0;
}
