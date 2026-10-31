#pragma once
#include "fa_jit.h"

/* Preparation-only bridge; not part of the public opcode API. */
fa_SequenceOperation fa_ops_sequence_operation(uint8_t opcode);
void fa_ops_sequence_bind(fa_JitPreparedOp* op);
