#include "inputs.h"

#include <string.h>

#include "allocator.h"
#include "scheduler.h"
#include "system.h"

/* The arena's block: a turn's wake addresses usually fit in one. */
#define TNA_INPUTS_BLOCK_BYTES 4096u

/* Record layout (core ADR 0090 section 3): the header's offsets. */
#define TNA_RECORD_HEADER 32u
#define TNA_RECORD_KIND_AT 2u
#define TNA_RECORD_FIXED_AT 4u
#define TNA_RECORD_TEXT_AT 24u
#define TNA_RECORD_VALUES_AT 28u
/* A checkpoint's part and owed counts: its eighth and ninth fields (core ADR 0091). */
#define TNA_CHECKPOINT_PARTS_FIELD 7u
#define TNA_CHECKPOINT_OWED_FIELD 8u

static uint64_t tna_le(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = count; index > 0; index -= 1u) {
        value = value << 8 | bytes[index - 1u];
    }
    return value;
}

static size_t tna_pad8(size_t length) {
    return (length + 7u) & ~(size_t)7u;
}

/* One record's span, or 0 when it is not all there. */
static size_t tna_one_record_span(const uint8_t *bytes, size_t length) {
    if (length < TNA_RECORD_HEADER) {
        return 0;
    }
    size_t at = (size_t)tna_le(bytes + TNA_RECORD_FIXED_AT, 4);
    uint64_t text = tna_le(bytes + TNA_RECORD_TEXT_AT, 4);
    uint32_t values = (uint32_t)tna_le(bytes + TNA_RECORD_VALUES_AT, 2);
    if (at < TNA_RECORD_HEADER) {
        return length; /* not a record: hand it to core, which says so */
    }
    at += tna_pad8((size_t)text);
    for (uint32_t value = 0; value < values; value += 1u) {
        for (;;) {
            if (at + 4u > length) {
                return 0;
            }
            uint64_t chunk = tna_le(bytes + at, 4);
            at += chunk == 0 ? 8u : tna_pad8(4u + (size_t)chunk);
            if (chunk == 0) {
                break;
            }
        }
    }
    return at <= length ? at : 0;
}

size_t tna_record_span(const uint8_t *bytes, size_t length, bool unit) {
    size_t first = tna_one_record_span(bytes, length);
    if (first == 0 || !unit || tna_le(bytes + TNA_RECORD_KIND_AT, 2) != TA_RECORD_CHECKPOINT) {
        return first;
    }
    size_t fixed = (size_t)tna_le(bytes + TNA_RECORD_FIXED_AT, 4);
    size_t owed_at = TNA_RECORD_HEADER + 8u * TNA_CHECKPOINT_OWED_FIELD;
    if (fixed < owed_at + 8u) {
        return first;
    }
    uint64_t more = tna_le(bytes + TNA_RECORD_HEADER + 8u * TNA_CHECKPOINT_PARTS_FIELD, 8) +
                    tna_le(bytes + owed_at, 8);
    size_t at = first;
    for (uint64_t record = 0; record < more; record += 1u) {
        size_t span = tna_one_record_span(bytes + at, length - at);
        if (span == 0) {
            return 0;
        }
        at += span;
    }
    return at;
}

void tna_inputs_init(tna_inputs *inputs, ta_allocator allocator) {
    *inputs = (tna_inputs){ .allocator = allocator };
    tna_vec_init(&inputs->families, allocator, TNA_LAYOUT(ta_str));
    tna_vec_init(&inputs->entries, allocator, TNA_LAYOUT(tna_input_entry));
    tna_map_init(&inputs->entry_index, allocator);
    tna_vec_init(&inputs->spare, allocator, TNA_LAYOUT(tna_vec));
    tna_map_init(&inputs->seqs, allocator);
    tna_map_init(&inputs->queued_address, allocator);
    tna_map_init(&inputs->mail_count, allocator);
    tna_map_init(&inputs->mail_since, allocator);
    tna_vec_init(&inputs->touched, allocator, TNA_LAYOUT(tna_input_touch));
    tna_map_init(&inputs->touched_index, allocator);
}

/* Forgets every queued message and waiting count: the log was turned off or
 * on again, and a message queued before now has no queued record. */
