#pragma once
/*
 * The kernel's own layout of what tinyactors_host/definitions.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/definitions.h"

struct tah_definitions {
    tah_map origins; /* definition ID -> tah_definition_origin, for NESTED and INVOKE */
    uint64_t registered;
    uint64_t retired;
    uint64_t freed;
    /* The origin of the registrations core makes now; HOST outside a child's compile. */
    tah_definition_origin registering;
};
