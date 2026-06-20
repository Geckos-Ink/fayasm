#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

/// Open-coded sqrt via Newton's method so we depend on no host import.
fn fa_sqrt(x: f64) -> f64 {
    if x <= 0.0 {
        return 0.0;
    }
    let mut guess = x;
    let mut i = 0;
    while i < 60 {
        guess = 0.5 * (guess + x / guess);
        i += 1;
    }
    guess
}

#[no_mangle]
pub extern "C" fn sample_f64_poly(x: f64) -> f64 {
    ((3.0 * x - 2.0) * x + 1.0) * x - 5.0
}

#[no_mangle]
pub extern "C" fn sample_f64_hypot(a: f64, b: f64) -> f64 {
    fa_sqrt(a * a + b * b)
}

#[no_mangle]
pub extern "C" fn sample_f32_lerp(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

#[no_mangle]
pub extern "C" fn sample_f64_round(x: f64) -> i32 {
    if x >= 0.0 {
        (x + 0.5) as i32
    } else {
        (x - 0.5) as i32
    }
}

#[no_mangle]
pub extern "C" fn sample_f64_series(n: i32) -> f64 {
    let mut total = 0.0;
    let mut i = 1;
    while i <= n {
        total += (i as f64) * 1.5;
        i += 1;
    }
    total
}
