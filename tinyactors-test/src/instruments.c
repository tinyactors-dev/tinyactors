#include "instruments.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "rpc.h"
#include "ta_rpc.h"
#include "value_codec.h"

/*
 * Instrumented adapters (see instruments.h). Each instance lives as long as
 * the host process: a system keeps borrowing its adapters until it is
 * destroyed, and a scenario may read a journal after that.
 *
 * Journal entries are objects {"seq", "op", ...}. `seq` is one counter
 * shared by every instrument, so entries of different instruments can be
 * ordered against each other.
 *
 * Faults are armed per operation with the "fail" control command:
 * {"op": <operation>, "count": n (default 1; 0 means every later call),
 *  "result": "err_*", "status": ..., "message": ...}. An armed fault answers
 * the operation without doing its work, and the journal entry records
 * "injected": true.
 */

typedef enum {
    TAT_INSTRUMENT_ALLOCATOR,
    TAT_INSTRUMENT_ROUTER,
    TAT_INSTRUMENT_DIRECTORY,
    TAT_INSTRUMENT_INVOKER,
    TAT_INSTRUMENT_IO,
    TAT_INSTRUMENT_LOG,
} tat_instrument_kind;

static const char *const TAT_INSTRUMENT_KINDS[] = {
    "allocator.counting", "router.recording", "directory.memory",
    "invoker.scripted",   "io.recording",     "log.recording",
};

/* allocator.counting: the size and alignment of every live allocation. */
typedef struct {
    void *ptr;
    size_t size;
    size_t alignment;
} tat_allocation;

#define TAT_SLOT_TOMBSTONE ((void *)(uintptr_t)1)

typedef struct {
    tat_allocation *slots;
    size_t capacity;
    /* Occupied slots, tombstones included. */
    size_t used;
    uint64_t live_allocations;
    uint64_t live_bytes;
    uint64_t peak_bytes;
    uint64_t allocations;
    uint64_t frees;
    uint64_t failures;
    uint64_t anomalies;
    /* Successful allocations still allowed before failing; -1 when disarmed. */
    int64_t fail_after;
    /* Failures still to inject once failing; 0 means every later allocation. */
    int64_t fail_remaining;
} tat_allocator_state;

/* router.recording: deliveries held back in "hold" mode. */
typedef struct {
    uint64_t id;
    ta_external_delivery delivery;
} tat_held_delivery;

typedef struct {
    bool hold;
    tat_held_delivery *held;
    size_t held_count;
    size_t held_capacity;
    uint64_t next_held;
    /* Resolves named destinations, as a host's own routing does. */
    struct tat_instrument *directory;
} tat_router_state;

typedef struct {
    /* "[family,name]" -> {"family", "name", "kind", "actor"}. */
    jv entries;
} tat_directory_state;

typedef struct {
    /* Service handle (decimal text) -> {"service", "parent", "invoke_id", "live"}. */
    jv services;
    uint64_t next_service;
} tat_invoker_state;

typedef struct tat_instrument {
    char *name;
    tat_instrument_kind kind;
    tat_bind *bind;
    ta_system *system;
    jv journal;
    /* Operation -> armed fault spec with "remaining". */
    jv faults;
    /* Keeps the last injected fault message alive for the core call. */
    char *fault_message;
    tat_allocator_state allocator;
    tat_router_state router;
    tat_directory_state directory;
    tat_invoker_state invoker;
} tat_instrument;

static ta_result tat_directory_resolve(void *self, const ta_actor_directory_resolve_params *params);
static jv tat_entry_record(ta_actor_directory_entry entry);

static tat_instrument **tat_instruments;
static size_t tat_instrument_count;
static size_t tat_instrument_capacity;
static uint64_t tat_journal_sequence;

/* ------------------------------------------------------------------------
 * Shared helpers
 */

static const char *const TAT_RESULT_NAMES[] = {
    "ok",       "err_invalid",     "err_nomem",       "err_not_found",          "err_not_reconstructible",
    "err_busy", "err_unsupported", "err_corrupt",     "err_definition_mismatch", "err_unloaded",
    "err_stale",
};

static jv tat_result_name(ta_result result) {
    int index = -(int)result;
    if (index >= 0 && (size_t)index < sizeof(TAT_RESULT_NAMES) / sizeof(TAT_RESULT_NAMES[0])) {
        return jv_string(TAT_RESULT_NAMES[index]);
    }
    return jv_number((double)result);
}

static bool tat_result_parse(const char *name, ta_result *out) {
    for (size_t index = 0; index < sizeof(TAT_RESULT_NAMES) / sizeof(TAT_RESULT_NAMES[0]); index += 1) {
        if (strcmp(TAT_RESULT_NAMES[index], name) == 0) {
            *out = (ta_result)(-(int)index);
            return true;
        }
    }
    return false;
}

static jv tat_text(ta_str text) {
    return text.ptr == NULL ? jv_string("") : jv_string_sized(text.ptr, (int)text.len);
}

static jv tat_u64(uint64_t value) {
    return tat_encode_u64(NULL, &value);
}

