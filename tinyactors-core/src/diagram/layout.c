/* diagram/layout.c — a statechart as nested boxes with orthogonal routes
 * (diagram.h): scxmljs's layout.ts, ported to C. The steps keep its
 * comments and its order, so the two read side by side; what differs:
 * every container is expanded (nothing is folded), history states of a
 * parallel state are laid out with its regions, and maps and sets are
 * arrays indexed by state. Freestanding: no recursion, no C library. */
#include "diagram/diagram.h"

#include "base/memory.h"
#include "diagram/geometry.h"

/* ---- the defaults (layout.ts) ---------------------------------------------------------- */

static const ta_diagram_spacing default_spacing = {18, 44, 14, 10, 6};
static const double initial_dot = 10; /* the initial marker's diameter */
static const double self_out = 1.6;   /* how far a self loop reaches past its box, in edge spacings */
static const double label_row = 320;  /* down: how wide a box may grow to put its outgoing labels side by side */
static const double big = 1e300;      /* beyond every coordinate */
enum { PORTAL_BORDERS = 3 };          /* a transition crossing this many container borders is listed, not routed */

/* ---- small things ------------------------------------------------------------------------ */

static uint32_t ceil_sqrt(uint32_t n) {
    uint32_t r = 0;
    while (r * r < n)
        r++;
    return r;
}

typedef ta_diagram_point point;
typedef ta_diagram_rect rect;
typedef ta_diagram_size size;

/* canonical <-> real: canonical x runs along the layers */
static point tp(point p, int down) {
    return down ? (point){p.y, p.x} : p;
}
static rect tr(rect r, int down) {
    return down ? (rect){r.y, r.x, r.h, r.w} : r;
}

/* ---- the context ------------------------------------------------------------------------ */

enum { SIDE_NEAR = 0, SIDE_FAR = 1, SIDE_BOTTOM = 2, SIDES = 3 };

enum {
    ROUTE_FORWARD = 0,
    ROUTE_REVERSED = 1,
    ROUTE_SAME = 2,
    ROUTE_REGIONS = 3,
    ROUTE_SELF = 4,
    ROUTE_TO_ANCESTOR = 5,
    ROUTE_TO_DESCENDANT = 6
};

enum { CONTAINER_NONE = 0, CONTAINER_ROOT = 1, CONTAINER_COMPOUND = 2, CONTAINER_PARALLEL = 3 };

/* One node of a container's layered arrangement: a child state or a dummy. */
typedef struct lnode {
    uint32_t state; /* TA_NIL for a dummy */
    double w, h;    /* canonical size */
    uint32_t layer, order;
    double x, y; /* canonical top-left, relative to the content origin */
} lnode;

typedef struct ledge {
    uint32_t from, to; /* nodes of the container */
    double weight;
} ledge;

/* A long edge's dummies: nodes first..first+count-1, layer order. */
typedef struct chain {
    uint32_t from, to; /* child states, the earlier layer's first */
    uint32_t first, count;
} chain;

typedef struct container {
    uint32_t kind;
    size whole; /* real size of the box */
    double header;
    point content; /* real offset of the content area inside the box */
    lnode *nodes;  /* the kids, in kids order, then the dummies */
    uint32_t node_count;
    uint32_t *layers; /* node indexes, layer by layer */
    uint32_t *layer_first, *layer_count;
    uint32_t layer_total;
    double *layer_x, *layer_w;
    double canon_w, canon_h;
    chain *chains;
    uint32_t chain_count;
    uint32_t lanes;
    double *gap_label;
    uint32_t *gap_channels;
    uint32_t right_margin;
} container;

typedef struct attachment {
    uint32_t box, side;
    double key, at, room, slot;
    uint8_t loop;
    uint8_t arrives; /* 0 none, 1 a number, 2 another attachment */
    double arrive_at;
    uint32_t arrive_from;
} attachment;

typedef struct channel {
    uint32_t container, gap; /* gap: TA_NIL for the right margin */
    uint32_t index;
} channel;

typedef struct pending {
    uint32_t transition, target, from, to, route, container;
    uint32_t start, end; /* attachments or TA_NIL */
    uint32_t lane;
    uint32_t first_channel, channel_count;
} pending;

typedef struct points {
    point *data;
    uint32_t length, capacity;
} points;

typedef struct layout {
    const ta_diagram_request *request;
    const ta_diagram_chart *chart;
    ta_growable_arena *arena;
    ta_diagram_spacing sp;
    int down;
    int failed;
    uint32_t n; /* states */
    uint32_t *end;        /* one past each state's last descendant */
    uint32_t *depth;
    uint32_t *kids, *kids_first, *kids_count; /* children, then history states */
    uint32_t *slot;       /* a state's place among its parent's kids */
    uint32_t *walk;       /* every state, parents first, kids in kids order */
    uint32_t *transitions; /* routed or lifted: every transition but the portals, in walk order */
    uint32_t transition_count;
    container *containers; /* per state */
    rect *local;           /* real position in the parent's content, and size */
    rect *abs;             /* real, absolute */
    uint8_t *placed;
    uint32_t *post;        /* containers, children before parents (the order they are laid out in) */
    uint32_t post_count;
} layout;

/* `count` zeroed items of `size` bytes (room for one more, so none is empty); NULL, and the
 * layout failed, when the arena cannot grow. */
static void *take(layout *l, size_t size, size_t count) {
    if (l->failed) return 0;
    size_t bytes = size * (count + 1);
    void *memory = ta_growable_arena_alloc(l->arena, bytes, 8);
    if (!memory) {
        l->failed = 1;
        return 0;
    }
    ta_memset(memory, 0, bytes);
    return memory;
}

static void points_push(layout *l, points *list, point p) {
    if (list->length == list->capacity) {
        uint32_t capacity = list->capacity ? list->capacity * 2 : 16;
        point *data = take(l, sizeof(point), capacity);
        if (!data) return;
        if (list->length) ta_memcpy(data, list->data, sizeof(point) * list->length);
        list->data = data;
        list->capacity = capacity;
    }
    list->data[list->length++] = p;
}

/* ---- the chart ------------------------------------------------------------------------ */

static const ta_diagram_state *state_of(const layout *l, uint32_t s) {
    return &l->chart->states[s];
}
static int is_history(const layout *l, uint32_t s) {
    uint32_t kind = state_of(l, s)->kind;
    return kind == TA_DIAGRAM_STATE_HISTORY_SHALLOW || kind == TA_DIAGRAM_STATE_HISTORY_DEEP;
}
static uint32_t parent_of(const layout *l, uint32_t s) {
    return state_of(l, s)->parent;
}
/* <state> with children other than history */
static int is_compound(const layout *l, uint32_t s) {
    if (state_of(l, s)->kind != TA_DIAGRAM_STATE_STATE) return 0;
    for (uint32_t i = 0; i < l->kids_count[s]; i++)
        if (!is_history(l, l->kids[l->kids_first[s] + i])) return 1;
    return 0;
}
static int is_container(const layout *l, uint32_t s) {
    uint32_t kind = state_of(l, s)->kind;
    return kind == TA_DIAGRAM_STATE_ROOT || kind == TA_DIAGRAM_STATE_PARALLEL || is_compound(l, s);
}
static int within(const layout *l, uint32_t s, uint32_t ancestor) {
    return s >= ancestor && s < l->end[ancestor];
}
/* The child of `c` on the way down to `s` (s itself when s is a child), or TA_NIL. */
static uint32_t child_of(const layout *l, uint32_t c, uint32_t s) {
    if (s == TA_NIL || !within(l, s, c) || s == c) return TA_NIL;
    while (parent_of(l, s) != c)
        s = parent_of(l, s);
    return s;
}
static const ta_diagram_transition *transition_of(const layout *l, uint32_t t) {
    return &l->chart->transitions[t];
}
static uint32_t target_of(const layout *l, const ta_diagram_transition *t, uint32_t i) {
    return l->chart->targets[t->first_target + i];
}
static size label_size(const layout *l, uint32_t t) {
    return l->request->label_sizes ? l->request->label_sizes[t] : (size){0, 0};
}
static double label_along(const layout *l, uint32_t t) {
    size s = label_size(l, t);
    return (l->down ? s.h : s.w) + 2 * l->sp.label;
}
static size label_canon(const layout *l, uint32_t t) {
    size s = label_size(l, t);
    return l->down ? (size){s.h, s.w} : s;
}
/* drawn as a loop on its own box */
static int self_loop(const ta_diagram_transition *t, uint32_t target) {
    return t->source == target;
}

/* Checks the chart is a tree in document order, then derives what the
 * layout walks: subtree ends, depths, kids lists, the walk. */
static ta_status prepare(layout *l) {
    const ta_diagram_chart *chart = l->chart;
    uint32_t n = chart->state_count;
    if (!n || chart->states[0].parent != TA_NIL) return TA_ERROR_INVALID;
    for (uint32_t s = 1; s < n; s++)
        if (chart->states[s].parent >= s) return TA_ERROR_INVALID;
    for (uint32_t t = 0; t < chart->transition_count; t++) {
        const ta_diagram_transition *transition = &chart->transitions[t];
        if (transition->source >= n) return TA_ERROR_INVALID;
        for (uint32_t i = 0; i < transition->target_count; i++)
            if (chart->targets[transition->first_target + i] >= n) return TA_ERROR_INVALID;
    }
    for (uint32_t s = 0; s < n; s++)
        for (uint32_t i = 0; i < chart->states[s].initial_count; i++)
            if (chart->targets[chart->states[s].first_initial + i] >= n) return TA_ERROR_INVALID;
    l->n = n;
    l->end = take(l, sizeof(uint32_t), n);
    l->depth = take(l, sizeof(uint32_t), n);
    l->kids = take(l, sizeof(uint32_t), n);
    l->kids_first = take(l, sizeof(uint32_t), n);
    l->kids_count = take(l, sizeof(uint32_t), n);
    l->slot = take(l, sizeof(uint32_t), n);
    l->walk = take(l, sizeof(uint32_t), n);
    if (l->failed) return TA_ERROR_NO_MEMORY;
    for (uint32_t s = n; s-- > 0;) {
        if (l->end[s] < s + 1) l->end[s] = s + 1;
        uint32_t parent = chart->states[s].parent;
        if (parent != TA_NIL && l->end[parent] < l->end[s]) l->end[parent] = l->end[s];
    }
    for (uint32_t s = 1; s < n; s++) {
        uint32_t parent = chart->states[s].parent;
        if (l->end[s] > l->end[parent]) return TA_ERROR_INVALID; /* not in document order */
        l->depth[s] = l->depth[parent] + 1;
        l->kids_count[parent]++;
    }
    /* kids: children in document order, then history states */
    uint32_t next = 0;
    for (uint32_t s = 0; s < n; s++) {
        l->kids_first[s] = next;
        next += l->kids_count[s];
        l->kids_count[s] = 0;
    }
    for (int history = 0; history < 2; history++)
        for (uint32_t s = 1; s < n; s++) {
            if (is_history(l, s) != history) continue;
            uint32_t parent = chart->states[s].parent;
            l->slot[s] = l->kids_count[parent];
            l->kids[l->kids_first[parent] + l->kids_count[parent]++] = s;
        }
    /* the walk: a state, then each kid's subtree in kids order */
    uint32_t *stack = take(l, sizeof(uint32_t), n);
    if (!stack) return TA_ERROR_NO_MEMORY;
    uint32_t top = 0, count = 0;
    stack[top++] = 0;
    while (top) {
        uint32_t s = stack[--top];
        l->walk[count++] = s;
        for (uint32_t i = l->kids_count[s]; i-- > 0;)
            stack[top++] = l->kids[l->kids_first[s] + i];
    }
    return TA_OK;
}

/* ---- portals ---------------------------------------------------------------------------- */

/* How many container borders a transition crosses from `from` to `to`. */
static uint32_t borders(const ta_diagram_chart *chart, const uint32_t *depth, uint32_t from, uint32_t to) {
    uint32_t lca = chart->states[from].parent;
    while (lca != TA_NIL) {
        uint32_t s = to;
        while (s != TA_NIL && s != lca)
            s = chart->states[s].parent;
        if (s == lca) break;
        lca = chart->states[lca].parent;
    }
    if (lca == TA_NIL) return 0;
    return depth[from] - depth[lca] - 1 + (depth[to] - depth[lca] - 1);
}

static int is_ancestor(const ta_diagram_chart *chart, uint32_t ancestor, uint32_t s) {
    for (; s != TA_NIL; s = chart->states[s].parent)
        if (s == ancestor) return 1;
    return 0;
}

