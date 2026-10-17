/*
 * GENERATED FILE. Do not edit; run `mise run amalgamate` in tinyxml.
 * The public API is include/txml.h. The private module headers (listed by
 * src/internal.h) and every module source follow, in that order.
 */
#include "txml.h"

/*
 * Functions not declared in txml.h are private to this file. Private helpers
 * may be unused in a given build, so they do not warn.
 */
#ifndef TXML_PRIVATE
#if defined(__GNUC__) || defined(__clang__)
#define TXML_PRIVATE static __attribute__((unused))
#else
#define TXML_PRIVATE static
#endif
#endif

/* ---- src/text.h ---- */
/* Private: byte-level text checks shared by the parser and the writer. */


/* Index of the first byte that is not well-formed UTF-8 or not an XML Char, or
 * `length` when there is none. A sequence cut off by the end counts as
 * invalid. */
TXML_PRIVATE uint32_t txml_text_first_invalid(const uint8_t *bytes, uint32_t length);

/*
 * The same check for input that arrives in pieces. `invalid` is the index of
 * the lead byte of the first bad character, or UINT32_MAX; `complete` is the
 * index just past the last whole character, which is less than `length` only
 * when the bytes end inside a sequence that could still turn out valid.
 */
typedef struct {
    uint32_t invalid;
    uint32_t complete;
} txml_text_scan;

TXML_PRIVATE txml_text_scan txml_text_scan_utf8(const uint8_t *bytes, uint32_t length);

/*
 * One character of well-formed UTF-8. `bytes` must start at a character
 * boundary with the whole character present, which the parser guarantees for
 * everything below `valid` and the writer by checking its input first. Returns
 * the code point and sets `width` to its length in bytes.
 */
TXML_PRIVATE uint32_t txml_text_decode(const uint8_t *bytes, uint32_t *width);

/* XML 1.0 (fifth edition) NameStartChar and NameChar, which include ':'. */
TXML_PRIVATE bool txml_text_is_name_start(uint32_t code);
TXML_PRIVATE bool txml_text_is_name_char(uint32_t code);

/*
 * Set `length` bytes to zero. The compiler calls memset behind the code's back
 * for a large struct initialiser, and can turn a plain byte loop into the same
 * call; either would make the library depend on the C library. This is the
 * loop, with the recognition that would undo it switched off where the compiler
 * offers a way to say so.
 */
TXML_PRIVATE void txml_text_zero(void *destination, size_t length);

/* ---- src/doc.h ---- */
/*
 * Private: the document's columns and the operations that build them.
 * Layout is specified in docs/internal/document-layout.md.
 *
 * Nothing in a column is a pointer; every reference is a uint32_t index or
 * pool offset. The parser appends through the functions below and the cursor
 * module reads through the accessors.
 */


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
    txml_column positions;  /* tree; one txml_span per node */
    txml_column attribute_sources; /* tree; one txml_attribute_source per attribute */
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
TXML_PRIVATE txml_result txml_doc_create(txml_doc **out, const txml_parser_config *config);

/* Append a node and its position entry; the node's `end` and `flags` are the
 * caller's to set (leaves and empty elements already closed, others OPEN). */
typedef struct {
    txml_node node;
    txml_position position;
    txml_position end; /* unset for a node that is appended open */
} txml_node_append;
TXML_PRIVATE txml_result txml_doc_append_node(txml_doc *doc, const txml_node_append *append, uint32_t *index);
/* Where an open node stops: its `end` index and the position after its end tag. */
typedef struct {
    uint32_t end;
    txml_position position;
} txml_close;
/* Set `end` on an open node, record where it ends and clear TXML_NODE_OPEN. */
TXML_PRIVATE void txml_doc_close_node(txml_doc *doc, uint32_t index, const txml_close *close);
/* Mark the parse finished: closes the document node, which ends at `end`, and
 * stops PENDING. */
TXML_PRIVATE void txml_doc_complete(txml_doc *doc, txml_position end);

/* Where an attribute was written; `start.line` is 0 when it has no source. */
typedef struct {
    txml_position start;
    txml_position value_start;
    txml_position end;
} txml_attribute_source;

/* Append one attribute. The caller ORs TXML_ATTRIBUTE_LAST and
 * TXML_ATTRIBUTE_DEFAULTED into `entry.name`. */
typedef struct {
    txml_attribute_entry entry;
    txml_attribute_source source;
} txml_attribute_append;
TXML_PRIVATE txml_result txml_doc_append_attribute(txml_doc *doc, const txml_attribute_append *append, uint32_t *index);

/* Copy bytes into the pool; the run never spans a page unless it is larger
 * than one, in which case it occupies consecutive whole pages. */
TXML_PRIVATE txml_result txml_doc_pool_append(txml_doc *doc, txml_str bytes, uint32_t *offset);

TXML_PRIVATE txml_result txml_doc_intern_namespace(txml_doc *doc, txml_str uri, uint32_t *id);
typedef struct {
    uint32_t namespace_id;
    txml_str local;
    txml_str prefix;
} txml_intern_params;
TXML_PRIVATE txml_result txml_doc_intern_name(txml_doc *doc, const txml_intern_params *params, uint32_t *id);

/* Read-side accessors; NULL / empty for anything out of range. */
TXML_PRIVATE const txml_node *txml_doc_node(const txml_doc *doc, uint32_t index);
TXML_PRIVATE const txml_attribute_entry *txml_doc_attribute(const txml_doc *doc, uint32_t index);
TXML_PRIVATE const txml_name_entry *txml_doc_name(const txml_doc *doc, uint32_t id);
TXML_PRIVATE txml_str txml_doc_pool_str(const txml_doc *doc, uint32_t offset, uint32_t length);
TXML_PRIVATE const txml_position *txml_doc_position(const txml_doc *doc, uint32_t index);
TXML_PRIVATE const txml_span *txml_doc_span(const txml_doc *doc, uint32_t index);
TXML_PRIVATE const txml_attribute_source *txml_doc_attribute_source(const txml_doc *doc, uint32_t index);
/* TXML_ATTR_NS_DECL is derived from the name's namespace, TXML_ATTR_DEFAULTED
 * from the entry's flag bit. */
TXML_PRIVATE uint32_t txml_doc_attribute_flags(const txml_doc *doc, const txml_attribute_entry *entry);
TXML_PRIVATE const txml_namespace_entry *txml_doc_namespace(const txml_doc *doc, uint32_t id);

typedef struct {
    bool attribute; /* an attribute index, not a node index */
    uint32_t index;
    txml_str prefix; /* as written */
} txml_prefix_request;

/* Record the prefix a node or attribute was written with, after it has been
 * appended. Costs nothing when it matches the name entry's prefix. */
TXML_PRIVATE txml_result txml_doc_record_prefix(txml_doc *doc, const txml_prefix_request *request);
/* The prefix a node (or attribute) was written with; empty when none. */
TXML_PRIVATE txml_str txml_doc_prefix_of(const txml_doc *doc, bool attribute, uint32_t index);

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

TXML_PRIVATE void txml_doc_run_begin(txml_doc *doc, txml_run *run);
/* Make room for `extra` more bytes, so a following append cannot fail. */
TXML_PRIVATE txml_result txml_doc_run_reserve(txml_doc *doc, txml_run *run, uint32_t extra);
TXML_PRIVATE txml_result txml_doc_run_append(txml_doc *doc, txml_run *run, txml_str bytes);
/* The bytes so far, valid until the next append. */
TXML_PRIVATE txml_str txml_doc_run_view(const txml_doc *doc, const txml_run *run);
/* Commit the run to the pool. Idempotent. */
TXML_PRIVATE void txml_doc_run_finish(txml_doc *doc, const txml_run *run);

/* ---- src/parser.h ---- */
/*
 * Private: the parser's state and the pieces its modules share.
 *
 * The scanner is a table of resumable states (ADR 0021). A state is a struct
 * holding two function pointers: `run` consumes whatever bytes are readable
 * and may transition by assigning `parser->state`; `at_eof` says what the end
 * of input means while that state is current. Everything a state needs to
 * continue after a chunk boundary lives in the parser, never in the window:
 * the window is compacted between feeds and no state keeps an index into it.
 *
 * Modules:
 *   parser.c   init, feed, step, positions, errors, appending leaves
 *   decode.c   sinks, the reference/entity/line-end decoder
 *   content.c  content, '<' dispatch, comments, CDATA, PIs, the declaration
 *   tag.c      start tags and end tags
 *   doctype.c  the DOCTYPE and its internal subset
 */


#define IDX_NONE UINT32_MAX

/* ------------------------------------------------------------------------
 * Small types.
 */

typedef struct txml_parser txml_parser;

/* What a state's run() reports. There is no "incomplete": a state consumes
 * everything readable, and the step loop asks for input when none is left. */
typedef enum {
    SCAN_OK,    /* progress; run the current state again */
    SCAN_YIELD, /* the step budget is spent; return TXML_STEP_PROGRESS */
    SCAN_NOMEM, /* an allocation failed; nothing half-done, retry later */
    SCAN_FAIL,  /* a document error was recorded */
    SCAN_DONE   /* the document is complete */
} scan;

typedef struct state {
    const char *name;
    scan (*run)(txml_parser *p);
    scan (*at_eof)(txml_parser *p);
} state;

typedef enum { STAGE_PROLOG, STAGE_CONTENT, STAGE_EPILOG } stage;
typedef enum { DECODE_TEXT, DECODE_ATTRIBUTE, DECODE_VERBATIM } decode_mode;

typedef struct {
    txml_document_error_code code;
    txml_limit_kind limit;
    const char *message;
} problem;

/* The compound literal lives for the statement that uses it. */
#define PROBLEM(code_, message_) (&(const problem){ (code_), TXML_LIMIT_NONE, (message_) })
#define PROBLEM_LIMIT(limit_, message_) (&(const problem){ TXML_DOC_LIMIT, (limit_), (message_) })

/* Where decoded bytes go. */
typedef struct sink {
    scan (*put)(txml_parser *p, const uint8_t *bytes, uint32_t length);
} sink;

typedef struct binding {
    struct binding *prev;
    txml_str prefix;
    uint32_t namespace_id;
} binding;

typedef struct {
    uint32_t node;
    binding *bindings; /* the binding stack before this element's declarations */
    txml_arena_mark mark;
    txml_str raw_name; /* as written, for end-tag matching */
} open_element;

/* An internal-subset general entity: its literal, character references already
 * expanded (XML 1.0 §4.5). */
typedef struct entity {
    txml_str name;
    txml_str value;
} entity;

/* An ATTLIST default: `element` and `name` are the qualified names as written. */
typedef struct attlist_default {
    struct attlist_default *next; /* the next default of the same element, in declaration order */
    txml_str element;
    txml_str name;
    txml_str value;
} attlist_default;

/* The defaults of one element. */
typedef struct {
    txml_str element;
    attlist_default *first;
    attlist_default *last;
} element_defaults;

/* An open-addressing table of pointers, keyed by one or two strings. It grows
 * by doubling in the parser arena; only pointers move, never the items. */
typedef struct {
    txml_str first;
    txml_str second;
} table_key;

typedef struct {
    table_key (*key)(const void *item);
} table_kind;

typedef struct {
    void **slots;
    uint32_t capacity; /* zero, or a power of two */
    uint32_t count;
} hashed;

typedef struct {
    txml_str prefix;
    txml_str local;
    uint32_t value_offset;
    uint32_t value_length;
    uint32_t name_id;
    txml_position position;
    txml_position value_start; /* first byte inside the quotes */
    txml_position value_end;   /* the closing quote */
    bool declaration;
    bool defaulted;
} attribute_record;

/* One level of entity expansion. */
typedef struct {
    const uint8_t *source;
    uint32_t length;
    uint32_t position;
    const entity *entity;
} frame;

/* A growable transient byte buffer for the small constructs that are worth
 * assembling whole: the XML declaration, an ENTITY or ATTLIST declaration, a
 * namespace URI. Bounded by max_document_bytes. */
typedef struct {
    uint8_t *data;
    uint32_t length;
    uint32_t capacity;
} buffer;

/* Decoding state that survives a chunk boundary. */
typedef struct {
    decode_mode mode;
    bool after_cr;          /* a '\r' was just emitted; a following '\n' is dropped */
    uint32_t brackets;      /* TEXT: consecutive ']' just emitted, at most 2 */
    bool in_ref;            /* between '&' and ';' */
    uint32_t ref_length;
    txml_position ref_position;
    bool expanding;         /* frames hold an unfinished expansion */
    uint32_t level;         /* index of the innermost frame; 0 means none */
} decoder;

/* What a construct changed, so a failed append can be undone. */
typedef struct {
    uint32_t nodes;
    uint32_t attributes;
    uint32_t node_prefixes;
    uint32_t attribute_prefixes;
    binding *bindings;
    txml_arena_mark mark;
} snapshot;

typedef enum {
    TP_ELEMENT_NAME,
    TP_SPACE,        /* between attributes: whitespace, '>' or '/' */
    TP_ATTRIBUTE_NAME,
    TP_AFTER_NAME,   /* whitespace, then '=' */
    TP_BEFORE_VALUE, /* whitespace, then a quote */
    TP_VALUE,
    TP_SLASH         /* after '/', expecting '>' */
} tag_phase;

typedef struct {
    tag_phase phase;
    bool seen_space;
    bool empty;
    uint8_t quote;
    uint32_t count;   /* attributes recorded so far */
    uint32_t written; /* the count when finishing began */
    bool finishing;
    binding *written_bindings;
    txml_arena_mark written_mark;
    txml_arena_mark tag_mark;   /* tag_arena, released when the tag ends */
    snapshot before;            /* taken when the tag began */
    txml_str raw_name;          /* a copy in stack_arena */
    txml_str element_prefix;
    txml_str element_local;
    txml_position name_position;
    txml_position slash_position;
} tag_work;

typedef enum { LIT_COMMENT, LIT_CDATA, LIT_DOCTYPE } literal_kind;

typedef enum {
    DP_SPACE,          /* the whitespace after "<!DOCTYPE" */
    DP_SPACE_SKIP,
    DP_NAME,
    DP_HEAD,           /* external identifier, collected up to '[' or '>' */
    DP_SUBSET,         /* between declarations */
    DP_PARAMETER,      /* the first character of %name; */
    DP_PARAMETER_NAME, /* the rest of %name; */
    DP_LT,             /* after '<' */
    DP_BANG,           /* after "<!" */
    DP_DASH,           /* after "<!-" */
    DP_KEYWORD,        /* the letters of ENTITY, ATTLIST, ... */
    DP_COMMENT,
    DP_PI_START,       /* the first character of a PI target */
    DP_PI_TARGET,
    DP_PI,             /* PI data, skipped to "?>" */
    DP_COLLECT,        /* ENTITY, ATTLIST, ELEMENT, NOTATION: collected, then read whole */
    DP_AFTER           /* after ']', up to '>' */
} doctype_phase;

/* What the small buffer holds when it is read (subset.c). */
typedef enum {
    DECL_DOCTYPE_HEAD, /* what follows the DOCTYPE name, through '[' or '>' */
    DECL_ENTITY,
    DECL_ATTLIST,
    DECL_ELEMENT,
    DECL_NOTATION
} declaration_kind;

typedef struct {
    doctype_phase phase;
    uint8_t quote;
    uint32_t pending; /* '-' or '?' held back while looking for a terminator */
    char keyword[12];
    uint32_t keyword_length;
    declaration_kind kind;
    txml_position lt_position;
} doctype_work;

struct txml_parser {
    txml_allocator allocator;
    txml_limits limits;
    uint32_t step_budget;
    uint32_t staging_length;

    txml_arena parser_arena; /* window, buffers, DTD tables */
    txml_arena stack_arena;  /* open-element names and bindings; LIFO */
    txml_arena tag_arena;    /* names of one start tag; released when it ends */
    txml_doc *doc;
    bool detached;

    /* Input. Bytes in [start, limit) are whole, valid UTF-8 characters. */
    uint8_t *window; /* staging_length + padding */
    uint32_t start;  /* first unconsumed byte */
    uint32_t end;    /* one past the last staged byte */
    uint32_t valid;  /* one past the last whole valid character */
    uint32_t bad;    /* index of the lead byte of the first invalid character */
    bool final_seen;
    bool fed_after_final;
    uint64_t fed;      /* bytes accepted by feeds */
    uint64_t consumed; /* offset of window[start] */
    uint32_t line;
    uint32_t column;

    /* The scanner. */
    const state *state;
    txml_position construct; /* where the construct being scanned began */
    uint32_t appended;       /* nodes appended during this step */

    /* Document structure. */
    stage stage;
    bool seen_root;
    bool seen_doctype;
    uint32_t bom_length; /* 3 when the input began with a UTF-8 byte order mark, else 0 */
    open_element *stack;
    uint32_t depth;
    binding *bindings;

    /* The DTD tables. */
    hashed entities;         /* entity, by name */
    hashed default_keys;     /* attlist_default, by element and attribute name */
    hashed element_defaults; /* element_defaults, by element */
    uint64_t table_bytes;
    uint64_t expansion_used;

    /* Decoding. */
    decoder dec;
    frame *frames;       /* limits.max_depth + 1 entries */
    uint8_t *reference;  /* the text between '&' and ';' */
    uint32_t reference_capacity;
    const sink *sink;
    txml_run run;
    buffer small;
    txml_position small_position; /* where small.data[0] came from */

    /* A name being read: an element, attribute or PI target. */
    uint8_t *name;
    uint32_t name_length;
    txml_position name_position;

    attribute_record *records;

    /* Per-construct working data. */
    bool text_active;
    struct {
        literal_kind kind;
        const char *expect;
        uint32_t index;
    } literal;
    struct { uint32_t dashes; txml_position first_dash; } comment;
    struct { uint32_t brackets; } cdata;
    struct {
        uint32_t question;
        txml_position colon_position;
        bool has_colon;
        txml_position question_position;
    } pi;
    struct { uint32_t question; } declaration;
    struct {
        uint32_t phase; /* 0: first byte, 1: matching the name, 2: before '>' */
        uint32_t matched;
        txml_position junk_position;
    } end_tag;
    tag_work tag;
    doctype_work doctype;

    bool done;
    bool failed;
    txml_document_error error;
};

/* ------------------------------------------------------------------------
 * parser.c
 */

/* Bytes the states may read: everything whole and valid. */
TXML_PRIVATE uint32_t txp_limit(const txml_parser *p);
TXML_PRIVATE txml_position txp_position_at(const txml_parser *p, uint32_t index);
/* The position of window[start]. */
TXML_PRIVATE txml_position txp_position_now(const txml_parser *p);
/* The position one past window[index], a delimiter that is not a line end. */
TXML_PRIVATE txml_position txp_position_after(const txml_parser *p, uint32_t index);
/* Consume up to `index`, keeping line, column and offset current. */
TXML_PRIVATE void txp_commit(txml_parser *p, uint32_t index);

/*
 * Positions captured inside a loop must not each rescan the window from
 * `start`, which is quadratic in the chunk. A cursor remembers the last index
 * it answered for, so a run of increasing indices costs one pass in all.
 */
typedef struct {
    uint32_t index;
    txml_position at;
    bool valid;
} position_cursor;

TXML_PRIVATE txml_position txp_cursor_position(const txml_parser *p, position_cursor *cursor, uint32_t index);

TXML_PRIVATE scan txp_fail(txml_parser *p, const problem *what, txml_position at);
TXML_PRIVATE scan txp_fail_result(txml_parser *p, txml_result result, txml_position at);
TXML_PRIVATE scan txp_check_budget(txml_parser *p, txml_position at);
TXML_PRIVATE bool txp_budget_spent(const txml_parser *p);
TXML_PRIVATE uint32_t txp_parent_node(const txml_parser *p);

typedef struct {
    txml_node_kind kind;
    uint32_t flags;
    uint32_t name;
    txml_position position;
    txml_position end; /* one past the closing delimiter */
} leaf;

/* Append a node whose text is `p->run`. Finishes the run first; safe to retry. */
TXML_PRIVATE scan txp_append_leaf(txml_parser *p, const leaf *what);

TXML_PRIVATE snapshot txp_snapshot(txml_parser *p);
/* Undo appended nodes, attributes and prefix exceptions. */
TXML_PRIVATE void txp_rollback_nodes(txml_parser *p, const snapshot *s);

/* The end of input inside a construct: UNEXPECTED_EOF at the current position. */
TXML_PRIVATE scan txp_eof_in_markup(txml_parser *p);

typedef struct {
    txml_intern_params params;
    txml_position at;
} intern_request;

/* Intern a name; a full names table is a TXML_DOC_LIMIT at `request->at`. */
TXML_PRIVATE scan txp_intern(txml_parser *p, const intern_request *request, uint32_t *id);

/* The character at window[index] cannot be part of a name where one must be
 * (or continue): TXML_DOC_NAME at that character. Only ever raised for
 * non-ASCII characters, since no non-ASCII character delimits markup. */
TXML_PRIVATE scan txp_fail_name_char(txml_parser *p, uint32_t index);

TXML_PRIVATE uint32_t txp_skip_space(const uint8_t *bytes, uint32_t from, uint32_t limit);
TXML_PRIVATE uint32_t txp_scan_name(const uint8_t *bytes, uint32_t from, uint32_t limit);

TXML_PRIVATE bool txp_is_space(uint8_t byte);
/* Whether the whole character at `bytes` starts, or continues, a Name; `width`
 * is its length. `bytes` must be at a character boundary below `valid`. */
TXML_PRIVATE bool txp_name_start_at(const uint8_t *bytes, uint32_t *width);
TXML_PRIVATE bool txp_name_char_at(const uint8_t *bytes, uint32_t *width);
TXML_PRIVATE uint32_t txp_literal_length(const char *literal);
TXML_PRIVATE bool txp_bytes_are(const uint8_t *bytes, uint32_t length, const char *literal);

/* ------------------------------------------------------------------------
 * decode.c
 */

extern const sink txp_sink_run;
extern const sink txp_sink_buffer;

TXML_PRIVATE scan txp_buffer_append(txml_parser *p, const uint8_t *bytes, uint32_t length);
TXML_PRIVATE txml_position txp_small_position_at(const txml_parser *p, uint32_t index);

TXML_PRIVATE void txp_dec_reset(txml_parser *p, decode_mode mode);

typedef struct {
    const uint8_t *bytes;
    uint32_t length;
} dec_input;

/* Decode `input` into the current sink. `consumed` is how many bytes were
 * fully handled, which on SCAN_NOMEM is where to resume. */
TXML_PRIVATE scan txp_dec_feed(txml_parser *p, const dec_input *input, uint32_t *consumed);
/* Finish a pending entity expansion. */
TXML_PRIVATE scan txp_dec_resume(txml_parser *p);
/* A construct is ending: an unfinished reference is malformed. */
TXML_PRIVATE scan txp_dec_end(txml_parser *p);

/* Read the character references of an entity literal, leaving &name;
 * references alone (XML 1.0 §4.5). The result is allocated in the parser arena. */
TXML_PRIVATE scan txp_expand_character_references(txml_parser *p, txml_str literal, txml_str *expanded);

typedef enum { CHARREF_OK, CHARREF_MALFORMED, CHARREF_ENCODING } charref_check;
/* Check the body of a numeric reference, "#123" or "#x7B", without the '&' and
 * ';'. */
TXML_PRIVATE charref_check txp_check_character_reference(const uint8_t *body, uint32_t length);

/* ------------------------------------------------------------------------
 * subset.c: reading the declarations of a DOCTYPE
 */

/* Read the declaration collected in p->small: the bytes after its keyword (or,
 * for DECL_DOCTYPE_HEAD, after the DOCTYPE name), through the '>' or '[' that
 * ended it. Entities and attribute defaults are recorded; everything is
 * checked against the XML 1.0 grammar. */
TXML_PRIVATE scan txp_read_declaration(txml_parser *p, declaration_kind kind);

/* ------------------------------------------------------------------------
 * tables.c: the DTD tables
 */

TXML_PRIVATE const entity *txp_find_entity(const txml_parser *p, txml_str name);
TXML_PRIVATE const element_defaults *txp_find_element_defaults(const txml_parser *p, txml_str element);
/* Record a declaration; the first for a name (or element and attribute) binds. */
TXML_PRIVATE scan txp_add_entity(txml_parser *p, txml_str name, txml_str value);

typedef struct {
    txml_str element;
    txml_str name;
    txml_str value;
} default_request;

TXML_PRIVATE scan txp_add_default(txml_parser *p, const default_request *request);

/* ------------------------------------------------------------------------
 * content.c
 */

/* The state a parse begins in: it consumes a UTF-8 byte order mark, if any. */
extern const state txp_state_start;
extern const state txp_state_content;
extern const state txp_state_lt;
extern const state txp_state_bang;
extern const state txp_state_literal;
extern const state txp_state_comment;
extern const state txp_state_cdata;
extern const state txp_state_pi_target;
extern const state txp_state_pi_space;
extern const state txp_state_pi_question;
extern const state txp_state_pi_data;
extern const state txp_state_declaration;

/* The state for `<!DOCTYPE` and its subset, and starting it. */
TXML_PRIVATE scan txp_begin_doctype(txml_parser *p);

/* ------------------------------------------------------------------------
 * tag.c
 */

extern const state txp_state_start_tag;
extern const state txp_state_end_tag;

/* window[start] is the first byte of the element name. */
TXML_PRIVATE scan txp_begin_start_tag(txml_parser *p);
/* The byte after "</" is next. */
TXML_PRIVATE void txp_begin_end_tag(txml_parser *p);

/* ------------------------------------------------------------------------
 * doctype.c
 */

extern const state txp_state_doctype;

/* ---- src/arena.c ---- */
/*
 * The linked-chunk arena (memory-architecture.md, "The arena").
 *
 * A chunk is a header followed by its payload, obtained from the backing
 * allocator in one block whose alignment is CHUNK_ALIGNMENT. The header is
 * padded to that alignment so payload offset 0 is aligned for every request
 * up to it; a request for more is honoured by rounding the address instead.
 */

#define CHUNK_ALIGNMENT ((size_t)64u)
#define CHUNK_GROWTH_CAP ((size_t)1048576u)

struct txml_arena_chunk {
    txml_arena_chunk *prev;   /* the chunk allocated before this one */
    txml_arena_chunk *spare;  /* a released chunk kept for the next growth */
    size_t capacity;          /* payload bytes */
    size_t offset;            /* bump offset into the payload */
    size_t used_base;         /* arena `used` when this chunk became the head */
};

#define CHUNK_HEADER \
    (((sizeof(struct txml_arena_chunk) + CHUNK_ALIGNMENT - 1u) / CHUNK_ALIGNMENT) * CHUNK_ALIGNMENT)

static unsigned char *chunk_payload(txml_arena_chunk *chunk) {
    return (unsigned char *)chunk + CHUNK_HEADER;
}

static void chunk_free(const txml_arena *arena, txml_arena_chunk *chunk) {
    txml_allocator_free_params params = {
        .ptr = chunk,
        .size = CHUNK_HEADER + chunk->capacity,
        .alignment = CHUNK_ALIGNMENT,
    };
    arena->backing.v->free(arena->backing.self, &params);
}

/* Free `chunk` and the spare it holds. Iterative: a spare never has a spare. */
static void chunk_release(txml_arena *arena, txml_arena_chunk *chunk) {
    txml_arena_chunk *spare = chunk->spare;
    arena->reserved -= CHUNK_HEADER + chunk->capacity;
    if (spare != NULL) {
        arena->reserved -= CHUNK_HEADER + spare->capacity;
        chunk_free(arena, spare);
    }
    chunk_free(arena, chunk);
}

static txml_arena_chunk *chunk_new(txml_arena *arena, size_t capacity) {
    if (capacity > (size_t)-1 - CHUNK_HEADER) {
        return NULL;
    }
    txml_arena_chunk *chunk = (txml_arena_chunk *)arena->backing.v->alloc(
        arena->backing.self, CHUNK_HEADER + capacity, CHUNK_ALIGNMENT);
    if (chunk == NULL) {
        return NULL;
    }
    chunk->prev = NULL;
    chunk->spare = NULL;
    chunk->capacity = capacity;
    chunk->offset = 0u;
    chunk->used_base = 0u;
    arena->reserved += CHUNK_HEADER + capacity;
    return chunk;
}

typedef struct {
    size_t size;
    size_t alignment;
} bump_request;

/* Bump inside `chunk`. Returns NULL when the request does not fit. */
static void *chunk_bump(txml_arena *arena, txml_arena_chunk *chunk, const bump_request *request) {
    const size_t size = request->size;
    const size_t alignment = request->alignment;
    const uintptr_t base = (uintptr_t)chunk_payload(chunk);
    const uintptr_t mask = (uintptr_t)alignment - 1u;
    const uintptr_t address = (base + chunk->offset + mask) & ~mask;
    const size_t start = (size_t)(address - base);

    if (start > chunk->capacity || size > chunk->capacity - start) {
        return NULL;
    }
    arena->used += (start + size) - chunk->offset;
    chunk->offset = start + size;
    return (void *)address;
}

TXML_API uint32_t txml_version(void) {
    return TXML_VERSION;
}

TXML_API txml_result txml_arena_init(txml_arena *arena, txml_allocator backing, size_t chunk_size) {
    if (arena == NULL || backing.v == NULL || backing.v->alloc == NULL || backing.v->free == NULL) {
        return TXML_ERR_INVALID;
    }
    arena->backing = backing;
    arena->head = NULL;
    arena->chunk_size = chunk_size == 0u ? TXML_ARENA_CHUNK_DEFAULT : chunk_size;
    arena->used = 0u;
    arena->reserved = 0u;
    return TXML_OK;
}

