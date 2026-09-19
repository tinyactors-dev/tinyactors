#include "invoke.h"
#include "interpreter.h"

/*
 * SCXML <invoke>: starting services at the end of a macrostep, cancelling
 * them when their state exits, <finalize>, autoforwarding, done.invoke, and
 * the host's asynchronous reports.
 *
 * Invoke IDs are SCXML text (the author's `id` or a generated
 * `<stateid>.<platformid>`). The runtime ta_invoke_id is the interned text's
 * atom + 1, so the text is always recoverable and `#_<invokeid>` targets
 * resolve by interning.
 */

/* ------------------------------------------------------------------------
 * IDs
 */

ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out) {
    ta_atom atom = 0;
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_atom_intern(&system->atoms, text, &atom);
    if (result == TA_OK) {
        *out = (ta_invoke_id)atom + 1u;
    }
    return result;
}

ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke) {
    if (invoke == 0 || invoke == TA_INVOKE_ID_NONE || invoke - 1u >= TA_ATOM_NONE) {
        return (ta_str){0};
    }
    return ta_atom_text(&system->atoms, (ta_atom)(invoke - 1u));
}

/* done.invoke.<id>, one event-name segment per dotted part of the ID text. */
static ta_result ta_invoke_done_name(ta_system *system, ta_invoke_id invoke, ta_event_name_id *out) {
    ta_str text = ta_invoke_id_text(system, invoke);
    ta_event_name_id name = TA_EVENT_NAME_DONE_INVOKE;
    uint32_t start = 0;
    for (uint32_t index = 0; text.ptr != NULL && index <= text.len; index += 1) {
        if (index < text.len && text.ptr[index] != '.') {
            continue;
        }
        if (index > start) {
            ta_atom atom = 0;
            ta_result result = ta_atom_intern(&system->atoms,
                                              (ta_str){ .ptr = text.ptr + start, .len = index - start }, &atom);
            if (result == TA_OK) {
                result = ta_event_name_child(&system->event_names, name, atom, &name);
            }
            if (result != TA_OK) {
                return result;
            }
        }
        start = index + 1u;
    }
    *out = name;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Records
 */

static int64_t ta_invoke_find(const ta_actor *actor, ta_invoke_id invoke) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1) {
        if (sparse->invokes[index].id == invoke) {
            return (int64_t)index;
        }
    }
    return -1;
}

static ta_result ta_invoke_grow(ta_system *system, ta_actor_sparse_state *sparse) {
    if (sparse->invoke_count < sparse->invoke_capacity) {
        return TA_OK;
    }
    uint32_t capacity = sparse->invoke_capacity == 0 ? 4u : sparse->invoke_capacity * 2u;
    ta_active_invoke *invokes = ta_mem_alloc(system->allocator, capacity * sizeof(ta_active_invoke),
                                             _Alignof(ta_active_invoke));
    ta_invoke_meta *meta = ta_mem_alloc(system->allocator, capacity * sizeof(ta_invoke_meta),
                                        _Alignof(ta_invoke_meta));
    if (invokes == NULL || meta == NULL) {
        ta_mem_free(system->allocator, invokes,
                    &(ta_allocator_free_params){ .size = capacity * sizeof(ta_active_invoke),
                                                 .alignment = _Alignof(ta_active_invoke) });
        ta_mem_free(system->allocator, meta,
                    &(ta_allocator_free_params){ .size = capacity * sizeof(ta_invoke_meta),
                                                 .alignment = _Alignof(ta_invoke_meta) });
        return TA_ERR_NOMEM;
    }
    if (sparse->invoke_count != 0) {
        ta_mem_copy(invokes, sparse->invokes, sparse->invoke_count * sizeof(ta_active_invoke));
        ta_mem_copy(meta, sparse->invoke_meta, sparse->invoke_count * sizeof(ta_invoke_meta));
    }
    ta_mem_free(system->allocator, sparse->invokes,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_active_invoke),
                                             .alignment = _Alignof(ta_active_invoke) });
    ta_mem_free(system->allocator, sparse->invoke_meta,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_invoke_meta),
                                             .alignment = _Alignof(ta_invoke_meta) });
    sparse->invokes = invokes;
    sparse->invoke_meta = meta;
    sparse->invoke_capacity = capacity;
    return TA_OK;
}

