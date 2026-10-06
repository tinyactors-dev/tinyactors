/* The harness on macOS: its headers hide mkdtemp (POSIX 2008) under
 * _POSIX_C_SOURCE, so ask for the full Darwin set. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "harness.c"
