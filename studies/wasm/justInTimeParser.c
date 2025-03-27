#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

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

// Legge un valore LEB128 unsigned a 32 bit
uint32_t read_uleb128(int fd, uint32_t* size_read) {
    uint32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (read(fd, &byte, 1) != 1) {
            return 0;
        }
        (*size_read)++;

        result |= ((byte & 0x7f) << shift);
        shift += 7;
    } while (byte & 0x80 && shift < 32);

    return result;
}

// Legge un valore LEB128 unsigned a 64 bit
uint64_t read_uleb128_64(int fd, uint32_t* size_read) {
    uint64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (read(fd, &byte, 1) != 1) {
            return 0;
        }
        (*size_read)++;

        result |= ((uint64_t)(byte & 0x7f) << shift);
        shift += 7;
    } while (byte & 0x80 && shift < 64);

    return result;
}

// Legge un valore LEB128 signed a 32 bit
int32_t read_sleb128(int fd, uint32_t* size_read) {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (read(fd, &byte, 1) != 1) {
            return 0;
        }
        (*size_read)++;

        result |= ((byte & 0x7f) << shift);
        shift += 7;
    } while (byte & 0x80 && shift < 32);

    // Gestisce il segno
    if (shift < 32 && (byte & 0x40)) {
        result |= (~0 << shift);
    }

    return result;
}

// Legge un valore LEB128 signed a 64 bit
int64_t read_sleb128_64(int fd, uint32_t* size_read) {
    int64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (read(fd, &byte, 1) != 1) {
            return 0;
        }
        (*size_read)++;

        result |= ((int64_t)(byte & 0x7f) << shift);
        shift += 7;
    } while (byte & 0x80 && shift < 64);

    // Gestisce il segno
    if (shift < 64 && (byte & 0x40)) {
        result |= ((int64_t)(~0) << shift);
    }

    return result;
}

// Legge una stringa (lunghezza + dati)
char* read_string(int fd, uint32_t* len) {
    uint32_t size_read;
    *len = read_uleb128(fd, &size_read);
    
    if (*len == 0) {
        return NULL;
    }
    
    char* str = (char*)malloc(*len + 1);
    if (!str) {
        return NULL;
    }
    
    if (read(fd, str, *len) != *len) {
        free(str);
        return NULL;
    }
    
    str[*len] = '\0';
    return str;
}

// Inizializza il modulo WASM
WasmModule* wasm_module_init(const char* filename) {
    WasmModule* module = (WasmModule*)malloc(sizeof(WasmModule));
    if (!module) {
        return NULL;
    }

    memset(module, 0, sizeof(WasmModule));
    module->filename = strdup(filename);
    
    module->fd = open(filename, O_RDONLY);
    if (module->fd < 0) {
        free(module->filename);
        free(module);
        return NULL;
    }
    
    return module;
}

// Libera la memoria del modulo
void wasm_module_free(WasmModule* module) {
    if (!module) {
        return;
    }
    
    if (module->fd >= 0) {
        close(module->fd);
    }
    
    if (module->filename) {
        free(module->filename);
    }
    
    if (module->sections) {
        for (uint32_t i = 0; i < module->num_sections; i++) {
            if (module->sections[i].name) {
                free(module->sections[i].name);
            }
        }
        free(module->sections);
    }
    
    if (module->types) {
        for (uint32_t i = 0; i < module->num_types; i++) {
            if (module->types[i].param_types) {
                free(module->types[i].param_types);
            }
            if (module->types[i].result_types) {
                free(module->types[i].result_types);
            }
        }
        free(module->types);
    }
    
    if (module->functions) {
        free(module->functions);
    }
    
    if (module->exports) {
        for (uint32_t i = 0; i < module->num_exports; i++) {
            if (module->exports[i].name) {
                free(module->exports[i].name);
            }
        }
        free(module->exports);
    }
    
    if (module->memories) {
        free(module->memories);
    }
    
    free(module);
}

// Carica e verifica l'intestazione del file WASM
int wasm_load_header(WasmModule* module) {
    uint32_t magic, version;
    
    lseek(module->fd, 0, SEEK_SET);
    
    if (read(module->fd, &magic, 4) != 4) {
        return -1;
    }
    
    if (magic != 0x6d736100) {  // "\0asm" in little-endian
        return -1;
    }
    
    if (read(module->fd, &version, 4) != 4) {
        return -1;
    }
    
    if (version != 1) {
        return -1;
    }
    
    module->magic = magic;
    module->version = version;
    
    return 0;
}

