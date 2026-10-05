/*
 * Implementation of the loads module. The contract (what a load is, the two
 * places mail waits, the request/finish protocol, the sessions' key addresses)
 * is in tinyactors_host/loads.h; this comment records what the code relies on.
 *
 * Purpose. A load is the host kernel's record that an address with no
 * resident actor is being brought in. The kernel owns the record, the
 * requests JavaScript has yet to hear of, and the wake-up heap. Core owns the
 * chart mail that waits for the same address (ADR 0099 section 4); this file
 * only starts a load when core reports LOAD_NEEDED (tah_loads_wanted) and
 * tells core how it ended (tah_loads_finish).
 *
 * Lifetimes. A tah_load is a record-lifetime object: it lives from
 * tah_loads_start to tah_loads_finish (or tah_loads_close) and is then given
 * back to a free list; its generation advances on reuse, so a finish with a
 * stale (index, generation) answers TA_ERR_STALE and touches nothing. The
 * address is retained by the record and by each wake-up heap entry, and
 * released when the record or the entry goes. The load's mailbox holds
 * scheduler envelopes (the host's own posts); they are the scheduler's, and
 * leave through tah_sched_hand_over (to the actor) or tah_sched_bury_waiting
 * (dead letters), never freed here.
 *
 * Allocation. Only through the tah_vec and tah_map containers on the
 * allocator given to tah_loads_init: the record vector, the family list, the
 * address map, the request queue and the wake-up heap. They grow only in
 * tah_loads_set_family, tah_loads_route, tah_loads_wanted and
 * tah_loads_wake_at, the functions that may return TA_ERR_NOMEM; every other
 * function, and tah_loads_finish above all, never allocates, so a load can
 * always end.
 * Frees go through the containers, which know their own sizes.
 *
 * Failure. tah_loads_start reserves the request slot and the map entry
 * before it retains the address or counts the load, so a failure leaves no
 * record and no count behind. tah_loads_route undoes a start whose first
 * message was refused. tah_loads_finish reports the outcome to core with
 * ta_address_loaded or ta_address_load_failed after the kernel's mail has
 * moved, so a handover that ran out of memory still ends the load (as FAILED)
 * and the senders hear error.communication.
 *
 * Order. Host posts for an address wait in the load's mailbox and chart mail
 * waits on core's list. When the load finishes, core's list moves to the run
 * queue when the actor is published (or ta_address_loaded names a gone
 * session's instance), and the kernel's mailbox is handed over afterwards by
 * tah_sched_hand_over. Chart mail for the address therefore precedes the
 * host's posts to it, whatever order they arrived in: each source keeps its
 * own order, and the two are not merged (recorded in core ADR 0099).
 *
 * Complexity. Lookup by address is one map probe, O(1). Starting and
 * finishing a load are O(1) plus the hand-over of its mailbox, O(k) in the
 * posts that waited. The wake-up heap is a binary min-heap: O(log n) to add
 * or pop; replacing or cancelling a wake-up is O(1) and leaves the old entry
 * to be skipped when it surfaces, so the heap can hold stale entries until
 * they come due.
 *
 * Usage (a kernel turn):
 *
 *     tah_loads_wanted(&loads, &(tah_load_wanted){ .address = notice.address, ... });
 *     tah_load_request request;
 *     while (tah_loads_next_request(&loads, &request)) { ...call the loader... }
 *     tah_loads_finish(&loads, &(tah_load_finish){ .index = request.index,
 *                                                  .generation = request.generation,
 *                                                  .outcome = TAH_LOAD_LOADED, ... });
 */

#include "tinyactors_host/loads.h"

#include "memory.h"


#include "tinyactors_host/allocator.h"
#include "allocator_private.h"
#include "loads_private.h"
#include "host_metrics_private.h"

const char *tah_load_outcome_name(tah_load_outcome outcome) {
    static const char *const names[TAH_LOAD_OUTCOMES] = {
        [TAH_LOAD_LOADED] = "loaded",       [TAH_LOAD_SPAWNED] = "spawned", [TAH_LOAD_NOT_FOUND] = "not_found",
        [TAH_LOAD_REJECTED] = "rejected",   [TAH_LOAD_FAILED] = "failed",   [TAH_LOAD_TIMED_OUT] = "timed_out",
        [TAH_LOAD_DONE] = "done",
    };
    return (uint32_t)outcome < TAH_LOAD_OUTCOMES ? names[outcome] : "failed";
}

