#include "deadline.h"
#include "../include/tinyactors.h"
#include "actor.h"
#include "envelope.h"
#include "heap.h"
#include "memory.h"
#include "name_area.h"
#include "routing.h"
#include "system.h"
#include "trace.h"

/* Deadline index pages hold 1024 positions, as the pool's pages hold items. */
#define TA_DEADLINE_PAGE_SHIFT 10u

static void *ta_deadline_item(void *items, uint32_t index) {
    return ta_envelope_pool_at((const ta_envelope_pool *)items, index);
}

static ta_heap_items ta_deadline_items(ta_system *system) {
    return (ta_heap_items){
        .item = ta_deadline_item,
        .items = &system->run.pool,
        .due_offset = offsetof(ta_envelope, due),
        .order_offset = offsetof(ta_envelope, source_seq),
        .position_offset = offsetof(ta_envelope, heap_index),
    };
}

static ta_envelope *ta_deadline_at(const ta_system *system, uint32_t index) {
    return ta_envelope_pool_at(&system->run.pool, index);
}

/*
 * The fixed part of a timer's record, its text's first field: what the
 * envelope's own fields do not hold. `origin` is written only for a
 * submitted send, and `family` only for a traced one.
 */
typedef struct {
    ta_event_name_id event;
    ta_atom io_type;
    ta_actor_ref destination;
    ta_invoke_id invoke_id;
    ta_content_id content;
    ta_content_id target_text;
    uint8_t bits;
} ta_deadline_record;

#define TA_DEADLINE_HAS_CONTENT ((uint8_t)0x01u)
#define TA_DEADLINE_HAS_TARGET_TEXT ((uint8_t)0x02u)

/* The packed bytes of a record, at most this many. */
#define TA_DEADLINE_RECORD_BYTES                                                                                  \
    (sizeof(ta_event_name_id) + sizeof(ta_atom) + sizeof(ta_actor_ref) + sizeof(ta_invoke_id) +                   \
     2u * sizeof(ta_content_id) + 1u + sizeof(ta_actor_ref) + sizeof(ta_atom))

typedef struct {
    uint8_t *bytes;
    uint32_t length;
} ta_deadline_packer;

static void ta_deadline_put(ta_deadline_packer *packer, const void *field, size_t size) {
    ta_mem_copy(packer->bytes + packer->length, field, size);
    packer->length += (uint32_t)size;
}

static void ta_deadline_get(ta_deadline_packer *packer, void *field, size_t size) {
    ta_mem_copy(field, packer->bytes + packer->length, size);
    packer->length += (uint32_t)size;
}

static bool ta_deadline_traced(const ta_delayed_send *send) {
    return !ta_trace_context_empty(&send->trace.context);
}

/* Packs what of `send` goes in the text's first field; answers its length. */
static uint32_t ta_deadline_pack(const ta_delayed_send *send, uint8_t *bytes) {
    ta_deadline_record record = {
        .event = send->event,
        .io_type = send->io_type,
        .destination = send->destination,
        .invoke_id = send->invoke_id,
        .content = send->content,
        .target_text = send->target_text,
        .bits = (uint8_t)((send->has_content ? TA_DEADLINE_HAS_CONTENT : 0u) |
                          (send->has_target_text ? TA_DEADLINE_HAS_TARGET_TEXT : 0u)),
    };
    ta_deadline_packer packer = { .bytes = bytes, .length = 0u };
    ta_deadline_put(&packer, &record.event, sizeof record.event);
    ta_deadline_put(&packer, &record.io_type, sizeof record.io_type);
    ta_deadline_put(&packer, &record.destination, sizeof record.destination);
    ta_deadline_put(&packer, &record.invoke_id, sizeof record.invoke_id);
    ta_deadline_put(&packer, &record.content, sizeof record.content);
    ta_deadline_put(&packer, &record.target_text, sizeof record.target_text);
    ta_deadline_put(&packer, &record.bits, sizeof record.bits);
    if (send->submitted) {
        ta_deadline_put(&packer, &send->origin, sizeof send->origin);
    }
    if (ta_deadline_traced(send)) {
        ta_deadline_put(&packer, &send->trace.cause.family, sizeof send->trace.cause.family);
    }
    return packer.length;
}

