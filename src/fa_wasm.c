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
        case VALTYPE_V128:
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

    if (shift < 64 && (byte & 0x40)) {
        result |= (~0LL << shift);
    }

    return result;
}

static int wasm_read_init_expr_offset(WasmModule* module, uint64_t* out) {
    if (!module || !out) {
        return -1;
    }
    uint8_t opcode = 0;
    if (wasm_stream_read(module, &opcode, 1) != 1) {
        return -1;
    }
    uint32_t size_read = 0;
    int64_t value = 0;
    switch (opcode) {
        case 0x41:
            value = read_sleb128(module, &size_read);
            break;
        case 0x42:
            value = read_sleb128_64(module, &size_read);
            break;
        default:
            return -1;
    }
    if (size_read == 0 || value < 0) {
        return -1;
    }
    uint8_t end = 0;
    if (wasm_stream_read(module, &end, 1) != 1 || end != 0x0B) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static bool wasm_is_ref_type(uint8_t ref_type) {
    return ref_type == VALTYPE_FUNCREF || ref_type == VALTYPE_EXTERNREF;
}

static int wasm_read_element_expr_ref(WasmModule* module, uint8_t elem_type, WasmElementInit* out) {
    if (!module || !out || !wasm_is_ref_type(elem_type)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->kind = WASM_ELEMENT_INIT_REF_VALUE;

    uint8_t opcode = 0;
    if (wasm_stream_read(module, &opcode, 1) != 1) {
        return -1;
    }

    uint32_t size_read = 0;
    switch (opcode) {
        case 0xD0: /* ref.null */
        {
            uint8_t null_type = 0;
            if (wasm_stream_read(module, &null_type, 1) != 1) {
                return -1;
            }
            if (null_type != elem_type) {
                return -1;
            }
            out->value = (fa_ptr)0;
            break;
        }
        case 0xD2: /* ref.func */
        {
            if (elem_type != VALTYPE_FUNCREF) {
                return -1;
            }
            const uint32_t func_index = read_uleb128(module, &size_read);
            if (size_read == 0) {
                return -1;
            }
            if (!fa_funcref_encode_u32(func_index, &out->value)) {
                return -1;
            }
            break;
        }
        case 0x23: /* global.get */
        {
            const uint32_t global_index = read_uleb128(module, &size_read);
            if (size_read == 0) {
                return -1;
            }
            if (!module->globals || global_index >= module->num_globals) {
                return -1;
            }
            const WasmGlobal* global = &module->globals[global_index];
            if (global->valtype != elem_type || global->is_mutable) {
                return -1;
            }
            out->kind = WASM_ELEMENT_INIT_GLOBAL_GET;
            out->global_index = global_index;
            break;
        }
        default:
            return -1;
    }

    uint8_t end = 0;
    if (wasm_stream_read(module, &end, 1) != 1 || end != 0x0B) {
        return -1;
    }
    return 0;
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
        for (uint32_t i = 0; i < module->num_functions; i++) {
            free(module->functions[i].import_module);
            free(module->functions[i].import_name);
        }
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

    if (module->tables) {
        for (uint32_t i = 0; i < module->num_tables; i++) {
            free(module->tables[i].import_module);
            free(module->tables[i].import_name);
        }
        free(module->tables);
    }

    if (module->elements) {
        for (uint32_t i = 0; i < module->num_elements; i++) {
            free(module->elements[i].elements);
        }
        free(module->elements);
    }

    if (module->data_segments) {
        for (uint32_t i = 0; i < module->num_data_segments; i++) {
            free(module->data_segments[i].data);
        }
        free(module->data_segments);
    }
    
    if (module->memories) {
        for (uint32_t i = 0; i < module->num_memories; i++) {
            free(module->memories[i].import_module);
            free(module->memories[i].import_name);
        }
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
    if (!module) {
        return -1;
    }

    WasmFunction* imported_functions = NULL;
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
            imported_functions = (WasmFunction*)calloc(imported_capacity, sizeof(WasmFunction));
            if (!imported_functions) {
                return -1;
            }
        }

        for (uint32_t j = 0; j < count; j++) {
            uint32_t module_len = 0;
            uint32_t name_len = 0;
            char* module_name = read_string(module, &module_len);
            char* import_name = read_string(module, &name_len);
            if (!module_name && module_len == 0) {
                module_name = (char*)calloc(1, 1);
            }
            if (!import_name && name_len == 0) {
                import_name = (char*)calloc(1, 1);
            }
            if (!module_name || !import_name) {
                free(module_name);
                free(import_name);
                free(imported_functions);
                return -1;
            }

            uint8_t kind = 0;
            if (wasm_stream_read(module, &kind, 1) != 1) {
                free(module_name);
                free(import_name);
                free(imported_functions);
                return -1;
            }
            switch (kind) {
                case 0: /* func */
                {
                    uint32_t type_index = read_uleb128(module, &size_read);
                    WasmFunction* func = &imported_functions[imported_count++];
                    func->type_index = type_index;
                    func->body_offset = 0;
                    func->body_size = 0;
                    func->is_imported = true;
                    func->import_module = module_name;
                    func->import_module_len = module_len;
                    func->import_name = import_name;
                    func->import_name_len = name_len;
                    module_name = NULL;
                    import_name = NULL;
                    break;
                }
                case 1: /* table */
                {
                    uint8_t elem_type = 0;
                    if (wasm_stream_read(module, &elem_type, 1) != 1) {
                        free(module_name);
                        free(import_name);
                        free(imported_functions);
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
                        free(module_name);
                        free(import_name);
                        free(imported_functions);
                        return -1;
                    }
                    break;
                }
                default:
                    free(module_name);
                    free(import_name);
                    free(imported_functions);
                    return -1;
            }
            free(module_name);
            free(import_name);
        }
        break;
    }

    bool found_function_section = false;
    uint32_t defined_count = 0;
    off_t functions_offset = 0;
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_FUNCTION) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                free(imported_functions);
                return -1;
            }

            uint32_t size_read;
            defined_count = read_uleb128(module, &size_read);
            functions_offset = module->sections[i].offset + size_read;
            found_function_section = true;

            const uint32_t total_functions = imported_count + defined_count;
            if (total_functions > 0) {
                module->functions = (WasmFunction*)calloc(total_functions, sizeof(WasmFunction));
                if (!module->functions) {
                    free(imported_functions);
                    return -1;
                }
            }
            module->num_functions = total_functions;
            module->num_imported_functions = imported_count;
            module->functions_offset = functions_offset;

            for (uint32_t j = 0; j < imported_count; ++j) {
                module->functions[j] = imported_functions[j];
            }
            free(imported_functions);
            imported_functions = NULL;

            for (uint32_t j = 0; j < defined_count; j++) {
                module->functions[imported_count + j].type_index = read_uleb128(module, &size_read);
                module->functions[imported_count + j].is_imported = false;
            }

            // Ora carica gli offset dei corpi delle funzioni dalla sezione Code
            for (uint32_t j = 0; j < module->num_sections; j++) {
                if (module->sections[j].type == SECTION_CODE) {
                    if (wasm_stream_seek(module, module->sections[j].offset, SEEK_SET) < 0) {
                        return -1;
                    }

                    uint32_t code_count = read_uleb128(module, &size_read);
                    off_t current_offset = module->sections[j].offset + size_read;

                    if (code_count != defined_count) {
                        return -1;  // Il numero di corpi di funzioni deve corrispondere al numero di funzioni
                    }

                    for (uint32_t k = 0; k < code_count; k++) {
                        uint32_t body_size = read_uleb128(module, &size_read);
                        current_offset += size_read;

                        WasmFunction* func = &module->functions[imported_count + k];
                        func->body_offset = current_offset;
                        func->body_size = body_size;

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

    if (!found_function_section) {
        if (imported_count > 0) {
            module->functions = imported_functions;
            module->num_functions = imported_count;
            module->num_imported_functions = imported_count;
        } else {
            free(imported_functions);
            module->num_functions = 0;
            module->num_imported_functions = 0;
        }
        module->functions_offset = 0;
        return 0;
    }

    free(imported_functions);
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

// Carica le tabelle dalla sezione Table
int wasm_load_tables(WasmModule* module) {
    if (!module) {
        return -1;
    }

    WasmTable* imported_tables = NULL;
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
            imported_tables = (WasmTable*)calloc(imported_capacity, sizeof(WasmTable));
            if (!imported_tables) {
                return -1;
            }
        }

        for (uint32_t j = 0; j < count; j++) {
            uint32_t module_len = 0;
            uint32_t name_len = 0;
            char* module_name = read_string(module, &module_len);
            char* import_name = read_string(module, &name_len);
            if (!module_name && module_len == 0) {
                module_name = (char*)calloc(1, 1);
            }
            if (!import_name && name_len == 0) {
                import_name = (char*)calloc(1, 1);
            }
            if (!module_name || !import_name) {
                free(module_name);
                free(import_name);
                free(imported_tables);
                return -1;
            }

            uint8_t kind = 0;
            if (wasm_stream_read(module, &kind, 1) != 1) {
                free(module_name);
                free(import_name);
                free(imported_tables);
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
                        free(module_name);
                        free(import_name);
                        free(imported_tables);
                        return -1;
                    }
                    if (elem_type != VALTYPE_FUNCREF && elem_type != VALTYPE_EXTERNREF) {
                        free(module_name);
                        free(import_name);
                        free(imported_tables);
                        return -1;
                    }
                    uint32_t flags = read_uleb128(module, &size_read);
                    uint32_t initial = read_uleb128(module, &size_read);
                    uint32_t maximum = 0;
                    const bool has_max = (flags & 0x1) != 0;
                    if (has_max) {
                        maximum = read_uleb128(module, &size_read);
                    }
                    WasmTable* table = &imported_tables[imported_count++];
                    table->elem_type = elem_type;
                    table->has_max = has_max;
                    table->initial_size = initial;
                    table->maximum_size = maximum;
                    table->is_imported = true;
                    table->import_module = module_name;
                    table->import_module_len = module_len;
                    table->import_name = import_name;
                    table->import_name_len = name_len;
                    module_name = NULL;
                    import_name = NULL;
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
                        free(module_name);
                        free(import_name);
                        free(imported_tables);
                        return -1;
                    }
                    break;
                }
                default:
                    free(module_name);
                    free(import_name);
                    free(imported_tables);
                    return -1;
            }
            free(module_name);
            free(import_name);
        }
        break;
    }

    bool found_table_section = false;
    uint32_t defined_count = 0;
    uint32_t tables_offset = 0;
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_TABLE) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                free(imported_tables);
                return -1;
            }

            uint32_t size_read;
            defined_count = read_uleb128(module, &size_read);
            tables_offset = module->sections[i].offset + size_read;
            found_table_section = true;

            const uint32_t total_tables = imported_count + defined_count;
            if (total_tables > 0) {
                module->tables = (WasmTable*)calloc(total_tables, sizeof(WasmTable));
                if (!module->tables) {
                    free(imported_tables);
                    return -1;
                }
            }
            module->num_tables = total_tables;
            module->num_imported_tables = imported_count;
            module->tables_offset = tables_offset;

            for (uint32_t j = 0; j < imported_count; ++j) {
                module->tables[j] = imported_tables[j];
            }
            free(imported_tables);
            imported_tables = NULL;

            for (uint32_t j = 0; j < defined_count; j++) {
                uint8_t elem_type = 0;
                if (wasm_stream_read(module, &elem_type, 1) != 1) {
                    return -1;
                }
                if (elem_type != VALTYPE_FUNCREF && elem_type != VALTYPE_EXTERNREF) {
                    return -1;
                }
                uint32_t flags = read_uleb128(module, &size_read);
                WasmTable* table = &module->tables[imported_count + j];
                table->elem_type = elem_type;
                table->has_max = (flags & 0x1) != 0;
                table->is_imported = false;
                table->initial_size = read_uleb128(module, &size_read);
                if (table->has_max) {
                    table->maximum_size = read_uleb128(module, &size_read);
                }
            }

            return 0;
        }
    }

    if (!found_table_section) {
        if (imported_count > 0) {
            module->tables = imported_tables;
            module->num_tables = imported_count;
            module->num_imported_tables = imported_count;
        } else {
            free(imported_tables);
            module->num_tables = 0;
            module->num_imported_tables = 0;
        }
        module->tables_offset = 0;
        return 0;
    }

    free(imported_tables);
    return -1;
}