static char *tat_strdup(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

static jv tat_value_or_null(const ta_value *value) {
    return value->v == NULL ? jv_null() : tat_value_encode(value);
}

/* The dotted text of an event name, or null when it cannot be formatted. */
static jv tat_event_name_text(const ta_system *system, ta_event_name_id name) {
    if (system == NULL) {
        return jv_null();
    }
    char small[128];
    uint32_t needed = 0;
    ta_event_name_format_params params = {.name = name, .buffer = small, .capacity = sizeof(small), .needed = &needed};
    ta_result result = ta_event_name_format(system, &params);
    if (result == TA_OK) {
        return jv_string(small);
    }
    if (result != TA_ERR_NOMEM || needed == 0) {
        return jv_null();
    }
    char *large = malloc(needed);
    if (large == NULL) {
        return jv_null();
    }
    params.buffer = large;
    params.capacity = needed;
    jv text = ta_event_name_format(system, &params) == TA_OK ? jv_string(large) : jv_null();
    free(large);
    return text;
}

/* An event as the generated codec encodes it, plus its dotted name. */
static jv tat_event_record(const tat_instrument *instrument, const ta_event *event) {
    jv record = tat_encode_ta_event(instrument->bind, event);
    return jv_object_set(record, jv_string("name_text"), tat_event_name_text(instrument->system, event->name));
}

static void tat_journal(tat_instrument *instrument, const char *op, jv entry) {
    tat_journal_sequence += 1;
    entry = jv_object_set(entry, jv_string("seq"), tat_u64(tat_journal_sequence));
    entry = jv_object_set(entry, jv_string("op"), jv_string(op));
    instrument->journal = jv_array_append(instrument->journal, entry);
}

/*
 * Takes one armed fault for `op`. On success *spec is the fault spec (owned)
 * and the instrument's fault message points at its message.
 */
static bool tat_take_fault(tat_instrument *instrument, const char *op, jv *spec) {
    jv armed = jv_object_get(jv_copy(instrument->faults), jv_string(op));
    if (jv_get_kind(armed) != JV_KIND_OBJECT) {
        jv_free(armed);
        return false;
    }
    jv remaining = jv_object_get(jv_copy(armed), jv_string("remaining"));
    double left = jv_get_kind(remaining) == JV_KIND_NUMBER ? jv_number_value(remaining) : 1;
    jv_free(remaining);
    if (left == 1) {
        instrument->faults = jv_object_delete(instrument->faults, jv_string(op));
    } else if (left > 1) {
        instrument->faults = jv_object_set(instrument->faults, jv_string(op),
                                           jv_object_set(jv_copy(armed), jv_string("remaining"), jv_number(left - 1)));
    }
    jv message = jv_object_get(jv_copy(armed), jv_string("message"));
    free(instrument->fault_message);
    instrument->fault_message =
        tat_strdup(jv_get_kind(message) == JV_KIND_STRING ? jv_string_value(message) : "injected fault");
    jv_free(message);
    *spec = armed;
    return true;
}

static ta_result tat_fault_result(jv spec, ta_result fallback) {
    jv name = jv_object_get(jv_copy(spec), jv_string("result"));
    ta_result result = fallback;
    if (jv_get_kind(name) == JV_KIND_STRING) {
        (void)tat_result_parse(jv_string_value(name), &result);
    }
    jv_free(name);
    return result;
}

static void tat_fault_fill(tat_instrument *instrument, jv spec, ta_result fallback, ta_fault *fault) {
    const char *message = instrument->fault_message == NULL ? "" : instrument->fault_message;
    *fault = (ta_fault){.code = tat_fault_result(spec, fallback),
                        .message = {.ptr = message, .len = (uint32_t)strlen(message)}};
}

static jv tat_injected(jv entry) {
    return jv_object_set(entry, jv_string("injected"), jv_true());
}

static tat_instrument *tat_instrument_find(const char *name) {
    for (size_t index = 0; index < tat_instrument_count; index += 1) {
        if (strcmp(tat_instruments[index]->name, name) == 0) {
            return tat_instruments[index];
        }
    }
    return NULL;
}

/*
 * Resolves config to an instrument of `kind`: an existing one when config
 * is {"name": ...} alone, else a new one. `keys` lists the config members
 * the kind accepts besides "name"; `needs_system` requires "system".
 */
static tat_instrument *tat_instrument_resolve(tat_bind *bind, jv config, tat_instrument_kind kind,
                                              const char *const *keys, bool needs_system, bool *created) {
    const char *kind_name = TAT_INSTRUMENT_KINDS[kind];
    *created = false;
    if (!jv_is_valid(config) || jv_get_kind(config) != JV_KIND_OBJECT) {
        tat_fail(bind, "%s needs config {\"name\": <instrument name>, ...}", kind_name);
        return NULL;
    }
    jv name = jv_object_get(jv_copy(config), jv_string("name"));
    if (jv_get_kind(name) != JV_KIND_STRING || jv_string_length_bytes(jv_copy(name)) == 0) {
        jv_free(name);
        tat_fail(bind, "%s needs config.name, a non-empty string", kind_name);
        return NULL;
    }
    tat_instrument *existing = tat_instrument_find(jv_string_value(name));
    if (existing != NULL) {
        bool alone = jv_object_length(jv_copy(config)) == 1;
        if (existing->kind != kind || !alone) {
            tat_fail(bind, "instrument \"%s\" already exists as %s; reuse it with {\"name\": ...} alone",
                     jv_string_value(name), TAT_INSTRUMENT_KINDS[existing->kind]);
            jv_free(name);
            return NULL;
        }
        jv_free(name);
        return existing;
    }
    jv_object_foreach(config, key, value) {
        bool known = strcmp(jv_string_value(key), "name") == 0 || (needs_system && strcmp(jv_string_value(key), "system") == 0);
        for (size_t index = 0; keys != NULL && keys[index] != NULL && !known; index += 1) {
            known = strcmp(jv_string_value(key), keys[index]) == 0;
        }
        if (!known) {
            tat_fail(bind, "%s: unknown config member \"%s\"", kind_name, jv_string_value(key));
        }
        jv_free(key);
        jv_free(value);
        if (!known) {
            jv_free(name);
            return NULL;
        }
    }
    void *system = NULL;
    if (needs_system) {
        jv handle = jv_object_get(jv_copy(config), jv_string("system"));
        bool ok = jv_is_valid(handle) && tat_handle_resolve(bind, handle, "system", &system);
        jv_free(handle);
        if (!ok) {
            jv_free(name);
            tat_fail(bind, "%s needs config.system, a system handle", kind_name);
            return NULL;
        }
    }
    if (tat_instrument_count == tat_instrument_capacity) {
        size_t capacity = tat_instrument_capacity == 0 ? 8 : tat_instrument_capacity * 2;
        tat_instrument **grown = realloc(tat_instruments, capacity * sizeof(*grown));
        if (grown == NULL) {
            jv_free(name);
            tat_fail(bind, "out of memory");
            return NULL;
        }
        tat_instruments = grown;
        tat_instrument_capacity = capacity;
    }
    tat_instrument *instrument = calloc(1, sizeof(*instrument));
    char *copy = tat_strdup(jv_string_value(name));
    jv_free(name);
    if (instrument == NULL || copy == NULL) {
        free(instrument);
        free(copy);
        tat_fail(bind, "out of memory");
        return NULL;
    }
    instrument->name = copy;
    instrument->kind = kind;
    instrument->bind = bind;
    instrument->system = system;
    instrument->journal = jv_array();
    instrument->faults = jv_object();
    instrument->allocator.fail_after = -1;
    instrument->directory.entries = jv_object();
    instrument->invoker.services = jv_object();
    instrument->invoker.next_service = 1;
    tat_instruments[tat_instrument_count++] = instrument;
    *created = true;
    return instrument;
}

/* ------------------------------------------------------------------------
 * allocator.counting
 *
 * Wraps libc and tracks every live allocation by address. A free of an
 * unknown address, or with a size or alignment other than the allocation's,
 * is an anomaly: it is journaled (op "free_unknown" or "free_mismatch") and
 * an unknown address is never passed to libc. Allocation failures are armed
 * with the "fail" command {"after": n, "count": m}: n more allocations
 * succeed, then m fail (m = 0: every later one fails). Config: {"name"}.
 */

static size_t tat_slot_hash(const void *ptr, size_t capacity) {
    uint64_t bits = (uint64_t)(uintptr_t)ptr;
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdULL;
    bits ^= bits >> 33;
    return (size_t)bits & (capacity - 1);
}

static tat_allocation *tat_slot_find(tat_allocator_state *state, const void *ptr) {
    if (state->capacity == 0) {
        return NULL;
    }
    for (size_t probe = tat_slot_hash(ptr, state->capacity), step = 0; step < state->capacity;
         probe = (probe + 1) & (state->capacity - 1), step += 1) {
        tat_allocation *slot = &state->slots[probe];
        if (slot->ptr == NULL) {
            return NULL;
        }
        if (slot->ptr == ptr) {
            return slot;
        }
    }
    return NULL;
}

static bool tat_slot_insert(tat_allocator_state *state, tat_allocation allocation);

static bool tat_slots_grow(tat_allocator_state *state) {
    size_t capacity = state->capacity == 0 ? 256 : state->capacity * 2;
    /* Rehash in place when tombstones, not live entries, fill the table. */
    if (state->capacity != 0 && state->live_allocations * 4 < state->capacity) {
        capacity = state->capacity;
    }
    tat_allocation *old = state->slots;
    size_t old_capacity = state->capacity;
    state->slots = calloc(capacity, sizeof(*state->slots));
    if (state->slots == NULL) {
        state->slots = old;
        return false;
    }
    state->capacity = capacity;
    state->used = 0;
    for (size_t index = 0; index < old_capacity; index += 1) {
        if (old[index].ptr != NULL && old[index].ptr != TAT_SLOT_TOMBSTONE) {
            (void)tat_slot_insert(state, old[index]);
        }
    }
    free(old);
    return true;
}

static bool tat_slot_insert(tat_allocator_state *state, tat_allocation allocation) {
    if ((state->used + 1) * 4 > state->capacity * 3 && !tat_slots_grow(state)) {
        return false;
    }
    size_t probe = tat_slot_hash(allocation.ptr, state->capacity);
    while (state->slots[probe].ptr != NULL && state->slots[probe].ptr != TAT_SLOT_TOMBSTONE) {
        probe = (probe + 1) & (state->capacity - 1);
    }
    if (state->slots[probe].ptr == NULL) {
        state->used += 1;
    }
    state->slots[probe] = allocation;
    return true;
}

static void *tat_counting_alloc(void *self, size_t size, size_t alignment) {
    tat_instrument *instrument = self;
    tat_allocator_state *state = &instrument->allocator;
    if (state->fail_after == 0) {
        state->failures += 1;
        jv entry = jv_object_set(jv_object(), jv_string("size"), tat_u64(size));
        tat_journal(instrument, "alloc_failed", jv_object_set(entry, jv_string("alignment"), tat_u64(alignment)));
        if (state->fail_remaining > 0 && --state->fail_remaining == 0) {
            state->fail_after = -1;
        }
        return NULL;
    }
    ta_allocator libc = tat_libc_allocator();
    void *ptr = libc.v->alloc(libc.self, size == 0 ? 1 : size, alignment);
    if (ptr == NULL) {
        return NULL;
    }
    if (!tat_slot_insert(state, (tat_allocation){.ptr = ptr, .size = size, .alignment = alignment})) {
        libc.v->free(libc.self, &(ta_allocator_free_params){.ptr = ptr, .size = size, .alignment = alignment});
        return NULL;
    }
    if (state->fail_after > 0) {
        state->fail_after -= 1;
    }
    state->allocations += 1;
    state->live_allocations += 1;
    state->live_bytes += size;
    if (state->live_bytes > state->peak_bytes) {
        state->peak_bytes = state->live_bytes;
    }
    return ptr;
}

static void tat_counting_free(void *self, const ta_allocator_free_params *params) {
    tat_instrument *instrument = self;
    tat_allocator_state *state = &instrument->allocator;
    if (params->ptr == NULL) {
        return;
    }
    tat_allocation *slot = tat_slot_find(state, params->ptr);
    if (slot == NULL) {
        state->anomalies += 1;
        jv entry = jv_object_set(jv_object(), jv_string("size"), tat_u64(params->size));
        tat_journal(instrument, "free_unknown", entry);
        return;
    }
    if (slot->size != params->size || slot->alignment != params->alignment) {
        state->anomalies += 1;
        jv entry = jv_object();
        entry = jv_object_set(entry, jv_string("allocated_size"), tat_u64(slot->size));
        entry = jv_object_set(entry, jv_string("allocated_alignment"), tat_u64(slot->alignment));
        entry = jv_object_set(entry, jv_string("freed_size"), tat_u64(params->size));
        entry = jv_object_set(entry, jv_string("freed_alignment"), tat_u64(params->alignment));
        tat_journal(instrument, "free_mismatch", entry);
    }
    state->frees += 1;
    state->live_allocations -= 1;
    state->live_bytes -= slot->size;
    ta_allocator libc = tat_libc_allocator();
    libc.v->free(libc.self, &(ta_allocator_free_params){.ptr = slot->ptr, .size = slot->size, .alignment = slot->alignment});
    slot->ptr = TAT_SLOT_TOMBSTONE;
}

static const ta_allocator_vtable TAT_COUNTING_ALLOCATOR = {.alloc = tat_counting_alloc, .free = tat_counting_free};

bool tat_make_counting_allocator(tat_bind *bind, jv config, void *out) {
    bool created = false;
    tat_instrument *instrument =
        tat_instrument_resolve(bind, config, TAT_INSTRUMENT_ALLOCATOR, NULL, false, &created);
    if (instrument == NULL) {
        return false;
    }
    *(ta_allocator *)out = (ta_allocator){.self = instrument, .v = &TAT_COUNTING_ALLOCATOR};
    return true;
}

static jv tat_allocator_stats(const tat_allocator_state *state) {
    jv stats = jv_object();
    stats = jv_object_set(stats, jv_string("live_allocations"), tat_u64(state->live_allocations));
    stats = jv_object_set(stats, jv_string("live_bytes"), tat_u64(state->live_bytes));
    stats = jv_object_set(stats, jv_string("peak_bytes"), tat_u64(state->peak_bytes));
    stats = jv_object_set(stats, jv_string("allocations"), tat_u64(state->allocations));
    stats = jv_object_set(stats, jv_string("frees"), tat_u64(state->frees));
    stats = jv_object_set(stats, jv_string("failures"), tat_u64(state->failures));
    stats = jv_object_set(stats, jv_string("anomalies"), tat_u64(state->anomalies));
    return jv_object_set(stats, jv_string("failing"), jv_bool(state->fail_after >= 0));
}

/* ------------------------------------------------------------------------
 * router.recording
 *
 * Journals every submitted external delivery (op "submit": destination,
 * event, status). Mode "direct" (the default) hands it to
 * ta_actor_deliver() like router.direct. Mode "hold" clones it and answers
 * accepted; the scenario then releases held deliveries in any order, or
 * drops them. A "fail" on op "submit" answers "status": "retry" or "error"
 * (the default) without delivering. Config: {"name", "system", "mode"?}.
 */

/*
 * Delivers to a runtime destination, or resolves a named one through the
 * router's directory instrument. Resolution goes through the directory's
 * `resolve` operation, the same one core calls (ADR 0037), so it is
 * journaled and a fault armed on `resolve` reaches routing too. A resident
 * entry is delivered; an unloaded one answers TA_ERR_UNLOADED, because this
 * router cannot load an actor (a host would); an unknown address answers
 * whatever resolve reported, TA_ERR_NOT_FOUND by default. Without a
 * directory, a named destination is unsupported. The resolved entry is
 * added to `entry`.
 */
static ta_result tat_router_deliver(tat_instrument *instrument, const ta_external_delivery *delivery, jv *entry) {
    ta_actor_id id;
    if (delivery->destination.kind == TA_ACTOR_REF_RUNTIME) {
        id = delivery->destination.as.runtime;
    } else {
        tat_instrument *directory = instrument->router.directory;
        if (directory == NULL) {
            return TA_ERR_UNSUPPORTED;
        }
        ta_actor_directory_entry resolved = {.kind = TA_ACTOR_DIRECTORY_NONE};
        ta_result result = tat_directory_resolve(
            directory, &(ta_actor_directory_resolve_params){.address = &delivery->destination.as.named,
                                                            .out = &resolved});
        if (entry != NULL) {
            *entry = jv_object_set(*entry, jv_string("resolved"), tat_entry_record(resolved));
        }
        if (result != TA_OK) {
            return result;
        }
        if (resolved.kind == TA_ACTOR_DIRECTORY_UNLOADED) {
            return TA_ERR_UNLOADED;
        }
        id = resolved.actor;
    }
    return ta_actor_deliver(instrument->system, id, &delivery->event);
}

static ta_external_event_submit_status tat_recording_submit(void *self, const ta_external_event_submit_params *params) {
    static const char not_delivered[] = "router.recording could not deliver the event";
    tat_instrument *instrument = self;
    tat_router_state *state = &instrument->router;
    const ta_external_delivery *delivery = params->delivery;
    jv entry = jv_object();
    entry = jv_object_set(entry, jv_string("destination"), tat_encode_ta_actor_ref(instrument->bind, &delivery->destination));
    entry = jv_object_set(entry, jv_string("event"), tat_event_record(instrument, &delivery->event));

    jv spec;
    if (tat_take_fault(instrument, "submit", &spec)) {
        jv status = jv_object_get(jv_copy(spec), jv_string("status"));
        bool retry = jv_get_kind(status) == JV_KIND_STRING && strcmp(jv_string_value(status), "retry") == 0;
        jv_free(status);
        if (!retry) {
            tat_fault_fill(instrument, spec, TA_ERR_UNSUPPORTED, params->fault);
        }
        jv_free(spec);
        entry = jv_object_set(entry, jv_string("status"), jv_string(retry ? "retry" : "error"));
        tat_journal(instrument, "submit", tat_injected(entry));
        return retry ? TA_EXTERNAL_EVENT_RETRY : TA_EXTERNAL_EVENT_ERROR;
    }

    if (state->hold) {
        if (state->held_count == state->held_capacity) {
            size_t capacity = state->held_capacity == 0 ? 8 : state->held_capacity * 2;
            tat_held_delivery *grown = realloc(state->held, capacity * sizeof(*grown));
            if (grown == NULL) {
                tat_journal(instrument, "submit", jv_object_set(entry, jv_string("status"), jv_string("retry")));
                return TA_EXTERNAL_EVENT_RETRY;
            }
            state->held = grown;
            state->held_capacity = capacity;
        }
        tat_held_delivery *held = &state->held[state->held_count];
        if (ta_external_delivery_clone(delivery, tat_libc_allocator(), &held->delivery) != TA_OK) {
            tat_journal(instrument, "submit", jv_object_set(entry, jv_string("status"), jv_string("retry")));
            return TA_EXTERNAL_EVENT_RETRY;
        }
        held->id = ++state->next_held;
        state->held_count += 1;
        entry = jv_object_set(entry, jv_string("status"), jv_string("accepted"));
        tat_journal(instrument, "submit", jv_object_set(entry, jv_string("held"), tat_u64(held->id)));
        return TA_EXTERNAL_EVENT_ACCEPTED;
    }

    ta_result result = tat_router_deliver(instrument, delivery, &entry);
    entry = jv_object_set(entry, jv_string("result"), tat_result_name(result));
    if (result == TA_OK) {
        tat_journal(instrument, "submit", jv_object_set(entry, jv_string("status"), jv_string("accepted")));
        return TA_EXTERNAL_EVENT_ACCEPTED;
    }
    if (result == TA_ERR_BUSY) {
        tat_journal(instrument, "submit", jv_object_set(entry, jv_string("status"), jv_string("retry")));
        return TA_EXTERNAL_EVENT_RETRY;
    }
    *params->fault = (ta_fault){.code = result, .message = {.ptr = not_delivered, .len = sizeof(not_delivered) - 1}};
    tat_journal(instrument, "submit", jv_object_set(entry, jv_string("status"), jv_string("error")));
    return TA_EXTERNAL_EVENT_ERROR;
}

static const ta_external_event_router_vtable TAT_RECORDING_ROUTER = {.submit = tat_recording_submit};

static bool tat_router_set_mode(tat_bind *bind, tat_instrument *instrument, jv mode) {
    if (jv_get_kind(mode) != JV_KIND_STRING ||
        (strcmp(jv_string_value(mode), "direct") != 0 && strcmp(jv_string_value(mode), "hold") != 0)) {
        return tat_fail(bind, "router.recording: mode is \"direct\" or \"hold\"");
    }
    instrument->router.hold = strcmp(jv_string_value(mode), "hold") == 0;
    return true;
}

bool tat_make_recording_router(tat_bind *bind, jv config, void *out) {
    static const char *const keys[] = {"mode", "directory", NULL};
    bool created = false;
    tat_instrument *instrument = tat_instrument_resolve(bind, config, TAT_INSTRUMENT_ROUTER, keys, true, &created);
    if (instrument == NULL) {
        return false;
    }
    if (created) {
        jv mode = jv_object_get(jv_copy(config), jv_string("mode"));
        bool ok = !jv_is_valid(mode) || jv_get_kind(mode) == JV_KIND_NULL || tat_router_set_mode(bind, instrument, mode);
        jv_free(mode);
        if (!ok) {
            return false;
        }
        jv directory = jv_object_get(jv_copy(config), jv_string("directory"));
        if (jv_get_kind(directory) == JV_KIND_STRING) {
            tat_instrument *found = tat_instrument_find(jv_string_value(directory));
            if (found == NULL || found->kind != TAT_INSTRUMENT_DIRECTORY) {
                tat_fail(bind, "router.recording: config.directory names no directory.memory instrument");
                jv_free(directory);
                return false;
            }
            instrument->router.directory = found;
        } else if (jv_is_valid(directory) && jv_get_kind(directory) != JV_KIND_NULL) {
            tat_fail(bind, "router.recording: config.directory must be an instrument name");
            jv_free(directory);
            return false;
        }
        jv_free(directory);
    }
    *(ta_external_event_router *)out = (ta_external_event_router){.self = instrument, .v = &TAT_RECORDING_ROUTER};
    return true;
}

static jv tat_held_record(tat_instrument *instrument, const tat_held_delivery *held) {
    jv record = jv_object_set(jv_object(), jv_string("id"), tat_u64(held->id));
    record = jv_object_set(record, jv_string("destination"),
                           tat_encode_ta_actor_ref(instrument->bind, &held->delivery.destination));
    return jv_object_set(record, jv_string("event"), tat_event_record(instrument, &held->delivery.event));
}

static tat_held_delivery *tat_held_find(tat_router_state *state, uint64_t id, size_t *index) {
    for (size_t at = 0; at < state->held_count; at += 1) {
        if (state->held[at].id == id) {
            *index = at;
            return &state->held[at];
        }
    }
    return NULL;
}

static void tat_held_remove(tat_router_state *state, size_t index) {
    ta_external_delivery_release(&state->held[index].delivery, tat_libc_allocator());
    memmove(&state->held[index], &state->held[index + 1], (state->held_count - index - 1) * sizeof(*state->held));
    state->held_count -= 1;
}

/* ------------------------------------------------------------------------
 * directory.memory
 *
 * A compare-and-swap actor directory in memory. claim fails with
 * TA_ERR_STALE when the address has an entry; replace and remove fail with
 * TA_ERR_STALE unless the current entry equals `expected` (an absent entry
 * equals kind "none"); resolve of an absent address writes kind "none" and
 * returns TA_ERR_NOT_FOUND. Every operation is journaled. Config: {"name"}.
 */

static jv tat_address_key(const ta_actor_address *address) {
    jv key = jv_array_append(jv_array(), tat_text(address->family.name));
    key = jv_array_append(key, tat_text(address->name));
    return jv_dump_string(key, 0);
}

static const char *const TAT_ENTRY_KINDS[] = {"none", "resident", "unloaded"};

static jv tat_entry_record(ta_actor_directory_entry entry) {
    jv record = jv_object_set(jv_object(), jv_string("kind"),
                              jv_string(entry.kind <= TA_ACTOR_DIRECTORY_UNLOADED ? TAT_ENTRY_KINDS[entry.kind] : "invalid"));
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        record = jv_object_set(record, jv_string("actor"), tat_u64(entry.actor));
    }
    return record;
}