TXML_API void *txml_arena_alloc(txml_arena *arena, size_t size, size_t alignment) {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        return NULL;
    }
    if (arena->head != NULL) {
        void *ptr = chunk_bump(arena, arena->head, &(bump_request){ size, alignment });
        if (ptr != NULL) {
            return ptr;
        }
    }

    /* Room for the worst-case alignment padding when the chunk's own
     * alignment is not enough. */
    const size_t slack = alignment > CHUNK_ALIGNMENT ? alignment : 0u;
    if (size > (size_t)-1 - slack) {
        return NULL;
    }
    const size_t needed = size + slack;

    if (needed > arena->chunk_size) {
        /* Oversized: a chunk of exactly this request, linked behind the head
         * so the head's remaining space stays usable. */
        txml_arena_chunk *big = chunk_new(arena, needed);
        if (big == NULL) {
            return NULL;
        }
        if (arena->head == NULL) {
            arena->head = big;
        } else {
            big->prev = arena->head->prev;
            arena->head->prev = big;
        }
        big->used_base = arena->used;
        return chunk_bump(arena, big, &(bump_request){ size, alignment });
    }

    txml_arena_chunk *chunk = NULL;
    if (arena->head != NULL && arena->head->spare != NULL && arena->head->spare->capacity >= needed) {
        chunk = arena->head->spare;
        arena->head->spare = NULL;
        chunk->offset = 0u;
    } else {
        chunk = chunk_new(arena, arena->chunk_size);
        if (chunk == NULL) {
            return NULL;
        }
        if (arena->chunk_size < CHUNK_GROWTH_CAP) {
            arena->chunk_size *= 2u;
            if (arena->chunk_size > CHUNK_GROWTH_CAP) {
                arena->chunk_size = CHUNK_GROWTH_CAP;
            }
        }
    }
    chunk->prev = arena->head;
    chunk->used_base = arena->used;
    arena->head = chunk;
    return chunk_bump(arena, chunk, &(bump_request){ size, alignment });
}

TXML_API txml_arena_mark txml_arena_position(const txml_arena *arena) {
    txml_arena_mark mark = { arena->head, arena->head == NULL ? 0u : arena->head->offset };
    return mark;
}

TXML_API void txml_arena_release(txml_arena *arena, txml_arena_mark mark) {
    txml_arena_chunk *spare = NULL;
    txml_arena_chunk *chunk = arena->head;

    while (chunk != mark.chunk && chunk != NULL) {
        txml_arena_chunk *prev = chunk->prev;
        if (spare == NULL && mark.chunk != NULL) {
            /* Keep one regular chunk for the next growth. */
            if (chunk->spare != NULL) {
                arena->reserved -= CHUNK_HEADER + chunk->spare->capacity;
                chunk_free(arena, chunk->spare);
                chunk->spare = NULL;
            }
            spare = chunk;
        } else {
            chunk_release(arena, chunk);
        }
        chunk = prev;
    }

    arena->head = mark.chunk;
    if (mark.chunk == NULL) {
        arena->used = 0u;
        return;
    }
    mark.chunk->offset = mark.offset;
    arena->used = mark.chunk->used_base + mark.offset;
    if (spare != NULL) {
        if (mark.chunk->spare != NULL) {
            chunk_release(arena, mark.chunk->spare);
        }
        mark.chunk->spare = spare;
    }
}

TXML_API void txml_arena_reset(txml_arena *arena) {
    txml_arena_chunk *chunk = arena->head;
    while (chunk != NULL) {
        txml_arena_chunk *prev = chunk->prev;
        chunk_release(arena, chunk);
        chunk = prev;
    }
    arena->head = NULL;
    arena->used = 0u;
    arena->reserved = 0u;
}

TXML_API void txml_arena_destroy(txml_arena *arena) {
    txml_arena_reset(arena);
}

/* ---- src/content.c ---- */
/*
 * Content and the constructs that are not tags: text, comments, CDATA
 * sections, processing instructions, and the XML declaration. Each is a state
 * that reads what is available, keeps whatever a chunk boundary could split
 * (a held-back '-', ']' or '?', a partial literal) in the parser, and streams
 * its bytes into a pool run.
 */

/* ------------------------------------------------------------------------
 * Helpers.
 */

static txml_position position_of(txml_parser *p, uint32_t index) {
    txp_commit(p, index);
    return txp_position_now(p);
}

/* Start streaming a run of decoded bytes. */
static void begin_run(txml_parser *p, decode_mode mode) {
    txml_doc_run_begin(p->doc, &p->run);
    txp_dec_reset(p, mode);
    p->sink = &txp_sink_run;
}

/* Feed `bytes` through the decoder. Returns the outcome of the feed. */
static scan feed(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    uint32_t consumed = 0u;
    return txp_dec_feed(p, &(dec_input){ bytes, length }, &consumed);
}

/* Feed the window bytes [from, to) and commit what was taken. */
static scan feed_window(txml_parser *p, uint32_t from, uint32_t to) {
    uint32_t consumed = 0u;
    const scan outcome = txp_dec_feed(p, &(dec_input){ p->window + from, to - from }, &consumed);
    txp_commit(p, from + consumed);
    return outcome;
}

/* ------------------------------------------------------------------------
 * Content: text, and the '<' that ends it.
 */

static scan flush_text(txml_parser *p) {
    scan outcome = txp_dec_end(p);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_TEXT, 0u, TXML_NAME_NONE, p->construct, txp_position_now(p) });
    if (outcome == SCAN_OK) {
        p->text_active = false;
    }
    return outcome;
}

static scan content_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    const uint32_t i = p->start;

    if (w[i] == '<') {
        if (p->text_active) {
            const scan outcome = flush_text(p);
            if (outcome != SCAN_OK) {
                return outcome;
            }
        }
        p->construct = txp_position_now(p);
        txp_commit(p, i + 1u);
        p->state = &txp_state_lt;
        return SCAN_OK;
    }

    if (!p->text_active) {
        if (txp_budget_spent(p)) {
            return SCAN_YIELD;
        }
        p->text_active = true;
        p->construct = txp_position_now(p);
        begin_run(p, DECODE_TEXT);
    }
    uint32_t j = i;
    while (j < limit && w[j] != '<') {
        j += 1u;
    }
    if (p->stage != STAGE_CONTENT) {
        /* Outside the document element only whitespace is character data. */
        for (uint32_t k = i; k < j; k++) {
            if (!txp_is_space(w[k])) {
                return txp_fail(p, p->seen_root ? PROBLEM(TXML_DOC_TRAILING_CONTENT, "content after the document element")
                                                : PROBLEM(TXML_DOC_SYNTAX, "text before the document element"),
                                txp_position_at(p, k));
            }
        }
    }
    return feed_window(p, i, j);
}

static scan content_eof(txml_parser *p) {
    if (p->text_active) {
        const scan outcome = flush_text(p);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    if (p->depth != 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_UNEXPECTED_EOF, "input ended with elements still open"), txp_position_now(p));
    }
    if (!p->seen_root) {
        return txp_fail(p, PROBLEM(TXML_DOC_NO_ROOT, "no document element"), txp_position_now(p));
    }
    txml_doc_complete(p->doc, txp_position_now(p));
    return SCAN_DONE;
}

const state txp_state_content = { "content", content_run, content_eof };

/*
 * The start of the input. A UTF-8 byte order mark is an encoding signature,
 * not content: it is consumed without a node, counted in the offset, and does
 * not move the column. U+FEFF anywhere else is character data. The first
 * character below `valid` is whole, so all three bytes are here. UTF-16 marks
 * are not valid UTF-8 and fail as encoding errors before this state runs.
 */
static scan start_run(txml_parser *p) {
    const uint8_t *w = p->window + p->start;

    if (w[0] == 0xEFu && w[1] == 0xBBu && w[2] == 0xBFu) {
        txp_commit(p, p->start + 3u);
        p->column = 1u;
        p->bom_length = 3u;
    }
    p->state = &txp_state_content;
    return SCAN_OK;
}

const state txp_state_start = { "start", start_run, content_eof };

/* ------------------------------------------------------------------------
 * After '<': which construct is this?
 */

static scan lt_run(txml_parser *p) {
    const uint8_t b = p->window[p->start];

    if (b == '/') {
        txp_commit(p, p->start + 1u);
        txp_begin_end_tag(p);
        return SCAN_OK;
    }
    /* Everything else appends a node, so the budget is checked here; an end
     * tag only patches one and never stalls a step. */
    if (txp_budget_spent(p)) {
        return SCAN_YIELD;
    }
    if (b == '!') {
        txp_commit(p, p->start + 1u);
        p->state = &txp_state_bang;
        return SCAN_OK;
    }
    if (b == '?') {
        txp_commit(p, p->start + 1u);
        p->name_length = 0u;
        p->pi.has_colon = false;
        p->state = &txp_state_pi_target;
        return SCAN_OK;
    }
    return txp_begin_start_tag(p);
}

const state txp_state_lt = { "lt", lt_run, txp_eof_in_markup };

static void set_literal(txml_parser *p, literal_kind kind, const char *expect) {
    p->literal.kind = kind;
    p->literal.expect = expect;
    p->literal.index = 0u;
}

/* After "<!": a comment, a CDATA section, or a DOCTYPE. */
static scan bang_run(txml_parser *p) {
    const uint8_t b = p->window[p->start];

    switch (b) {
    case '-':
        set_literal(p, LIT_COMMENT, "-");
        break;
    case '[':
        set_literal(p, LIT_CDATA, "CDATA[");
        break;
    case 'D':
        set_literal(p, LIT_DOCTYPE, "OCTYPE");
        break;
    default:
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unrecognized markup declaration"), p->construct);
    }
    txp_commit(p, p->start + 1u);
    p->state = &txp_state_literal;
    return SCAN_OK;
}

const state txp_state_bang = { "bang", bang_run, txp_eof_in_markup };

static scan begin_comment(txml_parser *p) {
    begin_run(p, DECODE_VERBATIM);
    p->comment.dashes = 0u;
    p->state = &txp_state_comment;
    return SCAN_OK;
}

static scan begin_cdata(txml_parser *p) {
    if (p->stage != STAGE_CONTENT) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "CDATA outside the document element"), p->construct);
    }
    begin_run(p, DECODE_VERBATIM);
    p->cdata.brackets = 0u;
    p->state = &txp_state_cdata;
    return SCAN_OK;
}

/* The rest of a fixed opener: the second '-', "CDATA[", "OCTYPE". */
static scan literal_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    const uint32_t length = txp_literal_length(p->literal.expect);
    uint32_t i = p->start;

    while (i < limit && p->literal.index < length) {
        if (w[i] != (uint8_t)p->literal.expect[p->literal.index]) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unrecognized markup declaration"), p->construct);
        }
        p->literal.index += 1u;
        i += 1u;
    }
    txp_commit(p, i);
    if (p->literal.index < length) {
        return SCAN_OK;
    }
    switch (p->literal.kind) {
    case LIT_COMMENT:
        return begin_comment(p);
    case LIT_CDATA:
        return begin_cdata(p);
    default:
        return txp_begin_doctype(p);
    }
}

const state txp_state_literal = { "literal", literal_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * Comments. A '-' is held back until the next byte says whether it belongs to
 * the body or to the "-->" that ends it.
 */

static scan comment_run(txml_parser *p) {
    static const uint8_t dash = '-';
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (p->comment.dashes != 0u) {
            if (b == '-') {
                if (p->comment.dashes == 2u) {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), p->comment.first_dash);
                }
                p->comment.dashes = 2u;
                i += 1u;
                continue;
            }
            if (p->comment.dashes == 2u) {
                if (b != '>') {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), p->comment.first_dash);
                }
                outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_COMMENT, 0u, TXML_NAME_NONE, p->construct, txp_position_after(p, i) });
                txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
                if (outcome == SCAN_OK) {
                    p->state = &txp_state_content;
                }
                return outcome;
            }
            outcome = feed(p, &dash, 1u); /* one '-' was just body text */
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->comment.dashes = 0u;
        }
        if (b == '-') {
            p->comment.first_dash = position_of(p, i);
            p->comment.dashes = 1u;
            i += 1u;
            continue;
        }
        uint32_t j = i;
        while (j < limit && w[j] != '-') {
            j += 1u;
        }
        outcome = feed_window(p, i, j);
        if (outcome != SCAN_OK || p->start != j) {
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_comment = { "comment", comment_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * CDATA sections: up to two ']' are held back the same way.
 */

static scan cdata_run(txml_parser *p) {
    static const uint8_t brackets[2] = { ']', ']' };
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (b == ']') {
            if (p->cdata.brackets == 2u) {
                outcome = feed(p, brackets, 1u); /* the oldest ']' is body text */
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
            } else {
                p->cdata.brackets += 1u;
            }
            i += 1u;
            continue;
        }
        if (p->cdata.brackets == 2u && b == '>') {
            outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_TEXT, TXML_NODE_CDATA, TXML_NAME_NONE, p->construct, txp_position_after(p, i) });
            txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
            if (outcome == SCAN_OK) {
                p->state = &txp_state_content;
            }
            return outcome;
        }
        if (p->cdata.brackets != 0u) {
            outcome = feed(p, brackets, p->cdata.brackets);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->cdata.brackets = 0u;
        }
        uint32_t j = i;
        while (j < limit && w[j] != ']') {
            j += 1u;
        }
        outcome = feed_window(p, i, j);
        if (outcome != SCAN_OK || p->start != j) {
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_cdata = { "cdata", cdata_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * Processing instructions.
 */

/* `close` is the index of the '>' that ends the instruction. */
static scan finish_pi(txml_parser *p, uint32_t close) {
    uint32_t name_id = 0u;

    /* The target is interned after the data run is finished, because the pool
     * has one writer at a time. */
    txml_doc_run_finish(p->doc, &p->run);
    const intern_request request = { { 0u, { (const char *)p->name, p->name_length }, { "", 0u } }, p->construct };
    const scan outcome = txp_intern(p, &request, &name_id);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    return txp_append_leaf(p, &(leaf){ TXML_NODE_PI, 0u, name_id, p->construct, txp_position_after(p, close) });
}

static scan begin_declaration(txml_parser *p) {
    p->small.length = 0u;
    p->small_position = p->construct;
    p->small_position.offset += 5u; /* after "<?xml" */
    p->small_position.column += 5u;
    p->declaration.question = 0u;
    p->state = &txp_state_declaration;
    return SCAN_OK;
}

/* The delimiter that ends a PI target has arrived at window[start]. */
static scan target_done(txml_parser *p, uint8_t delimiter) {
    const uint8_t *name = p->name;
    const bool any_xml = p->name_length == 3u && (name[0] | 0x20u) == 'x' && (name[1] | 0x20u) == 'm' && (name[2] | 0x20u) == 'l';

    if (p->pi.has_colon) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a target cannot contain a colon"), p->pi.colon_position);
    }
    if (any_xml) {
        if (txp_bytes_are(name, 3u, "xml") && p->construct.offset == p->bom_length && txp_is_space(delimiter)) {
            return begin_declaration(p);
        }
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML declaration is only allowed at the start of the document"),
                        p->construct);
    }
    if (delimiter == '?') {
        p->pi.question_position = txp_position_now(p);
        txp_commit(p, p->start + 1u);
        p->state = &txp_state_pi_question;
        return SCAN_OK;
    }
    if (txp_is_space(delimiter)) {
        p->state = &txp_state_pi_space;
        return SCAN_OK;
    }
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after the target"), txp_position_now(p));
}

static scan pi_target_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;

    while (i < limit) {
        const uint8_t b = w[i];

        uint32_t width = 0u;
        if (p->name_length == 0u) {
            if (!txp_name_start_at(w + i, &width)) {
                return txp_fail(p, PROBLEM(TXML_DOC_NAME, "expected a processing instruction target"), txp_position_at(p, i));
            }
            p->name_position = position_of(p, i);
        }
        if (!txp_name_char_at(w + i, &width)) {
            if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            }
            txp_commit(p, i);
            return target_done(p, b);
        }
        if (p->name_length + width > p->limits.max_name_length) {
            return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_NAME_LENGTH, "name too long"), p->name_position);
        }
        if (b == ':' && !p->pi.has_colon) {
            p->pi.has_colon = true;
            p->pi.colon_position = position_of(p, i);
        }
        for (uint32_t k = 0u; k < width; k++) {
            p->name[p->name_length++] = w[i + k];
        }
        i += width;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_pi_target = { "pi-target", pi_target_run, txp_eof_in_markup };

/* "<?target?>": nothing may follow the '?' but '>'. */
static scan pi_question_run(txml_parser *p) {
    if (p->window[p->start] != '>') {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after the target"), p->pi.question_position);
    }
    txml_doc_run_begin(p->doc, &p->run);
    const scan outcome = finish_pi(p, p->start);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    txp_commit(p, p->start + 1u);
    p->state = &txp_state_content;
    return SCAN_OK;
}

const state txp_state_pi_question = { "pi-question", pi_question_run, txp_eof_in_markup };

static scan pi_space_run(txml_parser *p) {
    const uint32_t limit = txp_limit(p);
    const uint32_t i = txp_skip_space(p->window, p->start, limit);

    txp_commit(p, i);
    if (i < limit) {
        begin_run(p, DECODE_VERBATIM);
        p->pi.question = 0u;
        p->state = &txp_state_pi_data;
    }
    return SCAN_OK;
}

const state txp_state_pi_space = { "pi-space", pi_space_run, txp_eof_in_markup };

static scan pi_data_run(txml_parser *p) {
    static const uint8_t question = '?';
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (b == '?') {
            if (p->pi.question == 1u) {
                outcome = feed(p, &question, 1u);
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
            }
            p->pi.question = 1u;
            i += 1u;
            continue;
        }
        if (p->pi.question == 1u && b == '>') {
            outcome = finish_pi(p, i);
            txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
            if (outcome == SCAN_OK) {
                p->state = &txp_state_content;
            }
            return outcome;
        }
        if (p->pi.question == 1u) {
            outcome = feed(p, &question, 1u);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->pi.question = 0u;
        }
        uint32_t j = i;
        while (j < limit && w[j] != '?') {
            j += 1u;
        }
        outcome = feed_window(p, i, j);
        if (outcome != SCAN_OK || p->start != j) {
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_pi_data = { "pi-data", pi_data_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * The XML declaration. It is small and has a fixed shape, so it is gathered
 * whole in the small buffer and read from there.
 */

/* The pseudo-attribute values of an XML declaration have fixed shapes:
 * version "1.N", an encoding name, standalone "yes" or "no". Checking them
 * keeps a value from carrying markup that the writer would have to escape. */
static bool declaration_value_valid(uint32_t which, const uint8_t *value, uint32_t length) {
    if (which == 0u) {
        if (length < 3u || value[0] != '1' || value[1] != '.') {
            return false;
        }
        for (uint32_t i = 2u; i < length; i++) {
            if (value[i] < '0' || value[i] > '9') {
                return false;
            }
        }
        return true;
    }
    if (which == 2u) {
        return txp_bytes_are(value, length, "yes") || txp_bytes_are(value, length, "no");
    }
    if (length == 0u || !((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z'))) {
        return false;
    }
    for (uint32_t i = 1u; i < length; i++) {
        const uint8_t b = value[i];
        if (!((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '.' || b == '_' || b == '-')) {
            return false;
        }
    }
    return true;
}

/* `delimiter` is the index of the '>' that ends the declaration. */
static scan finish_declaration(txml_parser *p, uint32_t delimiter) {
    static const char *const names[3] = { "version", "encoding", "standalone" };
    const uint8_t *w = p->small.data;
    const uint32_t close = p->small.length;
    uint32_t i = txp_skip_space(w, 0u, close);
    uint32_t count = 0u;
    uint32_t next_allowed = 0u;
    struct { uint32_t name, offset, length; } parsed[3];

    while (i < close) {
        const uint32_t name_end = txp_scan_name(w, i, close);
        uint32_t which = 3u;
        for (uint32_t k = next_allowed; k < 3u; k++) {
            if (txp_bytes_are(w + i, name_end - i, names[k])) {
                which = k;
                break;
            }
        }
        if (which == 3u || (count == 0u && which != 0u)) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed XML declaration"), txp_small_position_at(p, i));
        }
        next_allowed = which + 1u;
        i = txp_skip_space(w, name_end, close);
        if (i >= close || w[i] != '=') {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected '=' in the XML declaration"), txp_small_position_at(p, i));
        }
        i = txp_skip_space(w, i + 1u, close);
        if (i >= close || (w[i] != '"' && w[i] != '\'')) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "declaration values must be quoted"), txp_small_position_at(p, i));
        }
        uint32_t value_end = i + 1u;
        while (value_end < close && w[value_end] != w[i]) {
            value_end += 1u;
        }
        if (value_end >= close) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unterminated declaration value"), txp_small_position_at(p, i));
        }
        if (!declaration_value_valid(which, w + i + 1u, value_end - i - 1u)) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed XML declaration value"), txp_small_position_at(p, i));
        }
        if (which == 1u) {
            const uint8_t *e = w + i + 1u;
            const uint32_t n = value_end - i - 1u;
            const bool utf8 = n == 5u && (e[0] | 0x20u) == 'u' && (e[1] | 0x20u) == 't' && (e[2] | 0x20u) == 'f' &&
                              e[3] == '-' && e[4] == '8';
            if (!utf8) {
                return txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "only UTF-8 is supported"), txp_small_position_at(p, i));
            }
        }
        parsed[count].name = which;
        parsed[count].offset = i + 1u;
        parsed[count].length = value_end - i - 1u;
        count += 1u;
        i = value_end + 1u;
        const uint32_t after = txp_skip_space(w, i, close);
        if (after == i && after < close) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "declaration values must be separated by whitespace"),
                            txp_small_position_at(p, i));
        }
        i = after;
    }
    if (count == 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML declaration needs a version"), p->construct);
    }

    const snapshot before = txp_snapshot(p);
    const uint32_t index = p->doc->nodes.count;
    const txml_node_append append = {
        .node = {
            .kind = TXML_NODE_DECLARATION, .depth = 1u, .parent = 0u, .end = index + 1u, .name = TXML_NAME_NONE,
            .as.element = { p->doc->attributes.count, count },
        },
        .position = p->construct,
        .end = txp_position_after(p, delimiter),
    };
    uint32_t appended_index = 0u;
    txml_result result = txml_doc_append_node(p->doc, &append, &appended_index);
    for (uint32_t k = 0u; result == TXML_OK && k < count; k++) {
        uint32_t name_id = 0u;
        uint32_t offset = 0u;
        uint32_t attribute_index = 0u;
        const char *name = names[parsed[k].name];
        const txml_intern_params params = { 0u, { name, txp_literal_length(name) }, { "", 0u } };
        result = txml_doc_intern_name(p->doc, &params, &name_id);
        if (result == TXML_OK) {
            result = txml_doc_pool_append(p->doc, (txml_str){ (const char *)w + parsed[k].offset, parsed[k].length }, &offset);
        }
        if (result == TXML_OK) {
            /* Parsed from a buffer, so there is no source position: line 0. */
            const txml_attribute_append attribute = {
                { name_id | (k + 1u == count ? TXML_ATTRIBUTE_LAST : 0u), offset, parsed[k].length }, { { 0u, 0u, 0u }, { 0u, 0u, 0u }, { 0u, 0u, 0u } } };
            result = txml_doc_append_attribute(p->doc, &attribute, &attribute_index);
        }
    }
    if (result != TXML_OK) {
        txp_rollback_nodes(p, &before);
        return txp_fail_result(p, result, p->construct);
    }
    p->appended += 1u;
    return txp_check_budget(p, p->construct);
}

static scan declaration_run(txml_parser *p) {
    static const uint8_t question = '?';
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];

        if (b == '?') {
            if (p->declaration.question == 1u) {
                outcome = txp_buffer_append(p, &question, 1u);
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
            }
            p->declaration.question = 1u;
            i += 1u;
            continue;
        }
        if (p->declaration.question == 1u && b == '>') {
            outcome = finish_declaration(p, i);
            txp_commit(p, outcome == SCAN_OK ? i + 1u : i);
            if (outcome == SCAN_OK) {
                p->state = &txp_state_content;
            }
            return outcome;
        }
        if (p->declaration.question == 1u) {
            outcome = txp_buffer_append(p, &question, 1u);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            p->declaration.question = 0u;
        }
        uint32_t j = i;
        while (j < limit && w[j] != '?') {
            j += 1u;
        }
        outcome = txp_buffer_append(p, w + i, j - i);
        if (outcome != SCAN_OK) {
            txp_commit(p, i);
            return outcome;
        }
        i = j;
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_declaration = { "declaration", declaration_run, txp_eof_in_markup };

/* ---- src/cursor.c ---- */
/*
 * Cursors: navigation, ranges, and the read side of a node. Every function is
 * total (an absent cursor reads as absent) and is integer work on the node
 * column. See docs/internal/cursor.md.
 */

static const txml_node *node_of(txml_cursor cursor) {
    return txml_doc_node(cursor.doc, cursor.node);
}

static bool is_open(const txml_node *node) {
    return (node->flags & TXML_NODE_OPEN) != 0u;
}

TXML_API txml_cursor txml_doc_root(const txml_doc *doc) {
    txml_cursor cursor = { doc, 0u };
    return cursor;
}

TXML_API txml_cursor txml_cursor_at(const txml_doc *doc, uint32_t index) {
    txml_cursor cursor = { doc, index };
    return cursor;
}

TXML_API uint32_t txml_index(txml_cursor cursor) {
    return cursor.node;
}

/* ------------------------------------------------------------------------
 * Movement.
 */

TXML_API txml_move txml_first_child(txml_cursor *cursor) {
    const txml_node *node = node_of(*cursor);
    if (node == NULL) {
        return TXML_NONE;
    }
    const uint32_t next = cursor->node + 1u;
    /* An open node's children arrive as the parse proceeds; `next` exists as
     * soon as one has been appended. */
    const uint32_t limit = is_open(node) ? txml_doc_node_count(cursor->doc) : node->end;
    if (next < limit) {
        cursor->node = next;
        return TXML_MOVED;
    }
    return is_open(node) && !cursor->doc->complete ? TXML_PENDING : TXML_NONE;
}

TXML_API txml_move txml_next_sibling(txml_cursor *cursor) {
    const txml_node *node = node_of(*cursor);
    if (node == NULL || node->parent == TXML_INDEX_NONE) {
        return TXML_NONE;
    }
    if (is_open(node)) {
        return TXML_PENDING;
    }

    const txml_node *parent = txml_doc_node(cursor->doc, node->parent);
    const uint32_t count = txml_doc_node_count(cursor->doc);
    if (is_open(parent)) {
        /* Everything after an open node is inside it, so anything appended
         * past this node is a sibling; nothing yet means "ask again". */
        if (node->end < count) {
            cursor->node = node->end;
            return TXML_MOVED;
        }
        return TXML_PENDING;
    }
    if (node->end < parent->end) {
        cursor->node = node->end;
        return TXML_MOVED;
    }
    return TXML_NONE;
}

TXML_API txml_move txml_parent(txml_cursor *cursor) {
    const txml_node *node = node_of(*cursor);
    if (node == NULL || node->parent == TXML_INDEX_NONE) {
        return TXML_NONE;
    }
    cursor->node = node->parent;
    return TXML_MOVED;
}

TXML_API txml_move txml_next_element(txml_cursor *cursor) {
    txml_cursor probe = *cursor;
    for (;;) {
        const txml_move move = txml_next_sibling(&probe);
        if (move != TXML_MOVED) {
            return move;
        }
        if (node_of(probe)->kind == TXML_NODE_ELEMENT) {
            *cursor = probe;
            return TXML_MOVED;
        }
    }
}

/* ------------------------------------------------------------------------
 * Ranges.
 */

TXML_API uint32_t txml_subtree_end(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL || is_open(node) ? TXML_INDEX_NONE : node->end;
}

TXML_API bool txml_contains(txml_cursor ancestor, txml_cursor node) {
    const uint32_t end = txml_subtree_end(ancestor);
    return ancestor.doc == node.doc && end != TXML_INDEX_NONE && node.node >= ancestor.node && node.node < end;
}

TXML_API txml_move txml_find_next(txml_cursor *cursor, const txml_find *query) {
    if (node_of(*cursor) == NULL || query->name == TXML_NAME_NONE) {
        return TXML_NONE;
    }
    const uint32_t count = txml_doc_node_count(cursor->doc);
    const uint32_t bound = query->bound < count ? query->bound : count;

    for (uint32_t i = cursor->node + 1u; i < bound; i++) {
        const txml_node *node = txml_doc_node(cursor->doc, i);
        if (node->name == query->name && node->kind == TXML_NODE_ELEMENT) {
            cursor->node = i;
            return TXML_MOVED;
        }
    }
    /* Reached the end of what exists; more may still arrive inside the bound. */
    if (query->bound > count && !cursor->doc->complete) {
        return TXML_PENDING;
    }
    return TXML_NONE;
}

/* ------------------------------------------------------------------------
 * Reading.
 */

TXML_API txml_node_kind txml_kind(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL ? TXML_NODE_DOCUMENT : (txml_node_kind)node->kind;
}

TXML_API uint32_t txml_flags(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL ? 0u : node->flags;
}

TXML_API uint32_t txml_depth(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL ? 0u : node->depth;
}

TXML_API uint32_t txml_node_name_id(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    if (node == NULL || (node->kind != TXML_NODE_ELEMENT && node->kind != TXML_NODE_PI)) {
        return TXML_NAME_NONE;
    }
    return node->name;
}

TXML_API txml_str txml_node_prefix(txml_cursor cursor) {
    if (node_of(cursor) == NULL || txml_node_name_id(cursor) == TXML_NAME_NONE) {
        return txml_doc_pool_str(cursor.doc, 0u, 0u);
    }
    return txml_doc_prefix_of(cursor.doc, false, cursor.node);
}

TXML_API txml_str txml_node_text(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    if (node == NULL || (node->kind != TXML_NODE_TEXT && node->kind != TXML_NODE_COMMENT &&
                         node->kind != TXML_NODE_PI && node->kind != TXML_NODE_DOCTYPE)) {
        return txml_doc_pool_str(cursor.doc, 0u, 0u);
    }
    return txml_doc_pool_str(cursor.doc, node->as.text.offset, node->as.text.length);
}

