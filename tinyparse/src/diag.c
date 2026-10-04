/*
** diag.c -- the public diagnostic and tree accessors for a parser.
**
** Diagnostics are collected during the parse into the parser's tp_diag_list
** (see util.c for the collector). These thin accessors expose the parser's
** tree and its diagnostics through the public, opaque-handle API. The core
** never prints; it hands back structured data and lets the host render it.
** Freestanding.
*/
#include "internal.h"

/* ** The concrete syntax tree a parser has built, or NULL if none yet. */
const tinyparse_tree *tinyparse_parser_tree(const tinyparse_parser *parser)
{
    if (parser == 0)
        return 0;
    return parser->tree;
}

/* ** The number of diagnostics a parser has produced. */
uint32_t tinyparse_parser_diagnostic_count(const tinyparse_parser *parser)
{
    if (parser == 0)
        return 0;
    return parser->diagnostics.count;
}

/* ** The diagnostic at a given index, or NULL if out of range. */
const tinyparse_diagnostic *tinyparse_parser_diagnostic(
    const tinyparse_parser *parser, uint32_t index)
{
    if (parser == 0 || index >= parser->diagnostics.count)
        return 0;
    return &parser->diagnostics.items[index];
}

/* ** Append an error-severity recovery diagnostic carrying the author-supplied
** message, help, and label from a (recover ... (on eof ...)) clause. The byte
** slices already live in the grammar (interned/arena-backed) and outlive the
** parse, so they are referenced directly; only the label array is carved from
** the arena. A label with an empty message is still attached so the span is
** reported; an empty help slice is left empty and the CLI omits the help line.
** Companion to tp_diag_error, kept here beside the diagnostic accessors so the
** recovery path in parse.c has a one-line call. Returns 1, or 0 on OOM. */
int tp_diag_recover(tp_diag_list *list, tinyparse_string message,
                    tinyparse_string help, tinyparse_string label_message,
                    uint32_t start_byte, uint32_t end_byte)
{
    tinyparse_diagnostic *diag;
    tinyparse_label      *label;

    if (list->count == list->capacity) {
        uint32_t new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        tinyparse_diagnostic *grown = (tinyparse_diagnostic *)tp_alloc(
            list->arena, new_capacity * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < list->count; i++)
            grown[i] = list->items[i];
        list->items    = grown;
        list->capacity = new_capacity;
    }

    label = (tinyparse_label *)tp_alloc(list->arena, sizeof *label);
    if (label == 0)
        return 0;
    label->start_byte = start_byte;
    label->end_byte   = end_byte;
    label->message    = label_message;
    label->flags      = 0;

    diag = &list->items[list->count++];
    diag->severity    = TINYPARSE_SEVERITY_ERROR;
    diag->code        = tp_text("parse.recover");
    diag->message     = message;
    diag->labels      = label;
    diag->label_count = 1;
    diag->help        = help;
    diag->note        = tp_text("");
    return 1;
}
