/*
 * Private: the document's columns and the operations that build them.
 * Layout is specified in docs/internal/document-layout.md.
 *
 * Nothing in a column is a pointer; every reference is a uint32_t index or
 * pool offset. The parser appends through the functions below and the cursor
 * module reads through the accessors.
 */
#pragma once

#include "../include/txml.h"

/* One node, 24 bytes. `end` is the only field ever rewritten. */
typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t depth;
    uint32_t parent;
    uint32_t end;
    uint32_t name;
    union {
        struct { uint32_t first_attribute, attribute_count; } element;
        struct { uint32_t offset, length; } text;
    } as;
} txml_node;

/* One attribute, 12 bytes. Two bits of `name` are flags: the top bit marks the
 * last attribute of its element, which is how txml_next_attribute knows where
 * to stop without the attribute cursor carrying a bound; the next marks a
 * value supplied by an ATTLIST default. Name IDs stay below 2^30. */
typedef struct {
    uint32_t name;
    uint32_t value_offset;
    uint32_t value_length;
} txml_attribute_entry;

#define TXML_ATTRIBUTE_LAST ((uint32_t)0x80000000u)
#define TXML_ATTRIBUTE_DEFAULTED ((uint32_t)0x40000000u)
#define TXML_ATTRIBUTE_NAME_MASK ((uint32_t)0x3FFFFFFFu)
#define TXML_NAME_ID_LIMIT ((uint32_t)0x40000000u)

typedef struct {
    uint32_t namespace_id;
    uint32_t local_offset;
    uint32_t local_length;
    uint32_t prefix_offset;
    uint32_t prefix_length;
    uint32_t hash;
} txml_name_entry;

typedef struct {
    uint32_t uri_offset;
    uint32_t uri_length;
} txml_namespace_entry;

/* A prefix that differs from the one its name entry recorded first. Names are
 * identified by (namespace, local), so `p:b` and `q:b` share a name ID; the
 * name entry keeps the first prefix and every later spelling that differs is
 * one of these, keyed by node or attribute index (ADR 0020). */
typedef struct {
    uint32_t index;
    uint32_t offset;
    uint32_t length;
} txml_prefix_entry;

/* A paged vector: a directory of page pointers allocated once, pages allocated
 * lazily from the owning arena. Entries never move. */
typedef struct {
    void **directory;
    uint32_t directory_capacity;
    uint32_t count;
    uint32_t entry_size;
} txml_column;

struct txml_doc {
    txml_allocator allocator; /* the doc itself came from here */
    txml_limits limits;
    txml_arena values;
    txml_arena tree;

    txml_column nodes;      /* tree */
    txml_column attributes; /* tree */
    txml_column positions;  /* tree */
    txml_column node_prefixes;      /* tree; sorted by node index */
    txml_column attribute_prefixes; /* tree; sorted by attribute index */
    txml_column names;      /* values */
    txml_column namespaces; /* values */

    /* The pool: bytes, addressed by global offset, paged over `values`. */
    unsigned char **pool_directory;
    uint32_t pool_directory_capacity;
    uint32_t pool_shift;
    uint32_t pool_length;

    /* Name index: open addressing over name IDs; rebuilt on growth. */
    uint32_t *name_slots; /* TXML_NAME_NONE marks an empty slot */
    uint32_t name_slot_capacity;

    bool complete; /* the parse is over: nothing is open, nothing is pending */
};

/* Create an empty document: node 0 (the open document node) and namespace 0
 * (the empty URI) exist. Uses config->allocator, ->limits and ->chunk_size. */
txml_result txml_doc_create(txml_doc **out, const txml_parser_config *config);

/* Append a node and its position entry; the node's `end` and `flags` are the
 * caller's to set (leaves and empty elements already closed, others OPEN). */
