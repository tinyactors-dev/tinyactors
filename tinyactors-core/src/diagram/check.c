/* diagram/check.c — what a layout must never show, found by geometry alone (diagram.h,
 * ta_diagram_check): an arrow that stops short of its box or starts beside it, a route that
 * bends at an angle or runs through a box, an edge alone on a side that misses its middle,
 * boxes outside their parent or over each other, labels over labels or boxes. The outline is
 * the one drawn: a circle for a history state, a pill for a final, a rectangle for the rest.
 * Freestanding. */
#include "diagram/geometry.h"

static const double touching = 0.75; /* how far a point may be from an outline and still touch it */
static const double centred = 1.0;   /* how far an edge alone on a side may be from its middle */


typedef ta_diagram_point point;
typedef ta_diagram_rect rect;

/* ---- outlines ------------------------------------------------------------------------------ */

/* How far `p` is from the outline of `box` as drawn: 0 on it. */
static double from_outline(const ta_diagram_box *box, point p) {
    rect r = box->rect;
    uint32_t outline = ta_diagram_outline_of(box->kind);
    if (outline != TA_DIAGRAM_OUTLINE_RECTANGLE) { /* the points at the radius from the spine */
        ta_diagram_spine spine = ta_diagram_spine_of(r, outline);
        double sx = spine.b.x - spine.a.x, sy = spine.b.y - spine.a.y, length = sx * sx + sy * sy;
        double t = length != 0
                       ? ta_diagram_max(0, ta_diagram_min(1, ((p.x - spine.a.x) * sx + (p.y - spine.a.y) * sy) / length))
                       : 0;
        double dx = p.x - (spine.a.x + t * sx), dy = p.y - (spine.a.y + t * sy);
        return ta_diagram_abs(ta_diagram_root(dx * dx + dy * dy) - spine.radius);
    }
    int inside_x = p.x >= r.x && p.x <= r.x + r.w, inside_y = p.y >= r.y && p.y <= r.y + r.h;
    if (inside_x && inside_y)
        return ta_diagram_min(ta_diagram_min(p.x - r.x, r.x + r.w - p.x), ta_diagram_min(p.y - r.y, r.y + r.h - p.y));
    double dx = inside_x ? 0 : ta_diagram_min(ta_diagram_abs(p.x - r.x), ta_diagram_abs(p.x - r.x - r.w));
    double dy = inside_y ? 0 : ta_diagram_min(ta_diagram_abs(p.y - r.y), ta_diagram_abs(p.y - r.y - r.h));
    return ta_diagram_root(dx * dx + dy * dy);
}

typedef enum side { SIDE_LEFT = 0, SIDE_RIGHT = 1, SIDE_TOP = 2, SIDE_BOTTOM = 3, SIDES = 4 } side;

/* The side of a box's rectangle a point is nearest to. */
static uint32_t side_of(rect r, point p) {
    double distance[SIDES] = {ta_diagram_abs(p.x - r.x), ta_diagram_abs(p.x - r.x - r.w), ta_diagram_abs(p.y - r.y), ta_diagram_abs(p.y - r.y - r.h)};
    uint32_t best = SIDE_LEFT;
    for (uint32_t s = 1; s < SIDES; s++)
        if (distance[s] < distance[best]) best = s;
    return best;
}

static int is_leaf(const ta_diagram_box *box) {
    return box->kind == TA_DIAGRAM_BOX_ATOMIC || box->kind == TA_DIAGRAM_BOX_FINAL || box->kind == TA_DIAGRAM_BOX_HISTORY ||
           (box->kind == TA_DIAGRAM_BOX_REGION && box->header == 0);
}

/* 1 when the segment a-b passes through the inside of `r`, shrunk by `touching` all round. */
static int crosses(rect r, point a, point b) {
    rect inner = {r.x + touching, r.y + touching, r.w - 2 * touching, r.h - 2 * touching};
    if (inner.w <= 0 || inner.h <= 0) return 0;
    double x0 = ta_diagram_min(a.x, b.x), x1 = ta_diagram_max(a.x, b.x);
    double y0 = ta_diagram_min(a.y, b.y), y1 = ta_diagram_max(a.y, b.y);
    return x0 < inner.x + inner.w && inner.x < x1 && y0 < inner.y + inner.h && inner.y < y1;
}

