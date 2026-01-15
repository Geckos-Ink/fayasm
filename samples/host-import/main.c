#include "fa_runtime.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#define HOST_LIB_DEFAULT "./libfayasm_host_add.dylib"
#elif defined(_WIN32)
#define HOST_LIB_DEFAULT "fayasm_host_add.dll"
#else
#define HOST_LIB_DEFAULT "./libfayasm_host_add.so"
#endif

static const uint8_t kHostImportModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x02, 0x10, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x08,
    0x68, 0x6f, 0x73, 0x74, 0x5f, 0x61, 0x64, 0x64,
    0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x01,
    0x0a, 0x0a, 0x01, 0x08, 0x00, 0x41, 0x07, 0x41,
    0x05, 0x10, 0x00, 0x0b
};

static int find_exported_function(const WasmModule* module, const char* name, uint32_t* out_index) {
    if (!module || !name || !out_index || !module->exports) {
        return 0;
    }
    for (uint32_t i = 0; i < module->num_exports; ++i) {
        const WasmExport* export_entry = &module->exports[i];
        if (export_entry->kind != 0 || !export_entry->name) {
            continue;
        }
        if (strcmp(export_entry->name, name) == 0) {
            *out_index = export_entry->index;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* library_path = (argc > 1 && argv[1]) ? argv[1] : HOST_LIB_DEFAULT;
    WasmModule* module = wasm_module_init_from_memory(kHostImportModule, sizeof(kHostImportModule));
    if (!module) {
        fprintf(stderr, "Failed to load wasm module.\n");
        return 1;
    }

    fa_Runtime* runtime = fa_Runtime_init();
    if (!runtime) {
        fprintf(stderr, "Failed to create runtime.\n");
        wasm_module_free(module);
        return 1;
    }

    if (fa_Runtime_attach_module(runtime, module) != FA_RUNTIME_OK) {
        fprintf(stderr, "Failed to attach module.\n");
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    if (fa_Runtime_bindHostFunctionFromLibrary(runtime,
                                                    "env",
                                                    "host_add",
                                                    library_path,
                                                    "fayasm_host_add") != FA_RUNTIME_OK) {
        fprintf(stderr, "Failed to bind host function from %s.\n", library_path);
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    fa_Job* job = fa_Runtime_create_job(runtime);
    if (!job) {
        fprintf(stderr, "Failed to create job.\n");
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    uint32_t function_index = 0;
    if (!find_exported_function(module, "run", &function_index)) {
        fprintf(stderr, "Failed to locate export 'run'.\n");
        (void)fa_Runtime_destroy_job(runtime, job);
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    int status = fa_Runtime_execute_job(runtime, job, function_index);
    if (status != FA_RUNTIME_OK) {
        fprintf(stderr, "Execution failed with status %d.\n", status);
        (void)fa_Runtime_destroy_job(runtime, job);
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    const fa_JobValue* value = fa_JobStack_peek(&job->stack, 0);
    if (!value || value->kind != fa_job_value_i32) {
        fprintf(stderr, "Unexpected result on stack.\n");
        (void)fa_Runtime_destroy_job(runtime, job);
        fa_Runtime_free(runtime);
        wasm_module_free(module);
        return 1;
    }

    printf("Result: %d\n", value->payload.i32_value);

    (void)fa_Runtime_destroy_job(runtime, job);
    fa_Runtime_free(runtime);
    wasm_module_free(module);
    return 0;
}