/* The send stored in timer envelope `index`; its references stay the envelope's. */
static ta_delayed_send ta_deadline_read(const ta_system *system, uint32_t index) {
    const ta_envelope *envelope = ta_deadline_at(system, index);
    ta_delayed_send send = {
        .id = envelope->source_seq,
        .owner = envelope->from,
        .send_id = envelope->send_id,
        .due = envelope->due,
        .target = envelope->content,
        .data = envelope->data,
        .submitted = (envelope->flags & TA_ENVELOPE_FLAG_SUBMITTED) != 0u,
    };
    ta_name_area *names = (ta_name_area *)&system->run.names;
    ta_name_area_reader reader = ta_name_area_reader_of(envelope->text);
    uint32_t length = 0u;
    uint8_t bytes[TA_DEADLINE_RECORD_BYTES];
    if (ta_name_area_next(names, &reader, &length) != TA_OK || length > sizeof bytes) {
        return send;
    }
    uint32_t got = 0u;
    while (got < length) {
        ta_bytes span = ta_name_area_read(names, &reader, length - got);
        ta_mem_copy(bytes + got, span.ptr, span.len);
        got += span.len;
    }
    ta_deadline_record record;
    ta_deadline_packer packer = { .bytes = bytes, .length = 0u };
    ta_deadline_get(&packer, &record.event, sizeof record.event);
    ta_deadline_get(&packer, &record.io_type, sizeof record.io_type);
    ta_deadline_get(&packer, &record.destination, sizeof record.destination);
    ta_deadline_get(&packer, &record.invoke_id, sizeof record.invoke_id);
    ta_deadline_get(&packer, &record.content, sizeof record.content);
    ta_deadline_get(&packer, &record.target_text, sizeof record.target_text);
    ta_deadline_get(&packer, &record.bits, sizeof record.bits);
    send.event = record.event;
    send.io_type = record.io_type;
    send.destination = record.destination;
    send.invoke_id = record.invoke_id;
    send.content = record.content;
    send.target_text = record.target_text;
    send.has_content = (record.bits & TA_DEADLINE_HAS_CONTENT) != 0u;
    send.has_target_text = (record.bits & TA_DEADLINE_HAS_TARGET_TEXT) != 0u;
    if (send.submitted) {
        ta_deadline_get(&packer, &send.origin, sizeof send.origin);
    }
    if ((envelope->flags & TA_ENVELOPE_FLAG_TRACED) != 0u &&
        ta_name_area_trace_parent(names, envelope->text, &send.trace) == TA_OK) {
        ta_deadline_get(&packer, &send.trace.cause.family, sizeof send.trace.cause.family);
    }
    return send;
}

static void ta_deadline_send_release(ta_delayed_send *send) {
    ta_value_release(&send->target);
    ta_value_release(&send->data);
    /* A stored send owns its destination and origin references (ADR 0065). */
    ta_actor_ref_release(&send->destination);
    ta_actor_ref_release(&send->origin);
}

/* Takes envelope `index` out of the index, frees its text and gives it back. */
static void ta_deadline_free(ta_system *system, uint32_t index) {
    ta_heap_items items = ta_deadline_items(system);
    (void)ta_heap_remove(&system->deadline.heap, &items, index);
    ta_envelope *envelope = ta_deadline_at(system, index);
    ta_name_area_free(&system->run.names, &envelope->text);
    envelope->data = (ta_value){ 0 };
    envelope->content = (ta_value){ 0 };
    ta_envelope_pool_give(&system->run.pool, index);
}

/* Releases what envelope `index` holds and frees it. */
static void ta_deadline_drop(ta_system *system, uint32_t index) {
    ta_delayed_send send = ta_deadline_read(system, index);
    ta_deadline_send_release(&send);
    ta_deadline_free(system, index);
}

/* Which sends a removal from an owner list selects. */
typedef struct {
    ta_send_id send_id;
    bool any_send_id;
    bool include_submitted;
    /* Submitted sends that are not selected stay indexed with no owner. */
    bool detach_rest;
} ta_deadline_selection;

