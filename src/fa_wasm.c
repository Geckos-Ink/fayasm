#include "fa_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static ssize_t wasm_stream_read(WasmModule* module, void* out, size_t size) {
    if (!module || !out || size == 0) {
        return 0;
    }
    if (module->fd >= 0) {
        const ssize_t read_bytes = read(module->fd, out, size);
        if (read_bytes > 0) {
            module->cursor += read_bytes;
        }
        return read_bytes;
    }
    if (!module->buffer || module->buffer_size == 0) {
        return -1;
    }
    if (module->cursor < 0 || (size_t)module->cursor >= module->buffer_size) {
        return 0;
    }
    size_t remaining = module->buffer_size - (size_t)module->cursor;
    if (size > remaining) {
        size = remaining;
    }
    memcpy(out, module->buffer + module->cursor, size);
    module->cursor += (off_t)size;
    return (ssize_t)size;
}

static bool wasm_is_supported_valtype(uint8_t valtype) {
    switch (valtype) {
        case VALTYPE_I32:
        case VALTYPE_I64:
        case VALTYPE_F32:
        case VALTYPE_F64:
        case VALTYPE_FUNCREF:
        case VALTYPE_EXTERNREF:
            return true;
        default:
            return false;
    }
}

static off_t wasm_stream_seek(WasmModule* module, off_t offset, int whence) {
    if (!module) {
        return -1;
    }
    if (module->fd >= 0) {
        const off_t pos = lseek(module->fd, offset, whence);
        if (pos >= 0) {
            module->cursor = pos;
        }
        return pos;
    }
    off_t base = 0;
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = module->cursor;
            break;
        case SEEK_END:
            base = (off_t)module->buffer_size;
            break;
        default:
            return -1;
    }
    off_t pos = base + offset;
    if (pos < 0 || (size_t)pos > module->buffer_size) {
        return -1;
    }
    module->cursor = pos;
    return pos;
}

static off_t wasm_stream_size(const WasmModule* module) {
    if (!module) {
        return 0;
    }
    if (module->fd >= 0) {
        return module->stream_size;
    }
    return (off_t)module->buffer_size;
}

// Legge un valore LEB128 unsigned a 32 bit
uint32_t read_uleb128(WasmModule* module, uint32_t* size_read) {
    uint32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (wasm_stream_read(module, &byte, 1) != 1) {
            return 0;
        }
        (*size_read)++;

        result |= ((byte & 0x7f) << shift);
        shift += 7;
    } while (byte & 0x80 && shift < 32);

    return result;
}

// Legge un valore LEB128 unsigned a 64 bit
uint64_t read_uleb128_64(WasmModule* module, uint32_t* size_read) {
    uint64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (wasm_stream_read(module, &byte, 1) != 1) {
            return 0;
        }
        (*size_read)++;

        result |= ((uint64_t)(byte & 0x7f) << shift);
        shift += 7;
    } while (byte & 0x80 && shift < 64);

    return result;
}

