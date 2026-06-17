#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn sample_add_i32(a: i32, b: i32) -> i32 {
    a + b
}

#[no_mangle]
pub extern "C" fn sample_sum_to_n(n: i32) -> i32 {
    let mut total = 0;
    let mut i = 1;

    while i <= n {
        total += i;
        i += 1;
    }

    total
}

#[no_mangle]
pub extern "C" fn sample_scale_i64(x: i64, y: i64) -> i64 {
    (x * y) + 1
}
