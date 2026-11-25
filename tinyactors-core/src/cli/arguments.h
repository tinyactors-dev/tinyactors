/* cli/arguments.h — a command line read against a program's description.
 *
 * Git's shape, GNU's options: `program [options] <command> [options]
 * [arguments]`. A program describes its own options (valid anywhere on the
 * line, before or after the command) and its commands, each with its
 * options and how many arguments it takes. ta_cli_parse reads the words of
 * a command line against that description and says which command was
 * given, every option in the order it came, the arguments, or the first
 * error and what was probably meant.
 *
 * Options:
 *   --name                a flag
 *   --name VALUE          an option's value, the next word ...
 *   --name=VALUE          ... or after '='
 *   -n                    a flag by its short name
 *   -n VALUE, -nVALUE     a value by its short name
 *   -abc                  flags a, b and c; the last may take a value (-abo FILE)
 *   --                    every word after it is an argument
 *   -                     an argument (stdin or stdout, as the command says)
 * Options and arguments may interleave after the command. Long names are
 * matched whole, never by a prefix: a prefix that matches today may be
 * ambiguous tomorrow.
 *
 * Freestanding: no C library. What the parse makes (its values and
 * arguments) lives in the arena the caller gives, as long as the arena and
 * the words do; the words are never copied, so a value or an argument is a
 * view into them. Everything a description holds is borrowed from it. */
#ifndef TINYACTORS_CLI_ARGUMENTS_H
#define TINYACTORS_CLI_ARGUMENTS_H

#include <tinyactors/arena.h>
#include <tinyactors/types.h>

/* A view of a string literal, for the description tables. */
#define TA_CLI_TEXT(literal) {(literal), (uint32_t)(sizeof(literal) - 1u)}

typedef enum ta_cli_option_kind {
    TA_CLI_OPTION_FLAG = 0,  /* takes no value; given more than once, counted */
    TA_CLI_OPTION_VALUE = 1, /* takes one value, any text */
    TA_CLI_OPTION_CHOICE = 2 /* takes one of `choices` */
} ta_cli_option_kind;

typedef enum ta_cli_option_flags {
    TA_CLI_OPTION_REPEATABLE = 1u << 0, /* a value option given more than once keeps each value */
    TA_CLI_OPTION_REQUIRED = 1u << 1,   /* the command is refused without it */
    /* it answers by itself (--help, --version): when it is given, a
     * missing command, argument or required option is no error */
    TA_CLI_OPTION_ANSWERS = 1u << 2
} ta_cli_option_flags;

typedef struct ta_cli_option {
    ta_string_view name;       /* the long name, without "--" */
    uint32_t short_name;       /* an ASCII letter or digit, or 0 */
    uint32_t kind;             /* ta_cli_option_kind */
    uint32_t flags;            /* ta_cli_option_flags */
    uint32_t choice_count;     /* TA_CLI_OPTION_CHOICE: how many `choices` */
    const ta_string_view *choices;
    ta_string_view value_name; /* how help names the value: "FILE" */
    ta_string_view help;       /* one line */
} ta_cli_option;

typedef enum ta_cli_arity {
    TA_CLI_UNBOUNDED = 0x7FFFFFFF /* a command's maximum: any number of arguments */
} ta_cli_arity;

typedef struct ta_cli_command {
    ta_string_view name;
    ta_string_view summary;     /* one line */
    ta_string_view description; /* paragraphs, for help */
    ta_string_view arguments;   /* how help names them: "INPUT..." */
    uint32_t minimum, maximum;  /* how many arguments (maximum TA_CLI_UNBOUNDED) */
    uint32_t option_count;
    const ta_cli_option *options;
    uint32_t example_count;
    const ta_string_view *examples; /* command lines, without the program's name */
} ta_cli_command;

typedef struct ta_cli_program {
    ta_string_view name;
    ta_string_view summary;
    uint32_t option_count; /* the program's own, valid with every command */
    const ta_cli_option *options;
    uint32_t command_count;
    const ta_cli_command *commands;
} ta_cli_program;