/* Removes the selected sends from `owner`'s list and releases them. */
static bool ta_deadline_remove(ta_system *system, ta_deadline_owner *owner, const ta_deadline_selection *selection) {
    bool found = false;
    uint32_t previous = 0u;
    uint32_t current = owner->head;
    while (current != 0u) {
        ta_envelope *envelope = ta_deadline_at(system, current);
        uint32_t next = envelope->owner_next;
        bool submitted = (envelope->flags & TA_ENVELOPE_FLAG_SUBMITTED) != 0u;
        bool selected = (selection->any_send_id || envelope->send_id == selection->send_id) &&
                        (selection->include_submitted || !submitted);
        bool leaves = selected || selection->detach_rest;
        if (leaves) {
            if (previous == 0u) {
                owner->head = next;
            } else {
                ta_deadline_at(system, previous)->owner_next = next;
            }
            envelope->owner_next = 0u;
            owner->count -= 1u;
            if (selected) {
                ta_deadline_drop(system, current);
                found = true;
            }
        } else {
            previous = current;
        }
        current = next;
    }
    return found;
}

/* Takes envelope `index` off the list of the owner `from` names, if any. */
static void ta_deadline_unlink(ta_system *system, uint32_t index) {
    ta_envelope *envelope = ta_deadline_at(system, index);
    ta_actor *actor = envelope->from != 0u ? ta_actor_lookup(system, envelope->from) : NULL;
    if (actor == NULL) {
        return;
    }
    ta_deadline_owner *owner = &actor->timers;
    uint32_t previous = 0u;
    uint32_t current = owner->head;
    while (current != 0u && current != index) {
        previous = current;
        current = ta_deadline_at(system, current)->owner_next;
    }
    if (current == 0u) {
        return;
    }
    if (previous == 0u) {
        owner->head = envelope->owner_next;
    } else {
        ta_deadline_at(system, previous)->owner_next = envelope->owner_next;
    }
    envelope->owner_next = 0u;
    owner->count -= 1u;
}

/* ------------------------------------------------------------------------
 * Sorting a session's sends by (due, order), without recursion
 */

typedef struct {
    void *base;
    size_t size;
    /* Whether element `left` sorts before element `right`. */
    bool (*before)(const void *left, const void *right);
} ta_deadline_sort;

static void *ta_deadline_element(const ta_deadline_sort *sort, uint32_t index) {
    return (uint8_t *)sort->base + (size_t)index * sort->size;
}

static void ta_deadline_swap(const ta_deadline_sort *sort, uint32_t left, uint32_t right) {
    uint8_t *a = ta_deadline_element(sort, left);
    uint8_t *b = ta_deadline_element(sort, right);
    for (size_t byte = 0; byte < sort->size; byte += 1u) {
        uint8_t kept = a[byte];
        a[byte] = b[byte];
        b[byte] = kept;
    }
}

static void ta_deadline_sift(const ta_deadline_sort *sort, uint32_t root, uint32_t count) {
    for (;;) {
        uint32_t largest = root;
        uint32_t left = 2u * root + 1u;
        uint32_t right = left + 1u;
        if (left < count && sort->before(ta_deadline_element(sort, largest), ta_deadline_element(sort, left))) {
            largest = left;
        }
        if (right < count && sort->before(ta_deadline_element(sort, largest), ta_deadline_element(sort, right))) {
            largest = right;
        }
        if (largest == root) {
            return;
        }
        ta_deadline_swap(sort, root, largest);
        root = largest;
    }
}

/* Heapsort: O(k log k), in place. */
static void ta_deadline_sort_run(const ta_deadline_sort *sort, uint32_t count) {
    for (uint32_t root = count / 2u; root > 0u; root -= 1u) {
        ta_deadline_sift(sort, root - 1u, count);
    }
    for (uint32_t end = count; end > 1u; end -= 1u) {
        ta_deadline_swap(sort, 0u, end - 1u);
        ta_deadline_sift(sort, 0u, end - 1u);
    }
}

static bool ta_deadline_send_before(const void *left, const void *right) {
    const ta_delayed_send *a = left;
    const ta_delayed_send *b = right;
    return a->due < b->due || (a->due == b->due && a->id < b->id);
}

/* An envelope index with its key, for sorting a session's views. */
typedef struct {
    ta_time due;
    uint64_t order;
    uint32_t index;
} ta_deadline_key;

static bool ta_deadline_key_before(const void *left, const void *right) {
    const ta_deadline_key *a = left;
    const ta_deadline_key *b = right;
    return a->due < b->due || (a->due == b->due && a->order < b->order);
}

/* ------------------------------------------------------------------------
 * Within core
 */

