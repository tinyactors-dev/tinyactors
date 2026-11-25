/* The command line read against an order service's tool: its own options,
 * and commands that check, show and export orders. */
#include <stdlib.h>
#include <string.h>
#include "../harness/harness.h"
#include "cli/arguments.h"

/* The program's options, by index. */
enum { HELP, VERSION, VERBOSE, FORMAT, CONFIG };
/* The commands, by index. */
enum { CHECK, STATUS, EXPORT, REFUND };
/* check's options. */
enum { CHECK_OUTPUT, CHECK_STRICT, CHECK_INCLUDE, CHECK_TAG, CHECK_ALL };
/* status's and export's. */
enum { STATUS_ORDER };
enum { EXPORT_LEVEL };

static const ta_string_view formats[] = {TA_CLI_TEXT("markdown"), TA_CLI_TEXT("json"),
                                         TA_CLI_TEXT("xml")};
static const ta_string_view levels[] = {TA_CLI_TEXT("summary"), TA_CLI_TEXT("lines"),
                                        TA_CLI_TEXT("payments")};

static const ta_cli_option program_options[] = {
    {TA_CLI_TEXT("help"), 'h', TA_CLI_OPTION_FLAG, TA_CLI_OPTION_ANSWERS, 0, NULL, {0}, {0}},
    {TA_CLI_TEXT("version"), 0, TA_CLI_OPTION_FLAG, TA_CLI_OPTION_ANSWERS, 0, NULL, {0}, {0}},
    {TA_CLI_TEXT("verbose"), 'v', TA_CLI_OPTION_FLAG, 0, 0, NULL, {0}, {0}},
    {TA_CLI_TEXT("format"), 'f', TA_CLI_OPTION_CHOICE, 0, 3, formats, TA_CLI_TEXT("FORMAT"), {0}},
    {TA_CLI_TEXT("config"), 'c', TA_CLI_OPTION_VALUE, 0, 0, NULL, TA_CLI_TEXT("FILE"), {0}},
};

static const ta_cli_option check_options[] = {
    {TA_CLI_TEXT("output"), 'o', TA_CLI_OPTION_VALUE, 0, 0, NULL, TA_CLI_TEXT("FILE"), {0}},
    {TA_CLI_TEXT("strict"), 's', TA_CLI_OPTION_FLAG, 0, 0, NULL, {0}, {0}},
    {TA_CLI_TEXT("include"), 'I', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, NULL,
     TA_CLI_TEXT("DIRECTORY"), {0}},
    {TA_CLI_TEXT("tag"), 't', TA_CLI_OPTION_VALUE, 0, 0, NULL, TA_CLI_TEXT("TAG"), {0}},
    {TA_CLI_TEXT("all"), 'a', TA_CLI_OPTION_FLAG, 0, 0, NULL, {0}, {0}},
};

static const ta_cli_option status_options[] = {
    {TA_CLI_TEXT("order"), 0, TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REQUIRED, 0, NULL,
     TA_CLI_TEXT("ID"), {0}},
};

static const ta_cli_option export_options[] = {
    {TA_CLI_TEXT("level"), 'l', TA_CLI_OPTION_CHOICE, 0, 3, levels, TA_CLI_TEXT("LEVEL"), {0}},
};

/* refund's only option has no long name. */
static const ta_cli_option refund_options[] = {
    {{0}, 'r', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REQUIRED, 0, NULL, TA_CLI_TEXT("REASON"), {0}},
};

static const ta_cli_command commands[] = {
    {TA_CLI_TEXT("check"), {0}, {0}, TA_CLI_TEXT("ORDER..."), 1, TA_CLI_UNBOUNDED, 5, check_options, 0, NULL},
    {TA_CLI_TEXT("status"), {0}, {0}, {0}, 0, 0, 1, status_options, 0, NULL},
    {TA_CLI_TEXT("export"), {0}, {0}, TA_CLI_TEXT("ORDER [FILE]"), 1, 2, 1, export_options, 0, NULL},
    {TA_CLI_TEXT("refund"), {0}, {0}, TA_CLI_TEXT("ORDER"), 1, 1, 1, refund_options, 0, NULL},
};

static const ta_cli_program program = {
    TA_CLI_TEXT("orders"), TA_CLI_TEXT("checks and exports orders"), 5, program_options, 4, commands,
};

