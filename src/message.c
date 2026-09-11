#include "internal.h"

/* Shared sentinel has no mutable state. Its link and error code live in the
 * actor, reserving allocation-free error admission without an inline event. */
static tai_message_t reserved_error;

static ta_text_t platform_name(uint8_t code)
{
    return code == 1 ? ta_text_from_cstr("error.platform.memory") :
                       ta_text_from_cstr("error.platform.limit");
}

const ta_event_t *tai_current_event(const tai_actor_t *actor, ta_event_t *scratch)
{
    if (actor->current_error != 0) {
        *scratch = (ta_event_t){0};
        scratch->name = platform_name(actor->current_error);
        scratch->type = TA_EVENT_PLATFORM;
        return scratch;
    }
    return actor->current == NULL ? NULL : &actor->current->event;
}

static void append(tai_actor_t *actor, tai_message_t *message, uint8_t internal)
{
    tai_message_t **head = internal ? &actor->internal_head : &actor->external_head;
    tai_message_t **tail = internal ? &actor->internal_tail : &actor->external_tail;
    if (*tail == &reserved_error) actor->error_next = message;
    else if (*tail != NULL) (*tail)->next = message;
    else *head = message;
    *tail = message;
    if (internal) ++actor->internal_count; else ++actor->external_count;
    ++actor->system->stats.queued_events;
    /* Host completions can enqueue an internal error after the previous
     * macrostep became stable. Reenter eventless/internal selection without
     * requiring an unrelated external event to wake this actor. */
    if (internal && actor->stable) {
        actor->stable = 0;
        actor->eventless = 1;
    }
    tai_ready(actor);
    tai_mutated(actor->system);
}

static ta_status_t event_copy(const tai_value_scope_t *scope, const ta_event_t *source,
                              ta_event_t *out)
{
    ta_status_t status;
    *out = *source;
    status = tai_text_copy(scope->arena, source->name, &out->name);
    if (status == TA_OK) status = tai_text_copy(scope->arena, source->sendid, &out->sendid);
    if (status == TA_OK) status = tai_text_copy(scope->arena, source->origin, &out->origin);
    if (status == TA_OK) status = tai_text_copy(scope->arena, source->origintype, &out->origintype);
    if (status == TA_OK) status = tai_text_copy(scope->arena, source->invokeid, &out->invokeid);
    if (status == TA_OK) status = tai_payload_snapshot(scope, &source->data, &out->data);
    if (status == TA_OK && tai_trace_copy(scope, source->trace, &out->trace) != TA_OK) {
        out->trace = NULL;
        tai_saturate(&scope->system->stats.dropped_trace_records);
    }
    return status;
}

ta_status_t tai_enqueue(tai_actor_t *actor, const tai_delivery_t *delivery, uint64_t *out_id)
{
    ta_system_t *system = actor->system;
    const ta_event_t *event = &delivery->event;
    tai_value_scope_t scope;
    ta_arena_t *arena;
    tai_message_t *message;
    ta_memory_stats_t memory;
    ta_status_t status;
    if (out_id != NULL) *out_id = 0;
    if (!tai_text_valid(event->name, 0) || !tai_text_valid(event->sendid, 1) ||
        !tai_text_valid(event->origin, 1) || !tai_text_valid(event->origintype, 1) ||
        !tai_text_valid(event->invokeid, 1)) return TA_INVALID;
    if (event->name.size > system->config.limits.event_name_bytes ||
        system->stats.queued_events >= system->config.limits.queued_events ||
        (delivery->internal ? actor->internal_count - (actor->error_slot != 0) >= actor->limits->internal_events :
                              actor->external_count >= actor->limits->external_events) ||
        (event->message_id == 0 && system->next_message == 0)) return TA_LIMIT;
    if (actor->status >= TA_ACTOR_DONE) return TA_CANCELLED;
    status = tai_actor_scope(actor, TA_LIFETIME_MESSAGE, &arena);
    if (status != TA_OK) return status;
    status = tai_alloc(arena, sizeof(*message), (void **)&message);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    *message = (tai_message_t){0};
    message->arena = arena;
    message->invocation = delivery->invocation;
    scope = (tai_value_scope_t){system, arena};
    status = event_copy(&scope, event, &message->event);
    ta_arena_stats(arena, &memory);
    if (status == TA_OK && (actor->queued_bytes > actor->limits->queued_event_bytes ||
        memory.reserved_bytes > actor->limits->queued_event_bytes - actor->queued_bytes)) status = TA_LIMIT;
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    if (message->event.message_id == 0) {
        message->event.message_id = system->next_message;
        if (system->next_message == UINT64_MAX) system->next_message = 0; else ++system->next_message;
    }
    message->capacity = memory.reserved_bytes;
    actor->queued_bytes += message->capacity;
    append(actor, message, delivery->internal);
    tai_saturate(&system->stats.accepted_messages);
    if (out_id != NULL) *out_id = message->event.message_id;
    return TA_OK;
}

ta_status_t tai_dequeue(tai_actor_t *actor, uint8_t internal)
{
    tai_message_t **head = internal ? &actor->internal_head : &actor->external_head;
    tai_message_t **tail = internal ? &actor->internal_tail : &actor->external_tail;
    tai_message_t *message = *head;
    if (message == NULL) return TA_EMPTY;
    if (actor->current != NULL) ta_arena_close(actor->current->arena);
    actor->current = NULL;
    actor->current_error = 0;
    if (message == &reserved_error) {
        *head = actor->error_next;
        actor->error_next = NULL;
        actor->current_error = actor->error_slot;
        actor->error_slot = 0;
    } else {
        *head = message->next;
        actor->queued_bytes -= message->capacity;
        message->next = NULL;
        actor->current = message;
    }
    if (*head == NULL) *tail = NULL;
    if (internal) --actor->internal_count; else --actor->external_count;
    --actor->system->stats.queued_events;
    tai_mutated(actor->system);
    return TA_OK;
}

