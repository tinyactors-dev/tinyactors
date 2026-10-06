// Compile the public-header check as C++ as well.
#include <stddef.h> /* not <cstddef>: the pinned clang has no C++ library, and the check needs none */
#include "test_headers.c"
