/* diagram/diagram.h — a statechart drawn: its layout, and the layout as SVG.
 *
 * The layout is scxmljs's (`@tinyactors/scxmljs`, src/view/layout.ts, the
 * <scxml-view> element's), ported: the whole chart at once, as nested boxes
 * with orthogonal transition routes.
 *
 *   Per container (the root, compound states), children are arranged with a
 *   layered (Sugiyama-style) layout of the transitions between them:
 *     1. transitions between descendants of two different children are
 *        lifted to an edge between those children;
 *     2. cycles are broken by a greedy feedback arc set (Eades, Lin & Smyth),
 *        the initial child first, document order breaking ties; the reversed
 *        edges are routed as back edges;
 *     3. layers by longest path; edges spanning several layers get dummy
 *        nodes that reserve a corridor through the layers in between;
 *     4. crossings are reduced with barycentre sweeps (the best order wins);
 *     5. positions: layers side by side (gaps sized for labels and bend
 *        channels), nodes aligned towards the centre of their neighbours.
 *   Parallel states arrange their regions in a grid. Containers size
 *   themselves around their content, bottom-up.
 *
 *   Routing: forward edges leave the source box on its far side, carry their
 *   label in the gap after the source's layer, bend in a channel of their
 *   own and enter the target box on its near side; back edges leave
 *   backwards and come back through the same channels; edges between the
 *   regions of a parallel state run along lanes below them. Labels sit on
 *   their own route, the segment nearest the source first. Everything is
 *   computed with layers along +x and transposed for TA_DIAGRAM_DOWN.
 *
 * Transitions that cross three or more container borders, and a
 * container's transitions into its own descendants, are not routed: they
 * are listed in their source's box ("event → target"), with the
 * transitions that have no target (ta_diagram_portals).
 *
 * A container may be folded into one box (ta_diagram_fold): the chart is
 * drawn without its descendants, the box standing for them, and their
 * transitions to and from the rest of the chart start or end at it.
 *
 * Pure and deterministic: no I/O, no randomness, the same chart always
 * looks the same. Freestanding: every array lives in the arena the caller
 * gives, and nothing recurses (the chart is walked in document order).
 * Coordinates are in pixels, the origin top left. */
#ifndef TINYACTORS_DIAGRAM_DIAGRAM_H
#define TINYACTORS_DIAGRAM_DIAGRAM_H

#include "base/output.h"
#include <tinyactors/arena.h>

/* ---- the chart ------------------------------------------------------------------------- */

typedef enum ta_diagram_state_kind {
    TA_DIAGRAM_STATE_ROOT = 0, /* the <scxml> element */
    TA_DIAGRAM_STATE_STATE = 1, /* <state>: compound when it has children that are not history */
    TA_DIAGRAM_STATE_PARALLEL = 2,
    TA_DIAGRAM_STATE_FINAL = 3,
    TA_DIAGRAM_STATE_HISTORY_SHALLOW = 4,
    TA_DIAGRAM_STATE_HISTORY_DEEP = 5
} ta_diagram_state_kind;

/* A state, in document order: the root first, a parent before its
 * children, and every state's descendants right after it. */
typedef struct ta_diagram_state {
    uint32_t parent;    /* TA_NIL for the root */
    uint32_t kind;      /* ta_diagram_state_kind */
    ta_string_view name; /* its id; empty for the root */
    /* Its initial states (a compound state's, the root's), in `targets`. */
    uint32_t first_initial, initial_count;
    /* A folded container (ta_diagram_fold): how many states its box stands
     * for, its descendants but history states; 0 for every other state. */
    uint32_t hidden;
} ta_diagram_state;

/* A transition, in document order (a history state's default is one). */
typedef struct ta_diagram_transition {
    uint32_t source;
    uint32_t first_target, target_count; /* in `targets`; none: it stays in its state */
    ta_string_view events;    /* its event descriptors, separated by spaces */
    ta_string_view condition; /* empty when it has none */
} ta_diagram_transition;

typedef struct ta_diagram_chart {
    const ta_diagram_state *states;
    uint32_t state_count;
    const ta_diagram_transition *transitions;
    uint32_t transition_count;
    const uint32_t *targets; /* state indexes */
} ta_diagram_chart;

/* ---- geometry -------------------------------------------------------------------------- */

typedef struct ta_diagram_point {
    double x, y;
} ta_diagram_point;

typedef struct ta_diagram_size {
    double w, h;
} ta_diagram_size;

typedef struct ta_diagram_rect {
    double x, y, w, h;
} ta_diagram_rect;