void tah_loads_init(tah_loads *loads, ta_allocator allocator) {
    *loads = (tah_loads){ .allocator = allocator };
    tah_vec_init(&loads->families, allocator, TAH_LAYOUT(tah_loader_family));
    tah_vec_init(&loads->loads, allocator, TAH_LAYOUT(tah_load));
    tah_map_init(&loads->by_address, allocator);
    tah_vec_init(&loads->requests, allocator, TAH_LAYOUT(uint32_t));
    tah_vec_init(&loads->wakes, allocator, TAH_LAYOUT(tah_wake));
    tah_map_init(&loads->wake_due, allocator);
}

void tah_loads_close(tah_loads *loads) {
    for (uint32_t index = 0; index < loads->loads.count; index += 1u) {
        tah_load *load = tah_vec_at(&loads->loads, index);
        ta_address_release(load->address);
        load->address = NULL;
    }
    for (uint32_t index = 0; index < loads->wakes.count; index += 1u) {
        tah_wake *wake = tah_vec_at(&loads->wakes, index);
        ta_address_release(wake->address);
    }
    for (uint32_t index = 0; index < loads->families.count; index += 1u) {
        tah_loader_family *family = tah_vec_at(&loads->families, index);
        tah_free(loads->allocator, &(ta_allocator_free_params){ .ptr = family->name, .size = family->length + 1u,
                                                                 .alignment = 1 });
    }
    tah_vec_destroy(&loads->families);
    tah_vec_destroy(&loads->loads);
    tah_map_destroy(&loads->by_address);
    tah_vec_destroy(&loads->requests);
    tah_vec_destroy(&loads->wakes);
    tah_map_destroy(&loads->wake_due);
    loads->free_head = 0;
    loads->in_flight = 0;
    loads->requests_head = 0;
    loads->waiting = 0;
}

/* ------------------------------------------------------------------------
 * Families
 */

static tah_loader_family *tah_loads_family_named(const tah_loads *loads, ta_str name, uint32_t *at) {
    for (uint32_t index = 0; index < loads->families.count; index += 1u) {
        tah_loader_family *family = tah_vec_at(&loads->families, index);
        if (family->length == name.len && tah_mem_equal(family->name, name.ptr, name.len)) {
            if (at != NULL) {
                *at = index;
            }
            return family;
        }
    }
    return NULL;
}

