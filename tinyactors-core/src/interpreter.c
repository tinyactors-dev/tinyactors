#include "interpreter.h"
#include "layout.h"
#include "mail.h"

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

/* In `enabled`: the document's initial transition, which bootstrap takes. It
 * is not a transition ID (ADR 0071 numbers initial transitions after the
 * authored ones); it only marks the bootstrap microstep. */
#define TA_ENABLED_BOOTSTRAP (UINT32_MAX - 1u)

/* In `enabled`: an operator transition (ADR 0082), which `it->jump` holds
 * while the microstep begins. It has no executable content, so nothing
 * after that reads it, and a continuation carries this mark alone. */
#define TA_ENABLED_OPERATOR (UINT32_MAX - 2u)

/* Whether an entry of `enabled` is one of the definition's transitions. */
static bool ta_enabled_authored(ta_transition_id transition) {
    return transition != TA_ENABLED_BOOTSTRAP && transition != TA_ENABLED_OPERATOR;
}

/* A list of states: a transition's targets, in the definition or in an
 * operator transition. */
typedef struct {
    const ta_state_id *ids;
    uint32_t count;
} ta_state_list;

typedef struct {
    ta_state_id source;
    ta_state_list targets;
    ta_transition_type type;
    ta_span32 actions;
} ta_transition_view;

static ta_state_list ta_targets(const ta_interpreter *it, ta_span32 targets) {
    return (ta_state_list){ .ids = targets.length == 0 ? NULL : it->definition->targets + targets.offset,
                            .count = targets.length };
}

/* The executable content of an entry of `enabled`; an operator transition
 * has none. */
static ta_span32 ta_enabled_actions(const ta_definition *definition, ta_transition_id transition) {
    if (transition == TA_ENABLED_BOOTSTRAP) {
        return definition->initial_transitions[definition->root_initial].actions;
    }
    if (transition == TA_ENABLED_OPERATOR) {
        return (ta_span32){0};
    }
    return definition->transitions[transition].actions;
}

static ta_transition_view ta_view(const ta_interpreter *it, ta_transition_id transition) {
    const ta_definition *definition = it->definition;
    if (transition == TA_ENABLED_BOOTSTRAP) {
        const ta_initial_transition_def *initial = &definition->initial_transitions[definition->root_initial];
        return (ta_transition_view){
            .source = TA_STATE_NONE,
            .targets = ta_targets(it, initial->targets),
            .type = TA_TRANSITION_EXTERNAL,
            .actions = ta_enabled_actions(definition, transition),
        };
    }
    if (transition == TA_ENABLED_OPERATOR) {
        const ta_operator_jump *jump = it->jump;
        return (ta_transition_view){
            .source = jump == NULL ? TA_STATE_NONE : jump->source,
            .targets = { .ids = jump == NULL ? NULL : jump->targets, .count = jump == NULL ? 0 : jump->target_count },
            .type = TA_TRANSITION_INTERNAL,
        };
    }
    const ta_transition_def *def = &definition->transitions[transition];
    return (ta_transition_view){
        .source = def->source,
        .targets = ta_targets(it, def->targets),
        .type = def->type,
        .actions = ta_enabled_actions(definition, transition),
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
 * Tracing (ADR 0067)
 *
 * Every record is written once, when the step that produced it commits: a
 * step that runs out of memory and runs again (ADR 0070) writes its records
 * on the run that commits, and on no other. Events a step queued are written
 * as RAISED records from a queue mark, after the step's own record. The
 * guards of a selection wait in `conditions` until the selection completes.
 */

/* One guard the selection in progress evaluated. */
struct ta_trace_condition {
    ta_transition_id transition;
    ta_expr_id expr;
    bool result;
    bool failed;
    ta_event_name_id detail;
    int32_t code;
    /* The queue index of the error.execution it raised, or UINT32_MAX. */
    uint32_t raised;
};

static bool ta_tracing(const ta_interpreter *it, ta_trace_kind kind) {
    return (it->trace_kinds & TA_TRACE_BIT(kind)) != 0;
}

ta_trace_parent ta_actor_macrostep_parent(const ta_actor *actor) {
    return actor->event_block == NULL ? (ta_trace_parent){0} : actor->event_block->slots.trace;
}

ta_step_key ta_actor_step(const ta_actor *actor) {
    if (!actor->trace_begun) {
        return (ta_step_key){ .session = actor->session_id, .family = actor->definition->family };
    }
    return (ta_step_key){ .session = actor->session_id, .macrostep = actor->trace_macrostep,
                          .microstep = actor->trace_microstep, .family = actor->definition->family };
}

/* The key of the step running now; outside a macrostep, the session alone. */
static ta_step_key ta_trace_key(const ta_interpreter *it) {
    return ta_actor_step(it->actor);
}

ta_trace_parent ta_interpreter_trace_here(const ta_interpreter *it) {
    return (ta_trace_parent){ .context = it->trace_context, .cause = ta_trace_key(it) };
}

/* The iteration, from 0, whose body the innermost <foreach> among the
 * first `count` frames runs. */
static uint32_t ta_frames_iteration(const ta_exec_frame *frames, uint32_t count) {
    for (uint32_t index = count; index > 0; index -= 1u) {
        const ta_exec_frame *frame = &frames[index - 1u];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            return frame->iteration == 0 ? 0 : frame->iteration - 1u;
        }
    }
    return 0;
}

static ta_trace_position ta_trace_nowhere(void) {
    return (ta_trace_position){ .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE, .transition = TA_TRANSITION_NONE,
                                .action = TA_ACTION_NONE };
}

/* What a run keeps of where it stands: in scratch while it runs, in the
 * actor's continuation once it stopped. The sets and the default-history
 * map are read only at a microstep's start (ADR 0086). */
typedef struct {
    ta_micro_position at;
    const ta_transition_id *enabled;
    uint32_t enabled_count;
    const ta_exec_frame *frames;
    uint32_t frame_count;
    const uint64_t *to_exit;
    const uint64_t *to_enter;
    const uint64_t *default_entry;
    const ta_state_id *default_history;
} ta_run_view;

/* The phase of a position, with its state and transition; no action. */
static ta_trace_position ta_phase_position(const ta_definition *definition, const ta_run_view *view) {
    ta_trace_position position = ta_trace_nowhere();
    switch (view->at.phase) {
        case TA_PHASE_FINISH:
            if (view->at.stage != TA_FINISH_EXIT) {
                break;
            }
            /* fall through: exiting every state, highest first */
        case TA_PHASE_EXIT:
            position.at = TA_TRACE_AT_EXIT;
            position.state = view->at.state == 0 ? TA_STATE_NONE : view->at.state - 1u;
            break;
        case TA_PHASE_TRANSITIONS:
            position.at = TA_TRACE_AT_TRANSITION;
            if (view->at.state < view->enabled_count && ta_enabled_authored(view->enabled[view->at.state])) {
                position.transition = view->enabled[view->at.state];
                position.state = definition->transitions[position.transition].source;
            }
            break;
        case TA_PHASE_ENTER:
            position.at = TA_TRACE_AT_ENTER;
            position.state = view->at.state;
            break;
        case TA_PHASE_DATA:
        case TA_PHASE_SCRIPT:
            position.at = TA_TRACE_AT_BOOTSTRAP;
            break;
        case TA_PHASE_FINALIZE:
            position.at = TA_TRACE_AT_FINALIZE;
            break;
        case TA_PHASE_SELECT:
            position.at = TA_TRACE_AT_SELECT;
            break;
        case TA_PHASE_INVOKES:
            position.at = TA_TRACE_AT_INVOKE;
            position.state = view->at.state;
            break;
        case TA_PHASE_NONE:
        default:
            break;
    }
    return position;
}

/* The first block of a state's handlers that holds a unit, or an empty one. */
static ta_span32 ta_first_block(const ta_definition *definition, ta_span32 handlers) {
    for (uint32_t index = 0; index < handlers.length; index += 1u) {
        ta_span32 block = definition->handlers[handlers.offset + index];
        if (block.length != 0) {
            return block;
        }
    }
    return (ta_span32){0};
}

/* The position of the first action of `block`, run where `at` stands. */
static ta_trace_position ta_unit_at(const ta_definition *definition, const ta_run_view *view, ta_micro_position at,
                                    ta_span32 block) {
    ta_run_view there = *view;
    there.at = at;
    ta_trace_position position = ta_phase_position(definition, &there);
    position.action = block.length == 0 ? TA_ACTION_NONE : block.offset;
    return position;
}

/*
 * Where a microstep stands at its start (ADR 0086): the first unit it runs,
 * found from its sets without running anything, in the order the microstep
 * runs units: the <onexit> handlers of the exit set, highest first; the
 * enabled transitions' content; then, for each state of the entry set,
 * lowest first, its <onentry> handlers, its initial transition's content
 * and its history default's content. A microstep with no unit stands at
 * its first transition, with no action.
 */
static ta_trace_position ta_start_position(const ta_definition *definition, const ta_run_view *view) {
    for (ta_state_id state = definition->state_count; state > 0; state -= 1u) {
        ta_span32 block = ta_bits_get(view->to_exit, state - 1u)
                              ? ta_first_block(definition, definition->states[state - 1u].onexit_handlers)
                              : (ta_span32){0};
        if (block.length != 0) {
            return ta_unit_at(definition, view, (ta_micro_position){ .phase = TA_PHASE_EXIT, .state = state }, block);
        }
    }
    ta_micro_position transitions = { .phase = TA_PHASE_TRANSITIONS };
    for (uint32_t index = 0; index < view->enabled_count; index += 1u) {
        ta_span32 block = ta_enabled_actions(definition, view->enabled[index]);
        if (block.length != 0) {
            transitions.state = index;
            return ta_unit_at(definition, view, transitions, block);
        }
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        if (!ta_bits_get(view->to_enter, state)) {
            continue;
        }
        const ta_state_def *def = &definition->states[state];
        ta_span32 block = ta_first_block(definition, def->onentry_handlers);
        if (block.length == 0 && ta_bits_get(view->default_entry, state)) {
            block = definition->initial_transitions[def->initial].actions;
        }
        ta_state_id history = view->default_history[state];
        if (block.length == 0 && history != TA_STATE_NONE) {
            block = definition->transitions[definition->states[history].transitions.offset].actions;
        }
        if (block.length != 0) {
            return ta_unit_at(definition, view, (ta_micro_position){ .phase = TA_PHASE_ENTER, .state = state }, block);
        }
    }
    return ta_unit_at(definition, view, (ta_micro_position){ .phase = TA_PHASE_TRANSITIONS }, (ta_span32){0});
}

/*
 * Where a stopped run stands (ADR 0083): its phase, and the unit of
 * executable content it runs next. With a <foreach> on top of the frames,
 * that unit is the loop's next iteration: `action` is the <foreach> and
 * `iteration` the one it binds next. Otherwise it is the next action of the
 * innermost open block, in the iteration of the innermost <foreach> whose
 * body that block is. With no open block there is no action. At a
 * microstep's start it is the first unit the microstep runs (ADR 0086).
 */
static ta_trace_position ta_run_position(const ta_definition *definition, const ta_run_view *view) {
    if (view->at.phase == TA_PHASE_START) {
        return ta_start_position(definition, view);
    }
    ta_trace_position position = ta_phase_position(definition, view);
    if (view->frame_count == 0) {
        return position;
    }
    const ta_exec_frame *top = &view->frames[view->frame_count - 1u];
    if (top->kind == TA_EXEC_FRAME_FOREACH) {
        position.action = top->cursor;
        position.iteration = top->iteration;
        return position;
    }
    if (top->cursor < top->actions.length) {
        position.action = top->actions.offset + top->cursor;
    }
    position.iteration = ta_frames_iteration(view->frames, view->frame_count - 1u);
    return position;
}

static ta_run_view ta_interpreter_view(const ta_interpreter *it) {
    return (ta_run_view){ .at = it->at, .enabled = it->enabled, .enabled_count = it->enabled_count,
                          .frames = it->exec_frames, .frame_count = it->exec_count, .to_exit = it->to_exit,
                          .to_enter = it->to_enter, .default_entry = it->default_entry,
                          .default_history = it->default_history };
}

ta_trace_position ta_actor_position(const ta_actor *actor) {
    ta_continuation *saved = actor->sparse == NULL ? NULL : actor->sparse->paused;
    if (!actor->macrostep || saved == NULL) {
        return ta_trace_nowhere();
    }
    /* The sets in their order: active, to exit, to enter, default entry. */
    const uint64_t *sets = ta_continuation_sets(saved);
    ta_run_view view = { .at = saved->at, .enabled = ta_continuation_enabled(saved),
                         .enabled_count = saved->enabled_count, .frames = ta_continuation_frames(saved),
                         .frame_count = saved->frame_count, .to_exit = sets + saved->words,
                         .to_enter = sets + 2u * (size_t)saved->words,
                         .default_entry = sets + 3u * (size_t)saved->words,
                         .default_history = ta_continuation_history(saved) };
    return ta_run_position(actor->definition, &view);
}

/* Where the interpreter stands while a unit runs: the phase, the unit
 * running now, and the iteration its innermost <foreach> runs. */
static ta_trace_position ta_trace_position_now(const ta_interpreter *it) {
    ta_run_view view = ta_interpreter_view(it);
    ta_trace_position position = ta_phase_position(it->definition, &view);
    position.action = it->trace_action;
    position.iteration = ta_frames_iteration(it->exec_frames, it->exec_count);
    return position;
}

/* A record of `kind` at the step and position running now. */
static ta_trace_record ta_trace_record_now(const ta_interpreter *it, ta_trace_kind kind) {
    ta_trace_record record = ta_trace_record_blank(kind);
    record.key = ta_trace_key(it);
    record.position = ta_trace_position_now(it);
    record.context = it->trace_context;
    record.actor = it->actor->record.id;
    record.definition = it->definition->id;
    record.now = ta_actor_now(it->system, it->actor);
    return record;
}

/*
 * ta_trace_emit writes a record the interpreter built because its mask
 * asked for the kind. The values the record carries are rendered by the
 * running definition's data model, so the writer is given its binding.
 */
static void ta_trace_emit(const ta_interpreter *it, const ta_trace_record *record) {
    const ta_tel_values values = { .datamodel = &it->definition->datamodel,
                                   .shared = it->definition->datamodel_shared,
                                   .instance = it->actor->datamodel_instance,
                                   .host = &it->actor->host };
    ta_tel_write_trace(it->system, record, &values);
}

/* A fault's detail and code; its data went into the error event. */
static void ta_trace_fault(ta_trace_record *record, const ta_fault *fault) {
    record->has_fault = true;
    if (fault != NULL) {
        /* The data moved into the error event; ACTION_FAILED borrows it back. */
        record->fault = (ta_fault){ .detail = fault->detail, .code = fault->code, .message = fault->message };
    }
}

/* Counts one error event by its kind (ADR 0072); other events count nothing. */
void ta_interpreter_count_error(ta_system *system, const ta_actor *actor, ta_event_name_id name) {
    if (ta_trie_under(&system->event_names.trie, name, TA_EVENT_NAME_ERROR_EXECUTION)) {
        TA_COUNT(system, actor->definition, errors_execution);
    } else if (ta_trie_under(&system->event_names.trie, name, TA_EVENT_NAME_ERROR_COMMUNICATION)) {
        TA_COUNT(system, actor->definition, errors_communication);
    } else if (ta_trie_under(&system->event_names.trie, name, TA_EVENT_NAME_ERROR_PLATFORM)) {
        TA_COUNT(system, actor->definition, errors_platform);
    }
}

/* Counts the errors a unit queued after `mark`, when it commits. */
static void ta_count_raised(ta_interpreter *it, uint32_t mark) {
    const ta_event_block *queue = it->actor->event_block;
    for (uint32_t index = mark; queue != NULL && index < queue->count; index += 1u) {
        ta_interpreter_count_error(it->system, it->actor, queue->events[index].event.name);
    }
}

static void ta_trace_raised_records(ta_interpreter *it, uint32_t mark) {
    const ta_event_block *queue = it->actor->event_block;
    if (!ta_tracing(it, TA_TRACE_RAISED) || queue == NULL) {
        return;
    }
    for (uint32_t index = mark; index < queue->count; index += 1u) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_RAISED);
        record.event = &queue->events[index].event;
        ta_trace_emit(it, &record);
    }
}

void ta_interpreter_trace_raised(ta_interpreter *it, uint32_t mark) {
    ta_count_raised(it, mark);
    ta_trace_raised_records(it, mark);
}