static void *heap_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align;
    return malloc(size);
}

static void heap_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static const ta_allocator heap = {NULL, heap_alloc, heap_free};

typedef struct fixture {
    ta_growable_arena arena;
    ta_cli_parsed parsed;
} fixture;

static void fixture_init(fixture *fixture) {
    ta_growable_arena_config config = {sizeof config, 0, 256, &heap};
    ta_growable_arena_init(&fixture->arena, &config);
}

static ta_status parse_words(fixture *fixture, const char *const *words, uint32_t count) {
    ta_cli_words line = {count, words, &fixture->arena};
    return ta_cli_parse(&program, line, &fixture->parsed);
}

#define PARSE(fixture, ...)                                                                        \
    parse_words((fixture), (const char *const[]){__VA_ARGS__},                                     \
                (uint32_t)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)))

static int text_is(ta_string_view view, const char *text) {
    return view.length == strlen(text) && (view.length == 0 || memcmp(view.pointer, text, view.length) == 0);
}

static ta_cli_option_id in_program(uint32_t option) {
    ta_cli_option_id id = {TA_CLI_SCOPE_PROGRAM, option};
    return id;
}

static ta_cli_option_id in_command(uint32_t option) {
    ta_cli_option_id id = {TA_CLI_SCOPE_COMMAND, option};
    return id;
}

static const char *last_text(const fixture *fixture, ta_cli_option_id option) {
    static char text[64];
    const ta_cli_value *value = ta_cli_last(&fixture->parsed, option);
    if (!value) return "(none)";
    memcpy(text, value->text.pointer, value->text.length);
    text[value->text.length] = 0;
    return text;
}

