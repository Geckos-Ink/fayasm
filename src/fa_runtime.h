#pragma once

#include "fa_types.h"

#define FA_JOB_DATA_FLOW_WINDOW_SIZE 4 // resize in basis of strictly necessary

typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_JobDataFlow

typedef uint32_t jobId_t;

typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_OpCode

typedef struct {
    ptr ptr;
    fa_ptr offset;
    uint8_t size;
} fa_Track

typedef struct {
    jobId_t id;

    // The data flow window is basically a FIFO pointers register of pointers in use
    fa_JobDataFlow dataFlowWindow[FA_JOB_DATA_FLOW_WINDOW_SIZE];
    uint8_t dataFlowOffset;

} fa_Job;