TXML_API txml_result txml_node_position(txml_cursor cursor, txml_position *out) {
    if (node_of(cursor) == NULL) {
        return TXML_ERR_INVALID;
    }
    *out = *txml_doc_position(cursor.doc, cursor.node);
    return TXML_OK;
}

TXML_API txml_result txml_node_span(txml_cursor cursor, txml_span *out) {
    const txml_node *node = node_of(cursor);
    if (node == NULL) {
        return TXML_ERR_INVALID;
    }
    if (is_open(node)) {
        return TXML_ERR_PENDING;
    }
    *out = *txml_doc_span(cursor.doc, cursor.node);
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Attributes.
 */

TXML_API uint32_t txml_attribute_count(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    if (node == NULL || (node->kind != TXML_NODE_ELEMENT && node->kind != TXML_NODE_DECLARATION)) {
        return 0u;
    }
    return node->as.element.attribute_count;
}

TXML_API txml_move txml_first_attribute(txml_cursor cursor, txml_attr_cursor *out) {
    if (txml_attribute_count(cursor) == 0u) {
        return TXML_NONE;
    }
    out->doc = cursor.doc;
    out->index = node_of(cursor)->as.element.first_attribute;
    return TXML_MOVED;
}

TXML_API txml_move txml_next_attribute(txml_attr_cursor *attribute) {
    const txml_attribute_entry *entry = txml_doc_attribute(attribute->doc, attribute->index);
    if (entry == NULL || (entry->name & TXML_ATTRIBUTE_LAST) != 0u) {
        return TXML_NONE;
    }
    attribute->index += 1u;
    return TXML_MOVED;
}

TXML_API txml_result txml_attribute_read(txml_attr_cursor attribute, txml_attribute_view *out) {
    const txml_attribute_entry *entry = txml_doc_attribute(attribute.doc, attribute.index);
    if (entry == NULL) {
        return TXML_ERR_INVALID;
    }
    const uint32_t name = entry->name & TXML_ATTRIBUTE_NAME_MASK;
    out->name = name;
    out->value = txml_doc_pool_str(attribute.doc, entry->value_offset, entry->value_length);
    out->prefix = txml_doc_prefix_of(attribute.doc, true, attribute.index);
    out->flags = txml_doc_attribute_flags(attribute.doc, entry);
    return TXML_OK;
}

/* `count` bytes on from `at`, on the same line. */
static txml_position advance_within_line(txml_position at, uint32_t count) {
    at.offset += count;
    at.column += count;
    return at;
}

TXML_API txml_result txml_attribute_span_read(txml_attr_cursor attribute, txml_attribute_span *out) {
    const txml_attribute_entry *entry = txml_doc_attribute(attribute.doc, attribute.index);
    const txml_attribute_source *source = txml_doc_attribute_source(attribute.doc, attribute.index);
    if (entry == NULL || source == NULL) {
        return TXML_ERR_INVALID;
    }
    if (source->start.line == 0u) {
        return TXML_ERR_NOT_FOUND;
    }
    const txml_name_entry *name = txml_doc_name(attribute.doc, entry->name & TXML_ATTRIBUTE_NAME_MASK);
    if (name == NULL) {
        return TXML_ERR_INVALID;
    }
    const txml_str prefix = txml_doc_prefix_of(attribute.doc, true, attribute.index);
    const uint32_t written = prefix.length + (prefix.length != 0u ? 1u : 0u) + name->local_length;
    out->start = source->start;
    out->name_end = advance_within_line(source->start, written);
    out->value_start = source->value_start;
    /* The closing quote is the byte before `end`, and is never a line end. */
    out->value_end = source->end;
    out->value_end.offset -= 1u;
    out->value_end.column -= 1u;
    out->end = source->end;
    return TXML_OK;
}

TXML_API bool txml_find_attribute(txml_cursor cursor, uint32_t name, txml_str *value) {
    txml_attr_cursor attribute;
    txml_move move = txml_first_attribute(cursor, &attribute);

    while (move == TXML_MOVED) {
        const txml_attribute_entry *entry = txml_doc_attribute(attribute.doc, attribute.index);
        if ((entry->name & TXML_ATTRIBUTE_NAME_MASK) == name) {
            *value = txml_doc_pool_str(attribute.doc, entry->value_offset, entry->value_length);
            return true;
        }
        move = txml_next_attribute(&attribute);
    }
    return false;
}

/* ---- src/decode.c ---- */
/*
 * Decoding: character and entity references, line ends, attribute whitespace.
 *
 * Bytes arrive in pieces, so everything that can be split by a chunk boundary
 * is carried in `parser->dec`: a '\r' waiting to swallow a following '\n', the
 * run of ']' that may become "]]>", and a reference that has seen '&' but not
 * yet ';'. The output goes to a sink, which is either the pending pool run or
 * the small buffer. Entity expansion runs on an explicit frame stack.
 */

/* ------------------------------------------------------------------------
 * Sinks.
 */

static scan run_put(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    const txml_result result = txml_doc_run_append(p->doc, &p->run, (txml_str){ (const char *)bytes, length });
    return result == TXML_OK ? SCAN_OK : txp_fail_result(p, result, p->construct);
}

static scan buffer_put(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    return txp_buffer_append(p, bytes, length);
}

const sink txp_sink_run = { run_put };
const sink txp_sink_buffer = { buffer_put };

TXML_PRIVATE scan txp_buffer_append(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    buffer *b = &p->small;

    if (length > p->limits.max_document_bytes || b->length > p->limits.max_document_bytes - length) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "a declaration exceeds max_document_bytes"), p->construct);
    }
    if (b->length + length > b->capacity) {
        uint64_t capacity = (uint64_t)b->capacity * 2u;
        if (capacity < 256u) {
            capacity = 256u;
        }
        if (capacity < (uint64_t)b->length + length) {
            capacity = (uint64_t)b->length + length;
        }
        if (capacity > UINT32_MAX) {
            capacity = UINT32_MAX;
        }
        uint8_t *bigger = (uint8_t *)txml_arena_alloc(&p->parser_arena, (size_t)capacity, 8u);
        if (bigger == NULL) {
            return SCAN_NOMEM;
        }
        for (uint32_t i = 0u; i < b->length; i++) {
            bigger[i] = b->data[i];
        }
        b->data = bigger;
        b->capacity = (uint32_t)capacity;
    }
    for (uint32_t i = 0u; i < length; i++) {
        b->data[b->length + i] = bytes[i];
    }
    b->length += length;
    return SCAN_OK;
}

/* The position of small.data[index], counting from where the buffer began. */
TXML_PRIVATE txml_position txp_small_position_at(const txml_parser *p, uint32_t index) {
    txml_position position = p->small_position;
    for (uint32_t i = 0u; i < index && i < p->small.length; i++) {
        position.offset += 1u;
        if (p->small.data[i] == '\n') {
            position.line += 1u;
            position.column = 1u;
        } else {
            position.column += 1u;
        }
    }
    return position;
}

/* ------------------------------------------------------------------------
 * References.
 */

static uint32_t encode_utf8(uint32_t code, uint8_t *out) {
    if (code < 0x80u) {
        out[0] = (uint8_t)code;
        return 1u;
    }
    if (code < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (code >> 6));
        out[1] = (uint8_t)(0x80u | (code & 0x3Fu));
        return 2u;
    }
    if (code < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (code >> 12));
        out[1] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (code & 0x3Fu));
        return 3u;
    }
    out[0] = (uint8_t)(0xF0u | (code >> 18));
    out[1] = (uint8_t)(0x80u | ((code >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (code & 0x3Fu));
    return 4u;
}

static bool is_xml_char(uint32_t code) {
    return code == 0x9u || code == 0xAu || code == 0xDu || (code >= 0x20u && code <= 0xD7FFu) ||
           (code >= 0xE000u && code <= 0xFFFDu) || (code >= 0x10000u && code <= 0x10FFFFu);
}

typedef enum { REF_BYTES, REF_ENTITY, REF_MALFORMED, REF_ENCODING, REF_UNDECLARED, REF_NAME } ref_kind;

typedef struct {
    ref_kind kind;
    uint8_t bytes[4];
    uint32_t count;
    const entity *entity;
} ref_result;

/* Parse a numeric character reference body: "#123" or "#x7B". */
static ref_kind parse_character_reference(const uint8_t *name, uint32_t length, uint32_t *code_out) {
    uint32_t code = 0u;
    uint32_t k = 1u;
    uint32_t radix = 10u;

    if (length > 1u && name[1] == 'x') {
        radix = 16u;
        k = 2u;
    }
    if (k == length) {
        return REF_MALFORMED;
    }
    for (; k < length; k++) {
        uint32_t digit;
        if (name[k] >= '0' && name[k] <= '9') {
            digit = (uint32_t)(name[k] - '0');
        } else if (radix == 16u && name[k] >= 'a' && name[k] <= 'f') {
            digit = (uint32_t)(name[k] - 'a') + 10u;
        } else if (radix == 16u && name[k] >= 'A' && name[k] <= 'F') {
            digit = (uint32_t)(name[k] - 'A') + 10u;
        } else {
            return REF_MALFORMED;
        }
        code = code * radix + digit;
        if (code > 0x10FFFFu) {
            return REF_ENCODING;
        }
    }
    if (!is_xml_char(code)) {
        return REF_ENCODING;
    }
    *code_out = code;
    return REF_BYTES;
}

TXML_PRIVATE charref_check txp_check_character_reference(const uint8_t *body, uint32_t length) {
    uint32_t code = 0u;
    const ref_kind kind = parse_character_reference(body, length, &code);
    if (kind == REF_BYTES) {
        return CHARREF_OK;
    }
    return kind == REF_ENCODING ? CHARREF_ENCODING : CHARREF_MALFORMED;
}

static bool is_ref_byte(uint8_t b) {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_' || b == ':' || b == '-' ||
           b == '.' || b == '#';
}

/* Classify the text between '&' and ';'. */
static void classify_reference(const txml_parser *p, txml_str text, ref_result *out) {
    const uint8_t *name = (const uint8_t *)text.ptr;
    const uint32_t length = text.length;

    out->count = 0u;
    out->entity = NULL;
    if (length == 0u) {
        out->kind = REF_MALFORMED;
    } else if (name[0] == '#') {
        uint32_t code = 0u;
        out->kind = parse_character_reference(name, length, &code);
        if (out->kind == REF_BYTES) {
            out->count = encode_utf8(code, out->bytes);
        }
    } else if (txp_bytes_are(name, length, "amp")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '&';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "lt")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '<';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "gt")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '>';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "apos")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '\'';
        out->count = 1u;
    } else if (txp_bytes_are(name, length, "quot")) {
        out->kind = REF_BYTES;
        out->bytes[0] = '"';
        out->count = 1u;
    } else if (txp_scan_name(name, 0u, length) != length) {
        out->kind = REF_NAME;
    } else {
        out->entity = txp_find_entity(p, (txml_str){ (const char *)name, length });
        out->kind = out->entity == NULL ? REF_UNDECLARED : REF_ENTITY;
    }
}

static scan fail_reference(txml_parser *p, ref_kind kind) {
    const txml_position at = p->dec.ref_position;
    switch (kind) {
    case REF_ENCODING:
        return txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "character reference outside the Char production"), at);
    case REF_UNDECLARED:
        return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_ENTITY, "undeclared entity"), at);
    case REF_NAME:
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal entity name"), at);
    default:
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed reference"), at);
    }
}

/* ------------------------------------------------------------------------
 * Entity expansion.
 */

static scan push_frame(txml_parser *p, const entity *e) {
    decoder *d = &p->dec;

    if (d->level >= p->limits.max_depth) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DEPTH, "entities nested too deeply"), d->ref_position);
    }
    for (uint32_t k = 1u; k <= d->level; k++) {
        if (p->frames[k].entity == e) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "recursive entity reference"), d->ref_position);
        }
    }
    d->level += 1u;
    p->frames[d->level] = (frame){ (const uint8_t *)e->value.ptr, e->value.length, 0u, e };
    d->expanding = true;
    return SCAN_OK;
}

/* Everything produced under an entity, nested or not, is charged. */
static scan emit_expansion(txml_parser *p, const uint8_t *bytes, uint32_t length) {
    const scan outcome = p->sink->put(p, bytes, length);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    p->expansion_used += length;
    if (p->expansion_used > p->limits.entity_expansion_budget) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_ENTITY_EXPANSION, "entity expansion exceeds its budget"),
                        p->dec.ref_position);
    }
    return SCAN_OK;
}

static bool frame_special(decode_mode mode, uint8_t b) {
    switch (b) {
    case '\r':
    case '&':
    case '<':
        return true;
    case '\n':
    case '\t':
        return mode == DECODE_ATTRIBUTE;
    case ']':
        return mode == DECODE_TEXT;
    default:
        return false;
    }
}

TXML_PRIVATE scan txp_dec_resume(txml_parser *p) {
    decoder *d = &p->dec;

    while (d->level != 0u) {
        frame *f = &p->frames[d->level];
        if (f->position == f->length) {
            d->level -= 1u;
            continue;
        }

        /* A span of ordinary bytes goes out in one piece. */
        uint32_t stop = f->position;
        while (stop < f->length && !frame_special(d->mode, f->source[stop])) {
            stop += 1u;
        }
        if (stop > f->position) {
            const scan outcome = emit_expansion(p, f->source + f->position, stop - f->position);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            f->position = stop;
            continue;
        }

        const uint8_t b = f->source[f->position];
        if (b == '\r') {
            const uint8_t out = d->mode == DECODE_ATTRIBUTE ? (uint8_t)' ' : (uint8_t)'\n';
            const scan outcome = emit_expansion(p, &out, 1u);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            const bool pair = f->position + 1u < f->length && f->source[f->position + 1u] == '\n';
            f->position += pair ? 2u : 1u;
        } else if (b == '\n' || b == '\t') {
            const uint8_t out = ' ';
            const scan outcome = emit_expansion(p, &out, 1u);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            f->position += 1u;
        } else if (b == '<') {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, d->mode == DECODE_ATTRIBUTE ? "'<' in an attribute value"
                                                                                    : "markup in entity replacement text is not supported"),
                            d->ref_position);
        } else if (b == ']') {
            if (f->position + 2u < f->length && f->source[f->position + 1u] == ']' && f->source[f->position + 2u] == '>') {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "']]>' in character data"), d->ref_position);
            }
            const uint8_t out = ']';
            const scan outcome = emit_expansion(p, &out, 1u);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            f->position += 1u;
        } else { /* '&' */
            uint32_t semicolon = f->position + 1u;
            while (semicolon < f->length && f->source[semicolon] != ';') {
                semicolon += 1u;
            }
            if (semicolon == f->length) {
                return fail_reference(p, REF_MALFORMED);
            }
            ref_result ref;
            classify_reference(p, (txml_str){ (const char *)f->source + f->position + 1u, semicolon - f->position - 1u }, &ref);
            if (ref.kind == REF_BYTES) {
                const scan outcome = emit_expansion(p, ref.bytes, ref.count);
                if (outcome != SCAN_OK) {
                    return outcome;
                }
                f->position = semicolon + 1u;
            } else if (ref.kind == REF_ENTITY) {
                f->position = semicolon + 1u;
                const scan outcome = push_frame(p, ref.entity);
                if (outcome != SCAN_OK) {
                    return outcome;
                }
            } else {
                return fail_reference(p, ref.kind);
            }
        }
    }
    d->expanding = false;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * The decoder proper.
 */

TXML_PRIVATE void txp_dec_reset(txml_parser *p, decode_mode mode) {
    p->dec = (decoder){ .mode = mode };
}

TXML_PRIVATE scan txp_dec_end(txml_parser *p) {
    if (p->dec.in_ref) {
        return fail_reference(p, REF_MALFORMED);
    }
    return SCAN_OK;
}

static bool special(decode_mode mode, uint8_t b) {
    switch (mode) {
    case DECODE_VERBATIM:
        return b == '\r';
    case DECODE_ATTRIBUTE:
        return b == '\r' || b == '\n' || b == '\t' || b == '&' || b == '<';
    default:
        return b == '\r' || b == '&' || b == ']' || b == '>';
    }
}

/* The position of a byte the decoder is looking at: in the window when it
 * came from there, else wherever the construct began. The cursor keeps a run
 * of references in one span from rescanning the span for each. */
static txml_position source_position(const txml_parser *p, position_cursor *cursor, const uint8_t *at) {
    const uintptr_t address = (uintptr_t)at;
    const uintptr_t low = (uintptr_t)p->window;
    if (address >= low && address < low + p->end) {
        return txp_cursor_position(p, cursor, (uint32_t)(address - low));
    }
    return p->construct;
}

/* The ';' of a reference arrived. Returns SCAN_OK when the reference is
 * fully handled. On SCAN_NOMEM, `in_ref` still set means the ';' was not
 * consumed; cleared means an expansion is pending and it was. */
static scan finish_reference(txml_parser *p) {
    decoder *d = &p->dec;
    ref_result ref;

    classify_reference(p, (txml_str){ (const char *)p->reference, d->ref_length }, &ref);
    if (ref.kind == REF_BYTES) {
        const scan outcome = p->sink->put(p, ref.bytes, ref.count);
        if (outcome == SCAN_OK) {
            d->in_ref = false;
        }
        return outcome;
    }
    if (ref.kind == REF_ENTITY) {
        const scan outcome = push_frame(p, ref.entity);
        if (outcome != SCAN_OK) {
            return outcome;
        }
        d->in_ref = false;
        return txp_dec_resume(p);
    }
    return fail_reference(p, ref.kind);
}

TXML_PRIVATE scan txp_dec_feed(txml_parser *p, const dec_input *input, uint32_t *consumed) {
    decoder *d = &p->dec;
    const uint8_t *s = input->bytes;
    const uint32_t n = input->length;
    uint32_t i = 0u;
    uint32_t from = 0u; /* the start of the ordinary bytes not yet emitted */
    position_cursor cursor = { 0u, { 0u, 0u, 0u }, false };
    scan outcome;

    *consumed = 0u;
    if (d->expanding) {
        outcome = txp_dec_resume(p);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    if (d->brackets != 0u && n != 0u && s[0] != ']' && s[0] != '>') {
        d->brackets = 0u;
    }

    while (i < n) {
        const uint8_t b = s[i];

        if (d->in_ref) {
            if (b == ';') {
                outcome = finish_reference(p);
                if (outcome == SCAN_OK) {
                    i += 1u;
                    from = i;
                    continue;
                }
                *consumed = (outcome == SCAN_NOMEM && !d->in_ref) ? i + 1u : i;
                return outcome;
            }
            /* The characters are checked as a whole name when the ';' arrives;
             * here only what could never be part of one is refused. */
            if (d->ref_length >= p->reference_capacity || !(b >= 0x80u || is_ref_byte(b))) {
                return fail_reference(p, REF_MALFORMED);
            }
            p->reference[d->ref_length++] = b;
            i += 1u;
            from = i;
            continue;
        }
        if (d->after_cr) {
            d->after_cr = false;
            if (b == '\n') {
                i += 1u;
                from = i;
                continue;
            }
        }
        if (!special(d->mode, b)) {
            i += 1u;
            continue;
        }

        if (b == ']') {
            /* Take the whole run of ']' with the ordinary bytes before it. */
            uint32_t j = i;
            while (j < n && s[j] == ']') {
                j += 1u;
            }
            outcome = p->sink->put(p, s + from, j - from);
            if (outcome != SCAN_OK) {
                *consumed = from;
                return outcome;
            }
            d->brackets += j - i;
            if (d->brackets > 2u) {
                d->brackets = 2u;
            }
            i = j;
            from = i;
            if (i < n && s[i] != '>') {
                d->brackets = 0u;
            }
            continue;
        }

        if (i > from) {
            outcome = p->sink->put(p, s + from, i - from);
            if (outcome != SCAN_OK) {
                *consumed = from;
                return outcome;
            }
            from = i;
        }

        if (b == '>') {
            if (d->brackets >= 2u) {
                txml_position at = source_position(p, &cursor, s + i);
                at.offset -= 2u;
                at.column -= 2u;
                *consumed = i;
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "']]>' in character data"), at);
            }
            d->brackets = 0u;
            i += 1u; /* an ordinary byte; it goes out with the next span */
        } else if (b == '&') {
            d->in_ref = true;
            d->ref_length = 0u;
            d->ref_position = source_position(p, &cursor, s + i);
            d->brackets = 0u;
            i += 1u;
            from = i;
        } else if (b == '<') {
            *consumed = i;
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'<' in an attribute value"), source_position(p, &cursor, s + i));
        } else { /* '\r', or attribute whitespace */
            const uint8_t out = (d->mode == DECODE_ATTRIBUTE || b == '\t') ? (uint8_t)' ' : (uint8_t)'\n';
            outcome = p->sink->put(p, &out, 1u);
            if (outcome != SCAN_OK) {
                *consumed = i;
                return outcome;
            }
            d->after_cr = b == '\r';
            d->brackets = 0u;
            i += 1u;
            from = i;
        }
    }

    if (i > from) {
        outcome = p->sink->put(p, s + from, i - from);
        if (outcome != SCAN_OK) {
            *consumed = from;
            return outcome;
        }
    }
    *consumed = n;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Entity literals: character references are read when the entity is declared.
 */

/* Where in the small buffer `at` points, for an error position. */
static txml_position literal_position(const txml_parser *p, const uint8_t *at) {
    if (p->small.data != NULL && at >= p->small.data && at < p->small.data + p->small.length) {
        return txp_small_position_at(p, (uint32_t)(at - p->small.data));
    }
    return p->construct;
}

TXML_PRIVATE scan txp_expand_character_references(txml_parser *p, txml_str literal, txml_str *expanded) {
    const uint8_t *s = (const uint8_t *)literal.ptr;
    uint8_t *out = literal.length == 0u ? NULL : (uint8_t *)txml_arena_alloc(&p->parser_arena, literal.length, 1u);
    uint32_t n = 0u;
    uint32_t i = 0u;

    if (literal.length != 0u && out == NULL) {
        return SCAN_NOMEM;
    }
    while (i < literal.length) {
        if (s[i] != '&' || i + 1u >= literal.length || s[i + 1u] != '#') {
            out[n++] = s[i++];
            continue;
        }
        uint32_t semicolon = i + 2u;
        while (semicolon < literal.length && s[semicolon] != ';') {
            semicolon += 1u;
        }
        uint32_t code = 0u;
        if (semicolon == literal.length) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed character reference"), literal_position(p, s + i));
        }
        const ref_kind kind = parse_character_reference(s + i + 1u, semicolon - i - 1u, &code);
        if (kind == REF_ENCODING) {
            return txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "character reference outside the Char production"),
                            literal_position(p, s + i));
        }
        if (kind != REF_BYTES) {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "malformed character reference"), literal_position(p, s + i));
        }
        /* A reference is never longer than the bytes it stands for. */
        n += encode_utf8(code, out + n);
        i = semicolon + 1u;
    }
    *expanded = (txml_str){ out == NULL ? "" : (const char *)out, n };
    return SCAN_OK;
}

/* ---- src/doc.c ---- */
/*
 * The document: paged columns over two arenas, the byte pool, and name
 * interning. See docs/internal/document-layout.md and memory-architecture.md.
 */

#define COLUMN_PAGE_SHIFT 12u
#define COLUMN_PAGE_ENTRIES ((uint32_t)1u << COLUMN_PAGE_SHIFT)
#define COLUMN_PAGE_MASK (COLUMN_PAGE_ENTRIES - 1u)
#define NAME_SLOTS_INITIAL 64u
#define POOL_PAGE_MIN 1024u
#define POOL_PAGE_MAX 65536u

/* ------------------------------------------------------------------------
 * Columns.
 */

typedef struct {
    uint32_t entry_size;
    uint64_t max_document_bytes;
} column_shape;

static txml_result column_init(txml_column *column, txml_arena *arena, const column_shape *shape) {
    const uint32_t entry_size = shape->entry_size;
    const uint64_t max_document_bytes = shape->max_document_bytes;
    const uint64_t page_bytes = (uint64_t)COLUMN_PAGE_ENTRIES * entry_size;
    const uint64_t capacity = max_document_bytes / page_bytes + 1u;

    column->directory = (void **)txml_arena_alloc(arena, (size_t)capacity * sizeof(void *), sizeof(void *));
    if (column->directory == NULL) {
        return TXML_ERR_NOMEM;
    }
    for (uint64_t i = 0u; i < capacity; i++) {
        column->directory[i] = NULL;
    }
    column->directory_capacity = (uint32_t)capacity;
    column->count = 0u;
    column->entry_size = entry_size;
    return TXML_OK;
}

static void *column_entry(const txml_column *column, uint32_t index) {
    unsigned char *page = (unsigned char *)column->directory[index >> COLUMN_PAGE_SHIFT];
    return page + (size_t)(index & COLUMN_PAGE_MASK) * column->entry_size;
}

static void *column_at(const txml_column *column, uint32_t index) {
    return index < column->count ? column_entry(column, index) : NULL;
}

