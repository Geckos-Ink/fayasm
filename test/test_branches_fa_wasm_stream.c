#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

// Include the necessary headers from your project
#include "fa_wasm.h"
#include "fa_wasm_stream.h"

// --- Helper function for scanning ---

/**
 * @brief Scans forward in the stream to find the matching 'else' or 'end' for a block.
 *
 * This function is crucial for handling control flow without loading the entire function.
 * It starts scanning from the stream's current PC. It correctly handles nested blocks.
 *
 * @param stream The instruction stream.
 * @param found_else Pointer to an integer that will be 1 if an 'else' was found, 0 otherwise.
 * @return The PC offset of the instruction *after* the matching 'end' or at the 'else'.
 * Returns (uint32_t)-1 on failure (e.g., end of stream reached before block end).
 */
uint32_t wasm_instruction_stream_find_block_end(WasmInstructionStream* stream, int* found_else) {
    if (!stream || !stream->is_loaded) return (uint32_t)-1;

    uint32_t depth = 1;
    uint32_t scan_pc = stream->pc; // Start scanning from the current PC
    *found_else = 0;

    while (depth > 0 && scan_pc < stream->bytecode_size) {
        uint8_t opcode = stream->function_bytecode[scan_pc++];

        switch (opcode) {
            case 0x02: // block
            case 0x03: // loop
            case 0x04: // if
                depth++;
                // Skip immediates (type signature for block)
                if (opcode == 0x02 || opcode == 0x04) {
                    uint32_t dummy_bytes_read;
                    int32_t block_type;
                    uint32_t temp_pc = scan_pc;
                     // We need a way to read from a buffer without affecting the main stream PC
                    read_sleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &temp_pc, &dummy_bytes_read);
                    scan_pc = temp_pc;
                }
                break;

            case 0x05: // else
                if (depth == 1) {
                    *found_else = 1;
                    return scan_pc; // Return PC of the 'else' instruction
                }
                break;

            case 0x0B: // end
                depth--;
                if (depth == 0) {
                    return scan_pc; // Return PC *after* the 'end' instruction
                }
                break;
            
            // Opcodes with immediates that we need to skip over during the scan
            case 0x0C: // br
            case 0x0D: // br_if
            case 0x0E: // br_table
            case 0x10: // call
            case 0x20: // local.get
            case 0x21: // local.set
            case 0x22: // local.tee
            case 0x23: // global.get
            case 0x24: // global.set
            case 0x41: // i32.const
            case 0x42: // i64.const
            {
                uint32_t dummy_val, bytes_read;
                uint32_t temp_pc = scan_pc;
                // This is a simplified skip; br_table has more complex immediates
                read_uleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &temp_pc, &bytes_read);
                scan_pc = temp_pc;
                if (opcode == 0x0E) { // br_table needs to skip the whole table
                    uint32_t target_count = dummy_val;
                    for (uint32_t i = 0; i <= target_count; ++i) { // targets + default
                         read_uleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &temp_pc, &bytes_read);
                         scan_pc = temp_pc;
                    }
                }
                break;
            }
            // Add other opcodes with immediates here if necessary...
        }
    }

    return (uint32_t)-1; // End of block not found
}

/**
 * @brief A simplified WASM simulation focusing on branching.
 */
