/* cli/report.h — what a command found, as one document written three ways.
 *
 * A command builds its answer once, as a tree: objects with named members,
 * lists of items, and scalars (text, numbers, booleans), plus blocks of
 * preformatted text (a diagram, a listing). ta_report_render writes the
 * tree as Markdown for people, or as JSON or XML for programs; the three
 * carry the same members in the same order, so a script reading JSON sees
 * what a person reads in Markdown.
 *
 *   Markdown  the root's title as a heading; an object's scalars as a list
 *             of "**Name**: value" lines; an object member as a heading one
 *             level down; a list of flat objects (scalars only) as a table,
 *             its columns the members in the order first seen; any other
 *             list item under a heading of its own; a list of scalars as a
 *             bullet list; a block as a fenced code block in its language;
 *             an empty list as "_none_". Names are shown as words:
 *             "bootActors" is "Boot actors".
 *   JSON      the root is an object; members in order; numbers as
 *             integers; text escaped (control characters as \u00XX, UTF-8
 *             passed through); a block as a string. Two spaces of indent,
 *             a newline at the end.
 *   XML       <?xml version="1.0" encoding="UTF-8"?>, then the root as an
 *             element named by its title; an object's scalars as its
 *             attributes, its object, list and block members as child
 *             elements in order; a list's items as elements named by the
 *             list's item name; a scalar item as an element with its text;
 *             a block as an element with its text. Text and attributes are
 *             escaped (&amp; &lt; &gt; &quot; and control characters as
 *             character references). Two spaces of indent.
 *
 * Names are camelCase words of ASCII letters and digits, starting with a
 * lowercase letter: valid as JSON keys and XML names alike. The builder
 * keeps a stack of open objects and lists; a member written while a list
 * is open is one of its items, and its name is ignored.
 *
 * Freestanding: the tree lives in the arena given to ta_report_init, and
 * nothing is copied that the caller keeps alive until the render (names,
 * texts and blocks are borrowed views); ta_report_copy copies a text into
 * the arena for a caller whose text is short-lived. A builder call after a
 * failure does nothing; the render then returns the failure. */
#ifndef TINYACTORS_CLI_REPORT_H
#define TINYACTORS_CLI_REPORT_H

#include "base/output.h"
#include <tinyactors/arena.h>
#include <tinyactors/types.h>

typedef enum ta_report_format {
    TA_REPORT_MARKDOWN = 0,
    TA_REPORT_JSON = 1,
    TA_REPORT_XML = 2
} ta_report_format;

typedef struct ta_report_node ta_report_node;

typedef enum ta_report_limits {
    TA_REPORT_MAX_DEPTH = 64 /* objects and lists open at once */
} ta_report_limits;

typedef struct ta_report {
    ta_growable_arena *arena;
    ta_report_node *root;
    ta_report_node *open[TA_REPORT_MAX_DEPTH]; /* open[depth - 1] is where members go */
    uint32_t depth;
    ta_status status; /* the first failure: TA_ERROR_NO_MEMORY, or TA_ERROR_STATE for a misuse */
} ta_report;

/* A preformatted block: a diagram, a listing. */
typedef struct ta_report_code {
    ta_string_view language; /* Markdown's fence ("mermaid", "text"); empty: none */
    ta_string_view text;
} ta_report_code;

/* Start a document whose root object is titled `title` (a name: the XML
 * root element, the Markdown heading). */
void ta_report_init(ta_report *report, ta_growable_arena *arena, ta_string_view title);

/* Open an object (a member named `name`, or an item of the open list). */
void ta_report_object(ta_report *report, ta_string_view name);
/* Open a list named `name` whose items are named `item`. */
typedef struct ta_report_list_names {
    ta_string_view name, item;
} ta_report_list_names;
void ta_report_list(ta_report *report, ta_report_list_names names);
/* Close the innermost open object or list (TA_ERROR_STATE when only the
 * root is open). */
void ta_report_end(ta_report *report);

void ta_report_text(ta_report *report, ta_string_view name, ta_string_view value);
void ta_report_number(ta_report *report, ta_string_view name, uint64_t value);
void ta_report_boolean(ta_report *report, ta_string_view name, int value);
void ta_report_block(ta_report *report, ta_string_view name, ta_report_code code);
/* A value: one CBOR item (a neutral value, docs/protocol.md "Values", or
 * any well-formed item), borrowed until the render. Markdown writes it in
 * CBOR's diagnostic notation, which reads as JSON for JSON's values
 * (null, undefined, h'00ff' for bytes, 32("...") for a tag), as code; JSON
 * as the JSON value, with what JSON has no value for as an object: bytes
 * {"bytes": "00ff"}, undefined {"undefined": true}, another simple value
 * {"simple": 16}, a tag {"tag": 32, "value": ...}, a float JSON cannot
 * write ({"float": "NaN"}); XML as typed elements inside the member's:
 * <map> of <entry key="..."> (a key that is an integer as its digits),
 * <array>, <text>, <integer>, <float>, <bytes> (hexadecimal), <true/>,
 * <false/>, <null/>, <undefined/>, <simple>, <tag number="...">. A map
 * with a key that is neither text nor an integer is written, in JSON and
 * XML, as its diagnostic notation: {"diagnostic": "..."}, <diagnostic>.
 * A value that is not one well-formed item (ta_cbor_decode refuses it)
 * fails the report: TA_ERROR_INVALID. Markdown and JSON write it on one
 * line. In XML it is a child element, never an attribute. */
void ta_report_value(ta_report *report, ta_string_view name, ta_bytes cbor);

/* A copy of `text` in the report's arena (an empty view when it refused,
 * and the report's status is set). */
ta_string_view ta_report_copy(ta_report *report, ta_string_view text);

/* Write the document in `format` (ta_report_format) to `out`. The objects
 * and lists still open are closed first. TA_ERROR_NO_MEMORY or
 * TA_ERROR_STATE: the builder's failure, and nothing is written;
 * TA_ERROR_INVALID: an unknown format. */
ta_status ta_report_render(ta_report *report, uint32_t format, ta_output *out);

#endif /* TINYACTORS_CLI_REPORT_H */