typedef enum ta_diagram_direction {
    TA_DIAGRAM_RIGHT = 0, /* layers run left to right */
    TA_DIAGRAM_DOWN = 1   /* layers run top to bottom */
} ta_diagram_direction;

/* Gaps the layout keeps, in px. */
typedef struct ta_diagram_spacing {
    double node;    /* between boxes of one layer, and between parallel regions */
    double layer;   /* the least gap between layers (it grows for labels and bends) */
    double padding; /* inside containers, around their content */
    double edge;    /* between parallel edge segments: bend channels, lanes, ports */
    double label;   /* kept around labels */
} ta_diagram_spacing;

/* ---- the layout ------------------------------------------------------------------------ */

typedef struct ta_diagram_request {
    const ta_diagram_chart *chart;
    uint32_t direction; /* ta_diagram_direction */
    /* down: the width to keep within where the layout has a choice (parallel
     * regions wrap into fewer columns); 0 for none */
    double max_width;
    /* What each state needs for its own content: a leaf its whole box, a
     * container its header (the layout adds room for its children). */
    const ta_diagram_size *state_sizes;
    /* Each transition's label (as drawn, not transposed); 0 x 0 for none. */
    const ta_diagram_size *label_sizes;
    /* 1 for a transition listed in its source's box instead of routed
     * (ta_diagram_portals); NULL for none. */
    const uint8_t *portals;
    ta_growable_arena *arena;
} ta_diagram_request;

typedef enum ta_diagram_box_kind {
    TA_DIAGRAM_BOX_ROOT = 0,
    TA_DIAGRAM_BOX_ATOMIC = 1,
    TA_DIAGRAM_BOX_COMPOUND = 2,
    TA_DIAGRAM_BOX_PARALLEL = 3,
    TA_DIAGRAM_BOX_REGION = 4, /* a child of a parallel state */
    TA_DIAGRAM_BOX_FINAL = 5,
    TA_DIAGRAM_BOX_HISTORY = 6,
    TA_DIAGRAM_BOX_FOLDED = 7  /* a container drawn as one box (ta_diagram_state.hidden) */
} ta_diagram_box_kind;

typedef struct ta_diagram_box {
    uint32_t state;
    uint32_t kind; /* ta_diagram_box_kind */
    ta_diagram_rect rect;
    double header; /* the header band at the top of a container; 0 for a leaf */
    uint32_t depth; /* the root's is 0 */
} ta_diagram_box;

typedef enum ta_diagram_edge_kind {
    TA_DIAGRAM_EDGE_FORWARD = 0,       /* along the layers */
    TA_DIAGRAM_EDGE_BACK = 1,          /* against them, or between regions */
    TA_DIAGRAM_EDGE_SELF = 2,          /* a state to itself */
    TA_DIAGRAM_EDGE_TO_ANCESTOR = 3,   /* out of its own container */
    TA_DIAGRAM_EDGE_TO_DESCENDANT = 4  /* a container into one of its states */
} ta_diagram_edge_kind;

/* One transition to one of its targets. */
typedef struct ta_diagram_edge {
    uint32_t transition, target;
    uint32_t kind; /* ta_diagram_edge_kind */
    uint32_t first_point, point_count; /* an orthogonal polyline, the arrow at the last */
    uint32_t labelled; /* 1 when `label` holds the label (a transition is labelled once) */
    ta_diagram_rect label;
} ta_diagram_edge;

/* The dot and arrow marking a container's initial state. */
typedef struct ta_diagram_initial {
    uint32_t container, target;
    ta_diagram_point at; /* the dot's centre */
    uint32_t first_point, point_count;
} ta_diagram_initial;

typedef struct ta_diagram_layout {
    double width, height;
    const ta_diagram_box *boxes; /* parents before children: draw in this order */
    uint32_t box_count;
    const ta_diagram_edge *edges;
    uint32_t edge_count;
    const ta_diagram_initial *initials;
    uint32_t initial_count;
    const ta_diagram_point *points;
    /* Per state, the transitions drawn inside its box: those without a
     * target and the portals, in document order (`inside` from
     * inside_first[s], inside_count[s] of them). */
    const uint32_t *inside;
    const uint32_t *inside_first, *inside_count;
} ta_diagram_layout;

/* TA_OK; TA_ERROR_INVALID: a chart that is not a tree in document order, or
 * a target out of range; TA_ERROR_NO_MEMORY: the arena could not grow. */
ta_status ta_diagram_layout_chart(const ta_diagram_request *request, ta_diagram_layout *out);

/* 1 per transition better listed in its source's box than routed: a
 * container's transitions into its own descendants, and those crossing
 * three or more container borders. NULL when the arena could not grow. */
