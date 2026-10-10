/*
 * The lexer: a state machine fed one byte at a time, so a token may span any
 * number of chunks. It hands each token to the parser as soon as the byte
 * after it arrives (or the source ends).
 */
#include "syntax.h"

enum {
    L_START = 0,
    L_NAME,
    L_NUMBER,
    L_STRING,
    L_TEMPLATE,
    L_PUNCTUATOR,
    L_LINE_COMMENT,
    L_BLOCK_COMMENT,
    L_BOM,
    L_REGEX,
    L_FAILED
};

/* Substates of a regular expression literal. */
enum { R_BODY = 0, R_ESCAPE, R_FLAGS, R_FLAG_UTF8 };

/* Substates of a number. */
enum { N_ZERO = 0, N_INTEGER, N_FRACTION, N_EXPONENT_START, N_EXPONENT_SIGN, N_EXPONENT, N_RADIX, N_LEGACY, N_BIGINT };

/* Substates of a string or template. */
enum {
    S_TEXT = 0,
    S_ESCAPE,
    S_ESCAPE_ZERO,
    S_ESCAPE_CR,
    S_HEX,
    S_UNICODE_START,
    S_UNICODE,
    S_UNICODE_BRACED,
    S_DOLLAR,
    S_CR,
    S_AFTER_HIGH
};

/* What a byte did. */
typedef enum { TAKEN = 0, AGAIN, STOP } step;

typedef struct {
    const char *text;
    uint8_t punctuator;
} punctuator_entry;

static const punctuator_entry PUNCTUATORS[] = {
    { "{", SJS_P_LBRACE }, { "}", SJS_P_RBRACE }, { "(", SJS_P_LPAREN }, { ")", SJS_P_RPAREN },
    { "[", SJS_P_LBRACKET }, { "]", SJS_P_RBRACKET }, { ".", SJS_P_DOT }, { "...", SJS_P_ELLIPSIS },
    { ";", SJS_P_SEMICOLON }, { ",", SJS_P_COMMA }, { "<", SJS_P_LESS }, { ">", SJS_P_GREATER },
    { "<=", SJS_P_LESS_EQUAL }, { ">=", SJS_P_GREATER_EQUAL }, { "==", SJS_P_EQUAL }, { "!=", SJS_P_NOT_EQUAL },
    { "===", SJS_P_STRICT_EQUAL }, { "!==", SJS_P_STRICT_NOT_EQUAL }, { "+", SJS_P_PLUS }, { "-", SJS_P_MINUS },
    { "*", SJS_P_STAR }, { "/", SJS_P_SLASH }, { "%", SJS_P_PERCENT }, { "**", SJS_P_STAR_STAR },
    { "++", SJS_P_PLUS_PLUS }, { "--", SJS_P_MINUS_MINUS }, { "<<", SJS_P_SHIFT_LEFT }, { ">>", SJS_P_SHIFT_RIGHT },
    { ">>>", SJS_P_SHIFT_RIGHT_UNSIGNED }, { "&", SJS_P_AMPERSAND }, { "|", SJS_P_PIPE }, { "^", SJS_P_CARET },
    { "!", SJS_P_BANG }, { "~", SJS_P_TILDE }, { "&&", SJS_P_AND }, { "||", SJS_P_OR }, { "??", SJS_P_COALESCE },
    { "?", SJS_P_QUESTION }, { "?.", SJS_P_OPTIONAL }, { ":", SJS_P_COLON }, { "=", SJS_P_ASSIGN },
    { "+=", SJS_P_ASSIGN_OPERATOR }, { "-=", SJS_P_ASSIGN_OPERATOR }, { "*=", SJS_P_ASSIGN_OPERATOR },
    { "/=", SJS_P_ASSIGN_OPERATOR }, { "%=", SJS_P_ASSIGN_OPERATOR }, { "**=", SJS_P_ASSIGN_OPERATOR },
    { "<<=", SJS_P_ASSIGN_OPERATOR }, { ">>=", SJS_P_ASSIGN_OPERATOR }, { ">>>=", SJS_P_ASSIGN_OPERATOR },
    { "&=", SJS_P_ASSIGN_OPERATOR }, { "|=", SJS_P_ASSIGN_OPERATOR }, { "^=", SJS_P_ASSIGN_OPERATOR },
    { "&&=", SJS_P_ASSIGN_OPERATOR }, { "||=", SJS_P_ASSIGN_OPERATOR }, { "?" "?=", SJS_P_ASSIGN_OPERATOR },
    { "=>", SJS_P_ARROW }, { "#", SJS_P_HASH }, { "@", SJS_P_AT }
};

#define PUNCTUATOR_COUNT (sizeof PUNCTUATORS / sizeof PUNCTUATORS[0])

typedef struct {
    const char *text;
    uint8_t keyword;
} keyword_entry;

