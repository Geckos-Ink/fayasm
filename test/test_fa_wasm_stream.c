#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> // Required for UINT32_MAX

// Assuming fa_wasm.h and wasm_exec_stream.h are in the include path
// and fa_wasm.c, wasm_exec_stream.c will be linked.
#include "fa_wasm.h"
#include "wasm_exec_stream.h"

// --- Configuration for the simulation ---
#define MAX_CALL_DEPTH 32 // Simple call stack limit for simulation
#define MAX_EXECUTION_STEPS 1000 // Prevent infinite loops in simulation

// --- Simulated Call Stack Frame ---
typedef struct {
    uint32_t function_idx;
    uint32_t return_pc; // PC in the *caller* to return to
    // In a real VM, you'd also store local variables base pointer, etc.
} CallFrame;

// --- Main Simulation Function ---

/**
 * @brief Simulates a basic WASM runtime for a given module and entry function.
 *
 * @param wasm_file_path Path to the .wasm file.
 * @param entry_function_name Name of the exported function to start execution from.
 * @return 0 on successful simulation (or graceful exit), -1 on error.
 */
int simulate_wasm_runtime(const char* wasm_file_path, const char* entry_function_name) {
    printf("=== WASM Runtime Simulation Starting ===\n");
    printf("Module: %s, Entry Function: %s\n\n", wasm_file_path, entry_function_name);

    // 1. Initialize and load the WASM module
    WasmModule* module = wasm_module_init(wasm_file_path);
    if (!module) {
        fprintf(stderr, "Error: Failed to initialize WASM module from '%s'.\n", wasm_file_path);
        return -1;
    }

    printf("Loading module header...\n");
    if (wasm_load_header(module) != 0) {
        fprintf(stderr, "Error: Failed to load WASM header.\n");
        wasm_module_free(module);
        return -1;
    }

    printf("Scanning sections...\n");
    if (wasm_scan_sections(module) != 0) {
        fprintf(stderr, "Error: Failed to scan WASM sections.\n");
        wasm_module_free(module);
        return -1;
    }

    // Load necessary sections for execution
    printf("Loading types section...\n");
    if (wasm_load_types(module) != 0) { /*fprintf(stderr, "Warning: Failed to load types section (or not present).\n");*/ }
    
    printf("Loading functions section...\n");
    if (wasm_load_functions(module) != 0) { /*fprintf(stderr, "Warning: Failed to load functions section (or not present).\n");*/ }
    
    printf("Loading exports section...\n");
    if (wasm_load_exports(module) != 0) { /*fprintf(stderr, "Warning: Failed to load exports section (or not present).\n");*/ }
    
    // wasm_print_info(module); // Optional: Print module info for debugging

    // 2. Find the entry function by its export name
    uint32_t start_function_idx = UINT32_MAX;
    if (module->num_exports > 0 && entry_function_name != NULL) {
        for (uint32_t i = 0; i < module->num_exports; ++i) {
            if (module->exports[i].name && strcmp(module->exports[i].name, entry_function_name) == 0) {
                if (module->exports[i].kind == 0) { // Kind 0 is function
                    start_function_idx = module->exports[i].index;
                    printf("Found exported entry function '%s' at index %u.\n", entry_function_name, start_function_idx);
                    break;
                } else {
                    fprintf(stderr, "Warning: Export '%s' is not a function (kind %u).\n", entry_function_name, module->exports[i].kind);
                }
            }
        }
    } else if (entry_function_name == NULL && module->num_functions > 0) {
        // If no entry function name specified, try to use function 0 if available (common for simple test cases)
        start_function_idx = 0;
        printf("No entry function name specified, attempting to start with function index 0.\n");
    }


    if (start_function_idx == UINT32_MAX) {
        if (entry_function_name)
            fprintf(stderr, "Error: Entry function '%s' not found or not a function export.\n", entry_function_name);
        else
            fprintf(stderr, "Error: No entry function could be determined.\n");
        wasm_module_free(module);
        return -1;
    }
    if (start_function_idx >= module->num_functions) {
         fprintf(stderr, "Error: Start function index %u is out of bounds (num_functions: %u).\n", start_function_idx, module->num_functions);
         wasm_module_free(module);
         return -1;
    }


    // 3. Initialize the instruction stream
    WasmInstructionStream* stream = wasm_instruction_stream_init(module);
    if (!stream) {
        fprintf(stderr, "Error: Failed to initialize WASM instruction stream.\n");
        wasm_module_free(module);
        return -1;
    }

    // 4. Simulated Call Stack
    CallFrame call_stack[MAX_CALL_DEPTH];
    int call_stack_ptr = -1; // Points to the top of the stack

    // Initial "call" to the entry function
    printf("\nAttempting to load entry function %u...\n", start_function_idx);
    if (wasm_instruction_stream_load_function(stream, start_function_idx) != 0) {
        fprintf(stderr, "Error: Failed to load entry function %u.\n", start_function_idx);
        wasm_instruction_stream_free(stream);
        wasm_module_free(module);
        return -1;
    }
    printf("Successfully loaded function %u. Bytecode size: %u bytes.\n",
           stream->current_function_idx, stream->bytecode_size);


    // 5. Main Execution Loop
    uint8_t opcode_byte;
    uint32_t uleb_val, sleb_val_signed, bytes_read_leb;
    int execution_steps = 0;

    printf("\n--- Starting Execution Simulation ---\n");
    while (stream->is_loaded && execution_steps < MAX_EXECUTION_STEPS) {
        if (stream->pc >= stream->bytecode_size) {
            printf("[INFO] Reached end of bytecode for function %u (PC=%u, Size=%u).\n",
                   stream->current_function_idx, stream->pc, stream->bytecode_size);
            // This should ideally be handled by an 'end' opcode or a return.
            // For this simulation, if we run off the end, we treat it like a return.
            if (call_stack_ptr >= 0) { // Return from call
                CallFrame current_frame = call_stack[call_stack_ptr--];
                printf("Returning from function %u to function %u at PC %u.\n",
                       stream->current_function_idx, current_frame.function_idx, current_frame.return_pc);
                if (wasm_instruction_stream_load_function(stream, current_frame.function_idx) != 0) {
                    fprintf(stderr, "Error: Failed to load function %u on return.\n", current_frame.function_idx);
                    break; // Error
                }
                wasm_instruction_stream_set_pc(stream, current_frame.return_pc);
            } else { // End of main/entry function
                printf("Execution finished (end of entry function).\n");
                break;
            }
            continue;
        }

        uint32_t current_pc_local = wasm_instruction_stream_get_pc_offset(stream);
        off_t current_pc_global = wasm_instruction_stream_get_global_pc_offset(stream);

        // Read the opcode byte
        if (wasm_instruction_stream_read_byte(stream, &opcode_byte) != 0) {
            fprintf(stderr, "Error: Failed to read opcode byte at F%u:PC%u (Global: %ld).\n",
                    stream->current_function_idx, current_pc_local, (long)current_pc_global);
            break;
        }
        execution_steps++;

        printf("[F%u:PC%04u (G:%05ld)] Opcode: 0x%02X ",
               stream->current_function_idx, current_pc_local, (long)current_pc_global, opcode_byte);

        switch (opcode_byte) {
            case 0x00: // unreachable
                printf("unreachable\n");
                printf("Simulation halted due to unreachable opcode.\n");
                wasm_instruction_stream_unload_current_function(stream); // Stop execution
                break;

            case 0x01: // nop
                printf("nop\n");
                break;

            case 0x0B: // end
                printf("end\n");
                if (call_stack_ptr >= 0) { // Return from a call
                    CallFrame current_frame = call_stack[call_stack_ptr--];
                    printf("Returning from function %u to function %u at PC %u.\n",
                           stream->current_function_idx, current_frame.function_idx, current_frame.return_pc);
                    if (wasm_instruction_stream_load_function(stream, current_frame.function_idx) != 0) {
                        fprintf(stderr, "Error: Failed to load function %u on return.\n", current_frame.function_idx);
                        wasm_instruction_stream_unload_current_function(stream); // Stop
                    } else {
                         wasm_instruction_stream_set_pc(stream, current_frame.return_pc);
                    }
                } else { // End of the main/entry function
                    printf("Execution finished (end opcode in entry function).\n");
                    wasm_instruction_stream_unload_current_function(stream); // Stop execution
                }
                break;

            case 0x10: // call
                if (wasm_instruction_stream_read_uleb128(stream, &uleb_val, &bytes_read_leb) == 0) {
                    printf("call (func_idx: %u)\n", uleb_val);
                    if (call_stack_ptr + 1 < MAX_CALL_DEPTH) {
                        call_stack_ptr++;
                        call_stack[call_stack_ptr].function_idx = stream->current_function_idx;
                        call_stack[call_stack_ptr].return_pc = stream->pc; // PC after the call instruction

                        printf("Calling function %u from function %u. Return PC will be %u.\n",
                               uleb_val, stream->current_function_idx, stream->pc);

                        if (wasm_instruction_stream_load_function(stream, uleb_val) != 0) {
                            fprintf(stderr, "Error: Failed to load called function %u.\n", uleb_val);
                            wasm_instruction_stream_unload_current_function(stream); // Stop
                        } else {
                             printf("Successfully loaded function %u. Bytecode size: %u bytes.\n",
                                    stream->current_function_idx, stream->bytecode_size);
                        }
                    } else {
                        fprintf(stderr, "Error: Call stack overflow!\n");
                        wasm_instruction_stream_unload_current_function(stream); // Stop
                    }
                } else {
                    fprintf(stderr, "Error reading call func_idx.\n");
                    wasm_instruction_stream_unload_current_function(stream); // Stop
                }
                break;

            case 0x20: // local.get
                if (wasm_instruction_stream_read_uleb128(stream, &uleb_val, &bytes_read_leb) == 0) {
                    printf("local.get (local_idx: %u)\n", uleb_val);
                    // In a real VM: push local[uleb_val] onto operand stack
                } else {
                    fprintf(stderr, "Error reading local.get index.\n");
                    wasm_instruction_stream_unload_current_function(stream); // Stop
                }
                break;

            case 0x41: // i32.const
                if (wasm_instruction_stream_read_sleb128(stream, (int32_t*)&sleb_val_signed, &bytes_read_leb) == 0) {
                    printf("i32.const (value: %d)\n", (int32_t)sleb_val_signed);
                    // In a real VM: push sleb_val_signed onto operand stack
                } else {
                    fprintf(stderr, "Error reading i32.const value.\n");
                    wasm_instruction_stream_unload_current_function(stream); // Stop
                }
                break;
            
            // Add more opcodes here as needed for a more complete simulation
            // Example: 0x02 (block), 0x03 (loop), 0x04 (if), 0x0C (br), 0x0D (br_if)

            default:
                printf("UNKNOWN OPCODE\n");
                // For simplicity, we stop on unknown opcodes. A real VM might error or have other behavior.
                // wasm_instruction_stream_unload_current_function(stream);
                break;
        }
         // Small delay for readability if running too fast
         // #include <unistd.h> // for usleep
         // usleep(10000); 
    }

    if (execution_steps >= MAX_EXECUTION_STEPS) {
        printf("\n[WARN] Simulation stopped: Maximum execution steps reached (%d).\n", MAX_EXECUTION_STEPS);
    }

    printf("\n--- Execution Simulation Ended ---\n");

    // 6. Cleanup
    printf("\nCleaning up resources...\n");
    wasm_instruction_stream_free(stream);
    wasm_module_free(module);

    printf("=== WASM Runtime Simulation Complete ===\n");
    return 0;
}

