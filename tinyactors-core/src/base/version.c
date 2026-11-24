#include <tinyactors/types.h>

/* The macros as this library was compiled: a caller compiled against another
 * header reads its own copies of them. */
ta_version ta_library_version(void) {
    ta_version version = {TA_VERSION_MAJOR, TA_VERSION_MINOR, TA_VERSION_PATCH, TA_ABI_VERSION};
    return version;
}
