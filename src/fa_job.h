
#pragma once

#define FA_JOB_DATA_FLOW_WINDOW_SIZE 4 // resize in basis of strictly necessary
#define FA_JOB_DATA_FLOW_MAX_SIZE 8 // bytes

typedef struct fa_JobDataFlow;
typedef struct {
    ptr ptr;
    uint8_t size;  
    fa_JobDataFlow* follows;
} fa_JobDataFlow;

typedef uint32_t jobId_t;

// unused
typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_OpCode;

// unused
typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_Track;

typedef struct {
    jobId_t id;

    fa_ptr instructionPointer; // what instruction address is executing

    // pull and push registers
    fa_JobDataFlow* regs;
    
} fa_Job;

fa_Job* fa_Job_init();

fa_JobDataFlow* fa_JobDataFlow_init();
void fa_JobDataFlow_push(fa_JobDataFlow* data, fa_JobDataFlow* to);
fa_JobDataFlow* fa_JobDataFlow_pull(fa_JobDataFlow* from);