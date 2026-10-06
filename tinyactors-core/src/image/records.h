/* records.h — on-disk image format (.tai) and the validated in-memory view.
 *
 * Contract between ta_compile (writer) and ta_core/ta_system (readers).
 * All integers little-endian; all references are u32 offsets or indices.
 *
 * Index spaces:
 *   - per-chart records (states, transitions, expressions, sends, invokes, data)
 *     are referenced with chart-LOCAL indices (0 = chart's first record);
 *   - code pcs, u32-pool offsets, capture indices, string ids and event-name
 *     ids are GLOBAL.
 * States are numbered in document order (preorder); descendants of s are the
 * interval (s, s.end). State 0 of every chart is the <scxml> root.
 */
#ifndef TINYACTORS_IMAGE_RECORDS_H
#define TINYACTORS_IMAGE_RECORDS_H

#include "base/config.h"
#include <tinyactors/image.h>

#define TA_IMAGE_MAGIC   0x31494154u /* "TAI1" */
#define TA_IMAGE_VERSION 1u

enum {
    TA_SECTION_STRINGS = 1,
    TA_SECTION_CHARTS,
    TA_SECTION_STATES,
    TA_SECTION_TRANSITIONS,
    TA_SECTION_U32,
    TA_SECTION_CODE,
    TA_SECTION_EXPRESSIONS,
    TA_SECTION_CAPTURES,
    TA_SECTION_SENDS,
    TA_SECTION_INVOKES,
    TA_SECTION_DATA,
    TA_SECTION_EVENT_NODES,
    TA_SECTION_ACTORS,
    TA_SECTION_MAX_
};

typedef struct ta_header_record {
    uint32_t magic, version, total_size, section_count;
    uint64_t hash; /* FNV-1a 64 of the image with this field zeroed */
    uint32_t flags, chart_count;
    uint32_t reserved[8];
} ta_header_record; /* 64 bytes; followed by section_count ta_section_record */

typedef struct ta_section_record {
    uint32_t kind, offset, size, count;
} ta_section_record;

/* Strings: a string id is the offset of {u32 length; char bytes[length]; '\0'},
 * 4-aligned, inside the STRINGS section. */

typedef struct ta_chart_record {
    uint32_t name, datamodel, binding, flags;
    uint32_t first_state, state_count;
    uint32_t first_transition, transition_count;
    uint32_t first_expression, expression_count;
    uint32_t first_send, send_count;
    uint32_t first_invoke, invoke_count;
    uint32_t first_data, data_count;
    uint32_t first_code, code_count;
    uint32_t script;         /* instruction of top-level <script> or TA_NIL */
    uint32_t eventless_mask; /* u32-pool offset of an state_count-bit mask */
    uint32_t max_depth, max_foreach;
    uint32_t history_word_count;
    uint32_t source; /* string id: source name used for invoke src matching */
} ta_chart_record;

/* ta_state_record.kind: TA_STATE_* of <tinyactors/image.h> */
enum {
    TA_STATE_FLAG_ROOT = 1u << 0,
    TA_STATE_FLAG_HAS_EVENTLESS_TRANSITIONS = 1u << 1, /* has an eventless transition */
    TA_STATE_FLAG_HAS_EVENT_TRANSITIONS = 1u << 2      /* has an event transition */
};

typedef struct ta_state_record {
    uint32_t parent; /* TA_NIL for the root */
    uint32_t end;    /* one past the last descendant */
    uint8_t kind, depth;
    uint16_t flags;
    uint32_t id; /* string id */
    uint32_t first_transition, transition_count;
    uint32_t initial; /* compound: initial transition; history: default transition; else NIL */
    uint32_t onentry, onexit; /* code pcs or TA_NIL */
    uint32_t first_invoke, invoke_count;
    uint32_t first_data, data_count;
    uint32_t donedata;       /* capture index (final states) or TA_NIL */
    uint32_t done_event;     /* name id of done.state.<id> (compound/parallel) or TA_NIL */
    uint32_t history_offset; /* history states: word offset in the instance history area */
} ta_state_record;

