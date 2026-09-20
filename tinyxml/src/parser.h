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
#pragma once

#include "doc.h"
#include "text.h"

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
    DP_SPACE,      /* the whitespace after "<!DOCTYPE" */
    DP_SPACE_SKIP,
    DP_NAME,
    DP_REST,       /* external identifiers, up to '[' or '>' */
    DP_SUBSET,     /* between declarations */
    DP_PARAMETER,  /* %name; */
    DP_LT,         /* after '<' */
    DP_BANG,       /* after "<!" */
    DP_DASH,       /* after "<!-" */
    DP_KEYWORD,    /* the letters of ENTITY, ATTLIST, ... */
    DP_COMMENT,
    DP_PI,
    DP_SKIP,       /* a declaration that is read over: ELEMENT, NOTATION */
    DP_COLLECT,    /* a declaration that is kept: ENTITY, ATTLIST */
    DP_AFTER       /* after ']', up to '>' */
} doctype_phase;

typedef struct {
    doctype_phase phase;
    uint8_t quote;
    uint32_t pending; /* '-' or '?' held back while looking for a terminator */
    char keyword[12];
    uint32_t keyword_length;
    bool entity_declaration;
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
uint32_t txp_limit(const txml_parser *p);
txml_position txp_position_at(const txml_parser *p, uint32_t index);
/* The position of window[start]. */
txml_position txp_position_now(const txml_parser *p);
/* Consume up to `index`, keeping line, column and offset current. */
void txp_commit(txml_parser *p, uint32_t index);

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

txml_position txp_cursor_position(const txml_parser *p, position_cursor *cursor, uint32_t index);

scan txp_fail(txml_parser *p, const problem *what, txml_position at);
scan txp_fail_result(txml_parser *p, txml_result result, txml_position at);
scan txp_check_budget(txml_parser *p, txml_position at);
bool txp_budget_spent(const txml_parser *p);
uint32_t txp_parent_node(const txml_parser *p);

typedef struct {
    txml_node_kind kind;
    uint32_t flags;
    uint32_t name;
    txml_position position;
} leaf;

/* Append a node whose text is `p->run`. Finishes the run first; safe to retry. */
scan txp_append_leaf(txml_parser *p, const leaf *what);

snapshot txp_snapshot(txml_parser *p);
/* Undo appended nodes, attributes and prefix exceptions. */
void txp_rollback_nodes(txml_parser *p, const snapshot *s);

/* The end of input inside a construct: UNEXPECTED_EOF at the current position. */
scan txp_eof_in_markup(txml_parser *p);

typedef struct {
    txml_intern_params params;
    txml_position at;
} intern_request;

/* Intern a name; a full names table is a TXML_DOC_LIMIT at `request->at`. */
scan txp_intern(txml_parser *p, const intern_request *request, uint32_t *id);

/* The character at window[index] cannot be part of a name where one must be
 * (or continue): TXML_DOC_NAME at that character. Only ever raised for
 * non-ASCII characters, since no non-ASCII character delimits markup. */
scan txp_fail_name_char(txml_parser *p, uint32_t index);

uint32_t txp_skip_space(const uint8_t *bytes, uint32_t from, uint32_t limit);
uint32_t txp_scan_name(const uint8_t *bytes, uint32_t from, uint32_t limit);

bool txp_is_space(uint8_t byte);
/* Whether the whole character at `bytes` starts, or continues, a Name; `width`
 * is its length. `bytes` must be at a character boundary below `valid`. */
bool txp_name_start_at(const uint8_t *bytes, uint32_t *width);
bool txp_name_char_at(const uint8_t *bytes, uint32_t *width);
uint32_t txp_literal_length(const char *literal);
bool txp_bytes_are(const uint8_t *bytes, uint32_t length, const char *literal);

/* ------------------------------------------------------------------------
 * decode.c
 */

extern const sink txp_sink_run;
extern const sink txp_sink_buffer;

scan txp_buffer_append(txml_parser *p, const uint8_t *bytes, uint32_t length);
txml_position txp_small_position_at(const txml_parser *p, uint32_t index);

void txp_dec_reset(txml_parser *p, decode_mode mode);

typedef struct {
    const uint8_t *bytes;
    uint32_t length;
} dec_input;

/* Decode `input` into the current sink. `consumed` is how many bytes were
 * fully handled, which on SCAN_NOMEM is where to resume. */
scan txp_dec_feed(txml_parser *p, const dec_input *input, uint32_t *consumed);
/* Finish a pending entity expansion. */
scan txp_dec_resume(txml_parser *p);
/* A construct is ending: an unfinished reference is malformed. */
scan txp_dec_end(txml_parser *p);

/* Read the character references of an entity literal, leaving &name;
 * references alone (XML 1.0 §4.5). The result is allocated in the parser arena. */
scan txp_expand_character_references(txml_parser *p, txml_str literal, txml_str *expanded);

/* ------------------------------------------------------------------------
 * tables.c: the DTD tables
 */

const entity *txp_find_entity(const txml_parser *p, txml_str name);
const element_defaults *txp_find_element_defaults(const txml_parser *p, txml_str element);
/* Record a declaration; the first for a name (or element and attribute) binds. */
scan txp_add_entity(txml_parser *p, txml_str name, txml_str value);

typedef struct {
    txml_str element;
    txml_str name;
    txml_str value;
} default_request;

scan txp_add_default(txml_parser *p, const default_request *request);

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
scan txp_begin_doctype(txml_parser *p);

/* ------------------------------------------------------------------------
 * tag.c
 */

extern const state txp_state_start_tag;
extern const state txp_state_end_tag;

/* window[start] is the first byte of the element name. */
scan txp_begin_start_tag(txml_parser *p);
/* The byte after "</" is next. */
void txp_begin_end_tag(txml_parser *p);

/* ------------------------------------------------------------------------
 * doctype.c
 */

extern const state txp_state_doctype;
