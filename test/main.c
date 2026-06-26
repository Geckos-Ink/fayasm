#include "fa_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

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

typedef struct {
    uint8_t* opcodes;
    size_t count;
} TestJitBlob;

#define TEST_JIT_BLOB_SLOTS 16U

typedef struct {
    uint8_t* memory_blob;
    size_t memory_blob_size;
    int memory_spill_calls;
    int memory_load_calls;
    TestJitBlob jit_blobs[TEST_JIT_BLOB_SLOTS];
    int jit_spill_calls;
    int jit_load_calls;
    int trap_calls;
    uint32_t trap_target_function;
} OffloadState;

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

static void offload_state_free(OffloadState* state) {
    if (!state) {
        return;
    }
    free(state->memory_blob);
    state->memory_blob = NULL;
    state->memory_blob_size = 0;
    for (size_t i = 0; i < TEST_JIT_BLOB_SLOTS; ++i) {
        free(state->jit_blobs[i].opcodes);
        state->jit_blobs[i].opcodes = NULL;
        state->jit_blobs[i].count = 0;
    }
}

static int offload_jit_spill_hook(fa_Runtime* runtime,
                                  uint32_t function_index,
                                  const fa_JitProgram* program,
                                  size_t program_bytes,
                                  void* user_data) {
    (void)runtime;
    (void)program_bytes;
    OffloadState* state = (OffloadState*)user_data;
    if (!state || !program || !program->ops || program->count == 0) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (function_index >= TEST_JIT_BLOB_SLOTS) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    TestJitBlob* blob = &state->jit_blobs[function_index];
    uint8_t* opcodes = (uint8_t*)calloc(program->count, sizeof(uint8_t));
    if (!opcodes) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    size_t opcode_count = 0;
    if (!fa_jit_program_export_opcodes(program, opcodes, program->count, &opcode_count) ||
        opcode_count != program->count) {
        free(opcodes);
        return FA_RUNTIME_ERR_TRAP;
    }
    free(blob->opcodes);
    blob->opcodes = opcodes;
    blob->count = opcode_count;
    state->jit_spill_calls += 1;
    return FA_RUNTIME_OK;
}

