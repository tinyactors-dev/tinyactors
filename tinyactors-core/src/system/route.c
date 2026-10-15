/* route.c — the built-in SCXML I/O processor: local routing between
 * sessions, delayed sends and <cancel>, invocation lifecycle, done.invoke. */
#include <tinyactors/address.h>
#include "base/memory.h"
#include "system/system_internal.h"
#include "trace/record.h"

static ta_actor *actor_at(ta_system *system, uint32_t index) {
    return ta_actor_at(system, index);
}

/* error.communication for the sender of `event` (consumed). A delayed send can
 * fail when its sender is idle — it processes the error now, as a macrostep
 * of its own — or hibernated: the error waits in its mailbox, flagged, and
 * moves to the internal queue on wake (as for TA_EVENT_IN_ERROR_COMMUNICATION). */
static void communication_error(ta_system *system, uint32_t sender, uint32_t event_index) {
    ta_core *core = &system->core;
    uint32_t sendid = ta_event_at(core, event_index)->sendid;
    uint32_t trace = ta_event_at(core, event_index)->trace; /* the error has the send's cause */
    ta_actor *actor =
        sender != TA_NIL && ta_pool_live(&system->actors, sender) ? actor_at(system, sender) : 0;
    if (actor && actor->state == TA_ACTOR_HIBERNATED) {
        uint32_t pe = ta_event_new(core, 1);
        if (pe != TA_NIL) {
            ta_event *event = ta_event_at(core, pe);
            event->type = TA_EVENT_TYPE_PLATFORM;
            event->flags = TA_EVENT_FLAG_RAISE_ERROR_COMMUNICATION;
            event->sendid = sendid;
            ta_shared_string_retain(core, sendid);
            event->trace = trace;
            ta_trace_context_retain(core, trace);
            ta_system_mailbox_push(system, sender, pe);
        } else {
            core->dropped_errors++;
        }
    } else if (actor && actor->state != TA_ACTOR_DONE) {
        uint32_t raised = ta_core_raise_platform(core, ta_actor_instance(system, actor),
                                                 "error.communication", sendid);
        if (raised != TA_NIL && ta_event_at(core, raised)->trace == TA_NIL) {
            ta_event_at(core, raised)->trace = trace;
            ta_trace_context_retain(core, trace);
        }
        ta_system_make_runnable(system, sender);
    }
    ta_event_release(core, event_index);
}

/* "#_scxml_<digits>" -> serial, 0 if malformed (docs/protocol.md, Addresses) */
static uint64_t session_id(ta_string_view t) {
    ta_address address;
    return ta_address_parse_local(t, &address) == TA_OK && address.kind == TA_ADDRESS_SESSION
               ? address.session
               : 0;
}

/* A session of our shared base that is not here (TA_IO_PROCESSOR_SCXML_SHARED):
 * hand the send to the host with the full URI. Returns 1 if it took the event
 * and the target string reference. */
static int forward_shared(ta_system *system, uint32_t sender, uint32_t event_index,
                          uint32_t target) {
    ta_core *core = &system->core;
    uint32_t r = 0;
    while (r < system->config.io_processor_count &&
           (system->config.io_processors[r].flags &
            (TA_IO_PROCESSOR_SCXML_REMOTE | TA_IO_PROCESSOR_SCXML_SHARED)) !=
               (TA_IO_PROCESSOR_SCXML_REMOTE | TA_IO_PROCESSOR_SCXML_SHARED))
        r++;
    if (r == system->config.io_processor_count || ta_system_effect_full(system)) return 0;
    ta_string_view base = system->config.io_processors[r].location,
                   local = ta_shared_string_get(core, target);
    if ((uint64_t)base.length + local.length > sizeof core->scratch) return 0;
    ta_memcpy(core->scratch, base.pointer, base.length);
    ta_memcpy(core->scratch + base.length, local.pointer, local.length);
    uint32_t uri = ta_shared_string_new(core, core->scratch, base.length + local.length);
    if (uri == TA_NIL) return 0;
    ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_SEND, sender);
    ta_event *event = ta_event_at(core, event_index);
    effect->io_processor = r;
    effect->target = uri;
    effect->event_name = event->name;
    ta_shared_string_retain(core, event->name);
    effect->sendid = event->sendid;
    ta_shared_string_retain(core, event->sendid);
    effect->payload = event->payload;
    effect->event = event_index;
    effect->trace = ta_trace_context_value(core, event->trace);
    ta_shared_string_release(core, target);
    return 1;
}

