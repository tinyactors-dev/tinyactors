#include "tinyactors.h"

#include <stddef.h>
#include <stdint.h>

typedef struct ta_arena_chunk ta_arena_chunk_t;
typedef struct ta_arena_cleanup ta_arena_cleanup_t;

struct ta_arena_chunk {
    ta_arena_chunk_t *next;
    ta_chunk_t returned;
    size_t bump;
};

struct ta_arena_cleanup {
    ta_arena_cleanup_t *next;
    void (*function)(void *);
    void *argument;
};

struct ta_arena {
    ta_arena_t *parent;
    ta_arena_t *child_head;
    ta_arena_t *child_prev;
    ta_arena_t *child_next;
    ta_arena_chunk_t *chunks;
    ta_arena_cleanup_t *cleanups;
    ta_chunk_allocator_t allocator;
    ta_arena_options_t options;
    size_t reserved;
    size_t used;
    size_t peak;
    size_t chunks_count;
    size_t descendants;
    uint64_t failed;
    unsigned closing;
    size_t request_alignment;
};

struct ta_arena_align_probe { char c; union { void *p; uint64_t u; size_t z; } x; };

static size_t ta_arena_alignment(void)
{
    return offsetof(struct ta_arena_align_probe, x);
}

/* Private runtime query: first chunk bytes consumed before payload bumps. */
size_t tai_arena_control_bytes(void)
{
    size_t alignment = ta_arena_alignment();
    size_t header = sizeof(ta_arena_chunk_t);
    return (header + alignment - 1u) / alignment * alignment + sizeof(ta_arena_t);
}

static int ta_arena_add_overflow(size_t a, size_t b, size_t *out)
{
    if (b > (size_t)-1 - a) return 1;
    *out = a + b;
    return 0;
}

static int ta_arena_align_up(size_t value, size_t alignment, size_t *out)
{
    size_t rem;
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return 1;
    rem = value & (alignment - 1u);
    return ta_arena_add_overflow(value, rem ? alignment - rem : 0u, out);
}

static int ta_arena_valid_options(const ta_arena_options_t *o)
{
    if (o == NULL || o->maximum_chunk_bytes == 0u || o->maximum_bytes == 0u) return 0;
    if (o->initial_chunk_bytes > o->maximum_chunk_bytes) return 0;
    if (o->maximum_allocation == 0u || o->maximum_allocation > o->maximum_chunk_bytes) return 0;
    return 1;
}

static int ta_arena_valid_layout(const ta_layout_t *l)
{
    if (l == NULL || l->size == 0u || l->alignment == 0u) return 0;
    if ((l->alignment & (l->alignment - 1u)) != 0u) return 0;
    if (l->alignment > (size_t)UINTPTR_MAX) return 0;
    return 1;
}

static int ta_arena_ancestor_closing(const ta_arena_t *a)
{
    while (a != NULL) { if (a->closing) return 1; a = a->parent; }
    return 0;
}

static void ta_arena_failure(ta_arena_t *a)
{
    while (a != NULL) {
        if (a->failed != UINT64_MAX) ++a->failed;
        a = a->parent;
    }
}

static int ta_arena_reserve_possible(ta_arena_t *a, size_t amount)
{
    ta_arena_t *p = a;
    while (p != NULL) {
        if (amount > p->options.maximum_bytes - p->reserved) return 0;
        p = p->parent;
    }
    return 1;
}

static void ta_arena_reserve_add(ta_arena_t *a, size_t amount, size_t used)
{
    ta_arena_t *p = a;
    while (p != NULL) {
        p->reserved += amount;
        p->used += used;
        if (p->reserved > p->peak) p->peak = p->reserved;
        ++p->chunks_count;
        p = p->parent;
    }
}

static void ta_arena_release_accounting(ta_arena_t *a, size_t amount, size_t used)
{
    ta_arena_t *p = a;
    while (p != NULL) {
        p->reserved -= amount;
        p->used -= used;
        --p->chunks_count;
        p = p->parent;
    }
}

