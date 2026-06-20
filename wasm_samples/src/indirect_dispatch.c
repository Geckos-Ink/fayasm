#include <stdint.h>

/*
 * Indirect-dispatch smoke fixtures. A global array of function pointers forces
 * the toolchain to emit a real funcref table + element segment and reach the
 * callees through call_indirect (with runtime signature checks), while the
 * jump-table switch lowers to br_table. Neither construct was previously
 * exercised end-to-end from real compiler output -- earlier samples only used
 * direct calls and if/else chains.
 */

typedef int (*binop)(int, int);

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_max(int a, int b) { return a > b ? a : b; }

/* A function-pointer table emitted into the module's funcref table and
 * initialized via an element segment. External linkage (no `static`) keeps the
 * optimizer from proving the contents and devirtualizing, so indexing it with a
 * runtime value reliably lowers to call_indirect with a signature check. */
binop k_ops[4] = { op_add, op_sub, op_mul, op_max };

/* Single indirect call selected at runtime. dispatch(2, 6, 7) == 42 (mul). */
int sample_dispatch(int sel, int a, int b) {
    return k_ops[(unsigned)sel & 3u](a, b);
}

/* Fold every operator over (a, b): exercises call_indirect in a loop with a
 * data-dependent index. fold(6, 7) == 13 + (-1) + 42 + 7 == 61. */
int sample_dispatch_fold(int a, int b) {
    int acc = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        acc += k_ops[i](a, b);
    }
    return acc;
}

/* Dense switch -> br_table. Maps a small code to a derived value; the default
 * arm keeps the jump table from collapsing. classify(3) == 130. */
int sample_classify(int code) {
    int base;
    switch (code) {
        case 0: base = 10; break;
        case 1: base = 20; break;
        case 2: base = 40; break;
        case 3: base = 80; break;
        case 4: base = 160; break;
        case 5: base = 320; break;
        default: base = -1; break;
    }
    if (base < 0) {
        return -1;
    }
    return base + code * 10 + 20;
}
