#include "tinyactors_host/inputs.h"

#include "memory.h"


#include "tinyactors_host/allocator.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"

/* The arena's block: a turn's wake addresses usually fit in one. */
#define TAH_INPUTS_BLOCK_BYTES 4096u

/* Record layout (core ADR 0090 section 3): the header's offsets. */
#define TAH_RECORD_HEADER 32u
#define TAH_RECORD_KIND_AT 2u
#define TAH_RECORD_FIXED_AT 4u
#define TAH_RECORD_TEXT_AT 24u
#define TAH_RECORD_VALUES_AT 28u
/* A checkpoint's part and owed counts: its eighth and ninth fields (core ADR 0091). */
#define TAH_CHECKPOINT_PARTS_FIELD 7u
#define TAH_CHECKPOINT_OWED_FIELD 8u

static uint64_t tah_le(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = count; index > 0; index -= 1u) {
        value = value << 8 | bytes[index - 1u];
    }
    return value;
}

static size_t tah_pad8(size_t length) {
    return (length + 7u) & ~(size_t)7u;
}

/* One record's span, or 0 when it is not all there. */
static size_t tah_one_record_span(const uint8_t *bytes, size_t length) {
    if (length < TAH_RECORD_HEADER) {
        return 0;
    }
    size_t at = (size_t)tah_le(bytes + TAH_RECORD_FIXED_AT, 4);
    uint64_t text = tah_le(bytes + TAH_RECORD_TEXT_AT, 4);
    uint32_t values = (uint32_t)tah_le(bytes + TAH_RECORD_VALUES_AT, 2);
    if (at < TAH_RECORD_HEADER) {
        return length; /* not a record: hand it to core, which says so */
    }
    at += tah_pad8((size_t)text);
    for (uint32_t value = 0; value < values; value += 1u) {
        for (;;) {
            if (at + 4u > length) {
                return 0;
            }
            uint64_t chunk = tah_le(bytes + at, 4);
            at += chunk == 0 ? 8u : tah_pad8(4u + (size_t)chunk);
            if (chunk == 0) {
                break;
            }
        }
    }
    return at <= length ? at : 0;
}

size_t tah_record_span(const uint8_t *bytes, size_t length, bool unit) {
    size_t first = tah_one_record_span(bytes, length);
    if (first == 0 || !unit || tah_le(bytes + TAH_RECORD_KIND_AT, 2) != TA_RECORD_CHECKPOINT) {
        return first;
    }
    size_t fixed = (size_t)tah_le(bytes + TAH_RECORD_FIXED_AT, 4);
    size_t owed_at = TAH_RECORD_HEADER + 8u * TAH_CHECKPOINT_OWED_FIELD;
    if (fixed < owed_at + 8u) {
        return first;
    }
    uint64_t more = tah_le(bytes + TAH_RECORD_HEADER + 8u * TAH_CHECKPOINT_PARTS_FIELD, 8) +
                    tah_le(bytes + owed_at, 8);
    size_t at = first;
    for (uint64_t record = 0; record < more; record += 1u) {
        size_t span = tah_one_record_span(bytes + at, length - at);
        if (span == 0) {
            return 0;
        }
        at += span;
    }
    return at;
}

void tah_inputs_init(tah_inputs *inputs, ta_allocator allocator) {
    *inputs = (tah_inputs){ .allocator = allocator };
    tah_vec_init(&inputs->families, allocator, TAH_LAYOUT(ta_str));
    tah_vec_init(&inputs->entries, allocator, TAH_LAYOUT(tah_input_entry));
    tah_map_init(&inputs->entry_index, allocator);
    tah_vec_init(&inputs->spare, allocator, TAH_LAYOUT(tah_vec));
    tah_map_init(&inputs->seqs, allocator);
    tah_map_init(&inputs->queued_address, allocator);
    tah_map_init(&inputs->mail_count, allocator);
    tah_map_init(&inputs->mail_since, allocator);
    tah_vec_init(&inputs->touched, allocator, TAH_LAYOUT(tah_input_touch));
    tah_map_init(&inputs->touched_index, allocator);
}

