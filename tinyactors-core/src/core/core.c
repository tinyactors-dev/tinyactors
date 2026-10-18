/* core.c — the SCXML kernel as a resumable step function.
 *
 * Follows the W3C SCXML algorithm (Appendix D) with three changes of form,
 * not of semantics:
 *   1. every loop that can run long keeps its cursor in the instance or the
 *      workspace, so ta_core_step() can return TA_STEP_YIELD at any chunk
 *      boundary and resume exactly there;
 *   2. effects that leave the instance (send, invoke, cancel, log,
 *      autoforward, done) are returned to the caller instead of performed;
 *   3. recursion (entry-set computation, isInFinalState) is replaced by an
 *      explicit stack in the workspace or by interval/bitset scans.
 */
#include "core/core_internal.h"
#include "base/format.h"
#include "base/memory.h"
#include "trace/record.h"

struct ta_datamodel_environment {
    ta_core *core;
    ta_instance *instance;
    uint32_t charge;
};

enum { VM_DONE = 1000, KERNEL_CONTINUE = 1001 };

/* sub-phases of PHASE_MICROSTEP (workspace->subphase) */
enum {
    SUBPHASE_EXIT_COMPUTE = 0,
    SUBPHASE_EXIT,
    SUBPHASE_EXIT_CODE,
    SUBPHASE_EXIT_UNINVOKE,
    SUBPHASE_TRANSITIONS,
    SUBPHASE_ENTRY_COMPUTE,
    SUBPHASE_ENTER,
    SUBPHASE_ENTER_CODE,
    SUBPHASE_ENTER_DEFAULT,
    SUBPHASE_ENTER_HISTORY,
    SUBPHASE_ENTER_FINAL,
    SUBPHASE_RUN_CODE /* standalone code (top-level script, finalize) */
};

typedef struct kernel_context {
    ta_core *core;
    ta_instance *instance;
    const ta_chart_record *chart;
    const ta_state_record *states;
    const ta_transition_record *transitions;
    const ta_datamodel_binding *datamodel;
    ta_instance_layout instance_layout;
    ta_workspace_layout workspace_layout;
    ta_workspace *workspace;
    uint32_t *fuel;
    uint32_t state_count;
    struct ta_datamodel_environment environment;
} kernel_context;

/* ---- small helpers --------------------------------------------------------------- */

static void datamodel_release(struct kernel_context *k, ta_payload p);

static uint32_t *configuration_words(kernel_context *kernel) {
    return ta_instance_word(kernel->instance, kernel->instance_layout.config);
}
static uint32_t *workspace_at(kernel_context *kernel, uint32_t offset) {
    return (uint32_t *)(void *)kernel->workspace + offset;
}
static int leaf(kernel_context *kernel, uint32_t s) {
    return kernel->states[s].end == s + 1;
}
static int is_descendant(kernel_context *kernel, uint32_t s, uint32_t a) {
    return s > a && s < kernel->states[a].end;
}
static void burn(kernel_context *kernel, uint32_t n) {
    if (*kernel->fuel >= n) {
        *kernel->fuel -= n;
    } else {
        kernel->core->overrun += n - *kernel->fuel;
        *kernel->fuel = 0;
    }
}
/* Out of fuel at a chunk boundary inside a microstep? Uses the overrun
 * allowance first so short microsteps complete and yield with the workspace
 * released; only long ones suspend mid-way. */
static int exhausted(kernel_context *kernel) {
    if (*kernel->fuel) return 0;
    return kernel->core->overrun >= kernel->core->overrun_max;
}
static void charge(kernel_context *kernel) {
    if (kernel->environment.charge) burn(kernel, kernel->environment.charge);
    kernel->environment.charge = 0;
}

static void load_workspace(kernel_context *kernel) {
    kernel->workspace =
        kernel->instance->workspace == TA_NIL
            ? (ta_workspace *)0
            : (ta_workspace *)ta_pool_at(kernel->core->workspaces, kernel->instance->workspace);
}

static int acquire_workspace(kernel_context *kernel) {
    if (kernel->instance->workspace == TA_NIL) {
        /* none free: the actor waits for one (no refusal to mark) */
        if (!ta_pool_free_count(kernel->core->workspaces)) return 0;
        uint32_t i = ta_pool_alloc(kernel->core->workspaces);
        kernel->instance->workspace = i;
        load_workspace(kernel);
        ta_memset(kernel->workspace, 0, sizeof *kernel->workspace);
        kernel->workspace->instruction = TA_NIL;
        kernel->workspace->trace = TA_NIL;
    }
    load_workspace(kernel);
    return 1;
}

static void release_workspace(kernel_context *kernel) {
    if (kernel->instance->workspace == TA_NIL) return;
    ta_workspace *workspace = kernel->workspace;
    datamodel_release(kernel, workspace->done_payload);
    workspace->done_payload = 0;
    ta_foreach_frame *frame =
        (ta_foreach_frame *)(void *)workspace_at(kernel, kernel->workspace_layout.foreach_frames);
    while (workspace->foreach_top) {
        workspace->foreach_top--;
        if (kernel->datamodel->vtable->foreach_end)
            kernel->datamodel->vtable->foreach_end(
                &(ta_datamodel_call){kernel->datamodel->datamodel, kernel->instance->datamodel, 0},
                &(ta_datamodel_loop){frame[workspace->foreach_top].iteration, 0, 0, 0, 0});
    }
    ta_trace_context_release(kernel->core, workspace->trace);
    ta_pool_free(kernel->core->workspaces, kernel->instance->workspace);
    kernel->instance->workspace = TA_NIL;
    kernel->workspace = (ta_workspace *)0;
}

static void internal_queue_push(kernel_context *kernel, uint32_t event_index) {
    ta_event *event = ta_event_at(kernel->core, event_index);
    event->next = TA_NIL;
    if (kernel->instance->internal_queue_tail == TA_NIL)
        kernel->instance->internal_queue_head = event_index;
    else
        ta_event_at(kernel->core, kernel->instance->internal_queue_tail)->next = event_index;
    kernel->instance->internal_queue_tail = event_index;
}

static uint32_t internal_queue_pop(kernel_context *kernel) {
    uint32_t event = kernel->instance->internal_queue_head;
    if (event == TA_NIL) return TA_NIL;
    kernel->instance->internal_queue_head = ta_event_at(kernel->core, event)->next;
    if (kernel->instance->internal_queue_head == TA_NIL)
        kernel->instance->internal_queue_tail = TA_NIL;
    return event;
}

/* Count a platform error raised in an actor of `chart` (tinyactors/stats.h),
 * whether or not its event finds room: error.execution, error.communication,
 * error.platform and its descendants. */
static void count_error(ta_core *core, uint32_t chart, const char *name) {
    if (!core->chart_stats || name[0] != 'e' || name[5] != '.') return;
    ta_chart_stats *stats = &core->chart_stats[chart];
    if (name[6] == 'e')
        stats->errors_execution++;
    else if (name[6] == 'c')
        stats->errors_communication++;
    else if (name[6] == 'p')
        stats->errors_platform++;
}

static uint32_t raise_platform(ta_core *core, ta_instance *instance, const char *name,
                               uint32_t sendid) {
    count_error(core, instance->chart, name);
    uint32_t event_index = ta_event_new(core, 1);
    if (event_index == TA_NIL) {
        core->dropped_errors++;
        core->events->refused = 0; /* dropped, and the run goes on: nothing to grow */
        return TA_NIL;
    }
    ta_event *event = ta_event_at(core, event_index);
    uint32_t length = 0;
    while (name[length])
        length++;
    if (ta_event_set_name(core, event, name, length) != TA_OK) {
        ta_event_release(core, event_index);
        core->dropped_errors++;
        core->slab->refused = 0; /* dropped, and the run goes on: nothing to grow */
        return TA_NIL;
    }
    event->type = TA_EVENT_TYPE_PLATFORM;
    event->trace = ta_core_step_context(core, instance);
    if (sendid != TA_NIL) {
        ta_shared_string_retain(core, sendid);
        event->sendid = sendid;
    }
    event->next = TA_NIL;
    if (instance->internal_queue_tail == TA_NIL)
        instance->internal_queue_head = event_index;
    else
        ta_event_at(core, instance->internal_queue_tail)->next = event_index;
    instance->internal_queue_tail = event_index;
    return event_index;
}

static void trace_action_failed(kernel_context *kernel);

static void error_exec(kernel_context *kernel) {
    trace_action_failed(kernel);
    raise_platform(kernel->core, kernel->instance, "error.execution", TA_NIL);
}

static void init_k(kernel_context *kernel, ta_core *core, ta_instance *instance, uint32_t *fuel) {
    kernel->core = core;
    kernel->instance = instance;
    kernel->chart = &core->image_view.charts[instance->chart];
    kernel->states = ta_chart_states(&core->image_view, kernel->chart);
    kernel->transitions = ta_chart_transitions(&core->image_view, kernel->chart);
    kernel->datamodel = ta_core_datamodel(core, instance->chart);
    kernel->instance_layout = ta_instance_layout_of(kernel->chart);
    kernel->workspace_layout = ta_workspace_layout_of(kernel->chart);
    kernel->fuel = fuel;
    kernel->state_count = kernel->chart->state_count;
    kernel->environment.core = core;
    kernel->environment.instance = instance;
    kernel->environment.charge = 0;
    load_workspace(kernel);
}

/* ---- tracing (docs/adr/0006, section 4): facts, written when asked for --------------- */

/* A record about the current step (the microstep, or with `in_macrostep` the
 * macrostep), its header filled, or NULL. */
static ta_trace_record *trace_step(kernel_context *kernel, uint32_t kind, uint32_t size,
                                   int in_macrostep) {
    return ta_core_trace_step(kernel->core, kernel->instance, kind, size,
                              in_macrostep ? 0 : kernel->instance->microstep, kernel->fuel);
}

static void trace_action_failed(kernel_context *kernel) {
    ta_trace_record *record =
        trace_step(kernel, TA_TRACE_ACTION_FAILED,
                   (uint32_t)(sizeof *record + sizeof(ta_trace_action_failed)), 0);
    if (!record) return;
    ta_trace_action_failed *failed = (ta_trace_action_failed *)ta_trace_record_body(record);
    failed->opcode = kernel->workspace && kernel->workspace->vm_active &&
                             kernel->workspace->instruction != TA_NIL
                         ? kernel->core->image_view.code[kernel->workspace->instruction]
                         : TA_NIL;
    failed->expression = TA_NIL;
    ta_trace_record_commit(kernel->core->journal, record);
}

static void trace_condition(kernel_context *kernel, uint32_t transition, int result) {
    ta_trace_record *record = trace_step(
        kernel, TA_TRACE_CONDITION, (uint32_t)(sizeof *record + sizeof(ta_trace_condition)), 1);
    if (!record) return;
    ta_trace_condition *condition = (ta_trace_condition *)ta_trace_record_body(record);
    condition->transition = transition;
    condition->result = result ? 1u : 0u;
    ta_trace_record_commit(kernel->core->journal, record);
}

/* The configuration after a macrostep, its root left out. The macrostep's
 * context is let go, unless the session ended: then the system reports the
 * end (done.invoke, the DONE effect, `finished`) under it, and it goes with
 * the instance. */
static void trace_macrostep_end(kernel_context *kernel, int session_ended) {
    ta_core *core = kernel->core;
    if (kernel->instance->trace == TA_NIL) return;
    if (ta_core_kinds_of(core, kernel->instance) & TA_TRACE_BIT(TA_TRACE_MACROSTEP_END)) {
        const uint32_t *config = configuration_words(kernel);
        uint32_t count = ta_bitset_count(config, kernel->instance_layout.state_words);
        count -= ta_bitset_test(config, 0) ? 1u : 0u;
        ta_trace_record *record =
            trace_step(kernel, TA_TRACE_MACROSTEP_END,
                       (uint32_t)(sizeof *record + sizeof(ta_trace_macrostep_end) + 4u * count), 1);
        if (record) {
            ta_trace_macrostep_end *end = (ta_trace_macrostep_end *)ta_trace_record_body(record);
            end->state_count = count;
            uint32_t *states = (uint32_t *)(void *)(end + 1), n = 0;
            for (uint32_t s = ta_bitset_next(config, kernel->state_count, 1); s != TA_NIL;
                 s = ta_bitset_next(config, kernel->state_count, s + 1))
                states[n++] = s;
            ta_trace_record_commit(core->journal, record);
        }
    }
    if (session_ended) return;
    ta_trace_context_release(core, kernel->instance->trace);
    kernel->instance->trace = TA_NIL;
}