uint8_t *ta_diagram_portals(const ta_diagram_chart *chart, ta_growable_arena *arena);

/* ---- folding ------------------------------------------------------------------------------- */

typedef struct ta_diagram_fold_request {
    const ta_diagram_chart *chart;
    /* per state, 1: fold it into one box. A folded state inside another
     * folded one is hidden with it; a state that is no container (no
     * children but history states) is never folded. */
    const uint8_t *folded;
    ta_growable_arena *arena;
} ta_diagram_fold_request;

/* The chart as drawn with its folded containers. Every array is in the
 * request's arena; `chart`'s names are the original's. */
typedef struct ta_diagram_folding {
    /* The states that are drawn, in the original's document order, each
     * folded container a leaf whose `hidden` counts what it stands for.
     * Its transitions: the original's, in order, each target inside a
     * folded container replaced by that container's box (repeated ones
     * once), a source inside one likewise. A transition that stays inside
     * one folded box (its source and every target there, or no target) is
     * not drawn, but for the folded container's own transitions with no
     * target, which it lists as any box does. Initial states inside a
     * folded container are its box. */
    ta_diagram_chart chart;
    const uint32_t *drawn_state;      /* per original state: the state of `chart` drawing it */
    const uint32_t *drawn_transition; /* per original transition: its index in `chart`, TA_NIL when not drawn */
    const uint32_t *original_state;   /* per state of `chart`: its index in the original */
} ta_diagram_folding;

/* TA_OK; TA_ERROR_INVALID: a chart that is not a tree in document order;
 * TA_ERROR_NO_MEMORY. */
ta_status ta_diagram_fold(const ta_diagram_fold_request *request, ta_diagram_folding *out);

/* ---- checking a layout -------------------------------------------------------------- */

/* What a drawing must never show, each found by geometry alone, so a test can require none. */
typedef enum ta_diagram_problem_kind {
    TA_DIAGRAM_PROBLEM_START_DETACHED = 0, /* an edge's first point is not on its source's outline */
    TA_DIAGRAM_PROBLEM_END_DETACHED = 1,   /* an edge's arrow is not on its target's outline */
    TA_DIAGRAM_PROBLEM_NOT_ORTHOGONAL = 2, /* a segment neither horizontal nor vertical */
    TA_DIAGRAM_PROBLEM_ARROW_ALONG_SIDE = 3, /* the last segment runs along the side it ends on, not into it */
    TA_DIAGRAM_PROBLEM_NOT_CENTRED = 4,     /* an edge alone on a leaf's side misses the side's middle */
    TA_DIAGRAM_PROBLEM_THROUGH_BOX = 5,    /* a segment runs through a leaf's inside (its own ends' too) */
    TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT = 6, /* a box reaches past its parent's */
    TA_DIAGRAM_PROBLEM_BOXES_OVERLAP = 7,  /* two sibling boxes overlap */
    TA_DIAGRAM_PROBLEM_LABEL_OVERLAP = 8,  /* a label over another edge's label (`other`: that edge) */
    TA_DIAGRAM_PROBLEM_LABEL_ON_BOX = 9,   /* a label over a leaf (`other`: its state) */
    TA_DIAGRAM_PROBLEM_INITIAL_DETACHED = 10, /* an initial marker's arrow is not on its state's outline */
    TA_DIAGRAM_PROBLEM_LABEL_ON_EDGE = 11  /* a label over another edge's line (`other`: that edge) */
} ta_diagram_problem_kind;

/* A problem and what it is about: `subject` an edge's index, or a state for OUTSIDE_PARENT,
 * BOXES_OVERLAP and INITIAL_DETACHED (the marker's target); `other` the state the problem
 * touches (the marker's container), the other edge for LABEL_OVERLAP and LABEL_ON_EDGE, TA_NIL
 * for none. */
typedef struct ta_diagram_problem {
    uint32_t kind; /* ta_diagram_problem_kind */
    uint32_t subject, other;
} ta_diagram_problem;

typedef struct ta_diagram_check_request {
    const ta_diagram_chart *chart;
    const ta_diagram_layout *layout;
    ta_growable_arena *arena;
} ta_diagram_check_request;

typedef struct ta_diagram_problems {
    const ta_diagram_problem *list;
    uint32_t count;
} ta_diagram_problems;

/* Every problem of a layout, edges first, then boxes, then labels. TA_ERROR_NO_MEMORY when the
 * arena could not grow. */
ta_status ta_diagram_check(const ta_diagram_check_request *request, ta_diagram_problems *out);

/* The problem kind's name, for a report: "startDetached", "endDetached", ... */
ta_string_view ta_diagram_problem_name(uint32_t kind);

#endif