// Carica le memory dalla sezione Memory
int wasm_load_memories(WasmModule* module) {
    if (!module) {
        return -1;
    }

    WasmMemory* imported_memories = NULL;
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
            imported_memories = (WasmMemory*)calloc(imported_capacity, sizeof(WasmMemory));
            if (!imported_memories) {
                return -1;
            }
        }

        for (uint32_t j = 0; j < count; j++) {
            uint32_t module_len = 0;
            uint32_t name_len = 0;
            char* module_name = read_string(module, &module_len);
            char* import_name = read_string(module, &name_len);
            if (!module_name && module_len == 0) {
                module_name = (char*)calloc(1, 1);
            }
            if (!import_name && name_len == 0) {
                import_name = (char*)calloc(1, 1);
            }
            if (!module_name || !import_name) {
                free(module_name);
                free(import_name);
                free(imported_memories);
                return -1;
            }

            uint8_t kind = 0;
            if (wasm_stream_read(module, &kind, 1) != 1) {
                free(module_name);
                free(import_name);
                free(imported_memories);
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
                        free(module_name);
                        free(import_name);
                        free(imported_memories);
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
                    uint64_t initial = 0;
                    uint64_t maximum = 0;
                    if (memory64) {
                        initial = read_uleb128_64(module, &size_read);
                        if (flags & 0x1) {
                            maximum = read_uleb128_64(module, &size_read);
                        }
                    } else {
                        initial = read_uleb128(module, &size_read);
                        if (flags & 0x1) {
                            maximum = read_uleb128(module, &size_read);
                        }
                    }
                    WasmMemory* memory = &imported_memories[imported_count++];
                    memory->is_memory64 = memory64;
                    memory->has_max = (flags & 0x1) != 0;
                    memory->initial_size = initial;
                    memory->maximum_size = maximum;
                    memory->is_imported = true;
                    memory->import_module = module_name;
                    memory->import_module_len = module_len;
                    memory->import_name = import_name;
                    memory->import_name_len = name_len;
                    module_name = NULL;
                    import_name = NULL;
                    break;
                }
                case 3: /* global */
                {
                    uint8_t valtype = 0;
                    uint8_t mutability = 0;
                    if (wasm_stream_read(module, &valtype, 1) != 1 ||
                        wasm_stream_read(module, &mutability, 1) != 1) {
                        free(module_name);
                        free(import_name);
                        free(imported_memories);
                        return -1;
                    }
                    break;
                }
                default:
                    free(module_name);
                    free(import_name);
                    free(imported_memories);
                    return -1;
            }
            free(module_name);
            free(import_name);
        }
        break;
    }

    bool found_memory_section = false;
    uint32_t defined_count = 0;
    uint32_t memories_offset = 0;
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_MEMORY) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                free(imported_memories);
                return -1;
            }

            uint32_t size_read;
            defined_count = read_uleb128(module, &size_read);
            memories_offset = module->sections[i].offset + size_read;
            found_memory_section = true;

            const uint32_t total_memories = imported_count + defined_count;
            if (total_memories > 0) {
                module->memories = (WasmMemory*)calloc(total_memories, sizeof(WasmMemory));
                if (!module->memories) {
                    free(imported_memories);
                    return -1;
                }
            }
            module->num_memories = total_memories;
            module->num_imported_memories = imported_count;
            module->memories_offset = memories_offset;

            for (uint32_t j = 0; j < imported_count; ++j) {
                module->memories[j] = imported_memories[j];
            }
            free(imported_memories);
            imported_memories = NULL;

            for (uint32_t j = 0; j < defined_count; j++) {
                uint8_t flags = 0;
                if (wasm_stream_read(module, &flags, 1) != 1) {
                    return -1;
                }
                WasmMemory* memory = &module->memories[imported_count + j];
                memory->is_memory64 = (flags & 0x4) != 0;
                memory->has_max = (flags & 0x1) != 0;
                memory->is_imported = false;
                if (memory->is_memory64) {
                    memory->initial_size = read_uleb128_64(module, &size_read);
                } else {
                    memory->initial_size = read_uleb128(module, &size_read);
                }
                if (memory->has_max) {
                    if (memory->is_memory64) {
                        memory->maximum_size = read_uleb128_64(module, &size_read);
                    } else {
                        memory->maximum_size = read_uleb128(module, &size_read);
                    }
                }
            }

            return 0;
        }
    }

    if (!found_memory_section) {
        if (imported_count > 0) {
            module->memories = imported_memories;
            module->num_memories = imported_count;
            module->num_imported_memories = imported_count;
        } else {
            free(imported_memories);
            module->num_memories = 0;
            module->num_imported_memories = 0;
        }
        module->memories_offset = 0;
        return 0;
    }

    free(imported_memories);
    return -1;
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