/* Which table an option is in: the program's or the given command's. */
typedef enum ta_cli_scope { TA_CLI_SCOPE_PROGRAM = 0, TA_CLI_SCOPE_COMMAND = 1 } ta_cli_scope;

typedef struct ta_cli_option_id {
    uint32_t scope;  /* ta_cli_scope */
    uint32_t option; /* its index in that table */
} ta_cli_option_id;

/* One option as it was given. */
typedef struct ta_cli_value {
    ta_cli_option_id option;
    uint32_t choice;     /* TA_CLI_OPTION_CHOICE: the index in `choices`; else 0 */
    uint32_t word;       /* the index of the word it came from */
    ta_string_view text; /* the value; empty for a flag */
} ta_cli_value;

typedef enum ta_cli_error_kind {
    TA_CLI_ERROR_NONE = 0,
    TA_CLI_ERROR_NO_COMMAND,        /* no command was given */
    TA_CLI_ERROR_UNKNOWN_COMMAND,   /* `text` is not a command */
    TA_CLI_ERROR_UNKNOWN_OPTION,    /* `text` is not an option here */
    TA_CLI_ERROR_MISSING_VALUE,     /* option `text` needs a value, and the line ended */
    TA_CLI_ERROR_UNEXPECTED_VALUE,  /* flag `text` was given a value (--flag=x) */
    TA_CLI_ERROR_INVALID_CHOICE,    /* `text` is none of the option's choices */
    TA_CLI_ERROR_REPEATED_OPTION,   /* a value option that is not repeatable, given again */
    TA_CLI_ERROR_MISSING_OPTION,    /* a required option was not given */
    TA_CLI_ERROR_TOO_FEW_ARGUMENTS, /* fewer than the command's minimum */
    TA_CLI_ERROR_TOO_MANY_ARGUMENTS /* `text` is one past the command's maximum */
} ta_cli_error_kind;

typedef struct ta_cli_error {
    uint32_t kind;             /* ta_cli_error_kind */
    uint32_t word;             /* the word at fault, TA_NIL when none (a missing option) */
    ta_cli_option_id option;   /* the option concerned, when there is one */
    ta_string_view text;       /* the word, or the name, at fault */
    ta_string_view suggestion; /* the nearest command, option or choice; empty when none is near */
} ta_cli_error;

typedef struct ta_cli_parsed {
    uint32_t command;           /* its index in the program's commands; TA_NIL when none was given */
    uint32_t value_count;
    const ta_cli_value *values; /* in the order they were given */
    uint32_t argument_count;
    const ta_string_view *arguments;
    ta_cli_error error;         /* kind TA_CLI_ERROR_NONE when the line was read */
} ta_cli_parsed;

typedef struct ta_cli_words {
    uint32_t count;
    const char *const *words; /* NUL-terminated, without the program's own name (argv + 1) */
    ta_growable_arena *arena; /* what the parse makes */
} ta_cli_words;

/* Read `words` against `program` into `out`. TA_OK: the line was read.
 * TA_ERROR_INVALID: it was not, and out->error says why (out->command and
 * the values read before the error are filled in, so a caller can still see
 * --help). TA_ERROR_NO_MEMORY: the arena refused. */
ta_status ta_cli_parse(const ta_cli_program *program, ta_cli_words words, ta_cli_parsed *out);

/* How many times the option was given (a flag's count, a value option's
 * values). */
uint32_t ta_cli_count(const ta_cli_parsed *parsed, ta_cli_option_id option);
/* The last time the option was given, NULL when never. */
const ta_cli_value *ta_cli_last(const ta_cli_parsed *parsed, ta_cli_option_id option);

/* The edit distance between two names, at most `limit` (anything farther
 * is `limit` + 1): what "did you mean" is chosen by. */
typedef struct ta_cli_distance {
    ta_string_view from, to;
    uint32_t limit;
} ta_cli_distance;
uint32_t ta_cli_edit_distance(ta_cli_distance distance);

#endif /* TINYACTORS_CLI_ARGUMENTS_H */
