/* host_request.h — ta_host_request from tests: build a request with the CBOR
 * writer, send it, and read the reply as tokens and in diagnostic notation
 * (docs/requests.md). */
#ifndef TINYACTORS_TESTS_SUPPORT_HOST_REQUEST_H
#define TINYACTORS_TESTS_SUPPORT_HOST_REQUEST_H

#include <string.h>

#include <tinyactors/cbor.h>
#include <tinyactors/host.h>

#ifndef TEST_UNUSED
#define TEST_UNUSED __attribute__((unused))
#endif

typedef struct host_ask {
    ta_cbor_token tokens[128];
    char strings[2048];
    ta_cbor_writer writer;
    unsigned char request[4096];
    size_t request_length;
    unsigned char reply[1 << 16];
    ta_buffer answer; /* the reply's bytes (answer.length) */
    ta_cbor_token reply_tokens[4096];
    ta_cbor_document document; /* the reply, decoded */
    char text[1 << 16];        /* the reply in diagnostic notation */
} host_ask;

/* A request {"op": op, ...}: `fields` more pairs follow, written with
 * ta_cbor_write_* on the returned writer. */
TEST_UNUSED static ta_cbor_writer *ask_begin(host_ask *a, const char *operation, uint32_t fields) {
    memset(&a->writer, 0, sizeof a->writer);
    a->writer.document.tokens = a->tokens;
    a->writer.document.capacity = 128;
    a->writer.strings = (ta_buffer){a->strings, sizeof a->strings, 0};
    ta_cbor_write_map(&a->writer, fields + 1);
    ta_cbor_write_text(&a->writer, ta_string("op"));
    ta_cbor_write_text(&a->writer, ta_string(operation));
    return &a->writer;
}

/* Send what ask_begin started: the status of ta_host_request; on TA_OK the
 * reply is in a->document and a->text. */
TEST_UNUSED static ta_status ask_send(ta_host *host, host_ask *a) {
    ta_buffer request = {a->request, sizeof a->request, 0};
    if (ta_cbor_finish(&a->writer, &request) != TA_OK) return TA_ERROR_INVALID;
    a->request_length = request.length;
    a->answer = (ta_buffer){a->reply, sizeof a->reply, 0};
    a->text[0] = 0;
    ta_status status = ta_host_request(host, (ta_bytes){a->request, a->request_length}, &a->answer);
    if (status != TA_OK) return status;
    a->document = (ta_cbor_document){{a->reply, a->answer.length}, a->reply_tokens, 4096, 0};
    if (ta_cbor_decode(&a->document) != TA_OK) return TA_ERROR_INVALID;
    ta_buffer text = {a->text, sizeof a->text - 1, 0};
    if (ta_cbor_diagnostic((ta_bytes){a->reply, a->answer.length}, &text) != TA_OK)
        return TA_ERROR_INVALID;
    a->text[text.length] = 0;
    return TA_OK;
}

/* The token of reply field `key` (in the map at `map`, 0: the reply). */
TEST_UNUSED static uint32_t ask_field(const host_ask *a, uint32_t map, const char *key) {
    return ta_cbor_member(&a->document, map, ta_string(key));
}

/* An unsigned field of the reply; UINT64_MAX when missing or not one. */
TEST_UNUSED static uint64_t ask_uint(const host_ask *a, uint32_t map, const char *key) {
    uint32_t at = ask_field(a, map, key);
    if (at == TA_NIL || a->document.tokens[at].kind != TA_CBOR_UINT) return UINT64_MAX;
    return a->document.tokens[at].value;
}

/* {"op": "stats"}: one of the host's counters ("actors", "routed", ...). */
TEST_UNUSED static uint64_t host_stat(ta_host *host, const char *field) {
    static host_ask a;
    ask_begin(&a, "stats", 0);
    if (ask_send(host, &a) != TA_OK) return UINT64_MAX;
    uint32_t stats = ask_field(&a, 0, "host");
    return stats == TA_NIL ? UINT64_MAX : ask_uint(&a, stats, field);
}

/* {"op": "state", "session": session}: the reply's "value" in diagnostic
 * notation into text (cut to fit), or the error reply's text; the status of
 * ta_host_request. */
