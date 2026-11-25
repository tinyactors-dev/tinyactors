/* serve.c — the run server's methods (serve.h). A run.* method is a host
 * call, its params the call's struct fields in camelCase; every other
 * method is forwarded to ta_host_post and answered with core's reply. The
 * params of every method the protocol's schema has are checked against it
 * first (schema.h), so the methods below read params of the types the
 * schema gives them. */
#include "serve.h"

#include <tinyactors/address.h>

#include "base/memory.h"
#include "cli/json.h"
#include "core/core_internal.h"
#include "schema_run.h"

typedef enum serve_constants {
    SERVE_NANOSECONDS = 1000000000,
    SERVE_ALIGN = 8
} serve_constants;

/* ---- errors -------------------------------------------------------------------------- */

void tool_serve_refuse(tool_call *call, tool_serve_refusal refusal) {
    call->error.code = refusal.code;
    call->error.reason = ta_string(refusal.reason);
    call->error.path = refusal.path;
    call->error.message = refusal.message;
    call->error.status = ta_string("");
}

void tool_serve_status(tool_call *call, ta_status status) {
    static const struct {
        ta_status status;
        int32_t code;
        const char *reason, *message;
    } errors[] = {
        {TA_ERROR_STALE, TA_RPC_STALE, "stale", "the session or name does not exist (any more)"},
        {TA_ERROR_STATE, TA_RPC_STATE, "state", "the host cannot do it now"},
        {TA_ERROR_LIMIT, TA_RPC_LIMIT, "limit", "more than the host holds"},
        {TA_ERROR_NO_MEMORY, TA_RPC_MEMORY, "memory", "out of memory"},
        {TA_ERROR_INVALID, TA_RPC_INVALID, "invalid", "the host refused it"},
        {TA_ERROR_UNSUPPORTED, TA_RPC_UNSUPPORTED, "unsupported", "a value outside the value table"},
    };
    tool_serve_refusal refusal = {TA_RPC_INTERNAL_ERROR, 0, "internal", ta_string(""),
                                  ta_string("a refusal no call documents: please report it")};
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
        if (errors[i].status == status)
            refusal = (tool_serve_refusal){errors[i].code, 0, errors[i].reason, ta_string(""),
                                           ta_string(errors[i].message)};
    tool_serve_refuse(call, refusal);
    const char *name = ta_status_name(status);
    call->error.status = ta_string(name ? name : "");
}

/* The same, with the param it is about. */
static void refused_at(tool_call *call, ta_status status, const char *path) {
    tool_serve_status(call, status);
    call->error.path = ta_string(path);
}

static void invalid_param(tool_call *call, const char *reason, const char *path, const char *message) {
    tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INVALID_PARAMS, 0, reason, ta_string(path), ta_string(message)});
}

/* ---- params ---------------------------------------------------------------------------- */

static const ta_cbor_document *document_of(const tool_call *call) {
    return &call->message->document;
}

/* A param's token; TA_NIL when absent. */
static uint32_t param(const tool_call *call, const char *name) {
    if (call->message->params == TA_NIL) return TA_NIL;
    return ta_cbor_member(document_of(call), call->message->params, ta_string(name));
}

static const ta_cbor_token *token(const tool_call *call, uint32_t at) {
    return &document_of(call)->tokens[at];
}

static ta_string_view text_of(const tool_call *call, uint32_t at) {
    return at == TA_NIL ? ta_string("") : ta_cbor_string(document_of(call), at);
}

/* An unsigned param, or `otherwise` when absent (the schema checked its type and bounds). */
static uint64_t number_of(const tool_call *call, const char *name, uint64_t otherwise) {
    uint32_t at = param(call, name);
    return at == TA_NIL ? otherwise : token(call, at)->value;
}

static int flag_of(const tool_call *call, const char *name) {
    uint32_t at = param(call, name);
    return at != TA_NIL && token(call, at)->kind == TA_CBOR_SIMPLE && token(call, at)->value == TA_CBOR_TRUE;
}

static int refuse_present(tool_call *call, const char *name, const char *path, const char *message) {
    if (param(call, name) == TA_NIL) return 0;
    invalid_param(call, "unknown", path, message);
    return 1;
}

/* ---- result members ------------------------------------------------------------------- */

