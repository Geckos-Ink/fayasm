#include "fa_job.h"
#include "fa_runtime.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool fa_JobStack_reserve_spill(fa_JobStack* stack, size_t needed){
    if (!stack || needed > (size_t)FA_JOB_STACK_MAX_DEPTH) {
        return false;
    }
    if (needed <= stack->spill_capacity) {
        return true;
    }

    const size_t max_spill = (size_t)FA_JOB_STACK_MAX_DEPTH - (size_t)FA_JOB_REGISTER_WINDOW_SIZE;
    size_t capacity = stack->spill_capacity ? stack->spill_capacity : 16U;
    if (capacity > max_spill) {
        capacity = max_spill;
    }
    while (capacity < needed) {
        if (capacity > max_spill / 2U) {
            capacity = max_spill;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < needed || capacity > SIZE_MAX / sizeof(fa_JobValue)) {
        return false;
    }
    fa_JobValue* values = (fa_JobValue*)realloc(stack->spill_values,
                                                 capacity * sizeof(fa_JobValue));
    if (!values) {
        return false;
    }
    stack->spill_values = values;
    stack->spill_capacity = capacity;
    return true;
}

void fa_JobStack_reset(fa_JobStack* stack){
    if (!stack) {
        return;
    }
    stack->spill_count = 0;
    stack->window_start = 0;
    stack->window_count = 0;
    stack->size = 0;
}

bool fa_JobStack_push(fa_JobStack* stack, const fa_JobValue* value){
    if (!stack || !value) {
        return false;
    }
    if (stack->size >= (size_t)FA_JOB_STACK_MAX_DEPTH) {
        return false;
    }

#if FA_JOB_FIXED_REGISTERS
    if (stack->size < FA_JOB_REGISTER_WINDOW_SIZE) {
        stack->window[stack->size] = *value;
        stack->window_count++;
    } else {
        if (!fa_JobStack_reserve_spill(stack, stack->spill_count + 1U)) {
            return false;
        }
        stack->spill_values[stack->spill_count++] = *value;
    }
    stack->size++;
    return true;
#else
    if (stack->window_count < FA_JOB_REGISTER_WINDOW_SIZE) {
        const size_t index = ((size_t)stack->window_start + stack->window_count) %
                             (size_t)FA_JOB_REGISTER_WINDOW_SIZE;
        stack->window[index] = *value;
        stack->window_count++;
        stack->size++;
        return true;
    }

    if (!fa_JobStack_reserve_spill(stack, stack->spill_count + 1U)) {
        return false;
    }
    stack->spill_values[stack->spill_count++] = stack->window[stack->window_start];
    stack->window[stack->window_start] = *value;
    stack->window_start = (uint8_t)(((size_t)stack->window_start + 1U) %
                                    (size_t)FA_JOB_REGISTER_WINDOW_SIZE);
    stack->size++;
    return true;
#endif
}

bool fa_JobStack_pop(fa_JobStack* stack, fa_JobValue* out){
    if (!stack || stack->size == 0 || stack->window_count == 0) {
        return false;
    }
#if FA_JOB_FIXED_REGISTERS
    if (stack->spill_count > 0) {
        if (out) {
            *out = stack->spill_values[stack->spill_count - 1U];
        }
        stack->spill_count--;
    } else {
        if (out) {
            *out = stack->window[stack->window_count - 1U];
        }
        stack->window_count--;
    }
    stack->size--;
    return true;
#else
    const size_t newest = ((size_t)stack->window_start + stack->window_count - 1U) %
                          (size_t)FA_JOB_REGISTER_WINDOW_SIZE;
    if (out) {
        *out = stack->window[newest];
    }

    if (stack->spill_count > 0) {
        stack->window_start = (uint8_t)(((size_t)stack->window_start +
                                         (size_t)FA_JOB_REGISTER_WINDOW_SIZE - 1U) %
                                        (size_t)FA_JOB_REGISTER_WINDOW_SIZE);
        stack->window[stack->window_start] = stack->spill_values[--stack->spill_count];
    } else {
        stack->window_count--;
        if (stack->window_count == 0) {
            stack->window_start = 0;
        }
    }
    stack->size--;
    return true;
#endif
}

