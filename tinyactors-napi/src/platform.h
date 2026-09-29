#pragma once
/*
 * What the platform gives the host kernel through napi (kernel ADR 0001,
 * section 3): the kernel links no C library, so napi supplies the memory it
 * allocates from, as a ta_allocator over the process's malloc.
 */
#include "tinyactors.h"

/* The process's malloc and free as a ta_allocator; stateless, one for every system. */
ta_allocator tna_platform_allocator(void);