static const keyword_entry KEYWORDS[] = {
    { "const", SJS_K_CONST }, { "return", SJS_K_RETURN }, { "import", SJS_K_IMPORT },
    { "export", SJS_K_EXPORT }, { "new", SJS_K_NEW }, { "typeof", SJS_K_TYPEOF }, { "in", SJS_K_IN },
    { "true", SJS_K_TRUE }, { "false", SJS_K_FALSE }, { "null", SJS_K_NULL },
    { "break", SJS_K_RESERVED }, { "case", SJS_K_RESERVED }, { "catch", SJS_K_RESERVED },
    { "class", SJS_K_RESERVED }, { "continue", SJS_K_RESERVED }, { "debugger", SJS_K_RESERVED },
    { "default", SJS_K_RESERVED }, { "delete", SJS_K_RESERVED }, { "do", SJS_K_RESERVED },
    { "else", SJS_K_RESERVED }, { "enum", SJS_K_RESERVED }, { "extends", SJS_K_RESERVED },
    { "finally", SJS_K_RESERVED }, { "for", SJS_K_RESERVED }, { "function", SJS_K_RESERVED },
    { "if", SJS_K_RESERVED }, { "instanceof", SJS_K_RESERVED }, { "let", SJS_K_RESERVED },
    { "static", SJS_K_RESERVED }, { "super", SJS_K_RESERVED }, { "switch", SJS_K_RESERVED },
    { "this", SJS_K_RESERVED }, { "throw", SJS_K_RESERVED }, { "try", SJS_K_RESERVED },
    { "var", SJS_K_RESERVED }, { "void", SJS_K_RESERVED }, { "while", SJS_K_RESERVED },
    { "with", SJS_K_RESERVED }, { "yield", SJS_K_RESERVED }, { "await", SJS_K_RESERVED },
    { "async", SJS_K_RESERVED }, { "implements", SJS_K_RESERVED }, { "interface", SJS_K_RESERVED },
    { "package", SJS_K_RESERVED }, { "private", SJS_K_RESERVED }, { "protected", SJS_K_RESERVED },
    { "public", SJS_K_RESERVED },
    { "arguments", SJS_K_UNSUPPORTED_NAME }, { "eval", SJS_K_UNSUPPORTED_NAME }
};

#define KEYWORD_COUNT (sizeof KEYWORDS / sizeof KEYWORDS[0])

static bool is_name_start(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static bool is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static bool is_name_part(uint8_t c) {
    return is_name_start(c) || is_digit(c);
}

static int32_t hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static step lex_fail(sjs_lexer *lexer, sjs_error_code code, const char *message) {
    sjs_error_set(lexer->error, lexer->here, (sjs_problem){ code, message });
    lexer->state = L_FAILED;
    lexer->failed = true;
    return STOP;
}

static step lex_fail_at_start(sjs_lexer *lexer, sjs_error_code code, const char *message) {
    sjs_error_set(lexer->error, lexer->start, (sjs_problem){ code, message });
    lexer->state = L_FAILED;
    lexer->failed = true;
    return STOP;
}

static step text_append(sjs_lexer *lexer, const uint8_t *bytes, uint32_t length) {
    if (lexer->text.length + length > lexer->limits.max_token_bytes) {
        return lex_fail_at_start(lexer, SJS_ERROR_LIMIT, "a token is longer than max_token_bytes");
    }
    if (sjs_buf_append(&lexer->allocator, &lexer->text, (sjs_bytes){ bytes, length }) != SJS_OK) {
        return lex_fail(lexer, SJS_ERROR_MEMORY, "out of memory");
    }
    return TAKEN;
}

static step text_append_point(sjs_lexer *lexer, uint32_t point) {
    uint8_t bytes[4];
    uint32_t length = sjs_utf8_encode(point, bytes);
    return text_append(lexer, bytes, length);
}

static step emit(sjs_lexer *lexer, sjs_token *token) {
    token->at = lexer->start;
    token->newline_before = lexer->newline_before ? 1u : 0u;
    lexer->newline_before = false;
    sjs_result r = lexer->handler(lexer->handler_self, token);
    lexer->text.length = 0;
    lexer->state = L_START;
    if (r != SJS_OK) {
        lexer->failed = true;
        lexer->state = L_FAILED;
        return STOP;
    }
    if (lexer->regex_next != 0) {
        /* The parser read the `/` just handed to it as the start of a
         * regular expression: its body comes next, from the slash on. */
        uint8_t equals = lexer->regex_next == 2u ? '=' : 0;
        lexer->regex_next = 0;
        lexer->state = L_REGEX;
        lexer->substate = R_BODY;
        lexer->regex_class = false;
        if (equals != 0) return text_append(lexer, &equals, 1);
    }
    return TAKEN;
}

static void begin(sjs_lexer *lexer, uint8_t state) {
    lexer->state = state;
    lexer->substate = 0;
    lexer->start = lexer->here;
    lexer->text.length = 0;
}

/* ------------------------------------------------------------------------
 * Punctuators: the longest that matches, read a byte at a time.
 */

static bool punctuator_prefix(const char *text, uint32_t length) {
    for (uint32_t i = 0; i < PUNCTUATOR_COUNT; i++) {
        const char *p = PUNCTUATORS[i].text;
        uint32_t k = 0;
        while (k < length && p[k] == text[k]) k++;
        if (k == length) return true;
    }
    return false;
}

static uint8_t punctuator_find(const char *text, uint32_t length) {
    for (uint32_t i = 0; i < PUNCTUATOR_COUNT; i++) {
        const char *p = PUNCTUATORS[i].text;
        uint32_t k = 0;
        while (k < length && p[k] == text[k]) k++;
        if (k == length && p[k] == '\0') return PUNCTUATORS[i].punctuator;
    }
    return SJS_P_NONE;
}

static step emit_punctuator(sjs_lexer *lexer, uint8_t punctuator) {
    sjs_token token = { 0 };
    token.kind = SJS_TOKEN_PUNCTUATOR;
    token.punctuator = punctuator;
    token.text = (sjs_str){ lexer->punctuator, lexer->punctuator_length };
    return emit(lexer, &token);
}

/* Emit the longest punctuator the held bytes start with, and keep the rest. */
static step flush_punctuator(sjs_lexer *lexer) {
    uint32_t length = lexer->punctuator_length;
    while (length > 0 && punctuator_find(lexer->punctuator, length) == SJS_P_NONE) length--;
    if (length == 0) return lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "unexpected character");
    uint8_t punctuator = punctuator_find(lexer->punctuator, length);
    uint32_t total = lexer->punctuator_length;
    lexer->punctuator_length = length;
    if (emit_punctuator(lexer, punctuator) == STOP) return STOP;
    uint32_t rest = total - length;
    for (uint32_t i = 0; i < rest; i++) lexer->punctuator[i] = lexer->punctuator[length + i];
    lexer->punctuator_length = rest;
    if (rest > 0) {
        lexer->state = L_PUNCTUATOR;
        lexer->start.offset += length;
        lexer->start.column += length;
    }
    return TAKEN;
}

