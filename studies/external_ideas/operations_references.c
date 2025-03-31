/// Unused files, just for reference for generate_wasm_ops.py
/// operations took from m3_compile.c

const M3OpInfo c_operations [] =
{
    M3OP( "unreachable", 0, 0,  none,   d_logOp (Unreachable),              Compile_Unreachable ),  // 0x00
    M3OP( "nop", 1, 0,  none,   d_emptyOpList,                      Compile_Nop ),          // 0x01
    M3OP( "block", 2, 0,  none,   d_emptyOpList,                      Compile_LoopOrBlock ),  // 0x02
    M3OP( "loop", 3, 0,  none,   d_logOp (Loop),                     Compile_LoopOrBlock ),  // 0x03
    M3OP( "if", 4, -1,  none,   d_emptyOpList,                      Compile_If ),           // 0x04
    M3OP( "else", 5, 0,  none,   d_emptyOpList,                      Compile_Nop ),          // 0x05

    M3OP_RESERVED, M3OP_RESERVED, M3OP_RESERVED, M3OP_RESERVED, M3OP_RESERVED,                          // 0x06...0x0a

    M3OP( "end", 6, 0,  none,   d_emptyOpList,                      Compile_End ),          // 0x06
    M3OP( "br", 7, 0,  none,   d_logOp (Branch),                   Compile_Branch ),       // 0x07
    M3OP( "br_if", 8, -1,  none,   d_logOp2 (BranchIf_r, BranchIf_s),  Compile_Branch ),       // 0x08
    M3OP( "br_table", 9, -1,  none,   d_logOp (BranchTable),              Compile_BranchTable ),  // 0x09
    M3OP( "return", 10, 0,  any,    d_logOp (Return),                   Compile_Return ),       // 0x0a
    M3OP( "call", 11, 0,  any,    d_logOp (Call),                     Compile_Call ),         // 0x0b
    M3OP( "call_indirect", 12, 0,  any,    d_logOp (CallIndirect),             Compile_CallIndirect ), // 0x0c
    M3OP( "return_call", 13, 0,  any,    d_emptyOpList,                      Compile_Call ),         // 0x0d
    M3OP( "return_call_indirect", 14, 0,  any,    d_emptyOpList,                      Compile_CallIndirect ), // 0x0e

    M3OP_RESERVED,  M3OP_RESERVED,                                                                      // 0x14...
    M3OP_RESERVED,  M3OP_RESERVED, M3OP_RESERVED, M3OP_RESERVED,                                        // ...0x19

    M3OP( "drop", 15, -1,  none,   d_emptyOpList,                      Compile_Drop ),         // 0x0f
    M3OP( "select", 16, -2,  any,    d_emptyOpList,                      Compile_Select  ),      // 0x10

    M3OP_RESERVED,  M3OP_RESERVED, M3OP_RESERVED, M3OP_RESERVED,                                        // 0x1c...0x1f

    M3OP( "local.get", 17, 1,   any,    d_emptyOpList,                      Compile_GetLocal ),     // 0x11
    M3OP( "local.set", 18, 1,   none,   d_emptyOpList,                      Compile_SetLocal ),     // 0x12
    M3OP( "local.tee", 19, 0,   any,    d_emptyOpList,                      Compile_SetLocal ),     // 0x13
    M3OP( "global.get", 20, 1,   none,   d_emptyOpList,                      Compile_GetSetGlobal ), // 0x14
    M3OP( "global.set", 21, 1,   none,   d_emptyOpList,                      Compile_GetSetGlobal ), // 0x15

    M3OP_RESERVED,  M3OP_RESERVED, M3OP_RESERVED,                                                       // 0x25...0x27

    M3OP( "i32.load", 22, 0,   i_32,   d_unaryOpList (i32, Load_i32),      Compile_Load_Store ),   // 0x16
    M3OP( "i64.load", 23, 0,   i_64,   d_unaryOpList (i64, Load_i64),      Compile_Load_Store ),   // 0x17
    M3OP_F( "f32.load", 24, 0,   f_32,   d_unaryOpList (f32, Load_f32),      Compile_Load_Store ),   // 0x18
    M3OP_F( "f64.load", 25, 0,   f_64,   d_unaryOpList (f64, Load_f64),      Compile_Load_Store ),   // 0x19

    M3OP( "i32.load8_s", 26, 0,   i_32,   d_unaryOpList (i32, Load_i8),       Compile_Load_Store ),   // 0x1a
    M3OP( "i32.load8_u", 27, 0,   i_32,   d_unaryOpList (i32, Load_u8),       Compile_Load_Store ),   // 0x1b
    M3OP( "i32.load16_s", 28, 0,   i_32,   d_unaryOpList (i32, Load_i16),      Compile_Load_Store ),   // 0x1c
    M3OP( "i32.load16_u", 29, 0,   i_32,   d_unaryOpList (i32, Load_u16),      Compile_Load_Store ),   // 0x1d

    M3OP( "i64.load8_s", 30, 0,   i_64,   d_unaryOpList (i64, Load_i8),       Compile_Load_Store ),   // 0x1e
    M3OP( "i64.load8_u", 31, 0,   i_64,   d_unaryOpList (i64, Load_u8),       Compile_Load_Store ),   // 0x1f
    M3OP( "i64.load16_s", 32, 0,   i_64,   d_unaryOpList (i64, Load_i16),      Compile_Load_Store ),   // 0x20
    M3OP( "i64.load16_u", 33, 0,   i_64,   d_unaryOpList (i64, Load_u16),      Compile_Load_Store ),   // 0x21
    M3OP( "i64.load32_s", 34, 0,   i_64,   d_unaryOpList (i64, Load_i32),      Compile_Load_Store ),   // 0x22
    M3OP( "i64.load32_u", 35, 0,   i_64,   d_unaryOpList (i64, Load_u32),      Compile_Load_Store ),   // 0x23

    M3OP( "i32.store", 36, -2,  none,   d_binOpList (i32, Store_i32),       Compile_Load_Store ),   // 0x24
    M3OP( "i64.store", 37, -2,  none,   d_binOpList (i64, Store_i64),       Compile_Load_Store ),   // 0x25
    M3OP_F( "f32.store", 38, -2,  none,   d_storeFpOpList (f32, Store_f32),   Compile_Load_Store ),   // 0x26
    M3OP_F( "f64.store", 39, -2,  none,   d_storeFpOpList (f64, Store_f64),   Compile_Load_Store ),   // 0x27

    M3OP( "i32.store8", 40, -2,  none,   d_binOpList (i32, Store_u8),        Compile_Load_Store ),   // 0x28
    M3OP( "i32.store16", 41, -2,  none,   d_binOpList (i32, Store_i16),       Compile_Load_Store ),   // 0x29

    M3OP( "i64.store8", 42, -2,  none,   d_binOpList (i64, Store_u8),        Compile_Load_Store ),   // 0x2a
    M3OP( "i64.store16", 43, -2,  none,   d_binOpList (i64, Store_i16),       Compile_Load_Store ),   // 0x2b
    M3OP( "i64.store32", 44, -2,  none,   d_binOpList (i64, Store_i32),       Compile_Load_Store ),   // 0x2c

    M3OP( "memory.size", 45, 1,   i_32,   d_logOp (MemSize),                  Compile_Memory_Size ),  // 0x2d
    M3OP( "memory.grow", 46, 1,   i_32,   d_logOp (MemGrow),                  Compile_Memory_Grow ),  // 0x2e

    M3OP( "i32.const", 47, 1,   i_32,   d_logOp (Const32),                  Compile_Const_i32 ),    // 0x2f
    M3OP( "i64.const", 48, 1,   i_64,   d_logOp (Const64),                  Compile_Const_i64 ),    // 0x30
    M3OP_F( "f32.const", 49, 1,   f_32,   d_emptyOpList,                      Compile_Const_f32 ),    // 0x31
    M3OP_F( "f64.const", 50, 1,   f_64,   d_emptyOpList,                      Compile_Const_f64 ),    // 0x32

    M3OP( "i32.eqz", 51, 0,   i_32,   d_unaryOpList (i32, EqualToZero)        , NULL  ),          // 0x33
    M3OP( "i32.eq", 52, -1,  i_32,   d_commutativeBinOpList (i32, Equal)     , NULL  ),          // 0x34
    M3OP( "i32.ne", 53, -1,  i_32,   d_commutativeBinOpList (i32, NotEqual)  , NULL  ),          // 0x35
    M3OP( "i32.lt_s", 54, -1,  i_32,   d_binOpList (i32, LessThan)             , NULL  ),          // 0x36
    M3OP( "i32.lt_u", 55, -1,  i_32,   d_binOpList (u32, LessThan)             , NULL  ),          // 0x37
    M3OP( "i32.gt_s", 56, -1,  i_32,   d_binOpList (i32, GreaterThan)          , NULL  ),          // 0x38
    M3OP( "i32.gt_u", 57, -1,  i_32,   d_binOpList (u32, GreaterThan)          , NULL  ),          // 0x39
    M3OP( "i32.le_s", 58, -1,  i_32,   d_binOpList (i32, LessThanOrEqual)      , NULL  ),          // 0x3a
    M3OP( "i32.le_u", 59, -1,  i_32,   d_binOpList (u32, LessThanOrEqual)      , NULL  ),          // 0x3b
    M3OP( "i32.ge_s", 60, -1,  i_32,   d_binOpList (i32, GreaterThanOrEqual)   , NULL  ),          // 0x3c
    M3OP( "i32.ge_u", 61, -1,  i_32,   d_binOpList (u32, GreaterThanOrEqual)   , NULL  ),          // 0x3d

    M3OP( "i64.eqz", 62, 0,   i_32,   d_unaryOpList (i64, EqualToZero)        , NULL  ),          // 0x3e
    M3OP( "i64.eq", 63, -1,  i_32,   d_commutativeBinOpList (i64, Equal)     , NULL  ),          // 0x3f
    M3OP( "i64.ne", 64, -1,  i_32,   d_commutativeBinOpList (i64, NotEqual)  , NULL  ),          // 0x40
    M3OP( "i64.lt_s", 65, -1,  i_32,   d_binOpList (i64, LessThan)             , NULL  ),          // 0x41
    M3OP( "i64.lt_u", 66, -1,  i_32,   d_binOpList (u64, LessThan)             , NULL  ),          // 0x42
    M3OP( "i64.gt_s", 67, -1,  i_32,   d_binOpList (i64, GreaterThan)          , NULL  ),          // 0x43
    M3OP( "i64.gt_u", 68, -1,  i_32,   d_binOpList (u64, GreaterThan)          , NULL  ),          // 0x44
    M3OP( "i64.le_s", 69, -1,  i_32,   d_binOpList (i64, LessThanOrEqual)      , NULL  ),          // 0x45
    M3OP( "i64.le_u", 70, -1,  i_32,   d_binOpList (u64, LessThanOrEqual)      , NULL  ),          // 0x46
    M3OP( "i64.ge_s", 71, -1,  i_32,   d_binOpList (i64, GreaterThanOrEqual)   , NULL  ),          // 0x47
    M3OP( "i64.ge_u", 72, -1,  i_32,   d_binOpList (u64, GreaterThanOrEqual)   , NULL  ),          // 0x48

    M3OP_F( "f32.eq", 73, -1,  i_32,   d_commutativeBinOpList (f32, Equal)     , NULL  ),          // 0x49
    M3OP_F( "f32.ne", 74, -1,  i_32,   d_commutativeBinOpList (f32, NotEqual)  , NULL  ),          // 0x4a
    M3OP_F( "f32.lt", 75, -1,  i_32,   d_binOpList (f32, LessThan)             , NULL  ),          // 0x4b
    M3OP_F( "f32.gt", 76, -1,  i_32,   d_binOpList (f32, GreaterThan)          , NULL  ),          // 0x4c
    M3OP_F( "f32.le", 77, -1,  i_32,   d_binOpList (f32, LessThanOrEqual)      , NULL  ),          // 0x4d
    M3OP_F( "f32.ge", 78, -1,  i_32,   d_binOpList (f32, GreaterThanOrEqual)   , NULL  ),          // 0x4e

    M3OP_F( "f64.eq", 79, -1,  i_32,   d_commutativeBinOpList (f64, Equal)     , NULL  ),          // 0x4f
    M3OP_F( "f64.ne", 80, -1,  i_32,   d_commutativeBinOpList (f64, NotEqual)  , NULL  ),          // 0x50
    M3OP_F( "f64.lt", 81, -1,  i_32,   d_binOpList (f64, LessThan)             , NULL  ),          // 0x51
    M3OP_F( "f64.gt", 82, -1,  i_32,   d_binOpList (f64, GreaterThan)          , NULL  ),          // 0x52
    M3OP_F( "f64.le", 83, -1,  i_32,   d_binOpList (f64, LessThanOrEqual)      , NULL  ),          // 0x53
    M3OP_F( "f64.ge", 84, -1,  i_32,   d_binOpList (f64, GreaterThanOrEqual)   , NULL  ),          // 0x54

    M3OP( "i32.clz", 85, 0,   i_32,   d_unaryOpList (u32, Clz)                , NULL  ),          // 0x55
    M3OP( "i32.ctz", 86, 0,   i_32,   d_unaryOpList (u32, Ctz)                , NULL  ),          // 0x56
    M3OP( "i32.popcnt", 87, 0,   i_32,   d_unaryOpList (u32, Popcnt)             , NULL  ),          // 0x57

    M3OP( "i32.add", 88, -1,  i_32,   d_commutativeBinOpList (i32, Add)       , NULL  ),          // 0x58
    M3OP( "i32.sub", 89, -1,  i_32,   d_binOpList (i32, Subtract)             , NULL  ),          // 0x59
    M3OP( "i32.mul", 90, -1,  i_32,   d_commutativeBinOpList (i32, Multiply)  , NULL  ),          // 0x5a
    M3OP( "i32.div_s", 91, -1,  i_32,   d_binOpList (i32, Divide)               , NULL  ),          // 0x5b
    M3OP( "i32.div_u", 92, -1,  i_32,   d_binOpList (u32, Divide)               , NULL  ),          // 0x5c
    M3OP( "i32.rem_s", 93, -1,  i_32,   d_binOpList (i32, Remainder)            , NULL  ),          // 0x5d
    M3OP( "i32.rem_u", 94, -1,  i_32,   d_binOpList (u32, Remainder)            , NULL  ),          // 0x5e
    M3OP( "i32.and", 95, -1,  i_32,   d_commutativeBinOpList (u32, And)       , NULL  ),          // 0x5f
    M3OP( "i32.or", 96, -1,  i_32,   d_commutativeBinOpList (u32, Or)        , NULL  ),          // 0x60
    M3OP( "i32.xor", 97, -1,  i_32,   d_commutativeBinOpList (u32, Xor)       , NULL  ),          // 0x61
    M3OP( "i32.shl", 98, -1,  i_32,   d_binOpList (u32, ShiftLeft)            , NULL  ),          // 0x62
    M3OP( "i32.shr_s", 99, -1,  i_32,   d_binOpList (i32, ShiftRight)           , NULL  ),          // 0x63
    M3OP( "i32.shr_u", 100, -1,  i_32,   d_binOpList (u32, ShiftRight)           , NULL  ),          // 0x64
    M3OP( "i32.rotl", 101, -1,  i_32,   d_binOpList (u32, Rotl)                 , NULL  ),          // 0x65
    M3OP( "i32.rotr", 102, -1,  i_32,   d_binOpList (u32, Rotr)                 , NULL  ),          // 0x66

    M3OP( "i64.clz", 103, 0,   i_64,   d_unaryOpList (u64, Clz)                , NULL  ),          // 0x67
    M3OP( "i64.ctz", 104, 0,   i_64,   d_unaryOpList (u64, Ctz)                , NULL  ),          // 0x68
    M3OP( "i64.popcnt", 105, 0,   i_64,   d_unaryOpList (u64, Popcnt)             , NULL  ),          // 0x69

    M3OP( "i64.add", 106, -1,  i_64,   d_commutativeBinOpList (i64, Add)       , NULL  ),          // 0x6a
    M3OP( "i64.sub", 107, -1,  i_64,   d_binOpList (i64, Subtract)             , NULL  ),          // 0x6b
    M3OP( "i64.mul", 108, -1,  i_64,   d_commutativeBinOpList (i64, Multiply)  , NULL  ),          // 0x6c
    M3OP( "i64.div_s", 109, -1,  i_64,   d_binOpList (i64, Divide)               , NULL  ),          // 0x6d
    M3OP( "i64.div_u", 110, -1,  i_64,   d_binOpList (u64, Divide)               , NULL  ),          // 0x6e
    M3OP( "i64.rem_s", 111, -1,  i_64,   d_binOpList (i64, Remainder)            , NULL  ),          // 0x6f
    M3OP( "i64.rem_u", 112, -1,  i_64,   d_binOpList (u64, Remainder)            , NULL  ),          // 0x70
    M3OP( "i64.and", 113, -1,  i_64,   d_commutativeBinOpList (u64, And)       , NULL  ),          // 0x71
    M3OP( "i64.or", 114, -1,  i_64,   d_commutativeBinOpList (u64, Or)        , NULL  ),          // 0x72
    M3OP( "i64.xor", 115, -1,  i_64,   d_commutativeBinOpList (u64, Xor)       , NULL  ),          // 0x73
    M3OP( "i64.shl", 116, -1,  i_64,   d_binOpList (u64, ShiftLeft)            , NULL  ),          // 0x74
    M3OP( "i64.shr_s", 117, -1,  i_64,   d_binOpList (i64, ShiftRight)           , NULL  ),          // 0x75
    M3OP( "i64.shr_u", 118, -1,  i_64,   d_binOpList (u64, ShiftRight)           , NULL  ),          // 0x76
    M3OP( "i64.rotl", 119, -1,  i_64,   d_binOpList (u64, Rotl)                 , NULL  ),          // 0x77
    M3OP( "i64.rotr", 120, -1,  i_64,   d_binOpList (u64, Rotr)                 , NULL  ),          // 0x78

    M3OP_F( "f32.abs", 121, 0,   f_32,   d_unaryOpList(f32, Abs)                 , NULL  ),          // 0x79
    M3OP_F( "f32.neg", 122, 0,   f_32,   d_unaryOpList(f32, Negate)              , NULL  ),          // 0x7a
    M3OP_F( "f32.ceil", 123, 0,   f_32,   d_unaryOpList(f32, Ceil)                , NULL  ),          // 0x7b
    M3OP_F( "f32.floor", 124, 0,   f_32,   d_unaryOpList(f32, Floor)               , NULL  ),          // 0x7c
    M3OP_F( "f32.trunc", 125, 0,   f_32,   d_unaryOpList(f32, Trunc)               , NULL  ),          // 0x7d
    M3OP_F( "f32.nearest", 126, 0,   f_32,   d_unaryOpList(f32, Nearest)             , NULL  ),          // 0x7e
    M3OP_F( "f32.sqrt", 127, 0,   f_32,   d_unaryOpList(f32, Sqrt)                , NULL  ),          // 0x7f

    M3OP_F( "f32.add", 128, -1,  f_32,   d_commutativeBinOpList (f32, Add)       , NULL  ),          // 0x80
    M3OP_F( "f32.sub", 129, -1,  f_32,   d_binOpList (f32, Subtract)             , NULL  ),          // 0x81
    M3OP_F( "f32.mul", 130, -1,  f_32,   d_commutativeBinOpList (f32, Multiply)  , NULL  ),          // 0x82
    M3OP_F( "f32.div", 131, -1,  f_32,   d_binOpList (f32, Divide)               , NULL  ),          // 0x83
    M3OP_F( "f32.min", 132, -1,  f_32,   d_commutativeBinOpList (f32, Min)       , NULL  ),          // 0x84
    M3OP_F( "f32.max", 133, -1,  f_32,   d_commutativeBinOpList (f32, Max)       , NULL  ),          // 0x85
    M3OP_F( "f32.copysign", 134, -1,  f_32,   d_binOpList (f32, CopySign)             , NULL  ),          // 0x86

    M3OP_F( "f64.abs", 135, 0,   f_64,   d_unaryOpList(f64, Abs)                 , NULL  ),          // 0x87
    M3OP_F( "f64.neg", 136, 0,   f_64,   d_unaryOpList(f64, Negate)              , NULL  ),          // 0x88
    M3OP_F( "f64.ceil", 137, 0,   f_64,   d_unaryOpList(f64, Ceil)                , NULL  ),          // 0x89
    M3OP_F( "f64.floor", 138, 0,   f_64,   d_unaryOpList(f64, Floor)               , NULL  ),          // 0x8a
    M3OP_F( "f64.trunc", 139, 0,   f_64,   d_unaryOpList(f64, Trunc)               , NULL  ),          // 0x8b
    M3OP_F( "f64.nearest", 140, 0,   f_64,   d_unaryOpList(f64, Nearest)             , NULL  ),          // 0x8c
    M3OP_F( "f64.sqrt", 141, 0,   f_64,   d_unaryOpList(f64, Sqrt)                , NULL  ),          // 0x8d

    M3OP_F( "f64.add", 142, -1,  f_64,   d_commutativeBinOpList (f64, Add)       , NULL  ),          // 0x8e
    M3OP_F( "f64.sub", 143, -1,  f_64,   d_binOpList (f64, Subtract)             , NULL  ),          // 0x8f
    M3OP_F( "f64.mul", 144, -1,  f_64,   d_commutativeBinOpList (f64, Multiply)  , NULL  ),          // 0x90
    M3OP_F( "f64.div", 145, -1,  f_64,   d_binOpList (f64, Divide)               , NULL  ),          // 0x91
    M3OP_F( "f64.min", 146, -1,  f_64,   d_commutativeBinOpList (f64, Min)       , NULL  ),          // 0x92
    M3OP_F( "f64.max", 147, -1,  f_64,   d_commutativeBinOpList (f64, Max)       , NULL  ),          // 0x93
    M3OP_F( "f64.copysign", 148, -1,  f_64,   d_binOpList (f64, CopySign)             , NULL  ),          // 0x94

    M3OP( "i32.wrap/i64", 149, 0,   i_32,   d_unaryOpList (i32, Wrap_i64),          NULL    ),          // 0x95
    M3OP_F( "i32.trunc_s/f32", 150, 0,   i_32,   d_convertOpList (i32_Trunc_f32),        Compile_Convert ),  // 0x96
    M3OP_F( "i32.trunc_u/f32", 151, 0,   i_32,   d_convertOpList (u32_Trunc_f32),        Compile_Convert ),  // 0x97
    M3OP_F( "i32.trunc_s/f64", 152, 0,   i_32,   d_convertOpList (i32_Trunc_f64),        Compile_Convert ),  // 0x98
    M3OP_F( "i32.trunc_u/f64", 153, 0,   i_32,   d_convertOpList (u32_Trunc_f64),        Compile_Convert ),  // 0x99

    M3OP( "i64.extend_s/i32", 154, 0,   i_64,   d_unaryOpList (i64, Extend_i32),        NULL    ),          // 0x9a
    M3OP( "i64.extend_u/i32", 155, 0,   i_64,   d_unaryOpList (i64, Extend_u32),        NULL    ),          // 0x9b

    M3OP_F( "i64.trunc_s/f32", 156, 0,   i_64,   d_convertOpList (i64_Trunc_f32),        Compile_Convert ),  // 0x9c
    M3OP_F( "i64.trunc_u/f32", 157, 0,   i_64,   d_convertOpList (u64_Trunc_f32),        Compile_Convert ),  // 0x9d
    M3OP_F( "i64.trunc_s/f64", 158, 0,   i_64,   d_convertOpList (i64_Trunc_f64),        Compile_Convert ),  // 0x9e
    M3OP_F( "i64.trunc_u/f64", 159, 0,   i_64,   d_convertOpList (u64_Trunc_f64),        Compile_Convert ),  // 0x9f

    M3OP_F( "f32.convert_s/i32", 160, 0,   f_32,   d_convertOpList (f32_Convert_i32),      Compile_Convert ),  // 0xa0
    M3OP_F( "f32.convert_u/i32", 161, 0,   f_32,   d_convertOpList (f32_Convert_u32),      Compile_Convert ),  // 0xa1
    M3OP_F( "f32.convert_s/i64", 162, 0,   f_32,   d_convertOpList (f32_Convert_i64),      Compile_Convert ),  // 0xa2
    M3OP_F( "f32.convert_u/i64", 163, 0,   f_32,   d_convertOpList (f32_Convert_u64),      Compile_Convert ),  // 0xa3

    M3OP_F( "f32.demote/f64", 164, 0,   f_32,   d_unaryOpList (f32, Demote_f64),        NULL    ),          // 0xa4

    M3OP_F( "f64.convert_s/i32", 165, 0,   f_64,   d_convertOpList (f64_Convert_i32),      Compile_Convert ),  // 0xa5
    M3OP_F( "f64.convert_u/i32", 166, 0,   f_64,   d_convertOpList (f64_Convert_u32),      Compile_Convert ),  // 0xa6
    M3OP_F( "f64.convert_s/i64", 167, 0,   f_64,   d_convertOpList (f64_Convert_i64),      Compile_Convert ),  // 0xa7
    M3OP_F( "f64.convert_u/i64", 168, 0,   f_64,   d_convertOpList (f64_Convert_u64),      Compile_Convert ),  // 0xa8

    M3OP_F( "f64.promote/f32", 169, 0,   f_64,   d_unaryOpList (f64, Promote_f32),       NULL    ),          // 0xa9

    M3OP_F( "i32.reinterpret/f32", 170, 0, i_32,   d_convertOpList (i32_Reinterpret_f32),  Compile_Convert ),  // 0xaa
    M3OP_F( "i64.reinterpret/f64", 171, 0, i_64,   d_convertOpList (i64_Reinterpret_f64),  Compile_Convert ),  // 0xab
    M3OP_F( "f32.reinterpret/i32", 172, 0, f_32,   d_convertOpList (f32_Reinterpret_i32),  Compile_Convert ),  // 0xac
    M3OP_F( "f64.reinterpret/i64", 173, 0, f_64,   d_convertOpList (f64_Reinterpret_i64),  Compile_Convert ),  // 0xad

    M3OP( "i32.extend8_s", 174, 0,   i_32,   d_unaryOpList (i32, Extend8_s),        NULL    ),          // 0xae
    M3OP( "i32.extend16_s", 175, 0,   i_32,   d_unaryOpList (i32, Extend16_s),       NULL    ),          // 0xaf
    M3OP( "i64.extend8_s", 176, 0,   i_64,   d_unaryOpList (i64, Extend8_s),        NULL    ),          // 0xb0
    M3OP( "i64.extend16_s", 177, 0,   i_64,   d_unaryOpList (i64, Extend16_s),       NULL    ),          // 0xb1
    M3OP( "i64.extend32_s", 178, 0,   i_64,   d_unaryOpList (i64, Extend32_s),       NULL    ),          // 0xb2

# ifdef DEBUG // for codepage logging. the order doesn't matter:
#define d_m3DebugOp(OP, IDX) M3OP (#OP, IDX, 0, none, { op_##OP })

# if d_m3HasFloat
#define d_m3DebugTypedOp(OP, IDX) M3OP (#OP, IDX, 0, none, { op_##OP##_i32, op_##OP##_i64 })
# else
#define d_m3DebugTypedOp(OP, IDX) M3OP (#OP, IDX, 0, none, { op_##OP##_i32, op_##OP##_i64 })
# endif

    d_m3DebugOp (Compile, 179),          d_m3DebugOp (Entry, 180),            d_m3DebugOp (End, 181),
    d_m3DebugOp (Unsupported, 182),      d_m3DebugOp (CallRawFunction, 183),

    d_m3DebugOp (GetGlobal_s32, 184),    d_m3DebugOp (GetGlobal_s64, 185),    d_m3DebugOp (ContinueLoop, 186),     d_m3DebugOp (ContinueLoopIf, 187),

    d_m3DebugOp (CopySlot_32, 188),      d_m3DebugOp (PreserveCopySlot_32, 189), d_m3DebugOp (If_s, 190),          d_m3DebugOp (BranchIfPrologue_s, 191),
    d_m3DebugOp (CopySlot_64, 192),      d_m3DebugOp (PreserveCopySlot_64, 193), d_m3DebugOp (If_r, 194),          d_m3DebugOp (BranchIfPrologue_r, 195),

    d_m3DebugOp (Select_i32_rss, 196),   d_m3DebugOp (Select_i32_srs, 197),   d_m3DebugOp (Select_i32_ssr, 198),   d_m3DebugOp (Select_i32_sss, 199),
    d_m3DebugOp (Select_i64_rss, 200),   d_m3DebugOp (Select_i64_srs, 201),   d_m3DebugOp (Select_i64_ssr, 202),   d_m3DebugOp (Select_i64_sss, 203),

# if d_m3HasFloat
    d_m3DebugOp (Select_f32_sss, 204),   d_m3DebugOp (Select_f32_srs, 205),   d_m3DebugOp (Select_f32_ssr, 206),
    d_m3DebugOp (Select_f32_rss, 207),   d_m3DebugOp (Select_f32_rrs, 208),   d_m3DebugOp (Select_f32_rsr, 209),

    d_m3DebugOp (Select_f64_sss, 210),   d_m3DebugOp (Select_f64_srs, 211),   d_m3DebugOp (Select_f64_ssr, 212),
    d_m3DebugOp (Select_f64_rss, 213),   d_m3DebugOp (Select_f64_rrs, 214),   d_m3DebugOp (Select_f64_rsr, 215),
# endif

    d_m3DebugOp (MemFill, 216),          d_m3DebugOp (MemCopy, 217),

    d_m3DebugTypedOp (SetGlobal, 218),   d_m3DebugOp (SetGlobal_s32, 219),    d_m3DebugOp (SetGlobal_s64, 220),

    d_m3DebugTypedOp (SetRegister, 221), d_m3DebugTypedOp (SetSlot, 222),     d_m3DebugTypedOp (PreserveSetSlot, 223),
# endif

# if d_m3CascadedOpcodes
    [c_waOp_extended] = M3OP( "0xFC", 224, 0,  c_m3Type_unknown,   d_emptyOpList,  Compile_ExtendedOpcode ),  // 0xe0
# endif

# if DEBUG
    M3OP( "termination", 225, 0,  c_m3Type_unknown ), // 0xe1
# endif

#if WASM_ENABLE_OP_TRACE && WASM_DEBUG_DumpStack_InOps
    d_m3DebugOp (DumpStack, 226)
#endif
};