static void result_number(tool_call *call, const char *name, uint64_t value) {
    ta_cbor_write_text(&call->result, ta_string(name));
    ta_cbor_write_uint(&call->result, value);
}

static void result_text(tool_call *call, const char *name, ta_string_view text) {
    ta_cbor_write_text(&call->result, ta_string(name));
    ta_cbor_write_text(&call->result, text);
}

/* ---- values in ------------------------------------------------------------------------- */

/* A value param ("data" or "cbor", "params" or "paramsCBOR") as a payload of
 * the datamodel of the chart that reads it. */
typedef struct value_param {
    const char *json_name, *cbor_name;
    const char *json_path, *cbor_path;
    uint32_t segment, chart;
} value_param;

static ta_status value_read(tool_call *call, const value_param *value, ta_payload *out) {
    tool_server *server = call->server;
    *out = 0;
    uint32_t json = param(call, value->json_name), cbor = param(call, value->cbor_name);
    if (json == TA_NIL && cbor == TA_NIL) return TA_OK;
    const char *path = json != TA_NIL ? value->json_path : value->cbor_path;
    ta_host_segment_info info;
    ta_memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (ta_host_segment_info_get(server->host, value->segment, &info) != TA_OK || !info.image) {
        refused_at(call, TA_ERROR_STALE, path);
        return TA_ERROR_STALE;
    }
    if (!tool_equal(ta_image_chart_datamodel(info.image, value->chart), ta_string("ecmascript"))) {
        invalid_param(call, "range", path, "the chart's datamodel reads no values");
        return TA_ERROR_INVALID;
    }
    ta_bytes bytes;
    if (json != TA_NIL) {
        ta_cbor_writer writer;
        ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
        ta_rpc_write_item(&writer, (ta_rpc_item){document_of(call), json, 0});
        if (ta_cbor_writer_bytes(&writer, &bytes) != TA_OK) {
            refused_at(call, TA_ERROR_NO_MEMORY, path);
            return TA_ERROR_NO_MEMORY;
        }
    } else {
        ta_string_view text = text_of(call, cbor);
        size_t capacity = (size_t)text.length / 4 * 3 + 3;
        void *room = ta_growable_arena_alloc(&server->request, capacity, SERVE_ALIGN);
        ta_buffer buffer = {room, capacity, 0};
        if (!room) {
            refused_at(call, TA_ERROR_NO_MEMORY, path);
            return TA_ERROR_NO_MEMORY;
        }
        if (ta_json_base64url_read(text, &buffer) != TA_OK) {
            tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_MALFORMED, 0, "malformed", ta_string(path),
                                                         ta_string("not base64url text")});
            return TA_ERROR_INVALID;
        }
        bytes = (ta_bytes){buffer.pointer, buffer.length};
    }
    const ta_datamodel_vtable *vtable = server->datamodel.vtable;
    ta_status status = vtable->from_wire(server->datamodel.datamodel, bytes, out);
    if (status == TA_ERROR_NO_MEMORY) {
        refused_at(call, status, path);
    } else if (status != TA_OK) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_MALFORMED, 0, "malformed", ta_string(path),
                                                     ta_string("the datamodel cannot read this value")});
    }
    if (status != TA_OK) *out = 0;
    return status;
}

static void value_release(tool_server *server, ta_payload payload) {
    if (payload) server->datamodel.vtable->release(server->datamodel.datamodel, payload);
}

/* ---- run.addImage ---------------------------------------------------------------------- */

static ta_string_view severity_name(uint32_t severity) {
    return severity == TA_DIAGNOSTIC_ERROR     ? ta_string("error")
           : severity == TA_DIAGNOSTIC_WARNING ? ta_string("warning")
                                               : ta_string("note");
}