static ta_actor_directory_entry tat_directory_current(tat_instrument *instrument, const ta_actor_address *address) {
    jv stored = jv_object_get(jv_copy(instrument->directory.entries), tat_address_key(address));
    ta_actor_directory_entry entry = {.kind = TA_ACTOR_DIRECTORY_NONE};
    if (jv_get_kind(stored) == JV_KIND_OBJECT) {
        jv kind = jv_object_get(jv_copy(stored), jv_string("kind"));
        entry.kind = strcmp(jv_string_value(kind), "resident") == 0 ? TA_ACTOR_DIRECTORY_RESIDENT : TA_ACTOR_DIRECTORY_UNLOADED;
        jv_free(kind);
        jv actor = jv_object_get(jv_copy(stored), jv_string("actor"));
        if (jv_get_kind(actor) != JV_KIND_NULL) {
            (void)tat_decode_u64(instrument->bind, actor, &entry.actor);
        }
        jv_free(actor);
    }
    jv_free(stored);
    return entry;
}

static bool tat_entries_equal(ta_actor_directory_entry left, ta_actor_directory_entry right) {
    return left.kind == right.kind && (left.kind != TA_ACTOR_DIRECTORY_RESIDENT || left.actor == right.actor);
}

static void tat_directory_store(tat_instrument *instrument, const ta_actor_address *address, ta_actor_directory_entry entry) {
    if (entry.kind == TA_ACTOR_DIRECTORY_NONE) {
        instrument->directory.entries = jv_object_delete(instrument->directory.entries, tat_address_key(address));
        return;
    }
    jv record = tat_entry_record(entry);
    record = jv_object_set(record, jv_string("family"), tat_text(address->family.name));
    record = jv_object_set(record, jv_string("name"), tat_text(address->name));
    instrument->directory.entries = jv_object_set(instrument->directory.entries, tat_address_key(address), record);
}

