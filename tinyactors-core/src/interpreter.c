#include "interpreter.h"

/*
 * SCXML execution following the W3C algorithm (Appendix D). The actor's
 * configuration stores active atomic states; a microstep expands it into the
 * full active set, applies exits and entries to that set, and writes the
 * atomic states back.
 */

/* Executable content failed; error.execution is queued and the block stops (SCXML 4.9). */
#define TA_EXEC_FAILED 1

/* A block stopped at a pause point with its frames left open (ADR 0061). */
#define TA_EXEC_PAUSED 2

/* The root initial transition taken at bootstrap. */
#define TA_TRANSITION_BOOTSTRAP (UINT32_MAX - 1u)

typedef struct {
    ta_state_id source;
    ta_span32 targets;
    ta_transition_type type;
    ta_span32 actions;
} ta_transition_view;

static ta_transition_view ta_view(const ta_interpreter *it, ta_transition_id transition) {
    const ta_definition *definition = it->definition;
    if (transition == TA_TRANSITION_BOOTSTRAP) {
        const ta_initial_transition_def *initial = &definition->initial_transitions[definition->root_initial];
        return (ta_transition_view){
            .source = TA_STATE_NONE,
            .targets = initial->targets,
            .type = TA_TRANSITION_EXTERNAL,
            .actions = initial->actions,
        };
    }
    const ta_transition_def *def = &definition->transitions[transition];
    return (ta_transition_view){
        .source = def->source,
        .targets = def->targets,
        .type = def->type,
        .actions = def->actions,
    };
}

static const ta_state_def *ta_state(const ta_interpreter *it, ta_state_id state) {
    return &it->definition->states[state];
}

static bool ta_is_history(const ta_interpreter *it, ta_state_id state) {
    ta_state_kind kind = ta_state(it, state)->kind;
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static bool ta_descends(const ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    return ta_definition_is_descendant(it->definition, state, ancestor);
}

static ta_state_id ta_child_at(const ta_interpreter *it, ta_state_id state, uint32_t index) {
    return it->definition->children[ta_state(it, state)->children.offset + index];
}

static ta_state_id ta_target_at(const ta_interpreter *it, ta_span32 targets, uint32_t index) {
    return it->definition->targets[targets.offset + index];
}

static void ta_bits_clear(const ta_interpreter *it, uint64_t *words) {
    ta_mem_zero(words, it->words * sizeof(uint64_t));
}

static bool ta_bits_any(const ta_interpreter *it, const uint64_t *words) {
    for (uint32_t index = 0; index < it->words; index += 1) {
        if (words[index] != 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Events and errors
 */

static ta_actor_sparse_state *ta_sparse(ta_system *system, ta_actor *actor) {
    if (actor->sparse == NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            return NULL;
        }
        *actor->sparse = (ta_actor_sparse_state){ .final_state = TA_STATE_NONE };
    }
    return actor->sparse;
}

/* ------------------------------------------------------------------------
 * Internal queues (ADR 0064)
 */

/* A queue's first capacity, and the largest one the system caches. */
#define TA_INTERNAL_QUEUE_INITIAL 8u
#define TA_INTERNAL_QUEUE_CACHED_MAX 64u

size_t ta_internal_queue_bytes(uint32_t capacity) {
    return offsetof(ta_internal_queue, events) + (size_t)capacity * sizeof(ta_queued_event);
}

static void ta_queue_free(ta_system *system, ta_internal_queue *queue) {
    ta_mem_free(system->allocator, queue,
                &(ta_allocator_free_params){ .size = ta_internal_queue_bytes(queue->capacity),
                                             .alignment = _Alignof(ta_internal_queue) });
}

/*
 * Makes sure the actor holds a queue with room for one more event: its own,
 * else one from the system's cache, else a new one; a full queue doubles.
 * TA_ERR_NOMEM leaves the actor as it was, apart from a queue taken from
 * the cache, which it may keep.
 */
static ta_result ta_queue_room(ta_system *system, ta_actor *actor) {
    ta_internal_queue *queue = actor->internal_queue;
    if (queue == NULL) {
        if (system->queue_cache_count != 0) {
            system->queue_cache_count -= 1u;
            queue = system->queue_cache[system->queue_cache_count];
        } else {
            queue = ta_mem_alloc(system->allocator, ta_internal_queue_bytes(TA_INTERNAL_QUEUE_INITIAL),
                                 _Alignof(ta_internal_queue));
            if (queue == NULL) {
                return TA_ERR_NOMEM;
            }
            queue->capacity = TA_INTERNAL_QUEUE_INITIAL;
        }
        queue->count = 0;
        actor->internal_queue = queue;
    }
    if (queue->count < queue->capacity) {
        return TA_OK;
    }
    if (queue->capacity > UINT32_MAX / 2u) {
        return TA_ERR_NOMEM;
    }
    uint32_t capacity = queue->capacity * 2u;
    ta_internal_queue *grown = ta_mem_alloc(system->allocator, ta_internal_queue_bytes(capacity),
                                            _Alignof(ta_internal_queue));
    if (grown == NULL) {
        return TA_ERR_NOMEM;
    }
    grown->count = queue->count;
    grown->capacity = capacity;
    ta_mem_copy(grown->events, queue->events, (size_t)queue->count * sizeof(ta_queued_event));
    ta_queue_free(system, queue);
    actor->internal_queue = grown;
    return TA_OK;
}

/*
 * Gives an actor's queue back when the actor no longer needs it: it is
 * empty and no microstep is paused. It goes to the system's cache, or is
 * freed when the cache is full or the queue grew large. Cannot fail.
 */
static void ta_queue_settle(ta_system *system, ta_actor *actor) {
    ta_internal_queue *queue = actor->internal_queue;
    if (queue == NULL || queue->count != 0 || (actor->sparse != NULL && actor->sparse->paused != NULL)) {
        return;
    }
    actor->internal_queue = NULL;
    if (system->queue_cache_count < TA_INTERNAL_QUEUE_CACHE && queue->capacity <= TA_INTERNAL_QUEUE_CACHED_MAX) {
        system->queue_cache[system->queue_cache_count] = queue;
        system->queue_cache_count += 1u;
        return;
    }
    ta_queue_free(system, queue);
}

void ta_interpreter_queue_cache_destroy(ta_system *system) {
    while (system->queue_cache_count != 0) {
        system->queue_cache_count -= 1u;
        ta_queue_free(system, system->queue_cache[system->queue_cache_count]);
    }
}

ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event) {
    ta_actor_touch(actor);
    ta_result result = ta_queue_room(system, actor);
    if (result != TA_OK) {
        ta_queue_settle(system, actor);
        ta_event_release(event);
        return result;
    }
    ta_internal_queue *queue = actor->internal_queue;
    queue->events[queue->count] = (ta_queued_event){
        .event = *event,
        .flags = TA_EVENT_FLAG_OWNS_DATA | (event->type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
    };
    queue->count += 1u;
    return TA_OK;
}

static bool ta_dequeue_internal(ta_actor *actor, ta_event *out) {
    ta_internal_queue *queue = actor->internal_queue;
    if (queue == NULL || queue->count == 0) {
        return false;
    }
    *out = queue->events[0].event;
    for (uint32_t index = 1; index < queue->count; index += 1) {
        queue->events[index - 1u] = queue->events[index];
    }
    queue->count -= 1u;
    return true;
}

/* Puts the event selected for a failed microstep back where resume finds it. */
static void ta_restore_current_event(ta_actor *actor, bool external) {
    if (external) {
        /* ta_set_current_event moved the retained external_event into current_event. */
        actor->has_external_event = true;
    } else {
        /* The event came out of this queue, so it has room for it again. */
        ta_internal_queue *queue = actor->internal_queue;
        ta_queued_event *events = queue->events;
        for (uint32_t index = queue->count; index > 0; index -= 1u) {
            events[index] = events[index - 1u];
        }
        events[0] = (ta_queued_event){
            .event = actor->current_event,
            .flags = TA_EVENT_FLAG_OWNS_DATA |
                     (actor->current_event.type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
        };
        queue->count += 1u;
    }
    actor->current_event = (ta_event){0};
    actor->has_current_event = false;
}

typedef struct {
    ta_event_name_id name;
    ta_send_id send_id;
    ta_fault *fault;
} ta_error_params;

/* Queues a platform error event; the fault's data becomes the event data. */
static ta_result ta_raise_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    ta_event event = {
        .name = params->name,
        .type = TA_EVENT_PLATFORM,
        .send_id = params->send_id,
        .invoke_id = TA_INVOKE_ID_NONE,
    };
    if (params->fault != NULL) {
        event.data = params->fault->data;
        params->fault->data = (ta_value){0};
    }
    return ta_interpreter_enqueue_internal(system, actor, &event);
}

static ta_result ta_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

static void ta_set_current_event(ta_actor *actor, ta_event *event) {
    if (actor->has_current_event) {
        ta_event_release(&actor->current_event);
    }
    actor->current_event = *event;
    actor->has_current_event = true;
}

/* ------------------------------------------------------------------------
 * Data-model calls
 */

typedef struct {
    ta_value_source source;
    ta_value *out;
    ta_fault *fault;
} ta_value_params;

static ta_op_status ta_eval_source(ta_interpreter *it, const ta_value_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    *params->out = (ta_value){0};
    switch (params->source.kind) {
        case TA_VALUE_SOURCE_NONE:
            return TA_OP_OK;
        case TA_VALUE_SOURCE_EXPR:
            if (v->eval_value == NULL) {
                return TA_OP_ERROR;
            }
            return v->eval_value(definition->datamodel.self,
                                 &(ta_datamodel_eval_value_params){
                                     .shared = definition->datamodel_shared,
                                     .instance = it->actor->datamodel_instance,
                                     .expr = params->source.as.expr,
                                     .host = &it->actor->host,
                                     .out = params->out,
                                     .fault = params->fault,
                                 });
        case TA_VALUE_SOURCE_LOCATION:
            if (v->read_location == NULL) {
                return TA_OP_ERROR;
            }
            return v->read_location(definition->datamodel.self,
                                    &(ta_datamodel_read_location_params){
                                        .shared = definition->datamodel_shared,
                                        .instance = it->actor->datamodel_instance,
                                        .location = params->source.as.location,
                                        .host = &it->actor->host,
                                        .out = params->out,
                                        .fault = params->fault,
                                    });
        case TA_VALUE_SOURCE_CONTENT:
            if (v->import_content == NULL) {
                return TA_OP_ERROR;
            }
            return v->import_content(definition->datamodel.self,
                                     &(ta_datamodel_import_content_params){
                                         .shared = definition->datamodel_shared,
                                         .instance = it->actor->datamodel_instance,
                                         .content = definition->contents[params->source.as.content].bytes,
                                         .host = &it->actor->host,
                                         .out = params->out,
                                         .fault = params->fault,
                                     });
        default:
            return TA_OP_ERROR;
    }
}

/*
 * Evaluates an expression and reads its value as text through value_text.
 * `*text` is borrowed from `*holder`, which the caller releases after use.
 */
typedef struct {
    ta_expr_id expr;
    ta_value *holder;
    ta_str *text;
    ta_fault *fault;
} ta_text_eval_params;

static ta_op_status ta_eval_text(ta_interpreter *it, const ta_text_eval_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = ta_eval_source(it, &(ta_value_params){
                                                 .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = params->expr },
                                                 .out = params->holder,
                                                 .fault = params->fault,
                                             });
    if (status != TA_OP_OK) {
        return status;
    }
    if (v->value_text == NULL) {
        return TA_OP_ERROR;
    }
    *params->text = (ta_str){0};
    status = v->value_text(definition->datamodel.self,
                           &(ta_datamodel_value_text_params){
                               .shared = definition->datamodel_shared,
                               .instance = it->actor->datamodel_instance,
                               .value = params->holder,
                               .host = &it->actor->host,
                               .out = params->text,
                               .fault = params->fault,
                           });
    return status == TA_OP_OK && params->text->ptr == NULL && params->text->len != 0 ? TA_OP_ERROR : status;
}

static ta_op_status ta_eval_condition(ta_interpreter *it, ta_expr_id expr, bool *out, ta_fault *fault) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->eval_condition == NULL) {
        return TA_OP_ERROR;
    }
    return definition->datamodel.v->eval_condition(definition->datamodel.self,
                                                   &(ta_datamodel_eval_condition_params){
                                                       .shared = definition->datamodel_shared,
                                                       .instance = it->actor->datamodel_instance,
                                                       .expr = expr,
                                                       .host = &it->actor->host,
                                                       .out = out,
                                                       .fault = fault,
                                                   });
}