const M3OpInfo c_operationsFC [] =
{
    M3OP_F( "i32.trunc_s:sat/f32", 227, 0,   i_32,   d_convertOpList (i32_TruncSat_f32),        Compile_Convert ),  // 0xe3
    M3OP_F( "i32.trunc_u:sat/f32", 228, 0,   i_32,   d_convertOpList (u32_TruncSat_f32),        Compile_Convert ),  // 0xe4
    M3OP_F( "i32.trunc_s:sat/f64", 229, 0,   i_32,   d_convertOpList (i32_TruncSat_f64),        Compile_Convert ),  // 0xe5
    M3OP_F( "i32.trunc_u:sat/f64", 230, 0,   i_32,   d_convertOpList (u32_TruncSat_f64),        Compile_Convert ),  // 0xe6
    M3OP_F( "i64.trunc_s:sat/f32", 231, 0,   i_64,   d_convertOpList (i64_TruncSat_f32),        Compile_Convert ),  // 0xe7
    M3OP_F( "i64.trunc_u:sat/f32", 232, 0,   i_64,   d_convertOpList (u64_TruncSat_f32),        Compile_Convert ),  // 0xe8
    M3OP_F( "i64.trunc_s:sat/f64", 233, 0,   i_64,   d_convertOpList (i64_TruncSat_f64),        Compile_Convert ),  // 0xe9
    M3OP_F( "i64.trunc_u:sat/f64", 234, 0,   i_64,   d_convertOpList (u64_TruncSat_f64),        Compile_Convert ),  // 0xea

    M3OP_RESERVED, M3OP_RESERVED,

    M3OP( "memory.copy", 235, 0,   none,   d_emptyOpList,                           Compile_Memory_CopyFill ), // 0xeb
    M3OP( "memory.fill", 236, 0,   none,   d_emptyOpList,                           Compile_Memory_CopyFill ), // 0xec


# ifdef DEBUG
    M3OP( "termination", 237, 0,  c_m3Type_unknown ) // 0xed
# endif
};