static void ta_invoke_remove_at(ta_actor_sparse_state *sparse, uint32_t index) {
    for (uint32_t at = index + 1u; at < sparse->invoke_count; at += 1) {
        sparse->invokes[at - 1u] = sparse->invokes[at];
        sparse->invoke_meta[at - 1u] = sparse->invoke_meta[at];
    }
    sparse->invoke_count -= 1u;
}

/* Cancels the service and forgets the invocation. */
static void ta_invoke_cancel_at(ta_actor_sparse_state *sparse, uint32_t index) {
    ta_active_invoke active = sparse->invokes[index];
    ta_invoke_meta meta = sparse->invoke_meta[index];
    /* Remove first: the service may destroy a child that reports back reentrantly. */
    ta_invoke_remove_at(sparse, index);
    if (meta.invoker.v != NULL && meta.invoker.v->cancel != NULL) {
        ta_fault fault = {0};
        (void)meta.invoker.v->cancel(meta.invoker.self, active.service_handle, &fault);
        ta_value_release(&fault.data);
    }
}

static ta_vec *ta_invoke_pending(ta_system *system, ta_actor_sparse_state *sparse) {
    if (sparse->pending_invokes.element_size == 0) {
        ta_vec_init(&sparse->pending_invokes, system->allocator, TA_VEC_LAYOUT(ta_state_id));
    }
    return &sparse->pending_invokes;
}

static const ta_invoker *ta_invoke_find_invoker(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            return &entry->invoker;
        }
    }
    return NULL;
}

static ta_atom ta_invoke_normalize_type(const ta_system *system, ta_atom type) {
    static const char *const aliases[] = { "scxml", "http://www.w3.org/TR/scxml", "http://www.w3.org/TR/scxml/" };
    ta_str text = ta_atom_text(&system->atoms, type);
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(aliases); index += 1) {
        ta_str alias = { .ptr = aliases[index], .len = 0 };
        while (alias.ptr[alias.len] != '\0') {
            alias.len += 1;
        }
        if (ta_str_equal(text, alias)) {
            return system->scxml_invoke_type;
        }
    }
    return type;
}

/* ------------------------------------------------------------------------
 * Lifecycle hooks
 */

ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state) {
    const ta_definition *definition = it->definition;
    bool has_invokes = false;
    for (uint32_t index = 0; index < definition->invoke_decl_count && !has_invokes; index += 1) {
        has_invokes = definition->invokes[index].owner == state;
    }
    if (!has_invokes) {
        return TA_OK;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(it->system, it->actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_vec *pending = ta_invoke_pending(it->system, sparse);
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            return TA_OK;
        }
    }
    return ta_vec_push(pending, &state);
}

ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    ta_vec *pending = &sparse->pending_invokes;
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            *(ta_state_id *)ta_vec_at(pending, index) = *(ta_state_id *)ta_vec_at(pending, pending->count - 1u);
            pending->count -= 1u;
            break;
        }
    }
    for (uint32_t index = sparse->invoke_count; index > 0; index -= 1u) {
        if (sparse->invoke_meta[index - 1u].state == state) {
            ta_invoke_cancel_at(sparse, index - 1u);
        }
    }
    return TA_OK;
}

typedef struct {
    ta_value src;
    ta_value data;
    ta_value params;
    ta_fault fault;
} ta_invoke_values;

static void ta_invoke_values_release(ta_invoke_values *values) {
    ta_value_release(&values->src);
    ta_value_release(&values->data);
    ta_value_release(&values->params);
    ta_value_release(&values->fault.data);
}

static ta_op_status ta_invoke_text_value(ta_interpreter *it, ta_datamodel_text_value_params *params) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->text_value == NULL) {
        return TA_OP_ERROR;
    }
    params->shared = definition->datamodel_shared;
    params->instance = it->actor->datamodel_instance;
    params->host = &it->actor->host;
    return definition->datamodel.v->text_value(definition->datamodel.self, params);
}

