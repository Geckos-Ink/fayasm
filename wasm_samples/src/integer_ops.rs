#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn sample_signed_divrem(value: i32, divisor: i32) -> i32 {
    ((value / divisor) * 31) + (value % divisor)
}

#[no_mangle]
pub extern "C" fn sample_i32_bit_mix(value: u32) -> u32 {
    value.rotate_left(7) ^ (value.count_ones() << 24) ^ (value | 1).leading_zeros()
}

#[no_mangle]
pub extern "C" fn sample_i64_bit_mix(value: u64) -> u64 {
    value.rotate_right(13) ^ u64::from(value.count_ones())
}

#[inline(never)]
fn sample_nested_helper(lhs: i32, rhs: i32) -> i32 {
    (lhs * 3) + (rhs * 5)
}

#[no_mangle]
pub extern "C" fn sample_nested_calls(a: i32, b: i32, c: i32, d: i32) -> i32 {
    sample_nested_helper(a, b) + sample_nested_helper(c, d)
}