/* Deliver now. Consumes the event reference and the target string reference. */
void ta_system_deliver(ta_system *system, uint32_t sender, uint32_t event_index, uint32_t route,
                       uint32_t arg, uint32_t target) {
    ta_core *core = &system->core;
    ta_event *event = ta_event_at(core, event_index);
    uint32_t dst = TA_NIL;
    switch (route) {
    case TA_ROUTE_SELF:
        dst = sender;
        break;
    case TA_ROUTE_PARENT: {
        ta_actor *actor = sender == TA_NIL ? (ta_actor *)0 : actor_at(system, sender);
        dst = actor && !(actor->flags & ACTOR_FLAG_ORPHAN)
                  ? ta_system_resolve_actor(system, actor->parent)
                  : TA_NIL;
        if (dst != TA_NIL) {
            ta_instance *pin = ta_actor_instance(system, actor_at(system, dst));
            uint32_t id = ta_core_invoke_id(core, pin, actor->parent_slot);
            ta_shared_string_retain(core, id);
            ta_shared_string_release(core, event->invokeid);
            event->invokeid = id;
            event->invoke_slot = (uint16_t)actor->parent_slot;
            event->flags |= TA_EVENT_FLAG_FROM_CHILD;
        }
        break;
    }
    case TA_ROUTE_CHILD:
        if (arg & TA_EXTERNAL_INVOKE_BIT) { /* external invoke: hand to host */
            ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_SEND, sender);
            if (effect) {
                effect->token = arg & ~TA_EXTERNAL_INVOKE_BIT;
                effect->event_name = event->name;
                ta_shared_string_retain(core, event->name);
                effect->event = event_index;
                effect->trace = ta_trace_context_value(core, event->trace);
                ta_shared_string_release(core, target);
                return;
            }
            ta_shared_string_release(core, target);
            communication_error(system, sender, event_index);
            return;
        }
        dst = ta_system_resolve_actor(system, arg);
        break;
    case TA_ROUTE_SESSION: {
        /* a malformed session id names no session anywhere: error.communication
         * at once (internal queue); a well-formed one may live in another
         * system sharing our base */
        uint64_t serial = session_id(ta_shared_string_get(core, target));
        dst = ta_system_session_lookup(system, serial);
        if (serial && (dst == TA_NIL || actor_at(system, dst)->state == TA_ACTOR_DONE) &&
            forward_shared(system, sender, event_index, target))
            return;
        break;
    }
    case TA_ROUTE_EXTERNAL: {
        ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_SEND, sender);
        if (!effect) { /* outbox full is prevented by the scheduler; be safe */
            ta_shared_string_release(core, target);
            communication_error(system, sender, event_index);
            return;
        }
        effect->io_processor = arg;
        effect->target = target; /* ownership moves to the effect */
        effect->event_name = event->name;
        ta_shared_string_retain(core, event->name);
        effect->sendid = event->sendid;
        ta_shared_string_retain(core, event->sendid);
        effect->payload = event->payload;
        effect->event = event_index; /* payload stays alive until ack */
        effect->trace = ta_trace_context_value(core, event->trace);
        return;
    }
    }
    ta_shared_string_release(core, target);
    if (dst == TA_NIL || actor_at(system, dst)->state == TA_ACTOR_DONE) {
        communication_error(system, sender, event_index);
        return;
    }
    ta_system_mailbox_push(system, dst, event_index);
}

static uint64_t ns_to_ticks(const ta_system *system, uint64_t ns) {
    uint64_t tps = system->config.ticks_per_second, seconds = ns / 1000000000ull;
    if (tps && seconds > (UINT64_MAX - tps) / tps) return UINT64_MAX; /* saturate: never */
    return seconds * tps + (ns % 1000000000ull) * tps / 1000000000ull;
}

uint32_t ta_system_route_send(ta_system *system, uint32_t sender, const ta_core_out *out) {
    ta_core *core = &system->core;
    uint32_t target = out->target;
    ta_shared_string_retain(core, target); /* out's reference is released by the kernel */
    if (!out->delay_ns) {
        ta_system_deliver(system, sender, out->event, out->route, out->arg, target);
        return TA_POOL_NONE;
    }
    uint32_t ti = ta_pool_alloc(&system->timers);
    if (ti == TA_NIL) {
        ta_shared_string_release(core, target);
        ta_event_release(core, out->event);
        return TA_POOL_TIMERS;
    }
    ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
    ta_actor *actor = actor_at(system, sender);
    timer->owner = ta_pool_handle(&system->actors, sender);
    timer->event = out->event;
    timer->route = out->route;
    timer->arg = out->arg;
    timer->target = target;
    timer->owner_next = actor->timers;
    actor->timers = ti;
    /* saturating: a delay beyond 64 bits of nanoseconds (parsed as UINT64_MAX) is
     * "never" — it must not wrap around and fire early */
    uint64_t ticks = out->delay_ns == UINT64_MAX ? UINT64_MAX : ns_to_ticks(system, out->delay_ns);
    uint64_t due = ticks > UINT64_MAX - 1 - system->now ? UINT64_MAX - 1 : system->now + ticks;
    ta_heap_push(&system->heap, due, (uint32_t)system->sequence++, ti);
    return TA_POOL_NONE;
}

