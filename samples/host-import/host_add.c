#include "fa_runtime.h"

int fayasm_host_add(fa_Runtime* runtime, const fa_RuntimeHostCall* call, void* user_data) {
    (void)runtime;
    (void)user_data;
    if (!fa_RuntimeHostCall_expect(call, 2, 1)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    i32 lhs = 0;
    i32 rhs = 0;
    if (!fa_RuntimeHostCall_arg_i32(call, 0, &lhs) ||
        !fa_RuntimeHostCall_arg_i32(call, 1, &rhs)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!fa_RuntimeHostCall_set_i32(call, 0, lhs + rhs)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}
