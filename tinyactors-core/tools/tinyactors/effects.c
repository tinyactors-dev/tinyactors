/* effects.c — the run server's drain (serve.h): take, write a notification
 * per effect, ack; replies given to the posts that asked for them.
 * Everything an effect hands out (its fields, its payload's CBOR, a reply's
 * bytes) is read into a line, or copied, before the ack. */
#include "serve.h"

#include "base/memory.h"
#include "cli/json.h"

/* ---- values ------------------------------------------------------------------------ */

static void writer_start(tool_server *server, ta_cbor_writer *writer) {
    ta_cbor_writer_init(writer, (ta_buffer){0, 0, 0}, &server->request_allocator);
}

ta_status tool_value_member_write(tool_server *server, const tool_value_member *member, ta_cbor_writer *writer) {
    ta_cbor_document document;
    ta_status status = ta_rpc_decode(&(ta_rpc_decoding){member->cbor, &server->request}, &document);
    if (status != TA_OK) return status;
    /* what JSON changes, counted without writing (an output with no room) */
    ta_output counted = ta_output_over((ta_buffer){0, 0, 0}, 0);
    ta_json_writing writing = {member->cbor, &counted, 0};
    status = ta_json_write(&writing);
    int writable = status == TA_OK || status == TA_ERROR_NO_MEMORY;
    ta_cbor_write_text(writer, member->name);
    if (writable) ta_rpc_write_item(writer, (ta_rpc_item){&document, 0, 0});
    else ta_cbor_write_simple(writer, TA_CBOR_NULL); /* a key that is not text: JSON has none */
    if (!writable || writing.changes) {
        ta_cbor_write_text(writer, member->cbor_name);
        ta_cbor_write_bytes(writer, member->cbor); /* JSON writes bytes as base64url */
    }
    return TA_OK;
}

void tool_value_write(tool_server *server, const tool_value_out *value, ta_cbor_writer *writer) {
    if (!value->payload || value->datamodel != 0) return; /* the host's one datamodel is index 0 */
    const ta_datamodel_vtable *vtable = server->datamodel.vtable;
    ta_bytes cbor;
    if (!vtable->to_wire || vtable->to_wire(server->datamodel.datamodel, value->payload, &cbor) != TA_OK) return;
    tool_value_member_write(server, &(tool_value_member){cbor, value->name, value->cbor_name}, writer);
}

/* ---- notifications ------------------------------------------------------------------ */

static void member_text(ta_cbor_writer *writer, const char *name, ta_string_view text) {
    if (!text.length) return; /* a field the effect does not set is left out */
    ta_cbor_write_text(writer, ta_string(name));
    ta_cbor_write_text(writer, text);
}

static void member_number(ta_cbor_writer *writer, const char *name, uint64_t value) {
    ta_cbor_write_text(writer, ta_string(name));
    ta_cbor_write_uint(writer, value);
}

/* Where a notification is being written. */
typedef struct notice {
    tool_server *server;
    const ta_host_effect *effect;
    uint32_t index;
    uint32_t pad;
    ta_cbor_writer writer;
} notice;

static void notice_begin(notice *note, const char *method) {
    writer_start(note->server, &note->writer);
    ta_rpc_notification_begin(&note->writer, ta_string(method));
    ta_cbor_write_map(&note->writer, TA_CBOR_COUNT_LATER);
    member_number(&note->writer, "batch", note->server->batches);
    member_number(&note->writer, "index", note->index);
    if (note->effect->session) member_number(&note->writer, "session", note->effect->session);
}

static void notice_end(notice *note) {
    ta_cbor_write_end(&note->writer);
    ta_bytes cbor;
    if (ta_cbor_writer_bytes(&note->writer, &cbor) != TA_OK) {
        tool_complain(note->server->invocation, ta_string("out of memory writing a notification: it is lost"));
        return;
    }
    tool_serve_write(note->server, cbor);
}

static ta_string_view effect_text(const notice *note, uint32_t field) {
    return ta_host_effect_text(note->server->host, note->index, field);
}

