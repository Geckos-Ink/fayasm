#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

fn xs32(state: &mut u32) -> u32 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}

fn u01(state: &mut u32) -> f64 {
    (xs32(state) >> 8) as f64 * (1.0 / 16777216.0)
}

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
pub extern "C" fn sample_stats_mean(n: i32) -> f64 {
    if n <= 0 {
        return 0.0;
    }
    let mut state: u32 = 0x12345678;
    let mut sum = 0.0;
    let mut i = 0;
    while i < n {
        sum += u01(&mut state);
        i += 1;
    }
    sum / n as f64
}

#[no_mangle]
pub extern "C" fn sample_stats_variance(n: i32) -> f64 {
    if n <= 1 {
        return 0.0;
    }
    let mut state: u32 = 0x12345678;
    let mut mean = 0.0;
    let mut m2 = 0.0;
    let mut i = 1;
    while i <= n {
        let x = u01(&mut state);
        let delta = x - mean;
        mean += delta / i as f64;
        m2 += delta * (x - mean);
        i += 1;
    }
    m2 / (n - 1) as f64
}

#[no_mangle]
pub extern "C" fn sample_linreg_slope(n: i32) -> f64 {
    if n <= 1 {
        return 0.0;
    }
    let mut sx = 0.0;
    let mut sy = 0.0;
    let mut sxx = 0.0;
    let mut sxy = 0.0;
    let mut i = 0;
    while i < n {
        let x = i as f64;
        let y = 3.0 * x + 7.0 + ((i * 37) % 11 - 5) as f64;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        i += 1;
    }
    let denom = n as f64 * sxx - sx * sx;
    if denom == 0.0 {
        return 0.0;
    }
    (n as f64 * sxy - sx * sy) / denom
}

#[no_mangle]
pub extern "C" fn sample_pearson_r(n: i32) -> f64 {
    if n <= 1 {
        return 0.0;
    }
    let mut state: u32 = 0x9e3779b9;
    let mut sx = 0.0;
    let mut sy = 0.0;
    let mut sxx = 0.0;
    let mut syy = 0.0;
    let mut sxy = 0.0;
    let mut i = 0;
    while i < n {
        let x = i as f64 / n as f64;
        let y = 2.0 * x + (u01(&mut state) - 0.5);
        sx += x;
        sy += y;
        sxx += x * x;
        syy += y * y;
        sxy += x * y;
        i += 1;
    }
    let inv_n = 1.0 / n as f64;
    let cov = sxy - sx * sy * inv_n;
    let vx = sxx - sx * sx * inv_n;
    let vy = syy - sy * sy * inv_n;
    if vx <= 0.0 || vy <= 0.0 {
        return 0.0;
    }
    cov / (fa_sqrt(vx) * fa_sqrt(vy))
}

#[no_mangle]
pub extern "C" fn sample_monte_carlo_pi(n: i32, seed: i32) -> f64 {
    if n <= 0 {
        return 0.0;
    }
    let mut state = seed as u32;
    if state == 0 {
        state = 1;
    }
    let mut hits = 0;
    let mut i = 0;
    while i < n {
        let x = u01(&mut state);
        let y = u01(&mut state);
        if x * x + y * y <= 1.0 {
            hits += 1;
        }
        i += 1;
    }
    4.0 * hits as f64 / n as f64
}

const FA_SIEVE_MAX: usize = 100000;
static mut COMPOSITE: [u8; FA_SIEVE_MAX + 1] = [0; FA_SIEVE_MAX + 1];

#[no_mangle]
pub extern "C" fn sample_prime_count(limit: i32) -> i32 {
    if limit < 2 {
        return 0;
    }
    let mut limit = limit as usize;
    if limit > FA_SIEVE_MAX {
        limit = FA_SIEVE_MAX;
    }
    unsafe {
        let composite = &mut *core::ptr::addr_of_mut!(COMPOSITE);
        let mut i = 0;
        while i <= limit {
            composite[i] = 0;
            i += 1;
        }
        let mut count = 0;
        let mut i = 2;
        while i <= limit {
            if composite[i] == 0 {
                count += 1;
                let mut j = i * i;
                while j <= limit {
                    composite[j] = 1;
                    j += i;
                }
            }
            i += 1;
        }
        count
    }
}

const FA_MAT_MAX: usize = 40;
static mut MAT_A: [i32; FA_MAT_MAX * FA_MAT_MAX] = [0; FA_MAT_MAX * FA_MAT_MAX];
static mut MAT_B: [i32; FA_MAT_MAX * FA_MAT_MAX] = [0; FA_MAT_MAX * FA_MAT_MAX];
static mut MAT_C: [i32; FA_MAT_MAX * FA_MAT_MAX] = [0; FA_MAT_MAX * FA_MAT_MAX];

#[no_mangle]
pub extern "C" fn sample_matrix_checksum(n: i32) -> i64 {
    let mut n = n;
    if n < 1 {
        n = 1;
    }
    if n > FA_MAT_MAX as i32 {
        n = FA_MAT_MAX as i32;
    }
    let n = n as usize;
    unsafe {
        let a = &mut *core::ptr::addr_of_mut!(MAT_A);
        let b = &mut *core::ptr::addr_of_mut!(MAT_B);
        let c = &mut *core::ptr::addr_of_mut!(MAT_C);
        let mut i = 0;
        while i < n * n {
            a[i] = ((i as i32) * 7 + 3) % 13 - 6;
            b[i] = ((i as i32) * 5 + 1) % 11 - 5;
            i += 1;
        }
        let mut i = 0;
        while i < n {
            let mut j = 0;
            while j < n {
                let mut acc: i32 = 0;
                let mut k = 0;
                while k < n {
                    acc = acc.wrapping_add(a[i * n + k].wrapping_mul(b[k * n + j]));
                    k += 1;
                }
                c[i * n + j] = acc;
                j += 1;
            }
            i += 1;
        }
        let mut checksum: i64 = 0;
        let mut i = 0;
        while i < n * n {
            checksum += c[i] as i64 * (i as i64 + 1);
            i += 1;
        }
        checksum
    }
}

#[no_mangle]
pub extern "C" fn sample_stats_pipeline(n: i32) -> f64 {
    let mut acc = 0.0;
    acc += sample_stats_mean(n);
    acc += sample_stats_variance(n);
    acc += sample_linreg_slope(n);
    acc += sample_pearson_r(n);
    acc += sample_monte_carlo_pi(n, 424242);
    acc += sample_prime_count(n) as f64 * 1e-3;
    acc += sample_matrix_checksum(n % FA_MAT_MAX as i32 + 1) as f64 * 1e-6;
    acc
}
