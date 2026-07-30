#include "fa_ops_internal.h"

/* Scalar numeric handlers and macro-built microcode sequences. */

/*
 * Macro family: bitwise operations.
 * All generated handlers:
 * - pop two stack operands (`rhs`, then `lhs`)
 * - normalize them to u64
 * - apply `expr`
 * - mask/sign according to descriptor width/type
 */
#define DEFINE_BITWISE_OP(name, expr)                                            \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                   \
        (void)runtime;                                                           \
        if (!job || !descriptor) {                                               \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                              \
        }                                                                        \
        fa_JobValue rhs;                                                         \
        if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {                     \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        fa_JobValue lhs;                                                         \
        if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {                     \
            restore_stack_value(job, &rhs);                                      \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        u64 right = 0;                                                           \
        u64 left = 0;                                                            \
        if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) { \
            restore_stack_value(job, &lhs);                                      \
            restore_stack_value(job, &rhs);                                      \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        u64 outcome = (expr);                                                    \
        const uint8_t bits = descriptor->type.size ? descriptor->type.size * 8U : 32U; \
        const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed; \
        return push_int_checked(job, mask_unsigned_value(outcome, bits), bits, is_signed); \
    }

DEFINE_BITWISE_OP(op_bitwise_and_mc, left & right)
DEFINE_BITWISE_OP(op_bitwise_or_mc, left | right)
DEFINE_BITWISE_OP(op_bitwise_xor_mc, left ^ right)

/* Macro family: clz/ctz/popcnt for i32/i64 with descriptor-driven width. */
#define DEFINE_BITCOUNT_OP(name, op32, op64)                                     \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                   \
        (void)runtime;                                                           \
        if (!job || !descriptor) {                                               \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                              \
        }                                                                        \
        fa_JobValue value;                                                       \
        if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {                   \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        u64 raw = 0;                                                             \
        if (!job_value_to_u64(&value, &raw)) {                                   \
            restore_stack_value(job, &value);                                    \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U; \
        u64 result = 0;                                                          \
        if (width <= 32U) {                                                      \
            result = (u64)(op32((u32)raw));                                      \
        } else {                                                                 \
            result = (u64)(op64(raw));                                           \
        }                                                                        \
        return push_int_checked(job, result, width, false);                      \
    }

DEFINE_BITCOUNT_OP(op_bitcount_clz_mc, clz32, clz64)
DEFINE_BITCOUNT_OP(op_bitcount_ctz_mc, ctz32, ctz64)
DEFINE_BITCOUNT_OP(op_bitcount_popcnt_mc, popcnt32, popcnt64)

OP_RETURN_TYPE op_shift_left_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 left_raw = 0;
    if (!job_value_to_u64(&lhs, &left_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 base = (u32)left_raw;
        outcome = (u64)(base << amount);
    } else {
        const u64 base = left_raw;
        outcome = base << amount;
    }
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    return push_int_checked(job, outcome, width, is_signed);
}

OP_RETURN_TYPE op_shift_right_signed_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 left_signed = 0;
    if (!job_value_to_i64(&lhs, &left_signed)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const i32 base = (i32)left_signed;
        outcome = (u64)(i32)(base >> amount);
    } else {
        const i64 base = left_signed;
        outcome = (u64)(base >> amount);
    }
    return push_int_checked(job, outcome, width, true);
}

OP_RETURN_TYPE op_shift_right_unsigned_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 left_unsigned = 0;
    if (!job_value_to_u64(&lhs, &left_unsigned)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 base = (u32)left_unsigned;
        outcome = (u64)(base >> amount);
    } else {
        const u64 base = left_unsigned;
        outcome = base >> amount;
    }
    return push_int_checked(job, outcome, width, false);
}

OP_RETURN_TYPE op_rotate_left_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 value_raw = 0;
    if (!job_value_to_u64(&lhs, &value_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 value = (u32)value_raw;
        outcome = (u64)rotl32(value, amount);
    } else {
        const u64 value = value_raw;
        outcome = rotl64(value, amount);
    }
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    return push_int_checked(job, outcome, width, is_signed);
}

