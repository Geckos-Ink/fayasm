#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn sample_const42() -> i32 {
    42
}

#[no_mangle]
pub extern "C" fn sample_mul_add_const() -> i32 {
    let lhs = 6;
    let rhs = 7;
    let extra = 0;
    (lhs * rhs) + extra
}
