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

/* The same, with the param it is about (a path the request arena could
 * not hold, NULL: -32004). */
static void refused_at(tool_call *call, ta_status status, ta_string_view path) {
    tool_serve_status(call, path.pointer ? status : TA_ERROR_NO_MEMORY);
    if (path.pointer) call->error.path = path;
}

static void invalid_param(tool_call *call, const char *reason, ta_string_view path, const char *message) {
    if (!path.pointer) {
        tool_serve_status(call, TA_ERROR_NO_MEMORY);
        return;
    }
    tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INVALID_PARAMS, 0, reason, path, ta_string(message)});
}

/* ---- params ---------------------------------------------------------------------------- */

static const ta_cbor_document *document_of(const tool_call *call) {
    return &call->message->document;
}

/* Where params are read: an object of the message (TA_NIL: none), and the
 * path that names it in a refusal ("params", "params.events[2]"). */
typedef struct params_at {
    uint32_t object;
    uint32_t pad;
    ta_string_view path;
} params_at;

static params_at params_of(const tool_call *call) {
    return (params_at){call->message->params, 0, ta_string("params")};
}

/* A member's token; TA_NIL when absent. */
static uint32_t member_of(const tool_call *call, const params_at *at, const char *name) {
    if (at->object == TA_NIL) return TA_NIL;
    return ta_cbor_member(document_of(call), at->object, ta_string(name));
}

/* A member's path, "<at>.<name>", in the request arena (NULL: no room). */
static ta_string_view path_of(tool_call *call, const params_at *at, const char *name) {
    tool_text text;
    tool_text_over(&text, &call->server->request);
    tool_text_append(&text, at->path);
    tool_text_append(&text, ta_string("."));
    tool_text_append(&text, ta_string(name));
    return tool_text_view(&text);
}

/* A param's token; TA_NIL when absent. */
static uint32_t param(const tool_call *call, const char *name) {
    params_at at = params_of(call);
    return member_of(call, &at, name);
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

static int is_true(const tool_call *call, uint32_t at) {
    return at != TA_NIL && token(call, at)->kind == TA_CBOR_SIMPLE && token(call, at)->value == TA_CBOR_TRUE;
}

static int flag_of(const tool_call *call, const char *name) {
    return is_true(call, param(call, name));
}

static int refuse_present(tool_call *call, const char *name, const char *path, const char *message) {
    if (param(call, name) == TA_NIL) return 0;
    invalid_param(call, "unknown", ta_string(path), message);
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

/* A base64url param's bytes, decoded into the request arena: TA_OK;
 * TA_ERROR_INVALID: not base64url text; TA_ERROR_NO_MEMORY. */
static ta_status bytes_of(tool_call *call, uint32_t at, ta_bytes *out) {
    ta_string_view text = text_of(call, at);
    size_t capacity = (size_t)text.length / 4 * 3 + 3;
    void *room = ta_growable_arena_alloc(&call->server->request, capacity, SERVE_ALIGN);
    if (!room) return TA_ERROR_NO_MEMORY;
    ta_buffer buffer = {room, capacity, 0};
    if (ta_json_base64url_read(text, &buffer) != TA_OK) return TA_ERROR_INVALID;
    *out = (ta_bytes){buffer.pointer, buffer.length};
    return TA_OK;
}

/* ---- values in ------------------------------------------------------------------------- */

/* A value param ("data" or "cbor", "params" or "paramsCBOR") of an object,
 * as a payload of the datamodel of the chart that reads it. */
typedef struct value_param {
    const params_at *at;
    const char *json_name, *cbor_name;
    uint32_t segment, chart;
} value_param;

static ta_status value_read(tool_call *call, const value_param *value, ta_payload *out) {
    tool_server *server = call->server;
    *out = 0;
    uint32_t json = member_of(call, value->at, value->json_name), cbor = member_of(call, value->at, value->cbor_name);
    if (json == TA_NIL && cbor == TA_NIL) return TA_OK;
    ta_string_view path = path_of(call, value->at, json != TA_NIL ? value->json_name : value->cbor_name);
    if (!path.pointer) {
        tool_serve_status(call, TA_ERROR_NO_MEMORY);
        return TA_ERROR_NO_MEMORY;
    }
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
        ta_status read = bytes_of(call, cbor, &bytes);
        if (read == TA_ERROR_INVALID) {
            tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_MALFORMED, 0, "malformed", path,
                                                         ta_string("not base64url text")});
            return read;
        }
        if (read != TA_OK) {
            refused_at(call, read, path);
            return read;
        }
    }
    const ta_datamodel_vtable *vtable = server->datamodel.vtable;
    ta_status status = vtable->from_wire(server->datamodel.datamodel, bytes, out);
    if (status == TA_ERROR_NO_MEMORY) {
        refused_at(call, status, path);
    } else if (status != TA_OK) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_MALFORMED, 0, "malformed", path,
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
    ta_status status = bytes_of(call, at, &out->bytes);
    if (status == TA_ERROR_INVALID) {
        out->status = TA_ERROR_INVALID;
        out->complaint = ta_string("the image is not base64url text");
        return TA_OK;
    }
    if (status != TA_OK) return status;
    out->status = ta_image_open(out->bytes, &out->image);
    if (out->status != TA_OK) out->complaint = ta_string("the image does not verify: it is damaged, or of another format version");
    else out->opened = 1;
    return TA_OK;
}

