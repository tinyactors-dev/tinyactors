/* host_output.h — what the host writes for its caller (a reply, an envelope,
 * a blob, a telemetry request) is a view of the host's memory, valid until
 * the next call into the host (host.h): tests copy it where they keep it. */
#ifndef TINYACTORS_TESTS_SUPPORT_HOST_OUTPUT_H
#define TINYACTORS_TESTS_SUPPORT_HOST_OUTPUT_H

#include <stdio.h>
#include <string.h>

#include <tinyactors/types.h>

#ifndef TEST_UNUSED
#define TEST_UNUSED __attribute__((unused))
#endif

/* The view copied into `room` (`capacity` bytes); its length, 0 (and a
 * message) when it does not fit. */
TEST_UNUSED static size_t output_copy(ta_bytes view, void *room, size_t capacity) {
    if (view.length > capacity) {
        fprintf(stderr, "  a host output of %zu bytes does not fit in %zu\n", view.length, capacity);
        return 0;
    }
    if (view.length) memcpy(room, view.pointer, view.length);
    return view.length;
}

#endif /* TINYACTORS_TESTS_SUPPORT_HOST_OUTPUT_H */