// Carica gli element dalla sezione Element
int wasm_load_elements(WasmModule* module) {
    if (!module) {
        return -1;
    }

    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_ELEMENT) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                return -1;
            }

            uint32_t size_read;
            uint32_t count = read_uleb128(module, &size_read);

            module->num_elements = count;
            module->elements = (WasmElementSegment*)malloc(count * sizeof(WasmElementSegment));
            if (!module->elements) {
                return -1;
            }
            memset(module->elements, 0, count * sizeof(WasmElementSegment));

            module->elements_offset = module->sections[i].offset + size_read;

            for (uint32_t j = 0; j < count; j++) {
                uint32_t flags = read_uleb128(module, &size_read);
                WasmElementSegment* segment = &module->elements[j];
                segment->elem_type = VALTYPE_FUNCREF;
                segment->table_index = 0;
                bool uses_expr_list = false;
                switch (flags) {
                    case 0: /* active, table 0, legacy funcidx vector */
                        if (wasm_read_init_expr_offset(module, &segment->offset) != 0) {
                            return -1;
                        }
                        break;
                    case 1: /* passive, legacy funcidx vector */
                        segment->is_passive = true;
                        {
                            uint8_t elem_kind = 0x00;
                            if (wasm_stream_read(module, &elem_kind, 1) != 1) {
                                return -1;
                            }
                            if (elem_kind != 0x00 && elem_kind != VALTYPE_FUNCREF) {
                                return -1;
                            }
                        }
                        break;
                    case 2: /* active, explicit table, legacy funcidx vector */
                        segment->table_index = read_uleb128(module, &size_read);
                        if (size_read == 0) {
                            return -1;
                        }
                        if (wasm_read_init_expr_offset(module, &segment->offset) != 0) {
                            return -1;
                        }
                        {
                            uint8_t elem_kind = 0x00;
                            if (wasm_stream_read(module, &elem_kind, 1) != 1) {
                                return -1;
                            }
                            if (elem_kind != 0x00 && elem_kind != VALTYPE_FUNCREF) {
                                return -1;
                            }
                        }
                        break;
                    case 3: /* declarative, legacy funcidx vector */
                        segment->is_declarative = true;
                        {
                            uint8_t elem_kind = 0x00;
                            if (wasm_stream_read(module, &elem_kind, 1) != 1) {
                                return -1;
                            }
                            if (elem_kind != 0x00 && elem_kind != VALTYPE_FUNCREF) {
                                return -1;
                            }
                        }
                        break;
                    case 4: /* active, table 0, typed expression vector */
                        if (wasm_read_init_expr_offset(module, &segment->offset) != 0) {
                            return -1;
                        }
                        if (wasm_stream_read(module, &segment->elem_type, 1) != 1 ||
                            !wasm_is_ref_type(segment->elem_type)) {
                            return -1;
                        }
                        uses_expr_list = true;
                        break;
                    case 5: /* passive, typed expression vector */
                        segment->is_passive = true;
                        if (wasm_stream_read(module, &segment->elem_type, 1) != 1 ||
                            !wasm_is_ref_type(segment->elem_type)) {
                            return -1;
                        }
                        uses_expr_list = true;
                        break;
                    case 6: /* active, explicit table, typed expression vector */
                        segment->table_index = read_uleb128(module, &size_read);
                        if (size_read == 0) {
                            return -1;
                        }
                        if (wasm_read_init_expr_offset(module, &segment->offset) != 0) {
                            return -1;
                        }
                        if (wasm_stream_read(module, &segment->elem_type, 1) != 1 ||
                            !wasm_is_ref_type(segment->elem_type)) {
                            return -1;
                        }
                        uses_expr_list = true;
                        break;
                    case 7: /* declarative, typed expression vector */
                        segment->is_declarative = true;
                        if (wasm_stream_read(module, &segment->elem_type, 1) != 1 ||
                            !wasm_is_ref_type(segment->elem_type)) {
                            return -1;
                        }
                        uses_expr_list = true;
                        break;
                    default:
                        return -1;
                }

                uint32_t elem_count = read_uleb128(module, &size_read);
                if (size_read == 0) {
                    return -1;
                }
                segment->element_count = elem_count;
                if (elem_count > 0) {
                    segment->elements = (WasmElementInit*)calloc(elem_count, sizeof(WasmElementInit));
                    if (!segment->elements) {
                        return -1;
                    }
                }
                for (uint32_t k = 0; k < elem_count; k++) {
                    if (uses_expr_list) {
                        if (wasm_read_element_expr_ref(module, segment->elem_type, &segment->elements[k]) != 0) {
                            return -1;
                        }
                    } else {
                        const uint32_t func_index = read_uleb128(module, &size_read);
                        if (size_read == 0) {
                            return -1;
                        }
                        segment->elements[k].kind = WASM_ELEMENT_INIT_REF_VALUE;
                        if (!fa_funcref_encode_u32(func_index, &segment->elements[k].value)) {
                            return -1;
                        }
                    }
                }
            }

            return 0;
        }
    }

    module->num_elements = 0;
    return 0;
}