/* A number starting at `.5`: its fraction comes first. */
static void start_fraction(sjs_lexer *lexer) {
    lexer->state = L_NUMBER;
    lexer->substate = N_FRACTION;
    lexer->text.length = 0;
    lexer->radix = 10;
    lexer->exponent = 0;
    lexer->exponent_sign = 1;
    lexer->exponent_digits = 0;
    lexer->seen_digit = false;
    lexer->last_separator = false;
}

static step punctuator_byte(sjs_lexer *lexer, uint8_t c) {
    char *held = lexer->punctuator;
    uint32_t n = lexer->punctuator_length;
    if (n == 1 && held[0] == '/' && c == '/') {
        lexer->state = L_LINE_COMMENT;
        return TAKEN;
    }
    if (n == 1 && held[0] == '/' && c == '*') {
        lexer->state = L_BLOCK_COMMENT;
        lexer->substate = 0;
        return TAKEN;
    }
    if (n == 1 && held[0] == '.' && is_digit(c)) {
        /* `.5` is a number. */
        start_fraction(lexer);
        return AGAIN;
    }
    if (n == 2 && held[0] == '?' && held[1] == '.' && is_digit(c)) {
        /* `a?.5:b` is a conditional. */
        lexer->punctuator_length = 1;
        if (emit_punctuator(lexer, SJS_P_QUESTION) == STOP) return STOP;
        lexer->start.offset += 1;
        lexer->start.column += 1;
        start_fraction(lexer);
        return AGAIN;
    }
    if (n < 4) {
        held[n] = (char)c;
        if (punctuator_prefix(held, n + 1)) {
            lexer->punctuator_length = n + 1;
            return TAKEN;
        }
    }
    /* The byte does not extend what is held: emit the longest punctuator,
     * and read the byte again against what remains (or as the first of a
     * regular expression's body, which the punctuator began). */
    if (flush_punctuator(lexer) == STOP) return STOP;
    if (lexer->punctuator_length == 0 && lexer->state != L_REGEX) lexer->state = L_START;
    return AGAIN;
}

/* ------------------------------------------------------------------------
 * Regular expression literals, after the `/` the parser said begins one:
 * the body to an unescaped `/` outside a class, then the flags. The body
 * may hold any character but a line terminator. The token's text is the
 * body, a `/` and the flags; the parser classifies them (regex.c).
 */

static bool is_line_end_point(uint32_t point) {
    return point == 0x2028u || point == 0x2029u;
}

static step end_regex(sjs_lexer *lexer) {
    sjs_token token = { 0 };
    token.kind = SJS_TOKEN_REGEXP;
    token.text = (sjs_str){ (const char *)lexer->text.ptr, lexer->text.length };
    return emit(lexer, &token);
}

static step regex_byte(sjs_lexer *lexer, uint8_t c) {
    if (lexer->substate == R_FLAGS) {
        if (is_name_part(c)) return text_append(lexer, &c, 1);
        if (c == '\\') return lex_fail(lexer, SJS_ERROR_SYNTAX, "invalid regular expression flags");
        if (c >= 0x80u) {
            lexer->substate = R_FLAG_UTF8;
            return TAKEN;
        }
        if (end_regex(lexer) == STOP) return STOP;
        return AGAIN;
    }
    if (lexer->substate == R_FLAG_UTF8) {
        /* A character after the flags that is not ASCII: one JavaScript
         * would read as a flag, or after a literal, is a syntax error; white
         * space is JavaScript that staticjs does not read there. */
        if (lexer->utf8_need != 0) return TAKEN;
        uint32_t point = lexer->utf8_point;
        if (sjs_is_white_space(point)) {
            if (end_regex(lexer) == STOP) return STOP;
            return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "outside strings, templates and comments staticjs reads ASCII");
        }
        return lex_fail(lexer, SJS_ERROR_SYNTAX, "invalid regular expression flags");
    }
    if (c == '\n' || c == '\r') return lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "a regular expression cannot span lines");
    if (c >= 0x80u && lexer->utf8_need == 0 && is_line_end_point(lexer->utf8_point)) {
        return lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "a regular expression cannot span lines");
    }
    if (lexer->substate == R_ESCAPE) {
        lexer->substate = R_BODY;
        return text_append(lexer, &c, 1);
    }
    if (c == '\\') {
        lexer->substate = R_ESCAPE;
        return text_append(lexer, &c, 1);
    }
    if (lexer->regex_class) {
        if (c == ']') lexer->regex_class = false;
        return text_append(lexer, &c, 1);
    }
    if (c == '[') lexer->regex_class = true;
    if (c == '/') {
        lexer->substate = R_FLAGS;
        return text_append(lexer, &c, 1);
    }
    return text_append(lexer, &c, 1);
}