enum {
    TA_TRANSITION_FLAG_INTERNAL = 1u << 0,
    TA_TRANSITION_FLAG_HISTORY_TARGET = 1u
        << 1,                            /* targets a history state: domain computed at runtime */
    TA_TRANSITION_FLAG_INITIAL = 1u << 2 /* initial/history-default pseudo transition */
};

typedef struct ta_transition_record {
    uint32_t source;
    uint32_t domain;        /* precomputed transition domain; TA_NIL if targetless or dynamic */
    uint32_t condition;     /* expr or TA_NIL */
    uint32_t events_offset; /* u32 pool: event_count pairs {lo, hi} of event-name intervals */
    uint16_t event_count;   /* 0 = eventless */
    uint16_t flags;
    uint32_t targets_offset;
    uint16_t target_count;
    uint16_t pad;
    uint32_t action; /* code instruction or TA_NIL */
} ta_transition_record;

typedef struct ta_expression_record {
    uint32_t kind;   /* TA_EXPRESSION_* */
    uint32_t source; /* string id */
    uint32_t line, column;
} ta_expression_record;

typedef struct ta_capture_record {
    uint32_t namelist_offset, namelist_count; /* u32 pool: location expressions */
    uint32_t params_offset, param_count;      /* u32 pool: pairs {name string id, expr} */
    uint32_t content;                         /* expr or TA_NIL */
} ta_capture_record;

enum {
    TA_SEND_TARGET_NONE = 0,      /* own external queue */
    TA_SEND_TARGET_INTERNAL = 1,  /* "#_internal" */
    TA_SEND_TARGET_PARENT = 2,    /* "#_parent" */
    TA_SEND_TARGET_INVOKE = 3,    /* "#_<static invokeid>": target = invoke slot */
    TA_SEND_TARGET_STRING = 4,    /* other static string: target = string id */
    TA_SEND_TARGET_EXPRESSION = 5 /* targetexpr */
};

typedef struct ta_send_record {
    uint32_t event;     /* name id or TA_NIL */
    uint32_t eventexpr; /* expr or TA_NIL */
    uint32_t target_kind, target, targetexpr;
    uint32_t type; /* string id, or TA_NIL = SCXML */
    uint32_t typeexpr;
    uint32_t id; /* string id or TA_NIL */
    uint32_t idlocation;
    uint32_t delayexpr;
    uint32_t capture; /* capture index or TA_NIL */
    uint32_t pad;
    uint64_t delay_ns;
} ta_send_record;

typedef struct ta_invoke_record {
    uint32_t state;
    uint32_t type; /* string id or TA_NIL = SCXML */
    uint32_t typeexpr;
    uint32_t source_chart; /* statically linked child chart or TA_NIL */
    uint32_t source;       /* string id or TA_NIL */
    uint32_t srcexpr;
    uint32_t id; /* string id or TA_NIL */
    uint32_t idlocation;
    uint32_t capture;
    uint32_t autoforward;
    uint32_t finalize; /* code instruction or TA_NIL */
    uint32_t docexpr;  /* <content expr> yielding the child document text, or TA_NIL */
} ta_invoke_record;

typedef struct ta_data_record {
    uint32_t id; /* string id */
    uint32_t expression;
    uint32_t state;
} ta_data_record;

typedef struct ta_event_node_record {
    uint32_t end;    /* preorder interval end */
    uint32_t parent; /* TA_NIL for root */
    uint32_t token;  /* string id of the last token */
    uint32_t name;   /* string id of the full dotted name */
} ta_event_node_record;

typedef struct ta_actor_record {
    uint32_t name, chart;
} ta_actor_record;