static void tna_inputs_forget_mail(tna_inputs *inputs) {
    uint32_t cursor = 0;
    uint64_t key = 0;
    uint64_t value = 0;
    while (tna_map_next(&inputs->queued_address, &cursor, &key, &value)) {
        ta_address_release((ta_address *)(uintptr_t)value);
    }
    tna_map_destroy(&inputs->queued_address);
    tna_map_init(&inputs->queued_address, inputs->allocator);
    tna_map_destroy(&inputs->seqs);
    tna_map_init(&inputs->seqs, inputs->allocator);
    tna_map_destroy(&inputs->mail_count);
    tna_map_init(&inputs->mail_count, inputs->allocator);
    tna_map_destroy(&inputs->mail_since);
    tna_map_init(&inputs->mail_since, inputs->allocator);
}

/* Forgets this turn's touched addresses. */
static void tna_inputs_forget_touched(tna_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->touched.count; at += 1u) {
        ta_address_release(((tna_input_touch *)tna_vec_at(&inputs->touched, at))->address);
    }
    tna_vec_clear(&inputs->touched);
    tna_map_destroy(&inputs->touched_index);
    tna_map_init(&inputs->touched_index, inputs->allocator);
}

static void tna_inputs_free_families(tna_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        ta_str *family = tna_vec_at(&inputs->families, at);
        tna_free(inputs->allocator, &(ta_allocator_free_params){
                                        .ptr = (void *)family->ptr, .size = (size_t)family->len + 1u, .alignment = 1 });
    }
    tna_vec_clear(&inputs->families);
}

void tna_inputs_release(tna_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->entries.count; at += 1u) {
        tna_input_entry *entry = tna_vec_at(&inputs->entries, at);
        ta_address_release(entry->address);
        entry->bytes.count = 0;
        if (tna_vec_push(&inputs->spare, &entry->bytes) != TA_OK) {
            tna_vec_destroy(&entry->bytes);
        }
    }
    tna_vec_clear(&inputs->entries);
    tna_map_destroy(&inputs->entry_index);
    tna_map_init(&inputs->entry_index, inputs->allocator);
    inputs->count = 0;
    if (inputs->arena_ready) {
        ta_arena_reset(&inputs->arena);
    }
}

void tna_inputs_destroy(tna_system *system) {
    tna_inputs *inputs = &system->inputs;
    if (inputs->enabled && system->core != NULL) {
        ta_system_set_input_sink(system->core, (ta_byte_sink){ 0 });
    }
    tna_inputs_release(inputs);
    for (uint32_t at = 0; at < inputs->spare.count; at += 1u) {
        tna_vec_destroy(tna_vec_at(&inputs->spare, at));
    }
    tna_vec_destroy(&inputs->spare);
    tna_vec_destroy(&inputs->entries);
    tna_map_destroy(&inputs->entry_index);
    tna_inputs_free_families(inputs);
    tna_vec_destroy(&inputs->families);
    tna_inputs_forget_mail(inputs);
    tna_map_destroy(&inputs->seqs);
    tna_map_destroy(&inputs->queued_address);
    tna_map_destroy(&inputs->mail_count);
    tna_map_destroy(&inputs->mail_since);
    tna_inputs_forget_touched(inputs);
    tna_vec_destroy(&inputs->touched);
    tna_map_destroy(&inputs->touched_index);
    if (inputs->arena_ready) {
        ta_arena_destroy(&inputs->arena);
        inputs->arena_ready = false;
    }
    inputs->enabled = false;
}

static bool tna_inputs_family_listed(const tna_inputs *inputs, ta_str family) {
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        const ta_str *listed = tna_vec_at(&inputs->families, at);
        if (listed->len == family.len && memcmp(listed->ptr, family.ptr, family.len) == 0) {
            return true;
        }
    }
    return false;
}

/* Marks `address` touched this turn; `fixed` gives its wake outright. */
static void tna_inputs_touch(tna_inputs *inputs, ta_address *address, bool fixed, bool has_at, ta_time at) {
    if (address == NULL) {
        return;
    }
    uint64_t index = 0;
    tna_input_touch *touch = NULL;
    if (tna_map_get(&inputs->touched_index, (uint64_t)(uintptr_t)address, &index)) {
        touch = tna_vec_at(&inputs->touched, (uint32_t)index);
    } else {
        tna_input_touch fresh = { .address = address };
        if (tna_vec_push(&inputs->touched, &fresh) != TA_OK) {
            inputs->lost += 1u; /* its wake is not reported this turn */
            return;
        }
        if (tna_map_put(&inputs->touched_index, (uint64_t)(uintptr_t)address, inputs->touched.count - 1u) != TA_OK) {
            inputs->touched.count -= 1u;
            inputs->lost += 1u;
            return;
        }
        ta_address_retain(address);
        touch = tna_vec_at(&inputs->touched, inputs->touched.count - 1u);
    }
    if (fixed) {
        touch->fixed = true;
        touch->has_at = has_at;
        touch->at = at;
    }
}

