int sample_loop_sum(void) {
    int total = 0;
    for (int i = 1; i <= 10; ++i) {
        total += i;
    }
    return total;
}

int sample_factorial_6(void) {
    int value = 1;
    for (int i = 2; i <= 6; ++i) {
        value *= i;
    }
    return value;
}