static int offload_jit_load_hook(fa_Runtime* runtime,
                                 uint32_t function_index,
                                 fa_JitProgram* program_out,
                                 void* user_data) {
    (void)runtime;
    OffloadState* state = (OffloadState*)user_data;
    if (!state || !program_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (function_index >= TEST_JIT_BLOB_SLOTS) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    const TestJitBlob* blob = &state->jit_blobs[function_index];
    if (!blob->opcodes || blob->count == 0) {
        return FA_RUNTIME_ERR_STREAM;
    }
    if (!fa_jit_program_import_opcodes(blob->opcodes, blob->count, program_out)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    state->jit_load_calls += 1;
    return FA_RUNTIME_OK;
}

static int offload_memory_spill_hook(fa_Runtime* runtime,
                                     uint32_t memory_index,
                                     const fa_RuntimeMemory* memory,
                                     void* user_data) {
    (void)runtime;
    (void)memory_index;
    OffloadState* state = (OffloadState*)user_data;
    if (!state || !memory || !memory->data || memory->size_bytes == 0) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (memory->size_bytes > SIZE_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    size_t size = (size_t)memory->size_bytes;
    uint8_t* blob = (uint8_t*)realloc(state->memory_blob, size);
    if (!blob) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    memcpy(blob, memory->data, size);
    state->memory_blob = blob;
    state->memory_blob_size = size;
    state->memory_spill_calls += 1;
    return FA_RUNTIME_OK;
}

static int offload_memory_load_hook(fa_Runtime* runtime,
                                    uint32_t memory_index,
                                    fa_RuntimeMemory* memory,
                                    void* user_data) {
    (void)memory_index;
    OffloadState* state = (OffloadState*)user_data;
    if (!runtime || !state || !memory || memory->size_bytes == 0) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!state->memory_blob || state->memory_blob_size != memory->size_bytes) {
        return FA_RUNTIME_ERR_STREAM;
    }
    if (memory->size_bytes > (uint64_t)INT_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    uint8_t* data = (uint8_t*)runtime->malloc((int)memory->size_bytes);
    if (!data) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    memcpy(data, state->memory_blob, state->memory_blob_size);
    memory->data = data;
    state->memory_load_calls += 1;
    return FA_RUNTIME_OK;
}

static int offload_trap_jit_load_handler(fa_Runtime* runtime, uint32_t function_index, void* user_data) {
    OffloadState* state = (OffloadState*)user_data;
    if (!state) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    state->trap_calls += 1;
    if (function_index != state->trap_target_function) {
        return FA_RUNTIME_OK;
    }
    return fa_Runtime_jitLoadProgram(runtime, function_index);
}

/* Captures the result of round-tripping a real JIT program through the shared
   versioned spill envelope from inside a live runtime spill hook. */
typedef struct {
    int spill_calls;
    int validation_ok;
    uint8_t header_magic_ok;
    size_t blob_size;
    size_t opcode_count;
} SpillEnvelopeState;

static int spill_envelope_jit_hook(fa_Runtime* runtime,
                                   uint32_t function_index,
                                   const fa_JitProgram* program,
                                   size_t program_bytes,
                                   void* user_data) {
    (void)runtime;
    (void)function_index;
    (void)program_bytes;
    SpillEnvelopeState* st = (SpillEnvelopeState*)user_data;
    if (!st || !program || program->count == 0) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    st->spill_calls += 1;
    st->validation_ok = 0;

    size_t needed = fa_jit_program_serialized_size(program);
    if (needed == 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint8_t* blob = (uint8_t*)malloc(needed);
    if (!blob) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    size_t written = 0;
    if (!fa_jit_program_serialize(program, blob, needed, &written) || written != needed) {
        free(blob);
        return FA_RUNTIME_ERR_TRAP;
    }
    st->blob_size = written;
    st->header_magic_ok = (fa_spill_get_u32(blob) == FA_SPILL_MAGIC) ? 1u : 0u;

    fa_JitProgram restored = {0};
    fa_jit_program_init(&restored);
    int ok = 1;
    uint8_t* orig_ops = (uint8_t*)malloc(program->count);
    uint8_t* new_ops = NULL;
    if (!fa_jit_program_deserialize(blob, written, &restored) ||
        restored.count != program->count) {
        ok = 0;
    } else {
        new_ops = (uint8_t*)malloc(restored.count);
        size_t oc = 0;
        size_t nc = 0;
        if (!orig_ops || !new_ops ||
            !fa_jit_program_export_opcodes(program, orig_ops, program->count, &oc) ||
            !fa_jit_program_export_opcodes(&restored, new_ops, restored.count, &nc) ||
            oc != nc || memcmp(orig_ops, new_ops, oc) != 0) {
            ok = 0;
        }
        /* Microcode must have been rebuilt from the persisted opcode stream. */
        for (size_t i = 0; ok && i < restored.count; ++i) {
            if (restored.ops[i].step_count == 0) {
                ok = 0;
            }
        }
        st->opcode_count = restored.count;
    }
    free(orig_ops);
    free(new_ops);
    fa_jit_program_free(&restored);
    free(blob);

    st->validation_ok = ok;
    return ok ? FA_RUNTIME_OK : FA_RUNTIME_ERR_TRAP;
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

static WasmModule* load_module_from_path(const char* path, int load_exports) {
    if (!path) {
        return NULL;
    }
    WasmModule* module = wasm_module_init(path);
    if (!module) {
        return NULL;
    }
    if (wasm_load_header(module) != 0 ||
        wasm_scan_sections(module) != 0 ||
        wasm_load_types(module) != 0 ||
        wasm_load_functions(module) != 0 ||
        (load_exports && wasm_load_exports(module) != 0) ||
        wasm_load_tables(module) != 0 ||
        wasm_load_memories(module) != 0 ||
        wasm_load_globals(module) != 0 ||
        wasm_load_elements(module) != 0 ||
        wasm_load_data(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    return module;
}

static int file_exists(const char* path) {
    if (!path) {
        return 0;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int resolve_wasm_sample_path(const char* sample_name, char* out, size_t out_size) {
    if (!sample_name || !out || out_size == 0) {
        return 0;
    }
    const char* prefixes[] = {
        "wasm_samples/build",
        "../wasm_samples/build"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        int written = snprintf(out, out_size, "%s/%s", prefixes[i], sample_name);
        if (written <= 0 || (size_t)written >= out_size) {
            continue;
        }
        if (file_exists(out)) {
            return 1;
        }
    }
    return 0;
}

static int module_find_exported_function(const WasmModule* module,
                                         const char* const* candidate_names,
                                         size_t candidate_count,
                                         uint32_t* out_function_index) {
    if (!module || !candidate_names || candidate_count == 0 || !out_function_index) {
        return 0;
    }
    if (!module->exports || module->num_exports == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < module->num_exports; ++i) {
        const WasmExport* export_desc = &module->exports[i];
        if (!export_desc->name || export_desc->kind != 0) {
            continue;
        }
        for (size_t j = 0; j < candidate_count; ++j) {
            const char* candidate = candidate_names[j];
            if (!candidate) {
                continue;
            }
            if (strcmp(export_desc->name, candidate) == 0) {
                *out_function_index = export_desc->index;
                return 1;
            }
        }
    }
    return 0;
}

static int execute_expect_i32(fa_Runtime* runtime, fa_Job* job, uint32_t function_index, i32 expected) {
    if (!runtime || !job) {
        return 0;
    }
    int status = fa_Runtime_executeJob(runtime, job, function_index);
    if (status != FA_RUNTIME_OK) {
        return 0;
    }
    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != expected) {
        return 0;
    }
    return 1;
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

static int test_jit_program_opcode_roundtrip(void) {
    const uint8_t opcodes[] = { 0x6A, 0x8B, 0x1B };
    fa_JitProgram prepared = {0};
    fa_jit_program_init(&prepared);
    if (!fa_jit_prepare_program_from_opcodes(opcodes, sizeof(opcodes), &prepared)) {
        return 1;
    }

    uint8_t serialized[sizeof(opcodes)] = {0};
    size_t serialized_count = 0;
    if (!fa_jit_program_export_opcodes(&prepared, serialized, sizeof(serialized), &serialized_count) ||
        serialized_count != sizeof(opcodes) ||
        memcmp(serialized, opcodes, sizeof(opcodes)) != 0) {
        fa_jit_program_free(&prepared);
        return 1;
    }

    fa_JitProgram restored = {0};
    fa_jit_program_init(&restored);
    if (!fa_jit_program_import_opcodes(serialized, serialized_count, &restored)) {
        fa_jit_program_free(&prepared);
        return 1;
    }
    if (restored.count != prepared.count) {
        fa_jit_program_free(&restored);
        fa_jit_program_free(&prepared);
        return 1;
    }
    for (size_t i = 0; i < restored.count; ++i) {
        if (!restored.ops[i].descriptor ||
            restored.ops[i].descriptor->id != opcodes[i] ||
            restored.ops[i].step_count == 0) {
            fa_jit_program_free(&restored);
            fa_jit_program_free(&prepared);
            return 1;
        }
    }

    fa_jit_program_free(&restored);
    fa_jit_program_free(&prepared);
    return 0;
}

/* Exercises the versioned spill envelope for JIT programs: serialize -> inspect
   header -> deserialize -> opcode/microcode equivalence, plus rejection of
   corrupted magic, wrong kind, and truncated blobs. */
static int test_spill_envelope_jit_roundtrip(void) {
    const uint8_t opcodes[] = { 0x6A, 0x8B, 0x1B, 0x67 };
    fa_JitProgram prepared = {0};
    fa_jit_program_init(&prepared);
    if (!fa_jit_prepare_program_from_opcodes(opcodes, sizeof(opcodes), &prepared)) {
        return 1;
    }

    size_t needed = fa_jit_program_serialized_size(&prepared);
    if (needed != FA_SPILL_HEADER_BYTES + sizeof(opcodes)) {
        fa_jit_program_free(&prepared);
        return 1;
    }

    uint8_t blob[FA_SPILL_HEADER_BYTES + sizeof(opcodes)] = {0};
    size_t written = 0;
    /* Undersized buffer must be rejected before any bytes are written. */
    if (fa_jit_program_serialize(&prepared, blob, needed - 1, &written) || written != 0) {
        fa_jit_program_free(&prepared);
        return 1;
    }
    if (!fa_jit_program_serialize(&prepared, blob, sizeof(blob), &written) || written != needed) {
        fa_jit_program_free(&prepared);
        return 1;
    }

    /* Header fields are little-endian and self-describing. */
    if (fa_spill_get_u32(blob) != FA_SPILL_MAGIC ||
        fa_spill_get_u16(blob + 4) != (uint16_t)FA_SPILL_VERSION ||
        fa_spill_get_u16(blob + 6) != (uint16_t)FA_SPILL_KIND_JIT_OPCODES ||
        fa_spill_get_u64(blob + 8) != sizeof(opcodes)) {
        fa_jit_program_free(&prepared);
        return 1;
    }

    fa_JitProgram restored = {0};
    fa_jit_program_init(&restored);
    if (!fa_jit_program_deserialize(blob, written, &restored) ||
        restored.count != prepared.count) {
        fa_jit_program_free(&restored);
        fa_jit_program_free(&prepared);
        return 1;
    }
    for (size_t i = 0; i < restored.count; ++i) {
        if (!restored.ops[i].descriptor ||
            restored.ops[i].descriptor->id != opcodes[i] ||
            restored.ops[i].step_count == 0) {
            fa_jit_program_free(&restored);
            fa_jit_program_free(&prepared);
            return 1;
        }
    }
    fa_jit_program_free(&restored);

    /* Corrupted magic must be rejected. */
    uint8_t bad_magic[sizeof(blob)];
    memcpy(bad_magic, blob, sizeof(blob));
    bad_magic[0] ^= 0xFFu;
    fa_JitProgram reject = {0};
    fa_jit_program_init(&reject);
    if (fa_jit_program_deserialize(bad_magic, written, &reject)) {
        fa_jit_program_free(&reject);
        fa_jit_program_free(&prepared);
        return 1;
    }

    /* Wrong version must be rejected. */
    uint8_t bad_version[sizeof(blob)];
    memcpy(bad_version, blob, sizeof(blob));
    fa_spill_put_u16(bad_version + 4, (uint16_t)(FA_SPILL_VERSION + 1u));
    if (fa_jit_program_deserialize(bad_version, written, &reject)) {
        fa_jit_program_free(&reject);
        fa_jit_program_free(&prepared);
        return 1;
    }

    /* A memory blob must not deserialize as a JIT program (kind mismatch). */
    uint8_t wrong_kind[sizeof(blob)];
    memcpy(wrong_kind, blob, sizeof(blob));
    fa_spill_put_u16(wrong_kind + 6, (uint16_t)FA_SPILL_KIND_MEMORY);
    if (fa_jit_program_deserialize(wrong_kind, written, &reject)) {
        fa_jit_program_free(&reject);
        fa_jit_program_free(&prepared);
        return 1;
    }

    /* Truncated buffer (header only) must be rejected. */
    if (fa_jit_program_deserialize(blob, FA_SPILL_HEADER_BYTES, &reject)) {
        fa_jit_program_free(&reject);
        fa_jit_program_free(&prepared);
        return 1;
    }

    fa_jit_program_free(&prepared);
    return 0;
}

/* Real-WASM validation: load a compiled module, execute it so the runtime
   records + microcode-compiles the real opcode stream, then drive the live
   spill hook through the versioned JIT envelope and confirm the persisted blob
   round-trips back to an equivalent, re-microcoded program. */
static int test_spill_envelope_jit_real_wasm(void) {
    char path[PATH_MAX] = {0};
    if (!resolve_wasm_sample_path("control_flow.wasm", path, sizeof(path))) {
        printf("SKIP: test_spill_envelope_jit_real_wasm (build wasm_samples first)\n");
        return 0;
    }

    test_set_env("FAYASM_MICROCODE", "1");
    if (!fa_ops_microcode_enabled()) {
        return 1;
    }

    WasmModule* module = load_module_from_path(path, 1);
    if (!module) {
        return 1;
    }
    const char* exports[] = { "sample_loop_sum", "_sample_loop_sum" };
    uint32_t function_index = 0;
    if (!module_find_exported_function(module, exports, sizeof(exports) / sizeof(exports[0]),
                                       &function_index)) {
        wasm_module_free(module);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        return 1;
    }
    /* Lower JIT thresholds so a single execution prepares the microcode. */
    runtime->jit_context.config.min_ram_bytes = 0;
    runtime->jit_context.config.min_cpu_count = 1;
    runtime->jit_context.config.min_hot_loop_hits = 0;
    runtime->jit_context.config.min_executed_ops = 1;
    runtime->jit_context.config.min_advantage_score = 0.0f;

    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }
    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    SpillEnvelopeState state = {0};
    fa_RuntimeSpillHooks hooks = { spill_envelope_jit_hook, NULL, NULL, NULL, &state };
    fa_Runtime_setSpillHooks(runtime, &hooks);

    /* Execute the real module (also validates correct execution: loop sum). */
    if (fa_Runtime_executeJob(runtime, job, function_index) != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, NULL, NULL);
        return 1;
    }

    /* Spill the prepared program -> fires the validating envelope hook. */
    if (fa_Runtime_jitSpillProgram(runtime, function_index) != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, NULL, NULL);
        return 1;
    }

    int ok = (state.spill_calls > 0 &&
              state.validation_ok == 1 &&
              state.header_magic_ok == 1u &&
              state.opcode_count > 0 &&
              state.blob_size == FA_SPILL_HEADER_BYTES + state.opcode_count);
    cleanup_job(runtime, job, module, NULL, NULL);
    return ok ? 0 : 1;
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

static int test_imported_memory_rebind_after_attach(void) {
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

    uint8_t* memory_data_a = (uint8_t*)calloc(FA_WASM_PAGE_SIZE, 1);
    uint8_t* memory_data_b = (uint8_t*)calloc(FA_WASM_PAGE_SIZE, 1);
    if (!memory_data_a || !memory_data_b) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    i32 expected_a = 42;
    i32 expected_b = 99;
    memcpy(memory_data_a, &expected_a, sizeof(expected_a));
    memcpy(memory_data_b, &expected_b, sizeof(expected_b));

    fa_RuntimeHostMemory host_memory = {0};
    host_memory.data = memory_data_a;
    host_memory.size_bytes = FA_WASM_PAGE_SIZE;
    if (fa_Runtime_bindImportedMemory(runtime, "env", "mem0", &host_memory) != FA_RUNTIME_OK) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != expected_a) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    fa_RuntimeHostMemory invalid_memory = {0};
    invalid_memory.data = memory_data_b;
    invalid_memory.size_bytes = 0;
    if (fa_Runtime_bindImportedMemory(runtime, "env", "mem0", &invalid_memory) != FA_RUNTIME_ERR_TRAP) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (!runtime->memories || runtime->memories_count == 0 || runtime->memories[0].data != memory_data_a) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    host_memory.data = memory_data_b;
    host_memory.size_bytes = FA_WASM_PAGE_SIZE;
    if (fa_Runtime_bindImportedMemory(runtime, "env", "mem0", &host_memory) != FA_RUNTIME_OK) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (!runtime->memories || runtime->memories_count == 0 ||
        runtime->memories[0].data != memory_data_b ||
        runtime->memories[0].size_bytes != FA_WASM_PAGE_SIZE) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != expected_b) {
        free(memory_data_a);
        free(memory_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    free(memory_data_a);
    free(memory_data_b);
    bb_free(&imports);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_imported_table_rebind_after_attach(void) {
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

    const uint32_t table_size_a = 3;
    const uint32_t table_size_b = 5;
    fa_ptr* table_data_a = (fa_ptr*)calloc(table_size_a, sizeof(fa_ptr));
    fa_ptr* table_data_b = (fa_ptr*)calloc(table_size_b, sizeof(fa_ptr));
    if (!table_data_a || !table_data_b) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    fa_RuntimeHostTable host_table = {0};
    host_table.data = table_data_a;
    host_table.size = table_size_a;
    if (fa_Runtime_bindImportedTable(runtime, "env", "tbl0", &host_table) != FA_RUNTIME_OK) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, NULL, module, &module_bytes, &instructions);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != (i32)table_size_a) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    fa_RuntimeHostTable invalid_table = {0};
    invalid_table.data = table_data_b;
    invalid_table.size = 2;
    if (fa_Runtime_bindImportedTable(runtime, "env", "tbl0", &invalid_table) != FA_RUNTIME_ERR_TRAP) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (!runtime->tables || runtime->tables_count == 0 || runtime->tables[0].data != table_data_a) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    host_table.data = table_data_b;
    host_table.size = table_size_b;
    if (fa_Runtime_bindImportedTable(runtime, "env", "tbl0", &host_table) != FA_RUNTIME_OK) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (!runtime->tables || runtime->tables_count == 0 ||
        runtime->tables[0].data != table_data_b ||
        runtime->tables[0].size != table_size_b) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != (i32)table_size_b) {
        free(table_data_a);
        free(table_data_b);
        bb_free(&imports);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    free(table_data_a);
    free(table_data_b);
    bb_free(&imports);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_memory_spill_load_cycles(void) {
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

    OffloadState state = {0};
    fa_RuntimeSpillHooks hooks = {
        offload_jit_spill_hook,
        offload_jit_load_hook,
        offload_memory_spill_hook,
        offload_memory_load_hook,
        &state
    };
    fa_Runtime_setSpillHooks(runtime, &hooks);

    for (int i = 0; i < 3; ++i) {
        i32 expected = 100 + i;
        if (!runtime->memories || runtime->memories_count == 0 || !runtime->memories[0].data) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
        memcpy(runtime->memories[0].data, &expected, sizeof(expected));

        if (fa_Runtime_spillMemory(runtime, 0) != FA_RUNTIME_OK) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
        if (runtime->memories[0].data != NULL || !runtime->memories[0].is_spilled) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
        if (!execute_expect_i32(runtime, job, 0, expected)) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
        if (!runtime->memories[0].data || runtime->memories[0].is_spilled) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
    }

    if (state.memory_spill_calls < 3 || state.memory_load_calls < 3) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    offload_state_free(&state);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int test_jit_eviction_trap_reload_cycles(void) {
    test_set_env("FAYASM_MICROCODE", "1");
    if (!fa_ops_microcode_enabled()) {
        return 1;
    }

    const int warmup_pairs = 450;
    ByteBuffer func0 = {0};
    for (int i = 0; i < warmup_pairs; ++i) {
        bb_write_byte(&func0, 0x41);
        bb_write_sleb32(&func0, 0);
        bb_write_byte(&func0, 0x1A);
    }
    bb_write_byte(&func0, 0x41);
    bb_write_sleb32(&func0, 12);
    bb_write_byte(&func0, 0x0B);

    ByteBuffer func1 = {0};
    for (int i = 0; i < warmup_pairs; ++i) {
        bb_write_byte(&func1, 0x41);
        bb_write_sleb32(&func1, 0);
        bb_write_byte(&func1, 0x1A);
    }
    bb_write_byte(&func1, 0x41);
    bb_write_sleb32(&func1, 13);
    bb_write_byte(&func1, 0x0B);

    const uint8_t* bodies[] = { func0.data, func1.data };
    const size_t sizes[] = { func0.size, func1.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 2, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        bb_free(&func0);
        bb_free(&func1);
        bb_free(&module_bytes);
        return 1;
    }

    WasmModule* module = load_module_from_bytes(module_bytes.data, module_bytes.size);
    if (!module) {
        bb_free(&func0);
        bb_free(&func1);
        bb_free(&module_bytes);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        bb_free(&func0);
        bb_free(&func1);
        bb_free(&module_bytes);
        return 1;
    }
    runtime->jit_context.config.min_ram_bytes = 0;
    runtime->jit_context.config.min_cpu_count = 1;
    runtime->jit_context.config.min_hot_loop_hits = 0;
    runtime->jit_context.config.min_executed_ops = 1;
    runtime->jit_context.config.min_advantage_score = 0.0f;
    runtime->jit_context.config.max_cache_percent = 0;
    runtime->jit_context.config.max_ops_per_chunk = 0;
    runtime->jit_context.config.max_chunks = 0;

    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        bb_free(&func0);
        bb_free(&func1);
        bb_free(&module_bytes);
        return 1;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        bb_free(&func0);
        bb_free(&func1);
        bb_free(&module_bytes);
        return 1;
    }

    OffloadState state = {0};
    state.trap_target_function = 0;
    fa_RuntimeSpillHooks spill_hooks = {
        offload_jit_spill_hook,
        offload_jit_load_hook,
        offload_memory_spill_hook,
        offload_memory_load_hook,
        &state
    };
    fa_Runtime_setSpillHooks(runtime, &spill_hooks);

    if (!execute_expect_i32(runtime, job, 0, 12)) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }
    if (runtime->jit_cache_bytes == 0) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }

    if (!execute_expect_i32(runtime, job, 1, 13)) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }
    if (state.jit_spill_calls == 0 || !state.jit_blobs[0].opcodes || state.jit_blobs[0].count == 0) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }

    fa_RuntimeTrapHooks trap_hooks = { offload_trap_jit_load_handler, &state };
    fa_Runtime_setTrapHooks(runtime, &trap_hooks);
    if (fa_Runtime_setFunctionTrap(runtime, 0, true) != FA_RUNTIME_OK) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }

    if (!execute_expect_i32(runtime, job, 0, 12)) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }
    const int baseline_spills = state.jit_spill_calls;
    const int baseline_loads = state.jit_load_calls;

    for (int i = 0; i < 3; ++i) {
        if (fa_Runtime_jitSpillProgram(runtime, 0) != FA_RUNTIME_OK) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &func0);
            bb_free(&func1);
            return 1;
        }
        if (!execute_expect_i32(runtime, job, 0, 12)) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &func0);
            bb_free(&func1);
            return 1;
        }
    }

    if (state.trap_calls < 4 ||
        state.jit_spill_calls < baseline_spills + 3 ||
        state.jit_load_calls < baseline_loads + 3) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &func0);
        bb_free(&func1);
        return 1;
    }

    offload_state_free(&state);
    cleanup_job(runtime, job, module, &module_bytes, &func0);
    bb_free(&func1);
    return 0;
}

