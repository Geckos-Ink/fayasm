
#pragma once

#define FA_JOB_DATA_FLOW_WINDOW_SIZE 4 // resize in basis of strictly necessary

typedef struct {
    ptr ptr;
    uint8_t size;
    //fa_ptr offset; // y?   
} fa_JobDataFlow;

typedef uint32_t jobId_t;

typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_OpCode;

typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_Track;

typedef struct {
    jobId_t id;

    // The data flow window is basically a FIFO pointers register of pointers in use
    fa_JobDataFlow dataFlowWindow[FA_JOB_DATA_FLOW_WINDOW_SIZE];
    uint8_t dataFlowOffset;

    fa_ptr instructionPointer; // what instruction address is executing

} fa_Job;

//todo: Determine a more clear mechanism about data flows
// currently a dynamic pointer is done every time, making optimization useless
fa_JobDataFlow fa_JobDataFlow_data_push(fa_Job *job, ptr ptr, int size, fa_Malloc malloc, fa_Free free);
fa_JobDataFlow* fa_JobDataFlow_data_pull(fa_Job *job);