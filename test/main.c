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
                        uint32_t memory_max_pages) {
    if (!module || !bodies || !body_sizes || func_count == 0) {
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
    bb_write_uleb(&payload, 0);
    bb_write_uleb(&payload, 0);
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
                                    int with_memory,
                                    uint32_t memory_initial_pages,
                                    int memory_has_max,
                                    uint32_t memory_max_pages) {
    if (!module || !bodies || !body_sizes || func_count == 0) {
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
    bb_write_uleb(&payload, 0);
    bb_write_uleb(&payload, 0);
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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

static int test_call_depth_trap(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x10);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 0, 0)) {
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
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x3F);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x40);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x3F);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 1, 1)) {
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
    const fa_JobValue* mid = fa_JobStack_peek(&job->stack, 1);
    const fa_JobValue* base = fa_JobStack_peek(&job->stack, 2);
    if (!top || !mid || !base) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (top->payload.i32_value != 1 || base->payload.i32_value != 1 || mid->payload.i32_value != -1) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module_with_locals(&module_bytes, bodies, sizes, locals_list, locals_sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module_with_locals(&module_bytes, bodies, sizes, locals_list, locals_sizes, 1, 0, 0, 0, 0)) {
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
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x0D);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0)) {
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

    if (failures == 0) {
        printf("All tests passed.\n");
    } else {
        printf("%d test(s) failed.\n", failures);
    }
    return failures ? 1 : 0;
}