static void unlink_timer(ta_system *system, uint32_t owner_index, uint32_t ti) {
    ta_actor *actor = actor_at(system, owner_index);
    uint32_t *pp = &actor->timers;
    while (*pp != TA_NIL && *pp != ti)
        pp = &((ta_timer *)ta_pool_at(&system->timers, *pp))->owner_next;
    if (*pp == ti) *pp = ((ta_timer *)ta_pool_at(&system->timers, ti))->owner_next;
}

void ta_system_cancel(ta_system *system, uint32_t sender, uint32_t sendid) {
    ta_core *core = &system->core;
    ta_string_view want = ta_shared_string_get(core, sendid);
    ta_actor *actor = actor_at(system, sender);
    for (uint32_t ti = actor->timers; ti != TA_NIL;) {
        ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
        uint32_t next = timer->owner_next;
        uint32_t sid = ta_event_at(core, timer->event)->sendid;
        if (sid != TA_NIL && (sid == sendid || ta_shared_string_equal(core, sid, want))) {
            unlink_timer(system, sender, ti);
            ta_heap_remove(&system->heap, ti);
            ta_event_release(core, timer->event);
            ta_shared_string_release(core, timer->target);
            ta_pool_free(&system->timers, ti);
        }
        ti = next;
    }
}

void ta_system_cancel_timers(ta_system *system, uint32_t index) {
    ta_core *core = &system->core;
    ta_actor *actor = actor_at(system, index);
    while (actor->timers != TA_NIL) {
        uint32_t ti = actor->timers;
        ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
        actor->timers = timer->owner_next;
        ta_heap_remove(&system->heap, ti);
        ta_event_release(core, timer->event);
        ta_shared_string_release(core, timer->target);
        ta_pool_free(&system->timers, ti);
    }
}

void ta_system_fire_timers(ta_system *system) {
    const ta_heap_node *top;
    while ((top = ta_heap_top(&system->heap)) && top->due <= system->now) {
        uint32_t ti = ta_heap_pop(&system->heap);
        ta_timer timer = *(ta_timer *)ta_pool_at(&system->timers, ti);
        uint32_t owner = ta_system_resolve_actor(system, timer.owner);
        if (owner != TA_NIL) unlink_timer(system, owner, ti);
        ta_pool_free(&system->timers, ti);
        system->stats_timers++;
        ta_event *event = ta_event_at(&system->core, timer.event);
        if (event->trace == TA_NIL) { /* a timer of an untraced send starts a trace */
            ta_trace_context value;
            ta_trace_ask ask = {
                TA_TRACE_AT_START, TA_TRACE_FROM_TIMER, owner,
                owner == TA_NIL ? TA_NIL : ta_actor_chart(system, ta_actor_at(system, owner)),
                timer.event};
            event->trace = ta_system_start_trace(system, (ta_string_view){(const char *)0, 0}, 0,
                                                 &ask, &value);
        }
        ta_system_deliver(system, owner, timer.event, timer.route, timer.arg, timer.target);
    }
}

/* ---- invocation -------------------------------------------------------------------------- */

/* Spawn an SCXML child for an invoke. On pool exhaustion the spawn is parked
 * as a pending request (retried after growth) and TA_POOL_* is returned. */
static uint32_t park_spawn(ta_system *s, uint32_t parent, uint32_t slot, uint32_t chart,
                           uint32_t invokeid, ta_payload payload);

static uint32_t spawn_child(ta_system *system, uint32_t parent, uint32_t slot, uint32_t chart,
                            uint32_t invokeid, ta_payload payload, int may_park) {
    ta_core *core = &system->core;
    ta_status status;
    uint32_t ph = ta_pool_handle(&system->actors, parent);
    uint32_t c = ta_system_spawn_actor(system, chart, ta_shared_string_get(core, invokeid), payload,
                                       ph, slot, (ta_string_view){(const char *)0, 0}, &status);
    ta_instance *pin = ta_actor_instance(system, actor_at(system, parent));
    if (c == TA_NIL) {
        if (status == TA_ERROR_NO_MEMORY && may_park)
            return park_spawn(system, parent, slot, chart, invokeid, payload);
        if (payload) {
            const ta_datamodel_binding *binding =
                ta_core_datamodel(core, ta_actor_chart(system, actor_at(system, parent)));
            if (binding->vtable->release) binding->vtable->release(binding->datamodel, payload);
        }
        ta_core_raise_platform(core, pin, "error.execution", TA_NIL);
        return TA_POOL_NONE;
    }
    ta_core_set_invoke_child(core, pin, slot, ta_pool_handle(&system->actors, c));
    return TA_POOL_NONE;
}

