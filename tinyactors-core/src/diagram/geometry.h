/* diagram/geometry.h — the arithmetic the diagram's files share: freestanding, so no libm. */
#ifndef TINYACTORS_DIAGRAM_GEOMETRY_H
#define TINYACTORS_DIAGRAM_GEOMETRY_H

#include "diagram/diagram.h"

static inline double ta_diagram_max(double a, double b) {
    return a > b ? a : b;
}
static inline double ta_diagram_min(double a, double b) {
    return a < b ? a : b;
}
static inline double ta_diagram_abs(double value) {
    return value < 0 ? -value : value;
}
/* the smallest whole number not below `value`, for the small positive numbers a layout counts */
static inline double ta_diagram_ceil(double value) {
    double whole = (double)(int64_t)value;
    return whole < value ? whole + 1 : whole;
}
/* the square root of a non-negative number, by Newton's method */
static inline double ta_diagram_root(double value) {
    if (value <= 0) return 0;
    double guess = value > 1 ? value : 1;
    for (int i = 0; i < 64; i++) {
        double next = (guess + value / guess) / 2;
        if (ta_diagram_abs(next - guess) < 1e-9) break;
        guess = next;
    }
    return guess;
}

/* A box's drawn outline: a circle for a history state, a pill (its radius half its shorter
 * side) for a final, else its rectangle. ta_diagram_svg draws them so; ta_diagram_check
 * measures against them; the layout ends routes on them. */
typedef enum ta_diagram_outline {
    TA_DIAGRAM_OUTLINE_RECTANGLE = 0,
    TA_DIAGRAM_OUTLINE_CIRCLE = 1,
    TA_DIAGRAM_OUTLINE_PILL = 2
} ta_diagram_outline;

static inline uint32_t ta_diagram_outline_of(uint32_t box_kind) {
    return box_kind == TA_DIAGRAM_BOX_HISTORY ? TA_DIAGRAM_OUTLINE_CIRCLE
           : box_kind == TA_DIAGRAM_BOX_FINAL ? TA_DIAGRAM_OUTLINE_PILL
                                              : TA_DIAGRAM_OUTLINE_RECTANGLE;
}

/* A pill's spine: the segment its outline is `radius` away from. */
typedef struct ta_diagram_spine {
    ta_diagram_point a, b;
    double radius;
} ta_diagram_spine;

static inline ta_diagram_spine ta_diagram_spine_of(ta_diagram_rect r, uint32_t outline) {
    double radius = ta_diagram_min(r.w, r.h) / 2;
    if (outline == TA_DIAGRAM_OUTLINE_CIRCLE)
        return (ta_diagram_spine){{r.x + r.w / 2, r.y + r.h / 2}, {r.x + r.w / 2, r.y + r.h / 2}, r.w / 2};
    if (r.w >= r.h)
        return (ta_diagram_spine){{r.x + radius, r.y + r.h / 2}, {r.x + r.w - radius, r.y + r.h / 2}, radius};
    return (ta_diagram_spine){{r.x + r.w / 2, r.y + radius}, {r.x + r.w / 2, r.y + r.h - radius}, radius};
}

#endif