// Carica i data segment dalla sezione Data
int wasm_load_data(WasmModule* module) {
    for (uint32_t i = 0; i < module->num_sections; i++) {
        if (module->sections[i].type == SECTION_DATA) {
            if (wasm_stream_seek(module, module->sections[i].offset, SEEK_SET) < 0) {
                return -1;
            }

            uint32_t size_read;
            uint32_t count = read_uleb128(module, &size_read);

            module->num_data_segments = count;
            module->data_segments = (WasmDataSegment*)malloc(count * sizeof(WasmDataSegment));
            if (!module->data_segments) {
                return -1;
            }
            memset(module->data_segments, 0, count * sizeof(WasmDataSegment));

            module->data_segments_offset = module->sections[i].offset + size_read;

            for (uint32_t j = 0; j < count; j++) {
                uint32_t flags = read_uleb128(module, &size_read);
                WasmDataSegment* segment = &module->data_segments[j];
                segment->memory_index = 0;

                switch (flags) {
                    case 0:
                        if (wasm_read_init_expr_offset(module, &segment->offset) != 0) {
                            return -1;
                        }
                        break;
                    case 1:
                        segment->is_passive = true;
                        break;
                    case 2:
                        segment->memory_index = read_uleb128(module, &size_read);
                        if (wasm_read_init_expr_offset(module, &segment->offset) != 0) {
                            return -1;
                        }
                        break;
                    default:
                        return -1;
                }

                uint32_t data_size = read_uleb128(module, &size_read);
                segment->size = data_size;
                if (data_size > 0) {
                    segment->data = (uint8_t*)malloc(data_size);
                    if (!segment->data) {
                        return -1;
                    }
                    if (wasm_stream_read(module, segment->data, data_size) != (ssize_t)data_size) {
                        return -1;
                    }
                }
            }

            return 0;
        }
    }

    module->num_data_segments = 0;
    return 0;
}

