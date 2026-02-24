#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

typedef double          f64;
typedef float           f32;
typedef uint64_t        u64;
typedef int64_t         i64;
typedef uint32_t        u32;
typedef int32_t         i32;
typedef short unsigned  u16;
typedef short           i16;
typedef uint8_t         u8;
typedef int8_t          i8;

typedef uintptr_t fa_ptr;
typedef void* ptr; // just the current system pointer

/*
 * funcref encoding keeps null unambiguous with function index 0:
 *   null -> 0
 *   function index n -> n + 1
 */
static inline bool fa_funcref_encode_u32(u32 function_index, fa_ptr* out) {
    if (!out) {
        return false;
    }
    if ((u64)function_index + 1U > (u64)UINTPTR_MAX) {
        return false;
    }
    *out = (fa_ptr)((u64)function_index + 1U);
    return true;
}

static inline bool fa_funcref_decode_u32(fa_ptr encoded_ref, u32* out_function_index) {
    if (!out_function_index || encoded_ref == 0) {
        return false;
    }
    const u64 decoded = (u64)encoded_ref - 1U;
    if (decoded > (u64)UINT32_MAX) {
        return false;
    }
    *out_function_index = (u32)decoded;
    return true;
}

// Essential callbacks
typedef ptr (*fa_Malloc)(int);
typedef void (*fa_Free)(ptr);