/* ------------------------------------------------------------------------
 * The sink core writes records into
 */

/* The entry for `address` this turn, made when it has none; NULL without memory. */
static tna_input_entry *tna_inputs_entry(tna_inputs *inputs, ta_address *address) {
    uint64_t index = 0;
    if (tna_map_get(&inputs->entry_index, (uint64_t)(uintptr_t)address, &index)) {
        return tna_vec_at(&inputs->entries, (uint32_t)index);
    }
    tna_input_entry entry = { .address = address };
    if (inputs->spare.count != 0) {
        inputs->spare.count -= 1u;
        entry.bytes = *(tna_vec *)tna_vec_at(&inputs->spare, inputs->spare.count);
    } else {
        tna_vec_init(&entry.bytes, inputs->allocator, TNA_LAYOUT(uint8_t));
    }
    if (tna_vec_push(&inputs->entries, &entry) != TA_OK) {
        tna_vec_destroy(&entry.bytes);
        return NULL;
    }
    if (tna_map_put(&inputs->entry_index, (uint64_t)(uintptr_t)address, inputs->entries.count - 1u) != TA_OK) {
        inputs->entries.count -= 1u;
        tna_vec_destroy(&entry.bytes);
        return NULL;
    }
    ta_address_retain(address);
    return tna_vec_at(&inputs->entries, inputs->entries.count - 1u);
}

static ta_op_status tna_input_sink_begin(void *self) {
    tna_system *system = self;
    tna_inputs *inputs = &system->inputs;
    ta_address *address = inputs->host_address;
    ta_address *interned = NULL;
    if (address == NULL) {
        ta_input_owner owner;
        if (ta_system_input_owner(system->core, &owner) != TA_OK || owner.name.name.len == 0) {
            return TA_OP_NOMEM; /* core names every actor whose record it writes */
        }
        address = owner.address;
        /* A spawn's SPAWNED comes before it claims the address: its handle, by name. */
        if (address == NULL && ta_address_intern(system->core, &owner.name, &interned) != TA_OK) {
            return TA_OP_NOMEM;
        }
        address = address != NULL ? address : interned;
    }
    tna_input_entry *entry = tna_inputs_entry(inputs, address);
    if (entry == NULL) {
        ta_address_release(interned);
        return TA_OP_NOMEM;
    }
    inputs->open = true;
    inputs->open_entry = (uint32_t)(entry - (tna_input_entry *)inputs->entries.items);
    inputs->open_start = entry->bytes.count;
    tna_inputs_touch(inputs, address, false, false, 0);
    ta_address_release(interned); /* the entry and the touch hold it now */
    return TA_OP_OK;
}

static ta_op_status tna_input_sink_write(void *self, ta_bytes bytes) {
    tna_system *system = self;
    tna_inputs *inputs = &system->inputs;
    if (!inputs->open) {
        return TA_OP_ERROR;
    }
    tna_input_entry *entry = tna_vec_at(&inputs->entries, inputs->open_entry);
    if (bytes.len > UINT32_MAX - entry->bytes.count || tna_vec_reserve(&entry->bytes, entry->bytes.count + bytes.len) != TA_OK) {
        return TA_OP_NOMEM;
    }
    memcpy((uint8_t *)entry->bytes.items + entry->bytes.count, bytes.ptr, bytes.len);
    entry->bytes.count += bytes.len;
    return TA_OP_OK;
}

static void tna_input_sink_end(void *self, bool keep) {
    tna_system *system = self;
    tna_inputs *inputs = &system->inputs;
    if (!inputs->open) {
        return;
    }
    inputs->open = false;
    tna_input_entry *entry = tna_vec_at(&inputs->entries, inputs->open_entry);
    if (!keep) {
        entry->bytes.count = inputs->open_start;
        return;
    }
    inputs->count += 1u;
    tna_system_want_turn(system); /* the batch goes out after the next turn */
}

