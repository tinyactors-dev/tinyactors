/* analysis/machine.h — the configurations of one chart, between the files
 * of the analysis layer (analysis.h holds the model they share).
 *
 * A configuration is a bitset of the chart's states (bit s of word s / 32).
 * The machine computes, per transition, its domain and the states it
 * enters (SCXML's computeEntrySet, history entered by its default), once;
 * a search then walks configurations breadth first, keeping each once. */
#ifndef TINYACTORS_ANALYSIS_MACHINE_H
#define TINYACTORS_ANALYSIS_MACHINE_H

#include "analysis/analysis.h"

typedef struct ta_analysis_machine {
    const ta_image_view *view;
    const ta_chart_record *chart;
    const ta_state_record *states;           /* the chart's, chart-local */
    const ta_transition_record *transitions; /* the chart's, chart-local */
    uint32_t state_count, transition_count;
    uint32_t words; /* per configuration */
    ta_growable_arena *arena;
    /* Per transition: the earlier transition of its state always taken
     * before it (analysis.h, shadowed), or TA_NIL. */
    uint32_t *shadows;
    /* Per transition with targets that is not an initial one: its domain
     * (TA_NIL otherwise) and the states it enters (`words` each). */
    uint32_t *domains;
    uint32_t *entries;
    /* The states the first configuration holds: the root entered. */
    uint32_t *initial;
    /* History states some entry passes through (never active themselves). */
    uint32_t *histories;
    /* Scratch of one entry: a stack of tasks, three words each. */
    uint32_t *tasks;
    uint32_t task_count, task_capacity;
} ta_analysis_machine;

typedef struct ta_analysis_machine_setup {
    const ta_image_view *view;
    uint32_t chart;
    ta_growable_arena *arena;
} ta_analysis_machine_setup;

/* TA_OK, or TA_ERROR_NO_MEMORY. */
ta_status ta_analysis_machine_init(ta_analysis_machine *machine, ta_analysis_machine_setup setup);

/* States entered when `targets` are entered from `domain` (TA_NIL: from
 * outside the root, every ancestor entered), into `out` (`words`,
 * cleared first). */
typedef struct ta_analysis_entry {
    const uint32_t *targets;
    uint32_t count;
    uint32_t domain;
} ta_analysis_entry;
ta_status ta_analysis_enter(ta_analysis_machine *machine, ta_analysis_entry entry, uint32_t *out);

/* Whether a transition can be taken in the model: not an initial one,
 * with targets, and not shadowed by an earlier one. */
int ta_analysis_movable(const ta_analysis_machine *machine, uint32_t transition);

/* `next` = the configuration `from` after `transition` (movable). */
typedef struct ta_analysis_move {
    const uint32_t *from;
    uint32_t transition;
} ta_analysis_move;
void ta_analysis_apply(const ta_analysis_machine *machine, ta_analysis_move move, uint32_t *next);

/* Whether a configuration ends the actor (holds a final child of the root). */
int ta_analysis_ends(const ta_analysis_machine *machine, const uint32_t *configuration);

/* ---- the search ----------------------------------------------------------------------- */

typedef struct ta_analysis_search {
    ta_analysis_machine *machine;
    uint32_t limit;
    int record_edges; /* keep each configuration's successors */
    uint32_t count, capacity;
    uint32_t *configurations; /* count * words */
    uint32_t *parents, *via;  /* per configuration: how the search first reached it */
    uint32_t *table;          /* open addressing: configuration + 1, 0 empty */
    uint32_t table_size;      /* a power of two */
    uint32_t *first_edge;     /* per configuration and one past: its successors in `edges` */
    uint32_t *edges;
    uint32_t edge_count, edge_capacity;
    int stopped; /* the limit was reached */
} ta_analysis_search;

/* Search from `start` (`words`) until a configuration holds `target`
 * (TA_NIL: every configuration). `found`: its index, or TA_NIL. TA_OK, or
 * TA_ERROR_NO_MEMORY. */
typedef struct ta_analysis_goal {
    const uint32_t *start;
    uint32_t target;
} ta_analysis_goal;
ta_status ta_analysis_search_run(ta_analysis_search *search, ta_analysis_goal goal, uint32_t *found);

static inline int ta_analysis_bit(const uint32_t *set, uint32_t index) {
    return (int)((set[index >> 5] >> (index & 31u)) & 1u);
}
static inline void ta_analysis_set(uint32_t *set, uint32_t index) {
    set[index >> 5] |= 1u << (index & 31u);
}
/* Whether `state` is a proper descendant of `ancestor`. */
static inline int ta_analysis_below(const ta_analysis_machine *machine, uint32_t state, uint32_t ancestor) {
    return state > ancestor && state < machine->states[ancestor].end;
}
/* An array of `count` words from the machine's arena, or NULL. */
uint32_t *ta_analysis_words(ta_analysis_machine *machine, uint32_t count);

#endif