static void send_notice(notice *note) {
    const ta_host_effect *effect = note->effect;
    ta_cbor_writer *writer = &note->writer;
    tool_value_out data = {effect->payload, effect->datamodel, 0, ta_string("data"), ta_string("cbor")};
    notice_begin(note, "effect.send");
    if (effect->io_processor == 0) {
        /* to another system: the message its run.send takes as it is */
        ta_cbor_write_text(writer, ta_string("message"));
        ta_cbor_write_map(writer, TA_CBOR_COUNT_LATER);
        member_text(writer, "target", effect->target);
        member_text(writer, "event", effect->event);
        tool_value_write(note->server, &data, writer);
        member_text(writer, "sendid", effect->sendid);
        member_text(writer, "origin", effect_text(note, TA_EFFECT_FIELD_ORIGIN));
        member_text(writer, "traceparent", effect_text(note, TA_EFFECT_FIELD_TRACEPARENT));
        ta_cbor_write_end(writer);
    } else {
        if (effect->io_processor == TA_NIL) { /* to a service the client runs */
            member_number(writer, "invocation", effect->token);
            member_number(writer, "segment", effect->segment);
        } else if (effect->io_processor - 1 < note->server->processor_count) {
            member_text(writer, "type", note->server->processors[effect->io_processor - 1]);
        }
        member_text(writer, "target", effect->target);
        member_text(writer, "event", effect->event);
        member_text(writer, "sendid", effect->sendid);
        tool_value_write(note->server, &data, writer);
        member_text(writer, "traceparent", effect_text(note, TA_EFFECT_FIELD_TRACEPARENT));
    }
    if (effect->flags & TA_EFFECT_FLAG_WAITING) {
        ta_cbor_write_text(writer, ta_string("waiting"));
        ta_cbor_write_simple(writer, TA_CBOR_TRUE);
    }
    notice_end(note);
}

static void log_notice(notice *note) {
    notice_begin(note, "effect.log");
    member_text(&note->writer, "label", note->effect->auxiliary);
    member_text(&note->writer, "text", note->effect->event);
    notice_end(note);
}

static void done_notice(notice *note) {
    const ta_host_effect *effect = note->effect;
    notice_begin(note, "effect.done");
    member_text(&note->writer, "name", effect->target);
    member_text(&note->writer, "final", effect->auxiliary);
    tool_value_write(note->server,
                     &(tool_value_out){effect->payload, effect->datamodel, 0, ta_string("data"), ta_string("cbor")},
                     &note->writer);
    notice_end(note);
}

static void error_notice(notice *note) {
    const char *name = ta_status_name((ta_status)(int32_t)note->effect->flags);
    notice_begin(note, "effect.error");
    member_text(&note->writer, "status", ta_string(name ? name : "unknown"));
    notice_end(note);
}

/* A chart asked for a chart its image lacks: refused, as the bindings do,
 * until fetching at run time is built (the parent gets error.execution). */
static void request_notice(notice *note) {
    const ta_host_effect *effect = note->effect;
    ta_resume resume = {effect->segment, effect->token, TA_ERROR_INVALID, 0, 0};
    ta_host_resume(note->server->host, &resume);
    note->server->woke = 1;
    notice_begin(note, "effect.request");
    ta_cbor_write_text(&note->writer, ta_string("src"));
    ta_cbor_write_text(&note->writer, effect->auxiliary);
    notice_end(note);
}

/* A host notification: none is defined (docs/requests.md); written as it
 * is, should core add one. */
static void host_notice(notice *note) {
    ta_cbor_document document;
    if (ta_rpc_decode(&(ta_rpc_decoding){note->effect->wire, &note->server->request}, &document) != TA_OK) return;
    uint32_t method = ta_cbor_member(&document, 0, ta_string("method"));
    uint32_t params = ta_cbor_member(&document, 0, ta_string("params"));
    if (method == TA_NIL || document.tokens[method].kind != TA_CBOR_TEXT) return;
    writer_start(note->server, &note->writer);
    ta_rpc_notification_begin(&note->writer, ta_cbor_string(&document, method));
    if (params != TA_NIL) ta_rpc_write_item(&note->writer, (ta_rpc_item){&document, params, 0});
    else ta_cbor_write_map(&note->writer, 0);
    ta_bytes cbor;
    if (ta_cbor_writer_bytes(&note->writer, &cbor) == TA_OK) tool_serve_write(note->server, cbor);
}