ta_result tah_loads_set_family(tah_loads *loads, ta_str name, uint32_t max_waiting) {
    uint32_t at = 0;
    tah_loader_family *family = tah_loads_family_named(loads, name, &at);
    if (family != NULL && max_waiting != 0) {
        family->max_waiting = max_waiting;
        return TA_OK;
    }
    if (family != NULL) {
        /* Loads already in flight keep the limit they started with. */
        tah_free(loads->allocator, &(ta_allocator_free_params){ .ptr = family->name, .size = family->length + 1u,
                                                                 .alignment = 1 });
        tah_loader_family *last = tah_vec_at(&loads->families, loads->families.count - 1u);
        *family = *last;
        loads->families.count -= 1u;
        return TA_OK;
    }
    if (max_waiting == 0) {
        return TA_OK;
    }
    char *copy = tah_alloc(loads->allocator, (size_t)name.len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    tah_mem_copy(copy, name.ptr, name.len);
    copy[name.len] = '\0';
    tah_loader_family fresh = { .name = copy, .length = name.len, .max_waiting = max_waiting };
    if (tah_vec_push(&loads->families, &fresh) != TA_OK) {
        tah_free(loads->allocator, &(ta_allocator_free_params){ .ptr = copy, .size = (size_t)name.len + 1u,
                                                                 .alignment = 1 });
        return TA_ERR_NOMEM;
    }
    return TA_OK;
}

/* The loader limit of the family `address` belongs to, or 0 without a loader. */
static uint32_t tah_loads_limit(const tah_loads *loads, const ta_address *address) {
    if (loads->families.count == 0) {
        return 0;
    }
    const tah_loader_family *family = tah_loads_family_named(loads, ta_address_view(address).family.name, NULL);
    return family == NULL ? 0 : family->max_waiting;
}

/* ------------------------------------------------------------------------
 * Loads
 */

uint32_t tah_loads_pending_requests(const tah_loads *loads) {
    return loads->requests.count - loads->requests_head;
}

tah_load_info tah_loads_info(const tah_loads *loads, uint32_t index, uint32_t generation) {
    const tah_load *load = tah_loads_find(loads, index, generation);
    if (load == NULL) {
        return (tah_load_info){ 0 };
    }
    return (tah_load_info){ .address = load->address, .waiting = load->mail.count, .trace = load->trace };
}

void tah_loads_read_stats(const tah_loads *loads, tah_loads_stats *out) {
    *out = (tah_loads_stats){ .in_flight = loads->in_flight, .waiting = loads->waiting };
    for (uint32_t outcome = 0; outcome < TAH_LOAD_OUTCOMES; outcome += 1u) {
        out->outcomes[outcome] = loads->outcomes[outcome];
    }
}

tah_load *tah_loads_find(const tah_loads *loads, uint32_t index, uint32_t generation) {
    if (index == 0 || index >= loads->loads.count) {
        return NULL;
    }
    tah_load *load = tah_vec_at(&loads->loads, index);
    return load->address != NULL && load->generation == generation ? load : NULL;
}

bool tah_loads_waiting(const tah_loads *loads, const ta_address *address) {
    uint64_t index = 0;
    return tah_map_count(&loads->by_address) != 0 &&
           tah_map_get(&loads->by_address, (uint64_t)(uintptr_t)address, &index);
}

bool tah_loads_withdraw(tah_loads *loads, tah_sched *sched, uint32_t index) {
    tah_envelope *envelope = tah_sched_envelope(sched, index);
    uint64_t load_index = 0;
    if (envelope == NULL || envelope->target != 0 || envelope->address == NULL ||
        tah_map_count(&loads->by_address) == 0 ||
        !tah_map_get(&loads->by_address, (uint64_t)(uintptr_t)envelope->address, &load_index)) {
        return false;
    }
    tah_load *load = tah_vec_at(&loads->loads, (uint32_t)load_index);
    if (!tah_sched_withdraw_waiting(sched, &load->mail, index)) {
        return false;
    }
    loads->waiting -= 1u;
    return true;
}

/* A free record's index, with the record reset; 0 when there is no memory. */
static uint32_t tah_loads_take(tah_loads *loads) {
    if (loads->loads.count == 0) {
        tah_load none = { 0 };
        if (tah_vec_push(&loads->loads, &none) != TA_OK) { /* index 0 is never issued */
            return 0;
        }
    }
    uint32_t index = loads->free_head;
    if (index != 0) {
        loads->free_head = ((tah_load *)tah_vec_at(&loads->loads, index))->next_free;
    } else {
        tah_load fresh = { 0 };
        if (tah_vec_push(&loads->loads, &fresh) != TA_OK) {
            return 0;
        }
        index = loads->loads.count - 1u;
    }
    tah_load *load = tah_vec_at(&loads->loads, index);
    uint32_t generation = load->generation == UINT32_MAX || load->generation == 0 ? 1u : load->generation + 1u;
    *load = (tah_load){ .generation = generation };
    return index;
}

static void tah_loads_give(tah_loads *loads, uint32_t index) {
    tah_load *load = tah_vec_at(&loads->loads, index);
    ta_address_release(load->address);
    load->address = NULL;
    load->next_free = loads->free_head;
    loads->free_head = index;
}

/*
 * Starts a load for `address`: a record, its entry in the address map, and
 * a request. TA_ERR_NOMEM leaves nothing behind. The record's index goes to
 * `*out`.
 */
typedef struct {
    ta_address *address;
    uint32_t max_waiting;
    tah_load_why why;
    const ta_trace_parent *trace;
    uint64_t now_ns;
} tah_load_start;

static ta_result tah_loads_start(tah_loads *loads, const tah_load_start *start, uint32_t *out) {
    uint32_t index = tah_loads_take(loads);
    if (index == 0) {
        return TA_ERR_NOMEM;
    }
    if (tah_vec_reserve(&loads->requests, loads->requests.count + 1u) != TA_OK ||
        tah_map_put(&loads->by_address, (uint64_t)(uintptr_t)start->address, index) != TA_OK) {
        tah_load *load = tah_vec_at(&loads->loads, index);
        load->next_free = loads->free_head;
        loads->free_head = index;
        return TA_ERR_NOMEM;
    }
    (void)tah_vec_push(&loads->requests, &index); /* reserved above */
    tah_load *load = tah_vec_at(&loads->loads, index);
    ta_address_retain(start->address);
    load->address = start->address;
    load->max_waiting = start->max_waiting;
    load->why = (uint8_t)start->why;
    load->started_ns = start->now_ns;
    if (start->trace != NULL) {
        load->trace = *start->trace;
    }
    loads->in_flight += 1u;
    *out = index;
    return TA_OK;
}

/* Appends a message to a load's mailbox, or dead-letters it when it is full. */
static ta_result tah_loads_append(tah_loads *loads, tah_sched *sched, tah_load *load, const tah_message *message) {
    if (load->mail.count >= load->max_waiting) {
        ta_address_retain(load->address);
        ta_result result = tah_sched_post_dead(sched, &(tah_dead_post){ .address = load->address, .message = message,
                                                                        .reason = TAH_DEAD_WAITING_FULL });
        if (result != TA_OK) {
            ta_address_release(load->address);
        }
        return result;
    }
    ta_result result = tah_sched_post_waiting(
        sched, &(tah_waiting_post){ .box = &load->mail, .address = load->address, .message = message });
    if (result == TA_OK) {
        loads->waiting += 1u;
    }
    return result;
}

ta_result tah_loads_route(tah_loads *loads, const tah_load_route *route) {
    uint64_t found = 0;
    if (tah_map_count(&loads->by_address) != 0 &&
        tah_map_get(&loads->by_address, (uint64_t)(uintptr_t)route->address, &found)) {
        return tah_loads_append(loads, route->sched, tah_vec_at(&loads->loads, (uint32_t)found), route->message);
    }
    if (route->resident) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t limit = tah_loads_limit(loads, route->address);
    if (limit == 0) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t index = 0;
    ta_result result = tah_loads_start(loads, &(tah_load_start){ .address = route->address,
                                                                  .max_waiting = limit,
                                                                  .why = TAH_LOAD_WHY_MAIL,
                                                                  .trace = route->message->trace,
                                                                  .now_ns = route->now_ns },
                                       &index);
    if (result != TA_OK) {
        return result;
    }
    tah_load *load = tah_vec_at(&loads->loads, index);
    result = tah_loads_append(loads, route->sched, load, route->message);
    if (result != TA_OK) {
        /* Nothing was accepted: the load never happened. */
        loads->requests.count -= 1u;
        tah_map_remove(&loads->by_address, (uint64_t)(uintptr_t)route->address);
        loads->in_flight -= 1u;
        tah_loads_give(loads, index);
    }
    return result;
}

ta_result tah_loads_wanted(tah_loads *loads, const tah_load_wanted *wanted) {
    uint64_t found = 0;
    if (tah_map_count(&loads->by_address) != 0 &&
        tah_map_get(&loads->by_address, (uint64_t)(uintptr_t)wanted->address, &found)) {
        return TA_OK; /* the load in flight serves this mail too */
    }
    uint32_t limit = tah_loads_limit(loads, wanted->address);
    if (limit == 0) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t index = 0;
    return tah_loads_start(loads, &(tah_load_start){ .address = wanted->address,
                                                      .max_waiting = limit,
                                                      .why = wanted->session ? TAH_LOAD_WHY_SESSION : TAH_LOAD_WHY_MAIL,
                                                      .trace = wanted->trace,
                                                      .now_ns = wanted->now_ns },
                           &index);
}

bool tah_loads_next_request(tah_loads *loads, tah_load_request *out) {
    while (loads->requests_head < loads->requests.count) {
        uint32_t index = *(uint32_t *)tah_vec_at(&loads->requests, loads->requests_head);
        loads->requests_head += 1u;
        if (loads->requests_head == loads->requests.count) {
            tah_vec_clear(&loads->requests);
            loads->requests_head = 0;
        }
        tah_load *load = tah_vec_at(&loads->loads, index);
        if (load->address == NULL) {
            continue; /* finished before JavaScript heard of it */
        }
        *out = (tah_load_request){ .index = index, .generation = load->generation, .address = load->address,
                                   .why = (tah_load_why)load->why };
        return true;
    }
    return false;
}

/* What core's list hears when a load ends without an actor: the TA_REFUSED_LOAD_ reason of the outcome. */
static ta_refusal_reason tah_load_refusal(tah_load_outcome outcome) {
    switch (outcome) {
    case TAH_LOAD_NOT_FOUND:
    case TAH_LOAD_DONE: return TA_REFUSED_LOAD_NOT_FOUND;
    case TAH_LOAD_REJECTED: return TA_REFUSED_LOAD_REJECTED;
    case TAH_LOAD_TIMED_OUT: return TA_REFUSED_LOAD_TIMED_OUT;
    default: return TA_REFUSED_LOAD_FAILED;
    }
}

static tah_dead_reason tah_load_dead_reason(tah_load_outcome outcome) {
    switch (outcome) {
    case TAH_LOAD_NOT_FOUND: return TAH_DEAD_NOT_FOUND;
    case TAH_LOAD_REJECTED: return TAH_DEAD_REJECTED;
    case TAH_LOAD_TIMED_OUT: return TAH_DEAD_LOAD_TIMEOUT;
    case TAH_LOAD_DONE: return TAH_DEAD_DONE;
    default: return TAH_DEAD_LOAD_FAILED;
    }
}

ta_result tah_loads_finish(tah_loads *loads, const tah_load_finish *finish) {
    tah_load *load = tah_loads_find(loads, finish->index, finish->generation);
    if (load == NULL) {
        return TA_ERR_STALE;
    }
    tah_load_outcome outcome = finish->outcome;
    ta_result result = TA_OK;
    uint32_t waiting = load->mail.count;
    bool session = load->why == TAH_LOAD_WHY_SESSION;
    if (outcome == TAH_LOAD_LOADED || outcome == TAH_LOAD_SPAWNED) {
        if (session) {
            /* The new instance of the family has no address; it only has to be resident. */
            if (tah_sched_lookup(finish->sched, finish->actor) == NULL) {
                return TA_ERR_NOT_FOUND;
            }
        } else {
            ta_actor_directory_entry entry = tah_directory_find(finish->directory, load->address);
            if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT || entry.actor != finish->actor) {
                return TA_ERR_NOT_FOUND;
            }
        }
        result = tah_sched_hand_over(finish->sched, &load->mail, finish->actor);
        if (result != TA_OK) {
            outcome = TAH_LOAD_FAILED;
        } else if (!session) {
            tah_loads_cancel_wake(loads, load->address);
        }
    }
    if (load->mail.first != 0) {
        tah_sched_bury_waiting(finish->sched, &load->mail, tah_load_dead_reason(outcome));
    }
    /* Core's own list: a named address was served by the actor's becoming resident there; a session's key is named. */
    if (outcome == TAH_LOAD_LOADED || outcome == TAH_LOAD_SPAWNED) {
        if (session) {
            (void)ta_address_loaded(finish->core, &(ta_address_loaded_params){ .address = load->address,
                                                                              .actor = finish->actor });
        }
    } else {
        (void)ta_address_load_failed(finish->core, &(ta_address_load_failed_params){ .address = load->address,
                                                                                    .reason = tah_load_refusal(outcome) });
    }
    loads->waiting -= waiting;
    loads->outcomes[outcome] += 1u;
    double seconds = finish->now_ns > load->started_ns ? (double)(finish->now_ns - load->started_ns) / 1e9 : 0.0;
    tah_exemplar exemplar =
        tah_exemplar_of(&load->trace.context, ta_step_key_span_id(load->trace.cause), finish->wall_ns);
    tah_histogram_record(&finish->metrics->load_seconds, seconds, &exemplar);
    tah_map_remove(&loads->by_address, (uint64_t)(uintptr_t)load->address);
    loads->in_flight -= 1u;
    tah_loads_give(loads, finish->index);
    return result == TA_OK ? TA_OK : TA_ERR_NOMEM;
}