static uint32_t park_spawn(ta_system *system, uint32_t parent, uint32_t slot, uint32_t chart,
                           uint32_t invokeid, ta_payload payload) {
    ta_core *core = &system->core;
    uint32_t pool = ta_pool_free_count(&system->actors) == 0 ? TA_POOL_ACTORS : TA_POOL_SLAB;
    uint32_t ri = ta_pool_alloc(&system->requests);
    if (ri == TA_NIL) { /* cannot even park it: the invoke fails */
        const ta_datamodel_binding *binding =
            ta_core_datamodel(core, ta_actor_chart(system, actor_at(system, parent)));
        if (payload && binding->vtable->release)
            binding->vtable->release(binding->datamodel, payload);
        ta_core_raise_platform(core, ta_actor_instance(system, actor_at(system, parent)),
                               "error.execution", TA_NIL);
        return TA_POOL_REQUESTS;
    }
    ta_request *request = (ta_request *)ta_pool_at(&system->requests, ri);
    ta_memset(request, 0, sizeof *request);
    request->kind = REQUEST_PENDING_SPAWN;
    request->actor = ta_pool_handle(&system->actors, parent);
    request->slot = slot;
    request->invokeid = invokeid;
    ta_shared_string_retain(core, invokeid);
    request->source = chart;
    request->payload = payload;
    request->pad = core->chart_datamodel[ta_actor_chart(system, actor_at(system, parent))];
    return pool;
}

uint32_t ta_system_retry_pending(ta_system *system) {
    ta_core *core = &system->core;
    for (uint32_t i = 0; i < system->requests.high_water_mark; i++) {
        if (!ta_pool_live(&system->requests, i)) continue;
        ta_request *request = (ta_request *)ta_pool_at(&system->requests, i);
        if (request->kind != REQUEST_PENDING_SPAWN) continue;
        uint32_t parent = ta_system_resolve_actor(system, request->actor);
        ta_request request_record = *request;
        if (parent == TA_NIL ||
            ta_core_invoke_id(core, ta_actor_instance(system, actor_at(system, parent)),
                              request_record.slot) != request_record.invokeid) {
            const ta_datamodel_binding *binding = &system->datamodels[request_record.pad];
            if (request_record.payload && binding->vtable->release)
                binding->vtable->release(binding->datamodel, request_record.payload);
        } else {
            ta_status status;
            uint32_t ph = ta_pool_handle(&system->actors, parent);
            uint32_t c = ta_system_spawn_actor(system, request_record.source,
                                               ta_shared_string_get(core, request_record.invokeid),
                                               request_record.payload, ph, request_record.slot,
                                               (ta_string_view){(const char *)0, 0}, &status);
            if (c == TA_NIL && status == TA_ERROR_NO_MEMORY)
                return ta_pool_free_count(&system->actors) == 0 ? TA_POOL_ACTORS : TA_POOL_SLAB;
            if (c == TA_NIL) {
                const ta_datamodel_binding *binding = &system->datamodels[request_record.pad];
                if (request_record.payload && binding->vtable->release)
                    binding->vtable->release(binding->datamodel, request_record.payload);
                ta_core_raise_platform(core, ta_actor_instance(system, actor_at(system, parent)),
                                       "error.execution", TA_NIL);
            } else {
                ta_core_set_invoke_child(core, ta_actor_instance(system, actor_at(system, parent)),
                                         request_record.slot, ta_pool_handle(&system->actors, c));
            }
            ta_system_make_runnable(system, parent);
        }
        ta_shared_string_release(core, request_record.invokeid);
        ta_pool_free(&system->requests, i);
    }
    return TA_POOL_NONE;
}

/* The invoked record of the invoking step: the invocation, and the child
 * (0 when the host runs it or it is not running yet). */
static void trace_invoked(ta_system *system, ta_instance *pin, const ta_core_out *out) {
    ta_core *core = &system->core;
    ta_trace_record *record = ta_core_trace_step(
        core, pin, TA_TRACE_INVOKED, (uint32_t)(sizeof *record + sizeof(ta_trace_invoked)),
        pin->microstep, (const uint32_t *)0);
    if (!record) return;
    ta_trace_invoked *invoked = (ta_trace_invoked *)ta_trace_record_body(record);
    invoked->type = out->type;
    invoked->source = out->source;
    invoked->invokeid = out->invokeid;
    ta_shared_string_retain(core, invoked->type);
    ta_shared_string_retain(core, invoked->source);
    ta_shared_string_retain(core, invoked->invokeid);
    uint32_t child = ta_core_invoke_child(core, pin, out->slot);
    uint32_t c = child & TA_EXTERNAL_INVOKE_BIT ? TA_NIL : ta_system_resolve_actor(system, child);
    invoked->child = c == TA_NIL ? 0 : child;
    invoked->child_session = c == TA_NIL ? 0 : actor_at(system, c)->serial;
    ta_trace_record_commit(core->journal, record);
}

static uint32_t invoke(ta_system *system, uint32_t parent, const ta_core_out *out);