static txml_result column_push(txml_column *column, txml_arena *arena, void **entry) {
    const uint32_t page = column->count >> COLUMN_PAGE_SHIFT;

    if (page >= column->directory_capacity || column->count >= TXML_INDEX_NONE - 1u) {
        return TXML_ERR_LIMIT;
    }
    if (column->directory[page] == NULL) {
        void *fresh = txml_arena_alloc(arena, (size_t)COLUMN_PAGE_ENTRIES * column->entry_size, 8u);
        if (fresh == NULL) {
            return TXML_ERR_NOMEM;
        }
        column->directory[page] = fresh;
    }
    *entry = column_entry(column, column->count);
    column->count += 1u;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * The pool.
 */

#define POOL_DIRECTORY_INITIAL 16u

/* The most slots the directory needs: every page of the largest document the
 * limits allow, and the pages an oversized run skips to. */
static uint32_t pool_directory_limit(const txml_doc *doc) {
    return (uint32_t)(doc->limits.max_document_bytes >> doc->pool_shift) + 2u;
}

/* Room in the pool directory for slot `last`. The directory grows with the
 * document, doubling: a new array in the values arena takes the page
 * pointers, and the old one is abandoned there (counted in `used`, about
 * the size of the final one in all). The pages do not move, so no offset or
 * txml_str changes; only the index to them is copied. */
static txml_result pool_directory_reserve(txml_doc *doc, uint32_t last) {
    if (last < doc->pool_directory_capacity) {
        return TXML_OK;
    }
    const uint32_t limit = pool_directory_limit(doc);
    if (last >= limit) {
        return TXML_ERR_LIMIT;
    }
    uint64_t capacity = doc->pool_directory_capacity == 0u ? POOL_DIRECTORY_INITIAL : doc->pool_directory_capacity;
    while (capacity <= last) {
        capacity *= 2u;
    }
    if (capacity > limit) {
        capacity = limit;
    }
    unsigned char **directory = (unsigned char **)txml_arena_alloc(
        &doc->values, (size_t)capacity * sizeof(unsigned char *), sizeof(void *));
    if (directory == NULL) {
        return TXML_ERR_NOMEM;
    }
    uint32_t i = 0u;
    for (; i < doc->pool_directory_capacity; i++) {
        directory[i] = doc->pool_directory[i];
    }
    for (; i < (uint32_t)capacity; i++) {
        directory[i] = NULL;
    }
    doc->pool_directory = directory;
    doc->pool_directory_capacity = (uint32_t)capacity;
    return TXML_OK;
}

TXML_PRIVATE txml_str txml_doc_pool_str(const txml_doc *doc, uint32_t offset, uint32_t length) {
    static const char empty[1] = { 0 };
    txml_str out = { empty, 0u };

    if (length == 0u || offset > doc->pool_length || length > doc->pool_length - offset) {
        return out;
    }
    const uint32_t mask = ((uint32_t)1u << doc->pool_shift) - 1u;
    out.ptr = (const char *)(doc->pool_directory[offset >> doc->pool_shift] + (offset & mask));
    out.length = length;
    return out;
}

TXML_PRIVATE txml_result txml_doc_pool_append(txml_doc *doc, txml_str bytes, uint32_t *offset) {
    const uint32_t page_size = (uint32_t)1u << doc->pool_shift;
    const uint32_t mask = page_size - 1u;
    const uint32_t length = bytes.length;
    uint32_t start = doc->pool_length;

    if (length == 0u) {
        *offset = start;
        return TXML_OK;
    }

    /* A run that does not fit the rest of its page starts the next one; a run
     * larger than a page always starts on a boundary. */
    if (length > page_size || (start & mask) + length > page_size) {
        if (start & mask) {
            const uint32_t skip = page_size - (start & mask);
            if (start > UINT32_MAX - skip) {
                return TXML_ERR_LIMIT;
            }
            start += skip;
        }
    }
    if (start > UINT32_MAX - length) {
        return TXML_ERR_LIMIT;
    }

    const uint32_t first = start >> doc->pool_shift;
    const uint32_t last = (start + length - 1u) >> doc->pool_shift;
    const txml_result reserved = pool_directory_reserve(doc, last);
    if (reserved != TXML_OK) {
        return reserved;
    }

    if (length > page_size) {
        /* One block over consecutive slots keeps the run contiguous. */
        const uint32_t pages = last - first + 1u;
        unsigned char *block = (unsigned char *)txml_arena_alloc(&doc->values, (size_t)pages * page_size, 8u);
        if (block == NULL) {
            return TXML_ERR_NOMEM;
        }
        for (uint32_t k = 0u; k < pages; k++) {
            doc->pool_directory[first + k] = block + (size_t)k * page_size;
        }
    } else if (doc->pool_directory[first] == NULL) {
        unsigned char *page = (unsigned char *)txml_arena_alloc(&doc->values, page_size, 8u);
        if (page == NULL) {
            return TXML_ERR_NOMEM;
        }
        doc->pool_directory[first] = page;
    }

    unsigned char *destination = doc->pool_directory[first] + (start & mask);
    for (uint32_t i = 0u; i < length; i++) {
        destination[i] = (unsigned char)bytes.ptr[i];
    }
    doc->pool_length = start + length;
    *offset = start;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * The run builder.
 */

#define RUN_MINIMUM_CAPACITY 64u

TXML_PRIVATE void txml_doc_run_begin(txml_doc *doc, txml_run *run) {
    run->offset = doc->pool_length;
    run->length = 0u;
    run->capacity = 0u;
}

static unsigned char *run_base(const txml_doc *doc, uint32_t offset) {
    const uint32_t mask = ((uint32_t)1u << doc->pool_shift) - 1u;
    return doc->pool_directory[offset >> doc->pool_shift] + (offset & mask);
}

TXML_PRIVATE txml_result txml_doc_run_reserve(txml_doc *doc, txml_run *run, uint32_t extra) {
    const uint32_t page_size = (uint32_t)1u << doc->pool_shift;
    const uint32_t mask = page_size - 1u;

    if (extra > UINT32_MAX - run->length) {
        return TXML_ERR_LIMIT;
    }
    const uint32_t need = run->length + extra;
    if (need <= run->capacity || need == 0u) {
        return TXML_OK;
    }

    uint64_t base = run->offset;
    if (run->capacity == 0u) {
        /* First placement: use the tail of the current page when it is big
         * enough, and start a fresh page rather than crowd into a sliver. */
        const uint32_t start = run->offset;
        const uint32_t remaining = page_size - (start & mask);
        const uint32_t wanted = need < RUN_MINIMUM_CAPACITY ? RUN_MINIMUM_CAPACITY : need;
        if (wanted <= remaining) {
            const uint32_t slot = start >> doc->pool_shift;
            const txml_result reserved = pool_directory_reserve(doc, slot);
            if (reserved != TXML_OK) {
                return reserved;
            }
            if (doc->pool_directory[slot] == NULL) {
                unsigned char *page = (unsigned char *)txml_arena_alloc(&doc->values, page_size, 8u);
                if (page == NULL) {
                    return TXML_ERR_NOMEM;
                }
                doc->pool_directory[slot] = page;
            }
            run->capacity = remaining;
            return TXML_OK;
        }
    } else {
        base += run->capacity;
    }

    /* Move to a fresh block of whole pages after the old space. */
    const uint64_t begin = (base + mask) & ~(uint64_t)mask;
    uint64_t want = run->capacity == 0u ? need : (uint64_t)run->capacity * 2u;
    if (want < need) {
        want = need;
    }
    if (want < page_size) {
        want = page_size;
    }
    const uint64_t pages = (want + mask) >> doc->pool_shift;
    const uint64_t capacity = pages << doc->pool_shift;
    if (begin + capacity > UINT32_MAX || (begin >> doc->pool_shift) + pages > pool_directory_limit(doc)) {
        return TXML_ERR_LIMIT;
    }
    if ((uint64_t)doc->values.used + doc->tree.used + capacity > doc->limits.max_document_bytes) {
        return TXML_ERR_LIMIT;
    }
    const txml_result reserved = pool_directory_reserve(doc, (uint32_t)((begin >> doc->pool_shift) + pages - 1u));
    if (reserved != TXML_OK) {
        return reserved;
    }
    unsigned char *block = (unsigned char *)txml_arena_alloc(&doc->values, (size_t)capacity, 8u);
    if (block == NULL) {
        return TXML_ERR_NOMEM;
    }
    if (run->length != 0u) {
        const unsigned char *old = run_base(doc, run->offset);
        for (uint32_t i = 0u; i < run->length; i++) {
            block[i] = old[i];
        }
    }
    const uint32_t first = (uint32_t)(begin >> doc->pool_shift);
    for (uint32_t k = 0u; k < (uint32_t)pages; k++) {
        doc->pool_directory[first + k] = block + (size_t)k * page_size;
    }
    run->offset = (uint32_t)begin;
    run->capacity = (uint32_t)capacity;
    return TXML_OK;
}

TXML_PRIVATE txml_result txml_doc_run_append(txml_doc *doc, txml_run *run, txml_str bytes) {
    if (bytes.length == 0u) {
        return TXML_OK;
    }
    const txml_result result = txml_doc_run_reserve(doc, run, bytes.length);
    if (result != TXML_OK) {
        return result;
    }
    unsigned char *destination = run_base(doc, run->offset) + run->length;
    for (uint32_t i = 0u; i < bytes.length; i++) {
        destination[i] = (unsigned char)bytes.ptr[i];
    }
    run->length += bytes.length;
    return TXML_OK;
}

TXML_PRIVATE txml_str txml_doc_run_view(const txml_doc *doc, const txml_run *run) {
    static const char empty[1] = { 0 };
    if (run->length == 0u) {
        return (txml_str){ empty, 0u };
    }
    return (txml_str){ (const char *)run_base(doc, run->offset), run->length };
}

TXML_PRIVATE void txml_doc_run_finish(txml_doc *doc, const txml_run *run) {
    /* Never backwards: a retry after other pool appends must not rewind them. */
    if (run->length != 0u && run->offset + run->length > doc->pool_length) {
        doc->pool_length = run->offset + run->length;
    }
}

/* ------------------------------------------------------------------------
 * Names and namespaces.
 */

static uint32_t hash_step(uint32_t hash, unsigned char byte) {
    return (hash ^ byte) * 16777619u;
}

static uint32_t name_hash(txml_str uri, txml_str local) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0u; i < uri.length; i++) {
        hash = hash_step(hash, (unsigned char)uri.ptr[i]);
    }
    hash = hash_step(hash, 0xFFu);
    for (uint32_t i = 0u; i < local.length; i++) {
        hash = hash_step(hash, (unsigned char)local.ptr[i]);
    }
    return hash;
}

static txml_str namespace_uri(const txml_doc *doc, uint32_t id) {
    const txml_namespace_entry *entry = (const txml_namespace_entry *)column_at(&doc->namespaces, id);
    return entry == NULL ? txml_doc_pool_str(doc, 0u, 0u) : txml_doc_pool_str(doc, entry->uri_offset, entry->uri_length);
}

static uint32_t find_namespace(const txml_doc *doc, txml_str uri) {
    for (uint32_t id = 0u; id < doc->namespaces.count; id++) {
        if (txml_str_equal(namespace_uri(doc, id), uri)) {
            return id;
        }
    }
    return TXML_NAME_NONE;
}

TXML_PRIVATE txml_result txml_doc_intern_namespace(txml_doc *doc, txml_str uri, uint32_t *id) {
    const uint32_t found = find_namespace(doc, uri);
    if (found != TXML_NAME_NONE) {
        *id = found;
        return TXML_OK;
    }

    /* Namespaces are found by scanning, so their number is bounded like names. */
    if (doc->namespaces.count >= doc->limits.max_names) {
        return TXML_ERR_LIMIT;
    }
    uint32_t offset = 0u;
    txml_result result = txml_doc_pool_append(doc, uri, &offset);
    if (result != TXML_OK) {
        return result;
    }
    void *slot = NULL;
    result = column_push(&doc->namespaces, &doc->values, &slot);
    if (result != TXML_OK) {
        return result;
    }
    txml_namespace_entry *entry = (txml_namespace_entry *)slot;
    entry->uri_offset = offset;
    entry->uri_length = uri.length;
    *id = doc->namespaces.count - 1u;
    return TXML_OK;
}

typedef struct {
    uint32_t namespace_id;
    txml_str local;
    uint32_t hash;
} name_key;

static uint32_t probe_name(const txml_doc *doc, const name_key *key, uint32_t *free_slot) {
    const uint32_t namespace_id = key->namespace_id;
    const txml_str local = key->local;
    const uint32_t hash = key->hash;
    const uint32_t mask = doc->name_slot_capacity - 1u;
    uint32_t slot = hash & mask;

    for (;;) {
        const uint32_t id = doc->name_slots[slot];
        if (id == TXML_NAME_NONE) {
            *free_slot = slot;
            return TXML_NAME_NONE;
        }
        const txml_name_entry *entry = (const txml_name_entry *)column_entry(&doc->names, id);
        if (entry->hash == hash && entry->namespace_id == namespace_id &&
            txml_str_equal(txml_doc_pool_str(doc, entry->local_offset, entry->local_length), local)) {
            return id;
        }
        slot = (slot + 1u) & mask;
    }
}

static txml_result grow_name_slots(txml_doc *doc) {
    const uint32_t capacity = doc->name_slot_capacity * 2u;
    uint32_t *slots = (uint32_t *)txml_arena_alloc(&doc->values, (size_t)capacity * sizeof(uint32_t), sizeof(uint32_t));
    if (slots == NULL) {
        return TXML_ERR_NOMEM;
    }
    for (uint32_t i = 0u; i < capacity; i++) {
        slots[i] = TXML_NAME_NONE;
    }
    /* Only IDs move between tables; no address a caller holds changes. */
    for (uint32_t id = 0u; id < doc->names.count; id++) {
        const txml_name_entry *entry = (const txml_name_entry *)column_entry(&doc->names, id);
        uint32_t slot = entry->hash & (capacity - 1u);
        while (slots[slot] != TXML_NAME_NONE) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        slots[slot] = id;
    }
    doc->name_slots = slots;
    doc->name_slot_capacity = capacity;
    return TXML_OK;
}

TXML_PRIVATE txml_result txml_doc_intern_name(txml_doc *doc, const txml_intern_params *params, uint32_t *id) {
    if (params->namespace_id >= doc->namespaces.count) {
        return TXML_ERR_INVALID;
    }
    const uint32_t hash = name_hash(namespace_uri(doc, params->namespace_id), params->local);
    uint32_t free_slot = 0u;
    const uint32_t found = probe_name(doc, &(name_key){ params->namespace_id, params->local, hash }, &free_slot);
    if (found != TXML_NAME_NONE) {
        *id = found;
        return TXML_OK;
    }

    if (doc->names.count >= doc->limits.max_names || doc->names.count >= TXML_NAME_ID_LIMIT - 1u) {
        return TXML_ERR_LIMIT;
    }
    if ((doc->names.count + 1u) * 2u > doc->name_slot_capacity) {
        const txml_result grown = grow_name_slots(doc);
        if (grown != TXML_OK) {
            return grown;
        }
        probe_name(doc, &(name_key){ params->namespace_id, params->local, hash }, &free_slot);
    }

    uint32_t local_offset = 0u;
    uint32_t prefix_offset = 0u;
    txml_result result = txml_doc_pool_append(doc, params->local, &local_offset);
    if (result == TXML_OK) {
        result = txml_doc_pool_append(doc, params->prefix, &prefix_offset);
    }
    void *slot = NULL;
    if (result == TXML_OK) {
        result = column_push(&doc->names, &doc->values, &slot);
    }
    if (result != TXML_OK) {
        return result;
    }

    txml_name_entry *entry = (txml_name_entry *)slot;
    entry->namespace_id = params->namespace_id;
    entry->local_offset = local_offset;
    entry->local_length = params->local.length;
    entry->prefix_offset = prefix_offset;
    entry->prefix_length = params->prefix.length;
    entry->hash = hash;
    *id = doc->names.count - 1u;
    doc->name_slots[free_slot] = *id;
    return TXML_OK;
}

TXML_API uint32_t txml_doc_find_name(const txml_doc *doc, const txml_qname *name) {
    if (doc == NULL || name == NULL) {
        return TXML_NAME_NONE;
    }
    const uint32_t namespace_id = find_namespace(doc, name->ns);
    if (namespace_id == TXML_NAME_NONE) {
        return TXML_NAME_NONE;
    }
    uint32_t unused = 0u;
    return probe_name(doc, &(name_key){ namespace_id, name->local, name_hash(name->ns, name->local) }, &unused);
}

TXML_API uint32_t txml_doc_name_count(const txml_doc *doc) {
    return doc == NULL ? 0u : doc->names.count;
}

TXML_API txml_result txml_doc_name_at(const txml_doc *doc, uint32_t name, txml_qname *out) {
    const txml_name_entry *entry = txml_doc_name(doc, name);
    if (entry == NULL) {
        return TXML_ERR_NOT_FOUND;
    }
    out->ns = namespace_uri(doc, entry->namespace_id);
    out->local = txml_doc_pool_str(doc, entry->local_offset, entry->local_length);
    return TXML_OK;
}

TXML_PRIVATE const txml_namespace_entry *txml_doc_namespace(const txml_doc *doc, uint32_t id) {
    return doc == NULL ? NULL : (const txml_namespace_entry *)column_at(&doc->namespaces, id);
}

TXML_PRIVATE const txml_name_entry *txml_doc_name(const txml_doc *doc, uint32_t id) {
    return doc == NULL ? NULL : (const txml_name_entry *)column_at(&doc->names, id);
}

/* ------------------------------------------------------------------------
 * Nodes and attributes.
 */

TXML_PRIVATE const txml_node *txml_doc_node(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_node *)column_at(&doc->nodes, index);
}

TXML_PRIVATE const txml_attribute_entry *txml_doc_attribute(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_attribute_entry *)column_at(&doc->attributes, index);
}

TXML_PRIVATE const txml_span *txml_doc_span(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_span *)column_at(&doc->positions, index);
}

TXML_PRIVATE const txml_position *txml_doc_position(const txml_doc *doc, uint32_t index) {
    const txml_span *span = txml_doc_span(doc, index);
    return span == NULL ? NULL : &span->start;
}

TXML_PRIVATE const txml_attribute_source *txml_doc_attribute_source(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_attribute_source *)column_at(&doc->attribute_sources, index);
}

TXML_PRIVATE uint32_t txml_doc_attribute_flags(const txml_doc *doc, const txml_attribute_entry *attribute) {
    static const char xmlns[] = "http://www.w3.org/2000/xmlns/";
    const txml_name_entry *entry = txml_doc_name(doc, attribute->name & TXML_ATTRIBUTE_NAME_MASK);
    uint32_t flags = (attribute->name & TXML_ATTRIBUTE_DEFAULTED) != 0u ? TXML_ATTR_DEFAULTED : 0u;
    if (entry != NULL && txml_str_equal(namespace_uri(doc, entry->namespace_id), TXML_STR(xmlns))) {
        flags |= TXML_ATTR_NS_DECL;
    }
    return flags;
}

TXML_API uint32_t txml_doc_node_count(const txml_doc *doc) {
    return doc == NULL ? 0u : doc->nodes.count;
}

TXML_PRIVATE txml_result txml_doc_append_node(txml_doc *doc, const txml_node_append *append, uint32_t *index) {
    void *node_slot = NULL;
    void *position_slot = NULL;

    txml_result result = column_push(&doc->nodes, &doc->tree, &node_slot);
    if (result != TXML_OK) {
        return result;
    }
    result = column_push(&doc->positions, &doc->tree, &position_slot);
    if (result != TXML_OK) {
        doc->nodes.count -= 1u; /* keep the columns parallel */
        return result;
    }
    *(txml_node *)node_slot = append->node;
    *(txml_span *)position_slot = (txml_span){ append->position, append->end };
    *index = doc->nodes.count - 1u;
    return TXML_OK;
}

TXML_PRIVATE void txml_doc_close_node(txml_doc *doc, uint32_t index, const txml_close *close) {
    txml_node *node = (txml_node *)column_at(&doc->nodes, index);
    txml_span *span = (txml_span *)column_at(&doc->positions, index);
    if (node != NULL && span != NULL) {
        node->end = close->end;
        node->flags = (uint8_t)(node->flags & ~TXML_NODE_OPEN);
        span->end = close->position;
    }
}

TXML_PRIVATE void txml_doc_complete(txml_doc *doc, txml_position end) {
    txml_doc_close_node(doc, 0u, &(txml_close){ doc->nodes.count, end });
    doc->complete = true;
}

TXML_PRIVATE txml_result txml_doc_append_attribute(txml_doc *doc, const txml_attribute_append *append, uint32_t *index) {
    void *slot = NULL;
    void *source_slot = NULL;
    txml_result result = column_push(&doc->attributes, &doc->tree, &slot);
    if (result != TXML_OK) {
        return result;
    }
    result = column_push(&doc->attribute_sources, &doc->tree, &source_slot);
    if (result != TXML_OK) {
        doc->attributes.count -= 1u; /* keep the columns parallel */
        return result;
    }
    *(txml_attribute_source *)source_slot = append->source;
    *(txml_attribute_entry *)slot = append->entry;
    *index = doc->attributes.count - 1u;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Prefixes.
 */

static uint32_t name_of(const txml_doc *doc, bool attribute, uint32_t index) {
    if (attribute) {
        const txml_attribute_entry *entry = txml_doc_attribute(doc, index);
        return entry == NULL ? TXML_NAME_NONE : entry->name & TXML_ATTRIBUTE_NAME_MASK;
    }
    const txml_node *node = txml_doc_node(doc, index);
    return node == NULL ? TXML_NAME_NONE : node->name;
}

TXML_PRIVATE txml_str txml_doc_prefix_of(const txml_doc *doc, bool attribute, uint32_t index) {
    const txml_column *exceptions = attribute ? &doc->attribute_prefixes : &doc->node_prefixes;
    uint32_t low = 0u;
    uint32_t high = exceptions->count;

    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        const txml_prefix_entry *entry = (const txml_prefix_entry *)column_entry(exceptions, middle);
        if (entry->index == index) {
            return txml_doc_pool_str(doc, entry->offset, entry->length);
        }
        if (entry->index < index) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    const txml_name_entry *name = txml_doc_name(doc, name_of(doc, attribute, index));
    return name == NULL ? txml_doc_pool_str(doc, 0u, 0u) : txml_doc_pool_str(doc, name->prefix_offset, name->prefix_length);
}

TXML_PRIVATE txml_result txml_doc_record_prefix(txml_doc *doc, const txml_prefix_request *request) {
    const txml_name_entry *name = txml_doc_name(doc, name_of(doc, request->attribute, request->index));
    if (name == NULL) {
        return TXML_ERR_INVALID;
    }
    if (txml_str_equal(request->prefix, txml_doc_pool_str(doc, name->prefix_offset, name->prefix_length))) {
        return TXML_OK;
    }

    uint32_t offset = 0u;
    void *slot = NULL;
    txml_column *exceptions = request->attribute ? &doc->attribute_prefixes : &doc->node_prefixes;
    txml_result result = txml_doc_pool_append(doc, request->prefix, &offset);
    if (result == TXML_OK) {
        result = column_push(exceptions, &doc->tree, &slot);
    }
    if (result != TXML_OK) {
        return result;
    }
    *(txml_prefix_entry *)slot = (txml_prefix_entry){ request->index, offset, request->prefix.length };
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Lifetime.
 */

TXML_API const txml_arena *txml_doc_arena(const txml_doc *doc, txml_arena_kind which) {
    if (doc == NULL) {
        return NULL;
    }
    return which == TXML_ARENA_VALUES ? &doc->values : &doc->tree;
}

TXML_API void txml_doc_destroy(txml_doc *doc) {
    if (doc == NULL) {
        return;
    }
    const txml_allocator allocator = doc->allocator;
    txml_arena_destroy(&doc->values);
    txml_arena_destroy(&doc->tree);

    txml_allocator_free_params params = { .ptr = doc, .size = sizeof(txml_doc), .alignment = _Alignof(txml_doc) };
    allocator.v->free(allocator.self, &params);
}

static txml_result doc_build(txml_doc *doc) {
    const uint64_t budget = doc->limits.max_document_bytes;

    txml_result result = column_init(&doc->nodes, &doc->tree, &(column_shape){ sizeof(txml_node), budget });
    if (result == TXML_OK) {
        result = column_init(&doc->attributes, &doc->tree, &(column_shape){ sizeof(txml_attribute_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->positions, &doc->tree, &(column_shape){ sizeof(txml_span), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->attribute_sources, &doc->tree, &(column_shape){ sizeof(txml_attribute_source), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->node_prefixes, &doc->tree, &(column_shape){ sizeof(txml_prefix_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->attribute_prefixes, &doc->tree, &(column_shape){ sizeof(txml_prefix_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->names, &doc->values, &(column_shape){ sizeof(txml_name_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->namespaces, &doc->values, &(column_shape){ sizeof(txml_namespace_entry), budget });
    }
    if (result != TXML_OK) {
        return result;
    }

    /* the pool directory grows with the document (pool_directory_reserve) */
    result = pool_directory_reserve(doc, 0u);
    if (result != TXML_OK) {
        return result;
    }
    doc->name_slots = (uint32_t *)txml_arena_alloc(
        &doc->values, (size_t)NAME_SLOTS_INITIAL * sizeof(uint32_t), sizeof(uint32_t));
    if (doc->name_slots == NULL) {
        return TXML_ERR_NOMEM;
    }
    for (uint32_t i = 0u; i < NAME_SLOTS_INITIAL; i++) {
        doc->name_slots[i] = TXML_NAME_NONE;
    }
    doc->name_slot_capacity = NAME_SLOTS_INITIAL;

    /* Namespace 0 is the empty URI, and node 0 is the open document node. */
    void *slot = NULL;
    result = column_push(&doc->namespaces, &doc->values, &slot);
    if (result != TXML_OK) {
        return result;
    }
    ((txml_namespace_entry *)slot)->uri_offset = 0u;
    ((txml_namespace_entry *)slot)->uri_length = 0u;

    const txml_node document = {
        .kind = TXML_NODE_DOCUMENT,
        .flags = TXML_NODE_OPEN,
        .depth = 0u,
        .parent = TXML_INDEX_NONE,
        .end = TXML_INDEX_NONE,
        .name = TXML_NAME_NONE,
        .as.element = { 0u, 0u },
    };
    uint32_t index = 0u;
    const txml_node_append append = { document, { 0u, 1u, 1u }, { 0u, 1u, 1u } };
    return txml_doc_append_node(doc, &append, &index);
}

TXML_PRIVATE txml_result txml_doc_create(txml_doc **out, const txml_parser_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->allocator.v->alloc == NULL ||
        config->allocator.v->free == NULL) {
        return TXML_ERR_INVALID;
    }
    if (config->limits.max_depth > 65535u || config->limits.max_document_bytes > UINT32_MAX ||
        config->limits.max_document_bytes == 0u) {
        return TXML_ERR_INVALID;
    }

    txml_doc *doc = (txml_doc *)config->allocator.v->alloc(config->allocator.self, sizeof(txml_doc), _Alignof(txml_doc));
    if (doc == NULL) {
        return TXML_ERR_NOMEM;
    }

    const size_t chunk_size = config->chunk_size == 0u ? TXML_ARENA_CHUNK_DEFAULT : config->chunk_size;
    uint32_t pool_page = POOL_PAGE_MAX;
    if (chunk_size < POOL_PAGE_MAX) {
        pool_page = chunk_size < POOL_PAGE_MIN ? POOL_PAGE_MIN : (uint32_t)chunk_size;
    }
    uint32_t shift = 0u;
    while (((uint32_t)2u << shift) <= pool_page) {
        shift += 1u;
    }

    doc->allocator = config->allocator;
    doc->limits = config->limits;
    doc->pool_shift = shift;
    doc->pool_length = 0u;
    doc->pool_directory = NULL;
    doc->pool_directory_capacity = 0u;
    doc->name_slots = NULL;
    doc->name_slot_capacity = 0u;
    doc->complete = false;
    doc->nodes = (txml_column){ 0 };
    doc->attributes = (txml_column){ 0 };
    doc->positions = (txml_column){ 0 };
    doc->attribute_sources = (txml_column){ 0 };
    doc->node_prefixes = (txml_column){ 0 };
    doc->attribute_prefixes = (txml_column){ 0 };
    doc->names = (txml_column){ 0 };
    doc->namespaces = (txml_column){ 0 };

    txml_result result = txml_arena_init(&doc->values, config->allocator, chunk_size);
    if (result == TXML_OK) {
        result = txml_arena_init(&doc->tree, config->allocator, chunk_size);
    }
    if (result == TXML_OK) {
        result = doc_build(doc);
    }
    if (result != TXML_OK) {
        txml_doc_destroy(doc);
        return result;
    }
    *out = doc;
    return TXML_OK;
}

/* ---- src/doctype.c ---- */
/*
 * The DOCTYPE and its internal subset.
 *
 * The declaration's text streams into a pool run exactly as written, while a
 * second view of the same bytes, a state machine, walks its structure: the name,
 * the external identifier, the internal subset, and within it the declarations.
 * Comments, processing instructions and parameter-entity references are
 * checked and streamed past. The external identifier and each ENTITY, ATTLIST,
 * ELEMENT and NOTATION declaration are assembled whole in the small buffer, the
 * same buffer and bound the XML declaration uses, and read there against the
 * grammar by subset.c; only ENTITY and ATTLIST declarations are kept.
 *
 * Every byte is interpreted before any of it is emitted, and the run is
 * reserved first, so an allocation that fails can only fail before a byte is
 * consumed.
 */

/* ------------------------------------------------------------------------
 * The DOCTYPE state.
 */

TXML_PRIVATE scan txp_begin_doctype(txml_parser *p) {
    if (p->stage != STAGE_PROLOG || p->seen_doctype) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "a DOCTYPE must come once, before the document element"), p->construct);
    }
    txml_doc_run_begin(p->doc, &p->run);
    txp_dec_reset(p, DECODE_VERBATIM);
    p->sink = &txp_sink_run;
    p->doctype = (doctype_work){ .phase = DP_SPACE };
    p->state = &txp_state_doctype;
    return SCAN_OK;
}

static bool is_upper(uint8_t b) {
    return b >= 'A' && b <= 'Z';
}

static bool keyword_is(const doctype_work *d, const char *literal) {
    return txp_bytes_are((const uint8_t *)d->keyword, d->keyword_length, literal);
}

static scan unrecognized(txml_parser *p, txml_position at) {
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "unrecognized declaration in the internal subset"), at);
}

/* A name was needed at window[index]: an illegal name character, or plain
 * syntax. */
static scan expected_name(txml_parser *p, uint32_t index, const char *message) {
    uint32_t width = 0u;
    const uint8_t b = p->window[index];
    if (b >= 0x80u) {
        return txp_fail_name_char(p, index);
    }
    if (txp_name_char_at(p->window + index, &width)) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a name cannot start with this character"), txp_position_at(p, index));
    }
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, message), txp_position_at(p, index));
}

/* Track the quotes of a collected declaration. */
static void follow_quote(doctype_work *d, uint8_t b) {
    if (d->quote != 0u) {
        if (b == d->quote) {
            d->quote = 0u;
        }
    } else if (b == '"' || b == '\'') {
        d->quote = b;
    }
}

static bool is_xml_pi_target(const doctype_work *d) {
    return d->keyword_length == 3u && (((uint8_t)d->keyword[0] | 0x20u) == 'x') &&
           (((uint8_t)d->keyword[1] | 0x20u) == 'm') && (((uint8_t)d->keyword[2] | 0x20u) == 'l');
}

/* The byte ending a collected declaration has been appended: read it. */
static scan finish_collected(txml_parser *p, declaration_kind kind) {
    const scan outcome = txp_read_declaration(p, kind);
    if (outcome != SCAN_OK) {
        p->small.length -= 1u; /* this byte is read again on retry */
    }
    return outcome;
}

static scan doctype_run(txml_parser *p) {
    doctype_work *d = &p->doctype;
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    position_cursor cursor = { 0u, { 0u, 0u, 0u }, false };
    scan outcome = SCAN_OK;
    bool ended = false;

    const txml_result reserved = txml_doc_run_reserve(p->doc, &p->run, limit - i);
    if (reserved != TXML_OK) {
        return txp_fail_result(p, reserved, p->construct);
    }

    while (i < limit) {
        const uint8_t b = w[i];
        uint32_t width = 0u;
        switch (d->phase) {
        case DP_SPACE:
            if (!txp_is_space(b)) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected a name after DOCTYPE"), txp_position_at(p, i));
            }
            d->phase = DP_SPACE_SKIP;
            i += 1u;
            break;
        case DP_SPACE_SKIP:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (!txp_name_start_at(w + i, &width)) {
                return expected_name(p, i, "expected a name after DOCTYPE");
            } else {
                d->phase = DP_NAME;
            }
            break;
        case DP_NAME:
            if (txp_name_char_at(w + i, &width)) {
                i += width;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else {
                /* Everything up to '[' or '>' is the external identifier. */
                p->small.length = 0u;
                p->small_position = txp_cursor_position(p, &cursor, i);
                d->quote = 0u;
                d->phase = DP_HEAD;
            }
            break;
        case DP_HEAD:
            outcome = txp_buffer_append(p, &b, 1u);
            if (outcome != SCAN_OK) {
                goto stop;
            }
            follow_quote(d, b);
            if (d->quote == 0u && (b == '[' || b == '>')) {
                outcome = finish_collected(p, DECL_DOCTYPE_HEAD);
                if (outcome != SCAN_OK) {
                    goto stop;
                }
                if (b == '>') {
                    ended = true;
                    goto stop;
                }
                d->phase = DP_SUBSET;
            }
            i += 1u;
            break;
        case DP_AFTER:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '>') {
                ended = true;
                goto stop;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected '>' after the internal subset"), txp_position_at(p, i));
            }
            break;
        case DP_SUBSET:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '%') {
                d->phase = DP_PARAMETER;
                i += 1u;
            } else if (b == ']') {
                d->phase = DP_AFTER;
                i += 1u;
            } else if (b == '<') {
                d->lt_position = txp_cursor_position(p, &cursor, i);
                d->phase = DP_LT;
                i += 1u;
            } else {
                return unrecognized(p, txp_position_at(p, i));
            }
            break;
        case DP_PARAMETER:
            if (!txp_name_start_at(w + i, &width)) {
                return expected_name(p, i, "expected a name in a parameter-entity reference");
            }
            d->phase = DP_PARAMETER_NAME;
            i += width;
            break;
        case DP_PARAMETER_NAME:
            if (txp_name_char_at(w + i, &width)) {
                i += width;
            } else if (b == ';') {
                d->phase = DP_SUBSET;
                i += 1u;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "a parameter-entity reference must end in ';'"),
                                txp_position_at(p, i));
            }
            break;
        case DP_LT:
            if (b == '?') {
                d->phase = DP_PI_START;
            } else if (b == '!') {
                d->phase = DP_BANG;
            } else {
                return unrecognized(p, d->lt_position);
            }
            i += 1u;
            break;
        case DP_BANG:
            if (b == '-') {
                d->phase = DP_DASH;
                i += 1u;
            } else if (is_upper(b)) {
                d->keyword_length = 0u;
                d->phase = DP_KEYWORD;
            } else if (b == '[') {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "conditional sections are not allowed in an internal subset"),
                                d->lt_position);
            } else {
                return unrecognized(p, d->lt_position);
            }
            break;
        case DP_DASH:
            if (b != '-') {
                return unrecognized(p, d->lt_position);
            }
            d->phase = DP_COMMENT;
            d->pending = 0u;
            i += 1u;
            break;
        case DP_KEYWORD:
            if (is_upper(b)) {
                if (d->keyword_length >= sizeof(d->keyword)) {
                    return unrecognized(p, d->lt_position);
                }
                d->keyword[d->keyword_length++] = (char)b;
                i += 1u;
            } else if (keyword_is(d, "ENTITY") || keyword_is(d, "ATTLIST") || keyword_is(d, "ELEMENT") ||
                       keyword_is(d, "NOTATION")) {
                d->kind = keyword_is(d, "ENTITY")    ? DECL_ENTITY
                          : keyword_is(d, "ATTLIST") ? DECL_ATTLIST
                          : keyword_is(d, "ELEMENT") ? DECL_ELEMENT
                                                     : DECL_NOTATION;
                p->small.length = 0u;
                p->small_position = txp_cursor_position(p, &cursor, i);
                d->quote = 0u;
                d->phase = DP_COLLECT;
            } else {
                return unrecognized(p, d->lt_position);
            }
            break;
        case DP_COMMENT: /* skip to "-->"; a "--" that is not its start is an error */
            if (b == '-') {
                if (d->pending == 2u) {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), txp_position_at(p, i));
                }
                d->pending += 1u;
            } else if (d->pending == 2u) {
                if (b != '>') {
                    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'--' inside a comment"), txp_position_at(p, i));
                }
                d->phase = DP_SUBSET;
                d->pending = 0u;
            } else {
                d->pending = 0u;
            }
            i += 1u;
            break;
        case DP_PI_START:
            if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_NAME, "expected a processing instruction target"), txp_position_at(p, i));
            }
            d->keyword_length = 0u;
            d->phase = DP_PI_TARGET;
            break;
        case DP_PI_TARGET:
            if (b == ':') {
                return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a target cannot contain a colon"), txp_position_at(p, i));
            }
            if (txp_name_char_at(w + i, &width)) {
                if (d->keyword_length < 4u) {
                    d->keyword[d->keyword_length] = (char)b;
                }
                d->keyword_length += 1u;
                i += width;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else if (is_xml_pi_target(d)) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML declaration is only allowed at the start of the document"),
                                d->lt_position);
            } else if (b == '?' || txp_is_space(b)) {
                d->phase = DP_PI;
                d->pending = 0u;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected whitespace after the target"), txp_position_at(p, i));
            }
            break;
        case DP_PI: /* skip to "?>" */
            if (b == '?') {
                d->pending = 1u;
            } else if (b == '>' && d->pending == 1u) {
                d->phase = DP_SUBSET;
                d->pending = 0u;
            } else {
                d->pending = 0u;
            }
            i += 1u;
            break;
        default: /* DP_COLLECT */
            outcome = txp_buffer_append(p, &b, 1u);
            if (outcome != SCAN_OK) {
                goto stop;
            }
            follow_quote(d, b);
            if (d->quote == 0u && b == '>') {
                outcome = finish_collected(p, d->kind);
                if (outcome != SCAN_OK) {
                    goto stop;
                }
                d->phase = DP_SUBSET;
            }
            i += 1u;
            break;
        }
    }

