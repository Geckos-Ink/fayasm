#include "fa_ops_internal.h"

/* Portable pre-stacked instruction tape. These are ordinary compiled C99
 * delegates, selected once at preparation, not an opcode switch in execution.
 * The flat temporary register bank spans a sequence; public storage is touched
 * only on demand, overflow, traps, and synchronization boundaries. */
#define SEQUENCE_REGISTERS 16U
struct fa_SequenceContext {
    fa_Runtime* runtime;
    fa_Job* job;
    fa_JobValue values[SEQUENCE_REGISTERS];
    size_t count;
    bool stop;
};
typedef struct fa_SequenceContext Seq;

static int seq_flush(Seq* s) {
    /* Every deferred push already checked the logical cap and backing capacity.
     * Materializing cannot allocate: preserve exact OOM timing in seq_push. */
    for (size_t i = 0; i < s->count; ++i) {
        if (!fa_JobStack_push(&s->job->stack, &s->values[i])) return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    s->count = 0;
    return FA_RUNTIME_OK;
}

static bool seq_need(Seq* s, size_t n) {
    if (s->count >= n) return true;
    const size_t missing = n - s->count;
    if (s->job->stack.size < missing) return false;
    memmove(s->values + missing, s->values, s->count * sizeof(*s->values));
    for (size_t i = missing; i > 0; --i) fa_JobStack_pop(&s->job->stack, &s->values[i-1]);
    s->count = n;
    return true;
}

static int seq_push(Seq* s, const fa_JobValue* v) {
    const size_t depth = s->job->stack.size + s->count;
    if (depth >= FA_JOB_STACK_MAX_DEPTH) return FA_RUNTIME_ERR_OUT_OF_MEMORY;
    if (s->count == SEQUENCE_REGISTERS ||
        (depth >= FA_JOB_REGISTER_WINDOW_SIZE &&
         depth - FA_JOB_REGISTER_WINDOW_SIZE >= s->job->stack.spill_capacity)) {
        int status = seq_flush(s);
        if (status != FA_RUNTIME_OK) return status;
        return fa_JobStack_push(&s->job->stack, v) ? FA_RUNTIME_OK : FA_RUNTIME_ERR_OUT_OF_MEMORY;
    }
    s->values[s->count++] = *v;
    return FA_RUNTIME_OK;
}

static int seq_generic(Seq* s, const fa_JitPreparedOp* op) {
    /* A memory reload/spill callback can evict this very program. Snapshot all
     * arguments/steps before invoking it, and end the sequence afterwards. */
    const fa_JitPreparedOp saved = *op;
    s->stop = true;
    int status = seq_flush(s);
    if (status != FA_RUNTIME_OK) return status;
    size_t offset = 0;
    for (size_t i = 0; i < saved.arg_count; ++i) {
        if (!fa_JobRegWindow_push(s->job, saved.immediate + offset, saved.arg_sizes[i]))
            return FA_RUNTIME_ERR_TRAP;
        offset += saved.arg_sizes[i];
    }
    return fa_jit_execute_prepared_op(&saved, s->runtime, s->job);
}

/* Macro families: each expansion has a fixed input/output kind, expression,
 * and trap predicate. No descriptor type/operator selection on the hot path.
 * Kind mismatch uses the existing public handler's cross-kind contract. */
#define BINARY(code, in_kind, field, ctype, outkind, outfield, bits, sign, guard, expr) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    if (!seq_need(s, 2)) return seq_generic(s, op); \
    fa_JobValue a = s->values[s->count-2], b = s->values[s->count-1]; \
    if (a.kind != fa_job_value_##in_kind || b.kind != fa_job_value_##in_kind) return seq_generic(s, op); \
    const ctype left = a.payload.field, right = b.payload.field; \
    if (!(guard)) return seq_generic(s, op); \
    fa_JobValue result = {0}; \
    result.kind = fa_job_value_##outkind; result.bit_width = bits; result.is_signed = sign; \
    result.payload.outfield = (expr); \
    s->values[s->count-2] = result; s->count--; return FA_RUNTIME_OK; \
}
#define UNARY(code, in_kind, field, ctype, outkind, outfield, bits, sign, expr) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    if (!seq_need(s, 1)) return seq_generic(s, op); \
    const fa_JobValue a = s->values[s->count-1]; \
    if (a.kind != fa_job_value_##in_kind) return seq_generic(s, op); \
    const ctype value = a.payload.field; \
    fa_JobValue result = {0}; \
    result.kind = fa_job_value_##outkind; result.bit_width = bits; result.is_signed = sign; \
    result.payload.outfield = (expr); s->values[s->count-1] = result; return FA_RUNTIME_OK; \
}

#define SEQUENCE_BINARY(X) \
    X(6A, i32, u32_value, u32, i32, u32_value, 32, true, true, left + right) \
    X(6B, i32, u32_value, u32, i32, u32_value, 32, true, true, left - right) \
    X(6C, i32, u32_value, u32, i32, u32_value, 32, true, true, left * right) \
    X(6D, i32, i32_value, i32, i32, u32_value, 32, true, right != 0 && !(left == INT32_MIN && right == -1), left / right) \
    X(6E, i32, u32_value, u32, i32, u32_value, 32, false, right != 0, left / right) \
    X(6F, i32, i32_value, i32, i32, u32_value, 32, true, right != 0, (left == INT32_MIN && right == -1) ? 0 : left % right) \
    X(70, i32, u32_value, u32, i32, u32_value, 32, false, right != 0, left % right) \
    X(71, i32, u32_value, u32, i32, u32_value, 32, true, true, left & right) \
    X(72, i32, u32_value, u32, i32, u32_value, 32, true, true, left | right) \
    X(73, i32, u32_value, u32, i32, u32_value, 32, true, true, left ^ right) \
    X(74, i32, u32_value, u32, i32, u32_value, 32, true, true, left << (right & 31U)) \
    X(75, i32, i32_value, i32, i32, u32_value, 32, true, true, left >> (right & 31U)) \
    X(76, i32, u32_value, u32, i32, u32_value, 32, false, true, left >> (right & 31U)) \
    X(77, i32, u32_value, u32, i32, u32_value, 32, true, true, rotl32(left, right & 31U)) \
    X(78, i32, u32_value, u32, i32, u32_value, 32, true, true, rotr32(left, right & 31U)) \
    X(46, i32, u32_value, u32, i32, u32_value, 32, false, true, left == right) \
    X(47, i32, u32_value, u32, i32, u32_value, 32, false, true, left != right) \
    X(48, i32, i32_value, i32, i32, u32_value, 32, false, true, left < right) \
    X(49, i32, u32_value, u32, i32, u32_value, 32, false, true, left < right) \
    X(4A, i32, i32_value, i32, i32, u32_value, 32, false, true, left > right) \
    X(4B, i32, u32_value, u32, i32, u32_value, 32, false, true, left > right) \
    X(4C, i32, i32_value, i32, i32, u32_value, 32, false, true, left <= right) \
    X(4D, i32, u32_value, u32, i32, u32_value, 32, false, true, left <= right) \
    X(4E, i32, i32_value, i32, i32, u32_value, 32, false, true, left >= right) \
    X(4F, i32, u32_value, u32, i32, u32_value, 32, false, true, left >= right) \
    X(7C, i64, u64_value, u64, i64, u64_value, 64, true, true, left + right) \
    X(7D, i64, u64_value, u64, i64, u64_value, 64, true, true, left - right) \
    X(7E, i64, u64_value, u64, i64, u64_value, 64, true, true, left * right) \
    X(7F, i64, i64_value, i64, i64, u64_value, 64, true, right != 0 && !(left == INT64_MIN && right == -1), left / right) \
    X(80, i64, u64_value, u64, i64, u64_value, 64, false, right != 0, left / right) \
    X(81, i64, i64_value, i64, i64, u64_value, 64, true, right != 0, (left == INT64_MIN && right == -1) ? 0 : left % right) \
    X(82, i64, u64_value, u64, i64, u64_value, 64, false, right != 0, left % right) \
    X(83, i64, u64_value, u64, i64, u64_value, 64, true, true, left & right) \
    X(84, i64, u64_value, u64, i64, u64_value, 64, true, true, left | right) \
    X(85, i64, u64_value, u64, i64, u64_value, 64, true, true, left ^ right) \
    X(86, i64, u64_value, u64, i64, u64_value, 64, true, true, left << (right & 63U)) \
    X(87, i64, i64_value, i64, i64, u64_value, 64, true, true, left >> (right & 63U)) \
    X(88, i64, u64_value, u64, i64, u64_value, 64, false, true, left >> (right & 63U)) \
    X(89, i64, u64_value, u64, i64, u64_value, 64, true, true, rotl64(left, right & 63U)) \
    X(8A, i64, u64_value, u64, i64, u64_value, 64, true, true, rotr64(left, right & 63U)) \
    X(51, i64, u64_value, u64, i32, u32_value, 32, false, true, left == right) \
    X(52, i64, u64_value, u64, i32, u32_value, 32, false, true, left != right) \
    X(53, i64, i64_value, i64, i32, u32_value, 32, false, true, left < right) \
    X(54, i64, u64_value, u64, i32, u32_value, 32, false, true, left < right) \
    X(55, i64, i64_value, i64, i32, u32_value, 32, false, true, left > right) \
    X(56, i64, u64_value, u64, i32, u32_value, 32, false, true, left > right) \
    X(57, i64, i64_value, i64, i32, u32_value, 32, false, true, left <= right) \
    X(58, i64, u64_value, u64, i32, u32_value, 32, false, true, left <= right) \
    X(59, i64, i64_value, i64, i32, u32_value, 32, false, true, left >= right) \
    X(5A, i64, u64_value, u64, i32, u32_value, 32, false, true, left >= right) \
    X(92, f32, f32_value, f64, f32, f32_value, 32, false, true, left + right) \
    X(93, f32, f32_value, f64, f32, f32_value, 32, false, true, left - right) \
    X(94, f32, f32_value, f64, f32, f32_value, 32, false, true, left * right) \
    X(95, f32, f32_value, f64, f32, f32_value, 32, false, true, left / right) \
    X(96, f32, f32_value, f32, f32, f32_value, 32, false, true, fminf(left, right)) \
    X(97, f32, f32_value, f32, f32, f32_value, 32, false, true, fmaxf(left, right)) \
    X(98, f32, f32_value, f32, f32, f32_value, 32, false, true, copysignf(left, right)) \
    X(5B, f32, f32_value, f32, i32, u32_value, 32, false, true, left == right) \
    X(5C, f32, f32_value, f32, i32, u32_value, 32, false, true, left != right) \
    X(5D, f32, f32_value, f32, i32, u32_value, 32, false, true, left < right) \
    X(5E, f32, f32_value, f32, i32, u32_value, 32, false, true, left > right) \
    X(5F, f32, f32_value, f32, i32, u32_value, 32, false, true, left <= right) \
    X(60, f32, f32_value, f32, i32, u32_value, 32, false, true, left >= right) \
    X(A0, f64, f64_value, f64, f64, f64_value, 64, false, true, left + right) \
    X(A1, f64, f64_value, f64, f64, f64_value, 64, false, true, left - right) \
    X(A2, f64, f64_value, f64, f64, f64_value, 64, false, true, left * right) \
    X(A3, f64, f64_value, f64, f64, f64_value, 64, false, true, left / right) \
    X(A4, f64, f64_value, f64, f64, f64_value, 64, false, true, fmin(left, right)) \
    X(A5, f64, f64_value, f64, f64, f64_value, 64, false, true, fmax(left, right)) \
    X(A6, f64, f64_value, f64, f64, f64_value, 64, false, true, copysign(left, right)) \
    X(61, f64, f64_value, f64, i32, u32_value, 32, false, true, left == right) \
    X(62, f64, f64_value, f64, i32, u32_value, 32, false, true, left != right) \
    X(63, f64, f64_value, f64, i32, u32_value, 32, false, true, left < right) \
    X(64, f64, f64_value, f64, i32, u32_value, 32, false, true, left > right) \
    X(65, f64, f64_value, f64, i32, u32_value, 32, false, true, left <= right) \
    X(66, f64, f64_value, f64, i32, u32_value, 32, false, true, left >= right)
SEQUENCE_BINARY(BINARY)

#define SEQUENCE_UNARY(X) \
    X(67, i32, u32_value, u32, i32, u32_value, 32, false, clz32(value)) \
    X(68, i32, u32_value, u32, i32, u32_value, 32, false, ctz32(value)) \
    X(69, i32, u32_value, u32, i32, u32_value, 32, false, popcnt32(value)) \
    X(45, i32, u32_value, u32, i32, u32_value, 32, false, value == 0) \
    X(79, i64, u64_value, u64, i64, u64_value, 64, false, clz64(value)) \
    X(7A, i64, u64_value, u64, i64, u64_value, 64, false, ctz64(value)) \
    X(7B, i64, u64_value, u64, i64, u64_value, 64, false, popcnt64(value)) \
    X(50, i64, u64_value, u64, i32, u32_value, 32, false, value == 0) \
    X(8B, f32, f32_value, f32, f32, f32_value, 32, false, fabsf(value)) \
    X(8C, f32, f32_value, f32, f32, f32_value, 32, false, -value) \
    X(8D, f32, f32_value, f32, f32, f32_value, 32, false, ceilf(value)) \
    X(8E, f32, f32_value, f32, f32, f32_value, 32, false, floorf(value)) \
    X(8F, f32, f32_value, f32, f32, f32_value, 32, false, truncf(value)) \
    X(90, f32, f32_value, f32, f32, f32_value, 32, false, nearbyintf(value)) \
    X(91, f32, f32_value, f32, f32, f32_value, 32, false, sqrtf(value)) \
    X(99, f64, f64_value, f64, f64, f64_value, 64, false, fabs(value)) \
    X(9A, f64, f64_value, f64, f64, f64_value, 64, false, -value) \
    X(9B, f64, f64_value, f64, f64, f64_value, 64, false, ceil(value)) \
    X(9C, f64, f64_value, f64, f64, f64_value, 64, false, floor(value)) \
    X(9D, f64, f64_value, f64, f64, f64_value, 64, false, trunc(value)) \
    X(9E, f64, f64_value, f64, f64, f64_value, 64, false, nearbyint(value)) \
    X(9F, f64, f64_value, f64, f64, f64_value, 64, false, sqrt(value)) \
    X(A7, i64, u64_value, u64, i32, u32_value, 32, true, (u32)value) \
    X(AC, i32, u32_value, u32, i64, u64_value, 64, true, (u64)(i64)(i32)value) \
    X(AD, i32, u32_value, u32, i64, u64_value, 64, false, value) \
    X(B2, i32, u32_value, u32, f32, f32_value, 32, false, (f32)(i32)value) \
    X(B3, i32, u32_value, u32, f32, f32_value, 32, false, (f32)value) \
    X(B4, i64, u64_value, u64, f32, f32_value, 32, false, (f32)(i64)value) \
    X(B5, i64, u64_value, u64, f32, f32_value, 32, false, (f32)value) \
    X(B7, i32, u32_value, u32, f64, f64_value, 64, false, (f64)(i32)value) \
    X(B8, i32, u32_value, u32, f64, f64_value, 64, false, (f64)value) \
    X(B9, i64, u64_value, u64, f64, f64_value, 64, false, (f64)(i64)value) \
    X(BA, i64, u64_value, u64, f64, f64_value, 64, false, (f64)value) \
    X(B6, f64, f64_value, f64, f32, f32_value, 32, false, (f32)value) \
    X(BB, f32, f32_value, f32, f64, f64_value, 64, false, value) \
    X(C0, i32, u32_value, u32, i32, u32_value, 32, true, (u32)(i32)(int8_t)value) \
    X(C1, i32, u32_value, u32, i32, u32_value, 32, true, (u32)(i32)(int16_t)value) \
    X(C2, i64, u64_value, u64, i64, u64_value, 64, true, (u64)(i64)(int8_t)value) \
    X(C3, i64, u64_value, u64, i64, u64_value, 64, true, (u64)(i64)(int16_t)value) \
    X(C4, i64, u64_value, u64, i64, u64_value, 64, true, (u64)(i64)(int32_t)value)
SEQUENCE_UNARY(UNARY)


/* Truncation predicates are checked before touching the source register. */
#define TRUNC(code, in_kind, field, outkind, outfield, bits, sign) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    if (!seq_need(s, 1) || s->values[s->count-1].kind != fa_job_value_##in_kind) return seq_generic(s, op); \
    u64 raw = 0; \
    if (!trunc_f64_to_i##bits(s->values[s->count-1].payload.field, sign, &raw)) return seq_generic(s, op); \
    fa_JobValue v = {0}; v.kind = fa_job_value_##outkind; v.bit_width = bits; v.is_signed = sign; \
    v.payload.outfield = raw; s->values[s->count-1] = v; return FA_RUNTIME_OK; \
}
#define SEQUENCE_TRUNC(X) \
    X(A8, f32, f32_value, i32, u32_value, 32, true) \
    X(A9, f32, f32_value, i32, u32_value, 32, false) \
    X(AA, f64, f64_value, i32, u32_value, 32, true) \
    X(AB, f64, f64_value, i32, u32_value, 32, false) \
    X(AE, f32, f32_value, i64, u64_value, 64, true) \
    X(AF, f32, f32_value, i64, u64_value, 64, false) \
    X(B0, f64, f64_value, i64, u64_value, 64, true) \
    X(B1, f64, f64_value, i64, u64_value, 64, false)
SEQUENCE_TRUNC(TRUNC)

#define REINTERPRET(code, in_kind, field, outkind, outfield, bits, sign) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    if (!seq_need(s, 1) || s->values[s->count-1].kind != fa_job_value_##in_kind) return seq_generic(s, op); \
    fa_JobValue v = {0}; v.kind = fa_job_value_##outkind; v.bit_width = bits; v.is_signed = sign; \
    memcpy(&v.payload.outfield, &s->values[s->count-1].payload.field, bits/8); \
    s->values[s->count-1] = v; return FA_RUNTIME_OK; \
}
#define SEQUENCE_REINTERPRET(X) \
    X(BC, f32, f32_value, i32, u32_value, 32, true) \
    X(BD, f64, f64_value, i64, u64_value, 64, true) \
    X(BE, i32, u32_value, f32, f32_value, 32, false) \
    X(BF, i64, u64_value, f64, f64_value, 64, false)
