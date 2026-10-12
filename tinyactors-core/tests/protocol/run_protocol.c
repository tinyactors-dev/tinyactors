/* Protocol vectors (tests/protocol/README.md): the bytes a host takes and
 * gives across its API — requests and replies (docs/requests.md), host
 * configs (docs/host-config.md) and envelopes (docs/protocol.md, Envelope) —
 * checked byte for byte, but where the expected side holds a wildcard.
 * Usage: run_protocol <directory with requests.txt, config.txt, envelopes.txt>. */
#include "../support/compile_util.h"
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include <tinyactors/trace.h>
#include "../support/host_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An expected item 39999("uint") matches any one item of that type
 * ("uint", "int", "text", "bytes", "float", "any"). Tag 39999 is unassigned
 * in IANA's CBOR tags registry; core never writes it. */
#define WILDCARD_TAG 39999u

#define PINGED                                                                                     \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='pinged' "         \
    "initial='s'><datamodel><data id='n' expr='1'/><data id='who' expr=\"'bob'\"/>"              \
    "<data id='nothing'/></datamodel><state id='s'><transition event='ping' target='t'>"         \
    "<assign location='n' expr='n + 1'/></transition></state><state id='t'/></scxml>"
/* "go" sends "ping" with n to _event.data.to, and a "note" without data */
#define PINGER                                                                                     \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='pinger' "         \
    "initial='s'><state id='s'><transition event='go'><send targetexpr='_event.data.to' "          \
    "event='ping' id='p1'><param name='n' expr='_event.data.n'/></send><send "                     \
    "targetexpr='_event.data.to' event='note' id='p2'/></transition></state></scxml>"

/* ---- one host and what a setup put in it ---------------------------------------------- */

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    ta_image image;
    uint32_t peer_a, peer_c; /* "relay": the peers it delivers from and relays to */
} world;

static int failures, cases;

static void fail(const char *file, const char *name, const char *what) {
    failures++;
    fprintf(stderr, "FAIL %s \"%s\": %s\n", file, name, what);
}

static void world_settle(world *w) {
    ta_run result;
    int rounds = 0;
    do {
        ta_host_advance_time(w->host, (ta_host_time){0, 1700000000000000000ull});
        result = ta_host_run(w->host, 1000000);
        const ta_host_effect *effects;
        ta_host_effects(w->host, &effects);
        ta_host_effects_ack(w->host);
    } while (result.status != TA_RUN_IDLE && ++rounds < 100);
}

static void world_close(world *w) {
    if (w->host) ta_host_free(w->host);
    if (w->ecmascript) ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image_bytes);
    memset(w, 0, sizeof *w);
}

static uint32_t world_peer(world *w, const char *id) {
    ta_peer_config peer;
    memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    peer.flags = TA_PEER_TRUSTED;
    peer.id = ta_string(id);
    uint32_t handle = TA_NIL;
    if (ta_host_peer_add(w->host, &peer, &handle) != TA_OK) return TA_NIL;
    return handle;
}

static uint64_t world_spawn(world *w) {
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    uint64_t session = 0;
    ta_host_spawn(w->host, &spawn, &session);
    return session;
}

/* The setups (README.md): a host "h" with the ECMAScript datamodel, and
 *   empty   no chart;
 *   pinged  the chart "pinged" and one session (1) of it, run until idle;
 *   pinged2 the same with two sessions (1, 2);
 *   pinger  a host "a" with a peer "b" and the chart "pinger"; session 1 got
 *           "go" with {to: "tinyactors://b/@echo", n: 1}: two envelopes out;
 *   relay   a host "b" with the peers "a" and "c", no chart. */
