#include "fa_wasm_stream.h"
#include <stdlib.h>
#include <string.h> // For memset
#include <stdio.h>  // For NULL if not in stdlib.h for some compilers

// --- Helper LEB128 decoders for memory buffers ---

static uint32_t read_uleb128_from_memory(const uint8_t* buffer, uint32_t buffer_size, 
                                         uint32_t* current_offset_ptr, uint32_t* bytes_read_count) {
    uint32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *bytes_read_count = 0;
    uint32_t local_offset = *current_offset_ptr;

    do {
        if (local_offset >= buffer_size) {
            // Signal error: Read out of bounds
            // The caller should check bytes_read_count or a return code if this were more robust
            *current_offset_ptr = local_offset;
            // Mark as an error or return a sentinel if the function could return errors
            return 0; // Or handle error appropriately
        }
        byte = buffer[local_offset++];
        (*bytes_read_count)++;

        result |= ((uint32_t)(byte & 0x7F) << shift);
        shift += 7;
    } while ((byte & 0x80) && (shift < 32)); // Max 5 bytes for 32-bit ULEB128

    if ((byte & 0x80) && (shift >=32)) {
        // Malformed: ULEB128 for 32-bit integer is too long
        // Handle error
    }

    *current_offset_ptr = local_offset;
    return result;
}

static int32_t read_sleb128_from_memory(const uint8_t* buffer, uint32_t buffer_size,
                                        uint32_t* current_offset_ptr, uint32_t* bytes_read_count) {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *bytes_read_count = 0;
    uint32_t local_offset = *current_offset_ptr;

    do {
        if (local_offset >= buffer_size) {
            *current_offset_ptr = local_offset;
            return 0; // Error
        }
        byte = buffer[local_offset++];
        (*bytes_read_count)++;

        result |= ((int32_t)(byte & 0x7F) << shift);
        shift += 7;
    } while ((byte & 0x80) && (shift < 32));

    if ((byte & 0x80) && (shift >=32)) {
        // Malformed: SLEB128 for 32-bit integer is too long
        // Handle error
    }

    if ((shift < 32) && (byte & 0x40)) { // Sign extend
        result |= (~((int32_t)0) << shift);
    }

    *current_offset_ptr = local_offset;
    return result;
}


// --- Initialization and Cleanup ---

WasmInstructionStream* wasm_instruction_stream_init(WasmModule* module) {
    if (!module) {
        return NULL;
    }
    WasmInstructionStream* stream = (WasmInstructionStream*)malloc(sizeof(WasmInstructionStream));
    if (!stream) {
        return NULL;
    }
    stream->module = module;
    stream->current_function_idx = UINT32_MAX; // No function loaded initially
    stream->function_bytecode = NULL;
    stream->bytecode_size = 0;
    stream->pc = 0;
    stream->is_loaded = 0;
    return stream;
}

void wasm_instruction_stream_free(WasmInstructionStream* stream) {
    if (!stream) {
        return;
    }
    wasm_instruction_stream_unload_current_function(stream);
    free(stream);
}

// --- Loading and Unloading Function Bytecode ---

int wasm_instruction_stream_load_function(WasmInstructionStream* stream, uint32_t function_idx) {
    if (!stream || !stream->module) {
        return -1;
    }
    if (function_idx >= stream->module->num_functions) {
        return -1; // Invalid function index
    }

    // If the same function is already loaded, just reset PC (or do nothing based on desired behavior)
    if (stream->is_loaded && stream->current_function_idx == function_idx) {
        stream->pc = 0;
        return 0; // Success, already loaded
    }

    // Unload any currently loaded function
    if (stream->is_loaded) {
        wasm_instruction_stream_unload_current_function(stream);
    }

    // Load the new function's bytecode using the provided fa_wasm function
    stream->function_bytecode = wasm_load_function_body(stream->module, function_idx);
    if (!stream->function_bytecode) {
        stream->is_loaded = 0;
        return -1; // Failed to load function body
    }

    stream->bytecode_size = stream->module->functions[function_idx].body_size; //
    stream->current_function_idx = function_idx;
    stream->pc = 0; // Reset PC to the beginning of the new function
    stream->is_loaded = 1;
    
    return 0; // Success
}

void wasm_instruction_stream_unload_current_function(WasmInstructionStream* stream) {
    if (stream && stream->is_loaded && stream->function_bytecode) {
        free(stream->function_bytecode);
        stream->function_bytecode = NULL;
    }
    if (stream) {
        stream->bytecode_size = 0;
        stream->pc = 0;
        stream->current_function_idx = UINT32_MAX;
        stream->is_loaded = 0;
    }
}