SEQUENCE_REINTERPRET(REINTERPRET)

#define CONSTANT(code, in_kind, field, bits, sign) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    fa_JobValue v = {0}; v.kind = fa_job_value_##in_kind; v.bit_width = bits; v.is_signed = sign; \
    memcpy(&v.payload.field, op->immediate, bits/8); return seq_push(s, &v); \
}
#define SEQUENCE_CONSTANT(X) \
    X(41, i32, i32_value, 32, true) \
    X(42, i64, i64_value, 64, true) \
    X(43, f32, f32_value, 32, false) \
    X(44, f64, f64_value, 64, false)
SEQUENCE_CONSTANT(CONSTANT)

static uint32_t seq_index(const fa_JitPreparedOp* op) {
    uint32_t index; memcpy(&index, op->immediate, sizeof(index)); return index;
}
static int seq_20(Seq* s, const fa_JitPreparedOp* op) {
    uint32_t i = seq_index(op);
    if (i >= s->runtime->active_locals_count) return seq_generic(s, op);
    return seq_push(s, &s->runtime->active_locals[i]);
}
static int seq_21(Seq* s, const fa_JitPreparedOp* op) {
    uint32_t i = seq_index(op);
    if (i >= s->runtime->active_locals_count || !seq_need(s, 1)) return seq_generic(s, op);
    s->runtime->active_locals[i] = s->values[--s->count]; return FA_RUNTIME_OK;
}
static int seq_22(Seq* s, const fa_JitPreparedOp* op) {
    uint32_t i = seq_index(op);
    if (i >= s->runtime->active_locals_count || !seq_need(s, 1)) return seq_generic(s, op);
    s->runtime->active_locals[i] = s->values[s->count-1]; return FA_RUNTIME_OK;
}
static int seq_1A(Seq* s, const fa_JitPreparedOp* op) {
    if (!seq_need(s, 1)) return seq_generic(s, op);
    s->count--; return FA_RUNTIME_OK;
}
static int seq_1B(Seq* s, const fa_JitPreparedOp* op) {
    if (!seq_need(s, 3)) return seq_generic(s, op);
    const fa_JobValue* cond = &s->values[s->count-1];
    const fa_JobValue chosen = job_value_truthy(cond) ? s->values[s->count-3] : s->values[s->count-2];
    s->count -= 2; s->values[s->count-1] = chosen; return FA_RUNTIME_OK;
}