static void reserve_error(tai_actor_t *actor, ta_status_t status)
{
    if (actor->error_slot != 0) { tai_actor_fault(actor, status); return; }
    actor->error_slot = status == TA_NO_MEMORY ? 1 : 2;
    actor->error_next = NULL;
    append(actor, &reserved_error, 1);
}

void tai_execution_error(ta_context_t *context, ta_status_t status, ta_text_t sendid)
{
    tai_actor_t *actor = context->actor;
    ta_system_t *system = actor->system;
    ta_diagnostic_t diagnostic = {0};
    tai_delivery_t delivery = {0};
    ta_status_t admitted;
    if (status == TA_OK || actor->status >= TA_ACTOR_DONE) return;
    diagnostic.status = status;
    diagnostic.actor = (ta_actor_ref_t){actor->id};
    diagnostic.revision = actor->definition->revision;
    diagnostic.state = context->view.state_id;
    diagnostic.transition = context->view.transition_id;
    diagnostic.detail = ta_text_from_cstr("statechart execution error");
    if (system->config.diagnostic != NULL)
        system->config.diagnostic(system->config.host_user, &diagnostic);
    if (actor->extra != NULL) tai_span_error(actor->extra->span, &diagnostic);
    if (status == TA_FAULTED) { tai_actor_fault(actor, TA_LIMIT); return; }
    if (status == TA_NO_MEMORY || status == TA_LIMIT) { reserve_error(actor, status); return; }
    delivery.internal = 1;
    delivery.event.name = ta_text_from_cstr(status == TA_COMMUNICATION_ERROR ?
        "error.communication" : "error.execution");
    delivery.event.type = TA_EVENT_PLATFORM;
    delivery.event.sendid = sendid;
    admitted = tai_enqueue(actor, &delivery, NULL);
    if (admitted != TA_OK) reserve_error(actor, admitted);
}

ta_status_t ta_post(ta_system_t *system, const ta_post_options_t *delivery, uint64_t *out)
{
    tai_actor_t *actor;
    tai_delivery_t input = {0};
    ta_actor_ref_t ref;
    ta_status_t status;
    if (out != NULL) *out = 0;
    if (system == NULL || delivery == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (system->shutdown) return TA_CANCELLED;
    ref = delivery->target.instance;
    if (delivery->target.kind == TA_TARGET_NAME) {
        status = ta_name_lookup(system, delivery->target.text, &ref);
        if (status != TA_OK) return status;
    } else if (delivery->target.kind != TA_TARGET_INSTANCE) return TA_INVALID;
    actor = tai_actor_find(system, ref);
    if (actor == NULL) return TA_STALE;
    if (!tai_accepting(actor)) return TA_CANCELLED;
    input.event.name = delivery->message.name;
    input.event.data = delivery->message.data;
    input.event.trace = delivery->message.trace;
    input.event.type = TA_EVENT_EXTERNAL;
    status = tai_enqueue(actor, &input, out);
    if (status != TA_OK) tai_saturate(&system->stats.rejected_messages);
    return status;
}

ta_status_t ta_context_raise(ta_context_t *context, ta_text_t event, const ta_payload_t *data)
{
    tai_delivery_t input = {0};
    if (context == NULL || context->actor == NULL || !context->effects) return TA_INVALID;
    input.internal = 1;
    input.event.name = event;
    input.event.type = TA_EVENT_INTERNAL;
    if (data != NULL) input.event.data = *data;
    input.event.trace = context->view.trace;
    return tai_enqueue(context->actor, &input, NULL);
}

ta_status_t ta_io_receive(ta_system_t *system, const ta_io_route_t *route, const ta_event_t *event)
{
    tai_actor_t *actor;
    const ta_ioprocessor_desc_t *processor;
    tai_delivery_t input = {0};
    if (system == NULL || route == NULL || event == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (system->shutdown) return TA_CANCELLED;
    processor = tai_processor(system, route->processor);
    if (processor == NULL) return TA_UNSUPPORTED;
    actor = tai_actor_find(system, route->destination);
    if (actor == NULL) return TA_STALE;
    if (!tai_accepting(actor)) return TA_CANCELLED;
    if (processor == &system->config.ioprocessors[0]) {
        if (tai_text_compare(event->origintype, ta_text_from_cstr("scxml")) != 0 ||
            event->origin.size <= 8 || !tai_text_valid(event->origin, 0) ||
            tai_text_compare((ta_text_t){event->origin.data,8},ta_text_from_cstr("#_scxml_")) != 0)
            return TA_INVALID;
    }
    input.event = *event;
    input.event.type = TA_EVENT_EXTERNAL;
    input.event.invokeid = (ta_text_t){0};
    input.event.message_id = 0;
    return tai_enqueue(actor, &input, NULL);
}

ta_status_t ta_io_receive_error(ta_system_t *system, const ta_io_route_t *route, ta_text_t sendid)
{
    tai_actor_t *actor;
    ta_context_t context;
    if (system == NULL || route == NULL || !tai_text_valid(sendid, 1)) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (system->shutdown) return TA_CANCELLED;
    if (tai_processor(system, route->processor) == NULL) return TA_UNSUPPORTED;
    actor = tai_actor_find(system, route->destination);
    if (actor == NULL) return TA_STALE;
    if (!tai_accepting(actor)) return TA_CANCELLED;
    tai_context(actor, TA_PHASE_TRANSITION, &context);
    tai_execution_error(&context, TA_COMMUNICATION_ERROR, sendid);
    return TA_OK;
}