static ta_result ta_initialize_data(ta_interpreter *it, ta_state_id owner) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (uint32_t index = 0; index < definition->data_decl_count; index += 1) {
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner || decl->initializer == TA_INITIALIZER_NONE ||
            (it->bound_decls != NULL && ta_bits_get(it->bound_decls, index))) {
            continue;
        }
        ta_fault fault = {0};
        ta_value value = {0};
        ta_op_status status = ta_eval_source(it, &(ta_value_params){
                                                     .source = definition->initializers[decl->initializer].value,
                                                     .out = &value,
                                                     .fault = &fault,
                                                 });
        if (status == TA_OP_OK) {
            status = v->set_named == NULL
                         ? TA_OP_ERROR
                         : v->set_named(definition->datamodel.self,
                                        &(ta_datamodel_set_named_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = it->actor->datamodel_instance,
                                            .name = ta_atom_text(&it->system->atoms, decl->name),
                                            .value = &value,
                                            .host = &it->actor->host,
                                            .fault = &fault,
                                        });
            ta_value_release(&value);
        }
        if (status != TA_OP_OK) {
            /* The binding stays declared; the error does not stop other initializers. */
            ta_result result = ta_execution_error(it, &fault);
            if (result != TA_EXEC_FAILED) {
                return result;
            }
        }
    }
    return TA_OK;
}

static ta_result ta_declare_data(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (uint32_t index = 0; index < definition->data_decl_count && v->declare != NULL; index += 1) {
        ta_fault fault = {0};
        bool bound = false;
        ta_op_status status = v->declare(definition->datamodel.self,
                                         &(ta_datamodel_declare_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .name = ta_atom_text(&it->system->atoms,
                                                                  definition->data_decls[index].name),
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                             .bound = &bound,
                                         });
        if (bound && it->bound_decls != NULL) {
            ta_bits_put(it->bound_decls, index, true);
        }
        if (status != TA_OP_OK) {
            ta_result result = ta_execution_error(it, &fault);
            if (result != TA_EXEC_FAILED) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Sends
 */

static ta_send_id ta_literal_send_id(ta_atom atom) {
    return (ta_send_id)atom + 1u;
}

/*
 * Send IDs as text. A literal ID is the author's text (its atom); a generated
 * ID is TA_GENERATED_SEND_ID_PREFIX followed by the actor-local counter. Both
 * forms round-trip, so idlocation, sendidexpr, and _event.sendid agree.
 */
#define TA_GENERATED_SEND_ID_BIT ((ta_send_id)1u << 63)
#define TA_GENERATED_SEND_ID_PREFIX "#_send."

void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer) {
    if (id & TA_GENERATED_SEND_ID_BIT) {
        ta_text_put(writer, ta_str_from_cstr(TA_GENERATED_SEND_ID_PREFIX));
        ta_text_put_u64(writer, id & ~TA_GENERATED_SEND_ID_BIT);
    } else if (id != 0) {
        ta_text_put(writer, ta_atom_text(&system->atoms, (ta_atom)(id - 1u)));
    }
}

ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text) {
    ta_str prefix = ta_str_from_cstr(TA_GENERATED_SEND_ID_PREFIX);
    if (text.len > prefix.len && ta_str_equal((ta_str){ .ptr = text.ptr, .len = prefix.len }, prefix)) {
        uint64_t number = 0;
        bool digits = true;
        for (uint32_t index = prefix.len; index < text.len && digits; index += 1) {
            char digit = text.ptr[index];
            digits = digit >= '0' && digit <= '9' && number <= (UINT64_MAX >> 1) / 10u;
            number = number * 10u + (uint64_t)(digit - '0');
        }
        if (digits && number != 0 && (number & TA_GENERATED_SEND_ID_BIT) == 0) {
            return TA_GENERATED_SEND_ID_BIT | number;
        }
    }
    ta_atom atom = 0;
    return ta_atom_find(&system->atoms, text, &atom) == TA_OK ? ta_literal_send_id(atom) : 0;
}

ta_result ta_send_id_parse(const ta_system *system, ta_str text, ta_send_id *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_send_id id = ta_send_id_from_text(system, text);
    if (id == 0) {
        return TA_ERR_NOT_FOUND;
    }
    *out = id;
    return TA_OK;
}

ta_result ta_send_id_format(const ta_system *system, const ta_send_id_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->send_id == 0) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_send_id_write(system, params->send_id, &writer);
    return ta_text_finish(&writer, params->needed);
}

typedef struct {
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    ta_actor_ref destination;
    ta_event_name_id event;
    ta_send_id send_id;
    const ta_value *data;
    ta_fault *fault;
} ta_submit_params;

static ta_external_event_submit_status ta_submit_external(ta_system *system, const ta_submit_params *params) {
    if (system->external_events.v == NULL) {
        return TA_EXTERNAL_EVENT_ERROR;
    }
    /* A session ID that is not a resident runtime ID resolves here (ADR 0066). */
    ta_actor_ref destination;
    bool routed_by_name = ta_session_route(system, &params->destination, &destination);
    ta_external_delivery delivery = {
        .destination = destination,
        .event = {
            .name = params->event,
            .type = TA_EVENT_EXTERNAL,
            .send_id = params->send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = params->origin },
            .invoke_id = params->invoke_id,
            .data = *params->data,
        },
    };
    ta_external_event_submit_status status =
        system->external_events.v->submit(system->external_events.self,
                                          &(ta_external_event_submit_params){
                                              .delivery = &delivery,
                                              .fault = params->fault,
                                          });
    if (routed_by_name) {
        ta_actor_ref_release(&destination);
    }
    return status;
}

static ta_result ta_communication_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    if (actor->finished) {
        /* A send submitted before termination failed; the session can no longer observe it. */
        if (params->fault != NULL) {
            ta_value_release(&params->fault->data);
            params->fault->data = (ta_value){0};
        }
        return TA_OK;
    }
    ta_error_params error = *params;
    error.name = TA_EVENT_NAME_ERROR_COMMUNICATION;
    ta_result result = ta_raise_error(system, actor, &error);
    if (result == TA_OK && !actor->macrostep) {
        /* An asynchronous failure makes an idle actor runnable again. */
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return result;
}

void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes) {
    static const uint8_t empty[1] = { 0 };
    const ta_content_def *def = &definition->contents[content];
    *media_type = def->media_type;
    *bytes = def->bytes.ptr != NULL ? def->bytes : (ta_bytes){ .ptr = empty, .len = 0 };
}

ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send) {
    ta_actor *sender = ta_actor_lookup(system, send->owner);
    ta_fault fault = {0};
    ta_external_event_submit_status status = TA_EXTERNAL_EVENT_ERROR;
    /*
     * A submitted send was already sent: it is logically in the receiver's
     * queue and keeps its envelope even when the sending session is gone.
     */
    ta_actor_ref origin = send->submitted || sender == NULL ? send->origin : sender->record.self;
    /* Invoke: events from an invoked child to its parent carry the invoke ID. */
    ta_invoke_id invoke_id = send->submitted || sender == NULL
                                 ? send->invoke_id
                                 : ta_invoke_parent_id(sender, &send->destination);
    if (sender == NULL && !send->submitted) {
        status = TA_EXTERNAL_EVENT_ACCEPTED; /* the sender's session is gone; drop the send */
    } else if (!send->submitted && !ta_invoke_parent_accepts(system, sender, &send->destination)) {
        /* The parent already cancelled this invoked child. */
        status = TA_EXTERNAL_EVENT_ACCEPTED;
    } else if (ta_system_is_scxml_io(system, send->io_type)) {
        status = ta_submit_external(system, &(ta_submit_params){
                                                .origin = origin,
                                                .invoke_id = invoke_id,
                                                .destination = send->destination,
                                                .event = send->event,
                                                .send_id = send->send_id,
                                                .data = &send->data,
                                                .fault = &fault,
                                            });
    } else {
        const ta_io *io = ta_system_find_io(system, send->io_type);
        ta_send request = {
            .source = origin,
            .send_id = send->send_id,
            .event = send->event,
            .io_type = send->io_type,
            .target = send->target,
            .data = send->data,
        };
        /* A send to a host processor is dispatched while its sender lives,
         * so the content resolves against the sender's definition. */
        if (send->has_content && sender != NULL) {
            ta_interpreter_literal_content(sender->definition, send->content, &request.content_type,
                                           &request.content);
        }
        if (io != NULL && io->v->send(io->self, &request, &fault) == TA_OP_OK) {
            status = TA_EXTERNAL_EVENT_ACCEPTED;
        }
    }
    if (status == TA_EXTERNAL_EVENT_RETRY) {
        /*
         * The router accepted nothing (ADR 0040). The send is due, so it is
         * sent: it waits to be offered again with the envelope it has now,
         * <cancel> no longer reaches it, and it outlives its sender
         * (ADR 0054 D3, D5). The caller keeps it in the deadline index.
         */
        if (!send->submitted) {
            /* The stored send now owns its envelope's origin (ADR 0065). */
            ta_actor_ref_retain(&origin);
            ta_actor_ref_release(&send->origin);
            send->origin = origin;
            send->submitted = true;
            send->invoke_id = invoke_id;
        }
        ta_value_release(&fault.data);
        return status;
    }
    if (status == TA_EXTERNAL_EVENT_ERROR && sender != NULL) {
        (void)ta_communication_error(system, sender,
                                     &(ta_error_params){ .send_id = send->send_id, .fault = &fault });
    }
    ta_value_release(&fault.data);
    ta_value_release(&send->target);
    ta_value_release(&send->data);
    ta_actor_ref_release(&send->destination);
    ta_actor_ref_release(&send->origin);
    return status;
}

typedef struct {
    ta_value data;
    ta_value target;
    ta_scxml_target scxml;
    ta_atom io_type;
    ta_send_id send_id;
    bool scxml_io;
    bool has_content;
    ta_content_id content;
} ta_send_state;