/* Memory family: bind static memarg once; resolve the live memory on every
 * invocation. Never cache host buffers, and enter the checked generic boundary
 * for unloaded memory, invalid types, overflow or OOB before changing state. */
void fa_ops_sequence_bind(fa_JitPreparedOp* op) {
    const uint8_t code = op->descriptor->id;
    if (code < 0x28 || code > 0x3E || op->arg_count < 2) return;
    op->memory_index = 0;
    size_t offset = 0;
    if (op->arg_count == 3) {
        memcpy(&op->memory_index, op->immediate, sizeof(uint32_t));
        offset += op->arg_sizes[0];
    }
    offset += op->arg_sizes[op->arg_count - 2];
    op->memory_offset = 0;
    memcpy(&op->memory_offset, op->immediate + offset, op->arg_sizes[op->arg_count - 1]);
}

static uint8_t* seq_address(Seq* s, const fa_JitPreparedOp* op, const fa_JobValue* value, size_t width) {
    if (op->memory_index >= s->runtime->memories_count) return NULL;
    fa_RuntimeMemory* m = &s->runtime->memories[op->memory_index];
    if (!m->data || m->is_spilled || value->kind != (m->is_memory64 ? fa_job_value_i64 : fa_job_value_i32)) return NULL;
    const u64 base = m->is_memory64 ? value->payload.u64_value : value->payload.u32_value;
    if (op->memory_offset > UINT64_MAX - base) return NULL;
    const u64 addr = base + op->memory_offset;
    if (addr > SIZE_MAX || addr > m->size_bytes || width > m->size_bytes - addr) return NULL;
    return m->data + (size_t)addr;
}
#define LOAD(code, outkind, field, ctype, bits, sign) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    if (!seq_need(s, 1)) return seq_generic(s, op); \
    uint8_t* addr = seq_address(s, op, &s->values[s->count-1], sizeof(ctype)); \
    if (!addr) return seq_generic(s, op); \
    ctype raw; memcpy(&raw, addr, sizeof(raw)); \
    fa_JobValue v = {0}; v.kind = fa_job_value_##outkind; v.bit_width = bits; v.is_signed = sign; \
    v.payload.field = raw; s->values[s->count-1] = v; return FA_RUNTIME_OK; \
}
#define STORE(code, in_kind, field, ctype) \
static int seq_##code(Seq* s, const fa_JitPreparedOp* op) { \
    if (!seq_need(s, 2) || s->values[s->count-1].kind != fa_job_value_##in_kind) return seq_generic(s, op); \
    uint8_t* addr = seq_address(s, op, &s->values[s->count-2], sizeof(ctype)); \
    if (!addr) return seq_generic(s, op); \
    const ctype raw = (ctype)s->values[s->count-1].payload.field; memcpy(addr, &raw, sizeof(raw)); \
    s->count -= 2; return FA_RUNTIME_OK; \
}
#define SEQUENCE_LOAD(X) \
    X(28, i32, i32_value, i32, 32, true) \
    X(29, i64, i64_value, i64, 64, true) \
    X(2A, f32, f32_value, f32, 32, false) \
    X(2B, f64, f64_value, f64, 64, false) \
    X(2C, i32, i32_value, i8, 32, true) \
    X(2D, i32, u32_value, u8, 32, false) \
    X(2E, i32, i32_value, i16, 32, true) \
    X(2F, i32, u32_value, u16, 32, false) \
    X(30, i64, i64_value, i8, 64, true) \
    X(31, i64, u64_value, u8, 64, false) \
    X(32, i64, i64_value, i16, 64, true) \
    X(33, i64, u64_value, u16, 64, false) \
    X(34, i64, i64_value, i32, 64, true) \
    X(35, i64, u64_value, u32, 64, false)
