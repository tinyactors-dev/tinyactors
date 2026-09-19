/*
 * txml public API.
 *
 * This file is the complete, authored public interface of txml: every type,
 * constant, and function an embedder or binding may use is declared here, and
 * nothing else is. Implementation modules under src/ include this header and
 * add private declarations.
 *
 * txml is a freestanding XML reader and writer. It performs no I/O, allocates
 * only through an embedder-supplied allocator, never relocates what it has
 * allocated, and never recurses. A parsed document is a set of index-addressed
 * columns in one arena; callers navigate it with cursors, which are values.
 *
 * Every public function is declared with TXML_API. Define TXML_API before
 * including this header to add export attributes, e.g.
 * __attribute__((visibility("default"))) or __declspec(dllexport).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TXML_API
#define TXML_API
#endif

#define TXML_VERSION_MAJOR 0
#define TXML_VERSION_MINOR 1
#define TXML_VERSION_PATCH 0

/*
 * API annotations.
 *
 * These macros record what C types cannot say about the API: which pointers
 * are single elements, lists, or caller buffers; which may be null; which
 * outputs are valid after which results and who releases them; and how long
 * borrowed data stays valid. They expand to nothing unless TXML_API_EXTRACT is
 * defined, so they never change the compiled API.
 */
#ifdef TXML_API_EXTRACT
#define TXML_ANNOTATION __attribute__((annotate("txml")))
#else
#define TXML_ANNOTATION
#endif

/* Types. */
#define TXML_HANDLE TXML_ANNOTATION                   /* opaque object owned through a handle */
#define TXML_VIEW TXML_ANNOTATION                     /* opaque object borrowed from the library */
#define TXML_STRING TXML_ANNOTATION                   /* {ptr, length} text */
#define TXML_BYTES TXML_ANNOTATION                    /* {ptr, length} bytes */
#define TXML_ADAPTER TXML_ANNOTATION                  /* {self, v} implemented by the embedder */
#define TXML_VTABLE TXML_ANNOTATION                   /* the operations of an adapter */
#define TXML_VALUE TXML_ANNOTATION                    /* copyable value; no ownership */
#define TXML_TAGGED(tag, union_) TXML_ANNOTATION      /* field `tag` selects the member of `union_` */
#define TXML_SYMBOLS(prefix) TXML_ANNOTATION          /* enum symbols: names without `prefix`, lowercase */
#define TXML_CONSTANTS_OF(type, prefix) TXML_ANNOTATION /* anonymous enum: values of integer type `type` */
#define TXML_FLAG_SET(name, prefix) TXML_ANNOTATION   /* anonymous enum: a named set of bit flags */
#define TXML_SUCCESS TXML_ANNOTATION                  /* enum constant: the result that means success */

/* Members. */
#define TXML_WHEN(constant) TXML_ANNOTATION           /* union member selected by `constant` */
#define TXML_FLAGS(set) TXML_ANNOTATION               /* integer holding flags of `set` */

/* Pointers: exactly one shape. */
#define TXML_IN TXML_ANNOTATION                       /* one element the receiver reads */
#define TXML_OUT TXML_ANNOTATION                      /* one element the callee writes */
#define TXML_INOUT TXML_ANNOTATION                    /* one element the callee reads and writes */
#define TXML_BUFFER(capacity) TXML_ANNOTATION         /* caller array of `capacity` bytes */

/* Pointer facts. */
#define TXML_OPTIONAL TXML_ANNOTATION                 /* may be null; null means absent */
#define TXML_VALID_ON(constant) TXML_ANNOTATION       /* output valid only after result `constant` */
#define TXML_RELEASE(function) TXML_ANNOTATION        /* the receiver releases the output with `function` */
#define TXML_LIFETIME(scope) TXML_ANNOTATION          /* borrowed data stays valid for `scope` */
#define TXML_DESTROYS TXML_ANNOTATION                 /* the function destroys this handle */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Results, text, and positions
 */

typedef int32_t txml_result;