ta_result ta_deadline_init(ta_deadline *deadline, ta_allocator allocator) {
    ta_deadline fresh = { .next_order = 1u };
    ta_result result =
        ta_heap_init(&fresh.heap, &(ta_heap_params){ .allocator = allocator, .page_shift = TA_DEADLINE_PAGE_SHIFT });
    if (result != TA_OK) {
        return result;
    }
    ta_vec_init(&fresh.views, allocator, TA_VEC_LAYOUT(ta_delayed_send_info));
    result = ta_vec_reserve(&fresh.views, 2u * TA_DEADLINE_VIEWS_MIN);
    if (result != TA_OK) {
        ta_heap_destroy(&fresh.heap);
        return result;
    }
    *deadline = fresh;
    return TA_OK;
}

void ta_deadline_destroy(ta_system *system) {
    ta_heap_items items = ta_deadline_items(system);
    for (uint32_t index = ta_heap_pop(&system->deadline.heap, &items); index != 0u;
         index = ta_heap_pop(&system->deadline.heap, &items)) {
        ta_envelope *envelope = ta_deadline_at(system, index);
        ta_delayed_send send = ta_deadline_read(system, index);
        ta_deadline_send_release(&send);
        ta_name_area_free(&system->run.names, &envelope->text);
        envelope->data = (ta_value){ 0 };
        envelope->content = (ta_value){ 0 };
        ta_envelope_pool_give(&system->run.pool, index);
    }
    ta_heap_destroy(&system->deadline.heap);
    ta_vec_destroy(&system->deadline.views);
}

ta_result ta_deadline_grow(ta_system *system) {
    const ta_index_pool *items = &system->run.pool.items;
    ta_deadline *deadline = &system->deadline;
    /* Room to read the longest owner list seen, keys and infos together;
     * a refusal here only leaves inspection to grow it. */
    uint64_t views = (uint64_t)deadline->most_per_owner * 2u;
    if (views > deadline->views.capacity && views <= UINT32_MAX) {
        (void)ta_vec_reserve(&deadline->views, (uint32_t)views);
    }
    return ta_heap_grow(&deadline->heap, items->used + items->free);
}

ta_result ta_deadline_reserve(ta_system *system, uint32_t count) {
    ta_envelope_pool *pool = &system->run.pool;
    if (ta_quota_available(&pool->quota) < count) {
        return TA_ERR_FULL;
    }
    ta_result result = ta_index_pool_refill(&pool->items, count);
    if (result == TA_OK) {
        result = ta_deadline_grow(system);
    }
    return result == TA_ERR_FULL ? TA_ERR_NOMEM : result;
}

ta_result ta_deadline_schedule(ta_system *system,
                               ta_deadline_owner *owner,
                               const ta_delayed_send *send,
                               ta_record_capacity_resource *short_of) {
    ta_deadline *deadline = &system->deadline;
    if ((uint64_t)ta_heap_length(&deadline->heap) >= ta_heap_capacity(&deadline->heap)) {
        if (short_of != NULL) {
            *short_of = TA_RECORD_CAPACITY_RESOURCE_POOL;
        }
        return TA_ERR_FULL;
    }
    uint32_t index = 0u;
    if (ta_envelope_pool_take(&system->run.pool, &index) != TA_OK) {
        if (short_of != NULL) {
            *short_of = TA_RECORD_CAPACITY_RESOURCE_POOL;
        }
        return TA_ERR_FULL;
    }
    ta_delayed_send stored = *send;
    if (stored.id == 0u) {
        stored.id = deadline->next_order;
    }
    uint8_t bytes[TA_DEADLINE_RECORD_BYTES];
    uint32_t length = ta_deadline_pack(&stored, bytes);
    bool traced = ta_deadline_traced(&stored);
    ta_chunk_writer writer = ta_chunk_writer_new();
    ta_result result = ta_name_area_write(&system->run.names, &(ta_name_area_write_params){
                                                                  .writer = &writer,
                                                                  .text = { .ptr = bytes, .len = length },
                                                              });
    if (result == TA_OK && traced) {
        result = ta_name_area_write_trace_parent(&system->run.names, &(ta_name_area_trace_params){
                                                                         .writer = &writer,
                                                                         .parent = &stored.trace,
                                                                     });
        if (result != TA_OK) {
            ta_name_area_free(&system->run.names, &writer.chain);
        }
    }
    if (result != TA_OK) {
        ta_envelope_pool_give(&system->run.pool, index);
        if (short_of != NULL) {
            *short_of = TA_RECORD_CAPACITY_RESOURCE_NAMES;
        }
        return TA_ERR_FULL;
    }
    ta_envelope *envelope = ta_deadline_at(system, index);
    envelope->state = TA_ENVELOPE_TIMER;
    envelope->due = stored.due;
    envelope->source_seq = stored.id;
    envelope->send_id = stored.send_id;
    envelope->flags = (uint8_t)((stored.submitted ? TA_ENVELOPE_FLAG_SUBMITTED : 0u) |
                                (traced ? TA_ENVELOPE_FLAG_TRACED : 0u));
    envelope->from = stored.owner;
    envelope->text = writer.chain;
    envelope->data = stored.data;
    envelope->content = stored.target;
    ta_heap_items items = ta_deadline_items(system);
    (void)ta_heap_insert(&deadline->heap, &items, index);
    if (owner != NULL) {
        envelope->owner_next = owner->head;
        owner->head = index;
        owner->count += 1u;
        if (owner->count > deadline->most_per_owner) {
            deadline->most_per_owner = owner->count;
        }
    }
    if (stored.id >= deadline->next_order) {
        deadline->next_order = stored.id + 1u;
    }
    return TA_OK;
}