// temporary always allocated
#if (DEBUG && M3_FUNCTIONS_ENUM) || 1
enum M3OpNames {
    M3OP_NAME_UNREACHABLE = 0,
    M3OP_NAME_NOP = 1,
    M3OP_NAME_BLOCK = 2,
    M3OP_NAME_LOOP = 3,
    M3OP_NAME_IF = 4,
    M3OP_NAME_ELSE = 5,
    M3OP_NAME_END = 6,
    M3OP_NAME_BR = 7,
    M3OP_NAME_BR_IF = 8,
    M3OP_NAME_BR_TABLE = 9,
    M3OP_NAME_RETURN = 10,
    M3OP_NAME_CALL = 11,
    M3OP_NAME_CALL_INDIRECT = 12,
    M3OP_NAME_RETURN_CALL = 13,
    M3OP_NAME_RETURN_CALL_INDIRECT = 14,
    M3OP_NAME_DROP = 15,
    M3OP_NAME_SELECT = 16,
    M3OP_NAME_LOCAL_GET = 17,
    M3OP_NAME_LOCAL_SET = 18,
    M3OP_NAME_LOCAL_TEE = 19,
    M3OP_NAME_GLOBAL_GET = 20,
    M3OP_NAME_GLOBAL_SET = 21,
    M3OP_NAME_I32_LOAD = 22,
    M3OP_NAME_I64_LOAD = 23,
    M3OP_NAME_F32_LOAD = 24,
    M3OP_NAME_F64_LOAD = 25,
    M3OP_NAME_I32_LOAD8_S = 26,
    M3OP_NAME_I32_LOAD8_U = 27,
    M3OP_NAME_I32_LOAD16_S = 28,
    M3OP_NAME_I32_LOAD16_U = 29,
    M3OP_NAME_I64_LOAD8_S = 30,
    M3OP_NAME_I64_LOAD8_U = 31,
    M3OP_NAME_I64_LOAD16_S = 32,
    M3OP_NAME_I64_LOAD16_U = 33,
    M3OP_NAME_I64_LOAD32_S = 34,
    M3OP_NAME_I64_LOAD32_U = 35,
    M3OP_NAME_I32_STORE = 36,
    M3OP_NAME_I64_STORE = 37,
    M3OP_NAME_F32_STORE = 38,
    M3OP_NAME_F64_STORE = 39,
    M3OP_NAME_I32_STORE8 = 40,
    M3OP_NAME_I32_STORE16 = 41,
    M3OP_NAME_I64_STORE8 = 42,
    M3OP_NAME_I64_STORE16 = 43,
    M3OP_NAME_I64_STORE32 = 44,
    M3OP_NAME_MEMORY_SIZE = 45,
    M3OP_NAME_MEMORY_GROW = 46,
    M3OP_NAME_I32_CONST = 47,
    M3OP_NAME_I64_CONST = 48,
    M3OP_NAME_F32_CONST = 49,
    M3OP_NAME_F64_CONST = 50,
    M3OP_NAME_I32_EQZ = 51,
    M3OP_NAME_I32_EQ = 52,
    M3OP_NAME_I32_NE = 53,
    M3OP_NAME_I32_LT_S = 54,
    M3OP_NAME_I32_LT_U = 55,
    M3OP_NAME_I32_GT_S = 56,
    M3OP_NAME_I32_GT_U = 57,
    M3OP_NAME_I32_LE_S = 58,
    M3OP_NAME_I32_LE_U = 59,
    M3OP_NAME_I32_GE_S = 60,
    M3OP_NAME_I32_GE_U = 61,
    M3OP_NAME_I64_EQZ = 62,
    M3OP_NAME_I64_EQ = 63,
    M3OP_NAME_I64_NE = 64,
    M3OP_NAME_I64_LT_S = 65,
    M3OP_NAME_I64_LT_U = 66,
    M3OP_NAME_I64_GT_S = 67,
    M3OP_NAME_I64_GT_U = 68,
    M3OP_NAME_I64_LE_S = 69,
    M3OP_NAME_I64_LE_U = 70,
    M3OP_NAME_I64_GE_S = 71,
    M3OP_NAME_I64_GE_U = 72,
    M3OP_NAME_F32_EQ = 73,
    M3OP_NAME_F32_NE = 74,
    M3OP_NAME_F32_LT = 75,
    M3OP_NAME_F32_GT = 76,
    M3OP_NAME_F32_LE = 77,
    M3OP_NAME_F32_GE = 78,
    M3OP_NAME_F64_EQ = 79,
    M3OP_NAME_F64_NE = 80,
    M3OP_NAME_F64_LT = 81,
    M3OP_NAME_F64_GT = 82,
    M3OP_NAME_F64_LE = 83,
    M3OP_NAME_F64_GE = 84,
    M3OP_NAME_I32_CLZ = 85,
    M3OP_NAME_I32_CTZ = 86,
    M3OP_NAME_I32_POPCNT = 87,
    M3OP_NAME_I32_ADD = 88,
    M3OP_NAME_I32_SUB = 89,
    M3OP_NAME_I32_MUL = 90,
    M3OP_NAME_I32_DIV_S = 91,
    M3OP_NAME_I32_DIV_U = 92,
    M3OP_NAME_I32_REM_S = 93,
    M3OP_NAME_I32_REM_U = 94,
    M3OP_NAME_I32_AND = 95,
    M3OP_NAME_I32_OR = 96,
    M3OP_NAME_I32_XOR = 97,
    M3OP_NAME_I32_SHL = 98,
    M3OP_NAME_I32_SHR_S = 99,
    M3OP_NAME_I32_SHR_U = 100,
    M3OP_NAME_I32_ROTL = 101,
    M3OP_NAME_I32_ROTR = 102,
    M3OP_NAME_I64_CLZ = 103,
    M3OP_NAME_I64_CTZ = 104,
    M3OP_NAME_I64_POPCNT = 105,
    M3OP_NAME_I64_ADD = 106,
    M3OP_NAME_I64_SUB = 107,
    M3OP_NAME_I64_MUL = 108,
    M3OP_NAME_I64_DIV_S = 109,
    M3OP_NAME_I64_DIV_U = 110,
    M3OP_NAME_I64_REM_S = 111,
    M3OP_NAME_I64_REM_U = 112,
    M3OP_NAME_I64_AND = 113,
    M3OP_NAME_I64_OR = 114,
    M3OP_NAME_I64_XOR = 115,
    M3OP_NAME_I64_SHL = 116,
    M3OP_NAME_I64_SHR_S = 117,
    M3OP_NAME_I64_SHR_U = 118,
    M3OP_NAME_I64_ROTL = 119,
    M3OP_NAME_I64_ROTR = 120,
    M3OP_NAME_F32_ABS = 121,
    M3OP_NAME_F32_NEG = 122,
    M3OP_NAME_F32_CEIL = 123,
    M3OP_NAME_F32_FLOOR = 124,
    M3OP_NAME_F32_TRUNC = 125,
    M3OP_NAME_F32_NEAREST = 126,
    M3OP_NAME_F32_SQRT = 127,
    M3OP_NAME_F32_ADD = 128,
    M3OP_NAME_F32_SUB = 129,
    M3OP_NAME_F32_MUL = 130,
    M3OP_NAME_F32_DIV = 131,
    M3OP_NAME_F32_MIN = 132,
    M3OP_NAME_F32_MAX = 133,
    M3OP_NAME_F32_COPYSIGN = 134,
    M3OP_NAME_F64_ABS = 135,
    M3OP_NAME_F64_NEG = 136,
    M3OP_NAME_F64_CEIL = 137,
    M3OP_NAME_F64_FLOOR = 138,
    M3OP_NAME_F64_TRUNC = 139,
    M3OP_NAME_F64_NEAREST = 140,
    M3OP_NAME_F64_SQRT = 141,
    M3OP_NAME_F64_ADD = 142,
    M3OP_NAME_F64_SUB = 143,
    M3OP_NAME_F64_MUL = 144,
    M3OP_NAME_F64_DIV = 145,
    M3OP_NAME_F64_MIN = 146,
    M3OP_NAME_F64_MAX = 147,
    M3OP_NAME_F64_COPYSIGN = 148,
    M3OP_NAME_I32_WRAP_I64 = 149,
    M3OP_NAME_I32_TRUNC_S_F32 = 150,
    M3OP_NAME_I32_TRUNC_U_F32 = 151,
    M3OP_NAME_I32_TRUNC_S_F64 = 152,
    M3OP_NAME_I32_TRUNC_U_F64 = 153,
    M3OP_NAME_I64_EXTEND_S_I32 = 154,
    M3OP_NAME_I64_EXTEND_U_I32 = 155,
    M3OP_NAME_I64_TRUNC_S_F32 = 156,
    M3OP_NAME_I64_TRUNC_U_F32 = 157,
    M3OP_NAME_I64_TRUNC_S_F64 = 158,
    M3OP_NAME_I64_TRUNC_U_F64 = 159,
    M3OP_NAME_F32_CONVERT_S_I32 = 160,
    M3OP_NAME_F32_CONVERT_U_I32 = 161,
    M3OP_NAME_F32_CONVERT_S_I64 = 162,
    M3OP_NAME_F32_CONVERT_U_I64 = 163,
    M3OP_NAME_F32_DEMOTE_F64 = 164,
    M3OP_NAME_F64_CONVERT_S_I32 = 165,
    M3OP_NAME_F64_CONVERT_U_I32 = 166,
    M3OP_NAME_F64_CONVERT_S_I64 = 167,
    M3OP_NAME_F64_CONVERT_U_I64 = 168,
    M3OP_NAME_F64_PROMOTE_F32 = 169,
    M3OP_NAME_I32_REINTERPRET_F32 = 170,
    M3OP_NAME_I64_REINTERPRET_F64 = 171,
    M3OP_NAME_F32_REINTERPRET_I32 = 172,
    M3OP_NAME_F64_REINTERPRET_I64 = 173,
    M3OP_NAME_I32_EXTEND8_S = 174,
    M3OP_NAME_I32_EXTEND16_S = 175,
    M3OP_NAME_I64_EXTEND8_S = 176,
    M3OP_NAME_I64_EXTEND16_S = 177,
    M3OP_NAME_I64_EXTEND32_S = 178,
    M3OP_NAME_COMPILE = 179,
    M3OP_NAME_ENTRY = 180,
    M3OP_NAME_UNSUPPORTED = 182,
    M3OP_NAME_CALLRAWFUNCTION = 183,
    M3OP_NAME_GETGLOBAL_S32 = 184,
    M3OP_NAME_GETGLOBAL_S64 = 185,
    M3OP_NAME_CONTINUELOOP = 186,
    M3OP_NAME_CONTINUELOOPIF = 187,
    M3OP_NAME_COPYSLOT_32 = 188,
    M3OP_NAME_PRESERVECOPYSLOT_32 = 189,
    M3OP_NAME_IF_S = 190,
    M3OP_NAME_BRANCHIFPROLOGUE_S = 191,
    M3OP_NAME_COPYSLOT_64 = 192,
    M3OP_NAME_PRESERVECOPYSLOT_64 = 193,
    M3OP_NAME_IF_R = 194,
    M3OP_NAME_BRANCHIFPROLOGUE_R = 195,
    M3OP_NAME_SELECT_I32_RSS = 196,
    M3OP_NAME_SELECT_I32_SRS = 197,
    M3OP_NAME_SELECT_I32_SSR = 198,
    M3OP_NAME_SELECT_I32_SSS = 199,
    M3OP_NAME_SELECT_I64_RSS = 200,
    M3OP_NAME_SELECT_I64_SRS = 201,
    M3OP_NAME_SELECT_I64_SSR = 202,
    M3OP_NAME_SELECT_I64_SSS = 203,
    M3OP_NAME_SELECT_F32_SSS = 204,
    M3OP_NAME_SELECT_F32_SRS = 205,
    M3OP_NAME_SELECT_F32_SSR = 206,
    M3OP_NAME_SELECT_F32_RSS = 207,
    M3OP_NAME_SELECT_F32_RRS = 208,
    M3OP_NAME_SELECT_F32_RSR = 209,
    M3OP_NAME_SELECT_F64_SSS = 210,
    M3OP_NAME_SELECT_F64_SRS = 211,
    M3OP_NAME_SELECT_F64_SSR = 212,
    M3OP_NAME_SELECT_F64_RSS = 213,
    M3OP_NAME_SELECT_F64_RRS = 214,
    M3OP_NAME_SELECT_F64_RSR = 215,
    M3OP_NAME_MEMFILL = 216,
    M3OP_NAME_MEMCOPY = 217,
    M3OP_NAME_SETGLOBAL = 218,
    M3OP_NAME_SETGLOBAL_S32 = 219,
    M3OP_NAME_SETGLOBAL_S64 = 220,
    M3OP_NAME_SETREGISTER = 221,
    M3OP_NAME_SETSLOT = 222,
    M3OP_NAME_PRESERVESETSLOT = 223,
    M3OP_NAME_0XFC = 224,
    M3OP_NAME_DUMPSTACK = 226,
    M3OP_NAME_I32_TRUNC_S_SAT_F32 = 227,
    M3OP_NAME_I32_TRUNC_U_SAT_F32 = 228,
    M3OP_NAME_I32_TRUNC_S_SAT_F64 = 229,
    M3OP_NAME_I32_TRUNC_U_SAT_F64 = 230,
    M3OP_NAME_I64_TRUNC_S_SAT_F32 = 231,
    M3OP_NAME_I64_TRUNC_U_SAT_F32 = 232,
    M3OP_NAME_I64_TRUNC_S_SAT_F64 = 233,
    M3OP_NAME_I64_TRUNC_U_SAT_F64 = 234,
    M3OP_NAME_MEMORY_COPY = 235,
    M3OP_NAME_MEMORY_FILL = 236,
    M3OP_NAME_TERMINATION = 237,
};