stop:
    if (outcome == SCAN_FAIL) {
        return outcome;
    }
    if (i > p->start) {
        uint32_t consumed = 0u;
        txp_dec_feed(p, &(dec_input){ w + p->start, i - p->start }, &consumed);
    }
    txp_commit(p, i);
    if (!ended) {
        return outcome;
    }

    outcome = txp_append_leaf(p, &(leaf){ TXML_NODE_DOCTYPE, 0u, TXML_NAME_NONE, p->construct, txp_position_after(p, p->start) });
    if (outcome != SCAN_OK) {
        return outcome;
    }
    txp_commit(p, i + 1u);
    p->seen_doctype = true;
    p->state = &txp_state_content;
    return SCAN_OK;
}

const state txp_state_doctype = { "doctype", doctype_run, txp_eof_in_markup };

/* ---- src/parser.c ---- */
/*
 * The parser's core: construction, feeding, the step loop, and the helpers
 * every state uses. See parser.h for the shape of the whole, and
 * docs/internal/parser.md.
 */

#define STAGING_DEFAULT ((uint32_t)65536u)
#define STAGING_MINIMUM ((uint32_t)64u)
#define STAGING_PADDING ((uint32_t)64u)

/* ------------------------------------------------------------------------
 * Byte classes.
 */

TXML_PRIVATE bool txp_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

TXML_PRIVATE bool txp_name_start_at(const uint8_t *bytes, uint32_t *width) {
    return txml_text_is_name_start(txml_text_decode(bytes, width));
}

TXML_PRIVATE bool txp_name_char_at(const uint8_t *bytes, uint32_t *width) {
    return txml_text_is_name_char(txml_text_decode(bytes, width));
}

TXML_PRIVATE scan txp_fail_name_char(txml_parser *p, uint32_t index) {
    return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal name character"), txp_position_at(p, index));
}

TXML_PRIVATE uint32_t txp_skip_space(const uint8_t *bytes, uint32_t from, uint32_t limit) {
    while (from < limit && txp_is_space(bytes[from])) {
        from += 1u;
    }
    return from;
}

TXML_PRIVATE uint32_t txp_scan_name(const uint8_t *bytes, uint32_t from, uint32_t limit) {
    uint32_t width = 0u;
    if (from >= limit || !txp_name_start_at(bytes + from, &width)) {
        return from;
    }
    uint32_t i = from + width;
    while (i < limit && txp_name_char_at(bytes + i, &width)) {
        i += width;
    }
    return i;
}

TXML_PRIVATE scan txp_eof_in_markup(txml_parser *p) {
    return txp_fail(p, PROBLEM(TXML_DOC_UNEXPECTED_EOF, "input ended inside markup"), txp_position_now(p));
}

TXML_PRIVATE scan txp_intern(txml_parser *p, const intern_request *request, uint32_t *id) {
    const txml_result result = txml_doc_intern_name(p->doc, &request->params, id);
    if (result == TXML_ERR_LIMIT && p->doc->names.count >= p->limits.max_names) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_NAMES, "too many distinct names"), request->at);
    }
    return result == TXML_OK ? SCAN_OK : txp_fail_result(p, result, request->at);
}

TXML_PRIVATE uint32_t txp_literal_length(const char *literal) {
    uint32_t length = 0u;
    while (literal[length] != '\0') {
        length += 1u;
    }
    return length;
}

TXML_PRIVATE bool txp_bytes_are(const uint8_t *bytes, uint32_t length, const char *literal) {
    if (length != txp_literal_length(literal)) {
        return false;
    }
    for (uint32_t i = 0u; i < length; i++) {
        if (bytes[i] != (uint8_t)literal[i]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Positions.
 */

TXML_PRIVATE uint32_t txp_limit(const txml_parser *p) {
    return p->bad != IDX_NONE ? p->bad : p->valid;
}

TXML_PRIVATE txml_position txp_position_now(const txml_parser *p) {
    return (txml_position){ p->consumed, p->line, p->column };
}

TXML_PRIVATE txml_position txp_position_at(const txml_parser *p, uint32_t index) {
    txml_position position = txp_position_now(p);
    for (uint32_t i = p->start; i < index && i < p->end; i++) {
        position.offset += 1u;
        if (p->window[i] == '\n') {
            position.line += 1u;
            position.column = 1u;
        } else {
            position.column += 1u;
        }
    }
    return position;
}

TXML_PRIVATE txml_position txp_position_after(const txml_parser *p, uint32_t index) {
    txml_position position = txp_position_at(p, index);
    position.offset += 1u;
    position.column += 1u;
    return position;
}

TXML_PRIVATE txml_position txp_cursor_position(const txml_parser *p, position_cursor *cursor, uint32_t index) {
    if (!cursor->valid || index < cursor->index) {
        cursor->at = txp_position_now(p);
        cursor->index = p->start;
        cursor->valid = true;
    }
    for (; cursor->index < index && cursor->index < p->end; cursor->index++) {
        cursor->at.offset += 1u;
        if (p->window[cursor->index] == '\n') {
            cursor->at.line += 1u;
            cursor->at.column = 1u;
        } else {
            cursor->at.column += 1u;
        }
    }
    return cursor->at;
}

TXML_PRIVATE void txp_commit(txml_parser *p, uint32_t index) {
    for (uint32_t i = p->start; i < index; i++) {
        if (p->window[i] == '\n') {
            p->line += 1u;
            p->column = 1u;
        } else {
            p->column += 1u;
        }
    }
    p->consumed += index - p->start;
    p->start = index;
}

/* ------------------------------------------------------------------------
 * Errors, the budget, appending.
 */

TXML_PRIVATE scan txp_fail(txml_parser *p, const problem *what, txml_position at) {
    p->failed = true;
    p->error.code = what->code;
    p->error.limit = what->limit;
    p->error.at = at;
    p->error.message = (txml_str){ what->message, txp_literal_length(what->message) };
    return SCAN_FAIL;
}

TXML_PRIVATE scan txp_fail_result(txml_parser *p, txml_result result, txml_position at) {
    if (result == TXML_ERR_NOMEM) {
        return SCAN_NOMEM;
    }
    if (result == TXML_ERR_LIMIT) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "the document exceeds a storage limit"), at);
    }
    return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "internal inconsistency"), at);
}

TXML_PRIVATE scan txp_check_budget(txml_parser *p, txml_position at) {
    const txml_doc *doc = p->doc;
    if ((uint64_t)doc->values.used + doc->tree.used > p->limits.max_document_bytes) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "the document exceeds max_document_bytes"), at);
    }
    return SCAN_OK;
}

TXML_PRIVATE bool txp_budget_spent(const txml_parser *p) {
    return p->step_budget != 0u && p->appended >= p->step_budget;
}

TXML_PRIVATE uint32_t txp_parent_node(const txml_parser *p) {
    return p->depth == 0u ? 0u : p->stack[p->depth - 1u].node;
}

TXML_PRIVATE scan txp_append_leaf(txml_parser *p, const leaf *what) {
    uint32_t index = 0u;

    txml_doc_run_finish(p->doc, &p->run);
    const uint32_t next = p->doc->nodes.count;
    const txml_node_append append = {
        .node = {
            .kind = (uint8_t)what->kind, .flags = (uint8_t)what->flags, .depth = (uint16_t)(p->depth + 1u),
            .parent = txp_parent_node(p), .end = next + 1u, .name = what->name,
            .as.text = { p->run.offset, p->run.length },
        },
        .position = what->position,
        .end = what->end,
    };
    const txml_result result = txml_doc_append_node(p->doc, &append, &index);
    if (result != TXML_OK) {
        return txp_fail_result(p, result, what->position);
    }
    p->appended += 1u;
    return txp_check_budget(p, what->position);
}

TXML_PRIVATE snapshot txp_snapshot(txml_parser *p) {
    snapshot s = { p->doc->nodes.count, p->doc->attributes.count, p->doc->node_prefixes.count,
                   p->doc->attribute_prefixes.count, p->bindings, txml_arena_position(&p->stack_arena) };
    return s;
}

TXML_PRIVATE void txp_rollback_nodes(txml_parser *p, const snapshot *s) {
    p->doc->nodes.count = s->nodes;
    p->doc->positions.count = s->nodes;
    p->doc->attributes.count = s->attributes;
    p->doc->attribute_sources.count = s->attributes;
    p->doc->node_prefixes.count = s->node_prefixes;
    p->doc->attribute_prefixes.count = s->attribute_prefixes;
}

/* ------------------------------------------------------------------------
 * Construction.
 */

TXML_API txml_result txml_parser_init(txml_parser **out, const txml_parser_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->allocator.v->alloc == NULL ||
        config->allocator.v->free == NULL) {
        return TXML_ERR_INVALID;
    }
    const uint32_t staging = config->staging_length == 0u ? STAGING_DEFAULT : config->staging_length;
    if (staging < STAGING_MINIMUM || config->limits.max_attributes == 0u || config->limits.max_depth == 0u ||
        config->limits.max_name_length == 0u) {
        return TXML_ERR_INVALID;
    }

    txml_parser *p = (txml_parser *)config->allocator.v->alloc(config->allocator.self, sizeof(txml_parser),
                                                               _Alignof(txml_parser));
    if (p == NULL) {
        return TXML_ERR_NOMEM;
    }
    txml_text_zero(p, sizeof(*p));
    p->allocator = config->allocator;
    p->limits = config->limits;
    p->step_budget = config->step_budget;
    p->staging_length = staging;
    p->bad = IDX_NONE;
    p->line = p->column = 1u;
    p->state = &txp_state_start;
    p->stage = STAGE_PROLOG;
    p->error = (txml_document_error){ TXML_DOC_OK, TXML_LIMIT_NONE, { 0u, 1u, 1u }, { "", 0u } };
    p->reference_capacity = config->limits.max_name_length + 2u;

    txml_result result = txml_arena_init(&p->parser_arena, config->allocator, config->chunk_size);
    if (result == TXML_OK) {
        result = txml_arena_init(&p->stack_arena, config->allocator, config->chunk_size);
    }
    if (result == TXML_OK) {
        result = txml_arena_init(&p->tag_arena, config->allocator, config->chunk_size);
    }
    if (result == TXML_OK) {
        p->window = (uint8_t *)txml_arena_alloc(&p->parser_arena, (size_t)staging + STAGING_PADDING, 16u);
        p->name = (uint8_t *)txml_arena_alloc(&p->parser_arena, config->limits.max_name_length, 1u);
        p->reference = (uint8_t *)txml_arena_alloc(&p->parser_arena, p->reference_capacity, 1u);
        p->records = (attribute_record *)txml_arena_alloc(
            &p->parser_arena, (size_t)config->limits.max_attributes * sizeof(attribute_record), _Alignof(attribute_record));
        p->stack = (open_element *)txml_arena_alloc(&p->parser_arena, (size_t)config->limits.max_depth * sizeof(open_element),
                                                    _Alignof(open_element));
        p->frames = (frame *)txml_arena_alloc(&p->parser_arena, ((size_t)config->limits.max_depth + 1u) * sizeof(frame),
                                              _Alignof(frame));
        if (p->window == NULL || p->name == NULL || p->reference == NULL || p->records == NULL || p->stack == NULL ||
            p->frames == NULL) {
            result = TXML_ERR_NOMEM;
        }
    }
    if (result == TXML_OK) {
        /* A mark taken on an empty arena would free the whole first chunk on
         * release, once per tag; one byte keeps that chunk alive. */
        if (txml_arena_alloc(&p->stack_arena, 1u, 1u) == NULL || txml_arena_alloc(&p->tag_arena, 1u, 1u) == NULL) {
            result = TXML_ERR_NOMEM;
        }
    }
    if (result == TXML_OK) {
        for (uint32_t i = 0u; i < STAGING_PADDING; i++) {
            p->window[staging + i] = 0u;
        }
        result = txml_doc_create(&p->doc, config);
    }
    if (result != TXML_OK) {
        txml_parser_destroy(p);
        return result;
    }
    *out = p;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Feeding. Bytes are validated as they arrive: a whole valid character moves
 * `valid` past it, a bad one sets `bad`, and a character cut off by the end of
 * the chunk stays unreadable until its remaining bytes come.
 */

TXML_API uint32_t txml_parse_feed(txml_parser *p, txml_bytes chunk, bool final) {
    if (p == NULL || p->done || p->failed) {
        return 0u;
    }
    if (p->final_seen) {
        p->fed_after_final = chunk.length != 0u;
        return 0u;
    }
    if (p->start != 0u) {
        for (uint32_t i = p->start; i < p->end; i++) {
            p->window[i - p->start] = p->window[i];
        }
        p->end -= p->start;
        p->valid -= p->start;
        if (p->bad != IDX_NONE) {
            p->bad -= p->start;
        }
        p->start = 0u;
    }
    const uint32_t room = p->staging_length - p->end;
    const uint32_t take = chunk.length < room ? chunk.length : room;
    for (uint32_t i = 0u; i < take; i++) {
        p->window[p->end + i] = chunk.ptr[i];
    }
    p->end += take;
    p->fed += take;

    if (p->bad == IDX_NONE) {
        const txml_text_scan checked = txml_text_scan_utf8(p->window + p->valid, p->end - p->valid);
        if (checked.invalid != UINT32_MAX) {
            p->bad = p->valid + checked.invalid;
        }
        p->valid += checked.complete;
    }
    if (final && take == chunk.length) {
        p->final_seen = true;
        if (p->bad == IDX_NONE && p->valid < p->end) {
            p->bad = p->valid; /* input ended inside a character */
        }
    }
    return take;
}

/* ------------------------------------------------------------------------
 * The step loop.
 */

static txml_step step_result(scan outcome) {
    switch (outcome) {
    case SCAN_YIELD:
        return TXML_STEP_PROGRESS;
    case SCAN_NOMEM:
        return TXML_STEP_NEED_MEMORY;
    case SCAN_FAIL:
        return TXML_STEP_FAILED;
    default:
        return TXML_STEP_DONE;
    }
}

TXML_API txml_step txml_parse_step(txml_parser *p) {
    if (p->failed) {
        return TXML_STEP_FAILED;
    }
    if (p->done) {
        return TXML_STEP_DONE;
    }
    if (p->fed_after_final) {
        txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "input fed after the final chunk"), txp_position_now(p));
        return TXML_STEP_FAILED;
    }

    p->appended = 0u;
    for (;;) {
        scan outcome = SCAN_OK;

        /* An entity expansion that ran out of memory finishes first. */
        if (p->dec.expanding) {
            outcome = txp_dec_resume(p);
            if (outcome != SCAN_OK) {
                return step_result(outcome);
            }
        }

        if (p->start == txp_limit(p)) {
            if (p->bad != IDX_NONE) {
                txp_fail(p, PROBLEM(TXML_DOC_ENCODING, "not well-formed UTF-8 or a forbidden character"),
                         txp_position_at(p, p->bad));
                return TXML_STEP_FAILED;
            }
            if (!p->final_seen) {
                return TXML_STEP_NEED_INPUT;
            }
            outcome = p->state->at_eof(p);
        } else {
            outcome = p->state->run(p);
        }

        if (outcome == SCAN_DONE) {
            p->done = true;
        }
        if (outcome != SCAN_OK) {
            return step_result(outcome);
        }
    }
}

TXML_API const txml_doc *txml_parser_doc(const txml_parser *p) {
    return p == NULL ? NULL : p->doc;
}

TXML_API txml_result txml_parser_error(const txml_parser *p, txml_document_error *out) {
    if (p == NULL || !p->failed) {
        return TXML_ERR_NOT_FOUND;
    }
    *out = p->error;
    return TXML_OK;
}

TXML_API uint64_t txml_parser_offset(const txml_parser *p) {
    return p == NULL ? 0u : p->fed;
}

TXML_API void txml_parser_destroy(txml_parser *p) {
    if (p == NULL) {
        return;
    }
    const txml_allocator allocator = p->allocator;
    if (!p->detached && p->doc != NULL) {
        txml_doc_destroy(p->doc);
    }
    txml_arena_destroy(&p->tag_arena);
    txml_arena_destroy(&p->stack_arena);
    txml_arena_destroy(&p->parser_arena);
    txml_allocator_free_params params = { .ptr = p, .size = sizeof(txml_parser), .alignment = _Alignof(txml_parser) };
    allocator.v->free(allocator.self, &params);
}

TXML_API txml_result txml_parser_finish(txml_parser *p, txml_doc **out) {
    if (p == NULL || out == NULL || !p->done) {
        return TXML_ERR_INVALID;
    }
    *out = p->doc;
    p->detached = true;
    txml_parser_destroy(p);
    return TXML_OK;
}

/* ---- src/subset.c ---- */
/*
 * The declarations of a DOCTYPE, read whole.
 *
 * doctype.c streams the DOCTYPE's bytes through a state machine and, for each
 * declaration that has structure worth checking, assembles the bytes after its
 * keyword in the small buffer. This module reads that buffer against the XML 1.0
 * grammar. ENTITY and ATTLIST declarations are recorded (they change what a
 * document means); ELEMENT and NOTATION declarations, and the external
 * identifier of the DOCTYPE itself, are only checked.
 *
 * A non-validating processor still rejects a malformed declaration: it is a
 * well-formedness error. What is not checked is what needs the whole DTD to
 * decide, and the content model of an ELEMENT declaration beyond its shape:
 * parentheses balance, items and separators alternate, quantifiers follow an
 * item, and a mixed model is `(#PCDATA)` or `(#PCDATA|a|b)*`. A group may mix
 * `,` and `|`, which the grammar forbids, because telling them apart needs a
 * stack as deep as the model.
 *
 * Every function here is iterative and reads only [0, limit), where `limit`
 * is the index of the byte that ended the declaration.
 */

typedef struct {
    txml_parser *p;
    const uint8_t *w;
    uint32_t limit; /* the terminating '>' or '[' */
    uint32_t i;
} reader;

typedef struct {
    uint32_t start;
    uint32_t end;
} span;

typedef enum {
    LITERAL_ENTITY_VALUE,
    LITERAL_ATTRIBUTE_VALUE,
    LITERAL_SYSTEM,
    LITERAL_PUBID
} literal_type;

#define SYNTAX(message) PROBLEM(TXML_DOC_SYNTAX, (message))
#define BAD_NAME(message) PROBLEM(TXML_DOC_NAME, (message))

static scan raise(const reader *r, const problem *what, uint32_t at) {
    return txp_fail(r->p, what, txp_small_position_at(r->p, at));
}

static txml_str text_of(const reader *r, span range) {
    return (txml_str){ (const char *)r->w + range.start, range.end - range.start };
}

/* Skip whitespace; the result is how much there was. */
static uint32_t skip_space(reader *r) {
    const uint32_t from = r->i;
    r->i = txp_skip_space(r->w, r->i, r->limit);
    return r->i - from;
}

static scan need_space(reader *r) {
    if (skip_space(r) == 0u) {
        return raise(r, SYNTAX("expected whitespace"), r->i);
    }
    return SCAN_OK;
}