uint32_t ta_deadline_count(const ta_system *system, const ta_deadline_owner *owner) {
    (void)system;
    return owner->count;
}

ta_result ta_deadline_cancel(ta_system *system, ta_deadline_owner *owner, ta_send_id send_id) {
    return ta_deadline_remove(system, owner, &(ta_deadline_selection){ .send_id = send_id }) ? TA_OK
                                                                                            : TA_ERR_NOT_FOUND;
}

ta_result ta_deadline_withdraw(ta_system *system, ta_deadline_owner *owner, ta_send_id send_id) {
    return ta_deadline_remove(system, owner, &(ta_deadline_selection){ .send_id = send_id, .include_submitted = true })
               ? TA_OK
               : TA_ERR_NOT_FOUND;
}

bool ta_deadline_has_submitted(const ta_system *system, const ta_deadline_owner *owner, ta_send_id send_id) {
    for (uint32_t index = owner->head; index != 0u; index = ta_deadline_at(system, index)->owner_next) {
        const ta_envelope *envelope = ta_deadline_at(system, index);
        if (envelope->send_id == send_id && (envelope->flags & TA_ENVELOPE_FLAG_SUBMITTED) != 0u) {
            return true;
        }
    }
    return false;
}

void ta_deadline_cancel_unsubmitted(ta_system *system, ta_deadline_owner *owner) {
    (void)ta_deadline_remove(system, owner, &(ta_deadline_selection){ .any_send_id = true });
}

void ta_deadline_orphan(ta_system *system, ta_deadline_owner *owner) {
    (void)ta_deadline_remove(system, owner, &(ta_deadline_selection){ .any_send_id = true, .detach_rest = true });
}

void ta_deadline_owner_release(ta_system *system, ta_deadline_owner *owner) {
    (void)ta_deadline_remove(system, owner, &(ta_deadline_selection){ .any_send_id = true, .include_submitted = true });
}

uint32_t ta_deadline_copy(const ta_system *system, const ta_deadline_owner *owner, ta_delayed_send *out) {
    uint32_t count = 0u;
    for (uint32_t index = owner->head; index != 0u; index = ta_deadline_at(system, index)->owner_next) {
        if ((ta_deadline_at(system, index)->flags & TA_ENVELOPE_FLAG_SUBMITTED) != 0u) {
            continue;
        }
        if (out != NULL) {
            out[count] = ta_deadline_read(system, index);
        }
        count += 1u;
    }
    if (out != NULL) {
        ta_deadline_sort_run(
            &(ta_deadline_sort){ .base = out, .size = sizeof *out, .before = ta_deadline_send_before }, count);
    }
    return count;
}