uint8_t *ta_diagram_portals(const ta_diagram_chart *chart, ta_growable_arena *arena) {
    uint8_t *out = (uint8_t *)ta_growable_arena_alloc(arena, chart->transition_count + 1u, 8);
    uint32_t *depth = (uint32_t *)ta_growable_arena_alloc(arena, sizeof(uint32_t) * (chart->state_count + 1u), 8);
    if (!out || !depth) return 0;
    for (uint32_t s = 0; s < chart->state_count; s++) {
        uint32_t parent = chart->states[s].parent;
        depth[s] = parent < s ? depth[parent] + 1 : 0;
    }
    for (uint32_t t = 0; t < chart->transition_count; t++) {
        const ta_diagram_transition *transition = &chart->transitions[t];
        int far = transition->target_count > 0;
        for (uint32_t i = 0; i < transition->target_count && far; i++) {
            uint32_t from = transition->source, to = chart->targets[transition->first_target + i];
            if (to == from)
                far = 0;
            else if (is_ancestor(chart, from, to))
                far = 1; /* from anywhere inside `from` */
            else if (is_ancestor(chart, to, from))
                far = 0; /* out to an ancestor: a short hop through its border */
            else
                far = borders(chart, depth, from, to) >= PORTAL_BORDERS;
        }
        out[t] = (uint8_t)far;
    }
    return out;
}

/* ---- 2. breaking cycles: a greedy feedback arc set --------------------------------------- */

/* Each node's place in an order that keeps most edge weight pointing
 * forward (Eades, Lin & Smyth 1993, weighted). Ties go to `preferred`
 * nodes, then to input order. */
typedef struct arcs {
    const ledge *edges;
    uint32_t edge_count;
    const uint8_t *preferred;
    const uint8_t *remaining;
} arcs;

static double in_weight(const arcs *a, uint32_t n) {
    double sum = 0;
    for (uint32_t i = 0; i < a->edge_count; i++)
        if (a->edges[i].to == n && a->edges[i].from != n && a->remaining[a->edges[i].from]) sum += a->edges[i].weight;
    return sum;
}
static double out_weight(const arcs *a, uint32_t n) {
    double sum = 0;
    for (uint32_t i = 0; i < a->edge_count; i++)
        if (a->edges[i].from == n && a->edges[i].to != n && a->remaining[a->edges[i].to]) sum += a->edges[i].weight;
    return sum;
}

static void feedback_arc_order(layout *l, uint32_t count, const arcs *given, uint32_t *rank) {
    uint8_t *remaining = take(l, sizeof(uint8_t), count);
    uint32_t *left = take(l, sizeof(uint32_t), count), *right = take(l, sizeof(uint32_t), count);
    if (l->failed) return;
    arcs a = *given;
    a.remaining = remaining;
    for (uint32_t i = 0; i < count; i++)
        remaining[i] = 1;
    uint32_t left_count = 0, right_count = 0, remaining_count = count; /* right fills from its end */
    while (remaining_count) {
        int changed = 1;
        while (changed) {
            changed = 0;
            /* sinks go to the end, the last one first; never the preferred start while it has alternatives */
            uint32_t sink = TA_NIL;
            for (uint32_t i = 0; i < count; i++)
                if (remaining[i] && out_weight(&a, i) == 0 && !(a.preferred[i] && remaining_count > 1)) sink = i;
            if (sink != TA_NIL) {
                remaining[sink] = 0;
                remaining_count--;
                right[count - 1 - right_count++] = sink;
                changed = 1;
                continue;
            }
            uint32_t source = TA_NIL;
            for (uint32_t i = 0; i < count; i++) {
                if (!remaining[i] || in_weight(&a, i) != 0) continue;
                if (source == TA_NIL) source = i;
                if (a.preferred[i]) {
                    source = i;
                    break;
                }
            }
            if (source != TA_NIL) {
                remaining[source] = 0;
                remaining_count--;
                left[left_count++] = source;
                changed = 1;
            }
        }
        if (!remaining_count) break;
        uint32_t best = TA_NIL;
        double best_delta = -big;
        for (uint32_t i = 0; i < count; i++) {
            if (!remaining[i]) continue;
            double delta = out_weight(&a, i) - in_weight(&a, i) + (a.preferred[i] ? 0.5 : 0);
            if (delta > best_delta) {
                best_delta = delta;
                best = i;
            }
        }
        remaining[best] = 0;
        remaining_count--;
        left[left_count++] = best;
    }
    for (uint32_t i = 0; i < left_count; i++)
        rank[left[i]] = i;
    for (uint32_t i = 0; i < right_count; i++)
        rank[right[count - right_count + i]] = left_count + i;
}

/* ---- parallel regions -------------------------------------------------------------------- */

/* Parallel regions side by side in a grid (real orientation). */
static size arrange_regions(layout *l, uint32_t c, const size *sizes) {
    container *info = &l->containers[c];
    uint32_t first = l->kids_first[c], n = l->kids_count[c];
    const ta_diagram_spacing *sp = &l->sp;
    /* regions stack across the layer direction; many wrap into a grid */
    uint32_t stack = n <= 4 ? n : ceil_sqrt(n);
    uint32_t columns = l->down ? stack : (n + stack - 1) / stack;
    if (!columns) columns = 1;
    double *column_w = take(l, sizeof(double), n + 1), *row_h = take(l, sizeof(double), n + 1);
    if (l->failed) return (size){0, 0};
    for (;;) { /* down, in a narrow space: fewer columns (the regions then stack, as rows) */
        for (uint32_t i = 0; i < columns; i++)
            column_w[i] = 0;
        for (uint32_t i = 0; i < n; i++)
            column_w[i % columns] = ta_diagram_max(column_w[i % columns], sizes[l->kids[first + i]].w);
        double grid = sp->node * (columns - 1);
        for (uint32_t i = 0; i < columns; i++)
            grid += column_w[i];
        if (!(l->down && l->request->max_width > 0 && columns > 1 && grid + 2 * sp->padding > l->request->max_width))
            break;
        columns--;
    }
    uint32_t rows = (n + columns - 1) / columns;
    for (uint32_t i = 0; i < rows; i++)
        row_h[i] = 0;
    for (uint32_t i = 0; i < n; i++)
        row_h[i / columns] = ta_diagram_max(row_h[i / columns], sizes[l->kids[first + i]].h);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = l->kids[first + i], ci = i % columns, ri = i / columns;
        double x = 0, y = 0;
        for (uint32_t j = 0; j < ci; j++)
            x += column_w[j] + sp->node;
        for (uint32_t j = 0; j < ri; j++)
            y += row_h[j] + sp->node;
        /* regions fill their grid cell, so the dashed separators line up */
        l->local[k] = (rect){x, y, ta_diagram_max(sizes[k].w, column_w[ci]), ta_diagram_max(sizes[k].h, row_h[ri])};
    }
    /* transitions between regions run along lanes below the grid */
    uint32_t lanes = 0;
    for (uint32_t i = 0; i < l->transition_count; i++) {
        const ta_diagram_transition *t = transition_of(l, l->transitions[i]);
        if (!within(l, t->source, c)) continue;
        uint32_t a = child_of(l, c, t->source);
        for (uint32_t j = 0; j < t->target_count; j++) {
            uint32_t b = child_of(l, c, target_of(l, t, j));
            if (a != TA_NIL && b != TA_NIL && a != b) lanes++;
        }
    }
    info->lanes = lanes;
    double w = sp->node * (columns - 1), h = sp->node * (rows - 1) + (lanes ? (lanes + 1) * sp->edge : 0);
    for (uint32_t i = 0; i < columns; i++)
        w += column_w[i];
    for (uint32_t i = 0; i < rows; i++)
        h += row_h[i];
    return (size){w, h};
}

/* ---- layers ------------------------------------------------------------------------------- */

static lnode *node_of(container *info, uint32_t index) {
    return &info->nodes[index];
}

/* the lifted edges of a container: one per pair of children, weighted */
static void add_edge(ledge *edges, uint32_t *count, uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < *count; i++)
        if (edges[i].from == a && edges[i].to == b) {
            edges[i].weight++;
            return;
        }
    edges[(*count)++] = (ledge){a, b, 1};
}

static chain *chain_between(container *info, uint32_t from, uint32_t to) {
    for (uint32_t i = 0; i < info->chain_count; i++)
        if (info->chains[i].from == from && info->chains[i].to == to) return &info->chains[i];
    return 0;
}

/* The layer lists, rebuilt from each node's layer and order. */
typedef struct layering {
    uint32_t *list; /* node indexes, layer by layer */
    uint32_t *first, *count;
    uint32_t total;
} layering;

static uint32_t *layer_nodes(container *info, uint32_t layer) {
    return &info->layers[info->layer_first[layer]];
}

static void renumber(container *info) {
    for (uint32_t layer = 0; layer < info->layer_total; layer++)
        for (uint32_t i = 0; i < info->layer_count[layer]; i++)
            node_of(info, layer_nodes(info, layer)[i])->order = i;
}

/* A stable insertion sort of a layer by a key per node, ties by order. */
static void sort_layer(container *info, uint32_t layer, const double *key) {
    uint32_t *list = layer_nodes(info, layer);
    uint32_t count = info->layer_count[layer];
    for (uint32_t i = 1; i < count; i++) {
        uint32_t moving = list[i], j = i;
        for (; j > 0; j--) {
            uint32_t before = list[j - 1];
            double d = key[before] - key[moving];
            if (d < 0 || (d == 0 && node_of(info, before)->order <= node_of(info, moving)->order)) break;
            list[j] = before;
        }
        list[j] = moving;
    }
}

typedef struct segments {
    ledge *list;
    uint32_t count;
    uint32_t *up, *up_first, *up_count; /* per node: the segments into it */
    uint32_t *down, *down_first, *down_count;
} segments;

static double count_crossings(container *info, const segments *s) {
    double n = 0;
    for (uint32_t i = 0; i < s->count; i++)
        for (uint32_t j = i + 1; j < s->count; j++) {
            const ledge *a = &s->list[i], *b = &s->list[j];
            if (node_of(info, a->from)->layer != node_of(info, b->from)->layer) continue;
            double d = ((double)node_of(info, a->from)->order - node_of(info, b->from)->order) *
                       ((double)node_of(info, a->to)->order - node_of(info, b->to)->order);
            if (d < 0) n += a->weight * b->weight;
        }
    return n;
}

/* Move nodes to their desired spots where possible, keeping their order
 * and never overlapping. */
static void settle(layout *l, container *info, uint32_t layer, const double *desired, const uint8_t *has) {
    double previous_end = -big;
    for (uint32_t i = 0; i < info->layer_count[layer]; i++) {
        uint32_t index = layer_nodes(info, layer)[i];
        lnode *node = node_of(info, index);
        node->y = ta_diagram_max(has[index] ? desired[index] : node->y, previous_end);
        previous_end = node->y + node->h + (node->state == TA_NIL ? l->sp.edge : l->sp.node);
    }
}

static double center(const lnode *node) {
    return node->y + node->h / 2;
}

/* Where a node's ports are centred, from its top: a container's far and near sides keep their
 * ports clear of its header band when layers run right (ports, below), so its edges leave
 * from the middle of what is below the header; a leaf's and a dummy's from their middle. A
 * node aligned on its neighbours' ports, not their boxes, gets its edges straight. */
static double port_offset(const layout *l, const lnode *node) {
    double header = node->state != TA_NIL && !l->down ? l->containers[node->state].header : 0;
    return header + (node->h - header) / 2;
}
static double port_center(const layout *l, const lnode *node) {
    return node->y + port_offset(l, node);
}