/* Validates offload of grown memory: memory.grow extends the page set, then a
 * repeated spill/load cycle (driven through the explicit fa_Runtime_loadMemory
 * API) must preserve the full grown region byte-for-byte across iterations.
 * Guards against spill blobs capturing a stale (pre-grow) size on low-RAM
 * targets. */
static int test_memory_grow_spill_load_roundtrip(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);          /* i32.const 1 (grow delta) */
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x40);          /* memory.grow */
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x1A);          /* drop old size */
    bb_write_byte(&instructions, 0x3F);          /* memory.size -> pages */
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 1, 4, kResultI32, 1, NULL, 0)) {
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

    /* Grow from 1 to 2 pages and confirm the runtime tracked the new size. */
    if (!execute_expect_i32(runtime, job, 0, 2)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const uint64_t grown_bytes = 2ULL * FA_WASM_PAGE_SIZE;
    if (!runtime->memories || runtime->memories_count == 0 ||
        !runtime->memories[0].data ||
        runtime->memories[0].size_bytes != grown_bytes) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    /* Seed the whole grown region (including the second page) with a pattern. */
    for (uint64_t i = 0; i < grown_bytes; ++i) {
        runtime->memories[0].data[i] = (uint8_t)((i * 31u + 7u) & 0xFFu);
    }

    OffloadState state = {0};
    fa_RuntimeSpillHooks hooks = {
        offload_jit_spill_hook,
        offload_jit_load_hook,
        offload_memory_spill_hook,
        offload_memory_load_hook,
        &state
    };
    fa_Runtime_setSpillHooks(runtime, &hooks);

    const int cycles = 5;
    for (int c = 0; c < cycles; ++c) {
        if (fa_Runtime_spillMemory(runtime, 0) != FA_RUNTIME_OK ||
            runtime->memories[0].data != NULL ||
            !runtime->memories[0].is_spilled) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
        if (fa_Runtime_loadMemory(runtime, 0) != FA_RUNTIME_OK ||
            !runtime->memories[0].data ||
            runtime->memories[0].is_spilled ||
            runtime->memories[0].size_bytes != grown_bytes) {
            offload_state_free(&state);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
        for (uint64_t i = 0; i < grown_bytes; ++i) {
            if (runtime->memories[0].data[i] != (uint8_t)((i * 31u + 7u) & 0xFFu)) {
                offload_state_free(&state);
                cleanup_job(runtime, job, module, &module_bytes, &instructions);
                return 1;
            }
        }
    }

    if (state.memory_spill_calls < cycles || state.memory_load_calls < cycles) {
        offload_state_free(&state);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    offload_state_free(&state);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

/* Exercises the versioned linear-memory spill envelope: grow real memory, seed
   it, serialize, validate the self-describing header, scribble over the live
   bytes, restore from the blob, and confirm the post-grow size + every byte
   come back. Also restores the blob into a separate fresh instance (proving the
   blob carries its own size) and rejects corrupted/mismatched blobs. */
static int test_spill_envelope_memory_roundtrip(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);          /* i32.const 1 (grow delta) */
    bb_write_sleb32(&instructions, 1);
    bb_write_byte(&instructions, 0x40);          /* memory.grow */
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x1A);          /* drop old size */
    bb_write_byte(&instructions, 0x3F);          /* memory.size -> pages */
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 1, 1, 1, 4, kResultI32, 1, NULL, 0)) {
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

    if (!execute_expect_i32(runtime, job, 0, 2)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const uint64_t grown_bytes = 2ULL * FA_WASM_PAGE_SIZE;
    if (!runtime->memories || runtime->memories[0].size_bytes != grown_bytes ||
        !runtime->memories[0].data) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    for (uint64_t i = 0; i < grown_bytes; ++i) {
        runtime->memories[0].data[i] = (uint8_t)((i * 131u + 17u) & 0xFFu);
    }

    /* Serialize into a versioned blob. */
    size_t needed = fa_Runtime_serializedMemorySize(runtime, 0);
    if (needed != FA_SPILL_HEADER_BYTES + FA_SPILL_MEMORY_BODY_HEADER_BYTES + grown_bytes) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    uint8_t* blob = (uint8_t*)malloc(needed);
    if (!blob) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    size_t written = 0;
    /* Undersized buffer must be rejected. */
    if (fa_Runtime_serializeMemory(runtime, 0, blob, needed - 1, &written) || written != 0) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    if (!fa_Runtime_serializeMemory(runtime, 0, blob, needed, &written) || written != needed) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    /* Self-describing header: envelope + memory sub-header (flags + size). */
    if (fa_spill_get_u32(blob) != FA_SPILL_MAGIC ||
        fa_spill_get_u16(blob + 6) != (uint16_t)FA_SPILL_KIND_MEMORY ||
        fa_spill_get_u64(blob + 8) != FA_SPILL_MEMORY_BODY_HEADER_BYTES + grown_bytes ||
        blob[FA_SPILL_HEADER_BYTES] != 0x00u /* not memory64 */ ||
        fa_spill_get_u64(blob + FA_SPILL_HEADER_BYTES + 8) != grown_bytes) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    /* Corrupt the live memory, then restore it from the blob. */
    memset(runtime->memories[0].data, 0xAB, (size_t)grown_bytes);
    if (!fa_Runtime_deserializeMemory(runtime, 0, blob, written) ||
        runtime->memories[0].size_bytes != grown_bytes ||
        runtime->memories[0].is_spilled ||
        !runtime->memories[0].data) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    for (uint64_t i = 0; i < grown_bytes; ++i) {
        if (runtime->memories[0].data[i] != (uint8_t)((i * 131u + 17u) & 0xFFu)) {
            free(blob);
            cleanup_job(runtime, job, module, &module_bytes, &instructions);
            return 1;
        }
    }

    /* Restore the blob into a separate fresh instance (1-page module): the blob
       carries its own size, so the target ends up with the full grown region. */
    ByteBuffer module_bytes2 = {0};
    if (!build_module(&module_bytes2, bodies, sizes, 1, 1, 1, 1, 4, kResultI32, 1, NULL, 0)) {
        free(blob);
        bb_free(&module_bytes2);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    fa_Runtime* runtime2 = NULL;
    fa_Job* job2 = NULL;
    WasmModule* module2 = NULL;
    if (!run_job(&module_bytes2, &runtime2, &job2, &module2)) {
        free(blob);
        cleanup_job(runtime2, job2, module2, &module_bytes2, NULL);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    int cross_ok = (fa_Runtime_deserializeMemory(runtime2, 0, blob, written) &&
                    runtime2->memories[0].size_bytes == grown_bytes &&
                    runtime2->memories[0].data != NULL);
    for (uint64_t i = 0; cross_ok && i < grown_bytes; ++i) {
        if (runtime2->memories[0].data[i] != (uint8_t)((i * 131u + 17u) & 0xFFu)) {
            cross_ok = 0;
        }
    }
    cleanup_job(runtime2, job2, module2, &module_bytes2, NULL);
    if (!cross_ok) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    /* Corruption / mismatch rejection. */
    uint8_t saved_magic = blob[0];
    blob[0] ^= 0xFFu;
    int rejected = !fa_Runtime_deserializeMemory(runtime, 0, blob, written);
    blob[0] = saved_magic;
    if (!rejected) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    /* Wrong kind (claim JIT opcodes) must be rejected. */
    fa_spill_put_u16(blob + 6, (uint16_t)FA_SPILL_KIND_JIT_OPCODES);
    rejected = !fa_Runtime_deserializeMemory(runtime, 0, blob, written);
    fa_spill_put_u16(blob + 6, (uint16_t)FA_SPILL_KIND_MEMORY);
    if (!rejected) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    /* Truncated buffer (drops trailing payload) must be rejected. */
    if (fa_Runtime_deserializeMemory(runtime, 0, blob, written - 1)) {
        free(blob);
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    free(blob);
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return 0;
}

static int run_wasm_sample_export_i32(const char* sample_name,
                                      const char* test_name,
                                      const char* const* exports,
                                      size_t export_count,
                                      i32 expected);

static int test_wasm_sample_arithmetic(void) {
    const char* exports[] = { "sample_const42", "_sample_const42" };
    return run_wasm_sample_export_i32("arithmetic.wasm",
                                      "test_wasm_sample_arithmetic",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      42);
}

static int test_wasm_sample_arithmetic_mul_add(void) {
    const char* exports[] = { "sample_mul_add_const", "_sample_mul_add_const" };
    return run_wasm_sample_export_i32("arithmetic.wasm",
                                      "test_wasm_sample_arithmetic_mul_add",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      42);
}

static int test_wasm_sample_control_flow(void) {
    const char* exports[] = { "sample_loop_sum", "_sample_loop_sum" };
    return run_wasm_sample_export_i32("control_flow.wasm",
                                      "test_wasm_sample_control_flow",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      55);
}

static int test_wasm_sample_control_flow_factorial(void) {
    const char* exports[] = { "sample_factorial_6", "_sample_factorial_6" };
    return run_wasm_sample_export_i32("control_flow.wasm",
                                      "test_wasm_sample_control_flow_factorial",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      720);
}

static int test_wasm_sample_advanced_memory_mix(void) {
    const char* exports[] = { "sample_memory_mix", "_sample_memory_mix" };
    return run_wasm_sample_export_i32("advanced_runtime.wasm",
                                      "test_wasm_sample_advanced_memory_mix",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      3413);
}

static int test_wasm_sample_advanced_call_chain(void) {
    const char* exports[] = { "sample_call_chain", "_sample_call_chain" };
    return run_wasm_sample_export_i32("advanced_runtime.wasm",
                                      "test_wasm_sample_advanced_call_chain",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      150);
}

static int run_wasm_sample_export_i32(const char* sample_name,
                                      const char* test_name,
                                      const char* const* exports,
                                      size_t export_count,
                                      i32 expected) {
    char path[PATH_MAX] = {0};
    if (!resolve_wasm_sample_path(sample_name, path, sizeof(path))) {
        printf("SKIP: %s (build wasm_samples first)\n", test_name);
        return 0;
    }

    WasmModule* module = load_module_from_path(path, 1);
    if (!module) {
        return 1;
    }
    uint32_t function_index = 0;
    if (!module_find_exported_function(module, exports, export_count, &function_index)) {
        wasm_module_free(module);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }
    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    int ok = execute_expect_i32(runtime, job, function_index, expected);
    cleanup_job(runtime, job, module, NULL, NULL);
    return ok ? 0 : 1;
}

/* Builds an i32 argument value for parameterized smoke exports. */
static fa_JobValue sample_arg_i32(i32 v) {
    fa_JobValue value = {0};
    value.kind = fa_job_value_i32;
    value.is_signed = true;
    value.bit_width = 32;
    value.payload.i32_value = v;
    return value;
}

/* Builds an i64 argument value for parameterized smoke exports. */
static fa_JobValue sample_arg_i64(i64 v) {
    fa_JobValue value = {0};
    value.kind = fa_job_value_i64;
    value.is_signed = true;
    value.bit_width = 64;
    value.payload.i64_value = v;
    return value;
}

/* Builds an f32 argument value for parameterized smoke exports. */
static fa_JobValue sample_arg_f32(f32 v) {
    fa_JobValue value = {0};
    value.kind = fa_job_value_f32;
    value.bit_width = 32;
    value.payload.f32_value = v;
    return value;
}

/* Builds an f64 argument value for parameterized smoke exports. */
static fa_JobValue sample_arg_f64(f64 v) {
    fa_JobValue value = {0};
    value.kind = fa_job_value_f64;
    value.bit_width = 64;
    value.payload.f64_value = v;
    return value;
}

/* Loads a sample module and runs an export with typed args, comparing the
 * top-of-stack result against an expected i32/i64 value. Exercises
 * fa_Runtime_executeJobWithArgs and argument transfer with real toolchain
 * output. */
static int run_wasm_sample_export_checked(const char* sample_name,
                                          const char* test_name,
                                          const char* const* exports,
                                          size_t export_count,
                                          const fa_JobValue* args,
                                          uint32_t arg_count,
                                          fa_JobValue expected) {
    char path[PATH_MAX] = {0};
    if (!resolve_wasm_sample_path(sample_name, path, sizeof(path))) {
        printf("SKIP: %s (build wasm_samples first)\n", test_name);
        return 0;
    }

    WasmModule* module = load_module_from_path(path, 1);
    if (!module) {
        return 1;
    }
    uint32_t function_index = 0;
    if (!module_find_exported_function(module, exports, export_count, &function_index)) {
        wasm_module_free(module);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        return 1;
    }
    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }
    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    int status = fa_Runtime_executeJobWithArgs(runtime, job, function_index, args, arg_count);
    int ok = 0;
    if (status == FA_RUNTIME_OK) {
        const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
        if (value && value->kind == expected.kind) {
            if (expected.kind == fa_job_value_i32) {
                ok = (value->payload.i32_value == expected.payload.i32_value);
            } else if (expected.kind == fa_job_value_i64) {
                ok = (value->payload.i64_value == expected.payload.i64_value);
            } else if (expected.kind == fa_job_value_f32) {
                /* Tolerant compare: smoke fixtures pick exactly-representable
                 * results, but allow a small epsilon for iterative kernels. */
                ok = (fabsf(value->payload.f32_value - expected.payload.f32_value) <= 1e-4f);
            } else if (expected.kind == fa_job_value_f64) {
                ok = (fabs(value->payload.f64_value - expected.payload.f64_value) <= 1e-9);
            }
        }
    }
    cleanup_job(runtime, job, module, NULL, NULL);
    return ok ? 0 : 1;
}

/* Parameterized i32 export: sample_add_i32(40, 2) == 42 (arg transfer). */
static int test_wasm_sample_typed_add_i32(void) {
    const char* exports[] = { "sample_add_i32", "_sample_add_i32" };
    fa_JobValue args[] = { sample_arg_i32(40), sample_arg_i32(2) };
    return run_wasm_sample_export_checked("typed_values.wasm",
                                          "test_wasm_sample_typed_add_i32",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(42));
}

/* Arg-driven control flow: sample_sum_to_n(100) == 5050. */
static int test_wasm_sample_typed_sum_to_n(void) {
    const char* exports[] = { "sample_sum_to_n", "_sample_sum_to_n" };
    fa_JobValue args[] = { sample_arg_i32(100) };
    return run_wasm_sample_export_checked("typed_values.wasm",
                                          "test_wasm_sample_typed_sum_to_n",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(5050));
}

/* i64 params + i64 return: sample_scale_i64(100000, 100000) == 10000000001. */
static int test_wasm_sample_typed_scale_i64(void) {
    const char* exports[] = { "sample_scale_i64", "_sample_scale_i64" };
    fa_JobValue args[] = { sample_arg_i64(100000), sample_arg_i64(100000) };
    return run_wasm_sample_export_checked("typed_values.wasm",
                                          "test_wasm_sample_typed_scale_i64",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i64(10000000001LL));
}

/* Runs `f64.const value; (0xFC sub); end` as a one-shot i32-returning function
 * and checks the i32 payload. Exercises the scalar saturating truncation
 * conversions directly with hand-built bytecode. Returns 1 on match. */
static int run_trunc_sat_f64_i32_case(double value, uint8_t sub, i32 expected) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x44);          /* f64.const */
    bb_write_f64(&instructions, value);
    bb_write_byte(&instructions, 0xFC);
    bb_write_uleb(&instructions, sub);
    bb_write_byte(&instructions, 0x0B);          /* end */

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes, bodies, sizes, 1, 0, 0, 0, 0, kResultI32, 1, NULL, 0)) {
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 0;
    }
    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 0;
    }
    int status = fa_Runtime_executeJob(runtime, job, 0);
    int ok = 0;
    if (status == FA_RUNTIME_OK) {
        const fa_JobValue* top = fa_JobStack_peek(&job->stack, 0);
        ok = (top && top->kind == fa_job_value_i32 && top->payload.i32_value == expected);
    }
    cleanup_job(runtime, job, module, &module_bytes, &instructions);
    return ok;
}

