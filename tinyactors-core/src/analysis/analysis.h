/* analysis/analysis.h — what can be known about a chart without running it.
 *
 * Two questions, answered from a verified image's records alone:
 *
 *   lint   which states, transitions and events cannot do what they are
 *          written for: states no path reaches, states an actor can never
 *          leave or never end from, transitions an earlier one always
 *          takes first, eventless transitions that loop forever, events
 *          sent that nothing handles and handled that nothing sends, and
 *          sends whose static target can never be reached.
 *   path   the shortest sequence of transitions (by their number) from the
 *          initial configuration, or from a configuration entered at a
 *          state, to a configuration holding a state.
 *
 * The model. A configuration is a set of active states, as SCXML 3.11
 * defines one (a compound state has one active child, a parallel state all
 * of its regions, every active state's parent is active). From a
 * configuration, any transition whose source is active and which has
 * targets may be taken, one at a time, but one an earlier transition of
 * its state always takes first (the shadowed rule below): its exit set and entry set are
 * computed as SCXML's algorithm computes them (the transition domain; a
 * target entered with its ancestors up to the domain; a compound state
 * entered by its initial transition; a parallel state with each region; a
 * history state by its default transition, as if it held nothing yet).
 * Entering a final state that is a child of the root ends the actor: no
 * transition is taken from that configuration.
 *
 * What it does not model, and so over-approximates: events (any event may
 * come at any time, done.* and error.* included, and the application may
 * send any event), conditions (each may be true), the priority between
 * transitions beyond shadowing (an eventless transition with a condition
 * does not run before the others, a descendant's transition does not
 * block an ancestor's), several
 * transitions taken in one microstep (parallel regions take theirs one
 * after another), the values history records (its default is entered),
 * and invoked children (an invocation's events may come any time). So a
 * state found unreachable is unreachable whatever happens at run time,
 * while a state found reachable may still not be.
 *
 * Pure and freestanding: it reads the image view, writes every array into
 * the arena given (no recursion, no C library), and returns
 * TA_ERROR_NO_MEMORY when the arena refuses. It knows nothing of how the
 * answer is shown. */
#ifndef TINYACTORS_ANALYSIS_ANALYSIS_H
#define TINYACTORS_ANALYSIS_ANALYSIS_H

#include "image/records.h"
#include <tinyactors/arena.h>

/* How many configurations one chart's search may hold before it stops:
 * the default when a request gives 0. */
typedef enum ta_analysis_limits { TA_ANALYSIS_DEFAULT_LIMIT = 20000 } ta_analysis_limits;

/* ---- lint --------------------------------------------------------------------------------- */

typedef enum ta_analysis_rule {
    /* A state in no configuration any path reaches (only the topmost of
     * such a subtree is reported). */
    TA_ANALYSIS_UNREACHABLE = 0,
    /* A reachable atomic state that is not final, where an actor does
     * nothing more: no transition of its own or of an ancestor handles any
     * event, and none of a parallel region beside it exits it. A state
     * that handles events without leaving (a service's resting state) is
     * not one; whether it can end is never-ends'. */
    TA_ANALYSIS_DEAD_END,
    /* The chart has a final state at its top level, and from every
     * configuration holding this state none is reachable (the topmost
     * such state; a dead end is not reported twice). */
    TA_ANALYSIS_NEVER_ENDS,
    /* A transition an earlier transition of the same state always takes
     * first: one without a condition whose descriptors cover all of this
     * one's (`other`), or an eventless one without a condition, which
     * leaves before any event is taken. */
    TA_ANALYSIS_SHADOWED,
    /* An eventless transition without a condition that an actor takes
     * again and again without end: it stays in its state, or leads through
     * other such transitions back to it. */
    TA_ANALYSIS_EVENTLESS_LOOP,
    /* An event raised, or sent by name to a chart the image holds (itself,
     * its parent, an invoked chart), that no transition of that chart
     * handles. */
    TA_ANALYSIS_UNHANDLED_EVENT,
    /* With several charts (a system): a descriptor a chart handles that
     * nothing in the image raises or sends by name to it. Only the
     * application can send it. done.* and error.* and "*" are not
     * reported. */
    TA_ANALYSIS_UNSENT_EVENT,
    /* With several charts: a send whose event is computed (eventexpr) may
     * reach this chart, so which events it receives is unknown; its
     * handled events are not checked (`other`: the sending chart). */
    TA_ANALYSIS_UNKNOWN_EVENTS,
    /* A send whose static target can never be reached: "#_id" naming no
     * invocation of the chart, or text that is neither "#_..." nor an
     * absolute URI. */
    TA_ANALYSIS_SEND_TARGET,
    /* The search of the chart's configurations stopped at its limit:
     * never-ends was not checked. */
    TA_ANALYSIS_SEARCH_LIMIT,
    TA_ANALYSIS_RULE_COUNT
} ta_analysis_rule;

