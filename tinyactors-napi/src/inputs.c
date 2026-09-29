#include "inputs.h"

#include <string.h>

#include "allocator.h"
#include "scheduler.h"
#include "system.h"

/* The arena's block: a turn's records and their text usually fit in one. */
#define TNA_INPUTS_BLOCK_BYTES 16384u

static const char *const tna_input_kinds[TNA_INPUT_KINDS] = {
    "spawned", "queued",   "event", "report", "withdrawn", "abandoned",
    "released", "evicted", "finished", "fired", "dropped", "jump", "destroyed",
};

const char *tna_input_kind_name(tna_input_kind kind) {
    return kind < TNA_INPUT_KINDS ? tna_input_kinds[kind] : "event";
}

void tna_inputs_init(tna_inputs *inputs, ta_allocator allocator) {
    *inputs = (tna_inputs){ .allocator = allocator };
    tna_vec_init(&inputs->families, allocator, TNA_LAYOUT(ta_str));
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

void tna_inputs_destroy(tna_inputs *inputs) {
    uint32_t count = 0;
    tna_inputs_release(inputs, tna_inputs_take(inputs, &count));
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
    if (result != TA_OK) {
        tna_vec old = inputs->families;
        inputs->families = copies;
        tna_inputs_free_families(inputs);
        tna_vec_destroy(&inputs->families);
        inputs->families = old;
        return result;
    }
    tna_inputs_free_families(inputs);
    tna_vec_destroy(&inputs->families);
    inputs->families = copies;
    inputs->enabled = count > 0;
    /* A message queued before now has no queued record: its event has no seq. */
    tna_inputs_forget_mail(inputs);
    tna_sched_forget_logging(&system->sched);
    system->sched.observer = inputs->enabled
                                 ? (tna_sched_observer){ .self = system, .note = tna_inputs_note }
                                 : (tna_sched_observer){ 0 };
    return TA_OK;
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
        if (tna_vec_push(&inputs->touched, &fresh) != TA_OK ||
            tna_map_put(&inputs->touched_index, (uint64_t)(uintptr_t)address, inputs->touched.count - 1u) != TA_OK) {
            if (inputs->touched.count != 0 &&
                ((tna_input_touch *)tna_vec_at(&inputs->touched, inputs->touched.count - 1u))->address == address) {
                inputs->touched.count -= 1u;
            }
            inputs->lost += 1u; /* its wake is not reported this turn */
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

/* The queued message behind `envelope`, if the log numbered it: its seq and
 * address (retained, the caller releases it), taken out of the maps. */
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

static bool tna_inputs_copy(tna_inputs *inputs, ta_str text, ta_str *out) {
    if (text.ptr == NULL) {
        *out = (ta_str){ 0 };
        return true;
    }
    char *copy = ta_arena_alloc(&inputs->arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return true;
}

/* A new record for `handle`, linked last, touching the address; NULL
 * (counted) when there is no memory. */
static tna_input *tna_inputs_add(tna_system *system, tna_input_kind kind, ta_address *handle) {
    tna_inputs *inputs = &system->inputs;
    tna_inputs_touch(inputs, handle, false, false, 0);
    tna_input *input = ta_arena_alloc(&inputs->arena, sizeof *input, _Alignof(tna_input));
    if (input == NULL) {
        inputs->lost += 1u;
        return NULL;
    }
    *input = (tna_input){ .kind = kind, .time = ta_system_time(system->core) };
    ta_actor_address address = ta_address_view(handle);
    if (!tna_inputs_copy(inputs, address.family.name, &input->address.family.name) ||
        !tna_inputs_copy(inputs, address.name, &input->address.name)) {
        inputs->lost += 1u;
        return NULL;
    }
    if (inputs->last != NULL) {
        inputs->last->next = input;
    } else {
        inputs->first = input;
    }
    inputs->last = input;
    inputs->count += 1u;
    if (!system->inside_core) {
        tna_system_want_turn(system); /* a batch waits for the next turn */
    }
    return input;
}

/* The text of a send ID into the arena, or nothing for a send without one. */
static void tna_inputs_send_text(tna_system *system, ta_send_id send, ta_str *out) {
    *out = (ta_str){ 0 };
    if (send == 0) {
        return;
    }
    uint32_t needed = 0;
    if (ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send, .needed = &needed }) !=
            TA_ERR_NOMEM ||
        needed == 0) {
        return;
    }
    char *text = ta_arena_alloc(&system->inputs.arena, needed, 1);
    if (text != NULL && ta_send_id_format(system->core, &(ta_send_id_format_params){
                                                            .send_id = send, .buffer = text, .capacity = needed, .needed = &needed }) == TA_OK) {
        *out = (ta_str){ .ptr = text, .len = needed - 1u };
    }
}

static void tna_inputs_invoke_text(tna_system *system, ta_invoke_id invoke, ta_str *out) {
    *out = (ta_str){ 0 };
    ta_str text = { 0 };
    if (invoke != TA_INVOKE_ID_NONE && ta_system_invoke_id_text(system->core, invoke, &text) == TA_OK) {
        (void)tna_inputs_copy(&system->inputs, text, out);
    }
}

/* The event's fields: name, type, data (retained), send ID, origin and invocation. */
static void tna_inputs_event(tna_system *system, tna_input *input, const ta_event *event, uint64_t session) {
    input->name = event->name;
    input->type = event->type;
    if (event->data.v != NULL) {
        input->value = event->data;
        ta_value_retain(&input->value);
        input->has_value = true;
    }
    tna_inputs_send_text(system, event->send_id, &input->send_id);
    tna_inputs_invoke_text(system, event->invoke_id, &input->invoke_id);
    if (event->origin.kind != TA_ORIGIN_NONE) {
        ta_str type = { 0 };
        ta_str uri = { 0 };
        if (ta_origin_text(system->core, &(ta_origin_text_params){
                                             .origin = &event->origin,
                                             .session_id = session,
                                             .arena = ta_arena_allocator(&system->inputs.arena),
                                             .type = &type,
                                             .uri = &uri,
                                         }) == TA_OK) {
            (void)tna_inputs_copy(&system->inputs, type, &input->origin_type);
            (void)tna_inputs_copy(&system->inputs, uri, &input->origin);
        }
    }
}

static const char *tna_inputs_report_reason(uint8_t report) {
    switch (report) {
    case TNA_REPORT_QUARANTINED: return "quarantined";
    case TNA_REPORT_FAILED: return "failed";
    default: return "unavailable";
    }
}

/* A jump envelope's transition, copied into the record. */
static void tna_inputs_jump(tna_inputs *inputs, tna_input *input, const tna_jump *jump) {
    input->definition = jump->definition;
    input->source = jump->source;
    input->targets = ta_arena_alloc(&inputs->arena, (size_t)jump->count * sizeof(ta_state_id), _Alignof(ta_state_id));
    if (input->targets == NULL) {
        inputs->lost += 1u;
        return;
    }
    memcpy(input->targets, jump->targets, (size_t)jump->count * sizeof(ta_state_id));
    input->target_count = jump->count;
}

void tna_inputs_note(void *self, const tna_sched_note *note) {
    tna_system *system = self;
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return;
    }
    ta_address *handle = NULL;
    switch (note->kind) {
    case TNA_NOTE_QUEUED: {
        const tna_envelope *envelope = note->message;
        if (envelope == NULL || envelope->report != TNA_REPORT_NONE) {
            return; /* a report is recorded when core takes it */
        }
        if (note->address != NULL) {
            handle = note->address;
            if (!tna_inputs_family_listed(inputs, ta_address_view(handle).family.name)) {
                return;
            }
        } else if (!tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            return;
        }
        bool owed = inputs->redelivering;
        ta_time since = owed ? inputs->redeliver_since : ta_system_time(system->core);
        inputs->redelivering = false;
        uint64_t seq = ++inputs->next_seq;
        if (owed) {
            tna_inputs_touch(inputs, handle, false, false, 0);
            inputs->redelivered = seq; /* its queued record is in the log already */
        } else {
            tna_input *input = tna_inputs_add(system, TNA_INPUT_QUEUED, handle);
            if (input == NULL) {
                return;
            }
            input->actor = note->actor;
            input->seq = seq;
            tna_inputs_event(system, input, &envelope->event, 0);
        }
        if (tna_map_put(&inputs->seqs, note->envelope, seq) != TA_OK) {
            inputs->lost += 1u; /* its event will carry no seq */
            return;
        }
        if (tna_map_put(&inputs->queued_address, note->envelope, (uint64_t)(uintptr_t)handle) != TA_OK) {
            (void)tna_map_remove(&inputs->seqs, note->envelope);
            inputs->lost += 1u;
            return;
        }
        ta_address_retain(handle);
        tna_inputs_mail_add(inputs, handle, since);
        return;
    }
    case TNA_NOTE_DELIVERED: {
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        bool queued = tna_inputs_unqueue(inputs, note->envelope, &seq, &queued_address);
        if (tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            const tna_envelope *envelope = note->message;
            tna_input *input = tna_inputs_add(system, envelope->has_jump ? TNA_INPUT_JUMP : TNA_INPUT_EVENT, handle);
            if (input != NULL) {
                input->actor = note->actor;
                input->seq = queued ? seq : 0;
                tna_inputs_event(system, input, &envelope->event, 0);
                if (envelope->has_jump) {
                    tna_inputs_jump(inputs, input, envelope->jump);
                }
            }
        }
        ta_address_release(queued_address);
        return;
    }
    case TNA_NOTE_DROPPED: {
        uint64_t seq = 0;
        ta_address *queued_address = NULL;
        if (!tna_inputs_unqueue(inputs, note->envelope, &seq, &queued_address)) {
            return;
        }
        if (queued_address != NULL) {
            tna_input *input = tna_inputs_add(system, TNA_INPUT_DROPPED, queued_address);
            if (input != NULL) {
                input->seq = seq;
            }
            ta_address_release(queued_address);
        }
        return;
    }
    case TNA_NOTE_REPORTED: {
        if (!tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            return;
        }
        tna_input *input = tna_inputs_add(system, TNA_INPUT_REPORT, handle);
        if (input == NULL) {
            return;
        }
        const tna_envelope *envelope = note->message;
        input->actor = note->actor;
        input->ends = envelope->report != TNA_REPORT_QUARANTINED;
        const char *reason = tna_inputs_report_reason(envelope->report);
        (void)tna_inputs_copy(inputs, (ta_str){ .ptr = reason, .len = (uint32_t)strlen(reason) }, &input->reason);
        tna_inputs_invoke_text(system, envelope->event.invoke_id, &input->invoke_id);
        if (envelope->event.data.v != NULL) {
            input->value = envelope->event.data;
            ta_value_retain(&input->value);
            input->has_value = true;
        }
        return;
    }
    case TNA_NOTE_SETTLED:
        /* Its macrostep may have armed or cancelled sends: work its wake out. */
        if (tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            tna_inputs_touch(inputs, handle, false, false, 0);
            if (!system->inside_core) {
                tna_system_want_turn(system);
            }
        }
        return;
    case TNA_NOTE_FINISHED:
        if (tna_inputs_logged_handle(system, note->actor, NULL, &handle)) {
            tna_input *input = tna_inputs_add(system, TNA_INPUT_FINISHED, handle);
            if (input != NULL) {
                input->actor = note->actor;
            }
            tna_inputs_touch(inputs, handle, true, false, 0); /* napi never needs it back */
        }
        return;
    default:
        return;
    }
}

void tna_inputs_fired(tna_system *system, const ta_actor_ref *sender, ta_send_id send) {
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled || sender == NULL) {
        return;
    }
    ta_address *handle = NULL;
    if (sender->kind == TA_ACTOR_REF_NAMED) {
        handle = sender->as.named;
        if (handle == NULL || !tna_inputs_family_listed(inputs, ta_address_view(handle).family.name)) {
            return;
        }
    } else if (!tna_inputs_logged_handle(system, sender->as.runtime, NULL, &handle)) {
        return;
    }
    tna_input *input = tna_inputs_add(system, TNA_INPUT_FIRED, handle);
    if (input != NULL) {
        tna_inputs_send_text(system, send, &input->send_id);
    }
}

