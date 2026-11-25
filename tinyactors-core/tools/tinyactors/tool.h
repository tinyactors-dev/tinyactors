/* tool.h — the tinyactors command, between its two halves.
 *
 * The commands (every file of tools/tinyactors but platform.c) are
 * freestanding: they compile without the C library's headers, take their
 * memory from one arena, and reach files and streams only through the
 * platform below. platform.c is the operating system's half: main(), the
 * allocator behind the arena, reading files and standard input, writing
 * standard output and error, and writing a file so that it is whole or
 * absent. docs/cli.md describes the command; this is how it is built.
 *
 * Lifetimes: one invocation is one arena, made from the platform's
 * allocator when it starts and freed when it ends. Everything the commands
 * make (the parsed line, the files read, compiled images, the report) lives
 * in it, so nothing is freed one by one. */
#ifndef TINYACTORS_TOOL_H
#define TINYACTORS_TOOL_H

#include <tinyactors/arena.h>
#include <tinyactors/compile.h>
#include <tinyactors/image.h>

#include "base/output.h"
#include "cli/arguments.h"
#include "cli/report.h"
#include "image/records.h"

/* ---- the platform ---------------------------------------------------------------- */

typedef enum tool_stream { TOOL_STANDARD_OUTPUT = 1, TOOL_STANDARD_ERROR = 2 } tool_stream;

/* A file to read whole ("-": standard input) into `arena`. */
typedef struct tool_read {
    ta_string_view path;
    ta_growable_arena *arena;
} tool_read;

/* Bytes to write: to a stream, or (path not empty) to a file that is
 * replaced whole or left as it was ("-": standard output). */
typedef struct tool_write {
    uint32_t stream; /* tool_stream, when `path` is empty */
    ta_string_view path;
    ta_bytes bytes;
} tool_write;

typedef struct tool_platform {
    void *context;
    ta_allocator allocator; /* what the invocation's arena grows by */
    /* TA_OK; TA_ERROR_INVALID: no such file, or it cannot be read;
     * TA_ERROR_NO_MEMORY. The bytes are 8-aligned and live in the arena. */
    ta_status (*read)(void *context, const tool_read *request, ta_bytes *out);
    /* TA_OK; TA_ERROR_INVALID: it could not be written. */
    ta_status (*write)(void *context, const tool_write *request);
} tool_platform;

typedef struct tool_command_line {
    uint32_t count;
    const char *const *words; /* argv + 1 */
} tool_command_line;

/* Exit statuses (docs/cli.md, "Exit status"). */
typedef enum tool_exit {
    TOOL_EXIT_OK = 0,
    TOOL_EXIT_REFUSED = 1, /* an input was refused: a chart did not compile, an image did not
                              verify, or differences with diff --exit-code */
    TOOL_EXIT_USAGE = 2,   /* the command line was not understood */
    TOOL_EXIT_FAILURE = 3  /* a file could not be read or written, or memory ran out */
} tool_exit;

/* Run one invocation: the exit status. */
int tool_main(const tool_platform *platform, tool_command_line line);

/* ---- what the commands share ------------------------------------------------------ */

/* The program's own options (program.c). */
typedef enum tool_program_option {
    TOOL_OPTION_FORMAT = 0,
    TOOL_OPTION_HELP,
    TOOL_OPTION_VERSION,
    TOOL_PROGRAM_OPTION_COUNT
} tool_program_option;

/* An invocation: what every command gets. */
typedef struct tool {
    const tool_platform *platform;
    ta_growable_arena *arena;
    const ta_cli_program *program;
    const ta_cli_parsed *parsed;
    uint32_t format; /* ta_report_format */
    ta_report report;
    /* where the report goes: standard output, or standard error when the
     * command's own output takes standard output (compile -o -) */
    uint32_t report_stream;
    int exit; /* tool_exit: what the command decided */
} tool;

/* A command: fills t->report and sets t->exit. */
typedef void (*tool_run)(tool *t);

/* The command's own option, by its index in the command's table. */
uint32_t tool_count(const tool *t, uint32_t option);
const ta_cli_value *tool_last(const tool *t, uint32_t option);

/* A message for standard error, outside the report (a usage error, a file
 * that could not be read): "tinyactors: <text>\n". */
void tool_complain(tool *t, ta_string_view text);

/* Text built in the arena: ta_output growing through the arena's
 * allocator. tool_text_view gives what was written (empty after a
 * failure). */
typedef struct tool_text {
    ta_output output;
    ta_allocator allocator;
} tool_text;
void tool_text_init(tool *t, tool_text *text);
void tool_text_append(tool_text *text, ta_string_view piece);
void tool_text_number(tool_text *text, uint64_t value);
ta_string_view tool_text_view(const tool_text *text);