/* Saturation edge cases for the i32 trunc_sat conversions (0xFC 0x02 signed,
 * 0xFC 0x03 unsigned): NaN -> 0, +/-inf and overflow clamp to the type bounds,
 * negatives clamp to 0 for the unsigned form. Locks in the runtime handlers
 * that the floating_point smoke fixture only touches on the in-range path. */
static int test_trunc_sat_f64_i32_saturation(void) {
    const double inf = (double)INFINITY;
    const double nan_value = (double)NAN;
    /* i32.trunc_sat_f64_s (0x02). */
    if (!run_trunc_sat_f64_i32_case(3.9, 0x02, 3)) return 1;
    if (!run_trunc_sat_f64_i32_case(-3.9, 0x02, -3)) return 1;
    if (!run_trunc_sat_f64_i32_case(nan_value, 0x02, 0)) return 1;
    if (!run_trunc_sat_f64_i32_case(inf, 0x02, INT32_MAX)) return 1;
    if (!run_trunc_sat_f64_i32_case(-inf, 0x02, INT32_MIN)) return 1;
    if (!run_trunc_sat_f64_i32_case(1e30, 0x02, INT32_MAX)) return 1;
    if (!run_trunc_sat_f64_i32_case(-1e30, 0x02, INT32_MIN)) return 1;
    /* i32.trunc_sat_f64_u (0x03): UINT32_MAX reads back as -1 in i32 payload. */
    if (!run_trunc_sat_f64_i32_case(-1.0, 0x03, 0)) return 1;
    if (!run_trunc_sat_f64_i32_case(nan_value, 0x03, 0)) return 1;
    if (!run_trunc_sat_f64_i32_case(1e30, 0x03, (i32)UINT32_MAX)) return 1;
    if (!run_trunc_sat_f64_i32_case(4294967295.0, 0x03, (i32)UINT32_MAX)) return 1;
    return 0;
}