static ta_op_status ta_send_payload(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                    ta_fault *fault) {
    if (!state->scxml_io && send->data.kind == TA_VALUE_SOURCE_CONTENT) {
        /*
         * A host I/O processor receives literal content as bytes: its
         * interpretation is platform-specific (SCXML 6.4.2), and only the
         * built-in SCXML processor, whose receiver reads _event.data, imports
         * it (ADR 0053).
         */
        state->has_content = true;
        state->content = send->data.as.content;
        return TA_OP_OK;
    }
    if (send->params.length == 0) {
        return ta_eval_source(it, &(ta_value_params){ .source = send->data, .out = &state->data, .fault = fault });
    }
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->make_params == NULL) {
        return TA_OP_ERROR;
    }
    ta_named_value *values = ta_arena_alloc(&it->system->scratch, send->params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        return TA_OP_ERROR;
    }
    uint32_t evaluated = 0;
    ta_op_status status = TA_OP_OK;
    for (; evaluated < send->params.length && status == TA_OP_OK; evaluated += 1) {
        const ta_param_def *param = &definition->params[send->params.offset + evaluated];
        values[evaluated].name = ta_atom_text(&it->system->atoms, param->name);
        status = ta_eval_source(it, &(ta_value_params){ .source = param->value, .out = &values[evaluated].value,
                                                        .fault = fault });
    }
    if (status == TA_OP_OK) {
        status = v->make_params(definition->datamodel.self,
                                &(ta_datamodel_make_params_params){
                                    .shared = definition->datamodel_shared,
                                    .instance = it->actor->datamodel_instance,
                                    .values = values,
                                    .count = send->params.length,
                                    .host = &it->actor->host,
                                    .out = &state->data,
                                    .fault = fault,
                                });
    }
    for (uint32_t index = 0; index < evaluated; index += 1) {
        ta_value_release(&values[index].value);
    }
    return status;
}

static ta_op_status ta_send_target(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                   ta_fault *fault) {
    if (!state->scxml_io) {
        /* A custom I/O processor receives the evaluated value, not a URI. */
        return send->target.kind == TA_VALUE_SOURCE_NONE
                   ? TA_OP_OK
                   : ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                            .fault = fault });
    }
    if (send->target.kind == TA_VALUE_SOURCE_NONE) {
        state->scxml = (ta_scxml_target){ .kind = TA_SCXML_TARGET_SELF };
        return TA_OP_OK;
    }
    /*
     * One grammar for both spellings (ADR 0044): a literal target is the
     * definition's text, and an evaluated one is the text of its value. A
     * named address borrows that text, which the send record keeps alive by
     * retaining the value.
     */
    ta_str text;
    if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes content = it->definition->contents[send->target.as.content].bytes;
        text = (ta_str){ .ptr = (const char *)content.ptr, .len = content.len };
    } else {
        const ta_definition *definition = it->definition;
        const ta_datamodel_vtable *v = definition->datamodel.v;
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                                     .fault = fault });
        if (status != TA_OP_OK) {
            return status;
        }
        if (v->value_text == NULL) {
            return TA_OP_ERROR;
        }
        status = v->value_text(definition->datamodel.self,
                               &(ta_datamodel_value_text_params){
                                   .shared = definition->datamodel_shared,
                                   .instance = it->actor->datamodel_instance,
                                   .value = &state->target,
                                   .host = &it->actor->host,
                                   .out = &text,
                                   .fault = fault,
                               });
        if (status != TA_OP_OK) {
            return status;
        }
    }
    return ta_scxml_target_parse(it->system, text, &state->scxml) == TA_OK ? TA_OP_OK : TA_OP_ERROR;
}

/* Resolves an SCXML target to an external destination; false means inaccessible. */
static bool ta_send_destination(ta_interpreter *it, const ta_scxml_target *target, ta_actor_ref *out) {
    switch (target->kind) {
        case TA_SCXML_TARGET_SELF:
            /*
             * A send to self names *this* session by its session ID (ADR
             * 0066), which a reload does not change. While the session is
             * resident and never reloaded it is also the runtime ID, so it
             * needs no directory; otherwise submission resolves it through
             * the session table (ta_session_route). What a receiver replies
             * to is the origin, which for a named actor is the address.
             */
            *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = it->actor->session_id };
            return true;
        case TA_SCXML_TARGET_PARENT:
            if (it->actor->sparse == NULL || !it->actor->sparse->has_parent) {
                return false;
            }
            *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = it->actor->sparse->parent.actor };
            return true;
        case TA_SCXML_TARGET_ACTOR:
            *out = target->as.actor;
            return true;
        case TA_SCXML_TARGET_INVOKE:
            return ta_invoke_destination(it->actor, target->as.invoke, out);
        default:
            return false;
    }
}

/* Resolves `type` / `typeexpr`; an unknown processor is an execution error. */
static ta_op_status ta_send_io_type(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                    ta_fault *fault) {
    ta_system *system = it->system;
    ta_atom type = system->scxml_io_type;
    if (send->type.kind == TA_TEXT_SOURCE_ATOM) {
        type = send->type.as.atom;
    } else if (send->type.kind == TA_TEXT_SOURCE_EXPR) {
        ta_value holder = {0};
        ta_str text = {0};
        ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                                   .expr = send->type.as.expr, .holder = &holder,
                                                   .text = &text, .fault = fault,
                                               });
        if (status == TA_OP_OK && (text.len == 0 || ta_atom_intern(&system->atoms, text, &type) != TA_OK)) {
            status = TA_OP_ERROR;
        }
        ta_value_release(&holder);
        if (status != TA_OP_OK) {
            return status;
        }
    }
    state->scxml_io = ta_system_is_scxml_io(system, type);
    state->io_type = state->scxml_io ? system->scxml_io_type : type;
    return state->scxml_io || ta_system_find_io(system, type) != NULL ? TA_OP_OK : TA_OP_ERROR;
}

/* Resolves `event` / `eventexpr`; the expression's text is a dotted event name. */
static ta_op_status ta_send_event_name(ta_interpreter *it, const ta_send_action *send, ta_event_name_id *out,
                                       ta_fault *fault) {
    if (send->event.kind == TA_EVENT_SOURCE_NAME) {
        *out = send->event.as.name;
        return TA_OP_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                               .expr = send->event.as.expr, .holder = &holder,
                                               .text = &text, .fault = fault,
                                           });
    if (status == TA_OP_OK && ta_event_name_intern_dotted(&it->system->event_names, text, out) != TA_OK) {
        status = TA_OP_ERROR;
    }
    ta_value_release(&holder);
    return status;
}

/* Resolves `delay` / `delayexpr`; the expression's text is an SCXML duration. */
static ta_op_status ta_send_delay(ta_interpreter *it, const ta_send_action *send, ta_time *out, ta_fault *fault) {
    *out = 0;
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        *out = send->delay.as.literal;
        return TA_OP_OK;
    }
    if (send->delay.kind != TA_DELAY_SOURCE_EXPR) {
        return TA_OP_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                               .expr = send->delay.as.expr, .holder = &holder,
                                               .text = &text, .fault = fault,
                                           });
    if (status == TA_OP_OK && ta_duration_parse(text, out) != TA_OK) {
        status = TA_OP_ERROR;
    }
    ta_value_release(&holder);
    return status;
}

/* Stores generated text, such as an ID, through a location (`idlocation`). */
typedef struct {
    ta_expr_id location;
    ta_str text;
    ta_fault *fault;
} ta_store_text_params;

static ta_op_status ta_store_text(ta_interpreter *it, const ta_store_text_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->text_value == NULL || v->assign == NULL) {
        return TA_OP_ERROR;
    }
    ta_value value = {0};
    ta_op_status status = v->text_value(definition->datamodel.self,
                                        &(ta_datamodel_text_value_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = it->actor->datamodel_instance,
                                            .text = params->text,
                                            .host = &it->actor->host,
                                            .out = &value,
                                            .fault = params->fault,
                                        });
    if (status == TA_OP_OK) {
        status = v->assign(definition->datamodel.self,
                           &(ta_datamodel_assign_params){
                               .shared = definition->datamodel_shared,
                               .instance = it->actor->datamodel_instance,
                               .location = params->location,
                               .value = &value,
                               .host = &it->actor->host,
                               .fault = params->fault,
                           });
    }
    ta_value_release(&value);
    return status;
}

/*
 * Assigns the send ID. Without `id` or `idlocation` the send has no ID, so
 * its events carry a blank sendid (SCXML 5.10.1).
 */
static ta_op_status ta_send_assign_id(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                               ta_fault *fault) {
    state->send_id = 0;
    if (send->id.kind == TA_ID_SOURCE_LITERAL) {
        state->send_id = ta_literal_send_id(send->id.as.literal);
        return TA_OP_OK;
    }
    if (send->id.kind != TA_ID_SOURCE_LOCATION) {
        return TA_OP_OK;
    }
    it->actor->next_send_id += 1u;
    state->send_id = TA_GENERATED_SEND_ID_BIT | it->actor->next_send_id;
    char buffer[48];
    ta_text_writer writer = { .buffer = buffer, .capacity = sizeof(buffer) };
    ta_send_id_write(it->system, state->send_id, &writer);
    return ta_store_text(it, &(ta_store_text_params){
                                 .location = send->id.as.location,
                                 .text = { .ptr = buffer, .len = writer.length },
                                 .fault = fault,
                             });
}

/* An error while evaluating a <send> carries that send's ID (SCXML 5.10.1). */
static ta_result ta_send_execution_error(ta_interpreter *it, ta_send_id send_id, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION,
                                                          .send_id = send_id, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

static ta_result ta_exec_send(ta_interpreter *it, const ta_send_action *send) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    ta_send_state state = { .io_type = system->scxml_io_type, .scxml_io = true };
    ta_fault fault = {0};
    ta_event_name_id event_name = 0;
    ta_time delay = 0;
    ta_op_status status = ta_send_assign_id(it, send, &state, &fault);
    if (status == TA_OP_OK) {
        status = ta_send_event_name(it, send, &event_name, &fault);
    }
    if (status == TA_OP_OK) {
        status = ta_send_io_type(it, send, &state, &fault);
    }
    if (status == TA_OP_OK) {
        status = ta_send_delay(it, send, &delay, &fault);
    }
    if (status != TA_OP_OK) {
        return ta_send_execution_error(it, state.send_id, &fault);
    }
    status = ta_send_payload(it, send, &state, &fault);
    if (status == TA_OP_OK) {
        status = ta_send_target(it, send, &state, &fault);
    }
    if (status == TA_OP_OK && state.scxml_io && state.scxml.kind == TA_SCXML_TARGET_INTERNAL && delay != 0) {
        status = TA_OP_ERROR; /* a delayed send cannot target the internal queue */
    }
    if (status != TA_OP_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        ta_scxml_target_release(&state.scxml);
        return ta_send_execution_error(it, state.send_id, &fault);
    }

    if (state.scxml_io && state.scxml.kind == TA_SCXML_TARGET_INTERNAL) {
        ta_value_release(&state.target);
        ta_event event = {
            .name = event_name,
            .type = TA_EVENT_INTERNAL,
            .send_id = state.send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
            .invoke_id = TA_INVOKE_ID_NONE,
            .data = state.data,
        };
        /* The queued event owns its origin reference (ADR 0065). */
        ta_actor_ref_retain(&event.origin.as.actor);
        return ta_interpreter_enqueue_internal(system, actor, &event);
    }

    ta_delayed_send record = {
        .owner = actor->record.id,
        .send_id = state.send_id,
        .due = ta_clock_now(&system->clock) + delay,
        .event = event_name,
        .io_type = state.io_type,
        .target = state.target,
        .data = state.data,
        .has_content = state.has_content,
        .content = state.content,
    };
    /* A named target's reference passes to the send record (ADR 0065). */
    if (state.scxml_io && !ta_send_destination(it, &state.scxml, &record.destination)) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        ta_scxml_target_release(&state.scxml);
        return ta_communication_error(system, actor, &(ta_error_params){ .send_id = state.send_id });
    }
    if (!actor->published) {
        ta_result result = ta_actor_hold_send(
            system, actor, &(ta_activation_send){ .send = record, .immediate = delay == 0 });
        if (result != TA_OK) {
            ta_value_release(&state.data);
            ta_value_release(&state.target);
            ta_actor_ref_release(&record.destination);
        }
        return result;
    }
    if (delay == 0) {
        ta_external_event_submit_status submitted = ta_interpreter_dispatch_delayed(system, &record);
        if (submitted != TA_EXTERNAL_EVENT_RETRY) {
            return TA_OK;
        }
        /*
         * The router accepted nothing, typically because the destination is
         * this busy actor. The send, now marked submitted, waits in the
         * deadline index, due now, and keeps its order among later sends.
         */
    }
    ta_result result = ta_actor_schedule_send(system, actor, &record);
    if (result != TA_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        ta_actor_ref_release(&record.destination);
        ta_actor_ref_release(&record.origin);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Executable content
 */

/*
 * <foreach> iterates a shallow copy made by the data model. A failed
 * snapshot or binding, or an error in the body, stops the foreach and the
 * enclosing block (SCXML 4.6). The loop is a frame of the executor (see
 * ta_run_block): `actions` is the body, `cursor` the <foreach> action
 * itself, and the snapshot and position are the frame's.
 */
static ta_result ta_foreach_begin(ta_interpreter *it, ta_exec_frame *loop) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->foreach_snapshot == NULL || v->foreach_bind == NULL) {
        return ta_execution_error(it, NULL);
    }
    ta_fault fault = {0};
    if (v->foreach_snapshot(definition->datamodel.self,
                            &(ta_datamodel_foreach_snapshot_params){
                                .shared = definition->datamodel_shared,
                                .instance = it->actor->datamodel_instance,
                                .array = definition->action_defs[loop->cursor].as.foreach.array,
                                .host = &it->actor->host,
                                .out = &loop->iteration_snapshot,
                                .count = &loop->iteration_count,
                                .fault = &fault,
                            }) != TA_OP_OK) {
        return ta_execution_error(it, &fault);
    }
    return TA_OK;
}

