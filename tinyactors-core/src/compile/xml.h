/* xml.h — minimal non-validating XML parser producing a DOM in an arena.
 *
 * Supports elements, attributes, namespaces (prefix resolution), text,
 * CDATA, comments, PIs, a skipped DOCTYPE, the five predefined entities and
 * numeric character references. No DTD processing, no external entities. */
#ifndef TINYACTORS_COMPILE_XML_H
#define TINYACTORS_COMPILE_XML_H

#include "compile/compile_support.h"

typedef struct ta_xml_attribute {
    const char *name; /* qualified name, NUL-terminated */
    const char *value;
    uint32_t value_length;
    struct ta_xml_attribute *next;
} ta_xml_attribute;

typedef struct ta_xml_node {
    const char *name;          /* local name */
    const char *prefix;        /* NULL if none */
    const char *namespace_uri; /* resolved namespace URI ("" if none) */
    ta_xml_attribute *attributes;
    struct ta_xml_node *parent, *first, *last, *next;
    char *text; /* unescaped direct text + CDATA, NUL-terminated */
    uint32_t text_length;
    uint32_t inner_start, inner_end; /* source span of inner content */
    uint32_t line, column;
} ta_xml_node;

/* Linear in the document size; native stack use is constant. Polls
 * *abort_flag (if not NULL) per markup token and every 64 KiB within one;
 * once it is set, sets *aborted
 * and fails without a diagnostic. */
typedef struct ta_xml_parse_request {
    ta_string_view source;
    ta_diagnostics *diagnostics;
    ta_allocator *allocator; /* backing allocator for temporary vectors and maps */
    const char *file;
    const volatile int *abort_flag;
    int *aborted;
    int *out_of_memory;
} ta_xml_parse_request;

ta_xml_node *ta_xml_parse(ta_growable_arena *arena, const ta_xml_parse_request *request);
const ta_xml_attribute *ta_xml_find_attribute(const ta_xml_node *n, const char *name);
const char *ta_xml_attribute_value(const ta_xml_node *n, const char *name); /* NULL if absent */

#endif /* TINYACTORS_COMPILE_XML_H */