/* --name, --name VALUE, --name=VALUE, -n, -n VALUE, -nVALUE. */
static void test_forms(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "--verbose", "check", "--output", "report.md", "--tag=priority", "A-1"), TA_OK);
    CHECK_EQUAL(f.parsed.command, CHECK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 1);
    CHECK(strcmp(last_text(&f, in_command(CHECK_OUTPUT)), "report.md") == 0);
    CHECK_EQUAL(ta_cli_last(&f.parsed, in_command(CHECK_OUTPUT))->word, 2);
    CHECK(strcmp(last_text(&f, in_command(CHECK_TAG)), "priority") == 0);
    CHECK_EQUAL(f.parsed.argument_count, 1);
    CHECK(text_is(f.parsed.arguments[0], "A-1"));
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_NONE);

    CHECK_EQUAL(PARSE(&f, "-v", "check", "-o", "report.md", "-tpriority", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 1);
    CHECK(strcmp(last_text(&f, in_command(CHECK_OUTPUT)), "report.md") == 0);
    CHECK(strcmp(last_text(&f, in_command(CHECK_TAG)), "priority") == 0);
    CHECK_EQUAL(f.parsed.value_count, 3);
    CHECK_EQUAL(f.parsed.values[0].option.scope, TA_CLI_SCOPE_PROGRAM);
    CHECK_EQUAL(f.parsed.values[1].option.option, CHECK_OUTPUT);
    CHECK_EQUAL(f.parsed.values[2].option.option, CHECK_TAG);

    /* A value is the next word, whatever it looks like; '=' may be empty. */
    CHECK_EQUAL(PARSE(&f, "check", "--output", "--strict", "--tag=", "A-1"), TA_OK);
    CHECK(strcmp(last_text(&f, in_command(CHECK_OUTPUT)), "--strict") == 0);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_STRICT)), 0);
    CHECK(strcmp(last_text(&f, in_command(CHECK_TAG)), "") == 0);

    /* A value option at the end of the line has none. */
    CHECK_EQUAL(PARSE(&f, "check", "A-1", "--output"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_MISSING_VALUE);
    CHECK_EQUAL(f.parsed.error.word, 2);
    CHECK(text_is(f.parsed.error.text, "--output"));
    CHECK_EQUAL(f.parsed.error.option.option, CHECK_OUTPUT);
    CHECK_EQUAL(PARSE(&f, "check", "A-1", "-o"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_MISSING_VALUE);
    CHECK(text_is(f.parsed.error.text, "-o"));

    /* A flag takes no value. */
    CHECK_EQUAL(PARSE(&f, "check", "--strict=yes", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNEXPECTED_VALUE);
    CHECK_EQUAL(f.parsed.error.word, 1);
    CHECK(text_is(f.parsed.error.text, "--strict"));

    /* Long names match whole, never by prefix. */
    CHECK_EQUAL(PARSE(&f, "check", "--out", "x", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_OPTION);
    CHECK(text_is(f.parsed.error.text, "--out"));
    ta_growable_arena_free(&f.arena);
}

static void test_clusters(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "check", "-vsa", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 1);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_STRICT)), 1);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_ALL)), 1);
    CHECK_EQUAL(ta_cli_last(&f.parsed, in_command(CHECK_ALL))->word, 1);

    /* The last letter takes a value: from the next word or the rest. */
    CHECK_EQUAL(PARSE(&f, "check", "-sao", "report.md", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_ALL)), 1);
    CHECK(strcmp(last_text(&f, in_command(CHECK_OUTPUT)), "report.md") == 0);
    CHECK_EQUAL(f.parsed.argument_count, 1);
    CHECK_EQUAL(PARSE(&f, "check", "-soreport.md", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_STRICT)), 1);
    CHECK(strcmp(last_text(&f, in_command(CHECK_OUTPUT)), "report.md") == 0);

    /* A flag given twice is counted. */
    CHECK_EQUAL(PARSE(&f, "-vv", "check", "-v", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 3);

    /* An unknown letter inside a cluster is named alone. */
    CHECK_EQUAL(PARSE(&f, "check", "-sxa", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_OPTION);
    CHECK(text_is(f.parsed.error.text, "-x"));
    CHECK_EQUAL(f.parsed.error.word, 1);
    CHECK_EQUAL(f.parsed.value_count, 1); /* -s, read before the error */
    ta_growable_arena_free(&f.arena);
}

/* `--` ends options; a lone `-` is an argument. */
static void test_end_of_options(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "check", "-s", "--", "-v", "--strict", "--", "-"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_STRICT)), 1);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 0);
    CHECK_EQUAL(f.parsed.argument_count, 4);
    CHECK(text_is(f.parsed.arguments[0], "-v"));
    CHECK(text_is(f.parsed.arguments[1], "--strict"));
    CHECK(text_is(f.parsed.arguments[2], "--"));
    CHECK(text_is(f.parsed.arguments[3], "-"));

    CHECK_EQUAL(PARSE(&f, "check", "-", "-s"), TA_OK);
    CHECK_EQUAL(f.parsed.argument_count, 1);
    CHECK(text_is(f.parsed.arguments[0], "-"));
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_STRICT)), 1);

    /* After `--` the first word is still the command when none came. */
    CHECK_EQUAL(PARSE(&f, "-v", "--", "check", "--help"), TA_OK);
    CHECK_EQUAL(f.parsed.command, CHECK);
    CHECK_EQUAL(f.parsed.argument_count, 1);
    CHECK(text_is(f.parsed.arguments[0], "--help"));
    ta_growable_arena_free(&f.arena);
}

/* Options and arguments interleave after the command; program options are
 * valid before and after it. */
static void test_interleaving(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "--config", "orders.toml", "check", "A-1", "--strict", "A-2", "-v",
                      "--format", "json", "A-3"),
                TA_OK);
    CHECK_EQUAL(f.parsed.command, CHECK);
    CHECK_EQUAL(f.parsed.argument_count, 3);
    CHECK(text_is(f.parsed.arguments[0], "A-1"));
    CHECK(text_is(f.parsed.arguments[1], "A-2"));
    CHECK(text_is(f.parsed.arguments[2], "A-3"));
    CHECK(strcmp(last_text(&f, in_program(CONFIG)), "orders.toml") == 0);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 1);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_STRICT)), 1);
    CHECK_EQUAL(ta_cli_last(&f.parsed, in_program(FORMAT))->choice, 1);
    CHECK_EQUAL(f.parsed.value_count, 4);
    CHECK_EQUAL(f.parsed.values[0].option.option, CONFIG);
    CHECK_EQUAL(f.parsed.values[1].option.option, CHECK_STRICT);
    CHECK_EQUAL(f.parsed.values[2].option.option, VERBOSE);
    CHECK_EQUAL(f.parsed.values[3].option.option, FORMAT);

    /* A command's option before the command is not yet in scope. */
    CHECK_EQUAL(PARSE(&f, "--strict", "check", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_OPTION);
    CHECK_EQUAL(f.parsed.error.word, 0);
    CHECK_EQUAL(f.parsed.command, TA_NIL);

    /* Many values grow the array past its first capacity. */
    const char *many[42];
    many[0] = "check";
    for (uint32_t index = 1; index < 41; index++)
        many[index] = "-v";
    many[41] = "A-1";
    CHECK_EQUAL(parse_words(&f, many, 42), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 40);
    CHECK_EQUAL(f.parsed.values[39].word, 40);
    ta_growable_arena_free(&f.arena);
}