/* The image's diagnostics as one message: "file:line:column: severity: text; ...". */
static ta_string_view diagnostics_text(tool_server *server, const tool_image *image) {
    tool_text text;
    tool_text_over(&text, &server->request);
    tool_text_append(&text, image->complaint.length ? image->complaint : ta_string("the inputs make no image"));
    for (uint32_t i = 0; i < image->diagnostic_count; i++) {
        const ta_diagnostic *diagnostic = &image->diagnostics[i];
        tool_text_append(&text, i ? ta_string("; ") : ta_string(": "));
        tool_text_append(&text, diagnostic->file);
        tool_text_append(&text, ta_string(":"));
        tool_text_number(&text, diagnostic->line);
        tool_text_append(&text, ta_string(":"));
        tool_text_number(&text, diagnostic->column);
        tool_text_append(&text, ta_string(": "));
        tool_text_append(&text, severity_name(diagnostic->severity));
        tool_text_append(&text, ta_string(": "));
        tool_text_append(&text, diagnostic->message);
    }
    return tool_text_view(&text);
}

/* The image of `sources`: charts given as text, a src naming another by its name. */
static ta_status image_of_sources(tool_call *call, uint32_t list, uint32_t flags, tool_image *out) {
    tool_server *server = call->server;
    const ta_cbor_document *document = document_of(call);
    uint32_t count = document->tokens[list].count;
    ta_source *sources = (ta_source *)ta_growable_arena_alloc(&server->request, sizeof *sources * count, SERVE_ALIGN);
    if (!sources) return TA_ERROR_NO_MEMORY;
    uint32_t at = list + 1;
    for (uint32_t i = 0; i < count; i++) {
        sources[i].name = ta_cbor_string(document, ta_cbor_member(document, at, ta_string("name")));
        sources[i].text = ta_cbor_string(document, ta_cbor_member(document, at, ta_string("text")));
        at = ta_cbor_next(document, at);
    }
    ta_source_list list_of = {sources, count};
    uint32_t kind = tool_input_kind_of((ta_bytes){sources[0].text.pointer, sources[0].text.length});
    if (kind != TOOL_INPUT_MANIFEST) kind = TOOL_INPUT_CHART;
    tool_compilation compilation = {kind, flags, list_of, &server->request, &list_of};
    ta_status status = tool_compile_sources(server->invocation, &compilation, out);
    if (status != TA_OK) return status;
    if (out->status == TA_OK && (out->status = ta_image_open(out->bytes, &out->image)) == TA_OK) out->opened = 1;
    return TA_OK;
}

/* The image of `image`: a compiled image as base64url text. */
static ta_status image_of_bytes(tool_call *call, uint32_t at, tool_image *out) {
    tool_server *server = call->server;
    ta_string_view text = text_of(call, at);
    size_t capacity = (size_t)text.length / 4 * 3 + 3;
    void *room = ta_growable_arena_alloc(&server->request, capacity, SERVE_ALIGN);
    if (!room) return TA_ERROR_NO_MEMORY;
    ta_buffer buffer = {room, capacity, 0};
    if (ta_json_base64url_read(text, &buffer) != TA_OK) {
        out->status = TA_ERROR_INVALID;
        out->complaint = ta_string("the image is not base64url text");
        return TA_OK;
    }
    out->bytes = (ta_bytes){buffer.pointer, buffer.length};
    out->status = ta_image_open(out->bytes, &out->image);
    if (out->status != TA_OK) out->complaint = ta_string("the image does not verify: it is damaged, or of another format version");
    else out->opened = 1;
    return TA_OK;
}

