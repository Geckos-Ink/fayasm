#include "fa_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* exe) {
    const char* name = (exe && exe[0] != '\0') ? exe : "fayasm_run";
    printf("Usage: %s <module.wasm> <export_name>\n", name);
    printf("Executes a zero-argument exported function and prints returned values.\n");
}

static const char* runtime_status_string(int status) {
    switch (status) {
        case FA_RUNTIME_OK:
            return "ok";
        case FA_RUNTIME_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case FA_RUNTIME_ERR_OUT_OF_MEMORY:
            return "out of memory";
        case FA_RUNTIME_ERR_NO_MODULE:
            return "no module";
        case FA_RUNTIME_ERR_STREAM:
            return "stream error";
        case FA_RUNTIME_ERR_UNSUPPORTED:
            return "unsupported";
        case FA_RUNTIME_ERR_UNIMPLEMENTED_OPCODE:
            return "unimplemented opcode";
        case FA_RUNTIME_ERR_CALL_DEPTH_EXCEEDED:
            return "call depth exceeded";
        case FA_RUNTIME_ERR_TRAP:
            return "trap";
        default:
            return "unknown";
    }
}

static WasmModule* load_module_from_path(const char* path, int load_exports) {
    if (!path) {
        return NULL;
    }
    WasmModule* module = wasm_module_init(path);
    if (!module) {
        return NULL;
    }
    if (wasm_load_header(module) != 0 ||
        wasm_scan_sections(module) != 0 ||
        wasm_load_types(module) != 0 ||
        wasm_load_functions(module) != 0 ||
        (load_exports && wasm_load_exports(module) != 0) ||
        wasm_load_tables(module) != 0 ||
        wasm_load_memories(module) != 0 ||
        wasm_load_globals(module) != 0 ||
        wasm_load_elements(module) != 0 ||
        wasm_load_data(module) != 0) {
        wasm_module_free(module);
        return NULL;
    }
    return module;
}

static int module_find_exported_function(const WasmModule* module,
                                         const char* export_name,
                                         uint32_t* out_function_index) {
    if (!module || !export_name || !out_function_index) {
        return 0;
    }
    if (!module->exports || module->num_exports == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < module->num_exports; ++i) {
        const WasmExport* export_desc = &module->exports[i];
        if (!export_desc->name || export_desc->kind != 0) {
            continue;
        }
        if (strcmp(export_desc->name, export_name) == 0) {
            *out_function_index = export_desc->index;
            return 1;
        }
    }
    return 0;
}

static int value_matches_type(const fa_JobValue* value, uint32_t valtype) {
    if (!value) {
        return 0;
    }
    switch (valtype) {
        case VALTYPE_I32:
            return value->kind == fa_job_value_i32;
        case VALTYPE_I64:
            return value->kind == fa_job_value_i64;
        case VALTYPE_F32:
            return value->kind == fa_job_value_f32;
        case VALTYPE_F64:
            return value->kind == fa_job_value_f64;
        case VALTYPE_V128:
            return value->kind == fa_job_value_v128;
        case VALTYPE_FUNCREF:
        case VALTYPE_EXTERNREF:
            return value->kind == fa_job_value_ref;
        default:
            return 0;
    }
}

static const char* valtype_name(uint32_t valtype) {
    switch (valtype) {
        case VALTYPE_I32:
            return "i32";
        case VALTYPE_I64:
            return "i64";
        case VALTYPE_F32:
            return "f32";
        case VALTYPE_F64:
            return "f64";
        case VALTYPE_V128:
            return "v128";
        case VALTYPE_FUNCREF:
            return "funcref";
        case VALTYPE_EXTERNREF:
            return "externref";
        default:
            return "unknown";
    }
}

