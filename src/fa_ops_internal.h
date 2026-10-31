#pragma once

#include "fa_arch.h"
#include "fa_ops.h"
#include "fa_runtime.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OP_KEYWORD

typedef struct {
    const Operation* steps;
    uint8_t step_count;
} fa_Microcode;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

/*
 * Private helpers shared by the opcode-family translation units. These are
 * implementation details; the stable operation API remains in fa_ops.h.
 */
size_t op_value_width(const fa_WasmOp* op);

u64 sign_extend_value(u64 value, uint8_t bits);
u64 mask_unsigned_value(u64 value, uint8_t bits);
bool push_int_value(fa_Job* job, u64 value, uint8_t bit_width, bool is_signed);
bool push_float_value(fa_Job* job, double value, bool is_64);
bool push_bool_value(fa_Job* job, bool truthy);
int push_int_checked(fa_Job* job, u64 value, uint8_t bit_width, bool is_signed);
int push_float_checked(fa_Job* job, double value, bool is_64);
int push_bool_checked(fa_Job* job, bool truthy);
bool pop_stack_value(fa_Job* job, fa_JobValue* out);
int pop_stack_checked(fa_Job* job, fa_JobValue* out);
void restore_stack_value(fa_Job* job, const fa_JobValue* value);

bool job_value_to_u64(const fa_JobValue* value, u64* out);
bool job_value_to_i64(const fa_JobValue* value, i64* out);
bool job_value_to_f32(const fa_JobValue* value, f32* out);
bool job_value_to_f64(const fa_JobValue* value, f64* out);
bool job_value_to_v128(const fa_JobValue* value, fa_V128* out);
bool job_value_truthy(const fa_JobValue* value);
bool job_value_matches_valtype(const fa_JobValue* value, uint8_t valtype);
bool trunc_f64_to_i32(double value, bool is_signed, u64* out);
bool trunc_f64_to_i64(double value, bool is_signed, u64* out);
u64 trunc_sat_f64_to_i32(double value, bool is_signed);
u64 trunc_sat_f64_to_i64(double value, bool is_signed);

bool pop_reg_to_buffer(fa_Job* job, void* buffer, size_t size);
bool pop_reg_u64(fa_Job* job, u64* out);
int pop_reg_u64_checked(fa_Job* job, u64* out);
void discard_reg_arguments(fa_Job* job, uint8_t count);
int pop_address_checked_typed(fa_Job* job, u64* out, bool memory64);
int pop_index_checked(fa_Job* job, bool memory64, u64* out);
int pop_u32_checked(fa_Job* job, uint32_t* out);
int pop_ref_checked(fa_Job* job, fa_ptr* out);
int push_ref_checked(fa_Job* job, fa_ptr value);
int push_v128_checked(fa_Job* job, const fa_V128* value);

int memory_bounds_check(const fa_RuntimeMemory* memory, u64 addr, size_t size);
fa_RuntimeMemory* runtime_get_memory(fa_Runtime* runtime, u64 index);
int runtime_require_memory(fa_Runtime* runtime, u64 index, fa_RuntimeMemory** memory_out);
fa_RuntimeTable* runtime_get_table(fa_Runtime* runtime, u64 index);
int runtime_decode_funcref_index(const fa_Runtime* runtime,
                                 fa_ptr encoded_ref,
                                 uint32_t* out_function_index);
int runtime_validate_table_ref_value(const fa_Runtime* runtime,
                                     const fa_RuntimeTable* table,
                                     fa_ptr ref_value);
const WasmDataSegment* runtime_get_data_segment(const fa_Runtime* runtime, u64 index);
const WasmElementSegment* runtime_get_element_segment(const fa_Runtime* runtime, u64 index);
int runtime_resolve_element_ref(const fa_Runtime* runtime,
                                const WasmElementSegment* segment,
                                uint32_t element_index,
                                fa_ptr* out);
int pop_length_checked(fa_Job* job, bool memory64, u64* out);
int pop_byte_value_checked(fa_Job* job, u8* out);