static void test_repeats(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "check", "-I", "eu", "--include=us", "-Iapac", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_INCLUDE)), 3);
    CHECK(strcmp(last_text(&f, in_command(CHECK_INCLUDE)), "apac") == 0);
    CHECK(ta_cli_last(&f.parsed, in_command(CHECK_TAG)) == NULL);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_TAG)), 0);

    CHECK_EQUAL(PARSE(&f, "check", "--tag", "rush", "A-1", "-t", "gift"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_REPEATED_OPTION);
    CHECK_EQUAL(f.parsed.error.word, 4);
    CHECK_EQUAL(f.parsed.error.option.scope, TA_CLI_SCOPE_COMMAND);
    CHECK_EQUAL(f.parsed.error.option.option, CHECK_TAG);
    CHECK(text_is(f.parsed.error.text, "-t"));
    CHECK_EQUAL(f.parsed.command, CHECK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_command(CHECK_TAG)), 1);
    ta_growable_arena_free(&f.arena);
}

static void test_choices(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "--format=xml", "export", "--level", "payments", "A-1"), TA_OK);
    CHECK_EQUAL(ta_cli_last(&f.parsed, in_program(FORMAT))->choice, 2);
    CHECK_EQUAL(ta_cli_last(&f.parsed, in_command(EXPORT_LEVEL))->choice, 2);
    CHECK(strcmp(last_text(&f, in_command(EXPORT_LEVEL)), "payments") == 0);

    CHECK_EQUAL(PARSE(&f, "export", "-l", "lnies", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_INVALID_CHOICE);
    CHECK_EQUAL(f.parsed.error.word, 2);
    CHECK_EQUAL(f.parsed.error.option.option, EXPORT_LEVEL);
    CHECK(text_is(f.parsed.error.text, "lnies"));
    CHECK(text_is(f.parsed.error.suggestion, "lines"));

    CHECK_EQUAL(PARSE(&f, "--format=jsno", "status", "--order", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_INVALID_CHOICE);
    CHECK_EQUAL(f.parsed.error.word, 0);
    CHECK(text_is(f.parsed.error.suggestion, "json"));

    /* Exact match only: no case folding, no prefix; nothing near. */
    CHECK_EQUAL(PARSE(&f, "-f", "JSON", "status"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_INVALID_CHOICE);
    CHECK_EQUAL(f.parsed.error.suggestion.length, 0);
    CHECK_EQUAL(PARSE(&f, "-fmark", "status"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_INVALID_CHOICE);
    CHECK(text_is(f.parsed.error.text, "mark"));
    CHECK_EQUAL(f.parsed.error.suggestion.length, 0);
    ta_growable_arena_free(&f.arena);
}

static void test_suggestions(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "-v", "chekc", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_COMMAND);
    CHECK_EQUAL(f.parsed.error.word, 1);
    CHECK(text_is(f.parsed.error.text, "chekc"));
    CHECK(text_is(f.parsed.error.suggestion, "check"));
    CHECK_EQUAL(f.parsed.command, TA_NIL);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(VERBOSE)), 1);

    CHECK_EQUAL(PARSE(&f, "stauts"), TA_ERROR_INVALID);
    CHECK(text_is(f.parsed.error.suggestion, "status"));
    CHECK_EQUAL(PARSE(&f, "ship", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_COMMAND);
    CHECK_EQUAL(f.parsed.error.suggestion.length, 0);

    /* An option: the command's names and the program's, with "--". */
    CHECK_EQUAL(PARSE(&f, "check", "--stirct", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_OPTION);
    CHECK(text_is(f.parsed.error.text, "--stirct"));
    CHECK(text_is(f.parsed.error.suggestion, "--strict"));
    CHECK_EQUAL(PARSE(&f, "check", "--verbos=1", "A-1"), TA_ERROR_INVALID);
    CHECK(text_is(f.parsed.error.text, "--verbos"));
    CHECK(text_is(f.parsed.error.suggestion, "--verbose"));
    CHECK_EQUAL(PARSE(&f, "--outptu", "check"), TA_ERROR_INVALID); /* check's, not yet in scope */
    CHECK_EQUAL(f.parsed.error.suggestion.length, 0);
    CHECK_EQUAL(PARSE(&f, "check", "--dry-run", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.suggestion.length, 0);
    CHECK_EQUAL(PARSE(&f, "-x", "check"), TA_ERROR_INVALID);
    CHECK(text_is(f.parsed.error.text, "-x"));
    CHECK_EQUAL(f.parsed.error.suggestion.length, 0);

    /* A tie goes to the first in table order: "tags" is one from "tag"
     * (check's, looked at first) and no nearer to any other. */
    CHECK_EQUAL(PARSE(&f, "check", "--tags", "A-1"), TA_ERROR_INVALID);
    CHECK(text_is(f.parsed.error.suggestion, "--tag"));
    ta_growable_arena_free(&f.arena);
}

static void test_arity(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(parse_words(&f, NULL, 0), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_NO_COMMAND);
    CHECK_EQUAL(f.parsed.error.word, TA_NIL);
    CHECK_EQUAL(PARSE(&f, "-v"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_NO_COMMAND);

    CHECK_EQUAL(PARSE(&f, "check", "--strict"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_TOO_FEW_ARGUMENTS);
    CHECK_EQUAL(f.parsed.error.word, TA_NIL);
    CHECK(text_is(f.parsed.error.text, "check"));
    CHECK_EQUAL(f.parsed.command, CHECK);

    CHECK_EQUAL(PARSE(&f, "export", "A-1", "a.json"), TA_OK);
    CHECK_EQUAL(f.parsed.argument_count, 2);
    CHECK_EQUAL(PARSE(&f, "export", "A-1", "a.json", "-v", "b.json"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_TOO_MANY_ARGUMENTS);
    CHECK_EQUAL(f.parsed.error.word, 4);
    CHECK(text_is(f.parsed.error.text, "b.json"));
    CHECK_EQUAL(f.parsed.argument_count, 2);
    CHECK_EQUAL(PARSE(&f, "status", "--order", "A-1", "A-2"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_TOO_MANY_ARGUMENTS);
    CHECK(text_is(f.parsed.error.text, "A-2"));
    ta_growable_arena_free(&f.arena);
}

static void test_required(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "status", "--order", "A-1"), TA_OK);
    CHECK(strcmp(last_text(&f, in_command(STATUS_ORDER)), "A-1") == 0);

    CHECK_EQUAL(PARSE(&f, "status", "-v"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_MISSING_OPTION);
    CHECK_EQUAL(f.parsed.error.word, TA_NIL);
    CHECK_EQUAL(f.parsed.error.option.scope, TA_CLI_SCOPE_COMMAND);
    CHECK_EQUAL(f.parsed.error.option.option, STATUS_ORDER);
    CHECK(text_is(f.parsed.error.text, "--order"));

    /* An option with no long name is named by its letter. */
    CHECK_EQUAL(PARSE(&f, "refund", "A-1"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_MISSING_OPTION);
    CHECK(text_is(f.parsed.error.text, "-r"));
    CHECK_EQUAL(PARSE(&f, "refund", "-r", "damaged", "A-1"), TA_OK);
    ta_growable_arena_free(&f.arena);
}

/* --help and --version answer by themselves. */
static void test_answers(void) {
    fixture f;
    fixture_init(&f);
    CHECK_EQUAL(PARSE(&f, "--help"), TA_OK);
    CHECK_EQUAL(f.parsed.command, TA_NIL);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(HELP)), 1);
    CHECK_EQUAL(PARSE(&f, "--version"), TA_OK);
    CHECK_EQUAL(PARSE(&f, "check", "-h"), TA_OK); /* too few arguments */
    CHECK_EQUAL(f.parsed.command, CHECK);
    CHECK_EQUAL(PARSE(&f, "status", "--help"), TA_OK); /* --order missing */
    CHECK_EQUAL(PARSE(&f, "-vh", "refund"), TA_OK);

    /* Other errors stand; what came before them is kept. */
    CHECK_EQUAL(PARSE(&f, "--help", "check", "--bogus"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_UNKNOWN_OPTION);
    CHECK_EQUAL(f.parsed.command, CHECK);
    CHECK_EQUAL(ta_cli_count(&f.parsed, in_program(HELP)), 1);
    CHECK_EQUAL(PARSE(&f, "status", "--order", "A-1", "A-2", "--help"), TA_ERROR_INVALID);
    CHECK_EQUAL(f.parsed.error.kind, TA_CLI_ERROR_TOO_MANY_ARGUMENTS);
    ta_growable_arena_free(&f.arena);
}

/* Refusals of the call itself, and an arena that refuses. */
static void test_refusals(void) {
    fixture f;
    fixture_init(&f);
    ta_cli_words line = {1, NULL, &f.arena};
    CHECK_EQUAL(ta_cli_parse(&program, line, &f.parsed), TA_ERROR_INVALID);
    line.words = (const char *const[]){"status"};
    CHECK_EQUAL(ta_cli_parse(NULL, line, &f.parsed), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_cli_parse(&program, line, NULL), TA_ERROR_INVALID);

    ta_growable_arena inert;
    ta_growable_arena_init(&inert, NULL);
    ta_cli_words refused = {2, (const char *const[]){"check", "A-1"}, &inert};
    CHECK_EQUAL(ta_cli_parse(&program, refused, &f.parsed), TA_ERROR_NO_MEMORY);
    refused.words = (const char *const[]){"-v", "status"};
    CHECK_EQUAL(ta_cli_parse(&program, refused, &f.parsed), TA_ERROR_NO_MEMORY);
    ta_growable_arena_free(&f.arena);
}

static uint32_t distance(const char *from, const char *to, uint32_t limit) {
    ta_cli_distance query = {ta_string(from), ta_string(to), limit};
    return ta_cli_edit_distance(query);
}

static void test_edit_distance(void) {
    CHECK_EQUAL(distance("kitten", "sitting", 10), 3);
    CHECK_EQUAL(distance("sitting", "kitten", 10), 3);
    CHECK_EQUAL(distance("status", "stauts", 5), 1); /* a transposition */
    CHECK_EQUAL(distance("ab", "ba", 5), 1);
    CHECK_EQUAL(distance("ca", "abc", 5), 3); /* optimal string alignment, not Damerau's full metric */
    CHECK_EQUAL(distance("", "", 0), 0);
    CHECK_EQUAL(distance("", "json", 9), 4);
    CHECK_EQUAL(distance("check", "check", 0), 0);
    CHECK_EQUAL(distance("check", "chekc", 1), 1);
    /* Past the limit: limit + 1. */
    CHECK_EQUAL(distance("kitten", "sitting", 2), 3);
    CHECK_EQUAL(distance("kitten", "sitting", 1), 2);
    CHECK_EQUAL(distance("export", "status", 0), 1);
    CHECK_EQUAL(distance("a", "abcdef", 2), 3); /* the lengths alone */
    CHECK_EQUAL(distance("x", "y", UINT32_MAX), 1);
    /* Longer than the bound (64): never near. */
    char long_name[66];
    memset(long_name, 'a', 65);
    long_name[65] = 0;
    CHECK_EQUAL(distance(long_name, long_name, 3), 4);
    CHECK_EQUAL(distance(long_name, long_name, UINT32_MAX), UINT32_MAX);
    long_name[64] = 0;
    CHECK_EQUAL(distance(long_name, long_name, 3), 0);
}

int main(void) {
    RUN(test_forms);
    RUN(test_clusters);
    RUN(test_end_of_options);
    RUN(test_interleaving);
    RUN(test_repeats);
    RUN(test_choices);
    RUN(test_suggestions);
    RUN(test_arity);
    RUN(test_required);
    RUN(test_answers);
    RUN(test_refusals);
    RUN(test_edit_distance);
    TEST_MAIN_END();
}
