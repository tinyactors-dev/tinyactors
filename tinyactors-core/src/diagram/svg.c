/* diagram/svg.c — a chart's layout as SVG (svg.h), drawn as scxmljs's
 * <scxml-view> draws it: boxes with their names, a container's header
 * band, dashed parallel regions, finals with a double border, history as a
 * circled H, edges with arrows, transition labels as pills, the
 * transitions a box lists inside it as chips, and initial dots. Its
 * colours are the element's theme, light or dark by the reader's
 * preference. The sizes are the element's estimates (it measures text in a
 * browser; this cannot), less its buttons.
 *
 * Every byte goes through the ta_xml_writer: elements, attributes and
 * text, which it escapes. The stylesheet is the tables below, written as
 * the <style> element's text. */
#include "diagram/svg.h"

#include "base/memory.h"
#include "diagram/geometry.h"

/* ---- the element's measures (view/element.ts: estimateLabel, estimateBox) ---- */

static const double label_character = 6.8; /* an 11px monospace character */
static const double name_character = 7.6;  /* a 13px semibold character */
static const double label_height = 20;
static const double chip_row = 24;
static const double chips_width = 260;
static const double title_height = 30;
static const double page_margin = 12;

enum {
    CONDITION_SHOWN = 22, /* a longer condition is cut, with an ellipsis */
    INTERNAL_SHOWN = 3,   /* targetless transitions shown in a box; the rest counted */
    NUMBER_TEXT = 32      /* room for a number's text */
};

static uint32_t characters(ta_string_view text) { /* code points */
    uint32_t n = 0;
    for (uint32_t i = 0; i < text.length; i++)
        n += ((unsigned char)text.pointer[i] & 0xC0) != 0x80;
    return n;
}


/* "event event [condition]": the condition cut at CONDITION_SHOWN characters */
static uint32_t label_characters(const ta_diagram_transition *t) {
    uint32_t n = characters(t->events);
    uint32_t condition = characters(t->condition);
    if (condition) n += (n ? 1 : 0) + 2 + (condition > CONDITION_SHOWN ? CONDITION_SHOWN : condition);
    return n;
}

static ta_diagram_size label_estimate(const ta_diagram_transition *t) {
    uint32_t n = label_characters(t);
    return n ? (ta_diagram_size){ta_diagram_ceil(n * label_character + 18), label_height} : (ta_diagram_size){0, 0};
}

/* a portal's chip adds "→ target, target" */
static double targets_width(const ta_diagram_chart *chart, const ta_diagram_transition *t) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->target_count; i++)
        n += (i ? 2 : 0) + characters(chart->states[chart->targets[t->first_target + i]].name);
    return n * label_character + 16;
}

typedef struct chip_place {
    const ta_diagram_chart *chart;
    const uint8_t *portals;
} chip_place;

/* An internal chip's width. */
static double chip_width(chip_place where, uint32_t t) {
    const ta_diagram_transition *transition = &where.chart->transitions[t];
    int portal = where.portals && where.portals[t];
    double w = label_estimate(transition).w;
    if (w == 0) w = label_character + 18; /* an eventless one reads "ε" */
    return w + (portal ? targets_width(where.chart, transition) : 0);
}

static int is_history_kind(uint32_t kind) {
    return kind == TA_DIAGRAM_STATE_HISTORY_SHALLOW || kind == TA_DIAGRAM_STATE_HISTORY_DEEP;
}

static void *take(ta_growable_arena *arena, size_t size, size_t count) {
    return ta_growable_arena_alloc(arena, size * (count + 1), 8);
}