static void add_image(tool_call *call) {
    tool_server *server = call->server;
    uint32_t files = param(call, "files"), sources = param(call, "sources"), bytes = param(call, "image");
    uint32_t flags = flag_of(call, "warningsAsErrors") ? TA_COMPILE_WERROR : 0u;
    ta_string_view path = ta_string(files != TA_NIL ? "params.files" : sources != TA_NIL ? "params.sources" : "params.image");
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
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INVALID, 0, "invalid", path,
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
            refused_at(call, status, ta_string("params.chart"));
            return;
        }
    } else {
        if (segment == TA_NIL) {
            invalid_param(call, "missing", ta_string("params.segment"), "a chart's index needs its segment");
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
    params_at at = params_of(call);
    value_param params = {&at, "params", "paramsCBOR", ref.segment, ref.chart};
    if (value_read(call, &params, &request.params) != TA_OK) return;
    uint64_t session = 0;
    ta_status status = ta_host_spawn(server->host, &request, &session);
    if (status != TA_OK) {
        value_release(server, request.params);
        refused_at(call, status, ta_string("params.chart"));
        return;
    }
    char address[TA_ADDRESS_SESSION_MAX];
    ta_buffer buffer = {address, sizeof address, 0};
    result_number(call, "session", session);
    if (ta_address_format(server->system, session, &buffer) == TA_OK)
        result_text(call, "address", (ta_string_view){address, (uint32_t)buffer.length});
}

/* ---- run.send, run.sendMany, run.fail, run.terminate --------------------------------------- */

/* The session a target names (a session, or a local address), live in the
 * host, and where it lives. */
typedef struct live_session {
    uint64_t session;
    ta_host_location location;
} live_session;

/* TA_OK, or the call's error set at the target's path. */
static ta_status target_of(tool_call *call, const params_at *at, live_session *out) {
    tool_server *server = call->server;
    uint32_t target = member_of(call, at, "target");
    ta_status status = TA_OK;
    if (token(call, target)->kind == TA_CBOR_UINT) out->session = token(call, target)->value;
    else status = ta_host_resolve(server->host, text_of(call, target), &out->session);
    if (status == TA_OK) status = ta_host_locate(server->host, out->session, &out->location);
    if (status != TA_OK) refused_at(call, status, path_of(call, at, "target"));
    return status;
}

/* One event of run.send or run.sendMany as the host takes it, its payload
 * made by the datamodel of the target's chart: TA_OK, or the call's error
 * set and no payload made. As a service the client runs, `invocation` names
 * the invocation, and `done` ends it with core's done.invoke.<id>. */
static ta_status event_read(tool_call *call, const params_at *at, ta_event_in *event) {
    tool_server *server = call->server;
    ta_memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    uint32_t name = member_of(call, at, "event"), invocation = member_of(call, at, "invocation");
    int done = is_true(call, member_of(call, at, "done"));
    if (done && invocation == TA_NIL) {
        invalid_param(call, "missing", path_of(call, at, "invocation"), "done ends an invocation: name it");
        return TA_ERROR_INVALID;
    }
    if (done && name != TA_NIL) {
        invalid_param(call, "unknown", path_of(call, at, "event"), "done's event is core's, done.invoke.<id>");
        return TA_ERROR_INVALID;
    }
    if (!done && name == TA_NIL) {
        invalid_param(call, "missing", path_of(call, at, "event"), "an event has a name");
        return TA_ERROR_INVALID;
    }
    live_session target;
    ta_status status = target_of(call, at, &target);
    if (status != TA_OK) return status;
    ta_actor_info info;
    ta_memset(&info, 0, sizeof info);
    info.size = sizeof info;
    status = ta_actor_info_get(ta_host_system(server->host, target.location.segment), target.location.actor, &info);
    if (status != TA_OK) {
        refused_at(call, status, path_of(call, at, "target"));
        return status;
    }
    event->session = target.session;
    if (invocation != TA_NIL) {
        event->flags = done ? TA_EVENT_IN_INVOKE_DONE : TA_EVENT_IN_FROM_INVOKE;
        event->token = (uint32_t)token(call, invocation)->value;
    }
    event->name = text_of(call, name);
    event->sendid = text_of(call, member_of(call, at, "sendid"));
    event->origin = text_of(call, member_of(call, at, "origin"));
    event->origintype = text_of(call, member_of(call, at, "origintype"));
    event->raw = text_of(call, member_of(call, at, "raw"));
    event->traceparent = text_of(call, member_of(call, at, "traceparent"));
    value_param data = {at, "data", "cbor", target.location.segment, info.chart};
    return value_read(call, &data, &event->data);
}

/* Where the host's refusal of an event points: the invocation it names, or
 * the event. */
static ta_string_view refusal_path(tool_call *call, const params_at *at) {
    return member_of(call, at, "invocation") != TA_NIL ? path_of(call, at, "invocation") : at->path;
}

static void send(tool_call *call) {
    tool_server *server = call->server;
    params_at at = params_of(call);
    ta_event_in event;
    if (event_read(call, &at, &event) != TA_OK) return;
    ta_status status = ta_host_send(server->host, &event);
    if (status != TA_OK) {
        value_release(server, event.data);
        refused_at(call, status, refusal_path(call, &at));
    }
}

/* "params.events[<index>]", in the request arena. */
static ta_string_view element_path(tool_call *call, uint32_t index) {
    tool_text text;
    tool_text_over(&text, &call->server->request);
    tool_text_append(&text, ta_string("params.events["));
    tool_text_number(&text, index);
    tool_text_append(&text, ta_string("]"));
    return tool_text_view(&text);
}

/* Every event is read first: one the server cannot read refuses the request
 * and nothing is sent. Then the host takes them in order and stops at the
 * first it refuses; the result is the accepted prefix's length. */
static void send_many(tool_call *call) {
    tool_server *server = call->server;
    const ta_cbor_document *document = document_of(call);
    uint32_t list = param(call, "events"), count = document->tokens[list].count;
    ta_event_in *events =
        count ? (ta_event_in *)ta_growable_arena_alloc(&server->request, sizeof *events * count, SERVE_ALIGN) : 0;
    if (count && !events) {
        refused_at(call, TA_ERROR_NO_MEMORY, ta_string("params.events"));
        return;
    }
    uint32_t read = 0, at = list + 1;
    for (; read < count; read++) {
        params_at element = {at, 0, element_path(call, read)};
        if (!element.path.pointer) {
            tool_serve_status(call, TA_ERROR_NO_MEMORY);
            break;
        }
        if (event_read(call, &element, &events[read]) != TA_OK) break;
        at = ta_cbor_next(document, at);
    }
    uint32_t accepted = read == count ? ta_host_send_many(server->host, events, count) : 0;
    for (uint32_t i = accepted; i < read; i++) /* refused, or never sent: still the server's */
        value_release(server, events[i].data);
    if (read == count) result_number(call, "accepted", accepted);
}

/* error.communication for a send's sender (its sendid), or for the invoker
 * of an invocation the client runs: it failed. */
static void fail(tool_call *call) {
    tool_server *server = call->server;
    params_at at = params_of(call);
    live_session target;
    if (target_of(call, &at, &target) != TA_OK) return;
    uint32_t invocation = param(call, "invocation");
    ta_event_in event;
    ta_memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = target.session;
    event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    event.sendid = text_of(call, param(call, "sendid"));
    if (invocation != TA_NIL) {
        event.flags |= TA_EVENT_IN_FROM_INVOKE;
        event.token = (uint32_t)token(call, invocation)->value;
    }
    ta_status status = ta_host_send(server->host, &event);
    if (status != TA_OK) refused_at(call, status, refusal_path(call, &at));
}

static void terminate(tool_call *call) {
    params_at at = params_of(call);
    live_session target;
    if (target_of(call, &at, &target) != TA_OK) return;
    ta_status status = ta_host_terminate(call->server->host, target.session);
    if (status != TA_OK) refused_at(call, status, ta_string("params.target"));
}

/* ---- the lookups, run.boot ----------------------------------------------------------------- */

static void chart(tool_call *call) {
    ta_host_chart_ref ref;
    ta_status status = ta_host_chart(call->server->host, text_of(call, param(call, "name")), &ref);
    if (status != TA_OK) {
        refused_at(call, status, ta_string("params.name"));
        return;
    }
    result_number(call, "segment", ref.segment);
    result_number(call, "chart", ref.chart);
}

/* A session, or null for none. */
static void result_session(tool_call *call, uint64_t session) {
    ta_cbor_write_text(&call->result, ta_string("session"));
    if (session) ta_cbor_write_uint(&call->result, session);
    else ta_cbor_write_simple(&call->result, TA_CBOR_NULL);
}

static void resolve(tool_call *call) {
    uint64_t session = 0;
    ta_status status = ta_host_resolve(call->server->host, text_of(call, param(call, "address")), &session);
    if (status != TA_OK && status != TA_ERROR_STALE) {
        refused_at(call, status, ta_string("params.address"));
        return;
    }
    result_session(call, status == TA_OK ? session : 0);
}

static void actor_by_name(tool_call *call) {
    result_session(call, ta_host_actor_by_name(call->server->host, text_of(call, param(call, "name"))));
}

static void boot(tool_call *call) {
    ta_status status = ta_host_boot(call->server->host, (uint32_t)number_of(call, "segment", 0));
    if (status != TA_OK) refused_at(call, status, ta_string("params.segment"));
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
        ta_string_view path = ta_string(by != TA_NIL ? "params.by" : "params.to");
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
            invalid_param(call, "range", ta_string("params.wallNs"), "the wall clock does not go back");
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
    uint32_t flags = flag_of(call, "hold") ? (uint32_t)TOOL_DRAIN_HOLD : 0u;
    uint32_t budget = (uint32_t)number_of(call, "budget", TOOL_SERVE_BUDGET);
    uint64_t allowed = number_of(call, "runs", TOOL_SERVE_RUNS);
    tool_held_ack(server); /* the client has answered what it held */
    int held = tool_drain(server, flags);
    ta_run run = {TA_RUN_IDLE, 0, 0, 0, UINT64_MAX};
    uint32_t runs = 0;
    uint64_t fuel = 0;
    ta_string_view status = ta_string(held ? "held" : "yielded"), detail = ta_string("");
    while (!held && runs < allowed && !server->failed) {
        ta_host_advance_time(server->host, server->clock.now); /* before every run */
        run = ta_host_run(server->host, budget);
        runs++;
        fuel += run.used;
        server->woke = 0;
        if (tool_drain(server, flags)) {
            status = ta_string("held"); /* its senders wait for the client's run.turn or run.ack */
            break;
        }
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

/* The held batch acked: its senders go on, with what the client gave them
 * meanwhile. What the ack lets come is drained, without holding, so the
 * response leaves nothing open (a post or a snapshot may follow); nothing
 * runs. */
static void ack(tool_call *call) {
    tool_server *server = call->server;
    if (!server->held) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_STATE, 0, "state", ta_string(""),
                                                     ta_string("no batch is held: run.turn with hold holds one")});
        return;
    }
    tool_held_ack(server);
    tool_drain(server, 0);
}

