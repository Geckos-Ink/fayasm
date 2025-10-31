#pragma once

#include "fa_types.h"
#include "dynamic_list.h"
#include "fa_job.h"
#include "fa_wasm_stream.h"

enum {
    FA_RUNTIME_OK = 0,
    FA_RUNTIME_ERR_INVALID_ARGUMENT = -1,
    FA_RUNTIME_ERR_OUT_OF_MEMORY = -2,
    FA_RUNTIME_ERR_NO_MODULE = -3,
    FA_RUNTIME_ERR_STREAM = -4,
    FA_RUNTIME_ERR_UNSUPPORTED = -5,
    FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE = -6,
    FA_RUNTIME_ERR_CALL_DEPTH_EXCEEDED = -7
};

typedef struct {
    fa_Malloc malloc;
    fa_Free free;

    list_t* jobs; // fa_Job[]
    WasmModule* module;
    WasmInstructionStream* stream;
    jobId_t next_job_id;
    uint32_t max_call_depth;
} fa_Runtime;

fa_Runtime* fa_Runtime_init(void);
void fa_Runtime_free(fa_Runtime* runtime);

int fa_Runtime_attach_module(fa_Runtime* runtime, WasmModule* module);
void fa_Runtime_detach_module(fa_Runtime* runtime);

fa_Job* fa_Runtime_create_job(fa_Runtime* runtime);
int fa_Runtime_destroy_job(fa_Runtime* runtime, fa_Job* job);

int fa_Runtime_execute_job(fa_Runtime* runtime, fa_Job* job, uint32_t function_index);
