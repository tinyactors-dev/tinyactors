/* run.c — tinyactors run (serve.h): the options, the host config as the
 * protocol takes it, startup, the message loop and the end of input.
 * docs/cli.md, "Run", is the protocol's reference. */
#include "serve.h"

#include "base/memory.h"
#include "cli/json.h"

typedef enum run_constants {
    RUN_CHUNK = 65536,                  /* standard input is read this much at a time */
    RUN_ERROR_ROOM = 65536,             /* a refused config's reply */
    RUN_MAX_MESSAGE = 64 * 1024 * 1024, /* --max-message's default */
    RUN_SEED = 1,                       /* --seed's default: repeatable runs are the command's purpose */
    RUN_ALIGN = 8,
    RUN_MEMORY_ROOM = 160 /* the -32004 response written without the arena */
} run_constants;

static const ta_string_view run_system = TA_CLI_TEXT("tinyactors-run");

/* ---- writing ---------------------------------------------------------------------------- */

void tool_serve_write(tool_server *server, ta_bytes cbor) {
    if (server->failed) return;
    tool *t = server->invocation;
    tool_text line;
    tool_text_over(&line, &server->request);
    ta_status status = ta_rpc_line_write(cbor, &line.output);
    ta_string_view view = tool_text_view(&line);
    if (status != TA_OK || !view.pointer) {
        tool_complain(t, ta_string("a line could not be written as JSON: it is lost"));
        return;
    }
    tool_write request = {TOOL_STANDARD_OUTPUT, {0, 0}, {view.pointer, view.length}};
    if (t->platform->write(t->platform->context, &request) != TA_OK) server->failed = 1;
}

/* -32004 for a message no memory could be found for, written from a room
 * on the stack. */
static void memory_response(tool_server *server) {
    unsigned char room[RUN_MEMORY_ROOM];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){room, sizeof room, 0}, 0);
    ta_rpc_error error = {TA_RPC_MEMORY, 0, ta_string("memory"), ta_string(""), ta_string("out of memory"),
                          ta_string("TA_ERROR_NO_MEMORY")};
    ta_rpc_response response = {{0, TA_NIL, 0}, {0, 0, 0}, &error};
    ta_rpc_response_write(&response, &writer);
    ta_bytes cbor;
    unsigned char line[RUN_MEMORY_ROOM * 2];
    ta_output out = ta_output_over((ta_buffer){line, sizeof line, 0}, 0);
    tool *t = server->invocation;
    if (ta_cbor_writer_bytes(&writer, &cbor) != TA_OK || ta_rpc_line_write(cbor, &out) != TA_OK) {
        tool_complain(t, ta_string("out of memory: a message is lost"));
        return;
    }
    tool_write request = {TOOL_STANDARD_OUTPUT, {0, 0}, {line, out.length}};
    if (t->platform->write(t->platform->context, &request) != TA_OK) server->failed = 1;
}

/* ---- answering -------------------------------------------------------------------------- */

/* The response to one message, in the request arena (empty: none, for a
 * notification). */
static ta_bytes answer(tool_server *server, const ta_rpc_message *message) {
    ta_bytes none = {0, 0};
    tool_call call;
    ta_memset(&call, 0, sizeof call);
    call.server = server;
    call.message = message;
    ta_cbor_writer_init(&call.result, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_cbor_write_map(&call.result, TA_CBOR_COUNT_LATER);
    if (message->error.code) call.error = message->error;
    else tool_serve_call(&call);
    if (!message->error.code && message->kind != TA_RPC_REQUEST) return none;
    if (!call.error.code && call.response.length) return call.response;
    ta_cbor_document result;
    int has_result = 0;
    if (!call.error.code) {
        ta_cbor_write_end(&call.result);
        ta_bytes bytes;
        ta_status status = ta_cbor_writer_bytes(&call.result, &bytes);
        if (status == TA_OK) status = ta_rpc_decode(&(ta_rpc_decoding){bytes, &server->request}, &result);
        if (status == TA_OK) has_result = 1;
        else tool_serve_status(&call, status);
    }
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_rpc_response response = {{&message->document, message->error.code ? TA_NIL : message->id, 0},
                                {has_result ? &result : 0, 0, 0},
                                call.error.code ? &call.error : 0};
    ta_rpc_response_write(&response, &writer);
    ta_bytes out;
    return ta_cbor_writer_bytes(&writer, &out) == TA_OK ? out : none;
}

static void respond(tool_server *server, const ta_rpc_message *message) {
    ta_bytes response = answer(server, message);
    int expected = message->error.code || message->kind == TA_RPC_REQUEST;
    if (response.length) tool_serve_write(server, response);
    else if (expected) memory_response(server);
}

/* A batch: each element in order, as if sent alone, its notifications
 * written as they come; the responses as one array after the last. */
static void serve_batch(tool_server *server, const ta_rpc_message *batch) {
    ta_bytes *responses =
        (ta_bytes *)ta_growable_arena_alloc(&server->request, sizeof *responses * batch->count, RUN_ALIGN);
    if (!responses) {
        memory_response(server);
        return;
    }
    uint32_t answered = 0, at = batch->at + 1;
    for (uint32_t i = 0; i < batch->count && !server->failed; i++) {
        ta_rpc_message element;
        ta_rpc_element_read(&(ta_rpc_element){batch, at, 0}, &element);
        at = ta_cbor_next(&batch->document, at);
        ta_bytes response = answer(server, &element);
        if (response.length) responses[answered++] = response;
        else if (element.error.code || element.kind == TA_RPC_REQUEST) {
            memory_response(server);
            return;
        }
    }
    if (!answered) return;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_cbor_write_array(&writer, answered);
    for (uint32_t i = 0; i < answered; i++) {
        ta_cbor_document document;
        if (ta_rpc_decode(&(ta_rpc_decoding){responses[i], &server->request}, &document) != TA_OK) {
            memory_response(server);
            return;
        }
        ta_rpc_write_item(&writer, (ta_rpc_item){&document, 0, 0});
    }
    ta_bytes cbor;
    if (ta_cbor_writer_bytes(&writer, &cbor) != TA_OK) {
        memory_response(server);
        return;
    }
    tool_serve_write(server, cbor);
}

static void serve_line(tool_server *server, const ta_rpc_line *line) {
    ta_growable_arena_mark mark = ta_growable_arena_position(&server->request);
    ta_rpc_message message;
    ta_memset(&message, 0, sizeof message);
    message.method = message.params = message.id = TA_NIL;
    if (line->too_long) {
        message.error = (ta_rpc_error){TA_RPC_INVALID_REQUEST, 0, ta_string("limit"), ta_string(""),
                                       ta_string("a line longer than --max-message: skipped to its end"),
                                       ta_string("")};
        respond(server, &message);
    } else if (ta_rpc_parse(&(ta_rpc_parsing){line->text, &server->request}, &message) != TA_OK) {
        memory_response(server);
    } else if (message.kind == TA_RPC_BATCH && !message.error.code) {
        serve_batch(server, &message);
    } else {
        respond(server, &message);
    }
    ta_growable_arena_release(&server->request, mark);
    if (server->request.reserved > TOOL_SERVE_REQUEST_KEEP) ta_growable_arena_free(&server->request);
}

/* ---- the command line --------------------------------------------------------------------- */

static void usage(tool *t, ta_string_view text) {
    tool_complain(t, text);
    t->exit = TOOL_EXIT_USAGE;
}

/* A number option: 1 when absent (`out` untouched) or read and at most
 * `maximum`; 0 (told) otherwise. */
typedef struct number_option {
    uint32_t option;
    const char *name;
    uint64_t maximum;
} number_option;

static int number_of(tool *t, number_option option, uint64_t *out) {
    const ta_cli_value *value = tool_last(t, option.option);
    uint64_t number = 0;
    if (!value) return 1;
    if (tool_decimal(value->text, &number) != TA_OK || number > option.maximum) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string(option.name));
        tool_text_append(&text, ta_string(" takes a number from 0 to "));
        tool_text_number(&text, option.maximum);
        usage(t, tool_text_view(&text));
        return 0;
    }
    *out = number;
    return 1;
}

