#include "fa_job.h"
#include "fa_runtime.h"

#include <stdlib.h>

static void fa_JobStack_clear_nodes(fa_JobStack* stack){
    if (!stack) {
        return;
    }
    fa_JobStackValue* cur = stack->head;
    while (cur) {
        fa_JobStackValue* next = cur->next;
        free(cur);
        cur = next;
    }
    stack->head = NULL;
    stack->tail = NULL;
    stack->size = 0;
}

void fa_JobStack_reset(fa_JobStack* stack){
    fa_JobStack_clear_nodes(stack);
}

bool fa_JobStack_push(fa_JobStack* stack, const fa_JobValue* value){
    if (!stack || !value) {
        return false;
    }
    fa_JobStackValue* node = malloc(sizeof(fa_JobStackValue));
    if (!node) {
        return false;
    }
    node->value = *value;
    node->next = NULL;
    node->prev = stack->tail;
    if (stack->tail) {
        stack->tail->next = node;
    } else {
        stack->head = node;
    }
    stack->tail = node;
    stack->size++;
    return true;
}

bool fa_JobStack_pop(fa_JobStack* stack, fa_JobValue* out){
    if (!stack || !stack->tail) {
        return false;
    }
    fa_JobStackValue* node = stack->tail;
    if (out) {
        *out = node->value;
    }
    stack->tail = node->prev;
    if (stack->tail) {
        stack->tail->next = NULL;
    } else {
        stack->head = NULL;
    }
    free(node);
    if (stack->size > 0) {
        stack->size--;
    }
    return true;
}

const fa_JobValue* fa_JobStack_peek(const fa_JobStack* stack, size_t depth){
    if (!stack || !stack->tail) {
        return NULL;
    }
    const fa_JobStackValue* node = stack->tail;
    while (node && depth > 0) {
        node = node->prev;
        depth--;
    }
    if (!node) {
        return NULL;
    }
    return &node->value;
}

void fa_JobStack_free(fa_JobStack* stack){
    fa_JobStack_clear_nodes(stack);
}

fa_Job* fa_Job_init(){
    fa_Job* job = calloc(1, sizeof(fa_Job));
    if (!job) {
        return NULL;
    }
    
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
