#pragma once
#include "fa_types.h"
#include "fa_job.h"

typedef enum {
    wt_void = 0,
    wt_integer = 1,
    wt_unsigned_integer = 2,
    wt_float = 3
} fa_WasmType_type;

typedef struct {
    uint8_t type;
    uint8_t size; // bytes
    bool is_signed;
} fa_WasmType;

////////////////////////////////////////////////////////////////

typedef enum {
    wopt_unique = 0, // like if, block, loop etc.
    wopt_load = 1,
    wopt_store = 2,
    wopt_const = 3,
    wopt_gt = 4,
    wopt_lt = 5,
    wopt_eq = 6,
    wopt_ne = 7,
    wopt_ge = 8,
    wopt_le = 9,
    wopt_add = 10,
    wopt_sub = 11,
    wopt_mul = 12,
    wopt_div = 13,
    wopt_rem = 14,
    wopt_and = 15,
    wopt_or = 16,
    wopt_xor = 17,
    wopt_shl = 18,
    wopt_shr = 19,
    wopt_rotl = 20,
    wopt_rotr = 21,
    wopt_clz = 22,
    wopt_ctz = 23,
    wopt_popcnt = 24,
    wopt_eqz = 25,
    wopt_convert = 26,
    wopt_extend = 27,
    wopt_trunc = 28,
    wopt_wrap = 29,
    wopt_reinterpret = 30,
    wopt_drop = 31,
    wopt_select = 32,
    wopt_call = 33,
    wopt_return = 34
} fa_WasmOp_type;

#define OP_RETURN_TYPE void
#define OP_ARGUMENTS fa_Job* job
typedef OP_RETURN_TYPE (*Operation)(OP_ARGUMENTS);

typedef struct {
    uint8_t id;
    fa_WasmType type;
    fa_WasmOp_type op;
    uint8_t size_arg; // (i.e. 16 for i32.store16)
    uint8_t num_pull; // how many arguments takes from previous loads etc. on the stack
    uint8_t num_push; // how many arguments to push onto the stack
    uint8_t num_args; // how many arguments in the byte code

    Operation operation;
} fa_WasmOp;