ta_result ta_deadline_views(ta_system *system,
                            const ta_deadline_owner *owner,
                            const ta_delayed_send_info **out,
                            uint32_t *count) {
    uint32_t total = ta_deadline_count(system, owner);
    *out = NULL;
    *count = 0u;
    if (total == 0u) {
        return TA_OK;
    }
    /* The keys sort in the first `total` elements, each smaller than an
     * info; the infos go in the next `total`, so no key is overwritten
     * before it is read. */
    _Static_assert(sizeof(ta_deadline_key) <= sizeof(ta_delayed_send_info), "a key fits where an info goes");
    ta_vec *views = &system->deadline.views;
    if (total > UINT32_MAX / 2u) {
        return TA_ERR_NOMEM;
    }
    if (views->capacity < total * 2u) {
        ta_result result = ta_vec_reserve(views, total * 2u);
        if (result != TA_OK) {
            return result;
        }
    }
    ta_deadline_key *keys = views->items;
    uint32_t at = 0u;
    for (uint32_t index = owner->head; index != 0u; index = ta_deadline_at(system, index)->owner_next) {
        const ta_envelope *envelope = ta_deadline_at(system, index);
        keys[at] = (ta_deadline_key){ .due = envelope->due, .order = envelope->source_seq, .index = index };
        at += 1u;
    }
    ta_deadline_sort_run(&(ta_deadline_sort){ .base = keys, .size = sizeof *keys, .before = ta_deadline_key_before },
                         total);
    ta_delayed_send_info *infos = (ta_delayed_send_info *)views->items + total;
    for (uint32_t index = 0u; index < total; index += 1u) {
        ta_delayed_send send = ta_deadline_read(system, keys[index].index);
        infos[index] = (ta_delayed_send_info){
            .send_id = send.send_id,
            .due = send.due,
            .event = send.event,
            .io_type = send.io_type,
            .destination = send.destination,
            .target = send.target,
            .data = send.data,
            .submitted = send.submitted,
            .has_target_text = send.has_target_text,
            .target_text = send.target_text,
            .has_content = send.has_content,
            .content = send.content,
            .trace = send.trace,
        };
    }
    *out = infos;
    *count = total;
    return TA_OK;
}

/* A token's room in the name area: one field as long as a timer's record
 * and its trace field together, so the chunks it frees hold either. */
#define TA_DEADLINE_TOKEN_TEXT_BYTES (TA_DEADLINE_RECORD_BYTES + TA_NAME_AREA_TRACE_FIELD_BYTES)

ta_result ta_deadline_token_take(ta_system *system, uint32_t *out, ta_record_capacity_resource *short_of) {
    uint32_t index = 0u;
    if (ta_envelope_pool_take(&system->run.pool, &index) != TA_OK) {
        if (short_of != NULL) {
            *short_of = TA_RECORD_CAPACITY_RESOURCE_POOL;
        }
        return TA_ERR_FULL;
    }
    uint8_t room[TA_DEADLINE_TOKEN_TEXT_BYTES] = { 0 };
    ta_chunk_writer writer = ta_chunk_writer_new();
    if (ta_name_area_write(&system->run.names, &(ta_name_area_write_params){
                                                    .writer = &writer,
                                                    .text = { .ptr = room, .len = sizeof room },
                                                }) != TA_OK) {
        ta_envelope_pool_give(&system->run.pool, index);
        if (short_of != NULL) {
            *short_of = TA_RECORD_CAPACITY_RESOURCE_NAMES;
        }
        return TA_ERR_FULL;
    }
    ta_envelope *envelope = ta_deadline_at(system, index);
    envelope->state = TA_ENVELOPE_INVOCATION_TOKEN;
    envelope->text = writer.chain;
    *out = index;
    return TA_OK;
}

void ta_deadline_token_release(ta_system *system, uint32_t token) {
    if (token == 0u) {
        return;
    }
    ta_envelope *envelope = ta_deadline_at(system, token);
    ta_name_area_free(&system->run.names, &envelope->text);
    ta_envelope_pool_give(&system->run.pool, token);
}

bool ta_deadline_next_due(const ta_system *system, ta_time *out) {
    uint32_t head = ta_heap_head(&system->deadline.heap);
    if (head == 0u) {
        return false;
    }
    *out = ta_deadline_at(system, head)->due;
    return true;
}

ta_result ta_deadline_pop(ta_system *system, ta_delayed_send *out) {
    uint32_t head = ta_heap_head(&system->deadline.heap);
    if (head == 0u) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_deadline_read(system, head);
    ta_deadline_unlink(system, head);
    ta_deadline_free(system, head);
    return TA_OK;
}