void ta_system_external_free(ta_system *system, uint32_t ri) {
    ta_core *core = &system->core;
    ta_request *request = (ta_request *)ta_pool_at(&system->requests, ri);
    ta_shared_string_release(core, request->invokeid);
    ta_shared_string_release(core, request->source);
    ta_shared_string_release(core, request->type);
    if (request->payload && request->pad < system->datamodel_count) {
        const ta_datamodel_binding *binding = &system->datamodels[request->pad];
        if (binding->vtable->release) binding->vtable->release(binding->datamodel, request->payload);
    }
    ta_pool_free(&system->requests, ri);
}

uint32_t ta_system_external_invocation(ta_system *system, uint32_t from, ta_effect *out) {
    ta_core *core = &system->core;
    for (uint32_t i = from; i < system->requests.high_water_mark; i++) {
        if (!ta_pool_live(&system->requests, i)) continue;
        const ta_request *request = (const ta_request *)ta_pool_at(&system->requests, i);
        if (request->kind != REQUEST_EXTERNAL_INVOKE) continue;
        uint32_t parent = ta_system_resolve_actor(system, request->actor);
        if (parent == TA_NIL) continue;
        ta_memset(out, 0, sizeof *out);
        out->size = sizeof *out;
        out->kind = TA_EFFECT_INVOKE;
        out->actor = request->actor;
        out->session = actor_at(system, parent)->serial;
        out->token = ta_pool_handle(&system->requests, i);
        out->io_processor = TA_NIL;
        out->flags = TA_EFFECT_FLAG_RESTORED;
        out->target = ta_shared_string_get(core, request->source);
        out->event = ta_shared_string_get(core, request->type);
        out->sendid = ta_shared_string_get(core, request->invokeid);
        out->payload = request->payload;
        out->datamodel = request->payload && request->pad < system->datamodel_count ? request->pad : TA_NIL;
        return i;
    }
    return TA_NIL;
}

uint32_t ta_system_invoke(ta_system *system, uint32_t parent, const ta_core_out *out) {
    uint32_t pool = invoke(system, parent, out);
    trace_invoked(system, ta_actor_instance(system, actor_at(system, parent)), out);
    return pool;
}