static const ta_byte_sink_vtable tna_input_sink_vtable = {
    .begin = tna_input_sink_begin,
    .write = tna_input_sink_write,
    .end = tna_input_sink_end,
};

/* ------------------------------------------------------------------------
 * Configuration
 */

/* A 64-bit mix of the incarnation (SplitMix64's finalizer). */
static uint64_t tna_mix(uint64_t value) {
    value += 0x9e3779b97f4a7c15u;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9u;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebu;
    return value ^ (value >> 31);
}

ta_result tna_inputs_configure(tna_system *system, const ta_str *families, uint32_t count) {
    tna_inputs *inputs = &system->inputs;
    if (count > 0 && !inputs->arena_ready) {
        if (ta_arena_init(&inputs->arena, inputs->allocator, TNA_INPUTS_BLOCK_BYTES) != TA_OK) {
            return TA_ERR_NOMEM;
        }
        inputs->arena_ready = true;
    }
    tna_vec copies;
    tna_vec_init(&copies, inputs->allocator, TNA_LAYOUT(ta_str));
    ta_result result = tna_vec_reserve(&copies, count);
    for (uint32_t at = 0; result == TA_OK && at < count; at += 1u) {
        char *text = tna_alloc(inputs->allocator, (size_t)families[at].len + 1u, 1);
        if (text == NULL) {
            result = TA_ERR_NOMEM;
            break;
        }
        memcpy(text, families[at].ptr, families[at].len);
        text[families[at].len] = '\0';
        ta_str copy = { .ptr = text, .len = families[at].len };
        (void)tna_vec_push(&copies, &copy);
    }
    for (uint32_t at = 0; result == TA_OK && at < count; at += 1u) {
        result = ta_system_log_family(system->core, families[at], true);
    }
    if (result != TA_OK) {
        tna_vec old = inputs->families;
        inputs->families = copies;
        tna_inputs_free_families(inputs);
        tna_vec_destroy(&inputs->families);
        inputs->families = old;
        for (uint32_t at = 0; at < count; at += 1u) {
            if (!tna_inputs_family_listed(inputs, families[at])) {
                (void)ta_system_log_family(system->core, families[at], false);
            }
        }
        return result;
    }
    /* Families no longer listed stop being logged. */
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        ta_str *old = tna_vec_at(&inputs->families, at);
        bool kept = false;
        for (uint32_t index = 0; index < count && !kept; index += 1u) {
            kept = families[index].len == old->len && memcmp(families[index].ptr, old->ptr, old->len) == 0;
        }
        if (!kept) {
            (void)ta_system_log_family(system->core, *old, false);
        }
    }
    tna_inputs_free_families(inputs);
    tna_vec_destroy(&inputs->families);
    inputs->families = copies;
    inputs->enabled = count > 0;
    if (inputs->seq_base == 0) {
        inputs->seq_base = tna_mix(ta_system_incarnation(system->core)) & 0xffffffff00000000u;
        inputs->seq_base = inputs->seq_base == 0 ? (uint64_t)1 << 32 : inputs->seq_base;
        inputs->next_seq = inputs->seq_base;
    }
    /* A message queued before now has no queued record: its event has no host number. */
    tna_inputs_forget_mail(inputs);
    tna_sched_forget_logging(&system->sched);
    ta_system_set_input_sink(system->core, inputs->enabled
                                               ? (ta_byte_sink){ .self = system, .v = &tna_input_sink_vtable }
                                               : (ta_byte_sink){ 0 });
    system->sched.observer = inputs->enabled
                                 ? (tna_sched_observer){ .self = system, .note = tna_inputs_note,
                                                         .host_seq = tna_inputs_host_seq }
                                 : (tna_sched_observer){ 0 };
    return TA_OK;
}

bool tna_inputs_seq_is_ours(const tna_inputs *inputs, uint64_t seq) {
    return inputs->seq_base != 0 && (seq & 0xffffffff00000000u) == inputs->seq_base;
}