ta_status ta_diagram_measure(const ta_diagram_measure_request *request, ta_diagram_measures *out) {
    const ta_diagram_chart *chart = request->chart;
    uint32_t n = chart->state_count, transitions = chart->transition_count;
    out->states = take(request->arena, sizeof(ta_diagram_size), n);
    out->labels = take(request->arena, sizeof(ta_diagram_size), transitions);
    uint8_t *container = take(request->arena, 1, n);
    double *chips = take(request->arena, sizeof(double), n);
    uint32_t *internal = take(request->arena, sizeof(uint32_t), n);
    uint32_t *shown = take(request->arena, sizeof(uint32_t), n);
    if (!out->states || !out->labels || !container || !chips || !internal || !shown) return TA_ERROR_NO_MEMORY;
    for (uint32_t s = 0; s < n; s++) {
        container[s] = chart->states[s].kind == TA_DIAGRAM_STATE_PARALLEL;
        chips[s] = 0;
        internal[s] = shown[s] = 0;
    }
    for (uint32_t s = 1; s < n; s++)
        if (!is_history_kind(chart->states[s].kind) && chart->states[s].parent < s) container[chart->states[s].parent] = 1;
    chip_place where = {chart, request->portals};
    /* the portals, then the first few targetless transitions */
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t t = 0; t < transitions; t++) {
            const ta_diagram_transition *transition = &chart->transitions[t];
            int portal = request->portals && request->portals[t];
            out->labels[t] = transition->target_count && !portal ? label_estimate(transition) : (ta_diagram_size){0, 0};
            if (pass == 0 ? !portal : (transition->target_count || shown[transition->source] >= INTERNAL_SHOWN)) continue;
            if (pass == 1) shown[transition->source]++;
            internal[transition->source]++;
            chips[transition->source] += chip_width(where, t) + 4;
        }
    for (uint32_t s = 0; s < n; s++) {
        const ta_diagram_state *state = &chart->states[s];
        if (state->kind == TA_DIAGRAM_STATE_ROOT) {
            out->states[s] = (ta_diagram_size){0, 0};
            continue;
        }
        if (is_history_kind(state->kind)) {
            out->states[s] = (ta_diagram_size){26, 26};
            continue;
        }
        double name = characters(state->name) * name_character + 24 + (state->kind == TA_DIAGRAM_STATE_PARALLEL ? 16 : 0);
        double w = ta_diagram_max(64, ta_diagram_max(name, ta_diagram_min(chips[s], chips_width) + 22));
        double rows = internal[s] ? ta_diagram_ceil(ta_diagram_max(chips[s], 1) / chips_width) : 0;
        out->states[s] = container[s] ? (ta_diagram_size){w, 26 + rows * chip_row} : (ta_diagram_size){w, 34 + rows * chip_row};
    }
    return TA_OK;
}

/* ---- the stylesheet, as data ------------------------------------------------------------- */

/* The element's theme (ui/theme.ts): a token, its light value and its dark one. */
typedef struct theme_token {
    const char *name, *light, *dark;
} theme_token;

static const theme_token theme[] = {
    {"fg1", "#1c1c21", "#ececf1"},    {"fg2", "#484852", "#bcbcc6"},    {"fg3", "#6a6a75", "#90909b"},
    {"fg4", "#85858f", "#777781"},    {"bg", "#f6f6f8", "#141417"},     {"s1", "#ffffff", "#1c1c20"},
    {"s2", "#f0f0f3", "#242429"},     {"b2", "#d3d3da", "#393940"},     {"strong", "#8b8b96", "#6c6c77"},
    {"wait", "#a8560a", "#ffb46b"},   {"done", "#48566b", "#a7b5c9"},   {"error", "#c2332f", "#ff8a84"},
    {"sans", "system-ui, -apple-system, Roboto, sans-serif", "system-ui, -apple-system, Roboto, sans-serif"},
    {"mono", "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace",
     "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"},
};

typedef struct style_property {
    const char *name, *value;
} style_property;

enum { RULE_PROPERTIES = 5 };

/* A rule: a selector and its properties, the unused ones empty. */
typedef struct style_rule {
    const char *selector;
    style_property properties[RULE_PROPERTIES];
} style_rule;