enum TXML_CONSTANTS_OF(txml_result, "TXML_") {
    /* The call succeeded. */
    TXML_OK TXML_SUCCESS = 0,
    /* Writer only: the output buffer is full. Drain it and continue. */
    TXML_NEED_SPACE = 1,
    /* API misuse: a null handle, a cursor from another document, a call in
     * the wrong order. Never caused by document content. */
    TXML_ERR_INVALID = -1,
    /* The backing allocator refused a request. */
    TXML_ERR_NOMEM = -2,
    /* A txml_limits value was reached. */
    TXML_ERR_LIMIT = -3,
    /* The requested name, attribute, or node does not exist. */
    TXML_ERR_NOT_FOUND = -4,
    /* The answer depends on input that has not been parsed yet. */
    TXML_ERR_PENDING = -5,
    /* The document is not well-formed; read txml_parser_error() for details. */
    TXML_ERR_DOCUMENT = -6
};

/*
 * Text and bytes. Both are borrowed views: the `ptr`/`length` pair never owns
 * its storage. Lengths are byte counts, never character counts.
 */
typedef struct { const char *ptr; uint32_t length; } txml_str TXML_STRING;
typedef struct { const uint8_t *ptr; uint32_t length; } txml_bytes TXML_BYTES;
typedef struct { uint8_t *ptr; uint32_t capacity; } txml_buffer;

/* A string literal as a txml_str. The trailing NUL is not part of the text. */
#define TXML_STR(literal) ((txml_str){ (literal), (uint32_t)(sizeof(literal) - 1u) })

/* An expanded name: the namespace URI (empty when there is none) and the
 * local part. Prefixes are not part of a name's identity. */
typedef struct { txml_str ns; txml_str local; } txml_qname TXML_VALUE;

/* A location in the input. `offset` counts bytes from the first byte fed to
 * the parser; `line` and `column` count from 1, and `column` counts bytes. */
typedef struct {
    uint64_t offset;
    uint32_t line;
    uint32_t column;
} txml_position TXML_VALUE;

typedef enum {
    TXML_DOC_OK = 0,
    TXML_DOC_ENCODING,           /* not well-formed UTF-8, or a forbidden character */
    TXML_DOC_SYNTAX,             /* malformed markup */
    TXML_DOC_NAME,               /* not a legal XML name */
    TXML_DOC_TAG_MISMATCH,       /* end tag does not match the open element */
    TXML_DOC_DUPLICATE_ATTRIBUTE,
    TXML_DOC_UNDECLARED_PREFIX,
    TXML_DOC_UNDECLARED_ENTITY,
    TXML_DOC_UNEXPECTED_EOF,     /* input ended inside markup or with elements open */
    TXML_DOC_NO_ROOT,            /* no document element */
    TXML_DOC_TRAILING_CONTENT,   /* content after the document element */
    TXML_DOC_LIMIT               /* a txml_limits value was reached; see `limit` */
} txml_document_error_code TXML_SYMBOLS("TXML_DOC_");

/* Which limit a TXML_DOC_LIMIT error reached. */
typedef enum {
    TXML_LIMIT_NONE = 0,
    TXML_LIMIT_DEPTH,
    TXML_LIMIT_ATTRIBUTES,
    TXML_LIMIT_NAME_LENGTH,
    TXML_LIMIT_NAMES,
    TXML_LIMIT_DOCUMENT_BYTES,
    TXML_LIMIT_ENTITY_EXPANSION
} txml_limit_kind TXML_SYMBOLS("TXML_LIMIT_");

/*
 * A well-formedness failure. `message` is a fixed English phrase owned by the
 * library and valid for the process lifetime; it is a diagnostic, not an API
 * contract. Document errors are data: they never arrive as a txml_result from
 * a cursor call.
 */
typedef struct {
    txml_document_error_code code;
    txml_limit_kind limit;
    txml_position at;
    txml_str message;
} txml_document_error TXML_VALUE;

/* ========================================================================
 * Allocation
 *
 * The embedder supplies one allocator. There is no realloc: txml never moves
 * what it has allocated, so every pointer it hands out stays valid until the
 * owning object is destroyed.
 */

typedef struct { void *ptr; size_t size; size_t alignment; } txml_allocator_free_params;

