/* The harness on Linux: glibc declares mkdtemp and the rest of POSIX 2008
 * under _POSIX_C_SOURCE. */
#define _POSIX_C_SOURCE 200809L
#include "harness.c"
