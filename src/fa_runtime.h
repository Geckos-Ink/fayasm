#pragma once

#include "fa_types.h"

typedef ptr (*fa_Malloc)(int);
typedef void (*fa_Free)(ptr);

// Dichiara la struct utilizzando l'alias
struct fa_Runtime {
    fa_Malloc malloc;
    fa_Free free;
};

fa_Runtime fa_Runtime_init();