/* ------------------------------------------------------------------------
 * Names and numbers
 */

static step end_name(sjs_lexer *lexer) {
    sjs_token token = { 0 };
    token.kind = SJS_TOKEN_NAME;
    token.text = (sjs_str){ (const char *)lexer->text.ptr, lexer->text.length };
    for (uint32_t i = 0; i < KEYWORD_COUNT; i++) {
        if (sjs_str_equal(token.text, sjs_cstr(KEYWORDS[i].text))) {
            token.keyword = KEYWORDS[i].keyword;
            break;
        }
    }
    return emit(lexer, &token);
}

static step name_byte(sjs_lexer *lexer, uint8_t c) {
    if (is_name_part(c)) return text_append(lexer, &c, 1);
    if (c == '\\') return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs does not read escapes in names");
    if (c >= 0x80u) return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "names in staticjs are ASCII");
    if (end_name(lexer) == STOP) return STOP;
    return AGAIN;
}

static step start_number(sjs_lexer *lexer, uint8_t c) {
    begin(lexer, L_NUMBER);
    lexer->exponent = 0;
    lexer->exponent_sign = 1;
    lexer->exponent_digits = 0;
    lexer->seen_digit = true;
    lexer->last_separator = false;
    lexer->radix = 10;
    lexer->escape_value = 0;
    if (c == '0') {
        lexer->substate = N_ZERO;
        return TAKEN;
    }
    lexer->substate = N_INTEGER;
    return text_append(lexer, &c, 1);
}

static step end_number(sjs_lexer *lexer, uint8_t next, bool at_end) {
    if (!at_end && (is_name_part(next) || next == '\\')) {
        return lex_fail(lexer, SJS_ERROR_SYNTAX, "a number cannot be followed by a name or a digit");
    }
    if (lexer->last_separator) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator must sit between digits");
    sjs_token token = { 0 };
    token.kind = SJS_TOKEN_NUMBER;
    if (lexer->radix != 10) {
        if (!lexer->seen_digit) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a number needs digits after its prefix");
        token.number = (double)lexer->escape_value;
    } else {
        if (lexer->substate == N_EXPONENT_START || lexer->substate == N_EXPONENT_SIGN ||
            (lexer->substate == N_EXPONENT && lexer->exponent_digits == 0)) {
            return lex_fail(lexer, SJS_ERROR_SYNTAX, "an exponent needs digits");
        }
        sjs_str digits = { (const char *)lexer->text.ptr, lexer->text.length };
        token.number = sjs_number_from_decimal(digits, lexer->exponent);
    }
    return emit(lexer, &token);
}

static step number_digit(sjs_lexer *lexer, uint8_t c) {
    lexer->last_separator = false;
    lexer->seen_digit = true;
    return text_append(lexer, &c, 1);
}

static step number_separator(sjs_lexer *lexer) {
    if (lexer->last_separator || !lexer->seen_digit) {
        return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator must sit between digits");
    }
    lexer->last_separator = true;
    return TAKEN;
}

/* The `n` of a BigInt literal, after its digits (`zero`: the digit of
 * `0n`, which the text does not hold yet). The literal ends at the next
 * byte, which must not continue it. */
static step start_bigint(sjs_lexer *lexer, const uint8_t *zero) {
    lexer->substate = N_BIGINT;
    if (zero != NULL) return text_append(lexer, zero, 1);
    return TAKEN;
}

static step end_bigint(sjs_lexer *lexer) {
    sjs_token token = { 0 };
    token.kind = SJS_TOKEN_NUMBER;
    token.bigint = (uint8_t)lexer->radix;
    token.text = (sjs_str){ (const char *)lexer->text.ptr, lexer->text.length };
    return emit(lexer, &token);
}

