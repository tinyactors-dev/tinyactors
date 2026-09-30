#pragma once
/*
 * The kernel's own layout of what tinyactors_host/effects.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/effects.h"

struct tah_effects {
    ta_arena *arena;   /* the turn arena */
    tah_effect *first;
    tah_effect *last;
    uint32_t count;
    bool closed;       /* the system is closing: new effects are dropped */
    uint64_t dropped;  /* effects that could not be queued (no memory, or closing) */
};
