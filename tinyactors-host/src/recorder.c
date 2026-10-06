#include "tinyactors_host/recorder.h"

#include "memory.h"

#include "tinyactors_host/allocator.h"
#include "tinyactors_host/events.h"
#include "tinyactors_host/system.h"
#include "allocator_private.h"
#include "recorder_private.h"
#include "scheduler_private.h"
#include "system_private.h"

#define TAH_RECORDER_BLOCK_BYTES 4096u

void tah_recorder_init(tah_recorder *recorder, ta_allocator allocator) {
    *recorder = (tah_recorder){ .allocator = allocator };
    tah_vec_init(&recorder->families, allocator, TAH_LAYOUT(ta_str));
    tah_vec_init(&recorder->open, allocator, TAH_LAYOUT(tah_recorded));
    tah_vec_init(&recorder->records, allocator, TAH_LAYOUT(tah_recorded));
    tah_vec_init(&recorder->touched, allocator, TAH_LAYOUT(tah_recorder_touch));
    tah_map_init(&recorder->touched_index, allocator);
    tah_vec_init(&recorder->wakes, allocator, TAH_LAYOUT(tah_record_wake));
}

static void tah_recorded_release(tah_recorded *recorded) {
    ta_event_release(&recorded->event);
    ta_address_release(recorded->address);
    *recorded = (tah_recorded){ 0 };
}

static void tah_recorder_free_families(tah_recorder *recorder) {
    for (uint32_t at = 0; at < recorder->families.count; at += 1u) {
        ta_str *family = tah_vec_at(&recorder->families, at);
        tah_free(recorder->allocator, &(ta_allocator_free_params){ .ptr = (void *)family->ptr,
                                                                   .size = (size_t)family->len + 1u, .alignment = 1 });
    }
    tah_vec_clear(&recorder->families);
}

static void tah_recorder_clear(tah_vec *list) {
    for (uint32_t at = 0; at < list->count; at += 1u) {
        tah_recorded_release(tah_vec_at(list, at));
    }
    tah_vec_clear(list);
}

static void tah_recorder_forget_touched(tah_recorder *recorder) {
    for (uint32_t at = 0; at < recorder->touched.count; at += 1u) {
        ta_address_release(((tah_recorder_touch *)tah_vec_at(&recorder->touched, at))->address);
    }
    for (uint32_t at = 0; at < recorder->touched.count; at += 1u) {
        (void)tah_map_remove(&recorder->touched_index,
                             (uint64_t)(uintptr_t)((tah_recorder_touch *)tah_vec_at(&recorder->touched, at))->address);
    }
    tah_vec_clear(&recorder->touched);
}

void tah_recorder_destroy(tah_recorder *recorder) {
    tah_recorder_clear(&recorder->open);
    tah_recorder_clear(&recorder->records);
    tah_recorder_forget_touched(recorder);
    tah_recorder_free_families(recorder);
    tah_vec_destroy(&recorder->families);
    tah_vec_destroy(&recorder->open);
    tah_vec_destroy(&recorder->records);
    tah_vec_destroy(&recorder->touched);
    tah_map_destroy(&recorder->touched_index);
    tah_vec_destroy(&recorder->wakes);
    if (recorder->arena_ready) {
        ta_arena_destroy(&recorder->arena);
        recorder->arena_ready = false;
    }
    recorder->enabled = false;
}

