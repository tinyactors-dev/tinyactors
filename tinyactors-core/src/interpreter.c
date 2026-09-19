#include "interpreter.h"

/*
 * SCXML execution following the W3C algorithm (Appendix D). The actor's
 * configuration stores active atomic states; a microstep expands it into the
 * full active set, applies exits and entries to that set, and writes the
 * atomic states back.
 */

/* Executable content failed; error.execution is queued and the block stops (SCXML 4.9). */
#define TA_EXEC_FAILED 1

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
        *actor->sparse = (ta_actor_sparse_state){0};
        ta_vec_init(&actor->sparse->internal_events, system->allocator, TA_VEC_LAYOUT(ta_queued_event));
    }
    return actor->sparse;
}

ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event) {
    ta_actor_sparse_state *sparse = ta_sparse(system, actor);
    ta_queued_event queued = {
        .event = *event,
        .flags = TA_EVENT_FLAG_OWNS_DATA | (event->type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
    };
    ta_result result = sparse == NULL ? TA_ERR_NOMEM : ta_vec_push(&sparse->internal_events, &queued);
    if (result != TA_OK) {
        ta_event_release(event);
    }
    return result;
}

static bool ta_dequeue_internal(ta_actor *actor, ta_event *out) {
    if (actor->sparse == NULL || actor->sparse->internal_events.count == 0) {
        return false;
    }
    ta_vec *queue = &actor->sparse->internal_events;
    *out = ((ta_queued_event *)queue->items)[0].event;
    for (uint32_t index = 1; index < queue->count; index += 1) {
        ((ta_queued_event *)queue->items)[index - 1u] = ((ta_queued_event *)queue->items)[index];
    }
    queue->count -= 1u;
    return true;
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

/* Parses the standard lexical SCXML Event I/O Processor targets. */
static bool ta_parse_scxml_target(ta_system *system, ta_bytes text, ta_scxml_target *out) {
    ta_str target = { .ptr = (const char *)text.ptr, .len = text.len };
    static const struct { const char *text; ta_scxml_target_kind kind; } fixed[] = {
        { "#_internal", TA_SCXML_TARGET_INTERNAL },
        { "#_parent", TA_SCXML_TARGET_PARENT },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(fixed); index += 1) {
        ta_str candidate = { .ptr = fixed[index].text, .len = 0 };
        while (candidate.ptr[candidate.len] != '\0') {
            candidate.len += 1;
        }
        if (ta_str_equal(target, candidate)) {
            *out = (ta_scxml_target){ .kind = fixed[index].kind };
            return true;
        }
    }
    uint32_t prefix = 0;
    ta_scxml_target_kind kind = TA_SCXML_TARGET_INVOKE;
    if (target.len > 8 && ta_str_equal((ta_str){ .ptr = target.ptr, .len = 8 }, (ta_str){ .ptr = "#_scxml_", .len = 8 })) {
        prefix = 8;
        kind = TA_SCXML_TARGET_ACTOR;
    } else if (target.len > 2 && target.ptr[0] == '#' && target.ptr[1] == '_') {
        /* Invoke: `#_<invokeid>` names an invocation by its SCXML ID text. */
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE };
        return ta_invoke_id_from_text(system, (ta_str){ .ptr = target.ptr + 2, .len = target.len - 2 },
                                      &out->as.invoke) == TA_OK;
    } else {
        return false;
    }
    uint64_t number = 0;
    for (uint32_t index = prefix; index < target.len; index += 1) {
        char digit = target.ptr[index];
        if (digit < '0' || digit > '9' || number > (UINT64_MAX - 9u) / 10u) {
            if (kind == TA_SCXML_TARGET_ACTOR) {
                /*
                 * A well-formed session URI naming no session is inaccessible,
                 * not invalid (SCXML C.1): runtime actor ID 0 never exists, so
                 * delivery fails with error.communication.
                 */
                *out = (ta_scxml_target){ .kind = kind,
                                          .as.actor = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = 0 } };
                return true;
            }
            return false;
        }
        number = number * 10u + (uint64_t)(digit - '0');
    }
    *out = (ta_scxml_target){ .kind = kind };
    if (kind == TA_SCXML_TARGET_ACTOR) {
        out->as.actor = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = number };
    } else {
        out->as.invoke = number;
    }
    return true;
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
    ta_external_delivery delivery = {
        .destination = params->destination,
        .event = {
            .name = params->event,
            .type = TA_EVENT_EXTERNAL,
            .send_id = params->send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = params->origin },
            .invoke_id = params->invoke_id,
            .data = *params->data,
        },
    };
    return system->external_events.v->submit(system->external_events.self,
                                             &(ta_external_event_submit_params){
                                                 .delivery = &delivery,
                                                 .fault = params->fault,
                                             });
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
        if (io != NULL && io->v->send(io->self, &request, &fault) == TA_OP_OK) {
            status = TA_EXTERNAL_EVENT_ACCEPTED;
        }
    }
    if (status == TA_EXTERNAL_EVENT_RETRY) {
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
    return status;
}

