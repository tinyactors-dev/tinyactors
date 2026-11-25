/* The diagram layers: the layout (diagram/diagram.h) and its SVG (diagram/svg.h). The layout's
 * coordinates are checked against scxmljs's in test_tool (test_graph_layout); these are its
 * properties, its refusals and the SVG's escaping. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "diagram/svg.h"

typedef struct test_heap {
    size_t budget; /* bytes it gives before it refuses; SIZE_MAX: no limit */
} test_heap;

static void *test_alloc(void *context, size_t size, size_t align) {
    (void)align;
    test_heap *heap = (test_heap *)context;
    if (size > heap->budget) return NULL;
    heap->budget -= size;
    return malloc(size);
}

static void test_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

typedef struct test_arena {
    test_heap heap;
    ta_allocator allocator;
    ta_growable_arena arena;
} test_arena;

static void arena_begin(test_arena *a, size_t budget) {
    memset(a, 0, sizeof *a);
    a->heap.budget = budget;
    a->allocator = (ta_allocator){&a->heap, test_alloc, test_free};
    ta_growable_arena_config config = {sizeof config, 0, 4096, &a->allocator};
    ta_growable_arena_init(&a->arena, &config);
}

/* An order: open and paying in a compound `checkout`, paid and cancelled beside it; a parallel
 * `fulfilment` of two regions; a history state; a self loop; a transition with no target. */
static const ta_diagram_state order_states[] = {
    {TA_NIL, TA_DIAGRAM_STATE_ROOT, {"", 0}, 0, 1, 0},              /* 0 */
    {0, TA_DIAGRAM_STATE_STATE, {"checkout", 8}, 1, 1, 0},            /* 1 */
    {1, TA_DIAGRAM_STATE_STATE, {"open", 4}, 0, 0, 0},                /* 2 */
    {1, TA_DIAGRAM_STATE_STATE, {"paying", 6}, 0, 0, 0},              /* 3 */
    {1, TA_DIAGRAM_STATE_HISTORY_SHALLOW, {"resume", 6}, 0, 0, 0},    /* 4 */
    {0, TA_DIAGRAM_STATE_PARALLEL, {"fulfilment", 10}, 0, 0, 0},       /* 5 */
    {5, TA_DIAGRAM_STATE_STATE, {"packing", 7}, 0, 0, 0},             /* 6 */
    {5, TA_DIAGRAM_STATE_STATE, {"billing", 7}, 0, 0, 0},             /* 7 */
    {0, TA_DIAGRAM_STATE_FINAL, {"<paid & \"done\">", 15}, 0, 0, 0},   /* 8: a name to escape */
};
static const uint32_t order_targets[] = {
    1,    /* the root's initial: checkout */
    2,    /* checkout's initial: open */
    3,    /* open -> paying */
    5,    /* paying -> fulfilment */
    3,    /* paying -> paying */
    8,    /* fulfilment -> paid */
    2,    /* resume -> open */
};
static const ta_diagram_transition order_transitions[] = {
    {2, 2, 1, {"checkout", 8}, {"total > 0", 9}},
    {3, 3, 1, {"payment.done", 12}, {"", 0}},
    {3, 4, 1, {"payment.retry", 13}, {"attempts < 3", 12}},
    {3, 0, 0, {"item.added", 10}, {"", 0}},
    {5, 5, 1, {"done.state.fulfilment", 21}, {"", 0}},
    {4, 6, 1, {"", 0}, {"", 0}},
};
static const ta_diagram_chart order = {order_states, 9, order_transitions, 6, order_targets};

typedef struct drawn {
    uint8_t *portals;
    ta_diagram_measures measures;
    ta_diagram_layout layout;
} drawn;

static ta_status lay_out(test_arena *a, const ta_diagram_chart *chart, drawn *out) {
    out->portals = ta_diagram_portals(chart, &a->arena);
    if (!out->portals) return TA_ERROR_NO_MEMORY;
    ta_diagram_measure_request measure = {chart, out->portals, &a->arena};
    ta_status status = ta_diagram_measure(&measure, &out->measures);
    if (status != TA_OK) return status;
    ta_diagram_request request = {chart, TA_DIAGRAM_RIGHT, 0, out->measures.states, out->measures.labels,
                                  out->portals, &a->arena};
    return ta_diagram_layout_chart(&request, &out->layout);
}

static int inside(ta_diagram_rect inner, ta_diagram_rect outer) {
    return inner.x >= outer.x - 0.01 && inner.y >= outer.y - 0.01 && inner.x + inner.w <= outer.x + outer.w + 0.01 &&
           inner.y + inner.h <= outer.y + outer.h + 0.01;
}