/* The square of the distance from `p` to the segment a-b. */
static double to_segment(point p, point a, point b) {
    double sx = b.x - a.x, sy = b.y - a.y, length = sx * sx + sy * sy;
    double t = length != 0 ? ta_diagram_max(0, ta_diagram_min(1, ((p.x - a.x) * sx + (p.y - a.y) * sy) / length)) : 0;
    double dx = p.x - (a.x + t * sx), dy = p.y - (a.y + t * sy);
    return dx * dx + dy * dy;
}

/* The square of the distance between an axis-parallel segment a-b and a spine c-d, itself
 * axis-parallel or a point: 0 where they cross, else the least of the ends' distances. */
static double between_segments(point a, point b, point c, point d) {
    int ab_horizontal = ta_diagram_abs(a.y - b.y) < 0.01, cd_horizontal = ta_diagram_abs(c.y - d.y) < 0.01;
    if (ab_horizontal != cd_horizontal) { /* perpendicular: they cross where each spans the other */
        point h0 = ab_horizontal ? a : c, h1 = ab_horizontal ? b : d, v0 = ab_horizontal ? c : a, v1 = ab_horizontal ? d : b;
        if (v0.x >= ta_diagram_min(h0.x, h1.x) && v0.x <= ta_diagram_max(h0.x, h1.x) &&
            h0.y >= ta_diagram_min(v0.y, v1.y) && h0.y <= ta_diagram_max(v0.y, v1.y))
            return 0;
    }
    return ta_diagram_min(ta_diagram_min(to_segment(a, c, d), to_segment(b, c, d)),
                          ta_diagram_min(to_segment(c, a, b), to_segment(d, a, b)));
}

/* 1 when the segment a-b passes through the inside of the box's drawn outline. */
typedef struct segment {
    point a, b;
} segment;
static int through(const ta_diagram_box *box, segment s) {
    uint32_t outline = ta_diagram_outline_of(box->kind);
    if (outline == TA_DIAGRAM_OUTLINE_RECTANGLE) return crosses(box->rect, s.a, s.b);
    ta_diagram_spine spine = ta_diagram_spine_of(box->rect, outline);
    double inner = spine.radius - touching;
    return inner > 0 && between_segments(s.a, s.b, spine.a, spine.b) < inner * inner;
}

static int overlap(rect a, rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static int within(rect inner, rect outer) {
    return inner.x >= outer.x - touching && inner.y >= outer.y - touching &&
           inner.x + inner.w <= outer.x + outer.w + touching && inner.y + inner.h <= outer.y + outer.h + touching;
}

/* ---- the check ------------------------------------------------------------------------------ */

typedef struct checker {
    const ta_diagram_chart *chart;
    const ta_diagram_layout *layout;
    const ta_diagram_box **box_of; /* by state */
    ta_diagram_problem *list;
    uint32_t count, capacity;
} checker;

static void found(checker *c, ta_diagram_problem problem) {
    if (c->count < c->capacity) c->list[c->count++] = problem;
}

/* An end of a route: the box it touches, where, and the next point along the route. */
typedef struct end {
    const ta_diagram_box *box;
    point at, next;
} end;

/* Per box side, how many route ends touch it; an end alone on a leaf's side is at its middle. */
static uint32_t *side_counts(checker *c, ta_growable_arena *arena) {
    uint32_t *counts = (uint32_t *)ta_growable_arena_alloc(arena, sizeof(uint32_t) * (c->chart->state_count * SIDES + 1u), 8);
    if (!counts) return 0;
    for (uint32_t i = 0; i < c->chart->state_count * SIDES; i++)
        counts[i] = 0;
    const ta_diagram_layout *layout = c->layout;
    for (uint32_t i = 0; i < layout->edge_count; i++) {
        const ta_diagram_edge *edge = &layout->edges[i];
        const ta_diagram_point *p = &layout->points[edge->first_point];
        const ta_diagram_box *source = c->box_of[c->chart->transitions[edge->transition].source];
        const ta_diagram_box *target = c->box_of[edge->target];
        if (edge->point_count < 2) continue;
        counts[source->state * SIDES + side_of(source->rect, p[0])]++;
        counts[target->state * SIDES + side_of(target->rect, p[edge->point_count - 1])]++;
    }
    for (uint32_t i = 0; i < layout->initial_count; i++) {
        const ta_diagram_initial *marker = &layout->initials[i];
        const ta_diagram_box *target = c->box_of[marker->target];
        counts[target->state * SIDES + side_of(target->rect, layout->points[marker->first_point + marker->point_count - 1])]++;
    }
    return counts;
}

typedef struct end_check {
    end at;
    uint32_t subject;
    uint32_t detached;  /* the problem when the end is off its outline */
    int arrow;          /* the route's last end: it must run into its side */
    const uint32_t *counts;
} end_check;

static void check_end(checker *c, end_check e) {
    const ta_diagram_box *box = e.at.box;
    if (from_outline(box, e.at.at) > touching) {
        found(c, (ta_diagram_problem){e.detached, e.subject, box->state});
        return;
    }
    uint32_t side = side_of(box->rect, e.at.at);
    int horizontal = ta_diagram_abs(e.at.at.y - e.at.next.y) < 0.01 && ta_diagram_abs(e.at.at.x - e.at.next.x) >= 0.01;
    int along = side == SIDE_LEFT || side == SIDE_RIGHT ? !horizontal : horizontal;
    if (e.arrow && along && box->kind != TA_DIAGRAM_BOX_HISTORY)
        found(c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_ARROW_ALONG_SIDE, e.subject, box->state});
    if (!is_leaf(box) || !e.counts || e.counts[box->state * SIDES + side] != 1) return;
    rect r = box->rect;
    double middle = side == SIDE_LEFT || side == SIDE_RIGHT ? r.y + r.h / 2 : r.x + r.w / 2;
    double at = side == SIDE_LEFT || side == SIDE_RIGHT ? e.at.at.y : e.at.at.x;
    if (ta_diagram_abs(at - middle) > centred) found(c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_NOT_CENTRED, e.subject, box->state});
}