OP_RETURN_TYPE op_rotate_right_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 amount_raw = 0;
    if (!job_value_to_u64(&rhs, &amount_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 value_raw = 0;
    if (!job_value_to_u64(&lhs, &value_raw)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint8_t width = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    const uint8_t mask = width <= 32U ? 31U : 63U;
    const uint8_t amount = (uint8_t)(amount_raw & mask);
    u64 outcome = 0;
    if (width <= 32U) {
        const u32 value = (u32)value_raw;
        outcome = (u64)rotr32(value, amount);
    } else {
        const u64 value = value_raw;
        outcome = rotr64(value, amount);
    }
    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    return push_int_checked(job, outcome, width, is_signed);
}

/*
 * Macro family: relational/equality operators.
 * Signedness and float/int semantics are derived from `descriptor->type`.
 */
#define DEFINE_COMPARE_OP(name, cmp)                                            \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                  \
        (void)runtime;                                                          \
        if (!job || !descriptor) {                                              \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                             \
        }                                                                       \
        fa_JobValue rhs;                                                        \
        if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {                    \
            return FA_RUNTIME_ERR_TRAP;                                         \
        }                                                                       \
        fa_JobValue lhs;                                                        \
        if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {                    \
            restore_stack_value(job, &rhs);                                     \
            return FA_RUNTIME_ERR_TRAP;                                         \
        }                                                                       \
        bool result = false;                                                    \
        if (descriptor->type.type == wt_float) {                                \
            f64 right = 0.0;                                                    \
            f64 left = 0.0;                                                     \
            if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) { \
                restore_stack_value(job, &lhs);                                 \
                restore_stack_value(job, &rhs);                                 \
                return FA_RUNTIME_ERR_TRAP;                                     \
            }                                                                   \
            result = (left cmp right);                                          \
        } else {                                                                \
            const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed; \
            if (is_signed) {                                                    \
                i64 right = 0;                                                  \
                i64 left = 0;                                                   \
                if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) { \
                    restore_stack_value(job, &lhs);                             \
                    restore_stack_value(job, &rhs);                             \
                    return FA_RUNTIME_ERR_TRAP;                                 \
                }                                                               \
                result = (left cmp right);                                      \
            } else {                                                            \
                u64 right = 0;                                                  \
                u64 left = 0;                                                   \
                if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) { \
                    restore_stack_value(job, &lhs);                             \
                    restore_stack_value(job, &rhs);                             \
                    return FA_RUNTIME_ERR_TRAP;                                 \
                }                                                               \
                result = (left cmp right);                                      \
            }                                                                   \
        }                                                                       \
        return push_bool_checked(job, result);                                  \
    }

DEFINE_COMPARE_OP(op_compare_eq_mc, ==)
DEFINE_COMPARE_OP(op_compare_ne_mc, !=)
DEFINE_COMPARE_OP(op_compare_lt_mc, <)
DEFINE_COMPARE_OP(op_compare_gt_mc, >)
DEFINE_COMPARE_OP(op_compare_le_mc, <=)
DEFINE_COMPARE_OP(op_compare_ge_mc, >=)

OP_RETURN_TYPE op_arith_add_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left + right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left + right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left + right;
    return push_int_checked(job, outcome, result_bits, false);
}

OP_RETURN_TYPE op_arith_sub_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left - right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left - right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left - right;
    return push_int_checked(job, outcome, result_bits, false);
}

OP_RETURN_TYPE op_arith_mul_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left * right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left * right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left * right;
    return push_int_checked(job, outcome, result_bits, false);
}

