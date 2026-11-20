/* ta-layout: what one more of each capacity adds to a segment's region, as
 * ta_system_layout carves it. For each capacity it lays out a segment at
 * 2^16 and at 2^17 of it, everything else at its default, and prints the
 * difference per unit: "<capacity> <bytes>". `mise run layout` compares
 * these with what it computes from the types' sizes in the debug
 * information, so the two cannot drift apart unseen. */
#include <stdio.h>
#include <string.h>
#include <tinyactors/compile.h>
#include <tinyactors/image.h>
#include <tinyactors/system.h>

static const char chart[] = "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' "
                            "name='idle' initial='s'><state id='s'/></scxml>";

enum { SMALL = 1u << 16 };

typedef enum capacity { ACTORS, EVENTS, TIMERS, REQUESTS, OUTBOX, CAPACITIES } capacity;

static const char *const names[CAPACITIES] = {"actor", "event", "timer", "request", "outbox"};

/* The region of a segment of `image` with `count` of `which`. The slabs are
 * fixed, so an actor's strings and instance stay out of its count. */
static size_t region(const ta_image *image, capacity which, uint32_t count) {
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.instance_bytes = 4096;
    config.string_bytes = 16384;
    uint32_t *fields[CAPACITIES] = {&config.max_actors, &config.max_events, &config.max_timers,
                                    &config.max_requests, &config.outbox};
    *fields[which] = count;
    size_t bytes = 0;
    return ta_system_layout(image, &config, &bytes) == TA_OK ? bytes : 0;
}

int main(void) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source source = {{"idle.scxml", 10}, {chart, sizeof chart - 1}};
    ta_compile_result *result = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &result) != TA_OK) {
        fputs("ta-layout: the chart did not compile\n", stderr);
        return 1;
    }
    ta_string_view bytes = ta_compile_image(result);
    ta_image image;
    if (ta_image_open((ta_bytes){bytes.pointer, bytes.length}, &image) != TA_OK) {
        fputs("ta-layout: the image did not open\n", stderr);
        ta_compile_free(result);
        return 1;
    }
    int status = 0;
    for (capacity which = ACTORS; which < CAPACITIES; which++) {
        size_t small = region(&image, which, SMALL), large = region(&image, which, 2 * SMALL);
        if (!small || !large || (large - small) % SMALL) {
            fprintf(stderr, "ta-layout: %s: %zu B at %u, %zu B at %u\n", names[which], small, SMALL,
                    large, 2 * SMALL);
            status = 1;
            continue;
        }
        printf("%s %zu\n", names[which], (large - small) / SMALL);
    }
    ta_compile_free(result);
    return status;
}