static const style_rule rules[] = {
    {"text", {{"font-family", "var(--ta-sans)"}, {"fill", "var(--ta-fg1)"}}},
    {".ta-page", {{"fill", "var(--ta-bg)"}}},
    {".ta-title", {{"font-size", "15px"}, {"font-weight", "600"}}},
    {".ta-container", {{"fill", "var(--ta-s2)"}, {"fill-opacity", ".55"}, {"stroke", "var(--ta-b2)"}}},
    {".ta-container.ta-even", {{"fill", "var(--ta-s1)"}, {"fill-opacity", ".7"}}},
    {".ta-region", {{"fill", "none"}, {"stroke", "var(--ta-b2)"}, {"stroke-dasharray", "4 3"}}},
    {".ta-leaf, .ta-history", {{"fill", "var(--ta-s1)"}, {"stroke", "var(--ta-strong)"}}},
    {".ta-name", {{"font-size", "13px"}, {"font-weight", "600"}}},
    {".ta-kind", {{"font-size", "14px"}, {"font-weight", "600"}, {"fill", "var(--ta-fg3)"}}},
    {".ta-mark", {{"font-family", "var(--ta-mono)"}, {"font-size", "11px"}, {"font-weight", "600"}, {"fill", "var(--ta-fg2)"}}},
    {".ta-pill", {{"fill", "var(--ta-bg)"}, {"stroke", "var(--ta-b2)"}}},
    {".ta-pill.ta-done", {{"stroke", "var(--ta-done)"}}},
    {".ta-pill.ta-error", {{"stroke", "var(--ta-error)"}}},
    {".ta-event", {{"font-family", "var(--ta-mono)"}, {"font-size", "11px"}, {"font-weight", "500"}, {"fill", "var(--ta-fg2)"}}},
    {".ta-event .ta-done", {{"fill", "var(--ta-done)"}}},
    {".ta-event .ta-error", {{"fill", "var(--ta-error)"}}},
    {".ta-event .ta-condition", {{"fill", "var(--ta-wait)"}, {"font-weight", "400"}}},
    {".ta-event .ta-to", {{"fill", "var(--ta-fg3)"}, {"font-weight", "400"}}},
    {".ta-edge", {{"fill", "none"}, {"stroke", "var(--ta-fg4)"}, {"stroke-width", "1.25"}, {"stroke-linejoin", "round"}}},
    {".ta-initial-line", {{"fill", "none"}, {"stroke", "var(--ta-fg1)"}, {"stroke-width", "1.25"}}},
    {".ta-arrow", {{"fill", "var(--ta-fg4)"}}},
    {".ta-arrow.ta-initial, .ta-dot", {{"fill", "var(--ta-fg1)"}}},
};

/* ---- through the writer ---------------------------------------------------------------- */

typedef struct svg {
    ta_xml_writer *writer;
    const ta_diagram_drawing *d;
    double dx, dy; /* where the layout's origin is on the page */
    ta_status status; /* TA_ERROR_NO_MEMORY when the arena could not hold a path's data */
} svg;

static ta_string_view view_of(const char *text) {
    uint32_t n = 0;
    while (text[n])
        n++;
    return (ta_string_view){text, n};
}

static void open_element(ta_xml_writer *writer, const char *name) {
    ta_xml_writer_open(writer, view_of(name));
}
static void attribute(ta_xml_writer *writer, const char *name, const char *value) {
    ta_xml_writer_attribute(writer, view_of(name), view_of(value));
}
static void text(ta_xml_writer *writer, const char *value) {
    ta_xml_writer_text(writer, view_of(value));
}

/* A number with at most one decimal. */
typedef struct number_text {
    char bytes[NUMBER_TEXT];
} number_text;
static ta_string_view number_view(number_text *buffer, double value) {
    static const double largest = 1e18; /* tenths beyond it would not fit in 64 bits; no layout is that large */
    int negative = value < 0;
    double magnitude = negative ? -value : value;
    uint64_t tenths = (uint64_t)((magnitude < largest ? magnitude : largest) * 10 + 0.5);
    char digits[NUMBER_TEXT];
    uint32_t n = 0, length = 0;
    uint64_t whole = tenths / 10;
    do {
        digits[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    if (negative && tenths) buffer->bytes[length++] = '-';
    while (n)
        buffer->bytes[length++] = digits[--n];
    if (tenths % 10) {
        buffer->bytes[length++] = '.';
        buffer->bytes[length++] = (char)('0' + tenths % 10);
    }
    return (ta_string_view){buffer->bytes, length};
}

typedef struct numeric {
    const char *name;
    double value;
} numeric;
static void numbers(ta_xml_writer *writer, const numeric *list, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        number_text buffer;
        ta_xml_writer_attribute(writer, view_of(list[i].name), number_view(&buffer, list[i].value));
    }
}

/* The root's class: every rule is written under it, and every class and variable of the
 * drawing carries the prefix "ta-", so an SVG put inline in a page takes none of the page's
 * styles (a page's `.container { width: 100% }` would stretch a container's box) and gives the
 * page none of its own. */
static const char scope[] = "tinyactors-diagram";

/* `selector`, each of its comma-separated parts under the scope. */
static void scoped_selector(ta_xml_writer *writer, const char *selector) {
    ta_string_view all = view_of(selector);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= all.length; i++) {
        if (i < all.length && all.pointer[i] != ',') continue;
        while (start < i && all.pointer[start] == ' ')
            start++;
        if (start > 0) text(writer, ", ");
        text(writer, ".");
        text(writer, scope);
        text(writer, " ");
        ta_xml_writer_text(writer, (ta_string_view){all.pointer + start, i - start});
        start = i + 1;
    }
}

