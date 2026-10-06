/*
 * The matcher: a bottom-up tree automaton (iburg-style dynamic programming)
 * over the parser's events, driven by the rules of src/shapes.rules
 * (docs/matcher.md). It sees each event before the compiler does and tells
 * the compiler which functions compute a known shape. It never changes an
 * event and never changes what the compiler emits: a match only tags.
 *
 * MUTATION: it also reports rewrites (docs/matcher.md, section 12): nodes
 * the compiler compiles differently where a rewrite's rules match, which an
 * anchored lane matches beside the labeller, with shapes on and off.
 *
 *   parser --event--> sjs_matcher_event --> the compiler's own handling
 *                          |
 *                          +--> report(shape, node, start, arguments)
 *
 * The compiler calls sjs_matcher_event first in its sink, so it is a tee
 * between the parser and the compiler. A stream the matcher cannot read as
 * the tree of docs/matcher.md, section 2 (one no parser sends, such as a
 * hand-built one with node 0 on every event) stops the matching, not the
 * compile.
 */
#pragma once

#include "internal.h"

/* One argument of a match, by sjs_shape_arg_kind(). */
typedef struct {
    uint8_t kind;          /* SJS_SHAPE_ARG_* */
    uint8_t operand;       /* an operand's SJS_OPERAND_*: then a number's value, or a string's text */
    uint32_t count;        /* a flag, a count or an sjs_operator */
    double number;
    sjs_str text;          /* valid during the report only */
} sjs_shape_value;

/* A node that derives a shape: what, which node, from where, with what. */
typedef struct {
    sjs_shape shape;
    uint32_t node;         /* the event's node: for now always an arrow function */
    sjs_position start;    /* where the node's source begins */
    sjs_shape_value args[SJS_SHAPE_ARGS];
} sjs_shape_match;

/* MUTATION: a node a rewrite's rules match: which rewrite, which node, and
 * where the node's source begins (docs/matcher.md, 12.5). */
typedef struct {
    sjs_rewrite rewrite;
    uint32_t node;
    sjs_position start;
} sjs_rewrite_match;

typedef struct sjs_shape_reporter_vtable {
    /* SJS_OK to go on; any other result is sjs_matcher_event's. */
    sjs_result (*report)(void *self, const sjs_shape_match *match);
    /* MUTATION: a rewrite, during the event that ends its node; NULL to
     * ignore them. */
    sjs_result (*rewrite)(void *self, const sjs_rewrite_match *match);
} sjs_shape_reporter_vtable;

typedef struct { void *self; const sjs_shape_reporter_vtable *v; } sjs_shape_reporter;

typedef struct {
    const sjs_allocator *allocator;   /* the context's counting allocator */
    uint32_t max_nesting;             /* sizes its bound: 4 * max_nesting + 4 records */
    sjs_shape_reporter reporter;
    /* MUTATION: match the rewrites alone, not the shapes: what no_shapes
     * leaves on (docs/matcher.md, 12.6). */
    bool rewrites_only;
} sjs_matcher_config;

/* The program, the bracket around everything, which no rule matches: it
 * has no record. */
#define SJS_MATCHER_PROGRAM UINT32_MAX

/* Its state, held by its owner (the compiler holds one in itself). It
 * allocates nothing until the first event, and its text nothing until it
 * outgrows the bytes it has in itself. */
typedef struct {
    const sjs_allocator *allocator;
    sjs_shape_reporter reporter;
    uint32_t bound;        /* records at most; past it, the matcher stops */
    uint32_t top;          /* the innermost marker's record, or SJS_MATCHER_PROGRAM */
    uint32_t top_node;     /* its node */
    bool top_dead;         /* it can match no more: what is inside it is passed over */
    uint32_t peak;         /* the most records held at once */
    bool stopped;
    sjs_buf records;       /* cells and markers, of a fixed stride (matcher.c) */
    sjs_buf text;          /* text the records captured, in record order: in
                            * text_inline until it outgrows it */
    uint8_t text_inline[64];
    /* MUTATION: the anchored lane (docs/matcher.md, 12.4): its cells, in
     * lane_inline until they outgrow it; the most held at once; stopped by
     * a stream it cannot read. The labeller does not run when rewrites_only. */
    bool rewrites_only;
    bool lane_stopped;
    uint32_t lane_peak;
    sjs_buf lane;
    uint64_t lane_inline[16];
} sjs_matcher;

/* The matcher points into itself, so it is initialised where it stays. */
void sjs_matcher_init(sjs_matcher *matcher, const sjs_matcher_config *config);
/* Match one event, before its sink sees it. SJS_OK; SJS_ERR_NOMEM; or the
 * reporter's result, when it is not SJS_OK. An unreadable stream is SJS_OK:
 * the matcher stops and lets every later event pass. */
sjs_result sjs_matcher_event(sjs_matcher *matcher, const sjs_event *event);
void sjs_matcher_free(sjs_matcher *matcher);