/* Binds the item and index for the loop's next position. */
static ta_result ta_foreach_bind(ta_interpreter *it, ta_exec_frame *loop) {
    const ta_definition *definition = it->definition;
    const ta_foreach_action *foreach = &definition->action_defs[loop->cursor].as.foreach;
    ta_str item = ta_atom_text(&it->system->atoms, foreach->item);
    ta_str index = foreach->has_index ? ta_atom_text(&it->system->atoms, foreach->index) : (ta_str){0};
    ta_fault fault = {0};
    ta_op_status bound = definition->datamodel.v->foreach_bind(definition->datamodel.self,
                                                               &(ta_datamodel_foreach_bind_params){
                                                                   .shared = definition->datamodel_shared,
                                                                   .instance = it->actor->datamodel_instance,
                                                                   .snapshot = &loop->iteration_snapshot,
                                                                   .index = loop->iteration,
                                                                   .item = item,
                                                                   .index_name = index,
                                                                   .host = &it->actor->host,
                                                                   .fault = &fault,
                                                               });
    loop->iteration += 1u;
    return bound == TA_OP_OK ? TA_OK : ta_execution_error(it, &fault);
}

static ta_result ta_exec_action(ta_interpreter *it, const ta_action_def *action) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_fault fault = {0};
    ta_op_status status = TA_OP_OK;
    switch (action->kind) {
        case TA_ACTION_RAISE: {
            ta_value data = {0};
            status = ta_eval_source(it, &(ta_value_params){ .source = action->as.raise.data, .out = &data,
                                                            .fault = &fault });
            if (status != TA_OP_OK) {
                break;
            }
            ta_event event = {
                .name = action->as.raise.event,
                .type = TA_EVENT_INTERNAL,
                .invoke_id = TA_INVOKE_ID_NONE,
                .data = data,
            };
            return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
        }
        case TA_ACTION_SEND:
            return ta_exec_send(it, &action->as.send);
        case TA_ACTION_CANCEL: {
            ta_send_id send_id = 0;
            if (action->as.cancel.sendid.kind == TA_TEXT_SOURCE_ATOM) {
                send_id = ta_literal_send_id(action->as.cancel.sendid.as.atom);
            } else {
                ta_value holder = {0};
                ta_str text = {0};
                status = ta_eval_text(it, &(ta_text_eval_params){
                                              .expr = action->as.cancel.sendid.as.expr, .holder = &holder,
                                              .text = &text, .fault = &fault,
                                          });
                if (status == TA_OP_OK) {
                    send_id = ta_send_id_from_text(it->system, text);
                }
                ta_value_release(&holder);
            }
            if (status == TA_OP_OK && send_id != 0) {
                (void)ta_actor_cancel_held_send(it->actor, send_id);
                (void)ta_timer_cancel(&it->system->timers, ta_actor_timers(it->actor), send_id);
            }
            break;
        }
        case TA_ACTION_ASSIGN: {
            ta_value value = {0};
            status = ta_eval_source(it, &(ta_value_params){ .source = action->as.assign.value, .out = &value,
                                                            .fault = &fault });
            if (status == TA_OP_OK) {
                status = v->assign == NULL
                             ? TA_OP_ERROR
                             : v->assign(definition->datamodel.self,
                                         &(ta_datamodel_assign_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .location = action->as.assign.location,
                                             .value = &value,
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                         });
                ta_value_release(&value);
            }
            break;
        }
        case TA_ACTION_SCRIPT:
            status = v->execute_script == NULL
                         ? TA_OP_ERROR
                         : v->execute_script(definition->datamodel.self,
                                             &(ta_datamodel_execute_script_params){
                                                 .shared = definition->datamodel_shared,
                                                 .instance = it->actor->datamodel_instance,
                                                 .script = action->as.script,
                                                 .host = &it->actor->host,
                                                 .fault = &fault,
                                             });
            break;
        case TA_ACTION_LOG: {
            /*
             * SCXML 4.7: evaluating the expression can fail, which is an
             * ordinary execution error; writing the record cannot, so a
             * <log> never changes interpretation.
             */
            ta_log_record record = { .source = it->actor->record.self };
            if (action->as.log.has_label) {
                record.label = ta_atom_text(&it->system->atoms, action->as.log.label);
            }
            if (action->as.log.has_expr) {
                status = ta_eval_source(it, &(ta_value_params){
                                                .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = action->as.log.expr },
                                                .out = &record.value,
                                                .fault = &fault,
                                            });
                record.has_value = status == TA_OP_OK;
            }
            if (status == TA_OP_OK && it->system->log_sink.v != NULL) {
                if (it->actor->published) {
                    it->system->log_sink.v->write(it->system->log_sink.self, &record);
                } else {
                    ta_result result = ta_actor_hold_log(it->system, it->actor, &record);
                    if (result != TA_OK) {
                        ta_value_release(&record.value);
                        return result;
                    }
                }
            }
            ta_value_release(&record.value);
            break;
        }
        /* <if> and <foreach> open blocks, which ta_run_block runs. */
        default:
            status = TA_OP_ERROR;
            break;
    }
    return status == TA_OP_OK ? TA_OK : ta_execution_error(it, &fault);
}

static ta_result ta_exec_push(ta_interpreter *it, ta_exec_frame frame) {
    if (it->exec_count == it->exec_capacity) {
        return TA_ERR_INVALID; /* unreachable: the definition's depth sizes the frames */
    }
    it->exec_frames[it->exec_count++] = frame;
    return TA_OK;
}

static ta_result ta_exec_block(ta_interpreter *it, ta_span32 block) {
    return ta_exec_push(it, (ta_exec_frame){ .kind = TA_EXEC_FRAME_ACTIONS, .actions = block });
}

/*
 * Runs a block of executable content and every block nested in it, without
 * recursion (ADR 0060). Each open block is an ACTIONS frame; an <if> opens
 * the block its condition picks, and a <foreach> is a FOREACH frame that
 * opens its body once per position. The frames come from the interpreter's
 * scratch, so nothing here allocates them. An error anywhere stops the whole
 * block, as SCXML 4.9 requires, and every snapshot still held is released.
 */
/*
 * Whether the executor stops before its next unit (ADR 0061): only in a
 * microstep's own block, never in <finalize> or at termination, and only
 * once this call has run its action budget.
 */
static bool ta_exec_should_pause(const ta_interpreter *it, uint32_t floor) {
    return it->pausable && it->in_microstep && floor == 0 && it->action_budget != 0 &&
           it->actions >= it->action_budget;
}

static ta_result ta_run_block(ta_interpreter *it, ta_span32 block) {
    const ta_definition *definition = it->definition;
    uint32_t floor = it->exec_count;
    ta_result result = TA_OK;
    if (it->at.block_open) {
        /* Resuming: the frames the pause left, from the bottom, are this
         * block's, which a pause only ever leaves at the microstep's level. */
        floor = 0;
        it->at.block_open = false;
    } else {
        it->at.block_open = false;
        result = ta_exec_block(it, block);
    }
    while (result == TA_OK && it->exec_count > floor) {
        ta_exec_frame *frame = &it->exec_frames[it->exec_count - 1u];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            if (frame->iteration == frame->iteration_count) {
                ta_value_release(&frame->iteration_snapshot);
                it->exec_count -= 1u;
                continue;
            }
            if (ta_exec_should_pause(it, floor)) {
                it->at.block_open = true;
                return TA_EXEC_PAUSED;
            }
            it->actions += 1u;
            result = ta_foreach_bind(it, frame);
            if (result == TA_OK) {
                result = ta_exec_block(it, frame->actions);
            }
            continue;
        }
        if (frame->cursor == frame->actions.length) {
            it->exec_count -= 1u;
            continue;
        }
        if (ta_exec_should_pause(it, floor)) {
            it->at.block_open = true;
            return TA_EXEC_PAUSED;
        }
        it->actions += 1u;
        uint32_t index = frame->actions.offset + frame->cursor;
        frame->cursor += 1u;
        const ta_action_def *action = &definition->action_defs[index];
        if (action->kind == TA_ACTION_IF) {
            bool condition = false;
            ta_fault fault = {0};
            ta_op_status status = ta_eval_condition(it, action->as.conditional.condition, &condition, &fault);
            result = status != TA_OP_OK ? ta_execution_error(it, &fault)
                                        : ta_exec_block(it, condition ? action->as.conditional.then_actions
                                                                      : action->as.conditional.else_actions);
        } else if (action->kind == TA_ACTION_FOREACH) {
            ta_exec_frame loop = { .kind = TA_EXEC_FRAME_FOREACH, .actions = action->as.foreach.body_actions,
                                   .cursor = index };
            result = ta_foreach_begin(it, &loop);
            if (result == TA_OK) {
                result = ta_exec_push(it, loop);
                if (result != TA_OK) {
                    ta_value_release(&loop.iteration_snapshot);
                }
            }
        } else {
            result = ta_exec_action(it, action);
        }
    }
    while (it->exec_count > floor) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    return result;
}