static int world_open(world *w, const char *setup) {
    memset(w, 0, sizeof *w);
    const char *chart = NULL, *id = "h";
    if (!strcmp(setup, "pinged") || !strcmp(setup, "pinged2")) chart = PINGED;
    else if (!strcmp(setup, "pinger")) chart = PINGER, id = "a";
    else if (!strcmp(setup, "relay")) id = "b";
    else if (strcmp(setup, "empty")) return 0;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = ta_string(id);
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    config.system.trace_seed = 1;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    config.max_subscriptions = 1;
    if (test_host_new(&config, &w->host) != TA_OK) return 0;
    if (!strcmp(setup, "relay")) {
        w->peer_a = world_peer(w, "a");
        w->peer_c = world_peer(w, "c");
        return w->peer_a != TA_NIL && w->peer_c != TA_NIL;
    }
    if (!chart) return 1;
    size_t length;
    w->image_bytes = compile_chart("c.scxml", chart, NULL, &length, 0);
    uint32_t segment;
    if (!w->image_bytes || ta_image_open((ta_bytes){w->image_bytes, length}, &w->image) != TA_OK ||
        ta_host_add_image(w->host, &w->image, &segment) != TA_OK)
        return 0;
    uint64_t session = world_spawn(w);
    if (!strcmp(setup, "pinged2")) world_spawn(w);
    if (!strcmp(setup, "pinger")) {
        if (world_peer(w, "b") == TA_NIL) return 0;
        ta_payload data = ta_datamodel_ecmascript_payload_eval(
            w->ecmascript, 0, ta_string("({to: 'tinyactors://b/@echo', n: 1})"));
        ta_event_in event;
        memset(&event, 0, sizeof event);
        event.size = sizeof event;
        event.session = session;
        event.name = ta_string("go");
        event.data = data;
        if (ta_host_send(w->host, &event) != TA_OK) return 0;
        return 1; /* not settled: the case takes the envelopes */
    }
    world_settle(w);
    return 1;
}

/* ---- hex, diagnostic notation, and matching ---------------------------------------------- */

static size_t hex_read(const char *text, unsigned char *out, size_t capacity) {
    size_t n = 0;
    for (const char *p = text; p[0] && p[1] && n < capacity; p += 2) {
        unsigned int byte;
        if (sscanf(p, "%2x", &byte) != 1) break;
        out[n++] = (unsigned char)byte;
    }
    return n;
}

static void diagnostic(ta_bytes bytes, char *text, size_t capacity) {
    ta_buffer out = {text, capacity - 1, 0};
    if (bytes.length == 0) {
        snprintf(text, capacity, "(nothing)");
        return;
    }
    ta_status status = ta_cbor_diagnostic(bytes, &out);
    text[status == TA_OK ? out.length : 0] = 0;
    if (status != TA_OK) snprintf(text, capacity, "(not CBOR)");
}

static void hex_write(ta_bytes bytes, char *text, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; i < bytes.length && n + 3 < capacity; i++)
        n += (size_t)snprintf(text + n, capacity - n, "%02x", ((const unsigned char *)bytes.pointer)[i]);
    text[n] = 0;
}

static int wildcard_matches(ta_string_view type, uint32_t kind) {
    if (type.length == 3 && !memcmp(type.pointer, "any", 3)) return 1;
    if (type.length == 4 && !memcmp(type.pointer, "uint", 4)) return kind == TA_CBOR_UINT;
    if (type.length == 3 && !memcmp(type.pointer, "int", 3)) return kind == TA_CBOR_UINT || kind == TA_CBOR_NEGINT;
    if (type.length == 4 && !memcmp(type.pointer, "text", 4)) return kind == TA_CBOR_TEXT;
    if (type.length == 5 && !memcmp(type.pointer, "bytes", 5)) return kind == TA_CBOR_BYTES;
    if (type.length == 5 && !memcmp(type.pointer, "float", 5)) return kind == TA_CBOR_FLOAT;
    return 0;
}