void tna_inputs_redeliver(tna_inputs *inputs, ta_time since) {
    inputs->redelivering = inputs->enabled;
    inputs->redeliver_since = since;
    inputs->redelivered = 0;
}

uint64_t tna_inputs_redelivered(tna_inputs *inputs) {
    uint64_t seq = inputs->redelivered;
    inputs->redelivering = false;
    inputs->redelivered = 0;
    return seq;
}

void tna_inputs_host(tna_system *system, const tna_input_host *host) {
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return;
    }
    ta_address *handle = NULL;
    if (!tna_inputs_logged_handle(system, host->actor, NULL, &handle)) {
        return;
    }
    tna_input *input = tna_inputs_add(system, host->kind, handle);
    if (host->kind == TNA_INPUT_EVICTED) {
        tna_inputs_touch(inputs, handle, true, host->has_wake, host->wake);
    } else if (host->kind == TNA_INPUT_DESTROYED) {
        tna_inputs_touch(inputs, handle, true, false, 0); /* nothing will need it back */
    }
    if (input == NULL) {
        return;
    }
    input->actor = host->actor;
    if (host->value != NULL && host->value->v != NULL) {
        input->value = *host->value;
        ta_value_retain(&input->value);
        input->has_value = true;
    }
    if (host->kind == TNA_INPUT_SPAWNED) {
        (void)tna_inputs_copy(inputs, host->text, &input->revision);
    } else if (host->kind == TNA_INPUT_WITHDRAWN) {
        (void)tna_inputs_copy(inputs, host->text, &input->send_id);
    }
}

tna_input *tna_inputs_take(tna_inputs *inputs, uint32_t *count) {
    tna_input *first = inputs->first;
    *count = inputs->count;
    inputs->first = NULL;
    inputs->last = NULL;
    inputs->count = 0;
    return first;
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

void tna_inputs_release(tna_inputs *inputs, tna_input *first) {
    for (tna_input *input = first; input != NULL; input = input->next) {
        if (input->has_value) {
            ta_value_release(&input->value);
            input->has_value = false;
        }
    }
    /* Records made after the take (none, while the caller converts) keep the arena. */
    if (inputs->arena_ready && inputs->first == NULL) {
        ta_arena_reset(&inputs->arena);
    }
}