static void stylesheet(ta_xml_writer *writer) {
    open_element(writer, "style");
    for (int dark = 0; dark < 2; dark++) {
        text(writer, dark ? "\n@media (prefers-color-scheme: dark) { ." : "\n.");
        text(writer, scope);
        text(writer, " {");
        for (uint32_t i = 0; i < sizeof theme / sizeof theme[0]; i++) {
            text(writer, " --ta-");
            text(writer, theme[i].name);
            text(writer, ": ");
            text(writer, dark ? theme[i].dark : theme[i].light);
            text(writer, ";");
        }
        text(writer, dark ? " } }" : " }");
    }
    for (uint32_t i = 0; i < sizeof rules / sizeof rules[0]; i++) {
        text(writer, "\n");
        scoped_selector(writer, rules[i].selector);
        text(writer, " {");
        for (uint32_t j = 0; j < RULE_PROPERTIES && rules[i].properties[j].name; j++) {
            text(writer, " ");
            text(writer, rules[i].properties[j].name);
            text(writer, ": ");
            text(writer, rules[i].properties[j].value);
            text(writer, ";");
        }
        text(writer, " }");
    }
    text(writer, "\n");
    ta_xml_writer_close(writer);
}

/* An arrowhead 10 wide and 7 high, its tip at the line's end. */
static void marker(ta_xml_writer *writer, const char *id, const char *class_name) {
    open_element(writer, "marker");
    attribute(writer, "id", id);
    attribute(writer, "viewBox", "0 0 10 7");
    numeric place[4] = {{"refX", 9.5}, {"refY", 3.5}, {"markerWidth", 10}, {"markerHeight", 7}};
    numbers(writer, place, 4);
    attribute(writer, "markerUnits", "userSpaceOnUse");
    attribute(writer, "orient", "auto");
    open_element(writer, "path");
    attribute(writer, "class", class_name);
    attribute(writer, "d", "M0 0 L10 3.5 L0 7 z");
    ta_xml_writer_close(writer);
    ta_xml_writer_close(writer);
}

ta_status ta_diagram_svg_open(ta_xml_writer *writer, ta_diagram_size page) {
    open_element(writer, "svg");
    attribute(writer, "xmlns", "http://www.w3.org/2000/svg");
    attribute(writer, "class", scope);
    numeric size[2] = {{"width", page.w}, {"height", page.h}};
    numbers(writer, size, 2);
    number_text w, h;
    ta_string_view width = number_view(&w, page.w), height = number_view(&h, page.h);
    char box[3 * NUMBER_TEXT];
    uint32_t length = 0;
    for (const char *p = "0 0 "; *p; p++)
        box[length++] = *p;
    ta_memcpy(box + length, width.pointer, width.length);
    length += width.length;
    box[length++] = ' ';
    ta_memcpy(box + length, height.pointer, height.length);
    length += height.length;
    ta_xml_writer_attribute(writer, view_of("viewBox"), (ta_string_view){box, length});
    attribute(writer, "role", "img");
    stylesheet(writer);
    open_element(writer, "defs");
    marker(writer, "tinyactors-arrow", "ta-arrow");
    marker(writer, "tinyactors-arrow-initial", "ta-arrow ta-initial");
    ta_xml_writer_close(writer);
    open_element(writer, "rect");
    attribute(writer, "class", "ta-page");
    numeric ground[2] = {{"width", page.w}, {"height", page.h}};
    numbers(writer, ground, 2);
    return ta_xml_writer_close(writer);
}