/* Acquire and link a chunk. The returned descriptor is retained verbatim. */
static ta_status_t ta_arena_chunk(ta_arena_t *a, size_t minimum, ta_arena_chunk_t **out)
{
    ta_layout_t request;
    ta_chunk_t got;
    ta_arena_chunk_t *header;
    size_t ask;
    uintptr_t address;
    if (a == NULL || out == NULL || minimum == 0u) return TA_INVALID;
    if (minimum > a->options.maximum_chunk_bytes) return TA_LIMIT;
    ask = a->options.initial_chunk_bytes;
    if (ask < minimum) ask = minimum;
    if (ask > a->options.maximum_chunk_bytes) ask = a->options.maximum_chunk_bytes;
    request.size = ask;
    request.alignment = a->request_alignment;
    if (request.alignment < ta_arena_alignment()) request.alignment = ta_arena_alignment();
    got.data = NULL; got.layout.size = 0u; got.layout.alignment = 0u;
    if (!ta_arena_reserve_possible(a, ask)) return TA_LIMIT;
    {
        ta_status_t acquire_status = a->allocator.acquire(a->allocator.user, &request, &got);
        if (acquire_status != TA_OK) return acquire_status;
    }
    address = (uintptr_t)got.data;
    if (got.data == NULL || got.layout.size < ask || got.layout.size < minimum || got.layout.size > a->options.maximum_chunk_bytes ||
        got.layout.alignment == 0u || (got.layout.alignment & (got.layout.alignment - 1u)) != 0u ||
        got.layout.alignment < request.alignment ||
        (address & (request.alignment - 1u)) != 0u) {
        a->allocator.release(a->allocator.user, &got);
        return got.layout.size > a->options.maximum_chunk_bytes ? TA_LIMIT : TA_NO_MEMORY;
    }
    if (!ta_arena_reserve_possible(a, got.layout.size)) {
        a->allocator.release(a->allocator.user, &got);
        return TA_LIMIT;
    }
    header = (ta_arena_chunk_t *)got.data;
    header->returned = got;
    header->bump = sizeof(*header);
    header->next = a->chunks;
    a->chunks = header;
    ta_arena_reserve_add(a, got.layout.size, sizeof(*header));
    *out = header;
    return TA_OK;
}

static void *ta_arena_chunk_pointer(ta_arena_chunk_t *chunk, size_t offset)
{
    return (void *)((uint8_t *)chunk->returned.data + offset);
}

static ta_status_t ta_arena_control(ta_arena_t *parent, const ta_arena_options_t *options,
                                     ta_arena_t **out)
{
    ta_arena_t *a;
    ta_arena_chunk_t *chunk;
    size_t offset, need;
    ta_status_t status;
    ta_arena_t policy = {0};
    if (ta_arena_add_overflow(sizeof(ta_arena_chunk_t), ta_arena_alignment() - 1u, &offset) ||
        ta_arena_add_overflow(offset, sizeof(ta_arena_t), &need)) return TA_LIMIT;
    if (need > options->maximum_bytes) return TA_LIMIT;
    policy = *parent;
    policy.options = *options;
    policy.parent = parent;
    policy.chunks = NULL;
    policy.reserved = 0u;
    policy.used = 0u;
    policy.request_alignment = ta_arena_alignment();
    status = ta_arena_chunk(&policy, need, &chunk);
    if (status != TA_OK) return status;
    /* The acquisition is charged to the parent subtree, but the chunk belongs
       to the new arena. It is always the newly acquired head of parent's list. */
    if (policy.chunks == chunk) policy.chunks = chunk->next;
    chunk->next = NULL;
    if (ta_arena_align_up(sizeof(*chunk), ta_arena_alignment(), &offset) ||
        ta_arena_add_overflow(offset, sizeof(ta_arena_t), &need) || need > chunk->returned.layout.size ||
        chunk->returned.layout.size > options->maximum_bytes) {
        ta_arena_release_accounting(&policy, chunk->returned.layout.size, chunk->bump);
        parent->allocator.release(parent->allocator.user, &chunk->returned);
        return TA_NO_MEMORY;
    }
    a = (ta_arena_t *)ta_arena_chunk_pointer(chunk, offset);
    a->parent = parent; a->child_head = NULL; a->child_prev = NULL; a->child_next = NULL;
    a->chunks = chunk; a->cleanups = NULL; a->allocator = parent->allocator; a->options = *options;
    a->reserved = chunk->returned.layout.size; a->used = chunk->bump = need;
    a->peak = a->reserved; a->chunks_count = 1u; a->descendants = 0u; a->failed = 0u; a->closing = 0u;
    a->request_alignment = ta_arena_alignment();
    /* ta_arena_chunk accounted through parent, but its local counters begin here. */
    if (parent->parent == NULL && parent->chunks == chunk) { /* no-op; documents root-independent path */ }
    if (parent != NULL) {
        ta_arena_t *p = parent;
        ta_arena_t *old_head = parent->child_head;
        size_t delta = need - sizeof(*chunk);
        while (p != NULL) { ++p->descendants; p = p->parent; }
        p = parent;
        while (p != NULL) { p->used += delta; p = p->parent; }
        a->parent->child_head = a; a->child_next = old_head;
        if (a->child_next != NULL) a->child_next->child_prev = a;
    }
    *out = a;
    return TA_OK;
}

