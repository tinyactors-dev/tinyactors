/* runtime.c — hosted conveniences (libc/OS). No SCXML semantics here. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <tinyactors/runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "system/system_internal.h" /* region config for relocation */

/* ---- allocator ---------------------------------------------------------------------- */

static void *rt_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align; /* malloc alignment (>= 8) covers every library request */
    return malloc(size ? size : 1);
}
static void rt_free(void *context, void *p, size_t size) {
    (void)context;
    (void)size;
    free(p);
}

ta_allocator ta_runtime_malloc(void) {
    ta_allocator allocator = {NULL, rt_alloc, rt_free};
    return allocator;
}

/* ---- files -------------------------------------------------------------------------- */

ta_status ta_runtime_load_file(const char *path, void **bytes, size_t *length) {
    *bytes = NULL;
    *length = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return TA_ERROR_INVALID;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return TA_ERROR_INVALID;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return TA_ERROR_INVALID;
    }
    char *buffer = (char *)malloc((size_t)n + 1); /* malloc: >= 8-aligned */
    if (!buffer) {
        fclose(f);
        return TA_ERROR_NO_MEMORY;
    }
    size_t got = fread(buffer, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buffer);
        return TA_ERROR_INVALID;
    }
    buffer[n] = 0; /* convenient for text files; not counted in length */
    *bytes = buffer;
    *length = (size_t)n;
    return TA_OK;
}

void ta_runtime_free_file(void *bytes, size_t length) {
    (void)length;
    free(bytes);
}

struct ta_runtime_files {
    char **texts;
    char **names;
    size_t count, capacity;
};

ta_runtime_files *ta_runtime_files_new(void) {
    return (ta_runtime_files *)calloc(1, sizeof(ta_runtime_files));
}

void ta_runtime_files_free(ta_runtime_files *files) {
    if (!files) return;
    for (size_t i = 0; i < files->count; i++) {
        free(files->texts[i]);
        free(files->names[i]);
    }
    free(files->texts);
    free(files->names);
    free(files);
}

ta_status ta_runtime_resolve(void *context, ta_string_view base, ta_string_view source,
                             ta_source *out) {
    ta_runtime_files *files = (ta_runtime_files *)context;
    /* accept file: URIs */
    if (source.length > 7 && memcmp(source.pointer, "file://", 7) == 0) {
        source.pointer += 7;
        source.length -= 7;
    } else if (source.length > 5 && memcmp(source.pointer, "file:", 5) == 0) {
        source.pointer += 5;
        source.length -= 5;
    }
    /* directory of the referring file + src (absolute src used as is) */
    size_t dir = 0;
    for (size_t i = 0; i < base.length; i++)
        if (base.pointer[i] == '/' || base.pointer[i] == '\\') dir = i + 1;
    int absolute = source.length &&
                   (source.pointer[0] == '/' || (source.length > 1 && source.pointer[1] == ':'));
    if (absolute) dir = 0;
    char *path = (char *)malloc(dir + source.length + 1);
    if (!path) return TA_ERROR_NO_MEMORY;
    if (dir) memcpy(path, base.pointer, dir);
    memcpy(path + dir, source.pointer, source.length);
    path[dir + source.length] = 0;
    void *bytes;
    size_t length;
    ta_status status = ta_runtime_load_file(path, &bytes, &length);
    if (status != TA_OK) {
        free(path);
        return status;
    }
    if (files->count == files->capacity) {
        size_t nc = files->capacity ? files->capacity * 2 : 8;
        char **nt = (char **)realloc(files->texts, nc * sizeof(char *));
        if (nt) files->texts = nt;
        char **nn = (char **)realloc(files->names, nc * sizeof(char *));
        if (nn) files->names = nn;
        if (!nt || !nn) {
            free(bytes);
            free(path);
            return TA_ERROR_NO_MEMORY;
        }
        files->capacity = nc;
    }
    files->texts[files->count] = (char *)bytes;
    files->names[files->count] = path;
    files->count++;
    out->name.pointer = path;
    out->name.length = (uint32_t)strlen(path);
    out->text.pointer = (const char *)bytes;
    out->text.length = (uint32_t)length;
    return TA_OK;
}

ta_status ta_runtime_compile_file(const char *path, int manifest, ta_compile_result **out) {
    *out = NULL;
    ta_runtime_files *files = ta_runtime_files_new();
    if (!files) return TA_ERROR_NO_MEMORY;
    void *text;
    size_t length;
    ta_status status = ta_runtime_load_file(path, &text, &length);
    if (status != TA_OK) {
        ta_runtime_files_free(files);
        return status;
    }
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = ta_runtime_malloc();
    options.resolve = ta_runtime_resolve;
    options.resolve_context = files;
    ta_source source;
    source.name.pointer = path;
    source.name.length = (uint32_t)strlen(path);
    source.text.pointer = (const char *)text;
    source.text.length = (uint32_t)length;
    status = manifest ? ta_compile_system(&options, &source, out)
                      : ta_compile_charts(&options, &source, 1, out);
    ta_runtime_free_file(text, length);
    ta_runtime_files_free(files);
    return status;
}

/* ---- clock --------------------------------------------------------------------------- */

ta_time ta_runtime_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (ta_time)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ta_time)ts.tv_sec * 1000000000ull + (ta_time)ts.tv_nsec;
#endif
}

/* ---- growable system ------------------------------------------------------------------ */

#define RUNTIME_MAGIC 0x54524154u /* "TART" */

typedef struct region_header {
    uint32_t magic, pad;
    size_t total;
    void *self; /* malloc'd block */
} region_header;