ta_result tah_recorder_configure(tah_system *system, const ta_str *families, uint32_t count) {
    tah_recorder *recorder = &system->recorder;
    if (count > 0 && !recorder->arena_ready) {
        if (ta_arena_init(&recorder->arena, recorder->allocator, TAH_RECORDER_BLOCK_BYTES) != TA_OK) {
            return TA_ERR_NOMEM;
        }
        recorder->arena_ready = true;
    }
    tah_vec copies;
    tah_vec_init(&copies, recorder->allocator, TAH_LAYOUT(ta_str));
    ta_result result = tah_vec_reserve(&copies, count);
    for (uint32_t at = 0; result == TA_OK && at < count; at += 1u) {
        char *text = tah_alloc(recorder->allocator, (size_t)families[at].len + 1u, 1);
        if (text == NULL) {
            result = TA_ERR_NOMEM;
            break;
        }
        tah_mem_copy(text, families[at].ptr, families[at].len);
        text[families[at].len] = '\0';
        ta_str copy = { .ptr = text, .len = families[at].len };
        (void)tah_vec_push(&copies, &copy);
    }
    tah_vec old = recorder->families;
    recorder->families = copies;
    if (result != TA_OK) {
        tah_recorder_free_families(recorder);
        tah_vec_destroy(&recorder->families);
        recorder->families = old;
        return result;
    }
    tah_vec kept = recorder->families;
    recorder->families = old;
    tah_recorder_free_families(recorder);
    tah_vec_destroy(&recorder->families);
    recorder->families = kept;
    recorder->enabled = count > 0;
    /* Macrosteps open now began before recording did: they record nothing. */
    tah_recorder_clear(&recorder->open);
    tah_system_observe(system);
    return TA_OK;
}

bool tah_recorder_enabled(const tah_system *system) {
    return system->recorder.enabled;
}

static bool tah_recorder_family_listed(const tah_recorder *recorder, ta_str family) {
    for (uint32_t at = 0; at < recorder->families.count; at += 1u) {
        const ta_str *listed = tah_vec_at(&recorder->families, at);
        if (listed->len == family.len && tah_mem_equal(listed->ptr, family.ptr, family.len)) {
            return true;
        }
    }
    return false;
}

/* The address of `actor` when it is a published named actor of a recorded family (borrowed), or NULL. */
static ta_address *tah_recorder_address_of(tah_system *system, ta_actor_id actor, uint64_t *session) {
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, actor, &view) != TA_OK || !view.published ||
        view.self.kind != TA_ACTOR_REF_NAMED ||
        !tah_recorder_family_listed(&system->recorder, view.definition.family.name)) {
        return NULL;
    }
    if (session != NULL) {
        *session = view.session_id;
    }
    return view.self.as.named;
}

/* Marks `address` touched this turn; `fixed` gives its wake outright. */
static void tah_recorder_touch_address(tah_recorder *recorder, ta_address *address, bool fixed, bool has_at,
                                       ta_time at) {
    uint64_t index = 0;
    tah_recorder_touch *touch = NULL;
    if (tah_map_get(&recorder->touched_index, (uint64_t)(uintptr_t)address, &index)) {
        touch = tah_vec_at(&recorder->touched, (uint32_t)index);
    } else {
        tah_recorder_touch fresh = { .address = address };
        if (tah_vec_push(&recorder->touched, &fresh) != TA_OK) {
            recorder->lost += 1u;
            return;
        }
        if (tah_map_put(&recorder->touched_index, (uint64_t)(uintptr_t)address, recorder->touched.count - 1u) !=
            TA_OK) {
            recorder->touched.count -= 1u;
            recorder->lost += 1u;
            return;
        }
        ta_address_retain(address);
        touch = tah_vec_at(&recorder->touched, recorder->touched.count - 1u);
    }
    if (fixed) {
        touch->fixed = true;
        touch->has_at = has_at;
        touch->at = at;
    }
}

/* The core reason token of a report envelope (core ADR 0081). */
static ta_str tah_recorder_reason(uint8_t kind) {
    switch (kind) {
    case TAH_REPORT_QUARANTINED: return (ta_str){ .ptr = "quarantined", .len = 11 };
    case TAH_REPORT_FAILED: return (ta_str){ .ptr = "failed", .len = 6 };
    default: return (ta_str){ .ptr = "unavailable", .len = 11 };
    }
}

/* An invocation ID's text in a stack buffer: empty when it has none or
 * is longer than the buffer. */
static ta_str tah_recorder_invoke_id(const ta_system *core, ta_invoke_id invoke, char *buffer, uint32_t capacity) {
    uint32_t needed = 0;
    if (ta_system_invoke_id_format(core, &(ta_invoke_id_format_params){ .invoke = invoke, .buffer = buffer,
                                                                         .capacity = capacity,
                                                                         .needed = &needed }) != TA_OK) {
        return (ta_str){ 0 };
    }
    return (ta_str){ .ptr = buffer, .len = needed - 1u };
}

