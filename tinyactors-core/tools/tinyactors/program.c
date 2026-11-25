/* program.c — the tinyactors command's description, its dispatch, help and
 * version, and what the commands share (tool.h). Freestanding. */
#include "tool.h"

#include "base/format.h"
#include "base/memory.h"

#define T TA_CLI_TEXT

/* ---- the description ------------------------------------------------------------- */

static const ta_string_view formats[] = {T("markdown"), T("json"), T("xml"), T("svg")};

static const ta_cli_option program_options[TOOL_PROGRAM_OPTION_COUNT] = {
    [TOOL_OPTION_FORMAT] = {T("format"), 'f', TA_CLI_OPTION_CHOICE, 0, 4, formats, T("FORMAT"),
                            T("write the answer as markdown (the default), json or xml; graph also as svg")},
    [TOOL_OPTION_HELP] = {T("help"), 'h', TA_CLI_OPTION_FLAG, TA_CLI_OPTION_ANSWERS, 0, 0, T(""),
                          T("describe the command, or the program")},
    [TOOL_OPTION_VERSION] = {T("version"), 0, TA_CLI_OPTION_FLAG, TA_CLI_OPTION_ANSWERS, 0, 0,
                             T(""), T("print the version, as the version command does")},
};

#define WARNINGS_AS_ERRORS                                                                         \
    {T("warnings-as-errors"), 'W', TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),                            \
     T("refuse a chart that compiles with a warning")}
#define INCLUDE                                                                                    \
    {T("include"), 'I', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("DIR"),            \
     T("look for a src here when it is not beside the file naming it; repeat it for more, "      \
       "searched in order")}
#define CHART                                                                                      \
    {T("chart"), 'c', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("NAME"),             \
     T("look at this chart only (by its name); repeat it for more")}

static const ta_cli_option check_options[] = {WARNINGS_AS_ERRORS, INCLUDE};
static const ta_cli_option image_options[] = {WARNINGS_AS_ERRORS, INCLUDE, CHART};

static const ta_string_view emits[] = {[TOOL_EMIT_IMAGE] = T("image"), [TOOL_EMIT_C] = T("c")};
static const ta_cli_option compile_options[] = {
    [TOOL_OPTION_WARNINGS_AS_ERRORS] = WARNINGS_AS_ERRORS,
    [TOOL_OPTION_INCLUDE] = INCLUDE,
    [TOOL_COMPILE_OUTPUT] = {T("output"), 'o', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REQUIRED, 0, 0,
                             T("FILE"), T("where the image goes (- for standard output)")},
    [TOOL_COMPILE_EMIT] = {T("emit"), 'e', TA_CLI_OPTION_CHOICE, 0, 2, emits, T("KIND"),
                           T("write the image's bytes (image, the default) or a C array (c)")},
    [TOOL_COMPILE_SYMBOL] = {T("symbol"), 's', TA_CLI_OPTION_VALUE, 0, 0, 0, T("NAME"),
                             T("the C array's name with --emit c (default: tinyactors_image)")},
};

static const ta_string_view expression_kinds[] = {
    T("condition"), T("value"), T("location"), T("script"),
    T("string"),    T("content"), T("item"),   T("global-script")};