/* ---- run.hibernate, run.wake, run.sweep ----------------------------------------------------- */

/* The actor's blob, the client's to keep. The blob is the host's until the
 * ack: with no batch open, taking it opened a batch of its own, which the
 * drain acks once the blob is copied into the result; while a batch is
 * held, it joins that batch, and the client's run.turn or run.ack ends
 * both. */
static void hibernate(tool_call *call) {
    tool_server *server = call->server;
    ta_bytes blob;
    ta_status status = ta_host_hibernate(server->host, number_of(call, "session", 0), &blob);
    if (status != TA_OK) {
        refused_at(call, status, ta_string("params.session"));
        if (status == TA_ERROR_STATE)
            call->error.message = ta_string("the actor is not at rest, or hibernated already");
        return;
    }
    server->open = 1;
    ta_cbor_write_text(&call->result, ta_string("blob"));
    ta_cbor_write_bytes(&call->result, blob); /* copied; JSON writes bytes as base64url */
    tool_drain(server, 0); /* takes nothing while a batch is held */
}

/* A hibernated actor back from the blob run.hibernate gave: a session that
 * is not there is refused at the session, a blob the host refuses (another
 * session's or host's, an earlier one) at the blob. */
static void wake(tool_call *call) {
    tool_server *server = call->server;
    uint64_t session = number_of(call, "session", 0);
    ta_host_location location;
    ta_status status = ta_host_locate(server->host, session, &location);
    if (status != TA_OK) {
        refused_at(call, status, ta_string("params.session"));
        return;
    }
    ta_bytes blob;
    status = bytes_of(call, param(call, "blob"), &blob);
    if (status == TA_ERROR_INVALID) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_MALFORMED, 0, "malformed", ta_string("params.blob"),
                                                     ta_string("not base64url text")});
        return;
    }
    if (status == TA_OK) status = ta_host_wake(server->host, session, blob);
    if (status == TA_OK) return;
    refused_at(call, status, ta_string("params.blob"));
    if (status == TA_ERROR_STALE) call->error.message = ta_string("not the session's latest blob");
    if (status == TA_ERROR_INVALID) call->error.message = ta_string("not this session's blob, or not this host's");
}

