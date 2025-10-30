#include "fa_runtime.h"

ptr fa_default_malloc(int size){
    return malloc(size);
}

void fa_default_free(ptr region){
    free(region);
}

fa_Runtime* fa_Runtime_init(){
    fa_Runtime* runtime = malloc(sizeof(fa_Runtime));
    runtime->malloc = fa_default_malloc;
    runtime->free = fa_default_free;

    //todo: runtime->jobs [...]

    return runtime;
}