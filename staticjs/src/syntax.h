/*
 * The lexer and the parser's token: private to lexer.c and parser.c.
 */
#pragma once

#include "internal.h"

typedef enum {
    SJS_TOKEN_END = 0,
    SJS_TOKEN_NAME,           /* an identifier or a keyword; `keyword` says which */
    SJS_TOKEN_NUMBER,
    SJS_TOKEN_STRING,
    SJS_TOKEN_TEMPLATE,       /* `...` with no substitution */
    SJS_TOKEN_TEMPLATE_HEAD,  /* `...${ */
    SJS_TOKEN_TEMPLATE_MIDDLE,/* }...${ */
    SJS_TOKEN_TEMPLATE_TAIL,  /* }...` */
    SJS_TOKEN_PUNCTUATOR,
    SJS_TOKEN_REGEXP          /* a regular expression literal: its text is the body, a `/`, the flags */
} sjs_token_kind;

typedef enum {
    SJS_P_NONE = 0,
    SJS_P_LBRACE, SJS_P_RBRACE, SJS_P_LPAREN, SJS_P_RPAREN, SJS_P_LBRACKET, SJS_P_RBRACKET,
    SJS_P_DOT, SJS_P_ELLIPSIS, SJS_P_SEMICOLON, SJS_P_COMMA,
    SJS_P_LESS, SJS_P_GREATER, SJS_P_LESS_EQUAL, SJS_P_GREATER_EQUAL,
    SJS_P_EQUAL, SJS_P_NOT_EQUAL, SJS_P_STRICT_EQUAL, SJS_P_STRICT_NOT_EQUAL,
    SJS_P_PLUS, SJS_P_MINUS, SJS_P_STAR, SJS_P_SLASH, SJS_P_PERCENT, SJS_P_STAR_STAR,
    SJS_P_PLUS_PLUS, SJS_P_MINUS_MINUS,
    SJS_P_SHIFT_LEFT, SJS_P_SHIFT_RIGHT, SJS_P_SHIFT_RIGHT_UNSIGNED,
    SJS_P_AMPERSAND, SJS_P_PIPE, SJS_P_CARET, SJS_P_BANG, SJS_P_TILDE,
    SJS_P_AND, SJS_P_OR, SJS_P_COALESCE,
    SJS_P_QUESTION, SJS_P_OPTIONAL, SJS_P_COLON,
    SJS_P_ASSIGN, SJS_P_ASSIGN_OPERATOR, SJS_P_ARROW,
    SJS_P_HASH, SJS_P_AT
} sjs_punctuator;

typedef enum {
    SJS_K_NONE = 0,
    /* Keywords staticjs reads. */
    SJS_K_CONST, SJS_K_RETURN, SJS_K_IMPORT, SJS_K_EXPORT, SJS_K_NEW, SJS_K_TYPEOF, SJS_K_IN,
    SJS_K_TRUE, SJS_K_FALSE, SJS_K_NULL,
    /* Reserved words it does not. */
    SJS_K_RESERVED,
    /* Names JavaScript gives a meaning staticjs does not follow. */
    SJS_K_UNSUPPORTED_NAME
} sjs_keyword;

typedef struct {
    uint8_t kind;          /* sjs_token_kind */
    uint8_t punctuator;    /* sjs_punctuator */
    uint8_t keyword;       /* sjs_keyword */
    uint8_t newline_before;
    uint8_t lone_surrogate; /* a string with a surrogate that has no partner; its text is not the string */
    uint8_t escaped;       /* a string with an escape: not a directive such as "use strict" */
    uint8_t bigint;        /* a NUMBER that is a BigInt literal: its radix; its text its digits */
    sjs_position at;
    double number;
    sjs_str text;          /* the name, or the cooked string */
} sjs_token;

/* Called with each token; the text is valid during the call only. */
typedef sjs_result (*sjs_token_handler)(void *self, const sjs_token *token);

typedef struct {
    sjs_allocator allocator;
    sjs_limits limits;
    sjs_token_handler handler;
    void *handler_self;
    sjs_error_slot *error;
    /* Where the next byte is. */
    sjs_position here;
    bool previous_cr;
    /* UTF-8 checking across chunks. */
    uint32_t utf8_need;
    uint32_t utf8_point;
    uint32_t utf8_minimum;
    uint32_t utf8_seen;
    /* The token being read. */
    uint8_t state;
    uint8_t substate;
    uint8_t quote;
    bool newline_before;
    bool failed;
    sjs_position start;
    sjs_buf text;          /* the token's text so far */
    char punctuator[4];
    uint32_t punctuator_length;
    uint32_t escape_value;
    uint32_t escape_digits;
    uint32_t pending_high;  /* a high surrogate from \u, waiting for its pair */
    bool lone_surrogate;    /* the string holds a surrogate with no partner */
    bool escaped;           /* the string holds an escape */
    uint32_t radix;
    int32_t exponent;
    int32_t exponent_sign;
    uint32_t exponent_digits;
    bool seen_digit;
    bool last_separator;
    /* Braces: 0 for `{`, 1 for the `${` of a template. */
    sjs_buf braces;
    /* Set by the parser while it handles a `/` (1) or `/=` (2) token that
     * begins a regular expression: the lexer then reads the literal's body
     * and flags, the `=` of `/=` first, as one SJS_TOKEN_REGEXP. */
    uint8_t regex_next;
    bool regex_class;       /* in the body: inside [...] */
} sjs_lexer;

sjs_result sjs_lexer_init(sjs_lexer *lexer);
sjs_result sjs_lexer_push(sjs_lexer *lexer, sjs_bytes chunk);
sjs_result sjs_lexer_finish(sjs_lexer *lexer);
void sjs_lexer_destroy(sjs_lexer *lexer);