/* The actors of the host, which bound what a sweep can answer. */
static uint64_t actors_of(const tool_server *server) {
    uint64_t actors = 0;
    uint32_t count = ta_host_segment_count(server->host);
    for (uint32_t segment = 0; segment < count; segment++) {
        ta_host_segment_info info;
        ta_memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (ta_host_segment_info_get(server->host, segment, &info) == TA_OK && info.live) actors += info.actors;
    }
    return actors;
}

/* The sessions the CLOCK sweep finds at rest, for the client to hibernate.
 * Room for no more than it can answer: `limit`, the slots it scans and the
 * actors there are. */
static void sweep(tool_call *call) {
    tool_server *server = call->server;
    uint32_t scan = (uint32_t)number_of(call, "maxScan", 0);
    uint64_t limit = number_of(call, "limit", 0), actors = actors_of(server);
    if (limit > actors) limit = actors;
    uint32_t room = limit < scan ? (uint32_t)limit : scan;
    uint64_t *sessions = 0;
    if (room) { /* at most the host's actors: their records already fit in memory */
        sessions = (uint64_t *)ta_growable_arena_alloc(&server->request, sizeof *sessions * (size_t)room, SERVE_ALIGN);
        if (!sessions) {
            refused_at(call, TA_ERROR_NO_MEMORY, ta_string("params.limit"));
            return;
        }
    }
    uint32_t count = ta_host_sweep(server->host, scan, (ta_uint64_array){sessions, room});
    ta_cbor_write_text(&call->result, ta_string("sessions"));
    ta_cbor_write_array(&call->result, count);
    for (uint32_t i = 0; i < count; i++)
        ta_cbor_write_uint(&call->result, sessions[i]);
}