static step number_byte(sjs_lexer *lexer, uint8_t c) {
    switch (lexer->substate) {
    case N_ZERO:
        if (c == 'x' || c == 'X' || c == 'o' || c == 'O' || c == 'b' || c == 'B') {
            lexer->radix = (c == 'x' || c == 'X') ? 16u : ((c == 'o' || c == 'O') ? 8u : 2u);
            lexer->substate = N_RADIX;
            lexer->seen_digit = false;
            return TAKEN;
        }
        if (c == '_') return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator cannot follow a leading 0");
        if (is_digit(c)) {
            /* A legacy octal number, or `01n`, which is not JavaScript:
             * which one, its last digit tells. */
            lexer->substate = N_LEGACY;
            return TAKEN;
        }
        if (c == 'n') {
            uint8_t zero = '0';
            return start_bigint(lexer, &zero);
        }
        if (c == '.') {
            lexer->substate = N_FRACTION;
            lexer->seen_digit = false;
            return TAKEN;
        }
        if (c == 'e' || c == 'E') {
            lexer->substate = N_EXPONENT_START;
            return TAKEN;
        }
        if (end_number(lexer, c, false) == STOP) return STOP;
        return AGAIN;
    case N_INTEGER:
        if (is_digit(c)) return number_digit(lexer, c);
        if (c == '_') return number_separator(lexer);
        if (lexer->last_separator) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator must sit between digits");
        if (c == 'n') return start_bigint(lexer, NULL);
        if (c == '.') {
            lexer->substate = N_FRACTION;
            lexer->seen_digit = false;
            return TAKEN;
        }
        if (c == 'e' || c == 'E') {
            lexer->substate = N_EXPONENT_START;
            return TAKEN;
        }
        if (end_number(lexer, c, false) == STOP) return STOP;
        return AGAIN;
    case N_FRACTION:
        if (is_digit(c)) {
            lexer->exponent--;
            return number_digit(lexer, c);
        }
        if (c == '_') return number_separator(lexer);
        if (lexer->last_separator) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator must sit between digits");
        if (c == 'e' || c == 'E') {
            lexer->substate = N_EXPONENT_START;
            return TAKEN;
        }
        if (end_number(lexer, c, false) == STOP) return STOP;
        return AGAIN;
    case N_EXPONENT_START:
        lexer->exponent_sign = 1;
        lexer->exponent_digits = 0;
        lexer->escape_value = 0;
        if (c == '+' || c == '-') {
            lexer->exponent_sign = c == '-' ? -1 : 1;
            lexer->substate = N_EXPONENT_SIGN;
            return TAKEN;
        }
        lexer->substate = N_EXPONENT;
        return AGAIN;
    case N_EXPONENT_SIGN:
        lexer->substate = N_EXPONENT;
        return AGAIN;
    case N_EXPONENT:
        if (is_digit(c)) {
            lexer->last_separator = false;
            lexer->seen_digit = true;
            if (lexer->escape_value < 100000000u) lexer->escape_value = lexer->escape_value * 10u + (uint32_t)(c - '0');
            lexer->exponent_digits++;
            return TAKEN;
        }
        if (c == '_' && lexer->exponent_digits > 0) {
            if (lexer->last_separator) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator must sit between digits");
            lexer->last_separator = true;
            return TAKEN;
        }
        if (lexer->exponent_digits > 0) {
            lexer->exponent += lexer->exponent_sign * (int32_t)lexer->escape_value;
        }
        if (end_number(lexer, c, false) == STOP) return STOP;
        return AGAIN;
    case N_LEGACY:
        if (is_digit(c)) return TAKEN;
        if (c == 'n') return lex_fail(lexer, SJS_ERROR_SYNTAX, "a BigInt literal cannot start with 0");
        if (c == '_') return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator cannot follow a leading 0");
        return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs does not read legacy octal numbers");
    case N_BIGINT:
        if (is_name_part(c) || c == '\\') return lex_fail(lexer, SJS_ERROR_SYNTAX, "a number cannot be followed by a name or a digit");
        if (end_bigint(lexer) == STOP) return STOP;
        return AGAIN;
    default: { /* N_RADIX */
        int32_t v = hex_value(c);
        if (v >= 0 && (uint32_t)v < lexer->radix) {
            lexer->last_separator = false;
            lexer->seen_digit = true;
            return text_append(lexer, &c, 1);
        }
        if (c == '_') return number_separator(lexer);
        if (!lexer->seen_digit) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a number needs digits after its prefix");
        if (lexer->last_separator) return lex_fail(lexer, SJS_ERROR_SYNTAX, "a numeric separator must sit between digits");
        if (c == 'n') return start_bigint(lexer, NULL);
        if (is_name_part(c) || c == '\\') return lex_fail(lexer, SJS_ERROR_SYNTAX, "a number cannot be followed by a name or a digit");
        sjs_token token = { 0 };
        token.kind = SJS_TOKEN_NUMBER;
        token.number = sjs_number_from_binary((sjs_str){ (const char *)lexer->text.ptr, lexer->text.length }, lexer->radix);
        if (emit(lexer, &token) == STOP) return STOP;
        return AGAIN;
    }
    }
}

/* ------------------------------------------------------------------------
 * Strings and templates
 */

static step end_string(sjs_lexer *lexer, uint8_t kind) {
    sjs_token token = { 0 };
    token.kind = kind;
    token.lone_surrogate = lexer->lone_surrogate ? 1u : 0u;
    token.escaped = lexer->escaped ? 1u : 0u;
    lexer->lone_surrogate = false;
    lexer->escaped = false;
    token.text = (sjs_str){ (const char *)lexer->text.ptr, lexer->text.length };
    return emit(lexer, &token);
}

/* A surrogate with no partner. A string records it, and the parser refuses
 * the string: as unsupported, or as a syntax error where JavaScript requires
 * well-formed text (a module export name). A template refuses it here. */
static step note_lone_surrogate(sjs_lexer *lexer) {
    if (lexer->state != L_STRING) return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs strings cannot hold a lone surrogate");
    lexer->lone_surrogate = true;
    return TAKEN;
}

/* Where text goes on after an escape: a high surrogate still waits for a
 * low one across a line continuation. */
static uint8_t after_escape(const sjs_lexer *lexer) {
    return lexer->pending_high != 0 ? S_AFTER_HIGH : S_TEXT;
}

/* A code point from an escape; surrogates must come in pairs. */
static step escaped_point(sjs_lexer *lexer, uint32_t point) {
    lexer->substate = S_TEXT;
    if (lexer->pending_high != 0) {
        uint32_t high = lexer->pending_high;
        lexer->pending_high = 0;
        if (point >= 0xDC00u && point <= 0xDFFFu) {
            return text_append_point(lexer, 0x10000u + ((high - 0xD800u) << 10) + (point - 0xDC00u));
        }
        if (note_lone_surrogate(lexer) == STOP) return STOP;
    }
    if (point >= 0xD800u && point <= 0xDBFFu) {
        lexer->pending_high = point;
        lexer->substate = S_AFTER_HIGH;
        return TAKEN;
    }
    if (point >= 0xDC00u && point <= 0xDFFFu) return note_lone_surrogate(lexer);
    return text_append_point(lexer, point);
}