typedef struct {
    ta_value data;
    ta_value target;
    ta_scxml_target scxml;
    ta_atom io_type;
    ta_send_id send_id;
    bool scxml_io;
} ta_send_state;

static ta_op_status ta_send_payload(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                    ta_fault *fault) {
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
        /* A custom processor receives the target as a value; literal text is imported. */
        return ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target, .fault = fault });
    }
    if (send->target.kind == TA_VALUE_SOURCE_NONE) {
        state->scxml = (ta_scxml_target){ .kind = TA_SCXML_TARGET_SELF };
        return TA_OP_OK;
    }
    if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
        return ta_parse_scxml_target(it->system, it->definition->contents[send->target.as.content].bytes,
                                     &state->scxml)
                   ? TA_OP_OK
                   : TA_OP_ERROR;
    }
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                                 .fault = fault });
    if (status == TA_OP_OK) {
        status = v->decode_scxml_target == NULL
                     ? TA_OP_ERROR
                     : v->decode_scxml_target(definition->datamodel.self,
                                              &(ta_datamodel_decode_scxml_target_params){
                                                  .shared = definition->datamodel_shared,
                                                  .instance = it->actor->datamodel_instance,
                                                  .value = &state->target,
                                                  .host = &it->actor->host,
                                                  .out = &state->scxml,
                                                  .fault = fault,
                                              });
    }
    if (status == TA_OP_OK && state->scxml.kind == TA_SCXML_TARGET_ACTOR &&
        state->scxml.as.actor.kind == TA_ACTOR_REF_NAMED) {
        /* Named-address text is borrowed from the target value; copying it is not implemented yet. */
        status = TA_OP_ERROR;
    }
    return status;
}