/* An invocation's error report as the event the chart takes: error.communication.invoke.<id>.<reason>. */
static ta_result tah_recorder_report_event(tah_system *system, const tah_envelope *envelope, ta_event *out) {
    static const char prefix[] = "error.communication.invoke.";
    char id_text[256];
    ta_str id = tah_recorder_invoke_id(system->core, envelope->event.invoke_id, id_text, sizeof id_text);
    if (id.len == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str reason = tah_recorder_reason(envelope->report);
    size_t length = sizeof prefix - 1u + id.len + 1u + reason.len;
    char *text = tah_alloc(system->recorder.allocator, length, 1);
    if (text == NULL) {
        return TA_ERR_NOMEM;
    }
    tah_mem_copy(text, prefix, sizeof prefix - 1u);
    tah_mem_copy(text + sizeof prefix - 1u, id.ptr, id.len);
    text[sizeof prefix - 1u + id.len] = '.';
    tah_mem_copy(text + sizeof prefix + id.len, reason.ptr, reason.len);
    *out = (ta_event){ .type = TA_EVENT_PLATFORM, .invoke_id = envelope->event.invoke_id };
    ta_result result = tah_event_name_intern(system, (ta_str){ .ptr = text, .len = (uint32_t)length }, &out->name);
    tah_free(system->recorder.allocator, &(ta_allocator_free_params){ .ptr = text, .size = length, .alignment = 1 });
    if (result == TA_OK) {
        out->data = envelope->event.data;
        ta_value_retain(&out->data);
    }
    return result;
}

/* Core took a message or a report, or mail between actors from its own
 * queue: its macrostep is open, and its event is kept, at the time it was
 * taken, until the macrostep ends. */
static void tah_recorder_taken(tah_system *system, const tah_sched_note *note) {
    tah_recorder *recorder = &system->recorder;
    uint64_t session = 0;
    ta_address *address = tah_recorder_address_of(system, note->actor, &session);
    /* An operator's jump (core ADR 0082) is not an event the chart took by
     * its own transitions: replaying its event would take another path, so
     * it is not recorded (kernel ADR 0006 section 8, left open). */
    if (address == NULL || (note->message == NULL && note->event == NULL) ||
        (note->message != NULL && note->message->has_jump)) {
        return;
    }
    tah_recorded recorded = { .actor = note->actor, .address = address, .at = ta_system_time(system->core),
                              .session = session };
    if (note->kind == TAH_NOTE_REPORTED) {
        if (tah_recorder_report_event(system, note->message, &recorded.event) != TA_OK) {
            recorder->lost += 1u;
            return;
        }
    } else {
        ta_event_retain(note->message != NULL ? &note->message->event : note->event, &recorded.event);
    }
    ta_address_retain(address);
    if (tah_vec_push(&recorder->open, &recorded) != TA_OK) {
        recorder->lost += 1u;
        tah_recorded_release(&recorded);
    }
}

static int64_t tah_recorder_open_index(const tah_recorder *recorder, ta_actor_id actor) {
    for (uint32_t at = 0; at < recorder->open.count; at += 1u) {
        if (((const tah_recorded *)tah_vec_at(&recorder->open, at))->actor == actor) {
            return at;
        }
    }
    return -1;
}

static tah_recorded tah_recorder_take_open(tah_recorder *recorder, uint32_t index) {
    tah_recorded *items = recorder->open.items;
    tah_recorded taken = items[index];
    items[index] = items[recorder->open.count - 1u];
    recorder->open.count -= 1u;
    return taken;
}

/* `actor`'s macrostep ended: its event goes into this turn's records. */
static void tah_recorder_ended(tah_system *system, ta_actor_id actor) {
    tah_recorder *recorder = &system->recorder;
    int64_t index = tah_recorder_open_index(recorder, actor);
    if (index < 0) {
        return;
    }
    tah_recorded recorded = tah_recorder_take_open(recorder, (uint32_t)index);
    tah_recorder_touch_address(recorder, recorded.address, false, false, 0);
    if (tah_vec_push(&recorder->records, &recorded) != TA_OK) {
        recorder->lost += 1u;
        tah_recorded_release(&recorded);
    }
}

void tah_recorder_note(tah_system *system, const tah_sched_note *note) {
    tah_recorder *recorder = &system->recorder;
    if (!recorder->enabled) {
        return;
    }
    switch (note->kind) {
    case TAH_NOTE_DELIVERED:
    case TAH_NOTE_REPORTED:
        tah_recorder_taken(system, note);
        return;
    case TAH_NOTE_ENDED:
        tah_recorder_ended(system, note->actor);
        return;
    case TAH_NOTE_SETTLED:
    case TAH_NOTE_FINISHED: {
        /* Its macrostep may have armed or cancelled sends: work its wake out. */
        ta_address *address = tah_recorder_address_of(system, note->actor, NULL);
        if (address != NULL) {
            tah_recorder_touch_address(recorder, address, false, false, 0);
        }
        return;
    }
    default:
        return;
    }
}

void tah_recorder_removed(tah_system *system, ta_actor_id actor) {
    tah_recorder *recorder = &system->recorder;
    int64_t index = tah_recorder_open_index(recorder, actor);
    if (index >= 0) {
        tah_recorded recorded = tah_recorder_take_open(recorder, (uint32_t)index);
        tah_recorded_release(&recorded);
    }
}

void tah_recorder_evicted(tah_system *system, ta_actor_id actor, bool has_wake, ta_time wake) {
    ta_address *address = system->recorder.enabled ? tah_recorder_address_of(system, actor, NULL) : NULL;
    if (address != NULL) {
        tah_recorder_touch_address(&system->recorder, address, true, has_wake, wake);
        tah_system_want_turn(system); /* the wake goes out with the next turn's batch */
    }
}

void tah_recorder_fired(tah_system *system, const ta_actor_ref *sender) {
    tah_recorder *recorder = &system->recorder;
    if (!recorder->enabled) {
        return;
    }
    ta_address *address = NULL;
    if (sender->kind == TA_ACTOR_REF_NAMED) {
        ta_actor_address view = ta_address_view(sender->as.named);
        address = tah_recorder_family_listed(recorder, view.family.name) ? sender->as.named : NULL;
    } else if (sender->kind == TA_ACTOR_REF_RUNTIME) {
        address = tah_recorder_address_of(system, sender->as.runtime, NULL);
    }
    if (address != NULL) {
        tah_recorder_touch_address(recorder, address, false, false, 0);
    }
}

void tah_recorder_forget(tah_system *system, ta_actor_id actor) {
    tah_recorder_evicted(system, actor, false, 0);
}

uint32_t tah_recorder_count(const tah_system *system) {
    return system->recorder.records.count;
}

tah_record tah_recorder_record(const tah_system *system, uint32_t index) {
    const tah_recorded *recorded = tah_vec_at(&system->recorder.records, index);
    return (tah_record){ .address = ta_address_view(recorded->address), .event = &recorded->event, .at = recorded->at };
}

/* An event name's dotted text, in `arena`. */
static ta_result tah_recorder_name_text(const tah_system *system, ta_event_name_id name, ta_allocator arena, ta_str *out) {
    uint32_t needed = 0;
    (void)tah_event_name_format(system, &(ta_event_name_format_params){ .name = name, .needed = &needed });
    char *buffer = needed == 0 ? NULL : tah_alloc(arena, needed, 1);
    if (buffer == NULL ||
        tah_event_name_format(system, &(ta_event_name_format_params){ .name = name, .buffer = buffer,
                                                                           .capacity = needed, .needed = &needed }) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    *out = (ta_str){ .ptr = buffer, .len = needed - 1u };
    return TA_OK;
}

/* A send ID's text, in `arena`; empty for none. */
static ta_result tah_recorder_send_text(const tah_system *system, ta_send_id send, ta_allocator arena, ta_str *out) {
    uint32_t needed = 0;
    if (send == 0) {
        return TA_OK;
    }
    (void)ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send, .needed = &needed });
    char *buffer = needed == 0 ? NULL : tah_alloc(arena, needed, 1);
    if (buffer == NULL ||
        ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send, .buffer = buffer,
                                                                     .capacity = needed, .needed = &needed }) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    *out = (ta_str){ .ptr = buffer, .len = needed - 1u };
    return TA_OK;
}

