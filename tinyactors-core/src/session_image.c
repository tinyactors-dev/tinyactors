#include "session_image.h"
#include "byte_io.h"
#include "interpreter.h"
#include "timer.h"
#include "mail.h"

/*
 * A session image owns one arena, and every core-owned byte it keeps lives
 * there: the session storage, the queued events, the invocation records, the
 * delayed sends, and the text of the definition identity and of a named
 * address. That text would otherwise dangle — the identity's family name and
 * revision live in the *definition's* arena, and a named actor's address text
 * dies with the actor — and an image is meant to outlive both.
 *
 * Host values are not copied. They are retained, because a ta_value is a
 * handle and retain/release is its whole contract (ADR 0050). That is also
 * what makes capture all-or-nothing: every arena allocation happens first,
 * and only once all of them have succeeded does the image retain anything.
 * There is no half-retained image to unwind.
 */
struct ta_session_image {
    ta_allocator allocator;
    ta_arena arena;

    /* Local freshness: which actor this came from, and at which epoch. */
    ta_actor_id source_actor;
    /* The session this image continues (ADR 0066). */
    uint64_t session_id;
    uint64_t source_epoch;

    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;

    void *session_storage;
    size_t session_bytes;

    /* Execution state; the actor holds all of it between microsteps. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    /* The step counters, and whether the macrostep has begun (ADR 0067);
     * the macrostep's trace parent is in `slots`. */
    bool trace_begun;
    uint32_t trace_microstep;
    uint32_t trace_lifecycle;
    uint64_t trace_macrostep;
    /* The external and kept events and what `_event` names (ADR 0069). A
     * bound queue head is internal_events[0]. An operator transition waiting
     * with the external event is copied into the arena (ADR 0082). */
    ta_event_slots slots;
    uint64_t next_send_id;

    ta_queued_event *internal_events;
    uint32_t internal_count;
    /* Logical records only: a service handle is runtime-only and is dropped. */
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    ta_delayed_send *delayed_sends;
    uint32_t delayed_send_count;
    ta_state_id *pending_invokes;
    uint32_t pending_invoke_count;

    bool has_parent;
    ta_actor_parent parent;
    bool completion_pending;
    ta_actor_completion completion;
    /* The final state that ended the session, with `finished` (ADR 0056). */
    ta_state_id final_state;
    /* A microstep paused inside executable content (ADR 0061), or null. */
    ta_continuation *paused;
};

/* ------------------------------------------------------------------------
 * Arena-backed copies of the text an image must not borrow
 */

static ta_result ta_image_copy_str(ta_arena *arena, ta_str *text) {
    if (text->ptr == NULL) {
        return TA_OK;
    }
    char *copy = ta_arena_alloc(arena, (size_t)text->len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, text->ptr, text->len);
    copy[text->len] = '\0';
    text->ptr = copy;
    return TA_OK;
}

/*
 * An image carries its addresses as detached copies in its own arena (ADR
 * 0065): the strings, not the capturing system's handles, so the image does
 * not depend on that system (ADR 0028). A runtime ref owns nothing.
 */
static ta_result ta_image_copy_address(ta_arena *arena, ta_actor_ref *ref) {
    return ta_actor_ref_detach(arena, ref);
}

/*
 * An I/O origin goes into the image as text (ADR 0068): the processor type
 * and the URI, whatever its class. A closed scope, or a COMPUTED origin its
 * processor cannot render, is stored as no origin. Nothing is retained.
 */
static ta_result ta_image_copy_io_origin(const ta_system *system, ta_arena *arena, ta_origin *origin) {
    ta_str type = {0};
    ta_str uri = {0};
    ta_result result = ta_io_origin_type(system, origin, &type);
    if (result == TA_OK) {
        result = ta_io_origin_uri(system, &(ta_io_origin_render_params){
                                              .origin = origin,
                                              .arena = ta_arena_allocator(arena),
                                              .out = &uri,
                                          });
    }
    if (result == TA_ERR_NOT_FOUND) {
        *origin = (ta_origin){ .kind = TA_ORIGIN_NONE };
        return TA_OK;
    }
    ta_io_origin_entry *detached = NULL;
    if (result == TA_OK) {
        result = ta_io_origin_detach(arena, &(ta_io_origin_detach_params){ .type = type, .uri = uri, .out = &detached });
    }
    if (result != TA_OK) {
        return result;
    }
    *origin = (ta_origin){ .kind = TA_ORIGIN_IO,
                           .as.io = { .origin_class = TA_IO_ORIGIN_MESSAGE, .as.entry = detached } };
    return TA_OK;
}

static ta_result ta_image_copy_origin(const ta_system *system, ta_arena *arena, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        return ta_image_copy_io_origin(system, arena, &event->origin);
    }
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_detach(arena, origin);
}

/*
 * A detached I/O origin becomes a MESSAGE origin of this system holding its
 * URI. An unknown processor type leaves no origin: the event is delivered,
 * and only a reply is lost.
 */
static ta_result ta_image_localize_io_origin(ta_system *system, ta_origin *origin) {
    const ta_io_origin_entry *detached = origin->as.io.as.entry;
    if (origin->as.io.origin_class != TA_IO_ORIGIN_MESSAGE || !ta_io_origin_entry_detached(detached)) {
        return TA_OK;
    }
    ta_atom type = TA_ATOM_NONE;
    uint32_t processor = 0;
    if (ta_atom_find(&system->atoms, ta_io_origin_detached_type(detached), &type) != TA_OK ||
        ta_system_find_io(system, type, &processor) != TA_OK || processor == 0) {
        *origin = (ta_origin){ .kind = TA_ORIGIN_NONE };
        return TA_OK;
    }
    return ta_io_origin_make(system, &(ta_io_origin_make_params){
                                         .processor = processor,
                                         .origin_class = TA_IO_ORIGIN_MESSAGE,
                                         .uri = ta_io_origin_entry_uri(detached),
                                         .out = origin,
                                     });
}

/* Makes an event restored from an image hold this system's origin handle. */
static ta_result ta_image_localize_origin(ta_system *system, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        return ta_image_localize_io_origin(system, &event->origin);
    }
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_localize(system, origin);
}

static ta_result ta_image_copy_identity(ta_arena *arena, ta_definition_identity *identity) {
    ta_result result = ta_image_copy_str(arena, &identity->family.name);
    if (result != TA_OK) {
        return result;
    }
    ta_str revision = { .ptr = (const char *)identity->revision.ptr, .len = identity->revision.len };
    result = ta_image_copy_str(arena, &revision);
    identity->revision = (ta_bytes){ .ptr = (const uint8_t *)revision.ptr, .len = revision.len };
    return result;
}

/* ------------------------------------------------------------------------
 * Capture
 */

