#pragma once

#include "fa_types.h"

// Dichiara la struct utilizzando l'alias
typedef struct {
    fa_Malloc malloc;
    fa_Free free;
} fa_Runtime;

fa_Runtime fa_Runtime_init();