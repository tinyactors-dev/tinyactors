#include "session_image.h"
#include "interpreter.h"
#include "timer.h"

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
    bool has_external_event;
    ta_event external_event;
    bool has_macrostep_event;
    ta_event macrostep_event;
    bool has_current_event;
    ta_event current_event;
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

static ta_result ta_image_copy_origin(ta_arena *arena, ta_event *event) {
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_detach(arena, origin);
}

/* Makes an event restored from an image hold this system's origin handle. */
static ta_result ta_image_localize_origin(ta_system *system, ta_event *event) {
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
        ta_actor_address view = ta_address_view(actor->record.self.as.named);
        total += sizeof(ta_address) + (size_t)view.family.name.len + (size_t)view.name.len + 2u;
    }
    if (actor->internal_queue != NULL) {
        total += (size_t)actor->internal_queue->count * sizeof(ta_queued_event);
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
        result = ta_image_copy_origin(arena, &image->external_event);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(arena, &image->macrostep_event);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(arena, &image->current_event);
    }
    if (result != TA_OK) {
        return result;
    }

    if (actor->internal_queue != NULL && actor->internal_queue->count != 0) {
        uint32_t count = actor->internal_queue->count;
        image->internal_events = ta_arena_alloc(arena, (size_t)count * sizeof(ta_queued_event),
                                                _Alignof(ta_queued_event));
        if (image->internal_events == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->internal_events[index] = actor->internal_queue->events[index];
            result = ta_image_copy_origin(arena, &image->internal_events[index].event);
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
    ta_value_retain(&image->external_event.data);
    ta_value_retain(&image->macrostep_event.data);
    ta_value_retain(&image->current_event.data);
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
        .source_epoch = actor->session_epoch,
        .self = actor->record.self,
        .definition = actor->definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .bootstrapped = actor->bootstrapped,
        .macrostep = actor->macrostep,
        .finished = actor->finished,
        /*
         * An event is the actor's only while its flag is set. A cleared flag
         * can leave stale bytes behind: taking the external event moves it
         * into current_event and keeps the copy so a failed microstep can put
         * it back, and releasing an event leaves its origin in place. The
         * image takes, and later retains, detaches and releases, only what
         * the flags say the actor holds.
         */
        .has_external_event = actor->has_external_event,
        .external_event = actor->has_external_event ? actor->external_event : (ta_event){0},
        .has_macrostep_event = actor->has_macrostep_event,
        .macrostep_event = actor->has_macrostep_event ? actor->macrostep_event : (ta_event){0},
        .has_current_event = actor->has_current_event,
        .current_event = actor->has_current_event ? actor->current_event : (ta_event){0},
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
    ta_value_release(&image->external_event.data);
    ta_value_release(&image->macrostep_event.data);
    ta_value_release(&image->current_event.data);
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
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
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
        ta_image_rebind_self(&send.destination, image->source_actor, actor->record.id);
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
    if (!actor->macrostep && ta_interpreter_internal_pending(actor)) {
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return TA_OK;
}

/*
 * Gives the actor its own copy of a microstep the image captured paused
 * inside executable content (ADR 0061 section 4); its snapshots are
 * retained again, so the image keeps its own.
 */
static ta_result ta_image_restore_pause(const ta_session_image *image, ta_actor *actor, ta_system *system) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_continuation *copy = ta_mem_alloc(system->allocator, image->paused->bytes, TA_CONTINUATION_ALIGN);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, image->paused, image->paused->bytes);
    ta_continuation_retain(copy);
    sparse->paused = copy;
    return TA_OK;
}

ta_result ta_actor_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params) {
    if (system == NULL || params == NULL || params->image == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_session_image *image = (ta_session_image *)params->image;
    ta_definition_id definition_id = 0;
    ta_result result = ta_session_image_definition(system, image, &definition_id);
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
    actor->next_send_id = image->next_send_id;
    actor->session_epoch = image->source_epoch;
    if (image->has_external_event) {
        ta_event_retain(&image->external_event, &actor->external_event);
        actor->has_external_event = true;
    }
    if (image->has_macrostep_event) {
        ta_event_retain(&image->macrostep_event, &actor->macrostep_event);
        actor->has_macrostep_event = true;
    }
    if (image->has_current_event) {
        ta_event_retain(&image->current_event, &actor->current_event);
        actor->has_current_event = true;
    }
    /*
     * The image carries detached addresses; the actor holds this system's
     * handles (ADR 0065). A failure leaves a detached one behind, which
     * releasing ignores.
     */
    result = ta_actor_ref_localize(system, &actor->record.self);
    if (result == TA_OK && actor->has_external_event) {
        result = ta_image_localize_origin(system, &actor->external_event);
    }
    if (result == TA_OK && actor->has_macrostep_event) {
        result = ta_image_localize_origin(system, &actor->macrostep_event);
    }
    if (result == TA_OK && actor->has_current_event) {
        result = ta_image_localize_origin(system, &actor->current_event);
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
    *params->out = id;
    return TA_OK;
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

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    if (result != TA_OK) {
        /* Still resident, still routable, and the same image will do. */
        return result;
    }

    /*
     * The route is gone, so the session may go. Its not-yet-submitted sends
     * live in the image now and must not also sit in the resident heap,
     * where they would fire for an actor that is not there; a send already
     * handed to the router is an envelope of its own and stays, exactly as
     * it does when a session ends. One sent to this session itself names it
     * by address from here on, so its retry finds the session that holds
     * the address next rather than this runtime ID, which goes stale.
     */
    ta_timer_readdress_self(ta_actor_timers(actor), actor->record.id, &actor->record.self);
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_actor_release_slot(system, actor->record.id);
    ta_actor_free(system, actor);
    return TA_OK;
}

ta_result ta_actor_load(ta_system *system, const ta_actor_load_params *params) {
    if (system == NULL || params == NULL || params->image == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
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
    ta_result result = ta_actor_reconstruct(
        system,
        &(ta_actor_reconstruct_params){ .image = image,
                                        .datamodel_binding = params->datamodel_binding,
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
    *params->out = id;
    return TA_OK;
}
