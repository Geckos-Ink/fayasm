#include "fa_job.h"
#include "fa_runtime.h"

#include <stdlib.h>

void fa_JobStack_reset(fa_JobStack* stack){
    if (!stack) {
        return;
    }
    stack->top = 0;
}

bool fa_JobStack_push(fa_JobStack* stack, const fa_JobValue* value){
    if (!stack || !value) {
        return false;
    }
    if (stack->top >= FA_JOB_STACK_MAX_DEPTH) {
        return false;
    }
    stack->values[stack->top] = *value;
    stack->top++;
    return true;
}

bool fa_JobStack_pop(fa_JobStack* stack, fa_JobValue* out){
    if (!stack || stack->top == 0) {
        return false;
    }
    stack->top--;
    if (out) {
        *out = stack->values[stack->top];
    }
    return true;
}

const fa_JobValue* fa_JobStack_peek(const fa_JobStack* stack, size_t depth){
    if (!stack || stack->top == 0) {
        return NULL;
    }
    if (depth >= stack->top) {
        return NULL;
    }
    return &stack->values[stack->top - depth - 1];
}

fa_Job* fa_Job_init(){
    fa_Job* job = calloc(1, sizeof(fa_Job));
    if (!job) {
        return NULL;
    }
    job->regMaxPrecedes = FA_JOB_DATA_FLOW_WINDOW_SIZE;
    job->reg = NULL;
    job->instructionPointer = 0;
    job->id = 0;
    fa_JobStack_reset(&job->stack);
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