/* f64 args + f64 return: Horner polynomial 3x^3-2x^2+x-5 at x=2 == 13.0. */
static int test_wasm_sample_float_poly(void) {
    const char* exports[] = { "sample_f64_poly", "_sample_f64_poly" };
    fa_JobValue args[] = { sample_arg_f64(2.0) };
    return run_wasm_sample_export_checked("floating_point.wasm",
                                          "test_wasm_sample_float_poly",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_f64(13.0));
}

/* f64 mul/add + open-coded sqrt loop: hypot(3,4) == 5.0. */
static int test_wasm_sample_float_hypot(void) {
    const char* exports[] = { "sample_f64_hypot", "_sample_f64_hypot" };
    fa_JobValue args[] = { sample_arg_f64(3.0), sample_arg_f64(4.0) };
    return run_wasm_sample_export_checked("floating_point.wasm",
                                          "test_wasm_sample_float_hypot",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_f64(5.0));
}

/* f32 args + f32 return: lerp(0, 10, 0.5) == 5.0f. */
static int test_wasm_sample_float_lerp(void) {
    const char* exports[] = { "sample_f32_lerp", "_sample_f32_lerp" };
    fa_JobValue args[] = { sample_arg_f32(0.0f), sample_arg_f32(10.0f), sample_arg_f32(0.5f) };
    return run_wasm_sample_export_checked("floating_point.wasm",
                                          "test_wasm_sample_float_lerp",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_f32(5.0f));
}

/* f64->i32 saturating conversion (trunc_sat): round(3.7) == 4. Drives the
 * 0xFC 0x02 path that emcc emits for a plain (int) cast of a double. */
static int test_wasm_sample_float_round(void) {
    const char* exports[] = { "sample_f64_round", "_sample_f64_round" };
    fa_JobValue args[] = { sample_arg_f64(3.7) };
    return run_wasm_sample_export_checked("floating_point.wasm",
                                          "test_wasm_sample_float_round",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(4));
}

/* i32->f64 conversion inside a loop: sum(i*1.5, i=1..10) == 82.5. */
static int test_wasm_sample_float_series(void) {
    const char* exports[] = { "sample_f64_series", "_sample_f64_series" };
    fa_JobValue args[] = { sample_arg_i32(10) };
    return run_wasm_sample_export_checked("floating_point.wasm",
                                          "test_wasm_sample_float_series",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_f64(82.5));
}

/* call_indirect through a real funcref table: dispatch(2,6,7) selects mul. */
static int test_wasm_sample_dispatch_mul(void) {
    const char* exports[] = { "sample_dispatch", "_sample_dispatch" };
    fa_JobValue args[] = { sample_arg_i32(2), sample_arg_i32(6), sample_arg_i32(7) };
    return run_wasm_sample_export_checked("indirect_dispatch.wasm",
                                          "test_wasm_sample_dispatch_mul",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(42));
}

/* Repeated call_indirect in a loop: fold every op over (6,7) == 61. */
static int test_wasm_sample_dispatch_fold(void) {
    const char* exports[] = { "sample_dispatch_fold", "_sample_dispatch_fold" };
    fa_JobValue args[] = { sample_arg_i32(6), sample_arg_i32(7) };
    return run_wasm_sample_export_checked("indirect_dispatch.wasm",
                                          "test_wasm_sample_dispatch_fold",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(61));
}

/* Dense switch -> br_table: classify(5) == 320 + 50 + 20 == 390. */
static int test_wasm_sample_classify_brtable(void) {
    const char* exports[] = { "sample_classify", "_sample_classify" };
    fa_JobValue args[] = { sample_arg_i32(5) };
    return run_wasm_sample_export_checked("indirect_dispatch.wasm",
                                          "test_wasm_sample_classify_brtable",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(390));
}

/* br_table default arm: classify(9) falls through to -1. */
static int test_wasm_sample_classify_default(void) {
    const char* exports[] = { "sample_classify", "_sample_classify" };
    fa_JobValue args[] = { sample_arg_i32(9) };
    return run_wasm_sample_export_checked("indirect_dispatch.wasm",
                                          "test_wasm_sample_classify_default",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(-1));
}

/* Zero-arg memory-heavy export: bubble sort + weighted checksum == 204. */
static int test_wasm_sample_sort_checksum(void) {
    const char* exports[] = { "sample_sort_checksum", "_sample_sort_checksum" };
    return run_wasm_sample_export_i32("memory_ops.wasm",
                                      "test_wasm_sample_sort_checksum",
                                      exports,
                                      sizeof(exports) / sizeof(exports[0]),
                                      204);
}

/* memcpy/memset over a runtime length -> memory.copy / memory.fill, then a
 * stable unsigned checksum. pipeline(7, 64) == 1380176480. */
static int test_wasm_sample_buffer_pipeline(void) {
    const char* exports[] = { "sample_buffer_pipeline", "_sample_buffer_pipeline" };
    fa_JobValue args[] = { sample_arg_i32(7), sample_arg_i32(64) };
    return run_wasm_sample_export_checked("memory_ops.wasm",
                                          "test_wasm_sample_buffer_pipeline",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(1380176480));
}