static const ta_cli_option expressions_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART,
    [TOOL_EXPRESSIONS_KIND] = {T("kind"), 'k', TA_CLI_OPTION_CHOICE, TA_CLI_OPTION_REPEATABLE, 8,
                               expression_kinds, T("KIND"),
                               T("only expressions of this kind; repeat it for more")},
};
static const ta_cli_option diff_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART,
    [TOOL_DIFF_EXIT_CODE] = {T("exit-code"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                             T("exit with 1 when the two differ (with --breaking: when a change breaks "
                               "running actors), as git diff --exit-code does")},
    [TOOL_DIFF_BREAKING] = {T("breaking"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                            T("list only the changes that break actors running the old version")},
    [TOOL_DIFF_ALL] = {T("all"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                       T("list the charts that did not change too")},
};
static const ta_cli_option lint_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART,
    [TOOL_LINT_DENY] = {T("deny"), 'd', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("RULE"),
                        T("exit with 1 when this rule finds something (all: any rule); repeat it for more")},
    [TOOL_LINT_ALLOW] = {T("allow"), 'a', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("RULE"),
                         T("do not report this rule's findings (all: none); repeat it for more")},
};
static const ta_cli_option path_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART,
    [TOOL_PATH_TO] = {T("to"), 't', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REQUIRED, 0, 0, T("STATE"),
                      T("the state the last configuration holds")},
    [TOOL_PATH_FROM] = {T("from"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("STATE"),
                        T("start in this state, entered with its defaults, instead of the initial configuration")},
    [TOOL_PATH_UNREACHABLE] = {T("unreachable"), 'u', TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                               T("succeed when no path exists, and exit with 1 when one does")},
};

static const ta_cli_option canonical_options[] = {
    [TOOL_CANONICAL_WRITE] = {T("write"), 'w', TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                              T("rewrite each file in place, whole or not at all")},
    [TOOL_OPTION_INCLUDE] = INCLUDE,
    [TOOL_CANONICAL_CHECK] = {T("check"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                              T("write nothing; exit with 1 when a file is not in canonical form")},
};

#define OPTIONS(table) (uint32_t)(sizeof(table) / sizeof(table)[0]), (table)
#define EXAMPLES(table) (uint32_t)(sizeof(table) / sizeof(table)[0]), (table)

static const ta_string_view compile_examples[] = {
    T("compile order.scxml payment.scxml -o order.tai"),
    T("compile --system system.xml -o system.tai"),
    T("compile order.scxml --emit c --symbol order_image -o order_image.c"),
    T("compile order.scxml -o - | tinyactors inspect -")};
static const ta_string_view check_examples[] = {T("check charts/*.scxml"),
                                                T("check -W --format json order.scxml")};
static const ta_string_view inspect_examples[] = {T("inspect order.tai"), T("inspect system.xml"),
                                                  T("inspect --format json order.scxml")};
static const ta_string_view states_examples[] = {T("states order.scxml"),
                                                 T("states --chart payment system.tai")};
static const ta_string_view transitions_examples[] = {T("transitions order.scxml"),
                                                      T("transitions -f json order.tai")};
static const ta_string_view events_examples[] = {T("events system.xml")};
static const ta_string_view data_examples[] = {T("data order.scxml")};
static const ta_string_view expressions_examples[] = {
    T("expressions --kind condition order.scxml"), T("expressions -f xml order.tai")};
static const ta_string_view disassemble_examples[] = {T("disassemble --chart order order.tai")};
static const ta_string_view graph_examples[] = {T("graph -f svg order.scxml > order.svg"),
                                                T("graph -f svg --chart payment shop.tai > payment.svg"),
                                                T("graph -f json order.tai")};
static const ta_string_view diff_examples[] = {
    T("diff order-v1.tai order-v2.scxml"), T("diff --breaking --exit-code deployed.tai order.scxml"),
    T("diff -f json --chart order old.tai new.tai")};
static const ta_string_view lint_examples[] = {
    T("lint order.scxml"), T("lint --deny all --allow unsent-event system.xml"),
    T("lint -f json --chart payment shop.tai")};
static const ta_string_view path_examples[] = {
    T("path order.scxml --to shipped"), T("path order.scxml --from cancelled --to paid --unreachable"),
    T("path -f json --chart payment shop.tai --to settled")};
static const ta_string_view canonical_examples[] = {
    T("fmt order.scxml"), T("fmt -w charts/*.scxml system.xml"),
    T("fmt --check charts/*.scxml"), T("fmt --check -f json -I shared order.scxml")};
static const ta_string_view help_examples[] = {T("help"), T("help compile"),
                                               T("help -f json inspect")};
static const ta_string_view version_examples[] = {T("version"), T("version -f json")};

typedef enum tool_command_index {
    TOOL_COMMAND_COMPILE = 0,
    TOOL_COMMAND_CHECK,
    TOOL_COMMAND_INSPECT,
    TOOL_COMMAND_STATES,
    TOOL_COMMAND_TRANSITIONS,
    TOOL_COMMAND_EVENTS,
    TOOL_COMMAND_DATA,
    TOOL_COMMAND_EXPRESSIONS,
    TOOL_COMMAND_DISASSEMBLE,
    TOOL_COMMAND_GRAPH,
    TOOL_COMMAND_DIFF,
    TOOL_COMMAND_LINT,
    TOOL_COMMAND_PATH,
    TOOL_COMMAND_CANONICAL,
    TOOL_COMMAND_HELP,
    TOOL_COMMAND_VERSION,
    TOOL_COMMAND_COUNT
} tool_command_index;

#define INPUTS                                                                                     \
    T("Its inputs are one compiled image (.tai), one system manifest (<system>), or SCXML charts " \
      "(<scxml>), which are compiled together into one image first; - reads standard input.")

static const ta_cli_command commands[TOOL_COMMAND_COUNT] = {
    [TOOL_COMMAND_COMPILE] =
        {T("compile"), T("compile charts or a system manifest into an image"),
         T("Compiles SCXML charts (the first is the image's default boot chart) or one <system> "
           "manifest into a .tai image, reading each static src relative to the file that names "
           "it, then in each -I directory. The answer lists the diagnostics and what the image holds; when the image goes "
           "to standard output (-o -), the answer goes to standard error."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(compile_options), EXAMPLES(compile_examples)},
    [TOOL_COMMAND_CHECK] = {T("check"), T("check that charts compile, or that an image verifies"),
                            T("Compiles the inputs, or verifies an image, and writes nothing but "
                              "the diagnostics. " "Exits with 1 when an input is refused."),
                            T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(check_options),
                            EXAMPLES(check_examples)},
    [TOOL_COMMAND_INSPECT] = {T("inspect"), T("what an image holds: sections, charts, boot actors"),
                              INPUTS, T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                              EXAMPLES(inspect_examples)},
    [TOOL_COMMAND_STATES] = {T("states"), T("each chart's states, as a tree in document order"),
                             INPUTS, T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                             EXAMPLES(states_examples)},
    [TOOL_COMMAND_TRANSITIONS] = {T("transitions"),
                                  T("each chart's transitions: events, condition, targets"), INPUTS,
                                  T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                                  EXAMPLES(transitions_examples)},
    [TOOL_COMMAND_EVENTS] = {T("events"),
                             T("every event name, and the transitions, raises and sends using it"),
                             INPUTS, T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                             EXAMPLES(events_examples)},
    [TOOL_COMMAND_DATA] = {T("data"), T("each chart's data declarations: id, state, expression, line"), INPUTS, T("INPUT..."),
                           1, TA_CLI_UNBOUNDED, OPTIONS(image_options), EXAMPLES(data_examples)},
    [TOOL_COMMAND_EXPRESSIONS] = {T("expressions"),
                                  T("each chart's expressions and scripts, with where they are"),
                                  INPUTS, T("INPUT..."), 1, TA_CLI_UNBOUNDED,
                                  OPTIONS(expressions_options), EXAMPLES(expressions_examples)},
    [TOOL_COMMAND_DISASSEMBLE] = {T("disassemble"),
                                  T("each chart's executable content, instruction by instruction"),
                                  INPUTS, T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                                  EXAMPLES(disassemble_examples)},
    [TOOL_COMMAND_GRAPH] = {T("graph"),
                            T("each chart as a diagram: -f svg draws it; the others list its boxes and edges"),
                            INPUTS, T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                            EXAMPLES(graph_examples)},
    [TOOL_COMMAND_DIFF] =
        {T("diff"), T("what changed between two versions, and what it does to running actors"),
         T("Compares two images (each given as an image, a manifest or one chart) chart by chart, "
           "matched by name, as statecharts: states by id, transitions by source and events, "
           "data by id, invokes, executable content as decoded instructions, and the events each "
           "state handles. Each change is breaking for actors running the old version (a state "
           "removed, moved or of another kind, data removed, another datamodel, an event a state "
           "no longer handles) or a change of behaviour, and the answer says what a migration "
           "does to running actors. --breaking --exit-code fails a build that would break them."),
         T("OLD NEW"), 2, 2, OPTIONS(diff_options), EXAMPLES(diff_examples)},
    [TOOL_COMMAND_LINT] =
        {T("lint"), T("what cannot work as written: unreachable states, dead ends, shadowed transitions, ..."),
         T("Reads the charts' states, transitions, raises and sends, without running them, and reports what "
           "cannot do what it is written for. Rules: unreachable (a state no path enters), dead-end (a state "
           "an actor never leaves), never-ends (no final state reachable from it), shadowed (a transition an "
           "earlier one always takes first), eventless-loop (an eventless transition taken without end), "
           "unhandled-event (raised or sent to a chart of the image that handles it nowhere), unsent-event "
           "(with several charts: handled, but sent by nothing in the image), unknown-events (a computed "
           "event may arrive, so the last is not checked), send-target (a static target no send can reach), "
           "search-limit. Exits with 1 when a rule given to --deny finds something."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(lint_options), EXAMPLES(lint_examples)},
    [TOOL_COMMAND_PATH] =
        {T("path"), T("the shortest way to a state: events, conditions, transitions"),
         T("Searches the chart's configurations breadth first, from its initial configuration (or a state "
           "given with --from) to one holding the state given with --to, taking any transition whose "
           "source is active: events may come in any order and conditions may hold, so each step lists the "
           "events that take it and the condition it needs. Exits with 1 when no path exists (with "
           "--unreachable: when one does). The chart is the first one, or the one --chart names."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(path_options), EXAMPLES(path_examples)},
    [TOOL_COMMAND_CANONICAL] =
        {T("fmt"), T("rewrite charts and manifests in the canonical layout"),
         T("Writes each chart or system manifest in one canonical layout: two spaces per element "
           "level, one element, comment or processing instruction per line, attributes in the "
           "order written. Only the whitespace between elements changes: comments are kept where "
           "they are, attribute values as written, and the content of <script>, <data>, "
           "<content>, <assign>, of elements of other namespaces and of mixed content byte for "
           "byte. Each file's canonical text is compiled and must give the image its original "
           "gives (lines and columns aside), or the file is refused. One file goes to standard "
           "output; -w rewrites the files in place, each whole or not at all; --check writes "
           "nothing and exits with 1 when a file is not canonical. With -w or --check the answer "
           "lists the files and whether each changed."),
         T("FILE..."), 1, TA_CLI_UNBOUNDED, OPTIONS(canonical_options), EXAMPLES(canonical_examples)},
    [TOOL_COMMAND_HELP] = {T("help"), T("describe a command, or the program"), T(""),
                           T("[COMMAND]"), 0, 1, 0, 0, EXAMPLES(help_examples)},
    [TOOL_COMMAND_VERSION] = {T("version"), T("the version of the program and its library"), T(""),
                              T(""), 0, 0, 0, 0, EXAMPLES(version_examples)},
};

static const ta_cli_program program = {
    T("tinyactors"), T("compile, check and look into tinyactors charts, systems and images"),
    TOOL_PROGRAM_OPTION_COUNT, program_options, TOOL_COMMAND_COUNT, commands};

static void tool_help(tool *t);
static void tool_version(tool *t);

static const tool_run runs[TOOL_COMMAND_COUNT] = {
    [TOOL_COMMAND_COMPILE] = tool_compile,         [TOOL_COMMAND_CHECK] = tool_check,
    [TOOL_COMMAND_INSPECT] = tool_inspect,         [TOOL_COMMAND_STATES] = tool_states,
    [TOOL_COMMAND_TRANSITIONS] = tool_transitions, [TOOL_COMMAND_EVENTS] = tool_events,
    [TOOL_COMMAND_DATA] = tool_data,               [TOOL_COMMAND_EXPRESSIONS] = tool_expressions,
    [TOOL_COMMAND_DISASSEMBLE] = tool_disassemble, [TOOL_COMMAND_GRAPH] = tool_graph,
    [TOOL_COMMAND_DIFF] = tool_diff,               [TOOL_COMMAND_LINT] = tool_lint,
    [TOOL_COMMAND_PATH] = tool_path,               [TOOL_COMMAND_CANONICAL] = tool_canonical,
    [TOOL_COMMAND_HELP] = tool_help,
    [TOOL_COMMAND_VERSION] = tool_version,
};

/* ---- what the commands share --------------------------------------------------------- */

int tool_equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (!a.length || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

uint32_t tool_count(const tool *t, uint32_t option) {
    return ta_cli_count(t->parsed, (ta_cli_option_id){TA_CLI_SCOPE_COMMAND, option});
}

const ta_cli_value *tool_last(const tool *t, uint32_t option) {
    return ta_cli_last(t->parsed, (ta_cli_option_id){TA_CLI_SCOPE_COMMAND, option});
}

void tool_text_init(tool *t, tool_text *text) {
    text->allocator = ta_growable_arena_allocator(t->arena);
    text->output = ta_output_over((ta_buffer){0, 0, 0}, &text->allocator);
}

void tool_text_append(tool_text *text, ta_string_view piece) {
    ta_output_write(&text->output, piece.pointer, piece.length);
}

void tool_text_number(tool_text *text, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    ta_output_write(&text->output, digits, ta_format_u64(digits, value));
}

/* A delay as SCXML writes one: in the largest of s, ms, us and ns that
 * holds it whole ("30s", "1500ms"). */
void tool_text_duration(tool_text *text, uint64_t nanoseconds) {
    static const struct {
        uint64_t size;
        const char *unit;
    } units[] = {{1000000000u, "s"}, {1000000u, "ms"}, {1000u, "us"}, {1u, "ns"}};
    uint32_t i = 0;
    while (nanoseconds % units[i].size) i++;
    tool_text_number(text, nanoseconds / units[i].size);
    tool_text_append(text, ta_string(units[i].unit));
}

ta_string_view tool_text_view(const tool_text *text) {
    ta_string_view view = {(const char *)0, 0};
    if (!ta_output_fits(&text->output)) return view;
    view.pointer = (const char *)text->output.bytes;
    view.length = (uint32_t)text->output.length;
    return view;
}

void tool_complain(tool *t, ta_string_view text) {
    tool_text message;
    tool_text_init(t, &message);
    tool_text_append(&message, program.name);
    tool_text_append(&message, ta_string(": "));
    tool_text_append(&message, text);
    tool_text_append(&message, ta_string("\n"));
    ta_string_view view = tool_text_view(&message);
    tool_write request = {TOOL_STANDARD_ERROR, {0, 0}, {view.pointer, view.length}};
    t->platform->write(t->platform->context, &request);
}

ta_string_view tool_hexadecimal(tool *t, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char *text = (char *)ta_growable_arena_alloc(t->arena, 16, 1);
    ta_string_view view = {(const char *)0, 0};
    if (!text) return view;
    for (uint32_t i = 0; i < 16; i++)
        text[i] = digits[(value >> (60 - 4 * i)) & 15u];
    view.pointer = text;
    view.length = 16;
    return view;
}

ta_string_view tool_state_kind_name(uint32_t kind) {
    static const ta_string_view names[] = {
        [TA_STATE_ATOMIC] = T("atomic"),   [TA_STATE_COMPOUND] = T("compound"),
        [TA_STATE_PARALLEL] = T("parallel"), [TA_STATE_FINAL] = T("final"),
        [TA_STATE_HISTORY_SHALLOW] = T("shallow history"), [TA_STATE_HISTORY_DEEP] = T("deep history")};
    return kind < sizeof names / sizeof names[0] ? names[kind] : ta_string("unknown");
}

ta_string_view tool_expression_kind_name(uint32_t kind) {
    return kind >= TA_EXPRESSION_CONDITION && kind <= TA_EXPRESSION_GLOBAL_SCRIPT
               ? expression_kinds[kind - TA_EXPRESSION_CONDITION]
               : ta_string("unknown");
}

ta_string_view tool_opcode_name(uint32_t opcode) {
    static const ta_string_view names[TA_OPCODE_MAX_] = {
        [TA_OPCODE_END] = T("end"),
        [TA_OPCODE_BLOCK] = T("block"),
        [TA_OPCODE_RAISE] = T("raise"),
        [TA_OPCODE_SEND] = T("send"),
        [TA_OPCODE_CANCEL] = T("cancel"),
        [TA_OPCODE_CANCEL_EXPRESSION] = T("cancel-expression"),
        [TA_OPCODE_ASSIGN] = T("assign"),
        [TA_OPCODE_SCRIPT] = T("script"),
        [TA_OPCODE_LOG] = T("log"),
        [TA_OPCODE_JUMP_IF_FALSE] = T("jump-if-false"),
        [TA_OPCODE_JUMP] = T("jump"),
        [TA_OPCODE_FOREACH_BEGIN] = T("foreach"),
        [TA_OPCODE_FOREACH_NEXT] = T("foreach-next"),
        [TA_OPCODE_CUSTOM] = T("custom"),
    };
    return opcode < TA_OPCODE_MAX_ ? names[opcode] : ta_string("unknown");
}

/* ---- help ------------------------------------------------------------------------------ */

/* "-f, --format FORMAT" */
static ta_string_view option_signature(tool *t, const ta_cli_option *option) {
    tool_text text;
    tool_text_init(t, &text);
    if (option->short_name) {
        char letter[2] = {'-', (char)option->short_name};
        tool_text_append(&text, (ta_string_view){letter, 2});
        tool_text_append(&text, ta_string(", "));
    }
    tool_text_append(&text, ta_string("--"));
    tool_text_append(&text, option->name);
    if (option->kind != TA_CLI_OPTION_FLAG) {
        tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, option->value_name);
    }
    return tool_text_view(&text);
}

static ta_string_view joined(tool *t, const ta_string_view *items, uint32_t count) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < count; i++) {
        if (i) tool_text_append(&text, ta_string(", "));
        tool_text_append(&text, items[i]);
    }
    return tool_text_view(&text);
}

typedef struct option_table {
    const ta_cli_option *options;
    uint32_t count;
} option_table;

static void report_options(tool *t, ta_string_view name, option_table table) {
    ta_report_list(&t->report, (ta_report_list_names){name, ta_string("option")});
    for (uint32_t i = 0; i < table.count; i++) {
        const ta_cli_option *option = &table.options[i];
        ta_report_object(&t->report, ta_string(""));
        ta_report_text(&t->report, ta_string("option"), option_signature(t, option));
        ta_report_text(&t->report, ta_string("description"), option->help);
        ta_report_text(&t->report, ta_string("values"),
                       joined(t, option->choices, option->choice_count));
        ta_report_end(&t->report);
    }
    ta_report_end(&t->report);
}

static ta_string_view usage_line(tool *t, const ta_cli_command *command) {
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, program.name);
    if (!command) {
        tool_text_append(&text, ta_string(" [options] <command> [options] [arguments]"));
        return tool_text_view(&text);
    }
    tool_text_append(&text, ta_string(" "));
    tool_text_append(&text, command->name);
    if (command->option_count) tool_text_append(&text, ta_string(" [options]"));
    if (command->arguments.length) {
        tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, command->arguments);
    }
    return tool_text_view(&text);
}

static void tool_help(tool *t) {
    ta_report *report = &t->report;
    const ta_cli_command *command = 0;
    ta_string_view name = t->parsed->command == TOOL_COMMAND_HELP && t->parsed->argument_count
                              ? t->parsed->arguments[0]
                              : (t->parsed->command != TA_NIL && t->parsed->command != TOOL_COMMAND_HELP
                                     ? commands[t->parsed->command].name
                                     : ta_string(""));
    if (name.length) {
        for (uint32_t i = 0; i < TOOL_COMMAND_COUNT; i++)
            if (tool_equal(commands[i].name, name)) command = &commands[i];
        if (!command) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, ta_string("'"));
            tool_text_append(&text, name);
            tool_text_append(&text, ta_string("' is not a tinyactors command; see 'tinyactors help'"));
            tool_complain(t, tool_text_view(&text));
            t->exit = TOOL_EXIT_USAGE;
            return;
        }
    }
    ta_report_text(report, ta_string("name"), command ? command->name : program.name);
    ta_report_text(report, ta_string("summary"), command ? command->summary : program.summary);
    ta_report_text(report, ta_string("usage"), usage_line(t, command));
    if (command && command->description.length)
        ta_report_text(report, ta_string("description"), command->description);
    if (!command) {
        ta_report_list(report, (ta_report_list_names){ta_string("commands"), ta_string("command")});
        for (uint32_t i = 0; i < TOOL_COMMAND_COUNT; i++) {
            ta_report_object(report, ta_string(""));
            ta_report_text(report, ta_string("command"), commands[i].name);
            ta_report_text(report, ta_string("summary"), commands[i].summary);
            ta_report_end(report);
        }
        ta_report_end(report);
    } else {
        report_options(t, ta_string("options"), (option_table){command->options, command->option_count});
    }
    report_options(t, ta_string("programOptions"), (option_table){program_options, TOOL_PROGRAM_OPTION_COUNT});
    if (command) {
        ta_report_list(report, (ta_report_list_names){ta_string("examples"), ta_string("example")});
        for (uint32_t i = 0; i < command->example_count; i++) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, program.name);
            tool_text_append(&text, ta_string(" "));
            tool_text_append(&text, command->examples[i]);
            ta_report_text(report, ta_string(""), tool_text_view(&text));
        }
        ta_report_end(report);
    } else {
        ta_report_text(report, ta_string("more"),
                       ta_string("tinyactors help <command> describes a command"));
    }
}