/* ---- run.migrate ---------------------------------------------------------------------------- */

/* A migration to the current versions: every chart, a chart's, or one
 * session's; its effects come in later runs. A refusal points at the chart
 * when the host has none of that name, else at the session when one is
 * named. */
static void migrate(tool_call *call) {
    tool_server *server = call->server;
    ta_host_migration request;
    ta_memset(&request, 0, sizeof request);
    request.name = text_of(call, param(call, "chart"));
    request.session = number_of(call, "session", 0);
    request.flags = flag_of(call, "wake") ? (uint32_t)TA_HOST_MIGRATE_WAKE : 0u;
    uint32_t timeout = param(call, "timeout");
    if (timeout != TA_NIL) {
        time_value ticks;
        if (!time_of(call, timeout, &ticks)) {
            invalid_param(call, "range", ta_string("params.timeout"), "not a duration");
            return;
        }
        request.timeout = ticks.ticks;
    }
    uint32_t migration = 0;
    ta_status status = ta_host_migrate(server->host, &request, &migration);
    if (status != TA_OK) {
        ta_host_chart_ref ref;
        int named = request.name.length && ta_host_chart(server->host, request.name, &ref) == TA_OK;
        int at_chart = request.name.length && (!named || !request.session);
        refused_at(call, status,
                   ta_string(at_chart ? "params.chart" : request.session ? "params.session" : "params"));
        if (status == TA_ERROR_INVALID)
            call->error.message = ta_string(at_chart ? "the host has no chart of that name"
                                                     : "an invoked actor moves with its parent; or of another chart");
        return;
    }
    result_number(call, "migration", migration);
}

/* ---- run.snapshot, run.restore -------------------------------------------------------------- */

/* The label of the datamodels' state in a pack the server writes, and the
 * one it restores: a pack another program wrote holds another state. */
static const ta_string_view snapshot_label = TA_CLI_TEXT("tinyactors-run");

/* A path of a file the server reads or writes: "-" would be standard
 * input or output, which are the client's. 1 when it is a file, else 0
 * with the call's error set. */
static int file_path_of(tool_call *call, uint32_t at) {
    if (!tool_equal(text_of(call, at), ta_string("-")) && text_of(call, at).length) return 1;
    invalid_param(call, "range", ta_string("params.path"), "a file: standard input and output are the protocol's");
    return 0;
}