/* ------------------------------------------------------------------------
 * Wake-ups: a min-heap by due time
 */

static tah_wake *tah_wake_at(const tah_loads *loads, uint32_t index) {
    return tah_vec_at(&loads->wakes, index);
}

static void tah_wake_swap(tah_loads *loads, uint32_t a, uint32_t b) {
    tah_wake held = *tah_wake_at(loads, a);
    *tah_wake_at(loads, a) = *tah_wake_at(loads, b);
    *tah_wake_at(loads, b) = held;
}

static void tah_wake_sift_up(tah_loads *loads, uint32_t index) {
    while (index > 0) {
        uint32_t parent = (index - 1u) / 2u;
        if (tah_wake_at(loads, parent)->due <= tah_wake_at(loads, index)->due) {
            return;
        }
        tah_wake_swap(loads, parent, index);
        index = parent;
    }
}

static void tah_wake_sift_down(tah_loads *loads, uint32_t index) {
    uint32_t count = loads->wakes.count;
    for (;;) {
        uint32_t left = 2u * index + 1u;
        uint32_t right = left + 1u;
        uint32_t least = index;
        if (left < count && tah_wake_at(loads, left)->due < tah_wake_at(loads, least)->due) {
            least = left;
        }
        if (right < count && tah_wake_at(loads, right)->due < tah_wake_at(loads, least)->due) {
            least = right;
        }
        if (least == index) {
            return;
        }
        tah_wake_swap(loads, least, index);
        index = least;
    }
}

