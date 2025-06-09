#include "fa_runtime.h"

fa_Job* fa_Job_init(){
    fa_Job* job = malloc(sizeof(fa_Job));
    //todo: init vars
    return job;
}

fa_JobDataFlow* fa_JobDataFlow_data_push(fa_Job* job, int size){
    uint8_t offset = job.dataFlowOffset++ % FA_JOB_DATA_FLOW_WINDOW_SIZE;
    fa_JobDataFlow* data = &job.dataFlowWindow[offset];

    data.size = size;
    data.offset = offset;
    data.ptr = &job->dataWindows + (offset * FA_JOB_DATA_FLOW_MAX_SIZE);

    return data;
}

fa_JobDataFlow* fa_JobDataFlow_data_pull(fa_Job* job){
    return &job.dataFlowWindow[job.dataFlowOffset++ % FA_JOB_DATA_FLOW_WINDOW_SIZE];
}