typedef enum ta_analysis_severity { TA_ANALYSIS_WARNING = 0, TA_ANALYSIS_NOTE = 1 } ta_analysis_severity;

typedef struct ta_analysis_finding {
    uint32_t rule;     /* ta_analysis_rule */
    uint32_t severity; /* ta_analysis_severity */
    uint32_t chart;    /* the image's chart index */
    /* chart-local indexes, TA_NIL when the finding has none: */
    uint32_t state;      /* the state; for a transition or a send, where it is */
    uint32_t transition; /* the transition; for a send, the one whose actions hold it */
    uint32_t send;
    uint32_t event;  /* an event-name node, TA_NIL when none */
    uint32_t other;  /* shadowed: the earlier transition; unknown-events: the chart sending */
    uint32_t line;   /* 1-based, 0 when the image has no line for it */
    uint32_t column; /* 1-based, 0 when none */
} ta_analysis_finding;

typedef enum ta_analysis_flags {
    /* The charts are one system (a manifest, or several charts compiled
     * together): unsent-event and unknown-events are checked. */
    TA_ANALYSIS_SYSTEM = 1u << 0
} ta_analysis_flags;

typedef struct ta_analysis_request {
    const ta_image_view *view;  /* a verified image */
    const uint8_t *charts;      /* one per chart, 1: look at it; NULL: every chart */
    uint32_t flags;             /* ta_analysis_flags */
    uint32_t limit;             /* configurations per chart's search; 0: the default */
    ta_growable_arena *arena;   /* every array of the answer, and the scratch */
} ta_analysis_request;

typedef struct ta_analysis_findings {
    const ta_analysis_finding *list; /* by chart; within one, rule by rule in document order */
    uint32_t count;
} ta_analysis_findings;

/* TA_OK; TA_ERROR_INVALID: no view or arena; TA_ERROR_NO_MEMORY. */
ta_status ta_analysis_lint(const ta_analysis_request *request, ta_analysis_findings *out);

/* The rule's name ("unreachable", "dead-end", ...), and the rule a name
 * gives (TA_NIL: none). */
ta_string_view ta_analysis_rule_name(uint32_t rule);
uint32_t ta_analysis_rule_named(ta_string_view name);
/* The severity a rule's findings have (ta_analysis_severity). */
uint32_t ta_analysis_rule_severity(uint32_t rule);

/* ---- path --------------------------------------------------------------------------------- */

typedef struct ta_analysis_path_request {
    const ta_image_view *view;
    uint32_t chart; /* the image's chart index */
    uint32_t from;  /* chart-local state entered from the root (its defaults
                       completing it), or TA_NIL: the initial configuration */
    uint32_t to;    /* chart-local state the last configuration holds */
    uint32_t limit; /* configurations the search may hold; 0: the default */
    ta_growable_arena *arena;
} ta_analysis_path_request;

typedef enum ta_analysis_path_result {
    TA_ANALYSIS_PATH_FOUND = 0,      /* `steps` lead there */
    TA_ANALYSIS_PATH_UNREACHABLE = 1, /* every configuration was searched: none holds it */
    TA_ANALYSIS_PATH_LIMIT = 2        /* the search stopped at its limit before finding it */
} ta_analysis_path_result;

/* One step: a transition taken, and the configuration it leads to (a
 * bitset of the chart's states, bit s of word s / 32). */
typedef struct ta_analysis_step {
    uint32_t transition; /* chart-local; TA_NIL for the first configuration */
    const uint32_t *configuration;
} ta_analysis_step;

typedef struct ta_analysis_path {
    uint32_t result; /* ta_analysis_path_result */
    /* the first configuration, then one step per transition; when not
     * found, only the first configuration */
    const ta_analysis_step *steps;
    uint32_t step_count;
    uint32_t explored; /* configurations the search held */
} ta_analysis_path;

/* TA_OK; TA_ERROR_INVALID: a chart or state out of range, or a history
 * state (never active itself) as `from` or `to`; TA_ERROR_NO_MEMORY. */
ta_status ta_analysis_path_find(const ta_analysis_path_request *request, ta_analysis_path *out);

/* ---- event descriptors -------------------------------------------------------------------- */

/* Whether the descriptor `handler` matches the event name `event`, by
 * name: "*", the same name, or a prefix of it at a dot (SCXML 3.12.1;
 * "order" and "order.*" match "order.paid"). Within one image the event
 * names' trie answers the same by intervals; this is for names of two
 * images (diff). */
int ta_analysis_descriptor_covers(ta_string_view handler, ta_string_view event);

#endif
