/* The release's smoke test of libtinyactors (mise run release): built for
 * each Linux release target against the packed static and shared library,
 * and run in a container of that architecture. It uses the public header
 * alone: an address written and read back, and an image refused. */
#include "tinyactors.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    char buffer[TA_ADDRESS_SESSION_MAX];
    ta_string_view system = {"smoke", 5};
    uint32_t length = ta_address_format(system, 42, buffer, sizeof buffer);
    ta_address address;
    ta_string_view text = {buffer, length};
    if (length == 0 || ta_address_parse(text, &address) != TA_OK || address.session != 42 ||
        address.system.length != 5 || memcmp(address.system.pointer, "smoke", 5) != 0) {
        fprintf(stderr, "tinyactors smoke: the address did not round-trip\n");
        return 1;
    }
    static const unsigned char not_an_image[64];
    ta_image image;
    if (ta_image_open(not_an_image, sizeof not_an_image, &image) == TA_OK) {
        fprintf(stderr, "tinyactors smoke: an image of zeros was accepted\n");
        return 1;
    }
    printf("tinyactors %d.%d.%d: %.*s round-trips, a bad image is refused\n", TA_VERSION_MAJOR,
           TA_VERSION_MINOR, TA_VERSION_PATCH, (int)length, buffer);
    return 0;
}