/* Resolves an SCXML target to an external destination; false means inaccessible. */
static bool ta_send_destination(ta_interpreter *it, const ta_scxml_target *target, ta_actor_ref *out) {
    switch (target->kind) {
        case TA_SCXML_TARGET_SELF:
            *out = it->actor->record.self;
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
    };
    if (state.scxml_io && !ta_send_destination(it, &state.scxml, &record.destination)) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        return ta_communication_error(system, actor, &(ta_error_params){ .send_id = state.send_id });
    }
    if (delay == 0) {
        ta_external_event_submit_status submitted = ta_interpreter_dispatch_delayed(system, &record);
        if (submitted != TA_EXTERNAL_EVENT_RETRY) {
            return TA_OK;
        }
        /*
         * The router accepted nothing, typically because the destination is
         * this busy actor. The evaluated send waits in the deadline index,
         * due now, and keeps its order among later sends.
         */
        record.submitted = true;
        record.origin = actor->record.self;
        record.invoke_id = ta_invoke_parent_id(actor, &record.destination);
    }
    ta_result result = ta_timer_schedule(&system->timers, &record);
    if (result != TA_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Executable content
 */

static ta_result ta_run_block(ta_interpreter *it, ta_span32 block);

/*
 * <foreach> iterates a shallow copy made by the data model. A failed
 * snapshot or binding, or an error in the body, stops the foreach and the
 * enclosing block (SCXML 4.6).
 */
static ta_result ta_exec_foreach(ta_interpreter *it, const ta_foreach_action *foreach) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->foreach_snapshot == NULL || v->foreach_bind == NULL) {
        return ta_execution_error(it, NULL);
    }
    ta_fault fault = {0};
    ta_value snapshot = {0};
    uint32_t count = 0;
    if (v->foreach_snapshot(definition->datamodel.self,
                            &(ta_datamodel_foreach_snapshot_params){
                                .shared = definition->datamodel_shared,
                                .instance = it->actor->datamodel_instance,
                                .array = foreach->array,
                                .host = &it->actor->host,
                                .out = &snapshot,
                                .count = &count,
                                .fault = &fault,
                            }) != TA_OP_OK) {
        return ta_execution_error(it, &fault);
    }
    ta_str item = ta_atom_text(&it->system->atoms, foreach->item);
    ta_str index = foreach->has_index ? ta_atom_text(&it->system->atoms, foreach->index) : (ta_str){0};
    ta_result result = TA_OK;
    for (uint32_t position = 0; position < count && result == TA_OK; position += 1) {
        ta_op_status bound = v->foreach_bind(definition->datamodel.self,
                                             &(ta_datamodel_foreach_bind_params){
                                                 .shared = definition->datamodel_shared,
                                                 .instance = it->actor->datamodel_instance,
                                                 .snapshot = &snapshot,
                                                 .index = position,
                                                 .item = item,
                                                 .index_name = index,
                                                 .host = &it->actor->host,
                                                 .fault = &fault,
                                             });
        result = bound == TA_OP_OK ? ta_run_block(it, foreach->body_actions) : ta_execution_error(it, &fault);
    }
    ta_value_release(&snapshot);
    return result;
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
                (void)ta_timer_cancel(&it->system->timers, it->actor->record.id, send_id);
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
        case TA_ACTION_LOG:
            /* There is no logging sink yet; the expression still runs for its errors. */
            if (action->as.log.has_expr) {
                ta_value value = {0};
                status = ta_eval_source(it, &(ta_value_params){
                                                .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = action->as.log.expr },
                                                .out = &value,
                                                .fault = &fault,
                                            });
                ta_value_release(&value);
            }
            break;
        case TA_ACTION_FOREACH:
            return ta_exec_foreach(it, &action->as.foreach);
        case TA_ACTION_IF: {
            bool condition = false;
            status = ta_eval_condition(it, action->as.conditional.condition, &condition, &fault);
            if (status == TA_OP_OK) {
                return ta_run_block(it, condition ? action->as.conditional.then_actions
                                                  : action->as.conditional.else_actions);
            }
            break;
        }
        default:
            status = TA_OP_ERROR;
            break;
    }
    return status == TA_OP_OK ? TA_OK : ta_execution_error(it, &fault);
}