/* tna_inputs_logged(), also answering the address's handle (borrowed). */
static bool tna_inputs_logged_handle(tna_system *system, ta_actor_id actor, ta_actor_address *address,
                                     ta_address **handle) {
    tna_sched_entry *entry = tna_sched_lookup(&system->sched, actor);
    if (!system->inputs.enabled || (entry != NULL && (entry->flags & TNA_SCHED_LOG_KNOWN) != 0 &&
                                    (entry->flags & TNA_SCHED_LOGGED) == 0)) {
        return false;
    }
    /* Read-only, so it may run inside a router callback (the target of mail). */
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, actor, &view) != TA_OK) {
        return false;
    }
    /* An unpublished actor, a replay's above all, is logged by nothing it
     * does (napi ADR 0032 section 7). Not cached: once published it is. */
    if (!view.published) {
        return false;
    }
    bool logged = view.self.kind == TA_ACTOR_REF_NAMED &&
                  tna_inputs_family_listed(&system->inputs, view.definition.family.name);
    if (entry != NULL) {
        entry->flags |= TNA_SCHED_LOG_KNOWN;
        entry->flags = logged ? (uint8_t)(entry->flags | TNA_SCHED_LOGGED) : (uint8_t)(entry->flags & ~TNA_SCHED_LOGGED);
    }
    if (logged && address != NULL) {
        *address = ta_address_view(view.self.as.named);
    }
    if (logged && handle != NULL) {
        *handle = view.self.as.named;
    }
    return logged;
}

bool tna_inputs_logged(tna_system *system, ta_actor_id actor, ta_actor_address *address) {
    return tna_inputs_logged_handle(system, actor, address, NULL);
}

/* ------------------------------------------------------------------------
 * The mailbox's records
 */

/* One more queued message waiting for `address`, accepted at `since`. */
static void tna_inputs_mail_add(tna_inputs *inputs, ta_address *address, ta_time since) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t count = 0;
    (void)tna_map_get(&inputs->mail_count, key, &count);
    if (count == 0 && tna_map_put(&inputs->mail_since, key, since) != TA_OK) {
        inputs->lost += 1u;
    }
    if (tna_map_put(&inputs->mail_count, key, count + 1u) != TA_OK) {
        inputs->lost += 1u;
    }
}

/* One queued message for `address` delivered or dropped. */
static void tna_inputs_mail_done(tna_inputs *inputs, ta_address *address) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t count = 0;
    if (!tna_map_get(&inputs->mail_count, key, &count)) {
        return;
    }
    if (count <= 1u) {
        (void)tna_map_remove(&inputs->mail_count, key);
        (void)tna_map_remove(&inputs->mail_since, key);
    } else {
        (void)tna_map_put(&inputs->mail_count, key, count - 1u);
    }
}

/* The queued message behind `envelope`, if the log numbered it: its host
 * number and address (retained, the caller releases it), out of the maps. */
static bool tna_inputs_unqueue(tna_inputs *inputs, uint32_t envelope, uint64_t *seq, ta_address **address) {
    if (!tna_map_get(&inputs->seqs, envelope, seq)) {
        return false;
    }
    (void)tna_map_remove(&inputs->seqs, envelope);
    uint64_t value = 0;
    *address = NULL;
    if (tna_map_get(&inputs->queued_address, envelope, &value)) {
        (void)tna_map_remove(&inputs->queued_address, envelope);
        *address = (ta_address *)(uintptr_t)value;
        tna_inputs_mail_done(inputs, *address);
    }
    return true;
}

/* A definition of `family` to write a waiting message's data with: its
 * data model encodes the value. 0 when the system holds none. */
static ta_definition_id tna_inputs_family_definition(tna_system *system, ta_str family) {
    ta_definition_id page[64];
    uint32_t from = 0;
    for (;;) {
        uint32_t count = 0;
        ta_result result = ta_system_definitions(system->core, &(ta_system_definitions_params){
                                                                  .from = from, .limit = 64, .definitions = page,
                                                                  .capacity = 64, .count = &count });
        if (result != TA_OK) {
            return 0;
        }
        for (uint32_t index = 0; index < count; index += 1u) {
            ta_definition_inspection view;
            if (ta_definition_inspect(system->core, page[index], &view) == TA_OK &&
                view.identity.family.name.len == family.len &&
                memcmp(view.identity.family.name.ptr, family.ptr, family.len) == 0) {
                return page[index];
            }
        }
        if (count < 64) {
            return 0;
        }
        from = ta_actor_id_slot(page[count - 1u]) + 1u;
    }
}

/* The resident, published actor at `address`, or 0. */
static ta_actor_id tna_inputs_resident(tna_system *system, ta_address *address) {
    ta_actor_directory_entry entry = tna_directory_find(&system->directory, address);
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
        return 0;
    }
    ta_actor_inspection view;
    return ta_actor_inspect(system->core, entry.actor, &view) == TA_OK && view.published ? entry.actor : 0;
}