/* The expected item at *e against the actual one at *a; both move past it. */
static int item_matches(const ta_cbor_document *expected, uint32_t *e, const ta_cbor_document *actual,
                        uint32_t *a) {
    const ta_cbor_token *x = &expected->tokens[*e], *y = &actual->tokens[*a];
    if (x->kind == TA_CBOR_TAG && x->value == WILDCARD_TAG) {
        ta_string_view type = ta_cbor_string(expected, *e + 1);
        *e = ta_cbor_next(expected, *e);
        int ok = wildcard_matches(type, y->kind);
        *a = ta_cbor_next(actual, *a);
        return ok;
    }
    if (x->kind != y->kind || x->count != y->count) return 0;
    if (x->kind == TA_CBOR_TEXT || x->kind == TA_CBOR_BYTES) {
        ta_string_view s = ta_cbor_string(expected, *e), t = ta_cbor_string(actual, *a);
        if (s.length != t.length || memcmp(s.pointer, t.pointer, s.length)) return 0;
    } else if (x->kind != TA_CBOR_ARRAY && x->kind != TA_CBOR_MAP && x->value != y->value) {
        return 0;
    }
    uint32_t items = x->kind == TA_CBOR_ARRAY ? x->count
                     : x->kind == TA_CBOR_MAP ? 2 * x->count
                     : x->kind == TA_CBOR_TAG ? 1
                                              : 0;
    (*e)++;
    (*a)++;
    for (uint32_t i = 0; i < items; i++)
        if (!item_matches(expected, e, actual, a)) return 0;
    return 1;
}

static int bytes_match(ta_bytes expected, ta_bytes actual) {
    static ta_cbor_token x[4096], y[4096];
    ta_cbor_document d = {expected, x, 4096, 0}, g = {actual, y, 4096, 0};
    if (ta_cbor_decode(&d) != TA_OK || ta_cbor_decode(&g) != TA_OK) return 0;
    uint32_t e = 0, a = 0;
    return item_matches(&d, &e, &g, &a);
}

/* ---- a case: its lines -------------------------------------------------------------------- */

#define LINES 32
typedef struct vector {
    const char *file;
    char kind[32], name[160];
    char key[LINES][16];
    char *value[LINES];
    int count;
} vector;

static const char *vector_get(const vector *v, const char *key, int nth) {
    for (int i = 0; i < v->count; i++)
        if (!strcmp(v->key[i], key) && nth-- == 0) return v->value[i];
    return NULL;
}

/* Every hex line followed by a diag line must read as that diag. */
static void vector_check_diagnostics(const vector *v) {
    static unsigned char bytes[1 << 16];
    static char text[1 << 16];
    for (int i = 1; i < v->count; i++) {
        if (strcmp(v->key[i], "diag")) continue;
        size_t n = hex_read(v->value[i - 1], bytes, sizeof bytes);
        diagnostic((ta_bytes){bytes, n}, text, sizeof text);
        if (strcmp(text, v->value[i])) {
            char what[1024];
            snprintf(what, sizeof what, "the %s line reads as %.400s, not as its diag line", v->key[i - 1], text);
            fail(v->file, v->name, what);
        }
    }
}

static void report_mismatch(const vector *v, const char *what, ta_bytes expected, ta_bytes actual) {
    static char want[1 << 15], got[1 << 15], hex[1 << 15], message[1 << 17];
    diagnostic(expected, want, sizeof want);
    diagnostic(actual, got, sizeof got);
    hex_write(actual, hex, sizeof hex);
    snprintf(message, sizeof message, "%s\n  want %s\n  got  %s\n  got  %s", what, want, got, hex);
    fail(v->file, v->name, message);
}

/* ---- requests.txt ---------------------------------------------------------------------------- */

/* One request and its expected reply ("none": a notification's). With a
 * capacity: first refused in that many bytes and measured; the retry in the
 * length it asked for is the reply. */
