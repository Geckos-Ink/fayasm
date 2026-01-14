#pragma once

#include "fa_types.h"
#include "helpers/dynamic_list.h"
#include "fa_job.h"
#include "fa_wasm_stream.h"
#include "fa_jit.h"

struct fa_JitProgramCacheEntry;
struct fa_RuntimeHostBinding;
struct fa_RuntimeHostMemoryBinding;
struct fa_RuntimeHostTableBinding;

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
    bool is_spilled;
    bool is_host;
    bool owns_data;
} fa_RuntimeMemory;

typedef struct {
    fa_ptr* data;
    uint32_t size;
    uint32_t max_size;
    bool has_max;
    uint8_t elem_type;
    bool is_host;
    bool owns_data;
} fa_RuntimeTable;

typedef int (*fa_RuntimeFunctionTrapHook)(struct fa_Runtime* runtime, uint32_t function_index, void* user_data);

typedef struct {
    fa_RuntimeFunctionTrapHook on_function_trap;
    void* user_data;
} fa_RuntimeTrapHooks;

typedef int (*fa_RuntimeJitSpillHook)(struct fa_Runtime* runtime,
                                      uint32_t function_index,
                                      const fa_JitProgram* program,
                                      size_t program_bytes,
                                      void* user_data);

typedef int (*fa_RuntimeJitLoadHook)(struct fa_Runtime* runtime,
                                     uint32_t function_index,
                                     fa_JitProgram* program_out,
                                     void* user_data);

typedef int (*fa_RuntimeMemorySpillHook)(struct fa_Runtime* runtime,
                                         uint32_t memory_index,
                                         const fa_RuntimeMemory* memory,
                                         void* user_data);

typedef int (*fa_RuntimeMemoryLoadHook)(struct fa_Runtime* runtime,
                                        uint32_t memory_index,
                                        fa_RuntimeMemory* memory,
                                        void* user_data);

typedef struct {
    fa_RuntimeJitSpillHook jit_spill;
    fa_RuntimeJitLoadHook jit_load;
    fa_RuntimeMemorySpillHook memory_spill;
    fa_RuntimeMemoryLoadHook memory_load;
    void* user_data;
} fa_RuntimeSpillHooks;

typedef struct {
    const WasmFunctionType* signature;
    const fa_JobValue* args;
    uint32_t arg_count;
    fa_JobValue* results;
    uint32_t result_count;
    uint32_t function_index;
    const char* import_module;
    const char* import_name;
} fa_RuntimeHostCall;

typedef int (*fa_RuntimeHostFunction)(struct fa_Runtime* runtime,
                                      const fa_RuntimeHostCall* call,
                                      void* user_data);

typedef struct {
    uint8_t* data;
    uint64_t size_bytes;
} fa_RuntimeHostMemory;

typedef struct {
    fa_ptr* data;
    uint32_t size;
} fa_RuntimeHostTable;

typedef struct fa_Runtime {
    fa_Malloc malloc;
    fa_Free free;

    list_t* jobs; // fa_Job[]
    WasmModule* module;
    WasmInstructionStream* stream;
    jobId_t next_job_id;
    uint32_t max_call_depth;
    fa_RuntimeMemory* memories;
    uint32_t memories_count;
    fa_RuntimeTable* tables;
    uint32_t tables_count;
    bool* data_segments_dropped;
    uint32_t data_segments_count;
    bool* elem_segments_dropped;
    uint32_t elem_segments_count;
    fa_JobValue* active_locals;
    uint32_t active_locals_count;
    fa_JobValue* globals;
    uint32_t globals_count;
    fa_JitContext jit_context;
    fa_JitStats jit_stats;
    struct fa_JitProgramCacheEntry* jit_cache;
    uint32_t jit_cache_count;
    uint64_t jit_prepared_executions;
    size_t jit_cache_bytes;
    uint32_t jit_cache_eviction_cursor;
    bool jit_cache_prescanned;
    struct fa_RuntimeHostBinding* host_bindings;
    uint32_t host_binding_count;
    uint32_t host_binding_capacity;
    struct fa_RuntimeHostMemoryBinding* host_memory_bindings;
    uint32_t host_memory_binding_count;
    uint32_t host_memory_binding_capacity;
    struct fa_RuntimeHostTableBinding* host_table_bindings;
    uint32_t host_table_binding_count;
    uint32_t host_table_binding_capacity;
    uint8_t* function_traps;
    uint32_t function_trap_count;
    fa_RuntimeTrapHooks trap_hooks;
    fa_RuntimeSpillHooks spill_hooks;
} fa_Runtime;