void ta_interpreter_trace_invoke(ta_interpreter *it, ta_trace_kind kind, ta_invoke_id invoke) {
    if (kind == TA_TRACE_INVOKED) {
        TA_COUNT(it->system, it->actor->definition, invocations_started);
    } else if (kind == TA_TRACE_INVOKE_CANCELLED) {
        TA_COUNT(it->system, it->actor->definition, invocations_ended);
    }
    if (!ta_tracing(it, kind)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, kind);
    record.invoke = invoke;
    ta_trace_emit(it, &record);
}

void ta_interpreter_trace_forward(ta_interpreter *it, ta_invoke_id invoke, const ta_event *event) {
    if (!ta_tracing(it, TA_TRACE_FORWARDED)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_FORWARDED);
    record.invoke = invoke;
    record.event = event;
    ta_trace_emit(it, &record);
}

/* An action of the running unit failed; its error is queued. */
static void ta_trace_action_failed(ta_interpreter *it, const ta_fault *fault) {
    if (!ta_tracing(it, TA_TRACE_ACTION_FAILED)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_ACTION_FAILED);
    record.action = it->trace_action;
    ta_trace_fault(&record, fault);
    /* The error the failure just queued is the queue's last event, and its
     * data is what the operation reported: borrow it for the record. */
    const ta_event_block *queue = it->actor->event_block;
    if (queue != NULL && queue->count != 0) {
        record.fault.data = queue->events[queue->count - 1u].event.data;
    }
    ta_trace_emit(it, &record);
}

/* The states of `set` into `out`, in document order or its reverse. */
static uint32_t ta_trace_states(const ta_interpreter *it, const uint64_t *set, ta_state_id *out, bool reverse) {
    uint32_t count = 0;
    uint32_t states = it->definition->state_count;
    for (uint32_t index = 0; index < states; index += 1u) {
        ta_state_id state = reverse ? states - 1u - index : index;
        if (ta_bits_get(set, state)) {
            out[count++] = state;
        }
    }
    return count;
}

/*
 * The macrostep's first step has committed: taking its event can no longer
 * be undone. Its counter moves, and MACROSTEP_BEGIN is written, once. The
 * actor changes, so an image taken before no longer describes it.
 */
static void ta_trace_begin(ta_interpreter *it) {
    ta_actor *actor = it->actor;
    if (actor->trace_begun) {
        return;
    }
    ta_actor_touch(actor);
    actor->trace_begun = true;
    actor->trace_macrostep += 1u;
    TA_COUNT(it->system, actor->definition, macrosteps);
    actor->trace_microstep = 0;
    ta_trace_parent parent = ta_actor_macrostep_parent(actor);
    it->trace_context = parent.context;
    it->trace_kinds = ta_trace_macrostep_kinds(&it->system->trace_masks, &parent.context);
    if (!ta_tracing(it, TA_TRACE_MACROSTEP_BEGIN)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_MACROSTEP_BEGIN);
    record.position = ta_trace_nowhere();
    record.cause = parent.cause;
    const ta_event_block *block = actor->event_block;
    if (block != NULL && block->slots.external == TA_EXTERNAL_TAKEN) {
        record.event = &block->slots.external_event;
    }
    ta_trace_emit(it, &record);
}

/* The macrostep ended, stable or finished: MACROSTEP_END, with the active
 * configuration. */
static void ta_trace_end(ta_interpreter *it) {
    ta_actor *actor = it->actor;
    if (!actor->trace_begun) {
        return;
    }
    if (ta_tracing(it, TA_TRACE_MACROSTEP_END) && it->trace_states != NULL) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_MACROSTEP_END);
        record.position = ta_trace_nowhere();
        record.states = it->trace_states;
        record.state_count = ta_trace_states(it, it->active, it->trace_states, false);
        ta_trace_emit(it, &record);
    }
    actor->trace_begun = false;
}

/*
 * A microstep leaves selection and becomes visible: its counter moves, and
 * one MICROSTEP record names its event, its transitions in selection order,
 * the exit set in exit order, the entry set in entry order, and the history
 * states the exit recorded. It is written before its actions run.
 */
static void ta_trace_microstep_record(ta_interpreter *it, const ta_event *event) {
    if (!ta_tracing(it, TA_TRACE_MICROSTEP) || it->trace_states == NULL) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_MICROSTEP);
    record.position = ta_trace_nowhere();
    record.event = event;
    if (it->enabled_count != 0 && ta_enabled_authored(it->enabled[0])) {
        record.transitions = it->enabled;
        record.transition_count = it->enabled_count;
    } else if (it->jump != NULL) {
        /* An operator transition: its source and the targets it named. */
        record.state = it->jump->source;
        record.states = it->jump->targets;
        record.state_count = it->jump->target_count;
    }
    ta_state_id *lists = it->trace_states;
    record.exited = lists;
    record.exited_count = ta_trace_states(it, it->to_exit, lists, true);
    record.entered = lists + record.exited_count;
    record.entered_count = ta_trace_states(it, it->to_enter, lists + record.exited_count, false);
    ta_state_id *history = lists + record.exited_count + record.entered_count;
    const ta_definition *definition = it->definition;
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        const ta_state_def *def = &definition->states[state];
        if ((def->kind == TA_STATE_HISTORY_SHALLOW || def->kind == TA_STATE_HISTORY_DEEP) &&
            ta_bits_get(it->to_exit, def->parent)) {
            history[record.history_count++] = state;
        }
    }
    record.history = history;
    ta_trace_emit(it, &record);
}

/*
 * The microstep that reached a tenth of the limit, or the limit itself
 * (ADR 0075): a counter, and a record with its transitions and the
 * configuration before it. Counts move only here, on commit, so a retry
 * never counts twice.
 */
static void ta_trace_runaway_mark(ta_interpreter *it) {
    uint32_t limit = it->microstep_limit;
    uint32_t count = it->actor->trace_microstep;
    uint32_t warning = limit / 10u == 0 ? 1u : limit / 10u;
    if (limit == 0 || (count != warning && count != limit)) {
        return;
    }
    ta_trace_kind kind = count == limit ? TA_TRACE_RUNAWAY : TA_TRACE_LONG_MACROSTEP;
    if (kind == TA_TRACE_RUNAWAY) {
        TA_COUNT(it->system, it->actor->definition, runaways);
    } else {
        TA_COUNT(it->system, it->actor->definition, macrosteps_long);
    }
    if (!ta_tracing(it, kind) || it->trace_states == NULL) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, kind);
    record.position = ta_trace_nowhere();
    if (it->enabled_count != 0 && it->enabled[0] != TA_ENABLED_BOOTSTRAP) {
        record.transitions = it->enabled;
        record.transition_count = it->enabled_count;
    }
    record.states = it->trace_states;
    record.state_count = ta_trace_states(it, it->active, it->trace_states, false);
    ta_trace_emit(it, &record);
}

/* A microstep commits: its counter moves, its record is written, and the
 * runaway marks are checked. */
static void ta_trace_microstep(ta_interpreter *it, const ta_event *event) {
    it->actor->trace_microstep += 1u;
    TA_COUNT(it->system, it->actor->definition, microsteps);
    ta_trace_microstep_record(it, event);
    ta_trace_runaway_mark(it);
}

/* Keeps a guard the selection evaluated, to be written when it completes. */
static void ta_trace_guard(ta_interpreter *it, const struct ta_trace_condition *guard) {
    if (it->conditions != NULL && it->condition_count < it->condition_capacity) {
        it->conditions[it->condition_count++] = *guard;
    }
}

/*
 * A selection completed: its guards, in evaluation order, each followed by
 * the error it raised, or, without guards to write, the events it queued
 * after `mark`.
 */
static void ta_trace_selection(ta_interpreter *it, uint32_t mark) {
    uint32_t count = it->condition_count;
    it->condition_count = 0;
    ta_count_raised(it, mark);
    if (it->conditions == NULL || !ta_tracing(it, TA_TRACE_CONDITION)) {
        ta_trace_raised_records(it, mark);
        return;
    }
    const ta_event_block *queue = it->actor->event_block;
    for (uint32_t index = 0; index < count; index += 1u) {
        const struct ta_trace_condition *guard = &it->conditions[index];
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_CONDITION);
        record.position = ta_trace_nowhere();
        record.position.at = TA_TRACE_AT_SELECT;
        record.position.transition = guard->transition;
        record.position.state = it->definition->transitions[guard->transition].source;
        record.state = record.position.state;
        record.expr = guard->expr;
        record.result = guard->result;
        if (guard->failed) {
            record.has_fault = true;
            record.fault = (ta_fault){ .detail = guard->detail, .code = guard->code };
        }
        ta_trace_emit(it, &record);
        if (guard->raised != UINT32_MAX && queue != NULL && guard->raised < queue->count &&
            ta_tracing(it, TA_TRACE_RAISED)) {
            ta_trace_record raised = ta_trace_record_now(it, TA_TRACE_RAISED);
            raised.position = record.position;
            raised.event = &queue->events[guard->raised].event;
            ta_trace_emit(it, &raised);
        }
    }
}

/* The most guards one selection can evaluate: each atomic state walks its
 * ancestors' transitions, so shared ancestors count once per atomic state. */
static uint32_t ta_trace_guard_capacity(const ta_definition *definition) {
    uint64_t total = 0;
    for (ta_state_id atomic = 0; atomic < definition->state_count; atomic += 1u) {
        if (!ta_definition_is_atomic(definition, atomic)) {
            continue;
        }
        for (ta_state_id state = atomic; state != TA_STATE_NONE; state = definition->states[state].parent) {
            ta_span32 transitions = definition->states[state].transitions;
            for (uint32_t index = 0; index < transitions.length; index += 1u) {
                total += definition->transitions[transitions.offset + index].condition != UINT32_MAX ? 1u : 0u;
            }
        }
    }
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
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
 * Event blocks (ADR 0064, ADR 0069)
 */

/* The first capacity of a block's queue, which is also the largest a block
 * the system keeps as its workspace may have: it keeps a small block only. */
#define TA_EVENT_BLOCK_QUEUE_INITIAL 8u

size_t ta_event_block_bytes(uint32_t capacity) {
    return offsetof(ta_event_block, events) + (size_t)capacity * sizeof(ta_queued_event);
}

static void ta_block_free(ta_system *system, ta_event_block *block) {
    ta_mem_free(system->allocator, block,
                &(ta_allocator_free_params){ .size = ta_event_block_bytes(block->capacity),
                                             .alignment = _Alignof(ta_event_block) });
}

/* An empty block: the system's workspace, else a new one with room for
 * `capacity` events. Null when there is no memory for one. */
static ta_event_block *ta_block_take(ta_system *system, uint32_t capacity) {
    ta_event_block *block = system->workspace;
    if (block != NULL) {
        system->workspace = NULL;
    } else {
        block = ta_mem_alloc(system->allocator, ta_event_block_bytes(capacity), _Alignof(ta_event_block));
        if (block == NULL) {
            return NULL;
        }
        block->capacity = capacity;
    }
    block->count = 0;
    block->slots = (ta_event_slots){0};
    return block;
}

/*
 * ta_interpreter_hold_block lends the actor an event block if it holds none:
 * a cached one, else a header only, because a delivered event needs no queue
 * room and the block grows when the first internal event is queued. It
 * allocates only when the cache is empty. TA_ERR_NOMEM leaves the actor as it
 * was (ADR 0069); the block belongs to the actor until its input ends.
 */
ta_result ta_interpreter_hold_block(ta_system *system, ta_actor *actor) {
    if (actor->event_block == NULL) {
        /* A delivered event needs no queue room: a new block is a header
         * only, which grows when the first internal event is queued. */
        actor->event_block = ta_block_take(system, 0);
        if (actor->event_block == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    return TA_OK;
}

/*
 * Makes sure the actor holds a block with room for one more internal event:
 * its own, else the system's workspace, else a new one; a full queue
 * doubles. TA_ERR_NOMEM leaves the actor as it was, apart from a workspace
 * it took, which it may keep.
 */
static ta_result ta_queue_room(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL) {
        block = ta_block_take(system, TA_EVENT_BLOCK_QUEUE_INITIAL);
        if (block == NULL) {
            return TA_ERR_NOMEM;
        }
        actor->event_block = block;
    }
    if (block->count < block->capacity) {
        return TA_OK;
    }
    if (block->capacity > UINT32_MAX / 2u) {
        return TA_ERR_NOMEM;
    }
    uint32_t capacity = block->capacity == 0 ? TA_EVENT_BLOCK_QUEUE_INITIAL : block->capacity * 2u;
    ta_event_block *grown = ta_mem_alloc(system->allocator, ta_event_block_bytes(capacity),
                                         _Alignof(ta_event_block));
    if (grown == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(grown, block, ta_event_block_bytes(block->count));
    grown->capacity = capacity;
    ta_block_free(system, block);
    actor->event_block = grown;
    return TA_OK;
}

/*
 * Gives an actor's block back when the actor no longer needs it: it holds
 * no event, no microstep is paused, and no running macrostep's trace parent
 * (ADR 0067). It becomes the system's workspace, or is freed when there is
 * one already or the block grew past the first capacity. Cannot fail.
 */
static void ta_block_settle(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL || block->count != 0 || block->slots.external != TA_EXTERNAL_NONE || block->slots.has_kept ||
        (actor->sparse != NULL && actor->sparse->paused != NULL) || actor->macrostep) {
        return;
    }
    actor->event_block = NULL;
    if (system->workspace == NULL && block->capacity <= TA_EVENT_BLOCK_QUEUE_INITIAL) {
        system->workspace = block;
        return;
    }
    ta_block_free(system, block);
}

/* ta_interpreter_workspace_free: see interpreter.h. */
void ta_interpreter_workspace_free(ta_system *system) {
    if (system->workspace != NULL) {
        ta_block_free(system, system->workspace);
        system->workspace = NULL;
    }
}

ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event) {
    ta_actor_touch(actor);
    ta_result result = ta_queue_room(system, actor);
    if (result != TA_OK) {
        ta_block_settle(system, actor);
        ta_event_release(event);
        return result;
    }
    ta_event_block *queue = actor->event_block;
    queue->events[queue->count] = (ta_queued_event){
        .event = *event,
        .flags = TA_EVENT_FLAG_OWNS_DATA | (event->type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
    };
    queue->count += 1u;
    return TA_OK;
}

/* Removes the queue's head; its event is the caller's now. */
static ta_event ta_queue_pop(ta_actor *actor) {
    ta_event_block *queue = actor->event_block;
    ta_event head = queue->events[0].event;
    for (uint32_t index = 1; index < queue->count; index += 1) {
        queue->events[index - 1u] = queue->events[index];
    }
    queue->count -= 1u;
    return head;
}

/* How many events the actor's queue holds, a taken head included. */
static uint32_t ta_queue_mark(const ta_actor *actor) {
    return actor->event_block == NULL ? 0 : actor->event_block->count;
}

/*
 * Takes back, and releases, the events queued after `mark`: a step that ran
 * out of memory after queueing some is done again, and queues them again
 * (ADR 0070). Only events of that step lie after the mark.
 */
static void ta_queue_truncate(ta_actor *actor, uint32_t mark) {
    ta_event_block *queue = actor->event_block;
    while (queue != NULL && queue->count > mark) {
        queue->count -= 1u;
        ta_event_release(&queue->events[queue->count].event);
    }
}

/* Makes `event`, and its references, the kept event (ADR 0069). */
static void ta_keep_event(ta_event_slots *slots, const ta_event *event) {
    if (slots->has_kept) {
        ta_event_release(&slots->kept_event);
    }
    slots->kept_event = *event;
    slots->has_kept = true;
}

/*
 * Binds `_event` to an event just taken (SCXML 5.10.1); the actor holds a
 * block, since it holds the event. Nothing moves but a taken queue head,
 * which leaves the queue for the kept slot, so that a retry can bind it
 * again.
 */
static void ta_bind_event(ta_actor *actor, ta_event_binding taken) {
    ta_event_slots *slots = &actor->event_block->slots;
    if (slots->bound == TA_BOUND_INTERNAL) {
        ta_event head = ta_queue_pop(actor);
        ta_keep_event(slots, &head);
        slots->bound = TA_BOUND_KEPT;
    }
    slots->bound_before = slots->bound;
    slots->bound = (uint8_t)taken;
    if (taken == TA_BOUND_EXTERNAL) {
        slots->external = TA_EXTERNAL_TAKEN;
    }
}

/*
 * Gives back the event of a microstep that could not start (ADR 0047): it
 * never left its slot or the queue, so only the marks go back.
 */
static void ta_unbind_event(ta_actor *actor) {
    ta_event_slots *slots = &actor->event_block->slots;
    if (slots->bound == TA_BOUND_EXTERNAL) {
        slots->external = TA_EXTERNAL_WAITING;
    }
    slots->bound = slots->bound_before;
}

/* Frees the operator transition a waiting external event came with. */
static void ta_jump_free(ta_system *system, ta_event_slots *slots) {
    if (slots->jump != NULL) {
        ta_mem_free(system->allocator, slots->jump,
                    &(ta_allocator_free_params){ .size = slots->jump->bytes,
                                                 .alignment = _Alignof(ta_operator_jump) });
        slots->jump = NULL;
    }
}

/* Notes, as its macrostep ends, whether the external event it took enabled
 * nothing, for ta_input_result.ignored (ADR 0089 section 8). */
static void ta_note_ignored(ta_interpreter *it) {
    ta_event_block *block = it->actor->event_block;
    it->ended_ignored = block != NULL && block->slots.external_ignored;
    if (block != NULL) {
        block->slots.timed = false;
    }
}

/*
 * The logical time of the actor's macrostep (ADR 0089 section 9): the one
 * its first resume took, so it does not move while a macrostep spans
 * several resumes; the system's time when no macrostep has one.
 */
ta_time ta_interpreter_macrostep_time(const ta_system *system, const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (actor->macrostep && block != NULL && block->slots.timed) {
        return block->slots.time;
    }
    return ta_actor_now(system, actor);
}

/* The first resume of a macrostep takes its time, in a block the actor then
 * holds until the macrostep ends (ta_block_settle). */
static ta_result ta_take_macrostep_time(ta_system *system, ta_actor *actor) {
    ta_result result = ta_interpreter_hold_block(system, actor);
    if (result == TA_OK && !actor->event_block->slots.timed) {
        actor->event_block->slots.time = ta_actor_now(system, actor);
        actor->event_block->slots.timed = true;
    }
    return result;
}

/* Releases the external event, and the operator transition it came with. */
static void ta_release_external(ta_system *system, ta_event_slots *slots) {
    if (slots->external != TA_EXTERNAL_NONE) {
        ta_event_release(&slots->external_event);
        slots->external = TA_EXTERNAL_NONE;
    }
    slots->external_ignored = false;
    ta_jump_free(system, slots);
}

/* Releases every event the actor holds: its queue, taken head included,
 * its kept event and its external event. */
static void ta_release_events(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL) {
        return;
    }
    while (block->count != 0) {
        ta_event event = ta_queue_pop(actor);
        ta_event_release(&event);
    }
    ta_event_slots *slots = &block->slots;
    if (slots->has_kept) {
        ta_event_release(&slots->kept_event);
        slots->has_kept = false;
    }
    ta_release_external(system, slots);
    slots->bound = TA_BOUND_NONE;
}

/*
 * A macrostep ended with no internal event waiting. An invoked session keeps
 * the event `_event` names, and so its block, since cancelling it runs its
 * <onexit> between its macrosteps (SCXML 6.4); any other session lets go of
 * every event, and its block goes back when the run returns.
 */
static void ta_end_macrostep_events(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL || actor->sparse == NULL || !actor->sparse->has_parent) {
        ta_release_events(system, actor);
        return;
    }
    ta_event_slots *slots = &block->slots;
    if (slots->bound == TA_BOUND_INTERNAL) {
        ta_event head = ta_queue_pop(actor);
        ta_keep_event(slots, &head);
        slots->bound = TA_BOUND_KEPT;
    } else if (slots->bound == TA_BOUND_EXTERNAL) {
        ta_keep_event(slots, &slots->external_event);
        slots->external = TA_EXTERNAL_NONE;
        slots->bound = TA_BOUND_KEPT;
    }
    ta_release_external(system, slots);
}