static uint32_t invoke(ta_system *system, uint32_t parent, const ta_core_out *out) {
    ta_core *core = &system->core;
    ta_actor *pa = actor_at(system, parent);
    ta_instance *pin = ta_actor_instance(system, pa);
    if (out->type != TA_NIL) { /* external invoke: the host runs it */
        uint32_t ri = ta_pool_alloc(&system->requests);
        ta_effect_record *effect = ri == TA_NIL
                                       ? (ta_effect_record *)0
                                       : ta_system_effect_push(system, TA_EFFECT_INVOKE, parent);
        if (!effect) {
            if (ri != TA_NIL) ta_pool_free(&system->requests, ri);
            ta_core_raise_platform(core, pin, "error.execution", TA_NIL);
            return ri == TA_NIL ? TA_POOL_REQUESTS : TA_POOL_NONE;
        }
        ta_request *request = (ta_request *)ta_pool_at(&system->requests, ri);
        ta_memset(request, 0, sizeof *request);
        request->kind = REQUEST_EXTERNAL_INVOKE;
        request->actor = ta_pool_handle(&system->actors, parent);
        request->slot = out->slot;
        request->invokeid = out->invokeid;
        ta_shared_string_retain(core, out->invokeid);
        request->source = out->source; /* kept while it runs: a restore announces it again */
        ta_shared_string_retain(core, out->source);
        request->type = out->type;
        ta_shared_string_retain(core, out->type);
        request->pad = core->chart_datamodel[ta_actor_chart(system, pa)];
        const ta_datamodel_binding *binding = &system->datamodels[request->pad];
        if (out->payload && binding->vtable->retain) { /* the effect's reference, and the request's */
            binding->vtable->retain(binding->datamodel, out->payload);
            request->payload = out->payload;
        }
        uint32_t tok = ta_pool_handle(&system->requests, ri);
        effect->token = tok;
        effect->target = out->source;
        ta_shared_string_retain(core, out->source);
        effect->event_name = out->type;
        ta_shared_string_retain(core, out->type);
        effect->sendid = out->invokeid;
        ta_shared_string_retain(core, out->invokeid);
        effect->payload = out->payload;
        effect->datamodel = core->chart_datamodel[ta_actor_chart(system, pa)];
        effect->trace = ta_core_step_trace(core, pin);
        ta_core_set_invoke_child(core, pin, out->slot, tok | TA_EXTERNAL_INVOKE_BIT);
        return TA_POOL_NONE;
    }
    uint32_t chart = out->chart;
    if (chart == TA_NIL && out->source != TA_NIL) {
        ta_string_view name = ta_shared_string_get(core, out->source);
        /* "file:x.scxml", "file://x.scxml" and "./x.scxml" name the linked chart x.scxml */
        if (name.length > 7 && !ta_memcmp(name.pointer, "file://", 7)) {
            name.pointer += 7;
            name.length -= 7;
        } else if (name.length > 5 && !ta_memcmp(name.pointer, "file:", 5)) {
            name.pointer += 5;
            name.length -= 5;
        }
        if (name.length > 2 && name.pointer[0] == '.' && name.pointer[1] == '/') {
            name.pointer += 2;
            name.length -= 2;
        }
        uint32_t i = 0;
        while (i < name.length && (name.pointer[i] == ' ' || name.pointer[i] == '\t' ||
                                   name.pointer[i] == '\n' || name.pointer[i] == '\r'))
            i++;
        if (i < name.length && name.pointer[i] == '<') {
            /* a document: match the normalized text of documents that appear
             * literally in the image (see ta_compiler_document_key) */
            char *key = core->scratch;
            uint32_t n = 0, capacity = (uint32_t)sizeof core->scratch;
            const char *pre = "inline:";
            while (pre[n]) {
                key[n] = pre[n];
                n++;
            }
            int in_whitespace = 1, fits = 1;
            for (; i < name.length; i++) {
                char character = name.pointer[i];
                if (character == ' ' || character == '\t' || character == '\n' ||
                    character == '\r') {
                    in_whitespace = 1;
                    continue;
                }
                if (n + 2 >= capacity) {
                    fits = 0;
                    break;
                }
                if (in_whitespace && n > 7) key[n++] = ' ';
                in_whitespace = 0;
                key[n++] = character;
            }
            ta_string_view k = {key, n};
            chart = fits ? ta_image_chart_by_name(core->image, k) : TA_NIL;
        } else {
            chart = ta_image_chart_by_name(core->image, name);
        }
    }
    if (chart != TA_NIL)
        return spawn_child(system, parent, out->slot, chart, out->invokeid, out->payload, 1);
    /* unknown dynamic src: ask the host to resolve it to a chart index */
    uint32_t ri = ta_pool_alloc(&system->requests);
    ta_effect_record *effect = ri == TA_NIL
                                   ? (ta_effect_record *)0
                                   : ta_system_effect_push(system, TA_EFFECT_REQUEST, parent);
    if (!effect) {
        if (ri != TA_NIL) ta_pool_free(&system->requests, ri);
        const ta_datamodel_binding *binding = ta_core_datamodel(core, ta_actor_chart(system, pa));
        if (out->payload && binding->vtable->release)
            binding->vtable->release(binding->datamodel, out->payload);
        ta_core_raise_platform(core, pin, "error.execution", TA_NIL);
        return ri == TA_NIL ? TA_POOL_REQUESTS : TA_POOL_NONE;
    }
    ta_request *request = (ta_request *)ta_pool_at(&system->requests, ri);
    ta_memset(request, 0, sizeof *request);
    request->kind = REQUEST_RESOLVE_SOURCE;
    request->actor = ta_pool_handle(&system->actors, parent);
    request->slot = out->slot;
    request->invokeid = out->invokeid;
    ta_shared_string_retain(core, out->invokeid);
    request->source = out->source;
    ta_shared_string_retain(core, out->source);
    request->payload = out->payload;
    request->pad = core->chart_datamodel[ta_actor_chart(system, pa)];
    effect->token = ta_pool_handle(&system->requests, ri);
    effect->flags = TA_EFFECT_FLAG_NEEDS_RESPONSE;
    effect->auxiliary = out->source;
    ta_shared_string_retain(core, out->source);
    return TA_POOL_NONE;
}

ta_status ta_system_resume(ta_system *system, const ta_resume *resume) {
    if (system->in_call) return TA_ERROR_STATE;
    uint32_t request = resume->request;
    ta_status status = resume->status;
    ta_payload result = resume->result;
    ta_core *core = &system->core;
    uint32_t ri = ta_pool_resolve(&system->requests, request);
    if (ri == TA_NIL) return TA_ERROR_STALE;
    ta_request *request_record = (ta_request *)ta_pool_at(&system->requests, ri);
    if (request_record->kind != REQUEST_RESOLVE_SOURCE) return TA_ERROR_INVALID;
    uint32_t parent = ta_system_resolve_actor(system, request_record->actor);
    ta_request request_copy = *request_record;
    ta_pool_free(&system->requests, ri);
    ta_status result_status = TA_OK;
    if (parent == TA_NIL ||
        ta_core_invoke_id(core, ta_actor_instance(system, actor_at(system, parent)),
                          request_copy.slot) != request_copy.invokeid) {
        /* parent gone or invoke cancelled meanwhile */
        const ta_datamodel_binding *binding = &system->datamodels[request_copy.pad];
        if (request_copy.payload && binding->vtable->release)
            binding->vtable->release(binding->datamodel, request_copy.payload);
    } else if (status != TA_OK || result == 0 || result > system->chart_count) {
        const ta_datamodel_binding *binding = &system->datamodels[request_copy.pad];
        if (request_copy.payload && binding->vtable->release)
            binding->vtable->release(binding->datamodel, request_copy.payload);
        ta_core_raise_platform(core, ta_actor_instance(system, actor_at(system, parent)),
                               "error.execution", TA_NIL);
        ta_system_make_runnable(system, parent);
    } else {
        system->in_call = 1;
        uint32_t pool = spawn_child(system, parent, request_copy.slot, (uint32_t)(result - 1),
                                    request_copy.invokeid, request_copy.payload, 1);
        system->in_call = 0;
        if (pool) result_status = TA_ERROR_NO_MEMORY;
    }
    ta_shared_string_release(core, request_copy.invokeid);
    ta_shared_string_release(core, request_copy.source);
    return result_status;
}

