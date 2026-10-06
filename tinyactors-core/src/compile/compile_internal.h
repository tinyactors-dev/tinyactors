/* compile_internal.h — compiler internals shared by the compile units. */
#ifndef TINYACTORS_COMPILE_COMPILE_INTERNAL_H
#define TINYACTORS_COMPILE_COMPILE_INTERNAL_H

#include "compile/compile_support.h"
#include "image/records.h"
#include "compile/xml.h"

#define TA_SCXML_NAMESPACE "http://www.w3.org/2005/07/scxml"

typedef struct ta_pending_document {
    ta_xml_node *root; /* <scxml> element (NULL until parsed) */
    const char *name;  /* source name */
    const char *text;  /* source text (arena) */
    uint32_t length;
} ta_pending_document;

typedef struct ta_compiler {
    ta_allocator *allocator;
    ta_chunk_arena arena;
    ta_diagnostics diagnostics;
    const ta_compile_options *options;

    ta_vector strings;        /* bytes */
    ta_string_map string_map; /* string -> string id */
    ta_vector charts;         /* ta_chart_record */
    ta_vector states;         /* ta_state_record */
    ta_vector transitions;    /* ta_transition_record */
    ta_vector u32;            /* uint32_t */
    ta_vector code;           /* uint32_t */
    ta_vector expressions;    /* ta_expression_record */
    ta_vector captures;       /* ta_capture_record */
    ta_vector sends;          /* ta_send_record */
    ta_vector invokes;        /* ta_invoke_record */
    ta_vector data;           /* ta_data_record */
    ta_vector actors;         /* ta_actor_record */

    ta_vector event_names; /* ta_string_view: event names (pre-trie) */
    ta_string_map event_name_map;
    ta_vector event_nodes;     /* ta_event_node_record (built at the end) */
    ta_vector event_name_node; /* uint32_t: event name index -> trie node */

    ta_vector pending;        /* ta_pending_document, index == chart index */
    ta_string_map source_map; /* source name -> chart index */

    const volatile int *abort_flag; /* ta_compile_options.abort_flag (NULL: none) */
    int aborted;                    /* latched once the flag was seen set */
} ta_compiler;

/* Poll the host's abort flag; once set, every phase returns early. */
TA_INLINE int ta_compiler_stop(ta_compiler *compiler) {
    if (!compiler->aborted && compiler->abort_flag && *compiler->abort_flag) compiler->aborted = 1;
    return compiler->aborted;
}

uint32_t ta_compiler_string(ta_compiler *compiler, const char *s, uint32_t length);
uint32_t ta_compiler_event_name(ta_compiler *compiler, const char *s, uint32_t length);
/* Queue a chart for compilation (dedup by name); returns its chart index. */
uint32_t ta_compiler_queue(ta_compiler *compiler, const char *name, const char *text,
                           uint32_t length, ta_xml_node *root);

/* Compile pending chart `index` (appends to the global tables). */
void ta_compiler_compile_chart(ta_compiler *compiler, uint32_t index);
/* Build the event trie and patch name references. */
void ta_compiler_link_names(ta_compiler *compiler);
/* Serialize to an image buffer allocated with compiler->a. */
int ta_compiler_emit(ta_compiler *compiler, void **out, size_t *out_length);

int ta_parse_duration_ns(const char *s, uint64_t *out);
uint32_t ta_compiler_document_key(const char *text, uint32_t length, char *out, uint32_t capacity);

#endif /* TINYACTORS_COMPILE_COMPILE_INTERNAL_H */