fa_Runtime* fa_Runtime_init(void);
void fa_Runtime_free(fa_Runtime* runtime);

int fa_Runtime_attach_module(fa_Runtime* runtime, WasmModule* module);
void fa_Runtime_detach_module(fa_Runtime* runtime);

fa_Job* fa_Runtime_create_job(fa_Runtime* runtime);
int fa_Runtime_destroy_job(fa_Runtime* runtime, fa_Job* job);

int fa_Runtime_execute_job(fa_Runtime* runtime, fa_Job* job, uint32_t function_index);

int fa_Runtime_set_imported_global(fa_Runtime* runtime, uint32_t global_index, const fa_JobValue* value);

int fa_Runtime_bind_host_function(fa_Runtime* runtime,
                                  const char* module_name,
                                  const char* import_name,
                                  fa_RuntimeHostFunction function,
                                  void* user_data);
int fa_Runtime_bind_host_function_from_library(fa_Runtime* runtime,
                                               const char* module_name,
                                               const char* import_name,
                                               const char* library_path,
                                               const char* symbol_name);
int fa_Runtime_bind_imported_memory(fa_Runtime* runtime,
                                    const char* module_name,
                                    const char* import_name,
                                    const fa_RuntimeHostMemory* memory);
int fa_Runtime_bind_imported_table(fa_Runtime* runtime,
                                   const char* module_name,
                                   const char* import_name,
                                   const fa_RuntimeHostTable* table);

bool fa_RuntimeHostCall_expect(const fa_RuntimeHostCall* call, uint32_t arg_count, uint32_t result_count);
bool fa_RuntimeHostCall_arg_i32(const fa_RuntimeHostCall* call, uint32_t index, i32* out);
bool fa_RuntimeHostCall_arg_i64(const fa_RuntimeHostCall* call, uint32_t index, i64* out);
bool fa_RuntimeHostCall_arg_f32(const fa_RuntimeHostCall* call, uint32_t index, f32* out);
bool fa_RuntimeHostCall_arg_f64(const fa_RuntimeHostCall* call, uint32_t index, f64* out);
bool fa_RuntimeHostCall_arg_ref(const fa_RuntimeHostCall* call, uint32_t index, fa_ptr* out);
bool fa_RuntimeHostCall_set_i32(const fa_RuntimeHostCall* call, uint32_t index, i32 value);
bool fa_RuntimeHostCall_set_i64(const fa_RuntimeHostCall* call, uint32_t index, i64 value);
bool fa_RuntimeHostCall_set_f32(const fa_RuntimeHostCall* call, uint32_t index, f32 value);
bool fa_RuntimeHostCall_set_f64(const fa_RuntimeHostCall* call, uint32_t index, f64 value);
bool fa_RuntimeHostCall_set_ref(const fa_RuntimeHostCall* call, uint32_t index, fa_ptr value);

void fa_Runtime_set_trap_hooks(fa_Runtime* runtime, const fa_RuntimeTrapHooks* hooks);
int fa_Runtime_set_function_trap(fa_Runtime* runtime, uint32_t function_index, bool enabled);
void fa_Runtime_clear_function_traps(fa_Runtime* runtime);

void fa_Runtime_set_spill_hooks(fa_Runtime* runtime, const fa_RuntimeSpillHooks* hooks);
int fa_Runtime_jit_spill_program(fa_Runtime* runtime, uint32_t function_index);
int fa_Runtime_jit_load_program(fa_Runtime* runtime, uint32_t function_index);
int fa_Runtime_spill_memory(fa_Runtime* runtime, uint32_t memory_index);
int fa_Runtime_load_memory(fa_Runtime* runtime, uint32_t memory_index);
int fa_Runtime_ensure_memory_loaded(fa_Runtime* runtime, uint32_t memory_index);