ta_status ta_diagram_svg_close(ta_xml_writer *writer) {
    return ta_xml_writer_close(writer);
}

double ta_diagram_title_height(ta_string_view title) {
    return title.length ? title_height : 0;
}

/* ---- one chart ---------------------------------------------------------------------------- */

static void rectangle(svg *g, const char *class_name, ta_diagram_rect r, double radius) {
    open_element(g->writer, "rect");
    attribute(g->writer, "class", class_name);
    numeric a[5] = {{"x", r.x + g->dx}, {"y", r.y + g->dy}, {"width", r.w}, {"height", r.h}, {"rx", radius}};
    numbers(g->writer, a, 5);
    ta_xml_writer_close(g->writer);
}

/* Opens a <text>; its content follows, then ta_xml_writer_close. */
typedef struct text_place {
    const char *class_name;
    double x, y;
    int centred;
} text_place;
static void open_text(svg *g, text_place at) {
    open_element(g->writer, "text");
    attribute(g->writer, "class", at.class_name);
    numeric a[2] = {{"x", at.x + g->dx}, {"y", at.y + g->dy}};
    numbers(g->writer, a, 2);
    if (at.centred) attribute(g->writer, "text-anchor", "middle");
}

/* A <tspan> of `class_name` (none for NULL) holding `content`. */
static void span(svg *g, const char *class_name, ta_string_view content) {
    open_element(g->writer, "tspan");
    if (class_name) attribute(g->writer, "class", class_name);
    ta_xml_writer_text(g->writer, content);
    ta_xml_writer_close(g->writer);
}

static int same(ta_string_view a, const char *b) {
    ta_string_view other = view_of(b);
    return a.length == other.length && ta_memcmp(a.pointer, other.pointer, a.length) == 0;
}

/* The kind of an event name, as the element colours it: completions, errors, the rest. */
static const char *event_class(ta_string_view descriptor) {
    uint32_t start = 0;
    ta_string_view first = {descriptor.pointer, 0}, last = descriptor;
    for (uint32_t i = 0; i < descriptor.length; i++)
        if (descriptor.pointer[i] == '*') return 0;
    for (uint32_t i = 0; i <= descriptor.length; i++)
        if (i == descriptor.length || descriptor.pointer[i] == '.') {
            if (!first.length) first.length = i;
            last = (ta_string_view){descriptor.pointer + start, i - start};
            start = i + 1;
        }
    if (same(first, "done") || same(last, "done")) return "ta-done";
    if (same(first, "error") || same(last, "error")) return "ta-error";
    return 0;
}

/* A transition's label or chip: a pill with its events, its condition and, for a portal, its
 * targets. The pill takes the colour of a completion or an error. */
typedef struct pill {
    uint32_t transition;
    ta_diagram_rect at;
    int with_targets;
} pill;

/* the condition, cut at CONDITION_SHOWN characters, in brackets */
static void condition_span(svg *g, ta_string_view condition) {
    char cut[4 * CONDITION_SHOWN + 8];
    uint32_t length = 0, count = 0;
    cut[length++] = '[';
    int whole = characters(condition) <= CONDITION_SHOWN;
    for (uint32_t i = 0; i < condition.length; i++) {
        int starts = ((unsigned char)condition.pointer[i] & 0xC0) != 0x80;
        if (starts && !whole && ++count == CONDITION_SHOWN) break;
        cut[length++] = condition.pointer[i];
    }
    if (!whole) {
        static const char ellipsis[] = "\xE2\x80\xA6";
        for (uint32_t i = 0; ellipsis[i]; i++)
            cut[length++] = ellipsis[i];
    }
    cut[length++] = ']';
    span(g, "ta-condition", (ta_string_view){cut, length});
}