static void tool_version(tool *t) {
    ta_version version = ta_library_version();
    tool_text text;
    tool_text_init(t, &text);
    tool_text_number(&text, version.major);
    tool_text_append(&text, ta_string("."));
    tool_text_number(&text, version.minor);
    tool_text_append(&text, ta_string("."));
    tool_text_number(&text, version.patch);
    ta_string_view library = tool_text_view(&text);
    ta_string_view build = t->platform->version;
    ta_report_text(&t->report, ta_string("program"), program.name);
    ta_report_text(&t->report, ta_string("version"), build.length ? build : library);
    ta_report_text(&t->report, ta_string("library"), library);
    ta_report_number(&t->report, ta_string("abi"), version.abi);
    ta_report_number(&t->report, ta_string("imageFormat"), TA_IMAGE_VERSION);
}

/* ---- a usage error ------------------------------------------------------------------- */

static ta_string_view option_name_of(const ta_cli_parsed *parsed, ta_cli_option_id id) {
    const ta_cli_option *table = id.scope == TA_CLI_SCOPE_PROGRAM
                                     ? program_options
                                     : (parsed->command != TA_NIL ? commands[parsed->command].options : 0);
    return table ? table[id.option].name : ta_string("");
}

static void complain_usage(tool *t) {
    const ta_cli_parsed *parsed = t->parsed;
    const ta_cli_error *error = &parsed->error;
    tool_text text;
    tool_text_init(t, &text);
    ta_string_view command = parsed->command != TA_NIL ? commands[parsed->command].name : ta_string("");
    switch (error->kind) {
    case TA_CLI_ERROR_NO_COMMAND:
        tool_text_append(&text, ta_string("no command given"));
        break;
    case TA_CLI_ERROR_UNKNOWN_COMMAND:
        tool_text_append(&text, ta_string("'"));
        tool_text_append(&text, error->text);
        tool_text_append(&text, ta_string("' is not a tinyactors command"));
        break;
    case TA_CLI_ERROR_UNKNOWN_OPTION:
        tool_text_append(&text, ta_string("unknown option '"));
        tool_text_append(&text, error->text);
        tool_text_append(&text, ta_string("'"));
        break;
    case TA_CLI_ERROR_MISSING_VALUE:
        tool_text_append(&text, ta_string("option '"));
        tool_text_append(&text, error->text);
        tool_text_append(&text, ta_string("' needs a value"));
        break;
    case TA_CLI_ERROR_UNEXPECTED_VALUE:
        tool_text_append(&text, ta_string("option '"));
        tool_text_append(&text, error->text);
        tool_text_append(&text, ta_string("' takes no value"));
        break;
    case TA_CLI_ERROR_INVALID_CHOICE: {
        const ta_cli_option *option =
            error->option.scope == TA_CLI_SCOPE_PROGRAM
                ? &program_options[error->option.option]
                : &commands[parsed->command].options[error->option.option];
        tool_text_append(&text, ta_string("'"));
        tool_text_append(&text, error->text);
        tool_text_append(&text, ta_string("' is not a value of --"));
        tool_text_append(&text, option->name);
        tool_text_append(&text, ta_string(" (one of: "));
        tool_text_append(&text, joined(t, option->choices, option->choice_count));
        tool_text_append(&text, ta_string(")"));
        break;
    }
    case TA_CLI_ERROR_REPEATED_OPTION:
        tool_text_append(&text, ta_string("option --"));
        tool_text_append(&text, option_name_of(parsed, error->option));
        tool_text_append(&text, ta_string(" is given more than once"));
        break;
    case TA_CLI_ERROR_MISSING_OPTION:
        tool_text_append(&text, ta_string("option --"));
        tool_text_append(&text, option_name_of(parsed, error->option));
        tool_text_append(&text, ta_string(" is required"));
        break;
    case TA_CLI_ERROR_TOO_FEW_ARGUMENTS:
        tool_text_append(&text, ta_string("'"));
        tool_text_append(&text, command);
        tool_text_append(&text, ta_string("' needs "));
        tool_text_append(&text, commands[parsed->command].arguments);
        break;
    case TA_CLI_ERROR_TOO_MANY_ARGUMENTS:
        tool_text_append(&text, ta_string("unexpected argument '"));
        tool_text_append(&text, error->text);
        tool_text_append(&text, ta_string("'"));
        break;
    default:
        tool_text_append(&text, ta_string("the command line is not understood"));
    }
    if (error->suggestion.length) {
        tool_text_append(&text, ta_string("; did you mean '"));
        tool_text_append(&text, error->suggestion);
        tool_text_append(&text, ta_string("'?"));
    }
    tool_text_append(&text, ta_string("\nsee 'tinyactors help"));
    if (command.length) {
        tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, command);
    }
    tool_text_append(&text, ta_string("'"));
    tool_complain(t, tool_text_view(&text));
}

