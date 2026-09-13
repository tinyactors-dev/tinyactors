#include "internal.h"

typedef struct send_delivery {
    ta_context_t *context;
    const ta_send_t *send;
    uint64_t id;
    uint64_t accepted_ns, due_ns;
    const ta_trace_context_t *trace;
    tai_span_t *span;
} send_delivery_t;
typedef struct send_route {
    tai_actor_t *destination;
    const ta_ioprocessor_desc_t *processor;
    ta_text_t target;
    tai_invocation_t *invocation;
    uint8_t internal;
} send_route_t;

static ta_status_t resolve(send_delivery_t *delivery, send_route_t *route)
{
    tai_actor_t *source = delivery->context->actor;
    ta_system_t *system = source->system;
    ta_target_t target = delivery->send->target;
    ta_actor_ref_t ref = {0};
    ta_status_t status;
    *route = (send_route_t){0};
    route->processor = tai_processor(system, delivery->send->type);
    if (route->processor == NULL) return TA_EXECUTION_ERROR;
    if (route->processor == &system->config.ioprocessors[0] && target.kind == TA_TARGET_URI) {
        status = ta_target_parse(target.text, &target);
        if (status != TA_OK) return TA_EXECUTION_ERROR;
    }
    if (target.kind != TA_TARGET_URI && target.kind != TA_TARGET_INVOKE &&
        route->processor != &system->config.ioprocessors[0]) return TA_EXECUTION_ERROR;
    switch (target.kind) {
    case TA_TARGET_SELF: ref.id = source->id; break;
    case TA_TARGET_INTERNAL: ref.id = source->id; route->internal = 1; break;
    case TA_TARGET_INSTANCE: ref = target.instance; break;
    case TA_TARGET_NAME:
        status = ta_name_lookup(system, target.text, &ref);
        if (status != TA_OK) return TA_COMMUNICATION_ERROR;
        break;
    case TA_TARGET_PARENT:
        if (source->extra == NULL || source->extra->parent_invocation == NULL ||
            source->extra->parent_invocation->cancelled) return TA_COMMUNICATION_ERROR;
        route->invocation = source->extra->parent_invocation;
        ref = route->invocation->actor;
        break;
    case TA_TARGET_INVOKE: {
        tai_invocation_t *invocation = source->extra == NULL ? NULL : source->extra->invocations;
        while (invocation != NULL && tai_text_compare(invocation->id, target.text) != 0) invocation = invocation->next;
        if (invocation == NULL || invocation->cancelled || invocation->done) return TA_COMMUNICATION_ERROR;
        route->invocation = invocation;
        if (invocation->external) {
            route->processor = tai_processor(system, ta_text_from_cstr(invocation->service->event_processor));
            route->target = invocation->source.data != NULL ? invocation->source : invocation->id;
            return route->processor == NULL ? TA_EXECUTION_ERROR : TA_OK;
        }
        ref = invocation->child;
        break;
    }
    case TA_TARGET_URI:
        if (!tai_text_valid(target.text, 0)) return TA_EXECUTION_ERROR;
        if (route->processor == &system->config.ioprocessors[0] && target.text.size >= 8 &&
            tai_text_compare((ta_text_t){target.text.data,8},ta_text_from_cstr("#_scxml_")) == 0) {
            status = ta_session_lookup(system, (ta_text_t){target.text.data+8,target.text.size-8}, &ref);
            if (status != TA_OK) return TA_COMMUNICATION_ERROR;
        } else { route->target = target.text; return TA_OK; }
        break;
    default: return TA_EXECUTION_ERROR;
    }
    route->destination = tai_actor_find(system, ref);
    if (route->destination == NULL || !tai_accepting(route->destination)) return TA_COMMUNICATION_ERROR;
    return TA_OK;
}

static ta_status_t dispatch(send_delivery_t *delivery)
{
    tai_actor_t *actor = delivery->context->actor;
    send_route_t route;
    tai_delivery_t event = {0};
    ta_status_t status = resolve(delivery, &route);
    if (status != TA_OK) return status;
    event.event.name = delivery->send->event;
    event.event.message_id = delivery->id;
    event.event.trace = delivery->trace;
    event.event.sendid = delivery->send->sendid;
    event.event.type = route.internal ? TA_EVENT_INTERNAL : TA_EVENT_EXTERNAL;
    event.internal = route.internal;
    if (!route.internal) {
        event.event.origin = tai_session(actor, 1);
        event.event.origintype = ta_text_from_cstr("scxml");
    }
    if (route.invocation != NULL && route.destination != NULL && route.invocation->actor.id == route.destination->id) {
        event.invocation = route.invocation->ref;
        event.event.invokeid = route.invocation->id;
    }
    if (route.destination != NULL) {
        status = tai_body_payload(delivery->context, &delivery->send->body, &event.event.data);
        if (status != TA_OK) return status;
        status = tai_enqueue(route.destination, &event, NULL);
        return status == TA_OK ? TA_OK : TA_COMMUNICATION_ERROR;
    } else {
        ta_io_request_t request = {0};
        tai_io_t *io;
        request.kind = TA_IO_SEND;
        request.actor.id = actor->id;
        request.revision = actor->definition->revision;
        request.type = ta_text_from_cstr(route.processor->type);
        request.target = route.target;
        request.event = event.event;
        request.body = delivery->send->body;
        request.trace = delivery->trace;
        request.accepted_ns = delivery->accepted_ns;
        request.due_ns = delivery->due_ns;
        if (route.invocation != NULL) { request.invocation = route.invocation->ref; request.invokeid = route.invocation->id; }
        status = tai_io_open(actor, &request, &io);
        if (status != TA_OK) return status;
        io->span = delivery->span;
        delivery->span = NULL;
        tai_io_expose(io);
        return TA_OK;
    }
}