u32 rotl32(u32 value, uint8_t amount);
u32 rotr32(u32 value, uint8_t amount);
u64 rotl64(u64 value, uint8_t amount);
u64 rotr64(u64 value, uint8_t amount);
u32 clz32(u32 value);
u32 ctz32(u32 value);
u32 popcnt32(u32 value);
u64 clz64(u64 value);
u64 ctz64(u64 value);
u64 popcnt64(u64 value);

/* Opcode-family entry points used by the central descriptor registry. */
void fa_ops_numeric_register_microcode(const fa_Microcode** table);
OP_RETURN_TYPE op_arith_add_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_arith_div_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_arith_mul_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_arith_rem_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_arith_sub_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bitcount_clz_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bitcount_ctz_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bitcount_popcnt_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bitwise_and_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bitwise_or_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bitwise_xor_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_bulk_memory(OP_ARGUMENTS);
OP_RETURN_TYPE op_call(OP_ARGUMENTS);
OP_RETURN_TYPE op_call_indirect(OP_ARGUMENTS);
OP_RETURN_TYPE op_compare_eq_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_compare_ge_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_compare_gt_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_compare_le_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_compare_lt_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_compare_ne_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_const(OP_ARGUMENTS);
OP_RETURN_TYPE op_control(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f32_demote_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f32_from_i32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f32_from_i32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f32_from_i64_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f32_from_i64_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f64_from_i32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f64_from_i32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f64_from_i64_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f64_from_i64_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_f64_promote_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_extend16_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_extend8_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_f32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_f32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_f64_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_f64_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_sat_f32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_sat_f32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_sat_f64_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_trunc_sat_f64_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i32_wrap_i64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_extend16_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_extend32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_extend8_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_extend_i32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_extend_i32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_f32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_f32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_f64_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_f64_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_sat_f32_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_sat_f32_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_sat_f64_s_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_convert_i64_trunc_sat_f64_u_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_drop(OP_ARGUMENTS);
OP_RETURN_TYPE op_eqz(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_abs_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_abs_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_ceil_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_ceil_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_copysign_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_copysign_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_floor_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_floor_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_max_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_max_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_min_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_min_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_nearest_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_nearest_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_neg_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_neg_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_sqrt_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_sqrt_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_trunc_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_float_trunc_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_global(OP_ARGUMENTS);
OP_RETURN_TYPE op_load(OP_ARGUMENTS);
OP_RETURN_TYPE op_local(OP_ARGUMENTS);
OP_RETURN_TYPE op_memory_grow(OP_ARGUMENTS);
OP_RETURN_TYPE op_memory_size(OP_ARGUMENTS);
OP_RETURN_TYPE op_ref(OP_ARGUMENTS);
OP_RETURN_TYPE op_reinterpret_f32_from_i32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_reinterpret_f64_from_i64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_reinterpret_i32_from_f32_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_reinterpret_i64_from_f64_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_return(OP_ARGUMENTS);
OP_RETURN_TYPE op_rotate_left_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_rotate_right_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_select(OP_ARGUMENTS);
OP_RETURN_TYPE op_shift_left_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_shift_right_signed_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_shift_right_unsigned_mc(OP_ARGUMENTS);
OP_RETURN_TYPE op_simd(OP_ARGUMENTS);
OP_RETURN_TYPE op_store(OP_ARGUMENTS);
OP_RETURN_TYPE op_table(OP_ARGUMENTS);

/* Numeric microcode programs selected by the central dispatch table. */
extern const fa_Microcode mc_bitwise_and;
extern const fa_Microcode mc_bitwise_or;
extern const fa_Microcode mc_bitwise_xor;
extern const fa_Microcode mc_bitcount_clz;
extern const fa_Microcode mc_bitcount_ctz;
extern const fa_Microcode mc_bitcount_popcnt;
extern const fa_Microcode mc_shift_left;
extern const fa_Microcode mc_shift_right_signed;
extern const fa_Microcode mc_shift_right_unsigned;
extern const fa_Microcode mc_rotate_left;
extern const fa_Microcode mc_rotate_right;
extern const fa_Microcode mc_compare_eq;
extern const fa_Microcode mc_compare_ne;
extern const fa_Microcode mc_compare_lt;
extern const fa_Microcode mc_compare_gt;
extern const fa_Microcode mc_compare_le;
extern const fa_Microcode mc_compare_ge;
extern const fa_Microcode mc_arith_add;
extern const fa_Microcode mc_arith_sub;
extern const fa_Microcode mc_arith_mul;
extern const fa_Microcode mc_arith_div;
extern const fa_Microcode mc_arith_rem;
extern const fa_Microcode mc_convert_i32_wrap_i64;
extern const fa_Microcode mc_convert_i32_trunc_f32_s;
extern const fa_Microcode mc_convert_i32_trunc_f32_u;
extern const fa_Microcode mc_convert_i32_trunc_f64_s;
extern const fa_Microcode mc_convert_i32_trunc_f64_u;
extern const fa_Microcode mc_convert_i64_extend_i32_s;
extern const fa_Microcode mc_convert_i64_extend_i32_u;
extern const fa_Microcode mc_convert_i64_trunc_f32_s;
extern const fa_Microcode mc_convert_i64_trunc_f32_u;
extern const fa_Microcode mc_convert_i64_trunc_f64_s;
extern const fa_Microcode mc_convert_i64_trunc_f64_u;
extern const fa_Microcode mc_convert_f32_from_i32_s;
extern const fa_Microcode mc_convert_f32_from_i32_u;
extern const fa_Microcode mc_convert_f32_from_i64_s;
extern const fa_Microcode mc_convert_f32_from_i64_u;
extern const fa_Microcode mc_convert_f32_demote_f64;
extern const fa_Microcode mc_convert_f64_from_i32_s;
extern const fa_Microcode mc_convert_f64_from_i32_u;
extern const fa_Microcode mc_convert_f64_from_i64_s;
extern const fa_Microcode mc_convert_f64_from_i64_u;
extern const fa_Microcode mc_convert_f64_promote_f32;
extern const fa_Microcode mc_convert_i32_extend8_s;
extern const fa_Microcode mc_convert_i32_extend16_s;
extern const fa_Microcode mc_convert_i64_extend8_s;
extern const fa_Microcode mc_convert_i64_extend16_s;
extern const fa_Microcode mc_convert_i64_extend32_s;
extern const fa_Microcode mc_float_abs_f32;
extern const fa_Microcode mc_float_neg_f32;
extern const fa_Microcode mc_float_ceil_f32;
extern const fa_Microcode mc_float_floor_f32;
extern const fa_Microcode mc_float_trunc_f32;
extern const fa_Microcode mc_float_nearest_f32;
extern const fa_Microcode mc_float_sqrt_f32;
extern const fa_Microcode mc_float_min_f32;
extern const fa_Microcode mc_float_max_f32;
extern const fa_Microcode mc_float_copysign_f32;
extern const fa_Microcode mc_float_abs_f64;
extern const fa_Microcode mc_float_neg_f64;
extern const fa_Microcode mc_float_ceil_f64;
extern const fa_Microcode mc_float_floor_f64;
extern const fa_Microcode mc_float_trunc_f64;
extern const fa_Microcode mc_float_nearest_f64;
extern const fa_Microcode mc_float_sqrt_f64;
extern const fa_Microcode mc_float_min_f64;
extern const fa_Microcode mc_float_max_f64;
extern const fa_Microcode mc_float_copysign_f64;
extern const fa_Microcode mc_reinterpret_i32_from_f32;
extern const fa_Microcode mc_reinterpret_i64_from_f64;
extern const fa_Microcode mc_reinterpret_f32_from_i32;
extern const fa_Microcode mc_reinterpret_f64_from_i64;
extern const fa_Microcode mc_select;

void fa_ops_simd_init_dispatch(void);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif
