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
                            T("Write the answer in FORMAT; only graph answers in svg. Default: markdown.")},
    [TOOL_OPTION_HELP] = {T("help"), 'h', TA_CLI_OPTION_FLAG, TA_CLI_OPTION_ANSWERS, 0, 0, T(""),
                          T("Describe the command or the program.")},
    [TOOL_OPTION_VERSION] = {T("version"), 0, TA_CLI_OPTION_FLAG, TA_CLI_OPTION_ANSWERS, 0, 0,
                             T(""), T("Print the version as the version command does.")},
};

#define WARNINGS_AS_ERRORS                                                                         \
    {T("warnings-as-errors"), 'W', TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),                            \
     T("Refuse a chart that compiles with a warning.")}
#define INCLUDE                                                                                    \
    {T("include"), 'I', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("DIR"),            \
     T("Search DIR for a src that is not beside the file naming it, in the order given. "        \
       "Repeatable.")}
#define CHART                                                                                      \
    {T("chart"), 'c', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("NAME"),             \
     T("Look at the chart named NAME only. Repeatable.")}

#define SEGMENT                                                                                    \
    {T("segment"), 's', TA_CLI_OPTION_VALUE, 0, 0, 0, T("N"),                                     \
     T("Read the image that segment N of a snapshot runs. Default: the newest live segment.")}

static const ta_cli_option check_options[] = {WARNINGS_AS_ERRORS, INCLUDE};
static const ta_cli_option image_options[] = {WARNINGS_AS_ERRORS, INCLUDE, CHART, SEGMENT};
static const ta_cli_option actors_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART,
    [TOOL_OPTION_SEGMENT] = {T("segment"), 's', TA_CLI_OPTION_VALUE, 0, 0, 0, T("N"),
                             T("List the actors of segment N only. Default: every live segment.")},
    [TOOL_ACTORS_SESSION] = {T("session"), 0, TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0,
                             T("ID"), T("Look at session ID only. Repeatable.")},
};

static const ta_string_view emits[] = {[TOOL_EMIT_IMAGE] = T("image"), [TOOL_EMIT_C] = T("c")};
static const ta_cli_option compile_options[] = {
    [TOOL_OPTION_WARNINGS_AS_ERRORS] = WARNINGS_AS_ERRORS,
    [TOOL_OPTION_INCLUDE] = INCLUDE,
    [TOOL_COMPILE_OUTPUT] = {T("output"), 'o', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REQUIRED, 0, 0,
                             T("FILE"), T("Write the image to FILE, or to standard output when FILE is -.")},
    [TOOL_COMPILE_EMIT] = {T("emit"), 'e', TA_CLI_OPTION_CHOICE, 0, 2, emits, T("KIND"),
                           T("Write the image as KIND. Values: image (the image's bytes) or c (an 8-aligned "
                             "C array). Default: image.")},
    [TOOL_COMPILE_SYMBOL] = {T("symbol"), 's', TA_CLI_OPTION_VALUE, 0, 0, 0, T("NAME"),
                             T("Name the C array NAME with --emit c; NAME is a C identifier. "
                               "Default: tinyactors_image.")},
};

static const ta_string_view expression_kinds[] = {
    T("condition"), T("value"), T("location"), T("script"),
    T("string"),    T("content"), T("item"),   T("global-script")};
static const ta_cli_option expressions_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART, SEGMENT,
    [TOOL_EXPRESSIONS_KIND] = {T("kind"), 'k', TA_CLI_OPTION_CHOICE, TA_CLI_OPTION_REPEATABLE, 8,
                               expression_kinds, T("KIND"),
                               T("Keep only the expressions of KIND. Repeatable.")},
};
static const ta_cli_option diff_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART, SEGMENT,
    [TOOL_DIFF_EXIT_CODE] = {T("exit-code"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                             T("Exit with 1 when the two differ, as git diff --exit-code does; with "
                               "--breaking, when a change breaks running actors.")},
    [TOOL_DIFF_BREAKING] = {T("breaking"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                            T("List only the changes that break actors running the old version.")},
    [TOOL_DIFF_ALL] = {T("all"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                       T("List the charts that did not change too.")},
};
static const ta_cli_option lint_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART, SEGMENT,
    [TOOL_LINT_DENY] = {T("deny"), 'd', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("RULE"),
                        T("Exit with 1 when RULE finds something; all names every rule. Repeatable.")},
    [TOOL_LINT_ALLOW] = {T("allow"), 'a', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("RULE"),
                         T("Leave out the findings of RULE; all leaves out every rule's. Repeatable.")},
};
static const ta_cli_option path_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART, SEGMENT,
    [TOOL_PATH_TO] = {T("to"), 't', TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REQUIRED, 0, 0, T("STATE"),
                      T("Search for a configuration that holds STATE.")},
    [TOOL_PATH_FROM] = {T("from"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("STATE"),
                        T("Start in STATE, entered with its defaults. Default: the initial configuration.")},
    [TOOL_PATH_UNREACHABLE] = {T("unreachable"), 'u', TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                               T("Succeed only when no path exists, and exit with 1 otherwise.")},
};

static const ta_string_view directions[] = {[TOOL_GRAPH_DOWN] = T("down"), [TOOL_GRAPH_RIGHT] = T("right")};
static const ta_string_view themes[] = {[TOOL_GRAPH_THEME_AUTO] = T("auto"), [TOOL_GRAPH_THEME_LIGHT] = T("light"),
                                        [TOOL_GRAPH_THEME_DARK] = T("dark")};
static const ta_cli_option graph_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE, CHART, SEGMENT,
    [TOOL_GRAPH_DIRECTION] = {T("direction"), 0, TA_CLI_OPTION_CHOICE, 0, 2, directions, T("DIRECTION"),
                              T("Lay the states out in DIRECTION. Values: down (top to bottom) or right "
                                "(left to right). Default: down.")},
    [TOOL_GRAPH_THEME] = {T("theme"), 0, TA_CLI_OPTION_CHOICE, 0, 3, themes, T("THEME"),
                          T("Draw the colours of THEME with -f svg. Values: auto (the reader's preference), "
                            "light or dark. Default: auto.")},
    [TOOL_GRAPH_HIGHLIGHT] = {T("highlight"), 0, TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("STATE"),
                              T("Mark the box of STATE. Repeatable.")},
    [TOOL_GRAPH_PATH] = {T("path"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                         T("Mark the path the path command finds to --to from --from, its steps numbered.")},
    [TOOL_GRAPH_TO] = {T("to"), 't', TA_CLI_OPTION_VALUE, 0, 0, 0, T("STATE"),
                       T("End the path of --path at STATE.")},
    [TOOL_GRAPH_FROM] = {T("from"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("STATE"),
                         T("Start the path of --path at STATE. Default: the initial configuration.")},
    [TOOL_GRAPH_DEPTH] = {T("depth"), 'd', TA_CLI_OPTION_VALUE, 0, 0, 0, T("N"),
                          T("Fold every compound or parallel state nested deeper than N levels into one box.")},
    [TOOL_GRAPH_FOLD] = {T("fold"), 0, TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0, T("STATE"),
                         T("Fold the compound or parallel state STATE into one box. Repeatable.")},
};

static const ta_cli_option canonical_options[] = {
    [TOOL_CANONICAL_WRITE] = {T("write"), 'w', TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                              T("Rewrite each file in place, whole or not at all.")},
    [TOOL_OPTION_INCLUDE] = INCLUDE,
    [TOOL_CANONICAL_CHECK] = {T("check"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                              T("Write nothing, and exit with 1 when a file is not in canonical form.")},
};

static const ta_string_view realms[] = {T("auto"), T("isolated")};
static const ta_cli_option run_options[] = {
    WARNINGS_AS_ERRORS, INCLUDE,
    [TOOL_RUN_CONFIG] = {T("config"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("FILE"),
                         T("Read the host config from FILE as JSON (docs/host-config.md), its ioProcessors a "
                           "map of type to {location}. Default: every key core's.")},
    [TOOL_RUN_SYSTEM] = {T("system"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("ID"),
                         T("Set the config's systemID to ID. Default: tinyactors-run.")},
    [TOOL_RUN_IO_PROCESSOR] = {T("io-processor"), 0, TA_CLI_OPTION_VALUE, TA_CLI_OPTION_REPEATABLE, 0, 0,
                               T("TYPE[=LOCATION]"),
                               T("Add an I/O processor of TYPE, at LOCATION when given, that the client "
                                 "serves after the config's. Repeatable.")},
    [TOOL_RUN_BOOT] = {T("boot"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                       T("Spawn the boot actors of the inputs' system manifest before the first message.")},
    [TOOL_RUN_WALL_START] = {T("wall-start"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("NS"),
                             T("Start the wall clock at NS nanoseconds since the Unix epoch. Default: 0.")},
    [TOOL_RUN_SEED] = {T("seed"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("N"),
                       T("Seed Math.random with N; 0 takes the engine's own seed, new on every run. "
                         "Default: 1.")},
    [TOOL_RUN_MAX_OPS] = {T("max-ops"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("N"),
                          T("Allow quickjs N interrupt checks per evaluation. Default: quickjs's.")},
    [TOOL_RUN_MEMORY_LIMIT] = {T("memory-limit"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("BYTES"),
                               T("Limit quickjs's heap to BYTES. Default: quickjs's.")},
    [TOOL_RUN_REALM] = {T("realm"), 0, TA_CLI_OPTION_CHOICE, 0, 2, realms, T("REALM"),
                        T("Set how the charts share quickjs realms to REALM. Values: auto (shared unless "
                          "globals are declared) or isolated (one per session). Default: auto.")},
    [TOOL_RUN_MAX_MESSAGE] = {T("max-message"), 0, TA_CLI_OPTION_VALUE, 0, 0, 0, T("BYTES"),
                              T("Take input lines of at most BYTES. Default: 64 MiB.")},
    [TOOL_RUN_SCHEMA] = {T("schema"), 0, TA_CLI_OPTION_FLAG, 0, 0, 0, T(""),
                         T("Write the protocol's JSON Schema and exit.")},
};

#define OPTIONS(table) (uint32_t)(sizeof(table) / sizeof(table)[0]), (table)
#define EXAMPLES(table) (uint32_t)(sizeof(table) / sizeof(table)[0]), (table)
#define EXITS(table) (uint32_t)(sizeof(table) / sizeof(table)[0]), (table)

static const ta_cli_example compile_examples[] = {
    {T("Compile two charts into one image:"), T("compile order.scxml payment.scxml -o order.tai")},
    {T("Compile a system manifest into an image:"), T("compile system.xml -o system.tai")},
    {T("Write the image as a C array named order_image:"),
     T("compile order.scxml --emit c --symbol order_image -o order_image.c")},
    {T("Compile to standard output and inspect the image:"), T("compile order.scxml -o - | tinyactors inspect -")}};
static const ta_cli_example check_examples[] = {
    {T("Check that every chart in a directory compiles:"), T("check charts/*.scxml")},
    {T("Refuse a chart with a warning and answer in JSON:"), T("check -W --format json order.scxml")}};
static const ta_cli_example inspect_examples[] = {
    {T("Show what an image holds:"), T("inspect order.tai")},
    {T("Compile a system manifest and show its image:"), T("inspect system.xml")},
    {T("Compile a chart and show its image as JSON:"), T("inspect --format json order.scxml")},
    {T("Show what a snapshot holds:"), T("inspect shop.snapshot")},
    {T("Show a blob with its chart and configuration from a snapshot:"), T("inspect order-17.blob shop.snapshot")}};
static const ta_cli_example states_examples[] = {
    {T("List the states of a chart:"), T("states order.scxml")},
    {T("List the states of one chart of an image:"), T("states --chart payment system.tai")},
    {T("List the states in the image of a snapshot's segment 1:"), T("states --segment 1 shop.snapshot")}};
static const ta_cli_example actors_examples[] = {
    {T("List every actor of a snapshot:"), T("actors shop.snapshot")},
    {T("Show session 17 if it runs the order chart:"), T("actors --chart order --session 17 shop.snapshot")},
    {T("List every actor of a snapshot as JSON:"), T("actors -f json shop.snapshot")}};
static const ta_cli_example transitions_examples[] = {
    {T("List the transitions of a chart:"), T("transitions order.scxml")},
    {T("List the transitions of an image as JSON:"), T("transitions -f json order.tai")}};
static const ta_cli_example events_examples[] = {
    {T("List the events of a system manifest's charts:"), T("events system.xml")}};
static const ta_cli_example data_examples[] = {
    {T("List the data declarations of a chart:"), T("data order.scxml")}};
static const ta_cli_example expressions_examples[] = {
    {T("List the conditions of a chart:"), T("expressions --kind condition order.scxml")},
    {T("List the expressions of an image as XML:"), T("expressions -f xml order.tai")}};
static const ta_cli_example disassemble_examples[] = {
    {T("Disassemble one chart of an image:"), T("disassemble --chart order order.tai")}};
static const ta_cli_example graph_examples[] = {
    {T("Draw a chart as SVG:"), T("graph -f svg order.scxml > order.svg")},
    {T("Draw one chart of an image:"), T("graph -f svg --chart payment shop.tai > payment.svg")},
    {T("Draw a chart left to right in dark colours:"),
     T("graph -f svg --direction right --theme dark order.scxml > order.svg")},
    {T("Mark the path to shipped and highlight cancelled:"),
     T("graph -f svg --path --to shipped --highlight cancelled order.scxml > shipping.svg")},
    {T("Fold states nested deeper than one level, and checkout:"),
     T("graph -f svg --depth 1 --fold checkout shop.scxml > overview.svg")},
    {T("List the boxes and edges of an image's charts as JSON:"), T("graph -f json order.tai")}};
static const ta_cli_example diff_examples[] = {
    {T("Compare an image with a newer version of its chart:"), T("diff order-v1.tai order-v2.scxml")},
    {T("Fail if a change breaks running actors:"), T("diff --breaking --exit-code deployed.tai order.scxml")},
    {T("Compare one chart of two images as JSON:"), T("diff -f json --chart order old.tai new.tai")}};
static const ta_cli_example lint_examples[] = {
    {T("Lint a chart:"), T("lint order.scxml")},
    {T("Fail on a finding of every rule but unsent-event:"),
     T("lint --deny all --allow unsent-event system.xml")},
    {T("Lint one chart of an image as JSON:"), T("lint -f json --chart payment shop.tai")}};
static const ta_cli_example path_examples[] = {
    {T("Find the shortest path to shipped:"), T("path order.scxml --to shipped")},
    {T("Fail if paid can be reached from cancelled:"),
     T("path order.scxml --from cancelled --to paid --unreachable")},
    {T("Find a path in one chart of an image as JSON:"), T("path -f json --chart payment shop.tai --to settled")}};
static const ta_cli_example canonical_examples[] = {
    {T("Write a chart's canonical text to standard output:"), T("fmt order.scxml")},
    {T("Rewrite charts and a system manifest in place:"), T("fmt -w charts/*.scxml system.xml")},
    {T("Fail if a chart is not in canonical form:"), T("fmt --check charts/*.scxml")},
    {T("Check a chart that includes files from shared, in JSON:"), T("fmt --check -f json -I shared order.scxml")}};
static const ta_cli_example run_examples[] = {
    {T("Serve a host running one chart:"), T("run order.scxml")},
    {T("Serve two charts with a host config:"), T("run --config shop.json order.scxml payment.scxml")},
    {T("Serve system shop with a payments I/O processor, requests from a file:"),
     T("run --io-processor payments --system shop order.scxml < requests.jsonl")},
    {T("Write the protocol's JSON Schema to a file:"), T("run --schema > run.schema.json")}};
static const ta_cli_example help_examples[] = {
    {T("List the commands:"), T("help")},
    {T("Describe the compile command:"), T("help compile")},
    {T("Describe the inspect command in JSON:"), T("help -f json inspect")}};
static const ta_cli_example version_examples[] = {
    {T("Print the versions:"), T("version")},
    {T("Print the versions as JSON:"), T("version -f json")}};

/* Exit statuses beyond a usage error's (2), as the commands set them (tool.h, TOOL_EXIT_*). */
#define INPUT_REFUSED                                                                              \
    {1, T("An input is refused: it does not compile or verify, or it is damaged or of an "        \
          "unknown format.")}
#define CANNOT_READ                                                                                \
    {3, T("A file cannot be read or written, or memory runs out.")}

static const ta_cli_exit_status input_exits[] = {INPUT_REFUSED, CANNOT_READ};
static const ta_cli_exit_status compile_exits[] = {
    {1, T("An input is refused: it does not compile, or it is of an unknown format.")},
    {3, T("An input cannot be read, the image cannot be written or memory runs out.")}};
static const ta_cli_exit_status graph_exits[] = {
    INPUT_REFUSED, {1, T("With --path, no path reaches the --to state in any chart drawn.")}, CANNOT_READ};
static const ta_cli_exit_status diff_exits[] = {
    INPUT_REFUSED,
    {1, T("With --exit-code, the two versions differ; with --breaking too, a change breaks running "
          "actors.")},
    CANNOT_READ};
static const ta_cli_exit_status lint_exits[] = {
    INPUT_REFUSED, {1, T("A rule given to --deny finds something.")}, CANNOT_READ};
static const ta_cli_exit_status path_exits[] = {
    INPUT_REFUSED,
    {1, T("Without --unreachable, no path exists or the search stops at its limit.")},
    {1, T("With --unreachable, a path exists, or the search stops at its limit.")},
    CANNOT_READ};
static const ta_cli_exit_status canonical_exits[] = {
    {1, T("A file is refused: it is no XML fmt can rewrite, or its canonical text gives another "
          "image.")},
    {1, T("With --check, a file is not in canonical form.")},
    CANNOT_READ};
static const ta_cli_exit_status actors_exits[] = {
    {1, T("The snapshot cannot be restored: it is damaged or of another format.")}, CANNOT_READ};
static const ta_cli_exit_status run_exits[] = {
    {0, T("Standard input ends, or --schema has written the schema.")},
    {1, T("An input or the config is refused at startup.")},
    {3, T("An input or standard input cannot be read, standard output cannot be written or memory "
          "runs out.")}};

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
    TOOL_COMMAND_ACTORS,
    TOOL_COMMAND_RUN,
    TOOL_COMMAND_HELP,
    TOOL_COMMAND_VERSION,
    TOOL_COMMAND_COUNT
} tool_command_index;

/* What the commands that look into an image read, after their own first sentence. */
#define INPUTS                                                                                     \
    " Reads one image (.tai), one snapshot, one system manifest or SCXML charts, compiled "       \
    "together into one image first. --segment picks a snapshot's image, and - reads standard "    \
    "input."

static const ta_cli_command commands[TOOL_COMMAND_COUNT] = {
    [TOOL_COMMAND_COMPILE] =
        {T("compile"), T("Compile charts or a system manifest into an image"),
         T("Compiles SCXML charts or one system manifest into a .tai image. The first chart is the "
           "image's default boot chart. Each static src is read relative to the file that names it, "
           "then in each -I directory. The answer lists the diagnostics and what the image holds. "
           "With -o -, the image goes to standard output and the answer to standard error."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(compile_options), EXAMPLES(compile_examples),
         EXITS(compile_exits)},
    [TOOL_COMMAND_CHECK] = {T("check"), T("Check that charts compile or that an image verifies"),
                            T("Compiles the inputs, or verifies an image or every image of a snapshot. "
                              "Writes nothing but whether they are valid and the diagnostics."),
                            T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(check_options),
                            EXAMPLES(check_examples), EXITS(input_exits)},
    [TOOL_COMMAND_INSPECT] =
        {T("inspect"), T("Show what an image, a snapshot or a blob holds"),
         T("Shows an image's sections, charts and boot actors; it reads its inputs as the other "
           "commands do. Shows a snapshot's format, system, time, segments with their images and "
           "actors. Shows the datamodels' state of a snapshot too, decoded when it is CBOR. Shows "
           "the session a hibernation or export blob holds, the image it was written with and who "
           "can wake it. Shows a blob's datamodel state, and its chart and configuration when the "
           "image follows it: an image, charts or a snapshot."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options), EXAMPLES(inspect_examples),
         EXITS(input_exits)},
    [TOOL_COMMAND_STATES] = {T("states"), T("List each chart's states as a tree in document order"),
                             T("Lists each chart's states as a tree in document order: id, kind, parent, "
                               "depth and initial targets. Counts each state's transitions, invokes and "
                               "data." INPUTS),
                             T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                             EXAMPLES(states_examples), EXITS(input_exits)},
    [TOOL_COMMAND_TRANSITIONS] = {T("transitions"),
                                  T("List each chart's transitions with events, conditions and targets"),
                                  T("Lists each chart's transitions: kind, source, events, condition, "
                                    "targets, type and whether each has actions." INPUTS),
                                  T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                                  EXAMPLES(transitions_examples), EXITS(input_exits)},
    [TOOL_COMMAND_EVENTS] = {T("events"),
                             T("List every event name and the transitions, raises and sends using it"),
                             T("Lists every event name the charts use. Counts the transitions that handle "
                               "each one and the raises and sends that make it by name." INPUTS),
                             T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                             EXAMPLES(events_examples), EXITS(input_exits)},
    [TOOL_COMMAND_DATA] = {T("data"), T("List each chart's data declarations"),
                           T("Lists each chart's data declarations: id, the state declaring it, its "
                             "expression and its line." INPUTS),
                           T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                           EXAMPLES(data_examples), EXITS(input_exits)},
    [TOOL_COMMAND_EXPRESSIONS] = {T("expressions"),
                                  T("List each chart's expressions and scripts with where they are"),
                                  T("Lists each chart's expressions and scripts: kind, line, column and "
                                    "text." INPUTS),
                                  T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(expressions_options),
                                  EXAMPLES(expressions_examples), EXITS(input_exits)},
    [TOOL_COMMAND_DISASSEMBLE] = {T("disassemble"),
                                  T("List each chart's executable content instruction by instruction"),
                                  T("Lists each chart's executable content as instructions: their place, "
                                    "the entry points starting there and their operands." INPUTS),
                                  T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(image_options),
                                  EXAMPLES(disassemble_examples), EXITS(input_exits)},
    [TOOL_COMMAND_GRAPH] = {T("graph"), T("Draw each chart, or list its boxes and edges"),
                            T("Lays each chart out as nested boxes for its states and orthogonal routes "
                              "for its transitions. With -f svg it writes the picture; the other formats "
                              "list each box and edge." INPUTS),
                            T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(graph_options),
                            EXAMPLES(graph_examples), EXITS(graph_exits)},
    [TOOL_COMMAND_DIFF] =
        {T("diff"), T("Compare two versions and show what changes for running actors"),
         T("Compares two images chart by chart, matching charts by name. Each version is an image, a "
           "system manifest or one chart. Compares data by id, states by id and transitions by source and "
           "events. Compares invokes, executable content as decoded instructions and the events "
           "each state handles. Marks each change as breaking for actors running the old version, or "
           "as a change of behaviour. A state removed, moved or of another kind is breaking. So are "
           "data removed, another datamodel and an event a state no longer handles. The answer says "
           "what a migration does to running actors."),
         T("OLD NEW"), 2, 2, OPTIONS(diff_options), EXAMPLES(diff_examples), EXITS(diff_exits)},
    [TOOL_COMMAND_LINT] =
        {T("lint"), T("Find what cannot work as written in charts"),
         T("Reads the charts' states, transitions, raises and sends without running them. Reports what "
           "cannot do what it is written for, by rule. unreachable: a state no path enters. dead-end: a "
           "state an actor never leaves. never-ends: a state from which no final state is reachable. "
           "shadowed: a transition an earlier one always takes first. eventless-loop: an eventless "
           "transition taken without end. unhandled-event: raised or sent to a chart of the image that "
           "handles it nowhere. unsent-event: with several charts, handled but sent by nothing in the "
           "image. unknown-events: a computed event may arrive, so unsent-event is not checked. "
           "send-target: a static target no send can reach. search-limit: the search stopped at its "
           "limit, so never-ends is not checked."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(lint_options), EXAMPLES(lint_examples),
         EXITS(lint_exits)},
    [TOOL_COMMAND_PATH] =
        {T("path"), T("Find the shortest path to a state: events, conditions and transitions"),
         T("Searches a chart's configurations breadth first for one holding the --to state. It starts "
           "from the initial configuration, or from the --from state. Any transition whose source is "
           "active may be taken: events come in any order and conditions may hold. So each step lists "
           "the events that take it and the condition it needs. The chart is the first one, or the one "
           "--chart names."),
         T("INPUT..."), 1, TA_CLI_UNBOUNDED, OPTIONS(path_options), EXAMPLES(path_examples),
         EXITS(path_exits)},
    [TOOL_COMMAND_CANONICAL] =
        {T("fmt"), T("Rewrite charts and system manifests in the canonical layout"),
         T("Writes each chart or system manifest in one canonical layout. It indents two spaces per "
           "element level and writes one element, comment or processing instruction per line. "
           "Attributes stay in the order written. Only the whitespace between elements changes: "
           "comments stay where they are and attribute values as written. Script, data, content and "
           "assign elements keep their content byte for byte. So do elements of other namespaces and "
           "mixed content. Each file's canonical text must compile to the image its original gives, "
           "lines and columns aside, or the file is refused. One file goes to standard output. -w "
           "rewrites the files in place, each whole or not at all. --check writes nothing. With -w or "
           "--check the answer lists the files and whether each changed."),
         T("FILE..."), 1, TA_CLI_UNBOUNDED, OPTIONS(canonical_options), EXAMPLES(canonical_examples),
         EXITS(canonical_exits)},
    [TOOL_COMMAND_ACTORS] =
        {T("actors"), T("List every actor of a snapshot with its state and queues"),
         T("Restores the snapshot in memory, without its datamodels, and runs nothing. Lists each actor "
           "of each live segment: its session, name, chart, segment and state. Lists its parent, its "
           "running invocations and its configuration, the active states in document order. A "
           "hibernated actor's configuration is in its blob. Lists the events queued in its mailbox and "
           "its pending delayed sends with when they are due. --chart, --segment and --session narrow "
           "the list."),
         T("SNAPSHOT"), 1, 1, OPTIONS(actors_options), EXAMPLES(actors_examples), EXITS(actors_exits)},
    [TOOL_COMMAND_RUN] =
        {T("run"), T("Serve one host over JSON-RPC 2.0 on standard input and output"),
         T("Experimental: the protocol may change in any release until a second client uses it. Makes "
           "one host with quickjs and adds the inputs' image: charts, a system manifest or an image. "
           "Without inputs, the client adds images. Reads JSON-RPC 2.0 messages, one per line, from "
           "standard input and answers on standard output. run.* methods are the host's calls (run.spawn, run.send, run.advanceTime, "
           "run.turn, run.addImage, ...). Every other method is core's control plane (actor.state, "
           "host.stats, ...). What actors do comes back as effect.* notifications. Each is written "
           "before the response to the request that caused it. The clock is simulated: it moves only "
           "with run.advanceTime. The end of input ends the server. run --schema writes the protocol's "
           "JSON Schema (docs/cli.md, Run)."),
         T("[INPUT...]"), 0, TA_CLI_UNBOUNDED, OPTIONS(run_options), EXAMPLES(run_examples),
         EXITS(run_exits)},
    [TOOL_COMMAND_HELP] = {T("help"), T("Describe a command or the program"),
                           T("Describes the program and its commands, or one command: its usage, "
                             "options, examples and exit status."),
                           T("[COMMAND]"), 0, 1, 0, 0, EXAMPLES(help_examples), 0, 0},
    [TOOL_COMMAND_VERSION] = {T("version"), T("Print the versions of the program and its library"),
                              T("Prints the program's version and its library's version, ABI and image "
                                "format version."),
                              T(""), 0, 0, 0, 0, EXAMPLES(version_examples), 0, 0},
};

static const ta_cli_program program = {
    T("tinyactors"),
    T("Compile, check, run and inspect charts, images, snapshots and blobs"),
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
    [TOOL_COMMAND_ACTORS] = tool_actors,           [TOOL_COMMAND_RUN] = tool_serve,
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
    tool_text_over(text, t->arena);
}

void tool_text_over(tool_text *text, ta_growable_arena *arena) {
    text->allocator = ta_growable_arena_allocator(arena);
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

ta_string_view tool_state_id(const ta_image_view *view, const ta_chart_record *chart, uint32_t state) {
    if (state == TA_NIL || state >= chart->state_count) return ta_string("");
    ta_string_view id = ta_image_view_string(view, view->states[chart->first_state + state].id);
    return id.length ? id : ta_string("/scxml"); /* the root: its XPath, never an id */
}

ta_status tool_decimal(ta_string_view text, uint64_t *out) {
    uint64_t value = 0;
    if (!text.length) return TA_ERROR_INVALID;
    for (uint32_t i = 0; i < text.length; i++) {
        char digit = text.pointer[i];
        if (digit < '0' || digit > '9' || value > (UINT64_MAX - (uint64_t)(digit - '0')) / 10u)
            return TA_ERROR_INVALID;
        value = value * 10u + (uint64_t)(digit - '0');
    }
    *out = value;
    return TA_OK;
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
            tool_text_append(&text, command->examples[i].command);
            ta_report_object(report, ta_string(""));
            ta_report_text(report, ta_string("caption"), command->examples[i].caption);
            ta_report_text(report, ta_string("command"), tool_text_view(&text));
            ta_report_end(report);
        }
        ta_report_end(report);
        if (command->exit_status_count) {
            ta_report_list(report, (ta_report_list_names){ta_string("exitStatus"), ta_string("status")});
            for (uint32_t i = 0; i < command->exit_status_count; i++) {
                ta_report_object(report, ta_string(""));
                ta_report_number(report, ta_string("code"), command->exit_statuses[i].code);
                ta_report_text(report, ta_string("meaning"), command->exit_statuses[i].meaning);
                ta_report_end(report);
            }
            ta_report_end(report);
        }
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