static step escape_byte(sjs_lexer *lexer, uint8_t c) {
    switch (c) {
    case 'n': return escaped_point(lexer, '\n');
    case 't': return escaped_point(lexer, '\t');
    case 'r': return escaped_point(lexer, '\r');
    case 'b': return escaped_point(lexer, '\b');
    case 'f': return escaped_point(lexer, '\f');
    case 'v': return escaped_point(lexer, '\v');
    case '0':
        lexer->substate = S_ESCAPE_ZERO;
        return TAKEN;
    case 'x':
        lexer->substate = S_HEX;
        lexer->escape_value = 0;
        lexer->escape_digits = 0;
        return TAKEN;
    case 'u':
        lexer->substate = S_UNICODE_START;
        lexer->escape_value = 0;
        lexer->escape_digits = 0;
        return TAKEN;
    case '\n':
        lexer->substate = after_escape(lexer);
        return TAKEN;
    case '\r':
        lexer->substate = S_ESCAPE_CR;
        return TAKEN;
    default:
        if (is_digit(c)) return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs does not read octal escapes");
        if (c >= 0x80u) return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs escapes only ASCII characters");
        return escaped_point(lexer, c);
    }
}

/* Bytes of a string or a template that are not its delimiters. */
static step quoted_byte(sjs_lexer *lexer, uint8_t c) {
    switch (lexer->substate) {
    case S_ESCAPE:
        return escape_byte(lexer, c);
    case S_ESCAPE_ZERO:
        lexer->substate = S_TEXT;
        if (is_digit(c)) return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs does not read octal escapes");
        if (escaped_point(lexer, 0) == STOP) return STOP;
        return AGAIN;
    case S_ESCAPE_CR:
        lexer->substate = after_escape(lexer);
        return c == '\n' ? TAKEN : AGAIN;
    case S_HEX: {
        int32_t v = hex_value(c);
        if (v < 0) return lex_fail(lexer, SJS_ERROR_SYNTAX, "\\x needs two hexadecimal digits");
        lexer->escape_value = lexer->escape_value * 16u + (uint32_t)v;
        if (++lexer->escape_digits < 2) return TAKEN;
        return escaped_point(lexer, lexer->escape_value);
    }
    case S_UNICODE_START:
        if (c == '{') {
            lexer->substate = S_UNICODE_BRACED;
            return TAKEN;
        }
        lexer->substate = S_UNICODE;
        return AGAIN;
    case S_UNICODE: {
        int32_t v = hex_value(c);
        if (v < 0) return lex_fail(lexer, SJS_ERROR_SYNTAX, "\\u needs four hexadecimal digits");
        lexer->escape_value = lexer->escape_value * 16u + (uint32_t)v;
        if (++lexer->escape_digits < 4) return TAKEN;
        return escaped_point(lexer, lexer->escape_value);
    }
    case S_UNICODE_BRACED: {
        if (c == '}') {
            if (lexer->escape_digits == 0) return lex_fail(lexer, SJS_ERROR_SYNTAX, "\\u{} needs digits");
            return escaped_point(lexer, lexer->escape_value);
        }
        int32_t v = hex_value(c);
        if (v < 0) return lex_fail(lexer, SJS_ERROR_SYNTAX, "\\u{...} holds hexadecimal digits");
        lexer->escape_value = lexer->escape_value * 16u + (uint32_t)v;
        lexer->escape_digits++;
        if (lexer->escape_value > 0x10FFFFu) return lex_fail(lexer, SJS_ERROR_SYNTAX, "\\u{...} is past U+10FFFF");
        return TAKEN;
    }
    case S_AFTER_HIGH:
        if (c != '\\') {
            lexer->pending_high = 0;
            lexer->substate = S_TEXT;
            if (note_lone_surrogate(lexer) == STOP) return STOP;
            return AGAIN;
        }
        lexer->substate = S_ESCAPE;
        return TAKEN;
    default:
        return TAKEN;
    }
}

static step string_byte(sjs_lexer *lexer, uint8_t c) {
    if (lexer->substate != S_TEXT) return quoted_byte(lexer, c);
    if (c == lexer->quote) {
        if (end_string(lexer, SJS_TOKEN_STRING) == STOP) return STOP;
        return TAKEN;
    }
    if (c == '\\') {
        lexer->substate = S_ESCAPE;
        lexer->escaped = true;
        return TAKEN;
    }
    if (c == '\n' || c == '\r') return lex_fail(lexer, SJS_ERROR_SYNTAX, "a string cannot span lines");
    return text_append(lexer, &c, 1);
}

