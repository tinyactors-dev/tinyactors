/*
** version.c -- the pocketparse version string.
**
** Freestanding: no libc. A single literal, reachable by both the library
** and the hosted CLI through the public declaration in pocketparse.h.
*/
#include "pocketparse.h"

/* ** The compiled-in version, in major.minor.patch form. */
#define POCKETPARSE_VERSION "0.1.0"

/* ** Return the version string. */
const char *pocketparse_version_string(void)
{
    return POCKETPARSE_VERSION;
}