/* An external invocation: the client runs every service. It answers as the
 * service with run.send (its token as `invocation`, and `done`), or fails it
 * with run.fail. */
static void invoke_notice(notice *note) {
    const ta_host_effect *effect = note->effect;
    ta_cbor_writer *writer = &note->writer;
    notice_begin(note, "effect.invoke");
    member_number(writer, "segment", effect->segment);
    member_number(writer, "invocation", effect->token);
    member_text(writer, "type", effect->event);
    member_text(writer, "src", effect->target);
    member_text(writer, "invokeid", effect->sendid);
    tool_value_write(note->server,
                     &(tool_value_out){effect->payload, effect->datamodel, 0, ta_string("data"), ta_string("cbor")},
                     writer);
    if (effect->flags & TA_EFFECT_FLAG_RESTORED) {
        ta_cbor_write_text(writer, ta_string("restored"));
        ta_cbor_write_simple(writer, TA_CBOR_TRUE);
    }
    notice_end(note);
}

/* An invocation cancelled: the client stops what stands in for it; its
 * token is stale from now on. */
static void uninvoke_notice(notice *note) {
    notice_begin(note, "effect.uninvoke");
    member_number(&note->writer, "segment", note->effect->segment);
    member_number(&note->writer, "invocation", note->effect->token);
    notice_end(note);
}

/* ---- replies ----------------------------------------------------------------------- */

/* host.config's result: the clock's rate, the system id and the I/O
 * processors' types, copied into the invocation's arena (the reply ends at
 * the ack). */
static void config_read(tool_server *server, const ta_cbor_document *document, uint32_t result) {
    tool *t = server->invocation;
    uint32_t system = ta_cbor_member(document, result, ta_string("system"));
    uint32_t rate = system == TA_NIL ? TA_NIL : ta_cbor_member(document, system, ta_string("ticksPerSecond"));
    if (rate != TA_NIL && document->tokens[rate].kind == TA_CBOR_UINT && document->tokens[rate].value)
        server->clock.ticks_per_second = document->tokens[rate].value;
    tool_text id;
    tool_text_init(t, &id);
    tool_text_append(&id, ta_cbor_string(document, ta_cbor_member(document, result, ta_string("systemID"))));
    server->system = tool_text_view(&id);
    uint32_t list = ta_cbor_member(document, result, ta_string("ioProcessors"));
    if (list == TA_NIL || document->tokens[list].kind != TA_CBOR_ARRAY) return;
    uint32_t count = document->tokens[list].count;
    ta_string_view *types =
        count ? (ta_string_view *)ta_growable_arena_alloc(t->arena, sizeof *types * count, 8) : 0;
    if (count && !types) return;
    uint32_t at = list + 1;
    for (uint32_t i = 0; i < count; i++) {
        tool_text type;
        tool_text_init(t, &type);
        tool_text_append(&type, ta_cbor_string(document, ta_cbor_member(document, at, ta_string("type"))));
        types[i] = tool_text_view(&type);
        at = ta_cbor_next(document, at);
    }
    server->processors = types;
    server->processor_count = count;
}

/* A forwarded request's response: core's result or error, with the
 * client's id, made now (the reply ends at the ack). */
static void forwarded_answer(tool_call *call, const ta_cbor_document *document, uint32_t reply) {
    tool_server *server = call->server;
    const ta_rpc_message *message = call->message;
    uint32_t result = ta_cbor_member(document, reply, ta_string("result"));
    uint32_t failure = ta_cbor_member(document, reply, ta_string("error"));
    ta_rpc_error error;
    ta_memset(&error, 0, sizeof error);
    if (failure != TA_NIL) {
        uint32_t code = ta_cbor_member(document, failure, ta_string("code"));
        uint32_t data = ta_cbor_member(document, failure, ta_string("data"));
        error.code = code != TA_NIL && document->tokens[code].kind == TA_CBOR_NEGINT
                         ? (int32_t)(-1 - (int64_t)document->tokens[code].value)
                         : TA_RPC_INTERNAL_ERROR;
        error.message = ta_cbor_string(document, ta_cbor_member(document, failure, ta_string("message")));
        if (data != TA_NIL) {
            error.reason = ta_cbor_string(document, ta_cbor_member(document, data, ta_string("reason")));
            error.path = ta_cbor_string(document, ta_cbor_member(document, data, ta_string("path")));
        }
    }
    ta_cbor_writer writer;
    writer_start(server, &writer);
    ta_rpc_response response = {{&message->document, message->id, 0},
                                {result != TA_NIL ? document : 0, result, 0},
                                failure != TA_NIL ? &error : 0};
    ta_rpc_response_write(&response, &writer);
    if (ta_cbor_writer_bytes(&writer, &call->response) != TA_OK) {
        call->response = (ta_bytes){0, 0};
        tool_serve_status(call, TA_ERROR_NO_MEMORY);
    }
}

