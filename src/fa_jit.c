#include "fa_jit.h"
#include "fa_runtime.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif

static uint64_t clamp_u64(uint64_t value, uint64_t min, uint64_t max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static bool jit_env_flag(const char* name, bool* out) {
    if (!name || !out) {
        return false;
    }
    const char* value = getenv(name);
    if (!value) {
        return false;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

fa_JitProbe fa_jit_probe_system(void) {
    fa_JitProbe probe;
    memset(&probe, 0, sizeof(probe));
#if defined(_WIN32)
    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        probe.ram_bytes = (uint64_t)memory.ullTotalPhys;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors > 0) {
        probe.cpu_count = (uint32_t)info.dwNumberOfProcessors;
    }
#elif defined(__APPLE__) || defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        probe.ram_bytes = (uint64_t)pages * (uint64_t)page_size;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) {
        cpus = sysconf(_SC_NPROCESSORS_CONF);
    }
    if (cpus > 0) {
        probe.cpu_count = (uint32_t)cpus;
    }
#if defined(__APPLE__)
    if (probe.ram_bytes == 0) {
        uint64_t memsize = 0;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0 && memsize > 0) {
            probe.ram_bytes = memsize;
        }
    }
    if (probe.cpu_count == 0) {
        uint32_t ncpu = 0;
        size_t len = sizeof(ncpu);
        if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
            probe.cpu_count = ncpu;
        }
    }
#endif
#endif
    probe.ok = (probe.ram_bytes > 0 && probe.cpu_count > 0);
    return probe;
}

fa_JitConfig fa_jit_default_config(void) {
    fa_JitConfig config;
    config.min_ram_bytes = 64ULL * 1024ULL * 1024ULL;
    config.min_cpu_count = 2U;
    config.max_cache_percent = 4U;
    config.max_ops_per_chunk = 512U;
    config.max_chunks = 64U;
    config.min_hot_loop_hits = 16U;
    config.min_executed_ops = 1024ULL;
    config.min_advantage_score = 0.55f;
    config.prescan_functions = false;
    (void)jit_env_flag("FAYASM_JIT_PRESCAN", &config.prescan_functions);
    return config;
}

fa_JitBudget fa_jit_compute_budget(const fa_JitProbe* probe, const fa_JitConfig* config) {
    fa_JitBudget budget;
    memset(&budget, 0, sizeof(budget));
    if (!probe || !config) {
        return budget;
    }
    if (!probe->ok) {
        return budget;
    }
    const uint64_t base = probe->ram_bytes / 100ULL;
    const uint64_t cache = base * (uint64_t)config->max_cache_percent;
    budget.cache_budget_bytes = clamp_u64(cache, 64ULL * 1024ULL, probe->ram_bytes / 2ULL);
    budget.max_ops_per_chunk = config->max_ops_per_chunk;
    budget.max_chunks = config->max_chunks;
    return budget;
}

float fa_jit_score_advantage(const fa_JitConfig* config, const fa_JitStats* stats) {
    if (!config || !stats) {
        return 0.0f;
    }
    if (stats->executed_ops < config->min_executed_ops) {
        return 0.0f;
    }
    float hot_score = 0.0f;
    if (stats->hot_loop_hits >= config->min_hot_loop_hits) {
        hot_score = 1.0f;
    } else if (stats->hot_loop_hits > 0) {
        hot_score = 0.5f;
    }
    float decode_ratio = 0.0f;
    if (stats->executed_ops > 0) {
        decode_ratio = (float)stats->decoded_ops / (float)stats->executed_ops;
        if (decode_ratio > 1.0f) {
            decode_ratio = 1.0f;
        }
    }
    return (hot_score * 0.6f) + (decode_ratio * 0.4f);
}