static ta_op_status ta_invoke_value_text(ta_interpreter *it, ta_datamodel_value_text_params *params) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->value_text == NULL) {
        return TA_OP_ERROR;
    }
    params->shared = definition->datamodel_shared;
    params->instance = it->actor->datamodel_instance;
    params->host = &it->actor->host;
    return definition->datamodel.v->value_text(definition->datamodel.self, params);
}

/* The invoke's ID: the author's, or `<stateid>.<platformid>`, also stored through idlocation. */
static ta_op_status ta_invoke_resolve_id(ta_interpreter *it, const ta_invoke_decl *decl,
                                         ta_invoke_values *values, ta_invoke_id *out) {
    ta_system *system = it->system;
    if (decl->id.kind == TA_ID_SOURCE_LITERAL) {
        *out = (ta_invoke_id)decl->id.as.literal + 1u;
        return TA_OP_OK;
    }
    ta_str state = ta_atom_text(&system->atoms, it->definition->states[decl->owner].name);
    system->next_invoke_serial += 1u;
    char digits[20];
    uint32_t digit_count = 0;
    for (uint64_t serial = system->next_invoke_serial; serial != 0 || digit_count == 0; serial /= 10u) {
        digits[digit_count++] = (char)('0' + serial % 10u);
    }
    uint32_t length = state.len + 1u + digit_count;
    char *text = ta_arena_alloc(&system->scratch, length, 1);
    if (text == NULL) {
        return TA_OP_ERROR;
    }
    ta_mem_copy(text, state.ptr, state.len);
    text[state.len] = '.';
    for (uint32_t index = 0; index < digit_count; index += 1) {
        text[state.len + 1u + index] = digits[digit_count - 1u - index];
    }
    ta_str id = { .ptr = text, .len = length };
    if (ta_invoke_id_from_text(system, id, out) != TA_OK) {
        return TA_OP_ERROR;
    }
    if (decl->id.kind != TA_ID_SOURCE_LOCATION) {
        return TA_OP_OK;
    }
    const ta_definition *definition = it->definition;
    ta_value value = {0};
    ta_op_status status = ta_invoke_text_value(it, &(ta_datamodel_text_value_params){
                                                       .text = id, .out = &value, .fault = &values->fault });
    if (status == TA_OP_OK) {
        status = definition->datamodel.v->assign == NULL
                     ? TA_OP_ERROR
                     : definition->datamodel.v->assign(definition->datamodel.self,
                                                       &(ta_datamodel_assign_params){
                                                           .shared = definition->datamodel_shared,
                                                           .instance = it->actor->datamodel_instance,
                                                           .location = decl->id.as.location,
                                                           .value = &value,
                                                           .host = &it->actor->host,
                                                           .fault = &values->fault,
                                                       });
        ta_value_release(&value);
    }
    return status;
}

static ta_op_status ta_invoke_resolve_type(ta_interpreter *it, const ta_invoke_decl *decl,
                                           ta_invoke_values *values, ta_atom *out) {
    ta_system *system = it->system;
    if (decl->type.kind == TA_TEXT_SOURCE_NONE) {
        *out = system->scxml_invoke_type;
        return TA_OP_OK;
    }
    if (decl->type.kind == TA_TEXT_SOURCE_ATOM) {
        *out = ta_invoke_normalize_type(system, decl->type.as.atom);
        return TA_OP_OK;
    }
    ta_value value = {0};
    ta_str text = {0};
    ta_op_status status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                                      .source = { .kind = TA_VALUE_SOURCE_EXPR,
                                                                  .as.expr = decl->type.as.expr },
                                                      .out = &value,
                                                      .fault = &values->fault,
                                                  });
    if (status == TA_OP_OK) {
        status = ta_invoke_value_text(it, &(ta_datamodel_value_text_params){
                                              .value = &value, .out = &text, .fault = &values->fault });
    }
    if (status == TA_OP_OK) {
        ta_atom atom = 0;
        status = ta_atom_intern(&system->atoms, text, &atom) == TA_OK ? TA_OP_OK : TA_OP_ERROR;
        *out = ta_invoke_normalize_type(system, atom);
    }
    ta_value_release(&value);
    return status;
}

