//
//  Adapted from: m3_compile.h (WASM3)
//
//  Created by Steven Massey on 4/17/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#pragma once

#include <stdbool.h>

#include "wasm3.h"
#include "m3_code.h"
#include "m3_exec_defs.h"
#include "m3_function.h"

d_m3BeginExternC

#if DISABLE_WASM3_INLINE
    #define WASM3_STATIC static
    #define WASM3_STATIC_INLINE static
#else
    #define WASM3_STATIC static
    #define WASM3_STATIC_INLINE static inline
#endif

#if WASM_ENABLE_OP_TRACE
#define WASM3_STATIC_DEBUG
#else
#define WASM3_STATIC_DEBUG WASM3_STATIC
#endif

enum
{
    c_waOp_block                = 0x02,
    c_waOp_loop                 = 0x03,
    c_waOp_if                   = 0x04,
    c_waOp_else                 = 0x05,
    c_waOp_end                  = 0x0b,
    c_waOp_branch               = 0x0c,
    c_waOp_branchTable          = 0x0e,
    c_waOp_branchIf             = 0x0d,
    c_waOp_call                 = 0x10,
    c_waOp_getLocal             = 0x20,
    c_waOp_setLocal             = 0x21,
    c_waOp_teeLocal             = 0x22,

    c_waOp_getGlobal            = 0x23,

    c_waOp_store_f32            = 0x38,
    c_waOp_store_f64            = 0x39,

    c_waOp_i32_const            = 0x41,
    c_waOp_i64_const            = 0x42,
    c_waOp_f32_const            = 0x43,
    c_waOp_f64_const            = 0x44,

    c_waOp_extended             = 0xfc,

    c_waOp_memoryCopy           = 0xfc0a,
    c_waOp_memoryFill           = 0xfc0b
};


#define d_FuncRetType(ftype,i)  ((ftype)->types[(i)])
#define d_FuncArgType(ftype,i)  ((ftype)->types[(ftype)->numRets + (i)])

//-----------------------------------------------------------------------------------------------------------------------------------

typedef struct M3CompilationScope
{
    struct M3CompilationScope *     outer;

    pc_t                            pc;                 // used by ContinueLoop's
    pc_t                            patches;
    i32                             depth;
    u16                             exitStackIndex;
    u16                             blockStackIndex;
//    u16                             topSlot;
    IM3FuncType                     type;
    m3opcode_t                      opcode;
    bool                            isPolymorphic;
}
M3CompilationScope;

typedef M3CompilationScope *        IM3CompilationScope;

typedef struct
{
    IM3Runtime          runtime;
    IM3Module           module;

    bytes_t             wasm;
    bytes_t             wasmEnd;
    bytes_t             lastOpcodeStart;

    M3CompilationScope  block;

    IM3Function         function;

    IM3CodePage         page;

#if DEBUG
    u32                 numEmits;
    u32                 numOpcodes;
#endif

    u16                 stackFirstDynamicIndex;     // args and locals are pushed to the stack so that their slot locations can be tracked. the wasm model itself doesn't
                                                    // treat these values as being on the stack, so stackFirstDynamicIndex marks the start of the real Wasm stack
    u16                 stackIndex;                 // current stack top

    u16                 slotFirstConstIndex;
    u16                 slotMaxConstIndex;          // as const's are encountered during compilation this tracks their location in the "real" stack

    u16                 slotFirstLocalIndex;
    u16                 slotFirstDynamicIndex;      // numArgs + numLocals + numReservedConstants. the first mutable slot available to the compiler.

    u16                 maxStackSlots;

    m3slot_t            constants                   [d_m3MaxConstantTableSize];

    // 'wasmStack' holds slot locations
    u16                 wasmStack                   [d_m3MaxFunctionStackHeight];
    u8                  typeStack                   [d_m3MaxFunctionStackHeight];

    // 'm3Slots' contains allocation usage counts
    u8                  m3Slots                     [d_m3MaxFunctionSlots];

    u16                 slotMaxAllocatedIndexPlusOne;

    u16                 regStackIndexPlusOne        [2];

    m3opcode_t          previousOpcode;
}
M3Compilation;

typedef M3Compilation *                 IM3Compilation;