int tool_equal(ta_string_view a, ta_string_view b);

/* ---- inputs (input.c) ------------------------------------------------------------- */

typedef enum tool_input_kind {
    TOOL_INPUT_IMAGE = 0,   /* a compiled image (.tai): bytes starting with "TAI1" */
    TOOL_INPUT_CHART = 1,   /* SCXML: the root element is <scxml> */
    TOOL_INPUT_MANIFEST = 2 /* a system manifest: the root element is <system> */
} tool_input_kind;

/* The image the inputs make: an image read as it is, a manifest compiled,
 * or charts compiled together (the first the default boot chart). */
typedef struct tool_image {
    uint32_t kind;  /* tool_input_kind of the inputs */
    ta_image image; /* opened: valid when `opened` */
    int opened;
    ta_bytes bytes; /* the image's bytes, 8-aligned, in the arena */
    uint32_t diagnostic_count;
    const ta_diagnostic *diagnostics; /* the compile's, copied into the arena */
    ta_status status; /* TA_OK; the compile's or the open's refusal */
} tool_image;

typedef struct tool_inputs {
    uint32_t count;
    const ta_string_view *paths;
    uint32_t flags; /* TA_COMPILE_* for XML inputs */
} tool_inputs;

/* Read and make the image (diagnostics kept). TA_OK: t->exit untouched and
 * out->opened. Otherwise the reason was told (tool_complain for a file,
 * the report for diagnostics) and t->exit set. */
ta_status tool_load(tool *t, tool_inputs inputs, tool_image *out);

/* The inputs' diagnostics as a list member "diagnostics" of the report's
 * open object. */
void tool_report_diagnostics(tool *t, const tool_image *image);

/* The charts a command looks at: those named with --chart, or all.
 * `selected` has one entry per chart of the image (1: look at it).
 * TA_ERROR_INVALID: a name matches no chart (told, t->exit set). */
typedef struct tool_charts {
    uint32_t count;
    const uint8_t *selected;
} tool_charts;
ta_status tool_select_charts(tool *t, const tool_image *image, tool_charts *out);
/* The options every command that reads inputs starts its table with, and
 * those after them in each command's table (program.c). */
typedef enum tool_input_option {
    TOOL_OPTION_WARNINGS_AS_ERRORS = 0, /* -W: a compiled input's warnings refuse it */
    TOOL_OPTION_CHART = 1               /* --chart NAME, repeatable (not check, not compile) */
} tool_input_option;
typedef enum tool_compile_option {
    TOOL_COMPILE_OUTPUT = 1, /* -o FILE */
    TOOL_COMPILE_EMIT = 2,   /* --emit image|c */
    TOOL_COMPILE_SYMBOL = 3  /* --symbol NAME: the C array's name */
} tool_compile_option;
typedef enum tool_emit { TOOL_EMIT_IMAGE = 0, TOOL_EMIT_C = 1 } tool_emit;
typedef enum tool_expressions_option { TOOL_EXPRESSIONS_KIND = 2 } tool_expressions_option;
typedef enum tool_diff_option { TOOL_DIFF_EXIT_CODE = 2 } tool_diff_option;

/* What a command that looks into an image starts with: its arguments as
 * inputs (-W read), the image made from them, and the charts --chart
 * selects. TA_OK: the image is open. Otherwise the reason is told: for a
 * refused input the report holds its diagnostics, and t->exit is set. A
 * command that takes two images (diff) gives each its own path. */
typedef struct tool_opened {
    tool_image image;
    tool_charts charts;
} tool_opened;
typedef struct tool_open_request {
    uint32_t count;
    const ta_string_view *paths;
} tool_open_request;
ta_status tool_open(tool *t, tool_open_request request, tool_opened *out);

/* Names for what the image holds. */
ta_string_view tool_state_kind_name(uint32_t kind);
ta_string_view tool_expression_kind_name(uint32_t kind);
ta_string_view tool_opcode_name(uint32_t opcode);
/* A hash as 16 hexadecimal digits, in the arena. */
ta_string_view tool_hexadecimal(tool *t, uint64_t value);

/* ---- the commands ------------------------------------------------------------------ */

void tool_compile(tool *t);
void tool_check(tool *t);
void tool_inspect(tool *t);
void tool_states(tool *t);
void tool_transitions(tool *t);
void tool_events(tool *t);
void tool_data(tool *t);
void tool_expressions(tool *t);
void tool_disassemble(tool *t);
void tool_graph(tool *t);
void tool_diff(tool *t);

#endif /* TINYACTORS_TOOL_H */