/* Drop events from `child` still queued in the parent's mailbox (the
 * invocation is being cancelled while running: its events are ignored). */
static void purge_from(ta_system *system, uint32_t parent, uint32_t child) {
    ta_core *core = &system->core;
    ta_actor *pa = actor_at(system, parent);
    uint32_t prev = TA_NIL, event_index = pa->mailbox_head;
    while (event_index != TA_NIL) {
        ta_event *event = ta_event_at(core, event_index);
        uint32_t next = event->next;
        if ((event->flags & TA_EVENT_FLAG_FROM_CHILD) && event->origin == child) {
            if (prev == TA_NIL)
                pa->mailbox_head = next;
            else
                ta_event_at(core, prev)->next = next;
            if (pa->mailbox_tail == event_index) pa->mailbox_tail = prev;
            pa->mailbox_length--;
            ta_event_release(core, event_index);
        } else {
            prev = event_index;
        }
        event_index = next;
    }
}

void ta_system_uninvoke(ta_system *system, uint32_t parent, const ta_core_out *out) {
    ta_core *core = &system->core;
    if (out->child & TA_EXTERNAL_INVOKE_BIT) {
        uint32_t ri = ta_pool_resolve(&system->requests, out->child & ~TA_EXTERNAL_INVOKE_BIT);
        if (ri == TA_NIL) return;
        ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_UNINVOKE, parent);
        if (effect) effect->token = out->child & ~TA_EXTERNAL_INVOKE_BIT;
        purge_from(system, parent, out->child);
        ta_system_external_free(system, ri);
        return;
    }
    uint32_t c = ta_system_resolve_actor(system, out->child);
    if (c == TA_NIL) return; /* already finished: its events stay deliverable */
    purge_from(system, parent, out->child);
    ta_actor *ca = actor_at(system, c);
    ca->flags |= ACTOR_FLAG_CANCEL | ACTOR_FLAG_ORPHAN;
    /* pending events of a cancelled session are never processed */
    for (uint32_t event = ca->mailbox_head; event != TA_NIL;) {
        uint32_t nx = ta_event_at(core, event)->next;
        ta_event_release(core, event);
        event = nx;
    }
    ca->mailbox_head = ca->mailbox_tail = TA_NIL;
    ca->mailbox_length = 0;
    if (ca->state != TA_ACTOR_HIBERNATED) /* else: applied when it is woken */
        ta_core_cancel(core, ta_actor_instance(system, ca));
    ta_system_make_runnable(system, c);
}

void ta_system_autoforward(ta_system *system, uint32_t parent, const ta_core_out *out) {
    ta_core *core = &system->core;
    uint32_t source = out->event;
    /* the copy is caused by the parent's macrostep, which took the event */
    ta_instance *pin = ta_actor_instance(system, actor_at(system, parent));
    if (out->child & TA_EXTERNAL_INVOKE_BIT) { /* external invoke: forward via host */
        ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_SEND, TA_NIL);
        if (effect) {
            effect->token = out->child & ~TA_EXTERNAL_INVOKE_BIT;
            effect->event_name = ta_event_at(core, source)->name;
            ta_shared_string_retain(core, effect->event_name);
            effect->payload = ta_event_at(core, source)->payload;
            effect->event = source;
            effect->trace = ta_core_step_trace(core, pin);
            return;
        }
        ta_event_release(core, source);
        return;
    }
    uint32_t c = ta_system_resolve_actor(system, out->child);
    uint32_t event_index = c == TA_NIL ? TA_NIL : ta_event_new(core, 0);
    if (event_index == TA_NIL) {
        ta_event_release(core, source);
        return;
    }
    ta_event *event = ta_event_at(core, source), *n = ta_event_at(core, event_index);
    n->type = TA_EVENT_TYPE_EXTERNAL;
    n->name_id = event->name_id;
    n->name = event->name;
    ta_shared_string_retain(core, event->name);
    n->sendid = event->sendid;
    ta_shared_string_retain(core, event->sendid);
    n->origin = event->origin;
    n->origin_string = event->origin_string;
    ta_shared_string_retain(core, event->origin_string);
    n->origintype = event->origintype;
    ta_shared_string_retain(core, event->origintype);
    n->payload = event->payload;
    n->datamodel = event->datamodel;
    if (n->payload && n->datamodel < core->datamodel_count &&
        core->datamodels[n->datamodel].vtable->retain)
        core->datamodels[n->datamodel].vtable->retain(core->datamodels[n->datamodel].datamodel,
                                                      n->payload);
    else if (n->payload && n->datamodel < core->datamodel_count &&
             core->datamodels[n->datamodel].vtable->release)
        n->payload = 0; /* cannot share without retain: forward without data */
    n->trace = ta_core_step_context(core, pin);
    ta_event_release(core, source);
    ta_system_mailbox_push(system, c, event_index);
}