static jv tat_directory_entry(const ta_actor_address *address) {
    jv entry = jv_object_set(jv_object(), jv_string("family"), tat_text(address->family.name));
    return jv_object_set(entry, jv_string("name"), tat_text(address->name));
}

static ta_result tat_directory_finish(tat_instrument *instrument, const char *op, jv entry, ta_result result) {
    tat_journal(instrument, op, jv_object_set(entry, jv_string("result"), tat_result_name(result)));
    return result;
}

static bool tat_directory_fault(tat_instrument *instrument, const char *op, jv *entry, ta_result *result) {
    jv spec;
    if (!tat_take_fault(instrument, op, &spec)) {
        return false;
    }
    *result = tat_fault_result(spec, TA_ERR_STALE);
    jv_free(spec);
    *entry = tat_injected(*entry);
    return true;
}

static ta_result tat_directory_claim(void *self, const ta_actor_directory_claim_params *params) {
    tat_instrument *instrument = self;
    jv entry = jv_object_set(tat_directory_entry(params->address), jv_string("entry"), tat_entry_record(params->entry));
    ta_result result = TA_OK;
    if (!tat_directory_fault(instrument, "claim", &entry, &result)) {
        if (tat_directory_current(instrument, params->address).kind != TA_ACTOR_DIRECTORY_NONE) {
            result = TA_ERR_STALE;
        } else {
            tat_directory_store(instrument, params->address, params->entry);
        }
    }
    return tat_directory_finish(instrument, "claim", entry, result);
}