// --- Instruction Pointer, Fetching, and Navigation ---

WasmOpcode wasm_instruction_stream_peek_opcode(WasmInstructionStream* stream) {
    if (!stream || !stream->is_loaded || stream->pc >= stream->bytecode_size) {
        // Consider a more robust error reporting mechanism for a real VM
        return 0xFF; // Example: Return an invalid opcode or handle error
    }
    return stream->function_bytecode[stream->pc];
}

int wasm_instruction_stream_read_byte(WasmInstructionStream* stream, uint8_t* byte) {
    if (!stream || !stream->is_loaded || !byte) {
        return -1;
    }
    if (stream->pc >= stream->bytecode_size) {
        return -1; // PC out of bounds
    }
    *byte = stream->function_bytecode[stream->pc];
    stream->pc++;
    return 0;
}

int wasm_instruction_stream_advance_pc(WasmInstructionStream* stream, uint32_t num_bytes) {
    if (!stream || !stream->is_loaded) {
        return -1;
    }
    if (stream->pc + num_bytes > stream->bytecode_size) { // Check for overflow and bounds
        // Potentially set PC to bytecode_size to indicate end, or error out
        return -1; // New PC would be out of bounds
    }
    stream->pc += num_bytes;
    return 0;
}

int wasm_instruction_stream_set_pc(WasmInstructionStream* stream, uint32_t offset) {
    if (!stream || !stream->is_loaded) {
        return -1;
    }
    if (offset >= stream->bytecode_size) {
        return -1; // Offset out of bounds
    }
    stream->pc = offset;
    return 0;
}

int wasm_instruction_stream_read_uleb128(WasmInstructionStream* stream, uint32_t* value, uint32_t* bytes_read) {
    if (!stream || !stream->is_loaded || !value || !bytes_read) {
        return -1;
    }
    uint32_t initial_pc = stream->pc;
    *value = read_uleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, bytes_read);
    
    if (*bytes_read == 0 || stream->pc > stream->bytecode_size) { // Error during read or went out of bounds
        stream->pc = initial_pc; // Restore PC
        return -1;
    }
    return 0;
}

int wasm_instruction_stream_read_sleb128(WasmInstructionStream* stream, int32_t* value, uint32_t* bytes_read) {
    if (!stream || !stream->is_loaded || !value || !bytes_read) {
        return -1;
    }
    uint32_t initial_pc = stream->pc;
    *value = read_sleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, bytes_read);

    if (*bytes_read == 0 || stream->pc > stream->bytecode_size) { // Error during read or went out of bounds
        stream->pc = initial_pc; // Restore PC
        return -1;
    }
    return 0;
}

// --- Address Mapping and Information ---

int wasm_get_function_details_from_global_offset(WasmModule* module, off_t global_code_offset, uint32_t* out_function_idx, uint32_t* out_offset_in_function) {
    if (!module || !module->functions || !out_function_idx || !out_offset_in_function) {
        return -1;
    }

    for (uint32_t i = 0; i < module->num_functions; ++i) {
        WasmFunction* func = &module->functions[i]; //
        // body_offset is the start of the function (including its size ULEB) in the Code section payload
        // body_size is the size of the actual code and locals.
        // The question is if global_code_offset refers to an offset within the file or within the concatenated
        // function bodies. The `body_offset` in `WasmFunction` as per `fa_wasm.c` is the offset
        // from the start of the file to the function's code.

        if (global_code_offset >= func->body_offset && 
            global_code_offset < (func->body_offset + func->body_size)) {
            *out_function_idx = i;
            *out_offset_in_function = (uint32_t)(global_code_offset - func->body_offset);
            return 0; // Found
        }
    }
    return -1; // Not found
}

uint32_t wasm_instruction_stream_get_current_function_idx(WasmInstructionStream* stream) {
    if (stream && stream->is_loaded) {
        return stream->current_function_idx;
    }
    return UINT32_MAX; // Or some other indicator for "not loaded"
}

uint32_t wasm_instruction_stream_get_pc_offset(WasmInstructionStream* stream) {
    if (stream && stream->is_loaded) {
        return stream->pc;
    }
    return 0; 
}

off_t wasm_instruction_stream_get_global_pc_offset(WasmInstructionStream* stream) {
    if (!stream || !stream->is_loaded || !stream->module || stream->current_function_idx >= stream->module->num_functions) {
        return (off_t)-1;
    }
    
    // Get the base offset of the current function's body in the file
    off_t function_base_offset_in_file = stream->module->functions[stream->current_function_idx].body_offset;
    
    return function_base_offset_in_file + stream->pc;
}