typedef struct {
    txml_node node;
    txml_position position;
} txml_node_append;
txml_result txml_doc_append_node(txml_doc *doc, const txml_node_append *append, uint32_t *index);
/* Set `end` on an open node and clear TXML_NODE_OPEN. */
void txml_doc_close_node(txml_doc *doc, uint32_t index, uint32_t end);
/* Mark the parse finished: closes the document node and stops PENDING. */
void txml_doc_complete(txml_doc *doc);

/* Append one attribute. The caller ORs TXML_ATTRIBUTE_LAST and
 * TXML_ATTRIBUTE_DEFAULTED into `entry->name`. */
txml_result txml_doc_append_attribute(txml_doc *doc, const txml_attribute_entry *entry, uint32_t *index);

/* Copy bytes into the pool; the run never spans a page unless it is larger
 * than one, in which case it occupies consecutive whole pages. */
txml_result txml_doc_pool_append(txml_doc *doc, txml_str bytes, uint32_t *offset);

txml_result txml_doc_intern_namespace(txml_doc *doc, txml_str uri, uint32_t *id);
typedef struct {
    uint32_t namespace_id;
    txml_str local;
    txml_str prefix;
} txml_intern_params;
txml_result txml_doc_intern_name(txml_doc *doc, const txml_intern_params *params, uint32_t *id);

/* Read-side accessors; NULL / empty for anything out of range. */
const txml_node *txml_doc_node(const txml_doc *doc, uint32_t index);
const txml_attribute_entry *txml_doc_attribute(const txml_doc *doc, uint32_t index);
const txml_name_entry *txml_doc_name(const txml_doc *doc, uint32_t id);
txml_str txml_doc_pool_str(const txml_doc *doc, uint32_t offset, uint32_t length);
const txml_position *txml_doc_position(const txml_doc *doc, uint32_t index);
/* TXML_ATTR_NS_DECL is derived from the name's namespace, TXML_ATTR_DEFAULTED
 * from the entry's flag bit. */
uint32_t txml_doc_attribute_flags(const txml_doc *doc, const txml_attribute_entry *entry);
const txml_namespace_entry *txml_doc_namespace(const txml_doc *doc, uint32_t id);

typedef struct {
    bool attribute; /* an attribute index, not a node index */
    uint32_t index;
    txml_str prefix; /* as written */
} txml_prefix_request;

/* Record the prefix a node or attribute was written with, after it has been
 * appended. Costs nothing when it matches the name entry's prefix. */
txml_result txml_doc_record_prefix(txml_doc *doc, const txml_prefix_request *request);
/* The prefix a node (or attribute) was written with; empty when none. */
txml_str txml_doc_prefix_of(const txml_doc *doc, bool attribute, uint32_t index);

/*
 * The run builder: text whose length is not known when it starts (a text node,
 * a comment, an attribute value) is written into the pool as it arrives.
 *
 * A run is contiguous. When it outgrows its block it moves to a bigger one,
 * copying what it holds so far and abandoning the old space, which is why a
 * run is invisible until finished: nothing a reader can hold points into it.
 * At most one run is pending at a time, and nothing else may append to the
 * pool while it is; `finish` is what makes the bytes part of the pool.
 */
typedef struct {
    uint32_t offset;   /* pool offset of the first byte; may move while pending */
    uint32_t length;   /* bytes written */
    uint32_t capacity; /* contiguous bytes available from `offset`; 0 until placed */
} txml_run;

void txml_doc_run_begin(txml_doc *doc, txml_run *run);
/* Make room for `extra` more bytes, so a following append cannot fail. */
txml_result txml_doc_run_reserve(txml_doc *doc, txml_run *run, uint32_t extra);
txml_result txml_doc_run_append(txml_doc *doc, txml_run *run, txml_str bytes);
/* The bytes so far, valid until the next append. */
txml_str txml_doc_run_view(const txml_doc *doc, const txml_run *run);
/* Commit the run to the pool. Idempotent. */
void txml_doc_run_finish(txml_doc *doc, const txml_run *run);