/* What the arena has to hold, so one block usually serves the whole image. */
static size_t ta_image_measure(const ta_actor *actor) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    size_t total = actor->session_bytes + 64u;
    total += (size_t)actor->definition->identity.family.name.len + 1u;
    total += (size_t)actor->definition->identity.revision.len + 1u;
    if (actor->record.self.kind == TA_ACTOR_REF_NAMED) {
        total += ta_address_bytes(actor->record.self.as.named);
    }
    if (actor->event_block != NULL) {
        total += (size_t)actor->event_block->count * sizeof(ta_queued_event);
        if (actor->event_block->slots.jump != NULL) {
            /* An estimate only: the jump stored the size it was allocated with. */
            total += actor->event_block->slots.jump->bytes + _Alignof(ta_operator_jump);
        }
    }
    if (sparse != NULL) {
        total += (size_t)sparse->invoke_count * sizeof(ta_active_invoke);
        total += (size_t)sparse->pending_invokes.count * sizeof(ta_state_id);
    }
    total += (size_t)ta_timer_copy_unsubmitted(ta_actor_timers(actor), NULL) * sizeof(ta_delayed_send);
    if (sparse != NULL && sparse->paused != NULL) {
        total += sparse->paused->bytes + TA_CONTINUATION_ALIGN;
    }
    return total;
}

