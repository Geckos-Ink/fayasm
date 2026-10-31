
#pragma once

#include "fa_types.h"

#include <stddef.h>

/*
 * The immediate window is deliberately larger than the widest currently
 * decoded instruction context (multi-memory SIMD lane operations need five
 * entries). It is fixed-size: decode fails instead of allocating or silently
 * discarding an older immediate.
 */
#ifndef FA_JOB_DATA_FLOW_WINDOW_SIZE
#define FA_JOB_DATA_FLOW_WINDOW_SIZE 8U
#endif

#define FA_JOB_DATA_FLOW_MAX_SIZE 16U /* v128 immediate width, in bytes */

/*
 * By default keep the newest operand values in the job itself. Older values
 * spill into a contiguous, geometrically grown array; fixed slots below use
 * the opposite placement. The depth cap is a
 * safety boundary for malformed/unvalidated inputs and can be lowered for an
 * embedded build without changing runtime code.
 */
#ifndef FA_JOB_REGISTER_WINDOW_SIZE
#define FA_JOB_REGISTER_WINDOW_SIZE 8U
#endif

/* Fixed virtual registers R0..R(N-1) hold the first N operands. Values above
 * them use the retained spill array; no rotation or refill is needed. This
 * portable C99 backend does not reserve physical CPU registers. */
#ifndef FA_JOB_FIXED_REGISTERS
#define FA_JOB_FIXED_REGISTERS 0
#endif

#if FA_JOB_FIXED_REGISTERS != 0 && FA_JOB_FIXED_REGISTERS != 1
#error "FA_JOB_FIXED_REGISTERS must be 0 or 1"
#endif

#ifndef FA_JOB_STACK_MAX_DEPTH
#define FA_JOB_STACK_MAX_DEPTH 65536U
#endif

#if FA_JOB_DATA_FLOW_WINDOW_SIZE < 5U || FA_JOB_DATA_FLOW_WINDOW_SIZE > 255U
#error "FA_JOB_DATA_FLOW_WINDOW_SIZE must hold the widest decoded instruction context (5..255)"
#endif

#if FA_JOB_REGISTER_WINDOW_SIZE < 1U || FA_JOB_REGISTER_WINDOW_SIZE > 255U
#error "FA_JOB_REGISTER_WINDOW_SIZE must be in the range 1..255"
#endif

#if FA_JOB_STACK_MAX_DEPTH < FA_JOB_REGISTER_WINDOW_SIZE
#error "FA_JOB_STACK_MAX_DEPTH must be at least FA_JOB_REGISTER_WINDOW_SIZE"
#endif

typedef enum {
    fa_job_value_invalid = 0,
    fa_job_value_i32,
    fa_job_value_i64,
    fa_job_value_f32,
    fa_job_value_f64,
    fa_job_value_v128,
    fa_job_value_ref
} fa_JobValueKind;

typedef struct {
    u64 low;
    u64 high;
} fa_V128;

typedef union {
    i32 i32_value;
    u32 u32_value;
    i64 i64_value;
    u64 u64_value;
    f32 f32_value;
    f64 f64_value;
    fa_V128 v128_value;
    fa_ptr ref_value;
} fa_JobValuePayload;

typedef struct {
    fa_JobValueKind kind;
    bool is_signed;
    uint8_t bit_width;
    fa_JobValuePayload payload;
} fa_JobValue;

typedef struct {
    /* Window backend: oldest operands. Fixed backend: operands above R(N-1).
     * Both are in stack order and retain capacity across resets. */
    fa_JobValue* spill_values;
    size_t spill_count;
    size_t spill_capacity;

    /* Circular window, or fixed slots indexed by absolute operand depth.
     * window_start stays zero in fixed mode. Use helpers for either backend. */
    fa_JobValue window[FA_JOB_REGISTER_WINDOW_SIZE];
    uint8_t window_start;
    uint8_t window_count;

    /* Publicly observed by runtime control-frame accounting. */
    size_t size;
} fa_JobStack;

typedef struct fa_JobDataFlow {
    ptr ptr;
    uint8_t size;
    struct fa_JobDataFlow* follows;
    struct fa_JobDataFlow* precede;
    uint8_t storage[FA_JOB_DATA_FLOW_MAX_SIZE];
} fa_JobDataFlow;

typedef uint32_t jobId_t;

typedef struct {
    jobId_t id;

    fa_JobStack stack;

    fa_ptr instructionPointer; // what instruction address is executing

    /* Allocation-free decoded-immediate forwarding window. */
    fa_JobDataFlow reg_window[FA_JOB_DATA_FLOW_WINDOW_SIZE];
    uint8_t reg_count;
    fa_JobDataFlow* reg;

} fa_Job;

void fa_JobStack_reset(fa_JobStack* stack);
bool fa_JobStack_push(fa_JobStack* stack, const fa_JobValue* value);
bool fa_JobStack_pop(fa_JobStack* stack, fa_JobValue* out);
const fa_JobValue* fa_JobStack_peek(const fa_JobStack* stack, size_t depth);
void fa_JobStack_free(fa_JobStack* stack);

void fa_JobRegWindow_reset(fa_Job* job);
bool fa_JobRegWindow_push(fa_Job* job, const void* value, size_t size);
bool fa_JobRegWindow_pop(fa_Job* job, void* value_out, size_t size);

fa_Job* fa_Job_init(void);

fa_JobDataFlow* fa_JobDataFlow_init(void);
void fa_JobDataFlow_push(fa_JobDataFlow* data, fa_JobDataFlow* to);
fa_JobDataFlow* fa_JobDataFlow_pull(fa_JobDataFlow* from);