static void reply_given(tool_server *server, const ta_host_effect *effect) {
    ta_cbor_document document;
    if (ta_rpc_decode(&(ta_rpc_decoding){effect->wire, &server->request}, &document) != TA_OK) return;
    uint32_t id = ta_cbor_member(&document, 0, ta_string("id"));
    if (id == TA_NIL || document.tokens[id].kind != TA_CBOR_UINT) return; /* not one of ours */
    uint64_t handle = document.tokens[id].value;
    tool_post *post = (tool_post *)ta_table_get(&server->posts, handle);
    if (!post) return; /* a stale id: dropped */
    if (post->kind == TOOL_POST_CONFIG) {
        uint32_t result = ta_cbor_member(&document, 0, ta_string("result"));
        if (result != TA_NIL) config_read(server, &document, result);
    } else if (post->call) {
        forwarded_answer(post->call, &document, 0);
    }
    ta_table_remove(&server->posts, handle);
}

/* ---- the drain ---------------------------------------------------------------------- */

static void handle(tool_server *server, const ta_host_effect *effect, uint32_t index) {
    notice note;
    note.server = server;
    note.effect = effect;
    note.index = index;
    /* a notification's memory is the effect's alone; a reply's response is kept */
    ta_growable_arena_mark mark = ta_growable_arena_position(&server->request);
    switch (effect->kind) {
    case TA_EFFECT_SEND: send_notice(&note); break;
    case TA_EFFECT_LOG: log_notice(&note); break;
    case TA_EFFECT_DONE: done_notice(&note); break;
    case TA_EFFECT_ERROR: error_notice(&note); break;
    case TA_EFFECT_REQUEST: request_notice(&note); break;
    case TA_EFFECT_INVOKE: invoke_notice(&note); break;
    case TA_EFFECT_UNINVOKE: uninvoke_notice(&note); break;
    case TA_HOST_EFFECT_NOTIFICATION: host_notice(&note); break;
    case TA_HOST_EFFECT_REPLY: reply_given(server, effect); return;
    default: break; /* the rest come with later steps of the protocol */
    }
    ta_growable_arena_release(&server->request, mark);
}

/* Whether a batch holds a send only the client decides and whose sender
 * waits for the ack: to an I/O processor of the config, to the transport or
 * to a service the client runs (every send the host hands out is one). */
static int waits_for_the_client(const ta_host_effect *effects, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (effects[i].kind == TA_EFFECT_SEND && (effects[i].flags & TA_EFFECT_FLAG_WAITING)) return 1;
    return 0;
}

static void batch_ack(tool_server *server) {
    ta_host_effects_ack(server->host);
    server->open = 0;
    server->held = 0;
}

int tool_drain(tool_server *server, uint32_t flags) {
    for (;;) {
        if (server->held) return 1; /* a take would hand the same batch out again */
        const ta_host_effect *effects;
        uint32_t count = ta_host_effects(server->host, &effects);
        if (!count && !server->open) return 0;
        if (count) {
            server->open = 1;
            server->batches++;
            for (uint32_t i = 0; i < count; i++)
                handle(server, &effects[i], i);
            if ((flags & TOOL_DRAIN_HOLD) && waits_for_the_client(effects, count)) {
                server->held = 1;
                return 1;
            }
        }
        batch_ack(server); /* a batch of outputs alone (count 0) is acked here too */
    }
}

void tool_held_ack(tool_server *server) {
    if (server->held) batch_ack(server);
}
