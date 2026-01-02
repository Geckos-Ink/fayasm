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

// Essential callbacks
typedef ptr (*fa_Malloc)(int);
typedef void (*fa_Free)(ptr);