/* What core writes into a ta_buffer for run.snapshot: quickjs's state, or
 * the pack of the host and the server's state. */
typedef struct filling {
    tool_server *server;
    const ta_datamodel_state *state; /* the pack's; NULL: quickjs's state */
} filling;

static ta_status fill(const filling *what, ta_buffer *out) {
    if (!what->state) return ta_datamodel_ecmascript_snapshot(what->server->ecmascript, out);
    return ta_host_snapshot_pack(what->server->host, what->state, out);
}

/* It in the request arena: asked with no room for its length, then
 * written. */
static ta_status filled(const filling *what, ta_bytes *out) {
    ta_buffer probe = {0, 0, 0};
    ta_status status = fill(what, &probe);
    *out = (ta_bytes){0, 0};
    if (status == TA_OK) return TA_OK; /* nothing to write */
    if (status != TA_ERROR_NO_MEMORY || !probe.length) return status;
    void *room = ta_growable_arena_alloc(&what->server->request, probe.length, SERVE_ALIGN);
    if (!room) return TA_ERROR_NO_MEMORY;
    ta_buffer buffer = {room, probe.length, 0};
    status = fill(what, &buffer);
    if (status == TA_OK) *out = (ta_bytes){buffer.pointer, buffer.length};
    return status;
}

/* The server's state in the pack, beside the host's: quickjs's state (its
 * sessions, payloads and random state) and the wall clock, which the
 * host's snapshot does not keep. */
static ta_status snapshot_state_of(tool_server *server, ta_bytes quickjs, ta_bytes *out) {
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("quickjs"));
    ta_cbor_write_bytes(&writer, quickjs);
    ta_cbor_write_text(&writer, ta_string("wallNs"));
    ta_cbor_write_uint(&writer, server->clock.now.wall_ns);
    return ta_cbor_writer_bytes(&writer, out);
}

/* The quiescent host packed with the server's state (snapshot_state_of),
 * labelled as the server's, answered or written whole to `path`. Core
 * refuses it with a batch open or a migration running: checked first, so
 * the datamodel's state is not written for nothing. */
static void snapshot(tool_call *call) {
    tool_server *server = call->server;
    uint32_t path = param(call, "path");
    if (path != TA_NIL && !file_path_of(call, path)) return;
    ta_buffer quiet = {0, 0, 0};
    ta_status status = ta_host_snapshot(server->host, &quiet);
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY) {
        tool_serve_status(call, status);
        if (status == TA_ERROR_STATE)
            call->error.message = ta_string("a migration runs: snapshot after its effect.migrationDone");
        return;
    }
    ta_bytes quickjs, state = {0, 0}, pack;
    status = filled(&(filling){server, 0}, &quickjs);
    if (status == TA_ERROR_UNSUPPORTED) {
        tool_serve_status(call, status);
        tool_text why;
        tool_text_over(&why, &server->request);
        tool_text_append(&why, ta_string(ta_datamodel_ecmascript_last_error(server->ecmascript)));
        if (tool_text_view(&why).pointer) call->error.message = tool_text_view(&why);
        return;
    }
    if (status == TA_OK) status = snapshot_state_of(server, quickjs, &state);
    ta_datamodel_state labelled = {snapshot_label, state};
    if (status == TA_OK) status = filled(&(filling){server, &labelled}, &pack);
    if (status != TA_OK) {
        tool_serve_status(call, status);
        return;
    }
    if (path == TA_NIL) {
        ta_cbor_write_text(&call->result, ta_string("snapshot"));
        ta_cbor_write_bytes(&call->result, pack); /* JSON writes bytes as base64url */
        return;
    }
    tool *t = server->invocation;
    tool_write request = {0, text_of(call, path), pack};
    if (t->platform->write(t->platform->context, &request) != TA_OK) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INVALID, 0, "invalid", ta_string("params.path"),
                                                     ta_string("the file could not be written")});
        return;
    }
    result_number(call, "bytes", pack.length);
}

/* A refusal of run.restore: -32005 at `path`, saying why. */
static void restore_refuse(tool_call *call, ta_string_view path, const char *message) {
    tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INVALID, 0, "invalid", path, ta_string(message)});
    call->error.status = ta_string("TA_ERROR_INVALID");
}

/* The parts of a pack the server wrote: the host's snapshot and images
 * (contents), quickjs's state and the wall clock. */
typedef struct restoring {
    ta_host_snapshot_contents contents;
    ta_bytes quickjs;
    uint64_t wall_ns;
} restoring;