/* A failed block has already queued its error; execution continues with the next block. */
static ta_result ta_run_block_isolated(ta_interpreter *it, ta_span32 block) {
    ta_result result = ta_run_block(it, block);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/*
 * Runs a state's <onentry> or <onexit> handlers in document order. Each
 * handler is a separate block (SCXML 3.8, 3.9): an error skips the rest of
 * its own handler only.
 */
static ta_result ta_run_handlers(ta_interpreter *it, ta_span32 handlers) {
    /* `at.handler` is where a paused microstep resumes; it is 0 otherwise. */
    for (; it->at.handler < handlers.length; it->at.handler += 1u) {
        ta_result result = ta_run_block_isolated(it, it->definition->handlers[handlers.offset + it->at.handler]);
        if (result != TA_OK) {
            if (result != TA_EXEC_PAUSED) {
                it->at.handler = 0;
            }
            return result;
        }
    }
    it->at.handler = 0;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Transition selection
 */

/*
 * Adds a span of targets to `out`: a target itself, or for a history state
 * its recorded configuration, or else the targets of its default
 * transition, which may name history states again. Those are expanded from
 * a worklist, each at most once, so a history default that leads back to
 * itself ends instead of looping.
 */
static void ta_effective_span(ta_interpreter *it, ta_span32 targets, uint64_t *out, uint32_t *pending) {
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_state_id target = ta_target_at(it, targets, index);
        if (!ta_is_history(it, target)) {
            ta_bits_put(out, target, true);
        } else if (!ta_bits_get(it->history_seen, target)) {
            ta_bits_put(it->history_seen, target, true);
            it->history_stack[(*pending)++] = target;
        }
    }
}

static void ta_effective_targets(ta_interpreter *it, ta_span32 targets, uint64_t *out) {
    ta_bits_clear(it, it->history_seen);
    uint32_t pending = 0;
    ta_effective_span(it, targets, out, &pending);
    while (pending != 0) {
        ta_state_id history = it->history_stack[--pending];
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, ta_state(it, history)->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (uint32_t word = 0; word < it->words; word += 1) {
                out[word] |= recorded[word];
            }
        } else {
            const ta_transition_def *fallback = &it->definition->transitions[ta_state(it, history)->transitions.offset];
            ta_effective_span(it, fallback->targets, out, &pending);
        }
    }
}

/* False when the transition is targetless and so has no domain. */
static bool ta_transition_domain(ta_interpreter *it, ta_transition_id transition, ta_state_id *out) {
    ta_transition_view view = ta_view(it, transition);
    if (view.targets.length == 0) {
        return false;
    }
    if (view.source == TA_STATE_NONE) {
        *out = TA_STATE_NONE;
        return true;
    }
    uint64_t *targets = it->domain_targets;
    ta_bits_clear(it, targets);
    ta_effective_targets(it, view.targets, targets);
    const ta_definition *definition = it->definition;
    if (view.type == TA_TRANSITION_INTERNAL && ta_state(it, view.source)->kind == TA_STATE_COMPOUND) {
        bool inside = true;
        for (ta_state_id state = 0; state < definition->state_count && inside; state += 1) {
            inside = !ta_bits_get(targets, state) || ta_descends(it, state, view.source);
        }
        if (inside) {
            *out = view.source;
            return true;
        }
    }
    /* The least common compound ancestor of the source and every target. */
    for (ta_state_id ancestor = ta_state(it, view.source)->parent; ancestor != TA_STATE_NONE;
         ancestor = ta_state(it, ancestor)->parent) {
        if (ta_state(it, ancestor)->kind != TA_STATE_COMPOUND) {
            continue;
        }
        bool common = true;
        for (ta_state_id state = 0; state < definition->state_count && common; state += 1) {
            common = !ta_bits_get(targets, state) || ta_descends(it, state, ancestor);
        }
        if (common) {
            *out = ancestor;
            return true;
        }
    }
    *out = TA_STATE_NONE;
    return true;
}

static void ta_exit_set(ta_interpreter *it, ta_transition_id transition, uint64_t *out) {
    ta_state_id domain = TA_STATE_NONE;
    if (!ta_transition_domain(it, transition, &domain)) {
        return;
    }
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_descends(it, state, domain)) {
            ta_bits_put(out, state, true);
        }
    }
}

static void ta_remove_conflicts(ta_interpreter *it) {
    uint32_t kept = 0;
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_transition_id candidate = it->enabled[index];
        ta_bits_clear(it, it->exit_left);
        ta_exit_set(it, candidate, it->exit_left);
        bool preempted = false;
        for (uint32_t other = 0; other < kept && !preempted; other += 1) {
            it->remove[other] = false;
            ta_bits_clear(it, it->exit_right);
            ta_exit_set(it, it->filtered[other], it->exit_right);
            bool overlap = false;
            for (uint32_t word = 0; word < it->words && !overlap; word += 1) {
                overlap = (it->exit_left[word] & it->exit_right[word]) != 0;
            }
            if (!overlap) {
                continue;
            }
            if (ta_descends(it, ta_view(it, candidate).source, ta_view(it, it->filtered[other]).source)) {
                it->remove[other] = true;
            } else {
                preempted = true;
            }
        }
        if (preempted) {
            continue;
        }
        uint32_t write = 0;
        for (uint32_t other = 0; other < kept; other += 1) {
            if (!it->remove[other]) {
                it->filtered[write++] = it->filtered[other];
            }
        }
        kept = write;
        it->filtered[kept++] = candidate;
    }
    for (uint32_t index = 0; index < kept; index += 1) {
        it->enabled[index] = it->filtered[index];
    }
    it->enabled_count = kept;
}

static bool ta_transition_matches(ta_interpreter *it, const ta_transition_def *transition, const ta_event *event) {
    if (event == NULL) {
        return transition->event_patterns.length == 0;
    }
    for (uint32_t index = 0; index < transition->event_patterns.length; index += 1) {
        if (ta_event_pattern_matches(it->definition,
                                     &it->definition->event_patterns[transition->event_patterns.offset + index],
                                     event->name)) {
            return true;
        }
    }
    return false;
}

/* Selects the optimal enabled transition set; event is null for eventless selection. */
static ta_result ta_select_transitions(ta_interpreter *it, const ta_event *event) {
    const ta_definition *definition = it->definition;
    it->enabled_count = 0;
    for (ta_state_id atomic = 0; atomic < definition->state_count; atomic += 1) {
        if (!ta_bits_get(it->active, atomic) || !ta_definition_is_atomic(definition, atomic)) {
            continue;
        }
        bool found = false;
        for (ta_state_id state = atomic; state != TA_STATE_NONE && !found; state = ta_state(it, state)->parent) {
            ta_span32 transitions = ta_state(it, state)->transitions;
            for (uint32_t index = 0; index < transitions.length && !found; index += 1) {
                ta_transition_id id = transitions.offset + index;
                const ta_transition_def *transition = &definition->transitions[id];
                if (!ta_transition_matches(it, transition, event)) {
                    continue;
                }
                bool enabled = true;
                if (transition->condition != UINT32_MAX) {
                    ta_fault fault = {0};
                    if (ta_eval_condition(it, transition->condition, &enabled, &fault) != TA_OP_OK) {
                        enabled = false;
                        ta_result result = ta_execution_error(it, &fault);
                        if (result != TA_EXEC_FAILED) {
                            return result;
                        }
                    }
                }
                if (!enabled) {
                    continue;
                }
                found = true;
                bool duplicate = false;
                for (uint32_t other = 0; other < it->enabled_count && !duplicate; other += 1) {
                    duplicate = it->enabled[other] == id;
                }
                if (!duplicate) {
                    it->enabled[it->enabled_count++] = id;
                }
            }
        }
    }
    ta_remove_conflicts(it);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Microsteps
 */

static bool ta_entering_below(ta_interpreter *it, ta_state_id ancestor) {
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->to_enter, state) && ta_descends(it, state, ancestor)) {
            return true;
        }
    }
    return false;
}

/*
 * The entry set (SCXML addDescendantStatesToEnter and
 * addAncestorStatesToEnter), without recursion (ADR 0060). Each frame is one
 * activation of the recursive algorithm, resumed where it left off, so the
 * order of every step, which ta_entering_below and default_history depend
 * on, is the recursion's:
 *
 *   DESCENDANTS  addDescendantStatesToEnter(state), not yet started
 *   TARGETS      for each target: descendants; then for each: ancestors
 *   RECORDED     the same over a history state's recorded configuration
 *   ANCESTORS    addAncestorStatesToEnter, `state` the next ancestor
 *   REGIONS      a parallel state's regions, `cursor` the next child
 *
 * A history frame marks its history state busy until it ends. A history
 * default that leads back to a busy history state would recurse forever;
 * it is skipped. Every other chain of descendant frames goes to strictly
 * deeper states. So a chain holds at most one descendants frame per level
 * of nesting and one per history state, and each of those has at most two
 * frames above it (a loop, and an ancestors walk with its regions).
 */
typedef enum {
    TA_ENTRY_DESCENDANTS = 0,
    TA_ENTRY_TARGETS,
    TA_ENTRY_RECORDED,
    TA_ENTRY_ANCESTORS,
    TA_ENTRY_REGIONS
} ta_entry_kind;

struct ta_entry_frame {
    ta_entry_kind kind;
    bool ancestors;         /* TARGETS, RECORDED: the second loop */
    ta_state_id state;
    ta_state_id ancestor;
    ta_state_id history;    /* the history state that owns this frame, or TA_STATE_NONE */
    ta_span32 targets;
    uint32_t cursor;
};

/* The frame capacity ta_interpreter_init reserves: three per level of
 * nesting and per history state, and room for the first frame. */
static uint32_t ta_entry_capacity(const ta_definition *definition) {
    return 3u * (definition->max_state_depth + definition->history.slot_count + 1u) + 1u;
}

static bool ta_entry_push(ta_interpreter *it, uint32_t *top, struct ta_entry_frame frame) {
    if (*top == it->entry_capacity) {
        return false; /* unreachable: the bound above holds */
    }
    it->entry_frames[(*top)++] = frame;
    return true;
}

static struct ta_entry_frame ta_entry_descendants(ta_state_id state) {
    return (struct ta_entry_frame){ .kind = TA_ENTRY_DESCENDANTS, .state = state, .history = TA_STATE_NONE };
}

static struct ta_entry_frame ta_entry_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    return (struct ta_entry_frame){ .kind = TA_ENTRY_ANCESTORS, .state = ta_state(it, state)->parent,
                                    .ancestor = ancestor, .history = TA_STATE_NONE };
}

/* Starts addDescendantStatesToEnter(frame->state): rewrites the frame into
 * the loop the recursion would run, or pops it. */
static void ta_entry_start(ta_interpreter *it, uint32_t *top) {
    struct ta_entry_frame *frame = &it->entry_frames[*top - 1u];
    ta_state_id state = frame->state;
    const ta_state_def *def = ta_state(it, state);
    if (ta_is_history(it, state)) {
        if (ta_bits_get(it->history_busy, state)) {
            *top -= 1u;
            return;
        }
        ta_bits_put(it->history_busy, state, true);
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        if (ta_bits_any(it, recorded)) {
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_RECORDED, .state = state, .ancestor = def->parent,
                                              .history = state };
        } else {
            it->default_history[def->parent] = state;
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = def->parent, .history = state,
                                              .targets = it->definition->transitions[def->transitions.offset].targets };
        }
        return;
    }
    ta_bits_put(it->to_enter, state, true);
    if (def->kind == TA_STATE_COMPOUND) {
        ta_bits_put(it->default_entry, state, true);
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = state, .history = TA_STATE_NONE,
                                          .targets = it->definition->initial_transitions[def->initial].targets };
    } else if (def->kind == TA_STATE_PARALLEL) {
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_REGIONS, .state = state, .history = TA_STATE_NONE };
    } else {
        *top -= 1u;
    }
}

static void ta_entry_pop(ta_interpreter *it, uint32_t *top) {
    ta_state_id history = it->entry_frames[*top - 1u].history;
    if (history != TA_STATE_NONE) {
        ta_bits_put(it->history_busy, history, false);
    }
    *top -= 1u;
}

/* The next recorded state at or after `from`, or state_count. */
static ta_state_id ta_entry_next_recorded(ta_interpreter *it, const struct ta_entry_frame *frame, ta_state_id from) {
    const uint64_t *recorded =
        ta_history_slot_words(&it->actor->history, ta_state(it, frame->state)->history_slot);
    while (from < it->definition->state_count && !ta_bits_get(recorded, from)) {
        from += 1u;
    }
    return from;
}