const fa_JobValue* fa_JobStack_peek(const fa_JobStack* stack, size_t depth){
    if (!stack || depth >= stack->size) {
        return NULL;
    }

#if FA_JOB_FIXED_REGISTERS
    const size_t index = stack->size - 1U - depth;
    return index < FA_JOB_REGISTER_WINDOW_SIZE ? &stack->window[index] :
        &stack->spill_values[index - FA_JOB_REGISTER_WINDOW_SIZE];
#else
    if (depth < stack->window_count) {
        const size_t logical = (size_t)stack->window_count - 1U - depth;
        const size_t index = ((size_t)stack->window_start + logical) %
                             (size_t)FA_JOB_REGISTER_WINDOW_SIZE;
        return &stack->window[index];
    }

    const size_t spill_depth = depth - stack->window_count;
    return &stack->spill_values[stack->spill_count - 1U - spill_depth];
#endif
}

void fa_JobStack_free(fa_JobStack* stack){
    if (!stack) {
        return;
    }
    free(stack->spill_values);
    memset(stack, 0, sizeof(*stack));
}

void fa_JobRegWindow_reset(fa_Job* job){
    if (!job) {
        return;
    }
    for (size_t i = 0; i < FA_JOB_DATA_FLOW_WINDOW_SIZE; ++i) {
        fa_JobDataFlow* slot = &job->reg_window[i];
        slot->ptr = slot->storage;
        slot->size = 0;
        slot->follows = NULL;
        slot->precede = NULL;
    }
    job->reg_count = 0;
    job->reg = NULL;
}

bool fa_JobRegWindow_push(fa_Job* job, const void* value, size_t size){
    if (!job || !value || size == 0 || size > FA_JOB_DATA_FLOW_MAX_SIZE ||
        job->reg_count >= FA_JOB_DATA_FLOW_WINDOW_SIZE) {
        return false;
    }
    fa_JobDataFlow* slot = &job->reg_window[job->reg_count];
    slot->ptr = slot->storage;
    slot->size = (uint8_t)size;
    memcpy(slot->storage, value, size);
    if (size < FA_JOB_DATA_FLOW_MAX_SIZE) {
        memset(slot->storage + size, 0, FA_JOB_DATA_FLOW_MAX_SIZE - size);
    }
    slot->precede = job->reg;
    slot->follows = NULL;
    if (job->reg) {
        job->reg->follows = slot;
    }
    job->reg = slot;
    job->reg_count++;
    return true;
}

bool fa_JobRegWindow_pop(fa_Job* job, void* value_out, size_t size){
    if (!job || !value_out || size == 0 || job->reg_count == 0 || !job->reg) {
        return false;
    }
    fa_JobDataFlow* slot = job->reg;
    size_t copy = slot->size;
    if (copy > size) {
        copy = size;
    }
    memcpy(value_out, slot->storage, copy);
    if (copy < size) {
        memset((uint8_t*)value_out + copy, 0, size - copy);
    }

    job->reg = slot->precede;
    job->reg_count--;
    if (job->reg) {
        job->reg->follows = NULL;
    }
    slot->size = 0;
    slot->follows = NULL;
    slot->precede = NULL;
    return true;
}

fa_Job* fa_Job_init(void){
    fa_Job* job = calloc(1, sizeof(fa_Job));
    if (!job) {
        return NULL;
    }
    
    job->instructionPointer = 0;
    job->id = 0;
    fa_JobStack_reset(&job->stack);
    fa_JobRegWindow_reset(job);
    return job;
}

fa_JobDataFlow* fa_JobDataFlow_init(void){
    fa_JobDataFlow* data = calloc(1, sizeof(fa_JobDataFlow));
    if (data) {
        data->ptr = data->storage;
    }
    return data;
}

void fa_JobDataFlow_push(fa_JobDataFlow* data, fa_JobDataFlow* to){
    if (!data || !to) {
        return;
    }
    fa_JobDataFlow* cur = to;
    while(cur != NULL){
        if(cur->follows == NULL){
            cur->follows = data;
            data->precede = cur;
            break;
        }
        cur = cur->follows;
    }
}

fa_JobDataFlow* fa_JobDataFlow_pull(fa_JobDataFlow* from){
    if (!from) {
        return NULL;
    }
    fa_JobDataFlow* cur = from;
    while(cur->follows != NULL){
        cur = cur->follows;
    }
    return cur;
}