static int overlap(ta_diagram_rect a, ta_diagram_rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static const ta_diagram_box *box_of(const ta_diagram_layout *layout, uint32_t state) {
    for (uint32_t i = 0; i < layout->box_count; i++)
        if (layout->boxes[i].state == state) return &layout->boxes[i];
    return NULL;
}

/* Every box is inside its parent's and clear of its siblings'; every route is orthogonal and
 * runs from its source's box to its target's. */
static void test_layout_properties(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn d;
    CHECK_EQUAL(lay_out(&a, &order, &d), TA_OK);
    const ta_diagram_layout *layout = &d.layout;
    CHECK_EQUAL(layout->box_count, 9);
    CHECK(layout->boxes[0].kind == TA_DIAGRAM_BOX_ROOT);
    CHECK(box_of(layout, 6)->kind == TA_DIAGRAM_BOX_REGION);
    CHECK(box_of(layout, 4)->kind == TA_DIAGRAM_BOX_HISTORY);
    CHECK(box_of(layout, 8)->kind == TA_DIAGRAM_BOX_FINAL);
    for (uint32_t i = 1; i < layout->box_count; i++) {
        const ta_diagram_box *box = &layout->boxes[i];
        uint32_t parent = order.states[box->state].parent;
        CHECK(inside(box->rect, box_of(layout, parent)->rect));
        for (uint32_t j = 1; j < layout->box_count; j++) {
            const ta_diagram_box *other = &layout->boxes[j];
            if (j != i && order.states[other->state].parent == parent) CHECK(!overlap(box->rect, other->rect));
        }
    }
    /* five routed edges (item.added has no target): it is listed in paying's box */
    CHECK_EQUAL(layout->edge_count, 5);
    CHECK_EQUAL(layout->inside_count[3], 1);
    CHECK_EQUAL(layout->inside[layout->inside_first[3]], 3);
    for (uint32_t i = 0; i < layout->edge_count; i++) {
        const ta_diagram_edge *edge = &layout->edges[i];
        const ta_diagram_point *p = &layout->points[edge->first_point];
        CHECK(edge->point_count >= 2);
        for (uint32_t j = 1; j < edge->point_count; j++)
            CHECK(p[j].x == p[j - 1].x || p[j].y == p[j - 1].y);
        ta_diagram_rect source = box_of(layout, order.transitions[edge->transition].source)->rect;
        ta_diagram_rect target = box_of(layout, edge->target)->rect;
        ta_diagram_rect start = {p[0].x - 1, p[0].y - 1, 2, 2};
        ta_diagram_rect end = {p[edge->point_count - 1].x - 1, p[edge->point_count - 1].y - 1, 2, 2};
        CHECK(overlap(start, source));
        CHECK(overlap(end, target));
    }
    CHECK(layout->edges[2].kind == TA_DIAGRAM_EDGE_SELF);
    /* every label is on its own: none overlaps another, nor any leaf's box */
    for (uint32_t i = 0; i < layout->edge_count; i++) {
        if (!layout->edges[i].labelled) continue;
        for (uint32_t j = i + 1; j < layout->edge_count; j++)
            if (layout->edges[j].labelled) CHECK(!overlap(layout->edges[i].label, layout->edges[j].label));
    }
    /* initial markers: the root's (checkout) and checkout's (open); a parallel state has none */
    CHECK_EQUAL(layout->initial_count, 2);
    ta_growable_arena_free(&a.arena);
}

/* The same chart lays out the same way, to the bit. */
static void test_deterministic(void) {
    test_arena a, b;
    arena_begin(&a, (size_t)-1);
    arena_begin(&b, (size_t)-1);
    drawn first, second;
    CHECK_EQUAL(lay_out(&a, &order, &first), TA_OK);
    CHECK_EQUAL(lay_out(&b, &order, &second), TA_OK);
    CHECK(memcmp(first.layout.boxes, second.layout.boxes, sizeof(ta_diagram_box) * first.layout.box_count) == 0);
    CHECK_EQUAL(first.layout.edge_count, second.layout.edge_count);
    CHECK(memcmp(first.layout.edges, second.layout.edges, sizeof(ta_diagram_edge) * first.layout.edge_count) == 0);
    ta_growable_arena_free(&a.arena);
    ta_growable_arena_free(&b.arena);
}

/* A down layout is the right one transposed: layers run top to bottom. */
static void test_down(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn d;
    CHECK_EQUAL(lay_out(&a, &order, &d), TA_OK);
    ta_diagram_request request = {&order, TA_DIAGRAM_DOWN, 0, d.measures.states, d.measures.labels, d.portals, &a.arena};
    ta_diagram_layout down;
    CHECK_EQUAL(ta_diagram_layout_chart(&request, &down), TA_OK);
    /* checkout's open comes before paying: to its left, or above it */
    CHECK(box_of(&d.layout, 2)->rect.x < box_of(&d.layout, 3)->rect.x);
    CHECK(box_of(&down, 2)->rect.y < box_of(&down, 3)->rect.y);
    ta_growable_arena_free(&a.arena);
}

/* A fulfilment of three regions, with edges between them both ways: from a box above another box
 * of its own region, into a box with a sibling beyond it, across two regions. */
static const ta_diagram_state fulfilment_states[] = {
    {TA_NIL, TA_DIAGRAM_STATE_ROOT, {"", 0}, 0, 1, 0},             /* 0 */
    {0, TA_DIAGRAM_STATE_PARALLEL, {"fulfilment", 10}, 0, 0, 0},   /* 1 */
    {1, TA_DIAGRAM_STATE_STATE, {"picking", 7}, 1, 1, 0},          /* 2 */
    {2, TA_DIAGRAM_STATE_STATE, {"locating", 8}, 0, 0, 0},         /* 3 */
    {2, TA_DIAGRAM_STATE_STATE, {"picked", 6}, 0, 0, 0},           /* 4 */
    {1, TA_DIAGRAM_STATE_STATE, {"invoicing", 9}, 2, 1, 0},        /* 5 */
    {5, TA_DIAGRAM_STATE_STATE, {"drafting", 8}, 0, 0, 0},         /* 6 */
    {5, TA_DIAGRAM_STATE_STATE, {"issued", 6}, 0, 0, 0},           /* 7 */
    {5, TA_DIAGRAM_STATE_STATE, {"backorder", 9}, 0, 0, 0},        /* 8 */
    {1, TA_DIAGRAM_STATE_STATE, {"shipping", 8}, 3, 1, 0},         /* 9 */
    {9, TA_DIAGRAM_STATE_STATE, {"awaiting", 8}, 0, 0, 0},         /* 10 */
    {9, TA_DIAGRAM_STATE_STATE, {"labelled", 8}, 0, 0, 0},         /* 11 */
};
static const uint32_t fulfilment_targets[] = {1, 3, 6, 10, 4, 8, 11, 7, 3, 11, 6, 3};
static const ta_diagram_transition fulfilment_transitions[] = {
    {3, 4, 1, {"found", 5}, {"", 0}},    {3, 5, 1, {"short", 5}, {"", 0}},  {4, 6, 1, {"ship", 4}, {"", 0}},
    {6, 7, 1, {"issue", 5}, {"", 0}},    {8, 8, 1, {"restock", 7}, {"", 0}}, {10, 9, 1, {"label", 5}, {"", 0}},
    {11, 10, 1, {"bill", 4}, {"", 0}},   {10, 11, 1, {"recall", 6}, {"", 0}},
};
static const ta_diagram_chart fulfilment = {fulfilment_states, 12, fulfilment_transitions, 8, fulfilment_targets};

/* No route between regions runs through a box, right, down, and down in a narrow space (the
 * regions then stack), and the layout breaks none of the hard rules. */
static void test_between_regions(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn d;
    CHECK_EQUAL(lay_out(&a, &fulfilment, &d), TA_OK);
    static const struct {
        uint32_t direction;
        double max_width;
    } ways[] = {{TA_DIAGRAM_RIGHT, 0}, {TA_DIAGRAM_DOWN, 0}, {TA_DIAGRAM_DOWN, 300}};
    for (size_t w = 0; w < sizeof ways / sizeof ways[0]; w++) {
        ta_diagram_request request = {&fulfilment, ways[w].direction, ways[w].max_width, d.measures.states,
                                      d.measures.labels, d.portals, &a.arena};
        ta_diagram_layout layout;
        CHECK_EQUAL(ta_diagram_layout_chart(&request, &layout), TA_OK);
        ta_diagram_check_request check = {&fulfilment, &layout, &a.arena};
        ta_diagram_problems problems;
        CHECK_EQUAL(ta_diagram_check(&check, &problems), TA_OK);
        for (uint32_t i = 0; i < problems.count; i++) {
            uint32_t kind = problems.list[i].kind;
            CHECK(kind != TA_DIAGRAM_PROBLEM_THROUGH_BOX && kind != TA_DIAGRAM_PROBLEM_START_DETACHED &&
                  kind != TA_DIAGRAM_PROBLEM_END_DETACHED && kind != TA_DIAGRAM_PROBLEM_INITIAL_DETACHED &&
                  kind != TA_DIAGRAM_PROBLEM_NOT_ORTHOGONAL && kind != TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT &&
                  kind != TA_DIAGRAM_PROBLEM_BOXES_OVERLAP);
        }
    }
    ta_growable_arena_free(&a.arena);
}


/* The hard problems: what no layout may show. */
static const uint32_t hard_problems[] = {TA_DIAGRAM_PROBLEM_START_DETACHED, TA_DIAGRAM_PROBLEM_END_DETACHED,
                                         TA_DIAGRAM_PROBLEM_INITIAL_DETACHED, TA_DIAGRAM_PROBLEM_NOT_ORTHOGONAL,
                                         TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT, TA_DIAGRAM_PROBLEM_BOXES_OVERLAP};

static uint32_t hard_count(test_arena *a, const ta_diagram_chart *chart, const ta_diagram_layout *layout) {
    ta_diagram_check_request request = {chart, layout, &a->arena};
    ta_diagram_problems problems;
    if (ta_diagram_check(&request, &problems) != TA_OK) return UINT32_MAX;
    uint32_t count = 0;
    for (uint32_t i = 0; i < problems.count; i++)
        for (size_t k = 0; k < sizeof hard_problems / sizeof hard_problems[0]; k++)
            count += problems.list[i].kind == hard_problems[k];
    return count;
}

/* A chart laid out in a direction, with its own measures. */
static ta_status lay_out_toward(test_arena *a, const ta_diagram_chart *chart, uint32_t direction, drawn *out) {
    out->portals = ta_diagram_portals(chart, &a->arena);
    if (!out->portals) return TA_ERROR_NO_MEMORY;
    ta_diagram_measure_request measure = {chart, out->portals, &a->arena};
    ta_status status = ta_diagram_measure(&measure, &out->measures);
    if (status != TA_OK) return status;
    ta_diagram_request request = {chart, direction, 0, out->measures.states, out->measures.labels, out->portals, &a->arena};
    return ta_diagram_layout_chart(&request, &out->layout);
}

/* A support ticket closed from four of its states: the final many edges arrive at. */
static const ta_diagram_state ticket_states[] = {
    {TA_NIL, TA_DIAGRAM_STATE_ROOT, {"", 0}, 0, 1, 0},   /* 0 */
    {0, TA_DIAGRAM_STATE_STATE, {"triage", 6}, 0, 0, 0},  /* 1 */
    {0, TA_DIAGRAM_STATE_STATE, {"working", 7}, 0, 0, 0}, /* 2 */
    {0, TA_DIAGRAM_STATE_STATE, {"waiting", 7}, 0, 0, 0}, /* 3 */
    {0, TA_DIAGRAM_STATE_STATE, {"review", 6}, 0, 0, 0},  /* 4 */
    {0, TA_DIAGRAM_STATE_FINAL, {"closed", 6}, 0, 0, 0},  /* 5 */
};
static const uint32_t ticket_targets[] = {1, 2, 3, 4, 5, 5, 5, 5};
static const ta_diagram_transition ticket_transitions[] = {
    {1, 1, 1, {"assign", 6}, {"", 0}}, {2, 2, 1, {"ask", 3}, {"", 0}},     {3, 3, 1, {"answer", 6}, {"", 0}},
    {1, 4, 1, {"close", 5}, {"", 0}},  {2, 5, 1, {"close", 5}, {"", 0}},   {3, 6, 1, {"close", 5}, {"", 0}},
    {4, 7, 1, {"approve", 7}, {"", 0}},
};
static const ta_diagram_chart ticket = {ticket_states, 6, ticket_transitions, 7, ticket_targets};

/* Down, the default of the command: the charts here break no hard rule, and a final four edges
 * arrive at grows wide enough to keep their arrows apart. */
static void test_down_charts(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    const ta_diagram_chart *charts[] = {&order, &fulfilment, &ticket};
    for (size_t i = 0; i < sizeof charts / sizeof charts[0]; i++) {
        drawn d;
        CHECK_EQUAL(lay_out_toward(&a, charts[i], TA_DIAGRAM_DOWN, &d), TA_OK);
        CHECK_EQUAL(hard_count(&a, charts[i], &d.layout), 0);
    }
    drawn d;
    CHECK_EQUAL(lay_out_toward(&a, &ticket, TA_DIAGRAM_DOWN, &d), TA_OK);
    const ta_diagram_box *closed = box_of(&d.layout, 5);
    CHECK(closed->rect.w >= 5 * 12 + closed->rect.h);
    double least = 1e9;
    for (uint32_t i = 0; i < d.layout.edge_count; i++)
        for (uint32_t j = i + 1; j < d.layout.edge_count; j++) {
            const ta_diagram_edge *e = &d.layout.edges[i], *f = &d.layout.edges[j];
            if (e->target != 5 || f->target != 5) continue;
            double x = d.layout.points[e->first_point + e->point_count - 1].x;
            double y = d.layout.points[f->first_point + f->point_count - 1].x;
            double gap = x > y ? x - y : y - x;
            if (gap < least) least = gap;
        }
    CHECK(least >= 10);
    ta_growable_arena_free(&a.arena);
}

/* Folding checkout: one box standing for open and paying, the edges of its hidden states
 * leaving from it, the root's initial pointing at it, what stays inside it gone. */
static void test_fold(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    uint8_t folded[9] = {0};
    folded[1] = 1; /* checkout */
    folded[2] = 1; /* open: no container, never folded */
    ta_diagram_fold_request request = {&order, folded, &a.arena};
    ta_diagram_folding f;
    CHECK_EQUAL(ta_diagram_fold(&request, &f), TA_OK);
    CHECK_EQUAL(f.chart.state_count, 6); /* open, paying and the history state go */
    uint32_t checkout = f.drawn_state[1];
    CHECK_EQUAL(f.original_state[checkout], 1);
    CHECK_EQUAL(f.drawn_state[2], checkout);
    CHECK_EQUAL(f.drawn_state[3], checkout);
    CHECK_EQUAL(f.drawn_state[4], checkout);
    CHECK_EQUAL(f.chart.states[checkout].hidden, 2); /* open and paying, not the history state */
    CHECK_EQUAL(f.chart.states[checkout].initial_count, 0);
    CHECK_EQUAL(f.chart.targets[f.chart.states[0].first_initial], checkout);
    CHECK_EQUAL(f.drawn_transition[0], TA_NIL); /* open -> paying: inside */
    CHECK_EQUAL(f.drawn_transition[2], TA_NIL); /* paying's loop */
    CHECK_EQUAL(f.drawn_transition[3], TA_NIL); /* paying's targetless */
    CHECK_EQUAL(f.drawn_transition[5], TA_NIL); /* the history state's default */
    uint32_t leaving = f.drawn_transition[1];   /* paying -> fulfilment: from the box */
    CHECK(leaving != TA_NIL);
    CHECK_EQUAL(f.chart.transitions[leaving].source, checkout);
    CHECK_EQUAL(f.chart.transitions[leaving].events.length, 12);
    drawn d;
    CHECK_EQUAL(lay_out_toward(&a, &f.chart, TA_DIAGRAM_DOWN, &d), TA_OK);
    const ta_diagram_box *box = box_of(&d.layout, checkout);
    CHECK_EQUAL(box->kind, TA_DIAGRAM_BOX_FOLDED);
    CHECK(box->rect.w >= 96);
    CHECK_EQUAL(d.layout.inside_count[checkout], 0);
    CHECK_EQUAL(d.layout.initials[0].target, checkout);
    int attached = 0;
    for (uint32_t i = 0; i < d.layout.edge_count; i++)
        attached |= d.layout.edges[i].transition == leaving;
    CHECK(attached);
    CHECK_EQUAL(hard_count(&a, &f.chart, &d.layout), 0);
    CHECK_EQUAL(lay_out_toward(&a, &f.chart, TA_DIAGRAM_RIGHT, &d), TA_OK);
    CHECK_EQUAL(hard_count(&a, &f.chart, &d.layout), 0);
    /* a parallel state folds the same way; nothing folded is the chart itself */
    folded[1] = 0;
    folded[5] = 1;
    CHECK_EQUAL(ta_diagram_fold(&request, &f), TA_OK);
    CHECK_EQUAL(f.chart.states[f.drawn_state[5]].hidden, 2);
    CHECK_EQUAL(lay_out_toward(&a, &f.chart, TA_DIAGRAM_DOWN, &d), TA_OK);
    CHECK_EQUAL(box_of(&d.layout, f.drawn_state[5])->kind, TA_DIAGRAM_BOX_FOLDED);
    CHECK_EQUAL(hard_count(&a, &f.chart, &d.layout), 0);
    request.folded = NULL;
    CHECK_EQUAL(ta_diagram_fold(&request, &f), TA_OK);
    CHECK_EQUAL(f.chart.state_count, order.state_count);
    CHECK_EQUAL(f.chart.transition_count, order.transition_count);
    /* a chart that is not a tree is refused */
    ta_diagram_state backwards[2] = {{1, TA_DIAGRAM_STATE_ROOT, {"", 0}, 0, 0, 0}, {0, TA_DIAGRAM_STATE_STATE, {"open", 4}, 0, 0, 0}};
    ta_diagram_chart broken = {backwards, 2, NULL, 0, order_targets};
    request.chart = &broken;
    CHECK_EQUAL(ta_diagram_fold(&request, &f), TA_ERROR_INVALID);
    ta_growable_arena_free(&a.arena);
    /* an arena that stops growing fails the fold, never more */
    for (size_t budget = 0;; budget += 256) {
        test_arena small;
        arena_begin(&small, budget);
        ta_diagram_fold_request tight = {&order, folded, &small.arena};
        ta_status status = ta_diagram_fold(&tight, &f);
        CHECK(status == TA_OK || status == TA_ERROR_NO_MEMORY);
        ta_growable_arena_free(&small.arena);
        if (status == TA_OK || budget > 100000) break;
    }
}

/* A chart that is not a tree in document order, or a target past its states, is refused. */
static void test_refusals(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    ta_diagram_state backwards[3] = {order_states[0], {2, TA_DIAGRAM_STATE_STATE, {"open", 4}, 0, 0, 0},
                                     {0, TA_DIAGRAM_STATE_STATE, {"paid", 4}, 0, 0, 0}};
    ta_diagram_chart chart = {backwards, 3, NULL, 0, order_targets};
    ta_diagram_request request = {&chart, TA_DIAGRAM_RIGHT, 0, NULL, NULL, NULL, &a.arena};
    ta_diagram_layout layout;
    CHECK_EQUAL(ta_diagram_layout_chart(&request, &layout), TA_ERROR_INVALID);
    static const uint32_t far[] = {1, 7};
    ta_diagram_transition away = {1, 1, 1, {"go", 2}, {"", 0}};
    ta_diagram_state two[2] = {{TA_NIL, TA_DIAGRAM_STATE_ROOT, {"", 0}, 0, 1, 0}, {0, TA_DIAGRAM_STATE_STATE, {"open", 4}, 0, 0, 0}};
    chart = (ta_diagram_chart){two, 2, &away, 1, far};
    CHECK_EQUAL(ta_diagram_layout_chart(&request, &layout), TA_ERROR_INVALID);
    ta_growable_arena_free(&a.arena);
}

/* An arena that stops growing at any point fails the layout, never more. */
static void test_no_memory(void) {
    for (size_t budget = 0; budget < 200000; budget += 2048) {
        test_arena a;
        arena_begin(&a, budget);
        drawn d;
        ta_status status = lay_out(&a, &order, &d);
        CHECK(status == TA_OK || status == TA_ERROR_NO_MEMORY);
        ta_growable_arena_free(&a.arena);
        if (status == TA_OK) return;
    }
    CHECK(0); /* never had enough */
}

static int contains(ta_string_view text, const char *needle) {
    size_t n = strlen(needle);
    for (uint32_t i = 0; i + n <= text.length; i++)
        if (memcmp(text.pointer + i, needle, n) == 0) return 1;
    return 0;
}

/* The SVG: one well-formed document, every name and expression escaped by the writer. */
static void test_svg(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn d;
    CHECK_EQUAL(lay_out(&a, &order, &d), TA_OK);
    ta_xml_writer *writer;
    CHECK_EQUAL(ta_xml_writer_new(NULL, &writer), TA_OK);
    ta_diagram_page page = {{d.layout.width + 24, d.layout.height + 54}, TA_DIAGRAM_THEME_AUTO};
    CHECK_EQUAL(ta_diagram_svg_open(writer, &page), TA_OK);
    ta_diagram_drawing drawing = {&order, &d.layout, d.portals, {"order", 5}, 12, &a.arena, NULL, NULL, 0};
    CHECK_EQUAL(ta_diagram_svg(writer, &drawing), TA_OK);
    CHECK_EQUAL(ta_diagram_svg_close(writer), TA_OK);
    ta_string_view svg;
    CHECK_EQUAL(ta_xml_writer_finish(writer, &svg), TA_OK);
    CHECK(contains(svg, "&lt;paid &amp; \"done\"&gt;") || contains(svg, "&lt;paid &amp; \"done\">"));
    CHECK(contains(svg, "<tspan class=\"ta-condition\">[attempts &lt; 3]</tspan>"));
    CHECK(contains(svg, "<tspan class=\"ta-done\">done.state.fulfilment</tspan>")); /* a completion */
    CHECK(contains(svg, "<circle class=\"ta-history\""));
    CHECK(contains(svg, ">H</text>"));
    CHECK(contains(svg, "<path class=\"ta-edge\" d=\"M"));
    CHECK(contains(svg, "--ta-fg1: #1c1c21;"));
    ta_xml_writer_free(writer);
    ta_growable_arena_free(&a.arena);
}


/* An SVG of `chart` drawn as `drawing` says, on a page of `theme`. */
static ta_string_view drawn_svg(test_arena *a, const ta_diagram_drawing *drawing, uint32_t theme) {
    ta_allocator allocator = ta_growable_arena_allocator(&a->arena);
    ta_xml_writer *writer;
    if (ta_xml_writer_new(&allocator, &writer) != TA_OK) return (ta_string_view){"", 0};
    ta_diagram_page page = {{drawing->layout->width + 24, drawing->layout->height + 54}, theme};
    ta_diagram_svg_open(writer, &page);
    ta_diagram_svg(writer, drawing);
    ta_diagram_svg_close(writer);
    ta_string_view svg = {"", 0};
    if (ta_xml_writer_finish(writer, &svg) != TA_OK) svg = (ta_string_view){"", 0};
    return svg; /* the writer's memory is the arena's */
}

static uint32_t occurrences(ta_string_view text, const char *needle) {
    size_t n = strlen(needle);
    uint32_t count = 0;
    for (uint32_t i = 0; i + n <= text.length; i++)
        count += memcmp(text.pointer + i, needle, n) == 0;
    return count;
}

/* The themes: auto writes both token sets, the dark one under the reader's preference; light
 * and dark write only their own. */
static void test_themes(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn d;
    CHECK_EQUAL(lay_out(&a, &order, &d), TA_OK);
    ta_diagram_drawing drawing = {&order, &d.layout, d.portals, {"order", 5}, 12, &a.arena, NULL, NULL, 0};
    ta_string_view svg = drawn_svg(&a, &drawing, TA_DIAGRAM_THEME_AUTO);
    CHECK_EQUAL(occurrences(svg, "--ta-fg1:"), 2);
    CHECK(contains(svg, "@media (prefers-color-scheme: dark)"));
    svg = drawn_svg(&a, &drawing, TA_DIAGRAM_THEME_LIGHT);
    CHECK_EQUAL(occurrences(svg, "--ta-fg1:"), 1);
    CHECK(contains(svg, "--ta-fg1: #1c1c21;"));
    CHECK(contains(svg, "--ta-path: #1f6feb;"));
    CHECK(!contains(svg, "@media"));
    svg = drawn_svg(&a, &drawing, TA_DIAGRAM_THEME_DARK);
    CHECK_EQUAL(occurrences(svg, "--ta-fg1:"), 1);
    CHECK(contains(svg, "--ta-fg1: #ececf1;"));
    CHECK(contains(svg, "--ta-highlight: #f5b03d;"));
    CHECK(!contains(svg, "@media"));
    ta_growable_arena_free(&a.arena);
}

/* Marks are classes: a highlighted box, a path's boxes and edges, its steps numbered; and a
 * folded box is drawn on its stack with its count. */
static void test_marks(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn d;
    CHECK_EQUAL(lay_out(&a, &order, &d), TA_OK);
    uint8_t marks[9] = {0};
    marks[8] = TA_DIAGRAM_MARK_HIGHLIGHT;                   /* paid */
    marks[2] = marks[3] = TA_DIAGRAM_MARK_PATH;             /* open, paying */
    static const uint32_t path[] = {0, 2, 2, TA_NIL};       /* checkout, then the retry twice, then one hidden */
    ta_diagram_drawing drawing = {&order, &d.layout, d.portals, {"order", 5}, 12, &a.arena, marks, path, 4};
    ta_string_view svg = drawn_svg(&a, &drawing, TA_DIAGRAM_THEME_AUTO);
    CHECK(contains(svg, "class=\"ta-leaf ta-highlight\""));
    CHECK_EQUAL(occurrences(svg, "class=\"ta-leaf ta-path\""), 2);
    CHECK(contains(svg, "<path class=\"ta-edge ta-path\""));
    CHECK(contains(svg, "marker-end=\"url(#tinyactors-arrow-path)\""));
    CHECK(contains(svg, ">1</text>"));
    CHECK(contains(svg, ">2,3</text>")); /* one transition taken twice: one badge */
    CHECK(contains(svg, "class=\"ta-pill ta-path\""));
    CHECK(!contains(svg, ">4</text>"));
    /* a folded checkout */
    uint8_t folded[9] = {0, 1};
    ta_diagram_fold_request request = {&order, folded, &a.arena};
    ta_diagram_folding f;
    CHECK_EQUAL(ta_diagram_fold(&request, &f), TA_OK);
    drawn g;
    CHECK_EQUAL(lay_out_toward(&a, &f.chart, TA_DIAGRAM_DOWN, &g), TA_OK);
    ta_diagram_drawing folded_drawing = {&f.chart, &g.layout, g.portals, {"order", 5}, 12, &a.arena, NULL, NULL, 0};
    svg = drawn_svg(&a, &folded_drawing, TA_DIAGRAM_THEME_AUTO);
    CHECK(contains(svg, "class=\"ta-folded-stack\""));
    CHECK(contains(svg, "class=\"ta-leaf ta-folded\""));
    CHECK(contains(svg, ">2 states</text>"));
    ta_growable_arena_free(&a.arena);
}

/* A layout's copy that a test may damage: its boxes, edges, initials and points. */
typedef struct damaged {
    ta_diagram_box boxes[16];
    ta_diagram_edge edges[16];
    ta_diagram_initial initials[8];
    ta_diagram_point points[256];
    ta_diagram_layout layout;
} damaged;

static void damage_begin(damaged *d, const ta_diagram_layout *from) {
    memcpy(d->boxes, from->boxes, sizeof(ta_diagram_box) * from->box_count);
    memcpy(d->edges, from->edges, sizeof(ta_diagram_edge) * from->edge_count);
    memcpy(d->initials, from->initials, sizeof(ta_diagram_initial) * from->initial_count);
    uint32_t points = 0;
    for (uint32_t i = 0; i < from->edge_count; i++)
        if (from->edges[i].first_point + from->edges[i].point_count > points) points = from->edges[i].first_point + from->edges[i].point_count;
    for (uint32_t i = 0; i < from->initial_count; i++)
        if (from->initials[i].first_point + from->initials[i].point_count > points) points = from->initials[i].first_point + from->initials[i].point_count;
    memcpy(d->points, from->points, sizeof(ta_diagram_point) * points);
    d->layout = *from;
    d->layout.boxes = d->boxes;
    d->layout.edges = d->edges;
    d->layout.initials = d->initials;
    d->layout.points = d->points;
}

/* The problems of a kind the check finds in `layout`. */
static uint32_t problems_of(test_arena *a, const ta_diagram_layout *layout, uint32_t kind) {
    ta_diagram_check_request request = {&order, layout, &a->arena};
    ta_diagram_problems problems;
    if (ta_diagram_check(&request, &problems) != TA_OK) return UINT32_MAX;
    uint32_t count = 0;
    for (uint32_t i = 0; i < problems.count; i++)
        count += problems.list[i].kind == kind;
    return count;
}

static ta_diagram_box *damaged_box(damaged *d, uint32_t state) {
    for (uint32_t i = 0; i < d->layout.box_count; i++)
        if (d->boxes[i].state == state) return &d->boxes[i];
    return NULL;
}

/* Each kind of fault, made in a good layout, is found, and only where it was made. */
static void test_check(void) {
    test_arena a;
    arena_begin(&a, (size_t)-1);
    drawn good;
    CHECK_EQUAL(lay_out(&a, &order, &good), TA_OK);
    static const uint32_t hard[] = {TA_DIAGRAM_PROBLEM_START_DETACHED, TA_DIAGRAM_PROBLEM_END_DETACHED,
                                    TA_DIAGRAM_PROBLEM_INITIAL_DETACHED, TA_DIAGRAM_PROBLEM_NOT_ORTHOGONAL,
                                    TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT, TA_DIAGRAM_PROBLEM_BOXES_OVERLAP};
    for (size_t i = 0; i < sizeof hard / sizeof hard[0]; i++)
        CHECK_EQUAL(problems_of(&a, &good.layout, hard[i]), 0);

    damaged d;
    /* an arrow that starts 4px off its box */
    damage_begin(&d, &good.layout);
    d.points[d.edges[0].first_point].x -= 4;
    CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_START_DETACHED), 1);
    /* an arrow that stops 3px short of its box; with its first point moved too, a bent segment */
    damage_begin(&d, &good.layout);
    ta_diagram_point *last = &d.points[d.edges[0].first_point + d.edges[0].point_count - 1];
    last->x -= 3;
    CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_END_DETACHED), 1);
    last->y += 5;
    CHECK(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_NOT_ORTHOGONAL) >= 1);
    /* the final's pill: an arrow at its rectangle's corner is off its curve */
    damage_begin(&d, &good.layout);
    for (uint32_t i = 0; i < d.layout.edge_count; i++) {
        if (d.edges[i].target != 8) continue;
        ta_diagram_rect r = damaged_box(&d, 8)->rect;
        ta_diagram_point *end = &d.points[d.edges[i].first_point + d.edges[i].point_count - 1];
        ta_diagram_point *before = end - 1;
        *end = (ta_diagram_point){r.x, r.y + 2};
        *before = (ta_diagram_point){r.x - 20, r.y + 2};
        CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_END_DETACHED), 1);
    }
    /* an initial marker short of its state */
    damage_begin(&d, &good.layout);
    d.points[d.initials[0].first_point + 1].x -= 5;
    CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_INITIAL_DETACHED), 1);
    /* a box past its parent, and over its sibling */
    damage_begin(&d, &good.layout);
    damaged_box(&d, 3)->rect.x += 2000;
    CHECK(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT) >= 1);
    damage_begin(&d, &good.layout);
    damaged_box(&d, 3)->rect = damaged_box(&d, 2)->rect;
    CHECK(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_BOXES_OVERLAP) >= 1);
    /* a route through a box: open's straight to paying, moved to run through paid */
    damage_begin(&d, &good.layout);
    ta_diagram_rect paid = damaged_box(&d, 8)->rect;
    d.points[d.edges[0].first_point] = (ta_diagram_point){paid.x - 10, paid.y + paid.h / 2};
    d.points[d.edges[0].first_point + 1] = (ta_diagram_point){paid.x + paid.w + 10, paid.y + paid.h / 2};
    d.edges[0].point_count = 2;
    CHECK(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_THROUGH_BOX) >= 1);
    /* a label on a box */
    damage_begin(&d, &good.layout);
    for (uint32_t i = 0; i < d.layout.edge_count; i++)
        if (d.edges[i].labelled) {
            d.edges[i].label.x = damaged_box(&d, 2)->rect.x;
            d.edges[i].label.y = damaged_box(&d, 2)->rect.y;
            break;
        }
    CHECK(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_LABEL_ON_BOX) >= 1);
    /* a label beside its own route stays, one far from it is astray */
    CHECK_EQUAL(problems_of(&a, &good.layout, TA_DIAGRAM_PROBLEM_LABEL_ASTRAY), 0);
    for (uint32_t i = 0; i < good.layout.edge_count; i++) {
        if (!good.layout.edges[i].labelled) continue;
        damage_begin(&d, &good.layout);
        ta_diagram_rect *label = &d.edges[i].label;
        ta_diagram_point start = d.points[d.edges[i].first_point];
        label->x = start.x + label->h / 2;
        label->y = start.y + label->h / 2;
        CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_LABEL_ASTRAY), 0);
        label->x = start.x + 2 * label->h;
        label->y = start.y + 2 * label->h;
        CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_LABEL_ASTRAY), 1);
        label->x = d.layout.width + 100;
        CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_LABEL_ASTRAY), 1);
        break;
    }
    /* another transition's edge run 1px beside an edge's last segment reads as one line with it;
     * arriving at the same point only within a few px of it is a shared port */
    CHECK_EQUAL(problems_of(&a, &good.layout, TA_DIAGRAM_PROBLEM_EDGES_OVERLAP), 0);
    uint32_t tried = 0;
    for (uint32_t i = 0; i < good.layout.edge_count && !tried; i++) {
        const ta_diagram_edge *one = &good.layout.edges[i];
        const ta_diagram_point *p = &good.layout.points[one->first_point];
        ta_diagram_point end = p[one->point_count - 1], before = p[one->point_count - 2];
        double dx = end.x - before.x, dy = end.y - before.y, length = dx < 0 ? -dx : dx;
        length += dy < 0 ? -dy : dy;
        if (length < 40) continue;
        dx /= length;
        dy /= length;
        for (uint32_t j = 0; j < good.layout.edge_count; j++) {
            if (good.layout.edges[j].transition == one->transition) continue;
            static const uint32_t spare = 240; /* past every point of the good layout */
            CHECK(one->first_point + one->point_count < spare);
            damage_begin(&d, &good.layout);
            d.edges[j].first_point = spare;
            d.edges[j].point_count = 3;
            /* from the side, then 8px along the last segment into its end */
            d.points[spare] = (ta_diagram_point){end.x - 8 * dx - 30 * dy, end.y - 8 * dy + 30 * dx};
            d.points[spare + 1] = (ta_diagram_point){end.x - 8 * dx, end.y - 8 * dy};
            d.points[spare + 2] = end;
            CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_EDGES_OVERLAP), 0);
            /* 30px along it, into the same end: it runs with it */
            d.points[spare] = (ta_diagram_point){end.x - 30 * dx - 30 * dy, end.y - 30 * dy + 30 * dx};
            d.points[spare + 1] = (ta_diagram_point){end.x - 30 * dx, end.y - 30 * dy};
            CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_EDGES_OVERLAP), 1);
            /* 1px beside it, the whole last segment, to no shared end */
            d.edges[j].point_count = 2;
            d.points[spare] = (ta_diagram_point){before.x + dy, before.y - dx};
            d.points[spare + 1] = (ta_diagram_point){end.x + dy, end.y - dx};
            CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_EDGES_OVERLAP), 1);
            /* 4px beside it: two lines */
            d.points[spare] = (ta_diagram_point){before.x + 4 * dy, before.y - 4 * dx};
            d.points[spare + 1] = (ta_diagram_point){end.x + 4 * dy, end.y - 4 * dx};
            CHECK_EQUAL(problems_of(&a, &d.layout, TA_DIAGRAM_PROBLEM_EDGES_OVERLAP), 0);
            tried = 1;
            break;
        }
    }
    CHECK(tried);
    CHECK(ta_diagram_problem_name(TA_DIAGRAM_PROBLEM_END_DETACHED).length == 11);
    CHECK(ta_diagram_problem_name(TA_DIAGRAM_PROBLEM_EDGES_OVERLAP).length == 12);
    ta_growable_arena_free(&a.arena);
}

int main(void) {
    RUN(test_layout_properties);
    RUN(test_deterministic);
    RUN(test_down);
    RUN(test_between_regions);
    RUN(test_down_charts);
    RUN(test_fold);
    RUN(test_refusals);
    RUN(test_no_memory);
    RUN(test_svg);
    RUN(test_themes);
    RUN(test_marks);
    RUN(test_check);
    TEST_MAIN_END();
}