/* Every arena allocation of a capture; nothing here retains anything. */
static ta_result ta_image_fill(ta_session_image *image, const ta_actor *actor) {
    ta_arena *arena = &image->arena;
    const ta_system *system = actor->host_impl.system;
    const ta_actor_sparse_state *sparse = actor->sparse;

    image->session_storage = ta_arena_alloc(arena, actor->session_bytes, _Alignof(uint64_t));
    if (image->session_storage == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(image->session_storage, actor->session_storage, actor->session_bytes);
    image->session_bytes = actor->session_bytes;

    ta_result result = ta_image_copy_identity(arena, &image->definition);
    if (result == TA_OK) {
        result = ta_image_copy_address(arena, &image->self);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(system, arena, &image->slots.external_event);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(system, arena, &image->slots.kept_event);
    }
    if (result != TA_OK) {
        return result;
    }

    /* An operator transition waiting with the external event (ADR 0082). */
    if (actor->event_block != NULL && actor->event_block->slots.jump != NULL) {
        size_t bytes = actor->event_block->slots.jump->bytes;
        image->slots.jump = ta_arena_alloc(arena, bytes, _Alignof(ta_operator_jump));
        if (image->slots.jump == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(image->slots.jump, actor->event_block->slots.jump, bytes);
    }

    if (actor->event_block != NULL && actor->event_block->count != 0) {
        uint32_t count = actor->event_block->count;
        image->internal_events = ta_arena_alloc(arena, (size_t)count * sizeof(ta_queued_event),
                                                _Alignof(ta_queued_event));
        if (image->internal_events == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->internal_events[index] = actor->event_block->events[index];
            result = ta_image_copy_origin(system, arena, &image->internal_events[index].event);
            if (result != TA_OK) {
                return result;
            }
        }
        image->internal_count = count;
    }

    if (sparse != NULL && sparse->invoke_count != 0) {
        image->invokes = ta_arena_alloc(arena, (size_t)sparse->invoke_count * sizeof(ta_active_invoke),
                                        _Alignof(ta_active_invoke));
        if (image->invokes == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < sparse->invoke_count; index += 1) {
            image->invokes[index] = sparse->invokes[index];
            /* Runtime-only: the service is gone by the time this is restored. */
            image->invokes[index].service_handle = 0;
        }
        image->invoke_count = sparse->invoke_count;
    }

    if (sparse != NULL && sparse->pending_invokes.count != 0) {
        uint32_t count = sparse->pending_invokes.count;
        image->pending_invokes = ta_arena_alloc(arena, (size_t)count * sizeof(ta_state_id),
                                                _Alignof(ta_state_id));
        if (image->pending_invokes == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->pending_invokes[index] = *(const ta_state_id *)ta_vec_at(&sparse->pending_invokes, index);
        }
        image->pending_invoke_count = count;
    }

    if (sparse != NULL && sparse->paused != NULL) {
        image->paused = ta_arena_alloc(arena, sparse->paused->bytes, TA_CONTINUATION_ALIGN);
        if (image->paused == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(image->paused, sparse->paused, sparse->paused->bytes);
    }

    uint32_t sends = ta_timer_copy_unsubmitted(ta_actor_timers(actor), NULL);
    if (sends != 0) {
        image->delayed_sends = ta_arena_alloc(arena, (size_t)sends * sizeof(ta_delayed_send),
                                              _Alignof(ta_delayed_send));
        if (image->delayed_sends == NULL) {
            return TA_ERR_NOMEM;
        }
        (void)ta_timer_copy_unsubmitted(ta_actor_timers(actor), image->delayed_sends);
        image->delayed_send_count = sends;
        for (uint32_t index = 0; index < sends; index += 1) {
            ta_delayed_send *send = &image->delayed_sends[index];
            result = ta_image_copy_address(arena, &send->destination);
            if (result == TA_OK) {
                result = ta_image_copy_address(arena, &send->origin);
            }
            if (result != TA_OK) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* The second half of a capture: infallible, so it cannot leave a half-image. */
static void ta_image_retain(ta_session_image *image) {
    ta_value_retain(&image->slots.external_event.data);
    ta_value_retain(&image->slots.kept_event.data);
    ta_value_retain(&image->completion.data);
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_value_retain(&image->internal_events[index].event.data);
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_value_retain(&image->delayed_sends[index].target);
        ta_value_retain(&image->delayed_sends[index].data);
    }
    if (image->paused != NULL) {
        ta_continuation_retain(image->paused);
    }
}

/* The actor's event slots, with only the events it holds (see below). */
static ta_event_slots ta_image_slots(const ta_actor *actor) {
    if (actor->event_block == NULL) {
        return (ta_event_slots){0};
    }
    const ta_event_slots *slots = &actor->event_block->slots;
    return (ta_event_slots){
        .external = slots->external,
        .bound = slots->bound,
        .bound_before = slots->bound_before,
        .has_kept = slots->has_kept,
        .external_event = slots->external != TA_EXTERNAL_NONE ? slots->external_event : (ta_event){0},
        .kept_event = slots->has_kept ? slots->kept_event : (ta_event){0},
        .trace = actor->macrostep ? slots->trace : (ta_trace_parent){0},
    };
}

ta_result ta_actor_capture(ta_system *system, const ta_actor_capture_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = NULL;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* An unpublished prepared replacement is not a routable session yet. */
    if (!actor->published) {
        return TA_ERR_BUSY;
    }

    ta_session_image *image = ta_mem_alloc(system->allocator, sizeof(ta_session_image),
                                           _Alignof(ta_session_image));
    if (image == NULL) {
        return TA_ERR_NOMEM;
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    *image = (ta_session_image){
        .allocator = system->allocator,
        .source_actor = actor->record.id,
        .session_id = actor->session_id,
        .source_epoch = actor->session_epoch,
        .self = actor->record.self,
        .definition = actor->definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .bootstrapped = actor->bootstrapped,
        .macrostep = actor->macrostep,
        .finished = actor->finished,
        .trace_begun = actor->trace_begun,
        .trace_microstep = actor->trace_microstep,
        .trace_lifecycle = actor->trace_lifecycle,
        .trace_macrostep = actor->trace_macrostep,
        /*
         * An event is the actor's only while its flag says so. Releasing an
         * event leaves its bytes, origin included, in place, so the image
         * takes, and later retains, detaches and releases, only what the
         * flags say the actor holds.
         */
        .slots = ta_image_slots(actor),
        .next_send_id = actor->next_send_id,
        .has_parent = sparse != NULL && sparse->has_parent,
        .parent = sparse != NULL ? sparse->parent : (ta_actor_parent){0},
        .completion_pending = sparse != NULL && sparse->completion_pending,
        .completion = sparse != NULL ? sparse->completion : (ta_actor_completion){0},
        .final_state = sparse != NULL && actor->finished ? sparse->final_state : TA_STATE_NONE,
    };
    ta_result result = ta_arena_init(&image->arena, system->allocator, ta_image_measure(actor));
    if (result == TA_OK) {
        result = ta_image_fill(image, actor);
    }
    if (result != TA_OK) {
        /* Nothing was retained yet, so the arena is the whole of it. */
        ta_arena_destroy(&image->arena);
        ta_mem_free(system->allocator, image,
                    &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                                 .alignment = _Alignof(ta_session_image) });
        return result;
    }
    ta_image_retain(image);
    *params->out = image;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Reading and releasing an image
 */

ta_result ta_session_image_next_deadline(const ta_session_image *image, ta_time *out) {
    if (image == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (image->delayed_send_count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_time earliest = image->delayed_sends[0].due;
    for (uint32_t index = 1; index < image->delayed_send_count; index += 1) {
        if (image->delayed_sends[index].due < earliest) {
            earliest = image->delayed_sends[index].due;
        }
    }
    *out = earliest;
    return TA_OK;
}

void ta_session_image_destroy(ta_session_image *image) {
    if (image == NULL) {
        return;
    }
    /* The mirror of ta_image_retain(), in the same order. */
    ta_value_release(&image->slots.external_event.data);
    ta_value_release(&image->slots.kept_event.data);
    ta_value_release(&image->completion.data);
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_value_release(&image->internal_events[index].event.data);
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_value_release(&image->delayed_sends[index].target);
        ta_value_release(&image->delayed_sends[index].data);
    }
    if (image->paused != NULL) {
        ta_continuation_release(image->paused);
    }
    ta_allocator allocator = image->allocator;
    ta_arena_destroy(&image->arena);
    ta_mem_free(allocator, image,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                             .alignment = _Alignof(ta_session_image) });
}

/* ------------------------------------------------------------------------
 * Reconstruction
 */

/*
 * The definition the image names, which must be the exact revision. A
 * different revision of the same family would reinterpret every state ID, so
 * it is a mismatch rather than a near miss; no definition of the family at
 * all is simply not found.
 */
ta_result ta_session_image_definition(const ta_system *system, const ta_session_image *image, ta_definition_id *out) {
    if (system == NULL || image == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_system_find_definition(system, &image->definition, out);
    if (result != TA_ERR_NOT_FOUND) {
        return result;
    }
    for (uint32_t slot = 0; slot < ta_pool_extent(&system->definitions); slot += 1) {
        const ta_definition_slot *entry = ta_system_definition_slot(system, slot);
        const ta_definition *definition = entry->definition;
        if (definition != NULL && !definition->retired &&
            ta_str_equal(definition->identity.family.name, image->definition.family.name)) {
            return TA_ERR_DEFINITION_MISMATCH;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/*
 * A runtime reference to the actor that was captured means "itself", and the
 * reconstructed session has a new runtime ID. Left alone, a self-addressed
 * delayed send would fire into the id of a session that no longer exists.
 */
static void ta_image_rebind_self(ta_actor_ref *ref, ta_actor_id was, ta_actor_id now) {
    if (ref->kind == TA_ACTOR_REF_RUNTIME && ref->as.runtime == was) {
        ref->as.runtime = now;
    }
}

/* Restores the queued events, invocation losses, and delayed sends. */
static ta_result ta_image_restore_session(ta_session_image *image, ta_actor *actor, ta_system *system) {
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_event event;
        ta_event_retain(&image->internal_events[index].event, &event);
        ta_result result = ta_image_localize_origin(system, &event);
        if (result != TA_OK) {
            ta_event_release(&event);
            return result;
        }
        result = ta_interpreter_enqueue_internal(system, actor, &event);
        if (result != TA_OK) {
            return result;
        }
    }
    if (actor->event_block != NULL) {
        actor->event_block->slots.bound = image->slots.bound;
        actor->event_block->slots.bound_before = image->slots.bound_before;
    }
    /*
     * Invoker service handles do not survive an image, so every invocation
     * that was active is retired and reported once, in the order it was
     * captured, after the events the image already carried.
     */
    for (uint32_t index = 0; index < image->invoke_count; index += 1) {
        if (!image->invokes[index].active) {
            continue;
        }
        ta_result result = ta_invoke_report_lost(system, actor, image->invokes[index].id);
        if (result != TA_OK) {
            return result;
        }
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_delayed_send send = image->delayed_sends[index];
        /* A fresh timer ID; the SCXML send ID that <cancel> names is kept. */
        send.id = 0;
        send.owner = actor->record.id;
        /* A destination naming this session is its session ID and survives a
         * reload as it is; only an unnamed actor's runtime origin moves. */
        ta_image_rebind_self(&send.origin, image->source_actor, actor->record.id);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        /* The image's detached addresses become this system's handles. */
        ta_result result = ta_actor_ref_localize(system, &send.destination);
        if (result == TA_OK) {
            result = ta_actor_ref_localize(system, &send.origin);
        }
        if (result == TA_OK) {
            result = ta_actor_schedule_send(system, actor, &send);
        }
        if (result != TA_OK) {
            ta_value_release(&send.target);
            ta_value_release(&send.data);
            ta_actor_ref_release(&send.destination);
            ta_actor_ref_release(&send.origin);
            return result;
        }
    }
    /*
     * Reporting the lost invocations put work in the queue of an actor the
     * image recorded as idle, so it has a macrostep to run again.
     */
    if (ta_interpreter_internal_pending(actor)) {
        ta_interpreter_start_macrostep(system, actor, NULL);
    }
    return TA_OK;
}

/*
 * Gives the actor its own copy of a microstep the image captured paused
 * inside executable content (ADR 0061 section 4); its snapshots are
 * retained again, so the image keeps its own.
 */
static ta_result ta_image_restore_pause(const ta_session_image *image, ta_actor *actor, ta_system *system) {
    /* The offsets are not trusted from the image: they are computed for the
     * actor's definition, and the image's counts and size must agree with
     * that shape before anything is allocated. */
    ta_continuation shape;
    size_t needed = 0;
    ta_result result = ta_continuation_capacity(actor->definition, &shape, &needed);
    if (result != TA_OK) {
        return result;
    }
    const ta_continuation *saved = image->paused;
    if (saved->bytes < needed || saved->words != shape.words || saved->decl_words != shape.decl_words ||
        saved->state_count != shape.state_count || saved->enabled_count > shape.enabled_count ||
        saved->frame_count > shape.frame_count) {
        return TA_ERR_INVALID;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_continuation *copy = ta_mem_alloc(system->allocator, saved->bytes, TA_CONTINUATION_ALIGN);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, saved, saved->bytes);
    copy->offsets = shape.offsets;
    ta_continuation_retain(copy);
    sparse->paused = copy;
    return TA_OK;
}

/* Reconstruction without its LOADED record, which a load writes only once
 * the directory publishes the session. */
static ta_result ta_image_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params) {
    if (system == NULL || params == NULL || params->image == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_session_image *image = (ta_session_image *)params->image;
    ta_definition_id definition_id = 0;
    ta_result result = ta_session_image_definition(system, image, &definition_id);
    if (result != TA_OK) {
        return result;
    }
    /* A restored macrostep, or the report of a lost invocation, starts one (run.h): room first. */
    result = ta_run_reserve_start(system);
    if (result != TA_OK) {
        return result;
    }
    ta_definition *definition = ta_system_definition(system, definition_id);
    ta_actor *actor = NULL;
    result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;
    /*
     * The image's session continues here unless a resident session holds
     * its ID (ADR 0066): two live sessions never share one. Its ID is not
     * this actor's runtime ID, so the session table finds it.
     */
    if (image->session_id != 0 && !ta_session_resident(system, image->session_id)) {
        uint32_t node = 0;
        result = ta_session_node_new(&system->sessions,
                                     &(ta_session_node_params){ .id = image->session_id, .actor = actor }, &node);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
        ta_session_insert(&system->sessions, node);
        actor->session_id = image->session_id;
    }

    /* The interpreter state the image describes, restored before anything
     * fallible runs against it. */
    ta_mem_copy(actor->session_storage, image->session_storage, image->session_bytes);
    /*
     * A named actor's logical self is its address and survives verbatim; an
     * anonymous one's self is its runtime ID, and this is a new session, so
     * it answers to the new one. Restoring the captured ID would leave the
     * actor addressing a session that no longer exists — every self-send,
     * every reply address, every delayed timer.
     */
    actor->record.self = image->self;
    if (image->self.kind == TA_ACTOR_REF_RUNTIME) {
        actor->record.self.as.runtime = id;
    }
    actor->record.residency = image->residency;
    actor->published = image->published;
    actor->bootstrapped = image->bootstrapped;
    actor->macrostep = image->macrostep;
    actor->finished = image->finished;
    actor->trace_begun = image->trace_begun;
    actor->trace_microstep = image->trace_microstep;
    /* Skip the ordinal an UNLOADED record takes after this image was
     * captured (ta_actor_unload validates the image first, then writes it),
     * so the LOADED that follows never reuses its key. After a reconstruct
     * the ordinal is simply unused. */
    actor->trace_lifecycle = image->trace_lifecycle + 1u;
    actor->trace_macrostep = image->trace_macrostep;
    actor->next_send_id = image->next_send_id;
    actor->session_epoch = image->source_epoch;
    /* The events live in an event block (ADR 0069). What `_event` names is
     * restored with the queue it may point into. */
    ta_event_slots *slots = NULL;
    if (image->slots.external != TA_EXTERNAL_NONE || image->slots.has_kept ||
        !ta_trace_parent_empty(&image->slots.trace)) {
        result = ta_interpreter_hold_block(system, actor);
        slots = result == TA_OK ? &actor->event_block->slots : NULL;
    }
    if (slots != NULL) {
        slots->trace = image->slots.trace;
    }
    if (slots != NULL && image->slots.external != TA_EXTERNAL_NONE) {
        ta_event_retain(&image->slots.external_event, &slots->external_event);
        slots->external = image->slots.external;
    }
    if (slots != NULL && image->slots.has_kept) {
        ta_event_retain(&image->slots.kept_event, &slots->kept_event);
        slots->has_kept = true;
    }
    /*
     * The image carries detached addresses; the actor holds this system's
     * handles (ADR 0065). A failure leaves a detached one behind, which
     * releasing ignores.
     */
    if (result == TA_OK) {
        result = ta_actor_ref_localize(system, &actor->record.self);
    }
    if (result == TA_OK && slots != NULL && slots->external != TA_EXTERNAL_NONE) {
        result = ta_image_localize_origin(system, &slots->external_event);
    }
    if (result == TA_OK && slots != NULL && slots->has_kept) {
        result = ta_image_localize_origin(system, &slots->kept_event);
    }
    if (result == TA_OK && slots != NULL && image->slots.jump != NULL) {
        /* The size is computed here from the count, not trusted from the image's
         * copy of the header. A size that does not fit is a refused allocation:
         * nothing is allocated, and the failure path after this block releases
         * what the restore took. */
        uint32_t bytes = 0;
        if (ta_operator_jump_bytes(image->slots.jump->target_count, &bytes) != TA_OK) {
            result = TA_ERR_NOMEM;
        } else {
            slots->jump = ta_mem_alloc(system->allocator, bytes, _Alignof(ta_operator_jump));
            if (slots->jump == NULL) {
                result = TA_ERR_NOMEM;
            } else {
                ta_mem_copy(slots->jump, image->slots.jump, bytes);
                slots->jump->bytes = bytes;
            }
        }
    }
    if (result == TA_OK && image->paused != NULL) {
        result = ta_image_restore_pause(image, actor, system);
    }

    if (result == TA_OK && (image->finished || image->has_parent || image->completion.has_data ||
                            image->internal_count != 0 || image->invoke_count != 0 ||
                            image->pending_invoke_count != 0)) {
        ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
        if (sparse == NULL) {
            result = TA_ERR_NOMEM;
        } else {
            sparse->has_parent = image->has_parent;
            sparse->parent = image->parent;
            sparse->completion_pending = image->completion_pending;
            sparse->completion = image->completion;
            ta_value_retain(&sparse->completion.data);
            sparse->final_state = image->final_state;
            if (image->pending_invoke_count != 0 && sparse->pending_invokes.element_size == 0) {
                ta_vec_init(&sparse->pending_invokes, system->allocator, TA_VEC_LAYOUT(ta_state_id));
            }
            for (uint32_t index = 0; result == TA_OK && index < image->pending_invoke_count; index += 1) {
                result = ta_vec_push(&sparse->pending_invokes, &image->pending_invokes[index]);
            }
        }
    }
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = params->datamodel_binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result == TA_OK) {
        result = ta_image_restore_session(image, actor, system);
    }
    if (result != TA_OK) {
        /* Nothing of this actor is published, so the image is untouched and
         * the caller may try again. */
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees its delayed sends too */
        return result;
    }
    if (actor->macrostep) {
        ta_run_start(system, actor); /* the image was captured inside a macrostep: ta_run continues it */
    }
    *params->out = id;
    return TA_OK;
}

ta_result ta_actor_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params) {
    ta_result result = ta_image_reconstruct(system, params);
    if (result == TA_OK) {
        ta_trace_lifecycle(system, ta_actor_lookup(system, *params->out), TA_TRACE_LOADED, NULL);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Residency: unload and load
 *
 * A residency change is not a lifecycle change. The actor stays logically
 * alive throughout, its name stays reserved, and the only thing that moves
 * is where its session lives. The two operations are mirror images, and
 * each orders its directory swap against its fallible work so that a
 * failure anywhere leaves the actor exactly where it started (ADR 0014):
 *
 *   unload  swaps first, then discards  — nothing is thrown away until the
 *                                         route is gone, so a refused swap
 *                                         leaves a resident actor intact
 *   load    builds first, then swaps    — nothing is routable until the
 *                                         session is whole, so a refused
 *                                         swap leaves an unloaded actor
 */

ta_result ta_actor_unload(ta_system *system, const ta_actor_unload_params *params) {
    if (system == NULL || params == NULL || params->image == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* Only a named actor can be unloaded: an anonymous session has no name
     * to hold its place, so evicting it would simply lose it. */
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    /*
     * Inputs admitted for it, and mail waiting for it, would be refused when
     * they came up (ADR 0099 section 5): the host waits. The mail its own
     * paused macrostep sent itself is not waited for: it is not visible yet.
     */
    if (actor->queued != ta_mail_outgoing_to(system, actor->record.id)) {
        return TA_ERR_BUSY;
    }
    const ta_session_image *image = params->image;
    /*
     * The image must describe this actor as it is now. An image captured
     * before the actor processed another event would check-point a session
     * the actor has already moved past, so the host is sent back to capture
     * again rather than silently persisting the older one.
     */
    if (image->source_actor != actor->record.id || image->source_epoch != actor->session_epoch) {
        return TA_ERR_STALE;
    }
    /* Room for the submitted sends to outlive the session, before anything changes. */
    ta_result result = ta_timer_reserve_orphans(&system->timers, ta_actor_timers(actor));
    if (result != TA_OK) {
        return result;
    }
    /* And the entry that keeps the session findable while it is out (ADR 0066). */
    uint32_t parked = 0;
    result = ta_session_node_new(&system->sessions, &(ta_session_node_params){ .id = actor->session_id }, &parked);
    if (result != TA_OK) {
        return result;
    }

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    if (result != TA_OK) {
        /* Still resident, still routable, and the same image will do. */
        ta_session_node_free(&system->sessions, parked);
        return result;
    }

    /*
     * The route is gone, so the session may go. Its not-yet-submitted sends
     * live in the image now and must not also sit in the resident heap,
     * where they would fire for an actor that is not there; a send already
     * handed to the router is an envelope of its own and stays, exactly as
     * it does when a session ends. One sent to this session names it by
     * session ID, and the parked entry routes its retry by address to
     * whatever session loads here next (ADR 0066).
     */
    ta_address *address = actor->record.self.as.named;
    ta_session_park(&system->sessions, parked, address); /* retains the address past the actor's own reference */
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_trace_lifecycle(system, actor, TA_TRACE_UNLOADED, NULL);
    ta_run_unloaded(system, actor);
    ta_actor_release_slot(system, actor->record.id);
    ta_actor_free(system, actor);
    return TA_OK;
}

/* A load from an in-memory image, with the binding to use. */
typedef struct {
    const ta_session_image *image;
    const ta_value *binding;
    const ta_trace_parent *trace;
    ta_actor_id *out;
} ta_image_load_params;

static ta_result ta_image_load(ta_system *system, const ta_image_load_params *params) {
    const ta_session_image *image = params->image;
    if (image->self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }

    /*
     * Built off-route: the directory still reads UNLOADED, so nothing can
     * reach this session until it is whole. Everything that can fail —
     * finding the definition, allocating, binding the data model, restoring
     * the queue and the timers — happens here.
     */
    ta_actor_id id = 0;
    ta_result result = ta_image_reconstruct(
        system,
        &(ta_actor_reconstruct_params){ .image = image,
                                        .datamodel_binding = params->binding,
                                        .out = &id });
    if (result != TA_OK) {
        return result;
    }

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = ta_actor_lookup(system, id)->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = id },
        });
    if (result != TA_OK) {
        /*
         * Nothing ever routed to this session, so taking it back down leaves
         * no trace: the actor is still unloaded, the image is untouched, and
         * the host may load it again.
         */
        ta_actor *actor = ta_actor_lookup(system, id);
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees its delayed sends too */
        return result;
    }
    /* The session is resident again: its parked entry goes (ADR 0066). */
    ta_actor *loaded = ta_actor_lookup(system, id);
    ta_address *address = loaded->record.self.as.named;
    ta_session_drop_unloaded(&system->sessions, address);
    ta_trace_lifecycle(system, loaded, TA_TRACE_LOADED, params->trace);
    ta_mail_address_resident(system, address, id); /* inputs that waited for the load go to the session, in order */
    *params->out = id;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Offloading (ADR 0074)
 *
 * An offloaded actor is its image as bytes, in this build's private layout,
 * read only by the system that wrote them. Offloading is capture, encode,
 * unload; loading is decode, then the load above. The image is the one
 * place the actor's state is gathered already, so going through it keeps
 * one definition of "what an unloaded actor is".
 *
 * The layout: a header; the session storage; the definition identity's
 * text; the self address; the invocation records and pending invokes, raw;
 * the delayed sends, raw, each followed by its destination and origin; and
 * the values, each through the actor's data model (value_write): the root,
 * the completion data, then each send's target and data. Raw records are
 * safe because nothing but this build reads them; their pointers and values
 * are rewritten on the way back in.
 */

#define TA_OFFLOAD_MAGIC 0x464f4154u /* "TAOF" */

typedef struct {
    uint32_t magic;
    uint32_t header_bytes;
    uint64_t system;
    uint64_t token;
    ta_actor_id source_actor;
    uint64_t session_id;
    uint64_t source_epoch;
    uint64_t trace_macrostep;
    uint64_t next_send_id;
    uint32_t trace_microstep;
    uint32_t trace_lifecycle;
    uint32_t residency;
    ta_state_id final_state;
    uint32_t session_bytes;
    uint32_t family_len;
    uint32_t revision_len;
    uint32_t invoke_count;
    uint32_t pending_invoke_count;
    uint32_t delayed_send_count;
    uint8_t published;
    uint8_t bootstrapped;
    uint8_t finished;
    uint8_t trace_begun;
} ta_offload_header;

/* Value `index` of an image, in the order the bytes carry them: the root,
 * the completion data, then each send's target and data. */
static ta_value *ta_offload_slot(ta_session_image *image, ta_value *root, uint32_t index) {
    if (index == 0) {
        return root;
    }
    if (index == 1) {
        return &image->completion.data;
    }
    ta_delayed_send *send = &image->delayed_sends[(index - 2u) / 2u];
    return (index - 2u) % 2u == 0 ? &send->target : &send->data;
}

/* A ref: the raw record, then a named one's address record. */
static size_t ta_offload_ref_bytes(const ta_actor_ref *ref) {
    size_t bytes = sizeof(ta_actor_ref);
    if (ref->kind == TA_ACTOR_REF_NAMED && ref->as.named != NULL) {
        bytes += sizeof(uint32_t) + ta_address_bytes(ref->as.named);
    }
    return bytes;
}

static uint8_t *ta_offload_put(uint8_t *at, const void *source, size_t bytes) {
    if (bytes != 0) {
        ta_mem_copy(at, source, bytes);
    }
    return at + bytes;
}

static uint8_t *ta_offload_put_ref(uint8_t *at, const ta_actor_ref *ref) {
    at = ta_offload_put(at, ref, sizeof(ta_actor_ref));
    if (ref->kind == TA_ACTOR_REF_NAMED && ref->as.named != NULL) {
        uint32_t bytes = (uint32_t)ta_address_bytes(ref->as.named);
        at = ta_offload_put(at, &bytes, sizeof bytes);
        at = ta_offload_put(at, ref->as.named, bytes);
    }
    return at;
}

/* One value through the data model; an absent value is stored as absent. */
typedef struct {
    const ta_definition *definition;
    const ta_value *value;
    ta_arena *arena;
    ta_bytes *out;
} ta_offload_value_params;

static ta_result ta_offload_encode_value(const ta_offload_value_params *params) {
    *params->out = (ta_bytes){0};
    if (params->value == NULL || params->value->v == NULL) {
        return TA_OK;
    }
    const ta_datamodel *datamodel = &params->definition->datamodel;
    if (datamodel->v->value_write == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_arena_sink state;
    ta_arena_sink_init(&state, params->arena);
    ta_byte_sink sink = ta_arena_sink_as(&state);
    ta_fault fault = {0};
    (void)sink.v->begin(sink.self);
    ta_op_status status = datamodel->v->value_write(
        datamodel->self, &(ta_datamodel_value_write_params){ .shared = params->definition->datamodel_shared,
                                                             .value = params->value,
                                                             .sink = sink,
                                                             .durable = false,
                                                             .fault = &fault });
    sink.v->end(sink.self, status == TA_OP_OK);
    if (status == TA_OP_ERROR) {
        ta_value_release(&fault.data);
    }
    if (status == TA_OP_NOMEM) {
        return TA_ERR_NOMEM;
    }
    if (status != TA_OP_OK || state.len > UINT32_MAX - 1u) {
        return TA_ERR_UNSUPPORTED;
    }
    *params->out = (ta_bytes){ .ptr = state.ptr, .len = (uint32_t)state.len };
    return TA_OK;
}

/* A stable actor only (ADR 0074 section 6): nothing runs, waits or is owed. */
static bool ta_offload_stable(const ta_session_image *image) {
    return !image->macrostep && image->paused == NULL && image->internal_count == 0 &&
           !image->completion_pending && !image->has_parent && image->slots.external == TA_EXTERNAL_NONE &&
           !image->slots.has_kept;
}

typedef struct {
    ta_session_image *image;
    const ta_definition *definition;
    const ta_value *root;
    ta_system *system;
    ta_arena *arena;
    ta_bytes *out;
} ta_offload_encode_params;

static size_t ta_offload_records_bytes(const ta_session_image *image) {
    size_t total = sizeof(ta_offload_header) + image->session_bytes + image->definition.family.name.len +
                   image->definition.revision.len + ta_offload_ref_bytes(&image->self) +
                   (size_t)image->invoke_count * sizeof(ta_active_invoke) +
                   (size_t)image->pending_invoke_count * sizeof(ta_state_id) +
                   (size_t)image->delayed_send_count * sizeof(ta_delayed_send);
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        total += ta_offload_ref_bytes(&image->delayed_sends[index].destination);
        total += ta_offload_ref_bytes(&image->delayed_sends[index].origin);
    }
    return total;
}

static uint8_t *ta_offload_put_records(uint8_t *at, const ta_session_image *image, const ta_offload_header *header) {
    at = ta_offload_put(at, header, sizeof *header);
    at = ta_offload_put(at, image->session_storage, image->session_bytes);
    at = ta_offload_put(at, image->definition.family.name.ptr, image->definition.family.name.len);
    at = ta_offload_put(at, image->definition.revision.ptr, image->definition.revision.len);
    at = ta_offload_put_ref(at, &image->self);
    at = ta_offload_put(at, image->invokes, (size_t)image->invoke_count * sizeof(ta_active_invoke));
    at = ta_offload_put(at, image->pending_invokes, (size_t)image->pending_invoke_count * sizeof(ta_state_id));
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        const ta_delayed_send *send = &image->delayed_sends[index];
        at = ta_offload_put(at, send, sizeof *send);
        at = ta_offload_put_ref(at, &send->destination);
        at = ta_offload_put_ref(at, &send->origin);
    }
    return at;
}

static ta_offload_header ta_offload_header_of(const ta_session_image *image, const ta_system *system) {
    return (ta_offload_header){
        .magic = TA_OFFLOAD_MAGIC,
        .header_bytes = sizeof(ta_offload_header),
        .system = (uint64_t)(uintptr_t)system,
        .token = system->offload_token,
        .source_actor = image->source_actor,
        .session_id = image->session_id,
        .source_epoch = image->source_epoch,
        .trace_macrostep = image->trace_macrostep,
        .next_send_id = image->next_send_id,
        .trace_microstep = image->trace_microstep,
        .trace_lifecycle = image->trace_lifecycle,
        .residency = (uint32_t)image->residency,
        .final_state = image->final_state,
        .session_bytes = (uint32_t)image->session_bytes,
        .family_len = image->definition.family.name.len,
        .revision_len = (uint32_t)image->definition.revision.len,
        .invoke_count = image->invoke_count,
        .pending_invoke_count = image->pending_invoke_count,
        .delayed_send_count = image->delayed_send_count,
        .published = image->published,
        .bootstrapped = image->bootstrapped,
        .finished = image->finished,
        .trace_begun = image->trace_begun,
    };
}

static ta_result ta_offload_encode(const ta_offload_encode_params *params) {
    ta_session_image *image = params->image;
    uint32_t value_count = 2u + 2u * image->delayed_send_count;
    ta_bytes *values = ta_arena_alloc(params->arena, (size_t)value_count * sizeof(ta_bytes), _Alignof(ta_bytes));
    if (values == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_value root = params->root != NULL ? *params->root : (ta_value){0};
    size_t total = ta_offload_records_bytes(image);
    for (uint32_t index = 0; index < value_count; index += 1) {
        ta_result result = ta_offload_encode_value(&(ta_offload_value_params){
            .definition = params->definition,
            .value = ta_offload_slot(image, &root, index),
            .arena = params->arena,
            .out = &values[index],
        });
        if (result != TA_OK) {
            return result;
        }
        total += sizeof(uint32_t) + values[index].len;
    }
    uint8_t *buffer = ta_arena_alloc(params->arena, total, _Alignof(uint64_t));
    if (buffer == NULL) {
        return TA_ERR_NOMEM;
    }
    if (params->system->offload_token == 0) {
        params->system->offload_token = ta_system_new_span_id(params->system);
    }
    ta_offload_header header = ta_offload_header_of(image, params->system);
    uint8_t *at = ta_offload_put_records(buffer, image, &header);
    for (uint32_t index = 0; index < value_count; index += 1) {
        const ta_value *value = ta_offload_slot(image, &root, index);
        /* A present value's length carries +1, so zero bytes still say "present". */
        uint32_t length = value->v == NULL ? 0u : (uint32_t)values[index].len + 1u;
        at = ta_offload_put(at, &length, sizeof length);
        at = ta_offload_put(at, values[index].ptr, values[index].len);
    }
    *params->out = (ta_bytes){ .ptr = buffer, .len = (size_t)(at - buffer) };
    return TA_OK;
}

ta_result ta_actor_offload(ta_system *system, const ta_actor_offload_params *params) {
    if (system == NULL || params == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_bytes){0};
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = actor->definition;
    ta_session_image *image = NULL;
    ta_result result = ta_actor_capture(system, &(ta_actor_capture_params){ .actor = params->actor, .out = &image });
    if (result != TA_OK) {
        return result;
    }
    ta_bytes bytes = {0};
    result = ta_offload_stable(image) ? TA_OK : TA_ERR_BUSY;
    if (result == TA_OK) {
        result = ta_offload_encode(&(ta_offload_encode_params){ .image = image,
                                                                .definition = definition,
                                                                .root = params->root,
                                                                .system = system,
                                                                .arena = params->arena,
                                                                .out = &bytes });
    }
    if (result == TA_OK) {
        result = ta_actor_unload(system, &(ta_actor_unload_params){ .actor = params->actor, .image = image });
    }
    ta_session_image_destroy(image);
    if (result == TA_OK) {
        *params->out = bytes;
    }
    return result;
}

typedef struct {
    const uint8_t *at;
    const uint8_t *end;
} ta_offload_reader;

static bool ta_offload_take(ta_offload_reader *reader, void *out, size_t bytes) {
    if ((size_t)(reader->end - reader->at) < bytes) {
        return false;
    }
    if (bytes != 0) {
        ta_mem_copy(out, reader->at, bytes);
    }
    reader->at += bytes;
    return true;
}

/* A ref as ta_offload_put_ref() wrote it; a named one's address goes into `arena`, detached. */
static ta_result ta_offload_take_ref(ta_offload_reader *reader, ta_arena *arena, ta_actor_ref *ref) {
    if (!ta_offload_take(reader, ref, sizeof *ref)) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_INVALID;
    }
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL) {
        return TA_OK;
    }
    ref->as.named = NULL;
    uint32_t bytes = 0;
    if (!ta_offload_take(reader, &bytes, sizeof bytes) || bytes < offsetof(ta_address, text) ||
        (size_t)(reader->end - reader->at) < bytes) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_INVALID;
    }
    ta_address *address = ta_arena_alloc(arena, bytes, _Alignof(ta_address));
    if (address == NULL) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_NOMEM;
    }
    (void)ta_offload_take(reader, address, bytes);
    size_t expected = 0;
    if (ta_address_record_bytes(address->family_len, address->name_len, &expected) != TA_OK || expected != bytes) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_INVALID;
    }
    ta_address_detached_init(address);
    ref->as.named = address;
    return TA_OK;
}

/* The text of the image's identity, copied into its arena. */
static ta_result ta_offload_take_identity(ta_offload_reader *reader, const ta_offload_header *header,
                                          ta_session_image *image) {
    char *family = ta_arena_alloc(&image->arena, (size_t)header->family_len + 1u, 1);
    uint8_t *revision = ta_arena_alloc(&image->arena, (size_t)header->revision_len + 1u, 1);
    if (family == NULL || revision == NULL) {
        return TA_ERR_NOMEM;
    }
    if (!ta_offload_take(reader, family, header->family_len) ||
        !ta_offload_take(reader, revision, header->revision_len)) {
        return TA_ERR_INVALID;
    }
    family[header->family_len] = '\0';
    image->definition = (ta_definition_identity){
        .family = { .name = { .ptr = family, .len = header->family_len } },
        .revision = { .ptr = revision, .len = header->revision_len },
    };
    return TA_OK;
}

/* An array of raw records, copied into the image's arena. */
typedef struct {
    ta_offload_reader *reader;
    ta_arena *arena;
    uint32_t count;
    size_t size;
    size_t alignment;
    void **out;
} ta_offload_array_params;

static ta_result ta_offload_take_array(const ta_offload_array_params *params) {
    *params->out = NULL;
    if (params->count == 0) {
        return TA_OK;
    }
    size_t bytes = (size_t)params->count * params->size;
    void *copy = ta_arena_alloc(params->arena, bytes, params->alignment);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (!ta_offload_take(params->reader, copy, bytes)) {
        return TA_ERR_INVALID;
    }
    *params->out = copy;
    return TA_OK;
}

static ta_result ta_offload_take_sends(ta_offload_reader *reader, uint32_t count, ta_session_image *image) {
    if (count == 0) {
        return TA_OK;
    }
    image->delayed_sends = ta_arena_alloc(&image->arena, (size_t)count * sizeof(ta_delayed_send),
                                          _Alignof(ta_delayed_send));
    if (image->delayed_sends == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        ta_delayed_send *send = &image->delayed_sends[index];
        if (!ta_offload_take(reader, send, sizeof *send)) {
            return TA_ERR_INVALID;
        }
        /* From here on the values and refs are this image's own. */
        send->target = (ta_value){0};
        send->data = (ta_value){0};
        send->origin = (ta_actor_ref){0};
        image->delayed_send_count = index + 1u;
        ta_result result = ta_offload_take_ref(reader, &image->arena, &send->destination);
        if (result == TA_OK) {
            result = ta_offload_take_ref(reader, &image->arena, &send->origin);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* Everything but the values: arena copies of the records, refs detached. */
static ta_result ta_offload_take_records(ta_offload_reader *reader, const ta_offload_header *header,
                                         ta_session_image *image) {
    ta_arena *arena = &image->arena;
    ta_result result = ta_offload_take_array(&(ta_offload_array_params){
        .reader = reader, .arena = arena, .count = header->session_bytes, .size = 1,
        .alignment = _Alignof(uint64_t), .out = &image->session_storage });
    image->session_bytes = header->session_bytes;
    if (result == TA_OK) {
        result = ta_offload_take_identity(reader, header, image);
    }
    if (result == TA_OK) {
        result = ta_offload_take_ref(reader, arena, &image->self);
    }
    if (result == TA_OK) {
        result = ta_offload_take_array(&(ta_offload_array_params){
            .reader = reader, .arena = arena, .count = header->invoke_count, .size = sizeof(ta_active_invoke),
            .alignment = _Alignof(ta_active_invoke), .out = (void **)&image->invokes });
        image->invoke_count = image->invokes != NULL ? header->invoke_count : 0;
    }
    if (result == TA_OK) {
        result = ta_offload_take_array(&(ta_offload_array_params){
            .reader = reader, .arena = arena, .count = header->pending_invoke_count, .size = sizeof(ta_state_id),
            .alignment = _Alignof(ta_state_id), .out = (void **)&image->pending_invokes });
        image->pending_invoke_count = image->pending_invokes != NULL ? header->pending_invoke_count : 0;
    }
    if (result == TA_OK) {
        result = ta_offload_take_sends(reader, header->delayed_send_count, image);
    }
    return result;
}

/* The values, through the data model of the definition the image names. */
static ta_result ta_offload_take_values(ta_offload_reader *reader, ta_system *system, ta_session_image *image,
                                        ta_value *root) {
    ta_definition_id definition_id = 0;
    ta_result result = ta_session_image_definition(system, image, &definition_id);
    if (result != TA_OK) {
        return result;
    }
    const ta_definition *definition = ta_system_definition(system, definition_id);
    const ta_datamodel *datamodel = &definition->datamodel;
    uint32_t value_count = 2u + 2u * image->delayed_send_count;
    for (uint32_t index = 0; index < value_count; index += 1) {
        uint32_t length = 0;
        if (!ta_offload_take(reader, &length, sizeof length) ||
            (length != 0 && (size_t)(reader->end - reader->at) < (size_t)length - 1u)) {
            return TA_ERR_INVALID;
        }
        if (length == 0) {
            continue;
        }
        ta_bytes bytes = { .ptr = reader->at, .len = (size_t)length - 1u };
        reader->at += length - 1u;
        if (datamodel->v->value_read == NULL) {
            return TA_ERR_UNSUPPORTED;
        }
        ta_value value = {0};
        ta_memory_source memory;
        ta_memory_source_init(&memory, bytes);
        ta_fault fault = {0};
        ta_op_status status = datamodel->v->value_read(
            datamodel->self,
            &(ta_datamodel_value_read_params){ .shared = definition->datamodel_shared,
                                               .source = ta_memory_source_as(&memory),
                                               .durable = false,
                                               .out = &value,
                                               .fault = &fault });
        if (status == TA_OP_ERROR) {
            ta_value_release(&fault.data);
        }
        if (status == TA_OP_NOMEM) {
            return TA_ERR_NOMEM;
        }
        if (status != TA_OP_OK) {
            return TA_ERR_INVALID;
        }
        *ta_offload_slot(image, root, index) = value;
    }
    image->completion.has_data = image->completion.data.v != NULL;
    return reader->at == reader->end ? TA_OK : TA_ERR_INVALID;
}

static void ta_offload_free_image(ta_system *system, ta_session_image *image) {
    ta_mem_free(system->allocator, image,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                             .alignment = _Alignof(ta_session_image) });
}

/* A fresh image for bytes whose header says they are this system's. */
static ta_result ta_offload_image(ta_system *system, const ta_offload_header *header, size_t bytes,
                                  ta_session_image **out) {
    ta_session_image *image = ta_mem_alloc(system->allocator, sizeof(ta_session_image), _Alignof(ta_session_image));
    if (image == NULL) {
        return TA_ERR_NOMEM;
    }
    *image = (ta_session_image){
        .allocator = system->allocator,
        .source_actor = header->source_actor,
        .session_id = header->session_id,
        .source_epoch = header->source_epoch,
        .residency = (ta_actor_residency)header->residency,
        .published = header->published != 0,
        .bootstrapped = header->bootstrapped != 0,
        .finished = header->finished != 0,
        .trace_begun = header->trace_begun != 0,
        .trace_microstep = header->trace_microstep,
        .trace_lifecycle = header->trace_lifecycle,
        .trace_macrostep = header->trace_macrostep,
        .next_send_id = header->next_send_id,
        .final_state = header->final_state,
    };
    ta_result result = ta_arena_init(&image->arena, system->allocator, bytes + 256u);
    if (result != TA_OK) {
        ta_offload_free_image(system, image);
        return result;
    }
    *out = image;
    return TA_OK;
}

/* Bytes this system offloaded, back as an image; nothing is kept on failure. */
static ta_result ta_offload_decode(ta_system *system, ta_bytes bytes, ta_session_image **out, ta_value *root) {
    *root = (ta_value){0};
    ta_offload_reader reader = { .at = bytes.ptr, .end = bytes.ptr + bytes.len };
    ta_offload_header header;
    if (bytes.ptr == NULL || !ta_offload_take(&reader, &header, sizeof header) || header.magic != TA_OFFLOAD_MAGIC ||
        header.header_bytes != sizeof(ta_offload_header)) {
        return TA_ERR_INVALID;
    }
    if (header.system != (uint64_t)(uintptr_t)system || system->offload_token == 0 ||
        header.token != system->offload_token) {
        return TA_ERR_STALE;
    }
    ta_session_image *image = NULL;
    ta_result result = ta_offload_image(system, &header, bytes.len, &image);
    if (result == TA_OK) {
        result = ta_offload_take_records(&reader, &header, image);
        if (result == TA_OK) {
            result = ta_offload_take_values(&reader, system, image, root);
        }
        if (result != TA_OK) {
            ta_value_release(root);
            *root = (ta_value){0};
            ta_session_image_destroy(image);
            return result;
        }
        *out = image;
    }
    return result;
}

ta_result ta_actor_load(ta_system *system, const ta_actor_load_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->root != NULL) {
        *params->root = (ta_value){0};
    }
    if (params->image != NULL) {
        return ta_image_load(system, &(ta_image_load_params){ .image = params->image,
                                                              .binding = params->datamodel_binding,
                                                              .trace = &params->trace,
                                                              .out = params->out });
    }
    ta_session_image *image = NULL;
    ta_value root = {0};
    ta_result result = ta_offload_decode(system, params->bytes, &image, &root);
    if (result != TA_OK) {
        return result;
    }
    const ta_value *binding = params->datamodel_binding;
    if (binding == NULL && root.v != NULL) {
        binding = &root;
    }
    result = ta_image_load(system, &(ta_image_load_params){ .image = image,
                                                            .binding = binding,
                                                            .trace = &params->trace,
                                                            .out = params->out });
    ta_session_image_destroy(image);
    if (result == TA_OK && params->root != NULL) {
        *params->root = root;
    } else {
        ta_value_release(&root);
    }
    return result;
}
