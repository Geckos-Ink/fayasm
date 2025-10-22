
#pragma once

#define FA_JOB_DATA_FLOW_WINDOW_SIZE 4 // resize in basis of strictly necessary
#define FA_JOB_DATA_FLOW_MAX_SIZE 8 // bytes

typedef struct fa_JobDataFlow;
typedef struct {
    ptr ptr;
    uint8_t size;  
    fa_JobDataFlow* follows;
    fa_JobDataFlow* precede;
} fa_JobDataFlow;

typedef uint32_t jobId_t;

typedef struct {
    jobId_t id;

    //HERE: ADD (and define struct) SUPPORT TO STACKs

    fa_ptr instructionPointer; // what instruction address is executing

    // pull and push registers
    uint8_t regMaxPrecedes; // Default: 4
    fa_JobDataFlow* reg;

} fa_Job;

fa_Job* fa_Job_init();

fa_JobDataFlow* fa_JobDataFlow_init();
void fa_JobDataFlow_push(fa_JobDataFlow* data, fa_JobDataFlow* to);
fa_JobDataFlow* fa_JobDataFlow_pull(fa_JobDataFlow* from);