typedef struct {
    ta_event_name_id name;
    ta_send_id send_id;
    ta_fault *fault;
    /* A communication error that makes an idle actor run: what its
     * macrostep runs under, the failed send's parent. Null for none. */
    const ta_trace_parent *trace;
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

/*
 * What a failed step of evaluation is. A data-model or plugin operation
 * that fails is an execution error. One that ran out of memory and changed
 * nothing (TA_OP_NOMEM), like core's own allocation failing while it
 * interns a name or takes scratch, is TA_ERR_NOMEM: it stops the step with
 * nothing visible, never an error event (ADR 0047, ADR 0070).
 */
static ta_result ta_op_result(ta_op_status status) {
    return status == TA_OP_OK ? TA_OK : status == TA_OP_NOMEM ? TA_ERR_NOMEM : TA_EXEC_FAILED;
}

/* A failed operation: TA_ERR_NOMEM for TA_OP_NOMEM, which drops the fault,
 * otherwise error.execution with the fault's data (SCXML 5.10). */
static ta_result ta_op_failed(ta_interpreter *it, ta_op_status status, ta_fault *fault) {
    if (status == TA_OP_NOMEM) {
        ta_value_release(&fault->data);
        *fault = (ta_fault){0};
        return TA_ERR_NOMEM;
    }
    return ta_execution_error(it, fault);
}

static ta_result ta_core_result(ta_result result) {
    return result == TA_OK || result == TA_ERR_NOMEM ? result : TA_EXEC_FAILED;
}

typedef struct {
    ta_result result; /* the step's result */
    int32_t code;     /* the fault's code when core itself failed the step */
    const char *message;
} ta_core_fault_params;

/*
 * A step core failed itself, not a data model: give the fault a code and a
 * static message, so the error event and the ACTION_FAILED record say why
 * (ADR 0072's failure logs). A fault a data model already filled stays as it
 * is, and a result that is not TA_EXEC_FAILED passes through.
 */
static ta_result ta_core_fault(ta_fault *fault, const ta_core_fault_params *params) {
    if (params->result == TA_EXEC_FAILED && fault != NULL && fault->code == 0 && fault->message.len == 0) {
        fault->code = params->code;
        fault->message = ta_str_from_cstr(params->message);
    }
    return params->result;
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

/*
 * Initializes `owner`'s data from declaration `*cursor` on, and leaves
 * `*cursor` at the declaration whose error could not be queued when it
 * returns TA_ERR_NOMEM: that declaration's initializer failed, so running
 * it again changes nothing that ran before (ADR 0070).
 */
static ta_result ta_initialize_data(ta_interpreter *it, ta_state_id owner, uint32_t *cursor) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (; *cursor < definition->data_decl_count; *cursor += 1u) {
        uint32_t index = *cursor;
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner || decl->initializer == TA_INITIALIZER_NONE ||
            (it->at.phase == TA_PHASE_DATA && ta_bits_get(it->bound_decls, index))) {
            continue;
        }
        ta_fault fault = {0};
        ta_value value = {0};
        uint32_t mark = ta_queue_mark(it->actor);
        const ta_value_source *source = &definition->initializers[decl->initializer].value;
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = *source, .out = &value,
                                                                     .fault = &fault });
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
        }
        ta_result result = TA_OK;
        if (status != TA_OP_OK) {
            /* The binding stays declared; the error does not stop other initializers. */
            result = ta_op_failed(it, status, &fault);
            if (result != TA_EXEC_FAILED) {
                ta_value_release(&value);
                return result;
            }
        }
        if (ta_tracing(it, TA_TRACE_DATA_INIT)) {
            ta_trace_record record = ta_trace_record_now(it, TA_TRACE_DATA_INIT);
            record.state = owner;
            record.name = ta_atom_text(&it->system->atoms, decl->name);
            record.expr = source->kind == TA_VALUE_SOURCE_EXPR ? source->as.expr : TA_EXPR_ID_NONE;
            if (result == TA_OK) {
                record.has_value = true;
                record.value = value;
            } else {
                ta_trace_fault(&record, &fault);
            }
            ta_trace_emit(it, &record);
        }
        ta_value_release(&value);
        ta_interpreter_trace_raised(it, mark);
    }
    return TA_OK;
}

/*
 * Declares every datum from declaration `*cursor` on, and records the ones
 * the instance's binding supplies. On TA_ERR_NOMEM `*cursor` names the
 * declaration whose error could not be queued; it failed, so declaring it
 * again repeats nothing (ADR 0070).
 */
static ta_result ta_declare_data(ta_interpreter *it, uint32_t *cursor) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (; *cursor < definition->data_decl_count && v->declare != NULL; *cursor += 1u) {
        uint32_t index = *cursor;
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
        if (bound) {
            ta_bits_put(it->bound_decls, index, true);
        }
        if (status != TA_OP_OK) {
            uint32_t mark = ta_queue_mark(it->actor);
            ta_result result = ta_op_failed(it, status, &fault);
            if (result != TA_EXEC_FAILED) {
                return result;
            }
            /* A declaration that failed: DATA_INIT with its fault. */
            if (ta_tracing(it, TA_TRACE_DATA_INIT)) {
                ta_trace_record record = ta_trace_record_now(it, TA_TRACE_DATA_INIT);
                record.state = definition->data_decls[index].owner_state;
                record.name = ta_atom_text(&it->system->atoms, definition->data_decls[index].name);
                ta_trace_fault(&record, &fault);
                ta_trace_emit(it, &record);
            }
            ta_interpreter_trace_raised(it, mark);
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

ta_result ta_send_id_intern(ta_system *system, ta_str text, ta_send_id *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_send_id id = ta_send_id_from_text(system, text);
    if (id == 0) {
        ta_atom atom = 0;
        ta_result result = ta_atom_intern(&system->atoms, text, &atom);
        if (result != TA_OK) {
            return result;
        }
        id = ta_send_id_from_text(system, text);
    }
    if (id == 0) {
        return TA_ERR_INVALID;
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
    ta_actor_id sender;
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    ta_actor_ref destination;
    ta_event_name_id event;
    ta_send_id send_id;
    const ta_value *data;
    ta_trace_parent trace;
    /* True inside the sender's macrostep: mail takes an envelope from the running input's claim (mail.h). */
    bool in_run;
} ta_submit_params;

/*
 * Hands an event to another session as mail (mail.h): an envelope on the
 * running input's outgoing list inside the sender's macrostep, on the run
 * queue at a safe point (a due send, an activation send; the caller has made
 * room with ta_mail_prepare). A destination that is not resident waits for its
 * address, and one that can never be reached is answered at once, so the
 * sender can hear error.communication in the same macrostep. Answers what mail
 * answered.
 */
static ta_mail_status ta_submit_external(ta_system *system, const ta_submit_params *params) {
    /* A session ID that is not a resident runtime ID resolves here (ADR 0066). */
    ta_actor_ref destination;
    bool routed_by_name = ta_session_route(system, &params->destination, &destination);
    ta_mail_status mailed = ta_mail_send(system,
                                         &(ta_mail_params){
                                             .sender = params->sender,
                                             .destination = destination,
                                             .event = {
                                                 .name = params->event,
                                                 .type = TA_EVENT_EXTERNAL,
                                                 .send_id = params->send_id,
                                                 .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = params->origin },
                                                 .invoke_id = params->invoke_id,
                                                 .data = *params->data,
                                             },
                                             .trace = params->trace,
                                             .in_macrostep = params->in_run,
                                         });
    if (routed_by_name) {
        ta_actor_ref_release(&destination);
    }
    return mailed;
}

void ta_interpreter_raised_outside(ta_system *system, ta_actor *actor, ta_event_name_id name,
                                   const ta_trace_parent *parent) {
    ta_interpreter_count_error(system, actor, name);
    ta_trace_context none = { 0 };
    const ta_trace_context *context = parent != NULL ? &parent->context : &none;
    if ((ta_trace_macrostep_kinds(&system->trace_masks, context) & TA_TRACE_BIT(TA_TRACE_RAISED)) == 0) {
        return;
    }
    const ta_event_block *queue = actor->event_block;
    if (queue == NULL || queue->count == 0) {
        return;
    }
    ta_trace_record record = ta_trace_record_blank(TA_TRACE_RAISED);
    record.key = (ta_step_key){ .session = actor->session_id, .family = actor->definition->family };
    record.actor = actor->record.id;
    record.definition = actor->definition->id;
    record.now = ta_actor_now(system, actor);
    record.event = &queue->events[queue->count - 1u].event;
    if (parent != NULL) {
        record.context = parent->context;
        record.cause = parent->cause;
    }
    ta_tel_write_trace(system, &record, NULL);
}

static ta_result ta_communication_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    if (actor->finished) {
        /* A communication error for a finished session is dropped: it can no longer observe it. */
        if (params->fault != NULL) {
            ta_value_release(&params->fault->data);
            params->fault->data = (ta_value){0};
        }
        return TA_OK;
    }
    ta_error_params error = *params;
    error.name = TA_EVENT_NAME_ERROR_COMMUNICATION;
    /* An idle actor starts a macrostep for the error: room for it comes first (run.h). */
    ta_result result = actor->macrostep ? TA_OK : ta_run_reserve_start(system);
    if (result != TA_OK) {
        return result;
    }
    result = ta_raise_error(system, actor, &error);
    if (result == TA_OK && params->trace != NULL) {
        /* Queued outside a run, by a delayed send that failed: no unit
         * commits it, so it is counted and recorded here (ADR 0072). */
        ta_interpreter_raised_outside(system, actor, TA_EVENT_NAME_ERROR_COMMUNICATION, params->trace);
    }
    if (result == TA_OK) {
        /* An asynchronous failure makes an idle actor runnable again. */
        ta_interpreter_start_macrostep(system, actor, params->trace);
    }
    return result;
}

void ta_interpreter_start_macrostep(ta_system *system, ta_actor *actor, const ta_trace_parent *trace) {
    if (actor->macrostep) {
        return;
    }
    actor->macrostep = true;
    actor->record.residency = TA_ACTOR_RUNNING;
    if (actor->event_block != NULL) {
        actor->event_block->slots.trace = trace != NULL ? *trace : (ta_trace_parent){0};
    }
    ta_run_start(system, actor);
}

void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes) {
    static const uint8_t empty[1] = { 0 };
    const ta_content_def *def = &definition->contents[content];
    *media_type = def->media_type;
    *bytes = def->bytes.ptr != NULL ? def->bytes : (ta_bytes){ .ptr = empty, .len = 0 };
}

void ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send, ta_dispatch *dispatch) {
    ta_actor *sender = ta_actor_lookup(system, send->owner);
    ta_fault fault = {0};
    bool in_run = dispatch->in_run;
    bool failed = false;   /* the sender is to hear error.communication */
    bool refused = false;  /* mail found no envelope for it (TA_MAIL_FULL): a quota, not a route */
    *dispatch = (ta_dispatch){ .in_run = in_run, .sent = true };
    if (sender == NULL) {
        /* The sender's session is gone; the send is dropped. */
    } else if (!ta_invoke_parent_accepts(system, sender, &send->destination)) {
        /* The parent already cancelled this invoked child. */
    } else if (ta_system_is_scxml_io(system, send->io_type)) {
        ta_mail_status mailed = ta_submit_external(
            system, &(ta_submit_params){
                        .sender = send->owner,
                        .origin = sender->record.self,
                        /* Invoke: events from an invoked child to its parent carry the invoke ID. */
                        .invoke_id = ta_invoke_parent_id(sender, &send->destination),
                        .destination = send->destination,
                        .event = send->event,
                        .send_id = send->send_id,
                        .data = &send->data,
                        .trace = send->trace,
                        .in_run = in_run,
                    });
        dispatch->mailed = true;
        dispatch->delivered = mailed == TA_MAIL_QUEUED || mailed == TA_MAIL_WAITING;
        refused = mailed == TA_MAIL_FULL;
        failed = !dispatch->delivered;
    } else {
        const ta_io *io = ta_system_io_of(system, send->io_type);
        ta_send request = {
            .source = sender->record.self,
            .send_id = send->send_id,
            .event = send->event,
            .io_type = send->io_type,
            .target = send->target,
            .data = send->data,
            .trace = send->trace,
            .due = send->due,
        };
        /* A send to a host processor is dispatched while its sender lives,
         * so the content resolves against the sender's definition. */
        if (send->has_content) {
            ta_interpreter_literal_content(sender->definition, send->content, &request.content_type,
                                           &request.content);
        }
        if (send->has_target_text) {
            ta_str media_type = { 0 };
            ta_bytes text = { 0 };
            ta_interpreter_literal_content(sender->definition, send->target_text, &media_type, &text);
            request.target_text = (ta_str){ .ptr = (const char *)text.ptr, .len = text.len };
        }
        /* A processor that ran out of memory accepted nothing, and nothing keeps the send for another try
         * (ADR 0099 section 6): the sender hears error.communication, as for a processor that failed. */
        failed = io == NULL || io->v->send(io->self, &request, &fault) != TA_OP_OK;
    }
    if (refused && in_run) {
        /* No envelope was left in the input's claim or in the pool: a quota refusal like maxPendingSends, heard as
         * an execution error in the sender's own queue (ADR 0099 section 3). */
        static const char text[] = "no envelope is left for this mail (macrostepReserve, poolEnvelopes)";
        ta_fault refusal = { .code = TA_ERR_NOMEM, .message = { .ptr = text, .len = (uint32_t)(sizeof text - 1u) } };
        system->counters.quota_refusals += 1u;
        (void)ta_raise_error(system, sender, &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION,
                                                                 .fault = &refusal });
    } else if (failed) {
        (void)ta_communication_error(system, sender,
                                     &(ta_error_params){ .send_id = send->send_id, .fault = &fault,
                                                         .trace = in_run ? NULL : &send->trace });
    }
    dispatch->sent = !failed;
    ta_value_release(&fault.data);
    ta_value_release(&send->target);
    ta_value_release(&send->data);
    ta_actor_ref_release(&send->destination);
}

