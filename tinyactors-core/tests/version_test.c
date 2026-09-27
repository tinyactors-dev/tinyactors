/*
 * The version: ta_version() answers TA_VERSION of the header the library
 * was built from, packed as (major << 16) | (minor << 8) | patch.
 */
#include "../include/tinyactors.h"

#include <stdio.h>

int main(void) {
    int failures = 0;
    if (ta_version() != (uint32_t)TA_VERSION) {
        fprintf(stderr, "ta_version() = 0x%06x, TA_VERSION = 0x%06x\n", (unsigned)ta_version(), (unsigned)TA_VERSION);
        failures += 1;
    }
    if (TA_VERSION != ((TA_VERSION_MAJOR << 16) | (TA_VERSION_MINOR << 8) | TA_VERSION_PATCH) ||
        TA_VERSION_MINOR > 255 || TA_VERSION_PATCH > 255) {
        fprintf(stderr, "TA_VERSION does not pack TA_VERSION_MAJOR.MINOR.PATCH\n");
        failures += 1;
    }
    if (failures == 0) {
        printf("version test: %u.%u.%u\n", (unsigned)(ta_version() >> 16), (unsigned)((ta_version() >> 8) & 0xffu),
               (unsigned)(ta_version() & 0xffu));
    }
    return failures == 0 ? 0 : 1;
}
