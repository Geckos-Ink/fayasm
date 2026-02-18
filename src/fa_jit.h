#pragma once

#include "fa_ops.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef FA_JIT_MAX_STEPS_PER_OP
#define FA_JIT_MAX_STEPS_PER_OP 4
#endif

typedef enum {
    FA_JIT_TIER_OFF = 0,
    FA_JIT_TIER_MICROCODE = 1,
    FA_JIT_TIER_NATIVE = 2
} fa_JitTier;

typedef enum {
    FA_JIT_DECISION_OK = 0,
    FA_JIT_DECISION_LOW_RESOURCES,
    FA_JIT_DECISION_LOW_ADVANTAGE
} fa_JitDecisionReason;

typedef struct {
    uint64_t ram_bytes;
    uint32_t cpu_count;
    bool ok;
} fa_JitProbe;

typedef struct {
    uint64_t min_ram_bytes;
    uint32_t min_cpu_count;
    uint32_t max_cache_percent;
    uint32_t max_ops_per_chunk;
    uint32_t max_chunks;
    uint32_t min_hot_loop_hits;
    uint64_t min_executed_ops;
    float min_advantage_score;
    bool prescan_functions;
    bool prescan_force;
} fa_JitConfig;

typedef struct {
    uint64_t cache_budget_bytes;
    uint32_t max_ops_per_chunk;
    uint32_t max_chunks;
} fa_JitBudget;

typedef struct {
    uint64_t executed_ops;
    uint64_t decoded_ops;
    uint64_t hot_loop_hits;
} fa_JitStats;

typedef struct {
    fa_JitTier tier;
    fa_JitDecisionReason reason;
    float advantage_score;
    fa_JitBudget budget;
} fa_JitDecision;

typedef struct {
    const fa_WasmOp* descriptor;
    Operation steps[FA_JIT_MAX_STEPS_PER_OP];
    uint8_t step_count;
} fa_JitPreparedOp;

typedef struct {
    fa_JitPreparedOp* ops;
    size_t count;
    size_t capacity;
} fa_JitProgram;

typedef struct {
    fa_JitProbe probe;
    fa_JitConfig config;
    fa_JitDecision decision;
} fa_JitContext;

fa_JitProbe fa_jit_probe_system(void);
fa_JitConfig fa_jit_default_config(void);
fa_JitBudget fa_jit_compute_budget(const fa_JitProbe* probe, const fa_JitConfig* config);
float fa_jit_score_advantage(const fa_JitConfig* config, const fa_JitStats* stats);
fa_JitDecision fa_jit_decide(const fa_JitProbe* probe, const fa_JitConfig* config, const fa_JitStats* stats);

void fa_jit_context_init(fa_JitContext* ctx, const fa_JitConfig* config);
void fa_jit_context_update(fa_JitContext* ctx, const fa_JitStats* stats);
void fa_jit_context_apply_env_overrides(fa_JitContext* ctx);

void fa_jit_program_init(fa_JitProgram* program);
void fa_jit_program_free(fa_JitProgram* program);
bool fa_jit_prepare_op(const fa_WasmOp* descriptor, fa_JitPreparedOp* out);
bool fa_jit_prepare_program_from_opcodes(const uint8_t* opcodes, size_t opcode_count, fa_JitProgram* program);
bool fa_jit_program_export_opcodes(const fa_JitProgram* program,
                                   uint8_t* opcodes_out,
                                   size_t opcodes_capacity,
                                   size_t* opcode_count_out);
bool fa_jit_program_import_opcodes(const uint8_t* opcodes, size_t opcode_count, fa_JitProgram* program_out);
size_t fa_jit_program_estimate_bytes(const fa_JitProgram* program);
OP_RETURN_TYPE fa_jit_execute_prepared_op(const fa_JitPreparedOp* prepared, struct fa_Runtime* runtime, fa_Job* job);