/*
 * A <send> being evaluated. `event` and `delay` are its evaluated name and
 * delay; the values are owned until the send is committed.
 */
typedef struct {
    ta_value data;
    ta_value target;
    ta_scxml_target scxml;
    ta_atom io_type;
    ta_send_id send_id;
    ta_event_name_id event;
    ta_time delay;
    bool scxml_io;
    bool has_content;
    bool generated_id;
    ta_content_id content;
    /* A custom processor's literal target (ta_send.target_text). */
    bool has_target_text;
    ta_content_id target_text;
    /* The send left the actor: dispatched, stored to fire later, or held
     * for an unpublished actor's activation (ADR 0067's SENT). */
    bool sent;
} ta_send_state;

static void ta_send_state_release(ta_send_state *state) {
    ta_value_release(&state->data);
    ta_value_release(&state->target);
    ta_scxml_target_release(&state->scxml);
}

/*
 * A content-sized temporary that does not fit the render area (ADR 0092
 * section 3): an execution error that names the parameter, and fails the
 * same way for the same content, where the arena used to grow.
 */
void ta_interpreter_render_fault(ta_fault *fault) {
    static const char text[] = "the render area is full (renderBytes)";
    *fault = (ta_fault){ .code = TA_ERR_NOMEM, .message = { .ptr = text, .len = (uint32_t)(sizeof text - 1u) } };
}

static ta_result ta_send_payload(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
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
        return TA_OK;
    }
    if (send->params.length == 0) {
        return ta_op_result(
            ta_eval_source(it, &(ta_value_params){ .source = send->data, .out = &state->data, .fault = fault }));
    }
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->make_params == NULL) {
        return TA_EXEC_FAILED;
    }
    ta_named_value *values = ta_arena_alloc(&it->system->render, send->params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        ta_interpreter_render_fault(fault);
        return TA_EXEC_FAILED;
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
    return ta_op_result(status);
}

static ta_result ta_send_target(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                ta_fault *fault) {
    if (!state->scxml_io) {
        /*
         * A custom I/O processor receives a literal target as the text the
         * chart wrote, without the data model, and a targetexpr as the value
         * it evaluates to, not as a URI.
         */
        if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
            state->has_target_text = true;
            state->target_text = send->target.as.content;
            return TA_OK;
        }
        return send->target.kind == TA_VALUE_SOURCE_NONE
                   ? TA_OK
                   : ta_op_result(ta_eval_source(it, &(ta_value_params){ .source = send->target,
                                                                         .out = &state->target, .fault = fault }));
    }
    if (send->target.kind == TA_VALUE_SOURCE_NONE) {
        state->scxml = (ta_scxml_target){ .kind = TA_SCXML_TARGET_SELF };
        return TA_OK;
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
            return ta_op_result(status);
        }
        if (v->value_text == NULL) {
            return ta_core_fault(fault, &(ta_core_fault_params){
                                            .result = TA_EXEC_FAILED, .code = TA_ERR_UNSUPPORTED,
                                            .message = "the data model cannot turn the <send> target into text" });
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
            return ta_op_result(status);
        }
    }
    /* Interning a named address may run out of memory. */
    return ta_core_fault(fault, &(ta_core_fault_params){
                                    .result = ta_core_result(ta_scxml_target_parse(it->system, text, &state->scxml)),
                                    .code = TA_ERR_INVALID,
                                    .message = "the <send> target is not a target SCXML can send to" });
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
            *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME,
                                   .family = it->definition->family,
                                   .as.runtime = it->actor->session_id };
            return true;
        case TA_SCXML_TARGET_PARENT:
            if (it->actor->sparse == NULL || !it->actor->sparse->has_parent) {
                return false;
            }
            *out = ta_actor_runtime_ref(it->system, it->actor->sparse->parent.actor);
            return true;
        case TA_SCXML_TARGET_ACTOR:
            *out = target->as.actor;
            return true;
        case TA_SCXML_TARGET_INVOKE:
            if (!ta_invoke_destination(it->actor, target->as.invoke, out)) {
                return false;
            }
            out->family = ta_actor_family_of(it->system, out->as.runtime);
            return true;
        default:
            return false;
    }
}

/*
 * A replaying actor's invocations run nothing, so `#_<invokeid>` names no
 * child (ADR 0089 section 3). While the invocation is active the target is
 * still one the run being replayed could reach: it resolves to no session,
 * which the held send carries until it is published or dropped.
 */
static bool ta_send_replayed_invoke(ta_interpreter *it, const ta_scxml_target *target, ta_actor_ref *out) {
    ta_actor_ref none = { .kind = TA_ACTOR_REF_RUNTIME };
    if (target->kind != TA_SCXML_TARGET_INVOKE || !ta_invoke_is_active(it->actor, target->as.invoke)) {
        return false;
    }
    *out = none;
    return true;
}

/* Resolves `type` / `typeexpr`; an unknown processor is an execution error. */
static ta_result ta_send_io_type(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                 ta_fault *fault) {
    ta_system *system = it->system;
    ta_atom type = system->scxml_io_type;
    if (send->type.kind == TA_TEXT_SOURCE_ATOM) {
        type = send->type.as.atom;
    } else if (send->type.kind == TA_TEXT_SOURCE_EXPR) {
        ta_value holder = {0};
        ta_str text = {0};
        ta_result result = ta_op_result(ta_eval_text(it, &(ta_text_eval_params){
                                                             .expr = send->type.as.expr, .holder = &holder,
                                                             .text = &text, .fault = fault,
                                                         }));
        if (result == TA_OK) {
            result = text.len == 0 ? ta_core_fault(fault, &(ta_core_fault_params){
                                                              .result = TA_EXEC_FAILED, .code = TA_ERR_INVALID,
                                                              .message = "the <send> typeexpr gave empty text" })
                                   : ta_core_result(ta_atom_intern(&system->atoms, text, &type));
        }
        ta_value_release(&holder);
        if (result != TA_OK) {
            return result;
        }
    }
    state->scxml_io = ta_system_is_scxml_io(system, type);
    state->io_type = state->scxml_io ? system->scxml_io_type : type;
    return state->scxml_io || ta_system_io_of(system, type) != NULL
               ? TA_OK
               : ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = TA_EXEC_FAILED, .code = TA_ERR_NOT_FOUND,
                                          .message = "the <send> type names no I/O processor this system has" });
}

/* Resolves `event` / `eventexpr`; the expression's text is a dotted event name. */
static ta_result ta_send_event_name(ta_interpreter *it, const ta_send_action *send, ta_event_name_id *out,
                                    ta_fault *fault) {
    if (send->event.kind == TA_EVENT_SOURCE_NAME) {
        *out = send->event.as.name;
        return TA_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_result result = ta_op_result(ta_eval_text(it, &(ta_text_eval_params){
                                                         .expr = send->event.as.expr, .holder = &holder,
                                                         .text = &text, .fault = fault,
                                                     }));
    if (result == TA_OK) {
        result = ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = ta_core_result(ta_event_name_intern_dotted(&it->system->event_names,
                                                                                               text, out)),
                                          .code = TA_ERR_INVALID,
                                          .message = "the <send> eventexpr is not a dotted event name" });
    }
    ta_value_release(&holder);
    return result;
}

/* Resolves `delay` / `delayexpr`; the expression's text is an SCXML duration. */
static ta_result ta_send_delay(ta_interpreter *it, const ta_send_action *send, ta_time *out, ta_fault *fault) {
    *out = 0;
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        *out = send->delay.as.literal;
        return TA_OK;
    }
    if (send->delay.kind != TA_DELAY_SOURCE_EXPR) {
        return TA_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                               .expr = send->delay.as.expr, .holder = &holder,
                                               .text = &text, .fault = fault,
                                           });
    bool unparsed = status == TA_OP_OK && ta_duration_parse(text, out) != TA_OK;
    ta_value_release(&holder);
    if (unparsed) {
        return ta_core_fault(fault, &(ta_core_fault_params){
                                        .result = TA_EXEC_FAILED, .code = TA_ERR_INVALID,
                                        .message = "the <send> delayexpr is not an SCXML duration" });
    }
    return ta_op_result(status);
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
 * its events carry a blank sendid (SCXML 5.10.1). A generated ID is the
 * next value of the actor's counter, which moves only once the send is done
 * (ta_exec_send), so a send that runs out of memory and runs again stores
 * the same ID through idlocation (ADR 0070).
 */
static ta_result ta_send_assign_id(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                   ta_fault *fault) {
    state->send_id = 0;
    if (send->id.kind == TA_ID_SOURCE_LITERAL) {
        state->send_id = ta_literal_send_id(send->id.as.literal);
        return TA_OK;
    }
    if (send->id.kind != TA_ID_SOURCE_LOCATION) {
        return TA_OK;
    }
    state->generated_id = true;
    state->send_id = TA_GENERATED_SEND_ID_BIT | (it->actor->next_send_id + 1u);
    char buffer[48];
    ta_text_writer writer = { .buffer = buffer, .capacity = sizeof(buffer) };
    ta_send_id_write(it->system, state->send_id, &writer);
    return ta_op_result(ta_store_text(it, &(ta_store_text_params){
                                              .location = send->id.as.location,
                                              .text = { .ptr = buffer, .len = writer.length },
                                              .fault = fault,
                                          }));
}

/* An error while evaluating a <send> carries that send's ID (SCXML 5.10.1). */
static ta_result ta_send_execution_error(ta_interpreter *it, ta_send_id send_id, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION,
                                                          .send_id = send_id, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

/* Evaluates every attribute of a <send>, in the order SCXML 6.2 gives them. */
static ta_result ta_send_evaluate(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                  ta_fault *fault) {
    ta_result result = ta_send_assign_id(it, send, state, fault);
    if (result == TA_OK) {
        result = ta_send_event_name(it, send, &state->event, fault);
    }
    if (result == TA_OK) {
        result = ta_send_io_type(it, send, state, fault);
    }
    if (result == TA_OK) {
        result = ta_send_delay(it, send, &state->delay, fault);
    }
    if (result == TA_OK) {
        result = ta_send_payload(it, send, state, fault);
    }
    if (result == TA_OK) {
        result = ta_send_target(it, send, state, fault);
    }
    if (result == TA_OK && state->scxml_io && state->scxml.kind == TA_SCXML_TARGET_INTERNAL && state->delay != 0) {
        /* A delayed send cannot target the internal queue. */
        result = ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = TA_EXEC_FAILED, .code = TA_ERR_UNSUPPORTED,
                                          .message = "a <send> with a delay cannot target #_internal" });
    }
    if (result == TA_OK && !(state->scxml_io && state->scxml.kind == TA_SCXML_TARGET_INTERNAL)) {
        /*
         * The data leaves the session (ADR 0097, SCXML C.1), to another
         * session, an I/O processor or this session's external queue: what
         * the receiver gets is a copy made now, when the <send> runs (SCXML
         * 6.2), which nothing this session does later can change. An event
         * for #_internal stays in the session and is not copied.
         */
        result = ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = ta_op_result(ta_interpreter_copy_out(it, &state->data, fault)),
                                          .code = TA_ERR_UNSUPPORTED,
                                          .message = "the data model cannot copy the <send> data" });
    }
    return result;
}

/*
 * Queues, dispatches, holds or stores an evaluated <send>, which owns the
 * state's values from here. It runs out of memory only where nothing is
 * visible yet: holding it for an unpublished actor, or when core has no
 * envelope for the mail it sends (ADR 0099).
 */
static ta_result ta_send_commit(ta_interpreter *it, ta_send_state *state) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    if (state->scxml_io && state->scxml.kind == TA_SCXML_TARGET_INTERNAL) {
        ta_value_release(&state->target);
        ta_event event = {
            .name = state->event,
            .type = TA_EVENT_INTERNAL,
            .send_id = state->send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
            .invoke_id = TA_INVOKE_ID_NONE,
            .data = state->data,
        };
        /* The queued event owns its origin reference (ADR 0065). */
        ta_actor_ref_retain(&event.origin.as.actor);
        return ta_interpreter_enqueue_internal(system, actor, &event);
    }
    bool replaying = ta_actor_replaying(actor);
    if (replaying && state->delay == 0) {
        /* A replay crosses no border (ADR 0089 section 3): the run being
         * replayed sent this, so it counts as sent, and goes nowhere. */
        ta_send_state_release(state);
        state->sent = true;
        return TA_OK;
    }

    ta_delayed_send record = {
        .owner = actor->record.id,
        .send_id = state->send_id,
        .due = ta_actor_now(system, actor) + state->delay,
        .event = state->event,
        .io_type = state->io_type,
        .target = state->target,
        .data = state->data,
        .has_content = state->has_content,
        .content = state->content,
        .has_target_text = state->has_target_text,
        .target_text = state->target_text,
        .trace = ta_interpreter_trace_here(it),
    };
    /* A named target's reference passes to the send record (ADR 0065). */
    if (state->scxml_io && !ta_send_destination(it, &state->scxml, &record.destination) &&
        !(replaying && ta_send_replayed_invoke(it, &state->scxml, &record.destination))) {
        ta_send_state_release(state);
        return ta_communication_error(system, actor, &(ta_error_params){ .send_id = state->send_id });
    }
    if (!actor->published) {
        ta_result result = ta_actor_hold_send(
            system, actor, &(ta_activation_send){ .send = record, .immediate = state->delay == 0 });
        if (result != TA_OK) {
            ta_value_release(&record.data);
            ta_value_release(&record.target);
            ta_actor_ref_release(&record.destination);
        }
        state->sent = result == TA_OK;
        return result;
    }
    if (state->delay != 0 && actor->sparse != NULL &&
        actor->sparse->timers.count >= system->params[TA_PARAM_MAX_PENDING_SENDS]) {
        /* A quota (ADR 0092 section 4): the send is not made, and the
         * sender hears it as an execution error. */
        ta_value_release(&record.data);
        ta_value_release(&record.target);
        ta_actor_ref_release(&record.destination);
        system->counters.quota_refusals += 1u;
        static const char text[] = "too many pending delayed sends (maxPendingSends)";
        ta_fault fault = { .code = TA_ERR_NOMEM, .message = { .ptr = text, .len = (uint32_t)(sizeof text - 1u) } };
        ta_result raised = ta_execution_error(it, &fault);
        state->sent = false;
        return raised == TA_EXEC_FAILED ? TA_OK : raised;
    }
    if (state->delay == 0) {
        /* An executing macrostep pays for this mail from its claim, whoever sends it: an invoked child cancelled
         * by its parent's macrostep runs its <onexit> there, and its mail belongs to the parent's input (run.h,
         * ta_run_executing). Outside one (a host call that destroys the session) it is a safe point: the mail is
         * posted from the pool, as a due send is (mail.h), after growing the pool if it can be; if not, mail
         * finds no envelope and the sender hears error.communication. */
        bool own_run = ta_run_executing(system);
        if (!own_run) {
            (void)ta_mail_prepare(system, 1u);
        }
        ta_dispatch dispatch = { .in_run = own_run };
        ta_interpreter_dispatch_delayed(system, &record, &dispatch);
        state->sent = dispatch.sent;
        return TA_OK;
    }
    ta_result result = ta_actor_schedule_send(system, actor, &record);
    if (result != TA_OK) {
        ta_value_release(&record.data);
        ta_value_release(&record.target);
        ta_actor_ref_release(&record.destination);
    }
    state->sent = result == TA_OK;
    return result;
}