/* Forgets every queued message and waiting count: the log was turned off or
 * on again, and a message queued before now has no queued record. */
static void tah_inputs_forget_mail(tah_inputs *inputs) {
    uint32_t cursor = 0;
    uint64_t key = 0;
    uint64_t value = 0;
    while (tah_map_next(&inputs->queued_address, &cursor, &key, &value)) {
        ta_address_release((ta_address *)(uintptr_t)value);
    }
    tah_map_destroy(&inputs->queued_address);
    tah_map_init(&inputs->queued_address, inputs->allocator);
    tah_map_destroy(&inputs->seqs);
    tah_map_init(&inputs->seqs, inputs->allocator);
    tah_map_destroy(&inputs->mail_count);
    tah_map_init(&inputs->mail_count, inputs->allocator);
    tah_map_destroy(&inputs->mail_since);
    tah_map_init(&inputs->mail_since, inputs->allocator);
}

/* Forgets this turn's touched addresses. */
static void tah_inputs_forget_touched(tah_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->touched.count; at += 1u) {
        ta_address_release(((tah_input_touch *)tah_vec_at(&inputs->touched, at))->address);
    }
    tah_vec_clear(&inputs->touched);
    tah_map_destroy(&inputs->touched_index);
    tah_map_init(&inputs->touched_index, inputs->allocator);
}

static void tah_inputs_free_families(tah_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        ta_str *family = tah_vec_at(&inputs->families, at);
        tah_free(inputs->allocator, &(ta_allocator_free_params){
                                        .ptr = (void *)family->ptr, .size = (size_t)family->len + 1u, .alignment = 1 });
    }
    tah_vec_clear(&inputs->families);
}

void tah_inputs_release(tah_inputs *inputs) {
    for (uint32_t at = 0; at < inputs->entries.count; at += 1u) {
        tah_input_entry *entry = tah_vec_at(&inputs->entries, at);
        ta_address_release(entry->address);
        entry->bytes.count = 0;
        if (tah_vec_push(&inputs->spare, &entry->bytes) != TA_OK) {
            tah_vec_destroy(&entry->bytes);
        }
    }
    tah_vec_clear(&inputs->entries);
    tah_map_destroy(&inputs->entry_index);
    tah_map_init(&inputs->entry_index, inputs->allocator);
    inputs->count = 0;
    if (inputs->arena_ready) {
        ta_arena_reset(&inputs->arena);
    }
}

void tah_inputs_destroy(tah_system *system) {
    tah_inputs *inputs = &system->inputs;
    if (inputs->enabled && system->core != NULL) {
        ta_system_set_input_sink(system->core, (ta_byte_sink){ 0 });
    }
    tah_inputs_release(inputs);
    for (uint32_t at = 0; at < inputs->spare.count; at += 1u) {
        tah_vec_destroy(tah_vec_at(&inputs->spare, at));
    }
    tah_vec_destroy(&inputs->spare);
    tah_vec_destroy(&inputs->entries);
    tah_map_destroy(&inputs->entry_index);
    tah_inputs_free_families(inputs);
    tah_vec_destroy(&inputs->families);
    tah_inputs_forget_mail(inputs);
    tah_map_destroy(&inputs->seqs);
    tah_map_destroy(&inputs->queued_address);
    tah_map_destroy(&inputs->mail_count);
    tah_map_destroy(&inputs->mail_since);
    tah_inputs_forget_touched(inputs);
    tah_vec_destroy(&inputs->touched);
    tah_map_destroy(&inputs->touched_index);
    if (inputs->arena_ready) {
        ta_arena_destroy(&inputs->arena);
        inputs->arena_ready = false;
    }
    inputs->enabled = false;
}