SEQUENCE_LOAD(LOAD)

#define SEQUENCE_STORE(X) \
    X(36, i32, u32_value, u32) \
    X(37, i64, u64_value, u64) \
    X(38, f32, f32_value, f32) \
    X(39, f64, f64_value, f64) \
    X(3A, i32, u32_value, u8) \
    X(3B, i32, u32_value, u16) \
    X(3C, i64, u64_value, u8) \
    X(3D, i64, u64_value, u16) \
    X(3E, i64, u64_value, u32)
SEQUENCE_STORE(STORE)

fa_SequenceOperation fa_ops_sequence_operation(uint8_t opcode) {
    static const fa_SequenceOperation table[256] = {
#define MAP(code, ...) [0x##code] = seq_##code,
        SEQUENCE_BINARY(MAP)
        SEQUENCE_UNARY(MAP)
        SEQUENCE_TRUNC(MAP)
        SEQUENCE_REINTERPRET(MAP)
        SEQUENCE_CONSTANT(MAP)
        SEQUENCE_LOAD(MAP)
        SEQUENCE_STORE(MAP)
#undef MAP
        [0x20] = seq_20, [0x21] = seq_21, [0x22] = seq_22,
        [0x1A] = seq_1A, [0x1B] = seq_1B
    };
    return table[opcode] ? table[opcode] : seq_generic;
}