ta_status_t ta_arena_create(const ta_chunk_allocator_t *allocator, const ta_arena_options_t *options,
                            ta_arena_t **out_arena)
{
    ta_arena_t fake;
    ta_arena_t *a;
    ta_arena_options_t o;
    ta_status_t status;
    if (out_arena == NULL) return TA_INVALID;
    *out_arena = NULL;
    if (allocator == NULL || allocator->acquire == NULL || allocator->release == NULL ||
        !ta_arena_valid_options(options)) return TA_INVALID;
    o = *options;
    fake = (ta_arena_t){0};
    fake.parent = NULL; fake.chunks = NULL; fake.allocator = *allocator; fake.options = o;
    fake.reserved = 0u; fake.used = 0u; fake.peak = 0u; fake.chunks_count = 0u;
    fake.descendants = 0u; fake.failed = 0u; fake.child_head = NULL; fake.cleanups = NULL; fake.request_alignment = ta_arena_alignment();
    status = ta_arena_control(&fake, &o, &a);
    if (status != TA_OK) return status;
    /* The temporary parent is only an acquisition/accounting context. */
    a->parent = NULL; a->allocator = *allocator;
    *out_arena = a;
    return TA_OK;
}

ta_status_t ta_arena_open(ta_arena_t *parent, const ta_arena_options_t *options, ta_arena_t **out_child)
{
    ta_status_t status;
    if (out_child == NULL) return TA_INVALID;
    *out_child = NULL;
    if (parent == NULL || ta_arena_ancestor_closing(parent) || !ta_arena_valid_options(options)) return TA_INVALID;
    status = ta_arena_control(parent, options, out_child);
    if (status != TA_OK) ta_arena_failure(parent);
    return status;
}

ta_status_t ta_arena_alloc(ta_arena_t *a, const ta_layout_t *layout, void **out_memory)
{
    ta_arena_chunk_t *c;
    size_t offset, end, increment, minimum, aligned_header;
    uintptr_t address, base;
    if (out_memory == NULL) return TA_INVALID;
    *out_memory = NULL;
    if (a == NULL || ta_arena_ancestor_closing(a) || !ta_arena_valid_layout(layout)) { ta_arena_failure(a); return TA_INVALID; }
    c = a->chunks;
    base = c != NULL ? (uintptr_t)c->returned.data : 0u;
    if (c != NULL && base <= (uintptr_t)-1 - c->bump &&
        !ta_arena_align_up(base + c->bump, layout->alignment, &address) && address >= base &&
        (offset = (size_t)(address - base),
        !ta_arena_add_overflow(offset, layout->size, &end))) {
        increment = end - c->bump;
        if (increment <= a->options.maximum_allocation && end <= c->returned.layout.size) {
            address = (uintptr_t)ta_arena_chunk_pointer(c, offset);
            if ((address & (layout->alignment - 1u)) == 0u) {
                c->bump = end; a->used += increment;
                { ta_arena_t *p = a->parent; while (p != NULL) { p->used += increment; p = p->parent; } }
                *out_memory = (void *)address; return TA_OK;
            }
        }
    }
    if (ta_arena_align_up(sizeof(ta_arena_chunk_t), layout->alignment, &aligned_header) ||
        ta_arena_add_overflow(aligned_header, layout->size, &minimum) ||
        minimum > a->options.maximum_chunk_bytes || layout->size > a->options.maximum_allocation) {
        ta_arena_failure(a); return TA_LIMIT;
    }
    {
        ta_status_t chunk_status;
        size_t old_alignment = a->request_alignment;
        a->request_alignment = layout->alignment;
        chunk_status = ta_arena_chunk(a, minimum, &c);
        a->request_alignment = old_alignment;
        if (chunk_status != TA_OK) { ta_arena_failure(a); return chunk_status; }
    }
    base = (uintptr_t)c->returned.data;
    if (base > (uintptr_t)-1 - c->bump || ta_arena_align_up(base + c->bump, layout->alignment, &address) ||
        address < base || (offset = (size_t)(address - base), 0) ||
        ta_arena_add_overflow(offset, layout->size, &end) || end > c->returned.layout.size ||
        end - sizeof(ta_arena_chunk_t) > a->options.maximum_allocation) {
        a->chunks = c->next;
        ta_arena_release_accounting(a, c->returned.layout.size, c->bump);
        a->allocator.release(a->allocator.user, &c->returned);
        ta_arena_failure(a); return TA_LIMIT;
    }
    c->bump = end; increment = end - sizeof(ta_arena_chunk_t);
    a->used += increment; { ta_arena_t *p = a->parent; while (p != NULL) { p->used += increment; p = p->parent; } }
    *out_memory = ta_arena_chunk_pointer(c, offset);
    return TA_OK;
}