OP_RETURN_TYPE op_arith_div_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        f64 right = 0.0;
        f64 left = 0.0;
        if (!job_value_to_f64(&rhs, &right) || !job_value_to_f64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        return push_float_checked(job, left / right, descriptor->type.size == 8);
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        if (right == 0) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        if ((result_bits == 32U && left == (i64)INT32_MIN && right == -1) ||
            (result_bits == 64U && left == (i64)INT64_MIN && right == -1)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left / right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    if (right == 0) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left / right;
    return push_int_checked(job, outcome, result_bits, false);
}

OP_RETURN_TYPE op_arith_rem_mc(OP_ARGUMENTS) {
    (void)runtime;
    if (!job || !descriptor) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue rhs;
    if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue lhs;
    if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    if (descriptor->type.type == wt_float) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }

    const bool is_signed = descriptor->type.type == wt_integer && descriptor->type.is_signed;
    const uint8_t result_bits = descriptor->type.size ? descriptor->type.size * 8U : 32U;
    if (is_signed) {
        i64 right = 0;
        i64 left = 0;
        if (!job_value_to_i64(&rhs, &right) || !job_value_to_i64(&lhs, &left)) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        if (right == 0) {
            restore_stack_value(job, &lhs);
            restore_stack_value(job, &rhs);
            return FA_RUNTIME_ERR_TRAP;
        }
        i64 outcome = left % right;
        return push_int_checked(job, (u64)outcome, result_bits, true);
    }

    u64 right = 0;
    u64 left = 0;
    if (!job_value_to_u64(&rhs, &right) || !job_value_to_u64(&lhs, &left)) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    if (right == 0) {
        restore_stack_value(job, &lhs);
        restore_stack_value(job, &rhs);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 outcome = left % right;
    return push_int_checked(job, outcome, result_bits, false);
}

/*
 * Macro family: scalar conversion operators.
 * The macro centralizes the guard/stack-pop path; each expansion provides
 * only the conversion-specific body.
 */
#define DEFINE_CONVERT_OP(name, body)                                            \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                   \
        (void)runtime;                                                           \
        if (!job || !descriptor) {                                               \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                              \
        }                                                                        \
        fa_JobValue source;                                                      \
        if (pop_stack_checked(job, &source) != FA_RUNTIME_OK) {                  \
            return FA_RUNTIME_ERR_TRAP;                                          \
        }                                                                        \
        body                                                                     \
    }