static ta_result ta_run_block(ta_interpreter *it, ta_span32 block) {
    for (uint32_t index = 0; index < block.length; index += 1) {
        ta_result result = ta_exec_action(it, &it->definition->action_defs[block.offset + index]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
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
    for (uint32_t index = 0; index < handlers.length; index += 1) {
        ta_result result = ta_run_block_isolated(it, it->definition->handlers[handlers.offset + index]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Transition selection
 */

static void ta_effective_targets(ta_interpreter *it, ta_span32 targets, uint64_t *out) {
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_state_id target = ta_target_at(it, targets, index);
        if (!ta_is_history(it, target)) {
            ta_bits_put(out, target, true);
            continue;
        }
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, ta_state(it, target)->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (uint32_t word = 0; word < it->words; word += 1) {
                out[word] |= recorded[word];
            }
        } else {
            const ta_transition_def *fallback = &it->definition->transitions[ta_state(it, target)->transitions.offset];
            ta_effective_targets(it, fallback->targets, out);
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

static void ta_add_descendants(ta_interpreter *it, ta_state_id state);

static bool ta_entering_below(ta_interpreter *it, ta_state_id ancestor) {
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->to_enter, state) && ta_descends(it, state, ancestor)) {
            return true;
        }
    }
    return false;
}

static void ta_enter_parallel_regions(ta_interpreter *it, ta_state_id parallel) {
    for (uint32_t index = 0; index < ta_state(it, parallel)->children.length; index += 1) {
        ta_state_id child = ta_child_at(it, parallel, index);
        if (!ta_is_history(it, child) && !ta_entering_below(it, child)) {
            ta_add_descendants(it, child);
        }
    }
}

static void ta_add_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    for (ta_state_id at = ta_state(it, state)->parent; at != ancestor && at != TA_STATE_NONE;
         at = ta_state(it, at)->parent) {
        ta_bits_put(it->to_enter, at, true);
        if (ta_state(it, at)->kind == TA_STATE_PARALLEL) {
            ta_enter_parallel_regions(it, at);
        }
    }
}

static void ta_add_targets(ta_interpreter *it, ta_span32 targets, ta_state_id ancestor) {
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_add_descendants(it, ta_target_at(it, targets, index));
    }
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_add_ancestors(it, ta_target_at(it, targets, index), ancestor);
    }
}

static void ta_add_descendants(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    if (ta_is_history(it, state)) {
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (ta_state_id each = 0; each < it->definition->state_count; each += 1) {
                if (ta_bits_get(recorded, each)) {
                    ta_add_descendants(it, each);
                }
            }
            for (ta_state_id each = 0; each < it->definition->state_count; each += 1) {
                if (ta_bits_get(recorded, each)) {
                    ta_add_ancestors(it, each, def->parent);
                }
            }
        } else {
            it->default_history[def->parent] = state;
            ta_add_targets(it, it->definition->transitions[def->transitions.offset].targets, def->parent);
        }
        return;
    }
    ta_bits_put(it->to_enter, state, true);
    if (def->kind == TA_STATE_COMPOUND) {
        ta_bits_put(it->default_entry, state, true);
        ta_add_targets(it, it->definition->initial_transitions[def->initial].targets, state);
    } else if (def->kind == TA_STATE_PARALLEL) {
        ta_enter_parallel_regions(it, state);
    }
}