static ta_result tat_directory_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    tat_instrument *instrument = self;
    jv entry = tat_directory_entry(params->address);
    ta_result result = TA_OK;
    if (!tat_directory_fault(instrument, "resolve", &entry, &result)) {
        *params->out = tat_directory_current(instrument, params->address);
        result = params->out->kind == TA_ACTOR_DIRECTORY_NONE ? TA_ERR_NOT_FOUND : TA_OK;
        entry = jv_object_set(entry, jv_string("out"), tat_entry_record(*params->out));
    }
    return tat_directory_finish(instrument, "resolve", entry, result);
}

static ta_result tat_directory_replace(void *self, const ta_actor_directory_replace_params *params) {
    tat_instrument *instrument = self;
    jv entry = tat_directory_entry(params->address);
    entry = jv_object_set(entry, jv_string("expected"), tat_entry_record(params->expected));
    entry = jv_object_set(entry, jv_string("replacement"), tat_entry_record(params->replacement));
    ta_result result = TA_OK;
    if (!tat_directory_fault(instrument, "replace", &entry, &result)) {
        if (!tat_entries_equal(tat_directory_current(instrument, params->address), params->expected)) {
            result = TA_ERR_STALE;
        } else {
            tat_directory_store(instrument, params->address, params->replacement);
        }
    }
    return tat_directory_finish(instrument, "replace", entry, result);
}

static ta_result tat_directory_remove(void *self, const ta_actor_directory_remove_params *params) {
    tat_instrument *instrument = self;
    jv entry = jv_object_set(tat_directory_entry(params->address), jv_string("expected"), tat_entry_record(params->expected));
    ta_result result = TA_OK;
    if (!tat_directory_fault(instrument, "remove", &entry, &result)) {
        if (!tat_entries_equal(tat_directory_current(instrument, params->address), params->expected)) {
            result = TA_ERR_STALE;
        } else {
            tat_directory_store(instrument, params->address, (ta_actor_directory_entry){.kind = TA_ACTOR_DIRECTORY_NONE});
        }
    }
    return tat_directory_finish(instrument, "remove", entry, result);
}

static const ta_actor_directory_vtable TAT_MEMORY_DIRECTORY = {
    .claim = tat_directory_claim,
    .resolve = tat_directory_resolve,
    .replace = tat_directory_replace,
    .remove = tat_directory_remove,
};

bool tat_make_memory_directory(tat_bind *bind, jv config, void *out) {
    bool created = false;
    tat_instrument *instrument = tat_instrument_resolve(bind, config, TAT_INSTRUMENT_DIRECTORY, NULL, false, &created);
    if (instrument == NULL) {
        return false;
    }
    *(ta_actor_directory *)out = (ta_actor_directory){.self = instrument, .v = &TAT_MEMORY_DIRECTORY};
    return true;
}

/* ------------------------------------------------------------------------
 * invoker.scripted
 *
 * An invoked service that is not an actor. start journals the request and
 * answers a fresh service handle; the scenario later reports the service's
 * events, completion, or loss with ta.actor.report_invoke. cancel and
 * forward are journaled. "fail" arms op "start", "cancel", or "forward".
 * Config: {"name", "system"}.
 */

static jv tat_service_key(uint64_t service) {
    return jv_string_fmt("%llu", (unsigned long long)service);
}

