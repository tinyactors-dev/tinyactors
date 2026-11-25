/* serve.h — tinyactors run, the JSON-RPC 2.0 server (docs/cli.md, "Run"),
 * between its three files:
 *
 *   run.c      the command: options, the config, startup, the message loop,
 *              lines written to the client, the end of input;
 *   serve.c    the methods: run.* as the host's C calls, every other method
 *              forwarded to ta_host_post, params checked against the
 *              protocol's schema (schema.h) first;
 *   effects.c  the drain: take, a notification per effect, ack; replies
 *              matched to their posts.
 *
 * Freestanding, as every command: standard input and output through the
 * platform (tool.h). Lifetimes: the server, its host and its tables live
 * for the invocation; a message's CBOR, tokens, the values it converts and
 * the lines written for it live in the request arena, above a mark taken
 * when its line is read and released once it is answered; what the host
 * hands out is read into a notification before the batch's ack. */
#ifndef TINYACTORS_TOOL_SERVE_H
#define TINYACTORS_TOOL_SERVE_H

#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/table.h>

#include "cli/rpc.h"
#include "schema.h"
#include "tool.h"

typedef enum tool_serve_limits {
    TOOL_SERVE_BUDGET = 10000, /* run.turn's fuel per run when it names none */
    TOOL_SERVE_RUNS = 1000,    /* run.turn's runs at most when it names none */
    TOOL_SERVE_REQUEST_KEEP = 256 * 1024 /* the request arena's chunks kept between messages */
} tool_serve_limits;

/* What a pending post is, by the row its id names. */
typedef enum tool_post_kind {
    TOOL_POST_FORWARDED = 0, /* a client's request: answered with its id */
    TOOL_POST_CONFIG = 1     /* host.config at creation */
} tool_post_kind;

struct tool_call;
typedef struct tool_post {
    uint32_t kind; /* tool_post_kind */
    uint32_t pad;
    struct tool_call *call; /* TOOL_POST_FORWARDED: answered within it */
} tool_post;

typedef struct tool_clock {
    ta_system_time now;
    uint64_t ticks_per_second; /* host.config's, read once */
} tool_clock;

typedef struct tool_server {
    tool *invocation;
    ta_host *host;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding datamodel; /* the host's one datamodel: quickjs */
    ta_growable_arena request;      /* a mark per message; capped between messages */
    ta_allocator request_allocator; /* its view, for writers */
    ta_table posts;                 /* tool_post rows: a post's id is its handle */
    ta_rpc_lines lines;
    tool_clock clock;
    ta_string_view system;            /* the effective systemID, for addresses */
    const ta_string_view *processors; /* the effective ioProcessors' types, in order */
    uint32_t processor_count;
    uint32_t failed;  /* standard output could not be written: the client went away */
    uint32_t woke;    /* the drain gave the host an input (a refused request or invocation):
                         a run that went idle before it runs again */
    uint32_t pad;
    uint64_t batches; /* batches handed out */
} tool_server;

/* One message being answered. */
typedef struct tool_call {
    tool_server *server;
    const ta_rpc_message *message;
    ta_cbor_writer result; /* the result map's members, in the request arena */
    ta_rpc_error error;    /* code 0: none */
    ta_bytes response;     /* a forwarded request's response, made at its REPLY */
} tool_call;

/* ---- run.c ------------------------------------------------------------------------ */

/* One CBOR item as a line on standard output (a notification, a response):
 * a failure to write sets server->failed. */
void tool_serve_write(tool_server *server, ta_bytes cbor);

/* ---- serve.c ---------------------------------------------------------------------- */

/* Answer one request or notification (not a batch): call->error, or the
 * result members written into call->result (a map opened with
 * TA_CBOR_COUNT_LATER), or call->response (a forwarded one). */
void tool_serve_call(tool_call *call);

/* An error of the server's own for a status a C call refused with. */
void tool_serve_status(tool_call *call, ta_status status);
/* An error with a code, a reason and a path (static texts), and a message. */
typedef struct tool_serve_refusal {
    int32_t code;
    uint32_t pad;
    const char *reason;
    ta_string_view path, message;
} tool_serve_refusal;
void tool_serve_refuse(tool_call *call, tool_serve_refusal refusal);

/* ---- effects.c -------------------------------------------------------------------- */

/* Take, handle, ack until nothing is pending: each effect written as an
 * effect.* notification before the ack, each REPLY given to its post. */
void tool_drain(tool_server *server);

/* A payload as `name` (and its CBOR twin `cbor_name` when JSON changed
 * it), a member of the map the writer has open; nothing for no payload. */
typedef struct tool_value_out {
    ta_payload payload;
    uint32_t datamodel; /* the effect's: TA_NIL for none */
    uint32_t pad;
    ta_string_view name, cbor_name;
} tool_value_out;
void tool_value_write(tool_server *server, const tool_value_out *value, ta_cbor_writer *writer);

/* A neutral value's CBOR as a member `name` of the open map, and its twin
 * `cbor_name` (bytes, which JSON writes as base64url) when JSON changes it.
 * TA_OK; TA_ERROR_NO_MEMORY; TA_ERROR_INVALID: not one item. */
typedef struct tool_value_member {
    ta_bytes cbor;
    ta_string_view name, cbor_name;
} tool_value_member;
ta_status tool_value_member_write(tool_server *server, const tool_value_member *member, ta_cbor_writer *writer);

#endif /* TINYACTORS_TOOL_SERVE_H */