/* Nothing may follow but whitespace. */
static scan finish(reader *r) {
    skip_space(r);
    if (r->i != r->limit) {
        return raise(r, SYNTAX("unexpected character in the declaration"), r->i);
    }
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Names and keywords.
 */

/* A name where one must be. A non-ASCII character that cannot be part of a
 * name is illegal, not merely unexpected, since no such character delimits
 * markup; so is an ASCII name character that cannot start one. */
static scan bad_name_start(const reader *r, uint32_t at) {
    uint32_t width = 0u;
    if (at < r->limit && (r->w[at] >= 0x80u || txp_name_char_at(r->w + at, &width))) {
        return raise(r, BAD_NAME("not a legal name character here"), at);
    }
    return raise(r, SYNTAX("expected a name"), at);
}

static scan read_name(reader *r, span *out, bool ncname) {
    const uint32_t at = r->i;
    uint32_t width = 0u;

    if (at >= r->limit || !txp_name_start_at(r->w + at, &width)) {
        return bad_name_start(r, at);
    }
    const uint32_t stop = txp_scan_name(r->w, at, r->limit);
    if (stop < r->limit && r->w[stop] >= 0x80u) {
        return raise(r, BAD_NAME("not a legal name character"), stop);
    }
    if (ncname) {
        for (uint32_t k = at; k < stop; k++) {
            if (r->w[k] == ':') {
                return raise(r, BAD_NAME("a colon is not allowed in this name"), k);
            }
        }
    }
    *out = (span){ at, stop };
    r->i = stop;
    return SCAN_OK;
}

static scan read_nmtoken(reader *r) {
    const uint32_t at = r->i;
    uint32_t width = 0u;
    uint32_t k = at;

    while (k < r->limit && txp_name_char_at(r->w + k, &width)) {
        k += width;
    }
    if (k == at) {
        return bad_name_start(r, at);
    }
    if (k < r->limit && r->w[k] >= 0x80u) {
        return raise(r, BAD_NAME("not a legal name character"), k);
    }
    r->i = k;
    return SCAN_OK;
}

/* The run of name characters at r->i, without consuming it. */
static span peek_word(const reader *r) {
    return (span){ r->i, txp_scan_name(r->w, r->i, r->limit) };
}

static bool word_is(const reader *r, span word, const char *literal) {
    return txp_bytes_are(r->w + word.start, word.end - word.start, literal);
}

static scan expect_keyword(reader *r, const char *keyword) {
    const span word = peek_word(r);
    if (!word_is(r, word, keyword)) {
        return raise(r, SYNTAX("expected a keyword here"), r->i);
    }
    r->i = word.end;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Literals and references.
 */

static bool pubid_char(uint8_t b) {
    if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')) {
        return true;
    }
    switch (b) {
    case ' ': case '\r': case '\n': case '-': case '\'': case '(': case ')': case '+': case ',': case '.':
    case '/': case ':': case '=': case '?': case ';': case '!': case '*': case '#': case '@': case '$':
    case '_': case '%':
        return true;
    default:
        return false;
    }
}

/* The reference starting at `at` ('&'): well-formed, ending in ';'. `next` is
 * the index after it. Whether a named entity exists is settled where it is
 * used, not here. */
static scan check_reference(const reader *r, uint32_t at, uint32_t *next) {
    uint32_t stop = at + 1u;

    if (stop < r->limit && r->w[stop] == '#') {
        while (stop < r->limit && r->w[stop] != ';' && r->w[stop] != '"' && r->w[stop] != '\'') {
            stop += 1u;
        }
        if (stop >= r->limit || r->w[stop] != ';') {
            return raise(r, SYNTAX("malformed character reference"), at);
        }
        const charref_check checked = txp_check_character_reference(r->w + at + 1u, stop - at - 1u);
        if (checked == CHARREF_ENCODING) {
            return raise(r, PROBLEM(TXML_DOC_ENCODING, "character reference outside the Char production"), at);
        }
        if (checked != CHARREF_OK) {
            return raise(r, SYNTAX("malformed character reference"), at);
        }
    } else {
        stop = txp_scan_name(r->w, at + 1u, r->limit);
        if (stop == at + 1u) {
            return raise(r, SYNTAX("malformed reference"), at);
        }
        if (stop < r->limit && r->w[stop] >= 0x80u) {
            return raise(r, BAD_NAME("not a legal name character"), stop);
        }
        if (stop >= r->limit || r->w[stop] != ';') {
            return raise(r, SYNTAX("a reference must end in ';'"), at);
        }
        for (uint32_t k = at + 1u; k < stop; k++) {
            if (r->w[k] == ':') {
                return raise(r, BAD_NAME("a colon is not allowed in an entity name"), k);
            }
        }
    }
    *next = stop + 1u;
    return SCAN_OK;
}

/* A quoted literal at r->i; `out` is its contents, checked for the kind. */
static scan read_literal(reader *r, literal_type type, txml_str *out) {
    const uint32_t at = r->i;

    if (at >= r->limit || (r->w[at] != '"' && r->w[at] != '\'')) {
        return raise(r, SYNTAX("expected a quoted literal"), at);
    }
    const uint8_t quote = r->w[at];
    uint32_t k = at + 1u;

    while (k < r->limit && r->w[k] != quote) {
        const uint8_t b = r->w[k];
        if (type == LITERAL_ENTITY_VALUE && b == '%') {
            return raise(r, SYNTAX("a parameter-entity reference is not allowed in an internal-subset entity value"), k);
        }
        if (type == LITERAL_ATTRIBUTE_VALUE && b == '<') {
            return raise(r, SYNTAX("'<' in an attribute default"), k);
        }
        if (type == LITERAL_PUBID && !pubid_char(b)) {
            return raise(r, SYNTAX("not a legal public identifier character"), k);
        }
        if ((type == LITERAL_ENTITY_VALUE || type == LITERAL_ATTRIBUTE_VALUE) && b == '&') {
            const scan checked = check_reference(r, k, &k);
            if (checked != SCAN_OK) {
                return checked;
            }
            continue;
        }
        k += 1u;
    }
    if (k >= r->limit) {
        return raise(r, SYNTAX("unterminated literal"), at);
    }
    *out = (txml_str){ (const char *)r->w + at + 1u, k - at - 1u };
    r->i = k + 1u;
    return SCAN_OK;
}

/* SYSTEM "lit" | PUBLIC "pubid" "lit". A notation may leave the system literal
 * out (`public_alone`). */
static scan read_external_id(reader *r, bool public_alone) {
    const uint32_t at = r->i;
    const span word = peek_word(r);
    txml_str ignored;
    scan outcome;

    if (word_is(r, word, "SYSTEM")) {
        r->i = word.end;
        outcome = need_space(r);
        return outcome != SCAN_OK ? outcome : read_literal(r, LITERAL_SYSTEM, &ignored);
    }
    if (!word_is(r, word, "PUBLIC")) {
        return raise(r, SYNTAX("expected SYSTEM or PUBLIC"), at);
    }
    r->i = word.end;
    outcome = need_space(r);
    if (outcome == SCAN_OK) {
        outcome = read_literal(r, LITERAL_PUBID, &ignored);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }
    const uint32_t after_pubid = r->i;
    const uint32_t spaces = skip_space(r);
    if (r->i < r->limit && (r->w[r->i] == '"' || r->w[r->i] == '\'')) {
        if (spaces == 0u) {
            return raise(r, SYNTAX("expected whitespace"), r->i);
        }
        return read_literal(r, LITERAL_SYSTEM, &ignored);
    }
    if (!public_alone) {
        return raise(r, SYNTAX("a PUBLIC identifier needs a system literal"), r->i);
    }
    r->i = after_pubid;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * <!ENTITY ...>
 */

static scan read_entity(reader *r) {
    scan outcome = need_space(r);
    span name;
    txml_str value;

    if (outcome != SCAN_OK) {
        return outcome;
    }
    const bool parameter = r->i < r->limit && r->w[r->i] == '%';
    if (parameter) {
        r->i += 1u;
        outcome = need_space(r);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    outcome = read_name(r, &name, true);
    if (outcome == SCAN_OK) {
        outcome = need_space(r);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }

    if (r->i < r->limit && (r->w[r->i] == '"' || r->w[r->i] == '\'')) {
        outcome = read_literal(r, LITERAL_ENTITY_VALUE, &value);
        if (outcome == SCAN_OK && !parameter) {
            outcome = txp_add_entity(r->p, text_of(r, name), value);
        }
        return outcome != SCAN_OK ? outcome : finish(r);
    }

    /* External entities are checked and never fetched, so a reference to one
     * stays undeclared. Only a general entity may be unparsed. */
    outcome = read_external_id(r, false);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    const uint32_t spaces = skip_space(r);
    if (!parameter && r->i < r->limit) {
        if (spaces == 0u) {
            return raise(r, SYNTAX("expected whitespace"), r->i);
        }
        outcome = expect_keyword(r, "NDATA");
        if (outcome == SCAN_OK) {
            outcome = need_space(r);
        }
        if (outcome == SCAN_OK) {
            outcome = read_name(r, &name, true);
        }
        if (outcome != SCAN_OK) {
            return outcome;
        }
    }
    return finish(r);
}

/* ------------------------------------------------------------------------
 * <!ATTLIST ...>
 */

/* '(' Nmtoken ('|' Nmtoken)* ')', or with Names for a notation type. */
static scan read_enumeration(reader *r, bool notation) {
    span name;
    scan outcome;

    if (r->i >= r->limit || r->w[r->i] != '(') {
        return raise(r, SYNTAX("expected '('"), r->i);
    }
    r->i += 1u;
    for (;;) {
        skip_space(r);
        outcome = notation ? read_name(r, &name, true) : read_nmtoken(r);
        if (outcome != SCAN_OK) {
            return outcome;
        }
        skip_space(r);
        if (r->i < r->limit && r->w[r->i] == '|') {
            r->i += 1u;
            continue;
        }
        if (r->i < r->limit && r->w[r->i] == ')') {
            r->i += 1u;
            return SCAN_OK;
        }
        return raise(r, SYNTAX("expected '|' or ')'"), r->i);
    }
}

static scan read_attribute_type(reader *r) {
    static const char *const keywords[] = { "CDATA", "ID", "IDREF", "IDREFS", "ENTITY", "ENTITIES", "NMTOKEN", "NMTOKENS" };

    if (r->i < r->limit && r->w[r->i] == '(') {
        return read_enumeration(r, false);
    }
    const span word = peek_word(r);
    if (word_is(r, word, "NOTATION")) {
        r->i = word.end;
        const scan outcome = need_space(r);
        return outcome != SCAN_OK ? outcome : read_enumeration(r, true);
    }
    for (uint32_t k = 0u; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        if (word_is(r, word, keywords[k])) {
            r->i = word.end;
            return SCAN_OK;
        }
    }
    return raise(r, SYNTAX("unknown attribute type"), r->i);
}

static scan read_attlist(reader *r) {
    scan outcome = need_space(r);
    span element;
    span name;
    txml_str value;

    if (outcome == SCAN_OK) {
        outcome = read_name(r, &element, false);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }

    for (;;) {
        const uint32_t spaces = skip_space(r);
        if (r->i == r->limit) {
            return SCAN_OK;
        }
        if (spaces == 0u) {
            return raise(r, SYNTAX("expected whitespace"), r->i);
        }
        outcome = read_name(r, &name, false);
        if (outcome == SCAN_OK) {
            outcome = need_space(r);
        }
        if (outcome == SCAN_OK) {
            outcome = read_attribute_type(r);
        }
        if (outcome == SCAN_OK) {
            outcome = need_space(r);
        }
        if (outcome != SCAN_OK) {
            return outcome;
        }

        bool has_value = true;
        if (r->i < r->limit && r->w[r->i] == '#') {
            const uint32_t hash = r->i;
            r->i += 1u;
            const span word = peek_word(r);
            if (word_is(r, word, "REQUIRED") || word_is(r, word, "IMPLIED")) {
                r->i = word.end;
                has_value = false;
            } else if (word_is(r, word, "FIXED")) {
                r->i = word.end;
                outcome = need_space(r);
                if (outcome != SCAN_OK) {
                    return outcome;
                }
            } else {
                return raise(r, SYNTAX("unknown default declaration"), hash);
            }
        }
        if (has_value) {
            outcome = read_literal(r, LITERAL_ATTRIBUTE_VALUE, &value);
            if (outcome == SCAN_OK) {
                outcome = txp_add_default(r->p, &(default_request){ text_of(r, element), text_of(r, name), value });
            }
            if (outcome != SCAN_OK) {
                return outcome;
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * <!ELEMENT ...> and <!NOTATION ...>
 */

static bool is_quantifier(uint8_t b) {
    return b == '?' || b == '*' || b == '+';
}

/* A parenthesised content model, read without recursion: only the depth is
 * kept. */
static scan read_content_model(reader *r) {
    uint32_t depth = 0u;
    bool expect_item = true; /* an item must come next */
    bool mixed = false;
    uint32_t mixed_names = 0u;
    span name;

    for (;;) {
        skip_space(r);
        if (r->i >= r->limit) {
            return raise(r, SYNTAX("unbalanced parentheses in a content model"), r->i);
        }
        const uint8_t b = r->w[r->i];
        const uint32_t at = r->i;

        if (b == '(') {
            if (!expect_item || mixed) {
                return raise(r, SYNTAX("unexpected '(' in a content model"), at);
            }
            depth += 1u;
            r->i += 1u;
            skip_space(r);
            if (depth == 1u && r->i < r->limit && r->w[r->i] == '#') {
                const span word = (span){ r->i, txp_scan_name(r->w, r->i + 1u, r->limit) };
                if (!word_is(r, word, "#PCDATA")) {
                    return raise(r, SYNTAX("expected #PCDATA"), r->i);
                }
                mixed = true;
                expect_item = false;
                r->i = word.end;
            }
        } else if (b == '|' || b == ',') {
            if (expect_item || (mixed && b == ',')) {
                return raise(r, SYNTAX("unexpected separator in a content model"), at);
            }
            expect_item = true;
            r->i += 1u;
        } else if (b == ')') {
            if (expect_item || depth == 0u) {
                return raise(r, SYNTAX("unexpected ')' in a content model"), at);
            }
            depth -= 1u;
            r->i += 1u;
            if (mixed && depth == 0u) {
                if (r->i < r->limit && r->w[r->i] == '*') {
                    r->i += 1u;
                } else if (mixed_names > 0u) {
                    return raise(r, SYNTAX("a mixed content model with names must end in ')*'"), r->i);
                }
                return SCAN_OK;
            }
            if (r->i < r->limit && is_quantifier(r->w[r->i])) {
                r->i += 1u;
            }
            expect_item = false;
            if (depth == 0u) {
                return SCAN_OK;
            }
        } else if (b == '#') {
            return raise(r, SYNTAX("#PCDATA must come first in a mixed content model"), at);
        } else {
            if (!expect_item) {
                return raise(r, SYNTAX("expected a separator or ')' in a content model"), at);
            }
            const scan outcome = read_name(r, &name, false);
            if (outcome != SCAN_OK) {
                return outcome;
            }
            expect_item = false;
            if (mixed) {
                mixed_names += 1u;
            } else if (r->i < r->limit && is_quantifier(r->w[r->i])) {
                r->i += 1u;
            }
        }
    }
}

static scan read_element(reader *r) {
    scan outcome = need_space(r);
    span name;

    if (outcome == SCAN_OK) {
        outcome = read_name(r, &name, false);
    }
    if (outcome == SCAN_OK) {
        outcome = need_space(r);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }
    const span word = peek_word(r);
    if (word_is(r, word, "EMPTY") || word_is(r, word, "ANY")) {
        r->i = word.end;
    } else if (r->i < r->limit && r->w[r->i] == '(') {
        outcome = read_content_model(r);
        if (outcome != SCAN_OK) {
            return outcome;
        }
    } else {
        return raise(r, SYNTAX("expected EMPTY, ANY or a content model"), r->i);
    }
    return finish(r);
}

static scan read_notation(reader *r) {
    scan outcome = need_space(r);
    span name;

    if (outcome == SCAN_OK) {
        outcome = read_name(r, &name, true);
    }
    if (outcome == SCAN_OK) {
        outcome = need_space(r);
    }
    if (outcome == SCAN_OK) {
        outcome = read_external_id(r, true);
    }
    return outcome != SCAN_OK ? outcome : finish(r);
}

/* What follows the DOCTYPE's name: an optional external identifier. */
static scan read_doctype_head(reader *r) {
    const uint32_t spaces = skip_space(r);

    if (r->i == r->limit) {
        return SCAN_OK;
    }
    if (spaces == 0u) {
        return raise(r, SYNTAX("expected whitespace"), r->i);
    }
    const scan outcome = read_external_id(r, false);
    return outcome != SCAN_OK ? outcome : finish(r);
}

TXML_PRIVATE scan txp_read_declaration(txml_parser *p, declaration_kind kind) {
    reader r = { p, p->small.data, p->small.length - 1u, 0u };

    switch (kind) {
    case DECL_ENTITY:
        return read_entity(&r);
    case DECL_ATTLIST:
        return read_attlist(&r);
    case DECL_ELEMENT:
        return read_element(&r);
    case DECL_NOTATION:
        return read_notation(&r);
    default:
        return read_doctype_head(&r);
    }
}

/* ---- src/tables.c ---- */
/*
 * The DTD tables: general entities and ATTLIST defaults.
 *
 * Both are looked up by string, an entity at every reference and the defaults
 * of an element at every start tag, so a list would make the cost of a parse
 * the product of two document-controlled numbers. Each is an open-addressing
 * table of pointers that doubles in the parser arena.
 */

/* ------------------------------------------------------------------------
 * The generic table.
 */

static uint32_t hash_bytes(uint32_t hash, txml_str text) {
    for (uint32_t i = 0u; i < text.length; i++) {
        hash = (hash ^ (uint8_t)text.ptr[i]) * 16777619u;
    }
    return hash;
}

static uint32_t hash_key(const table_key *key) {
    uint32_t hash = hash_bytes(2166136261u, key->first);
    hash = (hash ^ 0xFFu) * 16777619u;
    return hash_bytes(hash, key->second);
}

static bool keys_equal(const table_key *left, const table_key *right) {
    return txml_str_equal(left->first, right->first) && txml_str_equal(left->second, right->second);
}

static void *table_find(const hashed *table, const table_kind *kind, const table_key *key) {
    if (table->capacity == 0u) {
        return NULL;
    }
    const uint32_t mask = table->capacity - 1u;
    for (uint32_t slot = hash_key(key) & mask; table->slots[slot] != NULL; slot = (slot + 1u) & mask) {
        const table_key found = kind->key(table->slots[slot]);
        if (keys_equal(&found, key)) {
            return table->slots[slot];
        }
    }
    return NULL;
}

typedef struct {
    void **slots;
    uint32_t capacity;
} slot_array;

static void place(const slot_array *array, uint32_t hash, void *item) {
    void **slots = array->slots;
    const uint32_t capacity = array->capacity;
    const uint32_t mask = capacity - 1u;
    uint32_t slot = hash & mask;
    while (slots[slot] != NULL) {
        slot = (slot + 1u) & mask;
    }
    slots[slot] = item;
}

typedef struct {
    hashed *table;
    const table_kind *kind;
} table_target;

static scan table_insert(txml_parser *p, const table_target *target, void *item) {
    hashed *table = target->table;
    const table_kind *kind = target->kind;
    if ((table->count + 1u) * 2u > table->capacity) {
        const uint32_t capacity = table->capacity == 0u ? 64u : table->capacity * 2u;
        void **slots = (void **)txml_arena_alloc(&p->parser_arena, (size_t)capacity * sizeof(void *), sizeof(void *));
        if (slots == NULL) {
            return SCAN_NOMEM;
        }
        for (uint32_t i = 0u; i < capacity; i++) {
            slots[i] = NULL;
        }
        for (uint32_t i = 0u; i < table->capacity; i++) {
            if (table->slots[i] != NULL) {
                const table_key key = kind->key(table->slots[i]);
                place(&(slot_array){ slots, capacity }, hash_key(&key), table->slots[i]);
            }
        }
        table->slots = slots;
        table->capacity = capacity;
    }
    const table_key key = kind->key(item);
    place(&(slot_array){ table->slots, table->capacity }, hash_key(&key), item);
    table->count += 1u;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Keys.
 */

static table_key entity_key(const void *item) {
    const entity *e = (const entity *)item;
    return (table_key){ e->name, { "", 0u } };
}

static table_key default_key(const void *item) {
    const attlist_default *d = (const attlist_default *)item;
    return (table_key){ d->element, d->name };
}

static table_key element_key(const void *item) {
    const element_defaults *e = (const element_defaults *)item;
    return (table_key){ e->element, { "", 0u } };
}

static const table_kind entity_kind = { entity_key };
static const table_kind default_kind = { default_key };
static const table_kind element_kind = { element_key };

TXML_PRIVATE const entity *txp_find_entity(const txml_parser *p, txml_str name) {
    return (const entity *)table_find(&p->entities, &entity_kind, &(table_key){ name, { "", 0u } });
}

TXML_PRIVATE const element_defaults *txp_find_element_defaults(const txml_parser *p, txml_str element) {
    return (const element_defaults *)table_find(&p->element_defaults, &element_kind, &(table_key){ element, { "", 0u } });
}

/* ------------------------------------------------------------------------
 * Adding declarations.
 */

static bool remember(txml_parser *p, txml_str text, txml_str *copy) {
    char *bytes = text.length == 0u ? NULL : (char *)txml_arena_alloc(&p->parser_arena, text.length, 1u);
    if (text.length != 0u && bytes == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < text.length; i++) {
        bytes[i] = text.ptr[i];
    }
    *copy = (txml_str){ bytes == NULL ? "" : bytes, text.length };
    return true;
}

static scan charge(txml_parser *p, uint64_t bytes) {
    p->table_bytes += bytes;
    if (p->table_bytes > p->limits.max_document_bytes) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "the DTD tables exceed max_document_bytes"), p->construct);
    }
    return SCAN_OK;
}

TXML_PRIVATE scan txp_add_entity(txml_parser *p, txml_str name, txml_str value) {
    if (txp_find_entity(p, name) != NULL) {
        return SCAN_OK; /* the first declaration binds */
    }
    txml_str expanded;
    scan outcome = txp_expand_character_references(p, value, &expanded);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    outcome = charge(p, (uint64_t)name.length + expanded.length + sizeof(entity));
    if (outcome != SCAN_OK) {
        return outcome;
    }
    entity *e = (entity *)txml_arena_alloc(&p->parser_arena, sizeof(entity), _Alignof(entity));
    if (e == NULL || !remember(p, name, &e->name)) {
        return SCAN_NOMEM;
    }
    e->value = expanded;
    return table_insert(p, &(table_target){ &p->entities, &entity_kind }, e);
}

TXML_PRIVATE scan txp_add_default(txml_parser *p, const default_request *request) {
    if (table_find(&p->default_keys, &default_kind, &(table_key){ request->element, request->name }) != NULL) {
        return SCAN_OK;
    }
    const scan charged = charge(p, (uint64_t)request->element.length + request->name.length + request->value.length +
                                       sizeof(attlist_default));
    if (charged != SCAN_OK) {
        return charged;
    }
    attlist_default *d = (attlist_default *)txml_arena_alloc(&p->parser_arena, sizeof(attlist_default), _Alignof(attlist_default));
    if (d == NULL || !remember(p, request->element, &d->element) || !remember(p, request->name, &d->name) ||
        !remember(p, request->value, &d->value)) {
        return SCAN_NOMEM;
    }
    d->next = NULL;

    element_defaults *list = (element_defaults *)table_find(&p->element_defaults, &element_kind,
                                                            &(table_key){ request->element, { "", 0u } });
    if (list == NULL) {
        list = (element_defaults *)txml_arena_alloc(&p->parser_arena, sizeof(element_defaults), _Alignof(element_defaults));
        if (list == NULL) {
            return SCAN_NOMEM;
        }
        list->element = d->element;
        list->first = list->last = NULL;
        const scan inserted = table_insert(p, &(table_target){ &p->element_defaults, &element_kind }, list);
        if (inserted != SCAN_OK) {
            return inserted;
        }
    }
    const scan keyed = table_insert(p, &(table_target){ &p->default_keys, &default_kind }, d);
    if (keyed != SCAN_OK) {
        return keyed;
    }
    if (list->last == NULL) {
        list->first = d;
    } else {
        list->last->next = d;
    }
    list->last = d;
    return SCAN_OK;
}

/* ---- src/tag.c ---- */
/*
 * Start tags and end tags.
 *
 * A start tag is a state machine over its own grammar: name, attribute name,
 * '=', quoted value, and back. Names are bounded, so they are copied out of
 * the window as they are read; values stream into pool runs. Nothing about the
 * tag is resolved until '>' because a declaration may follow the attribute
 * that uses it: at that point the namespaces are resolved, ATTLIST defaults are
 * added, names are interned in the order the tests pin, and the element and its
 * attributes are appended together.
 */

static const char XMLNS_URI[] = "http://www.w3.org/2000/xmlns/";
static const char XML_URI[] = "http://www.w3.org/XML/1998/namespace";

/* ------------------------------------------------------------------------
 * Names and bindings.
 */

typedef struct {
    txml_str prefix;
    txml_str local;
} split_name;

static bool split_qname(txml_str written, split_name *out) {
    const uint8_t *s = (const uint8_t *)written.ptr;
    const uint32_t length = written.length;
    txml_str *prefix = &out->prefix;
    txml_str *local = &out->local;
    uint32_t colon = length;
    for (uint32_t i = 0u; i < length; i++) {
        if (s[i] == ':') {
            if (colon != length) {
                return false;
            }
            colon = i;
        }
    }
    if (colon == length) {
        *prefix = (txml_str){ "", 0u };
        *local = (txml_str){ (const char *)s, length };
        return true;
    }
    if (colon == 0u || colon + 1u == length) {
        return false;
    }
    *prefix = (txml_str){ (const char *)s, colon };
    *local = (txml_str){ (const char *)s + colon + 1u, length - colon - 1u };
    return true;
}

static const binding *lookup(const txml_parser *p, txml_str prefix) {
    for (const binding *b = p->bindings; b != NULL; b = b->prev) {
        if (txml_str_equal(b->prefix, prefix)) {
            return b;
        }
    }
    return NULL;
}

static scan push_binding(txml_parser *p, txml_str prefix, uint32_t namespace_id) {
    binding *b = (binding *)txml_arena_alloc(&p->stack_arena, sizeof(binding), _Alignof(binding));
    char *copy = prefix.length == 0u ? NULL : (char *)txml_arena_alloc(&p->stack_arena, prefix.length, 1u);
    if (b == NULL || (prefix.length != 0u && copy == NULL)) {
        return SCAN_NOMEM;
    }
    for (uint32_t i = 0u; i < prefix.length; i++) {
        copy[i] = prefix.ptr[i];
    }
    b->prev = p->bindings;
    b->prefix = (txml_str){ copy == NULL ? "" : copy, prefix.length };
    b->namespace_id = namespace_id;
    p->bindings = b;
    return SCAN_OK;
}

typedef struct {
    txml_str prefix;
    bool attribute;
    txml_position at;
} name_ref;

/* Resolve a written name to a namespace ID. */
static scan resolve(txml_parser *p, const name_ref *name, uint32_t *namespace_id) {
    if (name->prefix.length == 0u) {
        const binding *b = name->attribute ? NULL : lookup(p, name->prefix);
        *namespace_id = b == NULL ? 0u : b->namespace_id;
        return SCAN_OK;
    }
    if (txml_str_equal(name->prefix, TXML_STR("xmlns"))) {
        return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "the xmlns prefix is reserved"), name->at);
    }
    const binding *b = lookup(p, name->prefix);
    if (b != NULL) {
        *namespace_id = b->namespace_id;
        return SCAN_OK;
    }
    if (txml_str_equal(name->prefix, TXML_STR("xml"))) {
        const txml_result result = txml_doc_intern_namespace(p->doc, TXML_STR(XML_URI), namespace_id);
        return result == TXML_OK ? SCAN_OK : txp_fail_result(p, result, name->at);
    }
    return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "undeclared namespace prefix"), name->at);
}

/* ------------------------------------------------------------------------
 * Attribute values.
 */

/* Decide where a value's decoded bytes go. A namespace declaration is
 * assembled whole so it can be interned; anything else streams into the pool. */
static void begin_value(txml_parser *p, attribute_record *record) {
    const bool bare = record->prefix.length == 0u && txml_str_equal(record->local, TXML_STR("xmlns"));
    const bool prefixed = txml_str_equal(record->prefix, TXML_STR("xmlns"));

    record->declaration = bare || prefixed;
    txp_dec_reset(p, DECODE_ATTRIBUTE);
    if (record->declaration) {
        p->small.length = 0u;
        p->sink = &txp_sink_buffer;
    } else {
        txml_doc_run_begin(p->doc, &p->run);
        p->sink = &txp_sink_run;
    }
}

/* The value is complete. A declaration is stored once, as the interned
 * namespace URI, and binds its prefix; anything else is committed to the
 * pool. Safe to retry after SCAN_NOMEM. */
static scan finish_value(txml_parser *p, attribute_record *record) {
    if (!record->declaration) {
        txml_doc_run_finish(p->doc, &p->run);
        record->value_offset = p->run.offset;
        record->value_length = p->run.length;
        return SCAN_OK;
    }

    const bool bare = record->prefix.length == 0u;
    const txml_str declared = bare ? (txml_str){ "", 0u } : record->local;
    const txml_str value = { (const char *)p->small.data, p->small.length };

    /* Namespaces in XML 1.0 §3: the xmlns prefix is never declared; the xml
     * prefix is declared only as its own namespace; and neither reserved
     * namespace is bound to any other prefix or to the default. */
    const bool is_xml_uri = txml_str_equal(value, TXML_STR(XML_URI));
    const bool is_xmlns_uri = txml_str_equal(value, TXML_STR(XMLNS_URI));

    if (!bare && txml_str_equal(declared, TXML_STR("xmlns"))) {
        return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "the xmlns prefix cannot be declared"), record->position);
    }
    if (!bare && txml_str_equal(declared, TXML_STR("xml"))) {
        if (!is_xml_uri) {
            return txp_fail(p, PROBLEM(TXML_DOC_UNDECLARED_PREFIX, "the xml prefix can only be bound to the XML namespace"),
                            record->position);
        }
    } else if (is_xml_uri) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the XML namespace can only be bound to the xml prefix"), record->position);
    }
    if (is_xmlns_uri) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "the xmlns namespace cannot be bound"), record->position);
    }
    if (!bare && value.length == 0u) {
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "a prefix cannot be bound to the empty namespace"), record->position);
    }
    uint32_t namespace_id = 0u;
    const txml_result result = txml_doc_intern_namespace(p->doc, value, &namespace_id);
    if (result != TXML_OK) {
        return txp_fail_result(p, result, record->position);
    }
    const txml_namespace_entry *entry = txml_doc_namespace(p->doc, namespace_id);
    record->value_offset = entry->uri_offset;
    record->value_length = entry->uri_length;
    return push_binding(p, declared, namespace_id);
}

/* Add the ATTLIST defaults of the element that the tag did not write. */
static scan add_defaults(txml_parser *p) {
    tag_work *t = &p->tag;
    const element_defaults *list = txp_find_element_defaults(p, t->raw_name);

    if (list == NULL) {
        return SCAN_OK;
    }
    for (const attlist_default *d = list->first; d != NULL; d = d->next) {
        split_name parts;
        if (!split_qname(d->name, &parts)) {
            return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal qualified name"), p->construct);
        }
        bool written = false;
        for (uint32_t k = 0u; k < t->count; k++) {
            if (txml_str_equal(parts.prefix, p->records[k].prefix) && txml_str_equal(parts.local, p->records[k].local)) {
                written = true;
                break;
            }
        }
        if (written) {
            continue;
        }
        if (t->count >= p->limits.max_attributes) {
            return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_ATTRIBUTES, "too many attributes"), p->construct);
        }

        attribute_record *record = &p->records[t->count];
        record->prefix = parts.prefix;
        record->local = parts.local;
        record->position = p->construct;
        record->defaulted = true;
        begin_value(p, record);

        uint32_t consumed = 0u;
        scan outcome = txp_dec_feed(p, &(dec_input){ (const uint8_t *)d->value.ptr, d->value.length }, &consumed);
        if (outcome == SCAN_OK) {
            outcome = txp_dec_end(p);
        }
        if (outcome == SCAN_OK) {
            outcome = finish_value(p, record);
        }
        if (outcome != SCAN_OK) {
            return outcome;
        }
        t->count += 1u;
    }
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * The end of a start tag.
 */

/* `close` is the index of the '>' that ends the tag. */
static scan finish_tag(txml_parser *p, uint32_t close) {
    tag_work *t = &p->tag;
    scan outcome;

    /* A retry after running out of memory starts from the written attributes. */
    if (!t->finishing) {
        t->finishing = true;
        t->written = t->count;
        t->written_bindings = p->bindings;
        t->written_mark = txml_arena_position(&p->stack_arena);
    } else {
        t->count = t->written;
        p->bindings = t->written_bindings;
        txml_arena_release(&p->stack_arena, t->written_mark);
        txp_rollback_nodes(p, &t->before);
    }

    outcome = add_defaults(p);
    if (outcome != SCAN_OK) {
        return outcome;
    }

    /* Resolve the element, then each attribute in order: written first, then
     * defaults. Interning order is part of the observable name table. */
    uint32_t element_namespace = 0u;
    uint32_t element_name = 0u;
    outcome = resolve(p, &(name_ref){ t->element_prefix, false, t->name_position }, &element_namespace);
    if (outcome == SCAN_OK) {
        const intern_request request = { { element_namespace, t->element_local, t->element_prefix }, t->name_position };
        outcome = txp_intern(p, &request, &element_name);
    }
    if (outcome != SCAN_OK) {
        return outcome;
    }
    for (uint32_t k = 0u; k < t->count; k++) {
        attribute_record *record = &p->records[k];
        uint32_t namespace_id = 0u;

        if (record->declaration) {
            const txml_result result = txml_doc_intern_namespace(p->doc, TXML_STR(XMLNS_URI), &namespace_id);
            if (result != TXML_OK) {
                return txp_fail_result(p, result, record->position);
            }
        } else {
            outcome = resolve(p, &(name_ref){ record->prefix, true, record->position }, &namespace_id);
            if (outcome != SCAN_OK) {
                return outcome;
            }
        }
        const intern_request request = { { namespace_id, record->local, record->prefix }, record->position };
        outcome = txp_intern(p, &request, &record->name_id);
        if (outcome != SCAN_OK) {
            return outcome;
        }
        for (uint32_t earlier = 0u; earlier < k; earlier++) {
            if (p->records[earlier].name_id == record->name_id) {
                return txp_fail(p, PROBLEM(TXML_DOC_DUPLICATE_ATTRIBUTE, "duplicate attribute"), record->position);
            }
        }
    }

    const uint32_t index = p->doc->nodes.count;
    const txml_node_append append = {
        .node = {
            .kind = TXML_NODE_ELEMENT, .flags = t->empty ? TXML_NODE_EMPTY : TXML_NODE_OPEN,
            .depth = (uint16_t)(p->depth + 1u), .parent = txp_parent_node(p), .end = t->empty ? index + 1u : TXML_INDEX_NONE,
            .name = element_name, .as.element = { p->doc->attributes.count, t->count },
        },
        .position = p->construct,
        .end = t->empty ? txp_position_after(p, close) : (txml_position){ 0u, 0u, 0u },
    };
    uint32_t appended_index = 0u;
    txml_result result = txml_doc_append_node(p->doc, &append, &appended_index);
    if (result == TXML_OK) {
        result = txml_doc_record_prefix(p->doc, &(txml_prefix_request){ false, appended_index, t->element_prefix });
    }
    for (uint32_t k = 0u; result == TXML_OK && k < t->count; k++) {
        uint32_t name = p->records[k].name_id;
        name |= k + 1u == t->count ? TXML_ATTRIBUTE_LAST : 0u;
        name |= p->records[k].defaulted ? TXML_ATTRIBUTE_DEFAULTED : 0u;
        const attribute_record *record = &p->records[k];
        txml_attribute_append attribute = { { name, record->value_offset, record->value_length }, { { 0u, 0u, 0u }, { 0u, 0u, 0u }, { 0u, 0u, 0u } } };
        if (!record->defaulted) {
            txml_position after_quote = record->value_end;
            after_quote.offset += 1u;
            after_quote.column += 1u;
            attribute.source = (txml_attribute_source){ record->position, record->value_start, after_quote };
        }
        uint32_t attribute_index = 0u;
        result = txml_doc_append_attribute(p->doc, &attribute, &attribute_index);
        if (result == TXML_OK) {
            result = txml_doc_record_prefix(p->doc, &(txml_prefix_request){ true, attribute_index, p->records[k].prefix });
        }
    }
    if (result != TXML_OK) {
        txp_rollback_nodes(p, &t->before);
        return txp_fail_result(p, result, p->construct);
    }

    if (t->empty) {
        p->bindings = t->before.bindings;
        txml_arena_release(&p->stack_arena, t->before.mark);
        if (p->depth == 0u) {
            p->seen_root = true;
            p->stage = STAGE_EPILOG;
        }
    } else {
        p->stack[p->depth] = (open_element){ index, t->before.bindings, t->before.mark, t->raw_name };
        p->depth += 1u;
        p->stage = STAGE_CONTENT;
    }
    txml_arena_release(&p->tag_arena, t->tag_mark);
    p->appended += 1u;
    return txp_check_budget(p, p->construct);
}

