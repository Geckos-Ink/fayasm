#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[inline(never)]
fn sample_mix_helper(lhs: i32, rhs: i32) -> i32 {
    (lhs * 3) + (rhs * 5)
}

#[no_mangle]
pub extern "C" fn sample_memory_mix() -> i32 {
    let mut buffer = [0u32; 8];
    let mut i = 0usize;
    while i < buffer.len() {
        buffer[i] = (i as u32 * 3) + 1;
        i += 1;
    }

    let mut checksum = 0u32;
    let mut j = 0usize;
    while j < buffer.len() {
        checksum ^= buffer[j] << (j as u32 & 7);
        j += 1;
    }

    checksum as i32
}

#[no_mangle]
pub extern "C" fn sample_call_chain() -> i32 {
    let mut acc = 0i32;
    let mut i = 0i32;
    while i < 6 {
        acc += sample_mix_helper(i, 6 - i);
        i += 1;
    }
    acc
}
