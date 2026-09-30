#pragma once
/*
 * The kernel's own layout of what tinyactors_host/offloads.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/offloads.h"

struct tah_offload {
    uint8_t *bytes;
    size_t length;
    /* References the entry holds to values its bytes name by handle. */
    ta_value *handles;
    uint32_t handle_count;
};

struct tah_offloads {
    ta_allocator allocator;
    tah_map entries; /* (uintptr_t)ta_address * -> tah_offload * */
    size_t bytes_in_use;
};