// Elenca le sezioni nel file WASM senza caricare i loro contenuti
int wasm_scan_sections(WasmModule* module) {
    lseek(module->fd, 8, SEEK_SET);  // Salta l'intestazione
    
    // Conteggio preliminare delle sezioni
    off_t pos = 8;
    struct stat st;
    fstat(module->fd, &st);
    uint32_t count = 0;
    
    while (pos < st.st_size) {
        uint8_t section_id;
        uint32_t section_size;
        uint32_t size_read;
        
        if (read(module->fd, &section_id, 1) != 1) {
            break;
        }
        pos += 1;
        
        section_size = read_uleb128(module->fd, &size_read);
        pos += size_read;
        
        // Salta il contenuto della sezione
        lseek(module->fd, section_size, SEEK_CUR);
        pos += section_size;
        
        count++;
    }
    
    // Alloca memoria per le sezioni
    module->sections = (WasmSection*)malloc(count * sizeof(WasmSection));
    if (!module->sections) {
        return -1;
    }
    memset(module->sections, 0, count * sizeof(WasmSection));
    
    // Ora leggi i metadati delle sezioni
    lseek(module->fd, 8, SEEK_SET);
    pos = 8;
    
    for (uint32_t i = 0; i < count; i++) {
        uint8_t section_id;
        uint32_t section_size;
        uint32_t size_read;
        
        if (read(module->fd, &section_id, 1) != 1) {
            break;
        }
        pos += 1;
        
        section_size = read_uleb128(module->fd, &size_read);
        pos += size_read;
        
        module->sections[i].type = (WasmSectionType)section_id;
        module->sections[i].size = section_size;
        module->sections[i].offset = pos;
        
        // Se è una sezione custom, leggi il nome
        if (section_id == SECTION_CUSTOM) {
            module->sections[i].name = read_string(module->fd, &module->sections[i].name_len);
            pos += module->sections[i].name_len + size_read;  // +size_read per la lunghezza codificata
            lseek(module->fd, module->sections[i].offset + module->sections[i].size, SEEK_SET);
        } else {
            lseek(module->fd, module->sections[i].offset + module->sections[i].size, SEEK_SET);
        }
        
        pos += section_size;
    }
    
    module->num_sections = count;
    return 0;
}

// Carica i tipi di funzione dalla sezione Type
int wasm_load_types(WasmModule* module) {
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_TYPE) {
            lseek(module->fd, module->sections[i].offset, SEEK_SET);
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module->fd, &size_read);
            
            module->num_types = count;
            module->types = (WasmFunctionType*)malloc(count * sizeof(WasmFunctionType));
            if (!module->types) {
                return -1;
            }
            memset(module->types, 0, count * sizeof(WasmFunctionType));
            
            module->types_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                uint8_t form;
                if (read(module->fd, &form, 1) != 1 || form != 0x60) {
                    return -1;  // Attualmente, solo il form 0x60 (func) è supportato
                }
                
                // Leggi i tipi dei parametri
                module->types[j].num_params = read_uleb128(module->fd, &size_read);
                if (module->types[j].num_params > 0) {
                    module->types[j].param_types = (uint32_t*)malloc(module->types[j].num_params * sizeof(uint32_t));
                    if (!module->types[j].param_types) {
                        return -1;
                    }
                    
                    for (uint32_t k = 0; k < module->types[j].num_params; k++) {
                        uint8_t param_type;
                        if (read(module->fd, &param_type, 1) != 1) {
                            return -1;
                        }
                        module->types[j].param_types[k] = param_type;
                    }
                }
                
                // Leggi i tipi dei risultati
                module->types[j].num_results = read_uleb128(module->fd, &size_read);
                if (module->types[j].num_results > 0) {
                    module->types[j].result_types = (uint32_t*)malloc(module->types[j].num_results * sizeof(uint32_t));
                    if (!module->types[j].result_types) {
                        return -1;
                    }
                    
                    for (uint32_t k = 0; k < module->types[j].num_results; k++) {
                        uint8_t result_type;
                        if (read(module->fd, &result_type, 1) != 1) {
                            return -1;
                        }
                        module->types[j].result_types[k] = result_type;
                    }
                }
            }
            
            return 0;
        }
    }
    
    return -1;  // Sezione Type non trovata
}

