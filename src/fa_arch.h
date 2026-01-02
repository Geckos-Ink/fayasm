#pragma once

#include <stdint.h>

// Override with compile definitions if needed (e.g., -DFA_ARCH_PTR_BYTES=8).
#ifndef FA_ARCH_PTR_BYTES
    #if defined(FA_ARCH_PTR_BITS)
        #define FA_ARCH_PTR_BYTES (FA_ARCH_PTR_BITS / 8)
    #elif defined(FA_ARCH_BYTES)
        #define FA_ARCH_PTR_BYTES FA_ARCH_BYTES
    #elif defined(__SIZEOF_POINTER__)
        #define FA_ARCH_PTR_BYTES __SIZEOF_POINTER__
    #elif INTPTR_MAX == INT64_MAX
        #define FA_ARCH_PTR_BYTES 8
    #elif INTPTR_MAX == INT32_MAX
        #define FA_ARCH_PTR_BYTES 4
    #else
        #define FA_ARCH_PTR_BYTES ((int)sizeof(void*))
    #endif
#endif

#ifndef FA_ARCH_PTR_BITS
    #define FA_ARCH_PTR_BITS (FA_ARCH_PTR_BYTES * 8)
#endif

#ifndef FA_ARCH_IS_64BIT
    #define FA_ARCH_IS_64BIT (FA_ARCH_PTR_BITS == 64)
#endif

#ifndef FA_ARCH_IS_32BIT
    #define FA_ARCH_IS_32BIT (FA_ARCH_PTR_BITS == 32)
#endif

#ifndef FA_ARCH_LITTLE_ENDIAN
    #if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
        #define FA_ARCH_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    #elif defined(_WIN32)
        #define FA_ARCH_LITTLE_ENDIAN 1
    #else
        #define FA_ARCH_LITTLE_ENDIAN 1
    #endif
#endif

#ifndef FA_ARCH_BIG_ENDIAN
    #define FA_ARCH_BIG_ENDIAN (!FA_ARCH_LITTLE_ENDIAN)
#endif

#ifndef FA_ARCH_CPU_X86_64
    #if defined(__x86_64__) || defined(_M_X64)
        #define FA_ARCH_CPU_X86_64 1
    #endif
#endif

#ifndef FA_ARCH_CPU_X86
    #if defined(__i386__) || defined(_M_IX86)
        #define FA_ARCH_CPU_X86 1
    #endif
#endif

#ifndef FA_ARCH_CPU_AARCH64
    #if defined(__aarch64__) || defined(_M_ARM64)
        #define FA_ARCH_CPU_AARCH64 1
    #endif
#endif

#ifndef FA_ARCH_CPU_ARM
    #if defined(__arm__) || defined(_M_ARM)
        #define FA_ARCH_CPU_ARM 1
    #endif
#endif

#ifndef FA_ARCH_CPU_RISCV
    #if defined(__riscv)
        #define FA_ARCH_CPU_RISCV 1
    #endif
#endif

#ifndef FA_ARCH_CPU_XTENSA
    #if defined(__XTENSA__)
        #define FA_ARCH_CPU_XTENSA 1
    #endif
#endif

#ifndef FA_ARCH_CPU_WASM
    #if defined(__wasm__)
        #define FA_ARCH_CPU_WASM 1
    #endif
#endif

#ifndef FA_ARCH_CPU_UNKNOWN
    #if !defined(FA_ARCH_CPU_X86_64) && !defined(FA_ARCH_CPU_X86) && \
        !defined(FA_ARCH_CPU_AARCH64) && !defined(FA_ARCH_CPU_ARM) && \
        !defined(FA_ARCH_CPU_RISCV) && !defined(FA_ARCH_CPU_XTENSA) && \
        !defined(FA_ARCH_CPU_WASM)
        #define FA_ARCH_CPU_UNKNOWN 1
    #endif
#endif