/* Data-segment lookup table read by runtime index: squares[5] == 25. */
static int test_wasm_sample_table_lookup(void) {
    const char* exports[] = { "sample_table_lookup", "_sample_table_lookup" };
    fa_JobValue args[] = { sample_arg_i32(5) };
    return run_wasm_sample_export_checked("memory_ops.wasm",
                                          "test_wasm_sample_table_lookup",
                                          exports,
                                          sizeof(exports) / sizeof(exports[0]),
                                          args,
                                          (uint32_t)(sizeof(args) / sizeof(args[0])),
                                          sample_arg_i32(25));
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
    bb_write_uleb(&elem_payload, 5);
    bb_write_byte(&elem_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&elem_payload, 1);
    bb_write_byte(&elem_payload, 0xD2);
    bb_write_uleb(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x0B);

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
    bb_write_uleb(&elem_payload, 0);

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

static int test_externref_table_active_null_elem_expr(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_EXTERNREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 1);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 4);
    bb_write_byte(&elem_payload, 0x41);
    bb_write_sleb32(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x0B);
    bb_write_byte(&elem_payload, VALTYPE_EXTERNREF);
    bb_write_uleb(&elem_payload, 1);
    bb_write_byte(&elem_payload, 0xD0);
    bb_write_byte(&elem_payload, VALTYPE_EXTERNREF);
    bb_write_byte(&elem_payload, 0x0B);

    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0x41);
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t result_types[] = { VALTYPE_EXTERNREF };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    1,
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
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&elem_payload);

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
    if (!value || value->kind != fa_job_value_ref || value->payload.ref_value != 0) {
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

static int test_table_init_global_get_element_expr(void) {
    ByteBuffer imports = {0};
    if (!bb_write_uleb(&imports, 1) ||
        !bb_write_string(&imports, "env") ||
        !bb_write_string(&imports, "g_ref") ||
        !bb_write_byte(&imports, 0x03) ||
        !bb_write_byte(&imports, VALTYPE_EXTERNREF) ||
        !bb_write_byte(&imports, 0x00)) {
        bb_free(&imports);
        return 1;
    }

    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_EXTERNREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 1);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 5);
    bb_write_byte(&elem_payload, VALTYPE_EXTERNREF);
    bb_write_uleb(&elem_payload, 1);
    bb_write_byte(&elem_payload, 0x23);
    bb_write_uleb(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x0B);

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
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x25);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0x0B);

    const uint8_t* bodies[] = { instructions.data };
    const size_t sizes[] = { instructions.size };
    const uint8_t result_types[] = { VALTYPE_EXTERNREF };
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
                                  NULL,
                                  0) ||
        !append_section(&module_bytes, SECTION_TABLE, &table_payload) ||
        !append_section(&module_bytes, SECTION_ELEMENT, &elem_payload)) {
        bb_free(&imports);
        bb_free(&table_payload);
        bb_free(&elem_payload);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
    bb_free(&imports);
    bb_free(&table_payload);
    bb_free(&elem_payload);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }

    fa_JobValue import_value = {0};
    import_value.kind = fa_job_value_ref;
    import_value.bit_width = (uint8_t)(sizeof(fa_ptr) * 8U);
    import_value.is_signed = false;
    import_value.payload.ref_value = (fa_ptr)0x1234U;
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
    if (!value || value->kind != fa_job_value_ref || value->payload.ref_value != (fa_ptr)0x1234U) {
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

static int test_call_indirect_basic(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 1);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x41);
    bb_write_sleb32(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x0B);
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 1);

    ByteBuffer caller = {0};
    bb_write_byte(&caller, 0x41);
    bb_write_sleb32(&caller, 0);
    bb_write_byte(&caller, 0x11);
    bb_write_uleb(&caller, 0);
    bb_write_uleb(&caller, 0);
    bb_write_byte(&caller, 0x0B);

    ByteBuffer callee = {0};
    bb_write_byte(&callee, 0x41);
    bb_write_sleb32(&callee, 42);
    bb_write_byte(&callee, 0x0B);

    const uint8_t* bodies[] = { caller.data, callee.data };
    const size_t sizes[] = { caller.size, callee.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    2,
                                    &table_payload,
                                    NULL,
                                    &elem_payload,
                                    NULL,
                                    kResultI32,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        bb_free(&elem_payload);
        bb_free(&caller);
        bb_free(&callee);
        cleanup_job(NULL, NULL, NULL, &module_bytes, NULL);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&elem_payload);
    bb_free(&caller);
    bb_free(&callee);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 42) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, NULL);
    return 0;
}

static int test_call_indirect_function_zero(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 1);

    ByteBuffer elem_payload = {0};
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x41);
    bb_write_sleb32(&elem_payload, 0);
    bb_write_byte(&elem_payload, 0x0B);
    bb_write_uleb(&elem_payload, 1);
    bb_write_uleb(&elem_payload, 0);

    ByteBuffer callee = {0};
    bb_write_byte(&callee, 0x41);
    bb_write_sleb32(&callee, 77);
    bb_write_byte(&callee, 0x0B);

    ByteBuffer caller = {0};
    bb_write_byte(&caller, 0x41);
    bb_write_sleb32(&caller, 0);
    bb_write_byte(&caller, 0x11);
    bb_write_uleb(&caller, 0);
    bb_write_uleb(&caller, 0);
    bb_write_byte(&caller, 0x0B);

    const uint8_t* bodies[] = { callee.data, caller.data };
    const size_t sizes[] = { callee.size, caller.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    2,
                                    &table_payload,
                                    NULL,
                                    &elem_payload,
                                    NULL,
                                    kResultI32,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        bb_free(&elem_payload);
        bb_free(&callee);
        bb_free(&caller);
        cleanup_job(NULL, NULL, NULL, &module_bytes, NULL);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&elem_payload);
    bb_free(&callee);
    bb_free(&caller);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 1);
    if (status != FA_RUNTIME_OK) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 77) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }
    if (fa_JobStack_peek(&job->stack, 1) != NULL) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    cleanup_job(runtime, job, module, &module_bytes, NULL);
    return 0;
}

static int test_call_indirect_null_trap(void) {
    ByteBuffer table_payload = {0};
    bb_write_uleb(&table_payload, 1);
    bb_write_byte(&table_payload, VALTYPE_FUNCREF);
    bb_write_uleb(&table_payload, 0);
    bb_write_uleb(&table_payload, 1);

    ByteBuffer caller = {0};
    bb_write_byte(&caller, 0x41);
    bb_write_sleb32(&caller, 0);
    bb_write_byte(&caller, 0x11);
    bb_write_uleb(&caller, 0);
    bb_write_uleb(&caller, 0);
    bb_write_byte(&caller, 0x0B);

    ByteBuffer callee = {0};
    bb_write_byte(&callee, 0x41);
    bb_write_sleb32(&callee, 7);
    bb_write_byte(&callee, 0x0B);

    const uint8_t* bodies[] = { caller.data, callee.data };
    const size_t sizes[] = { caller.size, callee.size };
    ByteBuffer module_bytes = {0};
    if (!build_module_with_sections(&module_bytes,
                                    bodies,
                                    sizes,
                                    2,
                                    &table_payload,
                                    NULL,
                                    NULL,
                                    NULL,
                                    kResultI32,
                                    1,
                                    NULL,
                                    0)) {
        bb_free(&table_payload);
        bb_free(&caller);
        bb_free(&callee);
        cleanup_job(NULL, NULL, NULL, &module_bytes, NULL);
        return 1;
    }
    bb_free(&table_payload);
    bb_free(&caller);
    bb_free(&callee);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, NULL);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
}

