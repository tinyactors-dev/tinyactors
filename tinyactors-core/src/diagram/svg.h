/* diagram/svg.h — a chart's layout drawn as SVG, as scxmljs's
 * <scxml-view> draws it (diagram.h has the layout).
 *
 * The drawing is written through a ta_xml_writer (<tinyactors/compile.h>),
 * which escapes every attribute and text and checks the nesting, and its
 * stylesheet is data: the theme's colours as tokens, light and dark, and
 * rules of properties and values, written as the <style> element's text.
 * A page is one <svg>: ta_diagram_svg_open, then ta_diagram_svg for each
 * chart, then ta_diagram_svg_close. Each returns the writer's status, which
 * stays the first error. */
#ifndef TINYACTORS_DIAGRAM_SVG_H
#define TINYACTORS_DIAGRAM_SVG_H

#include "diagram/diagram.h"

#include <tinyactors/compile.h>

/* The sizes <scxml-view> estimates for its boxes and labels, for text in
 * its fonts (13px names, 11px monospace labels), less its buttons: what
 * ta_diagram_svg draws. TA_ERROR_NO_MEMORY when the arena could not grow. */
typedef struct ta_diagram_measure_request {
    const ta_diagram_chart *chart;
    const uint8_t *portals; /* NULL for none */
    ta_growable_arena *arena;
} ta_diagram_measure_request;
typedef struct ta_diagram_measures {
    ta_diagram_size *states;
    ta_diagram_size *labels;
} ta_diagram_measures;
ta_status ta_diagram_measure(const ta_diagram_measure_request *request, ta_diagram_measures *out);

/* One chart on the page: its layout, its title, and where it starts. */
typedef struct ta_diagram_drawing {
    const ta_diagram_chart *chart;
    const ta_diagram_layout *layout;
    const uint8_t *portals;
    ta_string_view title; /* above the chart; empty for none */
    double top;
    ta_growable_arena *arena; /* for each path's data while it is written */
} ta_diagram_drawing;

/* The page: <svg> of that size, its stylesheet, its markers, its ground. */
ta_status ta_diagram_svg_open(ta_xml_writer *writer, ta_diagram_size page);
/* One chart, as a <g>. */
ta_status ta_diagram_svg(ta_xml_writer *writer, const ta_diagram_drawing *drawing);
ta_status ta_diagram_svg_close(ta_xml_writer *writer);
/* The height of a chart's title band (0 without one). */
double ta_diagram_title_height(ta_string_view title);

#endif
