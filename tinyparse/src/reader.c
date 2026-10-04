/*
** reader.c -- the .scm reader.
**
** Turns Scheme-shaped grammar bytes into a tree of tp_form records: lists,
** symbols, keyword symbols (trailing colon), strings with escapes, integers,
** byte/hex literals, with `;' line comments and whitespace skipped. It is not a
** Scheme evaluator -- it only reads data. Freestanding: all forms come from the
** arena, no libc.
*/
#include "internal.h"

/* ** The reader's mutable cursor over the source bytes. */
typedef struct tp_reader {
    tp_arena          *arena;
    const uint8_t     *bytes;
    uint32_t           length;
    uint32_t           position;
    int                ok;
    const char        *error;
    uint32_t           error_byte;
} tp_reader;

/* ** True for ASCII whitespace, which separates tokens and is discarded. */
static int tp_is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* ** True for a byte that terminates a bare symbol or number token. */
static int tp_is_delimiter(uint8_t c)
{
    return tp_is_space(c) || c == '(' || c == ')' || c == ';' || c == '"' || c == '\0';
}

/* ** Skip whitespace and `;'-to-end-of-line comments before the next form. */
static void tp_skip_trivia(tp_reader *reader)
{
    while (reader->position < reader->length) {
        uint8_t c = reader->bytes[reader->position];
        if (tp_is_space(c)) {
            reader->position++;
        } else if (c == ';') {
            while (reader->position < reader->length &&
                   reader->bytes[reader->position] != '\n')
                reader->position++;
        } else {
            break;
        }
    }
}

/* ** Allocate a fresh form of the given kind at the current source offset. */
static tp_form *tp_form_new(tp_reader *reader, tp_form_kind kind)
{
    tp_form *form = (tp_form *)tp_alloc(reader->arena, sizeof *form);
    if (form == 0) {
        reader->ok = 0;
        reader->error = "out of memory reading grammar";
        reader->error_byte = reader->position;
        return 0;
    }
    form->kind       = kind;
    form->text       = tp_text("");
    form->integer    = 0;
    form->items      = 0;
    form->count      = 0;
    form->start_byte = reader->position;
    return form;
}

static tp_form *tp_read_form(tp_reader *reader);

/* ** Read a parenthesised list. The opening `(' has been consumed. Children are
** collected into a geometrically grown vector. A missing `)' is an error. */
static tp_form *tp_read_list(tp_reader *reader)
{
    tp_form  *list = tp_form_new(reader, TP_FORM_LIST);
    tp_form **items = 0;
    uint32_t  count = 0;
    uint32_t  capacity = 0;

    if (list == 0)
        return 0;
    list->start_byte = reader->position - 1; /* the '(' */

    for (;;) {
        tp_skip_trivia(reader);
        if (reader->position >= reader->length) {
            reader->ok = 0;
            reader->error = "unterminated list";
            reader->error_byte = list->start_byte;
            return 0;
        }
        if (reader->bytes[reader->position] == ')') {
            reader->position++;
            break;
        }
        {
            tp_form *child = tp_read_form(reader);
            if (child == 0)
                return 0;
            if (count == capacity) {
                uint32_t new_capacity = capacity == 0 ? 4 : capacity * 2;
                tp_form **grown = (tp_form **)tp_alloc(
                    reader->arena, new_capacity * sizeof *grown);
                uint32_t i;
                if (grown == 0) {
                    reader->ok = 0;
                    reader->error = "out of memory reading list";
                    reader->error_byte = reader->position;
                    return 0;
                }
                for (i = 0; i < count; i++)
                    grown[i] = items[i];
                items = grown;
                capacity = new_capacity;
            }
            items[count++] = child;
        }
    }

    list->items = items;
    list->count = count;
    return list;
}

