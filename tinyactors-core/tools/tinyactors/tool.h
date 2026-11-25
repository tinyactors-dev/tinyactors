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
     * TA_ERROR_NO_MEMORY. The bytes are 8-aligned and live in the arena.
     * Standard input is read once: a later read of it gives the same bytes. */
    ta_status (*read)(void *context, const tool_read *request, ta_bytes *out);
    /* TA_OK; TA_ERROR_INVALID: it could not be written. */
    ta_status (*write)(void *context, const tool_write *request);
    /* Bytes from standard input as they come, into `room` (up to its
     * capacity; room->length set), without waiting for more than one read
     * gives: TA_OK; TA_OK with length 0 at the end of input;
     * TA_ERROR_INVALID: it could not be read. For tinyactors run, whose
     * client waits for each response before it writes more. */
    ta_status (*receive)(void *context, ta_buffer *room);
    /* The build's version: a development build's (0.4.0-dev.<date>.<n>.g<sha>),
     * or empty for the library's, which a release carries. */
    ta_string_view version;
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

/* -f svg: graph's picture, beside the report's three formats. */
enum { TOOL_FORMAT_SVG = TA_REPORT_XML + 1 };

/* An invocation: what every command gets. */
typedef struct tool {
    const tool_platform *platform;
    ta_growable_arena *arena;
    const ta_cli_program *program;
    const ta_cli_parsed *parsed;
    uint32_t format; /* ta_report_format, or TOOL_FORMAT_SVG */
    int answered;    /* the command wrote its answer itself: no report */
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
/* The same in another arena (a request's, which the run server releases). */
void tool_text_over(tool_text *text, ta_growable_arena *arena);
void tool_text_append(tool_text *text, ta_string_view piece);
void tool_text_number(tool_text *text, uint64_t value);
void tool_text_duration(tool_text *text, uint64_t nanoseconds);
ta_string_view tool_text_view(const tool_text *text);

int tool_equal(ta_string_view a, ta_string_view b);

/* ---- inputs (input.c) ------------------------------------------------------------- */

typedef enum tool_input_kind {
    TOOL_INPUT_IMAGE = 0,    /* a compiled image (.tai): bytes starting with "TAI1" */
    TOOL_INPUT_CHART = 1,    /* SCXML: the root element is <scxml> */
    TOOL_INPUT_MANIFEST = 2, /* a system manifest: the root element is <system> */
    TOOL_INPUT_SNAPSHOT = 3, /* a whole system, ta_host_snapshot_pack's: "tinyactors-snapshot " */
    TOOL_INPUT_BLOB = 4,     /* a hibernation ("TAHB") or export ("TAEX") blob */
    TOOL_INPUT_HOST_SNAPSHOT = 5 /* ta_host_snapshot's alone ("TAHS"): no images */
} tool_input_kind;

/* What `bytes` are, by their first bytes (never by a file's name): a
 * tool_input_kind, or TA_NIL for none of them. */
uint32_t tool_input_kind_of(ta_bytes bytes);

/* The image the inputs make: an image read as it is, a manifest compiled,
 * or charts compiled together (the first the default boot chart). */
typedef struct tool_image {
    uint32_t kind;    /* tool_input_kind of the inputs */
    uint32_t segment; /* a snapshot's: the segment whose image this is */
    ta_image image; /* opened: valid when `opened` */
    int opened;
    ta_bytes bytes; /* the image's bytes, 8-aligned, in the arena */
    uint32_t diagnostic_count;
    const ta_diagnostic *diagnostics; /* the compile's, copied into the arena */
    ta_status status; /* TA_OK; the compile's or the open's refusal */
    /* Why the inputs make no image when no diagnostic says it (a file that
     * cannot be read, inputs that do not go together; empty otherwise), and
     * the exit status a command gives for the refusal (tool_exit). */
    ta_string_view complaint;
    uint32_t exit;
    uint32_t pad;
} tool_image;

typedef struct tool_inputs {
    uint32_t count;
    const ta_string_view *paths;
    uint32_t flags;   /* TA_COMPILE_* for XML inputs */
    uint32_t segment; /* a snapshot's segment whose image is read; TA_NIL: its newest */
} tool_inputs;

/* Inputs to read, and the arena they are read and compiled into: the
 * invocation's for a command; a request's for the run server, released
 * once the image is added, so that each run.addImage leaves nothing behind. */
typedef struct tool_image_request {
    tool_inputs inputs;
    ta_growable_arena *arena;
} tool_image_request;
/* Read and make the image (diagnostics kept), all of it in the request's
 * arena, telling nothing. TA_OK: out->opened. Otherwise out->status is the
 * refusal and out->exit the exit status a command gives for it; the reason
 * is in out->diagnostics (a chart that does not compile, an image that does
 * not verify) or out->complaint (a file that cannot be read, inputs that do
 * not go together). A snapshot gives the image of one of its segments
 * (inputs.segment), each of its images verified. */
ta_status tool_image_make(tool *t, const tool_image_request *request, tool_image *out);
/* tool_image_make into the invocation's arena, then telling: TA_OK with
 * t->exit untouched and out->opened. Otherwise the complaint was told
 * (tool_complain; a command reports the diagnostics itself, as tool_open
 * does) and t->exit set. */
ta_status tool_load(tool *t, tool_inputs inputs, tool_image *out);

/* Sources to compile, already read: charts, or one manifest. */
typedef struct tool_compilation {
    uint32_t kind;  /* TOOL_INPUT_CHART or TOOL_INPUT_MANIFEST */
    uint32_t flags; /* TA_COMPILE_* */
    ta_source_list sources;
    ta_growable_arena *arena; /* what the resolver reads into, and the result is kept in */
    /* NULL: a src is a file, relative to the file naming it, then in each -I
     * directory. Otherwise a src is one of these documents, by its name
     * (charts given as text, as run.addImage's sources). */
    const ta_source_list *documents;
} tool_compilation;
/* Compile through the command's resolver into out->bytes and
 * out->diagnostics, both in the compilation's arena; out->status is the
 * compile's. TA_ERROR_NO_MEMORY: what it made could not be kept. Nothing is
 * told. */
ta_status tool_compile_sources(tool *t, const tool_compilation *compilation, tool_image *out);

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
typedef enum tool_selection_flags {
    /* a name no chart of this image has is no error: diff selects by name
     * across two images, and says itself when neither has it */
    TOOL_SELECT_NAMES_ELSEWHERE = 1u << 0
} tool_selection_flags;
typedef struct tool_selection {
    const tool_image *image;
    uint32_t flags; /* tool_selection_flags */
} tool_selection;
ta_status tool_select_charts(tool *t, tool_selection selection, tool_charts *out);
/* The options every command that reads inputs starts its table with, and
 * those after them in each command's table (program.c). */
typedef enum tool_input_option {
    TOOL_OPTION_WARNINGS_AS_ERRORS = 0, /* -W: a compiled input's warnings refuse it */
    TOOL_OPTION_INCLUDE = 1,            /* -I DIR, repeatable: where a src is looked for next */
    TOOL_OPTION_CHART = 2,              /* --chart NAME, repeatable (not check, not compile) */
    TOOL_OPTION_SEGMENT = 3             /* --segment N: a snapshot's segment (not check, not compile, not fmt) */
} tool_input_option;
typedef enum tool_compile_option {
    TOOL_COMPILE_OUTPUT = 2, /* -o FILE */
    TOOL_COMPILE_EMIT = 3,   /* --emit image|c */
    TOOL_COMPILE_SYMBOL = 4  /* --symbol NAME: the C array's name */
} tool_compile_option;
typedef enum tool_emit { TOOL_EMIT_IMAGE = 0, TOOL_EMIT_C = 1 } tool_emit;
typedef enum tool_expressions_option { TOOL_EXPRESSIONS_KIND = 4 } tool_expressions_option;
typedef enum tool_diff_option {
    TOOL_DIFF_EXIT_CODE = 4, /* --exit-code: 1 when they differ (only breaking ones with --breaking) */
    TOOL_DIFF_BREAKING = 5,  /* --breaking: only the differences that break running actors */
    TOOL_DIFF_ALL = 6        /* --all: unchanged charts too */
} tool_diff_option;
/* fmt's: -I at TOOL_OPTION_INCLUDE, as every command that reads charts */
typedef enum tool_canonical_option {
    TOOL_CANONICAL_WRITE = 0, /* -w: rewrite the files in place */
    TOOL_CANONICAL_CHECK = 2  /* --check: write nothing; 1 when a file is not canonical */
} tool_canonical_option;

typedef enum tool_lint_option {
    TOOL_LINT_DENY = 4, /* --deny RULE, repeatable ("all"): its findings exit 1 */
    TOOL_LINT_ALLOW = 5 /* --allow RULE, repeatable ("all"): its findings are not reported */
} tool_lint_option;
typedef enum tool_path_option {
    TOOL_PATH_TO = 4,         /* --to STATE (required) */
    TOOL_PATH_FROM = 5,       /* --from STATE */
    TOOL_PATH_UNREACHABLE = 6 /* --unreachable: succeed when no path exists */
} tool_path_option;
typedef enum tool_graph_option {
    TOOL_GRAPH_DIRECTION = 4, /* --direction down|right */
    TOOL_GRAPH_THEME = 5,     /* --theme auto|light|dark */
    TOOL_GRAPH_HIGHLIGHT = 6, /* --highlight STATE, repeatable */
    TOOL_GRAPH_PATH = 7,      /* --path: mark the path to --to */
    TOOL_GRAPH_TO = 8,        /* --to STATE */
    TOOL_GRAPH_FROM = 9,      /* --from STATE */
    TOOL_GRAPH_DEPTH = 10,    /* --depth N: fold the containers nested deeper */
    TOOL_GRAPH_FOLD = 11      /* --fold STATE, repeatable */
} tool_graph_option;
typedef enum tool_graph_direction { TOOL_GRAPH_DOWN = 0, TOOL_GRAPH_RIGHT = 1 } tool_graph_direction;
typedef enum tool_graph_theme {
    TOOL_GRAPH_THEME_AUTO = 0,
    TOOL_GRAPH_THEME_LIGHT = 1,
    TOOL_GRAPH_THEME_DARK = 2
} tool_graph_theme;
typedef enum tool_actors_option {
    TOOL_ACTORS_SESSION = 4 /* --session N, repeatable: these sessions only */
} tool_actors_option;
/* run's: -W and -I at their places, as every command that reads charts */
typedef enum tool_run_option {
    TOOL_RUN_CONFIG = 2,       /* --config FILE: the host config as JSON */
    TOOL_RUN_SYSTEM = 3,       /* --system ID: the config's systemID */
    TOOL_RUN_IO_PROCESSOR = 4, /* --io-processor TYPE[=LOCATION], repeatable */
    TOOL_RUN_BOOT = 5,         /* --boot: the image's boot actors */
    TOOL_RUN_WALL_START = 6,   /* --wall-start NS */
    TOOL_RUN_SEED = 7,         /* --seed N: Math.random's */
    TOOL_RUN_MAX_OPS = 8,      /* --max-ops N */
    TOOL_RUN_MEMORY_LIMIT = 9, /* --memory-limit BYTES */
    TOOL_RUN_REALM = 10,       /* --realm auto|isolated */
    TOOL_RUN_MAX_MESSAGE = 11, /* --max-message BYTES */
    TOOL_RUN_SCHEMA = 12       /* --schema: write the protocol's JSON Schema */
} tool_run_option;

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
    uint32_t flags; /* tool_selection_flags */
} tool_open_request;
ta_status tool_open(tool *t, tool_open_request request, tool_opened *out);

/* The id of state `state` of `chart` ("/scxml" for the root, "" for none). */
ta_string_view tool_state_id(const ta_image_view *view, const ta_chart_record *chart, uint32_t state);
/* A decimal number of the command line: TA_OK, or TA_ERROR_INVALID. */
ta_status tool_decimal(ta_string_view text, uint64_t *out);

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
void tool_lint(tool *t);
void tool_path(tool *t);
void tool_canonical(tool *t);
/* snapshot.c: what the runtime writes, a snapshot or a blob */
void tool_actors(tool *t);
/* run.c: one host served over JSON-RPC 2.0 on standard input and output */
void tool_serve(tool *t);
/* inspect of a snapshot or a blob: 1 when the first input is one (and it
 * was answered), 0 for an image's inputs, which inspect reads itself. */
int tool_inspect_stored(tool *t);

#endif /* TINYACTORS_TOOL_H */