/* A pack's parts, checked to be the server's: 1, or 0 with the call's
 * error set. */
static int restoring_of(tool_call *call, ta_bytes pack, ta_string_view path, restoring *out) {
    tool_server *server = call->server;
    ta_memset(out, 0, sizeof *out);
    out->contents.size = sizeof out->contents;
    if (ta_host_snapshot_unpack(pack, &out->contents) != TA_OK) {
        restore_refuse(call, path, "not a snapshot this build writes: of another format, or damaged");
        return 0;
    }
    if (!tool_equal(out->contents.datamodel, snapshot_label)) {
        restore_refuse(call, path, "a snapshot another program packed: its datamodels' state is not the server's");
        return 0;
    }
    if (!tool_equal(out->contents.system_id, server->system)) {
        restore_refuse(call, path, "a snapshot of another system: start the server with its --system");
        return 0;
    }
    ta_cbor_document state;
    ta_status status = ta_rpc_decode(
        &(ta_rpc_decoding){{out->contents.state, out->contents.state_length}, &server->request}, &state);
    if (status == TA_ERROR_NO_MEMORY) {
        refused_at(call, status, path);
        return 0;
    }
    uint32_t quickjs = status == TA_OK && state.tokens[0].kind == TA_CBOR_MAP
                           ? ta_cbor_member(&state, 0, ta_string("quickjs"))
                           : TA_NIL;
    uint32_t wall = quickjs != TA_NIL ? ta_cbor_member(&state, 0, ta_string("wallNs")) : TA_NIL;
    if (wall == TA_NIL || state.tokens[quickjs].kind != TA_CBOR_BYTES || state.tokens[wall].kind != TA_CBOR_UINT) {
        restore_refuse(call, path, "the server's state in the snapshot is damaged");
        return 0;
    }
    ta_string_view bytes = ta_cbor_string(&state, quickjs); /* a view into the pack */
    out->quickjs = (ta_bytes){bytes.pointer, bytes.length};
    out->wall_ns = state.tokens[wall].value;
    return 1;
}

/* A host and its quickjs made from a pack, for the server to take in
 * place of its own; nothing of the server's is touched. */
typedef struct restored_host {
    ta_host *host;
    ta_datamodel_ecmascript *ecmascript;
} restored_host;

static ta_status restored_of(tool_server *server, ta_bytes pack, const restoring *parts, restored_host *out) {
    out->host = 0;
    out->ecmascript = ta_datamodel_ecmascript_new(&server->ecmascript_config);
    if (!out->ecmascript) return TA_ERROR_NO_MEMORY;
    uint32_t count = parts->contents.image_count;
    ta_bytes *images = count ? (ta_bytes *)ta_growable_arena_alloc(&server->request, sizeof *images * count, SERVE_ALIGN)
                             : 0;
    ta_status status = count && !images ? TA_ERROR_NO_MEMORY : TA_OK;
    for (uint32_t i = 0; status == TA_OK && i < count; i++)
        images[i] = ta_host_snapshot_image(pack, i);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(out->ecmascript);
    ta_host_environment environment;
    ta_memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = 1;
    environment.datamodels = &binding;
    environment.allocator = server->invocation->platform->allocator;
    ta_host_snapshot_input input = {server->config, {parts->contents.host, parts->contents.host_length}, images, count, 0};
    if (status == TA_OK) status = ta_host_restore(&environment, &input, &out->host);
    /* the datamodel's handles valid again, and its random state, before any run */
    if (status == TA_OK) status = ta_datamodel_ecmascript_restore(out->ecmascript, parts->quickjs);
    if (status == TA_OK) return TA_OK;
    if (out->host) ta_host_free(out->host);
    ta_datamodel_ecmascript_free(out->ecmascript); /* after the host */
    out->host = 0;
    out->ecmascript = 0;
    return status;
}

/* The host replaced by one restored from a pack run.snapshot wrote, with
 * the server's config and datamodel limits: the old host and its quickjs
 * are freed once the new one is whole, and the clock goes on from the
 * snapshot's time. A refused restore leaves the server's host as it was.
 * The restored host's first turn announces each invocation that was
 * running (effect.invoke with `restored`). */