typedef M3Result (* M3Compiler)         (IM3Compilation, m3opcode_t);


//-----------------------------------------------------------------------------------------------------------------------------------


typedef struct M3OpInfo
{
    #if DEBUG
    #if !M3_FUNCTIONS_ENUM
    const char * const  name;
    #endif    
    #endif

    int idx; // before in debug

    i8                      stackOffset;
    u8                      type;

    // for most operations:
    // [0]= top operand in register, [1]= top operand in stack, [2]= both operands in stack
    IM3Operation            operations [4];

    M3Compiler              compiler;
}
M3OpInfo;

typedef const M3OpInfo *    IM3OpInfo;

IM3OpInfo  GetOpInfo  (m3opcode_t opcode);

// TODO: This helper should be removed, when MultiValue is implemented
WASM3_STATIC_INLINE
u8 GetSingleRetType(IM3FuncType ftype) {
    return (ftype && ftype->numRets) ? ftype->types[0] : (u8)c_m3Type_none;
}

WASM3_STATIC const u16 c_m3RegisterUnallocated = 0;
WASM3_STATIC const u16 c_slotUnused = 0xffff;

DEBUG_TYPE WASM_DEBUG_IsRegisterAllocated = WASM_DEBUG_ALL || (WASM_DEBUG && false);
WASM3_STATIC_INLINE bool  IsRegisterAllocated  (IM3Compilation o, u32 i_register)
{
    u16 reg = o->regStackIndexPlusOne[i_register]; 
    bool res = (o->regStackIndexPlusOne [i_register] != c_m3RegisterUnallocated);
    if(WASM_DEBUG_IsRegisterAllocated) {
        ESP_LOGI("WASM3", "IsRegisterAllocated: %d != %d ? %d", reg, c_m3RegisterUnallocated, res);
        //backtrace();
    }
    return res;
}

WASM3_STATIC_INLINE
bool  IsStackPolymorphic  (IM3Compilation o)
{
    return o->block.isPolymorphic;
}

WASM3_STATIC_INLINE bool  IsRegisterSlotAlias        (u16 i_slot)    { return (i_slot >= d_m3Reg0SlotAlias and i_slot != c_slotUnused); }
WASM3_STATIC_INLINE bool  IsFpRegisterSlotAlias      (u16 i_slot)    { return (i_slot == d_m3Fp0SlotAlias);  }
WASM3_STATIC_INLINE bool  IsIntRegisterSlotAlias     (u16 i_slot)    { return (i_slot == d_m3Reg0SlotAlias); }

#if DEBUG
    #if M3_FUNCTIONS_ENUM                  
        #define M3OP(name, ...) { __VA_ARGS__ }
        #define M3OP_RESERVED   { -2 }
    #else
        #define M3OP(...) { __VA_ARGS__ }
        #define M3OP_RESERVED { "reserved", -2 }
    #endif    
#else
    // previously M3OP(name, idx, ...)
    #define M3OP(name, ...) { __VA_ARGS__ }
    #define M3OP_RESERVED   { 0 }
#endif

#if d_m3HasFloat
    #define M3OP_F          M3OP
#elif d_m3NoFloatDynamic
    #define M3OP_F(n, i, o,t,op,...)        M3OP(n, i, o, t, { op_Unsupported, op_Unsupported, op_Unsupported, op_Unsupported }, __VA_ARGS__)
#else
    #define M3OP_F(...)     { -3, 0 }
#endif

//-----------------------------------------------------------------------------------------------------------------------------------

u16         GetMaxUsedSlotPlusOne       (IM3Compilation o);

M3Result    CompileBlock                (IM3Compilation io, IM3FuncType i_blockType, m3opcode_t i_blockOpcode);

M3Result    CompileBlockStatements      (IM3Compilation io);
M3Result    CompileFunction             (IM3Function io_function);

M3Result    CompileRawFunction          (IM3Module io_module, IM3Function io_function, const void * i_function, const void * i_userdata);

///
/// For debug purposes
///

WASM3_STATIC_DEBUG M3Result  Compile_Return  (IM3Compilation o, m3opcode_t i_opcode);
WASM3_STATIC_DEBUG M3Result  Compile_Const_i32  (IM3Compilation o, m3opcode_t i_opcode);

d_m3EndExternC
