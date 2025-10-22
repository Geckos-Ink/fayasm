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

    fa_JobDataFlow* head = NULL;
    uint8_t count = job_reg_count_and_head(job, &head);
    while (count > FA_JOB_DATA_FLOW_WINDOW_SIZE && head) {
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

///
///
///


void fa_ops_defs_populate(fa_WasmOp* ops) {
    if (!ops) {
        return;
    }
    memset(ops, 0, sizeof(fa_WasmOp) * 256);
    fa_WasmType i32_type = {wt_integer, 4, true};
    fa_WasmType i64_type = {wt_integer, 8, true};
    fa_WasmType u32_type = {wt_unsigned_integer, 4, false};
    fa_WasmType u64_type = {wt_unsigned_integer, 8, false};
    fa_WasmType f32_type = {wt_float, 4, false};
    fa_WasmType f64_type = {wt_float, 8, false};
    fa_WasmType void_type = {wt_void, 0, false};

    // Control flow operators
    // 0x00: unreachable
    ops[0].id = 0;
    ops[0].type = void_type;
    ops[0].op = wopt_unique;
    ops[0].num_pull = 0;
    ops[0].num_push = 0;
    ops[0].num_args = 0;
    //todo: implement ops[n].operation = ...

    // 0x01: nop
    ops[1].id = 1;
    ops[1].type = void_type;
    ops[1].op = wopt_unique;
    ops[1].num_pull = 0;
    ops[1].num_push = 0;
    ops[1].num_args = 0;

    // 0x02: block
    ops[2].id = 2;
    ops[2].type = void_type;
    ops[2].op = wopt_unique;
    ops[2].num_pull = 0;
    ops[2].num_push = 0;
    ops[2].num_args = 1;  // block type

    // 0x03: loop
    ops[3].id = 3;
    ops[3].type = void_type;
    ops[3].num_pull = 0;
    ops[3].num_push = 0;
    ops[3].num_args = 1;  // loop type

    // 0x04: if
    ops[4].id = 4;
    ops[4].type = void_type;
    ops[4].num_pull = 1;  // condition
    ops[4].num_push = 0;
    ops[4].num_args = 1;  // block type

    // 0x05: else
    ops[5].id = 5;
    ops[5].type = void_type;
    ops[5].num_pull = 0;
    ops[5].num_push = 0;
    ops[5].num_args = 0;

    // 0x0B: end
    ops[11].id = 11;
    ops[11].type = void_type;
    ops[11].num_pull = 0;
    ops[11].num_push = 0;
    ops[11].num_args = 0;

    // 0x0C: br
    ops[12].id = 12;
    ops[12].type = void_type;
    ops[12].num_pull = 0;
    ops[12].num_push = 0;
    ops[12].num_args = 1;  // label index

    // 0x0D: br_if
    ops[13].id = 13;
    ops[13].type = void_type;
    ops[13].num_pull = 1;  // condition
    ops[13].num_push = 0;
    ops[13].num_args = 1;  // label index

    // 0x0E: br_table
    ops[14].id = 14;
    ops[14].type = void_type;
    ops[14].num_pull = 1;  // index
    ops[14].num_push = 0;
    ops[14].num_args = 2;  // vector of label indices + default label

    // 0x0F: return
    ops[15].id = 15;
    ops[15].type = void_type;
    ops[15].num_pull = 0;
    ops[15].num_push = 0;
    ops[15].num_args = 0;

    // 0x10: call
    ops[16].id = 16;
    ops[16].type = void_type;
    ops[16].num_pull = 0;  // Depends on function signature
    ops[16].num_push = 0;  // Depends on function signature
    ops[16].num_args = 1;  // function index

    // 0x11: call_indirect
    ops[17].id = 17;
    ops[17].type = void_type;
    ops[17].num_pull = 1;  // table index + arguments
    ops[17].num_push = 0;  // Depends on function signature
    ops[17].num_args = 2;  // type index + reserved byte

    // Memory operators
    // 0x28: i32.load
    ops[40].id = 40;
    ops[40].type = i32_type;
    ops[40].op = wopt_load;
    ops[40].size_arg = 32;
    ops[40].num_pull = 1;  // address
    ops[40].num_push = 1;  // result
    ops[40].num_args = 2;  // align and offset

    // 0x29: i64.load
    ops[41].id = 41;
    ops[41].type = i64_type;
    ops[41].size_arg = 64;
    ops[41].num_pull = 1;  // address
    ops[41].num_push = 1;  // result
    ops[41].num_args = 2;  // align and offset

    // 0x2A: f32.load
    ops[42].id = 42;
    ops[42].type = f32_type;
    ops[42].size_arg = 32;
    ops[42].num_pull = 1;  // address
    ops[42].num_push = 1;  // result
    ops[42].num_args = 2;  // align and offset

    // 0x2B: f64.load
    ops[43].id = 43;
    ops[43].type = f64_type;
    ops[43].size_arg = 64;
    ops[43].num_pull = 1;  // address
    ops[43].num_push = 1;  // result
    ops[43].num_args = 2;  // align and offset

    // 0x2C: i32.load8_s
    ops[44].id = 44;
    ops[44].type = i32_type;
    ops[44].size_arg = 8;
    ops[44].num_pull = 1;  // address
    ops[44].num_push = 1;  // result
    ops[44].num_args = 2;  // align and offset

    // 0x2D: i32.load8_u
    ops[45].id = 45;
    ops[45].type = u32_type;
    ops[45].size_arg = 8;
    ops[45].num_pull = 1;  // address
    ops[45].num_push = 1;  // result
    ops[45].num_args = 2;  // align and offset

    // 0x2E: i32.load16_s
    ops[46].id = 46;
    ops[46].type = i32_type;
    ops[46].size_arg = 16;
    ops[46].num_pull = 1;  // address
    ops[46].num_push = 1;  // result
    ops[46].num_args = 2;  // align and offset

    // 0x2F: i32.load16_u
    ops[47].id = 47;
    ops[47].type = u32_type;
    ops[47].size_arg = 16;
    ops[47].num_pull = 1;  // address
    ops[47].num_push = 1;  // result
    ops[47].num_args = 2;  // align and offset

    // 0x30: i64.load8_s
    ops[48].id = 48;
    ops[48].type = i64_type;
    ops[48].size_arg = 8;
    ops[48].num_pull = 1;  // address
    ops[48].num_push = 1;  // result
    ops[48].num_args = 2;  // align and offset

    // 0x31: i64.load8_u
    ops[49].id = 49;
    ops[49].type = u64_type;
    ops[49].size_arg = 8;
    ops[49].num_pull = 1;  // address
    ops[49].num_push = 1;  // result
    ops[49].num_args = 2;  // align and offset

    // 0x32: i64.load16_s
    ops[50].id = 50;
    ops[50].type = i64_type;
    ops[50].size_arg = 16;
    ops[50].num_pull = 1;  // address
    ops[50].num_push = 1;  // result
    ops[50].num_args = 2;  // align and offset

    // 0x33: i64.load16_u
    ops[51].id = 51;
    ops[51].type = u64_type;
    ops[51].size_arg = 16;
    ops[51].num_pull = 1;  // address
    ops[51].num_push = 1;  // result
    ops[51].num_args = 2;  // align and offset

    // 0x34: i64.load32_s
    ops[52].id = 52;
    ops[52].type = i64_type;
    ops[52].size_arg = 32;
    ops[52].num_pull = 1;  // address
    ops[52].num_push = 1;  // result
    ops[52].num_args = 2;  // align and offset

    // 0x35: i64.load32_u
    ops[53].id = 53;
    ops[53].type = u64_type;
    ops[53].size_arg = 32;
    ops[53].num_pull = 1;  // address
    ops[53].num_push = 1;  // result
    ops[53].num_args = 2;  // align and offset

    // 0x36: i32.store
    ops[54].id = 54;
    ops[54].type = i32_type;
    ops[54].op = wopt_store;
    ops[54].size_arg = 32;
    ops[54].num_pull = 2;  // address and value
    ops[54].num_push = 0;
    ops[54].num_args = 2;  // align and offset

    // 0x37: i64.store
    ops[55].id = 55;
    ops[55].type = i64_type;
    ops[55].size_arg = 64;
    ops[55].num_pull = 2;  // address and value
    ops[55].num_push = 0;
    ops[55].num_args = 2;  // align and offset

    // 0x38: f32.store
    ops[56].id = 56;
    ops[56].type = f32_type;
    ops[56].size_arg = 32;
    ops[56].num_pull = 2;  // address and value
    ops[56].num_push = 0;
    ops[56].num_args = 2;  // align and offset

    // 0x39: f64.store
    ops[57].id = 57;
    ops[57].type = f64_type;
    ops[57].size_arg = 64;
    ops[57].num_pull = 2;  // address and value
    ops[57].num_push = 0;
    ops[57].num_args = 2;  // align and offset

    // 0x3A: i32.store8
    ops[58].id = 58;
    ops[58].type = i32_type;
    ops[58].size_arg = 8;
    ops[58].num_pull = 2;  // address and value
    ops[58].num_push = 0;
    ops[58].num_args = 2;  // align and offset

    // 0x3B: i32.store16
    ops[59].id = 59;
    ops[59].type = i32_type;
    ops[59].size_arg = 16;
    ops[59].num_pull = 2;  // address and value
    ops[59].num_push = 0;
    ops[59].num_args = 2;  // align and offset

    // 0x3C: i64.store8
    ops[60].id = 60;
    ops[60].type = i64_type;
    ops[60].size_arg = 8;
    ops[60].num_pull = 2;  // address and value
    ops[60].num_push = 0;
    ops[60].num_args = 2;  // align and offset

    // 0x3D: i64.store16
    ops[61].id = 61;
    ops[61].type = i64_type;
    ops[61].size_arg = 16;
    ops[61].num_pull = 2;  // address and value
    ops[61].num_push = 0;
    ops[61].num_args = 2;  // align and offset

    // 0x3E: i64.store32
    ops[62].id = 62;
    ops[62].type = i64_type;
    ops[62].size_arg = 32;
    ops[62].num_pull = 2;  // address and value
    ops[62].num_push = 0;
    ops[62].num_args = 2;  // align and offset

    // 0x3F: memory.size
    ops[63].id = 63;
    ops[63].type = i32_type;
    ops[63].num_pull = 0;
    ops[63].num_push = 1;  // size in pages
    ops[63].num_args = 1;  // reserved byte

    // 0x40: memory.grow
    ops[64].id = 64;
    ops[64].type = i32_type;
    ops[64].num_pull = 1;  // delta
    ops[64].num_push = 1;  // previous size
    ops[64].num_args = 1;  // reserved byte

    // Numeric operators
    // 0x41: i32.const
    ops[65].id = 65;
    ops[65].type = i32_type;
    ops[65].op = wopt_const;
    ops[65].num_pull = 0;
    ops[65].num_push = 1;  // constant
    ops[65].num_args = 1;  // constant value

    // 0x42: i64.const
    ops[66].id = 66;
    ops[66].type = i64_type;
    ops[66].num_pull = 0;
    ops[66].num_push = 1;  // constant
    ops[66].num_args = 1;  // constant value

    // 0x43: f32.const
    ops[67].id = 67;
    ops[67].type = f32_type;
    ops[67].num_pull = 0;
    ops[67].num_push = 1;  // constant
    ops[67].num_args = 1;  // constant value

    // 0x44: f64.const
    ops[68].id = 68;
    ops[68].type = f64_type;
    ops[68].num_pull = 0;
    ops[68].num_push = 1;  // constant
    ops[68].num_args = 1;  // constant value

    // 0x45: i32.eqz
    ops[69].id = 69;
    ops[69].type = i32_type;
    ops[69].op = wopt_eq;  // Using eq for eqz
    ops[69].num_pull = 1;
    ops[69].num_push = 1;
    ops[69].num_args = 0;

    // 0x46: i32.eq
    ops[70].id = 70;
    ops[70].type = i32_type;
    ops[70].op = wopt_eq;
    ops[70].num_pull = 2;
    ops[70].num_push = 1;
    ops[70].num_args = 0;

    // 0x47: i32.ne
    ops[71].id = 71;
    ops[71].type = i32_type;
    ops[71].op = wopt_ne;
    ops[71].num_pull = 2;
    ops[71].num_push = 1;
    ops[71].num_args = 0;

    // 0x48: i32.lt_s
    ops[72].id = 72;
    ops[72].type = i32_type;
    ops[72].op = wopt_lt;
    ops[72].num_pull = 2;
    ops[72].num_push = 1;
    ops[72].num_args = 0;

    // 0x49: i32.lt_u
    ops[73].id = 73;
    ops[73].type = u32_type;
    ops[73].op = wopt_lt;
    ops[73].num_pull = 2;
    ops[73].num_push = 1;
    ops[73].num_args = 0;

    // 0x4A: i32.gt_s
    ops[74].id = 74;
    ops[74].type = i32_type;
    ops[74].op = wopt_gt;
    ops[74].num_pull = 2;
    ops[74].num_push = 1;
    ops[74].num_args = 0;

    // 0x4B: i32.gt_u
    ops[75].id = 75;
    ops[75].type = u32_type;
    ops[75].op = wopt_gt;
    ops[75].num_pull = 2;
    ops[75].num_push = 1;
    ops[75].num_args = 0;

    // 0x4C: i32.le_s
    ops[76].id = 76;
    ops[76].type = i32_type;
    ops[76].op = wopt_le;
    ops[76].num_pull = 2;
    ops[76].num_push = 1;
    ops[76].num_args = 0;

    // 0x4D: i32.le_u
    ops[77].id = 77;
    ops[77].type = u32_type;
    ops[77].op = wopt_le;
    ops[77].num_pull = 2;
    ops[77].num_push = 1;
    ops[77].num_args = 0;

    // 0x4E: i32.ge_s
    ops[78].id = 78;
    ops[78].type = i32_type;
    ops[78].op = wopt_ge;
    ops[78].num_pull = 2;
    ops[78].num_push = 1;
    ops[78].num_args = 0;

    // 0x4F: i32.ge_u
    ops[79].id = 79;
    ops[79].type = u32_type;
    ops[79].op = wopt_ge;
    ops[79].num_pull = 2;
    ops[79].num_push = 1;
    ops[79].num_args = 0;

    // 0x51: i64.eq
    ops[81].id = 81;
    ops[81].type = i64_type;
    ops[81].op = wopt_eq;
    ops[81].num_pull = 2;
    ops[81].num_push = 1;
    ops[81].num_args = 0;

    // 0x52: i64.ne
    ops[82].id = 82;
    ops[82].type = i64_type;
    ops[82].op = wopt_ne;
    ops[82].num_pull = 2;
    ops[82].num_push = 1;
    ops[82].num_args = 0;

    // 0x53: i64.lt_s
    ops[83].id = 83;
    ops[83].type = i64_type;
    ops[83].op = wopt_lt;
    ops[83].num_pull = 2;
    ops[83].num_push = 1;
    ops[83].num_args = 0;

    // 0x54: i64.lt_u
    ops[84].id = 84;
    ops[84].type = u64_type;
    ops[84].op = wopt_lt;
    ops[84].num_pull = 2;
    ops[84].num_push = 1;
    ops[84].num_args = 0;

    // 0x55: i64.gt_s
    ops[85].id = 85;
    ops[85].type = i64_type;
    ops[85].op = wopt_gt;
    ops[85].num_pull = 2;
    ops[85].num_push = 1;
    ops[85].num_args = 0;

    // 0x56: i64.gt_u
    ops[86].id = 86;
    ops[86].type = u64_type;
    ops[86].op = wopt_gt;
    ops[86].num_pull = 2;
    ops[86].num_push = 1;
    ops[86].num_args = 0;

    // 0x57: i64.le_s
    ops[87].id = 87;
    ops[87].type = i64_type;
    ops[87].op = wopt_le;
    ops[87].num_pull = 2;
    ops[87].num_push = 1;
    ops[87].num_args = 0;

    // 0x58: i64.le_u
    ops[88].id = 88;
    ops[88].type = u64_type;
    ops[88].op = wopt_le;
    ops[88].num_pull = 2;
    ops[88].num_push = 1;
    ops[88].num_args = 0;

    // 0x59: i64.ge_s
    ops[89].id = 89;
    ops[89].type = i64_type;
    ops[89].op = wopt_ge;
    ops[89].num_pull = 2;
    ops[89].num_push = 1;
    ops[89].num_args = 0;

    // 0x5A: i64.ge_u
    ops[90].id = 90;
    ops[90].type = u64_type;
    ops[90].op = wopt_ge;
    ops[90].num_pull = 2;
    ops[90].num_push = 1;
    ops[90].num_args = 0;

    // Implement arithmetic operations
    // 0x6A: i32.add
    ops[106].id = 106;
    ops[106].type = i32_type;
    ops[106].op = wopt_add;
    ops[106].num_pull = 2;
    ops[106].num_push = 1;
    ops[106].num_args = 0;

    // 0x6B: i32.sub
    ops[107].id = 107;
    ops[107].type = i32_type;
    ops[107].op = wopt_sub;
    ops[107].num_pull = 2;
    ops[107].num_push = 1;
    ops[107].num_args = 0;

    // 0x6C: i32.mul
    ops[108].id = 108;
    ops[108].type = i32_type;
    ops[108].op = wopt_mul;
    ops[108].num_pull = 2;
    ops[108].num_push = 1;
    ops[108].num_args = 0;

    // 0x6D: i32.div_s
    ops[109].id = 109;
    ops[109].type = i32_type;
    ops[109].op = wopt_div;
    ops[109].num_pull = 2;
    ops[109].num_push = 1;
    ops[109].num_args = 0;

    // 0x6E: i32.div_u
    ops[110].id = 110;
    ops[110].type = u32_type;
    ops[110].op = wopt_div;
    ops[110].num_pull = 2;
    ops[110].num_push = 1;
    ops[110].num_args = 0;

    // 0x6F: i32.rem_s
    ops[111].id = 111;
    ops[111].type = i32_type;
    ops[111].op = wopt_rem;
    ops[111].num_pull = 2;
    ops[111].num_push = 1;
    ops[111].num_args = 0;

    // 0x70: i32.rem_u
    ops[112].id = 112;
    ops[112].type = u32_type;
    ops[112].op = wopt_rem;
    ops[112].num_pull = 2;
    ops[112].num_push = 1;
    ops[112].num_args = 0;

    // 0x71: i32.and
    ops[113].id = 113;
    ops[113].type = i32_type;
    ops[113].op = wopt_and;
    ops[113].num_pull = 2;
    ops[113].num_push = 1;
    ops[113].num_args = 0;

    // 0x72: i32.or
    ops[114].id = 114;
    ops[114].type = i32_type;
    ops[114].op = wopt_or;
    ops[114].num_pull = 2;
    ops[114].num_push = 1;
    ops[114].num_args = 0;

    // 0x73: i32.xor
    ops[115].id = 115;
    ops[115].type = i32_type;
    ops[115].op = wopt_xor;
    ops[115].num_pull = 2;
    ops[115].num_push = 1;
    ops[115].num_args = 0;

    // 0x74: i32.shl
    ops[116].id = 116;
    ops[116].type = i32_type;
    ops[116].op = wopt_shl;
    ops[116].num_pull = 2;
    ops[116].num_push = 1;
    ops[116].num_args = 0;

    // 0x75: i32.shr_s
    ops[117].id = 117;
    ops[117].type = i32_type;
    ops[117].op = wopt_shr;
    ops[117].num_pull = 2;
    ops[117].num_push = 1;
    ops[117].num_args = 0;

    // 0x76: i32.shr_u
    ops[118].id = 118;
    ops[118].type = u32_type;
    ops[118].op = wopt_shr;
    ops[118].num_pull = 2;
    ops[118].num_push = 1;
    ops[118].num_args = 0;

    // 0x77: i32.rotl
    ops[119].id = 119;
    ops[119].type = i32_type;
    ops[119].op = wopt_rotl;
    ops[119].num_pull = 2;
    ops[119].num_push = 1;
    ops[119].num_args = 0;

    // 0x78: i32.rotr
    ops[120].id = 120;
    ops[120].type = i32_type;
    ops[120].op = wopt_rotr;
    ops[120].num_pull = 2;
    ops[120].num_push = 1;
    ops[120].num_args = 0;

    // 0x79: i64.add
    ops[121].id = 121;
    ops[121].type = i64_type;
    ops[121].num_pull = 2;
    ops[121].num_push = 1;
    ops[121].num_args = 0;

    // 0x7A: i64.sub
    ops[122].id = 122;
    ops[122].type = i64_type;
    ops[122].num_pull = 2;
    ops[122].num_push = 1;
    ops[122].num_args = 0;

    // 0x7B: i64.mul
    ops[123].id = 123;
    ops[123].type = i64_type;
    ops[123].num_pull = 2;
    ops[123].num_push = 1;
    ops[123].num_args = 0;

    // 0x7C: i64.div_s
    ops[124].id = 124;
    ops[124].type = i64_type;
    ops[124].num_pull = 2;
    ops[124].num_push = 1;
    ops[124].num_args = 0;

    // 0x7D: i64.div_u
    ops[125].id = 125;
    ops[125].type = u64_type;
    ops[125].num_pull = 2;
    ops[125].num_push = 1;
    ops[125].num_args = 0;

    // 0x7E: i64.rem_s
    ops[126].id = 126;
    ops[126].type = i64_type;
    ops[126].num_pull = 2;
    ops[126].num_push = 1;
    ops[126].num_args = 0;

    // 0x7F: i64.rem_u
    ops[127].id = 127;
    ops[127].type = u64_type;
    ops[127].num_pull = 2;
    ops[127].num_push = 1;
    ops[127].num_args = 0;

    // 0x80: i64.and
    ops[128].id = 128;
    ops[128].type = i64_type;
    ops[128].num_pull = 2;
    ops[128].num_push = 1;
    ops[128].num_args = 0;

    // 0x81: i64.or
    ops[129].id = 129;
    ops[129].type = i64_type;
    ops[129].num_pull = 2;
    ops[129].num_push = 1;
    ops[129].num_args = 0;

    // 0x82: i64.xor
    ops[130].id = 130;
    ops[130].type = i64_type;
    ops[130].num_pull = 2;
    ops[130].num_push = 1;
    ops[130].num_args = 0;

    // 0x83: i64.shl
    ops[131].id = 131;
    ops[131].type = i64_type;
    ops[131].num_pull = 2;
    ops[131].num_push = 1;
    ops[131].num_args = 0;

    // 0x84: i64.shr_s
    ops[132].id = 132;
    ops[132].type = i64_type;
    ops[132].num_pull = 2;
    ops[132].num_push = 1;
    ops[132].num_args = 0;

    // 0x85: i64.shr_u
    ops[133].id = 133;
    ops[133].type = u64_type;
    ops[133].num_pull = 2;
    ops[133].num_push = 1;
    ops[133].num_args = 0;

    // 0x86: i64.rotl
    ops[134].id = 134;
    ops[134].type = i64_type;
    ops[134].num_pull = 2;
    ops[134].num_push = 1;
    ops[134].num_args = 0;

    // 0x87: i64.rotr
    ops[135].id = 135;
    ops[135].type = i64_type;
    ops[135].num_pull = 2;
    ops[135].num_push = 1;
    ops[135].num_args = 0;

    // 0x8A: f32.add
    ops[138].id = 138;
    ops[138].type = f32_type;
    ops[138].num_pull = 2;
    ops[138].num_push = 1;
    ops[138].num_args = 0;

    // 0x8B: f32.sub
    ops[139].id = 139;
    ops[139].type = f32_type;
    ops[139].num_pull = 2;
    ops[139].num_push = 1;
    ops[139].num_args = 0;

    // 0x8C: f32.mul
    ops[140].id = 140;
    ops[140].type = f32_type;
    ops[140].num_pull = 2;
    ops[140].num_push = 1;
    ops[140].num_args = 0;

    // 0x8D: f32.div
    ops[141].id = 141;
    ops[141].type = f32_type;
    ops[141].num_pull = 2;
    ops[141].num_push = 1;
    ops[141].num_args = 0;

    // 0x8E: f32.min
    ops[142].id = 142;
    ops[142].type = f32_type;
    ops[142].num_pull = 2;
    ops[142].num_push = 1;
    ops[142].num_args = 0;

    // 0x8F: f32.max
    ops[143].id = 143;
    ops[143].type = f32_type;
    ops[143].num_pull = 2;
    ops[143].num_push = 1;
    ops[143].num_args = 0;

    // 0x90: f32.copysign
    ops[144].id = 144;
    ops[144].type = f32_type;
    ops[144].num_pull = 2;
    ops[144].num_push = 1;
    ops[144].num_args = 0;

    // 0x91: f64.add
    ops[145].id = 145;
    ops[145].type = f64_type;
    ops[145].num_pull = 2;
    ops[145].num_push = 1;
    ops[145].num_args = 0;

    // 0x92: f64.sub
    ops[146].id = 146;
    ops[146].type = f64_type;
    ops[146].num_pull = 2;
    ops[146].num_push = 1;
    ops[146].num_args = 0;

    // 0x93: f64.mul
    ops[147].id = 147;
    ops[147].type = f64_type;
    ops[147].num_pull = 2;
    ops[147].num_push = 1;
    ops[147].num_args = 0;

    // 0x94: f64.div
    ops[148].id = 148;
    ops[148].type = f64_type;
    ops[148].num_pull = 2;
    ops[148].num_push = 1;
    ops[148].num_args = 0;

    // 0x95: f64.min
    ops[149].id = 149;
    ops[149].type = f64_type;
    ops[149].num_pull = 2;
    ops[149].num_push = 1;
    ops[149].num_args = 0;

    // 0x96: f64.max
    ops[150].id = 150;
    ops[150].type = f64_type;
    ops[150].num_pull = 2;
    ops[150].num_push = 1;
    ops[150].num_args = 0;

    // 0x97: f64.copysign
    ops[151].id = 151;
    ops[151].type = f64_type;
    ops[151].num_pull = 2;
    ops[151].num_push = 1;
    ops[151].num_args = 0;

    // Conversion operations
    // 0x98: i32.wrap_i64
    ops[152].id = 152;
    ops[152].type = i32_type;
    ops[152].num_pull = 1;
    ops[152].num_push = 1;
    ops[152].num_args = 0;

    // 0x99: i32.trunc_f32_s
    ops[153].id = 153;
    ops[153].type = i32_type;
    ops[153].num_pull = 1;
    ops[153].num_push = 1;
    ops[153].num_args = 0;

    // 0x9A: i32.trunc_f32_u
    ops[154].id = 154;
    ops[154].type = u32_type;
    ops[154].num_pull = 1;
    ops[154].num_push = 1;
    ops[154].num_args = 0;

    // 0x9B: i32.trunc_f64_s
    ops[155].id = 155;
    ops[155].type = i32_type;
    ops[155].num_pull = 1;
    ops[155].num_push = 1;
    ops[155].num_args = 0;

    // 0x9C: i32.trunc_f64_u
    ops[156].id = 156;
    ops[156].type = u32_type;
    ops[156].num_pull = 1;
    ops[156].num_push = 1;
    ops[156].num_args = 0;

    // 0x9D: i64.extend_i32_s
    ops[157].id = 157;
    ops[157].type = i64_type;
    ops[157].num_pull = 1;
    ops[157].num_push = 1;
    ops[157].num_args = 0;

    // 0x9E: i64.extend_i32_u
    ops[158].id = 158;
    ops[158].type = u64_type;
    ops[158].num_pull = 1;
    ops[158].num_push = 1;
    ops[158].num_args = 0;

    // 0x9F: i64.trunc_f32_s
    ops[159].id = 159;
    ops[159].type = i64_type;
    ops[159].num_pull = 1;
    ops[159].num_push = 1;
    ops[159].num_args = 0;

    // 0xA0: i64.trunc_f32_u
    ops[160].id = 160;
    ops[160].type = u64_type;
    ops[160].num_pull = 1;
    ops[160].num_push = 1;
    ops[160].num_args = 0;

    // 0xA1: i64.trunc_f64_s
    ops[161].id = 161;
    ops[161].type = i64_type;
    ops[161].num_pull = 1;
    ops[161].num_push = 1;
    ops[161].num_args = 0;

    // 0xA2: i64.trunc_f64_u
    ops[162].id = 162;
    ops[162].type = u64_type;
    ops[162].num_pull = 1;
    ops[162].num_push = 1;
    ops[162].num_args = 0;

    // 0xA3: f32.convert_i32_s
    ops[163].id = 163;
    ops[163].type = f32_type;
    ops[163].num_pull = 1;
    ops[163].num_push = 1;
    ops[163].num_args = 0;

    // 0xA4: f32.convert_i32_u
    ops[164].id = 164;
    ops[164].type = f32_type;
    ops[164].num_pull = 1;
    ops[164].num_push = 1;
    ops[164].num_args = 0;

    // 0xA5: f32.convert_i64_s
    ops[165].id = 165;
    ops[165].type = f32_type;
    ops[165].num_pull = 1;
    ops[165].num_push = 1;
    ops[165].num_args = 0;

    // 0xA6: f32.convert_i64_u
    ops[166].id = 166;
    ops[166].type = f32_type;
    ops[166].num_pull = 1;
    ops[166].num_push = 1;
    ops[166].num_args = 0;

    // 0xA7: f32.demote_f64
    ops[167].id = 167;
    ops[167].type = f32_type;
    ops[167].num_pull = 1;
    ops[167].num_push = 1;
    ops[167].num_args = 0;

    // 0xA8: f64.convert_i32_s
    ops[168].id = 168;
    ops[168].type = f64_type;
    ops[168].num_pull = 1;
    ops[168].num_push = 1;
    ops[168].num_args = 0;

    // 0xA9: f64.convert_i32_u
    ops[169].id = 169;
    ops[169].type = f64_type;
    ops[169].num_pull = 1;
    ops[169].num_push = 1;
    ops[169].num_args = 0;

    // 0xAA: f64.convert_i64_s
    ops[170].id = 170;
    ops[170].type = f64_type;
    ops[170].num_pull = 1;
    ops[170].num_push = 1;
    ops[170].num_args = 0;

    // 0xAB: f64.convert_i64_u
    ops[171].id = 171;
    ops[171].type = f64_type;
    ops[171].num_pull = 1;
    ops[171].num_push = 1;
    ops[171].num_args = 0;

    // 0xAC: f64.promote_f32
    ops[172].id = 172;
    ops[172].type = f64_type;
    ops[172].num_pull = 1;
    ops[172].num_push = 1;
    ops[172].num_args = 0;

    // Reinterpretation operations
    // 0xAD: i32.reinterpret_f32
    ops[173].id = 173;
    ops[173].type = i32_type;
    ops[173].num_pull = 1;
    ops[173].num_push = 1;
    ops[173].num_args = 0;

    // 0xAE: i64.reinterpret_f64
    ops[174].id = 174;
    ops[174].type = i64_type;
    ops[174].num_pull = 1;
    ops[174].num_push = 1;
    ops[174].num_args = 0;

    // 0xAF: f32.reinterpret_i32
    ops[175].id = 175;
    ops[175].type = f32_type;
    ops[175].num_pull = 1;
    ops[175].num_push = 1;
    ops[175].num_args = 0;

    // 0xB0: f64.reinterpret_i64
    ops[176].id = 176;
    ops[176].type = f64_type;
    ops[176].num_pull = 1;
    ops[176].num_push = 1;
    ops[176].num_args = 0;

    // Sign extension operations
    // 0xC0: i32.extend8_s
    ops[192].id = 192;
    ops[192].type = i32_type;
    ops[192].num_pull = 1;
    ops[192].num_push = 1;
    ops[192].num_args = 0;

    // 0xC1: i32.extend16_s
    ops[193].id = 193;
    ops[193].type = i32_type;
    ops[193].num_pull = 1;
    ops[193].num_push = 1;
    ops[193].num_args = 0;

    // 0xC2: i64.extend8_s
    ops[194].id = 194;
    ops[194].type = i64_type;
    ops[194].num_pull = 1;
    ops[194].num_push = 1;
    ops[194].num_args = 0;

    // 0xC3: i64.extend16_s
    ops[195].id = 195;
    ops[195].type = i64_type;
    ops[195].num_pull = 1;
    ops[195].num_push = 1;
    ops[195].num_args = 0;

    // 0xC4: i64.extend32_s
    ops[196].id = 196;
    ops[196].type = i64_type;
    ops[196].num_pull = 1;
    ops[196].num_push = 1;
    ops[196].num_args = 0;

    // SIMD operations (0xFD prefix, but we'll just use indices beyond standard ops)
    // 0xFD 0x00: v128.load
    ops[200].id = 200;  // Using index 200+ for SIMD
    ops[200].type = i32_type;  // Using i32 as placeholder for v128
    ops[200].size_arg = 128;
    ops[200].num_pull = 1;  // address
    ops[200].num_push = 1;  // result
    ops[200].num_args = 2;  // align and offset

    // Atomic operations (0xFE prefix, but we'll just use indices beyond SIMD ops)
    // 0xFE 0x00: memory.atomic.notify
    ops[220].id = 220;  // Using index 220+ for atomic ops
    ops[220].type = i32_type;
    ops[220].num_pull = 2;
    ops[220].num_push = 1;
    ops[220].num_args = 2;  // align and offset

    // Thread operations (0xFF prefix, but we'll just use indices beyond atomic ops)
    // Just placeholders for potential future thread ops
    ops[240].id = 240;
    ops[240].type = void_type;
    ops[240].num_pull = 0;
    ops[240].num_push = 0;
    ops[240].num_args = 0;

    // Ensure the specific case from the example
    // i32.load (opcode 0x28 = 40 in decimal)
    ops[28].id = 28;
    ops[28].type.type = wt_integer;
    ops[28].type.size = 4;
    ops[28].type.is_signed = true;
    ops[28].op = wopt_load;
    ops[28].size_arg = 32;
    ops[28].num_pull = 1;  // address
    ops[28].num_push = 1;  // result
    ops[28].num_args = 2;  // align and offset

    // todo: returns
}