static size arrange_layers(layout *l, uint32_t c, const size *sizes) {
    container *info = &l->containers[c];
    const ta_diagram_spacing *sp = &l->sp;
    int down = l->down;
    uint32_t first = l->kids_first[c], kid_count = l->kids_count[c];

    /* 1. lift transitions to edges between children */
    uint32_t possible = 0;
    for (uint32_t i = 0; i < l->transition_count; i++)
        possible += transition_of(l, l->transitions[i])->target_count;
    ledge *edges = take(l, sizeof(ledge), possible + 1);
    if (!edges) return (size){0, 0};
    uint32_t edge_count = 0;
    for (uint32_t i = 0; i < l->transition_count; i++) {
        const ta_diagram_transition *t = transition_of(l, l->transitions[i]);
        if (!within(l, t->source, c) || t->source == c) continue;
        uint32_t a = child_of(l, c, t->source);
        for (uint32_t j = 0; j < t->target_count; j++) {
            uint32_t target = target_of(l, t, j);
            if (!within(l, target, c) || target == c) continue;
            uint32_t b = child_of(l, c, target);
            if (a != TA_NIL && b != TA_NIL && a != b) add_edge(edges, &edge_count, l->slot[a], l->slot[b]);
        }
    }

    /* the nodes: the kids, then room for the dummies of every long edge */
    uint32_t dummy_room = 0;
    for (uint32_t i = 0; i < edge_count; i++)
        dummy_room += kid_count;
    info->nodes = take(l, sizeof(lnode), kid_count + dummy_room + 1);
    uint8_t *preferred = take(l, sizeof(uint8_t), kid_count + 1);
    uint32_t *rank = take(l, sizeof(uint32_t), kid_count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < kid_count; i++) {
        uint32_t k = l->kids[first + i];
        lnode *node = node_of(info, i);
        node->state = k;
        node->w = down ? sizes[k].h : sizes[k].w;
        node->h = down ? sizes[k].w : sizes[k].h;
    }
    info->node_count = kid_count;
    const ta_diagram_state *cs = state_of(l, c);
    for (uint32_t i = 0; i < cs->initial_count; i++) {
        uint32_t t = child_of(l, c, l->chart->targets[cs->first_initial + i]);
        if (t != TA_NIL) preferred[l->slot[t]] = 1;
    }

    /* 2. break cycles: a greedy feedback arc set on edge weights */
    arcs a = {edges, edge_count, preferred, 0};
    feedback_arc_order(l, kid_count, &a, rank);
    if (l->failed) return (size){0, 0};
    uint8_t *reversed = take(l, sizeof(uint8_t), edge_count + 1);
    uint32_t *indegree = take(l, sizeof(uint32_t), kid_count + 1), *queue = take(l, sizeof(uint32_t), kid_count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < edge_count; i++)
        reversed[i] = rank[edges[i].from] > rank[edges[i].to];

    /* 3. layers: longest path */
    for (uint32_t i = 0; i < edge_count; i++)
        if (!reversed[i]) indegree[edges[i].to]++;
    uint32_t queued = 0;
    for (uint32_t i = 0; i < kid_count; i++)
        if (!indegree[i]) queue[queued++] = i;
    for (uint32_t q = 0; q < queued; q++) {
        uint32_t n = queue[q];
        for (uint32_t i = 0; i < edge_count; i++) {
            if (reversed[i] || edges[i].from != n) continue;
            lnode *to = node_of(info, edges[i].to);
            if (to->layer < node_of(info, n)->layer + 1) to->layer = node_of(info, n)->layer + 1;
            if (--indegree[edges[i].to] == 0) queue[queued++] = edges[i].to;
        }
    }
    uint32_t layer_count = 1;
    for (uint32_t i = 0; i < kid_count; i++)
        if (node_of(info, i)->layer + 1 > layer_count) layer_count = node_of(info, i)->layer + 1;

    /* long edges -> dummy chains (one per lifted pair, in layer order; reversed edges run
     * backwards through the same kind of corridor), same-layer edges need none */
    segments s;
    ta_memset(&s, 0, sizeof s);
    s.list = take(l, sizeof(ledge), edge_count + dummy_room + 1);
    info->chains = take(l, sizeof(chain), edge_count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < edge_count; i++) {
        ledge e = edges[i];
        uint32_t from_layer = node_of(info, e.from)->layer, to_layer = node_of(info, e.to)->layer;
        if (from_layer == to_layer) continue;
        if (from_layer > to_layer) {
            e = (ledge){e.to, e.from, e.weight};
            uint32_t swap = from_layer;
            from_layer = to_layer;
            to_layer = swap;
        }
        if (to_layer - from_layer <= 1) {
            s.list[s.count++] = e;
            continue;
        }
        uint32_t from_state = node_of(info, e.from)->state, to_state = node_of(info, e.to)->state;
        chain *existing = chain_between(info, from_state, to_state);
        if (existing) {
            for (uint32_t d = 0; d < existing->count; d++)
                node_of(info, existing->first + d)->h += sp->edge;
            continue;
        }
        chain *made = &info->chains[info->chain_count++];
        *made = (chain){from_state, to_state, info->node_count, 0};
        uint32_t previous = e.from;
        for (uint32_t layer = from_layer + 1; layer < to_layer; layer++) {
            uint32_t d = info->node_count++;
            lnode *dummy = node_of(info, d);
            dummy->state = TA_NIL;
            dummy->w = 0;
            dummy->h = sp->edge * ta_diagram_min(3, e.weight);
            dummy->layer = layer;
            made->count++;
            s.list[s.count++] = (ledge){previous, d, e.weight};
            previous = d;
        }
        s.list[s.count++] = (ledge){previous, e.to, e.weight};
    }

    /* the layer lists: states in kids order, then dummies in the order they were made */
    info->layer_total = layer_count;
    info->layers = take(l, sizeof(uint32_t), info->node_count + 1);
    info->layer_first = take(l, sizeof(uint32_t), layer_count + 1);
    info->layer_count = take(l, sizeof(uint32_t), layer_count + 1);
    uint32_t *best = take(l, sizeof(uint32_t), info->node_count + 1);
    double *key = take(l, sizeof(double), info->node_count + 1);
    uint8_t *has = take(l, sizeof(uint8_t), info->node_count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < info->node_count; i++)
        info->layer_count[node_of(info, i)->layer]++;
    for (uint32_t layer = 0, next = 0; layer < layer_count; layer++) {
        info->layer_first[layer] = next;
        next += info->layer_count[layer];
        info->layer_count[layer] = 0;
    }
    for (uint32_t i = 0; i < info->node_count; i++) {
        uint32_t layer = node_of(info, i)->layer;
        info->layers[info->layer_first[layer] + info->layer_count[layer]++] = i;
    }

    /* 4. crossing reduction: barycentre sweeps, keep the best */
    renumber(info);
    s.up_first = take(l, sizeof(uint32_t), info->node_count + 1);
    s.up_count = take(l, sizeof(uint32_t), info->node_count + 1);
    s.down_first = take(l, sizeof(uint32_t), info->node_count + 1);
    s.down_count = take(l, sizeof(uint32_t), info->node_count + 1);
    s.up = take(l, sizeof(uint32_t), s.count + 1);
    s.down = take(l, sizeof(uint32_t), s.count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < s.count; i++) {
        s.down_count[s.list[i].from]++;
        s.up_count[s.list[i].to]++;
    }
    for (uint32_t i = 0, up = 0, dn = 0; i < info->node_count; i++) {
        s.up_first[i] = up;
        up += s.up_count[i];
        s.down_first[i] = dn;
        dn += s.down_count[i];
        s.up_count[i] = s.down_count[i] = 0;
    }
    for (uint32_t i = 0; i < s.count; i++) {
        uint32_t from = s.list[i].from, to = s.list[i].to;
        s.down[s.down_first[from] + s.down_count[from]++] = i;
        s.up[s.up_first[to] + s.up_count[to]++] = i;
    }
    ta_memcpy(best, info->layers, sizeof(uint32_t) * info->node_count);
    double best_crossings = count_crossings(info, &s);
    for (uint32_t sweep = 0; sweep < 8 && best_crossings > 0; sweep++) {
        int forward = sweep % 2 == 0;
        for (uint32_t step = 0; step + 1 < layer_count; step++) {
            uint32_t layer = forward ? step + 1 : layer_count - 2 - step;
            for (uint32_t i = 0; i < info->layer_count[layer]; i++) {
                uint32_t index = layer_nodes(info, layer)[i];
                uint32_t count = forward ? s.up_count[index] : s.down_count[index];
                const uint32_t *list = forward ? &s.up[s.up_first[index]] : &s.down[s.down_first[index]];
                double sum = 0, w = 0;
                for (uint32_t j = 0; j < count; j++) {
                    const ledge *segment = &s.list[list[j]];
                    sum += (double)node_of(info, forward ? segment->from : segment->to)->order * segment->weight;
                    w += segment->weight;
                }
                key[index] = w != 0 ? sum / w : (double)node_of(info, index)->order;
            }
            sort_layer(info, layer, key);
            for (uint32_t i = 0; i < info->layer_count[layer]; i++)
                node_of(info, layer_nodes(info, layer)[i])->order = i;
        }
        double crossings = count_crossings(info, &s);
        if (crossings < best_crossings) {
            best_crossings = crossings;
            ta_memcpy(best, info->layers, sizeof(uint32_t) * info->node_count);
        }
    }
    ta_memcpy(info->layers, best, sizeof(uint32_t) * info->node_count);
    renumber(info);

    /* boxes sending several labelled edges forward grow so every label gets its own port. Across
     * the layers that costs a label's height when they run right, but its whole width when they
     * run down: then a box grows only up to label_row (or its own width), and the labels that
     * don't fit side by side stack in the gap below it instead (5a) */
    double *outgoing = take(l, sizeof(double), kid_count + 1);
    uint8_t *outgoing_set = take(l, sizeof(uint8_t), kid_count + 1);
    double *label_rows = take(l, sizeof(double), kid_count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < l->transition_count; i++) {
        uint32_t ti = l->transitions[i];
        const ta_diagram_transition *t = transition_of(l, ti);
        if (!within(l, t->source, c) || t->source == c) continue;
        uint32_t from = t->source;
        if (parent_of(l, from) != c) continue; /* only edges leaving the child box itself */
        lnode *na = node_of(info, l->slot[from]);
        size label = label_size(l, ti);
        for (uint32_t j = 0; j < t->target_count; j++) {
            uint32_t target = target_of(l, t, j);
            if (!within(l, target, c) || target == c) continue;
            uint32_t b = child_of(l, c, target);
            /* forward edges and self loops each take a port slot as wide as their label */
            int loop = b == from && self_loop(t, target);
            if (b != TA_NIL && (loop || (b != from && node_of(info, l->slot[b])->layer > na->layer))) {
                outgoing[l->slot[from]] += (down ? label.w : label.h) + sp->label + (loop ? sp->edge : 0);
                outgoing_set[l->slot[from]] = 1;
            }
        }
    }
    for (uint32_t i = 0; i < kid_count; i++) {
        label_rows[i] = 1;
        if (!outgoing_set[i]) continue;
        lnode *node = node_of(info, i);
        double need = outgoing[i];
        double room = down ? ta_diagram_max(node->h, label_row) : big;
        if (down && need + sp->label > room) label_rows[i] = ta_diagram_ceil((need + sp->label) / room);
        double want = ta_diagram_min(need + sp->label, room);
        if (want > node->h) node->h = want;
    }

    /* 5a. gaps between layers: room for labels (at the source side) and bend channels */
    uint32_t gaps = layer_count - 1;
    info->gap_label = take(l, sizeof(double), gaps + 1);
    info->gap_channels = take(l, sizeof(uint32_t), gaps + 1);
    info->layer_x = take(l, sizeof(double), layer_count + 1);
    info->layer_w = take(l, sizeof(double), layer_count + 1);
    if (l->failed) return (size){0, 0};
    for (uint32_t i = 0; i < l->transition_count; i++) {
        uint32_t ti = l->transitions[i];
        const ta_diagram_transition *t = transition_of(l, ti);
        if (!within(l, t->source, c) || t->source == c) continue;
        uint32_t child_a = child_of(l, c, t->source);
        for (uint32_t j = 0; j < t->target_count; j++) {
            uint32_t target = target_of(l, t, j);
            if (!within(l, target, c) || target == c) continue;
            uint32_t child_b = child_of(l, c, target);
            if (child_a == TA_NIL || child_b == TA_NIL) continue;
            lnode *na = node_of(info, l->slot[child_a]), *nb = node_of(info, l->slot[child_b]);
            /* down: labels leaving a box that couldn't put them side by side stack, in rows */
            double stacked = label_along(l, ti) * label_rows[l->slot[child_a]];
            if (child_a == child_b) {
                /* a self loop reaches into the gap after its box, its label straddling the
                 * loop's far side (down: across the loop's far side; right: beside it) */
                if (self_loop(t, target) && na->layer < layer_count - 1)
                    info->gap_label[na->layer] = ta_diagram_max(info->gap_label[na->layer],
                                                      self_out * sp->edge + label_along(l, ti) / (down ? 2 : 1));
                continue;
            }
            if (nb->layer != na->layer) {
                /* forward: label in the gap after the source; reversed: in the gap before it */
                uint32_t lo = na->layer < nb->layer ? na->layer : nb->layer;
                uint32_t hi = na->layer < nb->layer ? nb->layer : na->layer;
                int64_t gap = nb->layer > na->layer ? (int64_t)na->layer : (int64_t)na->layer - 1;
                if (gap >= 0) info->gap_label[gap] = ta_diagram_max(info->gap_label[gap], stacked);
                for (uint32_t g = lo; g < hi; g++)
                    info->gap_channels[g]++;
            } else if (na->layer < layer_count - 1) {
                /* same layer: a short loop through the gap after it (or the right margin) */
                info->gap_channels[na->layer]++;
                info->gap_label[na->layer] = ta_diagram_max(info->gap_label[na->layer], stacked);
            } else
                info->right_margin++;
        }
    }
    /* the initial dot sits just before its target; targets in the first layer need room for it */
    double initial_pad = 0;
    for (uint32_t i = 0; i < kid_count; i++)
        if (preferred[i] && node_of(info, i)->layer == 0) initial_pad = initial_dot + sp->edge * 1.6;
    double x = initial_pad;
    for (uint32_t layer = 0; layer < layer_count; layer++) {
        double w = 0;
        for (uint32_t i = 0; i < info->layer_count[layer]; i++)
            w = ta_diagram_max(w, node_of(info, layer_nodes(info, layer)[i])->w);
        info->layer_x[layer] = x;
        info->layer_w[layer] = w;
        if (layer < layer_count - 1)
            x += w + ta_diagram_max(sp->layer, info->gap_label[layer] + (info->gap_channels[layer] + 1) * sp->edge);
        else
            x += w;
    }
    info->canon_w = x + (info->right_margin ? (info->right_margin + 1) * sp->edge : 0);

    /* 5b. positions across the layers: stack, then pull towards neighbours (order preserved) */
    for (uint32_t layer = 0; layer < layer_count; layer++) {
        double y = 0;
        for (uint32_t i = 0; i < info->layer_count[layer]; i++) {
            lnode *node = node_of(info, layer_nodes(info, layer)[i]);
            node->y = y;
            y += node->h + (node->state == TA_NIL ? sp->edge : sp->node);
        }
    }
    for (uint32_t pass = 0; pass < 4; pass++) {
        int forward = pass % 2 == 0;
        for (uint32_t step = 0; step + 1 < layer_count; step++) {
            uint32_t layer = forward ? step + 1 : layer_count - 2 - step;
            for (uint32_t i = 0; i < info->layer_count[layer]; i++) {
                uint32_t index = layer_nodes(info, layer)[i];
                uint32_t count = forward ? s.up_count[index] : s.down_count[index];
                const uint32_t *list = forward ? &s.up[s.up_first[index]] : &s.down[s.down_first[index]];
                has[index] = count > 0;
                if (!count) continue;
                double sum = 0, w = 0;
                for (uint32_t j = 0; j < count; j++) {
                    const ledge *segment = &s.list[list[j]];
                    sum += port_center(l, node_of(info, forward ? segment->from : segment->to)) * segment->weight;
                    w += segment->weight;
                }
                key[index] = sum / w - port_offset(l, node_of(info, index));
            }
            settle(l, info, layer, key, has);
        }
    }
    /* long-edge corridors: dummies lie on the straight line between their chain's ends */
    for (uint32_t i = 0; i < info->chain_count; i++) {
        const chain *found = &info->chains[i];
        double y0 = port_center(l, node_of(info, l->slot[found->from]));
        double y1 = port_center(l, node_of(info, l->slot[found->to]));
        for (uint32_t d = 0; d < found->count; d++) {
            lnode *dummy = node_of(info, found->first + d);
            double f = (double)(d + 1) / (double)(found->count + 1);
            dummy->y = y0 + (y1 - y0) * f - dummy->h / 2;
        }
    }
    for (uint32_t layer = 0; layer < layer_count; layer++) {
        int dummies = 0;
        for (uint32_t i = 0; i < info->layer_count[layer]; i++)
            dummies |= node_of(info, layer_nodes(info, layer)[i])->state == TA_NIL;
        if (!dummies) continue;
        /* keep the layer's order consistent with the new positions, then resolve overlaps */
        for (uint32_t i = 0; i < info->layer_count[layer]; i++) {
            uint32_t index = layer_nodes(info, layer)[i];
            has[index] = 1;
            key[index] = center(node_of(info, index));
        }
        sort_layer(info, layer, key);
        for (uint32_t i = 0; i < info->layer_count[layer]; i++) {
            uint32_t index = layer_nodes(info, layer)[i];
            node_of(info, index)->order = i;
            key[index] = node_of(info, index)->y;
        }
        settle(l, info, layer, key, has);
    }
    double min_y = big;
    for (uint32_t i = 0; i < info->node_count; i++)
        min_y = ta_diagram_min(min_y, node_of(info, i)->y);
    if (!info->node_count) min_y = 0;
    /* boxes start at the beginning of their layer; dummies keep to the middle, where the corridor runs */
    double canon_h = 0;
    for (uint32_t i = 0; i < info->node_count; i++) {
        lnode *node = node_of(info, i);
        node->y -= min_y;
        node->x = info->layer_x[node->layer] +
                  (node->state == TA_NIL ? (info->layer_w[node->layer] - node->w) / 2 : 0);
        canon_h = ta_diagram_max(canon_h, node->y + node->h);
    }
    info->canon_h = canon_h;
    for (uint32_t i = 0; i < kid_count; i++) {
        lnode *node = node_of(info, i);
        l->local[node->state] = tr((rect){node->x, node->y, node->w, node->h}, down);
    }
    return down ? (size){canon_h, info->canon_w} : (size){info->canon_w, canon_h};
}