typedef struct txml_allocator_vtable {
    /* Return `size` bytes aligned to `alignment`, or NULL. */
    void *(*alloc)(void *self, size_t size, size_t alignment);
    /* Release a block obtained from alloc(); `params` repeats its size and
     * alignment, so the embedder needs no header of its own. */
    void (*free)(void *self, TXML_IN const txml_allocator_free_params *params);
} txml_allocator_vtable TXML_VTABLE;

typedef struct { void *self; const txml_allocator_vtable *v; } txml_allocator TXML_ADAPTER;

/* ========================================================================
 * Arena
 *
 * A bump allocator over a linked list of chunks. Growing links a new chunk in
 * front of the old ones; nothing is copied and no address changes. Everything
 * a document owns lives in one arena, which is why every txml_str a document
 * hands out is valid for that document's lifetime.
 *
 * A request larger than the chunk size gets a chunk of its own, so a single
 * allocation is always contiguous.
 */

typedef struct txml_arena_chunk txml_arena_chunk TXML_VIEW;

typedef struct {
    txml_allocator backing;
    txml_arena_chunk *head;  /* newest chunk; each chunk links to the previous */
    size_t chunk_size;       /* minimum payload size of the next chunk */
    size_t used;             /* bytes handed out to callers */
    size_t reserved;         /* bytes obtained from the backing allocator */
} txml_arena;

/* A position in an arena, for releasing everything allocated after it. */
typedef struct { txml_arena_chunk *chunk; size_t offset; } txml_arena_mark TXML_VALUE;

/* Default chunk size when txml_arena_init() is given 0. */
#define TXML_ARENA_CHUNK_DEFAULT ((size_t)65536u)

TXML_API txml_result txml_arena_init(TXML_OUT TXML_RELEASE(txml_arena_destroy) txml_arena *arena,
                                     txml_allocator backing,
                                     size_t chunk_size);
/* Allocate `size` bytes aligned to `alignment` (a power of two). Returns NULL
 * only when the backing allocator refuses. */
TXML_API void *txml_arena_alloc(TXML_INOUT txml_arena *arena, size_t size, size_t alignment);
TXML_API txml_arena_mark txml_arena_position(TXML_IN const txml_arena *arena);
/* Free every chunk newer than `mark` and resume bump allocation there. One
 * spare chunk is retained to avoid churn. */
TXML_API void txml_arena_release(TXML_INOUT txml_arena *arena, txml_arena_mark mark);
/* Free every chunk but keep the arena usable. */
TXML_API void txml_arena_reset(TXML_INOUT txml_arena *arena);
TXML_API void txml_arena_destroy(TXML_INOUT TXML_DESTROYS txml_arena *arena);

/* ========================================================================
 * Limits and flags
 *
 * Every unbounded construct in XML has a number here. Reaching one ends the
 * parse with a TXML_DOC_LIMIT document error, never with unbounded memory use.
 */

/*
 * Two limits have representation ceilings, because a node's depth is stored
 * as uint16_t and pool offsets are uint32_t: max_depth may not exceed 65535
 * and max_document_bytes may not exceed UINT32_MAX. A configuration that
 * exceeds either is rejected by txml_parser_init() with TXML_ERR_INVALID.
 */
typedef struct {
    uint32_t max_depth;               /* element nesting; at most 65535 */
    uint32_t max_attributes;          /* attributes on one element */
    uint32_t max_name_length;         /* bytes in one qualified name */
    uint32_t max_names;               /* distinct expanded names per document */
    uint64_t max_document_bytes;      /* total document arena bytes; at most UINT32_MAX */
    uint64_t entity_expansion_budget; /* bytes produced by entity expansion */
} txml_limits;

#define TXML_LIMITS_DEFAULT                          \
    ((txml_limits){                                  \
        .max_depth = 256u,                           \
        .max_attributes = 256u,                      \
        .max_name_length = 1024u,                    \
        .max_names = 4096u,                          \
        .max_document_bytes = 64u * 1024u * 1024u,   \
        .entity_expansion_budget = 8u * 1024u * 1024u })