/* src (a literal becomes a data-model value), inline content, and params. */
static ta_op_status ta_invoke_resolve_values(ta_interpreter *it, const ta_invoke_decl *decl,
                                             ta_invoke_values *values) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = TA_OP_OK;
    if (decl->src.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes bytes = definition->contents[decl->src.as.content].bytes;
        status = ta_invoke_text_value(it, &(ta_datamodel_text_value_params){
                                              .text = { .ptr = (const char *)bytes.ptr, .len = bytes.len },
                                              .out = &values->src,
                                              .fault = &values->fault,
                                          });
    } else {
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = decl->src, .out = &values->src, .fault = &values->fault });
    }
    if (status == TA_OP_OK && decl->data.kind == TA_VALUE_SOURCE_CONTENT) {
        status = v->import_content == NULL
                     ? TA_OP_ERROR
                     : v->import_content(definition->datamodel.self,
                                         &(ta_datamodel_import_content_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .content = definition->contents[decl->data.as.content].bytes,
                                             .host = &it->actor->host,
                                             .out = &values->data,
                                             .fault = &values->fault,
                                         });
    } else if (status == TA_OP_OK) {
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = decl->data, .out = &values->data, .fault = &values->fault });
    }
    if (status != TA_OP_OK || decl->params.length == 0) {
        return status;
    }
    if (v->make_params == NULL) {
        return TA_OP_ERROR;
    }
    ta_named_value *named = ta_arena_alloc(&it->system->scratch, decl->params.length * sizeof(ta_named_value),
                                           _Alignof(ta_named_value));
    if (named == NULL) {
        return TA_OP_ERROR;
    }
    uint32_t evaluated = 0;
    for (; evaluated < decl->params.length && status == TA_OP_OK; evaluated += 1) {
        const ta_param_def *param = &definition->params[decl->params.offset + evaluated];
        named[evaluated].name = ta_atom_text(&it->system->atoms, param->name);
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = param->value, .out = &named[evaluated].value,
                                             .fault = &values->fault });
    }
    if (status == TA_OP_OK) {
        status = v->make_params(definition->datamodel.self,
                                &(ta_datamodel_make_params_params){
                                    .shared = definition->datamodel_shared,
                                    .instance = it->actor->datamodel_instance,
                                    .values = named,
                                    .count = decl->params.length,
                                    .host = &it->actor->host,
                                    .out = &values->params,
                                    .fault = &values->fault,
                                });
    }
    for (uint32_t index = 0; index < evaluated; index += 1) {
        ta_value_release(&named[index].value);
    }
    return status;
}

static ta_result ta_invoke_start(ta_interpreter *it, ta_invoke_decl_id decl_id) {
    ta_system *system = it->system;
    const ta_invoke_decl *decl = &it->definition->invokes[decl_id];
    ta_invoke_values values = {0};
    ta_invoke_id id = 0;
    ta_atom type = 0;
    ta_op_status status = ta_invoke_resolve_id(it, decl, &values, &id);
    if (status == TA_OP_OK) {
        status = ta_invoke_resolve_type(it, decl, &values, &type);
    }
    const ta_invoker *invoker = status == TA_OP_OK ? ta_invoke_find_invoker(system, type) : NULL;
    if (invoker == NULL || invoker->v == NULL || invoker->v->start == NULL) {
        status = TA_OP_ERROR;
    }
    if (status == TA_OP_OK) {
        status = ta_invoke_resolve_values(it, decl, &values);
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, it->actor);
    if (status == TA_OP_OK && (sparse == NULL || ta_invoke_grow(system, sparse) != TA_OK)) {
        ta_invoke_values_release(&values);
        return TA_ERR_NOMEM;
    }
    if (status != TA_OP_OK) {
        ta_result result = ta_interpreter_execution_error(it, &values.fault);
        ta_invoke_values_release(&values);
        return result;
    }
    /* Record first, so a child spawned during start() can attach to it. */
    uint32_t index = sparse->invoke_count;
    sparse->invokes[index] = (ta_active_invoke){
        .id = id, .type = type, .autoforward = decl->autoforward, .active = true,
    };
    sparse->invoke_meta[index] = (ta_invoke_meta){
        .decl = decl_id, .state = decl->owner, .invoker = *invoker, .child = 0,
    };
    sparse->invoke_count += 1u;
    ta_invoke_request request = {
        .system = system,
        .parent = it->actor->record.id,
        .invoke_id = id,
        .type = type,
        .src = values.src,
        .data = values.data,
        .params = values.params,
        .autoforward = decl->autoforward,
    };
    uint64_t service = 0;
    status = invoker->v->start(invoker->self, &(ta_invoker_start_params){
                                                  .request = &request, .service_handle = &service,
                                                  .fault = &values.fault });
    int64_t found = ta_invoke_find(it->actor, id);
    ta_result result = TA_OK;
    if (status == TA_OP_OK && found >= 0) {
        sparse->invokes[found].service_handle = service;
    } else if (status != TA_OP_OK) {
        if (found >= 0) {
            ta_invoke_remove_at(sparse, (uint32_t)found);
        }
        result = ta_interpreter_execution_error(it, &values.fault);
    }
    ta_invoke_values_release(&values);
    return result;
}