/* ------------------------------------------------------------------------
 * Names within a tag.
 */

static scan finish_element_name(txml_parser *p) {
    tag_work *t = &p->tag;
    char *copy = (char *)txml_arena_alloc(&p->stack_arena, p->name_length, 1u);

    if (copy == NULL) {
        return SCAN_NOMEM;
    }
    for (uint32_t i = 0u; i < p->name_length; i++) {
        copy[i] = (char)p->name[i];
    }
    t->raw_name = (txml_str){ copy, p->name_length };
    split_name element;
    if (!split_qname(t->raw_name, &element)) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal qualified name"), p->name_position);
    }
    t->element_prefix = element.prefix;
    t->element_local = element.local;
    t->name_position = p->name_position;
    t->phase = TP_SPACE;
    t->seen_space = false;
    return SCAN_OK;
}

static scan finish_attribute_name(txml_parser *p) {
    tag_work *t = &p->tag;
    char *copy = (char *)txml_arena_alloc(&p->tag_arena, p->name_length, 1u);

    if (copy == NULL) {
        return SCAN_NOMEM;
    }
    for (uint32_t i = 0u; i < p->name_length; i++) {
        copy[i] = (char)p->name[i];
    }
    attribute_record *record = &p->records[t->count];
    record->defaulted = false;
    record->declaration = false;
    record->position = p->name_position;
    split_name attribute;
    if (!split_qname((txml_str){ copy, p->name_length }, &attribute)) {
        return txp_fail(p, PROBLEM(TXML_DOC_NAME, "not a legal qualified name"), p->name_position);
    }
    record->prefix = attribute.prefix;
    record->local = attribute.local;
    t->phase = TP_AFTER_NAME;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * The start-tag state.
 */

TXML_PRIVATE scan txp_begin_start_tag(txml_parser *p) {
    const uint8_t b = p->window[p->start];
    tag_work *t = &p->tag;

    if (p->stage == STAGE_EPILOG) {
        return txp_fail(p, PROBLEM(TXML_DOC_TRAILING_CONTENT, "a second document element"), p->construct);
    }
    if (p->depth >= p->limits.max_depth) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DEPTH, "elements nested too deeply"), p->construct);
    }
    uint32_t width = 0u;
    if (!txp_name_start_at(p->window + p->start, &width)) {
        if (b >= 0x80u || txp_name_char_at(p->window + p->start, &width)) {
            return txp_fail(p, PROBLEM(TXML_DOC_NAME, "a name cannot start with this character"), txp_position_now(p));
        }
        return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "'<' does not start markup here"), p->construct);
    }

    t->phase = TP_ELEMENT_NAME;
    t->seen_space = false;
    t->empty = false;
    t->count = 0u;
    t->finishing = false;
    t->before = txp_snapshot(p);
    t->tag_mark = txml_arena_position(&p->tag_arena);
    p->name_length = 0u;
    p->name_position = txp_position_now(p);
    p->state = &txp_state_start_tag;
    return SCAN_OK;
}

static scan start_tag_run(txml_parser *p) {
    tag_work *t = &p->tag;
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;
    scan outcome;

    while (i < limit) {
        const uint8_t b = w[i];
        uint32_t width = 0u;
        bool finish = false;

        switch (t->phase) {
        case TP_ELEMENT_NAME:
        case TP_ATTRIBUTE_NAME:
            if (txp_name_char_at(w + i, &width)) {
                if (p->name_length + width > p->limits.max_name_length) {
                    return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_NAME_LENGTH, "name too long"), p->name_position);
                }
                for (uint32_t k = 0u; k < width; k++) {
                    p->name[p->name_length++] = w[i + k];
                }
                i += width;
                break;
            }
            if (b >= 0x80u) {
                txp_commit(p, i);
                return txp_fail_name_char(p, i);
            }
            /* The name is complete; the delimiter is read again in the next phase. */
            outcome = t->phase == TP_ELEMENT_NAME ? finish_element_name(p) : finish_attribute_name(p);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            break;

        case TP_SPACE:
            if (txp_is_space(b)) {
                t->seen_space = true;
                i += 1u;
            } else if (b == '>') {
                finish = true;
            } else if (b == '/') {
                txp_commit(p, i);
                t->slash_position = txp_position_now(p);
                t->phase = TP_SLASH;
                i += 1u;
            } else if (!t->seen_space) {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "attributes must be separated by whitespace"), txp_position_at(p, i));
            } else if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an attribute name"), txp_position_at(p, i));
            } else if (t->count >= p->limits.max_attributes) {
                return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_ATTRIBUTES, "too many attributes"), txp_position_at(p, i));
            } else {
                txp_commit(p, i);
                p->name_length = 0u;
                p->name_position = txp_position_now(p);
                t->phase = TP_ATTRIBUTE_NAME;
            }
            break;

        case TP_SLASH:
            if (b != '>') {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an attribute name"), t->slash_position);
            }
            t->empty = true;
            finish = true;
            break;

        case TP_AFTER_NAME:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '=') {
                t->phase = TP_BEFORE_VALUE;
                i += 1u;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected '=' after an attribute name"), txp_position_at(p, i));
            }
            break;

        case TP_BEFORE_VALUE:
            if (txp_is_space(b)) {
                i += 1u;
            } else if (b == '"' || b == '\'') {
                begin_value(p, &p->records[t->count]);
                p->records[t->count].value_start = txp_position_after(p, i);
                t->quote = b;
                t->phase = TP_VALUE;
                i += 1u;
            } else {
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "attribute values must be quoted"), txp_position_at(p, i));
            }
            break;

        default: { /* TP_VALUE */
            uint32_t j = i;
            while (j < limit && w[j] != t->quote) {
                j += 1u;
            }
            if (j > i) {
                uint32_t consumed = 0u;
                outcome = txp_dec_feed(p, &(dec_input){ w + i, j - i }, &consumed);
                txp_commit(p, i + consumed);
                if (outcome != SCAN_OK || consumed < j - i) {
                    return outcome;
                }
            }
            i = j;
            if (i < limit) { /* the closing quote */
                p->records[t->count].value_end = txp_position_at(p, i);
                outcome = txp_dec_end(p);
                if (outcome == SCAN_OK) {
                    outcome = finish_value(p, &p->records[t->count]);
                }
                if (outcome != SCAN_OK) {
                    txp_commit(p, i);
                    return outcome;
                }
                t->count += 1u;
                t->phase = TP_SPACE;
                t->seen_space = false;
                i += 1u;
            }
            break;
        }
        }

        if (finish) {
            outcome = finish_tag(p, i);
            if (outcome != SCAN_OK) {
                txp_commit(p, i);
                return outcome;
            }
            txp_commit(p, i + 1u);
            p->state = &txp_state_content;
            return SCAN_OK;
        }
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_start_tag = { "start-tag", start_tag_run, txp_eof_in_markup };

/* ------------------------------------------------------------------------
 * End tags. The name is compared with the open element's as it arrives, so
 * nothing is buffered.
 */

TXML_PRIVATE void txp_begin_end_tag(txml_parser *p) {
    p->end_tag.phase = 0u;
    p->end_tag.matched = 0u;
    p->state = &txp_state_end_tag;
}

static scan end_tag_run(txml_parser *p) {
    const uint8_t *w = p->window;
    const uint32_t limit = txp_limit(p);
    uint32_t i = p->start;

    while (i < limit) {
        const uint8_t b = w[i];

        uint32_t width = 0u;
        if (p->end_tag.phase == 0u) {
            if (!txp_name_start_at(w + i, &width)) {
                if (b >= 0x80u) {
                    return txp_fail_name_char(p, i);
                }
                return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "expected an element name"), txp_position_at(p, i));
            }
            if (p->depth == 0u) {
                return txp_fail(p, PROBLEM(TXML_DOC_TAG_MISMATCH, "end tag with no open element"), p->construct);
            }
            p->end_tag.phase = 1u;
        } else if (p->end_tag.phase == 1u) {
            const txml_str raw = p->stack[p->depth - 1u].raw_name;
            if (txp_name_char_at(w + i, &width)) {
                for (uint32_t k = 0u; k < width; k++) {
                    if (p->end_tag.matched >= raw.length || (uint8_t)raw.ptr[p->end_tag.matched] != w[i + k]) {
                        return txp_fail(p, PROBLEM(TXML_DOC_TAG_MISMATCH, "end tag does not match the open element"),
                                        p->construct);
                    }
                    p->end_tag.matched += 1u;
                }
                i += width;
            } else if (b >= 0x80u) {
                return txp_fail_name_char(p, i);
            } else {
                if (p->end_tag.matched != raw.length) {
                    return txp_fail(p, PROBLEM(TXML_DOC_TAG_MISMATCH, "end tag does not match the open element"), p->construct);
                }
                p->end_tag.junk_position = txp_position_at(p, i);
                p->end_tag.phase = 2u;
            }
        } else if (txp_is_space(b)) {
            i += 1u;
        } else if (b == '>') {
            open_element *top = &p->stack[p->depth - 1u];
            txml_doc_close_node(p->doc, top->node, &(txml_close){ p->doc->nodes.count, txp_position_after(p, i) });
            p->bindings = top->bindings;
            txml_arena_release(&p->stack_arena, top->mark);
            p->depth -= 1u;
            if (p->depth == 0u) {
                p->seen_root = true;
                p->stage = STAGE_EPILOG;
            }
            txp_commit(p, i + 1u);
            p->state = &txp_state_content;
            return SCAN_OK;
        } else {
            return txp_fail(p, PROBLEM(TXML_DOC_SYNTAX, "junk in an end tag"), p->end_tag.junk_position);
        }
    }
    txp_commit(p, limit);
    return SCAN_OK;
}

const state txp_state_end_tag = { "end-tag", end_tag_run, txp_eof_in_markup };

/* ---- src/text.c ---- */
/* The byte comparisons txml needs without a C library. */