static void draw_pill(svg *g, pill p) {
    const ta_diagram_chart *chart = g->d->chart;
    const ta_diagram_transition *t = &chart->transitions[p.transition];
    const char *kind = t->events.length ? event_class(t->events) : 0;
    rectangle(g, kind ? (kind[3] == 'd' ? "ta-pill ta-done" : "ta-pill ta-error") : "ta-pill", p.at, p.at.h / 2);
    open_text(g, (text_place){"ta-event", p.at.x + 8, p.at.y + p.at.h / 2 + 4, 0});
    /* each descriptor coloured by its kind; the writer's line breaks between the spans read
     * as the spaces between the words */
    uint32_t start = 0;
    int any = 0;
    for (uint32_t i = 0; i <= t->events.length; i++) {
        if (i < t->events.length && t->events.pointer[i] != ' ') continue;
        if (i > start) {
            ta_string_view descriptor = {t->events.pointer + start, i - start};
            span(g, event_class(descriptor), descriptor);
            any = 1;
        }
        start = i + 1;
    }
    if (!any) span(g, 0, view_of("\xCE\xB5")); /* eventless */
    if (t->condition.length) condition_span(g, t->condition);
    if (p.with_targets) {
        open_element(g->writer, "tspan");
        attribute(g->writer, "class", "ta-to");
        text(g->writer, "\xE2\x86\x92 ");
        for (uint32_t i = 0; i < t->target_count; i++) {
            if (i) text(g->writer, ", ");
            ta_xml_writer_text(g->writer, chart->states[chart->targets[t->first_target + i]].name);
        }
        ta_xml_writer_close(g->writer);
    }
    ta_xml_writer_close(g->writer);
}

/* The transitions a box lists: its portals, then the first few targetless ones, in rows of
 * chips at most chips_width wide. */
static void chips(svg *g, const ta_diagram_box *box, double top) {
    const ta_diagram_layout *layout = g->d->layout;
    uint32_t first = layout->inside_first[box->state], count = layout->inside_count[box->state];
    chip_place where = {g->d->chart, g->d->portals};
    double left = box->rect.x + 11, x = left, y = top;
    uint32_t targetless = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < count; i++) {
            uint32_t t = layout->inside[first + i];
            int portal = g->d->portals && g->d->portals[t];
            if (pass == 0 ? !portal : portal) continue;
            if (!portal && targetless++ >= INTERNAL_SHOWN) continue;
            double w = chip_width(where, t);
            if (x > left && x + w > left + chips_width) {
                x = left;
                y += chip_row;
            }
            draw_pill(g, (pill){t, {x, y, w, label_height}, portal});
            x += w + 4;
        }
    if (targetless > INTERNAL_SHOWN) {
        if (x > left && x + 60 > left + chips_width) {
            x = left;
            y += chip_row;
        }
        open_text(g, (text_place){"ta-mark", x, y + 14, 0});
        number_text buffer;
        text(g->writer, "+");
        ta_xml_writer_text(g->writer, number_view(&buffer, targetless - INTERNAL_SHOWN));
        text(g->writer, " more");
        ta_xml_writer_close(g->writer);
    }
}

static void draw_box(svg *g, const ta_diagram_box *box) {
    const ta_diagram_state *state = &g->d->chart->states[box->state];
    ta_diagram_rect r = box->rect;
    double middle = r.x + r.w / 2;
    switch (box->kind) {
    case TA_DIAGRAM_BOX_ROOT: return;
    case TA_DIAGRAM_BOX_HISTORY: {
        open_element(g->writer, "circle");
        attribute(g->writer, "class", "ta-history");
        numeric a[3] = {{"cx", middle + g->dx}, {"cy", r.y + r.h / 2 + g->dy}, {"r", r.w / 2}};
        numbers(g->writer, a, 3);
        ta_xml_writer_close(g->writer);
        open_text(g, (text_place){"ta-mark", middle, r.y + r.h / 2 + 4, 1});
        text(g->writer, state->kind == TA_DIAGRAM_STATE_HISTORY_DEEP ? "H*" : "H");
        ta_xml_writer_close(g->writer);
        return;
    }
    case TA_DIAGRAM_BOX_FINAL: /* a double border, pill-shaped */
        rectangle(g, "ta-leaf", r, ta_diagram_min(r.h, r.w) / 2);
        rectangle(g, "ta-leaf", (ta_diagram_rect){r.x + 2.5, r.y + 2.5, r.w - 5, r.h - 5}, ta_diagram_min(r.h, r.w) / 2 - 2.5);
        break;
    case TA_DIAGRAM_BOX_ATOMIC: rectangle(g, "ta-leaf", r, 7); break;
    case TA_DIAGRAM_BOX_REGION: rectangle(g, "ta-region", r, 4); break;
    default: rectangle(g, box->depth % 2 ? "ta-container" : "ta-container ta-even", r, 7); break;
    }
    int leaf = box->header == 0;
    double name_top = leaf ? 8 : 7;
    int centred = box->kind == TA_DIAGRAM_BOX_FINAL && !g->d->layout->inside_count[box->state];
    open_text(g, (text_place){"ta-name", centred ? middle : r.x + 11, r.y + name_top + 12, centred});
    ta_xml_writer_text(g->writer, state->name);
    if (state->kind == TA_DIAGRAM_STATE_PARALLEL) span(g, "ta-kind", view_of("\xE2\x88\xA5"));
    ta_xml_writer_close(g->writer);
    chips(g, box, r.y + name_top + 16 + (leaf ? 5 : 4));
}