/* ---- the config ------------------------------------------------------------------------------ */

/* An I/O processor of the config: its type, and its location (empty: none). */
typedef struct processor {
    ta_string_view type, location;
} processor;

typedef struct config_build {
    tool *t;
    ta_cbor_writer writer;
    ta_allocator allocator;
    processor *processors;
    uint32_t processor_count;
    uint32_t system_item; /* a systemID that is not text or is empty: its token, for core to refuse; else TA_NIL */
    ta_string_view system; /* the config's systemID, empty when it has none */
} config_build;

static int config_refuse(tool *t, ta_string_view text) {
    tool_text message;
    tool_text_init(t, &message);
    tool_text_append(&message, ta_string("the config is refused: "));
    tool_text_append(&message, text);
    tool_complain(t, tool_text_view(&message));
    t->exit = TOOL_EXIT_REFUSED;
    return 0;
}

static int processor_add(config_build *build, processor entry) {
    for (uint32_t i = 0; i < build->processor_count; i++)
        if (tool_equal(build->processors[i].type, entry.type)) {
            tool_text text;
            tool_text_init(build->t, &text);
            tool_text_append(&text, ta_string("the I/O processor "));
            tool_text_append(&text, entry.type);
            tool_text_append(&text, ta_string(" is named twice"));
            usage(build->t, tool_text_view(&text));
            return 0;
        }
    build->processors[build->processor_count++] = entry;
    return 1;
}

/* The config file's members, copied but systemID and ioProcessors, which
 * are read into the build. */
static int config_members(config_build *build, const ta_cbor_document *document) {
    tool *t = build->t;
    uint32_t at = 1;
    for (uint32_t pair = 0; pair < document->tokens[0].count; pair++) {
        ta_string_view key = ta_cbor_string(document, at);
        uint32_t value = at + 1;
        at = ta_cbor_next(document, value);
        if (tool_equal(key, ta_string("systemID"))) {
            /* one that is not text, or empty, goes to core as it is: core refuses it with its error */
            ta_string_view text = ta_cbor_string(document, value);
            if (document->tokens[value].kind == TA_CBOR_TEXT && text.length) build->system = text;
            else build->system_item = value;
        } else if (tool_equal(key, ta_string("ioProcessors"))) {
            if (document->tokens[value].kind != TA_CBOR_MAP)
                return config_refuse(t, ta_string("ioProcessors is an object: each type's {\"location\"}"));
            uint32_t entry = value + 1;
            for (uint32_t i = 0; i < document->tokens[value].count; i++) {
                ta_string_view type = ta_cbor_string(document, entry);
                uint32_t fields = entry + 1, location = ta_cbor_member(document, fields, ta_string("location"));
                const ta_cbor_token *token = &document->tokens[fields];
                if (token->kind != TA_CBOR_MAP || token->count > (location != TA_NIL ? 1u : 0u) ||
                    (location != TA_NIL && document->tokens[location].kind != TA_CBOR_TEXT))
                    return config_refuse(t, ta_string("an I/O processor is an object with a location, a string, at most"));
                if (!processor_add(build, (processor){type, location != TA_NIL ? ta_cbor_string(document, location)
                                                                                : ta_string("")}))
                    return 0;
                entry = ta_cbor_next(document, fields);
            }
        } else {
            ta_cbor_write_text(&build->writer, key);
            ta_rpc_write_item(&build->writer, (ta_rpc_item){document, value, 0});
        }
    }
    return 1;
}

/* The host config, as CBOR in the invocation's arena: the file's (JSON,
 * ioProcessors a map), the system id and the I/O processors of the command
 * line. 0 (told, t->exit set) when it cannot be made. */