/* Lays out every container, children before parents; returns the root's size. */
static void layout_containers(layout *l) {
    const ta_diagram_spacing *sp = &l->sp;
    size *sizes = take(l, sizeof(size), l->n);
    l->post = take(l, sizeof(uint32_t), l->n);
    if (l->failed) return;
    /* containers in post-order (scxmljs keeps them in the order their layout finishes) */
    uint32_t *stack = take(l, sizeof(uint32_t), 2 * l->n + 2);
    if (!stack) return;
    uint32_t top = 0;
    stack[top++] = 0;
    stack[top++] = 0;
    while (top) {
        uint32_t visited = stack[--top], s = stack[--top];
        if (visited) {
            if (is_container(l, s)) l->post[l->post_count++] = s;
            continue;
        }
        stack[top++] = s;
        stack[top++] = 1;
        for (uint32_t i = l->kids_count[s]; i-- > 0;) {
            stack[top++] = l->kids[l->kids_first[s] + i];
            stack[top++] = 0;
        }
    }
    for (uint32_t s = 0; s < l->n; s++)
        if (!is_container(l, s)) sizes[s] = l->request->state_sizes[s];
    for (uint32_t i = 0; i < l->post_count && !l->failed; i++) {
        uint32_t c = l->post[i];
        const ta_diagram_state *state = state_of(l, c);
        size head = state->kind == TA_DIAGRAM_STATE_ROOT ? (size){0, 0} : l->request->state_sizes[c];
        container *info = &l->containers[c];
        info->kind = state->kind == TA_DIAGRAM_STATE_ROOT       ? CONTAINER_ROOT
                     : state->kind == TA_DIAGRAM_STATE_PARALLEL ? CONTAINER_PARALLEL
                                                                : CONTAINER_COMPOUND;
        info->header = state->kind == TA_DIAGRAM_STATE_ROOT ? 0 : head.h;
        info->content = (point){sp->padding, info->header + sp->padding};
        size content = info->kind == CONTAINER_PARALLEL ? arrange_regions(l, c, sizes) : arrange_layers(l, c, sizes);
        double min_w = state->kind == TA_DIAGRAM_STATE_ROOT ? 0 : head.w;
        info->whole = (size){ta_diagram_max(min_w, content.w) + 2 * sp->padding, info->header + content.h + 2 * sp->padding};
        sizes[c] = info->whole;
    }
}

/* ---- absolute rectangles ------------------------------------------------------------------ */

static uint32_t box_kind(const layout *l, uint32_t s) {
    uint32_t kind = state_of(l, s)->kind;
    uint32_t parent = parent_of(l, s);
    if (kind == TA_DIAGRAM_STATE_ROOT) return TA_DIAGRAM_BOX_ROOT;
    if (kind == TA_DIAGRAM_STATE_PARALLEL) return TA_DIAGRAM_BOX_PARALLEL;
    if (kind == TA_DIAGRAM_STATE_FINAL) return TA_DIAGRAM_BOX_FINAL;
    if (kind == TA_DIAGRAM_STATE_HISTORY_SHALLOW || kind == TA_DIAGRAM_STATE_HISTORY_DEEP) return TA_DIAGRAM_BOX_HISTORY;
    if (parent != TA_NIL && state_of(l, parent)->kind == TA_DIAGRAM_STATE_PARALLEL) return TA_DIAGRAM_BOX_REGION;
    return is_compound(l, s) ? TA_DIAGRAM_BOX_COMPOUND : TA_DIAGRAM_BOX_ATOMIC;
}

static void place(layout *l, ta_diagram_box *boxes) {
    l->abs[0] = (rect){0, 0, l->containers[0].whole.w, l->containers[0].whole.h};
    for (uint32_t i = 0; i < l->n; i++) {
        uint32_t c = l->walk[i];
        container *info = &l->containers[c];
        boxes[i] = (ta_diagram_box){c, box_kind(l, c), l->abs[c], info->header, l->depth[c]};
        if (info->kind == CONTAINER_NONE) continue;
        rect r = l->abs[c];
        for (uint32_t j = 0; j < l->kids_count[c]; j++) {
            uint32_t k = l->kids[l->kids_first[c] + j];
            rect local = l->local[k];
            /* leaves keep their measured size; containers theirs; parallel regions fill their cell */
            int whole = info->kind != CONTAINER_PARALLEL && l->containers[k].kind != CONTAINER_NONE;
            double w = whole ? l->containers[k].whole.w : local.w, h = whole ? l->containers[k].whole.h : local.h;
            l->abs[k] = (rect){r.x + info->content.x + local.x, r.y + info->content.y + local.y, w, h};
        }
    }
}

/* ---- routing ------------------------------------------------------------------------------ */

typedef struct router {
    layout *l;
    pending *pendings;
    uint32_t pending_count;
    attachment *attachments;
    uint32_t attachment_count;
    /* attachments per box side, in the order the sides were first used */
    uint32_t *group_of; /* per box * SIDES */
    uint32_t *group_box, *group_side, *group_first, *group_count;
    uint32_t *group_list;
    double *group_start, *group_span;
    uint32_t group_total;
    channel *channels;
    uint32_t channel_count;
    uint32_t *escapes; /* per state */
    points scratch;
} router;

static rect canonical(const layout *l, uint32_t s) {
    return tr(l->abs[s], l->down);
}
/* canonical origin of a container's content area */
static point content_origin(const layout *l, uint32_t c) {
    const container *info = &l->containers[c];
    rect r = l->abs[c];
    return tp((point){r.x + info->content.x, r.y + info->content.y}, l->down);
}
static uint32_t layer_of(const layout *l, uint32_t c, uint32_t child) {
    return l->containers[c].nodes[l->slot[child]].layer;
}
static double center_y(const layout *l, uint32_t s) {
    rect r = canonical(l, s);
    return r.y + r.h / 2;
}
static double center_x(const layout *l, uint32_t s) {
    rect r = canonical(l, s);
    return r.x + r.w / 2;
}

static uint32_t attach(router *r, uint32_t box, uint32_t side, double key, double room) {
    attachment *a = &r->attachments[r->attachment_count];
    ta_memset(a, 0, sizeof *a);
    a->box = box;
    a->side = side;
    a->key = key;
    a->room = room;
    return r->attachment_count++;
}

/* ── 1. classify every (transition, target) pair ── */
static void classify(router *r) {
    layout *l = r->l;
    for (uint32_t i = 0; i < l->transition_count; i++) {
        uint32_t ti = l->transitions[i];
        const ta_diagram_transition *t = transition_of(l, ti);
        uint32_t from = t->source;
        for (uint32_t j = 0; j < t->target_count; j++) {
            uint32_t to = target_of(l, t, j);
            pending p;
            ta_memset(&p, 0, sizeof p);
            p.transition = ti;
            p.target = to;
            p.from = from;
            p.to = to;
            p.start = p.end = p.lane = TA_NIL;
            if (from == to) {
                p.route = ROUTE_SELF;
                p.container = from;
            } else if (within(l, from, to)) {
                p.route = ROUTE_TO_ANCESTOR;
                p.container = to;
            } else if (within(l, to, from)) {
                p.route = ROUTE_TO_DESCENDANT;
                p.container = from;
            } else {
                uint32_t c = parent_of(l, from);
                while (!within(l, to, c))
                    c = parent_of(l, c);
                p.container = c;
                if (l->containers[c].kind == CONTAINER_PARALLEL)
                    p.route = ROUTE_REGIONS;
                else {
                    uint32_t a = layer_of(l, c, child_of(l, c, from)), b = layer_of(l, c, child_of(l, c, to));
                    p.route = b > a ? ROUTE_FORWARD : b < a ? ROUTE_REVERSED : ROUTE_SAME;
                }
            }
            r->pendings[r->pending_count++] = p;
        }
    }
}