TEST_UNUSED static ta_status ask_state(ta_host *host, uint64_t session, char *text,
                                       size_t capacity) {
    static host_ask a;
    ta_cbor_writer *w = ask_begin(&a, "state", 1);
    ta_cbor_write_text(w, ta_string("session"));
    ta_cbor_write_uint(w, session);
    ta_status status = ask_send(host, &a);
    if (capacity) text[0] = 0;
    if (status != TA_OK) return status;
    const char *value = a.text;
    if (strncmp(value, "{\"value\":", 9) == 0) {
        value += 9;
        size_t n = strlen(value);
        if (n && value[n - 1] == '}') n--;
        if (n >= capacity) n = capacity ? capacity - 1 : 0;
        memcpy(text, value, n);
        if (capacity) text[n] = 0;
    } else if (capacity) {
        strncpy(text, a.text, capacity - 1);
        text[capacity - 1] = 0;
    }
    return TA_OK;
}

/* {"op": "stats"}'s "host" map as a ta_host_stats (out->size: set by this). */
TEST_UNUSED static void host_stats_fill(ta_host *host, ta_host_stats *out) {
    static host_ask a;
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    ask_begin(&a, "stats", 0);
    if (ask_send(host, &a) != TA_OK) return;
    uint32_t m = ask_field(&a, 0, "host");
    out->segments = (uint32_t)ask_uint(&a, m, "segments");
    out->actors = (uint32_t)ask_uint(&a, m, "actors");
    out->hibernated = (uint32_t)ask_uint(&a, m, "hibernated");
    out->runnable = (uint32_t)ask_uint(&a, m, "runnable");
    out->jobs = (uint32_t)ask_uint(&a, m, "jobs");
    out->region_bytes = ask_uint(&a, m, "regionBytes");
    out->delivered = ask_uint(&a, m, "delivered");
    out->routed = ask_uint(&a, m, "routed");
    out->migrated = ask_uint(&a, m, "migrated");
    out->retired = ask_uint(&a, m, "retired");
    out->fuel_clock = ask_uint(&a, m, "fuelClock");
}

/* {"op": "stats"}'s "charts" into out (up to capacity); returns how many
 * there are. Names point into a reply buffer the next call reuses. */
TEST_UNUSED static uint32_t host_chart_stats_fill(ta_host *host, ta_chart_stats *out,
                                                  uint32_t capacity) {
    static host_ask a;
    ask_begin(&a, "stats", 0);
    if (ask_send(host, &a) != TA_OK) return 0;
    uint32_t charts = ask_field(&a, 0, "charts"), count = a.document.tokens[charts].count;
    uint32_t at = charts + 1;
    for (uint32_t i = 0; i < count; i++, at = ta_cbor_next(&a.document, at)) {
        if (i >= capacity) continue;
        ta_chart_stats *c = &out[i];
        memset(c, 0, sizeof *c);
        c->size = sizeof *c;
        c->name = ta_cbor_string(&a.document, ask_field(&a, at, "name"));
        c->actors = (uint32_t)ask_uint(&a, at, "actors");
        c->hibernated = (uint32_t)ask_uint(&a, at, "hibernated");
        c->spawned = ask_uint(&a, at, "spawned");
        c->ended = ask_uint(&a, at, "ended");
        c->turns = ask_uint(&a, at, "turns");
        c->macrosteps = ask_uint(&a, at, "macrosteps");
        c->fuel = ask_uint(&a, at, "fuel");
        c->errors_execution = ask_uint(&a, at, "errorsExecution");
        c->errors_communication = ask_uint(&a, at, "errorsCommunication");
        c->errors_platform = ask_uint(&a, at, "errorsPlatform");
        c->faults = ask_uint(&a, at, "faults");
    }
    return count;
}

/* {"op": "actors"}: up to out.capacity sessions from *cursor (0 to start);
 * returns how many, 0 when every session was listed. */
TEST_UNUSED static uint32_t host_actors_page(ta_host *host, uint64_t *cursor, ta_uint64_array out) {
    static host_ask a;
    ta_cbor_writer *w = ask_begin(&a, "actors", 2);
    ta_cbor_write_text(w, ta_string("cursor"));
    ta_cbor_write_uint(w, *cursor);
    ta_cbor_write_text(w, ta_string("limit"));
    ta_cbor_write_uint(w, out.capacity);
    if (ask_send(host, &a) != TA_OK) return 0;
    uint32_t sessions = ask_field(&a, 0, "sessions"), next = ask_field(&a, 0, "cursor");
    uint32_t count = a.document.tokens[sessions].count;
    for (uint32_t i = 0; i < count; i++)
        out.pointer[i] = a.document.tokens[sessions + 1 + i].value;
    *cursor = a.document.tokens[next].kind == TA_CBOR_UINT ? a.document.tokens[next].value
                                                           : UINT64_MAX;
    return count;
}

#endif /* TINYACTORS_TESTS_SUPPORT_HOST_REQUEST_H */
