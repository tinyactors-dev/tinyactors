/* trace_step.c — what the kernel needs to trace (docs/adr/0006 in the repository
 * root, section 4): the context pool's references, the context a step's
 * events get, the fuel clock, and records about a step. It decides nothing:
 * the system sets the kinds, starts traces and samples them. */
#include "core/core_internal.h"
#include "base/memory.h"
#include "trace/record.h"

/* Dynamic strings in records are the kernel's slab strings. */
TA_STATIC_ASSERT(TA_STRING_REF_SLAB == TA_TRACE_STRING_DYNAMIC, slab_strings_are_dynamic_strings);

uint32_t ta_trace_context_new(ta_core *core, ta_trace_id trace, uint32_t flags,
                              ta_step_key cause) {
    if (!core->contexts) return TA_NIL;
    uint32_t context = ta_pool_alloc(core->contexts);
    if (context == TA_NIL) {
        core->contexts_dropped++;
        return TA_NIL;
    }
    ta_trace_entry *entry = ta_trace_entry_at(core, context);
    entry->trace = trace;
    entry->session = cause.session;
    entry->macrostep = cause.macrostep;
    entry->microstep = cause.microstep;
    entry->flags = flags;
    entry->refs = 1;
    entry->pad = 0;
    return context;
}

void ta_trace_context_retain(ta_core *core, uint32_t context) {
    if (context != TA_NIL) ta_trace_entry_at(core, context)->refs++;
}

void ta_trace_context_release(ta_core *core, uint32_t context) {
    if (context == TA_NIL) return;
    ta_trace_entry *entry = ta_trace_entry_at(core, context);
    TA_ASSERT(entry->refs > 0);
    if (--entry->refs == 0) ta_pool_free(core->contexts, context);
}

static ta_span_id trace_cause_span(const ta_trace_entry *entry) {
    ta_step_key cause = {entry->session, entry->macrostep, entry->microstep, 0};
    return ta_step_key_span_id(cause);
}

ta_trace_context ta_trace_context_value(const ta_core *core, uint32_t context) {
    ta_trace_context value;
    ta_memset(&value, 0, sizeof value);
    if (context == TA_NIL) return value;
    const ta_trace_entry *entry = ta_trace_entry_at(core, context);
    value.trace = entry->trace;
    value.parent = trace_cause_span(entry);
    value.flags = entry->flags;
    return value;
}

static uint64_t trace_session(const ta_core *core, const ta_instance *instance) {
    return core->serial_of ? core->serial_of(core->system, instance->actor) : 0;
}

uint32_t ta_core_step_context(ta_core *core, ta_instance *instance) {
    if (instance->trace == TA_NIL) return TA_NIL;
    ta_workspace *workspace =
        instance->workspace == TA_NIL
            ? (ta_workspace *)0
            : (ta_workspace *)ta_pool_at(core->workspaces, instance->workspace);
    if (workspace && workspace->trace != TA_NIL) { /* the step's events share one context */
        ta_trace_context_retain(core, workspace->trace);
        return workspace->trace;
    }
    const ta_trace_entry *macrostep = ta_trace_entry_at(core, instance->trace);
    ta_step_key cause = {trace_session(core, instance), instance->macrostep, instance->microstep,
                         0};
    uint32_t context = ta_trace_context_new(core, macrostep->trace, macrostep->flags, cause);
    if (context != TA_NIL && workspace) {
        workspace->trace = context;
        ta_trace_context_retain(core, context);
    }
    return context;
}

ta_trace_context ta_core_step_trace(const ta_core *core, const ta_instance *instance) {
    ta_trace_context value;
    ta_memset(&value, 0, sizeof value);
    if (instance->trace == TA_NIL) return value;
    const ta_trace_entry *macrostep = ta_trace_entry_at(core, instance->trace);
    ta_step_key step = {trace_session(core, instance), instance->macrostep, instance->microstep,
                        0};
    value.trace = macrostep->trace;
    value.parent = ta_step_key_span_id(step);
    value.flags = macrostep->flags;
    return value;
}

