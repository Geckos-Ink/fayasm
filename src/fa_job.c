#include "fa_runtime.h"

#include <stdlib.h>

fa_Job* fa_Job_init(){
    fa_Job* job = calloc(1, sizeof(fa_Job));
    if (!job) {
        return NULL;
    }
    job->regMaxPrecedes = FA_JOB_DATA_FLOW_WINDOW_SIZE;
    job->reg = NULL;
    job->instructionPointer = 0;
    job->id = 0;
    return job;
}

fa_JobDataFlow* fa_JobDataFlow_init(){
    fa_JobDataFlow* data = calloc(1, sizeof(fa_JobDataFlow));
    return data;
}

void fa_JobDataFlow_push(fa_JobDataFlow* data, fa_JobDataFlow* to){
    fa_JobDataFlow* cur = to;
    while(cur != NULL){
        if(cur->follows == NULL){
            cur->follows = data;
            break;
        }
        cur = cur->follows;
    }
}

fa_JobDataFlow* fa_JobDataFlow_pull(fa_JobDataFlow* from){
    fa_JobDataFlow* cur = from;
    while(cur->follows != NULL){
        cur = cur->follows;
    }
    return cur;
}