static void add_image(tool_call *call) {
    tool_server *server = call->server;
    uint32_t files = param(call, "files"), sources = param(call, "sources"), bytes = param(call, "image");
    uint32_t flags = flag_of(call, "warningsAsErrors") ? TA_COMPILE_WERROR : 0u;
    const char *path = files != TA_NIL ? "params.files" : sources != TA_NIL ? "params.sources" : "params.image";
    tool_image image;
    ta_memset(&image, 0, sizeof image);
    ta_status status;
    if (files != TA_NIL) {
        const ta_cbor_document *document = document_of(call);
        uint32_t count = document->tokens[files].count;
        ta_string_view *paths =
            (ta_string_view *)ta_growable_arena_alloc(&server->request, sizeof *paths * count, SERVE_ALIGN);
        if (!paths) {
            refused_at(call, TA_ERROR_NO_MEMORY, path);
            return;
        }
        for (uint32_t i = 0; i < count; i++)
            paths[i] = ta_cbor_string(document, files + 1 + i);
        tool_image_request request = {{count, paths, flags, TA_NIL}, &server->request};
        status = tool_image_make(server->invocation, &request, &image);
        if (status == TA_ERROR_NO_MEMORY) {
            refused_at(call, status, path);
            return;
        }
        status = TA_OK;
    } else if (sources != TA_NIL) {
        status = image_of_sources(call, sources, flags, &image);
    } else {
        status = image_of_bytes(call, bytes, &image);
    }
    if (status != TA_OK) {
        refused_at(call, status, path);
        return;
    }
    if (!image.opened) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INVALID, 0, "invalid", ta_string(path),
                                                     diagnostics_text(server, &image)});
        call->error.status = ta_string(ta_status_name(image.status) ? ta_status_name(image.status) : "");
        return;
    }
    uint32_t segment = 0;
    status = ta_host_add_image(server->host, image.bytes, &segment);
    if (status != TA_OK) {
        refused_at(call, status, path);
        return;
    }
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&image.image, &info);
    result_number(call, "segment", segment);
    ta_cbor_writer *result = &call->result;
    ta_cbor_write_text(result, ta_string("charts"));
    ta_cbor_write_array(result, info.chart_count);
    for (uint32_t chart = 0; chart < info.chart_count; chart++) {
        ta_cbor_write_map(result, 2);
        ta_cbor_write_text(result, ta_string("name"));
        ta_cbor_write_text(result, ta_image_chart_name(&image.image, chart));
        ta_cbor_write_text(result, ta_string("chart"));
        ta_cbor_write_uint(result, chart);
    }
    ta_cbor_write_text(result, ta_string("diagnostics"));
    ta_cbor_write_array(result, image.diagnostic_count);
    for (uint32_t i = 0; i < image.diagnostic_count; i++) {
        const ta_diagnostic *diagnostic = &image.diagnostics[i];
        ta_cbor_write_map(result, 5);
        ta_cbor_write_text(result, ta_string("file"));
        ta_cbor_write_text(result, diagnostic->file);
        ta_cbor_write_text(result, ta_string("line"));
        ta_cbor_write_uint(result, diagnostic->line);
        ta_cbor_write_text(result, ta_string("column"));
        ta_cbor_write_uint(result, diagnostic->column);
        ta_cbor_write_text(result, ta_string("severity"));
        ta_cbor_write_text(result, severity_name(diagnostic->severity));
        ta_cbor_write_text(result, ta_string("message"));
        ta_cbor_write_text(result, diagnostic->message);
    }
}

/* ---- run.spawn -------------------------------------------------------------------------- */

static void spawn(tool_call *call) {
    tool_server *server = call->server;
    uint32_t chart = param(call, "chart"), segment = param(call, "segment");
    ta_host_chart_ref ref = {0, 0};
    if (token(call, chart)->kind == TA_CBOR_TEXT) {
        if (refuse_present(call, "segment", "params.segment", "segment goes with a chart's index, not its name"))
            return;
        ta_status status = ta_host_chart(server->host, text_of(call, chart), &ref);
        if (status != TA_OK) {
            refused_at(call, status, "params.chart");
            return;
        }
    } else {
        if (segment == TA_NIL) {
            invalid_param(call, "missing", "params.segment", "a chart's index needs its segment");
            return;
        }
        ref.segment = (uint32_t)token(call, segment)->value;
        ref.chart = (uint32_t)token(call, chart)->value;
    }
    ta_spawn request;
    ta_memset(&request, 0, sizeof request);
    request.segment = ref.segment;
    request.chart = ref.chart;
    request.name = text_of(call, param(call, "name"));
    request.traceparent = text_of(call, param(call, "traceparent"));
    value_param params = {"params", "paramsCBOR", "params.params", "params.paramsCBOR", ref.segment, ref.chart};
    if (value_read(call, &params, &request.params) != TA_OK) return;
    uint64_t session = 0;
    ta_status status = ta_host_spawn(server->host, &request, &session);
    if (status != TA_OK) {
        value_release(server, request.params);
        refused_at(call, status, "params.chart");
        return;
    }
    char address[TA_ADDRESS_SESSION_MAX];
    ta_buffer buffer = {address, sizeof address, 0};
    result_number(call, "session", session);
    if (ta_address_format(server->system, session, &buffer) == TA_OK)
        result_text(call, "address", (ta_string_view){address, (uint32_t)buffer.length});
}