/* ── 2. ports: attachments per box side, spread evenly, ordered by where the other end is ── */
static void ports(router *r) {
    layout *l = r->l;
    const ta_diagram_spacing *sp = &l->sp;
    int down = l->down;
    for (uint32_t i = 0; i < r->pending_count; i++) {
        pending *p = &r->pendings[i];
        switch (p->route) {
        case ROUTE_FORWARD:
            /* down, a label sits across its edge just below the port: its width is the room it needs */
            p->start = attach(r, p->from, SIDE_FAR, center_y(l, p->to), down ? label_canon(l, p->transition).h + sp->label : 0);
            p->end = attach(r, p->to, SIDE_NEAR, center_y(l, p->from), 0);
            break;
        case ROUTE_REVERSED:
            p->start = attach(r, p->from, SIDE_NEAR, center_y(l, p->to), 0);
            p->end = attach(r, p->to, SIDE_FAR, center_y(l, p->from), 0);
            break;
        case ROUTE_SAME:
            p->start = attach(r, p->from, SIDE_FAR, center_y(l, p->to), 0);
            p->end = attach(r, p->to, SIDE_FAR, center_y(l, p->from), 0);
            break;
        case ROUTE_REGIONS:
            p->start = attach(r, p->from, SIDE_BOTTOM, center_x(l, p->to), 0);
            p->end = attach(r, p->to, SIDE_BOTTOM, center_x(l, p->from), 0);
            break;
        case ROUTE_TO_ANCESTOR:
            p->start = attach(r, p->from, SIDE_FAR, center_y(l, p->from), 0);
            break;
        case ROUTE_TO_DESCENDANT:
            p->end = attach(r, p->to, SIDE_NEAR, -big, 0);
            break;
        default: /* ROUTE_SELF: one slot on the far side, wide enough for the label to sit on the loop */
            p->start = attach(r, p->from, SIDE_FAR, center_y(l, p->from), label_canon(l, p->transition).h + sp->label + sp->edge);
            r->attachments[p->start].loop = 1;
            break;
        }
    }
    /* where an edge between sibling boxes reaches the box on the later layer: level with the
     * port it leaves from, or with the last dummy of its corridor */
    for (uint32_t i = 0; i < r->pending_count; i++) {
        pending *p = &r->pendings[i];
        if (p->route != ROUTE_FORWARD && p->route != ROUTE_REVERSED) continue;
        int forward = p->route == ROUTE_FORWARD;
        uint32_t early = forward ? p->from : p->to, late = forward ? p->to : p->from, c = p->container;
        if (parent_of(l, early) != c || parent_of(l, late) != c) continue; /* nested ends may detour */
        container *info = &l->containers[c];
        chain *found = chain_between(info, early, late);
        attachment *a = &r->attachments[forward ? p->end : p->start];
        if (found && found->count) {
            lnode *last = &info->nodes[found->first + found->count - 1];
            a->arrives = 1;
            a->arrive_at = content_origin(l, c).y + last->y + last->h / 2;
        } else {
            a->arrives = 2;
            a->arrive_from = forward ? p->start : p->end;
        }
    }
    /* the groups, in the order their sides were first used */
    for (uint32_t i = 0; i < l->n * SIDES; i++)
        r->group_of[i] = TA_NIL;
    for (uint32_t i = 0; i < r->attachment_count; i++) {
        uint32_t key = r->attachments[i].box * SIDES + r->attachments[i].side;
        if (r->group_of[key] == TA_NIL) {
            r->group_of[key] = r->group_total;
            r->group_box[r->group_total] = r->attachments[i].box;
            r->group_side[r->group_total] = r->attachments[i].side;
            r->group_total++;
        }
        r->group_count[r->group_of[key]]++;
    }
    for (uint32_t g = 0, next = 0; g < r->group_total; g++) {
        r->group_first[g] = next;
        next += r->group_count[g];
        r->group_count[g] = 0;
    }
    for (uint32_t i = 0; i < r->attachment_count; i++) {
        uint32_t g = r->group_of[r->attachments[i].box * SIDES + r->attachments[i].side];
        r->group_list[r->group_first[g] + r->group_count[g]++] = i;
    }
    for (uint32_t g = 0; g < r->group_total; g++) {
        uint32_t *list = &r->group_list[r->group_first[g]];
        uint32_t count = r->group_count[g];
        for (uint32_t i = 1; i < count; i++) { /* by key, stably */
            uint32_t moving = list[i], j = i;
            for (; j > 0 && r->attachments[list[j - 1]].key > r->attachments[moving].key; j--)
                list[j] = list[j - 1];
            list[j] = moving;
        }
        uint32_t box = r->group_box[g], side = r->group_side[g];
        rect rr = canonical(l, box);
        container *info = &l->containers[box];
        int has_info = info->kind != CONTAINER_NONE;
        /* on containers, keep ports clear of the header band */
        double across = has_info && !down ? info->header : 0, along = has_info && down ? info->header : 0;
        double span = ta_diagram_max(0, side == SIDE_BOTTOM ? rr.w - along : rr.h - across);
        double start = side == SIDE_BOTTOM ? rr.x + along : rr.y + across;
        double total = 0, loops = 0;
        uint32_t others = 0;
        int any_room = 0;
        for (uint32_t i = 0; i < count; i++) {
            attachment *a = &r->attachments[list[i]];
            total += ta_diagram_max(a->room, sp->edge);
            if (a->loop)
                loops += ta_diagram_max(a->room, sp->edge);
            else
                others++;
            any_room |= a->room > 0;
        }
        /* too short for every label: self loops keep their room (their label sits on them), the
         * other ports share the rest evenly (their labels slide along their edges instead) */
        int shared = total > span && loops > 0 && loops + others * sp->edge <= span;
        if (shared) total = span;
        if (any_room && total <= span) {
            /* ports in slots as wide as their labels, the row of slots centred on the side */
            double x = start + (span - total) / 2;
            for (uint32_t i = 0; i < count; i++) {
                attachment *a = &r->attachments[list[i]];
                double room = shared && !a->loop ? (span - loops) / others : ta_diagram_max(a->room, sp->edge);
                a->at = x + room / 2;
                a->slot = room;
                x += room;
            }
        } else
            for (uint32_t i = 0; i < count; i++) {
                attachment *a = &r->attachments[list[i]];
                a->at = start + span * ((double)(i + 1) / (double)(count + 1));
                a->slot = span / (count + 1);
            }
        r->group_start[g] = start;
        r->group_span[g] = span;
    }
    /* the last mile: a port where its edge arrives moves to meet it, so the edge runs straight in
     * (between its neighbours, and offset a final's rounded ends; a history circle keeps its ports) */
    for (uint32_t g = 0; g < r->group_total; g++) {
        uint32_t box = r->group_box[g];
        if (r->group_side[g] != SIDE_NEAR || is_history(l, box)) continue;
        rect rr = canonical(l, box);
        double start = r->group_start[g], span = r->group_span[g];
        double inset = state_of(l, box)->kind == TA_DIAGRAM_STATE_FINAL ? (down ? rr.w / 2 : rr.h / 4) : sp->edge;
        uint32_t *list = &r->group_list[r->group_first[g]];
        uint32_t count = r->group_count[g];
        for (uint32_t i = 0; i < count; i++) {
            attachment *a = &r->attachments[list[i]];
            if (!a->arrives) continue;
            double want = a->arrives == 1 ? a->arrive_at : r->attachments[a->arrive_from].at;
            double lo = ta_diagram_max(start + inset, i > 0 ? r->attachments[list[i - 1]].at + sp->edge : -big);
            double hi = ta_diagram_min(start + span - inset, i + 1 < count ? r->attachments[list[i + 1]].at - sp->edge : big);
            if (lo <= hi) a->at = ta_diagram_min(hi, ta_diagram_max(lo, want));
            /* still short: the port it leaves from slides within its slot to meet it */
            if (a->arrives == 2 && ta_diagram_abs(a->at - want) > 0.5 && !is_history(l, r->attachments[a->arrive_from].box)) {
                attachment *from = &r->attachments[a->arrive_from];
                double half = ta_diagram_max(0, (from->slot - sp->edge) / 2);
                from->at = ta_diagram_min(from->at + half, ta_diagram_max(from->at - half, a->at));
            }
        }
    }
}

/* ── 3. bend channels per gap (in order of the source port) and lanes per parallel state ── */

typedef struct ends {
    double s0, s1;
} ends;
static ends ends_of(const router *r, const pending *p) {
    double start = r->attachments[p->start].at, end = r->attachments[p->end].at;
    return p->route == ROUTE_REVERSED ? (ends){end, start} : (ends){start, end};
}
/* an edge going down bends further out the higher it starts; an edge going up, the lower it starts */
static double channel_order(const router *r, const pending *p, const pending *q) {
    ends a = ends_of(r, p), b = ends_of(r, q);
    int down_a = a.s1 >= a.s0, down_b = b.s1 >= b.s0;
    if (down_a != down_b) return down_a ? -1 : 1;
    if (down_a) return b.s0 - a.s0 != 0 ? b.s0 - a.s0 : b.s1 - a.s1;
    return a.s0 - b.s0 != 0 ? a.s0 - b.s0 : a.s1 - b.s1;
}

static void channels(router *r) {
    layout *l = r->l;
    /* each pending's channels: one per gap it crosses */
    uint32_t total = 0;
    for (uint32_t i = 0; i < r->pending_count; i++) {
        pending *p = &r->pendings[i];
        p->first_channel = total;
        uint32_t c = p->container;
        if (p->route == ROUTE_FORWARD || p->route == ROUTE_REVERSED) {
            uint32_t a = layer_of(l, c, child_of(l, c, p->from)), b = layer_of(l, c, child_of(l, c, p->to));
            p->channel_count = a > b ? a - b : b - a;
        } else if (p->route == ROUTE_SAME)
            p->channel_count = 1;
        total += p->channel_count;
    }
    r->channels = take(l, sizeof(channel), total + 1);
    pending **list = take(l, sizeof(pending *), r->pending_count + 1);
    channel **entry = take(l, sizeof(channel *), r->pending_count + 1);
    if (l->failed) return;
    for (uint32_t i = 0; i < r->pending_count; i++) {
        pending *p = &r->pendings[i];
        uint32_t c = p->container, k = p->first_channel;
        if (p->route == ROUTE_FORWARD || p->route == ROUTE_REVERSED) {
            uint32_t a = layer_of(l, c, child_of(l, c, p->from)), b = layer_of(l, c, child_of(l, c, p->to));
            for (uint32_t g = a < b ? a : b; g < (a < b ? b : a); g++)
                r->channels[k++] = (channel){c, g, 0};
        } else if (p->route == ROUTE_SAME) {
            uint32_t a = layer_of(l, c, child_of(l, c, p->from));
            r->channels[k++] = (channel){c, a < l->containers[c].layer_total - 1 ? a : TA_NIL, 0};
        }
    }
    r->channel_count = total;
    /* each key's users, in the order their channels were first used, sorted, numbered */
    uint8_t *done = take(l, sizeof(uint8_t), total + 1);
    if (!done) return;
    for (uint32_t e = 0; e < total; e++) {
        if (done[e]) continue;
        channel key = r->channels[e];
        uint32_t count = 0;
        for (uint32_t i = 0; i < r->pending_count; i++) {
            pending *p = &r->pendings[i];
            for (uint32_t k = 0; k < p->channel_count; k++) {
                channel *found = &r->channels[p->first_channel + k];
                if (found->container != key.container || found->gap != key.gap) continue;
                done[p->first_channel + k] = 1;
                list[count] = p;
                entry[count++] = found;
                break;
            }
        }
        for (uint32_t i = 1; i < count; i++) {
            pending *moving = list[i];
            channel *moving_entry = entry[i];
            uint32_t j = i;
            for (; j > 0 && channel_order(r, list[j - 1], moving) > 0; j--) {
                list[j] = list[j - 1];
                entry[j] = entry[j - 1];
            }
            list[j] = moving;
            entry[j] = moving_entry;
        }
        for (uint32_t i = 0; i < count; i++)
            entry[i]->index = i;
    }
    /* lanes: shortest first, then document order */
    for (uint32_t c = 0; c < l->n; c++) {
        if (l->containers[c].kind != CONTAINER_PARALLEL) continue;
        uint32_t count = 0;
        for (uint32_t i = 0; i < r->pending_count; i++)
            if (r->pendings[i].route == ROUTE_REGIONS && r->pendings[i].container == c) list[count++] = &r->pendings[i];
        for (uint32_t i = 1; i < count; i++) {
            pending *moving = list[i];
            double moving_key = ta_diagram_abs(center_x(l, moving->from) - center_x(l, moving->to));
            uint32_t j = i;
            for (; j > 0; j--) {
                double key = ta_diagram_abs(center_x(l, list[j - 1]->from) - center_x(l, list[j - 1]->to));
                if (key < moving_key || (key == moving_key && list[j - 1]->transition <= moving->transition)) break;
                list[j] = list[j - 1];
            }
            list[j] = moving;
        }
        for (uint32_t i = 0; i < count; i++)
            list[i]->lane = i;
    }
}