static int config_make(tool *t, ta_bytes *out) {
    config_build build;
    ta_memset(&build, 0, sizeof build);
    build.t = t;
    build.system_item = TA_NIL;
    build.allocator = ta_growable_arena_allocator(t->arena);
    ta_cbor_writer_init(&build.writer, (ta_buffer){0, 0, 0}, &build.allocator);
    ta_cbor_document document = {{0, 0}, 0, 0, 0};
    const ta_cli_value *file = tool_last(t, TOOL_RUN_CONFIG);
    if (file) {
        ta_bytes bytes;
        tool_read read = {file->text, t->arena};
        if (t->platform->read(t->platform->context, &read, &bytes) != TA_OK) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, ta_string("cannot read "));
            tool_text_append(&text, file->text);
            tool_complain(t, tool_text_view(&text));
            t->exit = TOOL_EXIT_FAILURE;
            return 0;
        }
        ta_cbor_writer json;
        ta_cbor_writer_init(&json, (ta_buffer){0, 0, 0}, &build.allocator);
        ta_json_reading reading = {{(const char *)bytes.pointer, (uint32_t)bytes.length}, &json, t->arena, {0, {"", 0}}};
        ta_status status = ta_json_read(&reading);
        ta_bytes cbor;
        if (status == TA_OK) status = ta_cbor_writer_bytes(&json, &cbor);
        if (status == TA_OK) status = ta_rpc_decode(&(ta_rpc_decoding){cbor, t->arena}, &document);
        if (status == TA_ERROR_NO_MEMORY) {
            tool_complain(t, ta_string("out of memory"));
            t->exit = TOOL_EXIT_FAILURE;
            return 0;
        }
        if (status != TA_OK) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, ta_string("it is not JSON: "));
            tool_text_append(&text, reading.failure.reason);
            tool_text_append(&text, ta_string(" at byte "));
            tool_text_number(&text, reading.failure.offset);
            return config_refuse(t, tool_text_view(&text));
        }
        if (document.tokens[0].kind != TA_CBOR_MAP) return config_refuse(t, ta_string("it is not a JSON object"));
    }
    uint32_t configured = document.count ? document.tokens[0].count : 0;
    uint32_t capacity = configured + tool_count(t, TOOL_RUN_IO_PROCESSOR) + 1;
    build.processors = (processor *)ta_growable_arena_alloc(t->arena, sizeof *build.processors * capacity, RUN_ALIGN);
    if (!build.processors) {
        tool_complain(t, ta_string("out of memory"));
        t->exit = TOOL_EXIT_FAILURE;
        return 0;
    }
    ta_cbor_write_map(&build.writer, TA_CBOR_COUNT_LATER);
    if (document.count && !config_members(&build, &document)) return 0;
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != TOOL_RUN_IO_PROCESSOR) continue;
        uint32_t equals = 0;
        while (equals < value->text.length && value->text.pointer[equals] != '=')
            equals++;
        processor entry = {{value->text.pointer, equals}, ta_string("")};
        if (equals < value->text.length)
            entry.location = (ta_string_view){value->text.pointer + equals + 1, value->text.length - equals - 1};
        if (!processor_add(&build, entry)) return 0;
    }
    const ta_cli_value *system = tool_last(t, TOOL_RUN_SYSTEM);
    if (system && (build.system_item != TA_NIL || (build.system.length && !tool_equal(system->text, build.system)))) {
        usage(t, ta_string("--system names another system than the config's systemID"));
        return 0;
    }
    ta_cbor_write_text(&build.writer, ta_string("systemID"));
    if (build.system_item != TA_NIL)
        ta_rpc_write_item(&build.writer, (ta_rpc_item){&document, build.system_item, 0});
    else
        ta_cbor_write_text(&build.writer, system ? system->text : build.system.length ? build.system : run_system);
    if (build.processor_count) {
        ta_cbor_write_text(&build.writer, ta_string("ioProcessors"));
        ta_cbor_write_array(&build.writer, build.processor_count);
        for (uint32_t i = 0; i < build.processor_count; i++) {
            ta_cbor_write_map(&build.writer, build.processors[i].location.length ? 2u : 1u);
            ta_cbor_write_text(&build.writer, ta_string("type"));
            ta_cbor_write_text(&build.writer, build.processors[i].type);
            if (build.processors[i].location.length) {
                ta_cbor_write_text(&build.writer, ta_string("location"));
                ta_cbor_write_text(&build.writer, build.processors[i].location);
            }
        }
    }
    ta_cbor_write_end(&build.writer);
    ta_status status = ta_cbor_writer_bytes(&build.writer, out);
    if (status == TA_ERROR_NO_MEMORY) {
        tool_complain(t, ta_string("out of memory"));
        t->exit = TOOL_EXIT_FAILURE;
        return 0;
    }
    if (status != TA_OK) return config_refuse(t, ta_string("a key or a value is not one the host takes"));
    return 1;
}

/* ---- startup --------------------------------------------------------------------------------- */

/* The datamodel's limits from the command line, a seed and the host's clock. */
static int datamodel_make(tool *t, tool_server *server) {
    uint64_t max_ops = 0, memory_limit = 0, seed = RUN_SEED;
    if (!number_of(t, (number_option){TOOL_RUN_MAX_OPS, "--max-ops", UINT32_MAX}, &max_ops) ||
        !number_of(t, (number_option){TOOL_RUN_MEMORY_LIMIT, "--memory-limit", SIZE_MAX}, &memory_limit) ||
        !number_of(t, (number_option){TOOL_RUN_SEED, "--seed", UINT64_MAX}, &seed))
        return 0;
    const ta_cli_value *realm = tool_last(t, TOOL_RUN_REALM);
    ta_datamodel_ecmascript_config config;
    ta_memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.max_ops = (uint32_t)max_ops;
    config.memory_limit = (size_t)memory_limit;
    config.realm = realm && realm->choice == 1 ? TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED : TA_DATAMODEL_ECMASCRIPT_REALM_AUTO;
    config.random_seed = seed;
    config.clock = TA_DATAMODEL_ECMASCRIPT_CLOCK_HOST;
    server->ecmascript_config = config;
    server->ecmascript = ta_datamodel_ecmascript_new(&config);
    if (!server->ecmascript) {
        tool_complain(t, ta_string("quickjs could not start: out of memory"));
        t->exit = TOOL_EXIT_FAILURE;
        return 0;
    }
    server->datamodel = ta_datamodel_ecmascript_binding(server->ecmascript);
    return 1;
}

/* A refused config's reply ({"error": ...}) as JSON on standard error. */
static void tell_refused_config(tool *t, ta_buffer error) {
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("the config is refused: "));
    ta_json_writing writing = {{error.pointer, error.length}, &text.output, 0};
    if (!error.length || error.length > error.capacity || ta_json_write(&writing) != TA_OK)
        tool_text_append(&text, ta_string("the host does not take it"));
    tool_complain(t, tool_text_view(&text));
}

static int host_make(tool *t, tool_server *server, ta_bytes config) {
    ta_buffer error = {ta_growable_arena_alloc(t->arena, RUN_ERROR_ROOM, RUN_ALIGN), RUN_ERROR_ROOM, 0};
    if (!error.pointer) error.capacity = 0;
    ta_host_environment environment;
    ta_memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = 1;
    environment.datamodels = &server->datamodel;
    environment.allocator = t->platform->allocator;
    environment.error = &error;
    ta_status status = ta_host_create(config, &environment, &server->host);
    if (status == TA_OK) return 1;
    server->host = 0;
    if (status == TA_ERROR_NO_MEMORY) {
        tool_complain(t, ta_string("out of memory making the host"));
        t->exit = TOOL_EXIT_FAILURE;
    } else {
        tell_refused_config(t, error);
        t->exit = TOOL_EXIT_REFUSED;
    }
    return 0;
}

/* host.config, posted and drained once, for the clock's rate, the system
 * id and the I/O processors' types (effects.c reads its reply). */