/* lexer->quote is 1 for a template that continues after a substitution. */
static step template_byte(sjs_lexer *lexer, uint8_t c) {
    if (lexer->substate == S_DOLLAR) {
        lexer->substate = S_TEXT;
        if (c == '{') {
            uint8_t open = 1;
            if (lexer->braces.length >= lexer->limits.max_nesting) {
                return lex_fail(lexer, SJS_ERROR_LIMIT, "templates and braces nest deeper than max_nesting");
            }
            if (sjs_buf_append(&lexer->allocator, &lexer->braces, (sjs_bytes){ &open, 1 }) != SJS_OK) {
                return lex_fail(lexer, SJS_ERROR_MEMORY, "out of memory");
            }
            return end_string(lexer, lexer->quote ? SJS_TOKEN_TEMPLATE_MIDDLE : SJS_TOKEN_TEMPLATE_HEAD);
        }
        uint8_t dollar = '$';
        if (text_append(lexer, &dollar, 1) == STOP) return STOP;
        return AGAIN;
    }
    if (lexer->substate == S_CR) {
        lexer->substate = S_TEXT;
        if (c == '\n') return TAKEN;
        return AGAIN;
    }
    if (lexer->substate != S_TEXT) return quoted_byte(lexer, c);
    switch (c) {
    case '`':
        return end_string(lexer, lexer->quote ? SJS_TOKEN_TEMPLATE_TAIL : SJS_TOKEN_TEMPLATE);
    case '\\':
        lexer->substate = S_ESCAPE;
        return TAKEN;
    case '$':
        lexer->substate = S_DOLLAR;
        return TAKEN;
    case '\r': {
        /* Line ends in a template read as \n. */
        uint8_t newline = '\n';
        lexer->substate = S_CR;
        return text_append(lexer, &newline, 1);
    }
    default:
        return text_append(lexer, &c, 1);
    }
}

/* ------------------------------------------------------------------------
 * Between tokens
 */

static step open_brace(sjs_lexer *lexer, uint8_t kind) {
    if (lexer->braces.length >= lexer->limits.max_nesting) {
        return lex_fail(lexer, SJS_ERROR_LIMIT, "templates and braces nest deeper than max_nesting");
    }
    if (sjs_buf_append(&lexer->allocator, &lexer->braces, (sjs_bytes){ &kind, 1 }) != SJS_OK) {
        return lex_fail(lexer, SJS_ERROR_MEMORY, "out of memory");
    }
    return TAKEN;
}

static step start_byte(sjs_lexer *lexer, uint8_t c) {
    switch (c) {
    case ' ':
    case '\t':
    case '\v':
    case '\f':
        return TAKEN;
    case '\n':
    case '\r':
        lexer->newline_before = true;
        return TAKEN;
    case '"':
    case '\'':
        begin(lexer, L_STRING);
        lexer->quote = c;
        lexer->pending_high = 0;
        return TAKEN;
    case '`':
        begin(lexer, L_TEMPLATE);
        lexer->quote = 0;
        lexer->pending_high = 0;
        return TAKEN;
    case '{':
        begin(lexer, L_PUNCTUATOR);
        if (open_brace(lexer, 0) == STOP) return STOP;
        lexer->punctuator[0] = '{';
        lexer->punctuator_length = 1;
        return emit_punctuator(lexer, SJS_P_LBRACE);
    case '}':
        begin(lexer, L_PUNCTUATOR);
        if (lexer->braces.length > 0) {
            uint8_t kind = lexer->braces.ptr[--lexer->braces.length];
            if (kind == 1) {
                begin(lexer, L_TEMPLATE);
                lexer->quote = 1;
                lexer->pending_high = 0;
                return TAKEN;
            }
        }
        lexer->punctuator[0] = '}';
        lexer->punctuator_length = 1;
        return emit_punctuator(lexer, SJS_P_RBRACE);
    default:
        break;
    }
    if (is_name_start(c)) {
        begin(lexer, L_NAME);
        return text_append(lexer, &c, 1);
    }
    if (is_digit(c)) return start_number(lexer, c);
    if (c == '\\') return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "staticjs does not read escapes in names");
    if (c >= 0x80u) {
        if (lexer->here.offset == 0 && c == 0xEFu) {
            begin(lexer, L_BOM);
            return TAKEN;
        }
        return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "outside strings, templates and comments staticjs reads ASCII");
    }
    if (c < 0x20u || c == 0x7Fu) return lex_fail(lexer, SJS_ERROR_SYNTAX, "unexpected control character");
    begin(lexer, L_PUNCTUATOR);
    lexer->punctuator[0] = (char)c;
    lexer->punctuator_length = 1;
    if (!punctuator_prefix(lexer->punctuator, 1)) return lex_fail(lexer, SJS_ERROR_SYNTAX, "unexpected character");
    return TAKEN;
}

static step lex_byte(sjs_lexer *lexer, uint8_t c) {
    switch (lexer->state) {
    case L_START:
        return start_byte(lexer, c);
    case L_NAME:
        return name_byte(lexer, c);
    case L_NUMBER:
        return number_byte(lexer, c);
    case L_STRING:
        return string_byte(lexer, c);
    case L_TEMPLATE:
        return template_byte(lexer, c);
    case L_PUNCTUATOR:
        return punctuator_byte(lexer, c);
    case L_REGEX:
        return regex_byte(lexer, c);
    case L_LINE_COMMENT:
        if (c == '\n' || c == '\r') {
            lexer->state = L_START;
            lexer->newline_before = true;
        }
        return TAKEN;
    case L_BLOCK_COMMENT:
        if (c == '\n' || c == '\r') lexer->newline_before = true;
        if (lexer->substate == 1 && c == '/') {
            lexer->state = L_START;
            return TAKEN;
        }
        lexer->substate = c == '*' ? 1 : 0;
        return TAKEN;
    case L_BOM:
        lexer->substate++;
        if ((lexer->substate == 1 && c != 0xBBu) || (lexer->substate == 2 && c != 0xBFu)) {
            return lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "outside strings, templates and comments staticjs reads ASCII");
        }
        if (lexer->substate == 2) lexer->state = L_START;
        return TAKEN;
    default:
        return STOP;
    }
}

