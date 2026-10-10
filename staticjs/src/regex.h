/*
 * Regular expressions (docs/regex.md): what the parser, the compiler, the
 * evaluator, the built-ins, the writer and the tests share. Private.
 *
 * A literal is classified when it is parsed (sjs_regex_check): matched
 * exactly, refused as unsupported (JavaScript staticjs leaves out), or
 * refused as a syntax error (what JavaScript refuses too). An exact one is
 * compiled, when the program compiles, to a program for a Pike VM
 * (sjs_regex_compile), the immutable SJS_REGEXP value its literal stands for.
 */
#pragma once

#include "internal.h"

/* Flags: an SJS_EV_REGEXP's count, and a regular expression's. In the
 * order RegExp.prototype.flags writes them, "dgimsuvy". */
#define SJS_REGEX_HAS_INDICES 1u   /* d */
#define SJS_REGEX_GLOBAL 2u        /* g */
#define SJS_REGEX_IGNORE_CASE 4u   /* i */
#define SJS_REGEX_MULTILINE 8u     /* m */
#define SJS_REGEX_DOT_ALL 16u      /* s */
#define SJS_REGEX_UNICODE 32u      /* u */
#define SJS_REGEX_UNICODE_SETS 64u /* v */
#define SJS_REGEX_STICKY 128u      /* y */

/* The flags of `text` (an SJS_REGEX_* set): false for a letter that is not
 * a flag, or one written twice, or u with v, which JavaScript refuses. */
bool sjs_regex_flags(sjs_str text, uint32_t *flags);

/* What becomes of a literal. */
typedef enum {
    SJS_REGEX_EXACT = 0,      /* compiled, and matched as JavaScript matches it */
    SJS_REGEX_UNSUPPORTED,    /* JavaScript, which staticjs leaves out */
    SJS_REGEX_SYNTAX,         /* not JavaScript */
    SJS_REGEX_LIMIT           /* groups nest deeper than max_nesting */
} sjs_regex_outcome;

typedef struct {
    sjs_str body;             /* between the slashes, as written: UTF-8 */
    uint32_t flags;           /* SJS_REGEX_*, valid ones */
} sjs_regex_literal;

typedef struct {
    uint8_t outcome;          /* sjs_regex_outcome */
    const char *message;      /* for anything but SJS_REGEX_EXACT */
} sjs_regex_verdict;

/* Classify a literal: SJS_OK with the verdict, or SJS_ERR_NOMEM. Its
 * scratch comes from `allocator` and is freed before it returns. */
typedef struct {
    const sjs_allocator *allocator;
    sjs_limits limits;
} sjs_regex_env;
sjs_result sjs_regex_check(sjs_regex_env env, sjs_regex_literal literal, sjs_regex_verdict *out);

/* Compile an exact literal into an SJS_REGEXP value: SJS_ERR_LIMIT when its
 * automaton would hold more than max_regex_states, SJS_ERR_NOMEM, or
 * SJS_ERR_PROGRAM for a literal sjs_regex_check does not call exact. */
sjs_result sjs_regex_compile(sjs_context *context, sjs_regex_literal literal, sjs_value *out);

/* A regular expression's source (its body as written) and flags. */
sjs_str sjs_regex_source(const sjs_heap *regexp);
uint32_t sjs_regex_flag_set(const sjs_heap *regexp);
/* Its flags as RegExp.prototype.flags writes them, into `out` (room for 8):
 * their count. */
uint32_t sjs_regex_flag_text(uint32_t flags, char *out);