/* ---- run.send ----------------------------------------------------------------------------- */

/* The session a target names: a session, or a local address. */
static ta_status target_of(tool_call *call, uint64_t *session) {
    uint32_t at = param(call, "target");
    if (token(call, at)->kind == TA_CBOR_UINT) {
        *session = token(call, at)->value;
        return TA_OK;
    }
    return ta_host_resolve(call->server->host, text_of(call, at), session);
}

static void send(tool_call *call) {
    tool_server *server = call->server;
    if (refuse_present(call, "invocation", "params.invocation", "services the client runs are not served yet") ||
        refuse_present(call, "done", "params.done", "services the client runs are not served yet"))
        return;
    if (param(call, "event") == TA_NIL) {
        invalid_param(call, "missing", "params.event", "an event has a name");
        return;
    }
    uint64_t session = 0;
    ta_status status = target_of(call, &session);
    ta_host_location location;
    if (status == TA_OK) status = ta_host_locate(server->host, session, &location);
    if (status != TA_OK) {
        refused_at(call, status, "params.target");
        return;
    }
    ta_actor_info info;
    ta_memset(&info, 0, sizeof info);
    info.size = sizeof info;
    status = ta_actor_info_get(ta_host_system(server->host, location.segment), location.actor, &info);
    if (status != TA_OK) {
        refused_at(call, status, "params.target");
        return;
    }
    ta_event_in event;
    ta_memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = session;
    event.name = text_of(call, param(call, "event"));
    event.sendid = text_of(call, param(call, "sendid"));
    event.origin = text_of(call, param(call, "origin"));
    event.origintype = text_of(call, param(call, "origintype"));
    event.raw = text_of(call, param(call, "raw"));
    event.traceparent = text_of(call, param(call, "traceparent"));
    value_param data = {"data", "cbor", "params.data", "params.cbor", location.segment, info.chart};
    if (value_read(call, &data, &event.data) != TA_OK) return;
    status = ta_host_send(server->host, &event);
    if (status != TA_OK) {
        value_release(server, event.data);
        refused_at(call, status, "params");
        return;
    }
}

/* ---- run.advanceTime -------------------------------------------------------------------- */

/* Nanoseconds as the host's ticks, as core converts a delay (saturating). */
static uint64_t ticks_of(const tool_server *server, uint64_t nanoseconds) {
    uint64_t rate = server->clock.ticks_per_second, seconds = nanoseconds / SERVE_NANOSECONDS;
    if (seconds > (UINT64_MAX - rate) / rate) return UINT64_MAX;
    return seconds * rate + (nanoseconds % SERVE_NANOSECONDS) * rate / SERVE_NANOSECONDS;
}

/* Ticks as nanoseconds (saturating). */
static uint64_t nanoseconds_of(const tool_server *server, uint64_t ticks) {
    uint64_t rate = server->clock.ticks_per_second, seconds = ticks / rate, rest = ticks % rate;
    if (seconds > UINT64_MAX / SERVE_NANOSECONDS) return UINT64_MAX;
    uint64_t whole = seconds * SERVE_NANOSECONDS;
    uint64_t part = rest > UINT64_MAX / SERVE_NANOSECONDS ? rest / rate * SERVE_NANOSECONDS : rest * SERVE_NANOSECONDS / rate;
    return part > UINT64_MAX - whole ? UINT64_MAX : whole + part;
}

/* A time param: ticks, or a duration as SCXML writes one; its nanoseconds too. */
typedef struct time_value {
    uint64_t ticks, nanoseconds;
} time_value;

static int time_of(tool_call *call, uint32_t at, time_value *out) {
    if (token(call, at)->kind == TA_CBOR_UINT) {
        out->ticks = token(call, at)->value;
        out->nanoseconds = nanoseconds_of(call->server, out->ticks);
        return 1;
    }
    if (!ta_core_parse_duration(text_of(call, at), &out->nanoseconds)) return 0;
    out->ticks = ticks_of(call->server, out->nanoseconds);
    return 1;
}

