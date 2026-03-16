#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn sample_loop_sum() -> i32 {
    let mut total = 0;
    let mut i = 1;

    while i <= 10 {
        total += i;
        i += 1;
    }

    total
}

#[no_mangle]
pub extern "C" fn sample_factorial_6() -> i32 {
    let mut value = 1;
    let mut i = 2;

    while i <= 6 {
        value *= i;
        i += 1;
    }

    value
}