/*
 * The SENT record of a committed send: its ID, and the event as it left,
 * whose data the record holds a reference to, since committing gave the
 * send's own away (ADR 0067).
 */
static void ta_trace_sent(ta_interpreter *it, const ta_send_state *state, const ta_value *data) {
    ta_event event = {
        .name = state->event,
        .type = TA_EVENT_EXTERNAL,
        .send_id = state->send_id,
        .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = it->actor->record.self },
        .invoke_id = TA_INVOKE_ID_NONE,
        .data = *data,
    };
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_SENT);
    record.send = state->send_id;
    record.event = &event;
    ta_trace_emit(it, &record);
}

/*
 * <send> in two parts (ADR 0070): evaluate every attribute, which may run
 * out of memory interning a name or taking scratch, then commit. A send
 * that runs out of memory is run again from the start: what it did before
 * is evaluation, and storing the same generated ID again, so the actor's
 * counter moves here, once the send has an outcome.
 */
static ta_result ta_exec_send(ta_interpreter *it, const ta_send_action *send) {
    ta_send_state state = { .io_type = it->system->scxml_io_type, .scxml_io = true };
    ta_fault fault = {0};
    ta_result result = ta_send_evaluate(it, send, &state, &fault);
    if (result == TA_OK) {
        /* Committing gives the data away; SENT reports it afterwards. */
        bool traced = ta_tracing(it, TA_TRACE_SENT);
        ta_value data = state.data;
        if (traced) {
            ta_value_retain(&data);
        }
        result = ta_send_commit(it, &state);
        if (result == TA_OK) {
            TA_COUNT(it->system, it->actor->definition, sends);
            if (state.delay != 0) {
                TA_COUNT(it->system, it->actor->definition, sends_delayed);
            }
        }
        if (traced && result == TA_OK && state.sent) {
            ta_trace_sent(it, &state, &data);
        }
        if (traced) {
            ta_value_release(&data);
        }
    } else {
        ta_send_state_release(&state);
        if (result == TA_EXEC_FAILED) {
            result = ta_send_execution_error(it, state.send_id, &fault);
            if (result == TA_EXEC_FAILED) {
                ta_trace_action_failed(it, &fault);
            }
        } else {
            ta_value_release(&fault.data);
        }
    }
    if (result != TA_ERR_NOMEM && state.generated_id) {
        it->actor->next_send_id += 1u;
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
    ta_op_status snapshot = v->foreach_snapshot(definition->datamodel.self,
                            &(ta_datamodel_foreach_snapshot_params){
                                .shared = definition->datamodel_shared,
                                .instance = it->actor->datamodel_instance,
                                .array = definition->action_defs[loop->cursor].as.foreach.array,
                                .host = &it->actor->host,
                                .out = &loop->iteration_snapshot,
                                .count = &loop->iteration_count,
                                .fault = &fault,
                            });
    if (snapshot == TA_OP_OK) {
        return TA_OK;
    }
    ta_result result = ta_op_failed(it, snapshot, &fault);
    if (result == TA_EXEC_FAILED) {
        ta_trace_action_failed(it, &fault);
    }
    return result;
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
    ta_result result = bound == TA_OP_OK ? TA_OK : ta_op_failed(it, bound, &fault);
    if (result == TA_OK && ta_tracing(it, TA_TRACE_FOREACH)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_FOREACH);
        record.iteration = loop->iteration;
        record.position.iteration = loop->iteration;
        record.has_value = true;
        record.value = loop->iteration_snapshot;
        ta_trace_emit(it, &record);
    } else if (result == TA_EXEC_FAILED) {
        ta_trace_action_failed(it, &fault);
    }
    if (result != TA_ERR_NOMEM) {
        loop->iteration += 1u; /* an iteration whose error could not be queued runs again */
    }
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
                (void)ta_actor_cancel_held_send(it->actor, send_id);
                if (ta_timer_cancel(&it->system->timers, ta_actor_timers(it->actor), send_id) == TA_OK) {
                    TA_COUNT(it->system, it->actor->definition, sends_cancelled);
                }
                if (ta_tracing(it, TA_TRACE_CANCELLED)) {
                    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_CANCELLED);
                    record.send = send_id;
                    ta_trace_emit(it, &record);
                }
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
                if (status == TA_OP_OK && ta_tracing(it, TA_TRACE_ASSIGN)) {
                    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_ASSIGN);
                    record.expr = action->as.assign.location;
                    record.has_value = true;
                    record.value = value;
                    ta_trace_emit(it, &record);
                }
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
            if (status != TA_OP_NOMEM) {
                /* A <script> reports its own run: SCRIPT, with the fault if it failed. */
                ta_result result = TA_OK;
                if (status != TA_OP_OK) {
                    result = ta_op_failed(it, status, &fault);
                    if (result == TA_ERR_NOMEM) {
                        return result;
                    }
                }
                if (ta_tracing(it, TA_TRACE_SCRIPT)) {
                    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_SCRIPT);
                    record.expr = action->as.script;
                    if (status != TA_OP_OK) {
                        ta_trace_fault(&record, &fault);
                    }
                    ta_trace_emit(it, &record);
                }
                /* Like every other action that fails, it also writes
                 * ACTION_FAILED, which a host may want without SCRIPT. */
                if (result == TA_EXEC_FAILED) {
                    ta_trace_action_failed(it, &fault);
                }
                return result;
            }
            break;
        case TA_ACTION_LOG: {
            /*
             * SCXML 4.7: evaluating the expression can fail, which is an
             * ordinary execution error; writing the record cannot, so a
             * <log> never changes interpretation.
             */
            ta_log_record record = { .source = it->actor->record.self, .key = ta_trace_key(it),
                                     .context = it->trace_context };
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
                } else if (ta_actor_replaying(it->actor)) {
                    /* The run being replayed wrote it (ADR 0089 section 3). */
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
    if (status == TA_OP_OK) {
        return TA_OK;
    }
    ta_result result = ta_op_failed(it, status, &fault);
    if (result == TA_EXEC_FAILED) {
        ta_trace_action_failed(it, &fault);
    }
    return result;
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
 * Whether the executor stops before its next unit (ADR 0061): only in a
 * microstep's own block of a bootstrapped actor, never in <finalize> or at
 * termination, and only once this call has run its action budget.
 */
static bool ta_exec_should_pause(const ta_interpreter *it, uint32_t floor) {
    return it->pausable && it->in_microstep && it->actor->bootstrapped && floor == 0 && it->action_budget != 0 &&
           it->actions >= it->action_budget;
}

/*
 * What a unit of executable content needs before it runs (ADR 0070): room
 * for the one internal event it may queue, its own or the error it raises,
 * and for a published actor's <send> with a delay, room to store the send.
 * Running out of memory here stops the run before the unit, with nothing
 * of it done; so a <script> that changed the data model and then failed
 * never runs twice because its error could not be queued.
 */
static ta_result ta_unit_reserve(ta_interpreter *it, const ta_action_def *action) {
    /* Content-sized temporaries live for one unit (ADR 0092 section 3): a
     * <foreach> of many sends reuses the render area instead of filling it. */
    ta_arena_reset(&it->system->render);
    ta_result result = ta_queue_room(it->system, it->actor);
    if (result == TA_OK && action != NULL && action->kind == TA_ACTION_SEND &&
        action->as.send.delay.kind != TA_DELAY_SOURCE_NONE && it->actor->published) {
        result = ta_actor_reserve_send(it->system, it->actor);
    }
    return result;
}

/*
 * Runs a block of executable content and every block nested in it, without
 * recursion (ADR 0060). Each open block is an ACTIONS frame; an <if> opens
 * the block its condition picks, and a <foreach> is a FOREACH frame that
 * opens its body once per position. The frames come from the interpreter's
 * scratch, so nothing here allocates them. An error anywhere stops the whole
 * block, as SCXML 4.9 requires, and every snapshot still held is released.
 *
 * A pause (ADR 0061) and running out of memory (ADR 0070) both stop before
 * a unit and leave the frames open, `at.block_open` set, for the run to
 * save. A unit that runs out of memory had no effect a second run of it
 * would repeat, so its frame is put back to before it.
 */
static ta_result ta_run_block(ta_interpreter *it, ta_span32 block) {
    const ta_definition *definition = it->definition;
    uint32_t floor = it->exec_count;
    ta_result result = TA_OK;
    if (it->at.block_open) {
        /* Resuming: the frames the stop left, from the bottom, are this
         * block's, which a stop only ever leaves at the step's own level. */
        floor = 0;
        it->at.block_open = false;
    } else {
        result = ta_exec_block(it, block);
    }
    while (result == TA_OK && it->exec_count > floor) {
        uint32_t top = it->exec_count - 1u;
        ta_exec_frame *frame = &it->exec_frames[top];
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
            result = ta_unit_reserve(it, NULL);
            if (result == TA_OK) {
                it->actions += 1u;
                it->trace_action = frame->cursor;
                uint32_t mark = ta_queue_mark(it->actor);
                result = ta_foreach_bind(it, frame);
                if (result != TA_ERR_NOMEM) {
                    ta_interpreter_trace_raised(it, mark);
                }
            }
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
        uint32_t index = frame->actions.offset + frame->cursor;
        const ta_action_def *action = &definition->action_defs[index];
        result = ta_unit_reserve(it, action);
        if (result != TA_OK) {
            break;
        }
        it->actions += 1u;
        frame->cursor += 1u;
        it->trace_action = index;
        uint32_t mark = ta_queue_mark(it->actor);
        if (action->kind == TA_ACTION_IF) {
            bool condition = false;
            ta_fault fault = {0};
            ta_op_status status = ta_eval_condition(it, action->as.conditional.condition, &condition, &fault);
            result = status != TA_OP_OK ? ta_op_failed(it, status, &fault) : TA_OK;
            if (result != TA_ERR_NOMEM && ta_tracing(it, TA_TRACE_CONDITION)) {
                ta_trace_record record = ta_trace_record_now(it, TA_TRACE_CONDITION);
                record.expr = action->as.conditional.condition;
                record.result = condition && result == TA_OK;
                if (result != TA_OK) {
                    ta_trace_fault(&record, &fault);
                }
                ta_trace_emit(it, &record);
            }
            if (result == TA_OK) {
                result = ta_exec_block(it, condition ? action->as.conditional.then_actions
                                                     : action->as.conditional.else_actions);
            }
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
        if (result != TA_ERR_NOMEM) {
            /* The unit committed: the events it queued, after its own record. */
            ta_interpreter_trace_raised(it, mark);
        }
        if (result == TA_ERR_NOMEM && it->exec_count == top + 1u) {
            /* Nothing was opened above it: the unit runs again from its start. */
            it->exec_frames[top].cursor -= 1u;
            it->actions -= 1u;
        }
    }
    if (result == TA_ERR_NOMEM && floor == 0 && it->exec_count != 0) {
        it->at.block_open = true;
        return result;
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
    it->trace_action = TA_ACTION_NONE;
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
static void ta_effective_span(ta_interpreter *it, ta_state_list targets, uint64_t *out, uint32_t *pending) {
    for (uint32_t index = 0; index < targets.count; index += 1) {
        ta_state_id target = targets.ids[index];
        if (!ta_is_history(it, target)) {
            ta_bits_put(out, target, true);
        } else if (!ta_bits_get(it->history_seen, target)) {
            ta_bits_put(it->history_seen, target, true);
            it->history_stack[(*pending)++] = target;
        }
    }
}

static void ta_effective_targets(ta_interpreter *it, ta_state_list targets, uint64_t *out) {
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
            ta_effective_span(it, ta_targets(it, fallback->targets), out, &pending);
        }
    }
}

/* False when the transition is targetless and so has no domain. */
static bool ta_transition_domain(ta_interpreter *it, ta_transition_id transition, ta_state_id *out) {
    ta_transition_view view = ta_view(it, transition);
    if (view.targets.count == 0) {
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

/* ta_transition_matches says whether `transition` is selectable for the take: with no event (NULL) only an eventless one is. */
static bool ta_transition_matches(ta_interpreter *it, const ta_transition_def *transition, const ta_event_atoms *event) {
    if (event == NULL) {
        return transition->event_patterns.length == 0;
    }
    for (uint32_t index = 0; index < transition->event_patterns.length; index += 1) {
        if (ta_event_pattern_matches(it->definition,
                                     &it->definition->event_patterns[transition->event_patterns.offset + index],
                                     *event)) {
            return true;
        }
    }
    return false;
}

/*
 * ta_interpreter_resolve_event puts the name of the event just taken into
 * it->event_atoms as atoms, once, so that selection compares atoms and never
 * walks the trie per descriptor (names.md, "Matching without interning"). It
 * is the one place that knows where the name's atoms come from. Until the
 * name area (Phase 4.2) the envelope carries no name bytes, only a trie node,
 * so the atoms are the node's own path (ta_trie_segments). They are not
 * rendered to dotted text and split again: an atom can contain a dot (an
 * invoke ID such as s3.7 is one atom of error.communication.invoke.<id>.<reason>),
 * and splitting would make it two segments and change which descriptors
 * match. 4.2 swaps this source for the envelope's own bytes and ta_name_resolve.
 *
 * Needs: the interpreter initialised (event_atoms has eventNameDepth room,
 * and no name is deeper than that). Guarantees: it never interns, never
 * allocates and never changes the atom table or the trie, so it may run
 * inside a macrostep. A name that is not a name of this system has zero atoms
 * and matches no descriptor.
 */
static void ta_interpreter_resolve_event(ta_interpreter *it, const ta_event *event) {
    ta_atom path[TA_TRIE_MAX_DEPTH];
    uint32_t count = ta_trie_segments(&it->system->event_names.trie, event->name, path);
    if (count > it->event_atom_capacity) {
        count = 0; /* cannot happen: the trie's max_depth is the capacity */
    }
    for (uint32_t index = 0; index < count; index += 1) {
        it->event_atoms[index] = path[index];
    }
    it->event_atom_count = count;
}

/* Selects the optimal enabled transition set; event is null for eventless selection. */
static ta_result ta_select_transitions(ta_interpreter *it, const ta_event *event) {
    const ta_definition *definition = it->definition;
    it->enabled_count = 0;
    it->condition_count = 0;
    if (event != NULL) {
        ta_interpreter_resolve_event(it, event);
    }
    ta_event_atoms atoms = { .atoms = it->event_atoms, .count = it->event_atom_count };
    const ta_event_atoms *taken = event != NULL ? &atoms : NULL;
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
                if (!ta_transition_matches(it, transition, taken)) {
                    continue;
                }
                bool enabled = true;
                if (transition->condition != UINT32_MAX) {
                    ta_fault fault = {0};
                    ta_op_status status = ta_eval_condition(it, transition->condition, &enabled, &fault);
                    struct ta_trace_condition guard = { .transition = id, .expr = transition->condition,
                                                        .result = enabled, .raised = UINT32_MAX };
                    if (status != TA_OP_OK) {
                        enabled = false;
                        ta_result result = ta_op_failed(it, status, &fault);
                        if (result != TA_EXEC_FAILED) {
                            return result;
                        }
                        guard = (struct ta_trace_condition){ .transition = id, .expr = transition->condition,
                                                             .failed = true, .detail = fault.detail,
                                                             .code = fault.code,
                                                             .raised = ta_queue_mark(it->actor) - 1u };
                    }
                    ta_trace_guard(it, &guard);
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
    ta_state_list targets;
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
            ta_span32 fallback = it->definition->transitions[def->transitions.offset].targets;
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = def->parent, .history = state,
                                              .targets = ta_targets(it, fallback) };
        }
        return;
    }
    ta_bits_put(it->to_enter, state, true);
    if (def->kind == TA_STATE_COMPOUND) {
        ta_bits_put(it->default_entry, state, true);
        ta_span32 initial = it->definition->initial_transitions[def->initial].targets;
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = state, .history = TA_STATE_NONE,
                                          .targets = ta_targets(it, initial) };
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
            if (frame->cursor == frame->targets.count) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            ta_state_id target = frame->targets.ids[frame->cursor++];
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
    ta_named_value *values = ta_arena_alloc(&it->system->render, params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        ta_fault full = {0};
        ta_interpreter_render_fault(&full);
        ta_result raised = ta_execution_error(it, &full);
        return raised == TA_EXEC_FAILED ? TA_OK : raised;
    }
    uint32_t count = 0;
    ta_result result = TA_OK;
    for (uint32_t index = 0; index < params.length && result == TA_OK; index += 1) {
        const ta_param_def *param = &definition->params[params.offset + index];
        ta_fault fault = {0};
        values[count].name = ta_atom_text(&it->system->atoms, param->name);
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = param->value,
                                                                     .out = &values[count].value, .fault = &fault });
        if (status == TA_OP_OK) {
            count += 1;
        } else {
            result = ta_op_failed(it, status, &fault);
            result = result == TA_EXEC_FAILED ? TA_OK : result;
        }
    }
    ta_fault fault = {0};
    if (result == TA_OK && count != 0) {
        ta_op_status status = v->make_params == NULL
                                  ? TA_OP_ERROR
                                  : v->make_params(definition->datamodel.self,
                                                   &(ta_datamodel_make_params_params){
                                                       .shared = definition->datamodel_shared,
                                                       .instance = it->actor->datamodel_instance,
                                                       .values = values,
                                                       .count = count,
                                                       .host = &it->actor->host,
                                                       .out = out,
                                                       .fault = &fault,
                                                   });
        if (status != TA_OP_OK) {
            *out = (ta_value){0};
            result = ta_op_failed(it, status, &fault);
            result = result == TA_EXEC_FAILED ? TA_OK : result;
        }
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
    ta_result result = ta_op_failed(it, status, &fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/* Keeps a top-level final state's done data as the actor's completion; the
 * actor holds its sparse state already. */
static void ta_store_completion(ta_interpreter *it, ta_value *data) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_value_release(&sparse->completion.data);
    sparse->completion = (ta_actor_completion){ .has_data = data->v != NULL, .data = *data };
    *data = (ta_value){0};
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

/*
 * Computes the entry set, once, as the microstep begins, after the exit set
 * and the history it records, which is all the entry set depends on: the
 * MICROSTEP record names it before any action runs (ADR 0067).
 */
static void ta_enter_set(ta_interpreter *it) {
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
        for (uint32_t target = 0; target < view.targets.count; target += 1) {
            ta_add_descendants(it, view.targets.ids[target]);
        }
        ta_bits_clear(it, it->entry_targets);
        ta_effective_targets(it, view.targets, it->entry_targets);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (ta_bits_get(it->entry_targets, state)) {
                ta_add_ancestors(it, state, ancestor);
            }
        }
    }
}

/*
 * Done data and done.state events for a final state just entered, or for a
 * top-level one the session's completion. It runs again from its start
 * when it runs out of memory (ADR 0070): its caller takes back the errors
 * and events it queued, and nothing else it does is visible until it
 * succeeds.
 */
static ta_result ta_enter_final(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    if (def->parent == TA_STATE_NONE && ta_sparse(it->system, it->actor) == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_value data = {0};
    ta_result result = ta_done_data_value(it, def->done_data, &data);
    if (result != TA_OK) {
        return result;
    }
    if (def->parent == TA_STATE_NONE) {
        ta_store_completion(it, &data);
        it->actor->sparse->final_state = state; /* ADR 0056 */
        it->actor->finished = true;
        return TA_OK;
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
 * or stopped microstep never activates a state or starts its data twice
 * (ADR 0061, ADR 0070).
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
            /* Invoke: start after the macrostep. Recording it is the only
             * part that allocates, and it changes nothing when it fails. */
            result = ta_invoke_state_entered(it, state);
            if (result != TA_OK) {
                return result;
            }
            ta_bits_put(it->active, state, true);
            it->at.stage = TA_ENTER_DATA;
            it->at.handler = 0;
        }
        if (it->at.stage == TA_ENTER_DATA) {
            /* Late binding: the state's data, once, from the declaration
             * `at.handler` names. */
            if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
                result = ta_initialize_data(it, state, &it->at.handler);
                if (result != TA_OK) {
                    return result;
                }
                ta_bits_put(it->actor->late_initialized, state, true);
            }
            it->at.handler = 0;
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
            uint32_t mark = ta_queue_mark(it->actor);
            result = ta_enter_final(it, state);
            if (result != TA_OK) {
                if (result == TA_ERR_NOMEM) {
                    ta_queue_truncate(it->actor, mark);
                }
                return result;
            }
            ta_interpreter_trace_raised(it, mark);
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

void ta_interpreter_occupancy(ta_actor *actor, bool arrive) {
    ta_definition *definition = (ta_definition *)actor->definition;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_configuration_contains_atomic(&actor->configuration, state)) {
            uint32_t *count = &definition->occupancy[state];
            *count = arrive ? *count + 1u : (*count == 0 ? 0 : *count - 1u);
        }
    }
}

static void ta_store_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_interpreter_occupancy(it->actor, false);
    ta_configuration_clear(&it->actor->configuration);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_definition_is_atomic(definition, state)) {
            ta_configuration_set_atomic(&it->actor->configuration, state, true);
        }
    }
    ta_interpreter_occupancy(it->actor, true);
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