enum TXML_FLAG_SET(txml_parser_flags, "TXML_") {
    TXML_KEEP_COMMENTS = 1u << 0,   /* keep comment nodes */
    TXML_KEEP_PIS = 1u << 1,        /* keep processing-instruction nodes */
    TXML_KEEP_CDATA = 1u << 2,      /* mark CDATA text with TXML_NODE_CDATA */
    TXML_KEEP_POSITIONS = 1u << 3,  /* record a position per node */
    TXML_SKIP_BLANK_TEXT = 1u << 4, /* drop whitespace-only text between elements */
    TXML_INTERNAL_SUBSET = 1u << 5, /* read the internal DTD subset: entities and attribute defaults */
    TXML_INPUT_PADDED = 1u << 6     /* every fed chunk has 64 readable bytes past its end */
};

/* ========================================================================
 * Parser
 *
 * The parser is fed byte chunks and produces a document. It never reads from a
 * file, socket, or entity resolver: external entities and external DTDs are
 * not fetched, which removes XXE by construction.
 */

typedef struct txml_parser txml_parser TXML_HANDLE;
typedef struct txml_doc txml_doc TXML_HANDLE;

typedef enum {
    TXML_STEP_PROGRESS,    /* the step budget was used up; call again */
    TXML_STEP_NEED_INPUT,  /* feed more bytes, or feed final = true */
    TXML_STEP_NEED_MEMORY, /* the allocator refused; give it more and call again */
    TXML_STEP_DONE,        /* the document is complete */
    TXML_STEP_FAILED       /* not well-formed; read txml_parser_error() */
} txml_step TXML_SYMBOLS("TXML_STEP_");

typedef struct {
    txml_allocator allocator;
    /* Optional arena for the document. When null, the parser initializes one
     * over `allocator`. When given, it is borrowed: the caller keeps it alive
     * until the document is destroyed, and owns it afterwards. */
    TXML_OPTIONAL txml_arena *arena;
    txml_limits limits;
    uint32_t flags TXML_FLAGS(txml_parser_flags);
    uint32_t staging_length; /* input window in bytes; 0 selects 64 KiB */
    size_t chunk_size;       /* arena chunk size; 0 selects TXML_ARENA_CHUNK_DEFAULT */
    uint32_t step_budget;    /* nodes appended per step; 0 means until the step blocks */
} txml_parser_config;

TXML_API txml_result txml_parser_init(TXML_OUT TXML_RELEASE(txml_parser_destroy) txml_parser **out,
                                      TXML_IN const txml_parser_config *config);

/*
 * Copy up to `chunk.length` bytes into the parser's staging window and return
 * how many were taken. The caller may reuse or free the chunk as soon as the
 * call returns. Pass `final = true` only with the chunk that ends the
 * document; if fewer bytes than offered are accepted, the flag is ignored and
 * the caller repeats it with the rest. A caller that ignores the return value
 * silently truncates its document, which surfaces as TXML_DOC_UNEXPECTED_EOF.
 * Feeding after final is TXML_ERR_INVALID reported by the next step.
 *
 * An XML declaration is validated and then dropped: it is not a node, and no
 * function reports its version, encoding, or standalone flag. A declaration
 * naming an encoding other than UTF-8 is TXML_DOC_ENCODING. What the writer
 * emits is governed by TXML_WRITE_DECLARATION alone.
 */
TXML_API uint32_t txml_parse_feed(txml_parser *parser, txml_bytes chunk, bool final);

/* Parse staged input. Each call appends nodes until the step budget is used
 * up or the parser blocks. */
TXML_API txml_step txml_parse_step(txml_parser *parser);

/* The document under construction. Valid from init until the parser is
 * destroyed or the document is detached; readable while parsing. */
TXML_API const txml_doc *txml_parser_doc(const txml_parser *parser);

/* The failure that ended the parse. TXML_ERR_NOT_FOUND while none happened. */
TXML_API txml_result txml_parser_error(const txml_parser *parser,
                                       TXML_OUT TXML_VALID_ON(TXML_OK) txml_document_error *out);

