/* ta-layout: what ta_system_layout carves for a segment's region.
 *
 *   ta-layout
 *       What one more of each capacity adds, for a one-state chart: for each
 *       capacity it lays out a segment at 2^16 and at 2^17 of it, everything
 *       else at its default, and prints the difference per unit:
 *       "<capacity> <bytes>" (an event also without tracing). `mise run
 *       layout` compares these with what it computes from the types' sizes
 *       in the debug information, so the two cannot drift apart unseen.
 *
 *   ta-layout FILE [NAME=VALUE ...]
 *       The region of a segment of FILE's image with the capacities given,
 *       named as the host config's "system" map names them (maxActors,
 *       maxEvents, maxTimers, workspaces, maxRequests, outbox,
 *       instanceBytes, stringBytes, traceBytes, traceContexts, traceRules,
 *       a count each, and tracing=true|false), every other at its default:
 *       "region <bytes>", what a host with that config reports as the
 *       segment's regionBytes. FILE is a compiled image (.tai), or an SCXML
 *       document (.scxml), compiled first. A program that measures a
 *       population runs it at N and at 0 of a thing and divides the
 *       difference by N.
 *
 * It calls only ta_compile_charts, ta_image_open and ta_system_layout, so a
 * binding builds it from this file against core's amalgamation. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* core's headers; built against core's amalgamation (a binding's build),
 * the amalgamation's two */
#if __has_include("tinyactors_runtime.h")
#include "tinyactors.h"
#include "tinyactors_runtime.h"
#else
#include <tinyactors/compile.h>
#include <tinyactors/image.h>
#include <tinyactors/system.h>
#endif

static const char chart[] = "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' "
                            "name='idle' initial='s'><state id='s'/></scxml>";

enum { SMALL = 1u << 16 };

typedef enum capacity { ACTORS, EVENTS, UNTRACED_EVENTS, TIMERS, REQUESTS, OUTBOX, CAPACITIES } capacity;

static const char *const names[CAPACITIES] = {"actor",   "event",   "untraced-event",
                                              "timer",   "request", "outbox"};

/* The region of a segment of `image` with `count` of `which`. The slabs are
 * fixed, so an actor's strings and instance stay out of its count. */
static size_t region(const ta_image *image, capacity which, uint32_t count) {
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.instance_bytes = 4096;
    config.string_bytes = 16384;
    if (which == UNTRACED_EVENTS) config.tracing = TA_TRACING_DISABLED;
    uint32_t *fields[CAPACITIES] = {&config.max_actors,   &config.max_events, &config.max_events,
                                    &config.max_timers,   &config.max_requests, &config.outbox};
    *fields[which] = count;
    size_t bytes = 0;
    return ta_system_layout(image, &config, &bytes) == TA_OK ? bytes : 0;
}

/* A compiled source: its result, kept while its image is read. */
static ta_compile_result *compile(ta_source source) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_compile_result *result = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &result) == TA_OK) return result;
    const ta_diagnostic *diagnostics = NULL;
    uint32_t count = result ? ta_compile_diagnostics(result, &diagnostics) : 0;
    for (uint32_t i = 0; i < count; i++)
        fprintf(stderr, "%.*s:%u:%u: %.*s\n", (int)diagnostics[i].file.length,
                diagnostics[i].file.pointer, diagnostics[i].line, diagnostics[i].column,
                (int)diagnostics[i].message.length, diagnostics[i].message.pointer);
    fprintf(stderr, "ta-layout: %.*s did not compile\n", (int)source.name.length, source.name.pointer);
    ta_compile_free(result);
    return NULL;
}

