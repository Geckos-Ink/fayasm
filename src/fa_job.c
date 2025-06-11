#include "fa_runtime.h"

fa_Job* fa_Job_init(){
    fa_Job* job = malloc(sizeof(fa_Job));
    //todo: init vars
    return job;
}

fa_JobDataFlow* fa_JobDataFlow_init(){
    fa_JobDataFlow* data = malloc(sizeof(fa_JobDataFlow));
    data->ptr = NULL;
    data->size = 0;
    data->follows = NULL; // .. just use calloc
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