ta_result tah_recorder_texts(const tah_system *system, uint32_t index, ta_allocator arena, tah_record_texts *out) {
    const tah_recorded *recorded = tah_vec_at(&system->recorder.records, index);
    const ta_event *event = &recorded->event;
    *out = (tah_record_texts){ .type = event->type };
    ta_result result = tah_recorder_name_text(system, event->name, arena, &out->name);
    if (result == TA_OK) {
        result = tah_recorder_send_text(system, event->send_id, arena, &out->send_id);
    }
    if (result == TA_OK && event->origin.kind != TA_ORIGIN_NONE) {
        result = ta_origin_text(system->core, &(ta_origin_text_params){ .origin = &event->origin,
                                                                        .session_id = recorded->session,
                                                                        .arena = arena,
                                                                        .type = &out->origin_type,
                                                                        .uri = &out->origin });
        if (result == TA_ERR_NOT_FOUND) {
            out->origin = (ta_str){ 0 };
            out->origin_type = (ta_str){ 0 };
            result = TA_OK; /* an origin with no text: a closed scope */
        }
    }
    if (result == TA_OK && event->invoke_id != TA_INVOKE_ID_NONE) {
        char id_text[256];
        ta_str id = tah_recorder_invoke_id(system->core, event->invoke_id, id_text, sizeof id_text);
        char *copy = id.len == 0 ? NULL : tah_alloc(arena, id.len, 1);
        if (copy != NULL) {
            tah_mem_copy(copy, id.ptr, id.len);
        }
        out->invoke_id = copy == NULL ? (ta_str){ 0 } : (ta_str){ .ptr = copy, .len = id.len };
    }
    return result;
}