// Carica un byte code di una funzione on-demand
uint8_t* wasm_load_function_body(WasmModule* module, uint32_t func_idx) {
    if (func_idx >= module->num_functions) {
        return NULL;
    }
    
    WasmFunction* func = &module->functions[func_idx];
    if (func->is_imported || func->body_size == 0) {
        return NULL;
    }
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
            if (module->memories[i].is_imported) {
                const char* module_name = module->memories[i].import_module ? module->memories[i].import_module : "<null>";
                const char* import_name = module->memories[i].import_name ? module->memories[i].import_name : "<null>";
                printf("Memory %u: Import=%s.%s, Type=%s, Initial=%" PRIu64 " pages",
                       i,
                       module_name,
                       import_name,
                       module->memories[i].is_memory64 ? "Memory64" : "Memory32",
                       module->memories[i].initial_size);
            } else {
                printf("Memory %u: Type=%s, Initial=%" PRIu64 " pages",
                       i,
                       module->memories[i].is_memory64 ? "Memory64" : "Memory32",
                       module->memories[i].initial_size);
            }
            
            if (module->memories[i].has_max) {
                printf(", Maximum=%" PRIu64 " pages", module->memories[i].maximum_size);
            } else {
                printf(", No maximum");
            }
            printf("\n");
        }
    }

    if (module->num_tables > 0) {
        printf("\n=== Tables (%u) ===\n", module->num_tables);
        for (uint32_t i = 0; i < module->num_tables; i++) {
            if (module->tables[i].is_imported) {
                const char* module_name = module->tables[i].import_module ? module->tables[i].import_module : "<null>";
                const char* import_name = module->tables[i].import_name ? module->tables[i].import_name : "<null>";
                printf("Table %u: Import=%s.%s, ElemType=0x%02X, Initial=%u",
                       i,
                       module_name,
                       import_name,
                       module->tables[i].elem_type,
                       module->tables[i].initial_size);
            } else {
                printf("Table %u: ElemType=0x%02X, Initial=%u",
                       i,
                       module->tables[i].elem_type,
                       module->tables[i].initial_size);
            }
            if (module->tables[i].has_max) {
                printf(", Maximum=%u", module->tables[i].maximum_size);
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
            if (module->functions[i].is_imported) {
                const char* module_name = module->functions[i].import_module ? module->functions[i].import_module : "<null>";
                const char* import_name = module->functions[i].import_name ? module->functions[i].import_name : "<null>";
                printf("Function %u: Import=%s.%s, Type=%u\n",
                       i, module_name, import_name, module->functions[i].type_index);
            } else {
                printf("Function %u: Type=%u, Body Offset=0x%" PRIxMAX ", Body Size=%u\n",
                       i, module->functions[i].type_index, (uintmax_t)module->functions[i].body_offset, module->functions[i].body_size);
            }
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
    wasm_load_tables(module);
    wasm_load_memories(module);
    wasm_load_globals(module);
    wasm_load_elements(module);
    wasm_load_data(module);
    
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
