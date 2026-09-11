#include "host.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct ta_test_block {
    struct ta_test_block *next;
    void *raw;
    ta_chunk_t chunk;
} ta_test_block_t;

static int power_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static size_t round_up(size_t value, size_t quantum)
{
    size_t remainder;
    if (quantum == 0u) return value;
    remainder = value % quantum;
    if (remainder == 0u) return value;
    if (value > SIZE_MAX - (quantum - remainder)) return 0u;
    return value + (quantum - remainder);
}

static ta_status_t test_acquire(void *user, const ta_layout_t *minimum,
                                ta_chunk_t *out_chunk)
{
    ta_test_allocator_t *allocator = (ta_test_allocator_t *)user;
    ta_test_block_t *block;
    uintptr_t aligned;
    size_t capacity;
    size_t extra;
    if (allocator == NULL || minimum == NULL || out_chunk == NULL ||
        minimum->size == 0u || !power_two(minimum->alignment)) return TA_INVALID;
    allocator->acquire_count++;
    if (allocator->fail_acquire != 0u &&
        allocator->acquire_count == allocator->fail_acquire) {
        allocator->failed_acquires++;
        return TA_NO_MEMORY;
    }
    capacity = round_up(minimum->size, allocator->round_bytes);
    if (capacity == 0u) return TA_NO_MEMORY;
    extra = minimum->alignment - 1u;
    if (capacity > SIZE_MAX - extra || sizeof(*block) > SIZE_MAX - capacity - extra)
        return TA_NO_MEMORY;
    block = (ta_test_block_t *)malloc(sizeof(*block));
    if (block == NULL) return TA_NO_MEMORY;
    block->raw = malloc(capacity + extra);
    if (block->raw == NULL) { free(block); return TA_NO_MEMORY; }
    aligned = ((uintptr_t)block->raw + extra) & ~(uintptr_t)extra;
    block->chunk.data = (void *)aligned;
    block->chunk.layout.size = capacity;
    block->chunk.layout.alignment = minimum->alignment;
    block->next = (ta_test_block_t *)allocator->active;
    allocator->active = block;
    allocator->live_blocks++;
    if (allocator->live_bytes > SIZE_MAX - capacity) {
        allocator->active = block->next;
        allocator->live_blocks--;
        free(block->raw); free(block);
        return TA_NO_MEMORY;
    }
    allocator->live_bytes += capacity;
    if (allocator->live_blocks > allocator->peak_blocks) allocator->peak_blocks = allocator->live_blocks;
    if (allocator->live_bytes > allocator->peak_bytes) allocator->peak_bytes = allocator->live_bytes;
    *out_chunk = block->chunk;
    return TA_OK;
}

static ta_test_block_t *find_block(ta_test_allocator_t *allocator,
                                   const ta_chunk_t *chunk,
                                   ta_test_block_t **out_previous)
{
    ta_test_block_t *previous = NULL;
    ta_test_block_t *block = (ta_test_block_t *)allocator->active;
    while (block != NULL) {
        if (block->chunk.data == chunk->data) {
            if (out_previous != NULL) *out_previous = previous;
            return block;
        }
        previous = block;
        block = block->next;
    }
    return NULL;
}

static void test_release(void *user, const ta_chunk_t *chunk)
{
    ta_test_allocator_t *allocator = (ta_test_allocator_t *)user;
    ta_test_block_t *previous;
    ta_test_block_t *block;
    ta_test_release_t *grown;
    if (allocator == NULL || chunk == NULL) return;
    block = find_block(allocator, chunk, &previous);
    if (block == NULL || block->chunk.layout.size != chunk->layout.size ||
        block->chunk.layout.alignment != chunk->layout.alignment) {
        allocator->bad_releases++;
        return;
    }
    if (allocator->release_count == allocator->release_capacity) {
        size_t capacity = allocator->release_capacity == 0u ? 8u : allocator->release_capacity;
        if (capacity <= SIZE_MAX / 2u) capacity *= 2u;
        else capacity = 0u;
        if (capacity != 0u && capacity <= SIZE_MAX / sizeof(*grown)) {
            grown = (ta_test_release_t *)realloc(allocator->releases, capacity * sizeof(*grown));
            if (grown != NULL) { allocator->releases = grown; allocator->release_capacity = capacity; }
        }
    }
    if (allocator->release_count < allocator->release_capacity) {
        allocator->releases[allocator->release_count].chunk = *chunk;
        allocator->releases[allocator->release_count].exact = 1;
        allocator->release_count++;
    }
    if (previous == NULL) allocator->active = block->next;
    else previous->next = block->next;
    allocator->live_blocks--;
    allocator->live_bytes -= block->chunk.layout.size;
    free(block->raw);
    free(block);
}