static void ask(const vector *v, ta_host *host, const char *send, const char *want, const char *capacity) {
    static unsigned char request[1 << 16], expected[1 << 16], reply[1 << 16];
    ta_bytes in = {request, hex_read(send, request, sizeof request)};
    ta_buffer out = {reply, sizeof reply, 0};
    if (capacity) {
        size_t room = (size_t)strtoul(capacity, NULL, 10);
        out.capacity = room;
        if (ta_host_request(host, in, &out) != TA_ERROR_NO_MEMORY || out.length <= room)
            fail(v->file, v->name, "a reply larger than capacity is not TA_ERROR_NO_MEMORY with its length");
        out = (ta_buffer){reply, out.length, 0};
    }
    size_t asked = out.capacity;
    if (ta_host_request(host, in, &out) != TA_OK) {
        fail(v->file, v->name, "ta_host_request did not return TA_OK");
    } else if (capacity && out.length != asked) {
        fail(v->file, v->name, "the retry's reply is not the length TA_ERROR_NO_MEMORY asked for");
    } else if (!strcmp(want, "none")) {
        if (out.length != 0)
            report_mismatch(v, "a notification got a reply", (ta_bytes){NULL, 0}, (ta_bytes){reply, out.length});
    } else {
        ta_bytes expect = {expected, hex_read(want, expected, sizeof expected)};
        if (!bytes_match(expect, (ta_bytes){reply, out.length}))
            report_mismatch(v, "the reply differs", expect, (ta_bytes){reply, out.length});
    }
}

/* Every send with the reply after it, in order, on one host; a capacity
 * line applies to the send after it. */
static void run_request(const vector *v) {
    const char *setup = vector_get(v, "setup", 0);
    world w;
    if (!world_open(&w, setup ? setup : "empty")) {
        world_close(&w);
        fail(v->file, v->name, "unknown setup");
        return;
    }
    const char *capacity = NULL, *send = NULL;
    int asked = 0;
    for (int i = 0; i < v->count; i++) {
        if (!strcmp(v->key[i], "capacity")) capacity = v->value[i];
        else if (!strcmp(v->key[i], "send")) send = v->value[i];
        else if (!strcmp(v->key[i], "reply") && send) {
            ask(v, w.host, send, v->value[i], capacity);
            send = capacity = NULL;
            asked++;
        }
    }
    if (!asked || send) fail(v->file, v->name, "each send needs a reply line (or reply none)");
    world_close(&w);
}

/* ---- config.txt ------------------------------------------------------------------------------- */

static void run_config(const vector *v) {
    static unsigned char config[8192], expected[8192], error[8192], request[8192], reply[1 << 16];
    const char *cbor = vector_get(v, "cbor", 0), *want = vector_get(v, "error", 0);
    if (!cbor || (!want && !vector_get(v, "ok", 0))) {
        fail(v->file, v->name, "needs cbor, and error or ok");
        return;
    }
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    ta_buffer error_reply = {error, sizeof error, 0};
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodels = &binding;
    environment.datamodel_count = 1;
    environment.allocator = ta_runtime_malloc();
    environment.error = &error_reply;
    ta_host *host = NULL;
    ta_status status = ta_host_create((ta_bytes){config, hex_read(cbor, config, sizeof config)}, &environment, &host);
    if (want) {
        ta_bytes expect = {expected, hex_read(want, expected, sizeof expected)};
        if (status != TA_ERROR_INVALID) fail(v->file, v->name, "ta_host_create did not refuse the config");
        else if (!bytes_match(expect, (ta_bytes){error, error_reply.length}))
            report_mismatch(v, "the error differs", expect, (ta_bytes){error, error_reply.length});
    } else if (status != TA_OK) {
        report_mismatch(v, "ta_host_create refused the config", (ta_bytes){NULL, 0}, (ta_bytes){error, error_reply.length});
    } else { /* then requests on the new host */
        const char *send, *answer;
        for (int k = 0; (send = vector_get(v, "send", k)) && (answer = vector_get(v, "reply", k)); k++) {
            ta_buffer out = {reply, sizeof reply, 0};
            ta_bytes expect = {expected, hex_read(answer, expected, sizeof expected)};
            if (ta_host_request(host, (ta_bytes){request, hex_read(send, request, sizeof request)}, &out) != TA_OK ||
                !bytes_match(expect, (ta_bytes){reply, out.length}))
                report_mismatch(v, "the reply differs", expect, (ta_bytes){reply, out.length});
        }
    }
    if (host) ta_host_free(host);
    ta_datamodel_ecmascript_free(ecmascript);
}