// Auto-generated array of operation names
static cstr_t opNames[] __attribute__((section(".rodata"))) = {
    "unreachable",
    "nop",
    "block",
    "loop",
    "if",
    "else",
    "end",
    "br",
    "br_if",
    "br_table",
    "return",
    "call",
    "call_indirect",
    "return_call",
    "return_call_indirect",
    "drop",
    "select",
    "local.get",
    "local.set",
    "local.tee",
    "global.get",
    "global.set",
    "i32.load",
    "i64.load",
    "f32.load",
    "f64.load",
    "i32.load8_s",
    "i32.load8_u",
    "i32.load16_s",
    "i32.load16_u",
    "i64.load8_s",
    "i64.load8_u",
    "i64.load16_s",
    "i64.load16_u",
    "i64.load32_s",
    "i64.load32_u",
    "i32.store",
    "i64.store",
    "f32.store",
    "f64.store",
    "i32.store8",
    "i32.store16",
    "i64.store8",
    "i64.store16",
    "i64.store32",
    "memory.size",
    "memory.grow",
    "i32.const",
    "i64.const",
    "f32.const",
    "f64.const",
    "i32.eqz",
    "i32.eq",
    "i32.ne",
    "i32.lt_s",
    "i32.lt_u",
    "i32.gt_s",
    "i32.gt_u",
    "i32.le_s",
    "i32.le_u",
    "i32.ge_s",
    "i32.ge_u",
    "i64.eqz",
    "i64.eq",
    "i64.ne",
    "i64.lt_s",
    "i64.lt_u",
    "i64.gt_s",
    "i64.gt_u",
    "i64.le_s",
    "i64.le_u",
    "i64.ge_s",
    "i64.ge_u",
    "f32.eq",
    "f32.ne",
    "f32.lt",
    "f32.gt",
    "f32.le",
    "f32.ge",
    "f64.eq",
    "f64.ne",
    "f64.lt",
    "f64.gt",
    "f64.le",
    "f64.ge",
    "i32.clz",
    "i32.ctz",
    "i32.popcnt",
    "i32.add",
    "i32.sub",
    "i32.mul",
    "i32.div_s",
    "i32.div_u",
    "i32.rem_s",
    "i32.rem_u",
    "i32.and",
    "i32.or",
    "i32.xor",
    "i32.shl",
    "i32.shr_s",
    "i32.shr_u",
    "i32.rotl",
    "i32.rotr",
    "i64.clz",
    "i64.ctz",
    "i64.popcnt",
    "i64.add",
    "i64.sub",
    "i64.mul",
    "i64.div_s",
    "i64.div_u",
    "i64.rem_s",
    "i64.rem_u",
    "i64.and",
    "i64.or",
    "i64.xor",
    "i64.shl",
    "i64.shr_s",
    "i64.shr_u",
    "i64.rotl",
    "i64.rotr",
    "f32.abs",
    "f32.neg",
    "f32.ceil",
    "f32.floor",
    "f32.trunc",
    "f32.nearest",
    "f32.sqrt",
    "f32.add",
    "f32.sub",
    "f32.mul",
    "f32.div",
    "f32.min",
    "f32.max",
    "f32.copysign",
    "f64.abs",
    "f64.neg",
    "f64.ceil",
    "f64.floor",
    "f64.trunc",
    "f64.nearest",
    "f64.sqrt",
    "f64.add",
    "f64.sub",
    "f64.mul",
    "f64.div",
    "f64.min",
    "f64.max",
    "f64.copysign",
    "i32.wrap/i64",
    "i32.trunc_s/f32",
    "i32.trunc_u/f32",
    "i32.trunc_s/f64",
    "i32.trunc_u/f64",
    "i64.extend_s/i32",
    "i64.extend_u/i32",
    "i64.trunc_s/f32",
    "i64.trunc_u/f32",
    "i64.trunc_s/f64",
    "i64.trunc_u/f64",
    "f32.convert_s/i32",
    "f32.convert_u/i32",
    "f32.convert_s/i64",
    "f32.convert_u/i64",
    "f32.demote/f64",
    "f64.convert_s/i32",
    "f64.convert_u/i32",
    "f64.convert_s/i64",
    "f64.convert_u/i64",
    "f64.promote/f32",
    "i32.reinterpret/f32",
    "i64.reinterpret/f64",
    "f32.reinterpret/i32",
    "f64.reinterpret/i64",
    "i32.extend8_s",
    "i32.extend16_s",
    "i64.extend8_s",
    "i64.extend16_s",
    "i64.extend32_s",
    "Compile",
    "Entry",
    "End",
    "Unsupported",
    "CallRawFunction",
    "GetGlobal_s32",
    "GetGlobal_s64",
    "ContinueLoop",
    "ContinueLoopIf",
    "CopySlot_32",
    "PreserveCopySlot_32",
    "If_s",
    "BranchIfPrologue_s",
    "CopySlot_64",
    "PreserveCopySlot_64",
    "If_r",
    "BranchIfPrologue_r",
    "Select_i32_rss",
    "Select_i32_srs",
    "Select_i32_ssr",
    "Select_i32_sss",
    "Select_i64_rss",
    "Select_i64_srs",
    "Select_i64_ssr",
    "Select_i64_sss",
    "Select_f32_sss",
    "Select_f32_srs",
    "Select_f32_ssr",
    "Select_f32_rss",
    "Select_f32_rrs",
    "Select_f32_rsr",
    "Select_f64_sss",
    "Select_f64_srs",
    "Select_f64_ssr",
    "Select_f64_rss",
    "Select_f64_rrs",
    "Select_f64_rsr",
    "MemFill",
    "MemCopy",
    "SetGlobal",
    "SetGlobal_s32",
    "SetGlobal_s64",
    "SetRegister",
    "SetSlot",
    "PreserveSetSlot",
    "0xFC",
    "DumpStack",
    "i32.trunc_s:sat/f32",
    "i32.trunc_u:sat/f32",
    "i32.trunc_s:sat/f64",
    "i32.trunc_u:sat/f64",
    "i64.trunc_s:sat/f32",
    "i64.trunc_u:sat/f32",
    "i64.trunc_s:sat/f64",
    "i64.trunc_u:sat/f64",
    "memory.copy",
    "memory.fill",
    "termination",
};

// Auto-generated getter function
static cstr_t str_unkwown = "unkwon";
static cstr_t getOpName(uint8_t id) {
    if(id < 0) return str_unkwown;
    return opNames[id];
}
#endif