/* Writes a host record for `address`; `actor` 0 names none. */
static ta_result tna_inputs_host_record(tna_system *system, ta_address *address, const ta_host_record_params *params) {
    tna_inputs *inputs = &system->inputs;
    ta_actor_address view = ta_address_view(address);
    ta_host_record_params record = *params;
    record.address = &view;
    ta_address *previous = inputs->host_address;
    inputs->host_address = address;
    bool inside = system->inside_core;
    system->inside_core = true;
    ta_result result = ta_system_write_host_record(system->core, &record);
    system->inside_core = inside;
    inputs->host_address = previous;
    return result;
}

static ta_result tna_inputs_queued(tna_system *system, const tna_sched_note *note) {
    tna_inputs *inputs = &system->inputs;
    const tna_envelope *envelope = note->message;
    if (envelope == NULL || envelope->report != TNA_REPORT_NONE) {
        return TA_OK; /* core writes a report as it takes it */
    }
    ta_address *handle = NULL;
    ta_actor_id actor = 0;
    if (note->address != NULL) {
        handle = note->address;
        if (!tna_inputs_family_listed(inputs, ta_address_view(handle).family.name)) {
            return TA_OK;
        }
    } else if (tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
        actor = note->actor;
    } else {
        return TA_OK;
    }
    bool owed = inputs->redelivering;
    ta_time since = owed ? inputs->redeliver_since : ta_system_time(system->core);
    uint64_t seq = owed ? inputs->redeliver_seq : inputs->next_seq + 1u;
    inputs->redelivering = false;
    if (!owed) {
        ta_definition_id definition = 0;
        if (actor == 0 && envelope->event.data.v != NULL) {
            definition = tna_inputs_family_definition(system, ta_address_view(handle).family.name);
        }
        ta_result result = tna_inputs_host_record(system, handle, &(ta_host_record_params){
                                                                       .kind = TA_RECORD_QUEUED,
                                                                       .host_seq = seq,
                                                                       .actor = actor,
                                                                       .definition = definition,
                                                                       .event = &envelope->event,
                                                                   });
        if (result != TA_OK) {
            return result; /* not accepted: the log could not hold it */
        }
        inputs->next_seq = seq;
    }
    tna_inputs_touch(inputs, handle, false, false, 0);
    if (tna_map_put(&inputs->seqs, note->envelope, seq) != TA_OK) {
        inputs->lost += 1u; /* its event will carry no host number */
        return TA_OK;
    }
    if (tna_map_put(&inputs->queued_address, note->envelope, (uint64_t)(uintptr_t)handle) != TA_OK) {
        (void)tna_map_remove(&inputs->seqs, note->envelope);
        inputs->lost += 1u;
        return TA_OK;
    }
    ta_address_retain(handle);
    tna_inputs_mail_add(inputs, handle, since);
    return TA_OK;
}

ta_result tna_inputs_note(void *self, const tna_sched_note *note) {
    tna_system *system = self;
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return TA_OK;
    }
    ta_address *handle = NULL;
    switch (note->kind) {
    case TNA_NOTE_QUEUED:
        return tna_inputs_queued(system, note);
    case TNA_NOTE_DELIVERED: {
        /* Core wrote its `event`, which carries the host number. */
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        if (tna_inputs_unqueue(inputs, note->envelope, &seq, &queued_address)) {
            tna_inputs_touch(inputs, queued_address, false, false, 0);
            ta_address_release(queued_address);
        }
        return TA_OK;
    }
    case TNA_NOTE_DROPPED: {
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        if (!tna_inputs_unqueue(inputs, note->envelope, &seq, &queued_address)) {
            return TA_OK;
        }
        if (queued_address != NULL) {
            if (tna_inputs_host_record(system, queued_address, &(ta_host_record_params){
                                                                   .kind = TA_RECORD_DROPPED,
                                                                   .host_seq = seq,
                                                                   .actor = tna_inputs_resident(system, queued_address),
                                                               }) != TA_OK) {
                inputs->lost += 1u;
            }
            tna_inputs_touch(inputs, queued_address, false, false, 0);
            ta_address_release(queued_address);
        }
        return TA_OK;
    }
    case TNA_NOTE_SETTLED:
    case TNA_NOTE_FINISHED:
        /* Its macrostep may have armed or cancelled sends, or it finished:
         * work its wake out. Core wrote what it accepted. */
        if (tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            tna_inputs_touch(inputs, handle, false, false, 0);
            tna_system_want_turn(system);
        }
        return TA_OK;
    default:
        return TA_OK;
    }
}