static ta_op_status tat_scripted_start(void *self, const ta_invoker_start_params *params) {
    tat_instrument *instrument = self;
    const ta_invoke_request *request = params->request;
    jv entry = jv_object();
    entry = jv_object_set(entry, jv_string("parent"), tat_u64(request->parent));
    entry = jv_object_set(entry, jv_string("invoke_id"), tat_u64(request->invoke_id));
    entry = jv_object_set(entry, jv_string("type"), tat_u64(request->type));
    entry = jv_object_set(entry, jv_string("src"), tat_value_or_null(&request->src));
    entry = jv_object_set(entry, jv_string("data"), tat_value_or_null(&request->data));
    entry = jv_object_set(entry, jv_string("params"), tat_value_or_null(&request->params));
    entry = jv_object_set(entry, jv_string("autoforward"), jv_bool(request->autoforward));
    jv spec;
    if (tat_take_fault(instrument, "start", &spec)) {
        tat_fault_fill(instrument, spec, TA_ERR_UNSUPPORTED, params->fault);
        jv_free(spec);
        tat_journal(instrument, "start", tat_injected(entry));
        return TA_OP_ERROR;
    }
    uint64_t service = instrument->invoker.next_service++;
    *params->service_handle = service;
    jv record = jv_object_set(jv_object(), jv_string("service"), tat_u64(service));
    record = jv_object_set(record, jv_string("parent"), tat_u64(request->parent));
    record = jv_object_set(record, jv_string("invoke_id"), tat_u64(request->invoke_id));
    record = jv_object_set(record, jv_string("live"), jv_true());
    instrument->invoker.services = jv_object_set(instrument->invoker.services, tat_service_key(service), record);
    tat_journal(instrument, "start", jv_object_set(entry, jv_string("service"), tat_u64(service)));
    return TA_OP_OK;
}

/* Journals a callback on a service; false when the service is unknown or ended. */
static bool tat_service_live(tat_instrument *instrument, uint64_t service, jv *entry) {
    jv record = jv_object_get(jv_copy(instrument->invoker.services), tat_service_key(service));
    jv live = jv_get_kind(record) == JV_KIND_OBJECT ? jv_object_get(jv_copy(record), jv_string("live")) : jv_false();
    bool result = jv_get_kind(live) == JV_KIND_TRUE;
    jv_free(live);
    jv_free(record);
    *entry = jv_object_set(*entry, jv_string("known"), jv_bool(result));
    return result;
}

static ta_op_status tat_scripted_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    tat_instrument *instrument = self;
    jv entry = jv_object_set(jv_object(), jv_string("service"), tat_u64(service_handle));
    bool live = tat_service_live(instrument, service_handle, &entry);
    jv spec;
    if (tat_take_fault(instrument, "cancel", &spec)) {
        tat_fault_fill(instrument, spec, TA_ERR_UNSUPPORTED, fault);
        jv_free(spec);
        tat_journal(instrument, "cancel", tat_injected(entry));
        return TA_OP_ERROR;
    }
    if (live) {
        jv key = tat_service_key(service_handle);
        jv record = jv_object_get(jv_copy(instrument->invoker.services), jv_copy(key));
        instrument->invoker.services =
            jv_object_set(instrument->invoker.services, key, jv_object_set(record, jv_string("live"), jv_false()));
    }
    tat_journal(instrument, "cancel", entry);
    return TA_OP_OK;
}

static ta_op_status tat_scripted_forward(void *self, const ta_invoker_forward_params *params) {
    tat_instrument *instrument = self;
    jv entry = jv_object_set(jv_object(), jv_string("service"), tat_u64(params->service_handle));
    (void)tat_service_live(instrument, params->service_handle, &entry);
    entry = jv_object_set(entry, jv_string("event"), tat_event_record(instrument, params->event));
    jv spec;
    if (tat_take_fault(instrument, "forward", &spec)) {
        tat_fault_fill(instrument, spec, TA_ERR_UNSUPPORTED, params->fault);
        jv_free(spec);
        tat_journal(instrument, "forward", tat_injected(entry));
        return TA_OP_ERROR;
    }
    tat_journal(instrument, "forward", entry);
    return TA_OP_OK;
}

static const ta_invoker_vtable TAT_SCRIPTED_INVOKER = {
    .start = tat_scripted_start,
    .cancel = tat_scripted_cancel,
    .forward = tat_scripted_forward,
};

bool tat_make_scripted_invoker(tat_bind *bind, jv config, void *out) {
    bool created = false;
    tat_instrument *instrument = tat_instrument_resolve(bind, config, TAT_INSTRUMENT_INVOKER, NULL, true, &created);
    if (instrument == NULL) {
        return false;
    }
    *(ta_invoker *)out = (ta_invoker){.self = instrument, .v = &TAT_SCRIPTED_INVOKER};
    return true;
}

/* ------------------------------------------------------------------------
 * io.recording
 *
 * An Event I/O Processor that journals every send (op "send": source,
 * send_id, event, io_type, target, data) and delivers nothing. "fail" on op
 * "send" answers TA_OP_ERROR with the armed fault. Config: {"name", "system"}.
 */

static ta_op_status tat_recording_send(void *self, const ta_send *send, ta_fault *fault) {
    tat_instrument *instrument = self;
    jv entry = jv_object();
    entry = jv_object_set(entry, jv_string("source"), tat_encode_ta_actor_ref(instrument->bind, &send->source));
    entry = jv_object_set(entry, jv_string("send_id"), tat_u64(send->send_id));
    entry = jv_object_set(entry, jv_string("event"), tat_u64(send->event));
    entry = jv_object_set(entry, jv_string("event_text"), tat_event_name_text(instrument->system, send->event));
    entry = jv_object_set(entry, jv_string("io_type"), tat_u64(send->io_type));
    entry = jv_object_set(entry, jv_string("target"), tat_value_or_null(&send->target));
    entry = jv_object_set(entry, jv_string("data"), tat_value_or_null(&send->data));
    jv spec;
    if (tat_take_fault(instrument, "send", &spec)) {
        tat_fault_fill(instrument, spec, TA_ERR_UNSUPPORTED, fault);
        jv_free(spec);
        tat_journal(instrument, "send", tat_injected(entry));
        return TA_OP_ERROR;
    }
    tat_journal(instrument, "send", entry);
    return TA_OP_OK;
}

static const ta_io_vtable TAT_RECORDING_IO = {.send = tat_recording_send};

bool tat_make_recording_io(tat_bind *bind, jv config, void *out) {
    bool created = false;
    tat_instrument *instrument = tat_instrument_resolve(bind, config, TAT_INSTRUMENT_IO, NULL, true, &created);
    if (instrument == NULL) {
        return false;
    }
    *(ta_io *)out = (ta_io){.self = instrument, .v = &TAT_RECORDING_IO};
    return true;
}

/* ------------------------------------------------------------------------
 * log.recording
 *
 * The system's <log> sink (ADR 0045). It journals each record (op "log":
 * source, label, value) and has no commands: writing a log cannot fail, so
 * there is nothing to inject. Config: {"name", "system"}.
 */

