/* runtime.c — conveniences over the C library's allocator: malloc as a
 * ta_allocator, and a system that grows into a larger block. No SCXML
 * semantics here. Files and the clock, which need an operating system, are
 * in runtime_hosted.c, built for every target but freestanding wasm
 * (docs/adr/0001-packaging-and-releases.md, decisions 9 and 19). */

#include <tinyactors/runtime.h>

#include <stdlib.h>
#include <string.h>

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

ta_status ta_runtime_system_new(const ta_system_setup *setup, ta_system **out) {
    void *block, *region;
    size_t length;
    *out = NULL;
    ta_status status = rt_alloc_region(setup->image, setup->config, &block, &region, &length);
    if (status != TA_OK) return status;
    ta_system_setup placed = *setup;
    placed.memory = (ta_memory){region, length};
    status = ta_system_init(&placed, out);
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
static int grow_system(ta_system **system_slot, uint32_t pool) {
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
        stats.size = sizeof stats; /* how much of it to fill */
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
        if (config.tracing == TA_TRACING_ENABLED &&
            half_full(stats.trace_contexts, stats.trace_contexts_capacity))
            dbl(&config.trace_contexts, TA_SYSTEM_MAX_POOL);
        break;
    }
    }
    if (!ok) return 0;
    /* every event can hold a context of its own (docs/adr/0006) */
    if (config.tracing == TA_TRACING_ENABLED && config.trace_contexts < config.max_events)
        config.trace_contexts = config.max_events;
    void *block, *region;
    size_t length;
    if (rt_alloc_region(system->core.image, &config, &block, &region, &length) != TA_OK) return 0;
    ta_system *new_system = system;
    if (ta_system_relocate(&new_system, (ta_memory){region, length}, &config) != TA_OK ||
        (void *)new_system != region) {
        free(block);
        return 0;
    }
    free(header_of(system)->self);
    *system_slot = new_system;
    return 1;
}

ta_status ta_runtime_system_grow(ta_system **system) {
    return grow_system(system, TA_POOL_NONE) ? TA_OK : TA_ERROR_NO_MEMORY;
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
        if (result.status != TA_RUN_EXHAUSTED || !grow_system(system, result.detail)) return total;
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