/* Child (or root) reached a top-level final state. */
void ta_system_done(ta_system *system, uint32_t index, ta_payload done, uint32_t final_state) {
    ta_core *core = &system->core;
    ta_actor *actor = actor_at(system, index);
    /* the session's end is reported under its last step (the kernel kept its context) */
    ta_instance *instance = ta_actor_instance(system, actor);
    {
        ta_trace_context value = ta_trace_context_value(core, instance->trace);
        value.parent = 0;
        ta_trace_record *record =
            ta_system_trace_lifecycle(system, index, TA_TRACE_FINISHED,
                                      (uint32_t)(sizeof *record + sizeof(ta_trace_finished)),
                                      &value, 0, TA_TRACE_STEP_FINISHED);
        if (record) {
            ta_trace_finished *finished = (ta_trace_finished *)ta_trace_record_body(record);
            finished->final_state = final_state;
            finished->name = actor->name;
            ta_shared_string_retain(core, actor->name);
            ta_trace_record_commit(core->journal, record);
        }
    }
    uint32_t parent = (actor->flags & ACTOR_FLAG_ORPHAN)
                          ? TA_NIL
                          : ta_system_resolve_actor(system, actor->parent);
    uint32_t self = ta_pool_handle(&system->actors, index);
    const ta_datamodel_binding *binding = ta_core_datamodel(core, ta_actor_chart(system, actor));
    if (parent != TA_NIL) {
        ta_instance *pin = ta_actor_instance(system, actor_at(system, parent));
        uint32_t id = ta_core_invoke_id(core, pin, actor->parent_slot);
        uint32_t event_index = ta_core_invoke_child(core, pin, actor->parent_slot) == self
                                   ? ta_event_new(core, 1)
                                   : TA_NIL;
        if (event_index != TA_NIL) {
            ta_event *event = ta_event_at(core, event_index);
            ta_string_view ids = ta_shared_string_get(core, id);
            char *buffer = core->scratch;
            uint32_t n = 0;
            const char *pre = "done.invoke.";
            while (pre[n]) {
                buffer[n] = pre[n];
                n++;
            }
            for (uint32_t i = 0; i < ids.length && n < sizeof core->scratch; i++)
                buffer[n++] = ids.pointer[i];
            if (ta_event_set_name(core, event, buffer, n) == TA_OK) {
                event->type = TA_EVENT_TYPE_EXTERNAL;
                event->origin = self;
                event->invokeid = id;
                ta_shared_string_retain(core, id);
                event->invoke_slot = (uint16_t)actor->parent_slot;
                event->flags = TA_EVENT_FLAG_FROM_CHILD | TA_EVENT_FLAG_DONE;
                event->payload = done;
                event->datamodel = core->chart_datamodel[ta_actor_chart(system, actor)];
                event->trace = ta_core_step_context(core, instance);
                done = 0;
                ta_system_mailbox_push(system, parent, event_index);
            } else {
                ta_event_release(core, event_index);
            }
        }
    } else if (!actor->parent && !(actor->flags & ACTOR_FLAG_ORPHAN)) {
        ta_effect_record *effect = ta_system_effect_push(system, TA_EFFECT_DONE, index);
        if (effect) {
            effect->payload = done;
            effect->datamodel = core->chart_datamodel[ta_actor_chart(system, actor)];
            effect->target = actor->name;
            ta_shared_string_retain(core, actor->name);
            effect->trace = ta_core_step_trace(core, instance);
            done = 0;
            if (final_state != TA_NIL) {
                ta_string_view fs =
                    ta_image_state_name(core->image, ta_actor_chart(system, actor), final_state);
                effect->auxiliary = ta_shared_string_new(core, fs.pointer, fs.length);
            }
        }
    }
    if (done && binding->vtable->release) binding->vtable->release(binding->datamodel, done);
    ta_system_reclaim(system, index, RECLAIM_ENDED);
}