int simulate_wasm_branching() {
    printf("=== WASM Branching Simulation Starting ===\n");

    // --- Create a dummy WASM module in memory ---
    // This WASM code is equivalent to:
    // func $main (param i32) (result i32)
    //   local.get 0
    //   i32.const 10
    //   i32.gt_s
    //   if (result i32)
    //     i32.const 100
    //   else
    //     i32.const 200
    //   end
    //   nop
    //   loop
    //     i32.const 1
    //     br 0
    //   end
    // end
    const uint8_t wasm_bytecode[] = {
        // Function body for a single function
        0x01, // 1 local entry
        0x01, 0x7f, // 1 local of type i32 (though we don't use it in this sim)
        
        // Main code
        0x20, 0x00,       // local.get 0
        0x41, 0x0A,       // i32.const 10
        0x48,             // i32.gt_s (signed greater than)
        0x04, 0x40,       // if block, type empty
            0x41, 0x64,   // i32.const 100
        0x05,             // else
            0x41, 0xC8, 0x01, // i32.const 200
        0x0B,             // end of if
        0x01,             // nop
        0x03, 0x40,       // loop block
            0x41, 0x01,   // i32.const 1
            0x0C, 0x00,   // br 0 (break to the loop start)
        0x0B,             // end of loop
        0x0B              // end of function
    };

    // --- Setup mock WasmModule and WasmInstructionStream ---
    WasmModule module = {0};
    WasmFunction function = {0};
    module.num_functions = 1;
    module.functions = &function;
    function.body_size = sizeof(wasm_bytecode);
    // In a real scenario, body_offset would be a file offset. Here it's 0.
    function.body_offset = 0; 

    WasmInstructionStream* stream = wasm_instruction_stream_init(&module);
    
    // Manually load the bytecode into the stream for this test
    stream->function_bytecode = (uint8_t*)malloc(sizeof(wasm_bytecode));
    memcpy(stream->function_bytecode, wasm_bytecode, sizeof(wasm_bytecode));
    stream->bytecode_size = sizeof(wasm_bytecode);
    stream->current_function_idx = 0;
    stream->is_loaded = 1;
    stream->pc = 0;

    printf("Successfully loaded function 0. Bytecode size: %u bytes.\n", stream->bytecode_size);
    printf("Simulating with a fake input value > 10 to test the 'if' branch.\n\n");

    // --- Main Execution Loop ---
    // A real VM would have an operand stack. We fake it.
    int32_t operand_stack[16];
    int sp = -1;
    // Let's pretend local.get 0 pushed a value of 15 onto the stack
    operand_stack[++sp] = 15; 
    
    uint8_t opcode_byte;
    uint32_t uleb_val, bytes_read_leb;
    int execution_steps = 0;
    int loop_counter = 0; // To prevent infinite loops in the test

    while (stream->pc < stream->bytecode_size && execution_steps < 100) {
        uint32_t current_pc = stream->pc;
        
        wasm_instruction_stream_read_byte(stream, &opcode_byte);
        execution_steps++;

        printf("[PC:%02u] Opcode: 0x%02X ", current_pc, opcode_byte);

        switch (opcode_byte) {
            case 0x0B: // end
                printf("end\n");
                if (stream->pc >= stream->bytecode_size) {
                    printf("End of function.\n");
                }
                break;
            
            case 0x01: printf("nop\n"); break;
            
            case 0x20: // local.get
                read_uleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, &bytes_read_leb);
                printf("local.get (faking push of 15)\n");
                // The value 15 was already pre-pushed for the test.
                break;

            case 0x41: // i32.const
                read_sleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, &bytes_read_leb);
                printf("i32.const\n");
                // In a real VM, you'd push the value. We do nothing.
                break;

            case 0x48: // i32.gt_s
                printf("i32.gt_s (15 > 10? -> true)\n");
                // Fake the comparison: pop two values, push result.
                // operand_stack[sp-1] > operand_stack[sp]
                // Our fake result is 'true' (1)
                operand_stack[sp-1] = 1;
                sp--;
                break;

            case 0x04: { // if
                printf("if\n");
                // Skip the block type immediate
                read_sleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, &bytes_read_leb);
                
                // Pop condition from stack
                int32_t condition = operand_stack[sp--];
                printf("  - Condition is %s\n", condition != 0 ? "true" : "false");

                if (condition == 0) { // If condition is false, jump to else or end
                    int found_else = 0;
                    uint32_t jump_target = wasm_instruction_stream_find_block_end(stream, &found_else);
                    
                    if (jump_target != (uint32_t)-1) {
                        printf("  - Condition is false, jumping to PC:%u\n", jump_target);
                        stream->pc = jump_target;
                    } else {
                        fprintf(stderr, "Error: Malformed if block, no matching end/else found!\n");
                        goto end_simulation;
                    }
                } else {
                    printf("  - Condition is true, executing 'then' block.\n");
                    // Just continue execution normally
                }
                break;
            }

            case 0x05: { // else
                printf("else\n");
                // This opcode is only reached if the 'if' condition was true.
                // It means we have finished the 'then' block and must now jump over the 'else' block.
                int dummy_found_else = 0;
                // The find_block_end function starting from an `else` will find the matching `end`.
                uint32_t jump_target = wasm_instruction_stream_find_block_end(stream, &dummy_found_else);

                if (jump_target != (uint32_t)-1) {
                    printf("  - Finished 'then' block, jumping past 'else' block to PC:%u\n", jump_target);
                    stream->pc = jump_target;
                } else {
                     fprintf(stderr, "Error: Malformed else block, no matching end found!\n");
                     goto end_simulation;
                }
                break;
            }

            case 0x03: // loop
                 printf("loop\n");
                 // Skip the block type immediate
                 read_sleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, &bytes_read_leb);
                 // In a simple loop, execution just enters. The `br` instruction handles the looping.
                 break;

            case 0x0C: { // br
                read_uleb128_from_memory(stream->function_bytecode, stream->bytecode_size, &stream->pc, &bytes_read_leb);
                uint32_t relative_depth = uleb_val;
                printf("br (depth: %u)\n", relative_depth);

                // This is a highly simplified branch. A real VM would unwind the stack.
                // For a `br` to a loop (depth 0), we effectively do nothing and let the loop continue.
                // For this test, we'll just stop it after a few iterations.
                if (++loop_counter > 2) {
                    printf("  - Breaking loop test after 3 iterations.\n");
                    int dummy;
                    // Find the end of the loop block and jump past it
                    stream->pc = wasm_instruction_stream_find_block_end(stream, &dummy);
                } else {
                    printf("  - Looping... (iteration %d)\n", loop_counter);
                    // To loop, we'd need to know the PC of the *start* of the loop.
                    // A real VM would manage a control stack for this.
                    // We'll just fake it by setting the PC manually.
                    stream->pc = 14; // Manually set to start of loop content
                }
                break;
            }

            // Skip over the locals definition
            case 0x01: {
                printf("local decl\n");
                // We are at the start of the function body. The first bytes define locals.
                // For this test, we'll just skip them manually as the parsing is complex.
                stream->pc = 3; // Start of actual code after locals
                break;
            }

            default:
                printf("UNKNOWN OPCODE\n");
                goto end_simulation;
        }
    }

end_simulation:
    printf("\n--- Execution Simulation Ended ---\n");

    // Cleanup
    free(stream->function_bytecode);
    wasm_instruction_stream_free(stream);

    printf("=== WASM Branching Simulation Complete ===\n");
    return 0;
}


int main() {
    // This file doesn't need fa_wasm.c to be compiled with it, as it simulates everything in-memory.
    // It does require fa_wasm_stream.c for the stream logic.
    // Compile with:
    // gcc -o wasm_branch_sim test_fa_wasm_stream_branch.c fa_wasm_stream.c -I. -Wall -g
    return simulate_wasm_branching();
}