/* ** Read a double-quoted string, decoding the escapes the grammar uses:
** \n \t \r \\ \" \0 and \xHH. The decoded bytes are stored in the form. An
** unterminated string is an error. */
static tp_form *tp_read_string(tp_reader *reader)
{
    tp_form *form = tp_form_new(reader, TP_FORM_STRING);
    uint8_t *out;
    uint32_t out_len = 0;
    uint32_t cap;

    if (form == 0)
        return 0;
    reader->position++; /* opening quote */

    /* Worst case the decoded string is no longer than the remaining input. */
    cap = reader->length - reader->position + 1;
    out = (uint8_t *)tp_alloc(reader->arena, cap == 0 ? 1 : cap);
    if (out == 0) {
        reader->ok = 0;
        reader->error = "out of memory reading string";
        reader->error_byte = reader->position;
        return 0;
    }

    while (reader->position < reader->length) {
        uint8_t c = reader->bytes[reader->position++];
        if (c == '"') {
            form->text.pointer = out;
            form->text.length  = out_len;
            return form;
        }
        if (c == '\\') {
            if (reader->position >= reader->length)
                break;
            {
                uint8_t e = reader->bytes[reader->position++];
                switch (e) {
                case 'n': out[out_len++] = '\n'; break;
                case 't': out[out_len++] = '\t'; break;
                case 'r': out[out_len++] = '\r'; break;
                case '0': out[out_len++] = '\0'; break;
                case '\\': out[out_len++] = '\\'; break;
                case '"': out[out_len++] = '"'; break;
                case 'x': {
                    uint8_t hi, lo, value = 0;
                    int got = 0;
                    while (got < 2 && reader->position < reader->length) {
                        uint8_t h = reader->bytes[reader->position];
                        uint8_t d;
                        if (h >= '0' && h <= '9') d = (uint8_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') d = (uint8_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') d = (uint8_t)(h - 'A' + 10);
                        else break;
                        value = (uint8_t)(value * 16 + d);
                        reader->position++;
                        got++;
                    }
                    (void)hi; (void)lo;
                    out[out_len++] = value;
                    break;
                }
                default:
                    out[out_len++] = e; /* unknown escape: keep the byte */
                    break;
                }
            }
        } else {
            out[out_len++] = c;
        }
    }

    reader->ok = 0;
    reader->error = "unterminated string";
    reader->error_byte = form->start_byte;
    return 0;
}

/* ** Decode a hex byte literal of the form #xHH... into bytes. The `#' has been
** seen; consumes the `x' and the following hex digit pairs. */
static tp_form *tp_read_hex(tp_reader *reader)
{
    tp_form *form = tp_form_new(reader, TP_FORM_BYTES);
    uint8_t *out;
    uint32_t out_len = 0;
    uint32_t cap;

    if (form == 0)
        return 0;
    reader->position++; /* '#' */
    if (reader->position < reader->length &&
        (reader->bytes[reader->position] == 'x' ||
         reader->bytes[reader->position] == 'X'))
        reader->position++;

    cap = reader->length - reader->position + 1;
    out = (uint8_t *)tp_alloc(reader->arena, cap == 0 ? 1 : cap);
    if (out == 0) {
        reader->ok = 0;
        reader->error = "out of memory reading byte literal";
        reader->error_byte = reader->position;
        return 0;
    }

    while (reader->position + 1 < reader->length) {
        uint8_t a = reader->bytes[reader->position];
        uint8_t b = reader->bytes[reader->position + 1];
        uint8_t da, db;
        if      (a >= '0' && a <= '9') da = (uint8_t)(a - '0');
        else if (a >= 'a' && a <= 'f') da = (uint8_t)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') da = (uint8_t)(a - 'A' + 10);
        else break;
        if      (b >= '0' && b <= '9') db = (uint8_t)(b - '0');
        else if (b >= 'a' && b <= 'f') db = (uint8_t)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') db = (uint8_t)(b - 'A' + 10);
        else break;
        out[out_len++] = (uint8_t)(da * 16 + db);
        reader->position += 2;
    }

    form->text.pointer = out;
    form->text.length  = out_len;
    return form;
}

