#pragma once

#include "fa_types.h"

#define FA_JOB_DATA_FLOW_WINDOW_SIZE 4 // resize in basis of strictly necessary

typedef uint32_t jobId_t;

typedef struct {
    jobId_t id;
    fa_ptr data_flow[FA_JOB_DATA_FLOW_WINDOW_SIZE];

} fa_Job;