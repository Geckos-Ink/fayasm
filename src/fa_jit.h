#pragma once

#include "fa_ops.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef FA_JIT_MAX_STEPS_PER_OP
#define FA_JIT_MAX_STEPS_PER_OP 4
#endif

/* ------------------------------------------------------------------------- *
 * Runtime-wide spill/load persistence envelope.
 *
 * Spill blobs (JIT opcode programs, linear memory) all begin with a fixed
 * 16-byte little-endian header so a single, versioned format persists across
 * boots and architectures. Multi-byte fields are encoded explicitly in
 * little-endian byte order (no struct packing / host-endianness assumptions),
 * which is what makes the blobs portable on embedded targets. Raw function
 * pointers are never serialized: JIT programs persist as opcode streams and are
 * recompiled to microcode on load.
 *
 * Header layout:
 *   offset 0  u32  magic           (FA_SPILL_MAGIC)
 *   offset 4  u16  version         (FA_SPILL_VERSION)
 *   offset 6  u16  kind            (fa_SpillKind)
 *   offset 8  u64  payload_bytes   (bytes following the header)
 * ------------------------------------------------------------------------- */
#define FA_SPILL_MAGIC        0x4D535946u /* 'F''Y''S''M' little-endian */
#define FA_SPILL_VERSION      1u
#define FA_SPILL_HEADER_BYTES 16u

typedef enum {
    FA_SPILL_KIND_JIT_OPCODES = 1,
    FA_SPILL_KIND_MEMORY = 2
} fa_SpillKind;

/* Little-endian primitive accessors shared by every spill payload so the
   on-disk byte order is fixed regardless of host endianness. */
static inline void fa_spill_put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static inline void fa_spill_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static inline void fa_spill_put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
}
static inline uint16_t fa_spill_get_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t fa_spill_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t fa_spill_get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* Writes the shared spill envelope. Returns FA_SPILL_HEADER_BYTES on success
   (the number of bytes written) or 0 if the buffer is too small. */
size_t fa_spill_write_header(uint8_t* out, size_t capacity, uint16_t kind, uint64_t payload_bytes);

/* Validates magic/version and that payload_bytes fits within the buffer.
   Returns false on any mismatch; on success reports kind + payload size. */
bool fa_spill_read_header(const uint8_t* in,
                          size_t size,
                          uint16_t* kind_out,
                          uint64_t* payload_bytes_out);

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

/* Versioned spill serialization for a prepared program. The blob is the shared
   spill envelope (kind FA_SPILL_KIND_JIT_OPCODES) followed by the opcode stream,
   so it persists across boots and rebuilds microcode on load. */
size_t fa_jit_program_serialized_size(const fa_JitProgram* program);
bool fa_jit_program_serialize(const fa_JitProgram* program,
                              uint8_t* out,
                              size_t capacity,
                              size_t* written_out);
bool fa_jit_program_deserialize(const uint8_t* buffer, size_t size, fa_JitProgram* program_out);
OP_RETURN_TYPE fa_jit_execute_prepared_op(const fa_JitPreparedOp* prepared, struct fa_Runtime* runtime, fa_Job* job);
