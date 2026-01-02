#pragma once

#include "fa_types.h"
#include "helpers/dynamic_list.h"
#include "fa_job.h"
#include "fa_wasm_stream.h"

#define FA_WASM_PAGE_SIZE 65536U

enum {
    FA_RUNTIME_OK = 0,
    FA_RUNTIME_ERR_INVALID_ARGUMENT = -1,
    FA_RUNTIME_ERR_OUT_OF_MEMORY = -2,
    FA_RUNTIME_ERR_NO_MODULE = -3,
    FA_RUNTIME_ERR_STREAM = -4,
    FA_RUNTIME_ERR_UNSUPPORTED = -5,
    FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE = -6,
    FA_RUNTIME_ERR_CALL_DEPTH_EXCEEDED = -7,
    FA_RUNTIME_ERR_TRAP = -8
};

typedef struct {
    uint8_t* data;
    uint64_t size_bytes;
    uint64_t max_size_bytes;
    bool has_max;
    bool is_memory64;
} fa_RuntimeMemory;

typedef struct fa_Runtime {
    fa_Malloc malloc;
    fa_Free free;

    list_t* jobs; // fa_Job[]
    WasmModule* module;
    WasmInstructionStream* stream;
    jobId_t next_job_id;
    uint32_t max_call_depth;
    fa_RuntimeMemory memory;
    fa_JobValue* active_locals;
    uint32_t active_locals_count;
    fa_JobValue* globals;
    uint32_t globals_count;
} fa_Runtime;

fa_Runtime* fa_Runtime_init(void);
void fa_Runtime_free(fa_Runtime* runtime);

int fa_Runtime_attach_module(fa_Runtime* runtime, WasmModule* module);
void fa_Runtime_detach_module(fa_Runtime* runtime);

fa_Job* fa_Runtime_create_job(fa_Runtime* runtime);
int fa_Runtime_destroy_job(fa_Runtime* runtime, fa_Job* job);

int fa_Runtime_execute_job(fa_Runtime* runtime, fa_Job* job, uint32_t function_index);