#define REGION_HEADER_SIZE ((sizeof(region_header) + 15u) & ~(size_t)15u)

static region_header *header_of(ta_system *system) {
    return (region_header *)(void *)((char *)system - REGION_HEADER_SIZE);
}

static ta_status rt_alloc_region(const ta_image *image, const ta_system_config *config,
                                 void **block, void **region, size_t *length) {
    size_t need;
    ta_status status = ta_system_layout(image, config, &need);
    if (status != TA_OK) return status;
    char *b = (char *)malloc(REGION_HEADER_SIZE + need);
    if (!b) return TA_ERROR_NO_MEMORY;
    region_header *h = (region_header *)(void *)b;
    h->magic = RUNTIME_MAGIC;
    h->total = REGION_HEADER_SIZE + need;
    h->self = b;
    *block = b;
    *region = b + REGION_HEADER_SIZE;
    *length = need;
    return TA_OK;
}

ta_status ta_runtime_system_new(const ta_image *image, const ta_system_config *config,
                                const ta_datamodel_binding *datamodels, uint32_t datamodel_count,
                                ta_system **out) {
    void *block, *region;
    size_t length;
    *out = NULL;
    ta_status status = rt_alloc_region(image, config, &block, &region, &length);
    if (status != TA_OK) return status;
    status = ta_system_init(region, length, image, config, datamodels, datamodel_count, out);
    if (status != TA_OK || (void *)*out != region) { /* malloc is 16-aligned: system == region */
        if (*out) ta_system_deinit(*out);
        free(block);
        *out = NULL;
        return status != TA_OK ? status : TA_ERROR_INVALID;
    }
    return TA_OK;
}

/* Double *v, saturating at `max`; 0 when it cannot grow any more. */
static int dbl(uint32_t *v, uint32_t max) {
    uint32_t n = *v >= max / 2 ? max : *v * 2;
    if (n <= *v) return 0;
    *v = n;
    return 1;
}

static int half_full(uint32_t used, uint32_t capacity) {
    return used >= capacity / 2;
}

/* Grow the exhausted pool (or, for TA_POOL_NONE, every pool that is at least
 * half full — all of them if none is). Fails when nothing can grow: the hard
 * per-system limits (TA_SYSTEM_MAX_*) are reached or memory is out. */
static int grow(ta_system **system_slot, uint32_t pool) {
    ta_system *system = *system_slot;
    ta_system_config config = system->config;
    int ok = 0;
    switch (pool) {
    case TA_POOL_EVENTS:
        ok = dbl(&config.max_events, TA_SYSTEM_MAX_POOL);
        break;
    case TA_POOL_TIMERS:
        ok = dbl(&config.max_timers, TA_SYSTEM_MAX_POOL);
        break;
    case TA_POOL_ACTORS:
        ok = dbl(&config.max_actors, TA_SYSTEM_MAX_ACTORS);
        break;
    case TA_POOL_REQUESTS:
        ok = dbl(&config.max_requests, TA_EXTERNAL_INVOKE_BIT - 2);
        break;
    case TA_POOL_SLAB:
        ok = dbl(&config.string_bytes, TA_SLAB_MAX_BYTES) |
             dbl(&config.instance_bytes, TA_SLAB_MAX_BYTES);
        break;
    default: {
        ta_system_stats stats;
        ta_system_stats_get(system, &stats);
        int a = half_full(stats.actors, stats.actors_capacity),
            e = half_full(stats.events, stats.events_capacity),
            t = half_full(stats.timers, stats.timers_capacity),
            sl = half_full(stats.slab_pages, stats.slab_pages_capacity);
        int any = a | e | t | sl;
        if (a || !any) ok |= dbl(&config.max_actors, TA_SYSTEM_MAX_ACTORS);
        if (e || !any) ok |= dbl(&config.max_events, TA_SYSTEM_MAX_POOL);
        if (t || !any) ok |= dbl(&config.max_timers, TA_SYSTEM_MAX_POOL);
        if (sl || a || !any) {
            ok |= dbl(&config.string_bytes, TA_SLAB_MAX_BYTES);
            ok |= dbl(&config.instance_bytes, TA_SLAB_MAX_BYTES);
        }
        break;
    }
    }
    if (!ok) return 0;
    void *block, *region;
    size_t length;
    if (rt_alloc_region(system->core.image, &config, &block, &region, &length) != TA_OK) return 0;
    ta_system *new_system;
    if (ta_system_relocate(system, region, length, &config, &new_system) != TA_OK ||
        (void *)new_system != region) {
        free(block);
        return 0;
    }
    free(header_of(system)->self);
    *system_slot = new_system;
    return 1;
}

ta_status ta_runtime_system_grow(ta_system **system) {
    return grow(system, TA_POOL_NONE) ? TA_OK : TA_ERROR_NO_MEMORY;
}

ta_run ta_runtime_run(ta_system **system, uint32_t budget) {
    ta_run total;
    memset(&total, 0, sizeof total);
    for (;;) {
        ta_run result = ta_system_run(*system, budget > total.used ? budget - total.used : 1);
        total.used += result.used;
        total.status = result.status;
        total.effect_count = result.effect_count;
        total.next_deadline = result.next_deadline;
        total.detail = result.detail;
        if (result.status != TA_RUN_EXHAUSTED || !grow(system, result.detail)) return total;
        if (total.used >= budget) {
            total.status = TA_RUN_YIELDED;
            return total;
        }
    }
}

void ta_runtime_system_free(ta_system *system) {
    if (!system) return;
    region_header *h = header_of(system);
    ta_system_deinit(system);
    if (h->magic == RUNTIME_MAGIC) free(h->self);
}