static void start_send(send_delivery_t *delivery)
{
    ta_span_start_t start = {0};
    tai_actor_t *actor = delivery->context->actor;
    start.operation = TA_SPAN_SEND;
    start.kind = TA_SPAN_PRODUCER;
    start.actor.id = actor->id;
    start.revision = actor->definition->revision;
    start.definition_name = actor->definition->description.name;
    start.message_id = delivery->id;
    start.event_name = delivery->send->event;
    if (delivery->send->body.kind == TA_BODY_CONTENT)
        start.event_data = &delivery->send->body.content;
    start.destination = delivery->send->target.text;
    start.processor = delivery->send->type;
    start.now_ns = actor->system->now_ns;
    start.parent = delivery->context->view.trace;
    tai_span_begin(actor->system, &start, &delivery->span);
    delivery->trace = delivery->span == NULL ? delivery->context->view.trace : &delivery->span->value.context;
}

static uint8_t valid_target(const ta_system_t *system, const ta_send_t *send)
{
    const ta_ioprocessor_desc_t *processor=tai_processor(system,send->type);
    ta_target_t target=send->target;
    if(processor==NULL)return 0;
    if(processor==&system->config.ioprocessors[0] && target.kind==TA_TARGET_URI &&
        ta_target_parse(target.text,&target)!=TA_OK)return 0;
    if(processor!=&system->config.ioprocessors[0] && target.kind!=TA_TARGET_URI && target.kind!=TA_TARGET_INVOKE)return 0;
    if(target.kind==TA_TARGET_NAME || target.kind==TA_TARGET_URI || target.kind==TA_TARGET_INVOKE)
        return tai_text_valid(target.text,0);
    if(target.kind==TA_TARGET_INSTANCE)return target.instance.id!=0;
    return target.kind==TA_TARGET_SELF || target.kind==TA_TARGET_INTERNAL || target.kind==TA_TARGET_PARENT;
}

ta_status_t ta_context_send(ta_context_t *context, const ta_send_t *send, uint64_t *out)
{
    send_delivery_t delivery = {0};
    ta_system_t *system;
    ta_status_t status;
    if (out != NULL) *out = 0;
    if (context == NULL || context->actor == NULL || !context->effects || send == NULL) return TA_INVALID;
    system = context->actor->system;
    if (!tai_text_valid(send->event, 0) || !tai_text_valid(send->type, 1) ||
        !tai_text_valid(send->sendid, 1) || (unsigned)send->target.kind > TA_TARGET_URI ||
        send->event.size > system->config.limits.event_name_bytes ||
        !valid_target(system,send)) return TA_EXECUTION_ERROR;
    delivery.context = context;
    delivery.send = send;
    delivery.accepted_ns = delivery.due_ns = system->now_ns;
    if (send->delay_ns != 0) {
        tai_timer_t *timer;
        tai_value_scope_t scope;
        status = tai_timer_schedule(context->actor, send, &timer);
        if (status != TA_OK) return status;
        delivery.id = timer->sequence;
        start_send(&delivery);
        timer->span = delivery.span;
        scope = (tai_value_scope_t){system,timer->arena};
        if (tai_trace_copy(&scope, delivery.trace, &timer->trace) != TA_OK)
            tai_saturate(&system->stats.dropped_trace_records);
        if (out != NULL) *out = timer->sequence;
        tai_saturate(&system->stats.accepted_messages);
        return TA_OK;
    }
    if (system->next_message == 0) return TA_LIMIT;
    delivery.id = system->next_message;
    system->next_message = delivery.id == UINT64_MAX ? 0 : delivery.id + 1;
    start_send(&delivery);
    status = dispatch(&delivery);
    tai_span_finish(delivery.span, status);
    if (status == TA_OK && out != NULL) *out = delivery.id;
    return status;
}

void tai_timer_dispatch(ta_system_t *system)
{
    tai_timer_t *timer = tai_timer_take(system);
    send_delivery_t delivery = {0};
    ta_context_t context;
    ta_status_t status;
    if (timer == NULL) return;
    tai_context(timer->actor, TA_PHASE_TRANSITION, &context);
    delivery.context = &context;
    delivery.send = &timer->send;
    delivery.id = timer->sequence;
    delivery.accepted_ns = timer->accepted_ns;
    delivery.due_ns = timer->deadline;
    delivery.trace = timer->trace;
    delivery.span = timer->span;
    timer->span = NULL;
    status = dispatch(&delivery);
    if (status != TA_OK && timer->actor->status < TA_ACTOR_DONE)
        tai_execution_error(&context, status == TA_EXECUTION_ERROR ? status : TA_COMMUNICATION_ERROR, timer->send.sendid);
    tai_span_finish(delivery.span, status);
    tai_timer_dispose(timer, status);
}