static void advance_time(tool_call *call) {
    tool_server *server = call->server;
    ta_system_time now = server->clock.now;
    uint32_t by = param(call, "by"), to = param(call, "to"), wall = param(call, "wallNs");
    time_value moved = {0, 0};
    if (by != TA_NIL || to != TA_NIL) {
        const char *path = by != TA_NIL ? "params.by" : "params.to";
        if (!time_of(call, by != TA_NIL ? by : to, &moved)) {
            invalid_param(call, "range", path, "not a duration");
            return;
        }
        if (to != TA_NIL) {
            if (moved.ticks < now.monotonic) {
                invalid_param(call, "range", path, "the clock does not go back");
                return;
            }
            moved.ticks -= now.monotonic;
            moved.nanoseconds = nanoseconds_of(server, moved.ticks);
        }
        if (moved.ticks > UINT64_MAX - now.monotonic) {
            invalid_param(call, "range", path, "past the end of the clock");
            return;
        }
        now.monotonic += moved.ticks;
        now.wall_ns = moved.nanoseconds > UINT64_MAX - now.wall_ns ? UINT64_MAX : now.wall_ns + moved.nanoseconds;
    }
    if (wall != TA_NIL) {
        if (token(call, wall)->value < server->clock.now.wall_ns) {
            invalid_param(call, "range", "params.wallNs", "the wall clock does not go back");
            return;
        }
        now.wall_ns = token(call, wall)->value;
    }
    server->clock.now = now;
    ta_host_advance_time(server->host, now);
    result_number(call, "time", now.monotonic);
    result_number(call, "wallNs", now.wall_ns);
}

/* ---- run.turn ------------------------------------------------------------------------------ */

static ta_string_view pool_name(uint32_t pool) {
    static const ta_string_view names[] = {
        [TA_POOL_NONE] = TA_CLI_TEXT("none"),     [TA_POOL_EVENTS] = TA_CLI_TEXT("events"),
        [TA_POOL_SLAB] = TA_CLI_TEXT("slab"),     [TA_POOL_TIMERS] = TA_CLI_TEXT("timers"),
        [TA_POOL_ACTORS] = TA_CLI_TEXT("actors"), [TA_POOL_REQUESTS] = TA_CLI_TEXT("requests")};
    return pool < sizeof names / sizeof names[0] ? names[pool] : ta_string("unknown");
}

static void turn(tool_call *call) {
    tool_server *server = call->server;
    if (flag_of(call, "hold")) {
        invalid_param(call, "unknown", "params.hold", "hold is not served yet");
        return;
    }
    uint32_t budget = (uint32_t)number_of(call, "budget", TOOL_SERVE_BUDGET);
    uint64_t allowed = number_of(call, "runs", TOOL_SERVE_RUNS);
    tool_drain(server);
    ta_run run = {TA_RUN_IDLE, 0, 0, 0, UINT64_MAX};
    uint32_t runs = 0;
    uint64_t fuel = 0;
    ta_string_view status = ta_string("yielded"), detail = ta_string("");
    while (runs < allowed && !server->failed) {
        ta_host_advance_time(server->host, server->clock.now); /* before every run */
        run = ta_host_run(server->host, budget);
        runs++;
        fuel += run.used;
        server->woke = 0;
        tool_drain(server);
        if (run.status == TA_RUN_EFFECTS || run.status == TA_RUN_EFFECTS_FULL || run.status == TA_RUN_YIELDED ||
            (run.status == TA_RUN_IDLE && server->woke))
            continue;
        if (run.status == TA_RUN_IDLE) {
            status = ta_string("idle");
        } else if (run.status == TA_RUN_EXHAUSTED) {
            status = ta_string("exhausted");
            detail = pool_name(run.detail);
        } else {
            const char *name = run.status == TA_RUN_FAULT ? ta_status_name((ta_status)(int32_t)run.detail) : 0;
            status = ta_string("fault");
            detail = ta_string(name ? name : "TA_ERROR_UNSUPPORTED");
        }
        break;
    }
    result_text(call, "status", status);
    if (detail.length) result_text(call, "detail", detail);
    result_number(call, "runs", runs);
    result_number(call, "fuel", fuel);
    result_number(call, "time", server->clock.now.monotonic);
    ta_cbor_write_text(&call->result, ta_string("nextDeadline"));
    if (run.next_deadline == UINT64_MAX) ta_cbor_write_simple(&call->result, TA_CBOR_NULL);
    else ta_cbor_write_uint(&call->result, run.next_deadline);
}