/* One step of the frame on top of the stack. */
static bool ta_entry_step(ta_interpreter *it, uint32_t *top) {
    struct ta_entry_frame *frame = &it->entry_frames[*top - 1u];
    switch (frame->kind) {
        case TA_ENTRY_DESCENDANTS:
            ta_entry_start(it, top);
            return true;
        case TA_ENTRY_TARGETS: {
            if (frame->cursor == frame->targets.length) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            ta_state_id target = ta_target_at(it, frame->targets, frame->cursor++);
            return ta_entry_push(it, top, frame->ancestors ? ta_entry_ancestors(it, target, frame->ancestor)
                                                           : ta_entry_descendants(target));
        }
        case TA_ENTRY_RECORDED: {
            ta_state_id next = ta_entry_next_recorded(it, frame, frame->cursor);
            if (next == it->definition->state_count) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            frame->cursor = next + 1u;
            return ta_entry_push(it, top, frame->ancestors ? ta_entry_ancestors(it, next, frame->ancestor)
                                                           : ta_entry_descendants(next));
        }
        case TA_ENTRY_ANCESTORS: {
            ta_state_id at = frame->state;
            if (at == frame->ancestor || at == TA_STATE_NONE) {
                ta_entry_pop(it, top);
                return true;
            }
            ta_bits_put(it->to_enter, at, true);
            frame->state = ta_state(it, at)->parent;
            if (ta_state(it, at)->kind == TA_STATE_PARALLEL) {
                return ta_entry_push(it, top, (struct ta_entry_frame){ .kind = TA_ENTRY_REGIONS, .state = at,
                                                                       .history = TA_STATE_NONE });
            }
            return true;
        }
        case TA_ENTRY_REGIONS: {
            ta_state_id parallel = frame->state;
            while (frame->cursor < ta_state(it, parallel)->children.length) {
                ta_state_id child = ta_child_at(it, parallel, frame->cursor++);
                if (!ta_is_history(it, child) && !ta_entering_below(it, child)) {
                    return ta_entry_push(it, top, ta_entry_descendants(child));
                }
            }
            ta_entry_pop(it, top);
            return true;
        }
    }
    return false;
}

static void ta_entry_run(ta_interpreter *it, struct ta_entry_frame first) {
    uint32_t top = 0;
    if (!ta_entry_push(it, &top, first)) {
        return;
    }
    while (top != 0 && ta_entry_step(it, &top)) {
    }
}

static void ta_add_descendants(ta_interpreter *it, ta_state_id state) {
    ta_entry_run(it, ta_entry_descendants(state));
}

static void ta_add_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    ta_entry_run(it, ta_entry_ancestors(it, state, ancestor));
}

/* The first state after `state`'s subtree: states are numbered in document
 * pre-order, so a subtree is a contiguous range of IDs. */
static ta_state_id ta_subtree_end(ta_interpreter *it, ta_state_id state) {
    ta_state_id last = state;
    while (ta_state(it, last)->children.length != 0) {
        last = ta_child_at(it, last, ta_state(it, last)->children.length - 1u);
    }
    return last + 1u;
}

/*
 * SCXML isInFinalState, bottom up in one pass (ADR 0060 section 4): every
 * child has a higher ID than its parent, so visiting the subtree in reverse
 * ID order answers each child before its parent.
 */
static bool ta_in_final_state(ta_interpreter *it, ta_state_id state) {
    for (ta_state_id at = ta_subtree_end(it, state); at > state;) {
        at -= 1u;
        const ta_state_def *def = ta_state(it, at);
        bool final = false;
        if (def->kind == TA_STATE_COMPOUND) {
            for (uint32_t index = 0; index < def->children.length && !final; index += 1) {
                ta_state_id child = ta_child_at(it, at, index);
                final = ta_state(it, child)->kind == TA_STATE_FINAL && ta_bits_get(it->active, child);
            }
        } else if (def->kind == TA_STATE_PARALLEL) {
            final = true;
            for (uint32_t index = 0; index < def->children.length && final; index += 1) {
                ta_state_id child = ta_child_at(it, at, index);
                final = ta_is_history(it, child) || ta_bits_get(it->final_states, child);
            }
        }
        ta_bits_put(it->final_states, at, final);
    }
    return ta_bits_get(it->final_states, state);
}

/* Queues done.state.<state>; takes ownership of data, which may be empty. */
static ta_result ta_raise_done(ta_interpreter *it, ta_state_id state, ta_value *data) {
    ta_event event = {
        .name = ta_state(it, state)->done_event,
        .type = TA_EVENT_INTERNAL,
        .invoke_id = TA_INVOKE_ID_NONE,
        .data = *data,
    };
    *data = (ta_value){0};
    return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
}

/*
 * <donedata> <param>s: a param whose value fails is ignored after queueing
 * error.execution (SCXML 5.7); the rest still form the payload.
 */
static ta_result ta_done_data_params(ta_interpreter *it, ta_span32 params, ta_value *out) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_named_value *values = ta_arena_alloc(&it->system->scratch, params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t count = 0;
    ta_result result = TA_OK;
    for (uint32_t index = 0; index < params.length && result == TA_OK; index += 1) {
        const ta_param_def *param = &definition->params[params.offset + index];
        ta_fault fault = {0};
        values[count].name = ta_atom_text(&it->system->atoms, param->name);
        if (ta_eval_source(it, &(ta_value_params){ .source = param->value, .out = &values[count].value,
                                                   .fault = &fault }) == TA_OP_OK) {
            count += 1;
        } else {
            result = ta_execution_error(it, &fault);
            result = result == TA_EXEC_FAILED ? TA_OK : result;
        }
    }
    ta_fault fault = {0};
    if (result == TA_OK && count != 0 &&
        (v->make_params == NULL ||
         v->make_params(definition->datamodel.self,
                        &(ta_datamodel_make_params_params){
                            .shared = definition->datamodel_shared,
                            .instance = it->actor->datamodel_instance,
                            .values = values,
                            .count = count,
                            .host = &it->actor->host,
                            .out = out,
                            .fault = &fault,
                        }) != TA_OP_OK)) {
        *out = (ta_value){0};
        result = ta_execution_error(it, &fault);
        result = result == TA_EXEC_FAILED ? TA_OK : result;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        ta_value_release(&values[index].value);
    }
    return result;
}

/*
 * Evaluates a final state's <donedata> into `out`. A failure queues
 * error.execution and leaves the data empty; the done event still follows.
 */
static ta_result ta_done_data_value(ta_interpreter *it, ta_done_data_id id, ta_value *out) {
    *out = (ta_value){0};
    if (id == TA_DONE_DATA_NONE) {
        return TA_OK;
    }
    const ta_definition *definition = it->definition;
    const ta_done_data_def *done = &definition->done_data[id];
    if (done->params.length != 0) {
        return ta_done_data_params(it, done->params, out);
    }
    ta_fault fault = {0};
    ta_op_status status = TA_OP_OK;
    if (done->content.kind == TA_VALUE_SOURCE_CONTENT) {
        const ta_datamodel_vtable *v = definition->datamodel.v;
        status = v->import_content == NULL
                     ? TA_OP_ERROR
                     : v->import_content(definition->datamodel.self,
                                         &(ta_datamodel_import_content_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .content = definition->contents[done->content.as.content].bytes,
                                             .host = &it->actor->host,
                                             .out = out,
                                             .fault = &fault,
                                         });
    } else {
        status = ta_eval_source(it, &(ta_value_params){ .source = done->content, .out = out, .fault = &fault });
    }
    if (status == TA_OP_OK) {
        return TA_OK;
    }
    *out = (ta_value){0};
    ta_result result = ta_execution_error(it, &fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/* Keeps a top-level final state's done data as the actor's completion. */
static ta_result ta_store_completion(ta_interpreter *it, ta_value *data) {
    ta_actor_sparse_state *sparse = ta_sparse(it->system, it->actor);
    if (sparse == NULL) {
        ta_value_release(data);
        return TA_ERR_NOMEM;
    }
    ta_value_release(&sparse->completion.data);
    sparse->completion = (ta_actor_completion){ .has_data = data->v != NULL, .data = *data };
    *data = (ta_value){0};
    return TA_OK;
}

/* Computes the exit set and records history, once, as a microstep begins. */
static void ta_exit_begin(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->to_exit);
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_exit_set(it, it->enabled[index], it->to_exit);
    }
    /* Record history before any exit changes the configuration. */
    for (ta_state_id history = 0; history < definition->state_count; history += 1) {
        const ta_state_def *def = ta_state(it, history);
        if (!ta_is_history(it, history) || !ta_bits_get(it->to_exit, def->parent)) {
            continue;
        }
        uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        ta_bits_clear(it, recorded);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (!ta_bits_get(it->active, state)) {
                continue;
            }
            bool keep = def->kind == TA_STATE_HISTORY_DEEP
                            ? ta_definition_is_atomic(definition, state) && ta_descends(it, state, def->parent)
                            : ta_state(it, state)->parent == def->parent;
            if (keep) {
                ta_bits_put(recorded, state, true);
            }
        }
    }
    it->at.state = definition->state_count;
    it->at.stage = TA_EXIT_HANDLERS;
}

/* Exits the exit set, highest first, from where `at` stands. */
static ta_result ta_exit_states(ta_interpreter *it) {
    while (it->at.state > 0) {
        ta_state_id state = it->at.state - 1u;
        if (!ta_bits_get(it->to_exit, state)) {
            it->at.state -= 1u;
            continue;
        }
        ta_result result = TA_OK;
        if (it->at.stage == TA_EXIT_HANDLERS) {
            result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_EXIT_HANDLERS_DONE;
        }
        result = ta_invoke_state_exited(it, state); /* Invoke: cancel this state's invocations. */
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
        it->at.state -= 1u;
        it->at.stage = TA_EXIT_HANDLERS;
    }
    return TA_OK;
}

/* Computes the entry set, once, after the transitions' content has run. */
static void ta_enter_begin(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->to_enter);
    ta_bits_clear(it, it->default_entry);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        it->default_history[state] = TA_STATE_NONE;
    }
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_transition_id transition = it->enabled[index];
        ta_transition_view view = ta_view(it, transition);
        ta_state_id ancestor = TA_STATE_NONE;
        if (!ta_transition_domain(it, transition, &ancestor)) {
            continue;
        }
        for (uint32_t target = 0; target < view.targets.length; target += 1) {
            ta_add_descendants(it, ta_target_at(it, view.targets, target));
        }
        ta_bits_clear(it, it->entry_targets);
        ta_effective_targets(it, view.targets, it->entry_targets);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (ta_bits_get(it->entry_targets, state)) {
                ta_add_ancestors(it, state, ancestor);
            }
        }
    }
    it->at.state = 0;
    it->at.stage = TA_ENTER_ACTIVATE;
}

/* Done data and done.state events for a final state just entered. */
static ta_result ta_enter_final(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    ta_value data = {0};
    ta_result result = ta_done_data_value(it, def->done_data, &data);
    if (result != TA_OK) {
        return result;
    }
    if (def->parent == TA_STATE_NONE) {
        it->actor->finished = true;
        result = ta_store_completion(it, &data);
        if (result == TA_OK) {
            it->actor->sparse->final_state = state; /* ADR 0056 */
        }
        return result;
    }
    result = ta_raise_done(it, def->parent, &data);
    ta_state_id grandparent = ta_state(it, def->parent)->parent;
    if (result == TA_OK && grandparent != TA_STATE_NONE &&
        ta_state(it, grandparent)->kind == TA_STATE_PARALLEL && ta_in_final_state(it, grandparent)) {
        ta_value none = {0};
        result = ta_raise_done(it, grandparent, &none);
    }
    return result;
}