/* ---- an invocation ---------------------------------------------------------------------- */

static int tool_finish(tool *t) {
    if (t->answered) return t->exit; /* the command wrote its answer itself (graph -f svg) */
    tool_text out;
    tool_text_init(t, &out);
    ta_status status = ta_report_render(&t->report, t->format, &out.output);
    ta_string_view view = tool_text_view(&out);
    if (status != TA_OK || (!view.pointer && out.output.length)) {
        tool_complain(t, ta_string("out of memory writing the answer"));
        return TOOL_EXIT_FAILURE;
    }
    tool_write request = {t->report_stream, {0, 0}, {view.pointer, view.length}};
    if (t->platform->write(t->platform->context, &request) != TA_OK) return TOOL_EXIT_FAILURE;
    return t->exit;
}

int tool_main(const tool_platform *platform, tool_command_line line) {
    ta_growable_arena arena;
    ta_growable_arena_init(&arena, &(ta_growable_arena_config){sizeof(ta_growable_arena_config), 0,
                                                               0, &platform->allocator});
    ta_cli_parsed parsed;
    tool t;
    ta_memset(&t, 0, sizeof t);
    t.platform = platform;
    t.arena = &arena;
    t.program = &program;
    t.parsed = &parsed;
    t.report_stream = TOOL_STANDARD_OUTPUT;
    t.exit = TOOL_EXIT_OK;
    ta_status status = ta_cli_parse(&program, (ta_cli_words){line.count, line.words, &arena}, &parsed);
    int result;
    if (status == TA_ERROR_NO_MEMORY) {
        tool_complain(&t, ta_string("out of memory"));
        result = TOOL_EXIT_FAILURE;
    } else {
        const ta_cli_value *format =
            ta_cli_last(&parsed, (ta_cli_option_id){TA_CLI_SCOPE_PROGRAM, TOOL_OPTION_FORMAT});
        t.format = format ? format->choice : TA_REPORT_MARKDOWN;
        int help = ta_cli_count(&parsed, (ta_cli_option_id){TA_CLI_SCOPE_PROGRAM, TOOL_OPTION_HELP}) > 0;
        int version =
            ta_cli_count(&parsed, (ta_cli_option_id){TA_CLI_SCOPE_PROGRAM, TOOL_OPTION_VERSION}) > 0;
        uint32_t command = parsed.command;
        if (status != TA_OK && parsed.error.kind == TA_CLI_ERROR_NO_COMMAND) {
            /* `tinyactors` alone: what it can do, and a usage error */
            ta_report_init(&t.report, &arena, commands[TOOL_COMMAND_HELP].name);
            parsed.command = TA_NIL;
            tool_help(&t);
            tool_finish(&t);
            result = TOOL_EXIT_USAGE;
        } else if (status != TA_OK && !(help || version)) {
            complain_usage(&t);
            result = TOOL_EXIT_USAGE;
        } else if (status != TA_OK && parsed.error.kind == TA_CLI_ERROR_UNKNOWN_COMMAND) {
            complain_usage(&t);
            result = TOOL_EXIT_USAGE;
        } else {
            if (version && !help) command = TOOL_COMMAND_VERSION;
            else if (help || command == TA_NIL) command = TOOL_COMMAND_HELP;
            ta_report_init(&t.report, &arena, commands[command].name);
            if (t.format == TOOL_FORMAT_SVG && command != TOOL_COMMAND_GRAPH) {
                /* a picture is graph's answer only */
                tool_complain(&t, ta_string("--format svg is for graph; this command answers in markdown, json or xml"));
                result = TOOL_EXIT_USAGE;
            } else {
                runs[command](&t);
                result = t.exit == TOOL_EXIT_USAGE || t.exit == TOOL_EXIT_FAILURE ? t.exit : tool_finish(&t);
            }
        }
    }
    ta_growable_arena_free(&arena);
    return result;
}