ta_status_t ta_arena_defer(ta_arena_t *a, void (*cleanup)(void *), void *argument)
{
    ta_arena_cleanup_t *record; ta_layout_t l; void *memory;
    if (a == NULL || a->closing || cleanup == NULL) return TA_INVALID;
    l.size = sizeof(*record); l.alignment = ta_arena_alignment();
    {
        ta_status_t status = ta_arena_alloc(a, &l, &memory);
        if (status != TA_OK) return status;
    }
    record = (ta_arena_cleanup_t *)memory; record->function = cleanup; record->argument = argument;
    record->next = a->cleanups; a->cleanups = record;
    return TA_OK;
}

void ta_arena_stats(const ta_arena_t *a, ta_memory_stats_t *out)
{
    if (out == NULL) return;
    out->reserved_bytes = a ? a->reserved : 0u; out->peak_reserved_bytes = a ? a->peak : 0u;
    out->used_bytes = a ? a->used : 0u; out->chunk_count = a ? a->chunks_count : 0u;
    out->descendant_count = a ? a->descendants : 0u; out->failed_allocations = a ? a->failed : 0u;
}

ta_status_t ta_arena_set_limit(ta_arena_t *a, size_t maximum_bytes)
{
    ta_arena_t *p;
    if (a == NULL || ta_arena_ancestor_closing(a) || maximum_bytes < a->reserved || maximum_bytes == 0u) return TA_INVALID;
    p = a->parent;
    while (p != NULL) { if (maximum_bytes > p->options.maximum_bytes) return TA_LIMIT; p = p->parent; }
    a->options.maximum_bytes = maximum_bytes; return TA_OK;
}

static void ta_arena_unlink(ta_arena_t *a)
{
    if (a->parent == NULL) return;
    if (a->child_prev) a->child_prev->child_next = a->child_next;
    else a->parent->child_head = a->child_next;
    if (a->child_next) a->child_next->child_prev = a->child_prev;
}

static void ta_arena_finish(ta_arena_t *a)
{
    ta_arena_chunk_t *c, *next; ta_arena_cleanup_t *d, *dn; ta_arena_t *p;
    p = a->parent;
    ta_arena_unlink(a);
    d = a->cleanups; while (d != NULL) { dn = d->next; d->function(d->argument); d = dn; }
    c = a->chunks; while (c != NULL) { next = c->next; ta_arena_release_accounting(a, c->returned.layout.size, c->bump); a->allocator.release(a->allocator.user, &c->returned); c = next; }
    while (p != NULL) { --p->descendants; p = p->parent; }
}

void ta_arena_close(ta_arena_t *arena)
{
    ta_arena_t *current, *child, *parent;
    if (arena == NULL || arena->closing) return;
    arena->closing = 1u; current = arena;
    for (;;) {
        child = current->child_head;
        if (child != NULL) { child->closing = 1u; current = child; continue; }
        parent = current->parent; ta_arena_finish(current);
        if (current == arena) break;
        current = parent;
    }
}
