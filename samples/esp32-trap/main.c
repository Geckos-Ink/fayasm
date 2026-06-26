#include "fa_runtime.h"
#include "fa_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifndef FAYASM_SAMPLE_WASM_PATH
#define FAYASM_SAMPLE_WASM_PATH "/sdcard/app.wasm"
#endif

/*
 * Persistence uses the runtime-wide versioned spill envelope (FA_SPILL_*),
 * shared by every host: blobs are produced by fa_jit_program_serialize /
 * fa_Runtime_serializeMemory and consumed by the matching deserialize calls.
 * The envelope carries its own magic/version/kind/size in fixed little-endian
 * byte order, so the on-SD format is portable across boots and architectures
 * without any hand-rolled struct layout. JIT blobs store opcode streams (never
 * raw function pointers) and rebuild microcode on load.
 */

static void make_jit_path(char* out, size_t out_size, uint32_t func_index) {
    snprintf(out, out_size, "/sdcard/fayasm_jit_%u.bin", func_index);
}

static void make_mem_path(char* out, size_t out_size, uint32_t mem_index) {
    snprintf(out, out_size, "/sdcard/fayasm_mem_%u.bin", mem_index);
}

/* Writes a finished spill blob to SD. */
static int write_blob_file(const char* path, const uint8_t* blob, size_t size) {
    FILE* file = fopen(path, "wb");
    if (!file) {
        return FA_RUNTIME_ERR_STREAM;
    }
    if (size > 0 && fwrite(blob, 1, size, file) != size) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    fclose(file);
    return FA_RUNTIME_OK;
}

/* Reads an entire spill blob from SD into a freshly allocated buffer. */
static int read_blob_file(const char* path, uint8_t** blob_out, size_t* size_out) {
    *blob_out = NULL;
    *size_out = 0;
    FILE* file = fopen(path, "rb");
    if (!file) {
        return FA_RUNTIME_ERR_STREAM;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    size_t size = (size_t)length;
    uint8_t* blob = (uint8_t*)malloc(size > 0 ? size : 1);
    if (!blob) {
        fclose(file);
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    if (size > 0 && fread(blob, 1, size, file) != size) {
        free(blob);
        fclose(file);
        return FA_RUNTIME_ERR_STREAM;
    }
    fclose(file);
    *blob_out = blob;
    *size_out = size;
    return FA_RUNTIME_OK;
}

static int jit_spill(fa_Runtime* runtime,
                     uint32_t function_index,
                     const fa_JitProgram* program,
                     size_t program_bytes,
                     void* user_data) {
    (void)runtime;
    (void)program_bytes;
    (void)user_data;
    size_t needed = fa_jit_program_serialized_size(program);
    if (needed == 0) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    uint8_t* blob = (uint8_t*)malloc(needed);
    if (!blob) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    size_t written = 0;
    if (!fa_jit_program_serialize(program, blob, needed, &written) || written != needed) {
        free(blob);
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    char path[64];
    make_jit_path(path, sizeof(path), function_index);
    int status = write_blob_file(path, blob, written);
    free(blob);
    return status;
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
    uint8_t* blob = NULL;
    size_t size = 0;
    int status = read_blob_file(path, &blob, &size);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    /* Validates magic/version/kind and rebuilds microcode from the opcodes. */
    if (!fa_jit_program_deserialize(blob, size, program_out)) {
        free(blob);
        return FA_RUNTIME_ERR_TRAP;
    }
    free(blob);
    return FA_RUNTIME_OK;
}

static int memory_spill(fa_Runtime* runtime,
                        uint32_t memory_index,
                        const fa_RuntimeMemory* memory,
                        void* user_data) {
    (void)user_data;
    if (!memory || !memory->data || memory->size_bytes == 0) {
        return FA_RUNTIME_OK;
    }
    size_t needed = fa_Runtime_serializedMemorySize(runtime, memory_index);
    if (needed == 0) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    uint8_t* blob = (uint8_t*)malloc(needed);
    if (!blob) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    size_t written = 0;
    if (!fa_Runtime_serializeMemory(runtime, memory_index, blob, needed, &written) || written != needed) {
        free(blob);
        return FA_RUNTIME_ERR_STREAM;
    }
    char path[64];
    make_mem_path(path, sizeof(path), memory_index);
    int status = write_blob_file(path, blob, written);
    free(blob);
    return status;
}

static int memory_load(fa_Runtime* runtime,
                       uint32_t memory_index,
                       fa_RuntimeMemory* memory,
                       void* user_data) {
    (void)user_data;
    if (!runtime || !memory || memory->size_bytes == 0) {
        return FA_RUNTIME_OK;
    }
    char path[64];
    make_mem_path(path, sizeof(path), memory_index);
    uint8_t* blob = NULL;
    size_t size = 0;
    int status = read_blob_file(path, &blob, &size);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    /* Restores size/flags/bytes and re-attaches the buffer to the memory. */
    if (!fa_Runtime_deserializeMemory(runtime, memory_index, blob, size)) {
        free(blob);
        return FA_RUNTIME_ERR_STREAM;
    }
    free(blob);
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
    int status = fa_Runtime_jitLoadProgram(runtime, function_index);
    if (status == FA_RUNTIME_OK) {
        (void)fa_Runtime_setFunctionTrap(runtime, function_index, false);
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

    if (fa_Runtime_attachModule(runtime, module) != FA_RUNTIME_OK) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    fa_RuntimeSpillHooks spill_hooks = { jit_spill, jit_load, memory_spill, memory_load, NULL };
    fa_Runtime_setSpillHooks(runtime, &spill_hooks);

    TrapState trap_state = {0};
    trap_state.target_function = 0;
    fa_RuntimeTrapHooks trap_hooks = { function_trap, &trap_state };
    fa_Runtime_setTrapHooks(runtime, &trap_hooks);
    (void)fa_Runtime_setFunctionTrap(runtime, trap_state.target_function, true);

    if (runtime->memories_count > 0) {
        (void)fa_Runtime_spillMemory(runtime, 0);
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    int status = fa_Runtime_executeJob(runtime, job, 0);
    if (status != FA_RUNTIME_OK) {
        printf("Execution error: %d\n", status);
    }

    (void)fa_Runtime_destroyJob(runtime, job);
    fa_Runtime_free(runtime);
    wasm_module_free(module);
    return status == FA_RUNTIME_OK ? 0 : 1;
}
