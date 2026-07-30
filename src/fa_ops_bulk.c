#include "fa_ops_internal.h"

/* Bulk memory/table operations and memory/table growth primitives. */

static int runtime_table_grow(fa_Runtime* runtime, u64 table_index, u64 delta, fa_ptr init_value, u64* prev_size_out, bool* grew_out);

/*
 * Handles 0xFC-prefixed subopcodes.
 * Immediates are already decoded into the reg window by the runtime decoder.
 * Dynamic indices/lengths are popped from the operand stack with strict typing.
 */
static OP_RETURN_TYPE op_bulk_memory_init(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 mem_index = 0;
    u64 data_index = 0;
    if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK ||
        pop_reg_u64_checked(job, &data_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* memory = NULL;
    int status = runtime_require_memory(runtime, mem_index, &memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    const WasmDataSegment* segment = runtime_get_data_segment(runtime, data_index);
    if (!segment || !runtime->data_segments_dropped ||
        data_index >= runtime->data_segments_count ||
        runtime->data_segments_dropped[data_index]) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 length = 0;
    if (pop_index_checked(job, memory->is_memory64, &length) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 src_offset = 0;
    if (pop_index_checked(job, memory->is_memory64, &src_offset) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 dst_addr = 0;
    if (pop_address_checked_typed(job, &dst_addr, memory->is_memory64) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (length > SIZE_MAX || src_offset > UINT64_MAX - length) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!segment->data && length > 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint64_t segment_end = src_offset + length;
    if (segment_end > segment->size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const size_t len = (size_t)length;
    if (memory_bounds_check(memory, dst_addr, len) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    memcpy(memory->data + (size_t)dst_addr, segment->data + (size_t)src_offset, len);
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_data_drop(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 data_index = 0;
    if (pop_reg_u64_checked(job, &data_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime_get_data_segment(runtime, data_index) ||
        !runtime->data_segments_dropped ||
        data_index >= runtime->data_segments_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    runtime->data_segments_dropped[data_index] = true;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_memory_copy(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 src_index = 0;
    u64 dst_index = 0;
    if (pop_reg_u64_checked(job, &src_index) != FA_RUNTIME_OK ||
        pop_reg_u64_checked(job, &dst_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* src_memory = NULL;
    fa_RuntimeMemory* dst_memory = NULL;
    int status = runtime_require_memory(runtime, src_index, &src_memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    status = runtime_require_memory(runtime, dst_index, &dst_memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    const bool length_is_64 = src_memory->is_memory64 || dst_memory->is_memory64;
    u64 length = 0;
    if (pop_length_checked(job, length_is_64, &length) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 src_addr = 0;
    if (pop_address_checked_typed(job, &src_addr, src_memory->is_memory64) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 dst_addr = 0;
    if (pop_address_checked_typed(job, &dst_addr, dst_memory->is_memory64) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (length > SIZE_MAX) {
        return FA_RUNTIME_ERR_TRAP;
    }
    size_t len = (size_t)length;
    if (memory_bounds_check(src_memory, src_addr, len) != FA_RUNTIME_OK ||
        memory_bounds_check(dst_memory, dst_addr, len) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    memmove(dst_memory->data + (size_t)dst_addr, src_memory->data + (size_t)src_addr, len);
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_memory_fill(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 mem_index = 0;
    if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* memory = NULL;
    int status = runtime_require_memory(runtime, mem_index, &memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    u64 length = 0;
    if (pop_length_checked(job, memory->is_memory64, &length) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u8 byte_value = 0;
    if (pop_byte_value_checked(job, &byte_value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 dst_addr = 0;
    if (pop_address_checked_typed(job, &dst_addr, memory->is_memory64) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (length > SIZE_MAX) {
        return FA_RUNTIME_ERR_TRAP;
    }
    size_t len = (size_t)length;
    if (memory_bounds_check(memory, dst_addr, len) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    memset(memory->data + (size_t)dst_addr, byte_value, len);
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_table_init(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    int status = FA_RUNTIME_OK;
    u64 elem_index = 0;
    u64 table_index = 0;
    if (pop_reg_u64_checked(job, &elem_index) != FA_RUNTIME_OK ||
        pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    const WasmElementSegment* segment = runtime_get_element_segment(runtime, elem_index);
    if (!table || !segment || !runtime->elem_segments_dropped ||
        elem_index >= runtime->elem_segments_count ||
        runtime->elem_segments_dropped[elem_index] || !segment->is_passive) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (segment->elem_type != table->elem_type) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t length = 0;
    if (pop_u32_checked(job, &length) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t src = 0;
    if (pop_u32_checked(job, &src) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t dst = 0;
    if (pop_u32_checked(job, &dst) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if ((uint64_t)src + length > segment->element_count ||
        (uint64_t)dst + length > table->size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!segment->elements && length > 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    for (uint32_t i = 0; i < length; ++i) {
        fa_ptr ref_value = 0;
        status = runtime_resolve_element_ref(runtime, segment, src + i, &ref_value);
        if (status != FA_RUNTIME_OK) {
            return status;
        }
        status = runtime_validate_table_ref_value(runtime, table, ref_value);
        if (status != FA_RUNTIME_OK) {
            return status;
        }
        table->data[dst + i] = ref_value;
    }
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_elem_drop(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 elem_index = 0;
    if (pop_reg_u64_checked(job, &elem_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!runtime_get_element_segment(runtime, elem_index) ||
        !runtime->elem_segments_dropped ||
        elem_index >= runtime->elem_segments_count) {
        return FA_RUNTIME_ERR_TRAP;
    }
    runtime->elem_segments_dropped[elem_index] = true;
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_table_copy(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 src_index = 0;
    u64 dst_index = 0;
    if (pop_reg_u64_checked(job, &src_index) != FA_RUNTIME_OK ||
        pop_reg_u64_checked(job, &dst_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeTable* src_table = runtime_get_table(runtime, src_index);
    fa_RuntimeTable* dst_table = runtime_get_table(runtime, dst_index);
    if (!src_table || !dst_table) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t length = 0;
    if (pop_u32_checked(job, &length) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t src = 0;
    if (pop_u32_checked(job, &src) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t dst = 0;
    if (pop_u32_checked(job, &dst) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if ((uint64_t)src + length > src_table->size ||
        (uint64_t)dst + length > dst_table->size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (length > 0) {
        memmove(dst_table->data + dst, src_table->data + src, length * sizeof(fa_ptr));
    }
    return FA_RUNTIME_OK;
}

static OP_RETURN_TYPE op_bulk_table_grow(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 table_index = 0;
    if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t delta = 0;
    if (pop_u32_checked(job, &delta) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_ptr init_value = 0;
    if (pop_ref_checked(job, &init_value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 prev_size = 0;
    bool grew = false;
    const int status = runtime_table_grow(runtime, table_index, delta, init_value, &prev_size, &grew);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    if (!grew) {
        return push_int_checked(job, (u32)UINT32_MAX, 32U, true);
    }
    return push_int_checked(job, prev_size, 32U, true);
}

static OP_RETURN_TYPE op_bulk_table_size(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 table_index = 0;
    if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    if (!table) {
        return FA_RUNTIME_ERR_TRAP;
    }
    return push_int_checked(job, table->size, 32U, true);
}

static OP_RETURN_TYPE op_bulk_table_fill(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 table_index = 0;
    if (pop_reg_u64_checked(job, &table_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    if (!table) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t length = 0;
    if (pop_u32_checked(job, &length) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_ptr value = 0;
    if (pop_ref_checked(job, &value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (runtime_validate_table_ref_value(runtime, table, value) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    uint32_t start = 0;
    if (pop_u32_checked(job, &start) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if ((uint64_t)start + length > table->size) {
        return FA_RUNTIME_ERR_TRAP;
    }
    for (uint32_t i = 0; i < length; ++i) {
        table->data[start + i] = value;
    }
    return FA_RUNTIME_OK;
}

/*
 * 0xFC bulk-memory/table dispatch now uses a prebuilt subopcode table.
 * The runtime still decodes immediates once, but execution no longer walks
 * a per-call switch tower for these suboperations.
 */
OP_RETURN_TYPE op_bulk_memory(OP_ARGUMENTS) {
    static const Operation kBulkHandlers[] = {
        [0] = op_convert_i32_trunc_sat_f32_s_mc,
        [1] = op_convert_i32_trunc_sat_f32_u_mc,
        [2] = op_convert_i32_trunc_sat_f64_s_mc,
        [3] = op_convert_i32_trunc_sat_f64_u_mc,
        [4] = op_convert_i64_trunc_sat_f32_s_mc,
        [5] = op_convert_i64_trunc_sat_f32_u_mc,
        [6] = op_convert_i64_trunc_sat_f64_s_mc,
        [7] = op_convert_i64_trunc_sat_f64_u_mc,
        [8] = op_bulk_memory_init,
        [9] = op_bulk_data_drop,
        [10] = op_bulk_memory_copy,
        [11] = op_bulk_memory_fill,
        [12] = op_bulk_table_init,
        [13] = op_bulk_elem_drop,
        [14] = op_bulk_table_copy,
        [15] = op_bulk_table_grow,
        [16] = op_bulk_table_size,
        [17] = op_bulk_table_fill
    };
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    u64 subopcode = 0;
    if (pop_reg_u64_checked(job, &subopcode) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (subopcode >= (sizeof(kBulkHandlers) / sizeof(kBulkHandlers[0]))) {
        return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
    Operation handler = kBulkHandlers[subopcode];
    if (!handler) {
        return FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE;
    }
    return handler(runtime, job, descriptor);
}

static int runtime_table_grow(fa_Runtime* runtime, u64 table_index, u64 delta, fa_ptr init_value, u64* prev_size_out, bool* grew_out) {
    if (!runtime || !prev_size_out || !grew_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_RuntimeTable* table = runtime_get_table(runtime, table_index);
    if (!table) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    int status = runtime_validate_table_ref_value(runtime, table, init_value);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    *prev_size_out = table->size;
    *grew_out = false;
    if (delta == 0) {
        *grew_out = true;
        return FA_RUNTIME_OK;
    }
    if (!table->owns_data) {
        return FA_RUNTIME_OK;
    }
    if (delta > UINT32_MAX - table->size) {
        return FA_RUNTIME_OK;
    }
    const size_t new_size = (size_t)table->size + (size_t)delta;
    if (new_size > UINT32_MAX) {
        return FA_RUNTIME_OK;
    }
    if (table->has_max && new_size > (size_t)table->max_size) {
        return FA_RUNTIME_OK;
    }
    if (new_size > SIZE_MAX / sizeof(fa_ptr)) {
        return FA_RUNTIME_OK;
    }
    fa_ptr* new_data = (fa_ptr*)realloc(table->data, new_size * sizeof(fa_ptr));
    if (!new_data) {
        return FA_RUNTIME_OK;
    }
    for (uint32_t i = table->size; i < (uint32_t)new_size; ++i) {
        new_data[i] = init_value;
    }
    table->data = new_data;
    table->size = (uint32_t)new_size;
    *grew_out = true;
    return FA_RUNTIME_OK;
}

/*
 * Grow helpers mirror WASM semantics:
 * - structural failure (limits/alloc) is reported via `grew_out = false`
 * - hard runtime faults still return an error code
 */
static int runtime_memory_grow(fa_Runtime* runtime, u64 mem_index, u64 delta_pages, u64* prev_pages_out, bool* grew_out) {
    if (!runtime || !prev_pages_out || !grew_out) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    int status = fa_Runtime_ensureMemoryLoaded(runtime, (uint32_t)mem_index);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    const uint64_t prev_pages = memory->size_bytes / FA_WASM_PAGE_SIZE;
    if (!memory->is_memory64 && prev_pages > UINT32_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    *prev_pages_out = prev_pages;
    *grew_out = false;
    if (delta_pages == 0) {
        *grew_out = true;
        return FA_RUNTIME_OK;
    }
    if (!memory->owns_data) {
        return FA_RUNTIME_OK;
    }
    const uint64_t new_pages = prev_pages + delta_pages;
    if (new_pages < prev_pages) {
        return FA_RUNTIME_OK;
    }
    if (memory->has_max) {
        const uint64_t max_pages = memory->max_size_bytes / FA_WASM_PAGE_SIZE;
        if (new_pages > max_pages) {
            return FA_RUNTIME_OK;
        }
    }
    if (new_pages > (UINT64_MAX / FA_WASM_PAGE_SIZE)) {
        return FA_RUNTIME_OK;
    }
    const uint64_t new_size_bytes = new_pages * FA_WASM_PAGE_SIZE;
    if (new_size_bytes > SIZE_MAX || new_size_bytes > (uint64_t)INT_MAX) {
        return FA_RUNTIME_OK;
    }
    uint8_t* new_data = (uint8_t*)runtime->malloc((int)new_size_bytes);
    if (!new_data) {
        return FA_RUNTIME_OK;
    }
    if (memory->data && memory->size_bytes > 0) {
        memcpy(new_data, memory->data, (size_t)memory->size_bytes);
    }
    if (new_size_bytes > memory->size_bytes) {
        memset(new_data + memory->size_bytes, 0, (size_t)(new_size_bytes - memory->size_bytes));
    }
    if (memory->data) {
        runtime->free(memory->data);
    }
    memory->data = new_data;
    memory->size_bytes = new_size_bytes;
    *grew_out = true;
    return FA_RUNTIME_OK;
}

OP_RETURN_TYPE op_memory_size(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->module || runtime->module->num_memories == 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 mem_index = 0;
    if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* memory = runtime_get_memory(runtime, mem_index);
    if (!memory) {
        return FA_RUNTIME_ERR_TRAP;
    }
    const uint64_t pages = memory->size_bytes / FA_WASM_PAGE_SIZE;
    const uint8_t bits = memory->is_memory64 ? 64U : 32U;
    const bool is_signed = true;
    if (!memory->is_memory64 && pages > UINT32_MAX) {
        return FA_RUNTIME_ERR_UNSUPPORTED;
    }
    return push_int_checked(job, pages, bits, is_signed);
}

OP_RETURN_TYPE op_memory_grow(OP_ARGUMENTS) {
    (void)descriptor;
    if (!runtime || !job) {
        return FA_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->module || runtime->module->num_memories == 0) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 mem_index = 0;
    if (pop_reg_u64_checked(job, &mem_index) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    fa_RuntimeMemory* memory = NULL;
    int status = runtime_require_memory(runtime, mem_index, &memory);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    fa_JobValue delta;
    if (pop_stack_checked(job, &delta) != FA_RUNTIME_OK) {
        return FA_RUNTIME_ERR_TRAP;
    }
    u64 delta_pages_raw = 0;
    if (memory->is_memory64) {
        if (delta.kind != fa_job_value_i64) {
            return FA_RUNTIME_ERR_TRAP;
        }
    } else {
        if (delta.kind != fa_job_value_i32) {
            return FA_RUNTIME_ERR_TRAP;
        }
    }
    if (!job_value_to_u64(&delta, &delta_pages_raw)) {
        return FA_RUNTIME_ERR_TRAP;
    }
    if (!memory->is_memory64 && delta_pages_raw > UINT32_MAX) {
        return push_int_checked(job, (u32)UINT32_MAX, 32U, true);
    }
    u64 prev_pages = 0;
    bool grew = false;
    status = runtime_memory_grow(runtime, mem_index, delta_pages_raw, &prev_pages, &grew);
    if (status != FA_RUNTIME_OK) {
        return status;
    }
    if (!grew) {
        if (memory->is_memory64) {
            return push_int_checked(job, UINT64_MAX, 64U, true);
        }
        return push_int_checked(job, (u32)UINT32_MAX, 32U, true);
    }
    return push_int_checked(job, prev_pages, memory->is_memory64 ? 64U : 32U, true);
}
