
#pragma once

#include "fa_types.h"

#include <stddef.h>

#define FA_JOB_DATA_FLOW_WINDOW_SIZE 4 // resize in basis of strictly necessary
#define FA_JOB_DATA_FLOW_MAX_SIZE 8 // bytes
#define FA_JOB_STACK_MAX_DEPTH 256

typedef enum {
    fa_job_value_invalid = 0,
    fa_job_value_i32,
    fa_job_value_i64,
    fa_job_value_f32,
    fa_job_value_f64,
    fa_job_value_ref
} fa_JobValueKind;

typedef union {
    i32 i32_value;
    u32 u32_value;
    i64 i64_value;
    u64 u64_value;
    f32 f32_value;
    f64 f64_value;
    fa_ptr ref_value;
} fa_JobValuePayload;

typedef struct {
    fa_JobValueKind kind;
    bool is_signed;
    uint8_t bit_width;
    fa_JobValuePayload payload;
} fa_JobValue;

typedef struct {
    fa_JobValue values[FA_JOB_STACK_MAX_DEPTH];
    size_t top;
} fa_JobStack;

typedef struct fa_JobDataFlow;
typedef struct {
    ptr ptr;
    uint8_t size;  
    fa_JobDataFlow* follow;
    fa_JobDataFlow* precede; // max precedes: FA_JOB_DATA_FLOW_WINDOW_SIZE
} fa_JobDataFlow;

typedef uint32_t jobId_t;

typedef struct {
    jobId_t id;

    fa_JobStack stack;

    fa_ptr instructionPointer; // what instruction address is executing

    // pull and push registers    
    fa_JobDataFlow* reg;

} fa_Job;

void fa_JobStack_reset(fa_JobStack* stack);
bool fa_JobStack_push(fa_JobStack* stack, const fa_JobValue* value);
bool fa_JobStack_pop(fa_JobStack* stack, fa_JobValue* out);
const fa_JobValue* fa_JobStack_peek(const fa_JobStack* stack, size_t depth);

fa_Job* fa_Job_init();

fa_JobDataFlow* fa_JobDataFlow_init();
void fa_JobDataFlow_push(fa_JobDataFlow* data, fa_JobDataFlow* to);
fa_JobDataFlow* fa_JobDataFlow_pull(fa_JobDataFlow* from);