/* A route's segments: orthogonal, and through no leaf but the ones it leaves and enters. */
typedef struct route_check {
    const ta_diagram_point *p;
    uint32_t count, subject;
    const ta_diagram_box *from, *to;
} route_check;

static void check_route(checker *c, route_check r) {
    const ta_diagram_layout *layout = c->layout;
    for (uint32_t j = 1; j < r.count; j++) {
        point a = r.p[j - 1], b = r.p[j];
        if (ta_diagram_abs(a.x - b.x) >= 0.01 && ta_diagram_abs(a.y - b.y) >= 0.01)
            found(c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_NOT_ORTHOGONAL, r.subject, TA_NIL});
        for (uint32_t k = 0; k < layout->box_count; k++) {
            const ta_diagram_box *box = &layout->boxes[k];
            /* its own boxes too: a route touches them only at its ports, on their outlines */
            if (!is_leaf(box)) continue;
            if (through(box, (segment){a, b})) found(c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_THROUGH_BOX, r.subject, box->state});
        }
    }
}

ta_status ta_diagram_check(const ta_diagram_check_request *request, ta_diagram_problems *out) {
    const ta_diagram_layout *layout = request->layout;
    const ta_diagram_chart *chart = request->chart;
    out->list = 0;
    out->count = 0;
    checker c = {chart, layout, 0, 0, 0, 0};
    uint32_t boxes = layout->box_count, edges = layout->edge_count;
    /* at most: per edge, two ends, an angle and its crossings; per box, its parent and siblings;
     * per label, the other labels and the boxes */
    c.capacity = edges * (4 + boxes) + layout->initial_count * 2 + boxes * (boxes + 1) + edges * (2 * edges + boxes) + 1;
    c.list = (ta_diagram_problem *)ta_growable_arena_alloc(request->arena, sizeof(ta_diagram_problem) * c.capacity, 8);
    c.box_of = (const ta_diagram_box **)ta_growable_arena_alloc(request->arena, sizeof(ta_diagram_box *) * (chart->state_count + 1u), 8);
    if (!c.list || !c.box_of) return TA_ERROR_NO_MEMORY;
    for (uint32_t s = 0; s < chart->state_count; s++)
        c.box_of[s] = 0;
    for (uint32_t i = 0; i < boxes; i++)
        if (layout->boxes[i].state < chart->state_count) c.box_of[layout->boxes[i].state] = &layout->boxes[i];
    for (uint32_t s = 0; s < chart->state_count; s++)
        if (!c.box_of[s]) return TA_ERROR_INVALID; /* a layout of another chart */
    const uint32_t *counts = side_counts(&c, request->arena);
    if (!counts) return TA_ERROR_NO_MEMORY;

    /* edges: their ends on their boxes' outlines, the arrow into its side, the route */
    for (uint32_t i = 0; i < edges; i++) {
        const ta_diagram_edge *edge = &layout->edges[i];
        const ta_diagram_point *p = &layout->points[edge->first_point];
        const ta_diagram_box *from = c.box_of[chart->transitions[edge->transition].source];
        const ta_diagram_box *to = c.box_of[edge->target];
        if (edge->point_count < 2) {
            found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_END_DETACHED, i, edge->target});
            continue;
        }
        uint32_t last = edge->point_count - 1;
        check_end(&c, (end_check){{from, p[0], p[1]}, i, TA_DIAGRAM_PROBLEM_START_DETACHED, 0, counts});
        check_end(&c, (end_check){{to, p[last], p[last - 1]}, i, TA_DIAGRAM_PROBLEM_END_DETACHED, 1, counts});
        check_route(&c, (route_check){p, edge->point_count, i, from, to});
    }
    /* initial markers: their arrows on their targets */
    for (uint32_t i = 0; i < layout->initial_count; i++) {
        const ta_diagram_initial *marker = &layout->initials[i];
        const ta_diagram_point *p = &layout->points[marker->first_point];
        uint32_t last = marker->point_count - 1;
        if (marker->point_count < 2) continue;
        if (from_outline(c.box_of[marker->target], p[last]) > touching)
            found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_INITIAL_DETACHED, marker->target, marker->container});
    }
    /* boxes: inside their parent, clear of their siblings */
    for (uint32_t i = 0; i < boxes; i++) {
        const ta_diagram_box *box = &layout->boxes[i];
        uint32_t parent = chart->states[box->state].parent;
        if (parent == TA_NIL) continue;
        if (!within(box->rect, c.box_of[parent]->rect))
            found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT, box->state, parent});
        for (uint32_t j = i + 1; j < boxes; j++) {
            const ta_diagram_box *other = &layout->boxes[j];
            if (chart->states[other->state].parent == parent && overlap(box->rect, other->rect))
                found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_BOXES_OVERLAP, box->state, other->state});
        }
    }
    /* labels: clear of each other and of every leaf */
    for (uint32_t i = 0; i < edges; i++) {
        const ta_diagram_edge *edge = &layout->edges[i];
        if (!edge->labelled) continue;
        for (uint32_t j = i + 1; j < edges; j++)
            if (layout->edges[j].labelled && overlap(edge->label, layout->edges[j].label))
                found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_LABEL_OVERLAP, i, j});
        for (uint32_t k = 0; k < boxes; k++)
            if (is_leaf(&layout->boxes[k]) && overlap(edge->label, layout->boxes[k].rect))
                found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_LABEL_ON_BOX, i, layout->boxes[k].state});
        for (uint32_t j = 0; j < edges; j++) {
            const ta_diagram_edge *other = &layout->edges[j];
            if (j == i || other->transition == edge->transition) continue;
            const ta_diagram_point *p = &layout->points[other->first_point];
            for (uint32_t k = 1; k < other->point_count; k++)
                if (crosses(edge->label, p[k - 1], p[k])) {
                    found(&c, (ta_diagram_problem){TA_DIAGRAM_PROBLEM_LABEL_ON_EDGE, i, j});
                    break;
                }
        }
    }
    out->list = c.list;
    out->count = c.count;
    return TA_OK;
}

static const char *const problem_names[] = {"startDetached", "endDetached",  "notOrthogonal",
                                            "arrowAlongSide", "notCentred",   "throughBox",
                                            "outsideParent", "boxesOverlap", "labelOverlap", "labelOnBox", "initialDetached",
                                            "labelOnEdge"};

ta_string_view ta_diagram_problem_name(uint32_t kind) {
    if (kind >= sizeof problem_names / sizeof problem_names[0]) return ta_string("unknown");
    return ta_string(problem_names[kind]);
}