/* A microstep, at its end: its transitions, the states it exited (in exit
 * order) and entered (in entry order). */
static void trace_microstep(kernel_context *kernel) {
    if (!(ta_core_kinds_of(kernel->core, kernel->instance) & TA_TRACE_BIT(TA_TRACE_MICROSTEP)))
        return;
    ta_workspace *workspace = kernel->workspace;
    const uint32_t *exit = workspace_at(kernel, kernel->workspace_layout.exit),
                   *entry = workspace_at(kernel, kernel->workspace_layout.entry),
                   *selected = workspace_at(kernel, kernel->workspace_layout.selected);
    uint32_t exited = ta_bitset_count(exit, kernel->workspace_layout.state_words),
             entered = ta_bitset_count(entry, kernel->workspace_layout.state_words);
    ta_trace_record *record =
        trace_step(kernel, TA_TRACE_MICROSTEP,
                   (uint32_t)(sizeof *record + sizeof(ta_trace_microstep) +
                              4u * (workspace->selected_count + exited + entered)),
                   0);
    if (!record) return;
    ta_trace_microstep *micro = (ta_trace_microstep *)ta_trace_record_body(record);
    micro->begin_fuel = workspace->trace_fuel;
    micro->name = workspace->trace_event == TA_NIL
                      ? TA_NIL
                      : ta_event_at(kernel->core, workspace->trace_event)->name;
    ta_shared_string_retain(kernel->core, micro->name);
    micro->transition_count = workspace->selected_count;
    micro->exited_count = exited;
    micro->entered_count = entered;
    uint32_t *list = (uint32_t *)(void *)(micro + 1);
    for (uint32_t i = 0; i < workspace->selected_count; i++)
        *list++ = selected[i];
    for (uint32_t s = ta_bitset_previous(exit, kernel->state_count - 1); s != TA_NIL && s != 0;
         s = ta_bitset_previous(exit, s - 1))
        *list++ = s;
    for (uint32_t s = ta_bitset_next(entry, kernel->state_count, 0); s != TA_NIL;
         s = ta_bitset_next(entry, kernel->state_count, s + 1))
        *list++ = s;
    ta_trace_record_commit(kernel->core->journal, record);
}

/* The event a macrostep takes, or the bootstrap (event TA_NIL). */
static void trace_macrostep_begin(ta_core *core, ta_instance *instance, uint32_t event_index,
                                  const uint32_t *fuel) {
    ta_trace_record *record =
        ta_core_trace_step(core, instance, TA_TRACE_MACROSTEP_BEGIN,
                           (uint32_t)(sizeof *record + sizeof(ta_trace_macrostep_begin)), 0, fuel);
    if (!record) return;
    ta_trace_macrostep_begin *begin = (ta_trace_macrostep_begin *)ta_trace_record_body(record);
    const ta_trace_entry *context = ta_trace_entry_at(core, instance->trace);
    begin->cause.session = context->session;
    begin->cause.macrostep = context->macrostep;
    begin->cause.microstep = context->microstep;
    begin->name = begin->sendid = begin->origin = begin->datamodel = TA_NIL;
    begin->actor_name = core->name_of ? core->name_of(core->system, instance->actor) : TA_NIL;
    ta_shared_string_retain(core, begin->actor_name);
    if (event_index != TA_NIL) {
        const ta_event *event = ta_event_at(core, event_index);
        begin->name = event->name;
        begin->name_id = event->name_id;
        begin->type = event->type;
        begin->sendid = event->sendid;
        begin->origin = event->origin_string;
        begin->origin_session =
            event->origin && core->serial_of ? core->serial_of(core->system, event->origin) : 0;
        ta_shared_string_retain(core, begin->name);
        ta_shared_string_retain(core, begin->sendid);
        ta_shared_string_retain(core, begin->origin);
        if ((ta_core_kinds_of(core, instance) & TA_TRACE_VALUES) && event->payload &&
            event->datamodel < core->datamodel_count) {
            const ta_datamodel_binding *binding = &core->datamodels[event->datamodel];
            if (binding->vtable->retain) {
                binding->vtable->retain(binding->datamodel, event->payload);
                begin->payload = event->payload;
                begin->datamodel = event->datamodel;
            }
        }
    }
    ta_trace_record_commit(core->journal, record);
}

/* A new macrostep: count it, and take the context of what started it. */
static void begin_macrostep(ta_core *core, ta_instance *instance, uint32_t event_index,
                            const uint32_t *fuel) {
    instance->macrostep++;
    instance->microstep = 0;
    core->span_actor = TA_NIL; /* the span cache (ta_core_trace_step) */
    ta_trace_context_release(core, instance->trace);
    instance->trace = TA_NIL;
    if (event_index != TA_NIL) {
        instance->trace = ta_event_at(core, event_index)->trace;
        ta_trace_context_retain(core, instance->trace);
    }
    trace_macrostep_begin(core, instance, event_index, fuel);
}

/* ---- datamodel wrappers ------------------------------------------------------------ */

static int datamodel_condition(kernel_context *kernel, uint32_t e) {
    int v = 0;
    ta_status status =
        kernel->datamodel->vtable->eval_condition
            ? kernel->datamodel->vtable->eval_condition(
                  &(ta_datamodel_call){kernel->datamodel->datamodel, kernel->instance->datamodel,
                                       &kernel->environment},
                  e, &v)
            : TA_ERROR_EXECUTION;
    charge(kernel);
    if (status != TA_OK) {
        error_exec(kernel);
        return 0;
    }
    return v != 0;
}

static ta_status datamodel_string(kernel_context *kernel, uint32_t e, ta_string_view *out) {
    out->pointer = (const char *)0;
    out->length = 0;
    ta_status status =
        kernel->datamodel->vtable->eval_string
            ? kernel->datamodel->vtable->eval_string(
                  &(ta_datamodel_call){kernel->datamodel->datamodel, kernel->instance->datamodel,
                                       &kernel->environment},
                  e, out)
            : TA_ERROR_EXECUTION;
    charge(kernel);
    /* longer than the system keeps: no growth makes room for it */
    if (status == TA_OK && out->length > TA_SYSTEM_MAX_STRING) status = TA_ERROR_EXECUTION;
    return status;
}

static ta_status datamodel_capture(kernel_context *kernel, uint32_t capacity, uint32_t kind,
                                   ta_payload *out) {
    *out = 0;
    if (capacity == TA_NIL) return TA_OK;
    const ta_capture_record *capture = &kernel->core->image_view.captures[capacity];
    ta_datamodel_capture capture_spec;
    capture_spec.kind = kind;
    capture_spec.namelist_count = capture->namelist_count;
    capture_spec.param_count = capture->param_count;
    capture_spec.content = capture->content;
    capture_spec.namelist = kernel->core->image_view.u32 + capture->namelist_offset;
    capture_spec.params = kernel->core->image_view.u32 + capture->params_offset;
    ta_status status =
        kernel->datamodel->vtable->capture
            ? kernel->datamodel->vtable->capture(&(ta_datamodel_call){kernel->datamodel->datamodel,
                                                                      kernel->instance->datamodel,
                                                                      &kernel->environment},
                                                 &capture_spec, out)
            : TA_ERROR_EXECUTION;
    charge(kernel);
    return status;
}

static void datamodel_release(struct kernel_context *kernel, ta_payload payload) {
    if (payload && kernel->datamodel->vtable->release)
        kernel->datamodel->vtable->release(kernel->datamodel->datamodel, payload);
}

static void init_data(kernel_context *kernel, uint32_t d) {
    ta_status status =
        kernel->datamodel->vtable->init_data
            ? kernel->datamodel->vtable->init_data(
                  &(ta_datamodel_call){kernel->datamodel->datamodel, kernel->instance->datamodel,
                                       &kernel->environment},
                  d)
            : TA_OK;
    charge(kernel);
    if (status != TA_OK) error_exec(kernel);
}

/* ---- _event ---------------------------------------------------------------------- */

static void bind_event(ta_core *core, ta_instance *instance, uint32_t event) {
    if (instance->current_event != TA_NIL) ta_event_release(core, instance->current_event);
    instance->current_event = event;
    instance->flags &= (uint8_t)~INSTANCE_FLAG_DETACHED;
    const ta_datamodel_binding *binding = ta_core_datamodel(core, instance->chart);
    if (binding->vtable->bind_event) {
        ta_event_view view;
        struct ta_datamodel_environment environment = {core, instance, 0};
        ta_event_view_of(core, event, &view);
        (void)binding->vtable->bind_event(
            &(ta_datamodel_call){binding->datamodel, instance->datamodel, &environment}, &view);
    }
}

static void maybe_detach(kernel_context *kernel) {
    if (kernel->instance->current_event == TA_NIL ||
        !(kernel->datamodel->vtable->flags & TA_DATAMODEL_EVENT_DETACHABLE))
        return;
    if (kernel->datamodel->vtable->detach_event)
        kernel->datamodel->vtable->detach_event(
            &(ta_datamodel_call){kernel->datamodel->datamodel, kernel->instance->datamodel, 0});
    ta_event_release(kernel->core, kernel->instance->current_event);
    kernel->instance->current_event = TA_NIL;
    kernel->instance->flags |= INSTANCE_FLAG_DETACHED;
}

/* ---- strings handed to the caller in core->out are owned by out and released at the
 * start of the next step (no-op for image strings). -------------------------------- */

static void release_temporaries(ta_core *core) {
    ta_shared_string_release(core, core->out.target);
    ta_shared_string_release(core, core->out.source);
    ta_shared_string_release(core, core->out.type);
    ta_shared_string_release(core, core->out.sendid);
    core->out.target = core->out.source = core->out.type = core->out.sendid = TA_NIL;
    core->out.label = TA_NIL;
}

/* ---- history ----------------------------------------------------------------------- */

static uint32_t *history_words(kernel_context *kernel, uint32_t h) {
    return ta_instance_word(kernel->instance,
                            kernel->instance_layout.history + kernel->states[h].history_offset);
}
static int history_recorded(kernel_context *kernel, uint32_t h) {
    return ta_bitset_test(
        ta_instance_word(kernel->instance, kernel->instance_layout.history_recorded), h);
}

/* iterate states stored in history h: bit b <-> state parent+1+b */
static uint32_t history_next(kernel_context *kernel, uint32_t h, uint32_t from_state) {
    uint32_t p = kernel->states[h].parent, nbits = kernel->states[p].end - p - 1;
    uint32_t b = from_state <= p ? 0 : from_state - p - 1;
    uint32_t r = ta_bitset_next(history_words(kernel, h), nbits, b);
    return r == TA_NIL ? TA_NIL : p + 1 + r;
}

static void record_history(kernel_context *kernel, uint32_t s) {
    const uint32_t *config = configuration_words(kernel);
    for (uint32_t h = s + 1; h < kernel->states[s].end; h = kernel->states[h].end) {
        uint8_t kind = kernel->states[h].kind;
        if (kind < TA_STATE_HISTORY_SHALLOW) continue;
        uint32_t nbits = kernel->states[s].end - s - 1;
        uint32_t *hv = history_words(kernel, h);
        ta_bitset_zero(hv, ta_bitset_words(nbits));
        if (kind == TA_STATE_HISTORY_DEEP) {
            for (uint32_t x = ta_bitset_next(config, kernel->states[s].end, s + 1); x != TA_NIL;
                 x = ta_bitset_next(config, kernel->states[s].end, x + 1))
                if (leaf(kernel, x)) ta_bitset_set(hv, x - s - 1);
        } else {
            for (uint32_t c = s + 1; c < kernel->states[s].end; c = kernel->states[c].end)
                if (ta_bitset_test(config, c)) ta_bitset_set(hv, c - s - 1);
        }
        ta_bitset_set(ta_instance_word(kernel->instance, kernel->instance_layout.history_recorded),
                      h);
    }
}