void ta_test_allocator_init(ta_test_allocator_t *allocator, size_t round_bytes,
                            size_t fail_acquire)
{
    if (allocator == NULL) return;
    memset(allocator, 0, sizeof(*allocator));
    allocator->round_bytes = round_bytes;
    allocator->fail_acquire = fail_acquire;
}

void ta_test_allocator_reset(ta_test_allocator_t *allocator)
{
    ta_test_block_t *block;
    ta_test_block_t *next;
    if (allocator == NULL) return;
    block = (ta_test_block_t *)allocator->active;
    while (block != NULL) { next = block->next; free(block->raw); free(block); block = next; }
    free(allocator->releases);
    memset(allocator, 0, sizeof(*allocator));
}

ta_chunk_allocator_t ta_test_allocator_callbacks(ta_test_allocator_t *allocator)
{
    ta_chunk_allocator_t callbacks;
    callbacks.user = allocator;
    callbacks.acquire = test_acquire;
    callbacks.release = test_release;
    return callbacks;
}

int ta_test_allocator_releases_ok(const ta_test_allocator_t *allocator)
{
    return allocator != NULL && allocator->live_blocks == 0u && allocator->live_bytes == 0u &&
           allocator->bad_releases == 0u;
}

static int append_entry(ta_test_trace_t *trace, const ta_test_trace_entry_t *entry)
{
    ta_test_trace_entry_t *grown;
    size_t capacity;
    if (trace->count == trace->capacity) {
        capacity = trace->capacity == 0u ? 16u : trace->capacity * 2u;
        grown = (ta_test_trace_entry_t *)realloc(trace->entries, capacity * sizeof(*grown));
        if (grown == NULL) { trace->dropped++; return 0; }
        trace->entries = grown; trace->capacity = capacity;
    }
    trace->entries[trace->count++] = *entry;
    return 1;
}

static int copy_entry_text(ta_test_trace_entry_t *entry)
{
    if (entry->text.data == NULL || entry->text.size == 0u) return 1;
    entry->owned_text = (char *)malloc(entry->text.size);
    if (entry->owned_text == NULL) return 0;
    memcpy(entry->owned_text, entry->text.data, entry->text.size);
    entry->text.data = entry->owned_text;
    return 1;
}

void ta_test_trace_init(ta_test_trace_t *trace)
{ if (trace != NULL) memset(trace, 0, sizeof(*trace)); }

void ta_test_trace_reset(ta_test_trace_t *trace)
{
    size_t index;
    if (trace == NULL) return;
    for (index = 0u; index < trace->count; ++index) free(trace->entries[index].owned_text);
    free(trace->entries); memset(trace, 0, sizeof(*trace));
}

int ta_test_trace_event(ta_test_trace_t *trace, ta_actor_ref_t actor,
                        const ta_event_t *event)
{
    ta_test_trace_entry_t entry;
    if (trace == NULL || event == NULL) return 0;
    memset(&entry, 0, sizeof(entry)); entry.kind = TA_TEST_TRACE_EVENT;
    entry.actor = actor; entry.text = event->name;
    if (!copy_entry_text(&entry)) { trace->dropped++; return 0; }
    if (!append_entry(trace, &entry)) { free(entry.owned_text); return 0; }
    return 1;
}

int ta_test_trace_action(ta_test_trace_t *trace, const ta_test_action_t *action)
{
    ta_test_trace_entry_t entry;
    if (trace == NULL || action == NULL) return 0;
    memset(&entry, 0, sizeof(entry)); entry.kind = TA_TEST_TRACE_ACTION;
    entry.actor = action->actor; entry.state = action->state;
    entry.transition = action->transition; entry.action = action->kind;
    entry.code = action->code;
    return append_entry(trace, &entry);
}

int ta_test_trace_terminal(ta_test_trace_t *trace,
                           const ta_actor_termination_t *termination)
{
    ta_test_trace_entry_t entry;
    if (trace == NULL || termination == NULL) return 0;
    memset(&entry, 0, sizeof(entry)); entry.kind = TA_TEST_TRACE_TERMINAL;
    entry.actor = termination->actor; entry.actor_status = termination->status;
    entry.status = termination->reason;
    return append_entry(trace, &entry);
}