/* ** True if the run of bytes is a plain decimal integer (optionally signed). */
static int tp_is_integer(const uint8_t *p, uint32_t n)
{
    uint32_t i = 0;
    if (n == 0)
        return 0;
    if (p[0] == '+' || p[0] == '-') {
        if (n == 1)
            return 0;
        i = 1;
    }
    for (; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    return 1;
}

/* ** Read a bare token: a symbol, a keyword (trailing colon), or an integer.
** The token runs to the next delimiter. */
static tp_form *tp_read_atom(tp_reader *reader)
{
    uint32_t start = reader->position;
    tp_form *form;
    const uint8_t *text;
    uint32_t       length;

    while (reader->position < reader->length &&
           !tp_is_delimiter(reader->bytes[reader->position]))
        reader->position++;

    text   = reader->bytes + start;
    length = reader->position - start;

    if (length > 0 && text[length - 1] == ':') {
        form = tp_form_new(reader, TP_FORM_KEYWORD);
        if (form == 0)
            return 0;
        form->start_byte   = start;
        form->text.pointer = text;
        form->text.length  = length - 1; /* strip the trailing colon */
        return form;
    }

    if (tp_is_integer(text, length)) {
        int64_t  value = 0;
        int      negative = 0;
        uint32_t i = 0;
        form = tp_form_new(reader, TP_FORM_INTEGER);
        if (form == 0)
            return 0;
        form->start_byte = start;
        if (text[0] == '+' || text[0] == '-') {
            negative = (text[0] == '-');
            i = 1;
        }
        for (; i < length; i++)
            value = value * 10 + (text[i] - '0');
        form->integer = negative ? -value : value;
        form->text.pointer = text;
        form->text.length  = length;
        return form;
    }

    form = tp_form_new(reader, TP_FORM_SYMBOL);
    if (form == 0)
        return 0;
    form->start_byte   = start;
    form->text.pointer = text;
    form->text.length  = length;
    return form;
}

/* ** Read the next single form. Dispatches on the leading byte. */
static tp_form *tp_read_form(tp_reader *reader)
{
    uint8_t c;
    tp_skip_trivia(reader);
    if (reader->position >= reader->length) {
        reader->ok = 0;
        reader->error = "unexpected end of grammar";
        reader->error_byte = reader->position;
        return 0;
    }

    c = reader->bytes[reader->position];
    if (c == '(') {
        reader->position++;
        return tp_read_list(reader);
    }
    if (c == ')') {
        reader->ok = 0;
        reader->error = "unexpected ')'";
        reader->error_byte = reader->position;
        return 0;
    }
    if (c == '"')
        return tp_read_string(reader);
    /* `#xHH...' is a hex byte literal. A `#' followed by anything else is an
    ** ordinary symbol -- this is how query predicates spell their heads, e.g.
    ** `#eq?' and `#match?' -- so only the `#x'/`#X' form enters the hex reader. */
    if (c == '#' && reader->position + 1 < reader->length &&
        (reader->bytes[reader->position + 1] == 'x' ||
         reader->bytes[reader->position + 1] == 'X'))
        return tp_read_hex(reader);
    return tp_read_atom(reader);
}

/* ** Read a whole .scm source into a vector of top-level forms. */
tp_read_result tp_read(tp_arena *arena, tinyparse_string source)
{
    tp_reader      reader;
    tp_read_result result;
    tp_form      **forms = 0;
    uint32_t       count = 0;
    uint32_t       capacity = 0;

    reader.arena      = arena;
    reader.bytes      = source.pointer;
    reader.length     = source.length;
    reader.position   = 0;
    reader.ok         = 1;
    reader.error      = "";
    reader.error_byte = 0;

    for (;;) {
        tp_form *form;
        tp_skip_trivia(&reader);
        if (reader.position >= reader.length)
            break;
        form = tp_read_form(&reader);
        if (form == 0)
            break;
        if (count == capacity) {
            uint32_t new_capacity = capacity == 0 ? 8 : capacity * 2;
            tp_form **grown = (tp_form **)tp_alloc(
                arena, new_capacity * sizeof *grown);
            uint32_t i;
            if (grown == 0) {
                reader.ok = 0;
                reader.error = "out of memory reading grammar";
                break;
            }
            for (i = 0; i < count; i++)
                grown[i] = forms[i];
            forms = grown;
            capacity = new_capacity;
        }
        forms[count++] = form;
    }

    result.forms      = forms;
    result.count      = count;
    result.ok         = reader.ok;
    result.error      = reader.ok ? "" : reader.error;
    result.error_byte = reader.error_byte;
    return result;
}