uint64_t ta_core_trace_fuel(const ta_core *core, const uint32_t *fuel) {
    if (!fuel) return core->trace_clock;
    return core->trace_clock + (uint64_t)(core->trace_step_fuel - *fuel) + core->overrun;
}

ta_trace_record *ta_core_trace_step(ta_core *core, ta_instance *instance, uint32_t kind,
                                    uint32_t size, uint32_t microstep, const uint32_t *fuel) {
    if (!(core->trace_kinds & TA_TRACE_BIT(kind)) || instance->trace == TA_NIL || !core->journal)
        return (ta_trace_record *)0;
    uint64_t at = ta_core_trace_fuel(core, fuel);
    ta_trace_record *record =
        ta_trace_record_reserve(core->journal, kind, size, at, core->trace_now);
    if (!record) return record;
    const ta_trace_entry *macrostep = ta_trace_entry_at(core, instance->trace);
    uint64_t session = trace_session(core, instance);
    record->flags = (uint16_t)macrostep->flags;
    record->actor = instance->actor;
    record->chart = instance->chart;
    record->key.session = session;
    record->key.macrostep = instance->macrostep;
    record->key.microstep = microstep;
    record->trace = macrostep->trace;
    record->span = ta_step_key_span_id(record->key);
    if (microstep) {
        ta_step_key outer = {session, instance->macrostep, 0, 0};
        record->parent = ta_step_key_span_id(outer);
    } else {
        record->parent = trace_cause_span(macrostep);
    }
    record->fuel = at;
    record->now = core->trace_now;
    return record;
}

static void trace_release_payload(ta_core *core, uint32_t datamodel, uint64_t payload) {
    if (payload && datamodel < core->datamodel_count && core->datamodels[datamodel].vtable->release)
        core->datamodels[datamodel].vtable->release(core->datamodels[datamodel].datamodel, payload);
}

void ta_core_trace_release(ta_core *core, const ta_trace_record *record) {
    const void *body = ta_trace_body(record);
    switch (record->kind) {
    case TA_TRACE_SPAWNED:
        ta_shared_string_release(core, ((const ta_trace_spawned *)body)->name);
        break;
    case TA_TRACE_MACROSTEP_BEGIN: {
        const ta_trace_macrostep_begin *begin = (const ta_trace_macrostep_begin *)body;
        ta_shared_string_release(core, begin->name);
        ta_shared_string_release(core, begin->sendid);
        ta_shared_string_release(core, begin->origin);
        trace_release_payload(core, begin->datamodel, begin->payload);
        break;
    }
    case TA_TRACE_RAISED:
        ta_shared_string_release(core, ((const ta_trace_raised *)body)->name);
        break;
    case TA_TRACE_SENT: {
        const ta_trace_sent *sent = (const ta_trace_sent *)body;
        ta_shared_string_release(core, sent->target);
        ta_shared_string_release(core, sent->name);
        ta_shared_string_release(core, sent->sendid);
        break;
    }
    case TA_TRACE_INVOKED: {
        const ta_trace_invoked *invoked = (const ta_trace_invoked *)body;
        ta_shared_string_release(core, invoked->type);
        ta_shared_string_release(core, invoked->source);
        ta_shared_string_release(core, invoked->invokeid);
        break;
    }
    case TA_TRACE_CANCELLED:
        ta_shared_string_release(core, ((const ta_trace_cancelled *)body)->sendid);
        break;
    case TA_TRACE_ASSIGN: {
        const ta_trace_assign *assign = (const ta_trace_assign *)body;
        trace_release_payload(core, assign->datamodel, assign->payload);
        break;
    }
    case TA_TRACE_LOG: {
        const ta_trace_log *log = (const ta_trace_log *)body;
        ta_shared_string_release(core, log->label);
        ta_shared_string_release(core, log->text);
        break;
    }
    default:
        break;
    }
}