static int config_read(tool *t, tool_server *server) {
    ta_growable_arena_mark mark = ta_growable_arena_position(&server->request);
    uint64_t handle = 0;
    tool_post *post = (tool_post *)ta_table_add(&server->posts, &handle);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("host.config"));
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, handle);
    ta_bytes message;
    if (!post || ta_cbor_writer_bytes(&writer, &message) != TA_OK || ta_host_post(server->host, message) != TA_OK) {
        tool_complain(t, ta_string("out of memory asking the host for its config"));
        t->exit = TOOL_EXIT_FAILURE;
        return 0;
    }
    post->kind = TOOL_POST_CONFIG;
    tool_drain(server, 0);
    server->batches = 0; /* the client's first batch is batch 1 */
    ta_growable_arena_release(&server->request, mark);
    if (!server->system.length || !server->clock.ticks_per_second) {
        tool_complain(t, ta_string("the host gave no config"));
        t->exit = TOOL_EXIT_FAILURE;
        return 0;
    }
    return 1;
}

static ta_string_view severity_word(uint32_t severity) {
    return severity == TA_DIAGNOSTIC_ERROR     ? ta_string("error")
           : severity == TA_DIAGNOSTIC_WARNING ? ta_string("warning")
                                               : ta_string("note");
}

/* The inputs' diagnostics on standard error, a line each. */
static void tell_diagnostics(tool *t, const tool_image *image) {
    for (uint32_t i = 0; i < image->diagnostic_count; i++) {
        const ta_diagnostic *diagnostic = &image->diagnostics[i];
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, diagnostic->file);
        tool_text_append(&text, ta_string(":"));
        tool_text_number(&text, diagnostic->line);
        tool_text_append(&text, ta_string(":"));
        tool_text_number(&text, diagnostic->column);
        tool_text_append(&text, ta_string(": "));
        tool_text_append(&text, severity_word(diagnostic->severity));
        tool_text_append(&text, ta_string(": "));
        tool_text_append(&text, diagnostic->message);
        tool_complain(t, tool_text_view(&text));
    }
}

/* The inputs' image as segment 0, and its boot actors with --boot. */
static int inputs_add(tool *t, tool_server *server) {
    const ta_cli_parsed *parsed = t->parsed;
    int boot = tool_count(t, TOOL_RUN_BOOT) > 0;
    if (!parsed->argument_count) {
        if (boot) usage(t, ta_string("--boot spawns the boot actors of the inputs' manifest: give one"));
        return !boot;
    }
    for (uint32_t i = 0; i < parsed->argument_count; i++)
        if (tool_equal(parsed->arguments[i], ta_string("-"))) {
            usage(t, ta_string("standard input is the client's: give the charts as files"));
            return 0;
        }
    uint32_t flags = tool_count(t, TOOL_OPTION_WARNINGS_AS_ERRORS) ? TA_COMPILE_WERROR : 0u;
    tool_image image;
    tool_image_request request = {{parsed->argument_count, parsed->arguments, flags, TA_NIL}, t->arena};
    ta_status status = tool_image_make(t, &request, &image);
    tell_diagnostics(t, &image);
    if (status != TA_OK) {
        if (image.complaint.length) tool_complain(t, image.complaint);
        t->exit = (int)image.exit;
        return 0;
    }
    uint32_t segment = 0;
    status = ta_host_add_image(server->host, image.bytes, &segment);
    if (status == TA_OK && boot) status = ta_host_boot(server->host, segment);
    if (status != TA_OK) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("the host refused the inputs' image: "));
        tool_text_append(&text, ta_string(ta_status_name(status) ? ta_status_name(status) : "unknown"));
        tool_complain(t, tool_text_view(&text));
        t->exit = status == TA_ERROR_NO_MEMORY ? TOOL_EXIT_FAILURE : TOOL_EXIT_REFUSED;
        return 0;
    }
    return 1;
}