/* ---- transition domains and selection ----------------------------------------------- */

/* All effective targets of t are proper descendants of a? */
static int all_effective_targets_descend(kernel_context *kernel,
                                         const ta_transition_record *transition, uint32_t a) {
    const uint32_t *tg = kernel->core->image_view.u32 + transition->targets_offset;
    for (uint32_t i = 0; i < transition->target_count; i++) {
        uint32_t s = tg[i];
        if (kernel->states[s].kind >= TA_STATE_HISTORY_SHALLOW) {
            if (history_recorded(kernel, s)) {
                for (uint32_t x = history_next(kernel, s, 0); x != TA_NIL;
                     x = history_next(kernel, s, x + 1))
                    if (!is_descendant(kernel, x, a)) return 0;
            } else if (kernel->states[s].initial != TA_NIL) {
                const ta_transition_record *d = &kernel->transitions[kernel->states[s].initial];
                for (uint32_t j = 0; j < d->target_count; j++)
                    if (!is_descendant(kernel, kernel->core->image_view.u32[d->targets_offset + j],
                                       a))
                        return 0;
            }
        } else if (!is_descendant(kernel, s, a)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t domain_of(kernel_context *kernel, uint32_t ti) {
    const ta_transition_record *transition = &kernel->transitions[ti];
    if (!transition->target_count) return TA_NIL;
    if (!(transition->flags & TA_TRANSITION_FLAG_HISTORY_TARGET) ||
        (transition->flags & TA_TRANSITION_FLAG_INITIAL))
        return transition->domain;
    uint32_t source = transition->source;
    if ((transition->flags & TA_TRANSITION_FLAG_INTERNAL) &&
        kernel->states[source].kind == TA_STATE_COMPOUND &&
        all_effective_targets_descend(kernel, transition, source))
        return source;
    for (uint32_t a = kernel->states[source].parent; a != TA_NIL; a = kernel->states[a].parent)
        if (kernel->states[a].kind == TA_STATE_COMPOUND &&
            all_effective_targets_descend(kernel, transition, a))
            return a;
    return 0;
}

static int nested(kernel_context *kernel, uint32_t a, uint32_t b) {
    if (a == TA_NIL || b == TA_NIL) return 0;
    return (a <= b && b < kernel->states[a].end) || (b <= a && a < kernel->states[b].end);
}

static void remove_conflicts(kernel_context *kernel) {
    uint32_t *sel = workspace_at(kernel, kernel->workspace_layout.selected),
             *dom = workspace_at(kernel, kernel->workspace_layout.domains);
    uint32_t n = kernel->workspace->selected_count, out = 0;
    for (uint32_t i = 0; i < n; i++)
        dom[i] = domain_of(kernel, sel[i]);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t1 = sel[i], d1 = dom[i];
        int preempted = 0;
        for (uint32_t j = 0; j < out; j++) {
            if (!nested(kernel, d1, dom[j])) continue; /* exit sets intersect iff domains nest */
            if (is_descendant(kernel, kernel->transitions[t1].source,
                              kernel->transitions[sel[j] & ~TA_STRING_REF_SLAB].source))
                sel[j] |= TA_STRING_REF_SLAB; /* mark for removal (high bit) */
            else {
                preempted = 1;
                break;
            }
        }
        if (preempted) {
            for (uint32_t j = 0; j < out; j++)
                sel[j] &= ~TA_STRING_REF_SLAB;
            continue;
        }
        uint32_t w = 0;
        for (uint32_t j = 0; j < out; j++)
            if (!(sel[j] & TA_STRING_REF_SLAB)) {
                sel[w] = sel[j];
                dom[w] = dom[j];
                w++;
            }
        out = w;
        sel[out] = t1;
        dom[out] = d1;
        out++;
    }
    kernel->workspace->selected_count = out;
}

static void select_transitions(kernel_context *kernel, uint32_t name_id, int eventless) {
    uint32_t *sel = workspace_at(kernel, kernel->workspace_layout.selected),
             *transitions_seen = workspace_at(kernel, kernel->workspace_layout.transitions_seen);
    const uint32_t *config = configuration_words(kernel), *u32 = kernel->core->image_view.u32;
    uint16_t need =
        eventless ? TA_STATE_FLAG_HAS_EVENTLESS_TRANSITIONS : TA_STATE_FLAG_HAS_EVENT_TRANSITIONS;
    kernel->workspace->selected_count = 0;
    ta_bitset_zero(transitions_seen, kernel->workspace_layout.transition_words);
    for (uint32_t s = ta_bitset_next(config, kernel->state_count, 1); s != TA_NIL;
         s = ta_bitset_next(config, kernel->state_count, s + 1)) {
        if (!leaf(kernel, s)) continue;
        for (uint32_t a = s; a != 0 && a != TA_NIL; a = kernel->states[a].parent) {
            if (!(kernel->states[a].flags & need)) continue;
            for (uint32_t ti = kernel->states[a].first_transition;
                 ti < kernel->states[a].first_transition + kernel->states[a].transition_count;
                 ti++) {
                const ta_transition_record *transition = &kernel->transitions[ti];
                if (eventless) {
                    if (transition->event_count) continue;
                } else {
                    uint32_t m = 0;
                    for (; m < transition->event_count; m++)
                        if (u32[transition->events_offset + 2 * m] <= name_id &&
                            name_id < u32[transition->events_offset + 2 * m + 1])
                            break;
                    if (m == transition->event_count) continue;
                }
                burn(kernel, 1);
                if (transition->condition != TA_NIL) {
                    int result = datamodel_condition(kernel, transition->condition);
                    trace_condition(kernel, ti, result);
                    if (!result) continue;
                }
                if (!ta_bitset_test(transitions_seen, ti)) {
                    ta_bitset_set(transitions_seen, ti);
                    sel[kernel->workspace->selected_count++] = ti;
                }
                goto next_atomic;
            }
        }
    next_atomic:;
    }
    remove_conflicts(kernel);
}

/* ---- entry set (iterative addDescendantStatesToEnter / addAncestorStatesToEnter) ----- */

#define ENTRY_DESCENDANTS  0u
#define ENTRY_ANCESTORS    1u
#define ENTRY_PARALLEL     2u
#define ENTRY_STATE(x)     ((x) & 0x3FFFFFFFu)
#define ENTRY_OPERATION(x) ((x) >> 30)

static int push(kernel_context *kernel, uint32_t x) {
    if (kernel->workspace->stack_top >= kernel->workspace_layout.stack_capacity) return 0;
    workspace_at(kernel, kernel->workspace_layout.stack)[kernel->workspace->stack_top++] = x;
    return 1;
}
static int push_anc(kernel_context *kernel, uint32_t s, uint32_t anc) {
    return push(kernel, anc) && push(kernel, (ENTRY_ANCESTORS << 30) | s);
}
static void reverse_from(kernel_context *kernel, uint32_t from) {
    uint32_t *stack = workspace_at(kernel, kernel->workspace_layout.stack);
    for (uint32_t a = from, b = kernel->workspace->stack_top; a + 1 < b; a++, b--) {
        uint32_t t = stack[a];
        stack[a] = stack[b - 1];
        stack[b - 1] = t;
    }
}

/* Push DESC for each target of transition `ti` (first target on top) after ANC
 * tasks for (target, anc). */
static int push_transition_targets(kernel_context *kernel, uint32_t ti, uint32_t anc) {
    const ta_transition_record *transition = &kernel->transitions[ti];
    const uint32_t *tg = kernel->core->image_view.u32 + transition->targets_offset;
    for (uint32_t i = 0; i < transition->target_count; i++)
        if (!push_anc(kernel, tg[i], anc)) return 0;
    uint32_t from = kernel->workspace->stack_top;
    for (uint32_t i = 0; i < transition->target_count; i++)
        if (!push(kernel, (ENTRY_DESCENDANTS << 30) | tg[i])) return 0;
    reverse_from(kernel, from);
    return 1;
}

static int push_children_par(kernel_context *kernel, uint32_t s) {
    uint32_t from = kernel->workspace->stack_top;
    for (uint32_t c = s + 1; c < kernel->states[s].end; c = kernel->states[c].end)
        if (kernel->states[c].kind < TA_STATE_HISTORY_SHALLOW &&
            !push(kernel, (ENTRY_PARALLEL << 30) | c))
            return 0;
    reverse_from(kernel, from);
    return 1;
}

static int drain_entry(kernel_context *kernel) {
    uint32_t *entry = workspace_at(kernel, kernel->workspace_layout.entry),
             *default_entry = workspace_at(kernel, kernel->workspace_layout.default_entry),
             *history_default_entry =
                 workspace_at(kernel, kernel->workspace_layout.history_default_entry);
    uint32_t *stk = workspace_at(kernel, kernel->workspace_layout.stack);
    while (kernel->workspace->stack_top) {
        uint32_t x = stk[--kernel->workspace->stack_top], s = ENTRY_STATE(x);
        switch (ENTRY_OPERATION(x)) {
        case ENTRY_DESCENDANTS:
            if (kernel->states[s].kind >= TA_STATE_HISTORY_SHALLOW) {
                uint32_t p = kernel->states[s].parent;
                if (history_recorded(kernel, s)) {
                    for (uint32_t h = history_next(kernel, s, 0); h != TA_NIL;
                         h = history_next(kernel, s, h + 1))
                        if (!push_anc(kernel, h, p)) return 0;
                    uint32_t from = kernel->workspace->stack_top;
                    for (uint32_t h = history_next(kernel, s, 0); h != TA_NIL;
                         h = history_next(kernel, s, h + 1))
                        if (!push(kernel, (ENTRY_DESCENDANTS << 30) | h)) return 0;
                    reverse_from(kernel, from);
                } else {
                    ta_bitset_set(history_default_entry, s);
                    if (kernel->states[s].initial != TA_NIL &&
                        !push_transition_targets(kernel, kernel->states[s].initial, p))
                        return 0;
                }
            } else {
                ta_bitset_set(entry, s);
                if (kernel->states[s].kind == TA_STATE_COMPOUND) {
                    ta_bitset_set(default_entry, s);
                    if (!push_transition_targets(kernel, kernel->states[s].initial, s)) return 0;
                } else if (kernel->states[s].kind == TA_STATE_PARALLEL) {
                    if (!push_children_par(kernel, s)) return 0;
                }
            }
            break;
        case ENTRY_PARALLEL:
            if (!ta_bitset_any_in_range(entry, s + 1, kernel->states[s].end) &&
                !push(kernel, (ENTRY_DESCENDANTS << 30) | s))
                return 0;
            break;
        case ENTRY_ANCESTORS: {
            uint32_t anc = stk[--kernel->workspace->stack_top];
            uint32_t a = kernel->states[s].parent;
            if (a == anc || a == TA_NIL || a == 0) break;
            ta_bitset_set(entry, a);
            if (!push_anc(kernel, a, anc)) return 0;
            if (kernel->states[a].kind == TA_STATE_PARALLEL && !push_children_par(kernel, a))
                return 0;
            break;
        }
        }
    }
    return 1;
}

static int compute_entry(kernel_context *kernel) {
    ta_bitset_zero(workspace_at(kernel, kernel->workspace_layout.entry),
                   kernel->workspace_layout.state_words);
    ta_bitset_zero(workspace_at(kernel, kernel->workspace_layout.default_entry),
                   kernel->workspace_layout.state_words);
    ta_bitset_zero(workspace_at(kernel, kernel->workspace_layout.history_default_entry),
                   kernel->workspace_layout.state_words);
    const uint32_t *sel = workspace_at(kernel, kernel->workspace_layout.selected),
                   *dom = workspace_at(kernel, kernel->workspace_layout.domains);
    for (uint32_t i = 0; i < kernel->workspace->selected_count; i++) {
        const ta_transition_record *transition = &kernel->transitions[sel[i]];
        if (!transition->target_count) continue;
        kernel->workspace->stack_top = 0;
        /* spec: addDescendant(target) for targets; addAncestor(eff, domain) for
         * effective targets. Effective targets of a history target are its
         * stored value or its default targets. */
        const uint32_t *tg = kernel->core->image_view.u32 + transition->targets_offset;
        for (uint32_t j = 0; j < transition->target_count; j++) {
            uint32_t s = tg[j];
            if (kernel->states[s].kind >= TA_STATE_HISTORY_SHALLOW) {
                if (history_recorded(kernel, s)) {
                    for (uint32_t h = history_next(kernel, s, 0); h != TA_NIL;
                         h = history_next(kernel, s, h + 1))
                        if (!push_anc(kernel, h, dom[i])) return 0;
                } else if (kernel->states[s].initial != TA_NIL) {
                    const ta_transition_record *d = &kernel->transitions[kernel->states[s].initial];
                    for (uint32_t m = 0; m < d->target_count; m++)
                        if (!push_anc(kernel, kernel->core->image_view.u32[d->targets_offset + m],
                                      dom[i]))
                            return 0;
                }
            } else if (!push_anc(kernel, s, dom[i])) {
                return 0;
            }
        }
        uint32_t from = kernel->workspace->stack_top;
        for (uint32_t j = 0; j < transition->target_count; j++)
            if (!push(kernel, (ENTRY_DESCENDANTS << 30) | tg[j])) return 0;
        reverse_from(kernel, from);
        if (!drain_entry(kernel)) return 0;
    }
    return 1;
}

/* isInFinalState for a parallel state p, without recursion: every active atomic
 * descendant must be a final state whose parent is compound and whose path up
 * to p consists only of parallel states. */
static int parallel_in_final(kernel_context *kernel, uint32_t p) {
    const uint32_t *config = configuration_words(kernel);
    int any = 0;
    for (uint32_t x = ta_bitset_next(config, kernel->states[p].end, p + 1); x != TA_NIL;
         x = ta_bitset_next(config, kernel->states[p].end, x + 1)) {
        if (!leaf(kernel, x)) continue;
        any = 1;
        uint32_t par = kernel->states[x].parent;
        if (kernel->states[x].kind != TA_STATE_FINAL ||
            kernel->states[par].kind != TA_STATE_COMPOUND)
            return 0;
        for (uint32_t a = kernel->states[par].parent; a != p; a = kernel->states[a].parent)
            if (kernel->states[a].kind != TA_STATE_PARALLEL) return 0;
    }
    return any;
}

/* ---- executable content VM -------------------------------------------------------- */

static ta_foreach_frame *current_foreach_frame(kernel_context *kernel) {
    return (ta_foreach_frame *)(void *)workspace_at(kernel,
                                                    kernel->workspace_layout.foreach_frames);
}

static void abort_block(kernel_context *kernel) {
    ta_foreach_frame *frame = current_foreach_frame(kernel);
    while (kernel->workspace->foreach_top) {
        kernel->workspace->foreach_top--;
        if (kernel->datamodel->vtable->foreach_end)
            kernel->datamodel->vtable->foreach_end(
                &(ta_datamodel_call){kernel->datamodel->datamodel, kernel->instance->datamodel, 0},
                &(ta_datamodel_loop){frame[kernel->workspace->foreach_top].iteration, 0, 0, 0, 0});
    }
    kernel->workspace->instruction = kernel->workspace->block_end;
}

static int str_is(ta_string_view s, const char *lit) {
    uint32_t n = 0;
    while (lit[n])
        n++;
    return s.length == n && ta_memcmp(s.pointer, lit, n) == 0;
}
static int str_starts(ta_string_view s, const char *lit) {
    uint32_t n = 0;
    while (lit[n])
        n++;
    return s.length >= n && ta_memcmp(s.pointer, lit, n) == 0;
}
/* RFC 3986 absolute URI: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
static int is_absolute_uri(ta_string_view s) {
    uint32_t i = 0;
    while (i < s.length) {
        char c = s.pointer[i];
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (c == ':') return i > 0;
        if (!alpha && (!i || !((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')))
            return 0;
        i++;
    }
    return 0;
}
static int is_scxml_type(ta_string_view t) {
    return t.length == 0 || str_is(t, "scxml") || str_is(t, "http://www.w3.org/TR/scxml/") ||
           str_is(t, "http://www.w3.org/TR/scxml") || str_is(t, TA_SCXML_IO_PROCESSOR);
}
static uint32_t find_io_processor(kernel_context *kernel, ta_string_view t) {
    for (uint32_t i = 0; i < kernel->core->io_processor_count; i++) {
        ta_string_view x = kernel->core->io_processors[i].type;
        if (x.length == t.length &&
            (t.length == 0 || ta_memcmp(x.pointer, t.pointer, t.length) == 0))
            return i;
    }
    return TA_NIL;
}

/* Find an active invoke by id string; returns slot or TA_NIL. */
static uint32_t find_invoke(kernel_context *kernel, ta_string_view id) {
    const uint32_t *child =
                       ta_instance_word(kernel->instance, kernel->instance_layout.invoke_children),
                   *ids = ta_instance_word(kernel->instance, kernel->instance_layout.invoke_ids);
    for (uint32_t s = 0; s < kernel->chart->invoke_count; s++)
        if (child[s] && ids[s] != TA_NIL && ta_shared_string_equal(kernel->core, ids[s], id))
            return s;
    return TA_NIL;
}

static uint32_t generate_id(kernel_context *kernel, const char *prefix, ta_string_view prefix2) {
    char *b = kernel->core->scratch;
    uint32_t n = 0;
    for (uint32_t i = 0; prefix[i] && n < 40; i++)
        b[n++] = prefix[i];
    for (uint32_t i = 0; i < prefix2.length && n < 100; i++)
        b[n++] = prefix2.pointer[i];
    b[n++] = '.';
    n +=
        ta_format_u64(b + n, kernel->instance->ids + 1u); /* unique within the session (2^32 ids) */
    uint32_t id = ta_shared_string_new(kernel->core, b, n);
    if (id != TA_NIL) kernel->instance->ids++;
    return id;
}

enum { SEND_OK = 0, SEND_ERROR_EXECUTION, SEND_ERROR_COMMUNICATION, SEND_NOMEM };

static void trace_sent(kernel_context *kernel, const ta_event *event, uint32_t target,
                       uint64_t delay) {
    ta_core *core = kernel->core;
    ta_trace_record *record =
        trace_step(kernel, TA_TRACE_SENT, (uint32_t)(sizeof *record + sizeof(ta_trace_sent)), 0);
    if (!record) return;
    ta_trace_sent *sent = (ta_trace_sent *)ta_trace_record_body(record);
    sent->target = target;
    sent->name = event->name;
    sent->name_id = event->name_id;
    sent->sendid = event->sendid;
    sent->delay = delay;
    sent->context = ta_trace_context_value(core, event->trace);
    ta_shared_string_retain(core, sent->target);
    ta_shared_string_retain(core, sent->name);
    ta_shared_string_retain(core, sent->sendid);
    ta_trace_record_commit(core->journal, record);
}

/* Execute <send>. Returns a step code, KERNEL_CONTINUE, or VM-level handling. */
static uint32_t opcode_send(kernel_context *kernel, uint32_t index) {
    ta_core *core = kernel->core;
    const ta_send_record *send = &core->image_view.sends[kernel->chart->first_send + index];
    uint32_t name_id = 0, name = TA_NIL, target = TA_NIL, sendid = TA_NIL;
    uint32_t route = TA_ROUTE_SELF, arg = 0, io_processor_index = TA_NIL;
    int internal = 0, status = SEND_OK;
    uint64_t delay = send->delay_ns;
    ta_payload payload = 0;
    ta_string_view s;

    /* event name */
    if (send->event != TA_NIL) {
        name_id = send->event;
        name = core->image_view.event_nodes[name_id].name;
    } else if (send->eventexpr != TA_NIL) {
        if (datamodel_string(kernel, send->eventexpr, &s) != TA_OK) {
            status = SEND_ERROR_EXECUTION;
            goto fail;
        }
        int exact = 0;
        name_id = ta_event_name_lookup(&core->image_view, s.pointer, s.length, &exact);
        name = exact ? core->image_view.event_nodes[name_id].name
                     : ta_shared_string_new(core, s.pointer, s.length);
        if (name == TA_NIL) {
            status = SEND_NOMEM;
            goto fail;
        }
    } else {
        name = core->image_view.event_nodes[0].name;
    }
    /* I/O processor type */
    if (send->type != TA_NIL) {
        io_processor_index =
            find_io_processor(kernel, ta_image_view_string(&core->image_view, send->type));
        if (io_processor_index == TA_NIL) {
            status = SEND_ERROR_EXECUTION;
            goto fail;
        }
    } else if (send->typeexpr != TA_NIL) {
        if (datamodel_string(kernel, send->typeexpr, &s) != TA_OK) {
            status = SEND_ERROR_EXECUTION;
            goto fail;
        }
        if (!is_scxml_type(s)) {
            io_processor_index = find_io_processor(kernel, s);
            if (io_processor_index == TA_NIL) {
                status = SEND_ERROR_EXECUTION;
                goto fail;
            }
        }
    }
    /* target */
    {
        uint32_t kind = send->target_kind;
        ta_string_view tstr = {(const char *)0, 0};
        if (kind == TA_SEND_TARGET_EXPRESSION) {
            if (datamodel_string(kernel, send->targetexpr, &s) != TA_OK) {
                status = SEND_ERROR_EXECUTION;
                goto fail;
            }
            target = ta_shared_string_new(core, s.pointer, s.length);
            if (target == TA_NIL) {
                status = SEND_NOMEM;
                goto fail;
            }
            tstr = ta_shared_string_get(core, target);
            kind = TA_SEND_TARGET_STRING;
        } else if (kind == TA_SEND_TARGET_STRING) {
            target = send->target;
            tstr = ta_image_view_string(&core->image_view, target);
        }
        if (io_processor_index != TA_NIL) {
            route = TA_ROUTE_EXTERNAL;
            arg = io_processor_index + 1; /* 0 is SCXML's (system.h) */
        } else if (kind == TA_SEND_TARGET_NONE) {
            route = TA_ROUTE_SELF;
        } else if (kind == TA_SEND_TARGET_INTERNAL ||
                   (kind == TA_SEND_TARGET_STRING && str_is(tstr, "#_internal"))) {
            internal = 1;
        } else if (kind == TA_SEND_TARGET_PARENT ||
                   (kind == TA_SEND_TARGET_STRING && str_is(tstr, "#_parent"))) {
            route = TA_ROUTE_PARENT;
        } else if (kind == TA_SEND_TARGET_INVOKE) {
            arg = ta_instance_word(kernel->instance,
                                   kernel->instance_layout.invoke_children)[send->target];
            route = TA_ROUTE_CHILD;
            if (!arg) status = SEND_ERROR_COMMUNICATION;
        } else if (str_starts(tstr, "#_scxml_")) {
            route = TA_ROUTE_SESSION;
        } else if (str_starts(tstr, "#_")) {
            ta_string_view id = {tstr.pointer + 2, tstr.length - 2};
            uint32_t slot = find_invoke(kernel, id);
            if (slot == TA_NIL)
                status = SEND_ERROR_COMMUNICATION;
            else {
                route = TA_ROUTE_CHILD;
                arg = ta_instance_word(kernel->instance,
                                       kernel->instance_layout.invoke_children)[slot];
            }
        } else {
            /* another system's session: out through the SCXML processor (the system's base) */
            if (!core->base.length || !is_absolute_uri(tstr)) {
                status = SEND_ERROR_EXECUTION; /* unsupported or invalid target (SCXML 6.2.4) */
            } else {
                ta_string_view base = core->base;
                ta_string_view rest = {tstr.pointer + base.length, tstr.length - base.length};
                if (tstr.length > base.length &&
                    ta_memcmp(tstr.pointer, base.pointer, base.length) == 0 &&
                    str_starts(rest, "#_scxml_")) { /* our own address: deliver locally */
                    uint32_t local = ta_shared_string_new(core, rest.pointer, rest.length);
                    if (TA_STRING_REF_IS_SLAB(target)) ta_shared_string_release(core, target);
                    target = local;
                    route = TA_ROUTE_SESSION;
                    if (target == TA_NIL) status = SEND_NOMEM;
                } else {
                    route = TA_ROUTE_EXTERNAL;
                    arg = 0; /* the SCXML processor */
                }
            }
        }
    }
    if (status != SEND_OK) goto fail;
    /* delay */
    if (send->delayexpr != TA_NIL) {
        if (datamodel_string(kernel, send->delayexpr, &s) != TA_OK ||
            !ta_core_parse_duration(s, &delay)) {
            status = SEND_ERROR_EXECUTION;
            goto fail;
        }
    }
    if (internal && delay) {
        status = SEND_ERROR_EXECUTION;
        goto fail;
    }
    /* no timer slot: do not execute the send; the host grows the pool and the
     * instruction is retried (like event exhaustion), so no send is lost */
    if (delay && core->timers && ta_pool_free_count(core->timers) == 0) {
        core->timers->refused = 1; /* what ta_system_exhausted names */
        status = SEND_NOMEM;
        goto fail;
    }
    /* send id */
    if (send->id != TA_NIL) {
        sendid = send->id;
    } else if (send->idlocation != TA_NIL) {
        ta_string_view none = {(const char *)0, 0};
        sendid = generate_id(kernel, "send", none);
        if (sendid == TA_NIL) {
            status = SEND_NOMEM;
            goto fail;
        }
        ta_string_view v = ta_shared_string_get(core, sendid);
        ta_status assign_status = kernel->datamodel->vtable->assign_string
                                      ? kernel->datamodel->vtable->assign_string(
                                            &(ta_datamodel_call){kernel->datamodel->datamodel,
                                                                 kernel->instance->datamodel, 0},
                                            send->idlocation, v)
                                      : TA_ERROR_EXECUTION;
        if (assign_status != TA_OK) {
            status = SEND_ERROR_EXECUTION;
            goto fail;
        }
    }
    /* payload */
    if (datamodel_capture(kernel, send->capture, TA_CAPTURE_SEND, &payload) != TA_OK) {
        status = SEND_ERROR_EXECUTION;
        goto fail;
    }
    {
        uint32_t event_index = ta_event_new(core, 0);
        if (event_index == TA_NIL) {
            status = SEND_NOMEM;
            goto fail;
        }
        ta_event *event = ta_event_at(core, event_index);
        event->type = internal ? TA_EVENT_TYPE_INTERNAL : TA_EVENT_TYPE_EXTERNAL;
        event->name_id = name_id;
        event->name = name;
        event->sendid = sendid;
        event->origin = internal ? 0 : kernel->instance->actor; /* #_internal: no origin */
        event->payload = payload;
        event->datamodel = core->chart_datamodel[kernel->instance->chart];
        event->trace = ta_core_step_context(core, kernel->instance);
        kernel->workspace->instruction += 2;
        trace_sent(kernel, event, target, delay);
        if (internal) {
            if (target != TA_NIL && TA_STRING_REF_IS_SLAB(target))
                ta_shared_string_release(core, target);
            internal_queue_push(kernel, event_index);
            return KERNEL_CONTINUE;
        }
        core->out.event = event_index;
        core->out.route = route;
        core->out.arg = arg;
        core->out.target = target; /* owned by out */
        core->out.delay_ns = delay;
        return TA_STEP_SEND;
    }
fail:
    datamodel_release(kernel, payload);
    if (TA_STRING_REF_IS_SLAB(name)) ta_shared_string_release(core, name);
    if (TA_STRING_REF_IS_SLAB(target)) ta_shared_string_release(core, target);
    if (status == SEND_NOMEM) {
        if (TA_STRING_REF_IS_SLAB(sendid)) ta_shared_string_release(core, sendid);
        if (send->id == TA_NIL && send->idlocation != TA_NIL && sendid != TA_NIL)
            kernel->instance->ids--; /* the retry generates the same id again */
        return TA_STEP_EXHAUSTED;    /* instruction unchanged: retried after growth */
    }
    raise_platform(core, kernel->instance,
                   status == SEND_ERROR_COMMUNICATION ? "error.communication" : "error.execution",
                   sendid);
    if (TA_STRING_REF_IS_SLAB(sendid)) ta_shared_string_release(core, sendid);
    if (status == SEND_ERROR_COMMUNICATION) {
        kernel->workspace->instruction += 2; /* communication errors do not abort the block */
        return KERNEL_CONTINUE;
    }
    abort_block(kernel);
    return KERNEL_CONTINUE;
}

static void trace_cancelled(kernel_context *kernel, uint32_t sendid) {
    ta_trace_record *record = trace_step(
        kernel, TA_TRACE_CANCELLED, (uint32_t)(sizeof *record + sizeof(ta_trace_cancelled)), 0);
    if (!record) return;
    ta_trace_cancelled *cancelled = (ta_trace_cancelled *)ta_trace_record_body(record);
    cancelled->sendid = sendid;
    ta_shared_string_retain(kernel->core, sendid);
    ta_trace_record_commit(kernel->core->journal, record);
}

/* An assignment; with TA_TRACE_VALUES, the value assigned, captured as a
 * payload. The capture is invisible to the chart: a failure leaves the value
 * out, and the fuel the datamodel reports for it is not charged. */
static void trace_assign(kernel_context *kernel, uint32_t location) {
    ta_core *core = kernel->core;
    ta_trace_record *record = trace_step(kernel, TA_TRACE_ASSIGN,
                                         (uint32_t)(sizeof *record + sizeof(ta_trace_assign)), 0);
    if (!record) return;
    ta_trace_assign *assign = (ta_trace_assign *)ta_trace_record_body(record);
    assign->location = location;
    assign->datamodel = TA_NIL;
    assign->payload = 0;
    if ((ta_core_kinds_of(kernel->core, kernel->instance) & TA_TRACE_VALUES) &&
        kernel->datamodel->vtable->capture) {
        ta_datamodel_capture capture;
        ta_memset(&capture, 0, sizeof capture);
        capture.kind = TA_CAPTURE_VALUE;
        capture.content = location;
        ta_payload value = 0;
        uint32_t charge_before = kernel->environment.charge;
        if (kernel->datamodel->vtable->capture(&(ta_datamodel_call){kernel->datamodel->datamodel,
                                                                    kernel->instance->datamodel,
                                                                    &kernel->environment},
                                               &capture, &value) == TA_OK &&
            value) {
            assign->payload = value;
            assign->datamodel = core->chart_datamodel[kernel->instance->chart];
        }
        kernel->environment.charge = charge_before;
    }
    ta_trace_record_commit(core->journal, record);
}

/* Run code from workspace->instruction. Returns VM_DONE at END, or a step code. */
static uint32_t vm(kernel_context *kernel) {
    ta_core *core = kernel->core;
    const uint32_t *code = core->image_view.code;
    const ta_datamodel_vtable *vtable = kernel->datamodel->vtable;
    void *datamodel = kernel->datamodel->datamodel;
    uint64_t di = kernel->instance->datamodel;
    for (;;) {
        if (exhausted(kernel)) return TA_STEP_YIELD;
        burn(kernel, 1);
        uint32_t p = kernel->workspace->instruction;
        const uint32_t *w = code + p + 1;
        ta_status status;
        switch (code[p]) {
        case TA_OPCODE_END:
            return VM_DONE;
        case TA_OPCODE_BLOCK:
            kernel->workspace->block_end = w[0];
            kernel->workspace->instruction = p + 2;
            break;
        case TA_OPCODE_RAISE: {
            uint32_t event_index = ta_event_new(core, 0);
            if (event_index == TA_NIL) return TA_STEP_EXHAUSTED;
            ta_event *event = ta_event_at(core, event_index);
            event->type = TA_EVENT_TYPE_INTERNAL; /* internal events have no origin */
            event->name_id = w[0];
            event->name = core->image_view.event_nodes[w[0]].name;
            event->trace = ta_core_step_context(core, kernel->instance);
            internal_queue_push(kernel, event_index);
            ta_trace_record *record = trace_step(
                kernel, TA_TRACE_RAISED, (uint32_t)(sizeof *record + sizeof(ta_trace_raised)), 0);
            if (record) {
                ta_trace_raised *raised = (ta_trace_raised *)ta_trace_record_body(record);
                raised->name = event->name; /* an image string */
                raised->name_id = event->name_id;
                ta_trace_record_commit(core->journal, record);
            }
            kernel->workspace->instruction = p + 2;
            break;
        }
        case TA_OPCODE_SEND: {
            uint32_t r = opcode_send(kernel, w[0]);
            if (r != KERNEL_CONTINUE) return r;
            break;
        }
        case TA_OPCODE_CANCEL:
            core->out.sendid = w[0]; /* image string: release is a no-op */
            kernel->workspace->instruction = p + 2;
            trace_cancelled(kernel, w[0]);
            return TA_STEP_CANCEL;
        case TA_OPCODE_CANCEL_EXPRESSION: {
            ta_string_view s;
            if (datamodel_string(kernel, w[0], &s) != TA_OK) {
                error_exec(kernel);
                abort_block(kernel);
                break;
            }
            uint32_t r = ta_shared_string_new(core, s.pointer, s.length);
            if (r == TA_NIL) return TA_STEP_EXHAUSTED;
            core->out.sendid = r;
            kernel->workspace->instruction = p + 2;
            trace_cancelled(kernel, r);
            return TA_STEP_CANCEL;
        }
        case TA_OPCODE_ASSIGN:
            status = vtable->assign
                         ? vtable->assign(&(ta_datamodel_call){datamodel, di, &kernel->environment},
                                          w[0], w[1])
                         : TA_ERROR_EXECUTION;
            charge(kernel);
            if (status != TA_OK) {
                error_exec(kernel);
                abort_block(kernel);
            } else {
                kernel->workspace->instruction = p + 3;
                trace_assign(kernel, w[0]);
            }
            break;
        case TA_OPCODE_SCRIPT:
            status = vtable->script
                         ? vtable->script(&(ta_datamodel_call){datamodel, di, &kernel->environment},
                                          w[0])
                         : TA_ERROR_EXECUTION;
            charge(kernel);
            if (status != TA_OK) {
                error_exec(kernel);
                abort_block(kernel);
            } else {
                kernel->workspace->instruction = p + 2;
            }
            break;
        case TA_OPCODE_LOG: {
            ta_string_view text = {(const char *)0, 0};
            if (w[1] != TA_NIL) {
                status =
                    vtable->log
                        ? vtable->log(&(ta_datamodel_call){datamodel, di, &kernel->environment},
                                      w[1], &text)
                        : TA_ERROR_EXECUTION;
                charge(kernel);
                if (status != TA_OK) {
                    error_exec(kernel);
                    abort_block(kernel);
                    break;
                }
            }
            core->out.label = w[0];
            core->out.text = text;
            kernel->workspace->instruction = p + 3;
            return TA_STEP_LOG;
        }
        case TA_OPCODE_JUMP_IF_FALSE:
            kernel->workspace->instruction = datamodel_condition(kernel, w[0]) ? p + 3 : w[1];
            break;
        case TA_OPCODE_JUMP:
            kernel->workspace->instruction = w[0];
            break;
        case TA_OPCODE_FOREACH_BEGIN: {
            if (kernel->workspace->foreach_top >=
                (kernel->chart->max_foreach ? kernel->chart->max_foreach : 1)) {
                core->out.fault = 1;
                return TA_STEP_FAULT;
            }
            ta_datamodel_loop loop = {0, 0, 0, TA_NIL, TA_NIL};
            status =
                vtable->foreach_begin
                    ? vtable->foreach_begin(
                          &(ta_datamodel_call){datamodel, di, &kernel->environment}, w[0], &loop)
                    : TA_ERROR_EXECUTION;
            uint64_t iter = loop.iter;
            uint32_t n = loop.n;
            charge(kernel);
            if (status != TA_OK) {
                error_exec(kernel);
                abort_block(kernel);
                break;
            }
            if (n == 0) {
                if (vtable->foreach_end)
                    vtable->foreach_end(&(ta_datamodel_call){datamodel, di, 0},
                                        &(ta_datamodel_loop){iter, 0, 0, 0, 0});
                kernel->workspace->instruction = w[3];
                break;
            }
            ta_foreach_frame *frame =
                &current_foreach_frame(kernel)[kernel->workspace->foreach_top++];
            frame->iteration = iter;
            frame->i = 0;
            frame->count = n;
            frame->body = p + 5;
            frame->item = w[1];
            frame->index = w[2];
            status = vtable->foreach_item
                         ? vtable->foreach_item(&(ta_datamodel_call){datamodel, di, 0},
                                                &(ta_datamodel_loop){iter, 0, 0, w[1], w[2]})
                         : TA_ERROR_EXECUTION;
            if (status != TA_OK) {
                error_exec(kernel);
                abort_block(kernel);
                break;
            }
            kernel->workspace->instruction = p + 5;
            break;
        }
        case TA_OPCODE_FOREACH_NEXT: {
            ta_foreach_frame *frame =
                &current_foreach_frame(kernel)[kernel->workspace->foreach_top - 1];
            if (++frame->i < frame->count) {
                status =
                    vtable->foreach_item
                        ? vtable->foreach_item(&(ta_datamodel_call){datamodel, di, 0},
                                               &(ta_datamodel_loop){frame->iteration, 0, frame->i,
                                                                    frame->item, frame->index})
                        : TA_ERROR_EXECUTION;
                if (status != TA_OK) {
                    error_exec(kernel);
                    abort_block(kernel);
                    break;
                }
                kernel->workspace->instruction = w[0];
            } else {
                if (vtable->foreach_end)
                    vtable->foreach_end(&(ta_datamodel_call){datamodel, di, 0},
                                        &(ta_datamodel_loop){frame->iteration, 0, 0, 0, 0});
                kernel->workspace->foreach_top--;
                kernel->workspace->instruction = p + 2;
            }
            break;
        }
        case TA_OPCODE_CUSTOM:
            status = vtable->custom
                         ? vtable->custom(&(ta_datamodel_call){datamodel, di, &kernel->environment},
                                          w[0])
                         : TA_ERROR_EXECUTION;
            charge(kernel);
            if (status != TA_OK) {
                error_exec(kernel);
                abort_block(kernel);
            } else {
                kernel->workspace->instruction = p + 2;
            }
            break;
        default:
            core->out.fault = 2;
            return TA_STEP_FAULT;
        }
    }
}

/* Run the code at `instruction` as part of the current sub-phase. Returns VM_DONE when
 * finished (or when instruction is NIL), else a step code to return. */
static uint32_t run_code(kernel_context *kernel, uint32_t pc) {
    if (!kernel->workspace->vm_active) {
        if (pc == TA_NIL) return VM_DONE;
        kernel->workspace->instruction = pc;
        kernel->workspace->block_end = pc;
        kernel->workspace->foreach_top = 0;
        kernel->workspace->vm_active = 1;
    }
    uint32_t r = vm(kernel);
    if (r == VM_DONE) {
        kernel->workspace->vm_active = 0;
        kernel->workspace->instruction = TA_NIL;
    }
    return r;
}

/* ---- invoke ----------------------------------------------------------------------- */

static uint32_t start_invoke(kernel_context *kernel, uint32_t slot) {
    ta_core *core = kernel->core;
    const ta_invoke_record *invoke = &core->image_view.invokes[kernel->chart->first_invoke + slot];
    uint32_t type = invoke->type, source_string = invoke->source, id = TA_NIL;
    ta_payload payload = 0;
    ta_string_view s;
    if (invoke->typeexpr != TA_NIL) {
        if (datamodel_string(kernel, invoke->typeexpr, &s) != TA_OK) goto err;
        type = is_scxml_type(s) ? TA_NIL : ta_shared_string_new(core, s.pointer, s.length);
        if (!is_scxml_type(s) && type == TA_NIL) return TA_STEP_EXHAUSTED;
    }
    if (invoke->srcexpr != TA_NIL) {
        if (datamodel_string(kernel, invoke->srcexpr, &s) != TA_OK) goto err;
        source_string = ta_shared_string_new(core, s.pointer, s.length);
        if (source_string == TA_NIL) goto nomem;
    } else if (invoke->docexpr != TA_NIL && invoke->source_chart == TA_NIL) {
        /* the document text itself; the system resolves it */
        if (datamodel_string(kernel, invoke->docexpr, &s) != TA_OK) goto err;
        source_string = ta_shared_string_new(core, s.pointer, s.length);
        if (source_string == TA_NIL) goto nomem;
    }
    if (invoke->id != TA_NIL) {
        id = invoke->id;
    } else {
        id = generate_id(kernel, "",
                         ta_image_view_string(&core->image_view, kernel->states[invoke->state].id));
        if (id == TA_NIL) goto nomem;
        if (invoke->idlocation != TA_NIL) {
            ta_status status = kernel->datamodel->vtable->assign_string
                                   ? kernel->datamodel->vtable->assign_string(
                                         &(ta_datamodel_call){kernel->datamodel->datamodel,
                                                              kernel->instance->datamodel, 0},
                                         invoke->idlocation, ta_shared_string_get(core, id))
                                   : TA_ERROR_EXECUTION;
            if (status != TA_OK) goto err;
        }
    }
    if (datamodel_capture(kernel, invoke->capture, TA_CAPTURE_INVOKE, &payload) != TA_OK) goto err;
    {
        uint32_t *ids = ta_instance_word(kernel->instance, kernel->instance_layout.invoke_ids);
        ta_shared_string_release(core, ids[slot]);
        ids[slot] = id;
    }
    core->out.slot = slot;
    core->out.chart = invoke->source_chart;
    core->out.source = source_string;
    core->out.type = type;
    core->out.invokeid = id;
    core->out.payload = payload;
    core->out.autoforward = invoke->autoforward;
    return TA_STEP_INVOKE;
nomem:
    if (TA_STRING_REF_IS_SLAB(type)) ta_shared_string_release(core, type);
    if (TA_STRING_REF_IS_SLAB(source_string) && source_string != invoke->source)
        ta_shared_string_release(core, source_string);
    return TA_STEP_EXHAUSTED;
err:
    error_exec(kernel);
    if (TA_STRING_REF_IS_SLAB(type)) ta_shared_string_release(core, type);
    if (TA_STRING_REF_IS_SLAB(source_string)) ta_shared_string_release(core, source_string);
    if (TA_STRING_REF_IS_SLAB(id)) ta_shared_string_release(core, id);
    return KERNEL_CONTINUE;
}

/* ---- microstep ---------------------------------------------------------------------- */

static int enqueue_done(kernel_context *kernel, uint32_t name_id, ta_payload payload) {
    uint32_t event_index = ta_event_new(kernel->core, 0);
    if (event_index == TA_NIL) return 0;
    ta_event *event = ta_event_at(kernel->core, event_index);
    event->type = TA_EVENT_TYPE_PLATFORM;
    event->name_id = name_id;
    event->name = kernel->core->image_view.event_nodes[name_id].name;
    event->payload = payload;
    event->datamodel = kernel->core->chart_datamodel[kernel->instance->chart];
    event->trace = ta_core_step_context(kernel->core, kernel->instance);
    internal_queue_push(kernel, event_index);
    return 1;
}

/* Returns KERNEL_CONTINUE when the microstep (or exit sequence) is complete. */
static uint32_t micro(kernel_context *kernel) {
    ta_workspace *workspace = kernel->workspace;
    uint32_t *config = configuration_words(kernel);
    for (;;) {
        if (exhausted(kernel)) return TA_STEP_YIELD;
        uint32_t r;
        switch (workspace->subphase) {
        case SUBPHASE_EXIT_COMPUTE: {
            uint32_t *exit = workspace_at(kernel, kernel->workspace_layout.exit);
            const uint32_t *dom = workspace_at(kernel, kernel->workspace_layout.domains);
            ta_bitset_zero(exit, kernel->workspace_layout.state_words);
            for (uint32_t i = 0; i < workspace->selected_count; i++) {
                uint32_t d = dom[i];
                if (d == TA_NIL) continue;
                for (uint32_t w = 0; w < kernel->workspace_layout.state_words; w++)
                    exit[w] |= config[w] & ta_bitset__range_mask(w, d + 1, kernel->states[d].end);
            }
            for (uint32_t s = ta_bitset_next(exit, kernel->state_count, 0); s != TA_NIL;
                 s = ta_bitset_next(exit, kernel->state_count, s + 1))
                if (!leaf(kernel, s)) record_history(kernel, s);
            workspace->cursor = kernel->state_count - 1;
            workspace->subphase = SUBPHASE_EXIT;
            burn(kernel, 1);
            break;
        }
        case SUBPHASE_EXIT: {
            uint32_t s = ta_bitset_previous(workspace_at(kernel, kernel->workspace_layout.exit),
                                            workspace->cursor);
            if (s == TA_NIL || s == 0) {
                if (workspace->pad) return KERNEL_CONTINUE; /* exitInterpreter finished */
                workspace->subphase = SUBPHASE_TRANSITIONS;
                workspace->cursor = 0;
                break;
            }
            workspace->cursor2 = s;
            workspace->auxiliary = 0;
            workspace->subphase = SUBPHASE_EXIT_CODE;
            burn(kernel, 1);
            break;
        }
        case SUBPHASE_EXIT_CODE:
            r = run_code(kernel, kernel->states[workspace->cursor2].onexit);
            if (r != VM_DONE) return r;
            workspace->subphase = SUBPHASE_EXIT_UNINVOKE;
            break;
        case SUBPHASE_EXIT_UNINVOKE: {
            uint32_t s = workspace->cursor2;
            const ta_state_record *state = &kernel->states[s];
            uint32_t *child =
                ta_instance_word(kernel->instance, kernel->instance_layout.invoke_children);
            while (workspace->auxiliary < state->invoke_count) {
                uint32_t slot = state->first_invoke + workspace->auxiliary++;
                if (child[slot]) {
                    kernel->core->out.slot = slot;
                    kernel->core->out.child = child[slot];
                    ta_core_clear_invoke_child(kernel->core, kernel->instance, slot);
                    return TA_STEP_UNINVOKE;
                }
            }
            if (workspace->pad && state->kind == TA_STATE_FINAL && state->parent == 0)
                workspace->final_state = s;
            if (workspace->pad && state->kind == TA_STATE_FINAL && state->parent == 0 &&
                state->donedata != TA_NIL && !workspace->done_payload) {
                ta_payload p = 0;
                if (datamodel_capture(kernel, state->donedata, TA_CAPTURE_DONEDATA, &p) != TA_OK)
                    error_exec(kernel);
                workspace->done_payload = p;
            }
            ta_bitset_clear(config, s);
            ta_bitset_clear(ta_instance_word(kernel->instance, kernel->instance_layout.to_invoke),
                            s);
            workspace->cursor = s - 1;
            workspace->subphase = SUBPHASE_EXIT;
            break;
        }
        case SUBPHASE_TRANSITIONS:
            if (workspace->cursor >= workspace->selected_count) {
                workspace->subphase = SUBPHASE_ENTRY_COMPUTE;
                break;
            }
            r = run_code(kernel,
                         kernel
                             ->transitions[workspace_at(
                                 kernel, kernel->workspace_layout.selected)[workspace->cursor]]
                             .action);
            if (r != VM_DONE) return r;
            workspace->cursor++;
            break;
        case SUBPHASE_ENTRY_COMPUTE:
            if (!compute_entry(kernel)) {
                kernel->core->out.fault = 3;
                return TA_STEP_FAULT;
            }
            burn(kernel, 1);
            workspace->cursor = 1;
            workspace->subphase = SUBPHASE_ENTER;
            break;
        case SUBPHASE_ENTER: {
            uint32_t s = ta_bitset_next(workspace_at(kernel, kernel->workspace_layout.entry),
                                        kernel->state_count, workspace->cursor);
            if (s == TA_NIL) return KERNEL_CONTINUE;
            workspace->cursor2 = s;
            ta_bitset_set(config, s);
            if (kernel->states[s].invoke_count)
                ta_bitset_set(ta_instance_word(kernel->instance, kernel->instance_layout.to_invoke),
                              s);
            if (kernel->chart->binding == TA_BINDING_LATE && kernel->states[s].data_count &&
                !ta_bitset_test(
                    ta_instance_word(kernel->instance, kernel->instance_layout.data_init), s)) {
                ta_bitset_set(ta_instance_word(kernel->instance, kernel->instance_layout.data_init),
                              s);
                for (uint32_t d = 0; d < kernel->states[s].data_count; d++)
                    init_data(kernel, kernel->states[s].first_data + d);
            }
            burn(kernel, 1);
            workspace->subphase = SUBPHASE_ENTER_CODE;
            break;
        }
        case SUBPHASE_ENTER_CODE:
            r = run_code(kernel, kernel->states[workspace->cursor2].onentry);
            if (r != VM_DONE) return r;
            workspace->subphase = SUBPHASE_ENTER_DEFAULT;
            break;
        case SUBPHASE_ENTER_DEFAULT: {
            uint32_t s = workspace->cursor2;
            if (ta_bitset_test(workspace_at(kernel, kernel->workspace_layout.default_entry), s)) {
                r = run_code(kernel, kernel->transitions[kernel->states[s].initial].action);
                if (r != VM_DONE) return r;
            }
            workspace->subphase = SUBPHASE_ENTER_HISTORY;
            workspace->auxiliary = s + 1;
            break;
        }
        case SUBPHASE_ENTER_HISTORY: {
            uint32_t s = workspace->cursor2;
            while (workspace->auxiliary < kernel->states[s].end) {
                uint32_t h = workspace->auxiliary;
                if (kernel->states[h].kind >= TA_STATE_HISTORY_SHALLOW &&
                    ta_bitset_test(
                        workspace_at(kernel, kernel->workspace_layout.history_default_entry), h) &&
                    kernel->states[h].initial != TA_NIL) {
                    r = run_code(kernel, kernel->transitions[kernel->states[h].initial].action);
                    if (r != VM_DONE) return r;
                }
                workspace->auxiliary = kernel->states[h].end;
            }
            workspace->subphase = SUBPHASE_ENTER_FINAL;
            workspace->auxiliary = 0;
            break;
        }
        case SUBPHASE_ENTER_FINAL: {
            uint32_t s = workspace->cursor2;
            if (kernel->states[s].kind == TA_STATE_FINAL) {
                uint32_t p = kernel->states[s].parent;
                if (p == 0) {
                    kernel->instance->flags |= INSTANCE_FLAG_FINAL;
                } else {
                    if (workspace->auxiliary == 0) {
                        ta_payload dd = 0;
                        if (kernel->states[s].donedata != TA_NIL &&
                            datamodel_capture(kernel, kernel->states[s].donedata,
                                              TA_CAPTURE_DONEDATA, &dd) != TA_OK) {
                            error_exec(kernel);
                            dd = 0;
                        }
                        if (!enqueue_done(kernel, kernel->states[p].done_event, dd)) {
                            datamodel_release(kernel, dd);
                            return TA_STEP_EXHAUSTED;
                        }
                        workspace->auxiliary = 1;
                    }
                    uint32_t gp = kernel->states[p].parent;
                    if (gp != TA_NIL && kernel->states[gp].kind == TA_STATE_PARALLEL &&
                        parallel_in_final(kernel, gp)) {
                        if (!enqueue_done(kernel, kernel->states[gp].done_event, 0))
                            return TA_STEP_EXHAUSTED;
                    }
                }
            }
            workspace->cursor = s + 1;
            workspace->subphase = SUBPHASE_ENTER;
            break;
        }
        case SUBPHASE_RUN_CODE:
            r = run_code(kernel, workspace->cursor);
            if (r != VM_DONE) return r;
            return KERNEL_CONTINUE;
        default:
            kernel->core->out.fault = 4;
            return TA_STEP_FAULT;
        }
    }
}

/* ---- phase machine ------------------------------------------------------------------- */

/* `event`: the event the transitions were selected for (TA_NIL: eventless) */
static void begin_micro(kernel_context *kernel, uint32_t event) {
    kernel->workspace->trace_event = event;
    kernel->workspace->subphase = SUBPHASE_EXIT_COMPUTE;
    kernel->workspace->vm_active = 0;
    kernel->workspace->pad = 0;
    kernel->instance->phase = PHASE_MICROSTEP;
    kernel->instance->microstep++;
    /* the events of the microstep get a context of their own */
    ta_trace_context_release(kernel->core, kernel->workspace->trace);
    kernel->workspace->trace = TA_NIL;
    kernel->workspace->trace_fuel = ta_core_trace_fuel(kernel->core, kernel->fuel);
}

uint32_t ta_core_step(ta_core *core, ta_instance *instance, uint32_t *fuel) {
    kernel_context kk, *kernel = &kk;
    release_temporaries(core);
    core->overrun = 0;
    core->trace_step_fuel = *fuel;
    core->out.event = TA_NIL;
    core->out.payload = 0;
    init_k(kernel, core, instance, fuel);
    /* Liveness with a bounded workspace pool: an instance resumed while holding a
     * workspace (it yielded mid-microstep) yields again at its next microstep
     * boundary, with the workspace released, so waiters get a turn. */
    int resumed_holding_workspace = instance->workspace != TA_NIL;
    for (;;) {
        /* inside a microstep, micro() decides (it may finish on overrun) */
        if (*fuel == 0 && instance->phase != PHASE_STABLE && instance->phase != PHASE_DONE &&
            instance->phase != PHASE_MICROSTEP)
            return TA_STEP_YIELD;
        switch (instance->phase) {
        case PHASE_INIT: {
            /* the bootstrap is macrostep 1; its context is the one the system
             * gave the instance */
            if (!instance->macrostep) {
                instance->macrostep = 1;
                instance->microstep = 0;
                trace_macrostep_begin(core, instance, TA_NIL, fuel);
            }
            /* Early binding assigns every <data> now. Late binding assigns the
             * root's (the document is entered at start; microstep entry never
             * includes the root) and the rest on their state's first entry. */
            uint32_t d0 = 0, n = kernel->chart->data_count;
            if (kernel->chart->binding == TA_BINDING_LATE) {
                d0 = kernel->states[0].first_data;
                n = kernel->states[0].data_count;
                ta_bitset_set(ta_instance_word(instance, kernel->instance_layout.data_init), 0);
            }
            while (instance->cursor < n) {
                if (*fuel == 0) return TA_STEP_YIELD;
                init_data(kernel, d0 + instance->cursor++);
                burn(kernel, 1);
            }
            instance->cursor = 0;
            instance->phase = PHASE_INIT_SCRIPT;
            break;
        }
        case PHASE_INIT_SCRIPT:
            if (kernel->chart->script != TA_NIL) {
                if (!acquire_workspace(kernel)) return TA_STEP_NEED_WORKSPACE;
                if (kernel->workspace->subphase != SUBPHASE_RUN_CODE) {
                    kernel->workspace->subphase = SUBPHASE_RUN_CODE;
                    kernel->workspace->vm_active = 0;
                    kernel->workspace->cursor = kernel->chart->script;
                }
                uint32_t r = micro(kernel);
                if (r != KERNEL_CONTINUE) return r;
                release_workspace(kernel);
            }
            instance->phase = PHASE_INIT_ENTER;
            break;
        case PHASE_INIT_ENTER:
            if (!acquire_workspace(kernel)) return TA_STEP_NEED_WORKSPACE;
            kernel->workspace->selected_count = 1;
            workspace_at(kernel, kernel->workspace_layout.selected)[0] = kernel->states[0].initial;
            workspace_at(kernel, kernel->workspace_layout.domains)[0] = 0;
            ta_bitset_zero(workspace_at(kernel, kernel->workspace_layout.exit),
                           kernel->workspace_layout.state_words);
            begin_micro(kernel, TA_NIL);
            kernel->workspace->subphase = SUBPHASE_ENTRY_COMPUTE;
            break;
        case PHASE_EVENTLESS:
            /* forced termination (host terminate / parent cancelling an invoke)
             * takes effect at the next microstep boundary, so even a runaway
             * eventless loop can be stopped */
            if (instance->flags & INSTANCE_FLAG_CANCEL) {
                instance->phase = PHASE_EXIT;
                break;
            }
            if (!ta_bitset_intersects(configuration_words(kernel),
                                      core->image_view.u32 + kernel->chart->eventless_mask,
                                      kernel->instance_layout.state_words)) {
                instance->phase = PHASE_INTERNAL;
                break;
            }
            if (!acquire_workspace(kernel)) return TA_STEP_NEED_WORKSPACE;
            select_transitions(kernel, 0, 1);
            if (kernel->workspace->selected_count) {
                begin_micro(kernel, TA_NIL);
            } else {
                release_workspace(kernel);
                instance->phase = PHASE_INTERNAL;
            }
            break;
        case PHASE_INTERNAL: {
            if (instance->internal_queue_head == TA_NIL) {
                instance->phase = PHASE_INVOKE;
                instance->cursor = 1;
                instance->cursor2 = 0;
                break;
            }
            if (!acquire_workspace(kernel)) return TA_STEP_NEED_WORKSPACE;
            uint32_t event_index = internal_queue_pop(kernel);
            bind_event(core, instance, event_index);
            burn(kernel, 1);
            select_transitions(kernel, ta_event_at(core, event_index)->name_id, 0);
            if (kernel->workspace->selected_count) {
                begin_micro(kernel, event_index);
            } else {
                release_workspace(kernel);
                instance->phase = PHASE_EVENTLESS;
            }
            break;
        }
        case PHASE_INVOKE: {
            uint32_t *ti = ta_instance_word(instance, kernel->instance_layout.to_invoke);
            const uint32_t *config = configuration_words(kernel);
            for (;;) {
                uint32_t s = ta_bitset_next(ti, kernel->state_count, instance->cursor);
                while (s != TA_NIL && !ta_bitset_test(config, s))
                    s = ta_bitset_next(ti, kernel->state_count, s + 1);
                if (s == TA_NIL) break;
                if (instance->cursor != s) {
                    instance->cursor = s;
                    instance->cursor2 = 0;
                }
                if (instance->cursor2 < kernel->states[s].invoke_count) {
                    uint32_t slot = kernel->states[s].first_invoke + instance->cursor2;
                    uint32_t r = start_invoke(kernel, slot);
                    if (r == TA_STEP_EXHAUSTED) return r;
                    instance->cursor2++;
                    burn(kernel, 1);
                    if (r == TA_STEP_INVOKE) return r;
                    continue;
                }
                instance->cursor = s + 1;
                instance->cursor2 = 0;
            }
            ta_bitset_zero(ti, kernel->instance_layout.state_words);
            if (instance->internal_queue_head != TA_NIL) {
                instance->phase = PHASE_EVENTLESS;
            } else if (instance->flags & INSTANCE_FLAG_CANCEL) {
                instance->phase = PHASE_EXIT;
            } else {
                instance->phase = PHASE_STABLE;
                trace_macrostep_end(kernel, 0);
                maybe_detach(kernel);
                return TA_STEP_STABLE;
            }
            break;
        }
        case PHASE_STABLE:
            if (instance->flags & INSTANCE_FLAG_CANCEL) {
                instance->phase = PHASE_EXIT;
                break;
            }
            if (instance->internal_queue_head !=
                TA_NIL) { /* e.g. error.communication reported by the host: a macrostep */
                begin_macrostep(core, instance, instance->internal_queue_head, fuel);
                instance->phase = PHASE_EVENTLESS;
                break;
            }
            return TA_STEP_STABLE;
        case PHASE_EXTERNAL: {
            /* finalize + autoforward for active invokes (spec mainEventLoop) */
            if (!acquire_workspace(kernel)) return TA_STEP_NEED_WORKSPACE;
            const ta_event *event = ta_event_at(core, instance->current_event);
            const uint32_t *child =
                               ta_instance_word(instance, kernel->instance_layout.invoke_children),
                           *ids = ta_instance_word(instance, kernel->instance_layout.invoke_ids);
            while (instance->cursor < kernel->chart->invoke_count) {
                uint32_t slot = instance->cursor;
                const ta_invoke_record *invoke =
                    &core->image_view.invokes[kernel->chart->first_invoke + slot];
                if (!child[slot] || !ta_bitset_test(configuration_words(kernel), invoke->state)) {
                    instance->cursor++;
                    instance->subphase = 0;
                    continue;
                }
                if (instance->subphase == 0) {
                    int match = event->invokeid != TA_NIL && ids[slot] != TA_NIL &&
                                (event->invokeid == ids[slot] ||
                                 ta_shared_string_equal(
                                     core, ids[slot], ta_shared_string_get(core, event->invokeid)));
                    if (match && invoke->finalize != TA_NIL) {
                        if (kernel->workspace->subphase != SUBPHASE_RUN_CODE) {
                            kernel->workspace->subphase = SUBPHASE_RUN_CODE;
                            kernel->workspace->vm_active = 0;
                            kernel->workspace->cursor = invoke->finalize;
                        }
                        uint32_t r = micro(kernel);
                        if (r != KERNEL_CONTINUE) return r;
                        kernel->workspace->subphase = 0;
                    }
                    instance->subphase = 1;
                }
                if (instance->subphase == 1) {
                    instance->subphase = 2;
                    if (invoke->autoforward) {
                        ta_event_retain(core, instance->current_event);
                        core->out.event = instance->current_event;
                        core->out.slot = slot;
                        core->out.child = child[slot];
                        return TA_STEP_AUTOFORWARD;
                    }
                }
                instance->cursor++;
                instance->subphase = 0;
            }
            select_transitions(kernel, event->name_id, 0);
            if (kernel->workspace->selected_count) {
                begin_micro(kernel, instance->current_event);
            } else {
                release_workspace(kernel);
                instance->phase = PHASE_EVENTLESS;
            }
            break;
        }
        case PHASE_MICROSTEP: {
            if (!kernel->workspace) {
                core->out.fault = 5;
                return TA_STEP_FAULT;
            }
            uint32_t r = micro(kernel);
            if (r != KERNEL_CONTINUE) return r;
            int was_exit = kernel->workspace->pad;
            /* the exit is finished, and calling micro again finishes it at
             * once: a refused done event is retried after growth */
            if (was_exit && core->done_event && !core->done_event(core->system, instance->actor))
                return TA_STEP_EXHAUSTED;
            uint32_t final_state = kernel->workspace->final_state;
            ta_payload done = kernel->workspace->done_payload;
            kernel->workspace->done_payload = 0;
            if (!was_exit) trace_microstep(kernel);
            release_workspace(kernel);
            if (was_exit) {
                trace_macrostep_end(kernel, 1); /* the configuration is empty now */
                instance->phase = PHASE_DONE;
                core->out.payload = done;
                core->out.final_state = final_state;
                return TA_STEP_DONE;
            }
            instance->phase =
                (instance->flags & INSTANCE_FLAG_FINAL) ? PHASE_EXIT : PHASE_EVENTLESS;
            if (resumed_holding_workspace || core->overrun) return TA_STEP_YIELD;
            break;
        }
        case PHASE_EXIT:
            if (!acquire_workspace(kernel)) return TA_STEP_NEED_WORKSPACE;
            ta_memcpy(workspace_at(kernel, kernel->workspace_layout.exit),
                      configuration_words(kernel),
                      (size_t)kernel->workspace_layout.state_words * 4);
            kernel->workspace->cursor = kernel->state_count - 1;
            kernel->workspace->vm_active = 0;
            kernel->workspace->pad = 1; /* exitInterpreter mode */
            kernel->workspace->final_state = TA_NIL;
            kernel->workspace->subphase = SUBPHASE_EXIT;
            instance->phase = PHASE_MICROSTEP;
            break;
        case PHASE_DONE:
            return TA_STEP_DONE;
        default:
            core->out.fault = 6;
            return TA_STEP_FAULT;
        }
    }
}

/* ---- public / system-facing API -------------------------------------------------------- */

void ta_core_release_out(ta_core *core) {
    release_temporaries(core);
}

size_t ta_core_instance_size(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    return chart < view->chart_count ? ta_instance_layout_of(&view->charts[chart]).size : 0;
}

size_t ta_core_workspace_size(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    return chart < view->chart_count ? ta_workspace_layout_of(&view->charts[chart]).size : 0;
}

uint32_t ta_core_chart_instance_size(const ta_core *core, uint32_t chart) {
    return ta_instance_layout_of(&core->image_view.charts[chart]).size;
}
uint32_t ta_core_chart_workspace_size(const ta_core *core, uint32_t chart) {
    return ta_workspace_layout_of(&core->image_view.charts[chart]).size;
}

ta_status ta_core_instance_init(ta_core *core, ta_instance *instance, uint32_t chart,
                                uint32_t actor, const ta_datamodel_system_variables *variables,
                                ta_payload init) {
    const ta_chart_record *chart_record = &core->image_view.charts[chart];
    ta_instance_layout l = ta_instance_layout_of(chart_record);
    ta_memset(instance, 0, l.size);
    instance->chart = chart;
    instance->phase = PHASE_INIT;
    instance->current_event = instance->internal_queue_head = instance->internal_queue_tail =
        instance->workspace = TA_NIL;
    instance->actor = actor;
    instance->trace = TA_NIL; /* the system gives the bootstrap its context */
    uint32_t *ids = ta_instance_word(instance, l.invoke_ids);
    for (uint32_t i = 0; i < chart_record->invoke_count; i++)
        ids[i] = TA_NIL;
    const ta_datamodel_binding *binding = ta_core_datamodel(core, chart);
    if (binding->vtable->create) {
        ta_status status = binding->vtable->create(
            binding->datamodel,
            &(ta_datamodel_start){core->chart_context[chart], variables, init, {0, 0}},
            &instance->datamodel);
        if (status != TA_OK) return status;
    } else if (init && binding->vtable->release) {
        binding->vtable->release(binding->datamodel, init);
    }
    return TA_OK;
}

void ta_core_instance_deinit(ta_core *core, ta_instance *instance) {
    kernel_context kk, *kernel = &kk;
    uint32_t fuel = 0;
    init_k(kernel, core, instance, &fuel);
    if (instance->current_event != TA_NIL) ta_event_release(core, instance->current_event);
    instance->current_event = TA_NIL;
    for (uint32_t event = internal_queue_pop(kernel); event != TA_NIL;
         event = internal_queue_pop(kernel))
        ta_event_release(core, event);
    uint32_t *ids = ta_instance_word(instance, kernel->instance_layout.invoke_ids);
    for (uint32_t i = 0; i < kernel->chart->invoke_count; i++) {
        ta_shared_string_release(core, ids[i]);
        ids[i] = TA_NIL;
    }
    release_workspace(kernel);
    ta_trace_context_release(core, instance->trace);
    instance->trace = TA_NIL;
    if (kernel->datamodel->vtable->destroy)
        kernel->datamodel->vtable->destroy(
            &(ta_datamodel_call){kernel->datamodel->datamodel, instance->datamodel, 0});
    instance->datamodel = 0;
    instance->phase = PHASE_DONE;
}

ta_status ta_core_deliver(ta_core *core, ta_instance *instance, uint32_t event) {
    if (instance->phase != PHASE_STABLE || (instance->flags & INSTANCE_FLAG_CANCEL))
        return TA_ERROR_STATE;
    begin_macrostep(core, instance, event, (const uint32_t *)0);
    bind_event(core, instance, event);
    instance->phase = PHASE_EXTERNAL;
    instance->cursor = 0;
    instance->subphase = 0;
    return TA_OK;
}

void ta_core_cancel(ta_core *core, ta_instance *instance) {
    (void)core;
    instance->flags |= INSTANCE_FLAG_CANCEL;
}

void ta_core_set_invoke_child(ta_core *core, ta_instance *instance, uint32_t slot, uint32_t child) {
    ta_instance_layout l = ta_instance_layout_of(&core->image_view.charts[instance->chart]);
    ta_instance_word(instance, l.invoke_children)[slot] = child;
}

void ta_core_clear_invoke_child(ta_core *core, ta_instance *instance, uint32_t slot) {
    ta_instance_layout l = ta_instance_layout_of(&core->image_view.charts[instance->chart]);
    ta_instance_word(instance, l.invoke_children)[slot] = 0;
    ta_shared_string_release(core, ta_instance_word(instance, l.invoke_ids)[slot]);
    ta_instance_word(instance, l.invoke_ids)[slot] = TA_NIL;
}

uint32_t ta_core_invoke_child(ta_core *core, ta_instance *instance, uint32_t slot) {
    ta_instance_layout l = ta_instance_layout_of(&core->image_view.charts[instance->chart]);
    return ta_instance_word(instance, l.invoke_children)[slot];
}

uint32_t ta_core_invoke_id(ta_core *core, ta_instance *instance, uint32_t slot) {
    ta_instance_layout l = ta_instance_layout_of(&core->image_view.charts[instance->chart]);
    return ta_instance_word(instance, l.invoke_ids)[slot];
}

uint32_t ta_core_raise_platform(ta_core *core, ta_instance *instance, const char *name,
                                uint32_t sendid) {
    return raise_platform(core, instance, name, sendid);
}

ta_status ta_core_raise_platform_data(ta_core *core, ta_instance *instance, const char *name,
                                      ta_payload data) {
    const ta_datamodel_binding *binding = ta_core_datamodel(core, instance->chart);
    if (raise_platform(core, instance, name, TA_NIL) == TA_NIL) { /* no event slot */
        if (data && binding->vtable->release) binding->vtable->release(binding->datamodel, data);
        return TA_ERROR_NO_MEMORY;
    }
    ta_event *event = ta_event_at(core, instance->internal_queue_tail);
    event->payload = data;
    event->datamodel = data ? core->chart_datamodel[instance->chart] : 0xFF;
    return TA_OK;
}

void ta_core_init_declaration(ta_core *core, ta_instance *instance, uint32_t declaration) {
    kernel_context kk, *kernel = &kk;
    uint32_t fuel = 0xFFFFFFFFu;
    init_k(kernel, core, instance, &fuel);
    init_data(kernel, declaration);
}

ta_status ta_core_datamodel_migrate(ta_core *core, ta_instance *instance, ta_payload *bindings) {
    const ta_datamodel_binding *binding = ta_core_datamodel(core, instance->chart);
    const ta_datamodel_vtable *vtable = binding->vtable;
    struct ta_datamodel_environment environment = {core, instance, 0};
    *bindings = 0;
    if (vtable->size < offsetof(ta_datamodel_vtable, migrate) + sizeof vtable->migrate ||
        !vtable->migrate)
        return TA_OK;
    return vtable->migrate(
        &(ta_datamodel_call){binding->datamodel, instance->datamodel, &environment}, bindings);
}

int ta_core_in_state(const ta_core *core, ta_instance *instance, uint32_t state) {
    const ta_chart_record *chart = &core->image_view.charts[instance->chart];
    if (state >= chart->state_count) return 0;
    return ta_bitset_test(ta_instance_word(instance, ta_instance_layout_of(chart).config), state);
}

/* ---- datamodel environment queries --------------------------------------------------------- */

int ta_datamodel_environment_in(const ta_datamodel_environment *environment, uint32_t state) {
    return ta_core_in_state(environment->core, environment->instance, state);
}
void ta_datamodel_environment_charge(ta_datamodel_environment *environment, uint32_t fuel) {
    environment->charge += fuel;
}
const ta_image *ta_datamodel_environment_image(const ta_datamodel_environment *environment) {
    return environment->core->image;
}
uint32_t ta_datamodel_environment_chart(const ta_datamodel_environment *environment) {
    return environment->instance->chart;
}
