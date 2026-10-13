/*
 * A compiled regular expression: the program of its automaton (docs/regex.md,
 * section 4), shared by regex.c, which writes it, and regex_match.c, which
 * runs it. Private.
 */
#pragma once

#include "regex.h"

/* A quantifier without a bound. */
#define RX_INFINITE UINT32_MAX
/* A capture slot not set. */
#define RX_UNSET UINT32_MAX

/* Instructions. Program counters are indices. */
enum {
    RX_CHAR = 0,     /* x: consume one character equal to x */
    RX_CHAR_FOLD,    /* x: consume one character whose case fold is x (i) */
    RX_ANY,          /* consume any character but a line terminator */
    RX_ANY_ALL,      /* consume any character (s) */
    RX_CLASS,        /* x: consume one character in class x */
    RX_MATCH,        /* the thread matched */
    /* The rest consume nothing: the epsilon closure follows them. */
    RX_SPLIT,        /* x, y: a thread at x first, then one at y */
    RX_JMP,          /* x: go to x */
    RX_SAVE,         /* x: the position into slot x */
    RX_CLEAR,        /* x, y: slots x to y unset (a quantifier's body, each iteration) */
    RX_MARK,         /* x: the position into register slot x */
    RX_CHECK,        /* x: kill the thread if the position equals register slot x */
    RX_ASSERT        /* x: RX_ASSERT_* holds here */
};

enum {
    RX_ASSERT_START = 0,   /* ^ */
    RX_ASSERT_LINE_START,  /* ^ with m */
    RX_ASSERT_END,         /* $ */
    RX_ASSERT_LINE_END,    /* $ with m */
    RX_ASSERT_WORD,        /* \b */
    RX_ASSERT_NOT_WORD     /* \B */
};

typedef struct {
    uint8_t op;
    /* How many loop registers bear on a thread here: those of the loops
     * that can match empty and whose optional iteration holds this pc
     * (docs/regex.md, 5.3). Registers 0 to relevant - 1. */
    uint8_t relevant;
    uint16_t unused;
    uint32_t x;
    uint32_t y;
} rx_ins;

/*
 * How a thread from the start (pc 0) begins: `lead_saves` SAVEs, then the
 * instruction at pc lead_saves. Only the start reaches those instructions
 * (every jump back goes to a loop's SPLIT), so where it cannot go on, its
 * closure always visits the same instructions, which a search counts
 * without following them (regex_match.c, "Skipping").
 */
enum {
    RX_LEAD_NONE = 0,
    RX_LEAD_CHAR,      /* CHAR of an ASCII character: lead_char */
    RX_LEAD_FOLD,      /* CHAR_FOLD of an ASCII letter: lead_char, lowercase */
    RX_LEAD_START      /* ASSERT ^ without m */
};

/* A class: its ranges, sorted and disjoint, from `start` in the ranges. */
typedef struct {
    uint32_t start;
    uint32_t count;
} rx_class;

/* A group's name, in the regexp's text after its source: empty for none. */
typedef struct {
    uint32_t offset;
    uint32_t length;
} rx_group_name;

/*
 * The value (SJS_HEAP_REGEXP): one block, immutable, holding no
 * references. After the header: the instructions, the classes, their
 * ranges (low, high pairs), each group's name record, for a program the
 * cache of a test takes (regex_match.c, "The cache") the classes of each
 * ASCII character (rx_dfa_map), then the source and the names' text.
 */
typedef struct {
    sjs_heap h;
    uint32_t flags;          /* SJS_REGEX_* */
    uint32_t groups;         /* capture groups, the whole match not counted */
    uint32_t registers;      /* d: loop registers, the deepest nesting of empty-capable loops */
    uint32_t slots;          /* per thread: 2 per group, the whole match's 2, and the registers */
    uint32_t threads;        /* the most threads a list holds: count << registers */
    uint32_t count;          /* instructions */
    uint32_t class_count;
    uint32_t range_count;
    uint32_t clear_slots;    /* the slots of every CLEAR together, for the closure's saves */
    uint32_t source_length;
    uint32_t text_length;    /* the source and the names */
    bool named;              /* a group has a name */
    uint8_t lead;            /* RX_LEAD_* */
    uint8_t lead_char;
    uint8_t dfa_classes;     /* the cache's classes (sjs_regex_dfa_classes), 0 when it refuses the program */
    uint8_t dfa_next;        /* its coarser classes, of the character after an edge's */
    uint32_t lead_saves;
    const rx_ins *code;
    const rx_class *classes;
    const uint32_t *ranges;
    const rx_group_name *names;
    const uint16_t *dfa_map; /* rx_dfa_map, for a program with dfa_classes */
    const char *text;        /* the source, then the names */
} sjs_regexp;

#define SJS_AS_REGEXP(v) ((const sjs_regexp *)(const void *)(v).as.heap)

/* The classes of each ASCII character, for a program with dfa_classes, as
 * the cache's rows read them: its class times the width of a row
 * (dfa_next + 1), shifted left by RX_DFA_NEXT_BITS, and its coarser class
 * (dfa_next) in those bits. */
static inline const uint16_t *rx_dfa_map(const sjs_regexp *re) {
    return re->dfa_map;
}

#define RX_DFA_NEXT_BITS 4u
/* The entries of rx_dfa_map. */
#define RX_DFA_ASCII 128u

/* Sort the ASCII characters into the classes of the cache of a test
 * (regex_match.c) for the program `re`, whose code, classes, ranges and
 * lead are set: its dfa_classes, 0 when the cache refuses the program, and
 * dfa_next, with each character's classes in `map` (RX_DFA_ASCII entries,
 * as rx_dfa_map holds them). */
void sjs_regex_dfa_classes(sjs_regexp *re, uint16_t *map);
