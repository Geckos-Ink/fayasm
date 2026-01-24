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

typedef int (*TestFn)(void);

typedef struct {
    const char* name;
    const char* area;
    const char* hint;
    TestFn fn;
} TestCase;

typedef struct {
    int calls;
    int status;
} TrapState;

static int trap_handler(fa_Runtime* runtime, uint32_t function_index, void* user_data) {
    (void)runtime;
    (void)function_index;
    TrapState* state = (TrapState*)user_data;
    if (state) {
        state->calls += 1;
        return state->status;
    }
    return FA_RUNTIME_ERR_TRAP;
}

static int host_add(fa_Runtime* runtime, const fa_RuntimeHostCall* call, void* user_data) {
    (void)runtime;
    (void)user_data;
    if (!fa_RuntimeHostCall_expect(call, 2, 1)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    i32 lhs = 0;
    i32 rhs = 0;
    if (!fa_RuntimeHostCall_arg_i32(call, 0, &lhs) ||
        !fa_RuntimeHostCall_arg_i32(call, 1, &rhs)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!fa_RuntimeHostCall_set_i32(call, 0, lhs + rhs)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}

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

static void test_set_env(const char* name, const char* value) {
    if (!name) {
        return;
    }
#if defined(_WIN32)
    if (!value) {
        _putenv_s(name, "");
    } else {
        _putenv_s(name, value);
    }
#else
    if (!value) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
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

static int build_module_with_sections(ByteBuffer* module,
                                      const uint8_t* const* bodies,
                                      const size_t* body_sizes,
                                      size_t func_count,
                                      const ByteBuffer* table_payload,
                                      const ByteBuffer* memory_payload,
                                      const ByteBuffer* element_payload,
                                      const ByteBuffer* data_payload,
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

    if (table_payload && table_payload->size > 0) {
        if (!append_section(module, SECTION_TABLE, table_payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    if (memory_payload && memory_payload->size > 0) {
        if (!append_section(module, SECTION_MEMORY, memory_payload)) {
            bb_free(&payload);
            return 0;
        }
    }

    if (element_payload && element_payload->size > 0) {
        if (!append_section(module, SECTION_ELEMENT, element_payload)) {
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

    if (data_payload && data_payload->size > 0) {
        if (!append_section(module, SECTION_DATA, data_payload)) {
            bb_free(&payload);
            return 0;
        }
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
    if (wasm_load_tables(module) != 0) {
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
    if (wasm_load_elements(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    if (wasm_load_data(module) != 0) {
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
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 0;
    }
    fa_Job* job = fa_Runtime_createJob(runtime);
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
        (void)fa_Runtime_destroyJob(runtime, job);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

static int test_jit_cache_dispatch(void) {
    test_set_env("FAYASM_MICROCODE", "1");
    test_set_env("FAYASM_JIT_PRESCAN", "1");

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

    WasmModule* module = load_module_from_bytes(module_bytes.data, module_bytes.size);
    if (!module) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    runtime->jit_context.config.min_ram_bytes = 0;
    runtime->jit_context.config.min_cpu_count = 1;
    runtime->jit_context.config.min_hot_loop_hits = 0;
    runtime->jit_context.config.min_executed_ops = 1;
    runtime->jit_context.config.min_advantage_score = 0.0f;
    runtime->jit_context.config.prescan_functions = true;

    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 12) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (!fa_ops_microcode_enabled() || runtime->jit_prepared_executions == 0) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_microcode_float_select(void) {
    test_set_env("FAYASM_MICROCODE", "1");
    if (!fa_ops_microcode_enabled()) {
        return 1;
    }
    const Operation* steps = NULL;
    uint8_t count = 0;
    if (!fa_ops_get_microcode_steps(0x8B, &steps, &count) || !steps || count == 0) {
        return 1;
    }
    if (!fa_ops_get_microcode_steps(0x96, &steps, &count) || !steps || count == 0) {
        return 1;
    }
    if (!fa_ops_get_microcode_steps(0xBC, &steps, &count) || !steps || count == 0) {
        return 1;
    }
    if (!fa_ops_get_microcode_steps(0x1B, &steps, &count) || !steps || count == 0) {
        return 1;
    }
    return 0;
}

static int test_host_import_call(void) {
    ByteBuffer imports = {0};
    if (!bb_write_uleb(&imports, 1)) {
        bb_free(&imports);
        return 1;
    }
    if (!bb_write_string(&imports, "env") || !bb_write_string(&imports, "host_add")) {
        bb_free(&imports);
        return 1;
    }
    if (!bb_write_byte(&imports, 0) || !bb_write_uleb(&imports, 0)) {
        bb_free(&imports);
        return 1;
    }

    ByteBuffer instructions = {0};
    if (!bb_write_byte(&instructions, 0x41) || !bb_write_sleb32(&instructions, 7)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x41) || !bb_write_sleb32(&instructions, 5)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x10) || !bb_write_uleb(&instructions, 0)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x0B)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t param_types[] = { VALTYPE_I32, VALTYPE_I32 };
    const uint8_t result_types[] = { VALTYPE_I32 };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_locals(&module_bytes,
                                  bodies,
                                  sizes,
                                  NULL,
                                  NULL,
                                  1,
                                  &imports,
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  result_types,
                                  1,
                                  param_types,
                                  2)) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&imports);

    WasmModule* module = load_module_from_bytes(module_bytes.data, module_bytes.size);
    if (!module) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        cleanup_job(NULL, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_Runtime_bindHostFunction(runtime, "env", "host_add", host_add, NULL) != FA_RUNTIME_OK) {
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 1);
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

static int test_imported_memory_binding(void) {
    ByteBuffer imports = {0};
    if (!bb_write_uleb(&imports, 1)) {
        bb_free(&imports);
        return 1;
    }
    if (!bb_write_string(&imports, "env") || !bb_write_string(&imports, "mem0")) {
        bb_free(&imports);
        return 1;
    }
    if (!bb_write_byte(&imports, 0x02) || !bb_write_uleb(&imports, 0x00) ||
        !bb_write_uleb(&imports, 1)) {
        bb_free(&imports);
        return 1;
    }

    ByteBuffer instructions = {0};
    if (!bb_write_byte(&instructions, 0x41) || !bb_write_sleb32(&instructions, 0)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x28) || !bb_write_uleb(&instructions, 0) ||
        !bb_write_uleb(&instructions, 0)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x0B)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }

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
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    WasmModule* module = load_module_from_bytes(module_bytes.data, module_bytes.size);
    if (!module) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    uint8_t* memory_data = (uint8_t*)calloc(FA_WASM_PAGE_SIZE, 1);
    if (!memory_data) {
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    i32 expected = 42;
    memcpy(memory_data, &expected, sizeof(expected));
    fa_RuntimeHostMemory host_memory = {0};
    host_memory.data = memory_data;
    host_memory.size_bytes = FA_WASM_PAGE_SIZE;
    if (fa_Runtime_bindImportedMemory(runtime, "env", "mem0", &host_memory) != FA_RUNTIME_OK) {
        free(memory_data);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        free(memory_data);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        free(memory_data);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        free(memory_data);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != expected) {
        free(memory_data);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    free(memory_data);
    bb_free(&imports);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_imported_table_binding(void) {
    ByteBuffer imports = {0};
    if (!bb_write_uleb(&imports, 1)) {
        bb_free(&imports);
        return 1;
    }
    if (!bb_write_string(&imports, "env") || !bb_write_string(&imports, "tbl0")) {
        bb_free(&imports);
        return 1;
    }
    if (!bb_write_byte(&imports, 0x01) || !bb_write_byte(&imports, VALTYPE_FUNCREF) ||
        !bb_write_uleb(&imports, 0x00) || !bb_write_uleb(&imports, 3)) {
        bb_free(&imports);
        return 1;
    }

    ByteBuffer instructions = {0};
    if (!bb_write_byte(&instructions, 0xFC) || !bb_write_uleb(&instructions, 16) ||
        !bb_write_uleb(&instructions, 0)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }
    if (!bb_write_byte(&instructions, 0x0B)) {
        bb_free(&imports);
        bb_free(&instructions);
        return 1;
    }

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
                                  NULL,
                                  0,
                                  0,
                                  0,
                                  0,
                                  kResultI32,
                                  1,
                                  NULL,
                                  0)) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    WasmModule* module = load_module_from_bytes(module_bytes.data, module_bytes.size);
    if (!module) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        bb_free(&imports);
        cleanup_job(NULL, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    const uint32_t table_size = 3;
    fa_ptr* table_data = (fa_ptr*)calloc(table_size, sizeof(fa_ptr));
    if (!table_data) {
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    fa_RuntimeHostTable host_table = {0};
    host_table.data = table_data;
    host_table.size = table_size;
    if (fa_Runtime_bindImportedTable(runtime, "env", "tbl0", &host_table) != FA_RUNTIME_OK) {
        free(table_data);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        free(table_data);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        free(table_data);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        free(table_data);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != (i32)table_size) {
        free(table_data);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    free(table_data);
    bb_free(&imports);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_CALL_DEPTH_EXCEEDED ? 0 : 1;
}

static int test_function_trap_allow(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
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

    TrapState state = {0};
    state.status = FA_RUNTIME_OK;
    fa_RuntimeTrapHooks hooks = { trap_handler, &state };
    fa_Runtime_setTrapHooks(runtime, &hooks);
    if (fa_Runtime_setFunctionTrap(runtime, 0, true) != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK || state.calls != 1) {
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

static int test_function_trap_block(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x01);
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

    TrapState state = {0};
    state.status = FA_RUNTIME_ERR_TRAP;
    fa_RuntimeTrapHooks hooks = { trap_handler, &state };
    fa_Runtime_setTrapHooks(runtime, &hooks);
    if (fa_Runtime_setFunctionTrap(runtime, 0, true) != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_ERR_TRAP || state.calls != 1) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

static int test_data_segment_init(void) {
    ByteBuffer memory_payload = {0};
    bb_write_uleb(&memory_payload, 1);
    bb_write_byte(&memory_payload, 0x00);
    bb_write_uleb(&memory_payload, 1);

    ByteBuffer data_payload = {0};
    bb_write_uleb(&data_payload, 1);
    bb_write_uleb(&data_payload, 1);
    bb_write_uleb(&data_payload, 4);
    bb_write_byte(&data_payload, 5);
    bb_write_byte(&data_payload, 0);
    bb_write_byte(&data_payload, 0);
    bb_write_byte(&data_payload, 0);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 4);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 8);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x28);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    1,
                                    NULL,
                                    &memory_payload,
                                    NULL,
                                    &data_payload,
                                    kResultI32,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&memory_payload);
        bb_free(&data_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&memory_payload);
    bb_free(&data_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 5) {
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

static int test_data_segment_active(void) {
    ByteBuffer memory_payload = {0};
    bb_write_uleb(&memory_payload, 1);
    bb_write_byte(&memory_payload, 0x00);
    bb_write_uleb(&memory_payload, 1);

    ByteBuffer data_payload = {0};
    bb_write_uleb(&data_payload, 1);
    bb_write_uleb(&data_payload, 0);
    bb_write_byte(&data_payload, 0x41);
    bb_write_sleb32(&data_payload, 0);
    bb_write_byte(&data_payload, 0x0B);
    bb_write_uleb(&data_payload, 4);
    bb_write_byte(&data_payload, 0x2A);
    bb_write_byte(&data_payload, 0);
    bb_write_byte(&data_payload, 0);
    bb_write_byte(&data_payload, 0);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x28);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    1,
                                    NULL,
                                    &memory_payload,
                                    NULL,
                                    &data_payload,
                                    kResultI32,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&memory_payload);
        bb_free(&data_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&memory_payload);
    bb_free(&data_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

static int test_data_drop_trap(void) {
    ByteBuffer memory_payload = {0};
    bb_write_uleb(&memory_payload, 1);
    bb_write_byte(&memory_payload, 0x00);
    bb_write_uleb(&memory_payload, 1);

    ByteBuffer data_payload = {0};
    bb_write_uleb(&data_payload, 1);
    bb_write_uleb(&data_payload, 1);
    bb_write_uleb(&data_payload, 1);
    bb_write_byte(&data_payload, 0x7F);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 8);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 9);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 8);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    1,
                                    NULL,
                                    &memory_payload,
                                    NULL,
                                    &data_payload,
                                    NULL,
                                    0,
                                    NULL,
                                    0)) {
        bb_free(&memory_payload);
        bb_free(&data_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&memory_payload);
    bb_free(&data_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_table_init_copy(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 2);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 1);
    bb_write_byte(&elem_payload, 0x00);
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 1);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 12);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 14);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    ByteBuffer dummy = {0};
    bb_write_byte(&dummy, 0x0B);

    const uint8_t* bodies[] = { instructions.data, dummy.data };
    const size_t sizes[] = { instructions.size, dummy.size };
    const uint8_t result_types[] = { VALTYPE_FUNCREF };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    2,
                                    &table_payload,
                                    NULL,
                                    &elem_payload,
                                    NULL,
                                    result_types,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        bb_free(&elem_payload);
        bb_free(&dummy);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&elem_payload);
    bb_free(&dummy);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_ref || value->payload.ref_value != 1) {
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

static int test_table_fill_size(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 3);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x41);
    bb_write_sleb32(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x0B);
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 1);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 2);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 17);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 2);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 16);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    ByteBuffer dummy = {0};
    bb_write_byte(&dummy, 0x0B);

    const uint8_t* bodies[] = { instructions.data, dummy.data };
    const size_t sizes[] = { instructions.size, dummy.size };
    const uint8_t result_types[] = { VALTYPE_FUNCREF, VALTYPE_I32 };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    2,
                                    &table_payload,
                                    NULL,
                                    &elem_payload,
                                    NULL,
                                    result_types,
                                    2,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        bb_free(&elem_payload);
        bb_free(&dummy);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&elem_payload);
    bb_free(&dummy);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* size_value = fa_JobStack_peek(&job->stack, 0);
    if (!size_value || size_value->kind != fa_job_value_i32 || size_value->payload.i32_value != 3) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* ref_value = fa_JobStack_peek(&job->stack, 1);
    if (!ref_value || ref_value->kind != fa_job_value_ref || ref_value->payload.ref_value != 1) {
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

static int test_elem_drop_trap(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 1);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 1);
    bb_write_byte(&elem_payload, 0x00);
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 0);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 12);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 13);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 12);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    ByteBuffer dummy = {0};
    bb_write_byte(&dummy, 0x0B);

    const uint8_t* bodies[] = { instructions.data, dummy.data };
    const size_t sizes[] = { instructions.size, dummy.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    2,
                                    &table_payload,
                                    NULL,
                                    &elem_payload,
                                    NULL,
                                    NULL,
                                    0,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        bb_free(&elem_payload);
        bb_free(&dummy);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&elem_payload);
    bb_free(&dummy);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_table_grow(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 2);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, 15);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    1,
                                    &table_payload,
                                    NULL,
                                    NULL,
                                    NULL,
                                    kResultI32,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&table_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