/* Removes the heap's top; its reference is the caller's. */
static tah_wake tah_wake_pop(tah_loads *loads) {
    tah_wake top = *tah_wake_at(loads, 0);
    loads->wakes.count -= 1u;
    if (loads->wakes.count != 0) {
        *tah_wake_at(loads, 0) = *tah_wake_at(loads, loads->wakes.count);
        tah_wake_sift_down(loads, 0);
    }
    return top;
}

/* True when the heap entry is its address's live wake-up. */
static bool tah_wake_live(const tah_loads *loads, const tah_wake *wake) {
    uint64_t due = 0;
    return tah_map_get(&loads->wake_due, (uint64_t)(uintptr_t)wake->address, &due) && due == wake->due;
}

ta_result tah_loads_wake_at(tah_loads *loads, ta_address *address, ta_time due) {
    if (tah_vec_reserve(&loads->wakes, loads->wakes.count + 1u) != TA_OK ||
        tah_map_put(&loads->wake_due, (uint64_t)(uintptr_t)address, due) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    ta_address_retain(address);
    tah_wake wake = { .due = due, .address = address };
    (void)tah_vec_push(&loads->wakes, &wake); /* reserved above */
    tah_wake_sift_up(loads, loads->wakes.count - 1u);
    return TA_OK;
}

void tah_loads_cancel_wake(tah_loads *loads, const ta_address *address) {
    if (tah_map_count(&loads->wake_due) != 0) {
        tah_map_remove(&loads->wake_due, (uint64_t)(uintptr_t)address);
    }
}

/* The earliest live wake-up's due time, dropping replaced and cancelled
 * ones from the top; false when there is none. */
static bool tah_loads_live_wake(tah_loads *loads, ta_time *due) {
    while (loads->wakes.count != 0 && !tah_wake_live(loads, tah_wake_at(loads, 0))) {
        tah_wake stale = tah_wake_pop(loads);
        ta_address_release(stale.address);
    }
    if (loads->wakes.count == 0) {
        return false;
    }
    *due = tah_wake_at(loads, 0)->due;
    return true;
}

void tah_loads_fire_wakes(tah_loads *loads, const tah_wake_fire *fire) {
    ta_time due = 0;
    while (tah_loads_live_wake(loads, &due) && due <= fire->now) {
        tah_wake wake = tah_wake_pop(loads);
        tah_map_remove(&loads->wake_due, (uint64_t)(uintptr_t)wake.address);
        ta_actor_directory_entry entry = tah_directory_find(fire->directory, wake.address);
        uint32_t limit = tah_loads_limit(loads, wake.address);
        if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT && limit != 0 && !tah_loads_waiting(loads, wake.address)) {
            uint32_t index = 0;
            if (tah_loads_start(loads, &(tah_load_start){ .address = wake.address, .max_waiting = limit,
                                                          .why = TAH_LOAD_WHY_TIMER, .now_ns = fire->now_ns },
                                &index) != TA_OK) {
                /* No memory: try again at the next turn, and stop for now. */
                (void)tah_loads_wake_at(loads, wake.address, wake.due);
                ta_address_release(wake.address);
                return;
            }
        }
        ta_address_release(wake.address);
    }
}
