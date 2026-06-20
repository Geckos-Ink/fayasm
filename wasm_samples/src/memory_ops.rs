#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn sample_sort_checksum() -> i32 {
    let mut a: [i32; 8] = [5, 3, 8, 1, 9, 2, 7, 4];
    let mut i = 0;
    while i < 8 {
        if a[i] == 9 {
            a[i] = 6;
        }
        i += 1;
    }
    let mut i = 0;
    while i < 8 {
        let mut j = 0;
        while j < 7 - i {
            if a[j] > a[j + 1] {
                a.swap(j, j + 1);
            }
            j += 1;
        }
        i += 1;
    }
    let mut sum = 0;
    let mut i = 0;
    while i < 8 {
        sum += a[i] * (i as i32 + 1);
        i += 1;
    }
    sum
}

#[no_mangle]
pub extern "C" fn sample_buffer_pipeline(seed: i32, n: i32) -> i32 {
    let mut n = n;
    if n < 0 {
        n = 0;
    }
    if n > 256 {
        n = 256;
    }
    let n = n as usize;
    let mut src: [u8; 256] = [0; 256];
    let mut dst: [u8; 256] = [0; 256];
    let mut i = 0;
    while i < n {
        src[i] = ((seed + (i as i32) * 31) & 0xff) as u8;
        i += 1;
    }
    dst[..n].copy_from_slice(&src[..n]);
    let mut i = 0;
    while i < n {
        src[i] = 0;
        i += 1;
    }
    let mut checksum: u32 = 0;
    let mut i = 0;
    while i < n {
        checksum = (checksum
            .wrapping_mul(131)
            .wrapping_add(dst[i] as u32)
            .wrapping_add(src[i] as u32))
            & 0x7fffffff;
        i += 1;
    }
    checksum as i32
}

#[no_mangle]
pub extern "C" fn sample_table_lookup(i: i32) -> i32 {
    static K_SQUARES: [i32; 16] = [
        0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225,
    ];
    K_SQUARES[(i as usize) & 15]
}
