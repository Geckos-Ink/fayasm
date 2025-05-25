#include "fa_runtime.h"

void data_push(fa_Job* job, ptr ptr, int size){
    fa_JobDataFlow* data = &job.dataFlowWindow[job.dataFlowOffset++ % FA_JOB_DATA_FLOW_WINDOW_SIZE];
}

fa_JobDataFlow* data_pull(fa_Job* job){
    return &job.dataFlowWindow[job.dataFlowOffset++ % FA_JOB_DATA_FLOW_WINDOW_SIZE];
}