/* ---- executable-content bytecode (u32 words; word 0 = opcode) ---------- */
enum {
    TA_OPCODE_END = 0,    /* end of content */
    TA_OPCODE_BLOCK = 1,  /* end_pc: an executable-content block (error skips to end_pc) */
    TA_OPCODE_RAISE = 2,  /* name_id */
    TA_OPCODE_SEND = 3,   /* send index */
    TA_OPCODE_CANCEL = 4, /* sendid string id */
    TA_OPCODE_CANCEL_EXPRESSION = 5, /* sendid expr */
    TA_OPCODE_ASSIGN = 6,            /* location expr, value expr */
    TA_OPCODE_SCRIPT = 7,            /* expr */
    TA_OPCODE_LOG = 8,               /* label string id | NIL, expr | NIL */
    TA_OPCODE_JUMP_IF_FALSE = 9,     /* cond expr, target instruction */
    TA_OPCODE_JUMP = 10,             /* target instruction */
    TA_OPCODE_FOREACH_BEGIN = 11,    /* array expr, item loc, index loc | NIL, end instruction */
    TA_OPCODE_FOREACH_NEXT = 12,     /* body instruction */
    TA_OPCODE_CUSTOM = 13,           /* custom index */
    TA_OPCODE_MAX_
};
static const uint8_t ta_opcode_length[TA_OPCODE_MAX_] = {1, 2, 2, 2, 2, 2, 3, 2, 3, 3, 2, 5, 2, 2};

/* Deepest <if>/<foreach> nesting the compiler emits. The verifier checks the
 * jumps of code nested this deep in one linear pass with bounded memory
 * (deeper code from elsewhere still verifies, just more slowly). */
#define TA_IMAGE_MAX_NESTING 64u

/* ---- validated host-side view (lives inside public ta_image storage) ---- */
typedef struct ta_image_view {
    const unsigned char *base;
    const unsigned char *strings;
    const ta_chart_record *charts;
    const ta_state_record *states;
    const ta_transition_record *transitions;
    const uint32_t *u32;
    const uint32_t *code;
    const ta_expression_record *expressions;
    const ta_capture_record *captures;
    const ta_send_record *sends;
    const ta_invoke_record *invokes;
    const ta_data_record *data;
    const ta_event_node_record *event_nodes;
    const ta_actor_record *actors;
    uint64_t hash;
    uint32_t size, strings_size, chart_count, state_count, transition_count, u32_count, code_count,
        expression_count, capture_count, send_count, invoke_count, data_count, event_node_count,
        actor_count;
} ta_image_view;

TA_INLINE const ta_image_view *ta_image_view_of(const ta_image *image) {
    return (const ta_image_view *)(const void *)image;
}

TA_INLINE ta_string_view ta_image_view_string(const ta_image_view *view, uint32_t sid) {
    ta_string_view s = {(const char *)0, 0};
    if (sid == TA_NIL) return s;
    s.length = *(const uint32_t *)(const void *)(view->strings + sid);
    s.pointer = (const char *)(view->strings + sid + 4);
    return s;
}

/* Chart-relative accessors. */
TA_INLINE const ta_state_record *ta_chart_states(const ta_image_view *view,
                                                 const ta_chart_record *chart) {
    return view->states + chart->first_state;
}
TA_INLINE const ta_transition_record *ta_chart_transitions(const ta_image_view *view,
                                                           const ta_chart_record *chart) {
    return view->transitions + chart->first_transition;
}

/* Event-name lookup: deepest trie node whose dotted name is a token prefix of
 * `name`. *exact is set when the node's name equals `name`. */
uint32_t ta_event_name_lookup(const ta_image_view *v, const char *name, uint32_t length,
                              int *exact);

/* FNV-1a 64 with the header hash field treated as zero (shared with writer). */
uint64_t ta_image_hash(const unsigned char *bytes, uint32_t length);

#endif /* TINYACTORS_IMAGE_RECORDS_H */