int fa_jit_execute_sequence(fa_JitProgram* program, const int32_t* pc_to_index,
                            size_t map_size, uint32_t* pc, size_t limit,
                            fa_Runtime* runtime, fa_Job* job,
                            size_t* executed, size_t* scalar_immediates) {
    if (!program || !pc_to_index || !pc || !runtime || !job || !executed || !scalar_immediates)
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    *executed = 0; *scalar_immediates = 0;
    if (*pc >= map_size || job->reg_count) return FA_RUNTIME_OK;
    int32_t index = pc_to_index[*pc];
    Seq s; s.runtime = runtime; s.job = job; s.count = 0; s.stop = false;
    int status = FA_RUNTIME_OK;
    while (*executed < limit && index >= 0 && (size_t)index < program->count) {
        fa_JitPreparedOp* op = &program->ops[index];
        if (!op->next_pc || op->source_pc != *pc || !op->sequence_operation) break;
        const uint32_t next_pc = op->next_pc;
        if (next_pc <= *pc || next_pc > map_size) break;
        const bool scalar = op->immediate_size != 0;
        /* Resolve links before calling a delegate; a generic callback can
         * evict the program. No program/op/map access after a stopping step. */
        int32_t next = -1;
        if (op->link_index) next = (int32_t)(op->link_index - 1U);
        else if (next_pc < map_size) {
            next = pc_to_index[next_pc];
            if (next >= 0) op->link_index = (uint32_t)next + 1U;
        }
        status = op->sequence_operation(&s, op);
        *pc = next_pc;
        (*executed)++;
        if (scalar) (*scalar_immediates)++;
        if (status != FA_RUNTIME_OK || s.stop) break;
        index = next;
    }
    const int flush_status = seq_flush(&s);
    return status != FA_RUNTIME_OK ? status : flush_status;
}