static int test_call_indirect_type_mismatch_trap(void) {
    ByteBuffer module_bytes = {0};
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (!bb_write_bytes(&module_bytes, header, sizeof(header))) {
        bb_free(&module_bytes);
        return 1;
    }

    ByteBuffer payload = {0};

    // Type section: type0 () -> i32, type1 () -> i64.
    bb_write_uleb(&payload, 2);
    bb_write_byte(&payload, 0x60);
    bb_write_uleb(&payload, 0);
    bb_write_uleb(&payload, 1);
    bb_write_byte(&payload, VALTYPE_I32);
    bb_write_byte(&payload, 0x60);
    bb_write_uleb(&payload, 0);
    bb_write_uleb(&payload, 1);
    bb_write_byte(&payload, VALTYPE_I64);
    if (!append_section(&module_bytes, SECTION_TYPE, &payload)) {
        bb_free(&payload);
        bb_free(&module_bytes);
        return 1;
    }

    // Function section: caller uses type0, callee uses type1.
    bb_reset(&payload);
    bb_write_uleb(&payload, 2);
    bb_write_uleb(&payload, 0);
    bb_write_uleb(&payload, 1);
    if (!append_section(&module_bytes, SECTION_FUNCTION, &payload)) {
        bb_free(&payload);
        bb_free(&module_bytes);
        return 1;
    }

    // Table section.
    bb_reset(&payload);
    bb_write_uleb(&payload, 1);
    bb_write_byte(&payload, VALTYPE_FUNCREF);
    bb_write_uleb(&payload, 0);
    bb_write_uleb(&payload, 1);
    if (!append_section(&module_bytes, SECTION_TABLE, &payload)) {
        bb_free(&payload);
        bb_free(&module_bytes);
        return 1;
    }

    // Element section: table[0] = function 1 (type1).
    bb_reset(&payload);
    bb_write_uleb(&payload, 1);
    bb_write_uleb(&payload, 0);
    bb_write_byte(&payload, 0x41);
    bb_write_sleb32(&payload, 0);
    bb_write_byte(&payload, 0x0B);
    bb_write_uleb(&payload, 1);
    bb_write_uleb(&payload, 1);
    if (!append_section(&module_bytes, SECTION_ELEMENT, &payload)) {
        bb_free(&payload);
        bb_free(&module_bytes);
        return 1;
    }

    // Code section.
    ByteBuffer caller = {0};
    bb_write_byte(&caller, 0x41);
    bb_write_sleb32(&caller, 0);
    bb_write_byte(&caller, 0x11);
    bb_write_uleb(&caller, 0);
    bb_write_uleb(&caller, 0);
    bb_write_byte(&caller, 0x0B);

    ByteBuffer callee = {0};
    bb_write_byte(&callee, 0x42);
    bb_write_sleb32(&callee, 9);
    bb_write_byte(&callee, 0x0B);

    bb_reset(&payload);
    bb_write_uleb(&payload, 2);
    bb_write_uleb(&payload, (uint32_t)(caller.size + 1));
    bb_write_uleb(&payload, 0);
    bb_write_bytes(&payload, caller.data, caller.size);
    bb_write_uleb(&payload, (uint32_t)(callee.size + 1));
    bb_write_uleb(&payload, 0);
    bb_write_bytes(&payload, callee.data, callee.size);
    if (!append_section(&module_bytes, SECTION_CODE, &payload)) {
        bb_free(&payload);
        bb_free(&caller);
        bb_free(&callee);
        bb_free(&module_bytes);
        return 1;
    }

    bb_free(&payload);
    bb_free(&caller);
    bb_free(&callee);

    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    WasmModule* module = NULL;
    if (!run_job(&module_bytes, &runtime, &job, &module)) {
        cleanup_job(runtime, job, module, &module_bytes, NULL);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    cleanup_job(runtime, job, module, &module_bytes, NULL);
    return status == FA_RUNTIME_ERR_TRAP ? 0 : 1;
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

/* Exercises op_simd_bitwise dispatch (v128.xor) through the SIMD table. */
static int test_simd_v128_bitwise_xor(void) {
    ByteBuffer instructions = {0};
    uint8_t lhs[16];
    uint8_t rhs[16];
    memset(lhs, 0xFF, sizeof(lhs));
    memset(rhs, 0x0F, sizeof(rhs));

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, lhs, sizeof(lhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, rhs, sizeof(rhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x51); /* v128.xor */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x16); /* i8x16.extract_lane_u */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 0xF0) {
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

/* Exercises op_simd_i16x8 dispatch (i16x8.add) through the SIMD table. */
static int test_simd_i16x8_add(void) {
    ByteBuffer instructions = {0};
    uint8_t lhs[16] = {4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0};
    uint8_t rhs[16] = {5, 0, 5, 0, 5, 0, 5, 0, 5, 0, 5, 0, 5, 0, 5, 0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, lhs, sizeof(lhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, rhs, sizeof(rhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x87); /* i16x8.add */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x18); /* i16x8.extract_lane_s */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 9) {
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

/* Exercises op_simd_i64x2 dispatch (i64x2.add) through the SIMD table. */
static int test_simd_i64x2_add(void) {
    ByteBuffer instructions = {0};
    uint8_t lhs[16] = {10, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0};
    uint8_t rhs[16] = {20, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, lhs, sizeof(lhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, rhs, sizeof(rhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0xb9); /* i64x2.add */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x1d); /* i64x2.extract_lane */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_i64 || value->payload.i64_value != 30) {
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

/* Exercises op_simd_cmp dispatch (i32x4.eq) through the SIMD table. */
static int test_simd_i32x4_eq(void) {
    ByteBuffer instructions = {0};
    uint8_t lhs[16] = {7, 0, 0, 0, 7, 0, 0, 0, 7, 0, 0, 0, 7, 0, 0, 0};
    uint8_t rhs[16] = {7, 0, 0, 0, 7, 0, 0, 0, 7, 0, 0, 0, 7, 0, 0, 0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, lhs, sizeof(lhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, rhs, sizeof(rhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x37); /* i32x4.eq */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x1b); /* i32x4.extract_lane */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != -1) {
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

/* Exercises op_simd_i32x4 dispatch (i32x4.mul) through the SIMD table. */
static int test_simd_i32x4_mul(void) {
    ByteBuffer instructions = {0};
    uint8_t lhs[16] = {3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0};
    uint8_t rhs[16] = {5, 0, 0, 0, 5, 0, 0, 0, 5, 0, 0, 0, 5, 0, 0, 0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, lhs, sizeof(lhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, rhs, sizeof(rhs));
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0xa4); /* i32x4.mul */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x1b); /* i32x4.extract_lane */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != 15) {
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

/* Exercises op_simd_f32x4 dispatch (f32x4.add) through the SIMD table. */
static int test_simd_f32x4_add(void) {
    ByteBuffer instructions = {0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_f32(&instructions, 1.5f);
    bb_write_f32(&instructions, 1.5f);
    bb_write_f32(&instructions, 1.5f);
    bb_write_f32(&instructions, 1.5f);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_f32(&instructions, 2.25f);
    bb_write_f32(&instructions, 2.25f);
    bb_write_f32(&instructions, 2.25f);
    bb_write_f32(&instructions, 2.25f);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0xc9); /* f32x4.add */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x1f); /* f32x4.extract_lane */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_f32 ||
        fabsf(value->payload.f32_value - 3.75f) > 0.0001f) {
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

/* Exercises op_simd_relaxed dispatch (f32x4.relaxed_min) through the SIMD table. */
static int test_simd_f32x4_relaxed_min(void) {
    ByteBuffer instructions = {0};

    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_f32(&instructions, 1.0f);
    bb_write_f32(&instructions, 1.0f);
    bb_write_f32(&instructions, 1.0f);
    bb_write_f32(&instructions, 1.0f);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x0c);
    bb_write_f32(&instructions, 2.0f);
    bb_write_f32(&instructions, 2.0f);
    bb_write_f32(&instructions, 2.0f);
    bb_write_f32(&instructions, 2.0f);
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x10d); /* f32x4.relaxed_min */
    bb_write_byte(&instructions, 0xFD);
    bb_write_uleb(&instructions, 0x1f); /* f32x4.extract_lane */
    bb_write_byte(&instructions, 0x00);
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
    if (!value || value->kind != fa_job_value_f32 ||
        fabsf(value->payload.f32_value - 1.0f) > 0.0001f) {
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

/* Exercises op_simd_memlane (v128.load32_lane) load + lane-index path. */
static int test_simd_v128_load32_lane(void) {
    ByteBuffer instructions = {0};
    const int32_t marker = 0x33333333; /* 858993459, positive sleb32 */
    const uint8_t zeros[16] = {0};

    /* Seed memory[0] with the marker via i32.store. */
    bb_write_byte(&instructions, 0x41);          /* i32.const 0 (store addr) */
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x41);          /* i32.const marker */
    bb_write_sleb32(&instructions, marker);
    bb_write_byte(&instructions, 0x36);          /* i32.store */
    bb_write_uleb(&instructions, 2);             /* align */
    bb_write_uleb(&instructions, 0);             /* offset */

    /* Load the marker into lane 1 of a zeroed vector. */
    bb_write_byte(&instructions, 0x41);          /* i32.const 0 (load base addr) */
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0xFD);          /* v128.const zeros */
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, zeros, sizeof(zeros));
    bb_write_byte(&instructions, 0xFD);          /* v128.load32_lane */
    bb_write_uleb(&instructions, 0x56);
    bb_write_uleb(&instructions, 2);             /* align */
    bb_write_uleb(&instructions, 0);             /* offset */
    bb_write_byte(&instructions, 1);             /* lane index */
    bb_write_byte(&instructions, 0xFD);          /* i32x4.extract_lane */
    bb_write_uleb(&instructions, 0x1b);
    bb_write_byte(&instructions, 1);             /* lane index */
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
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != marker) {
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

/* Exercises op_simd_memlane (v128.store32_lane) store + lane-index path. */
static int test_simd_v128_store32_lane(void) {
    ByteBuffer instructions = {0};
    /* Distinct little-endian 32-bit lanes so lane selection is observable. */
    const uint8_t lanes[16] = {
        0x11, 0x11, 0x11, 0x11,  /* lane 0 = 0x11111111 */
        0x22, 0x22, 0x22, 0x22,  /* lane 1 = 0x22222222 */
        0x33, 0x33, 0x33, 0x33,  /* lane 2 = 0x33333333 */
        0x44, 0x44, 0x44, 0x44   /* lane 3 = 0x44444444 */
    };
    const int32_t expected = 0x33333333; /* lane 2 */

    bb_write_byte(&instructions, 0x41);          /* i32.const 0 (store base addr) */
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0xFD);          /* v128.const lanes */
    bb_write_uleb(&instructions, 0x0c);
    bb_write_bytes(&instructions, lanes, sizeof(lanes));
    bb_write_byte(&instructions, 0xFD);          /* v128.store32_lane */
    bb_write_uleb(&instructions, 0x5a);
    bb_write_uleb(&instructions, 2);             /* align */
    bb_write_uleb(&instructions, 0);             /* offset */
    bb_write_byte(&instructions, 2);             /* lane index */
    bb_write_byte(&instructions, 0x41);          /* i32.const 0 (load addr) */
    bb_write_sleb32(&instructions, 0);
    bb_write_byte(&instructions, 0x28);          /* i32.load */
    bb_write_uleb(&instructions, 2);             /* align */
    bb_write_uleb(&instructions, 0);             /* offset */
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
    if (!value || value->kind != fa_job_value_i32 || value->payload.i32_value != expected) {
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

/* Exercises op_simd_f64x2 dispatch (f64x2.add) plus f64x2.extract_lane. */
static int test_simd_f64x2_add(void) {
    ByteBuffer instructions = {0};

    bb_write_byte(&instructions, 0xFD);          /* v128.const {1.5, 1.5} */
    bb_write_uleb(&instructions, 0x0c);
    bb_write_f64(&instructions, 1.5);
    bb_write_f64(&instructions, 1.5);
    bb_write_byte(&instructions, 0xFD);          /* v128.const {2.25, 2.25} */
    bb_write_uleb(&instructions, 0x0c);
    bb_write_f64(&instructions, 2.25);
    bb_write_f64(&instructions, 2.25);
    bb_write_byte(&instructions, 0xFD);          /* f64x2.add */
    bb_write_uleb(&instructions, 0xd4);
    bb_write_byte(&instructions, 0xFD);          /* f64x2.extract_lane */
    bb_write_uleb(&instructions, 0x21);
    bb_write_byte(&instructions, 0x01);          /* lane 1 */
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
    if (!value || value->kind != fa_job_value_f64 ||
        fabs(value->payload.f64_value - 3.75) > 0.0001) {
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

static int test_ref_ops_basic(void) {
    ByteBuffer instructions = {0};
    bb_write_byte(&instructions, 0xD0);
    bb_write_byte(&instructions, VALTYPE_FUNCREF);
    bb_write_byte(&instructions, 0xD1);
    bb_write_byte(&instructions, 0xD2);
    bb_write_uleb(&instructions, 0);
    bb_write_byte(&instructions, 0xD1);
    bb_write_byte(&instructions, 0x0B);

    ByteBuffer dummy = {0};
    bb_write_byte(&dummy, 0x0B);

    const uint8_t* bodies[] = { instructions.data, dummy.data };
    const size_t sizes[] = { instructions.size, dummy.size };
    const uint8_t result_types[] = { VALTYPE_I32, VALTYPE_I32 };
    ByteBuffer module_bytes = {0};
    if (!build_module(&module_bytes,
                      bodies,
                      sizes,
                      2,
                      0,
                      0,
                      0,
                      0,
                      result_types,
                      2,
                      NULL,
                      0)) {
        bb_free(&dummy);
        cleanup_job(NULL, NULL, NULL, &module_bytes, &instructions);
        return 1;
    }
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

    const fa_JobValue* second = fa_JobStack_peek(&job->stack, 0);
    if (!second || second->kind != fa_job_value_i32 || second->payload.i32_value != 0) {
        cleanup_job(runtime, job, module, &module_bytes, &instructions);
        return 1;
    }
    const fa_JobValue* first = fa_JobStack_peek(&job->stack, 1);
    if (!first || first->kind != fa_job_value_i32 || first->payload.i32_value != 1) {
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
    TEST_CASE("test_jit_program_opcode_roundtrip", "jit", "src/fa_jit.c (opcode serialization)", test_jit_program_opcode_roundtrip),
    TEST_CASE("test_spill_envelope_jit_roundtrip", "offload", "src/fa_jit.c (versioned spill envelope), fa_jit_program_serialize/deserialize", test_spill_envelope_jit_roundtrip),
    TEST_CASE("test_spill_envelope_jit_real_wasm", "offload", "src/fa_jit.c (versioned spill envelope) via real wasm + live spill hook", test_spill_envelope_jit_real_wasm),
    TEST_CASE("test_host_import_call", "runtime", "src/fa_runtime.c (host imports), src/fa_wasm.c (import parsing)", test_host_import_call),
    TEST_CASE("test_imported_memory_binding", "memory", "src/fa_runtime.c (host memory imports), src/fa_ops.c (load)", test_imported_memory_binding),
    TEST_CASE("test_imported_memory_rebind_after_attach", "memory", "src/fa_runtime.c (host memory rebind propagation)", test_imported_memory_rebind_after_attach),
    TEST_CASE("test_imported_table_binding", "table", "src/fa_runtime.c (host table imports), src/fa_ops.c (table.size)", test_imported_table_binding),
    TEST_CASE("test_imported_table_rebind_after_attach", "table", "src/fa_runtime.c (host table rebind propagation)", test_imported_table_rebind_after_attach),
    TEST_CASE("test_memory_spill_load_cycles", "offload", "src/fa_runtime.c (memory spill/load hooks)", test_memory_spill_load_cycles),
    TEST_CASE("test_jit_eviction_trap_reload_cycles", "offload", "src/fa_runtime.c (jit eviction/load), trap hooks", test_jit_eviction_trap_reload_cycles),
    TEST_CASE("test_memory_grow_spill_load_roundtrip", "offload", "src/fa_runtime.c (memory grow + spill/load), fa_Runtime_loadMemory", test_memory_grow_spill_load_roundtrip),
    TEST_CASE("test_spill_envelope_memory_roundtrip", "offload", "src/fa_runtime.c (versioned memory spill envelope), fa_Runtime_serializeMemory/deserializeMemory", test_spill_envelope_memory_roundtrip),
    TEST_CASE("test_wasm_sample_arithmetic", "wasm-sample", "wasm_samples/build/arithmetic.wasm", test_wasm_sample_arithmetic),
    TEST_CASE("test_wasm_sample_arithmetic_mul_add", "wasm-sample", "wasm_samples/build/arithmetic.wasm", test_wasm_sample_arithmetic_mul_add),
    TEST_CASE("test_wasm_sample_control_flow", "wasm-sample", "wasm_samples/build/control_flow.wasm", test_wasm_sample_control_flow),
    TEST_CASE("test_wasm_sample_control_flow_factorial", "wasm-sample", "wasm_samples/build/control_flow.wasm", test_wasm_sample_control_flow_factorial),
    TEST_CASE("test_wasm_sample_advanced_memory_mix", "wasm-sample", "wasm_samples/build/advanced_runtime.wasm", test_wasm_sample_advanced_memory_mix),
    TEST_CASE("test_wasm_sample_advanced_call_chain", "wasm-sample", "wasm_samples/build/advanced_runtime.wasm", test_wasm_sample_advanced_call_chain),
    TEST_CASE("test_wasm_sample_typed_add_i32", "wasm-sample", "wasm_samples/build/typed_values.wasm", test_wasm_sample_typed_add_i32),
    TEST_CASE("test_wasm_sample_typed_sum_to_n", "wasm-sample", "wasm_samples/build/typed_values.wasm", test_wasm_sample_typed_sum_to_n),
    TEST_CASE("test_wasm_sample_typed_scale_i64", "wasm-sample", "wasm_samples/build/typed_values.wasm", test_wasm_sample_typed_scale_i64),
    TEST_CASE("test_wasm_sample_float_poly", "wasm-sample", "wasm_samples/build/floating_point.wasm", test_wasm_sample_float_poly),
    TEST_CASE("test_wasm_sample_float_hypot", "wasm-sample", "wasm_samples/build/floating_point.wasm", test_wasm_sample_float_hypot),
    TEST_CASE("test_wasm_sample_float_lerp", "wasm-sample", "wasm_samples/build/floating_point.wasm", test_wasm_sample_float_lerp),
    TEST_CASE("test_wasm_sample_float_round", "wasm-sample", "wasm_samples/build/floating_point.wasm", test_wasm_sample_float_round),
    TEST_CASE("test_wasm_sample_float_series", "wasm-sample", "wasm_samples/build/floating_point.wasm", test_wasm_sample_float_series),
    TEST_CASE("test_wasm_sample_dispatch_mul", "wasm-sample", "wasm_samples/build/indirect_dispatch.wasm", test_wasm_sample_dispatch_mul),
    TEST_CASE("test_wasm_sample_dispatch_fold", "wasm-sample", "wasm_samples/build/indirect_dispatch.wasm", test_wasm_sample_dispatch_fold),
    TEST_CASE("test_wasm_sample_classify_brtable", "wasm-sample", "wasm_samples/build/indirect_dispatch.wasm", test_wasm_sample_classify_brtable),
    TEST_CASE("test_wasm_sample_classify_default", "wasm-sample", "wasm_samples/build/indirect_dispatch.wasm", test_wasm_sample_classify_default),
    TEST_CASE("test_wasm_sample_sort_checksum", "wasm-sample", "wasm_samples/build/memory_ops.wasm", test_wasm_sample_sort_checksum),
    TEST_CASE("test_wasm_sample_buffer_pipeline", "wasm-sample", "wasm_samples/build/memory_ops.wasm", test_wasm_sample_buffer_pipeline),
    TEST_CASE("test_wasm_sample_table_lookup", "wasm-sample", "wasm_samples/build/memory_ops.wasm", test_wasm_sample_table_lookup),
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
    TEST_CASE("test_externref_table_active_null_elem_expr", "table", "src/fa_wasm.c (element expr parsing), src/fa_runtime.c (segment init)", test_externref_table_active_null_elem_expr),
    TEST_CASE("test_table_init_global_get_element_expr", "table", "src/fa_wasm.c (element expr global.get), src/fa_ops.c (table.init)", test_table_init_global_get_element_expr),
    TEST_CASE("test_call_indirect_basic", "call-indirect", "src/fa_runtime.c (decode+dispatch), src/fa_ops.c (call_indirect)", test_call_indirect_basic),
    TEST_CASE("test_call_indirect_function_zero", "call-indirect", "src/fa_ops.c (funcref decode), src/fa_runtime.c (dispatch)", test_call_indirect_function_zero),
    TEST_CASE("test_call_indirect_null_trap", "call-indirect", "src/fa_ops.c (table lookup trap)", test_call_indirect_null_trap),
    TEST_CASE("test_call_indirect_type_mismatch_trap", "call-indirect", "src/fa_ops.c (signature validation)", test_call_indirect_type_mismatch_trap),
    TEST_CASE("test_elem_drop_trap", "table", "src/fa_ops.c (elem.drop)", test_elem_drop_trap),
    TEST_CASE("test_table_grow", "table", "src/fa_ops.c (table.grow)", test_table_grow),
    TEST_CASE("test_simd_v128_const", "simd", "src/fa_runtime.c (simd decode), src/fa_ops.c (op_simd)", test_simd_v128_const),
    TEST_CASE("test_simd_i32x4_splat", "simd", "src/fa_ops.c (op_simd splat)", test_simd_i32x4_splat),
    TEST_CASE("test_simd_v128_load_store", "simd", "src/fa_runtime.c (simd memarg), src/fa_ops.c (op_simd)", test_simd_v128_load_store),
    TEST_CASE("test_simd_i8x16_add", "simd", "src/fa_ops.c (i8x16.add)", test_simd_i8x16_add),
    TEST_CASE("test_simd_i8x16_replace_extract", "simd", "src/fa_ops.c (lane ops)", test_simd_i8x16_replace_extract),
    TEST_CASE("test_simd_trunc_sat_f32x4", "simd", "src/fa_ops.c (trunc sat)", test_simd_trunc_sat_f32x4),
    TEST_CASE("test_trunc_sat_f64_i32_saturation", "convert", "src/fa_ops.c (scalar trunc_sat 0xFC 0x00-0x07)", test_trunc_sat_f64_i32_saturation),
    TEST_CASE("test_simd_v128_bitwise_xor", "simd", "src/fa_ops.c (op_simd_bitwise)", test_simd_v128_bitwise_xor),
    TEST_CASE("test_simd_i16x8_add", "simd", "src/fa_ops.c (op_simd_i16x8)", test_simd_i16x8_add),
    TEST_CASE("test_simd_i64x2_add", "simd", "src/fa_ops.c (op_simd_i64x2)", test_simd_i64x2_add),
    TEST_CASE("test_simd_i32x4_eq", "simd", "src/fa_ops.c (op_simd_cmp)", test_simd_i32x4_eq),
    TEST_CASE("test_simd_i32x4_mul", "simd", "src/fa_ops.c (op_simd_i32x4)", test_simd_i32x4_mul),
    TEST_CASE("test_simd_f32x4_add", "simd", "src/fa_ops.c (op_simd_f32x4)", test_simd_f32x4_add),
    TEST_CASE("test_simd_f32x4_relaxed_min", "simd", "src/fa_ops.c (op_simd_relaxed)", test_simd_f32x4_relaxed_min),
    TEST_CASE("test_simd_v128_load32_lane", "simd", "src/fa_ops.c (op_simd_memlane load)", test_simd_v128_load32_lane),
    TEST_CASE("test_simd_v128_store32_lane", "simd", "src/fa_ops.c (op_simd_memlane store)", test_simd_v128_store32_lane),
    TEST_CASE("test_simd_f64x2_add", "simd", "src/fa_ops.c (op_simd_f64x2)", test_simd_f64x2_add),
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
    TEST_CASE("test_ref_ops_basic", "refs", "src/fa_runtime.c (decode), src/fa_ops.c (ref ops)", test_ref_ops_basic),
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