/*
 * Enters the entry set, lowest first, from where `at` stands. Each stage
 * of a state happens once; `at.stage` records the ones done, so a paused
 * microstep never activates a state or starts its data twice (ADR 0061).
 */
static ta_result ta_enter_states(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    while (it->at.state < definition->state_count) {
        ta_state_id state = it->at.state;
        if (!ta_bits_get(it->to_enter, state)) {
            it->at.state += 1u;
            continue;
        }
        const ta_state_def *def = ta_state(it, state);
        ta_result result = TA_OK;
        if (it->at.stage == TA_ENTER_ACTIVATE) {
            ta_bits_put(it->active, state, true);
            result = ta_invoke_state_entered(it, state); /* Invoke: start after the macrostep. */
            if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
                ta_bits_put(it->actor->late_initialized, state, true);
                result = ta_initialize_data(it, state);
            }
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_ENTER_HANDLERS;
        }
        if (it->at.stage == TA_ENTER_HANDLERS) {
            result = ta_run_handlers(it, def->onentry_handlers);
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_ENTER_INITIAL;
        }
        if (it->at.stage == TA_ENTER_INITIAL) {
            if (ta_bits_get(it->default_entry, state)) {
                result = ta_run_block_isolated(it, definition->initial_transitions[def->initial].actions);
                if (result != TA_OK) {
                    return result;
                }
            }
            it->at.stage = TA_ENTER_HISTORY;
        }
        if (it->at.stage == TA_ENTER_HISTORY) {
            if (it->default_history[state] != TA_STATE_NONE) {
                const ta_state_def *history = ta_state(it, it->default_history[state]);
                result = ta_run_block_isolated(it, definition->transitions[history->transitions.offset].actions);
                if (result != TA_OK) {
                    return result;
                }
            }
            it->at.stage = TA_ENTER_DONE;
        }
        if (def->kind == TA_STATE_FINAL) {
            result = ta_enter_final(it, state);
            if (result != TA_OK) {
                return result;
            }
        }
        it->at.state += 1u;
        it->at.stage = TA_ENTER_ACTIVATE;
    }
    return TA_OK;
}

static void ta_load_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->active);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (!ta_configuration_contains_atomic(&it->actor->configuration, state)) {
            continue;
        }
        for (ta_state_id at = state; at != TA_STATE_NONE; at = ta_state(it, at)->parent) {
            ta_bits_put(it->active, at, true);
        }
    }
}

static void ta_store_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_configuration_clear(&it->actor->configuration);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_definition_is_atomic(definition, state)) {
            ta_configuration_set_atomic(&it->actor->configuration, state, true);
        }
    }
}

/*
 * Before exit, actions and entry make the microstep visible, make sure the
 * actor holds an internal queue with room for one event (lent, ADR 0064) and,
 * when the microstep may need it, its sparse state. So a full allocator
 * pauses at the boundary, and a retry starts the same microstep again
 * (ADR 0047).
 */
static ta_result ta_prepare_microstep_queue(ta_interpreter *it) {
    ta_result result = ta_queue_room(it->system, it->actor);
    /* A microstep that may keep a send, a completion, an invocation or a
     * held effect gets its sparse state now, while nothing is visible. */
    if (result == TA_OK && (it->definition->microstep_needs_sparse || !it->actor->published) &&
        ta_sparse(it->system, it->actor) == NULL) {
        result = TA_ERR_NOMEM;
    }
    return result;
}

static void ta_compact_sparse(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL || sparse->paused != NULL || sparse->invoke_count != 0 ||
        sparse->pending_invokes.count != 0 || sparse->activation_effects != NULL || sparse->has_parent ||
        sparse->completion_pending || sparse->completion.has_data) {
        return;
    }
    ta_invoke_release(it->system, it->actor);
    if (sparse->timers.count != 0) {
        /* Pending delayed sends keep the sparse state (ADR 0054). */
        return;
    }
    ta_timer_list_destroy(&it->system->timers, &sparse->timers);
    ta_mem_free(it->system->allocator, sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    it->actor->sparse = NULL;
}

/*
 * Runs a microstep, or continues the one `at` describes. TA_EXEC_PAUSED
 * leaves it open: the position, the sets and the frames stay in the
 * interpreter for ta_pause_save (ADR 0061), and the actor's configuration
 * is still the one before the microstep.
 */
static ta_result ta_microstep(ta_interpreter *it) {
    ta_result result = TA_OK;
    if (it->at.phase == TA_MICRO_NONE) {
        /* A microstep changes the configuration, history, or the queue: an
         * image taken before it no longer describes this actor. */
        if (it->actor->bootstrapped) {
            result = ta_prepare_microstep_queue(it);
        }
        if (result != TA_OK) {
            return result;
        }
        ta_actor_touch(it->actor);
        ta_exit_begin(it);
        it->at.phase = TA_MICRO_EXIT;
    }
    it->in_microstep = true;
    if (it->at.phase == TA_MICRO_EXIT) {
        result = ta_exit_states(it);
        if (result == TA_OK) {
            it->at.phase = TA_MICRO_TRANSITIONS;
            it->at.state = 0;
        }
    }
    if (result == TA_OK && it->at.phase == TA_MICRO_TRANSITIONS) {
        for (; result == TA_OK && it->at.state < it->enabled_count; it->at.state += 1u) {
            result = ta_run_block_isolated(it, ta_view(it, it->enabled[it->at.state]).actions);
            if (result != TA_OK) {
                break;
            }
        }
        if (result == TA_OK) {
            ta_enter_begin(it);
            it->at.phase = TA_MICRO_ENTER;
        }
    }
    if (result == TA_OK && it->at.phase == TA_MICRO_ENTER) {
        result = ta_enter_states(it);
    }
    it->in_microstep = false;
    if (result == TA_EXEC_PAUSED) {
        return result;
    }
    it->at = (ta_micro_position){0};
    ta_store_active(it);
    it->steps += 1u;
    return result;
}

/*
 * Moves a paused microstep out of scratch into the actor (ADR 0061
 * section 3). The frames' snapshots move with them. Fails without
 * changing anything when there is no memory.
 */
