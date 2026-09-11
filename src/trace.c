#include "internal.h"

static void span_cleanup(void *argument)
{
    tai_span_t *span = (tai_span_t *)argument;
    ta_span_end_t end;
    if (span == NULL || !span->started || span->system == NULL ||
        span->system->config.trace.end == NULL) return;
    end.span = &span->value;
    end.now_ns = span->system->now_ns;
    end.status = span->status;
    span->system->config.trace.end(span->system->config.trace.user, &end);
    span->started = 0u;
}

static void trace_drop(ta_system_t *system)
{
    if (system != NULL) tai_saturate(&system->stats.dropped_trace_records);
}

static ta_status_t copy_context(const tai_value_scope_t *scope, const ta_trace_context_t *source,
                                const ta_trace_context_t **out)
{
    if (out == NULL) return TA_INVALID;
    *out = NULL;
    if (source == NULL) return TA_OK;
    return tai_trace_copy(scope, source, out);
}

static ta_status_t copy_start(const tai_value_scope_t *scope, const ta_span_start_t *source,
                              ta_span_start_t *out)
{
    ta_arena_t *arena = scope->arena;
    size_t i;
    ta_status_t status;
    ta_trace_context_t *links;
    const ta_trace_context_t *link;
    if (source == NULL || out == NULL) return TA_INVALID;
    *out = *source;
    out->arena = arena;
    status = tai_text_copy(arena, source->event_name, &out->event_name);
    if (status == TA_OK) status = tai_text_copy(arena, source->destination, &out->destination);
    if (status == TA_OK) status = tai_text_copy(arena, source->processor, &out->processor);
    if (status == TA_OK) status = copy_context(scope, source->parent, &out->parent);
    if (status != TA_OK) return status;
    out->links = NULL;
    if (source->link_count == 0u) return TA_OK;
    if (source->links == NULL || source->link_count > (size_t)-1 / sizeof(*links)) return TA_INVALID;
    status = tai_alloc(arena, source->link_count * sizeof(*links), (void **)&links);
    if (status != TA_OK) return status;
    for (i = 0u; i < source->link_count; ++i) {
        links[i] = source->links[i];
        status = tai_trace_copy(scope, &source->links[i], &link);
        if (status == TA_OK && link != NULL) links[i] = *link;
        if (status != TA_OK) return status;
    }
    out->links = links;
    return TA_OK;
}

void tai_span_begin(ta_system_t *system, const ta_span_start_t *start, tai_span_t **out)
{
    ta_arena_t *arena;
    tai_span_t *span;
    ta_span_start_t copied;
    ta_status_t status;
    if (out == NULL) return;
    *out = NULL;
    if (system == NULL || start == NULL || system->config.trace.start == NULL ||
        system->config.trace.end == NULL) return;
    status = tai_scope(system, TA_LIFETIME_TRACE, &arena);
    if (status != TA_OK) { trace_drop(system); return; }
    status = tai_alloc(arena, sizeof(*span), (void **)&span);
    if (status != TA_OK) { ta_arena_close(arena); trace_drop(system); return; }
    *span = (tai_span_t){0};
    span->system = system;
    span->arena = arena;
    span->status = TA_OK;
    status = ta_arena_defer(arena, span_cleanup, span);
    {
        tai_value_scope_t scope = {system, arena};
        if (status == TA_OK) status = copy_start(&scope, start, &copied);
    }
    if (status != TA_OK) { ta_arena_close(arena); trace_drop(system); return; }
    status = system->config.trace.start(system->config.trace.user, &copied, &span->value);
    if (status != TA_OK) { ta_arena_close(arena); trace_drop(system); return; }
    span->started = 1u;
    *out = span;
}

void tai_span_finish(tai_span_t *span, ta_status_t status)
{
    if (span == NULL || span->arena == NULL) return;
    span->status = status;
    ta_arena_close(span->arena);
}

void tai_span_error(tai_span_t *span, const ta_diagnostic_t *error)
{
    if (span == NULL || error == NULL || !span->started || span->system == NULL ||
        span->system->config.trace.error == NULL) return;
    span->system->config.trace.error(span->system->config.trace.user, span->value.handle, error);
}
