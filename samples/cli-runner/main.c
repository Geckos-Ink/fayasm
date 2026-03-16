#include "fa_runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* exe) {
    const char* name = (exe && exe[0] != '\0') ? exe : "fayasm_run";
    printf("Usage: %s <module.wasm> <export_name> [typed-arg ...]\n", name);
    printf("Executes an exported function and prints returned values.\n");
    printf("Typed args must use one of: i32:<value> i64:<value> f32:<value> f64:<value>\n");
    printf("Example: %s module.wasm add i32:7 i32:5\n", name);
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

static int parse_i32_value(const char* text, i32* out) {
    if (!text || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    intmax_t parsed = strtoimax(text, &end, 0);
    if (end == text || !end || *end != '\0' || errno == ERANGE ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        return 0;
    }
    *out = (i32)parsed;
    return 1;
}

static int parse_i64_value(const char* text, i64* out) {
    if (!text || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    intmax_t parsed = strtoimax(text, &end, 0);
    if (end == text || !end || *end != '\0' || errno == ERANGE ||
        parsed < INT64_MIN || parsed > INT64_MAX) {
        return 0;
    }
    *out = (i64)parsed;
    return 1;
}

static int parse_f32_value(const char* text, f32* out) {
    if (!text || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    float parsed = strtof(text, &end);
    if (end == text || !end || *end != '\0' || errno == ERANGE) {
        return 0;
    }
    *out = (f32)parsed;
    return 1;
}

static int parse_f64_value(const char* text, f64* out) {
    if (!text || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || !end || *end != '\0' || errno == ERANGE) {
        return 0;
    }
    *out = (f64)parsed;
    return 1;
}

static int parse_typed_argument(const char* token, fa_JobValue* out, uint32_t* out_type) {
    if (!token || !out || !out_type) {
        return 0;
    }
    const char* sep = strchr(token, ':');
    if (!sep || sep == token || sep[1] == '\0') {
        return 0;
    }
    const size_t type_len = (size_t)(sep - token);
    const char* value_text = sep + 1;

    memset(out, 0, sizeof(*out));

    if (type_len == 3 && strncmp(token, "i32", 3) == 0) {
        i32 value = 0;
        if (!parse_i32_value(value_text, &value)) {
            return 0;
        }
        out->kind = fa_job_value_i32;
        out->is_signed = true;
        out->bit_width = 32U;
        out->payload.i32_value = value;
        *out_type = VALTYPE_I32;
        return 1;
    }
    if (type_len == 3 && strncmp(token, "i64", 3) == 0) {
        i64 value = 0;
        if (!parse_i64_value(value_text, &value)) {
            return 0;
        }
        out->kind = fa_job_value_i64;
        out->is_signed = true;
        out->bit_width = 64U;
        out->payload.i64_value = value;
        *out_type = VALTYPE_I64;
        return 1;
    }
    if (type_len == 3 && strncmp(token, "f32", 3) == 0) {
        f32 value = 0.0f;
        if (!parse_f32_value(value_text, &value)) {
            return 0;
        }
        out->kind = fa_job_value_f32;
        out->is_signed = false;
        out->bit_width = 32U;
        out->payload.f32_value = value;
        *out_type = VALTYPE_F32;
        return 1;
    }
    if (type_len == 3 && strncmp(token, "f64", 3) == 0) {
        f64 value = 0.0;
        if (!parse_f64_value(value_text, &value)) {
            return 0;
        }
        out->kind = fa_job_value_f64;
        out->is_signed = false;
        out->bit_width = 64U;
        out->payload.f64_value = value;
        *out_type = VALTYPE_F64;
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* module_path = argv[1];
    const char* export_name = argv[2];
    WasmModule* module = NULL;
    fa_Runtime* runtime = NULL;
    fa_Job* job = NULL;
    fa_JobValue* args = NULL;
    int exit_code = 1;

    module = load_module_from_path(module_path, 1);
    if (!module) {
        fprintf(stderr, "error: failed to load wasm module '%s'\n", module_path);
        return 2;
    }

    uint32_t function_index = 0;
    if (!module_find_exported_function(module, export_name, &function_index)) {
        fprintf(stderr, "error: export '%s' not found or not a function\n", export_name);
        exit_code = 3;
        goto cleanup;
    }
    if (function_index >= module->num_functions) {
        fprintf(stderr, "error: export '%s' points to invalid function index %u\n",
                export_name,
                function_index);
        exit_code = 3;
        goto cleanup;
    }

    const WasmFunction* function = &module->functions[function_index];
    if (function->type_index >= module->num_types) {
        fprintf(stderr, "error: invalid type index %u for export '%s'\n",
                function->type_index,
                export_name);
        exit_code = 3;
        goto cleanup;
    }
    const WasmFunctionType* signature = &module->types[function->type_index];
    const uint32_t provided_args = (uint32_t)(argc - 3);
    if (provided_args != signature->num_params) {
        fprintf(stderr,
                "error: export '%s' expects %u argument(s), but %u were provided\n",
                export_name,
                signature->num_params,
                provided_args);
        exit_code = 4;
        goto cleanup;
    }
    if (signature->num_params > 0 && !signature->param_types) {
        fprintf(stderr, "error: export '%s' has invalid parameter signature metadata\n", export_name);
        exit_code = 4;
        goto cleanup;
    }

    if (signature->num_params > 0) {
        args = (fa_JobValue*)calloc(signature->num_params, sizeof(fa_JobValue));
        if (!args) {
            fprintf(stderr, "error: out of memory while preparing arguments\n");
            exit_code = 4;
            goto cleanup;
        }
        for (uint32_t i = 0; i < signature->num_params; ++i) {
            uint32_t parsed_type = 0;
            if (!parse_typed_argument(argv[3 + i], &args[i], &parsed_type)) {
                fprintf(stderr,
                        "error: invalid argument %u '%s' (expected format <type>:<value>)\n",
                        i,
                        argv[3 + i]);
                exit_code = 4;
                goto cleanup;
            }
            if (parsed_type != signature->param_types[i]) {
                fprintf(stderr,
                        "error: argument %u type mismatch: expected %s, got %s\n",
                        i,
                        valtype_name(signature->param_types[i]),
                        valtype_name(parsed_type));
                exit_code = 4;
                goto cleanup;
            }
        }
    }

    runtime = fa_Runtime_init();
    if (!runtime) {
        fprintf(stderr, "error: failed to initialize runtime\n");
        exit_code = 5;
        goto cleanup;
    }
    int status = fa_Runtime_attachModule(runtime, module);
    if (status != FA_RUNTIME_OK) {
        fprintf(stderr, "error: fa_Runtime_attachModule failed: %s (%d)\n", runtime_status_string(status), status);
        exit_code = 6;
        goto cleanup;
    }

    job = fa_Runtime_createJob(runtime);
    if (!job) {
        fprintf(stderr, "error: fa_Runtime_createJob failed\n");
        exit_code = 7;
        goto cleanup;
    }

    status = fa_Runtime_executeJobWithArgs(runtime, job, function_index, args, signature->num_params);
    if (status != FA_RUNTIME_OK) {
        fprintf(stderr, "error: fa_Runtime_executeJobWithArgs failed: %s (%d)\n", runtime_status_string(status), status);
        exit_code = 8;
        goto cleanup;
    }

    if (signature->num_results == 0) {
        printf("ok: export '%s' executed (no results)\n", export_name);
    } else {
        for (uint32_t i = 0; i < signature->num_results; ++i) {
            const size_t depth = (size_t)(signature->num_results - 1U - i);
            const fa_JobValue* value = fa_JobStack_peek(&job->stack, depth);
            if (!value || !value_matches_type(value, signature->result_types[i])) {
                fprintf(stderr, "error: missing or invalid value for result[%u]\n", i);
                exit_code = 9;
                goto cleanup;
            }
            print_value(i, signature->result_types[i], value);
        }
    }

    exit_code = 0;

cleanup:
    free(args);
    if (job && runtime) {
        (void)fa_Runtime_destroyJob(runtime, job);
    }
    if (runtime) {
        fa_Runtime_free(runtime);
    }
    if (module) {
        wasm_module_free(module);
    }
    return exit_code;
}