static void restore(tool_call *call) {
    tool_server *server = call->server;
    uint32_t given = param(call, "snapshot"), path = param(call, "path");
    ta_string_view where = ta_string(given != TA_NIL ? "params.snapshot" : "params.path");
    ta_bytes pack;
    ta_status status;
    if (given != TA_NIL) {
        status = bytes_of(call, given, &pack); /* 8-aligned, as the pack's parts need */
        if (status == TA_ERROR_INVALID) {
            tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_MALFORMED, 0, "malformed", where,
                                                         ta_string("not base64url text")});
            return;
        }
    } else {
        if (!file_path_of(call, path)) return;
        tool *t = server->invocation;
        tool_read read = {text_of(call, path), &server->request};
        status = t->platform->read(t->platform->context, &read, &pack); /* 8-aligned */
        if (status == TA_ERROR_INVALID) {
            restore_refuse(call, where, "the file cannot be read");
            return;
        }
    }
    if (status != TA_OK) {
        refused_at(call, status, where);
        return;
    }
    restoring parts;
    if (!restoring_of(call, pack, where, &parts)) return;
    restored_host made;
    status = restored_of(server, pack, &parts, &made);
    if (status != TA_OK) {
        refused_at(call, status, where);
        if (status == TA_ERROR_INVALID) call->error.message = ta_string("the snapshot does not restore with this build");
        return;
    }
    ta_host_free(server->host);
    ta_datamodel_ecmascript_free(server->ecmascript); /* after the host */
    server->host = made.host;
    server->ecmascript = made.ecmascript;
    server->datamodel = ta_datamodel_ecmascript_binding(made.ecmascript);
    server->clock.now = (ta_system_time){parts.contents.time, parts.wall_ns};
    ta_host_advance_time(server->host, server->clock.now);
    ta_cbor_write_text(&call->result, ta_string("segments"));
    ta_cbor_write_array(&call->result, TA_CBOR_COUNT_LATER);
    uint32_t count = ta_host_segment_count(server->host);
    for (uint32_t segment = 0; segment < count; segment++) {
        ta_host_segment_info info;
        ta_memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (ta_host_segment_info_get(server->host, segment, &info) == TA_OK && info.live)
            ta_cbor_write_uint(&call->result, segment);
    }
    ta_cbor_write_end(&call->result);
    result_number(call, "time", server->clock.now.monotonic);
    result_number(call, "wallNs", server->clock.now.wall_ns);
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
    tool_drain(server, 0);
    if (!answered) return;
    tool_post *post = (tool_post *)ta_table_get(&server->posts, handle);
    if (post) { /* no reply came: the post is no longer this call's */
        post->call = 0;
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_INTERNAL_ERROR, 0, "internal", ta_string(""),
                                                     ta_string("the host gave no reply: please report it")});
    }
}

/* ---- the methods ----------------------------------------------------------------------------- */

/* The methods a held batch allows (docs/cli.md, "Held batches"): inputs,
 * lookups, the clock, the sweep, a migration started and a blob taken
 * (which joins the batch), which core takes while a batch is open, and the
 * two that end it. Refused: what would need the batch's ack (a forwarded
 * post, whose reply comes only after it) and what needs the host
 * quiescent (an image added, a snapshot taken or restored). */
static const tool_method methods[] = {
    {TA_CLI_TEXT("run.addImage"), add_image, 0, 0},
    {TA_CLI_TEXT("run.boot"), boot, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.chart"), chart, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.spawn"), spawn, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.send"), send, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.sendMany"), send_many, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.fail"), fail, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.terminate"), terminate, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.resolve"), resolve, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.actorByName"), actor_by_name, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.advanceTime"), advance_time, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.turn"), turn, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.ack"), ack, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.hibernate"), hibernate, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.wake"), wake, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.sweep"), sweep, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.migrate"), migrate, TOOL_METHOD_WHILE_HELD, 0},
    {TA_CLI_TEXT("run.snapshot"), snapshot, 0, 0},
    {TA_CLI_TEXT("run.restore"), restore, 0, 0},
    {TA_CLI_TEXT("run.schema"), schema, TOOL_METHOD_WHILE_HELD, 0},
};

tool_method_list tool_serve_methods(void) {
    return (tool_method_list){methods, (uint32_t)(sizeof methods / sizeof methods[0]), 0};
}

static void refuse_held(tool_call *call) {
    tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_STATE, 0, "state", ta_string(""),
                                                 ta_string("a batch is held: answer its sends, then run.turn or run.ack")});
}

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
            if (server->held && !(methods[i].flags & TOOL_METHOD_WHILE_HELD)) refuse_held(call);
            else methods[i].run(call);
            return;
        }
    if (is_server_area(name)) {
        tool_serve_refuse(call, (tool_serve_refusal){TA_RPC_METHOD_NOT_FOUND, 0, "unknown", ta_string("method"),
                                                     ta_string("no such method of the server's")});
        return;
    }
    if (server->held) refuse_held(call);
    else forward(call);
}