ta_result ta_invoke_start_pending(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL || sparse->pending_invokes.count == 0) {
        return TA_OK;
    }
    const ta_definition *definition = it->definition;
    ta_vec *pending = &sparse->pending_invokes;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        bool listed = false;
        for (uint32_t index = 0; index < pending->count && !listed; index += 1) {
            listed = *(ta_state_id *)ta_vec_at(pending, index) == state;
        }
        if (!listed || !ta_actor_state_active(it->actor, state)) {
            continue;
        }
        for (uint32_t decl = 0; decl < definition->invoke_decl_count; decl += 1) {
            if (definition->invokes[decl].owner != state) {
                continue;
            }
            ta_result result = ta_invoke_start(it, decl);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    pending->count = 0;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Communication
 */

typedef struct {
    ta_actor *sender;
    ta_actor_ref destination;
    ta_event event;
} ta_invoke_delivery;

/*
 * Offers an event to another session through the router. When the router
 * accepts nothing, the event waits in the deadline index, due now, owned by
 * the sender, so ta_system_dispatch_due() offers it again.
 */
static ta_result ta_invoke_submit(ta_system *system, const ta_invoke_delivery *delivery) {
    ta_external_event_submit_status status = TA_EXTERNAL_EVENT_ERROR;
    ta_fault fault = {0};
    if (system->external_events.v != NULL) {
        ta_external_delivery external = { .destination = delivery->destination, .event = delivery->event };
        status = system->external_events.v->submit(system->external_events.self,
                                                   &(ta_external_event_submit_params){
                                                       .delivery = &external, .fault = &fault });
    }
    ta_value_release(&fault.data);
    if (status != TA_EXTERNAL_EVENT_RETRY) {
        return TA_OK;
    }
    ta_delayed_send record = {
        .owner = delivery->sender->record.id,
        .send_id = delivery->event.send_id,
        .due = ta_clock_now(&system->clock),
        .event = delivery->event.name,
        .io_type = system->scxml_io_type,
        .destination = delivery->destination,
        .submitted = true,
        .origin = delivery->event.origin.kind == TA_ORIGIN_ACTOR ? delivery->event.origin.as.actor
                                                                  : delivery->sender->record.self,
        .invoke_id = delivery->event.invoke_id,
    };
    ta_result result = ta_value_clone(&delivery->event.data, &record.data);
    if (result == TA_OK) {
        result = ta_timer_schedule(&system->timers, &record);
        if (result != TA_OK) {
            ta_value_release(&record.data);
        }
    }
    return result;
}

bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out) {
    int64_t index = ta_invoke_find(actor, invoke);
    if (index < 0 || !actor->sparse->invoke_meta[index].has_child) {
        return false;
    }
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = actor->sparse->invoke_meta[index].child };
    return true;
}

ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination) {
    if (sender == NULL || sender->sparse == NULL || !sender->sparse->has_parent ||
        destination->kind != TA_ACTOR_REF_RUNTIME || destination->as.runtime != sender->sparse->parent.actor) {
        return TA_INVOKE_ID_NONE;
    }
    return sender->sparse->parent.invoke_id;
}