static void tat_recording_log(void *self, const ta_log_record *record) {
    tat_instrument *instrument = self;
    jv entry = jv_object();
    entry = jv_object_set(entry, jv_string("source"), tat_encode_ta_actor_ref(instrument->bind, &record->source));
    entry = jv_object_set(entry, jv_string("label"), tat_text(record->label));
    entry = jv_object_set(entry, jv_string("value"),
                          record->has_value ? tat_value_or_null(&record->value) : jv_null());
    entry = jv_object_set(entry, jv_string("has_value"), jv_bool(record->has_value));
    tat_journal(instrument, "log", entry);
}

static const ta_log_sink_vtable TAT_RECORDING_LOG = {.write = tat_recording_log};

bool tat_make_recording_log(tat_bind *bind, jv config, void *out) {
    bool created = false;
    tat_instrument *instrument = tat_instrument_resolve(bind, config, TAT_INSTRUMENT_LOG, NULL, true, &created);
    if (instrument == NULL) {
        return false;
    }
    *(ta_log_sink *)out = (ta_log_sink){.self = instrument, .v = &TAT_RECORDING_LOG};
    return true;
}

/* ------------------------------------------------------------------------
 * Host methods
 */

static jv tat_invalid(int *error_code, jv message) {
    *error_code = RPC_INVALID_PARAMS;
    return message;
}

/* params.name resolved to an instrument, or null with *error set. */
static tat_instrument *tat_named(jv params, int *error_code, jv *error) {
    jv name = jv_object_get(jv_copy(params), jv_string("name"));
    if (jv_get_kind(name) != JV_KIND_STRING) {
        jv_free(name);
        *error = tat_invalid(error_code, jv_string("params.name must be an instrument name"));
        return NULL;
    }
    tat_instrument *instrument = tat_instrument_find(jv_string_value(name));
    if (instrument == NULL) {
        *error = tat_invalid(error_code, jv_string_fmt("no instrument named \"%s\"", jv_string_value(name)));
    }
    jv_free(name);
    return instrument;
}

jv tat_instrument_list_method(tat_bind *bind, jv params, int *error_code) {
    (void)bind;
    (void)params;
    (void)error_code;
    jv list = jv_array();
    for (size_t index = 0; index < tat_instrument_count; index += 1) {
        jv record = jv_object_set(jv_object(), jv_string("name"), jv_string(tat_instruments[index]->name));
        record = jv_object_set(record, jv_string("kind"), jv_string(TAT_INSTRUMENT_KINDS[tat_instruments[index]->kind]));
        list = jv_array_append(list, record);
    }
    return list;
}

/* {"name", "since"?: seq}: journal entries with a sequence above `since`. */
jv tat_instrument_journal_method(tat_bind *bind, jv params, int *error_code) {
    (void)bind;
    jv error;
    tat_instrument *instrument = tat_named(params, error_code, &error);
    if (instrument == NULL) {
        return error;
    }
    jv since = jv_object_get(jv_copy(params), jv_string("since"));
    double after = jv_get_kind(since) == JV_KIND_NUMBER ? jv_number_value(since) : 0;
    jv_free(since);
    jv entries = jv_array();
    jv_array_foreach(instrument->journal, index, entry) {
        jv seq = jv_object_get(jv_copy(entry), jv_string("seq"));
        bool newer = jv_number_value(seq) > after;
        jv_free(seq);
        if (newer) {
            entries = jv_array_append(entries, entry);
        } else {
            jv_free(entry);
        }
        (void)index;
    }
    return entries;
}

static bool tat_arg_u64(tat_bind *bind, jv args, const char *name, uint64_t *out) {
    jv value = jv_object_get(jv_copy(args), jv_string(name));
    bool ok = jv_is_valid(value) && jv_get_kind(value) != JV_KIND_NULL && tat_decode_u64(bind, value, out);
    jv_free(value);
    return ok;
}

/* "fail" for any instrument but the allocator: arms a fault for args.op. */
static jv tat_arm_fault(tat_instrument *instrument, const char *const *ops, jv args, int *error_code) {
    jv op = jv_object_get(jv_copy(args), jv_string("op"));
    bool known = false;
    for (size_t index = 0; jv_get_kind(op) == JV_KIND_STRING && ops[index] != NULL; index += 1) {
        known = known || strcmp(ops[index], jv_string_value(op)) == 0;
    }
    if (!known) {
        jv_free(op);
        return tat_invalid(error_code, jv_string_fmt("%s: fail needs args.op, one of its operations",
                                                     TAT_INSTRUMENT_KINDS[instrument->kind]));
    }
    jv result = jv_object_get(jv_copy(args), jv_string("result"));
    ta_result parsed;
    if (jv_get_kind(result) == JV_KIND_STRING && !tat_result_parse(jv_string_value(result), &parsed)) {
        jv_free(op);
        jv_free(result);
        return tat_invalid(error_code, jv_string("fail: args.result must be a ta_result name such as \"err_busy\""));
    }
    jv_free(result);
    jv count = jv_object_get(jv_copy(args), jv_string("count"));
    double remaining = jv_get_kind(count) == JV_KIND_NUMBER ? jv_number_value(count) : 1;
    jv_free(count);
    jv spec = jv_object_delete(jv_object_delete(jv_copy(args), jv_string("op")), jv_string("count"));
    spec = jv_object_set(spec, jv_string("remaining"), jv_number(remaining));
    instrument->faults = jv_object_set(instrument->faults, op, spec);
    return jv_copy(instrument->faults);
}

static jv tat_control_allocator(tat_instrument *instrument, const char *command, jv args, int *error_code) {
    tat_allocator_state *state = &instrument->allocator;
    if (strcmp(command, "stats") == 0) {
        return tat_allocator_stats(state);
    }
    if (strcmp(command, "fail") == 0) {
        uint64_t after = 0;
        uint64_t count = 1;
        (void)tat_arg_u64(instrument->bind, args, "after", &after);
        (void)tat_arg_u64(instrument->bind, args, "count", &count);
        state->fail_after = (int64_t)after;
        state->fail_remaining = (int64_t)count;
        return tat_allocator_stats(state);
    }
    if (strcmp(command, "clear_faults") == 0) {
        state->fail_after = -1;
        return tat_allocator_stats(state);
    }
    return tat_invalid(error_code, jv_string("allocator.counting commands: stats, fail {after, count}, clear_faults"));
}

static jv tat_control_router(tat_instrument *instrument, const char *command, jv args, int *error_code) {
    static const char *const ops[] = {"submit", NULL};
    tat_router_state *state = &instrument->router;
    if (strcmp(command, "fail") == 0) {
        return tat_arm_fault(instrument, ops, args, error_code);
    }
    if (strcmp(command, "mode") == 0) {
        jv mode = jv_object_get(jv_copy(args), jv_string("mode"));
        bool ok = tat_router_set_mode(instrument->bind, instrument, mode);
        jv_free(mode);
        return ok ? jv_string(state->hold ? "hold" : "direct") : tat_bind_take_error(instrument->bind, error_code);
    }
    if (strcmp(command, "held") == 0) {
        jv list = jv_array();
        for (size_t index = 0; index < state->held_count; index += 1) {
            list = jv_array_append(list, tat_held_record(instrument, &state->held[index]));
        }
        return list;
    }
    if (strcmp(command, "release") == 0 || strcmp(command, "drop") == 0) {
        uint64_t id = 0;
        size_t index = 0;
        if (!tat_arg_u64(instrument->bind, args, "id", &id) || tat_held_find(state, id, &index) == NULL) {
            return tat_invalid(error_code, jv_string_fmt("router.recording %s: args.id names no held delivery", command));
        }
        jv entry = tat_held_record(instrument, &state->held[index]);
        if (strcmp(command, "drop") == 0) {
            tat_held_remove(state, index);
            tat_journal(instrument, "drop", jv_copy(entry));
            return entry;
        }
        ta_result result = tat_router_deliver(instrument, &state->held[index].delivery, &entry);
        /* A busy actor keeps the delivery held; release it again later. */
        if (result != TA_ERR_BUSY) {
            tat_held_remove(state, index);
        }
        entry = jv_object_set(entry, jv_string("result"), tat_result_name(result));
        tat_journal(instrument, "release", jv_copy(entry));
        return entry;
    }
    if (strcmp(command, "clear_faults") == 0) {
        jv_free(instrument->faults);
        instrument->faults = jv_object();
        return jv_object();
    }
    return tat_invalid(error_code,
                       jv_string("router.recording commands: mode {mode}, held, release {id}, drop {id}, fail {op, ...}, clear_faults"));
}