static ta_result ta_pause_save(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = ta_sparse(it->system, it->actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_continuation shape = { .at = it->at, .words = it->words, .state_count = it->definition->state_count,
                              .enabled_count = it->enabled_count, .frame_count = it->exec_count };
    shape.bytes = ta_continuation_bytes(&shape);
    ta_continuation *saved = ta_mem_alloc(it->system->allocator, shape.bytes, TA_CONTINUATION_ALIGN);
    if (saved == NULL) {
        return TA_ERR_NOMEM;
    }
    *saved = shape;
    uint64_t *sets = ta_continuation_sets(saved);
    const uint64_t *from[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(from); index += 1u) {
        ta_mem_copy(sets + (size_t)index * it->words, from[index], (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(ta_continuation_history(saved), it->default_history,
                (size_t)shape.state_count * sizeof(ta_state_id));
    ta_mem_copy(ta_continuation_enabled(saved), it->enabled, (size_t)shape.enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(ta_continuation_frames(saved), it->exec_frames, (size_t)shape.frame_count * sizeof(ta_exec_frame));
    sparse->paused = saved;
    it->exec_count = 0;
    it->at = (ta_micro_position){0};
    return TA_OK;
}

/* Moves a paused microstep back into scratch; the actor no longer holds it. */
static void ta_pause_restore(ta_interpreter *it) {
    ta_continuation *saved = it->actor->sparse->paused;
    const uint64_t *sets = ta_continuation_sets(saved);
    uint64_t *to[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(to); index += 1u) {
        ta_mem_copy(to[index], sets + (size_t)index * it->words, (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(it->default_history, ta_continuation_history(saved), (size_t)saved->state_count * sizeof(ta_state_id));
    ta_mem_copy(it->enabled, ta_continuation_enabled(saved), (size_t)saved->enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(it->exec_frames, ta_continuation_frames(saved), (size_t)saved->frame_count * sizeof(ta_exec_frame));
    it->enabled_count = saved->enabled_count;
    it->exec_count = saved->frame_count;
    it->at = saved->at;
    it->actor->sparse->paused = NULL;
    ta_mem_free(it->system->allocator, saved,
                &(ta_allocator_free_params){ .size = saved->bytes, .alignment = TA_CONTINUATION_ALIGN });
}

/* Drops a restored pause without finishing it: its snapshots are released. */
static void ta_pause_drop(ta_interpreter *it) {
    while (it->exec_count > 0) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    it->at = (ta_micro_position){0};
}

/*
 * Runs or continues a microstep. A pause that cannot be saved does not
 * pause: the microstep runs one more unit and tries again (ADR 0061
 * section 3), so the budget never turns into an error.
 */
static ta_result ta_run_microstep(ta_interpreter *it) {
    for (;;) {
        ta_result result = ta_microstep(it);
        if (result != TA_EXEC_PAUSED) {
            return result;
        }
        if (ta_pause_save(it) == TA_OK) {
            return TA_EXEC_PAUSED;
        }
        it->action_budget = it->actions + 1u;
    }
}

/* Exits every active state in exit order when a top-level final state is reached. */
static ta_result ta_finish(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    for (ta_state_id index = definition->state_count; index > 0; index -= 1u) {
        ta_state_id state = index - 1u;
        if (!ta_bits_get(it->active, state)) {
            continue;
        }
        ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
    }
    ta_store_active(it);
    /* Sends the router already refused once stay queued: they were sent before termination. */
    ta_timer_cancel_unsubmitted(&it->system->timers, ta_actor_timers(it->actor));
    ta_event event;
    while (ta_dequeue_internal(it->actor, &event)) {
        ta_event_release(&event);
    }
    if (it->actor->has_macrostep_event) {
        ta_event_release(&it->actor->macrostep_event);
        it->actor->has_macrostep_event = false;
    }
    it->actor->macrostep = false;
    it->actor->record.residency = TA_ACTOR_DONE;
    /* Invoke: cancel this session's invocations and report done.invoke to its parent. */
    return ta_invoke_finished(it);
}

static ta_result ta_bootstrap(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    /* Top-level data is initialized at bootstrap under either binding (it belongs to <scxml>). */
    it->bound_decls = ta_arena_alloc(&it->system->scratch,
                                     (ta_bits_words(definition->data_decl_count) + 1u) * sizeof(uint64_t),
                                     _Alignof(uint64_t));
    if (it->bound_decls == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_zero(it->bound_decls, (ta_bits_words(definition->data_decl_count) + 1u) * sizeof(uint64_t));
    ta_result result = ta_declare_data(it);
    if (result == TA_OK) {
        result = ta_initialize_data(it, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < definition->state_count &&
                                definition->binding_mode == TA_BINDING_EARLY;
         state += 1) {
        result = ta_initialize_data(it, state);
    }
    if (result == TA_OK) {
        result = ta_run_block_isolated(it, definition->global_script_actions);
    }
    if (result == TA_OK) {
        it->enabled[0] = TA_TRANSITION_BOOTSTRAP;
        it->enabled_count = 1;
        result = ta_microstep(it);
    }
    it->bound_decls = NULL;
    it->actor->bootstrapped = true;
    return result;
}

static ta_result ta_interpreter_init(ta_interpreter *it, ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    uint32_t words = ta_bits_words(definition->state_count);
    *it = (ta_interpreter){ .system = system, .actor = actor, .definition = definition, .words = words };
    ta_arena_reset(&system->scratch);
    uint64_t **sets[] = {
        &it->active, &it->to_exit, &it->to_enter, &it->default_entry,
        &it->domain_targets, &it->entry_targets, &it->exit_left, &it->exit_right,
        &it->history_seen, &it->history_busy, &it->final_states,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(sets); index += 1) {
        *sets[index] = ta_arena_alloc(&system->scratch, words * sizeof(uint64_t), _Alignof(uint64_t));
        if (*sets[index] == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    /* At most one transition per atomic state, plus the bootstrap transition. */
    uint32_t capacity = definition->state_count + 1u;
    it->default_history = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_state_id), _Alignof(ta_state_id));
    it->enabled = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    it->filtered = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    it->remove = ta_arena_alloc(&system->scratch, capacity * sizeof(bool), _Alignof(bool));
    if (it->default_history == NULL || it->enabled == NULL || it->filtered == NULL || it->remove == NULL) {
        return TA_ERR_NOMEM;
    }
    it->entry_capacity = ta_entry_capacity(definition);
    it->entry_frames = ta_arena_alloc(&system->scratch, it->entry_capacity * sizeof(struct ta_entry_frame),
                                      _Alignof(struct ta_entry_frame));
    /* Each history state is expanded at most once. */
    it->history_stack = ta_arena_alloc(&system->scratch, (definition->history.slot_count + 1u) * sizeof(ta_state_id),
                                       _Alignof(ta_state_id));
    /* An open block per level, and a <foreach> frame under each body. */
    it->exec_capacity = 2u * definition->max_action_depth + 1u;
    it->exec_frames = ta_arena_alloc(&system->scratch, it->exec_capacity * sizeof(ta_exec_frame), _Alignof(ta_exec_frame));
    if (it->entry_frames == NULL || it->history_stack == NULL || it->exec_frames == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_bits_clear(it, it->history_busy);
    ta_load_active(it);
    if (actor->sparse != NULL && actor->sparse->paused != NULL) {
        ta_pause_restore(it); /* the live set, too, is the paused microstep's */
    }
    actor->live_active = it->active;
    return TA_OK;
}

static ta_result ta_interpreter_run(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params) {
    ta_run_status *status = params->status;
    if (actor->finished) {
        *status = TA_RUN_DONE;
        return TA_OK;
    }
    ta_interpreter interpreter;
    ta_interpreter *it = &interpreter;
    ta_result result = ta_interpreter_init(it, system, actor);
    if (result == TA_OK && !actor->bootstrapped) {
        result = ta_bootstrap(it);
    }
    it->pausable = true;
    it->action_budget = params->action_budget;
    if (result == TA_OK && it->at.phase != TA_MICRO_NONE) {
        /* Continue the microstep a pause inside executable content left. The
         * actor changes again, so an image taken while paused is stale. */
        ta_actor_touch(actor);
        bool restore_internal = it->at.restore_internal;
        bool restore_external = it->at.restore_external;
        result = ta_run_microstep(it);
        if (result == TA_EXEC_PAUSED) {
            *status = TA_RUN_PAUSED;
            return TA_OK;
        }
        if (result == TA_ERR_NOMEM && (restore_internal || restore_external)) {
            ta_restore_current_event(actor, restore_external);
        }
    }
    while (result == TA_OK && !actor->finished) {
        if (it->steps >= params->budget) {
            *status = TA_RUN_PAUSED;
            return TA_OK;
        }
        result = ta_select_transitions(it, NULL);
        if (result != TA_OK) {
            break;
        }
        if (it->enabled_count != 0) {
            result = ta_run_microstep(it);
            if (result == TA_EXEC_PAUSED) {
                *status = TA_RUN_PAUSED;
                return TA_OK;
            }
            continue;
        }
        ta_event event;
        bool restore_internal = false;
        bool restore_external = false;
        if (ta_dequeue_internal(actor, &event)) {
            ta_set_current_event(actor, &event);
            restore_internal = true;
        } else if ((result = ta_invoke_start_pending(it)) != TA_OK) {
            break; /* Invoke: the macrostep ended, so start its states' invocations. */
        } else if (ta_interpreter_internal_pending(actor)) {
            continue;
        } else if (actor->has_external_event) {
            actor->has_external_event = false;
            ta_set_current_event(actor, &actor->external_event);
            restore_external = true;
            /* Invoke: <finalize> and autoforwarding. */
            result = ta_invoke_before_external(it);
            if (result != TA_OK) {
                break;
            }
        } else {
            ta_compact_sparse(it);
            if (actor->has_macrostep_event) {
                ta_event_release(&actor->macrostep_event);
                actor->has_macrostep_event = false;
            }
            actor->macrostep = false;
            actor->record.residency = TA_ACTOR_IDLE;
            *status = TA_RUN_STABLE;
            return TA_OK;
        }
        result = ta_select_transitions(it, &actor->current_event);
        if (result == TA_OK && it->enabled_count != 0) {
            it->at.restore_internal = restore_internal;
            it->at.restore_external = restore_external;
            result = ta_run_microstep(it);
            if (result == TA_EXEC_PAUSED) {
                *status = TA_RUN_PAUSED;
                return TA_OK;
            }
            if (result == TA_ERR_NOMEM && (restore_internal || restore_external)) {
                ta_restore_current_event(actor, restore_external);
            }
        } else {
            it->steps += 1u;
        }
    }
    if (result != TA_OK) {
        *status = TA_RUN_ERROR;
        return result;
    }
    result = ta_finish(it);
    *status = result == TA_OK ? TA_RUN_DONE : TA_RUN_ERROR;
    return result;
}

ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params) {
    ta_result result = ta_interpreter_run(system, actor, params);
    /* The live set lives in scratch memory; between calls In() derives from the configuration. */
    actor->live_active = NULL;
    /* An empty queue goes back; queued events or a paused microstep keep it. */
    ta_queue_settle(system, actor);
    return result;
}

/*
 * One session being exited by ta_interpreter_exit_invoked: its interpreter,
 * the scratch arena that interpreter lives in, the states it still has to
 * exit, and, while a child it invoked is being exited above it, the
 * invocation to cancel once that child is done.
 */
typedef struct {
    ta_actor *actor;
    ta_interpreter it;
    ta_arena arena;
    ta_state_id next;     /* states left to exit: those below `next`, highest first */
    ta_state_id exiting;  /* the state whose invocations are being cancelled */
    uint32_t cursor;      /* its invocations left: those below `cursor` */
    bool waiting;         /* `taken`'s child is being exited above this frame */
    ta_invoke_taken taken;
} ta_exit_frame;

/*
 * Starts exiting a session: a scratch arena of its own, so the interpreter
 * that cancelled it keeps its scratch, and an interpreter over it. Nothing
 * happens when the session is gone or finished, or when there is no memory
 * to exit it with.
 */
static void ta_exit_open(ta_system *system, ta_vec *frames, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL || actor->finished) {
        return;
    }
    ta_exit_frame frame = { .actor = actor, .exiting = TA_STATE_NONE };
    if (ta_arena_init(&frame.arena, system->allocator, system->scratch.block_size) != TA_OK) {
        return;
    }
    ta_arena previous = system->scratch;
    system->scratch = frame.arena;
    ta_result result = ta_interpreter_init(&frame.it, system, actor);
    /* A session paused inside a microstep exits its live set; the rest of
     * the pause is dropped (ADR 0061 section 6). */
    ta_pause_drop(&frame.it);
    frame.arena = system->scratch;
    frame.next = actor->definition->state_count;
    if (result == TA_OK) {
        result = ta_vec_push(frames, &frame);
    }
    if (result != TA_OK) {
        actor->live_active = NULL;
        ta_arena_destroy(&frame.arena);
        system->scratch = previous;
    }
}

/*
 * One step of the session on top: exit its next state, cancel that state's
 * next invocation, or finish. `system->scratch` is the session's arena while
 * it runs; a step that keeps the session saves the arena back to its frame.
 */
static void ta_exit_step(ta_system *system, ta_vec *frames) {
    ta_exit_frame *frame = ta_vec_at(frames, frames->count - 1u);
    ta_interpreter *it = &frame->it;
    if (frame->waiting) {
        frame->waiting = false;
        ta_invoke_cancel_service(&frame->taken);
        frame->arena = system->scratch;
        return;
    }
    if (frame->exiting != TA_STATE_NONE) {
        if (!ta_invoke_take(frame->actor, frame->exiting, &frame->cursor, &frame->taken)) {
            ta_bits_put(it->active, frame->exiting, false);
            frame->exiting = TA_STATE_NONE;
        } else if (frame->taken.meta.has_child) {
            frame->waiting = true;
            frame->arena = system->scratch;
            ta_exit_open(system, frames, frame->taken.meta.child); /* may move `frame` */
            return;
        } else {
            ta_invoke_cancel_service(&frame->taken);
        }
        frame->arena = system->scratch;
        return;
    }
    while (frame->next > 0 && !ta_bits_get(it->active, frame->next - 1u)) {
        frame->next -= 1u;
    }
    if (frame->next == 0) {
        ta_store_active(it);
        frame->actor->live_active = NULL;
        ta_arena_destroy(&system->scratch);
        frames->count -= 1u;
        return;
    }
    ta_state_id state = --frame->next;
    ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
    frame->arena = system->scratch;
    if (result != TA_OK) {
        ta_bits_put(it->active, state, false);
        frame->next = 0; /* exitInterpreter stops at a failure, as it did */
        return;
    }
    ta_invoke_forget_pending(frame->actor, state);
    frame->exiting = state;
    frame->cursor = UINT32_MAX;
}

void ta_interpreter_exit_invoked(ta_system *system, ta_actor_id id) {
    /* Cancelling an invocation happens inside the parent's interpreter:
     * every exited session gets its own scratch, and the parent's comes back. */
    ta_arena outer = system->scratch;
    ta_vec frames;
    ta_vec_init(&frames, system->allocator, TA_VEC_LAYOUT(ta_exit_frame));
    ta_exit_open(system, &frames, id);
    while (frames.count != 0) {
        ta_exit_frame *frame = ta_vec_at(&frames, frames.count - 1u);
        system->scratch = frame->arena;
        ta_exit_step(system, &frames);
    }
    ta_vec_destroy(&frames);
    system->scratch = outer;
}

/* ------------------------------------------------------------------------
 * Services for invoke.c
 */

ta_actor_sparse_state *ta_interpreter_sparse(ta_system *system, ta_actor *actor) {
    return ta_sparse(system, actor);
}

ta_op_status ta_interpreter_eval(ta_interpreter *it, const ta_interpreter_value_params *params) {
    return ta_eval_source(it, &(ta_value_params){ .source = params->source, .out = params->out,
                                                  .fault = params->fault });
}

ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_execution_error(it, fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

ta_result ta_interpreter_run_actions(ta_interpreter *it, ta_span32 block) {
    return ta_run_block_isolated(it, block);
}

bool ta_interpreter_internal_pending(const ta_actor *actor) {
    return actor->internal_queue != NULL && actor->internal_queue->count != 0;
}

void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor) {
    if (actor->has_external_event) {
        ta_event_release(&actor->external_event);
        actor->has_external_event = false;
    }
    if (actor->has_macrostep_event) {
        ta_event_release(&actor->macrostep_event);
        actor->has_macrostep_event = false;
    }
    if (actor->has_current_event) {
        ta_event_release(&actor->current_event);
        actor->has_current_event = false;
    }
    ta_event event;
    while (ta_dequeue_internal(actor, &event)) {
        ta_event_release(&event);
    }
    if (actor->sparse == NULL) {
        ta_queue_settle(system, actor);
        return;
    }
    ta_invoke_release(system, actor); /* Invoke: cancel services, free bookkeeping. */
    ta_continuation_free(system->allocator, &actor->sparse->paused);
    ta_queue_settle(system, actor); /* empty, and no longer paused */
    /* The owner heap must not keep pointing into the freed sparse state. */
    ta_timer_list_destroy(&system->timers, &actor->sparse->timers);
    ta_value_release(&actor->sparse->completion.data);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}
