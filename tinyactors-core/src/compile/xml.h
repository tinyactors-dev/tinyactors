/* xml.h — minimal non-validating XML parser producing a DOM in an arena.
 *
 * Supports elements, attributes, namespaces (prefix resolution), text,
 * CDATA, comments, PIs, a skipped DOCTYPE, the five predefined entities and
 * numeric character references. No DTD processing, no external entities.
 *
 * The DOM the compiler reads has the elements, their attributes and their
 * text. A caller that writes the document again (tinyactors fmt) asks for
 * its content too: every piece of each element's content in document order
 * (text, CDATA, comments, processing instructions, elements) and the
 * document's own (the XML declaration, comments, the root), each as it is
 * written in the source. */
#ifndef TINYACTORS_COMPILE_XML_H
#define TINYACTORS_COMPILE_XML_H

#include "compile/compile_support.h"

typedef struct ta_xml_attribute {
    const char *name; /* qualified name, NUL-terminated */
    const char *value;
    uint32_t value_length;
    ta_string_view markup; /* the value as written between its quotes: a view of the source */
    struct ta_xml_attribute *next;
} ta_xml_attribute;

typedef enum ta_xml_content_kind {
    TA_XML_CONTENT_ELEMENT = 0,
    TA_XML_CONTENT_TEXT = 1,        /* character data, its references as written */
    TA_XML_CONTENT_CDATA = 2,       /* <![CDATA[...]]> */
    TA_XML_CONTENT_COMMENT = 3,     /* <!--...--> */
    TA_XML_CONTENT_INSTRUCTION = 4, /* <?target ...?>: a processing instruction or the XML declaration */
    TA_XML_CONTENT_DECLARATION = 5  /* <!...>: a document type declaration */
} ta_xml_content_kind;

/* One piece of content. Text between two pieces of markup is one piece;
 * whitespace outside the root element is none. */
typedef struct ta_xml_content {
    uint32_t kind;                /* ta_xml_content_kind */
    ta_string_view markup;        /* the piece as written, its delimiters included: a view of the
                                     source (an element: from its start tag through its end tag) */
    struct ta_xml_node *element;  /* TA_XML_CONTENT_ELEMENT */
    struct ta_xml_content *next;
} ta_xml_content;

typedef struct ta_xml_node {
    const char *name;          /* local name */
    const char *prefix;        /* NULL if none */
    const char *namespace_uri; /* resolved namespace URI ("" if none) */
    ta_xml_attribute *attributes;
    struct ta_xml_node *parent, *first, *last, *next;
    ta_xml_content *content; /* in document order, when the parse request asks for it; else NULL */
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
    /* Not NULL: keep the content (ta_xml_node.content), and the document's
     * own pieces here: what comes before the root, the root, what follows. */
    ta_xml_content **document;
} ta_xml_parse_request;

ta_xml_node *ta_xml_parse(ta_growable_arena *arena, const ta_xml_parse_request *request);
const ta_xml_attribute *ta_xml_find_attribute(const ta_xml_node *n, const char *name);
const char *ta_xml_attribute_value(const ta_xml_node *n, const char *name); /* NULL if absent */

#endif /* TINYACTORS_COMPILE_XML_H */
