#pragma once

#include "tinyactors.h"

/* The test host's single libc allocation boundary. */
ta_allocator tat_libc_allocator(void);