static uint32_t channel_index(const router *r, const pending *p, uint32_t c, uint32_t gap) {
    for (uint32_t k = 0; k < p->channel_count; k++) {
        const channel *found = &r->channels[p->first_channel + k];
        if (found->container == c && found->gap == gap) return found->index;
    }
    return 0;
}

static double gap_start(const layout *l, uint32_t c, uint32_t g) {
    const container *info = &l->containers[c];
    return content_origin(l, c).x + info->layer_x[g] + info->layer_w[g];
}

static double channel_x(const router *r, const pending *p, uint32_t c, uint32_t g) {
    const layout *l = r->l;
    const container *info = &l->containers[c];
    uint32_t last = info->layer_total - 1;
    if (g >= last) {
        uint32_t i = channel_index(r, p, c, TA_NIL);
        return content_origin(l, c).x + info->layer_x[last] + info->layer_w[last] + (i + 1) * l->sp.edge;
    }
    uint32_t i = channel_index(r, p, c, g);
    return gap_start(l, c, g) + info->gap_label[g] + (i + 1) * l->sp.edge;
}

/* hierarchy-aware exits: leave nested boxes on one side without crossing their siblings */
static double gutter(router *r, uint32_t parent, double y, double *offset) {
    layout *l = r->l;
    uint32_t i = r->escapes[parent]++;
    rect pr = canonical(l, parent);
    double top = content_origin(l, parent).y - l->sp.padding / 2;
    double bottom = pr.y + pr.h - l->sp.padding / 2;
    *offset = l->sp.edge * (0.6 + (i % 3) * 0.4);
    return ta_diagram_abs(y - top) <= ta_diagram_abs(y - bottom) ? top : bottom;
}

typedef struct way {
    uint32_t from, up_to;
    int direction; /* +1: far, -1: near */
} way;

/* From `start` on a side of `from` out through the same side of its ancestor `up_to`, onto `out`. */
static void exit_points(router *r, way w, point start, points *out) {
    layout *l = r->l;
    point p = start;
    points_push(l, out, start);
    uint32_t current = w.from;
    while (current != w.up_to) {
        uint32_t parent = parent_of(l, current);
        rect pr = canonical(l, parent), cr = canonical(l, current);
        double border = w.direction > 0 ? pr.x + pr.w : pr.x;
        int blocked = 0;
        for (uint32_t i = 0; i < l->kids_count[parent] && !blocked; i++) {
            uint32_t k = l->kids[l->kids_first[parent] + i];
            if (k == current) continue;
            rect kr = canonical(l, k);
            int beyond = w.direction > 0 ? kr.x >= cr.x + cr.w - 0.5 : kr.x + kr.w <= cr.x + 0.5;
            blocked = beyond && kr.y - 2 < p.y && p.y < kr.y + kr.h + 2;
        }
        if (blocked) {
            double offset;
            double gy = gutter(r, parent, p.y, &offset);
            double xe = w.direction > 0 ? cr.x + cr.w + offset : cr.x - offset;
            points_push(l, out, (point){xe, p.y});
            points_push(l, out, (point){xe, gy});
            p = (point){border, gy};
        } else
            p = (point){border, p.y};
        points_push(l, out, p);
        current = parent;
    }
}

/* Through `up_to`'s side into `end` on the same side of `to` (the reverse of an exit). */
static void enter_points(router *r, way w, point end, points *out) {
    uint32_t mark = out->length;
    exit_points(r, w, end, out);
    if (r->l->failed) return;
    for (uint32_t i = mark, j = out->length - 1; i < j; i++, j--) {
        point swap = out->data[i];
        out->data[i] = out->data[j];
        out->data[j] = swap;
    }
}

/* Horizontal run through layers lo..hi (canonical), bending in each gap's channel and passing
 * dummies. */
typedef struct run {
    uint32_t container, lo, hi;
    double from_y, to_y;
} run;
static void corridor(router *r, const pending *p, run c, points *out) {
    layout *l = r->l;
    container *info = &l->containers[c.container];
    uint32_t a = child_of(l, c.container, p->route == ROUTE_REVERSED ? p->to : p->from);
    uint32_t b = child_of(l, c.container, p->route == ROUTE_REVERSED ? p->from : p->to);
    const chain *found = chain_between(info, a, b);
    double y = c.from_y;
    for (uint32_t g = c.lo; g < c.hi; g++) {
        double x = channel_x(r, p, c.container, g);
        points_push(l, out, (point){x, y});
        const lnode *next = g + 1 < c.hi && found && g - c.lo < found->count ? &info->nodes[found->first + (g - c.lo)] : 0;
        y = next ? content_origin(l, c.container).y + next->y + next->h / 2 : c.to_y;
        points_push(l, out, (point){x, y});
        if (next)
            points_push(l, out, (point){content_origin(l, c.container).x + info->layer_x[g + 1] + info->layer_w[g + 1], y});
    }
}

static double lane_y(const layout *l, uint32_t c, uint32_t lane) {
    rect rr = canonical(l, c);
    return rr.y + rr.h - l->sp.padding / 2 - (l->containers[c].lanes - lane) * l->sp.edge;
}

/* Straighten sub-pixel jogs, then drop repeated points and the middle of collinear runs. */
static uint32_t simplify(point *p, uint32_t count) {
    /* a jog of less than a pixel only splits a straight line in two: move the run of points
     * sharing the jogged coordinate onto the previous point's */
    for (uint32_t i = 1; i < count; i++)
        for (int axis = 0; axis < 2; axis++) {
            double *at = axis ? &p[i].y : &p[i].x, *before = axis ? &p[i - 1].y : &p[i - 1].x;
            double d = ta_diagram_abs(*at - *before);
            if (!(d > 0.01 && d < 1)) continue;
            double old = *at;
            for (uint32_t j = i; j < count; j++) {
                double *v = axis ? &p[j].y : &p[j].x;
                if (ta_diagram_abs(*v - old) >= 0.01) break;
                *v = *before;
            }
        }
    uint32_t out = 0;
    for (uint32_t i = 0; i < count; i++) {
        point q = p[i];
        if (out && ta_diagram_abs(p[out - 1].x - q.x) < 0.01 && ta_diagram_abs(p[out - 1].y - q.y) < 0.01) continue;
        p[out++] = q;
        while (out >= 3) {
            point a = p[out - 3], b = p[out - 2], c = p[out - 1];
            int collinear = (ta_diagram_abs(a.x - b.x) < 0.01 && ta_diagram_abs(b.x - c.x) < 0.01) ||
                            (ta_diagram_abs(a.y - b.y) < 0.01 && ta_diagram_abs(b.y - c.y) < 0.01);
            if (!collinear) break;
            p[out - 2] = p[out - 1];
            out--;
        }
    }
    return out;
}

/* the square of the distance (the nearest segment is all that is asked) */
static double distance_to_segment(point p, point a, point b) {
    double dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
    double t = length != 0 ? ta_diagram_max(0, ta_diagram_min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / length)) : 0;
    double ex = p.x - (a.x + t * dx), ey = p.y - (a.y + t * dy);
    return ex * ex + ey * ey;
}

/* One edge's route (canonical), its simplified points appended to `all`. */
typedef struct routed {
    uint32_t first, count; /* in the canonical points */
    uint32_t prefer;
    uint32_t pending;
} routed;

