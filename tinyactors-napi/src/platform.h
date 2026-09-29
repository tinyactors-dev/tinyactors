#pragma once
/*
 * What the platform gives the host kernel through napi (kernel ADR 0001,
 * section 3): the kernel links no C library, so napi supplies memory (the
 * process's malloc), clocks (CLOCK_MONOTONIC and the wall clock) and
 * randomness (the OS), as a tah_platform every system is created with.
 */
#include "tinyactors.h"
#include "tinyactors_host.h"

/* The process's malloc and free as a ta_allocator; stateless, one for every system. */
ta_allocator tna_platform_allocator(void);
/* The platform napi gives every system; stateless. */
tah_platform tna_platform(void);
/* The monotonic clock, in nanoseconds, for napi's own code outside a system. */
uint64_t tna_monotonic_ns(void);
