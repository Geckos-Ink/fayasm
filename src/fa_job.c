#include "fa_runtime.h"

fa_JobDataFlow fa_JobDataFlow_data_push(fa_Job* job, ptr ptr, int size, fa_Malloc malloc, fa_Free free){
    fa_JobDataFlow* data = &job.dataFlowWindow[job.dataFlowOffset++ % FA_JOB_DATA_FLOW_WINDOW_SIZE];

    if(data->size != size){
        if(data->ptr != NULL){
            free(data->ptr);
        }

        data->ptr = malloc(size);
    }

    return data;
}

fa_JobDataFlow* fa_JobDataFlow_data_pull(fa_Job* job){
    return &job.dataFlowWindow[job.dataFlowOffset++ % FA_JOB_DATA_FLOW_WINDOW_SIZE];
}