/* ---- run.schema ----------------------------------------------------------------------------- */

static void schema(tool_call *call) {
    tool_server *server = call->server;
    ta_bytes text = tool_schema_text();
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_json_reading reading = {{(const char *)text.pointer, (uint32_t)text.length}, &writer, &server->request, {0, {"", 0}}};
    ta_status status = ta_json_read(&reading);
    ta_bytes cbor;
    ta_cbor_document document;
    if (status == TA_OK) status = ta_cbor_writer_bytes(&writer, &cbor);
    if (status == TA_OK) status = ta_rpc_decode(&(ta_rpc_decoding){cbor, &server->request}, &document);
    if (status != TA_OK) {
        tool_serve_status(call, status);
        return;
    }
    ta_cbor_write_text(&call->result, ta_string("schema"));
    ta_rpc_write_item(&call->result, (ta_rpc_item){&document, 0, 0});
}

/* ---- forwarding ------------------------------------------------------------------------------ */

static void forward(tool_call *call) {
    tool_server *server = call->server;
    uint64_t handle = 0;
    int answered = call->message->kind == TA_RPC_REQUEST;
    if (answered) {
        tool_post *post = (tool_post *)ta_table_add(&server->posts, &handle);
        if (!post) {
            tool_serve_status(call, TA_ERROR_NO_MEMORY);
            return;
        }
        post->kind = TOOL_POST_FORWARDED;
        post->call = call;
    }
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_rpc_forward_write(&(ta_rpc_forward){call->message, handle}, &writer);
    ta_bytes message;
    ta_status status = ta_cbor_writer_bytes(&writer, &message);
    if (status == TA_OK) status = ta_host_post(server->host, message);
    if (status != TA_OK) {
        if (answered) ta_table_remove(&server->posts, handle);
        tool_serve_status(call, status);
        return;
    }
    tool_drain(server);
    if (!answered) return;
    tool_post *post = (tool_post *)ta_table_get(&server->posts, handle);
    if (post) { /* no reply came: the post is no longer this call's */
        post->call = 0;
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INTERNAL_ERROR, 0, "internal", ta_string(""),
                                                     ta_string("the host gave no reply: please report it")});
    }
}

/* ---- the methods ----------------------------------------------------------------------------- */

typedef void (*tool_method_run)(tool_call *call);
typedef struct tool_method {
    ta_string_view name;
    tool_method_run run;
} tool_method;

static const tool_method methods[] = {
    {TA_CLI_TEXT("run.addImage"), add_image},     {TA_CLI_TEXT("run.spawn"), spawn},
    {TA_CLI_TEXT("run.send"), send},              {TA_CLI_TEXT("run.advanceTime"), advance_time},
    {TA_CLI_TEXT("run.turn"), turn},              {TA_CLI_TEXT("run.schema"), schema},
};

static int is_server_area(ta_string_view name) {
    static const ta_string_view area = TA_CLI_TEXT("run.");
    return name.length >= area.length && ta_memcmp(name.pointer, area.pointer, area.length) == 0;
}

void tool_serve_call(tool_call *call) {
    tool_server *server = call->server;
    const ta_rpc_message *message = call->message;
    ta_string_view name = ta_cbor_string(&message->document, message->method);
    /* the params of a method the schema has, checked against it */
    uint32_t described = tool_schema_method_find(&tool_schema_run, name);
    if (described < tool_schema_run.method_count) {
        tool_schema_input input = {&tool_schema_run, &message->document, message->params, 0, ta_string("params"),
                                   &server->request};
        tool_schema_error error;
        ta_status status = tool_schema_check_params(described, &input, &error);
        if (status == TA_ERROR_INVALID) {
            call->error = (ta_rpc_error){error.code, 0, error.reason, error.path, error.message, ta_string("")};
            return;
        }
        if (status != TA_OK) {
            tool_serve_status(call, status);
            return;
        }
    }
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        if (tool_equal(methods[i].name, name)) {
            methods[i].run(call);
            return;
        }
    if (is_server_area(name)) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_METHOD_NOT_FOUND, 0, "unknown", ta_string("method"),
                                                     ta_string("no such method of the server's")});
        return;
    }
    forward(call);
}