static bool bytes_equal(const char *left, const char *right, uint32_t length) {
    for (uint32_t i = 0u; i < length; i++) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

TXML_API bool txml_str_equal(txml_str left, txml_str right) {
    return left.length == right.length && bytes_equal(left.ptr, right.ptr, left.length);
}

TXML_API bool txml_qname_equal(const txml_qname *left, const txml_qname *right) {
    return txml_str_equal(left->ns, right->ns) && txml_str_equal(left->local, right->local);
}

TXML_API bool txml_str_starts_with(txml_str text, txml_str prefix) {
    return prefix.length <= text.length && bytes_equal(text.ptr, prefix.ptr, prefix.length);
}

TXML_PRIVATE txml_text_scan txml_text_scan_utf8(const uint8_t *s, uint32_t length) {
    txml_text_scan result = { UINT32_MAX, length };
    uint32_t i = 0u;

    while (i < length) {
        const uint8_t b = s[i];
        uint32_t need = 0u;
        uint8_t low = 0x80u, high = 0xBFu;

        if (b < 0x80u) {
            if (b < 0x20u && b != '\t' && b != '\n' && b != '\r') {
                result.invalid = i;
                result.complete = i;
                return result;
            }
            i += 1u;
            continue;
        }
        if (b >= 0xC2u && b <= 0xDFu) {
            need = 1u;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            need = 2u;
            if (b == 0xE0u) {
                low = 0xA0u;
            } else if (b == 0xEDu) {
                high = 0x9Fu;
            }
        } else if (b >= 0xF0u && b <= 0xF4u) {
            need = 3u;
            if (b == 0xF0u) {
                low = 0x90u;
            } else if (b == 0xF4u) {
                high = 0x8Fu;
            }
        } else {
            result.invalid = i;
            result.complete = i;
            return result;
        }

        /* Check the continuation bytes that are here; a short tail is
         * incomplete rather than invalid. */
        const uint32_t have = length - i - 1u < need ? length - i - 1u : need;
        for (uint32_t k = 1u; k <= have; k++) {
            const uint8_t c = s[i + k];
            const bool ok = k == 1u ? (c >= low && c <= high) : (c & 0xC0u) == 0x80u;
            if (!ok) {
                result.invalid = i;
                result.complete = i;
                return result;
            }
        }
        if (have < need) {
            result.complete = i;
            return result;
        }
        if (b == 0xEFu && s[i + 1u] == 0xBFu && (s[i + 2u] == 0xBEu || s[i + 2u] == 0xBFu)) {
            result.invalid = i; /* U+FFFE and U+FFFF */
            result.complete = i;
            return result;
        }
        i += need + 1u;
    }
    return result;
}

TXML_PRIVATE uint32_t txml_text_first_invalid(const uint8_t *s, uint32_t length) {
    const txml_text_scan scan = txml_text_scan_utf8(s, length);
    return scan.invalid != UINT32_MAX ? scan.invalid : scan.complete;
}

TXML_PRIVATE uint32_t txml_text_decode(const uint8_t *s, uint32_t *width) {
    const uint8_t b = s[0];

    if (b < 0x80u) {
        *width = 1u;
        return b;
    }
    if (b < 0xE0u) {
        *width = 2u;
        return ((uint32_t)(b & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if (b < 0xF0u) {
        *width = 3u;
        return ((uint32_t)(b & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
    }
    *width = 4u;
    return ((uint32_t)(b & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) |
           (uint32_t)(s[3] & 0x3Fu);
}

static bool in(uint32_t code, uint32_t low, uint32_t high) {
    return code >= low && code <= high;
}

TXML_PRIVATE bool txml_text_is_name_start(uint32_t c) {
    if (c < 0x80u) {
        return in(c, 'a', 'z') || in(c, 'A', 'Z') || c == '_' || c == ':';
    }
    return in(c, 0xC0u, 0xD6u) || in(c, 0xD8u, 0xF6u) || in(c, 0xF8u, 0x2FFu) || in(c, 0x370u, 0x37Du) ||
           in(c, 0x37Fu, 0x1FFFu) || in(c, 0x200Cu, 0x200Du) || in(c, 0x2070u, 0x218Fu) || in(c, 0x2C00u, 0x2FEFu) ||
           in(c, 0x3001u, 0xD7FFu) || in(c, 0xF900u, 0xFDCFu) || in(c, 0xFDF0u, 0xFFFDu) || in(c, 0x10000u, 0xEFFFFu);
}

TXML_PRIVATE bool txml_text_is_name_char(uint32_t c) {
    if (c < 0x80u) {
        return txml_text_is_name_start(c) || in(c, '0', '9') || c == '-' || c == '.';
    }
    return txml_text_is_name_start(c) || c == 0xB7u || in(c, 0x300u, 0x36Fu) || in(c, 0x203Fu, 0x2040u);
}

#if defined(__clang__)
#define TXML_NO_LOOP_IDIOMS __attribute__((no_builtin))
#elif defined(__GNUC__)
#define TXML_NO_LOOP_IDIOMS __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define TXML_NO_LOOP_IDIOMS
#endif

TXML_PRIVATE TXML_NO_LOOP_IDIOMS void txml_text_zero(void *destination, size_t length) {
    unsigned char *bytes = (unsigned char *)destination;
    for (size_t i = 0u; i < length; i++) {
        bytes[i] = 0u;
    }
}

/* ---- src/writer.c ---- */
/*
 * The writer: events in, bytes out. See docs/internal/writer.md.
 *
 * Every emit call is compiled synchronously into a small operation: all state
 * changes (frames, bindings, needs) happen at call time, and only the byte
 * production is deferred. An operation is a sequence of phases (optional XML
 * declaration, namespace declarations, tag close, main body), each of which
 * fills a short queue of segments; the queue runner writes segments into the
 * caller's buffer and stops with TXML_NEED_SPACE when it is full. A drain
 * resumes at the exact segment and byte, so no string is ever copied and no
 * byte is ever produced twice.
 *
 * txml_write_subtree is a walk over a document that compiles the same
 * operations one at a time; its position lives in the writer.
 *
 * Functions take at most three parameters; what does not fit travels in a
 * request struct.
 */

#define QUEUE_MAX 16u
#define ESCAPE_MAX 16u
#define ARENA_CHUNK ((size_t)8192u)

static const char WRITER_XMLNS_URI[] = "http://www.w3.org/2000/xmlns/";
static const char WRITER_XML_URI[] = "http://www.w3.org/XML/1998/namespace";

/* ------------------------------------------------------------------------
 * Types.
 */

typedef struct wbinding {
    struct wbinding *prev;
    txml_str prefix;
    txml_str uri;
    uint32_t depth; /* the frame depth whose start tag declared it */
} wbinding;

/* A binding a start tag still has to declare. */
typedef struct need {
    struct need *next;
    txml_str prefix;
    txml_str uri;
    bool emit;
} need;

typedef struct attribute_name {
    struct attribute_name *next;
    txml_str ns;
    txml_str local;
} attribute_name;

typedef struct {
    txml_str prefix;
    txml_str local;
    wbinding *bindings; /* the binding stack before this element's declarations */
    txml_arena_mark mark;
    uint32_t subtree_end; /* txml_write_subtree only */
    bool self_close;      /* txml_write_subtree: the source was <a/> */
    bool had_child;
    bool suppress; /* an ancestor or this element received text: no indentation */
} writer_frame;

typedef enum { STATE_PROLOG, STATE_TAG, STATE_CONTENT, STATE_EPILOG } writer_state;
typedef enum { CLOSE_NONE, CLOSE_GT, CLOSE_SELF } close_mode;
typedef enum {
    OP_NONE, OP_START, OP_ATTRIBUTE, OP_TEXT, OP_CDATA, OP_COMMENT, OP_PI, OP_END, OP_DECLARATION, OP_DOCTYPE
} op_kind;
typedef enum { SEG_RAW, SEG_TEXT, SEG_ATTRIBUTE, SEG_CDATA, SEG_INDENT } segment_kind;

typedef struct {
    const uint8_t *ptr;
    uint32_t length; /* SEG_INDENT: the spaces after the newline */
    segment_kind kind;
} segment;

typedef struct {
    segment items[QUEUE_MAX];
    uint32_t count;
    uint32_t index;
    uint32_t offset;
    uint8_t escape[ESCAPE_MAX];
    uint32_t escape_length;
    uint32_t escape_offset;
} queue;

/* The operation in progress. */
typedef struct {
    op_kind kind;
    uint32_t phase;
    uint32_t main_phase;
    bool xml_declaration;
    close_mode close;
    need *cursor; /* next declaration to emit */
    int32_t indent; /* spaces after a newline, or -1 for none */
    txml_str prefix; /* names, values: stable until the operation completes */
    txml_str local;
    txml_str value;
    txml_str extra;
    bool has_attribute_declaration; /* an invented prefix declared before the attribute */
    txml_str declared_prefix;
    txml_str declared_uri;
    bool has_release;
    txml_arena_mark release;
    /* OP_DECLARATION: a document declaration node's attributes */
    txml_cursor node;
    txml_attr_cursor attribute;
    uint32_t attributes_left;
} operation;

typedef struct {
    bool active;
    const txml_doc *doc;
    uint32_t index;
    uint32_t end;
    uint32_t base_depth;
    bool in_attributes;
    txml_attr_cursor attribute;
    uint32_t attributes_left;
} walk_state;

struct txml_writer {
    txml_allocator allocator;
    txml_limits limits;
    uint32_t flags;
    txml_buffer output;
    uint32_t output_length; /* produced and not yet consumed */

    txml_arena arena;
    writer_frame *frames;
    uint32_t depth;
    wbinding *bindings;
    writer_state state;
    bool declaration_done;
    bool pending;
    bool broken;

    /* The start tag being written. */
    need *needs;
    need *needs_tail;
    attribute_name *attribute_names;
    uint32_t attribute_count;

    operation op;
    queue queue;
    walk_state walk;
};

/* ------------------------------------------------------------------------
 * Byte-level helpers.
 */

static uint32_t literal_length(const char *literal) {
    uint32_t length = 0u;
    while (literal[length] != '\0') {
        length += 1u;
    }
    return length;
}

static txml_str str_of(const char *literal) {
    return (txml_str){ literal, literal_length(literal) };
}

/* A Name without a colon: the shape of a prefix, a local part, a PI target.
 * The bytes must be well-formed UTF-8, which is checked first. */
static bool is_ncname(txml_str name) {
    const uint8_t *bytes = (const uint8_t *)name.ptr;
    uint32_t i = 0u;

    if (name.length == 0u || txml_text_first_invalid(bytes, name.length) != name.length) {
        return false;
    }
    while (i < name.length) {
        uint32_t width = 0u;
        const uint32_t code = txml_text_decode(bytes + i, &width);
        const bool ok = i == 0u ? txml_text_is_name_start(code) : txml_text_is_name_char(code);
        if (!ok || code == ':') {
            return false;
        }
        i += width;
    }
    return true;
}

static bool valid_text(txml_str text) {
    return txml_text_first_invalid((const uint8_t *)text.ptr, text.length) == text.length;
}

static bool is_xml_target(txml_str target) {
    return target.length == 3u && (target.ptr[0] | 0x20) == 'x' && (target.ptr[1] | 0x20) == 'm' && (target.ptr[2] | 0x20) == 'l';
}

static bool put(txml_writer *w, uint8_t byte) {
    if (w->output_length == w->output.capacity) {
        return false;
    }
    w->output.ptr[w->output_length] = byte;
    w->output_length += 1u;
    return true;
}

/* ------------------------------------------------------------------------
 * The segment queue.
 */

static void queue_clear(queue *q) {
    q->count = q->index = q->offset = 0u;
    q->escape_length = q->escape_offset = 0u;
}

static bool queue_pending(const queue *q) {
    return q->index < q->count || q->escape_offset < q->escape_length;
}

static void queue_add(queue *q, segment_kind kind, txml_str text) {
    q->items[q->count++] = (segment){ (const uint8_t *)text.ptr, text.length, kind };
}

static void queue_raw(queue *q, const char *literal) {
    queue_add(q, SEG_RAW, str_of(literal));
}

static void queue_name(queue *q, txml_str prefix, txml_str local) {
    if (prefix.length != 0u) {
        queue_add(q, SEG_RAW, prefix);
        queue_raw(q, ":");
    }
    queue_add(q, SEG_RAW, local);
}

static void queue_indent(queue *q, int32_t spaces) {
    if (spaces >= 0) {
        q->items[q->count++] = (segment){ NULL, (uint32_t)spaces, SEG_INDENT };
    }
}

static void escape_set(queue *q, const char *literal) {
    q->escape_length = literal_length(literal);
    q->escape_offset = 0u;
    for (uint32_t i = 0u; i < q->escape_length; i++) {
        q->escape[i] = (uint8_t)literal[i];
    }
}

static uint32_t segment_length(const segment *s) {
    return s->kind == SEG_INDENT ? s->length + 1u : s->length;
}

/* Write queued segments. TXML_NEED_SPACE leaves the queue exactly where it
 * stopped, mid-escape included. */
static txml_result queue_run(txml_writer *w) {
    queue *q = &w->queue;

    for (;;) {
        while (q->escape_offset < q->escape_length) {
            if (!put(w, q->escape[q->escape_offset])) {
                return TXML_NEED_SPACE;
            }
            q->escape_offset += 1u;
        }
        if (q->index >= q->count) {
            return TXML_OK;
        }
        const segment *s = &q->items[q->index];
        if (q->offset >= segment_length(s)) {
            q->index += 1u;
            q->offset = 0u;
            continue;
        }

        const uint8_t byte = s->kind == SEG_INDENT ? (q->offset == 0u ? (uint8_t)'\n' : (uint8_t)' ') : s->ptr[q->offset];
        switch (s->kind) {
        case SEG_RAW:
        case SEG_INDENT:
            if (!put(w, byte)) {
                return TXML_NEED_SPACE;
            }
            q->offset += 1u;
            break;
        case SEG_TEXT:
        case SEG_ATTRIBUTE: {
            const bool attribute = s->kind == SEG_ATTRIBUTE;
            q->offset += 1u;
            if (byte == '&') {
                escape_set(q, "&amp;");
            } else if (byte == '<') {
                escape_set(q, "&lt;");
            } else if (byte == '>') {
                escape_set(q, "&gt;");
            } else if (byte == '\r') {
                escape_set(q, "&#13;");
            } else if (attribute && byte == '"') {
                escape_set(q, "&quot;");
            } else if (attribute && byte == '\t') {
                escape_set(q, "&#9;");
            } else if (attribute && byte == '\n') {
                escape_set(q, "&#10;");
            } else if (!put(w, byte)) {
                q->offset -= 1u;
                return TXML_NEED_SPACE;
            }
            break;
        }
        case SEG_CDATA:
            if (byte == ']' && q->offset + 2u < s->length && s->ptr[q->offset + 1u] == ']' && s->ptr[q->offset + 2u] == '>') {
                /* "]]>" is split across two sections; they re-parse to the same characters. */
                escape_set(q, "]]]]><![CDATA[>");
                q->offset += 3u;
            } else {
                if (!put(w, byte)) {
                    return TXML_NEED_SPACE;
                }
                q->offset += 1u;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------
 * Bindings and needs.
 */

typedef struct {
    bool bound;
    txml_str uri;
    uint32_t depth;
    const wbinding *binding;
} binding_lookup;

static binding_lookup lookup_prefix(const txml_writer *w, txml_str prefix) {
    for (const wbinding *b = w->bindings; b != NULL; b = b->prev) {
        if (txml_str_equal(b->prefix, prefix)) {
            return (binding_lookup){ true, b->uri, b->depth, b };
        }
    }
    if (txml_str_equal(prefix, TXML_STR("xml"))) {
        return (binding_lookup){ true, TXML_STR(WRITER_XML_URI), 0u, NULL };
    }
    return (binding_lookup){ false, { "", 0u }, 0u, NULL };
}

static bool copy_str(txml_writer *w, txml_str text, txml_str *copy) {
    char *bytes = text.length == 0u ? NULL : (char *)txml_arena_alloc(&w->arena, text.length, 1u);
    if (text.length != 0u && bytes == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < text.length; i++) {
        bytes[i] = text.ptr[i];
    }
    *copy = (txml_str){ bytes == NULL ? "" : bytes, text.length };
    return true;
}

typedef struct {
    txml_str prefix;
    txml_str uri;
} declaration_request;

static txml_result push_writer_binding(txml_writer *w, const declaration_request *request) {
    wbinding *b = (wbinding *)txml_arena_alloc(&w->arena, sizeof(wbinding), _Alignof(wbinding));
    if (b == NULL || !copy_str(w, request->prefix, &b->prefix) || !copy_str(w, request->uri, &b->uri)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    b->prev = w->bindings;
    b->depth = w->depth;
    w->bindings = b;
    return TXML_OK;
}

/* Record that the tag being written must declare prefix -> uri unless a
 * declaration written explicitly says the same by the time it closes. */
static txml_result add_need(txml_writer *w, const declaration_request *request) {
    for (const need *n = w->needs; n != NULL; n = n->next) {
        if (txml_str_equal(n->prefix, request->prefix)) {
            return txml_str_equal(n->uri, request->uri) ? TXML_OK : TXML_ERR_INVALID;
        }
    }
    need *n = (need *)txml_arena_alloc(&w->arena, sizeof(need), _Alignof(need));
    if (n == NULL || !copy_str(w, request->prefix, &n->prefix) || !copy_str(w, request->uri, &n->uri)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    n->next = NULL;
    n->emit = false;
    if (w->needs_tail == NULL) {
        w->needs = n;
    } else {
        w->needs_tail->next = n;
    }
    w->needs_tail = n;
    return TXML_OK;
}

/* A need whose binding is already in scope is a no-op. */
static txml_result require_binding(txml_writer *w, const declaration_request *request) {
    const binding_lookup found = lookup_prefix(w, request->prefix);
    if (found.bound && txml_str_equal(found.uri, request->uri)) {
        return TXML_OK;
    }
    if (found.bound && found.depth == w->depth && w->depth != 0u && found.binding != NULL) {
        return TXML_ERR_INVALID; /* the same tag already binds it differently */
    }
    return add_need(w, request);
}

/* The innermost, unshadowed binding of `uri` to a non-default prefix (or, if
 * allowed, the default one). */
typedef struct {
    txml_str uri;
    bool allow_default;
} prefix_query;

static bool find_prefix_for(const txml_writer *w, const prefix_query *query, txml_str *prefix) {
    const txml_str uri = query->uri;
    const bool allow_default = query->allow_default;
    for (const wbinding *b = w->bindings; b != NULL; b = b->prev) {
        if (!txml_str_equal(b->uri, uri) || (b->prefix.length == 0u && !allow_default)) {
            continue;
        }
        if (lookup_prefix(w, b->prefix).binding == b) {
            *prefix = b->prefix;
            return true;
        }
    }
    for (const need *n = w->needs; n != NULL; n = n->next) {
        if (txml_str_equal(n->uri, uri) && (n->prefix.length != 0u || allow_default)) {
            *prefix = n->prefix;
            return true;
        }
    }
    return false;
}

static bool prefix_in_use(const txml_writer *w, txml_str prefix) {
    if (lookup_prefix(w, prefix).bound) {
        return true;
    }
    for (const need *n = w->needs; n != NULL; n = n->next) {
        if (txml_str_equal(n->prefix, prefix)) {
            return true;
        }
    }
    return false;
}

/* nsN, the first one nothing in scope or pending uses. */
static bool invent_prefix(txml_writer *w, txml_str *prefix) {
    char digits[12];
    for (uint32_t n = 1u;; n++) {
        uint32_t length = 0u;
        for (uint32_t v = n; v != 0u; v /= 10u) {
            digits[length++] = (char)('0' + v % 10u);
        }
        char *name = (char *)txml_arena_alloc(&w->arena, 2u + length, 1u);
        if (name == NULL) {
            return false;
        }
        name[0] = 'n';
        name[1] = 's';
        for (uint32_t i = 0u; i < length; i++) {
            name[2u + i] = digits[length - 1u - i];
        }
        const txml_str candidate = { name, 2u + length };
        if (!prefix_in_use(w, candidate)) {
            *prefix = candidate;
            return true;
        }
    }
}

/* ------------------------------------------------------------------------
 * Operations.
 */

static void op_begin(txml_writer *w, op_kind kind) {
    txml_text_zero(&w->op, sizeof(w->op));
    w->op.kind = kind;
    w->op.indent = -1;
    queue_clear(&w->queue);
}

/* Whether the writer is where a document allows no content: before or after
 * its element. A fragment (TXML_WRITE_FRAGMENT) has no such place. */
static bool outside_root(const txml_writer *w) {
    return (w->state == STATE_PROLOG || w->state == STATE_EPILOG) && (w->flags & TXML_WRITE_FRAGMENT) == 0u;
}

static void op_declaration_if_needed(txml_writer *w) {
    if ((w->flags & TXML_WRITE_DECLARATION) != 0u && !w->declaration_done) {
        w->declaration_done = true;
        w->op.xml_declaration = true;
    }
}

/* Decide which needs the tag being closed must emit, and bind them. */
static txml_result settle_tag(txml_writer *w, close_mode mode) {
    if (w->state != STATE_TAG) {
        return TXML_OK;
    }
    for (need *n = w->needs; n != NULL; n = n->next) {
        const binding_lookup found = lookup_prefix(w, n->prefix);
        if (found.bound && txml_str_equal(found.uri, n->uri)) {
            continue;
        }
        if (found.bound && found.binding != NULL && found.depth == w->depth) {
            return TXML_ERR_INVALID;
        }
        if (n->prefix.length == 0u && n->uri.length == 0u && !found.bound) {
            continue; /* undeclaring a default that is not there */
        }
        n->emit = true;
        const txml_result pushed = push_writer_binding(w, &(declaration_request){ n->prefix, n->uri });
        if (pushed != TXML_OK) {
            return pushed;
        }
    }
    w->op.cursor = w->needs;
    w->op.close = mode;
    w->needs = w->needs_tail = NULL;
    w->state = STATE_CONTENT;
    return TXML_OK;
}

static void tag_reset(txml_writer *w) {
    w->needs = w->needs_tail = NULL;
    w->attribute_names = NULL;
    w->attribute_count = 0u;
}

typedef struct {
    txml_str ns;
    txml_str local;
    txml_str prefix;
    bool exact; /* the prefix is authoritative: an empty one means unprefixed */
} name_spec;

static bool ns_is_xmlns(txml_str ns) {
    return txml_str_equal(ns, TXML_STR(WRITER_XMLNS_URI));
}

/* Checks that need no state. */
static txml_result validate_name(const name_spec *name, bool trusted) {
    if (trusted) {
        return TXML_OK;
    }
    if (!is_ncname(name->local) || !valid_text(name->ns)) {
        return TXML_ERR_INVALID;
    }
    if (name->prefix.length != 0u) {
        if (!is_ncname(name->prefix) || name->ns.length == 0u || txml_str_equal(name->prefix, TXML_STR("xmlns"))) {
            return TXML_ERR_INVALID;
        }
        if (txml_str_equal(name->prefix, TXML_STR("xml")) != txml_str_equal(name->ns, TXML_STR(WRITER_XML_URI))) {
            return TXML_ERR_INVALID;
        }
    }
    return TXML_OK;
}

/* Choose the prefix an element is written with, recording what it needs. */
static txml_result plan_element(txml_writer *w, const name_spec *name, txml_str *prefix) {
    *prefix = name->prefix;

    if (name->prefix.length != 0u) {
        return txml_str_equal(name->prefix, TXML_STR("xml")) ? TXML_OK : require_binding(w, &(declaration_request){ name->prefix, name->ns });
    }
    if (name->ns.length == 0u) {
        /* Unqualified: undeclare a default that would otherwise capture it. */
        const binding_lookup found = lookup_prefix(w, (txml_str){ "", 0u });
        if (found.bound && found.uri.length != 0u) {
            return add_need(w, &(declaration_request){ { "", 0u }, { "", 0u } });
        }
        return TXML_OK;
    }
    if (name->exact) {
        return require_binding(w, &(declaration_request){ { "", 0u }, name->ns });
    }
    if (txml_str_equal(name->ns, TXML_STR(WRITER_XML_URI))) {
        *prefix = TXML_STR("xml");
        return TXML_OK;
    }
    if (find_prefix_for(w, &(prefix_query){ name->ns, true }, prefix)) {
        return TXML_OK;
    }
    const binding_lookup found = lookup_prefix(w, (txml_str){ "", 0u });
    if (!found.bound || found.uri.length == 0u) {
        *prefix = (txml_str){ "", 0u };
        return add_need(w, &(declaration_request){ { "", 0u }, name->ns });
    }
    if (!invent_prefix(w, prefix)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    return add_need(w, &(declaration_request){ *prefix, name->ns });
}

/* ------------------------------------------------------------------------
 * Emit-call compilation.
 */

typedef struct {
    name_spec name;
    uint32_t subtree_end;
    bool self_close;
    bool trusted;
} start_request;

static txml_result check_output_ready(const txml_writer *w) {
    return w == NULL || w->pending || w->broken || w->op.kind != OP_NONE ? TXML_ERR_INVALID : TXML_OK;
}

static int32_t indent_for(const txml_writer *w) {
    if ((w->flags & TXML_WRITE_INDENT) == 0u || w->depth == 0u || w->frames[w->depth - 1u].suppress) {
        return -1;
    }
    return (int32_t)(2u * w->depth);
}

static void mark_child(txml_writer *w) {
    if (w->depth != 0u) {
        w->frames[w->depth - 1u].had_child = true;
    }
}

static txml_result compile_start(txml_writer *w, const start_request *request) {
    txml_result result = validate_name(&request->name, request->trusted);
    if (result != TXML_OK) {
        return result;
    }
    if (w->state == STATE_EPILOG && (w->flags & TXML_WRITE_FRAGMENT) == 0u) {
        return TXML_ERR_INVALID;
    }
    if (w->depth >= w->limits.max_depth ||
        (uint64_t)request->name.prefix.length + request->name.local.length + 1u > w->limits.max_name_length) {
        return TXML_ERR_LIMIT;
    }

    op_begin(w, OP_START);
    op_declaration_if_needed(w);
    result = settle_tag(w, CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    w->op.indent = indent_for(w);
    const bool suppress = w->depth != 0u && w->frames[w->depth - 1u].suppress;
    mark_child(w);

    /* The new tag's declarations belong to the new depth. */
    const txml_arena_mark mark = txml_arena_position(&w->arena);
    writer_frame *f = &w->frames[w->depth];
    f->bindings = w->bindings;
    f->mark = mark;
    tag_reset(w);
    w->depth += 1u;

    txml_str prefix = { "", 0u };
    result = plan_element(w, &request->name, &prefix);
    if (result == TXML_OK && (!copy_str(w, prefix, &f->prefix) || !copy_str(w, request->name.local, &f->local))) {
        result = TXML_ERR_NOMEM;
    }
    if (result != TXML_OK) {
        w->broken = true; /* the previous tag is already settled: not recoverable */
        return result;
    }
    f->subtree_end = request->subtree_end;
    f->self_close = request->self_close;
    f->had_child = false;
    f->suppress = suppress;
    w->state = STATE_TAG;
    w->op.prefix = f->prefix;
    w->op.local = f->local;
    return TXML_OK;
}

typedef struct {
    name_spec name;
    txml_str value;
    bool trusted;
} attribute_request;

static txml_result compile_attribute(txml_writer *w, const attribute_request *request) {
    if (w->state != STATE_TAG) {
        return TXML_ERR_INVALID;
    }
    const name_spec *name = &request->name;
    const bool declaration = ns_is_xmlns(name->ns);
    txml_result result = TXML_OK;

    if (!request->trusted && (!valid_text(request->value) || !is_ncname(name->local) || !valid_text(name->ns))) {
        return TXML_ERR_INVALID;
    }
    if (!declaration) {
        name_spec checked = *name;
        result = validate_name(&checked, request->trusted);
        if (result != TXML_OK) {
            return result;
        }
    }
    if (w->attribute_count >= w->limits.max_attributes) {
        return TXML_ERR_LIMIT;
    }
    const bool default_declaration = declaration && txml_str_equal(name->local, TXML_STR("xmlns"));
    if (declaration && !default_declaration && request->value.length == 0u) {
        return TXML_ERR_INVALID;
    }
    for (const attribute_name *n = w->attribute_names; n != NULL; n = n->next) {
        if (txml_str_equal(n->ns, name->ns) && txml_str_equal(n->local, name->local)) {
            return TXML_ERR_INVALID; /* the same expanded name twice on one tag */
        }
    }

    op_begin(w, OP_ATTRIBUTE);
    txml_str prefix = name->prefix;
    txml_str invented = { "", 0u };
    bool invent = false;

    if (declaration) {
        prefix = default_declaration ? (txml_str){ "", 0u } : TXML_STR("xmlns");
    } else if (name->ns.length == 0u) {
        prefix = (txml_str){ "", 0u };
    } else if (name->prefix.length != 0u) {
        if (!txml_str_equal(name->prefix, TXML_STR("xml"))) {
            result = require_binding(w, &(declaration_request){ name->prefix, name->ns });
        }
    } else if (txml_str_equal(name->ns, TXML_STR(WRITER_XML_URI))) {
        prefix = TXML_STR("xml");
    } else if (!find_prefix_for(w, &(prefix_query){ name->ns, false }, &prefix)) {
        /* An attribute cannot use the default namespace: it needs a prefix now. */
        invent = invent_prefix(w, &invented);
        if (!invent) {
            w->broken = true;
            return TXML_ERR_NOMEM;
        }
        prefix = invented;
    }
    if (result != TXML_OK) {
        return result;
    }

    attribute_name *entry = (attribute_name *)txml_arena_alloc(&w->arena, sizeof(attribute_name), _Alignof(attribute_name));
    if (entry == NULL || !copy_str(w, name->ns, &entry->ns) || !copy_str(w, name->local, &entry->local)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    entry->next = w->attribute_names;
    w->attribute_names = entry;
    w->attribute_count += 1u;

    if (declaration) {
        result = push_writer_binding(w, &(declaration_request){ default_declaration ? (txml_str){ "", 0u } : name->local, request->value });
        w->op.prefix = prefix;
    } else {
        if (invent) {
            result = push_writer_binding(w, &(declaration_request){ invented, name->ns });
            w->op.has_attribute_declaration = true;
            w->op.declared_prefix = invented;
            w->op.declared_uri = name->ns;
            /* The invented prefix is also an expanded-name entry on this tag. */
            attribute_name *declared = (attribute_name *)txml_arena_alloc(&w->arena, sizeof(attribute_name), _Alignof(attribute_name));
            if (declared == NULL || !copy_str(w, TXML_STR(WRITER_XMLNS_URI), &declared->ns) || !copy_str(w, invented, &declared->local)) {
                w->broken = true;
                return TXML_ERR_NOMEM;
            }
            declared->next = w->attribute_names;
            w->attribute_names = declared;
        }
        w->op.prefix = prefix;
    }
    if (result == TXML_OK && !copy_str(w, w->op.prefix, &w->op.prefix)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    w->op.local = entry->local;
    w->op.value = request->value;
    return result;
}

typedef struct {
    txml_str text;
    op_kind kind; /* OP_TEXT or OP_CDATA */
    bool trusted;
} text_request;

static txml_result compile_text(txml_writer *w, const text_request *request) {
    if (!request->trusted) {
        if (!valid_text(request->text)) {
            return TXML_ERR_INVALID;
        }
        if (outside_root(w)) {
            return TXML_ERR_INVALID;
        }
    }
    if (request->kind == OP_TEXT && request->text.length == 0u) {
        return TXML_OK; /* nothing to write, and it must not close the tag */
    }
    if (request->kind == OP_CDATA && outside_root(w)) {
        return TXML_ERR_INVALID;
    }

    op_begin(w, request->kind);
    op_declaration_if_needed(w);
    const txml_result result = settle_tag(w, CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    if (w->depth != 0u) {
        w->frames[w->depth - 1u].suppress = true;
    }
    w->op.value = request->text;
    return TXML_OK;
}

typedef struct {
    txml_str first;  /* the comment text, or the PI target */
    txml_str second; /* the PI data */
    op_kind kind;    /* OP_COMMENT or OP_PI */
    bool trusted;
} leaf_request;

static txml_result compile_leaf(txml_writer *w, const leaf_request *request) {
    if (!request->trusted) {
        if (!valid_text(request->first) || !valid_text(request->second)) {
            return TXML_ERR_INVALID;
        }
        if (request->kind == OP_COMMENT) {
            const txml_str text = request->first;
            if (text.length != 0u && text.ptr[text.length - 1u] == '-') {
                return TXML_ERR_INVALID;
            }
            for (uint32_t i = 0u; i + 1u < text.length; i++) {
                if (text.ptr[i] == '-' && text.ptr[i + 1u] == '-') {
                    return TXML_ERR_INVALID;
                }
            }
        } else {
            if (!is_ncname(request->first) || is_xml_target(request->first)) {
                return TXML_ERR_INVALID;
            }
            for (uint32_t i = 0u; i + 1u < request->second.length; i++) {
                if (request->second.ptr[i] == '?' && request->second.ptr[i + 1u] == '>') {
                    return TXML_ERR_INVALID;
                }
            }
        }
    }

    op_begin(w, request->kind);
    op_declaration_if_needed(w);
    const txml_result result = settle_tag(w, CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    w->op.indent = indent_for(w);
    mark_child(w);
    w->op.value = request->first;
    w->op.extra = request->second;
    return TXML_OK;
}

typedef struct {
    bool walking;
    bool self_close;
} end_request;

static txml_result compile_end(txml_writer *w, const end_request *request) {
    if (w->depth == 0u) {
        return TXML_ERR_INVALID;
    }
    op_begin(w, OP_END);
    const writer_frame *top = &w->frames[w->depth - 1u];
    bool self = false;

    if (w->state == STATE_TAG) {
        self = request->walking ? request->self_close : (w->flags & TXML_WRITE_EMPTY_TAGS) != 0u;
    }
    const txml_result result = settle_tag(w, self ? CLOSE_SELF : CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    w->op.prefix = top->prefix;
    w->op.local = top->local;
    w->op.has_release = true;
    w->op.release = top->mark;
    w->bindings = top->bindings;
    w->depth -= 1u;
    if (!self) {
        w->op.main_phase = 0u;
        w->op.indent = top->had_child && !top->suppress && (w->flags & TXML_WRITE_INDENT) != 0u ? (int32_t)(2u * w->depth) : -1;
    } else {
        w->op.main_phase = 1u; /* nothing to write after "/>" */
    }
    w->state = w->depth == 0u ? STATE_EPILOG : STATE_CONTENT;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * The operation stepper.
 */

static void queue_declaration(txml_writer *w, const need *n) {
    queue *q = &w->queue;
    if (n->prefix.length == 0u) {
        queue_raw(q, " xmlns=\"");
    } else {
        queue_raw(q, " xmlns:");
        queue_add(q, SEG_RAW, n->prefix);
        queue_raw(q, "=\"");
    }
    queue_add(q, SEG_ATTRIBUTE, n->uri);
    queue_raw(q, "\"");
}

/* Fill the queue with the operation's main body, phase by phase. Returns
 * true when the body is finished. */
static bool step_main(txml_writer *w) {
    queue *q = &w->queue;
    operation *op = &w->op;
    const uint32_t phase = op->main_phase++;

    switch (op->kind) {
    case OP_START:
        queue_indent(q, op->indent);
        queue_raw(q, "<");
        queue_name(q, op->prefix, op->local);
        (void)phase;
        return true;
    case OP_ATTRIBUTE:
        if (phase == 0u && op->has_attribute_declaration) {
            queue_raw(q, " xmlns:");
            queue_add(q, SEG_RAW, op->declared_prefix);
            queue_raw(q, "=\"");
            queue_add(q, SEG_ATTRIBUTE, op->declared_uri);
            queue_raw(q, "\"");
            return false;
        }
        queue_raw(q, " ");
        queue_name(q, op->prefix, op->local);
        queue_raw(q, "=\"");
        queue_add(q, SEG_ATTRIBUTE, op->value);
        queue_raw(q, "\"");
        return true;
    case OP_TEXT:
        queue_add(q, SEG_TEXT, op->value);
        return true;
    case OP_CDATA:
        queue_raw(q, "<![CDATA[");
        queue_add(q, SEG_CDATA, op->value);
        queue_raw(q, "]]>");
        return true;
    case OP_COMMENT:
        queue_indent(q, op->indent);
        queue_raw(q, "<!--");
        queue_add(q, SEG_RAW, op->value);
        queue_raw(q, "-->");
        return true;
    case OP_PI:
        queue_indent(q, op->indent);
        queue_raw(q, "<?");
        queue_add(q, SEG_RAW, op->value);
        if (op->extra.length != 0u) {
            queue_raw(q, " ");
            queue_add(q, SEG_RAW, op->extra);
        }
        queue_raw(q, "?>");
        return true;
    case OP_END:
        if (phase == 0u) {
            queue_indent(q, op->indent);
            queue_raw(q, "</");
            queue_name(q, op->prefix, op->local);
            queue_raw(q, ">");
        }
        return true;
    case OP_DOCTYPE:
        queue_raw(q, "<!DOCTYPE");
        queue_add(q, SEG_RAW, op->value);
        queue_raw(q, ">");
        return true;
    case OP_DECLARATION:
        if (phase == 0u) {
            queue_raw(q, "<?xml");
            return false;
        }
        if (op->attributes_left == 0u) {
            queue_raw(q, "?>");
            return true;
        }
        {
            txml_attribute_view view;
            txml_qname name = { { "", 0u }, { "", 0u } };
            if (txml_attribute_read(op->attribute, &view) == TXML_OK) {
                txml_doc_name_at(op->attribute.doc, view.name, &name);
            }
            queue_raw(q, " ");
            queue_add(q, SEG_RAW, name.local);
            queue_raw(q, "=\"");
            queue_add(q, SEG_ATTRIBUTE, view.value);
            queue_raw(q, "\"");
            op->attributes_left -= 1u;
            if (op->attributes_left != 0u) {
                txml_next_attribute(&op->attribute);
            }
        }
        return false;
    case OP_NONE:
        break;
    }
    return true;
}

/* Called with an empty queue: fill it with the next piece of the operation, or
 * finish the operation. */
static void step_op(txml_writer *w) {
    operation *op = &w->op;

    switch (op->phase) {
    case 0:
        op->phase = 1u;
        if (op->xml_declaration) {
            queue_raw(&w->queue, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
            if ((w->flags & TXML_WRITE_INDENT) != 0u) {
                queue_raw(&w->queue, "\n");
            }
        }
        return;
    case 1:
        while (op->cursor != NULL) {
            const need *n = op->cursor;
            op->cursor = n->next;
            if (n->emit) {
                queue_declaration(w, n);
                return;
            }
        }
        op->phase = 2u;
        return;
    case 2:
        op->phase = 3u;
        if (op->close == CLOSE_GT) {
            queue_raw(&w->queue, ">");
        } else if (op->close == CLOSE_SELF) {
            queue_raw(&w->queue, "/>");
        }
        return;
    case 3:
        if (step_main(w)) {
            op->phase = 4u;
        }
        return;
    default:
        if (op->has_release) {
            txml_arena_release(&w->arena, op->release);
        }
        op->kind = OP_NONE;
        return;
    }
}

/* ------------------------------------------------------------------------
 * The subtree walk.
 */

static txml_result walk_element(txml_writer *w, txml_cursor node) {
    txml_qname name;
    const uint32_t id = txml_node_name_id(node);
    if (txml_doc_name_at(node.doc, id, &name) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    const txml_result result = compile_start(w, &(start_request){
        { name.ns, name.local, txml_node_prefix(node), true }, txml_subtree_end(node),
        (txml_flags(node) & TXML_NODE_EMPTY) != 0u, true });
    if (result != TXML_OK) {
        return result;
    }
    w->walk.attributes_left = txml_attribute_count(node);
    w->walk.in_attributes = true;
    if (w->walk.attributes_left != 0u) {
        txml_first_attribute(node, &w->walk.attribute);
    }
    return TXML_OK;
}

static txml_result walk_attribute(txml_writer *w) {
    walk_state *walk = &w->walk;
    txml_attribute_view view;
    txml_qname name;

    if (txml_attribute_read(walk->attribute, &view) != TXML_OK || txml_doc_name_at(walk->doc, view.name, &name) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    walk->attributes_left -= 1u;
    if (walk->attributes_left != 0u) {
        txml_next_attribute(&walk->attribute);
    }
    if ((view.flags & TXML_ATTR_DEFAULTED) != 0u) {
        return TXML_OK; /* supplied by the DTD, not written */
    }
    return compile_attribute(w, &(attribute_request){ { name.ns, name.local, view.prefix, true }, view.value, true });
}

static txml_result walk_node(txml_writer *w) {
    walk_state *walk = &w->walk;
    const txml_cursor node = txml_cursor_at(walk->doc, walk->index);
    const txml_node_kind kind = txml_kind(node);
    const txml_str text = txml_node_text(node);

    walk->index += 1u;
    switch (kind) {
    case TXML_NODE_DOCUMENT:
        return TXML_OK;
    case TXML_NODE_ELEMENT:
        return walk_element(w, node);
    case TXML_NODE_TEXT:
        return compile_text(w, &(text_request){ text, (txml_flags(node) & TXML_NODE_CDATA) != 0u ? OP_CDATA : OP_TEXT, true });
    case TXML_NODE_COMMENT:
        return compile_leaf(w, &(leaf_request){ text, { "", 0u }, OP_COMMENT, true });
    case TXML_NODE_PI: {
        txml_qname target;
        if (txml_doc_name_at(walk->doc, txml_node_name_id(node), &target) != TXML_OK) {
            return TXML_ERR_INVALID;
        }
        return compile_leaf(w, &(leaf_request){ target.local, text, OP_PI, true });
    }
    case TXML_NODE_DECLARATION:
        if (w->state != STATE_PROLOG || w->depth != 0u || (w->flags & TXML_WRITE_FRAGMENT) != 0u) {
            return TXML_ERR_INVALID;
        }
        op_begin(w, OP_DECLARATION);
        w->declaration_done = true;
        w->op.attributes_left = txml_attribute_count(node);
        if (w->op.attributes_left != 0u) {
            txml_first_attribute(node, &w->op.attribute);
        }
        return TXML_OK;
    case TXML_NODE_DOCTYPE:
        if (w->state != STATE_PROLOG || w->depth != 0u || (w->flags & TXML_WRITE_FRAGMENT) != 0u) {
            return TXML_ERR_INVALID;
        }
        op_begin(w, OP_DOCTYPE);
        op_declaration_if_needed(w);
        w->op.value = text;
        return TXML_OK;
    }
    return TXML_ERR_INVALID;
}

static txml_result walk_step(txml_writer *w) {
    walk_state *walk = &w->walk;

    if (walk->in_attributes) {
        if (walk->attributes_left != 0u) {
            return walk_attribute(w);
        }
        walk->in_attributes = false;
    }
    if (w->depth > walk->base_depth && w->frames[w->depth - 1u].subtree_end == walk->index) {
        return compile_end(w, &(end_request){ true, w->frames[w->depth - 1u].self_close });
    }
    if (walk->index >= walk->end) {
        walk->active = false;
        return TXML_OK;
    }
    return walk_node(w);
}

/* ------------------------------------------------------------------------
 * Driving.
 */

static txml_result advance(txml_writer *w) {
    for (;;) {
        if (queue_pending(&w->queue)) {
            if (queue_run(w) == TXML_NEED_SPACE) {
                w->pending = true;
                return TXML_NEED_SPACE;
            }
        }
        if (w->op.kind != OP_NONE) {
            queue_clear(&w->queue);
            step_op(w);
            continue;
        }
        if (w->walk.active) {
            const txml_result result = walk_step(w);
            if (result != TXML_OK) {
                w->walk.active = false;
                w->op.kind = OP_NONE;
                queue_clear(&w->queue);
                w->pending = false;
                return result;
            }
            continue;
        }
        w->pending = false;
        return TXML_OK;
    }
}

/* Run a freshly compiled operation, turning a compile failure into the result. */
static txml_result run(txml_writer *w, txml_result compiled) {
    if (compiled != TXML_OK) {
        w->op.kind = OP_NONE; /* a refused call leaves nothing pending */
        queue_clear(&w->queue);
        return compiled;
    }
    return advance(w);
}

/* ------------------------------------------------------------------------
 * Public API.
 */

TXML_API txml_result txml_writer_init(txml_writer **out, const txml_writer_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->allocator.v->alloc == NULL ||
        config->allocator.v->free == NULL || config->output.ptr == NULL || config->output.capacity == 0u ||
        config->limits.max_depth == 0u || config->limits.max_depth > 65535u || config->limits.max_name_length == 0u ||
        (config->flags & (TXML_WRITE_FRAGMENT | TXML_WRITE_DECLARATION)) == (TXML_WRITE_FRAGMENT | TXML_WRITE_DECLARATION)) {
        return TXML_ERR_INVALID;
    }
    txml_writer *w = (txml_writer *)config->allocator.v->alloc(config->allocator.self, sizeof(txml_writer), _Alignof(txml_writer));
    if (w == NULL) {
        return TXML_ERR_NOMEM;
    }
    w->allocator = config->allocator;
    w->limits = config->limits;
    w->flags = config->flags;
    w->output = config->output;
    w->output_length = 0u;
    w->frames = NULL;
    w->depth = 0u;
    w->bindings = NULL;
    w->state = STATE_PROLOG;
    w->declaration_done = false;
    w->pending = false;
    w->broken = false;
    tag_reset(w);
    txml_text_zero(&w->op, sizeof(w->op));
    queue_clear(&w->queue);
    txml_text_zero(&w->walk, sizeof(w->walk));
    w->arena.head = NULL;

    txml_result result = txml_arena_init(&w->arena, config->allocator, ARENA_CHUNK);
    if (result == TXML_OK) {
        w->frames = (writer_frame *)txml_arena_alloc(&w->arena, (size_t)config->limits.max_depth * sizeof(writer_frame), _Alignof(writer_frame));
        if (w->frames == NULL) {
            result = TXML_ERR_NOMEM;
        }
    }
    if (result != TXML_OK) {
        txml_writer_destroy(w);
        return result;
    }
    *out = w;
    return TXML_OK;
}

TXML_API txml_result txml_write_start(txml_writer *w, const txml_write_name *name) {
    if (check_output_ready(w) != TXML_OK || name == NULL) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_start(w, &(start_request){ { name->ns, name->local, name->prefix, false }, TXML_INDEX_NONE, false, false }));
}

TXML_API txml_result txml_write_attribute(txml_writer *w, const txml_write_attr *attribute) {
    if (check_output_ready(w) != TXML_OK || attribute == NULL) {
        return TXML_ERR_INVALID;
    }
    const txml_write_name *name = &attribute->name;
    return run(w, compile_attribute(w, &(attribute_request){ { name->ns, name->local, name->prefix, false }, attribute->value, false }));
}

TXML_API txml_result txml_write_text(txml_writer *w, txml_str text) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_text(w, &(text_request){ text, OP_TEXT, false }));
}

TXML_API txml_result txml_write_cdata(txml_writer *w, txml_str text) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_text(w, &(text_request){ text, OP_CDATA, false }));
}

TXML_API txml_result txml_write_comment(txml_writer *w, txml_str text) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_leaf(w, &(leaf_request){ text, { "", 0u }, OP_COMMENT, false }));
}

TXML_API txml_result txml_write_pi(txml_writer *w, txml_str target, txml_str data) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_leaf(w, &(leaf_request){ target, data, OP_PI, false }));
}

TXML_API txml_result txml_write_end(txml_writer *w) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_end(w, &(end_request){ false, false }));
}

TXML_API txml_result txml_write_subtree(txml_writer *w, txml_cursor cursor) {
    if (check_output_ready(w) != TXML_OK || w->walk.active) {
        return TXML_ERR_INVALID;
    }
    const uint32_t end = txml_subtree_end(cursor);
    if (cursor.doc == NULL || cursor.node >= txml_doc_node_count(cursor.doc) || end == TXML_INDEX_NONE) {
        return TXML_ERR_INVALID; /* absent, or still open: no bound to walk to */
    }
    if (w->state == STATE_EPILOG && (w->flags & TXML_WRITE_FRAGMENT) == 0u) {
        return TXML_ERR_INVALID;
    }
    /* A document that carries its own declaration never gets a second one. */
    if (txml_kind(cursor) == TXML_NODE_DOCUMENT && cursor.node + 1u < end &&
        txml_kind(txml_cursor_at(cursor.doc, cursor.node + 1u)) == TXML_NODE_DECLARATION) {
        w->declaration_done = true;
    }
    w->walk = (walk_state){ .active = true, .doc = cursor.doc, .index = cursor.node, .end = end, .base_depth = w->depth };
    return advance(w);
}

TXML_API txml_bytes txml_writer_output(const txml_writer *w) {
    if (w == NULL) {
        return (txml_bytes){ NULL, 0u };
    }
    return (txml_bytes){ w->output.ptr, w->output_length };
}

TXML_API txml_result txml_writer_drain(txml_writer *w) {
    if (w == NULL) {
        return TXML_ERR_INVALID;
    }
    w->output_length = 0u;
    if (!w->pending) {
        return TXML_OK;
    }
    return advance(w);
}

TXML_API txml_result txml_writer_finish(txml_writer *w) {
    if (w == NULL || w->pending || w->broken || w->op.kind != OP_NONE || w->walk.active || w->depth != 0u) {
        return TXML_ERR_INVALID;
    }
    return TXML_OK;
}

TXML_API void txml_writer_destroy(txml_writer *w) {
    if (w == NULL) {
        return;
    }
    const txml_allocator allocator = w->allocator;
    txml_arena_destroy(&w->arena);
    txml_allocator_free_params params = { .ptr = w, .size = sizeof(txml_writer), .alignment = _Alignof(txml_writer) };
    allocator.v->free(allocator.self, &params);
}
