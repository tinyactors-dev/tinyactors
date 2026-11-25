/* diagram/fold.c — a chart with some of its containers folded into one box
 * each (diagram.h, ta_diagram_fold): scxmljs's `collapsed` set, made into
 * the chart the layout reads. scxmljs's layoutChart kept the whole chart
 * and asked, for every state, which box draws it (`drawn`); here that
 * question is answered once, before the layout, so the layout, its checks
 * and the drawing see an ordinary chart whose folded containers are
 * leaves. Freestanding: arrays in the arena, no recursion (a parent comes
 * before its children). */
#include "diagram/diagram.h"

#include "base/memory.h"

static void *take(ta_growable_arena *arena, size_t size, size_t count) {
    void *memory = ta_growable_arena_alloc(arena, size * (count + 1), 8);
    if (memory) ta_memset(memory, 0, size * (count + 1));
    return memory;
}

static int is_history(uint32_t kind) {
    return kind == TA_DIAGRAM_STATE_HISTORY_SHALLOW || kind == TA_DIAGRAM_STATE_HISTORY_DEEP;
}

/* The targets a transition keeps: each drawn box once, in order. */
typedef struct target_list {
    uint32_t *targets;
    uint32_t count;
} target_list;

static void add_target(target_list *list, uint32_t first, uint32_t state) {
    for (uint32_t i = first; i < list->count; i++)
        if (list->targets[i] == state) return;
    list->targets[list->count++] = state;
}

ta_status ta_diagram_fold(const ta_diagram_fold_request *request, ta_diagram_folding *out) {
    const ta_diagram_chart *chart = request->chart;
    uint32_t n = chart->state_count;
    ta_memset(out, 0, sizeof *out);
    if (!n || chart->states[0].parent != TA_NIL) return TA_ERROR_INVALID;
    for (uint32_t s = 1; s < n; s++)
        if (chart->states[s].parent >= s) return TA_ERROR_INVALID;
    uint32_t total_targets = 0;
    for (uint32_t t = 0; t < chart->transition_count; t++) {
        const ta_diagram_transition *transition = &chart->transitions[t];
        if (transition->source >= n) return TA_ERROR_INVALID;
        for (uint32_t i = 0; i < transition->target_count; i++)
            if (chart->targets[transition->first_target + i] >= n) return TA_ERROR_INVALID;
        total_targets += transition->target_count;
    }
    for (uint32_t s = 0; s < n; s++) {
        for (uint32_t i = 0; i < chart->states[s].initial_count; i++)
            if (chart->targets[chart->states[s].first_initial + i] >= n) return TA_ERROR_INVALID;
        total_targets += chart->states[s].initial_count;
    }
    ta_growable_arena *arena = request->arena;
    uint32_t *drawn = take(arena, sizeof(uint32_t), n);         /* the drawn state, as an original index */
    uint8_t *container = take(arena, sizeof(uint8_t), n);      /* has children other than history */
    uint8_t *folds = take(arena, sizeof(uint8_t), n);          /* folded, and not hidden itself */
    uint32_t *index = take(arena, sizeof(uint32_t), n);        /* original -> drawn chart */
    uint32_t *original = take(arena, sizeof(uint32_t), n);
    uint32_t *drawn_state = take(arena, sizeof(uint32_t), n);
    uint32_t *drawn_transition = take(arena, sizeof(uint32_t), chart->transition_count);
    ta_diagram_state *states = take(arena, sizeof(ta_diagram_state), n);
    ta_diagram_transition *transitions = take(arena, sizeof(ta_diagram_transition), chart->transition_count);
    uint32_t *targets = take(arena, sizeof(uint32_t), total_targets);
    if (!drawn || !container || !folds || !index || !original || !drawn_state || !drawn_transition || !states ||
        !transitions || !targets)
        return TA_ERROR_NO_MEMORY;
    for (uint32_t s = 1; s < n; s++)
        if (!is_history(chart->states[s].kind)) container[chart->states[s].parent] = 1;
    /* which box draws each state: the outermost folded container holding it, else itself
     * (scxmljs's `drawn`); a parent comes first, so its answer is known */
    for (uint32_t s = 0; s < n; s++) {
        uint32_t parent = chart->states[s].parent;
        uint32_t above = parent == TA_NIL ? s : drawn[parent];
        int hidden = parent != TA_NIL && (above != parent || folds[parent]);
        drawn[s] = hidden ? above : s;
        folds[s] = !hidden && s != 0 && container[s] && request->folded && request->folded[s];
    }
    uint32_t count = 0;
    for (uint32_t s = 0; s < n; s++) {
        if (drawn[s] != s) continue;
        index[s] = count;
        original[count] = s;
        const ta_diagram_state *state = &chart->states[s];
        states[count] = (ta_diagram_state){state->parent == TA_NIL ? TA_NIL : index[state->parent], state->kind,
                                           state->name, 0, 0, 0};
        count++;
    }
    for (uint32_t s = 0; s < n; s++) {
        drawn_state[s] = index[drawn[s]];
        if (drawn[s] != s && !is_history(chart->states[s].kind)) states[index[drawn[s]]].hidden++;
    }
    target_list list = {targets, 0};
    /* initial states: each drawn box once; a folded container has none */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = original[i];
        if (folds[s]) continue;
        const ta_diagram_state *state = &chart->states[s];
        uint32_t first = list.count;
        for (uint32_t j = 0; j < state->initial_count; j++)
            add_target(&list, first, index[drawn[chart->targets[state->first_initial + j]]]);
        states[i].first_initial = first;
        states[i].initial_count = list.count - first;
    }
    uint32_t transition_count = 0;
    for (uint32_t t = 0; t < chart->transition_count; t++) {
        const ta_diagram_transition *transition = &chart->transitions[t];
        uint32_t source = drawn[transition->source], first = list.count;
        int own = source == transition->source; /* the box's own transition, not a hidden state's */
        for (uint32_t j = 0; j < transition->target_count; j++) {
            uint32_t target = chart->targets[transition->first_target + j];
            uint32_t box = drawn[target];
            /* inside one folded box: a hidden state's, or the folded container's into its own
             * states; its own to itself stays, a loop on the box */
            if (box == source && folds[source] && !(own && target == source)) continue;
            add_target(&list, first, index[box]);
        }
        int kept = list.count > first || (!transition->target_count && own);
        if (!kept) {
            drawn_transition[t] = TA_NIL;
            continue;
        }
        drawn_transition[t] = transition_count;
        transitions[transition_count++] =
            (ta_diagram_transition){index[source], first, list.count - first, transition->events, transition->condition};
    }
    out->chart = (ta_diagram_chart){states, count, transitions, transition_count, targets};
    out->drawn_state = drawn_state;
    out->drawn_transition = drawn_transition;
    out->original_state = original;
    return TA_OK;
}