fa_JitDecision fa_jit_decide(const fa_JitProbe* probe, const fa_JitConfig* config, const fa_JitStats* stats) {
    fa_JitDecision decision;
    memset(&decision, 0, sizeof(decision));
    if (!probe || !config) {
        decision.tier = FA_JIT_TIER_OFF;
        decision.reason = FA_JIT_DECISION_LOW_RESOURCES;
        return decision;
    }
    if (!probe->ok || probe->ram_bytes < config->min_ram_bytes || probe->cpu_count < config->min_cpu_count) {
        decision.tier = FA_JIT_TIER_OFF;
        decision.reason = FA_JIT_DECISION_LOW_RESOURCES;
        return decision;
    }
    decision.budget = fa_jit_compute_budget(probe, config);
    decision.advantage_score = fa_jit_score_advantage(config, stats);
    if (decision.advantage_score < config->min_advantage_score) {
        decision.tier = FA_JIT_TIER_OFF;
        decision.reason = FA_JIT_DECISION_LOW_ADVANTAGE;
        return decision;
    }
    decision.tier = FA_JIT_TIER_MICROCODE;
    decision.reason = FA_JIT_DECISION_OK;
    return decision;
}

void fa_jit_context_init(fa_JitContext* ctx, const fa_JitConfig* config) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->config = config ? *config : fa_jit_default_config();
    ctx->probe = fa_jit_probe_system();
    ctx->decision = fa_jit_decide(&ctx->probe, &ctx->config, NULL);
}

void fa_jit_context_update(fa_JitContext* ctx, const fa_JitStats* stats) {
    if (!ctx) {
        return;
    }
    ctx->probe = fa_jit_probe_system();
    ctx->decision = fa_jit_decide(&ctx->probe, &ctx->config, stats);
}

void fa_jit_program_init(fa_JitProgram* program) {
    if (!program) {
        return;
    }
    memset(program, 0, sizeof(*program));
}

void fa_jit_program_free(fa_JitProgram* program) {
    if (!program) {
        return;
    }
    free(program->ops);
    program->ops = NULL;
    program->count = 0;
    program->capacity = 0;
}

bool fa_jit_prepare_op(const fa_WasmOp* descriptor, fa_JitPreparedOp* out) {
    if (!descriptor || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->descriptor = descriptor;
    const Operation* mc_steps = NULL;
    uint8_t mc_count = 0;
    if (fa_ops_get_microcode_steps(descriptor->id, &mc_steps, &mc_count) && mc_steps && mc_count > 0) {
        if (mc_count > FA_JIT_MAX_STEPS_PER_OP) {
            mc_count = FA_JIT_MAX_STEPS_PER_OP;
        }
        memcpy(out->steps, mc_steps, (size_t)mc_count * sizeof(Operation));
        out->step_count = mc_count;
        return true;
    }
    if (!descriptor->operation) {
        return false;
    }
    out->steps[0] = descriptor->operation;
    out->step_count = 1;
    return true;
}

bool fa_jit_prepare_program_from_opcodes(const uint8_t* opcodes, size_t opcode_count, fa_JitProgram* program) {
    if (!opcodes || opcode_count == 0 || !program) {
        return false;
    }
    fa_jit_program_free(program);
    program->ops = (fa_JitPreparedOp*)calloc(opcode_count, sizeof(fa_JitPreparedOp));
    if (!program->ops) {
        return false;
    }
    program->capacity = opcode_count;
    for (size_t i = 0; i < opcode_count; ++i) {
        const fa_WasmOp* descriptor = fa_get_op(opcodes[i]);
        if (!descriptor || !fa_jit_prepare_op(descriptor, &program->ops[i])) {
            fa_jit_program_free(program);
            return false;
        }
        program->count++;
    }
    return true;
}

OP_RETURN_TYPE fa_jit_execute_prepared_op(const fa_JitPreparedOp* prepared, struct fa_Runtime* runtime, fa_Job* job) {
    if (!prepared || !prepared->descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (prepared->step_count == 0) {
        return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
    for (uint8_t i = 0; i < prepared->step_count; ++i) {
        Operation step = prepared->steps[i];
        if (!step) {
            return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
        }
        int status = step(runtime, job, prepared->descriptor);
        if (status != FA_RUNTIME_OK) {
            return status;
        }
    }
    return FA_RUNTIME_OK;
}

size_t fa_jit_program_estimate_bytes(const fa_JitProgram* program) {
    if (!program || !program->ops) {
        return 0;
    }
    return program->count * sizeof(fa_JitPreparedOp);
}