/* ------------------------------------------------------------------------
 * Running a microstep
 */

/* Opens the microstep of the enabled transitions: the exit set, history
 * recorded before anything exits, and the entry set; then its MICROSTEP
 * record. It stands at its start (ADR 0086). It changes the actor, so an
 * image taken before it no longer describes it. `event` is the event it
 * takes, or null. */
static void ta_microstep_begin(ta_interpreter *it, const ta_event *event) {
    ta_actor_touch(it->actor);
    it->at = (ta_micro_position){ .phase = TA_PHASE_START };
    ta_exit_begin(it);
    ta_enter_set(it);
    ta_trace_microstep(it, event);
}

/* Passes a microstep's start: its first state to exit is next. */
static void ta_microstep_pass_start(ta_interpreter *it) {
    it->at = (ta_micro_position){ .phase = TA_PHASE_EXIT, .state = it->definition->state_count,
                                  .stage = TA_EXIT_HANDLERS };
}

/*
 * Runs the open microstep from where `at` stands. A pause (TA_EXEC_PAUSED)
 * or a failed allocation (TA_ERR_NOMEM) leaves it open: the position, the
 * sets and the frames stay in the interpreter for the run to save, and the
 * actor's configuration is still the one before the microstep (ADR 0061,
 * ADR 0070).
 */
static ta_result ta_microstep(ta_interpreter *it) {
    if (it->at.phase == TA_PHASE_START) {
        /* A microstep this call began; one it resumed at its start has
         * passed it already. Nothing has run, so stopping here needs no
         * block open (ADR 0086). */
        if (it->pause_at_start && it->actor->bootstrapped) {
            return TA_EXEC_PAUSED;
        }
        ta_microstep_pass_start(it);
    }
    ta_result result = TA_OK;
    it->in_microstep = true;
    if (it->at.phase == TA_PHASE_EXIT) {
        result = ta_exit_states(it);
        if (result == TA_OK) {
            it->at.phase = TA_PHASE_TRANSITIONS;
            it->at.state = 0;
        }
    }
    if (result == TA_OK && it->at.phase == TA_PHASE_TRANSITIONS) {
        for (; result == TA_OK && it->at.state < it->enabled_count; it->at.state += 1u) {
            result = ta_run_block_isolated(it, ta_view(it, it->enabled[it->at.state]).actions);
            if (result != TA_OK) {
                break;
            }
        }
        if (result == TA_OK) {
            it->at.phase = TA_PHASE_ENTER;
            it->at.state = 0;
            it->at.stage = TA_ENTER_ACTIVATE;
        }
    }
    if (result == TA_OK && it->at.phase == TA_PHASE_ENTER) {
        result = ta_enter_states(it);
    }
    it->in_microstep = false;
    if (result == TA_EXEC_PAUSED || result == TA_ERR_NOMEM) {
        return result;
    }
    it->at = (ta_micro_position){0};
    ta_store_active(it);
    it->steps += 1u;
    /* The first microstep an actor completes is its bootstrap's. */
    it->actor->bootstrapped = true;
    return result;
}

/*
 * A selection for an event completed: its guards are written, then the
 * microstep of its transitions, or, when none is enabled, the event is
 * consumed.
 */
static void ta_after_selection(ta_interpreter *it, uint32_t mark) {
    ta_trace_selection(it, mark);
    ta_actor *actor = it->actor;
    bool external = actor->event_block != NULL && actor->event_block->slots.bound == TA_BOUND_EXTERNAL;
    if (external) {
        TA_COUNT(it->system, actor->definition, events_taken);
    }
    if (it->enabled_count != 0) {
        ta_microstep_begin(it, ta_actor_bound_event(actor));
        return;
    }
    /* The event is consumed and enabled nothing (ADR 0072). */
    if (external) {
        actor->event_block->slots.external_ignored = true;
        TA_COUNT(it->system, actor->definition, events_ignored);
    } else {
        TA_COUNT(it->system, actor->definition, internal_ignored);
    }
    if (ta_tracing(it, TA_TRACE_IGNORED)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_IGNORED);
        record.event = ta_actor_bound_event(actor);
        ta_trace_emit(it, &record);
    }
    it->at = (ta_micro_position){0};
    it->steps += 1u;
}

/*
 * Selects the transitions for an event just taken. Nothing has run for it
 * yet, so running out of memory gives it back, and takes back the errors
 * its guards queued: the next run takes it again (ADR 0069, ADR 0070). A
 * selection that completes commits the take, and so the macrostep.
 */
