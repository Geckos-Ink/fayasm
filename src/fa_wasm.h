#pragma once


// Macro per convertire byte LEB128 a intero
#define MAX_LEB128_SIZE 5

// Tipi di valore WebAssembly
typedef enum {
    VALTYPE_I32 = 0x7F,
    VALTYPE_I64 = 0x7E,
    VALTYPE_F32 = 0x7D,
    VALTYPE_F64 = 0x7C,
    VALTYPE_V128 = 0x7B,
    VALTYPE_FUNCREF = 0x70,
    VALTYPE_EXTERNREF = 0x6F
} WasmValType;

// Tipi di sezione WASM
typedef enum {
    SECTION_CUSTOM = 0,
    SECTION_TYPE = 1,
    SECTION_IMPORT = 2,
    SECTION_FUNCTION = 3,
    SECTION_TABLE = 4,
    SECTION_MEMORY = 5,
    SECTION_GLOBAL = 6,
    SECTION_EXPORT = 7,
    SECTION_START = 8,
    SECTION_ELEMENT = 9,
    SECTION_CODE = 10,
    SECTION_DATA = 11
} WasmSectionType;

typedef struct {
    WasmSectionType type;
    uint32_t size;
    off_t offset;
    char* name;            // Solo per sezioni custom
    uint32_t name_len;     // Solo per sezioni custom
} WasmSection;

// Sezione di memoria
typedef struct {
    bool is_memory64;      // true se utilizza indirizzamento a 64 bit
    uint64_t initial_size; // Dimensione iniziale (in pagine)
    uint64_t maximum_size; // Dimensione massima (in pagine, opzionale)
    bool has_max;          // Indica se è specificata una dimensione massima
} WasmMemory;

typedef struct {
    uint32_t num_params;
    uint32_t num_results;
    uint32_t* param_types;
    uint32_t* result_types;
} WasmFunctionType;

typedef struct {
    uint32_t type_index;
    off_t body_offset;
    uint32_t body_size;
} WasmFunction;

typedef struct {
    char* name;
    uint32_t name_len;
    uint32_t kind;         // 0=function, 1=table, 2=memory, 3=global
    uint32_t index;
} WasmExport;

typedef struct {
    uint32_t magic;        // 0x6d736100
    uint32_t version;      // 1

    // Indici delle sezioni
    uint32_t num_sections;
    WasmSection* sections;

    // Tipi di funzioni
    uint32_t num_types;
    WasmFunctionType* types;
    off_t types_offset;

    // Funzioni
    uint32_t num_functions;
    WasmFunction* functions;
    off_t functions_offset;

    // Export
    uint32_t num_exports;
    WasmExport* exports;
    off_t exports_offset;

    // Memoria
    uint32_t num_memories;
    WasmMemory* memories;
    off_t memories_offset;
    
    // File handle
    int fd;
    char* filename;
} WasmModule;

WasmModule* wasm_module_init(const char* filename);
void wasm_module_free(WasmModule* module);
int wasm_load_header(WasmModule* module);
int wasm_scan_sections(WasmModule* module);
int wasm_load_types(WasmModule* module);
int wasm_load_functions(WasmModule* module);
int wasm_load_exports(WasmModule* module);
int wasm_load_memories(WasmModule* module);
uint8_t* wasm_load_function_body(WasmModule* module, uint32_t func_idx);
void wasm_print_info(WasmModule* module);