static void route_one(router *r, const pending *p, routed *out, points *all) {
    layout *l = r->l;
    const ta_diagram_spacing *sp = &l->sp;
    rect s = canonical(l, p->from), t = canonical(l, p->to);
    points *pts = &r->scratch;
    pts->length = 0;
    uint32_t prefer = 0, c = p->container;
    switch (p->route) {
    case ROUTE_FORWARD:
    case ROUTE_REVERSED:
    case ROUTE_SAME: {
        /* a reversed edge is laid out like a forward edge from the target to the source, then reversed */
        int reversed = p->route == ROUTE_REVERSED, same = p->route == ROUTE_SAME;
        uint32_t early = reversed ? p->to : p->from, late = reversed ? p->from : p->to;
        rect er = reversed ? t : s, lr = reversed ? s : t;
        double early_at = r->attachments[reversed ? p->end : p->start].at;
        double late_at = r->attachments[reversed ? p->start : p->end].at;
        uint32_t a = child_of(l, c, early), b = child_of(l, c, late);
        exit_points(r, (way){early, a, 1}, (point){er.x + er.w, early_at}, pts);
        if (l->failed) return;
        double out_y = pts->data[pts->length - 1].y;
        points inn = {0, 0, 0};
        enter_points(r, (way){late, b, same ? 1 : -1}, (point){same ? lr.x + lr.w : lr.x, late_at}, &inn);
        if (l->failed) return;
        if (same) {
            double x = channel_x(r, p, c, layer_of(l, c, a));
            points_push(l, pts, (point){x, out_y});
            points_push(l, pts, (point){x, inn.data[0].y});
        } else
            corridor(r, p, (run){c, layer_of(l, c, a), layer_of(l, c, b), out_y, inn.data[0].y}, pts);
        for (uint32_t i = 0; i < inn.length; i++)
            points_push(l, pts, inn.data[i]);
        if (l->failed) return;
        if (reversed)
            for (uint32_t i = 0, j = pts->length - 1; i < j; i++, j--) {
                point swap = pts->data[i];
                pts->data[i] = pts->data[j];
                pts->data[j] = swap;
            }
        prefer = 0; /* at the source: a label further along reads as belonging to something else */
        break;
    }
    case ROUTE_REGIONS: {
        double x0 = r->attachments[p->start].at, x1 = r->attachments[p->end].at, ly = lane_y(l, c, p->lane);
        points_push(l, pts, (point){x0, s.y + s.h});
        points_push(l, pts, (point){x0, ly});
        points_push(l, pts, (point){x1, ly});
        points_push(l, pts, (point){x1, t.y + t.h});
        prefer = 1;
        break;
    }
    case ROUTE_TO_ANCESTOR: {
        exit_points(r, (way){p->from, child_of(l, p->to, p->from), 1}, (point){s.x + s.w, r->attachments[p->start].at},
                    pts);
        if (l->failed) return;
        points_push(l, pts, (point){t.x + t.w, pts->data[pts->length - 1].y}); /* onto its border, from inside */
        break;
    }
    case ROUTE_TO_DESCENDANT: {
        /* from just inside the container, along its top gutter, then down just before the target */
        uint32_t b = child_of(l, p->from, p->to);
        points inn = {0, 0, 0};
        enter_points(r, (way){p->to, b, -1}, (point){t.x, r->attachments[p->end].at}, &inn);
        if (l->failed) return;
        point first = inn.data[0];
        double gy = content_origin(l, p->from).y - sp->padding / 2;
        const container *fi = &l->containers[p->from];
        double xg = fi->kind == CONTAINER_PARALLEL
                        ? first.x - sp->edge
                        : content_origin(l, p->from).x + fi->layer_x[layer_of(l, p->from, b)] - sp->edge;
        points_push(l, pts, (point){s.x, gy}); /* from its own border */
        points_push(l, pts, (point){xg, gy});
        points_push(l, pts, (point){xg, first.y});
        for (uint32_t i = 0; i < inn.length; i++)
            points_push(l, pts, inn.data[i]);
        break;
    }
    default: { /* ROUTE_SELF */
        double reach = sp->edge * self_out;
        const attachment *a = &r->attachments[p->start];
        /* the loop's ends, as far apart as its label needs, within its slot */
        double half = ta_diagram_max(sp->edge / 2, ta_diagram_min(label_canon(l, p->transition).h + sp->label, a->slot - sp->edge) / 2);
        double ya = a->at - half, yb = a->at + half;
        points_push(l, pts, (point){s.x + s.w, ya});
        points_push(l, pts, (point){s.x + s.w + reach, ya});
        points_push(l, pts, (point){s.x + s.w + reach, yb});
        points_push(l, pts, (point){s.x + s.w, yb});
        prefer = 1;
        break;
    }
    }
    if (l->failed || pts->length == 0) return;
    /* map the preferred segment through simplification: the nearest to its old midpoint */
    point pa = pts->data[prefer < pts->length - 1 ? prefer : pts->length - (pts->length > 1 ? 2 : 1)];
    point pb = pts->data[prefer + 1 < pts->length ? prefer + 1 : pts->length - 1];
    point mid = {(pa.x + pb.x) / 2, (pa.y + pb.y) / 2};
    uint32_t first = all->length;
    for (uint32_t i = 0; i < pts->length; i++)
        points_push(l, all, pts->data[i]);
    if (l->failed) return;
    uint32_t count = simplify(&all->data[first], pts->length);
    all->length = first + count;
    uint32_t best = 0;
    double best_d = big;
    for (uint32_t i = 0; i + 1 < count; i++) {
        double d = distance_to_segment(mid, all->data[first + i], all->data[first + i + 1]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    *out = (routed){first, count, best, (uint32_t)(p - r->pendings)};
}

static uint32_t edge_kind(uint32_t route) {
    switch (route) {
    case ROUTE_FORWARD: return TA_DIAGRAM_EDGE_FORWARD;
    case ROUTE_SELF: return TA_DIAGRAM_EDGE_SELF;
    case ROUTE_TO_ANCESTOR: return TA_DIAGRAM_EDGE_TO_ANCESTOR;
    case ROUTE_TO_DESCENDANT: return TA_DIAGRAM_EDGE_TO_DESCENDANT;
    default: return TA_DIAGRAM_EDGE_BACK;
    }
}

/* ── 5. labels ── */

static int overlaps(rect a, rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
static rect inflate(rect r, double d) {
    return (rect){r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d};
}
/* 1 when the axis-parallel segment a-b passes through the inside of `r` */
static int segment_through(rect r, point a, point b) {
    double x0 = ta_diagram_min(a.x, b.x), x1 = ta_diagram_max(a.x, b.x);
    double y0 = ta_diagram_min(a.y, b.y), y1 = ta_diagram_max(a.y, b.y);
    return x0 < r.x + r.w && r.x < x1 && y0 < r.y + r.h && r.y < y1;
}

static const double clearance = 2; /* between a label and what it must not touch */

/* How strictly a place must be free: of other edges' lines too, and inside the container its
 * edge runs in, or only of labels and boxes. */
typedef enum label_tier { TIER_CLEAR = 0, TIER_LINES_ALLOWED = 1, TIERS = 2 } label_tier;

typedef struct labeller {
    const ta_diagram_spacing *sp;
    rect *obstacles; /* the leaves */
    uint32_t obstacle_count;
    rect *bands; /* the containers' header bands: their own margin is room enough */
    uint32_t band_count;
    rect *placed;
    uint32_t placed_count;
    const point *points;          /* every route's canonical points */
    const ta_diagram_edge *edges; /* every route, its points among `points` */
    uint32_t edge_count;
    uint32_t transition; /* the label's own: its routes may run under it */
    int bounded;         /* 1: at TIER_CLEAR the label stays inside `bound` */
    rect bound;          /* the innermost container but the root holding both ends of its edge */
    uint32_t tier;       /* a label_tier */
    uint32_t *order;     /* scratch: the segments of the route, in the order they are tried */
} labeller;

/* A route to label: its points, the segment its label prefers, its kind and the label's size. */
typedef struct label_route {
    const point *points;
    uint32_t count, prefer, kind;
    size size;
} label_route;

/* 1 when a label at `r` keeps clear of the placed labels, the leaves and the header bands, and
 * at TIER_CLEAR of other edges' lines and inside its bound, by a margin of `clearance` (none
 * from a band) */
static int is_free(const labeller *b, rect r) {
    rect wide = inflate(r, clearance);
    for (uint32_t i = 0; i < b->placed_count; i++)
        if (overlaps(b->placed[i], wide)) return 0;
    for (uint32_t i = 0; i < b->obstacle_count; i++)
        if (overlaps(b->obstacles[i], wide)) return 0;
    for (uint32_t i = 0; i < b->band_count; i++)
        if (overlaps(b->bands[i], r)) return 0;
    if (b->tier == TIER_LINES_ALLOWED) return 1;
    if (b->bounded && (r.x < b->bound.x || r.y < b->bound.y || r.x + r.w > b->bound.x + b->bound.w ||
                       r.y + r.h > b->bound.y + b->bound.h))
        return 0;
    for (uint32_t i = 0; i < b->edge_count; i++) {
        const ta_diagram_edge *edge = &b->edges[i];
        if (edge->transition == b->transition) continue;
        const point *p = &b->points[edge->first_point];
        for (uint32_t k = 1; k < edge->point_count; k++)
            if (segment_through(wide, p[k - 1], p[k])) return 0;
    }
    return 1;
}

static double segment_length(point a, point b) {
    return ta_diagram_abs(b.x - a.x) + ta_diagram_abs(b.y - a.y);
}

static rect rect_at(size s, double x, double y) {
    return (rect){x - s.w / 2, y - s.h / 2, s.w, s.h};
}

/* the preferred segment, then the ones next to it along the route, longer ones first among equals */
static uint32_t segment_order(const labeller *b, const label_route *route) {
    const point *pts = route->points;
    uint32_t segments = route->count - 1, prefer = route->prefer, *order = b->order;
    uint32_t n = 0;
    order[n++] = prefer;
    for (uint32_t i = 0; i < segments; i++)
        if (i != prefer) order[n++] = i;
    for (uint32_t i = 2; i < n; i++) {
        uint32_t moving = order[i], j = i;
        for (; j > 1; j--) {
            uint32_t before = order[j - 1];
            int64_t distance_before = (int64_t)before - (int64_t)prefer, distance_moving = (int64_t)moving - (int64_t)prefer;
            uint64_t ab = (uint64_t)(distance_before < 0 ? -distance_before : distance_before), am = (uint64_t)(distance_moving < 0 ? -distance_moving : distance_moving);
            if (ab < am || (ab == am && segment_length(pts[before], pts[before + 1]) >=
                                            segment_length(pts[moving], pts[moving + 1])))
                break;
            order[j] = before;
        }
        order[j] = moving;
    }
    return n;
}

/* Where along a segment a label slides to, the `i`th try from 1: its middle, then in twentieths
 * either side of it, the side nearer the segment's start first. */
enum { slides = 19 };
static double slide(uint32_t i) {
    double away = (double)(i / 2) * 0.05;
    return i % 2 ? 0.5 + away : 0.5 - away;
}

/* Where a label goes, at the labeller's tier: on its own route — the preferred segment first,
 * then the others, sliding along each; then beside a segment; only then stepping aside. 0 when
 * no place is free. */
static int place_at_tier(const labeller *b, const label_route *route, rect *chosen) {
    const point *pts = route->points;
    size s = route->size;
    uint32_t n = segment_order(b, route);
    const uint32_t *order = b->order;
    for (uint32_t k = 0; k < n; k++) {
        point a = pts[order[k]], z = pts[order[k] + 1];
        int horizontal = ta_diagram_abs(a.y - z.y) < 0.01;
        double room = horizontal ? ta_diagram_abs(z.x - a.x) - s.w : ta_diagram_abs(z.y - a.y) - s.h;
        if (room < 4) continue;
        /* an edge between layers that runs straight from its port: the label just past the port first */
        int past_port = order[k] == 0 && horizontal &&
                        (route->kind == TA_DIAGRAM_EDGE_FORWARD || route->kind == TA_DIAGRAM_EDGE_BACK);
        for (uint32_t i = past_port ? 0 : 1; i <= slides; i++) {
            double t = i == 0 ? (s.w / 2 + 2 * b->sp->label) / segment_length(a, z) : slide(i);
            rect r = rect_at(s, a.x + (z.x - a.x) * t, a.y + (z.y - a.y) * t);
            /* keep the label inside the segment's extent */
            if (horizontal ? r.x < ta_diagram_min(a.x, z.x) || r.x + r.w > ta_diagram_max(a.x, z.x)
                           : r.y < ta_diagram_min(a.y, z.y) || r.y + r.h > ta_diagram_max(a.y, z.y))
                continue;
            if (is_free(b, r)) {
                *chosen = r;
                return 1;
            }
        }
    }
    /* beside a segment, just clear of the line: either side, preferred segment first */
    for (uint32_t k = 0; k < n; k++) {
        point a = pts[order[k]], z = pts[order[k] + 1];
        int horizontal = ta_diagram_abs(a.y - z.y) < 0.01;
        double room = horizontal ? ta_diagram_abs(z.x - a.x) - s.w : ta_diagram_abs(z.y - a.y) - s.h;
        if (room < 0) continue;
        double offset = (horizontal ? s.h : s.w) / 2 + 4;
        for (uint32_t i = 1; i <= slides; i++) {
            double mx = a.x + (z.x - a.x) * slide(i), my = a.y + (z.y - a.y) * slide(i);
            for (int side = 1; side >= -1; side -= 2) {
                rect r = horizontal ? rect_at(s, mx, my + side * offset) : rect_at(s, mx + side * offset, my);
                if (is_free(b, r)) {
                    *chosen = r;
                    return 1;
                }
            }
        }
    }
    /* step aside from a segment, closest first: the preferred one, then the others, a step of
     * about half a label's height at a time (beside a vertical one, from just clear of it),
     * sliding along it at each distance */
    static const int steps[10] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5};
    for (uint32_t k = 0; k < n; k++) {
        point a = pts[order[k]], z = pts[order[k] + 1];
        int horizontal = ta_diagram_abs(a.y - z.y) < 0.01;
        double unit = (s.h + 4) * 0.6, beside = horizontal ? 0 : s.w / 2 + 4 - unit;
        double length = segment_length(a, z), along = horizontal ? s.w : s.h;
        double beyond = length > 0 && along > length ? (along / 2 - 4) / length : 0;
        for (uint32_t i = 0; i < 10; i++) {
            int step = steps[i];
            double across = (step < 0 ? -1 : 1) * (beside + (step < 0 ? -step : step) * unit);
            /* along it; then, for a segment shorter than the label, just past either end */
            for (uint32_t j = 1; j <= slides + 2; j++) {
                double t = j <= slides ? slide(j) : j == slides + 1 ? 1 + beyond : -beyond;
                double mx = a.x + (z.x - a.x) * t, my = a.y + (z.y - a.y) * t;
                rect r = horizontal ? rect_at(s, mx, my + across) : rect_at(s, mx + across, my);
                if (is_free(b, r)) {
                    *chosen = r;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Where a label goes: clear of other edges' lines, and inside the container its edge runs in,
 * when any place is; else clear of labels and boxes only; else at its preferred segment's
 * middle. */
static rect place_label(labeller *b, const label_route *route) {
    rect chosen;
    for (b->tier = TIER_CLEAR; b->tier < TIERS; b->tier++)
        if (place_at_tier(b, route, &chosen)) return chosen;
    point a = route->points[route->prefer], z = route->points[route->prefer + 1];
    return rect_at(route->size, (a.x + z.x) / 2, (a.y + z.y) / 2);
}

/* ---- ends on the drawn outline ---------------------------------------------------------- */

static point transposed(point p) {
    return (point){p.y, p.x};
}

/* Where a route's end `at`, reached along the axis-parallel segment from `from`, meets the drawn
 * outline of a circle or a pill: moved along that segment onto the outline, on the side `from`
 * is. An end whose line misses the shape stays (ta_diagram_check then reports it). */
typedef struct outline_end {
    rect box;
    uint32_t outline;
    point at, from;
} outline_end;

static point onto_outline(outline_end e) {
    if (e.outline == TA_DIAGRAM_OUTLINE_RECTANGLE) return e.at;
    int vertical = ta_diagram_abs(e.at.x - e.from.x) < 0.01;
    if (vertical) { /* solve it as a horizontal one */
        e.box = (rect){e.box.y, e.box.x, e.box.h, e.box.w};
        e.at = transposed(e.at);
        e.from = transposed(e.from);
    }
    ta_diagram_spine spine = ta_diagram_spine_of(e.box, e.outline);
    double y = e.at.y, near_x, far_x;
    if (ta_diagram_abs(spine.a.y - spine.b.y) < 0.01) { /* the spine along the line */
        double dy = y - spine.a.y;
        if (ta_diagram_abs(dy) > spine.radius) return vertical ? transposed(e.at) : e.at;
        double half = ta_diagram_root(spine.radius * spine.radius - dy * dy);
        near_x = ta_diagram_min(spine.a.x, spine.b.x) - half;
        far_x = ta_diagram_max(spine.a.x, spine.b.x) + half;
    } else { /* the spine across it: the round end nearest the line, or the straight sides */
        double top = ta_diagram_min(spine.a.y, spine.b.y), bottom = ta_diagram_max(spine.a.y, spine.b.y);
        double nearest = y < top ? top : y > bottom ? bottom : y, dy = y - nearest;
        if (ta_diagram_abs(dy) > spine.radius) return vertical ? transposed(e.at) : e.at;
        double half = ta_diagram_root(spine.radius * spine.radius - dy * dy);
        near_x = spine.a.x - half;
        far_x = spine.a.x + half;
    }
    point moved = {e.from.x <= (near_x + far_x) / 2 ? near_x : far_x, y};
    return vertical ? transposed(moved) : moved;
}

/* Every route that ends at a history state's circle or a final's pill ends on it, and one that
 * leaves a circle leaves from it: its end moved along its last segment, so the arrow touches
 * what is drawn. */
static void ends_onto_outlines(const layout *l, ta_diagram_layout *out, point *pts) {
    for (uint32_t i = 0; i < out->edge_count; i++) {
        const ta_diagram_edge *edge = &out->edges[i];
        if (edge->point_count < 2) continue;
        point *p = &pts[edge->first_point];
        uint32_t last = edge->point_count - 1;
        uint32_t target = edge->target, source = l->chart->transitions[edge->transition].source;
        p[last] = onto_outline((outline_end){l->abs[target], ta_diagram_outline_of(box_kind(l, target)), p[last], p[last - 1]});
        p[0] = onto_outline((outline_end){l->abs[source], ta_diagram_outline_of(box_kind(l, source)), p[0], p[1]});
    }
    for (uint32_t i = 0; i < out->initial_count; i++) {
        const ta_diagram_initial *marker = &out->initials[i];
        point *p = &pts[marker->first_point];
        uint32_t last = marker->point_count - 1;
        p[last] = onto_outline((outline_end){l->abs[marker->target], ta_diagram_outline_of(box_kind(l, marker->target)),
                                             p[last], p[last - 1]});
    }
}

/* ---- contain ------------------------------------------------------------------------------ */

typedef struct extent {
    double x0, y0, x1, y1;
} extent;
static void extend(extent *e, rect r) {
    e->x0 = ta_diagram_min(e->x0, r.x);
    e->y0 = ta_diagram_min(e->y0, r.y);
    e->x1 = ta_diagram_max(e->x1, r.x + r.w);
    e->y1 = ta_diagram_max(e->y1, r.y + r.h);
}

/* Edges routed around a container's outside, and their labels, can reach past the root's box:
 * move everything so nothing has a negative coordinate, and grow the size to hold it all. */
static void contain(ta_diagram_layout *out, ta_diagram_box *boxes, ta_diagram_edge *edges, ta_diagram_initial *initials,
                    point *pts) {
    extent e = {0, 0, out->width, out->height};
    for (uint32_t i = 0; i < out->edge_count; i++) {
        for (uint32_t j = 0; j < edges[i].point_count; j++) {
            point p = pts[edges[i].first_point + j];
            extend(&e, (rect){p.x, p.y, 0, 0});
        }
        if (edges[i].labelled) extend(&e, edges[i].label);
    }
    for (uint32_t i = 0; i < out->initial_count; i++) {
        point p = initials[i].at;
        extend(&e, (rect){p.x - initial_dot / 2, p.y - initial_dot / 2, initial_dot, initial_dot});
    }
    double dx = -e.x0, dy = -e.y0;
    if (dx == 0 && dy == 0 && e.x1 <= out->width && e.y1 <= out->height) return;
    for (uint32_t i = 0; i < out->box_count; i++)
        if (boxes[i].kind != TA_DIAGRAM_BOX_ROOT) {
            boxes[i].rect.x += dx;
            boxes[i].rect.y += dy;
        }
    for (uint32_t i = 0; i < out->edge_count; i++) {
        for (uint32_t j = 0; j < edges[i].point_count; j++) {
            pts[edges[i].first_point + j].x += dx;
            pts[edges[i].first_point + j].y += dy;
        }
        if (edges[i].labelled) {
            edges[i].label.x += dx;
            edges[i].label.y += dy;
        }
    }
    for (uint32_t i = 0; i < out->initial_count; i++) {
        initials[i].at.x += dx;
        initials[i].at.y += dy;
        for (uint32_t j = 0; j < initials[i].point_count; j++) {
            pts[initials[i].first_point + j].x += dx;
            pts[initials[i].first_point + j].y += dy;
        }
    }
    out->width = e.x1 + dx;
    out->height = e.y1 + dy;
    for (uint32_t i = 0; i < out->box_count; i++)
        if (boxes[i].kind == TA_DIAGRAM_BOX_ROOT) boxes[i].rect = (rect){0, 0, out->width, out->height};
}

/* ---- the layout --------------------------------------------------------------------------- */

static void route(layout *l, ta_diagram_layout *out, ta_diagram_box *boxes) {
    const ta_diagram_spacing *sp = &l->sp;
    int down = l->down;
    router r;
    ta_memset(&r, 0, sizeof r);
    r.l = l;
    uint32_t pairs = 0;
    for (uint32_t i = 0; i < l->transition_count; i++)
        pairs += transition_of(l, l->transitions[i])->target_count;
    r.pendings = take(l, sizeof(pending), pairs + 1);
    r.attachments = take(l, sizeof(attachment), 2 * pairs + 1);
    r.group_of = take(l, sizeof(uint32_t), l->n * SIDES);
    r.group_box = take(l, sizeof(uint32_t), 2 * pairs + 1);
    r.group_side = take(l, sizeof(uint32_t), 2 * pairs + 1);
    r.group_first = take(l, sizeof(uint32_t), 2 * pairs + 1);
    r.group_count = take(l, sizeof(uint32_t), 2 * pairs + 1);
    r.group_list = take(l, sizeof(uint32_t), 2 * pairs + 1);
    r.group_start = take(l, sizeof(double), 2 * pairs + 1);
    r.group_span = take(l, sizeof(double), 2 * pairs + 1);
    r.escapes = take(l, sizeof(uint32_t), l->n);
    routed *all_routed = take(l, sizeof(routed), pairs + 1);
    if (l->failed) return;
    classify(&r);
    ports(&r);
    channels(&r);
    if (l->failed) return;

    /* ── 4. points (canonical); each edge names the segment its label prefers ── */
    points canonical_points = {0, 0, 0};
    for (uint32_t i = 0; i < r.pending_count && !l->failed; i++)
        route_one(&r, &r.pendings[i], &all_routed[i], &canonical_points);
    if (l->failed) return;

    ta_diagram_edge *edges = take(l, sizeof(ta_diagram_edge), r.pending_count + 1);
    ta_diagram_initial *initials = take(l, sizeof(ta_diagram_initial), l->n + 1);
    if (l->failed) return;
    for (uint32_t i = 0; i < r.pending_count; i++) {
        const pending *p = &r.pendings[i];
        edges[i] = (ta_diagram_edge){p->transition, p->target, edge_kind(p->route), all_routed[i].first,
                                     all_routed[i].count, 0, {0, 0, 0, 0}};
    }

    /* ── 5. labels (canonical) ── */
    labeller b;
    ta_memset(&b, 0, sizeof b);
    b.sp = sp;
    b.obstacles = take(l, sizeof(rect), l->n + 1);
    b.bands = take(l, sizeof(rect), l->n + 1);
    b.placed = take(l, sizeof(rect), r.pending_count + 1);
    b.points = canonical_points.data;
    b.edges = edges;
    b.edge_count = r.pending_count;
    uint8_t *labelled = take(l, sizeof(uint8_t), l->chart->transition_count + 1);
    b.order = take(l, sizeof(uint32_t), canonical_points.length + 1);
    if (l->failed) return;
    for (uint32_t i = 0; i < l->n; i++) {
        uint32_t s = l->walk[i];
        if (s == 0) continue;
        const container *info = &l->containers[s];
        rect a = l->abs[s];
        if (info->kind == CONTAINER_NONE)
            b.obstacles[b.obstacle_count++] = tr(a, down);
        else if (info->header > 0)
            b.bands[b.band_count++] = tr((rect){a.x, a.y, a.w, info->header}, down);
    }
    /* self loops first: their label has one place (on the loop); the others can slide along their edges */
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < r.pending_count; i++) {
            ta_diagram_edge *edge = &edges[i];
            if ((edge->kind == TA_DIAGRAM_EDGE_SELF) != (pass == 0)) continue;
            size real = label_size(l, edge->transition);
            if (!(real.w > 0 && real.h > 0) || labelled[edge->transition] || all_routed[i].count < 2) continue;
            labelled[edge->transition] = 1;
            b.transition = edge->transition;
            /* the innermost container but the root holding both ends */
            uint32_t source = l->chart->transitions[edge->transition].source, holder = parent_of(l, source);
            while (holder != TA_NIL && !within(l, edge->target, holder))
                holder = parent_of(l, holder);
            b.bounded = holder != TA_NIL && holder != 0;
            if (b.bounded) b.bound = canonical(l, holder);
            rect chosen = place_label(&b, &(label_route){&canonical_points.data[all_routed[i].first], all_routed[i].count,
                                                         all_routed[i].prefer, edge->kind, label_canon(l, edge->transition)});
            b.placed[b.placed_count++] = chosen;
            edge->labelled = 1;
            edge->label = tr(chosen, down);
        }
    for (uint32_t i = 0; i < canonical_points.length; i++)
        canonical_points.data[i] = tp(canonical_points.data[i], down);

    /* ── 6. initial markers: a dot just before each initial target ── */
    uint32_t initial_count = 0;
    for (uint32_t i = 0; i < l->post_count; i++) {
        uint32_t c = l->post[i];
        if (l->containers[c].kind == CONTAINER_PARALLEL) continue;
        const ta_diagram_state *cs = state_of(l, c);
        for (uint32_t j = 0; j < cs->initial_count; j++) {
            uint32_t to = l->chart->targets[cs->first_initial + j];
            if (!within(l, to, c) || to == c) continue;
            rect t = canonical(l, to);
            double ty = t.y + ta_diagram_min(t.h / 2, 16);
            point dot = {t.x - sp->edge * 1.6 - initial_dot / 2, ty};
            ta_diagram_initial *marker = &initials[initial_count++];
            marker->container = c;
            marker->target = to;
            marker->at = tp(dot, down);
            marker->first_point = canonical_points.length;
            marker->point_count = 2;
            points_push(l, &canonical_points, tp((point){dot.x + initial_dot / 2, ty}, down));
            points_push(l, &canonical_points, tp((point){t.x, ty}, down));
        }
    }
    if (l->failed) return;
    out->edges = edges;
    out->edge_count = r.pending_count;
    out->initials = initials;
    out->initial_count = initial_count;
    out->points = canonical_points.data;
    ends_onto_outlines(l, out, canonical_points.data);
    contain(out, boxes, edges, initials, canonical_points.data);
}

/* What each box lists inside: its transitions without a target and its portals, in walk order. */
static void inside(layout *l, ta_diagram_layout *out, const uint8_t *portals) {
    uint32_t *first = take(l, sizeof(uint32_t), l->n), *count = take(l, sizeof(uint32_t), l->n);
    uint32_t *list = take(l, sizeof(uint32_t), l->chart->transition_count + 1);
    if (l->failed) return;
    for (uint32_t t = 0; t < l->chart->transition_count; t++)
        if (!l->chart->transitions[t].target_count || (portals && portals[t])) count[l->chart->transitions[t].source]++;
    for (uint32_t s = 0, next = 0; s < l->n; s++) {
        first[s] = next;
        next += count[s];
        count[s] = 0;
    }
    for (uint32_t t = 0; t < l->chart->transition_count; t++) {
        uint32_t s = l->chart->transitions[t].source;
        if (!l->chart->transitions[t].target_count || (portals && portals[t])) list[first[s] + count[s]++] = t;
    }
    out->inside = list;
    out->inside_first = first;
    out->inside_count = count;
}

ta_status ta_diagram_layout_chart(const ta_diagram_request *request, ta_diagram_layout *out) {
    layout l;
    ta_memset(&l, 0, sizeof l);
    ta_memset(out, 0, sizeof *out);
    l.request = request;
    l.chart = request->chart;
    l.arena = request->arena;
    l.sp = default_spacing;
    l.down = request->direction == TA_DIAGRAM_DOWN;
    ta_status status = prepare(&l);
    if (status != TA_OK) return status;
    /* every transition, in walk order, but the portals and those with no target */
    l.transitions = take(&l, sizeof(uint32_t), l.chart->transition_count + 1);
    l.containers = take(&l, sizeof(container), l.n);
    l.local = take(&l, sizeof(rect), l.n);
    l.abs = take(&l, sizeof(rect), l.n);
    ta_diagram_box *boxes = take(&l, sizeof(ta_diagram_box), l.n);
    if (l.failed) return TA_ERROR_NO_MEMORY;
    /* transitions of a state stay in document order; states in walk order */
    for (uint32_t i = 0; i < l.n; i++) {
        uint32_t s = l.walk[i];
        for (uint32_t t = 0; t < l.chart->transition_count; t++) {
            const ta_diagram_transition *transition = &l.chart->transitions[t];
            if (transition->source != s || !transition->target_count) continue;
            if (request->portals && request->portals[t]) continue;
            l.transitions[l.transition_count++] = t;
        }
    }
    layout_containers(&l);
    if (l.failed) return TA_ERROR_NO_MEMORY;
    place(&l, boxes);
    out->width = l.containers[0].whole.w;
    out->height = l.containers[0].whole.h;
    out->boxes = boxes;
    out->box_count = l.n;
    route(&l, out, boxes);
    inside(&l, out, request->portals);
    return l.failed ? TA_ERROR_NO_MEMORY : TA_OK;
}
