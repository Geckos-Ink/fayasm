#include "fa_ops.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OP_KEYWORD

static fa_WasmOp g_ops[256];
static bool g_ops_initialized = false;

static size_t op_value_width(const fa_WasmOp* op) {
    if (!op) {
        return 0;
    }
    if (op->size_arg != 0) {
        size_t bytes = op->size_arg / 8;
        if (bytes == 0 && (op->size_arg % 8) != 0) {
            bytes = (op->size_arg + 7) / 8;
        }
        if (bytes > 0) {
            if (bytes > FA_JOB_DATA_FLOW_MAX_SIZE) {
                bytes = FA_JOB_DATA_FLOW_MAX_SIZE;
            }
            return bytes;
        }
    }
    if (op->type.size != 0) {
        size_t bytes = op->type.size;
        if (bytes > FA_JOB_DATA_FLOW_MAX_SIZE) {
            bytes = FA_JOB_DATA_FLOW_MAX_SIZE;
        }
        return bytes;
    }
    return sizeof(fa_ptr);
}

static fa_JobDataFlow* job_node_from_value(const void* data, size_t size) {
    if (!data || size == 0 || size > FA_JOB_DATA_FLOW_MAX_SIZE) {
        return NULL;
    }
    fa_JobDataFlow* node = calloc(1, sizeof(fa_JobDataFlow) + size);
    if (!node) {
        return NULL;
    }
    node->size = (uint8_t)size;
    node->ptr = (void*)((uint8_t*)(node + 1));
    memcpy(node->ptr, data, size);
    return node;
}

static void job_node_destroy(fa_JobDataFlow* node) {
    if (!node) {
        return;
    }
    free(node);
}

static uint8_t job_reg_count_and_head(const fa_Job* job, fa_JobDataFlow** head_out) {
    uint8_t count = 0;
    fa_JobDataFlow* head = NULL;
    for (fa_JobDataFlow* cur = job ? job->reg : NULL; cur; cur = cur->precede) {
        head = cur;
        ++count;
    }
    if (head_out) {
        *head_out = head;
    }
    return count;
}

static void job_reg_enforce_limit(fa_Job* job) {
    if (!job) {
        return;
    }
    if (job->regMaxPrecedes == 0) {
        job->regMaxPrecedes = FA_JOB_DATA_FLOW_WINDOW_SIZE;
    }
    fa_JobDataFlow* head = NULL;
    uint8_t count = job_reg_count_and_head(job, &head);
    while (count > job->regMaxPrecedes && head) {
        fa_JobDataFlow* next = head->follows;
        if (next) {
            next->precede = NULL;
        } else {
            job->reg = NULL;
        }
        job_node_destroy(head);
        head = next;
        --count;
    }
}

static void job_reg_push_node(fa_Job* job, fa_JobDataFlow* node) {
    if (!job || !node) {
        return;
    }
    node->follows = NULL;
    if (!job->reg) {
        node->precede = NULL;
        job->reg = node;
    } else {
        node->precede = job->reg;
        job->reg->follows = node;
        job->reg = node;
    }
    job_reg_enforce_limit(job);
}

static fa_JobDataFlow* job_reg_pop_tail(fa_Job* job) {
    if (!job) {
        return NULL;
    }
    fa_JobDataFlow* tail = job->reg;
    if (!tail) {
        return NULL;
    }
    fa_JobDataFlow* new_tail = tail->precede;
    if (new_tail) {
        new_tail->follows = NULL;
    }
    job->reg = new_tail;
    tail->precede = NULL;
    tail->follows = NULL;
    return tail;
}

static bool job_reg_push_value(fa_Job* job, const void* data, size_t size) {
    fa_JobDataFlow* node = job_node_from_value(data, size);
    if (!node) {
        return false;
    }
    job_reg_push_node(job, node);
    return true;
}

OP_KEYWORD OP_RETURN_TYPE op_i64_load(OP_ARGUMENTS) {
    if (!job || !descriptor) {
        return;
    }

    size_t payload_size = op_value_width(descriptor);
    if (payload_size == 0 || payload_size > sizeof(i64)) {
        payload_size = sizeof(i64);
    }

    fa_JobDataFlow* address_node = job_reg_pop_tail(job);
    if (!address_node) {
        return;
    }

    bool success = false;
    i64 value = 0;

    if (address_node->ptr != NULL) {
        if (address_node->size == sizeof(fa_ptr) || address_node->size == sizeof(uint32_t)) {
            fa_ptr addr = 0;
            memcpy(&addr, address_node->ptr, address_node->size);
            if (addr != 0) {
                const uint8_t* memory = (const uint8_t*)(uintptr_t)addr;
                memcpy(&value, memory, payload_size);
                success = true;
            }
        } else if (address_node->size >= payload_size) {
            memcpy(&value, address_node->ptr, payload_size);
            success = true;
        }
    }

    if (!success) {
        job_reg_push_node(job, address_node);
        return;
    }

    job_node_destroy(address_node);

    uint8_t meaningful_bits = (uint8_t)(payload_size * 8U);
    if (meaningful_bits < 64U && meaningful_bits > 0U) {
        if (descriptor->type.is_signed) {
            i64 sign_mask = (i64)1 << (meaningful_bits - 1U);
            const bool negative = (value & sign_mask) != 0;
            if (negative) {
                i64 extension_mask = ~(((i64)1 << meaningful_bits) - 1);
                value |= extension_mask;
            } else {
                i64 mask = ((i64)1 << meaningful_bits) - 1;
                value &= mask;
            }
        } else {
            i64 mask = ((i64)1 << meaningful_bits) - 1;
            value &= mask;
        }
    }

    job_reg_push_value(job, &value, sizeof(value));
}

static void init_ops_once(void) {
    if (g_ops_initialized) {
        return;
    }

    memset(g_ops, 0, sizeof(g_ops));

    const fa_WasmType void_type = {wt_void, 0, false};
    const fa_WasmType i64_type = {wt_integer, 8, true};

    g_ops[0x00] = (fa_WasmOp){
        .id = 0x00,
        .type = void_type,
        .op = wopt_unique,
        .num_pull = 0,
        .num_push = 0,
        .num_args = 0,
    };

    g_ops[0x01] = (fa_WasmOp){
        .id = 0x01,
        .type = void_type,
        .op = wopt_unique,
        .num_pull = 0,
        .num_push = 0,
        .num_args = 0,
    };

    g_ops[0x0B] = (fa_WasmOp){
        .id = 0x0B,
        .type = void_type,
        .op = wopt_return,
        .num_pull = 0,
        .num_push = 0,
        .num_args = 0,
    };

    g_ops[0x29] = (fa_WasmOp){
        .id = 0x29,
        .type = i64_type,
        .op = wopt_load,
        .size_arg = 64,
        .num_pull = 1,
        .num_push = 1,
        .num_args = 2,
        .operation = op_i64_load,
    };

    g_ops_initialized = true;
}

const fa_WasmOp* fa_instance_ops(void) {
    init_ops_once();
    return g_ops;
}

const fa_WasmOp* fa_get_op(uint8_t opcode) {
    init_ops_once();
    return &g_ops[opcode];
}

OP_RETURN_TYPE fa_execute_op(uint8_t opcode, fa_Job* job) {
    const fa_WasmOp* op = fa_get_op(opcode);
    if (!op || !op->operation) {
        return;
    }
    op->operation(job, op);
}
