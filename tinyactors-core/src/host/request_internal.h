/* host/request_internal.h — reading a request and writing its reply, shared
 * by the control plane (request.c, ta_host_post) and the snapshot reader
 * (snapshot_reader.c, ta_host_snapshot_reader_request): the envelope of
 * docs/requests.md, its params' readers, its errors and the writer of a
 * decoded span. A request is decoded into tokens on the caller's stack and
 * its reply written into `out`. */
#ifndef TINYACTORS_HOST_REQUEST_INTERNAL_H
#define TINYACTORS_HOST_REQUEST_INTERNAL_H

#include <tinyactors/cbor.h>
#include <tinyactors/otlp.h>
#include "base/output.h"

/* Error codes: JSON-RPC's where they fit, the server range for the host's own. */
enum {
    REQUEST_PARSE = -32700,   /* not one well-formed CBOR item */
    REQUEST_INVALID = -32600, /* not a request: its members are wrong */
    REQUEST_METHOD = -32601,  /* no such method */
    REQUEST_PARAMS = -32602,  /* a param has the wrong type, is missing, unknown, or out of range */
    REQUEST_STALE = -32001,   /* the session or watch does not exist (any more) */
    REQUEST_STATE = -32002,   /* the host cannot do it now */
    REQUEST_LIMIT = -32003,   /* more than the request or the host holds */
    REQUEST_MEMORY = -32004,  /* the host's allocator refused */
    REQUEST_RULE = -32005,    /* the host refused a rule as malformed */
    REQUEST_BYTES = -32006    /* bytes in params are not what the method reads */
};

typedef struct request {
    struct ta_host *host;               /* the control plane's; NULL for a snapshot reader */
    const struct ta_host_snapshot_reader *reader; /* the snapshot reader's; NULL for the control plane */
    ta_cbor_document *document;
    ta_output out;
    uint32_t id;   /* the request's "id" token; TA_NIL: none (or not readable) */
    int replies;   /* 1: a request, whose reply carries "id" (null when unreadable); 0: a config */
    int notification; /* no "id": applied, nothing written, errors not reported */
    const char *prefix; /* of a field's path: "params." (a request) or "" (a config) */
    int refused;   /* an error reply was written */
    int no_room;   /* the allocator refused room for the reply: nothing applied */
    char path[96]; /* where the error is, for {"error": {"data": {"path"}}} */
} request;

typedef struct request_method {
    const char *name;
    void (*run)(request *r, uint32_t params); /* params: the map, or TA_NIL for none */
} request_method;

typedef struct request_methods {
    const request_method *methods;
    uint32_t count;
} request_methods;

/* Reads the message of r->document (decoding it) and writes its reply into
 * r->out: {"id", "result"} from the method `methods` names, or {"id",
 * "error"}. 1 when it was a notification (no "id"): applied, nothing
 * written, and r->out replaced by an output that writes nothing. */
int request_answer(request *r, request_methods methods);

ta_string_view request_text_of(const char *text);
int request_text_is(ta_string_view text, const char *name);

/* ---- writing ---- */
void request_put_key(request *r, const char *key);
void request_put_int(request *r, int64_t value);
void request_put_double(request *r, double value);
void request_put_null(request *r);
void request_put_bool(request *r, int value);
/* {"id": ..., `key`: the caller writes the value next */
void request_open(request *r, const char *key);
void request_put_trace_id(request *r, ta_trace_id trace);
void request_put_span_id(request *r, ta_span_id span);
void request_put_scalar(request *r, const ta_otlp_value *value);
void request_put_value(request *r, const ta_otlp_value *value);
void request_put_attributes(request *r, ta_otlp_fields attributes);
/* A decoded span, as trace.spans.decode writes each. */
void request_put_span(request *r, const ta_otlp_span *span);

/* ---- errors: the first one stays; the code follows from the reason ---- */
void request_refuse_with(request *r, int code, const char *reason, const char *message);
void request_refuse(request *r, const char *reason, const char *message);
void request_path_member(request *r, const char *member);
void request_path_view(request *r, const char *outer, uint32_t index, ta_string_view field);
void request_path_set(request *r, const char *outer, uint32_t index, const char *field);

/* ---- reading params: 1 with the value (or `fallback` when absent), 0 after refusing ---- */
const ta_cbor_token *request_token(const request *r, uint32_t at);
int request_keys_known(request *r, uint32_t map, const char *outer, uint32_t index, const char *const *known);
uint32_t request_member(const request *r, uint32_t map, const char *key);
int request_is_null(const request *r, uint32_t at);
int request_read_uint(request *r, uint32_t at, uint64_t fallback, uint64_t *value);
int request_read_number(request *r, uint32_t at, double fallback, double *value);
int request_read_bool(request *r, uint32_t at, int fallback, int *value);
int request_read_text(request *r, uint32_t at, ta_string_view *value);

#endif /* TINYACTORS_HOST_REQUEST_INTERNAL_H */