static int test_simd_v128_const(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 12);
    for (int i = 0; i < 16; ++i) {
        bb_write_byte(&instructions, (uint8_t)i);
    }
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_V128 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, result_types, 1, NULL, 0)) {
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    const uint8_t expected[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    if (!value || value->kind != fa_job_value_v128 ||
        memcmp(&value->payload.v128_value, expected, sizeof(expected)) != 0) {
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

static int test_simd_i32x4_splat(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 7);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 17);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_V128 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, result_types, 1, NULL, 0)) {
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    const uint32_t expected[4] = {7, 7, 7, 7};
    if (!value || value->kind != fa_job_value_v128 ||
        memcmp(&value->payload.v128_value, expected, sizeof(expected)) != 0) {
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

static int test_simd_v128_load_store(void) {
    ByteBuffer instructions = {0};
    const uint8_t payload[16] = {
        0x10, 0x11, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x1E, 0x1F
    };

    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, payload, sizeof(payload));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0b);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x00);
    bb_write_uleb(&instructions, 0);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_V128 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 0, 0, result_types, 1, NULL, 0)) {
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_v128 ||
        memcmp(&value->payload.v128_value, payload, sizeof(payload)) != 0) {
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

static int test_simd_i8x16_add(void) {
    ByteBuffer instructions = {0};
    uint8_t left[16] = {0};
    uint8_t right[16] = {0};
    uint8_t expected[16] = {0};
    memset(left, 1, sizeof(left));
    memset(right, 2, sizeof(right));
    memset(expected, 3, sizeof(expected));

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, left, sizeof(left));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, right, sizeof(right));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x6a);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t result_types[] = { VALTYPE_V128 };
    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, result_types, 1, NULL, 0)) {
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_v128 ||
        memcmp(&value->payload.v128_value, expected, sizeof(expected)) != 0) {
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

static int test_simd_i8x16_replace_extract(void) {
    ByteBuffer instructions = {0};
    uint8_t zeros[16] = {0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, zeros, sizeof(zeros));
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 127);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x17);
    bb_write_byte(&instructions, 3);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x16);
    bb_write_byte(&instructions, 3);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 127) {
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

static int test_simd_trunc_sat_f32x4(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x43);
    bb_write_f32(&instructions, 3.75f);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x13);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0xdc);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x1b);
    bb_write_byte(&instructions, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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
    if (fa_Runtime_setImportedGlobal(runtime, 0, &import_value) != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

    int status = fa_Runtime_executeJob(runtime, job, 0);
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

#define TEST_CASE(name, area, hint, fn) { name, area, hint, fn }
static const TestCase kTestCases[] = {
    TEST_CASE("test_jit_cache_dispatch", "jit", "src/fa_runtime.c (jit dispatch), src/fa_jit.c (prepared ops)", test_jit_cache_dispatch),
    TEST_CASE("test_microcode_float_select", "jit", "src/fa_ops.c (microcode table)", test_microcode_float_select),
    TEST_CASE("test_host_import_call", "runtime", "src/fa_runtime.c (host imports), src/fa_wasm.c (import parsing)", test_host_import_call),
    TEST_CASE("test_imported_memory_binding", "memory", "src/fa_runtime.c (host memory imports), src/fa_ops.c (load)", test_imported_memory_binding),
    TEST_CASE("test_imported_table_binding", "table", "src/fa_runtime.c (host table imports), src/fa_ops.c (table.size)", test_imported_table_binding),
    TEST_CASE("test_stack_arithmetic", "arith", "src/fa_ops.c (integer ops)", test_stack_arithmetic),
    TEST_CASE("test_div_by_zero_trap", "arith", "src/fa_ops.c (div traps)", test_div_by_zero_trap),
    TEST_CASE("test_multi_value_return", "control", "src/fa_runtime.c (multi-value returns)", test_multi_value_return),
    TEST_CASE("test_call_depth_trap", "control", "src/fa_runtime.c (call depth)", test_call_depth_trap),
    TEST_CASE("test_function_trap_allow", "trap", "src/fa_runtime.c (function trap hooks)", test_function_trap_allow),
    TEST_CASE("test_function_trap_block", "trap", "src/fa_runtime.c (function trap hooks)", test_function_trap_block),
    TEST_CASE("test_memory_oob_trap", "memory", "src/fa_ops.c (load/store), src/fa_runtime.c (bounds)", test_memory_oob_trap),
    TEST_CASE("test_memory_grow_failure", "memory", "src/fa_ops.c (memory.grow), src/fa_runtime.c (grow)", test_memory_grow_failure),
    TEST_CASE("test_memory64_grow_size", "memory64", "src/fa_ops.c (memory.grow), src/fa_runtime.c (grow)", test_memory64_grow_size),
    TEST_CASE("test_multi_memory_memarg", "memory", "src/fa_runtime.c (memarg decode), src/fa_ops.c (load/store)", test_multi_memory_memarg),
    TEST_CASE("test_bulk_memory_copy_fill", "bulk-memory", "src/fa_ops.c (op_bulk_memory)", test_bulk_memory_copy_fill),
    TEST_CASE("test_data_segment_init", "bulk-memory", "src/fa_ops.c (memory.init), src/fa_runtime.c (segments)", test_data_segment_init),
    TEST_CASE("test_data_segment_active", "bulk-memory", "src/fa_runtime.c (segments init)", test_data_segment_active),
    TEST_CASE("test_data_drop_trap", "bulk-memory", "src/fa_ops.c (data.drop)", test_data_drop_trap),
    TEST_CASE("test_table_init_copy", "table", "src/fa_ops.c (table.init/copy), src/fa_runtime.c (tables)", test_table_init_copy),
    TEST_CASE("test_table_fill_size", "table", "src/fa_ops.c (table.fill/size)", test_table_fill_size),
    TEST_CASE("test_elem_drop_trap", "table", "src/fa_ops.c (elem.drop)", test_elem_drop_trap),
    TEST_CASE("test_table_grow", "table", "src/fa_ops.c (table.grow)", test_table_grow),
    TEST_CASE("test_simd_v128_const", "simd", "src/fa_runtime.c (simd decode), src/fa_ops.c (op_simd)", test_simd_v128_const),
    TEST_CASE("test_simd_i32x4_splat", "simd", "src/fa_ops.c (op_simd splat)", test_simd_i32x4_splat),
    TEST_CASE("test_simd_v128_load_store", "simd", "src/fa_runtime.c (simd memarg), src/fa_ops.c (op_simd)", test_simd_v128_load_store),
    TEST_CASE("test_simd_i8x16_add", "simd", "src/fa_ops.c (i8x16.add)", test_simd_i8x16_add),
    TEST_CASE("test_simd_i8x16_replace_extract", "simd", "src/fa_ops.c (lane ops)", test_simd_i8x16_replace_extract),
    TEST_CASE("test_simd_trunc_sat_f32x4", "simd", "src/fa_ops.c (trunc sat)", test_simd_trunc_sat_f32x4),
    TEST_CASE("test_i32_clz", "arith", "src/fa_ops.c (i32 clz)", test_i32_clz),
    TEST_CASE("test_f32_abs", "arith", "src/fa_ops.c (f32 abs)", test_f32_abs),
    TEST_CASE("test_local_get_set", "locals", "src/fa_ops.c (local.get/set)", test_local_get_set),
    TEST_CASE("test_local_tee", "locals", "src/fa_ops.c (local.tee)", test_local_tee),
    TEST_CASE("test_br_if_stack_effect", "control", "src/fa_runtime.c (control stack)", test_br_if_stack_effect),
    TEST_CASE("test_i64_add", "arith", "src/fa_ops.c (i64 add)", test_i64_add),
    TEST_CASE("test_f64_mul", "arith", "src/fa_ops.c (f64 mul)", test_f64_mul),
    TEST_CASE("test_trunc_f32_nan_trap", "conversion", "src/fa_ops.c (trunc f32->i)", test_trunc_f32_nan_trap),
    TEST_CASE("test_trunc_f32_overflow_trap", "conversion", "src/fa_ops.c (trunc f32->i)", test_trunc_f32_overflow_trap),
    TEST_CASE("test_trunc_f64_overflow_trap", "conversion", "src/fa_ops.c (trunc f64->i)", test_trunc_f64_overflow_trap),
    TEST_CASE("test_if_else_false", "control", "src/fa_runtime.c (if/else)", test_if_else_false),
    TEST_CASE("test_block_result_br", "control", "src/fa_runtime.c (block results)", test_block_result_br),
    TEST_CASE("test_block_result_arity_trap", "control", "src/fa_runtime.c (arity checks)", test_block_result_arity_trap),
    TEST_CASE("test_br_to_end", "control", "src/fa_runtime.c (br)", test_br_to_end),
    TEST_CASE("test_br_table_branch", "control", "src/fa_runtime.c (br_table)", test_br_table_branch),
    TEST_CASE("test_loop_label_result", "control", "src/fa_runtime.c (loop label types)", test_loop_label_result),
    TEST_CASE("test_loop_label_type_mismatch_trap", "control", "src/fa_runtime.c (loop label types)", test_loop_label_type_mismatch_trap),
    TEST_CASE("test_global_get_set", "globals", "src/fa_ops.c (global.get/set)", test_global_get_set),
    TEST_CASE("test_global_get_initializer", "globals", "src/fa_runtime.c (global init)", test_global_get_initializer),
    TEST_CASE("test_global_import_initializer", "globals", "src/fa_runtime.c (imported globals)", test_global_import_initializer),
    TEST_CASE("test_global_set_immutable_trap", "globals", "src/fa_ops.c (global.set)", test_global_set_immutable_trap),
    TEST_CASE("test_global_set_type_mismatch_trap", "globals", "src/fa_ops.c (global.set type check)", test_global_set_type_mismatch_trap),
    TEST_CASE("test_local_f32_default", "locals", "src/fa_runtime.c (locals init)", test_local_f32_default),
};
#undef TEST_CASE

static int test_matches_filter(const TestCase* test, const char* filter) {
    if (!test) {
        return 0;
    }
    if (!filter || filter[0] == '\0') {
        return 1;
    }
    if (strstr(test->name, filter)) {
        return 1;
    }
    if (test->area && strstr(test->area, filter)) {
        return 1;
    }
    if (test->hint && strstr(test->hint, filter)) {
        return 1;
    }
    return 0;
}

static void print_usage(const char* exe) {
    const char* name = (exe && exe[0] != '\0') ? exe : "fayasm_test_main";
    printf("Usage: %s [options] [filter]\n", name);
    printf("  --list               List tests with area and hint\n");
    printf("  --jit-prescan        Enable JIT prescan\n");
    printf("  --jit-prescan-force  Force JIT prescan at runtime\n");
    printf("  filter               Substring match on name, area, or hint\n");
}

static void list_tests(void) {
    const size_t count = sizeof(kTestCases) / sizeof(kTestCases[0]);
    for (size_t i = 0; i < count; ++i) {
        const TestCase* test = &kTestCases[i];
        printf("%-32s %-14s %s\n",
               test->name,
               test->area ? test->area : "-",
               test->hint ? test->hint : "-");
    }
}

static int run_tests(const char* filter) {
    const size_t count = sizeof(kTestCases) / sizeof(kTestCases[0]);
    size_t ran = 0;
    size_t skipped = 0;
    int failures = 0;

    for (size_t i = 0; i < count; ++i) {
        const TestCase* test = &kTestCases[i];
        if (!test_matches_filter(test, filter)) {
            skipped++;
            continue;
        }
        ran++;
        if (test->fn() != 0) {
            printf("FAIL: %s [%s] %s\n",
                   test->name,
                   test->area ? test->area : "-",
                   test->hint ? test->hint : "-");
            failures++;
        }
    }

    if (ran == 0) {
        printf("No tests matched filter: %s\n", filter ? filter : "(null)");
        return 1;
    }

    if (failures == 0) {
        printf("All tests passed. Ran %zu", ran);
    } else {
        printf("%d test(s) failed. Ran %zu", failures, ran);
    }
    if (skipped > 0) {
        printf(" (skipped %zu)", skipped);
    }
    printf(".\n");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* filter = NULL;
    int list_only = 0;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--list") == 0) {
            list_only = 1;
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(arg, "--jit-prescan") == 0) {
            test_set_env("FAYASM_JIT_PRESCAN", "1");
            continue;
        }
        if (strcmp(arg, "--jit-prescan-force") == 0) {
            test_set_env("FAYASM_JIT_PRESCAN_FORCE", "1");
            continue;
        }
        filter = arg;
    }
    if (list_only) {
        list_tests();
        return 0;
    }
    return run_tests(filter);
}