/* Bytes accepted by txml_parse_feed() so far. */
TXML_API uint64_t txml_parser_offset(const txml_parser *parser);

/* Detach the finished document and destroy the parser. Only after
 * TXML_STEP_DONE; otherwise TXML_ERR_INVALID and nothing is destroyed. */
TXML_API txml_result txml_parser_finish(TXML_DESTROYS txml_parser *parser,
                                        TXML_OUT TXML_RELEASE(txml_doc_destroy) txml_doc **out);

/* Destroy the parser and, unless it was detached, its document. */
TXML_API void txml_parser_destroy(TXML_DESTROYS txml_parser *parser);

/* Destroy a detached document. Chunks of a borrowed arena are released, the
 * arena itself is not destroyed. */
TXML_API void txml_doc_destroy(TXML_DESTROYS txml_doc *doc);

/* ========================================================================
 * Document
 *
 * Nodes are stored in document order. A node's index is its identity, and a
 * subtree is the half-open index range [index, end). Names are interned per
 * document: a name ID is meaningless for any other document.
 */

#define TXML_NAME_NONE ((uint32_t)UINT32_MAX)
#define TXML_INDEX_NONE ((uint32_t)UINT32_MAX)

/* Resolve an expanded name to this document's name ID, or TXML_NAME_NONE when
 * the document does not contain it. A query that resolves to TXML_NAME_NONE
 * can be answered without scanning anything. */
TXML_API uint32_t txml_doc_find_name(const txml_doc *doc, TXML_IN const txml_qname *name);
TXML_API uint32_t txml_doc_name_count(const txml_doc *doc);
TXML_API txml_result txml_doc_name_at(const txml_doc *doc,
                                      uint32_t name,
                                      TXML_OUT TXML_LIFETIME(document) txml_qname *out);

/* Nodes appended so far, including the document node. */
TXML_API uint32_t txml_doc_node_count(const txml_doc *doc);

/* Accounting for the document's arena, borrowed or owned. */
TXML_API const txml_arena *txml_doc_arena(const txml_doc *doc) TXML_LIFETIME(document);

/*
 * Streaming: a consumer that has finished with a completed subtree can
 * discard it and reuse its memory. `txml_doc_mark()` returns the next node
 * index; `txml_doc_rewind()` drops every node from `mark` on and releases
 * their arena memory. Rewinding is refused with TXML_ERR_INVALID while any
 * element at or after `mark` is still open, and it invalidates every cursor
 * and txml_str at or after `mark`. Names and namespaces are never renumbered,
 * so a name ID a consumer holds stays valid across a rewind.
 */
TXML_API uint32_t txml_doc_mark(const txml_doc *doc);
TXML_API txml_result txml_doc_rewind(txml_doc *doc, uint32_t mark);

/* ========================================================================
 * Cursors
 *
 * A cursor is a value: a document and a node index. Copy it, store it, compare
 * indices for document order. It owns nothing and allocates nothing.
 *
 * Cursor calls are total: they never allocate and never return an API error.
 * A cursor whose index is out of range, or that was made for another
 * document, reads as absent — TXML_NONE, TXML_NAME_NONE, TXML_INDEX_NONE, an
 * empty txml_str, false. Mixing cursors from two documents is still a bug;
 * it is simply not one the library can report.
 */

typedef struct { const txml_doc *doc; uint32_t node; } txml_cursor TXML_VALUE;
typedef struct { const txml_doc *doc; uint32_t index; } txml_attr_cursor TXML_VALUE;

typedef enum {
    TXML_MOVED,  /* the cursor now points at the requested node */
    TXML_NONE,   /* there is no such node; the cursor did not move */
    TXML_PENDING /* the answer needs input not parsed yet; the cursor did not move */
} txml_move TXML_SYMBOLS("TXML_");

typedef enum {
    TXML_NODE_DOCUMENT, /* index 0; the parent of the document element */
    TXML_NODE_ELEMENT,
    TXML_NODE_TEXT,
    TXML_NODE_COMMENT,
    TXML_NODE_PI
} txml_node_kind TXML_SYMBOLS("TXML_NODE_");