static void print_value(uint32_t index, uint32_t valtype, const fa_JobValue* value) {
    printf("result[%u] (%s): ", index, valtype_name(valtype));
    switch (valtype) {
        case VALTYPE_I32:
            printf("%" PRId32 "\n", value->payload.i32_value);
            break;
        case VALTYPE_I64:
            printf("%" PRId64 "\n", value->payload.i64_value);
            break;
        case VALTYPE_F32:
            printf("%.9g\n", (double)value->payload.f32_value);
            break;
        case VALTYPE_F64:
            printf("%.17g\n", value->payload.f64_value);
            break;
        case VALTYPE_V128:
            printf("0x%016" PRIx64 "%016" PRIx64 "\n",
                   value->payload.v128_value.high,
                   value->payload.v128_value.low);
            break;
        case VALTYPE_FUNCREF:
        case VALTYPE_EXTERNREF:
            printf("0x%" PRIx64 "\n", (uint64_t)value->payload.ref_value);
            break;
        default:
            printf("<unsupported>\n");
            break;
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* module_path = argv[1];
    const char* export_name = argv[2];

    WasmModule* module = load_module_from_path(module_path, 1);
    if (!module) {
        fprintf(stderr, "error: failed to load wasm module '%s'\n", module_path);
        return 2;
    }

    uint32_t function_index = 0;
    if (!module_find_exported_function(module, export_name, &function_index)) {
        fprintf(stderr, "error: export '%s' not found or not a function\n", export_name);
        wasm_module_free(module);
        return 3;
    }
    if (function_index >= module->num_functions) {
        fprintf(stderr, "error: export '%s' points to invalid function index %u\n",
                export_name,
                function_index);
        wasm_module_free(module);
        return 3;
    }

    const WasmFunction* function = &module->functions[function_index];
    if (function->type_index >= module->num_types) {
        fprintf(stderr, "error: invalid type index %u for export '%s'\n",
                function->type_index,
                export_name);
        wasm_module_free(module);
        return 3;
    }
    const WasmFunctionType* signature = &module->types[function->type_index];
    if (signature->num_params != 0) {
        fprintf(stderr,
                "error: export '%s' has %u parameter(s); fayasm_run currently supports only zero-arg exports\n",
                export_name,
                signature->num_params);
        wasm_module_free(module);
        return 4;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        fprintf(stderr, "error: failed to initialize runtime\n");
        wasm_module_free(module);
        return 5;
    }
    int status = fa_Runtime_attachModule(runtime, module);
    if (status != FA_RUNTIME_OK) {
        fprintf(stderr, "error: fa_Runtime_attachModule failed: %s (%d)\n", runtime_status_string(status), status);
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 6;
    }

    fa_Job* job = fa_Runtime_createJob(runtime);
    if (!job) {
        fprintf(stderr, "error: fa_Runtime_createJob failed\n");
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 7;
    }

    status = fa_Runtime_executeJob(runtime, job, function_index);
    if (status != FA_RUNTIME_OK) {
        fprintf(stderr, "error: fa_Runtime_executeJob failed: %s (%d)\n", runtime_status_string(status), status);
        fa_Runtime_destroyJob(runtime, job);
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 8;
    }

    if (signature->num_results == 0) {
        printf("ok: export '%s' executed (no results)\n", export_name);
    } else {
        for (uint32_t i = 0; i < signature->num_results; ++i) {
            const size_t depth = (size_t)(signature->num_results - 1U - i);
            const fa_JobValue* value = fa_JobStack_peek(&job->stack, depth);
            if (!value || !value_matches_type(value, signature->result_types[i])) {
                fprintf(stderr, "error: missing or invalid value for result[%u]\n", i);
                fa_Runtime_destroyJob(runtime, job);
                fa_Runtime_free(runtime);
                wasm_module_free(module);
                return 9;
            }
            print_value(i, signature->result_types[i], value);
        }
    }

    fa_Runtime_destroyJob(runtime, job);
    fa_Runtime_free(runtime);
    wasm_module_free(module);
    return 0;
}
