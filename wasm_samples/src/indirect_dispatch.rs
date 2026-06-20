#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

extern "C" fn op_add(a: i32, b: i32) -> i32 {
    a + b
}
extern "C" fn op_sub(a: i32, b: i32) -> i32 {
    a - b
}
extern "C" fn op_mul(a: i32, b: i32) -> i32 {
    a * b
}
extern "C" fn op_max(a: i32, b: i32) -> i32 {
    if a > b {
        a
    } else {
        b
    }
}

/// Function-pointer table -> funcref table + element segment + call_indirect.
static K_OPS: [extern "C" fn(i32, i32) -> i32; 4] = [op_add, op_sub, op_mul, op_max];

#[no_mangle]
pub extern "C" fn sample_dispatch(sel: i32, a: i32, b: i32) -> i32 {
    K_OPS[(sel as usize) & 3](a, b)
}

#[no_mangle]
pub extern "C" fn sample_dispatch_fold(a: i32, b: i32) -> i32 {
    let mut acc = 0;
    let mut i = 0usize;
    while i < 4 {
        acc += K_OPS[i](a, b);
        i += 1;
    }
    acc
}

#[no_mangle]
pub extern "C" fn sample_classify(code: i32) -> i32 {
    let base = match code {
        0 => 10,
        1 => 20,
        2 => 40,
        3 => 80,
        4 => 160,
        5 => 320,
        _ => -1,
    };
    if base < 0 {
        return -1;
    }
    base + code * 10 + 20
}