static int check(void) {
    ta_compile_result *result =
        compile((ta_source){{"idle.scxml", 10}, {chart, sizeof chart - 1}});
    if (!result) return 1;
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

/* ---- the region of a given image and capacities -------------------------------------------- */

/* A capacity the host config's "system" map names, and where it goes. */
typedef struct setting {
    const char *name;
    size_t offset; /* of its uint32_t in ta_system_config */
} setting;

static const setting settings[] = {
    {"maxActors", offsetof(ta_system_config, max_actors)},
    {"maxEvents", offsetof(ta_system_config, max_events)},
    {"maxTimers", offsetof(ta_system_config, max_timers)},
    {"workspaces", offsetof(ta_system_config, workspaces)},
    {"maxRequests", offsetof(ta_system_config, max_requests)},
    {"outbox", offsetof(ta_system_config, outbox)},
    {"instanceBytes", offsetof(ta_system_config, instance_bytes)},
    {"stringBytes", offsetof(ta_system_config, string_bytes)},
    {"traceBytes", offsetof(ta_system_config, trace_bytes)},
    {"traceContexts", offsetof(ta_system_config, trace_contexts)},
    {"traceRules", offsetof(ta_system_config, trace_rules)},
};

/* `argument` ("NAME=VALUE") into `config`; 0 when it names nothing or its
 * value is not a count of 32 bits (or, for tracing, true or false). */
static int setting_read(const char *argument, ta_system_config *config) {
    const char *equals = strchr(argument, '=');
    if (!equals) return 0;
    size_t length = (size_t)(equals - argument);
    const char *value = equals + 1;
    if (length == 7 && memcmp(argument, "tracing", 7) == 0) {
        if (strcmp(value, "true") == 0) config->tracing = TA_TRACING_ENABLED;
        else if (strcmp(value, "false") == 0) config->tracing = TA_TRACING_DISABLED;
        else return 0;
        return 1;
    }
    for (size_t i = 0; i < sizeof settings / sizeof settings[0]; i++) {
        if (strlen(settings[i].name) != length || memcmp(argument, settings[i].name, length) != 0)
            continue;
        char *end = NULL;
        unsigned long long count = strtoull(value, &end, 10);
        if (!*value || *end || *value == '-' || count > UINT32_MAX) return 0;
        uint32_t field = (uint32_t)count;
        memcpy((unsigned char *)config + settings[i].offset, &field, sizeof field);
        return 1;
    }
    return 0;
}

/* The whole file at `path`, in a block of its own (*out, freed by the
 * caller); 0 when it cannot be read. */
static int file_read(const char *path, ta_bytes *out) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    unsigned char *bytes = length >= 0 && fseek(file, 0, SEEK_SET) == 0
                               ? (unsigned char *)malloc((size_t)length + 1)
                               : NULL;
    int read = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!read) {
        free(bytes);
        return 0;
    }
    *out = (ta_bytes){bytes, (size_t)length};
    return 1;
}

static int ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text), suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(text + length - suffix_length, suffix) == 0;
}

static int measure(int count, char **arguments) {
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    for (int i = 1; i < count; i++)
        if (!setting_read(arguments[i], &config)) {
            fprintf(stderr, "ta-layout: %s: not a capacity of the host config's system map\n",
                    arguments[i]);
            return 2;
        }
    ta_bytes file;
    if (!file_read(arguments[0], &file)) {
        fprintf(stderr, "ta-layout: %s: cannot be read\n", arguments[0]);
        return 1;
    }
    ta_compile_result *result = NULL;
    ta_bytes bytes = file;
    if (ends_with(arguments[0], ".scxml")) {
        size_t name = strlen(arguments[0]);
        result = compile((ta_source){{arguments[0], (uint32_t)name},
                                     {(const char *)file.pointer, (uint32_t)file.length}});
        if (!result) {
            free((void *)file.pointer);
            return 1;
        }
        ta_string_view image = ta_compile_image(result);
        bytes = (ta_bytes){image.pointer, image.length};
    }
    ta_image image;
    size_t region_bytes = 0;
    int status = 0;
    if (ta_image_open(bytes, &image) != TA_OK) {
        fprintf(stderr, "ta-layout: %s: not an image\n", arguments[0]);
        status = 1;
    } else if (ta_system_layout(&image, &config, &region_bytes) != TA_OK) {
        fprintf(stderr, "ta-layout: %s: ta_system_layout refused it\n", arguments[0]);
        status = 1;
    } else {
        printf("region %zu\n", region_bytes);
    }
    ta_compile_free(result);
    free((void *)file.pointer);
    return status;
}

int main(int count, char **arguments) {
    return count > 1 ? measure(count - 1, arguments + 1) : check();
}
