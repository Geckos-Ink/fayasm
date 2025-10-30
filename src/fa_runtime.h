#pragma once

#include "fa_types.h"
#include "dynamic_list.h"

// Dichiara la struct utilizzando l'alias
typedef struct {
    fa_Malloc malloc;
    fa_Free free;

    list_t* jobs; // fa_Job[]
} fa_Runtime;

fa_Runtime* fa_Runtime_init();