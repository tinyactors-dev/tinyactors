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
    TA_DIAGRAM_BOX_HISTORY = 6
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

#endif