uint64_t tna_inputs_host_seq(void *self, uint32_t envelope) {
    tna_system *system = self;
    uint64_t seq = 0;
    return tna_map_get(&system->inputs.seqs, envelope, &seq) ? seq : 0;
}

ta_result tna_inputs_evicted(tna_system *system, ta_actor_id actor, bool has_wake, ta_time wake) {
    ta_address *handle = NULL;
    if (!tna_inputs_logged_handle(system, actor, NULL, &handle)) {
        return TA_OK;
    }
    ta_result result = tna_inputs_host_record(system, handle, &(ta_host_record_params){
                                                                  .kind = TA_RECORD_EVICTED, .actor = actor });
    if (result == TA_OK) {
        tna_inputs_touch(&system->inputs, handle, true, has_wake, wake);
    }
    return result;
}

void tna_inputs_forget(tna_system *system, ta_address *address) {
    if (system->inputs.enabled && address != NULL &&
        tna_inputs_family_listed(&system->inputs, ta_address_view(address).family.name)) {
        tna_inputs_touch(&system->inputs, address, true, false, 0); /* nothing will need it back */
        tna_system_want_turn(system);
    }
}

void tna_inputs_redeliver(tna_inputs *inputs, uint64_t seq, ta_time since) {
    inputs->redelivering = inputs->enabled && seq != 0;
    inputs->redeliver_seq = seq;
    inputs->redeliver_since = since;
}

void tna_inputs_redelivered(tna_inputs *inputs) {
    inputs->redelivering = false;
    inputs->redeliver_seq = 0;
}

/* ------------------------------------------------------------------------
 * Wakes
 */

static bool tna_inputs_copy(tna_inputs *inputs, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(&inputs->arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return true;
}

/* A touched address's wake; false when it has none to report (no resident
 * actor and no queued mail: it keeps the wake it had). */
static bool tna_inputs_wake_of(tna_system *system, const tna_input_touch *touch, bool *has_at, ta_time *at) {
    if (touch->fixed) {
        *has_at = touch->has_at;
        *at = touch->at;
        return true;
    }
    tna_inputs *inputs = &system->inputs;
    uint64_t key = (uint64_t)(uintptr_t)touch->address;
    uint64_t since = 0;
    bool mail = tna_map_get(&inputs->mail_since, key, &since);
    *has_at = mail;
    *at = mail ? (ta_time)since : 0;
    ta_actor_directory_entry entry = tna_directory_find(&system->directory, touch->address);
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
        return mail;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, entry.actor, &view) != TA_OK) {
        return mail;
    }
    if (view.has_completion) {
        *has_at = false; /* finished: nothing will need it again */
        return true;
    }
    for (uint32_t index = 0; index < view.delayed_send_count; index += 1u) {
        ta_time due = view.delayed_sends[index].due;
        if (!*has_at || due < *at) {
            *has_at = true;
            *at = due;
        }
    }
    return true;
}

tna_input_wake *tna_inputs_wakes(tna_system *system, uint32_t *count) {
    tna_inputs *inputs = &system->inputs;
    *count = 0;
    uint32_t touched = inputs->touched.count;
    tna_input_wake *wakes = touched == 0 || !inputs->arena_ready
                                ? NULL
                                : ta_arena_alloc(&inputs->arena, (size_t)touched * sizeof *wakes, _Alignof(tna_input_wake));
    for (uint32_t at = 0; wakes != NULL && at < touched; at += 1u) {
        const tna_input_touch *touch = tna_vec_at(&inputs->touched, at);
        tna_input_wake wake = { 0 };
        if (!tna_inputs_wake_of(system, touch, &wake.has_at, &wake.at)) {
            continue;
        }
        ta_actor_address address = ta_address_view(touch->address);
        if (!tna_inputs_copy(inputs, address.family.name, &wake.address.family.name) ||
            !tna_inputs_copy(inputs, address.name, &wake.address.name)) {
            inputs->lost += 1u;
            continue;
        }
        wakes[(*count)++] = wake;
    }
    if (touched != 0 && wakes == NULL) {
        inputs->lost += 1u;
    }
    tna_inputs_forget_touched(inputs);
    return wakes;
}