// --- Example Main Function ---
// You would typically compile this file along with fa_wasm.c and wasm_exec_stream.c
// Example compile command (GCC):
// gcc -o wasm_sim main_simulation.c fa_wasm.c wasm_exec_stream.c -I. -Wall -g
// (Assuming all .c and .h files are in the current directory, and -I. tells GCC to look here for headers)

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_wasm_file> [entry_function_name]\n", argv[0]);
        fprintf(stderr, "Example: %s my_module.wasm main\n", argv[0]);
        fprintf(stderr, "If entry_function_name is omitted, it will try to run function index 0 if available.\n");
        // Create a tiny dummy WASM file for testing if no arguments are given
        const char* dummy_wasm_content = 
            "\x00\x61\x73\x6d" // magic
            "\x01\x00\x00\x00" // version
            // Type section (id 1)
            "\x01" // section id
            "\x07" // section size
            "\x01" // num types
            "\x60" // func type
            "\x00" // num params
            "\x00" // num results
            // Function section (id 3)
            "\x03" // section id
            "\x02" // section size
            "\x01" // num functions
            "\x00" // type index 0 for function 0
            // Export section (id 7)
            "\x07" // section id
            "\x08" // section size
            "\x01" // num exports
            "\x04" // string length of "main"
            "\x6d\x61\x69\x6e" // "main"
            "\x00" // export kind: function
            "\x00" // export func index 0
            // Code section (id 10)
            "\x0a" // section id
            "\x09" // section size
            "\x01" // num functions
            // Function 0 body
            "\x07" // body size (excluding this size byte)
            "\x00" // num local entries (0 for now)
            "\x41\x2a" // i32.const 42
            "\x20\x00" // local.get 0 (dummy, no locals defined, but shows opcode)
            "\x01"     // nop
            "\x0b";    // end
        
        FILE* fp = fopen("dummy.wasm", "wb");
        if (fp) {
            fwrite(dummy_wasm_content, 1, sizeof(dummy_wasm_content) -1, fp);
            fclose(fp);
            printf("Created dummy.wasm for testing. Run: %s dummy.wasm main\n", argv[0]);
        }
        return 1;
    }

    const char* wasm_file = argv[1];
    const char* entry_func = (argc > 2) ? argv[2] : "main"; // Default to "main" or let simulation try index 0

    if (argc <=2 && strcmp(wasm_file, "dummy.wasm") == 0) {
         printf("Running simulation with dummy.wasm and entry function 'main'.\n");
         entry_func = "main"; // Ensure "main" is used for the dummy
    } else if (argc <= 2) { // If only wasm file is provided, try with NULL entry function name
        printf("Running simulation with %s and attempting default entry (function index 0 or exported 'main').\n", wasm_file);
        entry_func = NULL; // Let simulate_wasm_runtime try to find 'main' or use index 0
    }


    return simulate_wasm_runtime(wasm_file, entry_func);
}