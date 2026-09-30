#pragma once
/*
 * The kernel's own layout of what tinyactors_host/directory.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/directory.h"

struct tah_directory {
    tah_map entries; /* (uintptr_t)ta_address * -> ta_actor_id, 0 when unloaded */
};