static bool ta_in_final_state(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    if (def->kind == TA_STATE_COMPOUND) {
        for (uint32_t index = 0; index < def->children.length; index += 1) {
            ta_state_id child = ta_child_at(it, state, index);
            if (ta_state(it, child)->kind == TA_STATE_FINAL && ta_bits_get(it->active, child)) {
                return true;
            }
        }
        return false;
    }
    if (def->kind == TA_STATE_PARALLEL) {
        for (uint32_t index = 0; index < def->children.length; index += 1) {
            ta_state_id child = ta_child_at(it, state, index);
            if (!ta_is_history(it, child) && !ta_in_final_state(it, child)) {
                return false;
            }
        }
        return true;
    }
    return false;
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

static ta_result ta_exit_states(ta_interpreter *it) {
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
    for (ta_state_id index = definition->state_count; index > 0; index -= 1u) {
        ta_state_id state = index - 1u;
        if (!ta_bits_get(it->to_exit, state)) {
            continue;
        }
        ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
        if (result == TA_OK) {
            result = ta_invoke_state_exited(it, state); /* Invoke: cancel this state's invocations. */
        }
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
    }
    return TA_OK;
}

static ta_result ta_enter_states(ta_interpreter *it) {
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
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (!ta_bits_get(it->to_enter, state)) {
            continue;
        }
        const ta_state_def *def = ta_state(it, state);
        ta_bits_put(it->active, state, true);
        ta_result result = ta_invoke_state_entered(it, state); /* Invoke: start after the macrostep. */
        if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
            ta_bits_put(it->actor->late_initialized, state, true);
            result = ta_initialize_data(it, state);
        }
        if (result == TA_OK) {
            result = ta_run_handlers(it, def->onentry_handlers);
        }
        if (result == TA_OK && ta_bits_get(it->default_entry, state)) {
            result = ta_run_block_isolated(it, definition->initial_transitions[def->initial].actions);
        }
        if (result == TA_OK && it->default_history[state] != TA_STATE_NONE) {
            const ta_state_def *history = ta_state(it, it->default_history[state]);
            result = ta_run_block_isolated(it, definition->transitions[history->transitions.offset].actions);
        }
        if (result != TA_OK) {
            return result;
        }
        if (def->kind != TA_STATE_FINAL) {
            continue;
        }
        ta_value data = {0};
        result = ta_done_data_value(it, def->done_data, &data);
        if (result != TA_OK) {
            return result;
        }
        if (def->parent == TA_STATE_NONE) {
            it->actor->finished = true;
            result = ta_store_completion(it, &data);
            if (result != TA_OK) {
                return result;
            }
            continue;
        }
        result = ta_raise_done(it, def->parent, &data);
        ta_state_id grandparent = ta_state(it, def->parent)->parent;
        if (result == TA_OK && grandparent != TA_STATE_NONE &&
            ta_state(it, grandparent)->kind == TA_STATE_PARALLEL && ta_in_final_state(it, grandparent)) {
            ta_value none = {0};
            result = ta_raise_done(it, grandparent, &none);
        }
        if (result != TA_OK) {
            return result;
        }
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

static ta_result ta_microstep(ta_interpreter *it) {
    ta_result result = ta_exit_states(it);
    for (uint32_t index = 0; result == TA_OK && index < it->enabled_count; index += 1) {
        result = ta_run_block_isolated(it, ta_view(it, it->enabled[index]).actions);
    }
    if (result == TA_OK) {
        result = ta_enter_states(it);
    }
    ta_store_active(it);
    it->steps += 1u;
    return result;
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
    ta_timer_cancel_unsubmitted(&it->system->timers, it->actor->record.id);
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_event event;
    while (sparse != NULL && ta_dequeue_internal(it->actor, &event)) {
        ta_event_release(&event);
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
    ta_load_active(it);
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
            result = ta_microstep(it);
            continue;
        }
        ta_event event;
        if (ta_dequeue_internal(actor, &event)) {
            ta_set_current_event(actor, &event);
        } else if ((result = ta_invoke_start_pending(it)) != TA_OK) {
            break; /* Invoke: the macrostep ended, so start its states' invocations. */
        } else if (ta_interpreter_internal_pending(actor)) {
            continue;
        } else if (actor->has_external_event) {
            actor->has_external_event = false;
            ta_set_current_event(actor, &actor->external_event);
            /* Invoke: <finalize> and autoforwarding. */
            result = ta_invoke_before_external(it);
            if (result != TA_OK) {
                break;
            }
        } else {
            actor->macrostep = false;
            actor->record.residency = TA_ACTOR_IDLE;
            *status = TA_RUN_STABLE;
            return TA_OK;
        }
        result = ta_select_transitions(it, &actor->current_event);
        if (result == TA_OK && it->enabled_count != 0) {
            result = ta_microstep(it);
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
    return result;
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
    return actor->sparse != NULL && actor->sparse->internal_events.count != 0;
}

void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor) {
    if (actor->has_external_event) {
        ta_event_release(&actor->external_event);
        actor->has_external_event = false;
    }
    if (actor->has_current_event) {
        ta_event_release(&actor->current_event);
        actor->has_current_event = false;
    }
    if (actor->sparse == NULL) {
        return;
    }
    ta_invoke_release(system, actor); /* Invoke: cancel services, free bookkeeping. */
    ta_event event;
    while (ta_dequeue_internal(actor, &event)) {
        ta_event_release(&event);
    }
    ta_vec_destroy(&actor->sparse->internal_events);
    ta_value_release(&actor->sparse->completion.data);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}