/* ---- envelopes.txt ------------------------------------------------------------------------------ */

/* An envelope (or error envelope) read back into a PEER (PEER_ERROR) effect,
 * so ta_host_envelope writes it again. */
static ta_status write_back(ta_host *host, const ta_envelope *envelope, const ta_peer_error *error,
                            ta_buffer *out) {
    ta_host_effect effect;
    memset(&effect, 0, sizeof effect);
    effect.size = sizeof effect;
    if (error) {
        effect.kind = TA_HOST_EFFECT_PEER_ERROR;
        effect.token = error->reason;
        effect.auxiliary = error->origin;
        effect.msgid = error->msgid;
        effect.sendid = error->sendid;
        return ta_host_envelope(host, &effect, out);
    }
    effect.kind = TA_HOST_EFFECT_PEER;
    effect.target = envelope->target;
    effect.event = envelope->name;
    effect.sendid = envelope->sendid;
    effect.auxiliary = envelope->origin;
    effect.origintype = envelope->origintype;
    effect.msgid = envelope->msgid;
    effect.deduplication_key = envelope->deduplication_key;
    effect.flags = envelope->hops;
    effect.wire = envelope->wire;
    effect.datamodel = TA_NIL;
    if (envelope->traceparent.length && ta_traceparent_parse(envelope->traceparent, &effect.trace) != TA_OK)
        return TA_ERROR_INVALID;
    return ta_host_envelope(host, &effect, out);
}

/* The PEER and PEER_ERROR effects of one run, written as bytes, against the
 * vector's `envelope` lines in order. */
static void check_effects(const vector *v, world *w) {
    static unsigned char bytes[8192], expected[8192];
    ta_host_advance_time(w->host, (ta_host_time){0, 1700000000000000000ull});
    ta_host_run(w->host, 1000000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w->host, &effects), k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (effects[i].kind != TA_HOST_EFFECT_PEER && effects[i].kind != TA_HOST_EFFECT_PEER_ERROR) continue;
        const char *want = vector_get(v, "envelope", (int)k++);
        ta_buffer out = {bytes, sizeof bytes, 0};
        if (ta_host_envelope(w->host, &effects[i], &out) != TA_OK) {
            fail(v->file, v->name, "ta_host_envelope refused an effect");
            continue;
        }
        if (!want) {
            report_mismatch(v, "an envelope more than expected", (ta_bytes){NULL, 0}, (ta_bytes){bytes, out.length});
            continue;
        }
        ta_bytes expect = {expected, hex_read(want, expected, sizeof expected)};
        if (!bytes_match(expect, (ta_bytes){bytes, out.length}))
            report_mismatch(v, "the envelope differs", expect, (ta_bytes){bytes, out.length});
    }
    if (vector_get(v, "envelope", (int)k)) fail(v->file, v->name, "fewer envelopes than expected");
    ta_host_effects_ack(w->host);
}