static ta_result ta_select_taken(ta_interpreter *it) {
    /* A copy: an error.execution a guard raises can grow the queue, which
     * moves a bound queue head. */
    ta_event taken = *ta_actor_bound_event(it->actor);
    uint32_t mark = ta_queue_mark(it->actor);
    ta_result result = ta_select_transitions(it, &taken);
    if (result != TA_OK) {
        if (result == TA_ERR_NOMEM) {
            ta_queue_truncate(it->actor, mark);
            ta_unbind_event(it->actor);
        }
        return result;
    }
    ta_trace_begin(it);
    ta_after_selection(it, mark);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Phases outside a microstep (ADR 0070)
 */

/*
 * The bootstrap's data (SCXML 5.2): declares every datum, then initializes
 * the top-level data and, with early binding, every state's, from where
 * `at` stands; then the top-level <script>.
 */
static ta_result ta_run_data(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_result result = TA_OK;
    if (it->at.stage == TA_DATA_DECLARE) {
        result = ta_declare_data(it, &it->at.handler);
        if (result != TA_OK) {
            return result;
        }
        it->at.stage = TA_DATA_INITIALIZE;
        it->at.state = 0;
        it->at.handler = 0;
    }
    uint32_t owners = definition->binding_mode == TA_BINDING_EARLY ? definition->state_count + 1u : 1u;
    for (; it->at.state < owners; it->at.state += 1u, it->at.handler = 0) {
        ta_state_id owner = it->at.state == 0 ? TA_STATE_NONE : it->at.state - 1u;
        result = ta_initialize_data(it, owner, &it->at.handler);
        if (result != TA_OK) {
            return result;
        }
    }
    it->at = (ta_micro_position){ .phase = TA_PHASE_SCRIPT };
    return TA_OK;
}

/* The bootstrap's top-level <script> (SCXML 5.8), then its initial microstep. */
static ta_result ta_run_script(ta_interpreter *it) {
    ta_result result = ta_run_block_isolated(it, it->definition->global_script_actions);
    if (result != TA_OK) {
        return result;
    }
    it->enabled[0] = TA_ENABLED_BOOTSTRAP;
    it->enabled_count = 1;
    ta_microstep_begin(it, NULL);
    return TA_OK;
}

/*
 * An external event was taken (SCXML mainEventLoop): the <finalize> of the
 * invocation it came from, then autoforwarding it to every invocation that
 * asks for that; its transitions are selected after both.
 */
static ta_result ta_run_finalize(ta_interpreter *it) {
    /* A copy: <finalize> may grow the internal queue, which can move the
     * bound event (ADR 0069). The slot keeps the data retained. */
    const ta_event event = *ta_actor_bound_event(it->actor);
    if (it->at.stage == TA_FINALIZE_BLOCK) {
        ta_span32 finalize = {0};
        if (!it->at.block_open) {
            (void)ta_invoke_finalize_actions(it->actor, &event, &finalize);
        }
        ta_result result = ta_run_block_isolated(it, finalize);
        if (result != TA_OK) {
            return result;
        }
        it->at.stage = TA_FINALIZE_FORWARD;
        it->at.state = 0;
    }
    ta_result result = ta_invoke_autoforward(it, &event, &it->at.state);
    if (result != TA_OK) {
        return result;
    }
    it->at = (ta_micro_position){ .phase = TA_PHASE_SELECT };
    return TA_OK;
}

/*
 * Selects the transitions for the external event <finalize> ran for. The
 * event cannot be given back any more, so running out of memory stops the
 * run here, and the next one selects again.
 */
static ta_result ta_run_select(ta_interpreter *it) {
    ta_event taken = *ta_actor_bound_event(it->actor);
    uint32_t mark = ta_queue_mark(it->actor);
    ta_result result = ta_select_transitions(it, &taken);
    if (result != TA_OK) {
        if (result == TA_ERR_NOMEM) {
            ta_queue_truncate(it->actor, mark);
        }
        return result;
    }
    ta_after_selection(it, mark);
    return TA_OK;
}

/* Starts the invocations of the states the macrostep entered, from the
 * state and <invoke> `at` names. */
static ta_result ta_run_invokes(ta_interpreter *it) {
    ta_trace_begin(it);
    ta_result result = ta_invoke_start_pending(it, &it->at.state, &it->at.handler);
    if (result != TA_OK) {
        return result;
    }
    it->at = (ta_micro_position){0};
    it->after_invokes = true;
    return TA_OK;
}

/*
 * A top-level final state was entered (SCXML exitInterpreter): every
 * active state is exited, highest first, running its <onexit>; then the
 * session ends, and an invoked one reports done.invoke to its parent. The
 * end runs again when it runs out of memory storing that report, and
 * repeats nothing visible: the invocations it cancelled are gone.
 */
static ta_result ta_run_finish(ta_interpreter *it) {
    if (it->at.stage == TA_FINISH_EXIT) {
        for (; it->at.state > 0; it->at.state -= 1u) {
            ta_state_id state = it->at.state - 1u;
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
        it->at.stage = TA_FINISH_END;
    }
    /* Every send still pending (a delayed one) is cancelled: the session ends here. */
    ta_timer_cancel_all(&it->system->timers, ta_actor_timers(it->actor));
    ta_note_ignored(it);
    ta_release_events(it->system, it->actor);
    /* Invoke: cancel this session's invocations and report done.invoke to its parent. */
    ta_result result = ta_invoke_finished(it);
    if (result != TA_OK) {
        return result;
    }
    it->at = (ta_micro_position){0};
    ta_trace_end(it);
    ta_actor *actor = it->actor;
    /* A lifecycle record: its own key, and the step that finished as cause. */
    ta_step_key step = ta_trace_key(it);
    ta_step_key key = ta_trace_lifecycle_key(actor);
    TA_COUNT(it->system, actor->definition, finished);
    if (ta_trace_lifecycle_wanted(&it->system->trace_masks, TA_TRACE_FINISHED, &it->trace_context)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_FINISHED);
        record.key = key;
        record.cause = step;
        record.position = (ta_trace_position){ .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE,
                                               .transition = TA_TRANSITION_NONE, .action = TA_ACTION_NONE };
        record.state = actor->sparse != NULL ? actor->sparse->final_state : TA_STATE_NONE;
        ta_trace_emit(it, &record);
    }
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_DONE;
    return TA_OK;
}

/* Continues the phase `at` names. */
static ta_result ta_continue_phase(ta_interpreter *it) {
    switch (it->at.phase) {
        case TA_PHASE_START:
        case TA_PHASE_EXIT:
        case TA_PHASE_TRANSITIONS:
        case TA_PHASE_ENTER:
            return ta_microstep(it);
        case TA_PHASE_DATA:
            return ta_run_data(it);
        case TA_PHASE_SCRIPT:
            return ta_run_script(it);
        case TA_PHASE_FINALIZE:
            return ta_run_finalize(it);
        case TA_PHASE_SELECT:
            return ta_run_select(it);
        case TA_PHASE_INVOKES:
            return ta_run_invokes(it);
        case TA_PHASE_FINISH:
            return ta_run_finish(it);
        case TA_PHASE_NONE:
        default:
            return TA_ERR_INVALID;
    }
}

/*
 * Takes an external event that came with an operator transition (ADR 0082):
 * the transition, not selection, takes it, as the macrostep's first step.
 * Nothing here allocates, so there is nothing to give back, and taking it
 * commits the macrostep. <finalize> and autoforwarding do not run, since
 * no chart transition sees the event. Once the microstep has begun, its
 * sets hold all it needs, and the operator transition is freed.
 */
static void ta_take_jump(ta_interpreter *it) {
    ta_actor *actor = it->actor;
    ta_bind_event(actor, TA_BOUND_EXTERNAL);
    ta_trace_begin(it);
    TA_COUNT(it->system, actor->definition, events_taken);
    ta_event_slots *slots = &actor->event_block->slots;
    it->jump = slots->jump;
    it->enabled[0] = TA_ENABLED_OPERATOR;
    it->enabled_count = 1;
    ta_microstep_begin(it, ta_actor_bound_event(actor));
    it->jump = NULL;
    ta_jump_free(it->system, slots);
}

/*
 * Nothing is open: the next step of the macrostep (SCXML mainEventLoop).
 * An eventless transition first, then the next internal event, then the
 * invocations of the states the macrostep entered, then the next external
 * event. Right after the invocations started, eventless transitions are
 * selected again only if they queued an internal event. A macrostep that
 * starts with a delivered event takes it first: the last one ended stable,
 * so SCXML selects no eventless transition before it. With nothing left,
 * the macrostep has ended and `*stable` is set.
 */
static ta_result ta_next_step(ta_interpreter *it, bool *stable) {
    ta_actor *actor = it->actor;
    bool external_waiting =
        actor->event_block != NULL && actor->event_block->slots.external == TA_EXTERNAL_WAITING;
    if (external_waiting && !actor->trace_begun && actor->event_block->slots.jump != NULL) {
        /* An operator transition is the first microstep of its macrostep,
         * before internal events queued since it was delivered, so it runs
         * from the configuration it was checked against. */
        ta_take_jump(it);
        return TA_OK;
    }
    bool skip_to_external = !ta_interpreter_internal_pending(actor) &&
                            (it->after_invokes || (external_waiting && !actor->trace_begun));
    it->after_invokes = false;
    if (!skip_to_external) {
        uint32_t mark = ta_queue_mark(actor);
        ta_result result = ta_select_transitions(it, NULL);
        if (result != TA_OK) {
            if (result == TA_ERR_NOMEM) {
                ta_queue_truncate(actor, mark);
            }
            return result;
        }
        /* A selection that completes is the macrostep's first step, if
         * nothing else was. */
        ta_trace_begin(it);
        ta_trace_selection(it, mark);
        if (it->enabled_count != 0) {
            ta_microstep_begin(it, NULL);
            return TA_OK;
        }
        if (ta_interpreter_internal_pending(actor)) {
            ta_bind_event(actor, TA_BOUND_INTERNAL);
            return ta_select_taken(it);
        }
        if ((actor->published || ta_actor_replaying(actor)) && ta_invoke_has_pending(actor)) {
            it->at = (ta_micro_position){ .phase = TA_PHASE_INVOKES };
            return TA_OK;
        }
    }
    if (actor->event_block != NULL && actor->event_block->slots.external == TA_EXTERNAL_WAITING) {
        /* It stays in its slot, where inspection reports it, until the
         * macrostep ends. */
        ta_bind_event(actor, TA_BOUND_EXTERNAL);
        /* Invoke: <finalize> and autoforwarding. The event cannot be given
         * back once they start, so taking it commits the macrostep. */
        if (ta_invoke_before_external(actor, ta_actor_bound_event(actor))) {
            it->at = (ta_micro_position){ .phase = TA_PHASE_FINALIZE, .stage = TA_FINALIZE_BLOCK };
            ta_trace_begin(it);
            return TA_OK;
        }
        return ta_select_taken(it);
    }
    ta_compact_sparse(it);
    ta_trace_end(it);
    ta_note_ignored(it);
    ta_end_macrostep_events(it->system, actor);
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    *stable = true;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Stopping inside a phase (ADR 0061, ADR 0070)
 */

/* The executor's frames: an open block per level, and a <foreach> frame
 * under each body (ADR 0060). */
static uint32_t ta_exec_capacity(const ta_definition *definition) {
    return 2u * definition->max_action_depth + 1u;
}

/* The words of a set over the definition's data declarations; never 0. */
static uint32_t ta_decl_words(const ta_definition *definition) {
    return ta_bits_words(definition->data_decl_count) + 1u;
}

/*
 * ta_continuation_capacity describes the largest continuation a run of this
 * definition can save: every set, every enabled transition, and every frame
 * the executor can open. It fills `*shape` with those counts and the offsets
 * of its arrays, and `*bytes` with its size. It answers TA_ERR_NOMEM, with
 * the outputs untouched, when that does not fit (ta_continuation_bytes). It
 * never allocates; it is called where a continuation is allocated or loaded
 * (ta_reserve_stop, the restore of an image), and the result is stored.
 */
ta_result ta_continuation_capacity(const ta_definition *definition, ta_continuation *shape, size_t *bytes) {
    ta_continuation capacity = {
        .words = ta_bits_words(definition->state_count),
        .decl_words = ta_decl_words(definition),
        .state_count = definition->state_count,
        .enabled_count = definition->state_count + 1u,
        .frame_count = ta_exec_capacity(definition),
    };
    size_t total = 0;
    ta_result result = ta_continuation_bytes(&capacity, &total, &capacity.offsets);
    if (result == TA_OK) {
        *shape = capacity;
        *bytes = total;
    }
    return result;
}

static void ta_continuation_forget(ta_system *system, ta_continuation *continuation) {
    /* Its frames moved into scratch or were never there: nothing to release. */
    ta_mem_free(system->allocator, continuation,
                &(ta_allocator_free_params){ .size = continuation->bytes, .alignment = TA_CONTINUATION_ALIGN });
}

/* Gives a continuation whose frames moved back into scratch to the system
 * as its spare, keeping the larger of the two. */
static void ta_continuation_recycle(ta_system *system, ta_continuation *continuation) {
    ta_continuation *spare = system->spare_continuation;
    if (spare != NULL && spare->bytes >= continuation->bytes) {
        ta_continuation_forget(system, continuation);
        return;
    }
    if (spare != NULL) {
        ta_continuation_forget(system, spare);
    }
    system->spare_continuation = continuation;
}

void ta_interpreter_release_spares(ta_system *system) {
    if (system->spare_continuation != NULL) {
        ta_continuation_forget(system, system->spare_continuation);
        system->spare_continuation = NULL;
    }
    if (system->spare_sparse != NULL) {
        ta_mem_free(system->allocator, system->spare_sparse,
                    &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                                 .alignment = _Alignof(ta_actor_sparse_state) });
        system->spare_sparse = NULL;
    }
}

/*
 * Makes sure a run of the actor can stop anywhere without allocating
 * (ADR 0070): a continuation large enough for its definition and, for an
 * actor without one, a sparse state to hang it from. An actor that stopped
 * before stops into its own continuation again. TA_ERR_NOMEM here happens
 * before the run does anything.
 */
static ta_result ta_reserve_stop(ta_system *system, ta_actor *actor) {
    if (actor->sparse != NULL && actor->sparse->paused != NULL) {
        return TA_OK;
    }
    ta_continuation shape;
    size_t bytes = 0;
    ta_result result = ta_continuation_capacity(actor->definition, &shape, &bytes);
    if (result != TA_OK) {
        return result;
    }
    ta_continuation *spare = system->spare_continuation;
    if (spare == NULL || spare->bytes < bytes) {
        ta_continuation *grown = ta_mem_alloc(system->allocator, bytes, TA_CONTINUATION_ALIGN);
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        *grown = (ta_continuation){ .bytes = bytes };
        if (spare != NULL) {
            ta_continuation_forget(system, spare);
        }
        system->spare_continuation = grown;
        spare = grown;
    }
    /* A spare that is reused may have been laid out for another definition:
     * its offsets are rewritten for this one, and fit because spare->bytes
     * is at least the bytes this shape needs. */
    spare->offsets = shape.offsets;
    if (actor->sparse == NULL && system->spare_sparse == NULL) {
        system->spare_sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                            _Alignof(ta_actor_sparse_state));
        if (system->spare_sparse == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    return TA_OK;
}

/*
 * Stops the run where `at` stands: the position, the sets and the frames
 * move out of scratch into a continuation the actor owns, the one it
 * resumed from or the system's spare, which ta_reserve_stop provided, so
 * stopping cannot fail. The frames' snapshots move with them.
 */
static void ta_stop(ta_interpreter *it) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    if (actor->sparse == NULL) {
        actor->sparse = system->spare_sparse;
        system->spare_sparse = NULL;
        *actor->sparse = (ta_actor_sparse_state){ .final_state = TA_STATE_NONE };
    }
    ta_continuation *saved = it->resumed;
    it->resumed = NULL;
    if (saved == NULL) {
        saved = system->spare_continuation;
        system->spare_continuation = NULL;
    }
    /* The offsets stay: they were set for the capacity shape when the
     * continuation was reserved or loaded, and the counts written here never
     * exceed it. */
    *saved = (ta_continuation){ .bytes = saved->bytes, .at = it->at, .words = it->words,
                                .decl_words = it->decl_words, .state_count = it->definition->state_count,
                                .enabled_count = it->enabled_count, .frame_count = it->exec_count,
                                .offsets = saved->offsets };
    ta_mem_copy(ta_continuation_decls(saved), it->bound_decls, (size_t)it->decl_words * sizeof(uint64_t));
    uint64_t *sets = ta_continuation_sets(saved);
    const uint64_t *from[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(from); index += 1u) {
        ta_mem_copy(sets + (size_t)index * it->words, from[index], (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(ta_continuation_history(saved), it->default_history,
                (size_t)saved->state_count * sizeof(ta_state_id));
    ta_mem_copy(ta_continuation_enabled(saved), it->enabled, (size_t)saved->enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(ta_continuation_frames(saved), it->exec_frames, (size_t)saved->frame_count * sizeof(ta_exec_frame));
    actor->sparse->paused = saved;
    it->exec_count = 0;
    it->at = (ta_micro_position){0};
}

/* Moves a stopped run back into scratch. The actor no longer holds the
 * continuation; the run keeps it, to stop into again or to recycle. */
static void ta_stop_restore(ta_interpreter *it) {
    ta_continuation *saved = it->actor->sparse->paused;
    const uint64_t *sets = ta_continuation_sets(saved);
    uint64_t *to[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(to); index += 1u) {
        ta_mem_copy(to[index], sets + (size_t)index * it->words, (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(it->bound_decls, ta_continuation_decls(saved), (size_t)saved->decl_words * sizeof(uint64_t));
    ta_mem_copy(it->default_history, ta_continuation_history(saved), (size_t)saved->state_count * sizeof(ta_state_id));
    ta_mem_copy(it->enabled, ta_continuation_enabled(saved), (size_t)saved->enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(it->exec_frames, ta_continuation_frames(saved), (size_t)saved->frame_count * sizeof(ta_exec_frame));
    it->enabled_count = saved->enabled_count;
    it->exec_count = saved->frame_count;
    it->at = saved->at;
    it->actor->sparse->paused = NULL;
    it->resumed = saved;
}

/* Drops a restored stop without finishing it: its snapshots are released,
 * and its continuation goes back to the system. */
static void ta_stop_drop(ta_interpreter *it) {
    while (it->exec_count > 0) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    it->at = (ta_micro_position){0};
    if (it->resumed != NULL) {
        ta_continuation_recycle(it->system, it->resumed);
        it->resumed = NULL;
    }
}

/* One allocation of ta_interpreter_init, added to a layout the way an
 * arena lays it out: `count` items of `type`, at its alignment. */
#define TA_SCRATCH_TAKE(layout, type, items)                                                      \
    ta_layout_add((layout), &(ta_layout_entry){ .size = sizeof(type), .alignment = _Alignof(type), \
                                                .count = (items) })

/*
 * The bytes of interpreter scratch a definition needs (ADR 0092 section 3):
 * everything ta_interpreter_init takes, in its order and alignment, with
 * both trace buffers, whether or not a sink wants them. The compiler checks
 * it against interpreterBytes, so a definition that registers never runs
 * out of structural scratch.
 *
 * The figure is a pure computation with call lifetime: it allocates nothing
 * and keeps nothing. It must stay in lock-step with ta_interpreter_init,
 * entry for entry, because the arena pads each allocation to its alignment
 * from the start of its data; a transposed pair would let a chart register
 * with too little scratch. When a size does not fit in a size_t it answers
 * TA_ERR_NOMEM and writes nothing to `*out`; the compiler treats that as
 * "needs more than any system has".
 */
ta_result ta_interpreter_scratch_need(const ta_definition *definition, size_t *out) {
    /* Positions are counted from the block's data, which starts aligned to
     * the block's own alignment, as ta_arena_init_fixed lays it out. */
    size_t start = offsetof(ta_arena_block, data);
    ta_layout layout;
    ta_layout_init(&layout, start);
    uint32_t words = ta_bits_words(definition->state_count);
    for (uint32_t index = 0; index < 11u; index += 1u) {
        TA_SCRATCH_TAKE(&layout, uint64_t, words);
    }
    size_t capacity = (size_t)definition->state_count + 1u;
    TA_SCRATCH_TAKE(&layout, ta_state_id, capacity);
    TA_SCRATCH_TAKE(&layout, ta_transition_id, capacity);
    TA_SCRATCH_TAKE(&layout, ta_transition_id, capacity);
    TA_SCRATCH_TAKE(&layout, bool, capacity);
    TA_SCRATCH_TAKE(&layout, struct ta_entry_frame, ta_entry_capacity(definition));
    TA_SCRATCH_TAKE(&layout, ta_state_id, (size_t)definition->history.slot_count + 1u);
    TA_SCRATCH_TAKE(&layout, ta_exec_frame, ta_exec_capacity(definition));
    TA_SCRATCH_TAKE(&layout, uint64_t, ta_decl_words(definition));
    TA_SCRATCH_TAKE(&layout, ta_atom, definition->system->event_names.trie.max_depth);
    /* 3 * state_count + 1 items, as four entries so that no count wraps: same alignment, no gap between. */
    for (uint32_t index = 0; index < 3u; index += 1u) {
        TA_SCRATCH_TAKE(&layout, ta_state_id, definition->state_count);
    }
    TA_SCRATCH_TAKE(&layout, ta_state_id, 1u);
    TA_SCRATCH_TAKE(&layout, struct ta_trace_condition, (size_t)ta_trace_guard_capacity(definition) + 1u);
    size_t end;
    ta_result result = ta_layout_finish(&layout, &end, NULL);
    if (result == TA_OK) {
        *out = end - start;
    }
    return result;
}

static ta_result ta_interpreter_init(ta_interpreter *it, ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    uint32_t words = ta_bits_words(definition->state_count);
    *it = (ta_interpreter){ .system = system, .actor = actor, .definition = definition, .words = words };
    ta_arena_reset(&system->scratch);
    ta_arena_reset(&system->render);
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
    it->exec_capacity = ta_exec_capacity(definition);
    it->exec_frames = ta_arena_alloc(&system->scratch, it->exec_capacity * sizeof(ta_exec_frame), _Alignof(ta_exec_frame));
    it->decl_words = ta_decl_words(definition);
    it->bound_decls = ta_arena_alloc(&system->scratch, (size_t)it->decl_words * sizeof(uint64_t), _Alignof(uint64_t));
    /* The taken event's atoms: eventNameDepth of them, the longest name there is. */
    it->event_atom_capacity = system->event_names.trie.max_depth;
    it->event_atoms = ta_arena_alloc(&system->scratch, (size_t)it->event_atom_capacity * sizeof(ta_atom),
                                     _Alignof(ta_atom));
    if (it->entry_frames == NULL || it->history_stack == NULL || it->exec_frames == NULL || it->bound_decls == NULL ||
        it->event_atoms == NULL) {
        return TA_ERR_NOMEM;
    }
    /* Tracing (ADR 0067): what this macrostep builds, and the scratch its
     * records need, taken only when the sink wants those records. */
    ta_trace_parent parent = ta_actor_macrostep_parent(actor);
    it->trace_context = parent.context;
    it->trace_kinds = ta_trace_macrostep_kinds(&system->trace_masks, &parent.context);
    it->trace_action = TA_ACTION_NONE;
    uint32_t wanted = ta_trace_any_kinds(&system->trace_masks);
    if ((wanted & (TA_TRACE_BIT(TA_TRACE_MICROSTEP) | TA_TRACE_BIT(TA_TRACE_MACROSTEP_END) |
                   TA_TRACE_BIT(TA_TRACE_LONG_MACROSTEP) | TA_TRACE_BIT(TA_TRACE_RUNAWAY))) != 0) {
        it->trace_states = ta_arena_alloc(&system->scratch, (3u * (size_t)definition->state_count + 1u) *
                                                                sizeof(ta_state_id),
                                          _Alignof(ta_state_id));
        if (it->trace_states == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    if ((wanted & TA_TRACE_BIT(TA_TRACE_CONDITION)) != 0) {
        it->condition_capacity = ta_trace_guard_capacity(definition);
        it->conditions = ta_arena_alloc(&system->scratch,
                                        ((size_t)it->condition_capacity + 1u) * sizeof(struct ta_trace_condition),
                                        _Alignof(struct ta_trace_condition));
        if (it->conditions == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    ta_bits_clear(it, it->history_busy);
    ta_load_active(it);
    if (actor->sparse != NULL && actor->sparse->paused != NULL) {
        ta_stop_restore(it); /* the live set, too, is the stopped run's */
    }
    actor->live_active = it->active;
    return TA_OK;
}

/* PAUSED, RESUMED or STOPPED, for a macrostep that has begun, at the
 * position the run stands at: the one ta_actor_inspect() reports while it
 * is stopped (ADR 0083). */
static void ta_trace_control(ta_interpreter *it, ta_trace_kind kind) {
    if (kind == TA_TRACE_PAUSED) {
        TA_COUNT(it->system, it->actor->definition, runs_paused);
    } else if (kind == TA_TRACE_STOPPED) {
        TA_COUNT(it->system, it->actor->definition, runs_stopped);
    }
    if (it->actor->trace_begun && ta_tracing(it, kind)) {
        ta_trace_record record = ta_trace_record_now(it, kind);
        ta_run_view view = ta_interpreter_view(it);
        record.position = ta_run_position(it->definition, &view);
        ta_trace_emit(it, &record);
    }
}

/*
 * Runs the actor until its macrostep ends, the microstep budget is spent,
 * or a pause or a failed allocation stops it inside a phase, where the
 * next run continues (ADR 0047, ADR 0061, ADR 0070). A failed allocation
 * with nothing open, while selecting transitions, leaves nothing to
 * continue: the next run selects again. A macrostep that has run its
 * microstep limit, between microsteps with nothing open, ends as RUNAWAY
 * (ADR 0075, ADR 0099 section 7): the caller abandons it.
 */
static ta_result ta_run_loop(ta_interpreter *it, uint32_t budget, ta_step_status *status) {
    ta_actor *actor = it->actor;
    for (;;) {
        ta_result result = TA_OK;
        if (it->at.phase != TA_PHASE_NONE) {
            result = ta_continue_phase(it);
        } else if (!actor->bootstrapped) {
            it->at = (ta_micro_position){ .phase = TA_PHASE_DATA, .stage = TA_DATA_DECLARE };
            ta_mem_zero(it->bound_decls, (size_t)it->decl_words * sizeof(uint64_t));
            /* The bootstrap has no event to give back: it begins here. */
            ta_trace_begin(it);
        } else if (actor->finished) {
            if (actor->record.residency == TA_ACTOR_DONE) {
                *status = TA_STEP_DONE;
                return TA_OK;
            }
            it->at = (ta_micro_position){ .phase = TA_PHASE_FINISH, .state = it->definition->state_count,
                                          .stage = TA_FINISH_EXIT };
        } else if (it->microstep_limit != 0 && actor->trace_begun && actor->trace_microstep >= it->microstep_limit) {
            /* Between microsteps, nothing open: the caller abandons the macrostep. */
            *status = TA_STEP_RUNAWAY;
            return TA_OK;
        } else if (it->steps >= budget) {
            ta_trace_control(it, TA_TRACE_PAUSED);
            *status = TA_STEP_PAUSED;
            return TA_OK;
        } else {
            bool stable = false;
            result = ta_next_step(it, &stable);
            if (result == TA_OK && stable) {
                *status = TA_STEP_STABLE;
                return TA_OK;
            }
        }
        if (result == TA_OK) {
            continue;
        }
        if (result == TA_EXEC_PAUSED) {
            ta_trace_control(it, TA_TRACE_PAUSED);
            ta_stop(it);
            *status = TA_STEP_PAUSED;
            return TA_OK;
        }
        if (result == TA_ERR_NOMEM) {
            ta_trace_control(it, TA_TRACE_STOPPED);
        }
        if (result == TA_ERR_NOMEM && it->at.phase != TA_PHASE_NONE) {
            ta_stop(it);
        }
        return result;
    }
}

ta_result ta_interpreter_run_input(ta_system *system, ta_actor *actor, const ta_input_run *run,
                                   ta_input_result *out) {
    *out = (ta_input_result){ .status = TA_STEP_STABLE };
    if (actor->finished && (actor->sparse == NULL || actor->sparse->paused == NULL)) {
        out->status = TA_STEP_DONE;
        return TA_OK;
    }
    if (!actor->macrostep) {
        /* Idle: its last macrostep ended stable, and nothing waits. */
        return TA_OK;
    }
    ta_interpreter interpreter;
    ta_interpreter *it = &interpreter;
    ta_result result = ta_reserve_stop(system, actor);
    if (result == TA_OK) {
        result = ta_take_macrostep_time(system, actor);
    }
    if (result == TA_OK) {
        result = ta_interpreter_init(it, system, actor);
    }
    if (result != TA_OK) {
        return result;
    }
    it->pausable = true;
    it->action_budget = run->action_budget;
    uint32_t limit = actor->definition->microstep_limit == 0 ? system->params[TA_PARAM_MICROSTEP_LIMIT]
                                                            : actor->definition->microstep_limit;
    it->microstep_limit = limit == TA_MICROSTEP_LIMIT_NONE ? 0 : limit;
    it->pause_at_start = run->pause_at_start;
    if (it->at.phase != TA_PHASE_NONE) {
        /* Continuing a stopped run changes the actor again, so an image
         * taken while it was stopped no longer describes it. */
        ta_actor_touch(actor);
    }
    ta_trace_control(it, TA_TRACE_RESUMED);
    if (it->at.phase == TA_PHASE_START) {
        /* It stopped at a microstep's start: this call passes it. */
        ta_microstep_pass_start(it);
    }
    result = ta_run_loop(it, run->budget, &out->status);
    out->steps = it->steps;
    if (result == TA_OK && (out->status == TA_STEP_STABLE || out->status == TA_STEP_DONE)) {
        out->ignored = it->ended_ignored;
    }
    /* The live set lives in scratch memory; between calls In() derives from the configuration. */
    actor->live_active = NULL;
    if (it->resumed != NULL) {
        ta_continuation_recycle(system, it->resumed);
    }
    /* An empty queue goes back; queued events or a stopped run keep it. */
    ta_block_settle(system, actor);
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
    ta_arena render;
    ta_state_id next;     /* states left to exit: those below `next`, highest first */
    ta_state_id exiting;  /* the state whose invocations are being cancelled */
    uint32_t cursor;      /* its invocations left: those below `cursor` */
    bool waiting;         /* `taken`'s child is being exited above this frame */
    ta_invoke_taken taken;
} ta_exit_frame;

/*
 * Starts exiting a session: a nested interpreter region of its own (ADR
 * 0092 section 3), region `frames->count + 1`, so the interpreter that
 * cancelled it keeps its scratch, and an interpreter over it. The region is
 * allocated the first time an exit reaches this depth, and kept. Nothing
 * happens when the session is gone or finished, when the depth passes
 * interpreterNesting, or when there is no memory for the region.
 */
static void ta_exit_open(ta_system *system, ta_vec *frames, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL || actor->finished) {
        return;
    }
    ta_exit_frame frame = { .actor = actor, .exiting = TA_STATE_NONE };
    if (ta_system_nested_region(system, frames->count + 1u, &frame.arena, &frame.render) != TA_OK) {
        return;
    }
    ta_arena previous = system->scratch;
    ta_arena previous_render = system->render;
    system->scratch = frame.arena;
    system->render = frame.render;
    ta_result result = ta_interpreter_init(&frame.it, system, actor);
    /* A session stopped inside a microstep exits its live set; the rest of
     * the stop is dropped (ADR 0061 section 6). */
    ta_stop_drop(&frame.it);
    frame.arena = system->scratch;
    frame.render = system->render;
    frame.next = actor->definition->state_count;
    if (result == TA_OK) {
        result = ta_vec_push(frames, &frame);
    }
    if (result != TA_OK) {
        actor->live_active = NULL;
        system->scratch = previous;
        system->render = previous_render;
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
        frame->render = system->render;
        return;
    }
    if (frame->exiting != TA_STATE_NONE) {
        if (!ta_invoke_take(frame->actor, frame->exiting, &frame->cursor, &frame->taken)) {
            ta_bits_put(it->active, frame->exiting, false);
            frame->exiting = TA_STATE_NONE;
            it->at = (ta_micro_position){0};
            frame->arena = system->scratch;
            frame->render = system->render;
        frame->render = system->render;
            return;
        }
        ta_interpreter_trace_invoke(it, TA_TRACE_INVOKE_CANCELLED, frame->taken.active.id);
        if (frame->taken.meta.has_child) {
            frame->waiting = true;
            frame->arena = system->scratch;
            frame->render = system->render;
        frame->render = system->render;
            ta_exit_open(system, frames, frame->taken.meta.child); /* may move `frame` */
            return;
        } else {
            ta_invoke_cancel_service(&frame->taken);
        }
        frame->arena = system->scratch;
        frame->render = system->render;
        return;
    }
    while (frame->next > 0 && !ta_bits_get(it->active, frame->next - 1u)) {
        frame->next -= 1u;
    }
    if (frame->next == 0) {
        ta_store_active(it);
        frame->actor->live_active = NULL;
        /* The region stays allocated for the next exit this deep. */
        frames->count -= 1u;
        return;
    }
    ta_state_id state = --frame->next;
    /* The position records report: the <onexit> of `state` (ADR 0067). */
    it->at = (ta_micro_position){ .phase = TA_PHASE_EXIT, .state = state + 1u };
    ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
    frame->arena = system->scratch;
    frame->render = system->render;
    if (result != TA_OK) {
        /* Nothing continues an exit: a block that ran out of memory left its
         * frames open for a stop, and they are dropped. */
        ta_stop_drop(it);
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
    ta_arena outer_render = system->render;
    ta_vec frames;
    ta_vec_init(&frames, system->allocator, TA_VEC_LAYOUT(ta_exit_frame));
    ta_exit_open(system, &frames, id);
    while (frames.count != 0) {
        ta_exit_frame *frame = ta_vec_at(&frames, frames.count - 1u);
        system->scratch = frame->arena;
        system->render = frame->render;
        ta_exit_step(system, &frames);
    }
    ta_vec_destroy(&frames);
    system->scratch = outer;
    system->render = outer_render;
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

ta_op_status ta_interpreter_copy_out(ta_interpreter *it, ta_value *value, ta_fault *fault) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (value->v == NULL || v->copy_out == NULL) {
        return TA_OP_OK;
    }
    ta_value copy = {0};
    ta_op_status status = v->copy_out(definition->datamodel.self, &(ta_datamodel_copy_out_params){
                                                                       .shared = definition->datamodel_shared,
                                                                       .instance = it->actor->datamodel_instance,
                                                                       .value = value,
                                                                       .host = &it->actor->host,
                                                                       .out = &copy,
                                                                       .fault = fault,
                                                                   });
    if (status == TA_OP_OK) {
        ta_value_release(value);
        *value = copy;
    }
    return status;
}

ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_execution_error(it, fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

ta_result ta_interpreter_queue_room(ta_system *system, ta_actor *actor) {
    return ta_queue_room(system, actor);
}

ta_result ta_interpreter_queue_reserve(ta_system *system, ta_actor *actor, uint32_t count) {
    ta_result result = ta_queue_room(system, actor);
    while (result == TA_OK && actor->event_block->capacity - actor->event_block->count < count) {
        ta_event_block *block = actor->event_block;
        if (block->capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        uint32_t capacity = block->capacity * 2u;
        ta_event_block *grown = ta_mem_alloc(system->allocator, ta_event_block_bytes(capacity),
                                             _Alignof(ta_event_block));
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(grown, block, ta_event_block_bytes(block->count));
        grown->capacity = capacity;
        ta_block_free(system, block);
        actor->event_block = grown;
    }
    return result;
}

uint32_t ta_interpreter_queue_mark(const ta_actor *actor) {
    return ta_queue_mark(actor);
}

bool ta_interpreter_internal_pending(const ta_actor *actor) {
    return ta_actor_waiting_internal(actor) != 0;
}

void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor) {
    ta_release_events(system, actor);
    /* The actor goes: its block goes too, whatever it ran under, and even
     * mid-macrostep, which otherwise keeps it (ta_block_settle). */
    actor->macrostep = false;
    if (actor->event_block != NULL) {
        actor->event_block->slots.trace = (ta_trace_parent){0};
    }
    if (actor->sparse == NULL) {
        ta_block_settle(system, actor);
        return;
    }
    ta_invoke_release(system, actor); /* Invoke: cancel services, free bookkeeping. */
    ta_continuation_free(system->allocator, &actor->sparse->paused);
    ta_block_settle(system, actor); /* empty, and no longer paused */
    /* The owner heap must not keep pointing into the freed sparse state. */
    ta_timer_list_destroy(&system->timers, &actor->sparse->timers);
    ta_value_release(&actor->sparse->completion.data);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}

/* ------------------------------------------------------------------------
 * Operator transitions and abandoning a macrostep (ADR 0082)
 */

/* Where a target sits for the legality check: a history state stands for
 * its parent, whose region it restores. */
static ta_state_id ta_jump_place(const ta_definition *definition, ta_state_id target) {
    ta_state_kind kind = definition->states[target].kind;
    bool history = kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
    return history ? definition->states[target].parent : target;
}

/*
 * Whether two targets can be in one legal configuration: neither is the
 * other or its ancestor, and the first state above both is a <parallel>.
 * The root counts as a compound state.
 */
static bool ta_jump_compatible(const ta_definition *definition, ta_state_id first, ta_state_id second) {
    if (first == TA_STATE_NONE || second == TA_STATE_NONE || first == second ||
        ta_definition_is_descendant(definition, first, second) ||
        ta_definition_is_descendant(definition, second, first)) {
        return false;
    }
    ta_state_id meet = definition->states[first].parent;
    while (meet != TA_STATE_NONE && !ta_definition_is_descendant(definition, second, meet)) {
        meet = definition->states[meet].parent;
    }
    return meet != TA_STATE_NONE && definition->states[meet].kind == TA_STATE_PARALLEL;
}

bool ta_interpreter_jump_valid(const ta_definition *definition, const ta_operator_transition *transition) {
    uint32_t count = transition->target_count;
    if (count == 0 || transition->targets == NULL ||
        (transition->source != TA_STATE_NONE && transition->source >= definition->state_count)) {
        return false;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        if (transition->targets[index] >= definition->state_count) {
            return false;
        }
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_state_id first = ta_jump_place(definition, transition->targets[index]);
        for (uint32_t other = index + 1u; other < count; other += 1u) {
            if (!ta_jump_compatible(definition, first, ta_jump_place(definition, transition->targets[other]))) {
                return false;
            }
        }
    }
    return true;
}

ta_result ta_interpreter_abandon(ta_system *system, ta_actor *actor) {
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (!actor->macrostep) {
        return TA_ERR_INVALID;
    }
    /* A run stopped inside a step, or a bootstrap whose microstep has not
     * completed, holds no configuration an idle actor could keep. */
    if (!actor->bootstrapped || (actor->sparse != NULL && actor->sparse->paused != NULL)) {
        return TA_ERR_BUSY;
    }
    ta_interpreter interpreter;
    ta_interpreter *it = &interpreter;
    ta_result result = ta_interpreter_init(it, system, actor);
    actor->live_active = NULL;
    if (result != TA_OK) {
        return result;
    }
    uint32_t dropped = ta_actor_waiting_internal(actor);
    if (ta_tracing(it, TA_TRACE_ABANDONED)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_ABANDONED);
        record.position = ta_trace_nowhere();
        record.event = ta_actor_external_event(actor);
        record.dropped = dropped;
        ta_trace_emit(it, &record);
    }
    ta_trace_end(it);
    /* The waiting internal events go; `_event` and the external event end
     * as at the end of any macrostep (ADR 0069). */
    ta_queue_truncate(actor, ta_queue_mark(actor) - dropped);
    ta_end_macrostep_events(system, actor);
    ta_actor_touch(actor);
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    ta_compact_sparse(it);
    ta_block_settle(system, actor);
    return TA_OK;
}
