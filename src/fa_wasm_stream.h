#pragma once

#include "fa_wasm.h" // Assuming fa_wasm.h is in the include path
#include <stdint.h>
#include <stddef.h> // For size_t

// Forward declaration
//typedef struct WasmModule WasmModule;

/**
 * @brief Represents the state of the WASM instruction stream for execution.
 *
 * This structure holds the currently loaded function's bytecode, the program
 * counter (pc) within that bytecode, and a reference to the overall WASM module.
 * It facilitates lazy loading by only keeping one function's bytecode in memory
 * at a time (the one currently being executed or about to be).
 */
typedef struct {
    WasmModule* module;             /**< Pointer to the parsed WASM module. */
    uint32_t current_function_idx;  /**< Index of the function currently loaded. */
    uint8_t* function_bytecode;     /**< Buffer holding the bytecode for the current_function_idx. */
    uint32_t bytecode_size;         /**< Size of function_bytecode in bytes. */
    uint32_t pc;                    /**< Program counter: offset within function_bytecode. */
    int      is_loaded;             /**< Flag to indicate if a function is currently loaded. */
} WasmInstructionStream;

/**
 * @brief Represents a WASM opcode.
 *
 * For simplicity, this is typedef'd as a uint8_t. Real WASM instruction
 * decoding would involve parsing this opcode and any subsequent immediates.
 */
typedef uint8_t WasmOpcode;


// --- Initialization and Cleanup ---

/**
 * @brief Initializes a new WASM instruction stream manager.
 *
 * @param module A pointer to an already parsed WasmModule.
 * @return A pointer to the newly allocated WasmInstructionStream, or NULL on failure.
 * The stream initially has no function loaded.
 */
WasmInstructionStream* wasm_instruction_stream_init(WasmModule* module);

/**
 * @brief Frees the WasmInstructionStream and any loaded function bytecode.
 *
 * @param stream A pointer to the WasmInstructionStream to be freed.
 */
void wasm_instruction_stream_free(WasmInstructionStream* stream);


// --- Loading and Unloading Function Bytecode (Lazy Loading) ---

/**
 * @brief Loads a function's bytecode into the instruction stream.
 *
 * If another function is already loaded, it will be unloaded first.
 * If the requested function is already loaded, the PC is reset to 0.
 * Uses `wasm_load_function_body` from `fa_wasm.h` internally.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @param function_idx The index of the function to load (from WasmModule's functions array).
 * @return 0 on success, -1 on failure (e.g., invalid index, memory allocation error, read error).
 */
int wasm_instruction_stream_load_function(WasmInstructionStream* stream, uint32_t function_idx);

/**
 * @brief Unloads the currently loaded function's bytecode from the stream.
 *
 * This frees the memory occupied by the function's bytecode and resets
 * related fields in the stream.
 *
 * @param stream A pointer to the WasmInstructionStream.
 */
void wasm_instruction_stream_unload_current_function(WasmInstructionStream* stream);


// --- Instruction Pointer, Fetching, and Navigation ---

/**
 * @brief Gets the opcode byte at the current program counter (PC).
 *
 * Does not advance the PC. Assumes a function is loaded.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @return The WasmOpcode (byte) at the current PC. Returns 0 or an error
 * indicator if no function is loaded or PC is out of bounds.
 * (Error handling might need to be more robust for a real VM).
 */
WasmOpcode wasm_instruction_stream_peek_opcode(WasmInstructionStream* stream);

/**
 * @brief Reads the byte at the current program counter (PC) and advances the PC by one.
 *
 * Assumes a function is loaded.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @param[out] byte Pointer to store the read byte.
 * @return 0 on success, -1 if no function is loaded or PC is out of bounds.
 */
int wasm_instruction_stream_read_byte(WasmInstructionStream* stream, uint8_t* byte);

/**
 * @brief Advances the program counter (PC) by a specified number of bytes.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @param num_bytes The number of bytes to advance the PC.
 * @return 0 on success, -1 if the new PC would be out of bounds or no function is loaded.
 */
int wasm_instruction_stream_advance_pc(WasmInstructionStream* stream, uint32_t num_bytes);

/**
 * @brief Sets the program counter (PC) to a specific offset within the current function's bytecode.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @param offset The new offset for the PC.
 * @return 0 on success, -1 if the offset is out of bounds or no function is loaded.
 */
int wasm_instruction_stream_set_pc(WasmInstructionStream* stream, uint32_t offset);

/**
 * @brief Reads a ULEB128 encoded unsigned 32-bit integer from the current PC
 * and advances the PC past the read bytes.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @param[out] value Pointer to store the decoded ULEB128 value.
 * @param[out] bytes_read Pointer to store the number of bytes consumed from the stream for this value.
 * @return 0 on success, -1 on failure (e.g., out of bounds, malformed).
 */
int wasm_instruction_stream_read_uleb128(WasmInstructionStream* stream, uint32_t* value, uint32_t* bytes_read);

/**
 * @brief Reads an SLEB128 encoded signed 32-bit integer from the current PC
 * and advances the PC past the read bytes.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @param[out] value Pointer to store the decoded SLEB128 value.
 * @param[out] bytes_read Pointer to store the number of bytes consumed from the stream for this value.
 * @return 0 on success, -1 on failure (e.g., out of bounds, malformed).
 */
int wasm_instruction_stream_read_sleb128(WasmInstructionStream* stream, int32_t* value, uint32_t* bytes_read);


// --- Address Mapping and Information ---

/**
 * @brief Determines the function index and offset within that function for a given global file offset.
 *
 * A "global file offset" refers to an absolute byte offset from the beginning of the WASM file's
 * code section where function bodies are defined.
 * This uses the `body_offset` and `body_size` from the `WasmFunction` structures in `WasmModule`.
 *
 * @param module A pointer to the WasmModule.
 * @param global_code_offset The global file offset into the code section.
 * @param[out] out_function_idx Pointer to store the index of the function containing this offset.
 * @param[out] out_offset_in_function Pointer to store the offset within that function's bytecode.
 * @return 0 if the offset maps to a function, -1 otherwise (e.g., offset is outside any known function body).
 */
int wasm_get_function_details_from_global_offset(WasmModule* module, off_t global_code_offset, uint32_t* out_function_idx, uint32_t* out_offset_in_function);

/**
 * @brief Gets the index of the currently loaded function.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @return The current function index, or a special value (e.g., UINT32_MAX) if no function is loaded.
 */
uint32_t wasm_instruction_stream_get_current_function_idx(WasmInstructionStream* stream);

/**
 * @brief Gets the current program counter (PC) offset within the loaded function's bytecode.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @return The current PC offset, or 0 if no function is loaded.
 */
uint32_t wasm_instruction_stream_get_pc_offset(WasmInstructionStream* stream);

/**
 * @brief Gets the global file offset corresponding to the current PC.
 *
 * This calculates the absolute offset in the WASM file's code section.
 *
 * @param stream A pointer to the WasmInstructionStream.
 * @return The global file offset, or (off_t)-1 if no function is loaded or an error occurs.
 */
off_t wasm_instruction_stream_get_global_pc_offset(WasmInstructionStream* stream);