void ta_invoke_attach_child(ta_system *system, const ta_actor_parent *parent, ta_actor_id child) {
    ta_actor *actor = ta_actor_lookup(system, parent->actor);
    int64_t index = actor == NULL ? -1 : ta_invoke_find(actor, parent->invoke_id);
    if (index >= 0) {
        actor->sparse->invoke_meta[index].child = child;
        actor->sparse->invoke_meta[index].has_child = true;
    }
}

static ta_result ta_invoke_forward(ta_interpreter *it, uint32_t index, const ta_event *event) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_invoke_meta meta = sparse->invoke_meta[index];
    if (meta.has_child) {
        /* The forwarded event keeps its envelope; it is not an event of the child's own invocation. */
        ta_event copy = *event;
        copy.invoke_id = TA_INVOKE_ID_NONE;
        return ta_invoke_submit(it->system, &(ta_invoke_delivery){
                                                .sender = it->actor,
                                                .destination = { .kind = TA_ACTOR_REF_RUNTIME,
                                                                 .as.runtime = meta.child },
                                                .event = copy,
                                            });
    }
    if (meta.invoker.v != NULL && meta.invoker.v->forward != NULL) {
        ta_fault fault = {0};
        (void)meta.invoker.v->forward(meta.invoker.self, &(ta_invoker_forward_params){
                                                              .service_handle = sparse->invokes[index].service_handle,
                                                              .event = event,
                                                              .fault = &fault,
                                                          });
        ta_value_release(&fault.data);
    }
    return TA_OK;
}

ta_result ta_invoke_before_external(ta_interpreter *it) {
    const ta_event *event = &it->actor->current_event;
    if (event->invoke_id != TA_INVOKE_ID_NONE) {
        /*
         * An event of an invocation cancelled since it was queued is still
         * processed, without <finalize>: cancelling destroys the service, so
         * nothing it sends afterwards arrives, and stale host reports are
         * refused by ta_actor_report_invoke().
         */
        int64_t index = ta_invoke_find(it->actor, event->invoke_id);
        if (index < 0) {
            return TA_OK;
        }
        ta_span32 finalize = it->definition->invokes[it->actor->sparse->invoke_meta[index].decl].finalize_actions;
        ta_result result = ta_interpreter_run_actions(it, finalize);
        if (result != TA_OK) {
            return result;
        }
    }
    ta_actor_sparse_state *sparse = it->actor->sparse;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1) {
        if (sparse->invokes[index].autoforward) {
            ta_result result = ta_invoke_forward(it, index, event);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* Sends done.invoke.<id> with the completion data to the invoking parent. */
static ta_result ta_invoke_publish(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    ta_event_name_id name = 0;
    ta_result result = ta_invoke_done_name(system, sparse->parent.invoke_id, &name);
    if (result != TA_OK) {
        return result;
    }
    ta_event event = {
        .name = name,
        .type = TA_EVENT_EXTERNAL,
        .send_id = TA_SEND_ID_COMPLETION,
        .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
        .invoke_id = sparse->parent.invoke_id,
        .data = sparse->completion.has_data ? sparse->completion.data : (ta_value){0},
    };
    return ta_invoke_submit(system, &(ta_invoke_delivery){
                                        .sender = actor,
                                        .destination = { .kind = TA_ACTOR_REF_RUNTIME,
                                                         .as.runtime = sparse->parent.actor },
                                        .event = event,
                                    });
}

ta_result ta_invoke_finished(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    while (sparse->invoke_count != 0) {
        ta_invoke_cancel_at(sparse, sparse->invoke_count - 1u);
    }
    sparse->pending_invokes.count = 0;
    if (!sparse->has_parent) {
        return TA_OK;
    }
    sparse->completion_pending = true;
    return ta_invoke_publish(it->system, it->actor);
}

void ta_invoke_release(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL) {
        return;
    }
    while (sparse->invoke_count != 0) {
        ta_invoke_cancel_at(sparse, sparse->invoke_count - 1u);
    }
    ta_mem_free(system->allocator, sparse->invokes,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_active_invoke),
                                             .alignment = _Alignof(ta_active_invoke) });
    ta_mem_free(system->allocator, sparse->invoke_meta,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_invoke_meta),
                                             .alignment = _Alignof(ta_invoke_meta) });
    sparse->invokes = NULL;
    sparse->invoke_meta = NULL;
    sparse->invoke_capacity = 0;
    if (sparse->pending_invokes.element_size != 0) {
        ta_vec_destroy(&sparse->pending_invokes);
    }
}