DEFINE_CONVERT_OP(op_convert_i32_wrap_i64_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, (u32)value, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f32_s_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32((double)value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f32_u_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32((double)value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, false);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f64_s_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32(value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_f64_u_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i32(value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 32U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_extend_i32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, sign_extend_value((u64)value, 32U), 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend_i32_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, mask_unsigned_value(value, 32U), 64U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f32_s_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64((double)value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f32_u_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64((double)value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f64_s_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64(value, true, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_f64_u_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 truncated = 0;
    if (!trunc_f64_to_i64(value, false, &truncated)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, truncated, 64U, false);
})

/*
 * Saturating (non-trapping) float->int conversions, 0xFC subopcodes 0x00..0x07.
 * These mirror the trapping trunc ops above but clamp instead of trapping, and
 * are dispatched from op_bulk_memory's subopcode table rather than the primary
 * opcode descriptors. emcc emits these by default (nontrapping-fptoint) for a
 * plain (int)/(long) cast of a float, so real toolchain output depends on them.
 */
DEFINE_CONVERT_OP(op_convert_i32_trunc_sat_f32_s_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i32((double)value, true), 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_sat_f32_u_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i32((double)value, false), 32U, false);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_sat_f64_s_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i32(value, true), 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_trunc_sat_f64_u_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i32(value, false), 32U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_sat_f32_s_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i64((double)value, true), 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_sat_f32_u_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i64((double)value, false), 64U, false);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_sat_f64_s_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i64(value, true), 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_trunc_sat_f64_u_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, trunc_sat_f64_to_i64(value, false), 64U, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i32_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i64_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_from_i64_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f32_demote_f64_mc, {
    f64 value = 0.0;
    if (!job_value_to_f64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f32)value, false);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i32_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i64_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_from_i64_u_mc, {
    u64 value = 0;
    if (!job_value_to_u64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_f64_promote_f32_mc, {
    f32 value = 0.0f;
    if (!job_value_to_f32(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_float_checked(job, (f64)value, true);
})

DEFINE_CONVERT_OP(op_convert_i32_extend8_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i32 extended = (i32)(int8_t)(value & 0xFF);
    return push_int_checked(job, (u32)extended, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i32_extend16_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i32 extended = (i32)(int16_t)(value & 0xFFFF);
    return push_int_checked(job, (u32)extended, 32U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend8_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 extended = (i64)(int8_t)(value & 0xFF);
    return push_int_checked(job, (u64)extended, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend16_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 extended = (i64)(int16_t)(value & 0xFFFF);
    return push_int_checked(job, (u64)extended, 64U, true);
})

DEFINE_CONVERT_OP(op_convert_i64_extend32_s_mc, {
    i64 value = 0;
    if (!job_value_to_i64(&source, &value)) {
        restore_stack_value(job, &source);
        return FA_RUNTIME_ERR_TRAP;
    }
    i64 extended = (i64)(int32_t)(value & 0xFFFFFFFFULL);
    return push_int_checked(job, (u64)extended, 64U, true);
})

/*
 * Macro families for float unary/binary-special/reinterpret operators.
 * These keep stack discipline and trap behavior consistent across many opcodes.
 */
#define DEFINE_FLOAT_UNARY_OP(name, ctype, to_func, expr, is_64)                \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                 \
        (void)runtime;                                                         \
        if (!job || !descriptor) {                                             \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                            \
        }                                                                      \
        fa_JobValue value;                                                     \
        if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {                 \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        ctype source = 0;                                                      \
        if (!to_func(&value, &source)) {                                       \
            restore_stack_value(job, &value);                                  \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        ctype result = (expr);                                                 \
        return push_float_checked(job, result, is_64);                         \
    }

#define DEFINE_FLOAT_BINARY_SPECIAL_OP(name, ctype, to_func, expr, is_64)       \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                 \
        (void)runtime;                                                         \
        if (!job || !descriptor) {                                             \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                            \
        }                                                                      \
        fa_JobValue rhs;                                                       \
        if (pop_stack_checked(job, &rhs) != FA_RUNTIME_OK) {                   \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        fa_JobValue lhs;                                                       \
        if (pop_stack_checked(job, &lhs) != FA_RUNTIME_OK) {                   \
            restore_stack_value(job, &rhs);                                    \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        ctype right = 0;                                                       \
        ctype left = 0;                                                        \
        if (!to_func(&rhs, &right) || !to_func(&lhs, &left)) {                 \
            restore_stack_value(job, &lhs);                                    \
            restore_stack_value(job, &rhs);                                    \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        ctype result = (expr);                                                 \
        return push_float_checked(job, result, is_64);                         \
    }

#define DEFINE_REINTERPRET_FLOAT_TO_INT_OP(name, float_kind, float_field, float_type, to_func, int_type, int_bits) \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                 \
        (void)runtime;                                                         \
        if (!job || !descriptor) {                                             \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                            \
        }                                                                      \
        fa_JobValue source;                                                    \
        if (pop_stack_checked(job, &source) != FA_RUNTIME_OK) {                \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        int_type raw = 0;                                                      \
        if (source.kind == float_kind) {                                       \
            memcpy(&raw, &source.payload.float_field, sizeof(raw));            \
        } else {                                                               \
            float_type value = 0;                                              \
            if (!to_func(&source, &value)) {                                   \
                restore_stack_value(job, &source);                             \
                return FA_RUNTIME_ERR_TRAP;                                    \
            }                                                                  \
            memcpy(&raw, &value, sizeof(raw));                                 \
        }                                                                      \
        return push_int_checked(job, (u64)raw, int_bits, true);                 \
    }

#define DEFINE_REINTERPRET_INT_TO_FLOAT_OP(name, int_type, float_type, is_64)   \
    OP_RETURN_TYPE name(OP_ARGUMENTS) {                                 \
        (void)runtime;                                                         \
        if (!job || !descriptor) {                                             \
            return FA_RUNTIME_ERR_INVALID_ARGUMENT;                            \
        }                                                                      \
        fa_JobValue source;                                                    \
        if (pop_stack_checked(job, &source) != FA_RUNTIME_OK) {                \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        u64 raw = 0;                                                           \
        if (!job_value_to_u64(&source, &raw)) {                                \
            restore_stack_value(job, &source);                                 \
            return FA_RUNTIME_ERR_TRAP;                                        \
        }                                                                      \
        int_type cast_raw = (int_type)raw;                                     \
        float_type value = 0;                                                  \
        memcpy(&value, &cast_raw, sizeof(value));                              \
        return push_float_checked(job, value, is_64);                          \
    }

DEFINE_FLOAT_UNARY_OP(op_float_abs_f32_mc, f32, job_value_to_f32, fabsf(source), false)
DEFINE_FLOAT_UNARY_OP(op_float_neg_f32_mc, f32, job_value_to_f32, -source, false)
DEFINE_FLOAT_UNARY_OP(op_float_ceil_f32_mc, f32, job_value_to_f32, ceilf(source), false)
DEFINE_FLOAT_UNARY_OP(op_float_floor_f32_mc, f32, job_value_to_f32, floorf(source), false)
DEFINE_FLOAT_UNARY_OP(op_float_trunc_f32_mc, f32, job_value_to_f32, truncf(source), false)
DEFINE_FLOAT_UNARY_OP(op_float_nearest_f32_mc, f32, job_value_to_f32, nearbyintf(source), false)
DEFINE_FLOAT_UNARY_OP(op_float_sqrt_f32_mc, f32, job_value_to_f32, sqrtf(source), false)

DEFINE_FLOAT_UNARY_OP(op_float_abs_f64_mc, f64, job_value_to_f64, fabs(source), true)
DEFINE_FLOAT_UNARY_OP(op_float_neg_f64_mc, f64, job_value_to_f64, -source, true)
DEFINE_FLOAT_UNARY_OP(op_float_ceil_f64_mc, f64, job_value_to_f64, ceil(source), true)
DEFINE_FLOAT_UNARY_OP(op_float_floor_f64_mc, f64, job_value_to_f64, floor(source), true)
DEFINE_FLOAT_UNARY_OP(op_float_trunc_f64_mc, f64, job_value_to_f64, trunc(source), true)
DEFINE_FLOAT_UNARY_OP(op_float_nearest_f64_mc, f64, job_value_to_f64, nearbyint(source), true)
DEFINE_FLOAT_UNARY_OP(op_float_sqrt_f64_mc, f64, job_value_to_f64, sqrt(source), true)

DEFINE_FLOAT_BINARY_SPECIAL_OP(op_float_min_f32_mc, f32, job_value_to_f32, fminf(left, right), false)
DEFINE_FLOAT_BINARY_SPECIAL_OP(op_float_max_f32_mc, f32, job_value_to_f32, fmaxf(left, right), false)
DEFINE_FLOAT_BINARY_SPECIAL_OP(op_float_copysign_f32_mc, f32, job_value_to_f32, copysignf(left, right), false)

DEFINE_FLOAT_BINARY_SPECIAL_OP(op_float_min_f64_mc, f64, job_value_to_f64, fmin(left, right), true)
DEFINE_FLOAT_BINARY_SPECIAL_OP(op_float_max_f64_mc, f64, job_value_to_f64, fmax(left, right), true)
DEFINE_FLOAT_BINARY_SPECIAL_OP(op_float_copysign_f64_mc, f64, job_value_to_f64, copysign(left, right), true)

DEFINE_REINTERPRET_FLOAT_TO_INT_OP(op_reinterpret_i32_from_f32_mc, fa_job_value_f32, f32_value, f32, job_value_to_f32, u32, 32U)
DEFINE_REINTERPRET_FLOAT_TO_INT_OP(op_reinterpret_i64_from_f64_mc, fa_job_value_f64, f64_value, f64, job_value_to_f64, u64, 64U)
DEFINE_REINTERPRET_INT_TO_FLOAT_OP(op_reinterpret_f32_from_i32_mc, u32, f32, false)
DEFINE_REINTERPRET_INT_TO_FLOAT_OP(op_reinterpret_f64_from_i64_mc, u64, f64, true)

OP_RETURN_TYPE op_select(OP_ARGUMENTS);

/*
 * A microcode "program" is currently a short sequence of existing C handlers.
 * Most entries are single-step today, but the structure supports multi-step
 * expansions without changing the dispatch contract.
 */
#define DEFINE_MICROCODE(name, ...)                                            \
    static const Operation name##_steps[] = { __VA_ARGS__ };                    \
    const fa_Microcode name = {                                                 \
        name##_steps,                                                          \
        (uint8_t)(sizeof(name##_steps) / sizeof(name##_steps[0]))              \
    };

DEFINE_MICROCODE(mc_bitwise_and, op_bitwise_and_mc)
DEFINE_MICROCODE(mc_bitwise_or, op_bitwise_or_mc)
DEFINE_MICROCODE(mc_bitwise_xor, op_bitwise_xor_mc)
DEFINE_MICROCODE(mc_bitcount_clz, op_bitcount_clz_mc)
DEFINE_MICROCODE(mc_bitcount_ctz, op_bitcount_ctz_mc)
DEFINE_MICROCODE(mc_bitcount_popcnt, op_bitcount_popcnt_mc)
DEFINE_MICROCODE(mc_shift_left, op_shift_left_mc)
DEFINE_MICROCODE(mc_shift_right_signed, op_shift_right_signed_mc)
DEFINE_MICROCODE(mc_shift_right_unsigned, op_shift_right_unsigned_mc)
DEFINE_MICROCODE(mc_rotate_left, op_rotate_left_mc)
DEFINE_MICROCODE(mc_rotate_right, op_rotate_right_mc)
DEFINE_MICROCODE(mc_compare_eq, op_compare_eq_mc)
DEFINE_MICROCODE(mc_compare_ne, op_compare_ne_mc)
DEFINE_MICROCODE(mc_compare_lt, op_compare_lt_mc)
DEFINE_MICROCODE(mc_compare_gt, op_compare_gt_mc)
DEFINE_MICROCODE(mc_compare_le, op_compare_le_mc)
DEFINE_MICROCODE(mc_compare_ge, op_compare_ge_mc)
DEFINE_MICROCODE(mc_arith_add, op_arith_add_mc)
DEFINE_MICROCODE(mc_arith_sub, op_arith_sub_mc)
DEFINE_MICROCODE(mc_arith_mul, op_arith_mul_mc)
DEFINE_MICROCODE(mc_arith_div, op_arith_div_mc)
DEFINE_MICROCODE(mc_arith_rem, op_arith_rem_mc)
DEFINE_MICROCODE(mc_convert_i32_wrap_i64, op_convert_i32_wrap_i64_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f32_s, op_convert_i32_trunc_f32_s_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f32_u, op_convert_i32_trunc_f32_u_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f64_s, op_convert_i32_trunc_f64_s_mc)
DEFINE_MICROCODE(mc_convert_i32_trunc_f64_u, op_convert_i32_trunc_f64_u_mc)
DEFINE_MICROCODE(mc_convert_i64_extend_i32_s, op_convert_i64_extend_i32_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend_i32_u, op_convert_i64_extend_i32_u_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f32_s, op_convert_i64_trunc_f32_s_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f32_u, op_convert_i64_trunc_f32_u_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f64_s, op_convert_i64_trunc_f64_s_mc)
DEFINE_MICROCODE(mc_convert_i64_trunc_f64_u, op_convert_i64_trunc_f64_u_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i32_s, op_convert_f32_from_i32_s_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i32_u, op_convert_f32_from_i32_u_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i64_s, op_convert_f32_from_i64_s_mc)
DEFINE_MICROCODE(mc_convert_f32_from_i64_u, op_convert_f32_from_i64_u_mc)
DEFINE_MICROCODE(mc_convert_f32_demote_f64, op_convert_f32_demote_f64_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i32_s, op_convert_f64_from_i32_s_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i32_u, op_convert_f64_from_i32_u_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i64_s, op_convert_f64_from_i64_s_mc)
DEFINE_MICROCODE(mc_convert_f64_from_i64_u, op_convert_f64_from_i64_u_mc)
DEFINE_MICROCODE(mc_convert_f64_promote_f32, op_convert_f64_promote_f32_mc)
DEFINE_MICROCODE(mc_convert_i32_extend8_s, op_convert_i32_extend8_s_mc)
DEFINE_MICROCODE(mc_convert_i32_extend16_s, op_convert_i32_extend16_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend8_s, op_convert_i64_extend8_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend16_s, op_convert_i64_extend16_s_mc)
DEFINE_MICROCODE(mc_convert_i64_extend32_s, op_convert_i64_extend32_s_mc)
DEFINE_MICROCODE(mc_float_abs_f32, op_float_abs_f32_mc)
DEFINE_MICROCODE(mc_float_neg_f32, op_float_neg_f32_mc)
DEFINE_MICROCODE(mc_float_ceil_f32, op_float_ceil_f32_mc)
DEFINE_MICROCODE(mc_float_floor_f32, op_float_floor_f32_mc)
DEFINE_MICROCODE(mc_float_trunc_f32, op_float_trunc_f32_mc)
DEFINE_MICROCODE(mc_float_nearest_f32, op_float_nearest_f32_mc)
DEFINE_MICROCODE(mc_float_sqrt_f32, op_float_sqrt_f32_mc)
DEFINE_MICROCODE(mc_float_min_f32, op_float_min_f32_mc)
DEFINE_MICROCODE(mc_float_max_f32, op_float_max_f32_mc)
DEFINE_MICROCODE(mc_float_copysign_f32, op_float_copysign_f32_mc)
DEFINE_MICROCODE(mc_float_abs_f64, op_float_abs_f64_mc)
DEFINE_MICROCODE(mc_float_neg_f64, op_float_neg_f64_mc)
DEFINE_MICROCODE(mc_float_ceil_f64, op_float_ceil_f64_mc)
DEFINE_MICROCODE(mc_float_floor_f64, op_float_floor_f64_mc)
DEFINE_MICROCODE(mc_float_trunc_f64, op_float_trunc_f64_mc)
DEFINE_MICROCODE(mc_float_nearest_f64, op_float_nearest_f64_mc)
DEFINE_MICROCODE(mc_float_sqrt_f64, op_float_sqrt_f64_mc)
DEFINE_MICROCODE(mc_float_min_f64, op_float_min_f64_mc)
DEFINE_MICROCODE(mc_float_max_f64, op_float_max_f64_mc)
DEFINE_MICROCODE(mc_float_copysign_f64, op_float_copysign_f64_mc)
DEFINE_MICROCODE(mc_reinterpret_i32_from_f32, op_reinterpret_i32_from_f32_mc)
DEFINE_MICROCODE(mc_reinterpret_i64_from_f64, op_reinterpret_i64_from_f64_mc)
DEFINE_MICROCODE(mc_reinterpret_f32_from_i32, op_reinterpret_f32_from_i32_mc)
DEFINE_MICROCODE(mc_reinterpret_f64_from_i64, op_reinterpret_f64_from_i64_mc)
DEFINE_MICROCODE(mc_select, op_select)

#undef DEFINE_MICROCODE
#undef DEFINE_BITCOUNT_OP
#undef DEFINE_BITWISE_OP
#undef DEFINE_COMPARE_OP
#undef DEFINE_CONVERT_OP
#undef DEFINE_FLOAT_UNARY_OP
#undef DEFINE_FLOAT_BINARY_SPECIAL_OP
#undef DEFINE_REINTERPRET_FLOAT_TO_INT_OP
#undef DEFINE_REINTERPRET_INT_TO_FLOAT_OP

OP_RETURN_TYPE op_drop(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue value;
    if (pop_stack_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return FA_RUNTIME_OK;
}

OP_RETURN_TYPE op_select(OP_ARGUMENTS) {
    (void)runtime;
    (void)descriptor;
    if (!job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_JobValue condition;
    if (pop_stack_checked(job, &condition) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue value_false;
    if (pop_stack_checked(job, &value_false) != FA_RUNTIME_OK) {
        restore_stack_value(job, &condition);
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_JobValue value_true;
    if (pop_stack_checked(job, &value_true) != FA_RUNTIME_OK) {
        restore_stack_value(job, &value_false);
        restore_stack_value(job, &condition);
        return FA_RUNTIME_ERR_TRAP;
    }
    const bool truthy = job_value_truthy(&condition);
    const fa_JobValue* chosen = truthy ? &value_true : &value_false;
    if (!fa_JobStack_push(&job->stack, chosen)) {
        return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    return FA_RUNTIME_OK;
}
