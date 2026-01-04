#include "fa_runtime.h"
#include "fa_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifndef FAYASM_SAMPLE_WASM_PATH
#define FAYASM_SAMPLE_WASM_PATH "/sdcard/app.wasm"
#endif

#define JIT_MAGIC 0x54494A46u
#define MEM_MAGIC 0x4D454D46u

typedef struct {
    uint32_t magic;
    uint32_t count;
} JitFileHeader;

typedef struct {
    uint32_t magic;
    uint64_t size;
} MemFileHeader;

static void make_jit_path(char* out, size_t out_size, uint32_t func_index) {
    snprintf(out, out_size, "/sdcard/fayasm_jit_%u.bin", func_index);
}

static void make_mem_path(char* out, size_t out_size, uint32_t mem_index) {
    snprintf(out, out_size, "/sdcard/fayasm_mem_%u.bin", mem_index);
}

static int jit_spill(fa_Runtime* runtime,
                     uint32_t function_index,
                     const fa_JitProgram* program,
                     size_t program_bytes,
                     void* user_data) {
    (void)runtime;
    (void)user_data;
    if (!program || !program->ops || program->count == 0 || program_bytes == 0) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    char path[64];
    make_jit_path(path, sizeof(path), function_index);
    FILE* file = fopen(path, "wb");
    if (!file) {
        return FA_RUNTIME_ERR_STREAM;
    }
    JitFileHeader header = { JIT_MAGIC, (uint32_t)program->count };
    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        fwrite(program->ops, sizeof(fa_JitPreparedOp), program->count, file) != program->count) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    fclose(file);
    return FA_RUNTIME_OK;
}

static int jit_load(fa_Runtime* runtime,
                    uint32_t function_index,
                    fa_JitProgram* program_out,
                    void* user_data) {
    (void)runtime;
    (void)user_data;
    if (!program_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    char path[64];
    make_jit_path(path, sizeof(path), function_index);
    FILE* file = fopen(path, "rb");
    if (!file) {
        return FA_RUNTIME_ERR_STREAM;
    }
    JitFileHeader header = {0};
    if (fread(&header, sizeof(header), 1, file) != 1 || header.magic != JIT_MAGIC) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    if (header.count == 0) {
        fclose(file);
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_jit_program_init(program_out);
    program_out->ops = (fa_JitPreparedOp*)calloc(header.count, sizeof(fa_JitPreparedOp));
    if (!program_out->ops) {
        fclose(file);
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    program_out->capacity = header.count;
    program_out->count = header.count;
    if (fread(program_out->ops, sizeof(fa_JitPreparedOp), header.count, file) != header.count) {
        fa_jit_program_free(program_out);
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    fclose(file);
    return FA_RUNTIME_OK;
}

static int memory_spill(fa_Runtime* runtime,
                        uint32_t memory_index,
                        const fa_RuntimeMemory* memory,
                        void* user_data) {
    (void)runtime;
    (void)user_data;
    if (!memory || !memory->data || memory->size_bytes == 0) {
        return FA_RUNTIME_OK;
    }
    if (memory->size_bytes > SIZE_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    const size_t bytes = (size_t)memory->size_bytes;
    char path[64];
    make_mem_path(path, sizeof(path), memory_index);
    FILE* file = fopen(path, "wb");
    if (!file) {
        return FA_RUNTIME_ERR_STREAM;
    }
    MemFileHeader header = { MEM_MAGIC, memory->size_bytes };
    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        fwrite(memory->data, 1, bytes, file) != bytes) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    fclose(file);
    return FA_RUNTIME_OK;
}

static int memory_load(fa_Runtime* runtime,
                       uint32_t memory_index,
                       fa_RuntimeMemory* memory,
                       void* user_data) {
    (void)user_data;
    if (!runtime || !memory || memory->size_bytes == 0) {
        return FA_RUNTIME_OK;
    }
    if (memory->size_bytes > SIZE_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    const size_t bytes = (size_t)memory->size_bytes;
    char path[64];
    make_mem_path(path, sizeof(path), memory_index);
    FILE* file = fopen(path, "rb");
    if (!file) {
        return FA_RUNTIME_ERR_STREAM;
    }
    MemFileHeader header = {0};
    if (fread(&header, sizeof(header), 1, file) != 1 || header.magic != MEM_MAGIC) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    if (header.size != memory->size_bytes) {
        fclose(file);
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    memory->data = (uint8_t*)runtime->malloc((int)memory->size_bytes);
    if (!memory->data) {
        fclose(file);
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    if (fread(memory->data, 1, bytes, file) != bytes) {
        runtime->free(memory->data);
        memory->data = NULL;
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    fclose(file);
    return FA_RUNTIME_OK;
}

typedef struct {
    uint32_t target_function;
} TrapState;

static int function_trap(fa_Runtime* runtime, uint32_t function_index, void* user_data) {
    TrapState* state = (TrapState*)user_data;
    if (!state || function_index != state->target_function) {
        return FA_RUNTIME_OK;
    }
    int status = fa_Runtime_jit_load_program(runtime, function_index);
    if (status == FA_RUNTIME_OK) {
        (void)fa_Runtime_set_function_trap(runtime, function_index, false);
    }
    return FA_RUNTIME_OK;
}

int main(void) {
    WasmModule* module = wasm_module_init(FAYASM_SAMPLE_WASM_PATH);
    if (!module) {
        printf("Failed to open %s\n", FAYASM_SAMPLE_WASM_PATH);
        return 1;
    }
    if (wasm_load_header(module) != 0 ||
        wasm_scan_sections(module) != 0 ||
        wasm_load_types(module) != 0 ||
        wasm_load_functions(module) != 0 ||
        wasm_load_tables(module) != 0 ||
        wasm_load_memories(module) != 0 ||
        wasm_load_globals(module) != 0 ||
        wasm_load_elements(module) != 0 ||
        wasm_load_data(module) != 0) {
        printf("Failed to load module sections\n");
        wasm_module_free(module);
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        wasm_module_free(module);
        return 1;
    }

    runtime->jit_context.config.min_ram_bytes = 0;
    runtime->jit_context.config.min_cpu_count = 1;
    runtime->jit_context.config.min_hot_loop_hits = 0;
    runtime->jit_context.config.min_executed_ops = 1;
    runtime->jit_context.config.min_advantage_score = 0.0f;
    runtime->jit_context.config.prescan_functions = true;

    if (fa_Runtime_attach_module(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    fa_RuntimeSpillHooks spill_hooks = { jit_spill, jit_load, memory_spill, memory_load, NULL };
    fa_Runtime_set_spill_hooks(runtime, &spill_hooks);

    TrapState trap_state = {0};
    trap_state.target_function = 0;
    fa_RuntimeTrapHooks trap_hooks = { function_trap, &trap_state };
    fa_Runtime_set_trap_hooks(runtime, &trap_hooks);
    (void)fa_Runtime_set_function_trap(runtime, trap_state.target_function, true);

    if (runtime->memories_count > 0) {
        (void)fa_Runtime_spill_memory(runtime, 0);
    }

    fa_Job* job = fa_Runtime_create_job(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        printf("Execution error: %d\n", status);
    }

    (void)fa_Runtime_destroy_job(runtime, job);
    fa_Runtime_free(runtime);
    wasm_module_free(module);
    return status == FA_RUNTIME_OK ? 0 : 1;
}