static bool tah_inputs_family_listed(const tah_inputs *inputs, ta_str family) {
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        const ta_str *listed = tah_vec_at(&inputs->families, at);
        if (listed->len == family.len && tah_mem_equal(listed->ptr, family.ptr, family.len)) {
            return true;
        }
    }
    return false;
}

/* Marks `address` touched this turn; `fixed` gives its wake outright. */
static void tah_inputs_touch(tah_inputs *inputs, ta_address *address, bool fixed, bool has_at, ta_time at) {
    if (address == NULL) {
        return;
    }
    uint64_t index = 0;
    tah_input_touch *touch = NULL;
    if (tah_map_get(&inputs->touched_index, (uint64_t)(uintptr_t)address, &index)) {
        touch = tah_vec_at(&inputs->touched, (uint32_t)index);
    } else {
        tah_input_touch fresh = { .address = address };
        if (tah_vec_push(&inputs->touched, &fresh) != TA_OK) {
            inputs->lost += 1u; /* its wake is not reported this turn */
            return;
        }
        if (tah_map_put(&inputs->touched_index, (uint64_t)(uintptr_t)address, inputs->touched.count - 1u) != TA_OK) {
            inputs->touched.count -= 1u;
            inputs->lost += 1u;
            return;
        }
        ta_address_retain(address);
        touch = tah_vec_at(&inputs->touched, inputs->touched.count - 1u);
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
static tah_input_entry *tah_inputs_entry(tah_inputs *inputs, ta_address *address) {
    uint64_t index = 0;
    if (tah_map_get(&inputs->entry_index, (uint64_t)(uintptr_t)address, &index)) {
        return tah_vec_at(&inputs->entries, (uint32_t)index);
    }
    tah_input_entry entry = { .address = address };
    if (inputs->spare.count != 0) {
        inputs->spare.count -= 1u;
        entry.bytes = *(tah_vec *)tah_vec_at(&inputs->spare, inputs->spare.count);
    } else {
        tah_vec_init(&entry.bytes, inputs->allocator, TAH_LAYOUT(uint8_t));
    }
    if (tah_vec_push(&inputs->entries, &entry) != TA_OK) {
        tah_vec_destroy(&entry.bytes);
        return NULL;
    }
    if (tah_map_put(&inputs->entry_index, (uint64_t)(uintptr_t)address, inputs->entries.count - 1u) != TA_OK) {
        inputs->entries.count -= 1u;
        tah_vec_destroy(&entry.bytes);
        return NULL;
    }
    ta_address_retain(address);
    return tah_vec_at(&inputs->entries, inputs->entries.count - 1u);
}

static ta_op_status tah_input_sink_begin(void *self) {
    tah_system *system = self;
    tah_inputs *inputs = &system->inputs;
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
    tah_input_entry *entry = tah_inputs_entry(inputs, address);
    if (entry == NULL) {
        ta_address_release(interned);
        return TA_OP_NOMEM;
    }
    inputs->open = true;
    inputs->open_entry = (uint32_t)(entry - (tah_input_entry *)inputs->entries.items);
    inputs->open_start = entry->bytes.count;
    tah_inputs_touch(inputs, address, false, false, 0);
    ta_address_release(interned); /* the entry and the touch hold it now */
    return TA_OP_OK;
}

static ta_op_status tah_input_sink_write(void *self, ta_bytes bytes) {
    tah_system *system = self;
    tah_inputs *inputs = &system->inputs;
    if (!inputs->open) {
        return TA_OP_ERROR;
    }
    tah_input_entry *entry = tah_vec_at(&inputs->entries, inputs->open_entry);
    if (bytes.len > UINT32_MAX - entry->bytes.count || tah_vec_reserve(&entry->bytes, entry->bytes.count + bytes.len) != TA_OK) {
        return TA_OP_NOMEM;
    }
    tah_mem_copy((uint8_t *)entry->bytes.items + entry->bytes.count, bytes.ptr, bytes.len);
    entry->bytes.count += bytes.len;
    return TA_OP_OK;
}

static void tah_input_sink_end(void *self, bool keep) {
    tah_system *system = self;
    tah_inputs *inputs = &system->inputs;
    if (!inputs->open) {
        return;
    }
    inputs->open = false;
    tah_input_entry *entry = tah_vec_at(&inputs->entries, inputs->open_entry);
    if (!keep) {
        entry->bytes.count = inputs->open_start;
        return;
    }
    inputs->count += 1u;
    tah_system_want_turn(system); /* the batch goes out after the next turn */
}

static const ta_byte_sink_vtable tah_input_sink_vtable = {
    .begin = tah_input_sink_begin,
    .write = tah_input_sink_write,
    .end = tah_input_sink_end,
};

/* ------------------------------------------------------------------------
 * Configuration
 */

/* A 64-bit mix of the incarnation (SplitMix64's finalizer). */
static uint64_t tah_mix(uint64_t value) {
    value += 0x9e3779b97f4a7c15u;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9u;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebu;
    return value ^ (value >> 31);
}

ta_result tah_inputs_configure(tah_system *system, const ta_str *families, uint32_t count) {
    tah_inputs *inputs = &system->inputs;
    if (count > 0 && !inputs->arena_ready) {
        if (ta_arena_init(&inputs->arena, inputs->allocator, TAH_INPUTS_BLOCK_BYTES) != TA_OK) {
            return TA_ERR_NOMEM;
        }
        inputs->arena_ready = true;
    }
    tah_vec copies;
    tah_vec_init(&copies, inputs->allocator, TAH_LAYOUT(ta_str));
    ta_result result = tah_vec_reserve(&copies, count);
    for (uint32_t at = 0; result == TA_OK && at < count; at += 1u) {
        char *text = tah_alloc(inputs->allocator, (size_t)families[at].len + 1u, 1);
        if (text == NULL) {
            result = TA_ERR_NOMEM;
            break;
        }
        tah_mem_copy(text, families[at].ptr, families[at].len);
        text[families[at].len] = '\0';
        ta_str copy = { .ptr = text, .len = families[at].len };
        (void)tah_vec_push(&copies, &copy);
    }
    for (uint32_t at = 0; result == TA_OK && at < count; at += 1u) {
        result = ta_system_log_family(system->core, families[at], true);
    }
    if (result != TA_OK) {
        tah_vec old = inputs->families;
        inputs->families = copies;
        tah_inputs_free_families(inputs);
        tah_vec_destroy(&inputs->families);
        inputs->families = old;
        for (uint32_t at = 0; at < count; at += 1u) {
            if (!tah_inputs_family_listed(inputs, families[at])) {
                (void)ta_system_log_family(system->core, families[at], false);
            }
        }
        return result;
    }
    /* Families no longer listed stop being logged. */
    for (uint32_t at = 0; at < inputs->families.count; at += 1u) {
        ta_str *old = tah_vec_at(&inputs->families, at);
        bool kept = false;
        for (uint32_t index = 0; index < count && !kept; index += 1u) {
            kept = families[index].len == old->len && tah_mem_equal(families[index].ptr, old->ptr, old->len);
        }
        if (!kept) {
            (void)ta_system_log_family(system->core, *old, false);
        }
    }
    tah_inputs_free_families(inputs);
    tah_vec_destroy(&inputs->families);
    inputs->families = copies;
    inputs->enabled = count > 0;
    if (inputs->seq_base == 0) {
        inputs->seq_base = tah_mix(ta_system_incarnation(system->core)) & 0xffffffff00000000u;
        inputs->seq_base = inputs->seq_base == 0 ? (uint64_t)1 << 32 : inputs->seq_base;
        inputs->next_seq = inputs->seq_base;
    }
    /* A message queued before now has no queued record: its event has no host number. */
    tah_inputs_forget_mail(inputs);
    tah_sched_forget_logging(&system->sched);
    ta_system_set_input_sink(system->core, inputs->enabled
                                               ? (ta_byte_sink){ .self = system, .v = &tah_input_sink_vtable }
                                               : (ta_byte_sink){ 0 });
    system->sched.observer = inputs->enabled
                                 ? (tah_sched_observer){ .self = system, .note = tah_inputs_note,
                                                         .host_seq = tah_inputs_host_seq }
                                 : (tah_sched_observer){ 0 };
    return TA_OK;
}

bool tah_inputs_seq_is_ours(const tah_inputs *inputs, uint64_t seq) {
    return inputs->seq_base != 0 && (seq & 0xffffffff00000000u) == inputs->seq_base;
}

/* tah_inputs_logged(), also answering the address's handle (borrowed). */
static bool tah_inputs_logged_handle(tah_system *system, ta_actor_id actor, ta_actor_address *address,
                                     ta_address **handle) {
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    if (!system->inputs.enabled || (entry != NULL && (entry->flags & TAH_SCHED_LOG_KNOWN) != 0 &&
                                    (entry->flags & TAH_SCHED_LOGGED) == 0)) {
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
                  tah_inputs_family_listed(&system->inputs, view.definition.family.name);
    if (entry != NULL) {
        entry->flags |= TAH_SCHED_LOG_KNOWN;
        entry->flags = logged ? (uint8_t)(entry->flags | TAH_SCHED_LOGGED) : (uint8_t)(entry->flags & ~TAH_SCHED_LOGGED);
    }
    if (logged && address != NULL) {
        *address = ta_address_view(view.self.as.named);
    }
    if (logged && handle != NULL) {
        *handle = view.self.as.named;
    }
    return logged;
}

bool tah_inputs_logged(tah_system *system, ta_actor_id actor, ta_actor_address *address) {
    return tah_inputs_logged_handle(system, actor, address, NULL);
}

/* ------------------------------------------------------------------------
 * The mailbox's records
 */

/* One more queued message waiting for `address`, accepted at `since`. */
static void tah_inputs_mail_add(tah_inputs *inputs, ta_address *address, ta_time since) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t count = 0;
    (void)tah_map_get(&inputs->mail_count, key, &count);
    if (count == 0 && tah_map_put(&inputs->mail_since, key, since) != TA_OK) {
        inputs->lost += 1u;
    }
    if (tah_map_put(&inputs->mail_count, key, count + 1u) != TA_OK) {
        inputs->lost += 1u;
    }
}

/* One queued message for `address` delivered or dropped. */
static void tah_inputs_mail_done(tah_inputs *inputs, ta_address *address) {
    uint64_t key = (uint64_t)(uintptr_t)address;
    uint64_t count = 0;
    if (!tah_map_get(&inputs->mail_count, key, &count)) {
        return;
    }
    if (count <= 1u) {
        (void)tah_map_remove(&inputs->mail_count, key);
        (void)tah_map_remove(&inputs->mail_since, key);
    } else {
        (void)tah_map_put(&inputs->mail_count, key, count - 1u);
    }
}

/* The queued message behind `envelope`, if the log numbered it: its host
 * number and address (retained, the caller releases it), out of the maps. */
static bool tah_inputs_unqueue(tah_inputs *inputs, uint32_t envelope, uint64_t *seq, ta_address **address) {
    if (!tah_map_get(&inputs->seqs, envelope, seq)) {
        return false;
    }
    (void)tah_map_remove(&inputs->seqs, envelope);
    uint64_t value = 0;
    *address = NULL;
    if (tah_map_get(&inputs->queued_address, envelope, &value)) {
        (void)tah_map_remove(&inputs->queued_address, envelope);
        *address = (ta_address *)(uintptr_t)value;
        tah_inputs_mail_done(inputs, *address);
    }
    return true;
}

/* A definition of `family` to write a waiting message's data with: its
 * data model encodes the value. 0 when the system holds none. */
static ta_definition_id tah_inputs_family_definition(tah_system *system, ta_str family) {
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
                tah_mem_equal(view.identity.family.name.ptr, family.ptr, family.len)) {
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
static ta_actor_id tah_inputs_resident(tah_system *system, ta_address *address) {
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, address);
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
        return 0;
    }
    ta_actor_inspection view;
    return ta_actor_inspect(system->core, entry.actor, &view) == TA_OK && view.published ? entry.actor : 0;
}

/* Writes a host record for `address`; `actor` 0 names none. */
static ta_result tah_inputs_host_record(tah_system *system, ta_address *address, const ta_host_record_params *params) {
    tah_inputs *inputs = &system->inputs;
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

static ta_result tah_inputs_queued(tah_system *system, const tah_sched_note *note) {
    tah_inputs *inputs = &system->inputs;
    const tah_envelope *envelope = note->message;
    if (envelope == NULL || envelope->report != TAH_REPORT_NONE) {
        return TA_OK; /* core writes a report as it takes it */
    }
    ta_address *handle = NULL;
    ta_actor_id actor = 0;
    if (note->address != NULL) {
        handle = note->address;
        if (!tah_inputs_family_listed(inputs, ta_address_view(handle).family.name)) {
            return TA_OK;
        }
    } else if (tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
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
            definition = tah_inputs_family_definition(system, ta_address_view(handle).family.name);
        }
        ta_result result = tah_inputs_host_record(system, handle, &(ta_host_record_params){
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
    tah_inputs_touch(inputs, handle, false, false, 0);
    if (tah_map_put(&inputs->seqs, note->envelope, seq) != TA_OK) {
        inputs->lost += 1u; /* its event will carry no host number */
        return TA_OK;
    }
    if (tah_map_put(&inputs->queued_address, note->envelope, (uint64_t)(uintptr_t)handle) != TA_OK) {
        (void)tah_map_remove(&inputs->seqs, note->envelope);
        inputs->lost += 1u;
        return TA_OK;
    }
    ta_address_retain(handle);
    tah_inputs_mail_add(inputs, handle, since);
    return TA_OK;
}

ta_result tah_inputs_note(void *self, const tah_sched_note *note) {
    tah_system *system = self;
    tah_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return TA_OK;
    }
    ta_address *handle = NULL;
    switch (note->kind) {
    case TAH_NOTE_QUEUED:
        return tah_inputs_queued(system, note);
    case TAH_NOTE_DELIVERED: {
        /* Core wrote its `event`, which carries the host number. */
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        if (tah_inputs_unqueue(inputs, note->envelope, &seq, &queued_address)) {
            tah_inputs_touch(inputs, queued_address, false, false, 0);
            ta_address_release(queued_address);
        }
        return TA_OK;
    }
    case TAH_NOTE_DROPPED: {
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        if (!tah_inputs_unqueue(inputs, note->envelope, &seq, &queued_address)) {
            return TA_OK;
        }
        if (queued_address != NULL) {
            if (tah_inputs_host_record(system, queued_address, &(ta_host_record_params){
                                                                   .kind = TA_RECORD_DROPPED,
                                                                   .host_seq = seq,
                                                                   .actor = tah_inputs_resident(system, queued_address),
                                                               }) != TA_OK) {
                inputs->lost += 1u;
            }
            tah_inputs_touch(inputs, queued_address, false, false, 0);
            ta_address_release(queued_address);
        }
        return TA_OK;
    }
    case TAH_NOTE_SETTLED:
    case TAH_NOTE_FINISHED:
        /* Its macrostep may have armed or cancelled sends, or it finished:
         * work its wake out. Core wrote what it accepted. */
        if (tah_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            tah_inputs_touch(inputs, handle, false, false, 0);
            tah_system_want_turn(system);
        }
        return TA_OK;
    default:
        return TA_OK;
    }
}

uint64_t tah_inputs_host_seq(void *self, uint32_t envelope) {
    tah_system *system = self;
    uint64_t seq = 0;
    return tah_map_get(&system->inputs.seqs, envelope, &seq) ? seq : 0;
}

ta_result tah_inputs_evicted(tah_system *system, ta_actor_id actor, bool has_wake, ta_time wake) {
    ta_address *handle = NULL;
    if (!tah_inputs_logged_handle(system, actor, NULL, &handle)) {
        return TA_OK;
    }
    ta_result result = tah_inputs_host_record(system, handle, &(ta_host_record_params){
                                                                  .kind = TA_RECORD_EVICTED, .actor = actor });
    if (result == TA_OK) {
        tah_inputs_touch(&system->inputs, handle, true, has_wake, wake);
    }
    return result;
}

void tah_inputs_forget(tah_system *system, ta_address *address) {
    if (system->inputs.enabled && address != NULL &&
        tah_inputs_family_listed(&system->inputs, ta_address_view(address).family.name)) {
        tah_inputs_touch(&system->inputs, address, true, false, 0); /* nothing will need it back */
        tah_system_want_turn(system);
    }
}

void tah_inputs_redeliver(tah_inputs *inputs, uint64_t seq, ta_time since) {
    inputs->redelivering = inputs->enabled && seq != 0;
    inputs->redeliver_seq = seq;
    inputs->redeliver_since = since;
}

void tah_inputs_redelivered(tah_inputs *inputs) {
    inputs->redelivering = false;
    inputs->redeliver_seq = 0;
}

/* ------------------------------------------------------------------------
 * Wakes
 */

static bool tah_inputs_copy(tah_inputs *inputs, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(&inputs->arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return false;
    }
    tah_mem_copy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return true;
}

/* A touched address's wake; false when it has none to report (no resident
 * actor and no queued mail: it keeps the wake it had). */
static bool tah_inputs_wake_of(tah_system *system, const tah_input_touch *touch, bool *has_at, ta_time *at) {
    if (touch->fixed) {
        *has_at = touch->has_at;
        *at = touch->at;
        return true;
    }
    tah_inputs *inputs = &system->inputs;
    uint64_t key = (uint64_t)(uintptr_t)touch->address;
    uint64_t since = 0;
    bool mail = tah_map_get(&inputs->mail_since, key, &since);
    *has_at = mail;
    *at = mail ? (ta_time)since : 0;
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, touch->address);
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

tah_input_wake *tah_inputs_wakes(tah_system *system, uint32_t *count) {
    tah_inputs *inputs = &system->inputs;
    *count = 0;
    uint32_t touched = inputs->touched.count;
    tah_input_wake *wakes = touched == 0 || !inputs->arena_ready
                                ? NULL
                                : ta_arena_alloc(&inputs->arena, (size_t)touched * sizeof *wakes, _Alignof(tah_input_wake));
    for (uint32_t at = 0; wakes != NULL && at < touched; at += 1u) {
        const tah_input_touch *touch = tah_vec_at(&inputs->touched, at);
        tah_input_wake wake = { 0 };
        if (!tah_inputs_wake_of(system, touch, &wake.has_at, &wake.at)) {
            continue;
        }
        ta_actor_address address = ta_address_view(touch->address);
        if (!tah_inputs_copy(inputs, address.family.name, &wake.address.family.name) ||
            !tah_inputs_copy(inputs, address.name, &wake.address.name)) {
            inputs->lost += 1u;
            continue;
        }
        wakes[(*count)++] = wake;
    }
    if (touched != 0 && wakes == NULL) {
        inputs->lost += 1u;
    }
    tah_inputs_forget_touched(inputs);
    return wakes;
}