static int server_start(tool *t, tool_server *server) {
    uint64_t wall = 0, max_message = RUN_MAX_MESSAGE;
    if (!number_of(t, (number_option){TOOL_RUN_WALL_START, "--wall-start", UINT64_MAX}, &wall) ||
        !number_of(t, (number_option){TOOL_RUN_MAX_MESSAGE, "--max-message", SIZE_MAX}, &max_message))
        return 0;
    ta_bytes config;
    if (!config_make(t, &config) || !datamodel_make(t, server)) return 0;
    const ta_allocator *allocator = &t->platform->allocator;
    ta_growable_arena_init(&server->request, &(ta_growable_arena_config){sizeof(ta_growable_arena_config), 0, 0, allocator});
    server->request_allocator = ta_growable_arena_allocator(&server->request);
    ta_table_init(&server->posts, &(ta_table_config){sizeof(ta_table_config), sizeof(tool_post), allocator});
    ta_rpc_lines_init(&server->lines, &(ta_rpc_lines_config){allocator, (size_t)max_message});
    server->clock.now = (ta_system_time){0, wall};
    server->config = config;
    if (!host_make(t, server, config) || !config_read(t, server)) return 0;
    ta_host_advance_time(server->host, server->clock.now);
    return inputs_add(t, server);
}

/* The end of input: a held batch is acked (its senders go on, as if the
 * client had taken their sends without an answer), what is pending drained
 * to the client; with an encoder, the open spans ended (trace.end) and
 * every span and log record still held written to the client; then the
 * host freed and quickjs after it. No run: what the client sees at the end
 * follows from its last message alone. */
static void server_stop(tool_server *server) {
    if (server->host) {
        ta_growable_arena_mark mark = ta_growable_arena_position(&server->request);
        tool_held_ack(server);
        tool_drain(server, 0);
        if (!server->failed) tool_telemetry_end(server);
        ta_growable_arena_release(&server->request, mark);
        ta_host_free(server->host);
    }
    if (server->ecmascript) ta_datamodel_ecmascript_free(server->ecmascript); /* after the host */
    ta_table_free(&server->posts);
    ta_rpc_lines_free(&server->lines);
    ta_growable_arena_free(&server->request);
}

/* ---- the message loop ------------------------------------------------------------------------- */

static void serve(tool *t, tool_server *server) {
    unsigned char *room = (unsigned char *)ta_growable_arena_alloc(t->arena, RUN_CHUNK, RUN_ALIGN);
    if (!room) {
        tool_complain(t, ta_string("out of memory"));
        t->exit = TOOL_EXIT_FAILURE;
        return;
    }
    for (;;) {
        ta_buffer chunk = {room, RUN_CHUNK, 0};
        if (t->platform->receive(t->platform->context, &chunk) != TA_OK) {
            tool_complain(t, ta_string("standard input could not be read"));
            t->exit = TOOL_EXIT_FAILURE;
            return;
        }
        if (ta_rpc_lines_feed(&server->lines, (ta_bytes){room, chunk.length}) != TA_OK) {
            tool_complain(t, ta_string("out of memory reading standard input"));
            t->exit = TOOL_EXIT_FAILURE;
            return;
        }
        ta_rpc_line line;
        while (!server->failed && ta_rpc_lines_next(&server->lines, &line))
            serve_line(server, &line);
        if (server->failed) {
            t->exit = TOOL_EXIT_FAILURE; /* the client went away */
            return;
        }
        if (!chunk.length) return; /* the end of input */
    }
}

void tool_serve(tool *t) {
    t->answered = 1; /* the protocol is the answer: no report */
    if (tool_count(t, TOOL_RUN_SCHEMA)) {
        tool_write request = {TOOL_STANDARD_OUTPUT, {0, 0}, tool_schema_text()};
        if (t->platform->write(t->platform->context, &request) != TA_OK) t->exit = TOOL_EXIT_FAILURE;
        return;
    }
    tool_server server;
    ta_memset(&server, 0, sizeof server);
    server.invocation = t;
    if (server_start(t, &server)) serve(t, &server);
    server_stop(&server);
}