static bool tah_recorder_copy(tah_recorder *recorder, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(&recorder->arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return false;
    }
    tah_mem_copy(copy, text.ptr, text.len);
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return true;
}

/* A touched address's wake; false when it has none to report (no resident actor, and none was given). */
static bool tah_recorder_wake_of(tah_system *system, const tah_recorder_touch *touch, bool *has_at, ta_time *at) {
    if (touch->fixed) {
        *has_at = touch->has_at;
        *at = touch->at;
        return true;
    }
    *has_at = false;
    *at = 0;
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, touch->address);
    ta_actor_inspection view;
    if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT || ta_actor_inspect(system->core, entry.actor, &view) != TA_OK) {
        return false;
    }
    if (view.has_completion) {
        return true; /* finished: nothing will need it again */
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

const tah_record_wake *tah_recorder_wakes(tah_system *system, uint32_t *count) {
    tah_recorder *recorder = &system->recorder;
    *count = 0;
    tah_vec_clear(&recorder->wakes);
    if (recorder->touched.count != 0 && tah_vec_reserve(&recorder->wakes, recorder->touched.count) != TA_OK) {
        recorder->lost += 1u;
        tah_recorder_forget_touched(recorder);
        return NULL;
    }
    for (uint32_t at = 0; recorder->arena_ready && at < recorder->touched.count; at += 1u) {
        const tah_recorder_touch *touch = tah_vec_at(&recorder->touched, at);
        tah_record_wake wake = { 0 };
        if (!tah_recorder_wake_of(system, touch, &wake.has_at, &wake.at)) {
            continue;
        }
        ta_actor_address address = ta_address_view(touch->address);
        if (!tah_recorder_copy(recorder, address.family.name, &wake.address.family.name) ||
            !tah_recorder_copy(recorder, address.name, &wake.address.name)) {
            recorder->lost += 1u;
            continue;
        }
        (void)tah_vec_push(&recorder->wakes, &wake);
    }
    tah_recorder_forget_touched(recorder);
    *count = recorder->wakes.count;
    return recorder->wakes.count == 0 ? NULL : recorder->wakes.items;
}

void tah_recorder_release(tah_system *system) {
    tah_recorder *recorder = &system->recorder;
    tah_recorder_clear(&recorder->records);
    tah_vec_clear(&recorder->wakes);
    if (recorder->arena_ready) {
        ta_arena_reset(&recorder->arena);
    }
}

uint64_t tah_recorder_lost(const tah_system *system) {
    return system->recorder.lost;
}