static bool tat_arg_address(tat_bind *bind, jv args, ta_actor_address *out) {
    return tat_decode_ta_actor_address(bind, args, out);
}

static jv tat_control_directory(tat_instrument *instrument, const char *command, jv args, int *error_code) {
    static const char *const ops[] = {"claim", "resolve", "replace", "remove", NULL};
    if (strcmp(command, "fail") == 0) {
        return tat_arm_fault(instrument, ops, args, error_code);
    }
    if (strcmp(command, "entries") == 0) {
        jv list = jv_array();
        jv_object_foreach(instrument->directory.entries, key, value) {
            jv_free(key);
            list = jv_array_append(list, value);
        }
        return list;
    }
    if (strcmp(command, "put") == 0) {
        /* Changes an entry behind core's back, as another process would. */
        ta_actor_address address;
        jv where = jv_object();
        jv family = jv_object_get(jv_copy(args), jv_string("family"));
        jv name = jv_object_get(jv_copy(args), jv_string("name"));
        where = jv_object_set(where, jv_string("family"), jv_object_set(jv_object(), jv_string("name"), family));
        where = jv_object_set(where, jv_string("name"), name);
        bool ok = tat_arg_address(instrument->bind, where, &address);
        jv_free(where);
        if (!ok) {
            jv_free(tat_bind_take_error(instrument->bind, error_code));
            return tat_invalid(error_code, jv_string("directory.memory put: args needs family and name strings"));
        }
        jv kind = jv_object_get(jv_copy(args), jv_string("kind"));
        ta_actor_directory_entry entry = {.kind = TA_ACTOR_DIRECTORY_NONE};
        bool known = false;
        for (size_t index = 0; jv_get_kind(kind) == JV_KIND_STRING && index < 3; index += 1) {
            if (strcmp(TAT_ENTRY_KINDS[index], jv_string_value(kind)) == 0) {
                entry.kind = (ta_actor_directory_entry_kind)index;
                known = true;
            }
        }
        jv_free(kind);
        if (!known) {
            return tat_invalid(error_code, jv_string("directory.memory put: args.kind is none, resident, or unloaded"));
        }
        if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT && !tat_arg_u64(instrument->bind, args, "actor", &entry.actor)) {
            return tat_invalid(error_code, jv_string("directory.memory put: a resident entry needs args.actor"));
        }
        tat_directory_store(instrument, &address, entry);
        return tat_entry_record(entry);
    }
    if (strcmp(command, "clear_faults") == 0) {
        jv_free(instrument->faults);
        instrument->faults = jv_object();
        return jv_object();
    }
    return tat_invalid(error_code, jv_string("directory.memory commands: entries, put {family, name, kind, actor?}, fail {op, ...}, clear_faults"));
}

static jv tat_control_invoker(tat_instrument *instrument, const char *command, jv args, int *error_code) {
    static const char *const ops[] = {"start", "cancel", "forward", NULL};
    if (strcmp(command, "fail") == 0) {
        return tat_arm_fault(instrument, ops, args, error_code);
    }
    if (strcmp(command, "services") == 0) {
        jv list = jv_array();
        jv_object_foreach(instrument->invoker.services, key, value) {
            jv_free(key);
            list = jv_array_append(list, value);
        }
        return list;
    }
    if (strcmp(command, "clear_faults") == 0) {
        jv_free(instrument->faults);
        instrument->faults = jv_object();
        return jv_object();
    }
    return tat_invalid(error_code, jv_string("invoker.scripted commands: services, fail {op, ...}, clear_faults"));
}

static jv tat_control_log(tat_instrument *instrument, const char *command, jv args, int *error_code) {
    (void)instrument;
    (void)command;
    (void)args;
    return tat_invalid(error_code,
                       jv_string("log.recording has no commands; read its journal with instrument.journal"));
}

static jv tat_control_io(tat_instrument *instrument, const char *command, jv args, int *error_code) {
    static const char *const ops[] = {"send", NULL};
    if (strcmp(command, "fail") == 0) {
        return tat_arm_fault(instrument, ops, args, error_code);
    }
    if (strcmp(command, "clear_faults") == 0) {
        jv_free(instrument->faults);
        instrument->faults = jv_object();
        return jv_object();
    }
    return tat_invalid(error_code, jv_string("io.recording commands: fail {op, ...}, clear_faults"));
}

/* {"name", "command", "args"?}: steers one instrument between steps. */
jv tat_instrument_control_method(tat_bind *bind, jv params, int *error_code) {
    (void)bind;
    jv error;
    tat_instrument *instrument = tat_named(params, error_code, &error);
    if (instrument == NULL) {
        return error;
    }
    jv command = jv_object_get(jv_copy(params), jv_string("command"));
    if (jv_get_kind(command) != JV_KIND_STRING) {
        jv_free(command);
        return tat_invalid(error_code, jv_string("params.command must be a string"));
    }
    jv args = jv_object_get(jv_copy(params), jv_string("args"));
    if (!jv_is_valid(args) || jv_get_kind(args) == JV_KIND_NULL) {
        jv_free(args);
        args = jv_object();
    }
    if (jv_get_kind(args) != JV_KIND_OBJECT) {
        jv_free(args);
        jv_free(command);
        return tat_invalid(error_code, jv_string("params.args must be an object"));
    }
    jv result;
    switch (instrument->kind) {
        case TAT_INSTRUMENT_ALLOCATOR:
            result = tat_control_allocator(instrument, jv_string_value(command), args, error_code);
            break;
        case TAT_INSTRUMENT_ROUTER:
            result = tat_control_router(instrument, jv_string_value(command), args, error_code);
            break;
        case TAT_INSTRUMENT_DIRECTORY:
            result = tat_control_directory(instrument, jv_string_value(command), args, error_code);
            break;
        case TAT_INSTRUMENT_INVOKER:
            result = tat_control_invoker(instrument, jv_string_value(command), args, error_code);
            break;
        case TAT_INSTRUMENT_LOG:
            result = tat_control_log(instrument, jv_string_value(command), args, error_code);
            break;
        case TAT_INSTRUMENT_IO:
        default:
            result = tat_control_io(instrument, jv_string_value(command), args, error_code);
            break;
    }
    jv_free(args);
    jv_free(command);
    return result;
}