/* An orthogonal path through `count` points of the layout, with an arrow at its end: its data
 * ("M x,y L x,y ...") made in the arena, above a mark released once it is written. */
typedef struct path_place {
    const char *class_name, *marker;
    uint32_t first, count;
} path_place;
static void path(svg *g, path_place at) {
    const ta_diagram_point *p = &g->d->layout->points[at.first];
    ta_growable_arena_mark mark = ta_growable_arena_position(g->d->arena);
    ta_allocator allocator = ta_growable_arena_allocator(g->d->arena);
    ta_output data = ta_output_over((ta_buffer){0, 0, 0}, &allocator);
    for (uint32_t i = 0; i < at.count; i++) {
        number_text x, y;
        ta_string_view xs = number_view(&x, p[i].x + g->dx), ys = number_view(&y, p[i].y + g->dy);
        ta_output_write(&data, i ? " L" : "M", i ? 2 : 1);
        ta_output_write(&data, xs.pointer, xs.length);
        ta_output_write(&data, ",", 1);
        ta_output_write(&data, ys.pointer, ys.length);
    }
    open_element(g->writer, "path");
    attribute(g->writer, "class", at.class_name);
    if (ta_output_fits(&data))
        ta_xml_writer_attribute(g->writer, view_of("d"), (ta_string_view){(const char *)data.bytes, (uint32_t)data.length});
    else
        g->status = TA_ERROR_NO_MEMORY;
    attribute(g->writer, "marker-end", at.marker);
    ta_xml_writer_close(g->writer);
    ta_growable_arena_release(g->d->arena, mark);
}

ta_status ta_diagram_svg(ta_xml_writer *writer, const ta_diagram_drawing *d) {
    svg g = {writer, d, page_margin, d->top + ta_diagram_title_height(d->title), TA_OK};
    const ta_diagram_layout *layout = d->layout;
    open_element(writer, "g");
    if (d->title.length) {
        open_text(&g, (text_place){"ta-title", 0, -10, 0});
        ta_xml_writer_text(writer, d->title);
        ta_xml_writer_close(writer);
    }
    for (uint32_t i = 0; i < layout->box_count; i++)
        draw_box(&g, &layout->boxes[i]);
    for (uint32_t i = 0; i < layout->edge_count; i++)
        path(&g, (path_place){"ta-edge", "url(#tinyactors-arrow)", layout->edges[i].first_point, layout->edges[i].point_count});
    for (uint32_t i = 0; i < layout->initial_count; i++) {
        const ta_diagram_initial *m = &layout->initials[i];
        path(&g, (path_place){"ta-initial-line", "url(#tinyactors-arrow-initial)", m->first_point, m->point_count});
        open_element(writer, "circle");
        attribute(writer, "class", "ta-dot");
        numeric a[3] = {{"cx", m->at.x + g.dx}, {"cy", m->at.y + g.dy}, {"r", 4.5}};
        numbers(writer, a, 3);
        ta_xml_writer_close(writer);
    }
    for (uint32_t i = 0; i < layout->edge_count; i++)
        if (layout->edges[i].labelled) draw_pill(&g, (pill){layout->edges[i].transition, layout->edges[i].label, 0});
    ta_status status = ta_xml_writer_close(writer);
    return status != TA_OK ? status : g.status;
}