// Legge un valore LEB128 signed a 32 bit
int32_t read_sleb128(WasmModule* module, uint32_t* size_read) {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (wasm_stream_read(module, &byte, 1) != 1) {
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
int64_t read_sleb128_64(WasmModule* module, uint32_t* size_read) {
    int64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    *size_read = 0;

    do {
        if (wasm_stream_read(module, &byte, 1) != 1) {
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
char* read_string(WasmModule* module, uint32_t* len) {
    uint32_t size_read;
    *len = read_uleb128(module, &size_read);
    
    if (*len == 0) {
        return NULL;
    }
    
    char* str = (char*)malloc(*len + 1);
    if (!str) {
        return NULL;
    }
    
    if (wasm_stream_read(module, str, *len) != (ssize_t)*len) {
        free(str);
        return NULL;
    }
    
    str[*len] = '\0';
    return str;
}

///
///
///

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

    struct stat st;
    if (fstat(module->fd, &st) != 0) {
        close(module->fd);
        free(module->filename);
        free(module);
        return NULL;
    }
    module->cursor = 0;
    module->stream_size = st.st_size;
    
    return module;
}

WasmModule* wasm_module_init_from_memory(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return NULL;
    }
    WasmModule* module = (WasmModule*)malloc(sizeof(WasmModule));
    if (!module) {
        return NULL;
    }
    memset(module, 0, sizeof(WasmModule));

    uint8_t* copy = (uint8_t*)malloc(size);
    if (!copy) {
        free(module);
        return NULL;
    }
    memcpy(copy, data, size);
    module->buffer = copy;
    module->buffer_size = size;
    module->buffer_owned = true;
    module->cursor = 0;
    module->stream_size = (off_t)size;
    module->fd = -1;
    module->filename = strdup("<memory>");
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

    if (module->buffer_owned && module->buffer) {
        free((void*)module->buffer);
        module->buffer = NULL;
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

    if (module->globals) {
        free(module->globals);
    }
    
    free(module);
}

// Carica e verifica l'intestazione del file WASM
int wasm_load_header(WasmModule* module) {
    uint32_t magic, version;
    
    if (wasm_stream_seek(module, 0, SEEK_SET) < 0) {
        return -1;
    }
    
    if (wasm_stream_read(module, &magic, 4) != 4) {
        return -1;
    }
    
    if (magic != 0x6d736100) {  // "\0asm" in little-endian
        return -1;
    }
    
    if (wasm_stream_read(module, &version, 4) != 4) {
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
    if (wasm_stream_seek(module, 8, SEEK_SET) < 0) {  // Salta l'intestazione
        return -1;
    }
    
    // Conteggio preliminare delle sezioni
    off_t pos = 8;
    const off_t stream_size = wasm_stream_size(module);
    uint32_t count = 0;
    
    while (pos < stream_size) {
        uint8_t section_id;
        uint32_t section_size;
        uint32_t size_read;
        
        if (wasm_stream_read(module, &section_id, 1) != 1) {
            break;
        }
        pos += 1;
        
        section_size = read_uleb128(module, &size_read);
        pos += size_read;
        
        // Salta il contenuto della sezione
        if (wasm_stream_seek(module, section_size, SEEK_CUR) < 0) {
            break;
        }
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
    if (wasm_stream_seek(module, 8, SEEK_SET) < 0) {
        return -1;
    }
    pos = 8;
    
    for (uint32_t i = 0; i < count; i++) {
        uint8_t section_id;
        uint32_t section_size;
        uint32_t size_read;
        
        if (wasm_stream_read(module, &section_id, 1) != 1) {
            break;
        }
        pos += 1;
        
        section_size = read_uleb128(module, &size_read);
        pos += size_read;
        
        module->sections[i].type = (WasmSectionType)section_id;
        module->sections[i].size = section_size;
        module->sections[i].offset = pos;
        
        // Se è una sezione custom, leggi il nome
        if (section_id == SECTION_CUSTOM) {
            module->sections[i].name = read_string(module, &module->sections[i].name_len);
        }
        if (wasm_stream_seek(module, module->sections[i].offset + module->sections[i].size, SEEK_SET) < 0) {
            break;
        }
        pos = module->sections[i].offset + module->sections[i].size;
    }
    
    module->num_sections = count;
    return 0;
}

// Carica i tipi di funzione dalla sezione Type
int wasm_load_types(WasmModule* module) {
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_TYPE) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                return -1;
            }
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module, &size_read);
            
            module->num_types = count;
            module->types = (WasmFunctionType*)malloc(count * sizeof(WasmFunctionType));
            if (!module->types) {
                return -1;
            }
            memset(module->types, 0, count * sizeof(WasmFunctionType));
            
            module->types_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                uint8_t form;
                if (wasm_stream_read(module, &form, 1) != 1 || form != 0x60) {
                    return -1;  // Attualmente, solo il form 0x60 (func) è supportato
                }
                
                // Leggi i tipi dei parametri
                module->types[j].num_params = read_uleb128(module, &size_read);
                if (module->types[j].num_params > 0) {
                    module->types[j].param_types = (uint32_t*)malloc(module->types[j].num_params * sizeof(uint32_t));
                    if (!module->types[j].param_types) {
                        return -1;
                    }
                    
                    for (uint32_t k = 0; k < module->types[j].num_params; k++) {
                        uint8_t param_type;
                        if (wasm_stream_read(module, &param_type, 1) != 1) {
                            return -1;
                        }
                        module->types[j].param_types[k] = param_type;
                    }
                }
                
                // Leggi i tipi dei risultati
                module->types[j].num_results = read_uleb128(module, &size_read);
                if (module->types[j].num_results > 0) {
                    module->types[j].result_types = (uint32_t*)malloc(module->types[j].num_results * sizeof(uint32_t));
                    if (!module->types[j].result_types) {
                        return -1;
                    }
                    
                    for (uint32_t k = 0; k < module->types[j].num_results; k++) {
                        uint8_t result_type;
                        if (wasm_stream_read(module, &result_type, 1) != 1) {
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
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                return -1;
            }
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module, &size_read);
            
            module->num_functions = count;
            module->functions = (WasmFunction*)malloc(count * sizeof(WasmFunction));
            if (!module->functions) {
                return -1;
            }
            memset(module->functions, 0, count * sizeof(WasmFunction));
            
            module->functions_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                module->functions[j].type_index = read_uleb128(module, &size_read);
            }
            
            // Ora carica gli offset dei corpi delle funzioni dalla sezione Code
            for (uint32_t j = 0; j < module->num_sections; j++) {
                if (module->sections[j].type == SECTION_CODE) {
                    if (wasm_stream_seek(module, module->sections[j].offset, SEEK_SET) < 0) {
                        return -1;
                    }
                    
                    uint32_t code_count = read_uleb128(module, &size_read);
                    off_t current_offset = module->sections[j].offset + size_read;
                    
                    if (code_count != count) {
                        return -1;  // Il numero di corpi di funzioni deve corrispondere al numero di funzioni
                    }
                    
                    for (uint32_t k = 0; k < code_count; k++) {
                        uint32_t body_size = read_uleb128(module, &size_read);
                        current_offset += size_read;
                        
                        module->functions[k].body_offset = current_offset;
                        module->functions[k].body_size = body_size;
                        
                        // Salta il corpo della funzione
                        if (wasm_stream_seek(module, body_size, SEEK_CUR) < 0) {
                            return -1;
                        }
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
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                return -1;
            }
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module, &size_read);
            
            module->num_exports = count;
            module->exports = (WasmExport*)malloc(count * sizeof(WasmExport));
            if (!module->exports) {
                return -1;
            }
            memset(module->exports, 0, count * sizeof(WasmExport));
            
            module->exports_offset = module->sections[i].offset + size_read;
            
            for (uint32_t j = 0; j < count; j++) {
                module->exports[j].name = read_string(module, &module->exports[j].name_len);
                
                if (wasm_stream_read(module, &module->exports[j].kind, 1) != 1) {
                    return -1;
                }
                
                module->exports[j].index = read_uleb128(module, &size_read);
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
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                return -1;
            }
            
            uint32_t size_read;
            uint32_t count = read_uleb128(module, &size_read);
            
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
                if (wasm_stream_read(module, &flags, 1) != 1) {
                    return -1;
                }
                
                // Verifica se è memory64 (bit 0x4)
                module->memories[j].is_memory64 = (flags & 0x4) != 0;
                
                // Verifica se ha un valore massimo (bit 0x1)
                module->memories[j].has_max = (flags & 0x1) != 0;
                
                // Leggi la dimensione iniziale
                if (module->memories[j].is_memory64) {
                    module->memories[j].initial_size = read_uleb128_64(module, &size_read);
                } else {
                    module->memories[j].initial_size = read_uleb128(module, &size_read);
                }
                
                // Leggi la dimensione massima se presente
                if (module->memories[j].has_max) {
                    if (module->memories[j].is_memory64) {
                        module->memories[j].maximum_size = read_uleb128_64(module, &size_read);
                    } else {
                        module->memories[j].maximum_size = read_uleb128(module, &size_read);
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

// Carica i globali dalla sezione Global
int wasm_load_globals(WasmModule* module) {
    WasmGlobal* imported_globals = NULL;
    uint32_t imported_count = 0;
    uint32_t imported_capacity = 0;

    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type != SECTION_IMPORT) {
            continue;
        }
        if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
            return -1;
        }

        uint32_t size_read;
        uint32_t count = read_uleb128(module, &size_read);
        imported_capacity = count;
        if (imported_capacity > 0) {
            imported_globals = (WasmGlobal*)calloc(imported_capacity, sizeof(WasmGlobal));
            if (!imported_globals) {
                return -1;
            }
        }

        for (uint32_t j = 0; j < count; j++) {
            uint32_t name_len = 0;
            char* module_name = read_string(module, &name_len);
            char* import_name = read_string(module, &name_len);
            if (module_name) {
                free(module_name);
            }
            if (import_name) {
                free(import_name);
            }

            uint8_t kind = 0;
            if (wasm_stream_read(module, &kind, 1) != 1) {
                free(imported_globals);
                return -1;
            }
            switch (kind) {
                case 0: /* func */
                    (void)read_uleb128(module, &size_read);
                    break;
                case 1: /* table */
                {
                    uint8_t elem_type = 0;
                    if (wasm_stream_read(module, &elem_type, 1) != 1) {
                        free(imported_globals);
                        return -1;
                    }
                    uint32_t flags = read_uleb128(module, &size_read);
                    (void)read_uleb128(module, &size_read);
                    if (flags & 0x1) {
                        (void)read_uleb128(module, &size_read);
                    }
                    break;
                }
                case 2: /* memory */
                {
                    uint32_t flags = read_uleb128(module, &size_read);
                    const bool memory64 = (flags & 0x4) != 0;
                    if (memory64) {
                        (void)read_uleb128_64(module, &size_read);
                        if (flags & 0x1) {
                            (void)read_uleb128_64(module, &size_read);
                        }
                    } else {
                        (void)read_uleb128(module, &size_read);
                        if (flags & 0x1) {
                            (void)read_uleb128(module, &size_read);
                        }
                    }
                    break;
                }
                case 3: /* global */
                {
                    uint8_t valtype = 0;
                    uint8_t mutability = 0;
                    if (wasm_stream_read(module, &valtype, 1) != 1 ||
                        wasm_stream_read(module, &mutability, 1) != 1) {
                        free(imported_globals);
                        return -1;
                    }
                    if (!wasm_is_supported_valtype(valtype) || mutability > 1) {
                        free(imported_globals);
                        return -1;
                    }
                    WasmGlobal* global = &imported_globals[imported_count++];
                    global->valtype = valtype;
                    global->is_mutable = (mutability == 1);
                    global->is_imported = true;
                    global->init_kind = WASM_GLOBAL_INIT_NONE;
                    global->init_index = 0;
                    global->init_raw = 0;
                    break;
                }
                default:
                    free(imported_globals);
                    return -1;
            }
        }
        break;
    }

    WasmGlobal* globals = NULL;
    uint32_t defined_count = 0;
    uint32_t globals_offset = 0;
    uint32_t global_section_index = UINT32_MAX;

    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_GLOBAL) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                free(imported_globals);
                return -1;
            }

            uint32_t size_read;
            defined_count = read_uleb128(module, &size_read);
            globals_offset = module->sections[i].offset + size_read;
            global_section_index = i;
            break;
        }
    }

    const uint32_t total_globals = imported_count + defined_count;
    if (total_globals > 0) {
        globals = (WasmGlobal*)calloc(total_globals, sizeof(WasmGlobal));
        if (!globals) {
            free(imported_globals);
            return -1;
        }
    }
    for (uint32_t i = 0; i < imported_count; ++i) {
        globals[i] = imported_globals[i];
    }
    free(imported_globals);

    module->globals = globals;
    module->num_globals = total_globals;
    module->globals_offset = globals_offset;

    if (defined_count == 0) {
        return 0;
    }
    if (global_section_index == UINT32_MAX) {
        return -1;
    }
    if (wasm_stream_seek(module, module->sections[global_section_index].offset, SEEK_SET) < 0) {
        return -1;
    }
    {
        uint32_t size_read;
        (void)read_uleb128(module, &size_read);
    }

    for (uint32_t j = 0; j < defined_count; j++) {
        uint32_t size_read = 0;
        uint8_t valtype = 0;
        if (wasm_stream_read(module, &valtype, 1) != 1) {
            return -1;
        }
        uint8_t mutability = 0;
        if (wasm_stream_read(module, &mutability, 1) != 1) {
            return -1;
        }
        if (!wasm_is_supported_valtype(valtype) || mutability > 1) {
            return -1;
        }
        WasmGlobal* global = &module->globals[imported_count + j];
        global->valtype = valtype;
        global->is_mutable = (mutability == 1);
        global->is_imported = false;

        uint8_t init_opcode = 0;
        if (wasm_stream_read(module, &init_opcode, 1) != 1) {
            return -1;
        }
        switch (init_opcode) {
            case 0x41: /* i32.const */
            {
                int32_t value = read_sleb128(module, &size_read);
                global->init_raw = (uint64_t)(int64_t)value;
                global->init_kind = WASM_GLOBAL_INIT_CONST;
                break;
            }
            case 0x42: /* i64.const */
            {
                int64_t value = read_sleb128_64(module, &size_read);
                global->init_raw = (uint64_t)value;
                global->init_kind = WASM_GLOBAL_INIT_CONST;
                break;
            }
            case 0x43: /* f32.const */
            {
                uint32_t raw = 0;
                if (wasm_stream_read(module, &raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
                    return -1;
                }
                global->init_raw = raw;
                global->init_kind = WASM_GLOBAL_INIT_CONST;
                break;
            }
            case 0x44: /* f64.const */
            {
                uint64_t raw = 0;
                if (wasm_stream_read(module, &raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
                    return -1;
                }
                global->init_raw = raw;
                global->init_kind = WASM_GLOBAL_INIT_CONST;
                break;
            }
            case 0x23: /* global.get */
            {
                uint32_t index = read_uleb128(module, &size_read);
                global->init_index = index;
                global->init_kind = WASM_GLOBAL_INIT_GET;
                break;
            }
            default:
                return -1;
        }

        uint8_t end = 0;
        if (wasm_stream_read(module, &end, 1) != 1 || end != 0x0B) {
            return -1;
        }
    }

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
    
    if (wasm_stream_seek(module, func->body_offset, SEEK_SET) < 0) {
        free(body);
        return NULL;
    }
    if (wasm_stream_read(module, body, func->body_size) != (ssize_t)func->body_size) {
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
        printf("Section %u: Type=%u, Size=%u, Offset=0x%" PRIxMAX, 
               i, module->sections[i].type, module->sections[i].size, (uintmax_t)module->sections[i].offset);
        
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
            printf("Memory %u: Type=%s, Initial=%" PRIu64 " pages", 
                   i, 
                   module->memories[i].is_memory64 ? "Memory64" : "Memory32",
                   module->memories[i].initial_size);
            
            if (module->memories[i].has_max) {
                printf(", Maximum=%" PRIu64 " pages", module->memories[i].maximum_size);
            } else {
                printf(", No maximum");
            }
            printf("\n");
        }
    }

    if (module->num_globals > 0) {
        printf("\n=== Globals (%u) ===\n", module->num_globals);
        for (uint32_t i = 0; i < module->num_globals; i++) {
            const char* kind = module->globals[i].is_imported ? "import" :
                (module->globals[i].init_kind == WASM_GLOBAL_INIT_GET ? "get" : "const");
            if (module->globals[i].is_imported) {
                printf("Global %u: Type=0x%02X, Mutable=%s, Kind=%s\n",
                       i,
                       module->globals[i].valtype,
                       module->globals[i].is_mutable ? "true" : "false",
                       kind);
            } else if (module->globals[i].init_kind == WASM_GLOBAL_INIT_GET) {
                printf("Global %u: Type=0x%02X, Mutable=%s, Kind=%s, Index=%u\n",
                       i,
                       module->globals[i].valtype,
                       module->globals[i].is_mutable ? "true" : "false",
                       kind,
                       module->globals[i].init_index);
            } else {
                printf("Global %u: Type=0x%02X, Mutable=%s, Kind=%s, Init=0x%016" PRIx64 "\n",
                       i,
                       module->globals[i].valtype,
                       module->globals[i].is_mutable ? "true" : "false",
                       kind,
                       module->globals[i].init_raw);
            }
        }
    }
    
    if (module->num_functions > 0) {
        printf("\n=== Functions (%u) ===\n", module->num_functions);
        for (uint32_t i = 0; i < module->num_functions; i++) {
            printf("Function %u: Type=%u, Body Offset=0x%" PRIxMAX ", Body Size=%u\n", 
                   i, module->functions[i].type_index, (uintmax_t)module->functions[i].body_offset, module->functions[i].body_size);
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
int fa_wasm_example(int argc, char** argv) {
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
    wasm_load_globals(module);
    
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