enum TXML_FLAG_SET(txml_node_flags, "TXML_NODE_") {
    TXML_NODE_EMPTY = 1u << 0, /* written as <a/> */
    TXML_NODE_CDATA = 1u << 1, /* text written as a CDATA section */
    TXML_NODE_OPEN = 1u << 2   /* the end tag has not been parsed yet */
};

enum TXML_FLAG_SET(txml_attribute_flags, "TXML_ATTR_") {
    TXML_ATTR_NS_DECL = 1u << 0,  /* xmlns or xmlns:prefix */
    TXML_ATTR_DEFAULTED = 1u << 1 /* supplied by an internal-subset default */
};

typedef struct {
    uint32_t name;
    txml_str value;
    uint32_t flags TXML_FLAGS(txml_attribute_flags);
} txml_attribute_view TXML_VALUE;

/* Navigation. Every function moves the cursor only on TXML_MOVED. */
TXML_API txml_cursor txml_doc_root(const txml_doc *doc);
TXML_API txml_move txml_first_child(TXML_INOUT txml_cursor *cursor);
TXML_API txml_move txml_next_sibling(TXML_INOUT txml_cursor *cursor);
TXML_API txml_move txml_parent(TXML_INOUT txml_cursor *cursor);
/* The next sibling that is an element, skipping text, comments, and PIs. */
TXML_API txml_move txml_next_element(TXML_INOUT txml_cursor *cursor);

/* Ranges. A subtree is [txml_index(c), txml_subtree_end(c)). */
TXML_API uint32_t txml_index(txml_cursor cursor);
TXML_API txml_cursor txml_cursor_at(const txml_doc *doc, uint32_t index);
/* One past the last descendant, or TXML_INDEX_NONE while the node is open. */
TXML_API uint32_t txml_subtree_end(txml_cursor cursor);
/* Whether `node` is `ancestor` itself or one of its descendants. False while
 * `ancestor` is still open. */
TXML_API bool txml_contains(txml_cursor ancestor, txml_cursor node);

/* A name scan over a node range: the fastest query the document supports. */
typedef struct {
    uint32_t name;  /* the name ID to match */
    uint32_t bound; /* stop before this index; TXML_INDEX_NONE means the whole document */
} txml_find;

/* Move to the next element after the cursor, below `bound`, whose name
 * matches. TXML_NONE when there is none. */
TXML_API txml_move txml_find_next(TXML_INOUT txml_cursor *cursor, TXML_IN const txml_find *query);

/* Reading. */
TXML_API txml_node_kind txml_kind(txml_cursor cursor);
TXML_API uint32_t txml_flags(txml_cursor cursor) TXML_FLAGS(txml_node_flags);
TXML_API uint32_t txml_depth(txml_cursor cursor);
/* The element's or PI target's name ID; TXML_NAME_NONE for other kinds. */
TXML_API uint32_t txml_node_name_id(txml_cursor cursor);
/* The prefix as written, empty when there was none. Not part of identity. */
TXML_API txml_str txml_node_prefix(txml_cursor cursor) TXML_LIFETIME(document);
/* Text of a TEXT, COMMENT, or PI node, normalized; empty for other kinds. */
TXML_API txml_str txml_node_text(txml_cursor cursor) TXML_LIFETIME(document);
/* TXML_ERR_NOT_FOUND unless the document was parsed with TXML_KEEP_POSITIONS. */
TXML_API txml_result txml_node_position(txml_cursor cursor, TXML_OUT txml_position *out);

/* Attributes, in document order. */
TXML_API uint32_t txml_attribute_count(txml_cursor cursor);
TXML_API txml_move txml_first_attribute(txml_cursor cursor, TXML_OUT txml_attr_cursor *out);
TXML_API txml_move txml_next_attribute(TXML_INOUT txml_attr_cursor *attribute);
TXML_API txml_result txml_attribute_read(txml_attr_cursor attribute,
                                         TXML_OUT TXML_LIFETIME(document) txml_attribute_view *out);
/* Look up one attribute by name ID. An unprefixed attribute has no namespace,
 * so a default xmlns does not apply to it. */