static void run_envelope(const vector *v) {
    static unsigned char input[8192], back[8192], written[8192];
    const char *cbor = vector_get(v, "cbor", 0), *setup = vector_get(v, "setup", 0);
    world w;
    const char *kind = v->kind;
    int routed = !strcmp(kind, "relay") || !strcmp(kind, "refused");
    if (!world_open(&w, setup ? setup : routed ? "relay" : "empty")) {
        world_close(&w);
        fail(v->file, v->name, "unknown setup");
        return;
    }
    ta_bytes in = {input, cbor ? hex_read(cbor, input, sizeof input) : 0};
    if (!strcmp(kind, "send")) {
        check_effects(v, &w);
    } else if (!strcmp(kind, "relay") || !strcmp(kind, "refused")) {
        /* from peer "a": relayed on to "c" as it came, one hop more; or
         * refused, with an error envelope back to "a" */
        ta_envelope envelope;
        int32_t outcome = !strcmp(kind, "relay") ? TA_ROUTE_RELAYED : TA_ROUTE_REFUSED;
        if (ta_host_envelope_read(w.host, in, &envelope) != TA_OK) fail(v->file, v->name, "not read");
        else if (ta_host_deliver(w.host, w.peer_a, &envelope) != outcome)
            fail(v->file, v->name, outcome == TA_ROUTE_RELAYED ? "not relayed" : "not refused");
        else check_effects(v, &w);
    } else if (!strcmp(kind, "roundtrip") || !strcmp(kind, "error-roundtrip")) {
        const char *want = vector_get(v, "back", 0);
        ta_bytes expect = want ? (ta_bytes){back, hex_read(want, back, sizeof back)} : in;
        ta_buffer out = {written, sizeof written, 0};
        ta_envelope envelope;
        ta_peer_error error;
        int is_error = !strcmp(kind, "error-roundtrip");
        ta_status status = is_error ? ta_host_peer_error_read(in, &error) : ta_host_envelope_read(w.host, in, &envelope);
        if (status != TA_OK) fail(v->file, v->name, "not read");
        else if (write_back(w.host, is_error ? NULL : &envelope, is_error ? &error : NULL, &out) != TA_OK)
            fail(v->file, v->name, "not written back");
        else if (!bytes_match(expect, (ta_bytes){written, out.length}))
            report_mismatch(v, "written back differently", expect, (ta_bytes){written, out.length});
    } else if (!strcmp(kind, "invalid") || !strcmp(kind, "error-invalid")) {
        ta_envelope envelope;
        ta_peer_error error;
        ta_status status = !strcmp(kind, "invalid") ? ta_host_envelope_read(w.host, in, &envelope)
                                                    : ta_host_peer_error_read(in, &error);
        if (status != TA_ERROR_INVALID) fail(v->file, v->name, "not refused as TA_ERROR_INVALID");
    } else {
        fail(v->file, v->name, "unknown kind");
    }
    world_close(&w);
}

/* ---- files -------------------------------------------------------------------------------------- */

static void run_vector(vector *v) {
    if (!v->kind[0]) return;
    cases++;
    vector_check_diagnostics(v);
    if (!strcmp(v->kind, "request")) run_request(v);
    else if (!strcmp(v->kind, "config")) run_config(v);
    else run_envelope(v);
}

static int run_file(const char *directory, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "run_protocol: cannot read %s\n", path);
        return 0;
    }
    static char lines[LINES][1 << 15];
    static vector v;
    memset(&v, 0, sizeof v);
    v.file = name;
    int before = cases;
    char line[1 << 15];
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#') continue;
        if (!line[0]) {
            run_vector(&v);
            memset(&v, 0, sizeof v);
            v.file = name;
            continue;
        }
        char *space = strchr(line, ' ');
        char *rest = space ? space + 1 : line + strlen(line);
        if (space) *space = 0;
        if (!v.kind[0]) {
            snprintf(v.kind, sizeof v.kind, "%s", line);
            snprintf(v.name, sizeof v.name, "%s", rest);
        } else if (v.count < LINES) {
            snprintf(v.key[v.count], sizeof v.key[v.count], "%s", line);
            snprintf(lines[v.count], sizeof lines[v.count], "%s", rest);
            v.value[v.count] = lines[v.count];
            v.count++;
        }
    }
    run_vector(&v);
    fclose(file);
    printf("ok   %s: %d cases\n", name, cases - before);
    return 1;
}

int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : "tests/protocol";
    int read = run_file(directory, "requests.txt") & run_file(directory, "config.txt") &
               run_file(directory, "envelopes.txt");
    printf("%d cases, %d failures\n", cases, failures);
    return read && failures == 0 ? 0 : 1;
}