// Carica gli indici dei tipi delle funzioni dalla sezione Function
int wasm_load_functions(WasmModule* module) {
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_FUNCTION) {
            lseek(module->fd, module->sections[i].offset, SEEK_SET);
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module->fd, &size_read);
            
            module->num_functions = count;
            module->functions = (WasmFunction*)malloc(count * sizeof(WasmFunction));
            if (!module->functions) {
                return -1;
            }
            memset(module->functions, 0, count * sizeof(WasmFunction));
            
            module->functions_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                module->functions[j].type_index = read_uleb128(module->fd, &size_read);
            }
            
            // Ora carica gli offset dei corpi delle funzioni dalla sezione Code
            for (uint32_t j = 0; j < module->num_sections; j++) {
                if (module->sections[j].type == SECTION_CODE) {
                    lseek(module->fd, module->sections[j].offset, SEEK_SET);
                    
                    uint32_t code_count = read_uleb128(module->fd, &size_read);
                    off_t current_offset = module->sections[j].offset + size_read;
                    
                    if (code_count != count) {
                        return -1;  // Il numero di corpi di funzioni deve corrispondere al numero di funzioni
                    }
                    
                    for (uint32_t k = 0; k < code_count; k++) {
                        uint32_t body_size = read_uleb128(module->fd, &size_read);
                        current_offset += size_read;
                        
                        module->functions[k].body_offset = current_offset;
                        module->functions[k].body_size = body_size;
                        
                        // Salta il corpo della funzione
                        lseek(module->fd, body_size, SEEK_CUR);
                        current_offset += body_size;
                    }
                    
                    break;
                }
            }
            
            return 0;
        }
    }
    
    return -1;  // Sezione Function non trovata
}

// Carica gli export dalla sezione Export
int wasm_load_exports(WasmModule* module) {
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_EXPORT) {
            lseek(module->fd, module->sections[i].offset, SEEK_SET);
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module->fd, &size_read);
            
            module->num_exports = count;
            module->exports = (WasmExport*)malloc(count * sizeof(WasmExport));
            if (!module->exports) {
                return -1;
            }
            memset(module->exports, 0, count * sizeof(WasmExport));
            
            module->exports_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                module->exports[j].name = read_string(module->fd, &module->exports[j].name_len);
                
                if (read(module->fd, &module->exports[j].kind, 1) != 1) {
                    return -1;
                }
                
                module->exports[j].index = read_uleb128(module->fd, &size_read);
            }
            
            return 0;
        }
    }
    
    return -1;  // Sezione Export non trovata
}

// Carica le memory dalla sezione Memory
int wasm_load_memories(WasmModule* module) {
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_MEMORY) {
            lseek(module->fd, module->sections[i].offset, SEEK_SET);
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module->fd, &size_read);
            
            module->num_memories = count;
            module->memories = (WasmMemory*)malloc(count * sizeof(WasmMemory));
            if (!module->memories) {
                return -1;
            }
            memset(module->memories, 0, count * sizeof(WasmMemory));
            
            module->memories_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                // Leggi i limiti
                uint8_t flags;
                if (read(module->fd, &flags, 1) != 1) {
                    return -1;
                }
                
                // Verifica se è memory64 (bit 0x4)
                module->memories[j].is_memory64 = (flags & 0x4) != 0;
                
                // Verifica se ha un valore massimo (bit 0x1)
                module->memories[j].has_max = (flags & 0x1) != 0;
                
                // Leggi la dimensione iniziale
                if (module->memories[j].is_memory64) {
                    module->memories[j].initial_size = read_uleb128_64(module->fd, &size_read);
                } else {
                    module->memories[j].initial_size = read_uleb128(module->fd, &size_read);
                }
                
                // Leggi la dimensione massima se presente
                if (module->memories[j].has_max) {
                    if (module->memories[j].is_memory64) {
                        module->memories[j].maximum_size = read_uleb128_64(module->fd, &size_read);
                    } else {
                        module->memories[j].maximum_size = read_uleb128(module->fd, &size_read);
                    }
                }
            }
            
            return 0;
        }
    }
    
    // È possibile che non ci sia una sezione memory
    module->num_memories = 0;
    return 0;
}

// Carica un byte code di una funzione on-demand
uint8_t* wasm_load_function_body(WasmModule* module, uint32_t func_idx) {
    if (func_idx >= module->num_functions) {
        return NULL;
    }
    
    WasmFunction* func = &module->functions[func_idx];
    uint8_t* body = (uint8_t*)malloc(func->body_size);
    if (!body) {
        return NULL;
    }
    
    lseek(module->fd, func->body_offset, SEEK_SET);
    if (read(module->fd, body, func->body_size) != func->body_size) {
        free(body);
        return NULL;
    }
    
    return body;
}