/* Well-formed UTF-8, checked across chunk boundaries. */
static bool utf8_byte(sjs_lexer *lexer, uint8_t c) {
    if (lexer->utf8_need == 0) {
        if (c < 0x80u) return true;
        if ((c & 0xE0u) == 0xC0u) {
            lexer->utf8_need = 1;
            lexer->utf8_point = c & 0x1Fu;
            lexer->utf8_minimum = 0x80u;
        } else if ((c & 0xF0u) == 0xE0u) {
            lexer->utf8_need = 2;
            lexer->utf8_point = c & 0x0Fu;
            lexer->utf8_minimum = 0x800u;
        } else if ((c & 0xF8u) == 0xF0u) {
            lexer->utf8_need = 3;
            lexer->utf8_point = c & 0x07u;
            lexer->utf8_minimum = 0x10000u;
        } else {
            return false;
        }
        return true;
    }
    if ((c & 0xC0u) != 0x80u) return false;
    lexer->utf8_point = (lexer->utf8_point << 6) | (c & 0x3Fu);
    if (--lexer->utf8_need == 0) {
        uint32_t p = lexer->utf8_point;
        if (p < lexer->utf8_minimum || p > 0x10FFFFu || (p >= 0xD800u && p <= 0xDFFFu)) return false;
    }
    return true;
}

static void advance(sjs_lexer *lexer, uint8_t c) {
    lexer->here.offset++;
    if (c == '\n') {
        if (!lexer->previous_cr) {
            lexer->here.line++;
        }
        lexer->here.column = 1;
    } else if (c == '\r') {
        lexer->here.line++;
        lexer->here.column = 1;
    } else {
        lexer->here.column++;
    }
    lexer->previous_cr = c == '\r';
}

sjs_result sjs_lexer_init(sjs_lexer *lexer) {
    lexer->here = (sjs_position){ 0, 1, 1 };
    lexer->start = lexer->here;
    lexer->state = L_START;
    return SJS_OK;
}

sjs_result sjs_lexer_push(sjs_lexer *lexer, sjs_bytes chunk) {
    if (lexer->failed) return SJS_ERR_PROGRAM;
    for (uint32_t i = 0; i < chunk.length; i++) {
        uint8_t c = chunk.ptr[i];
        if (lexer->here.offset >= lexer->limits.max_source_bytes) {
            lex_fail(lexer, SJS_ERROR_LIMIT, "the source is longer than max_source_bytes");
            return SJS_ERR_PROGRAM;
        }
        if (!utf8_byte(lexer, c)) {
            lex_fail(lexer, SJS_ERROR_SYNTAX, "the source is not well-formed UTF-8");
            return SJS_ERR_PROGRAM;
        }
        step s;
        do {
            s = lex_byte(lexer, c);
        } while (s == AGAIN);
        if (s == STOP) return SJS_ERR_PROGRAM;
        advance(lexer, c);
    }
    return SJS_OK;
}

sjs_result sjs_lexer_finish(sjs_lexer *lexer) {
    if (lexer->failed) return SJS_ERR_PROGRAM;
    if (lexer->utf8_need != 0) {
        lex_fail(lexer, SJS_ERROR_SYNTAX, "the source is not well-formed UTF-8");
        return SJS_ERR_PROGRAM;
    }
    step s = TAKEN;
    switch (lexer->state) {
    case L_NAME:
        s = end_name(lexer);
        break;
    case L_NUMBER:
        if (lexer->substate == N_RADIX || lexer->substate == N_LEGACY) {
            /* A byte that cannot be a digit ends it. */
            s = number_byte(lexer, ' ');
        } else if (lexer->substate == N_BIGINT) {
            s = end_bigint(lexer);
        } else {
            if (lexer->substate == N_EXPONENT && lexer->exponent_digits > 0) {
                lexer->exponent += lexer->exponent_sign * (int32_t)lexer->escape_value;
            }
            s = end_number(lexer, 0, true);
        }
        break;
    case L_PUNCTUATOR:
        while (s != STOP && lexer->punctuator_length > 0) s = flush_punctuator(lexer);
        /* A `/` at the end that begins a regular expression has no body. */
        if (s != STOP && lexer->state == L_REGEX) s = lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "the source ends inside a regular expression");
        break;
    case L_REGEX:
        if (lexer->substate == R_FLAGS) s = end_regex(lexer);
        else if (lexer->substate == R_FLAG_UTF8) s = lex_fail(lexer, SJS_ERROR_SYNTAX, "invalid regular expression flags");
        else s = lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "the source ends inside a regular expression");
        break;
    case L_STRING:
        s = lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "the source ends inside a string");
        break;
    case L_TEMPLATE:
        s = lex_fail_at_start(lexer, SJS_ERROR_SYNTAX, "the source ends inside a template");
        break;
    case L_BLOCK_COMMENT:
        s = lex_fail(lexer, SJS_ERROR_SYNTAX, "the source ends inside a comment");
        break;
    case L_BOM:
        s = lex_fail(lexer, SJS_ERROR_UNSUPPORTED, "outside strings, templates and comments staticjs reads ASCII");
        break;
    default:
        break;
    }
    if (s == STOP) return SJS_ERR_PROGRAM;
    lexer->start = lexer->here;
    sjs_token end = { 0 };
    end.kind = SJS_TOKEN_END;
    if (emit(lexer, &end) == STOP) return SJS_ERR_PROGRAM;
    return SJS_OK;
}

void sjs_lexer_destroy(sjs_lexer *lexer) {
    sjs_buf_free(&lexer->allocator, &lexer->text);
    sjs_buf_free(&lexer->allocator, &lexer->braces);
}
