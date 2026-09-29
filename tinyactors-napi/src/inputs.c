#include "inputs.h"

#include <string.h>

#include "allocator.h"
#include "scheduler.h"
#include "system.h"

/* The arena's block: a turn's records and their text usually fit in one. */
#define TNA_INPUTS_BLOCK_BYTES 16384u

static const char *const tna_input_kinds[TNA_INPUT_KINDS] = {
    "spawned", "queued", "event", "report", "withdrawn", "abandoned", "released", "evicted", "finished",
};

const char *tna_input_kind_name(tna_input_kind kind) {
    return kind < TNA_INPUT_KINDS ? tna_input_kinds[kind] : "event";
}

void tna_inputs_init(tna_inputs *inputs, ta_allocator allocator) {
    *inputs = (tna_inputs){ .allocator = allocator };
    tna_vec_init(&inputs->families, allocator, TNA_LAYOUT(ta_str));
    tna_map_init(&inputs->seqs, allocator);
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
    tna_map_destroy(&inputs->seqs);
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
    tna_map_destroy(&inputs->seqs);
    tna_map_init(&inputs->seqs, inputs->allocator);
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

bool tna_inputs_logged(tna_system *system, ta_actor_id actor, ta_actor_address *address) {
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
    bool logged = view.self.kind == TA_ACTOR_REF_NAMED &&
                  tna_inputs_family_listed(&system->inputs, view.definition.family.name);
    if (entry != NULL) {
        entry->flags |= TNA_SCHED_LOG_KNOWN;
        entry->flags = logged ? (uint8_t)(entry->flags | TNA_SCHED_LOGGED) : (uint8_t)(entry->flags & ~TNA_SCHED_LOGGED);
    }
    if (logged && address != NULL) {
        *address = ta_address_view(view.self.as.named);
    }
    return logged;
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

/* A new record for `address`, linked last; NULL (counted) when there is no memory. */
static tna_input *tna_inputs_add(tna_system *system, tna_input_kind kind, const ta_actor_address *address) {
    tna_inputs *inputs = &system->inputs;
    tna_input *input = ta_arena_alloc(&inputs->arena, sizeof *input, _Alignof(tna_input));
    if (input == NULL) {
        inputs->dropped += 1u;
        return NULL;
    }
    *input = (tna_input){ .kind = kind, .time = ta_system_time(system->core) };
    if (!tna_inputs_copy(inputs, address->family.name, &input->address.family.name) ||
        !tna_inputs_copy(inputs, address->name, &input->address.name)) {
        inputs->dropped += 1u;
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

void tna_inputs_note(void *self, const tna_sched_note *note) {
    tna_system *system = self;
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return;
    }
    ta_actor_address address = { 0 };
    switch (note->kind) {
    case TNA_NOTE_QUEUED: {
        const tna_envelope *envelope = note->message;
        if (envelope == NULL || envelope->report != TNA_REPORT_NONE) {
            return; /* a report is recorded when core takes it */
        }
        if (note->address != NULL) {
            address = ta_address_view(note->address);
            if (!tna_inputs_family_listed(inputs, address.family.name)) {
                return;
            }
        } else if (!tna_inputs_logged(system, note->actor, &address)) {
            return;
        }
        tna_input *input = tna_inputs_add(system, TNA_INPUT_QUEUED, &address);
        if (input == NULL) {
            return;
        }
        input->actor = note->actor;
        input->seq = ++inputs->next_seq;
        tna_inputs_event(system, input, &envelope->event, 0);
        if (tna_map_put(&inputs->seqs, note->envelope, input->seq) != TA_OK) {
            inputs->dropped += 1u; /* its event will carry no seq */
        }
        return;
    }
    case TNA_NOTE_DELIVERED: {
        uint64_t seq = 0;
        bool queued = tna_map_get(&inputs->seqs, note->envelope, &seq);
        if (queued) {
            (void)tna_map_remove(&inputs->seqs, note->envelope);
        }
        if (!tna_inputs_logged(system, note->actor, &address)) {
            return;
        }
        tna_input *input = tna_inputs_add(system, TNA_INPUT_EVENT, &address);
        if (input == NULL) {
            return;
        }
        input->actor = note->actor;
        input->seq = queued ? seq : 0;
        tna_inputs_event(system, input, &note->message->event, 0);
        return;
    }
    case TNA_NOTE_REPORTED: {
        if (!tna_inputs_logged(system, note->actor, &address)) {
            return;
        }
        tna_input *input = tna_inputs_add(system, TNA_INPUT_REPORT, &address);
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
    case TNA_NOTE_FINISHED:
        if (tna_inputs_logged(system, note->actor, &address)) {
            tna_input *input = tna_inputs_add(system, TNA_INPUT_FINISHED, &address);
            if (input != NULL) {
                input->actor = note->actor;
            }
        }
        return;
    default:
        return;
    }
}

void tna_inputs_host(tna_system *system, const tna_input_host *host) {
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled) {
        return;
    }
    ta_actor_address address = { 0 };
    if (host->address != NULL) {
        address = *host->address;
        if (!tna_inputs_family_listed(inputs, address.family.name)) {
            return;
        }
    } else if (!tna_inputs_logged(system, host->actor, &address)) {
        return;
    }
    tna_input *input = tna_inputs_add(system, host->kind, &address);
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