// Funzione per visualizzare informazioni sul modulo
void wasm_print_info(WasmModule* module) {
    printf("=== WASM Module Info ===\n");
    printf("Magic: 0x%08X\n", module->magic);
    printf("Version: %u\n", module->version);
    printf("Number of sections: %u\n", module->num_sections);
    
    printf("\n=== Sections ===\n");
    for (uint32_t i = 0; i < module->num_sections; i++) {
        printf("Section %u: Type=%u, Size=%u, Offset=0x%lx", 
               i, module->sections[i].type, module->sections[i].size, module->sections[i].offset);
        
        if (module->sections[i].type == SECTION_CUSTOM && module->sections[i].name) {
            printf(", Name=\"%s\"", module->sections[i].name);
        }
        printf("\n");
    }
    
    if (module->num_types > 0) {
        printf("\n=== Types (%u) ===\n", module->num_types);
        for (uint32_t i = 0; i < module->num_types; i++) {
            printf("Type %u: ", i);
            printf("Params(%u): [", module->types[i].num_params);
            for (uint32_t j = 0; j < module->types[i].num_params; j++) {
                printf("%u%s", module->types[i].param_types[j], j < module->types[i].num_params - 1 ? ", " : "");
            }
            printf("], Results(%u): [", module->types[i].num_results);
            for (uint32_t j = 0; j < module->types[i].num_results; j++) {
                printf("%u%s", module->types[i].result_types[j], j < module->types[i].num_results - 1 ? ", " : "");
            }
            printf("]\n");
        }
    }
    
    if (module->num_memories > 0) {
        printf("\n=== Memories (%u) ===\n", module->num_memories);
        for (uint32_t i = 0; i < module->num_memories; i++) {
            printf("Memory %u: Type=%s, Initial=%lu pages", 
                   i, 
                   module->memories[i].is_memory64 ? "Memory64" : "Memory32",
                   module->memories[i].initial_size);
            
            if (module->memories[i].has_max) {
                printf(", Maximum=%lu pages", module->memories[i].maximum_size);
            } else {
                printf(", No maximum");
            }
            printf("\n");
        }
    }
    
    if (module->num_functions > 0) {
        printf("\n=== Functions (%u) ===\n", module->num_functions);
        for (uint32_t i = 0; i < module->num_functions; i++) {
            printf("Function %u: Type=%u, Body Offset=0x%lx, Body Size=%u\n", 
                   i, module->functions[i].type_index, module->functions[i].body_offset, module->functions[i].body_size);
        }
    }
    
    if (module->num_exports > 0) {
        printf("\n=== Exports (%u) ===\n", module->num_exports);
        for (uint32_t i = 0; i < module->num_exports; i++) {
            const char* kind_str = "Unknown";
            switch (module->exports[i].kind) {
                case 0: kind_str = "Function"; break;
                case 1: kind_str = "Table"; break;
                case 2: kind_str = "Memory"; break;
                case 3: kind_str = "Global"; break;
            }
            printf("Export %u: Name=\"%s\", Kind=%s, Index=%u\n", 
                   i, module->exports[i].name, kind_str, module->exports[i].index);
        }
    }
}

// Funzione di esempio per utilizzare il parser
int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Uso: %s <file.wasm>\n", argv[0]);
        return 1;
    }
    
    WasmModule* module = wasm_module_init(argv[1]);
    if (!module) {
        printf("Errore nell'inizializzazione del modulo WASM\n");
        return 1;
    }
    
    if (wasm_load_header(module) < 0) {
        printf("Errore nella lettura dell'intestazione WASM\n");
        wasm_module_free(module);
        return 1;
    }
    
    if (wasm_scan_sections(module) < 0) {
        printf("Errore nella scansione delle sezioni WASM\n");
        wasm_module_free(module);
        return 1;
    }
    
    // Carica le sezioni principali
    wasm_load_types(module);
    wasm_load_functions(module);
    wasm_load_exports(module);
    wasm_load_memories(module);
    
    // Stampa le informazioni sul modulo
    wasm_print_info(module);
    
    // Test di caricamento del corpo di una funzione
    if (module->num_functions > 0) {
        printf("\n=== Testing function body loading ===\n");
        uint32_t func_idx = 0;
        uint8_t* body = wasm_load_function_body(module, func_idx);
        
        if (body) {
            printf("First 16 bytes of function %u body: ", func_idx);
            for (uint32_t i = 0; i < (module->functions[func_idx].body_size < 16 ? module->functions[func_idx].body_size : 16); i++) {
                printf("%02X ", body[i]);
            }
            printf("\n");
            
            free(body);
        } else {
            printf("Failed to load function %u body\n", func_idx);
        }
    }
    
    wasm_module_free(module);
    return 0;
}