TXML_API bool txml_find_attribute(txml_cursor cursor,
                                  uint32_t name,
                                  TXML_OUT TXML_LIFETIME(document) txml_str *value);

/* ========================================================================
 * Writer
 *
 * The writer takes events and fills a caller buffer. It knows nothing about
 * documents: anything that can emit start/attribute/text/end can produce XML.
 * txml_write_subtree() is the bridge from a parsed document.
 */

typedef struct txml_writer txml_writer TXML_HANDLE;

enum TXML_FLAG_SET(txml_writer_flags, "TXML_WRITE_") {
    TXML_WRITE_DECLARATION = 1u << 0, /* emit <?xml version="1.0" encoding="UTF-8"?> */
    TXML_WRITE_INDENT = 1u << 1,      /* indent element content by depth */
    TXML_WRITE_EMPTY_TAGS = 1u << 2   /* write childless elements as <a/> */
};

typedef struct {
    txml_allocator allocator; /* backs the writer's element and namespace stack */
    txml_buffer output;       /* filled by the writer, drained by the caller */
    txml_limits limits;
    uint32_t flags TXML_FLAGS(txml_writer_flags);
} txml_writer_config;

/* A name to write. An empty `prefix` with a non-empty `ns` asks the writer to
 * pick or reuse a prefix; an empty `ns` writes an unqualified name. */
typedef struct { txml_str ns; txml_str local; txml_str prefix; } txml_write_name TXML_VALUE;
typedef struct { txml_write_name name; txml_str value; } txml_write_attr TXML_VALUE;

TXML_API txml_result txml_writer_init(TXML_OUT TXML_RELEASE(txml_writer_destroy) txml_writer **out,
                                      TXML_IN const txml_writer_config *config);

/*
 * Every emit call returns TXML_OK or TXML_NEED_SPACE. On TXML_NEED_SPACE the
 * caller writes out txml_writer_output() and calls txml_writer_drain(), which
 * continues the interrupted call and may return TXML_NEED_SPACE again. Text
 * passed to a call that returned TXML_NEED_SPACE must stay valid until that
 * call finally returns TXML_OK.
 */
TXML_API txml_result txml_write_start(txml_writer *writer, TXML_IN const txml_write_name *name);
TXML_API txml_result txml_write_attribute(txml_writer *writer, TXML_IN const txml_write_attr *attribute);
TXML_API txml_result txml_write_text(txml_writer *writer, txml_str text);
TXML_API txml_result txml_write_cdata(txml_writer *writer, txml_str text);
TXML_API txml_result txml_write_comment(txml_writer *writer, txml_str text);
/* A processing instruction: `<?target data?>`. The target is an unqualified
 * name; the data is written verbatim and may not contain "?>". */
TXML_API txml_result txml_write_pi(txml_writer *writer, txml_str target, txml_str data);
TXML_API txml_result txml_write_end(txml_writer *writer);

/* Write a node and its descendants. Resumable: the writer remembers where in
 * the walk it stopped, and drains continue it. */
TXML_API txml_result txml_write_subtree(txml_writer *writer, txml_cursor cursor);

/* Bytes produced and not yet consumed. */
TXML_API txml_bytes txml_writer_output(const txml_writer *writer) TXML_LIFETIME(drain);
/* Consume them and continue any interrupted operation. */
TXML_API txml_result txml_writer_drain(txml_writer *writer);
/* Check that every element was closed. Remaining bytes stay in the output. */
TXML_API txml_result txml_writer_finish(txml_writer *writer);
TXML_API void txml_writer_destroy(TXML_DESTROYS txml_writer *writer);

/* ========================================================================
 * Text helpers
 *
 * txml uses no C library, so these are the comparisons it needs internally and
 * callers usually want too.
 */

TXML_API bool txml_str_equal(txml_str left, txml_str right);
TXML_API bool txml_qname_equal(TXML_IN const txml_qname *left, TXML_IN const txml_qname *right);
/* Whether `text` begins with `prefix`; the SCXML event-descriptor case. */
TXML_API bool txml_str_starts_with(txml_str text, txml_str prefix);

#ifdef __cplusplus
}
#endif