/* ------------------------------------------------------------------------
 * Host reports
 */

static ta_result ta_invoke_unavailable(ta_system *system, ta_actor *actor, const ta_invoke_report_params *params) {
    /* error.communication.invoke.<id>.unavailable */
    ta_event_name_id name = 0;
    ta_atom invoke_atom = 0;
    ta_atom unavailable = 0;
    ta_result result = ta_atom_intern(&system->atoms, (ta_str){ .ptr = "invoke", .len = 6 }, &invoke_atom);
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, (ta_str){ .ptr = "unavailable", .len = 11 }, &unavailable);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, TA_EVENT_NAME_ERROR_COMMUNICATION, invoke_atom, &name);
    }
    ta_str text = ta_invoke_id_text(system, params->invoke_id);
    ta_atom id_atom = 0;
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, text, &id_atom);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, name, id_atom, &name);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, name, unavailable, &name);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_event event = { .name = name, .type = TA_EVENT_PLATFORM, .invoke_id = TA_INVOKE_ID_NONE };
    if (params->as.fault != NULL) {
        result = ta_value_clone(&params->as.fault->data, &event.data);
        if (result != TA_OK) {
            return result;
        }
    }
    result = ta_interpreter_enqueue_internal(system, actor, &event);
    if (result == TA_OK && !actor->macrostep) {
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return result;
}

ta_result ta_actor_report_invoke(ta_system *system, const ta_invoke_report_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    int64_t index = ta_invoke_find(actor, params->invoke_id);
    if (index < 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = params->invoke_id };
    switch (params->kind) {
        case TA_INVOKE_REPORT_EVENT:
            if (params->as.event == NULL) {
                return TA_ERR_INVALID;
            }
            event = *params->as.event;
            event.type = TA_EVENT_EXTERNAL;
            event.invoke_id = params->invoke_id;
            return ta_actor_deliver(system, params->actor, &event);
        case TA_INVOKE_REPORT_DONE: {
            ta_result result = ta_invoke_done_name(system, params->invoke_id, &event.name);
            if (result != TA_OK) {
                return result;
            }
            if (params->as.done_data != NULL) {
                event.data = *params->as.done_data;
            }
            return ta_actor_deliver(system, params->actor, &event);
        }
        case TA_INVOKE_REPORT_UNAVAILABLE:
            ta_invoke_remove_at(actor->sparse, (uint32_t)index);
            return ta_invoke_unavailable(system, actor, params);
        default:
            return TA_ERR_INVALID;
    }
}

ta_result ta_actor_publish_completion(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_sparse_state *sparse = actor->sparse;
    if (!actor->finished || sparse == NULL || !sparse->has_parent) {
        return TA_ERR_INVALID;
    }
    ta_actor *parent = ta_actor_lookup(system, sparse->parent.actor);
    if (parent == NULL || ta_invoke_find(parent, sparse->parent.invoke_id) < 0) {
        (void)ta_timer_withdraw(&system->timers, id, TA_SEND_ID_COMPLETION);
        return TA_ERR_NOT_FOUND;
    }
    if (ta_timer_withdraw(&system->timers, id, TA_SEND_ID_COMPLETION) != TA_OK) {
        return TA_OK; /* already delivered */
    }
    return ta_invoke_publish(system, actor);
}
