/* The router of ta_host (docs/protocol.md: Routing, Security, Errors,
 * Delivery, Observability): three hosts "a", "b", "c" wired by a test
 * transport that moves TA_HOST_EFFECT_PEER / TA_HOST_EFFECT_PEER_ERROR effects into
 * the other host's ta_host_deliver / ta_host_peer_error. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/cbor.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"
#include "../support/host_config.h"

#include <stdlib.h>
#include <string.h>

#define LOGCHK(i, want)                                                                            \
    do {                                                                                           \
        CHECK(strcmp(nodes[i].log, want) == 0);                                                    \
        if (strcmp(nodes[i].log, want))                                                            \
            fprintf(stderr, "  log %s: \"%s\", want \"%s\"\n", IDS[i], nodes[i].log, want);        \
    } while (0)
#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"

/* a pinger: "go" sends "ping" to _event.data.to; logs replies and errors */
#define PINGER                                                                                     \
    SC("pinger", "initial='s'><state id='s'>"                                                      \
                 "<transition event='go'><send targetexpr='_event.data.to' event='ping' "          \
                 "id='p1'><param name='n' expr='_event.data.n'/></send></transition>"              \
                 "<transition event='pong'><log expr=\"'pong:' + _event.data.n\"/></transition>"   \
                 "<transition event='error.communication'><log "                                   \
                 "expr=\"'comm:' + _event.sendid\"/></transition></state>")
/* an echo: replies "pong" to _event.origin */
#define ECHO                                                                                       \
    SC("echo", "initial='s'><datamodel><data id='seen' expr='0'/></datamodel><state id='s'>"       \
               "<transition event='ping'><assign location='seen' expr='seen + 1'/>"                \
               "<send targetexpr='_event.origin' event='pong'>"                                    \
               "<param name='n' expr='_event.data.n'/></send></transition>"                        \
               "<transition event='other'><log expr=\"'other'\"/></transition></state>")

enum { A, B, C, N };
static const char *IDS[N] = {"a", "b", "c"};

typedef struct node {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *bytes;
    ta_image image;
    uint32_t peer[N]; /* handle of each other node in this node's router */
    char log[4096];
} node;

static node nodes[N];
/* a node's datamodel vtable, when a test counts its calls (NULL: ECMAScript's) */
static const ta_datamodel_vtable *vtable_for[N];
static int link_up[N][N]; /* the test transport between i and j */
static int dropped;       /* envelopes the transport lost (link down, no queue) */
static int over_bytes;    /* the transport crosses a process boundary: CBOR envelopes */

static void open_node(int i, const char *chart) {
    node *n = &nodes[i];
    memset(n, 0, sizeof *n);
    n->ecmascript = ta_datamodel_ecmascript_new(NULL);
    n->binding = ta_datamodel_ecmascript_binding(n->ecmascript);
    if (vtable_for[i]) n->binding.vtable = vtable_for[i];
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){IDS[i], 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &n->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(test_host_new(&config, &n->host), TA_OK);
    size_t length;
    n->bytes = compile_chart("c.scxml", chart, NULL, &length, 0);
    CHECK(n->bytes && ta_image_open((ta_bytes){n->bytes, length}, &n->image) == TA_OK);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(n->host, &n->image, &segment), TA_OK);
}

static void close_all(void) {
    for (int i = 0; i < N; i++) {
        if (nodes[i].host) ta_host_free(nodes[i].host);
        if (nodes[i].ecmascript) ta_datamodel_ecmascript_free(nodes[i].ecmascript);
        free(nodes[i].bytes);
        memset(&nodes[i], 0, sizeof nodes[i]); /* nodes a test did not open stay closed */
    }
    memset(link_up, 0, sizeof link_up);
    memset(vtable_for, 0, sizeof vtable_for);
    dropped = 0;
    over_bytes = 0;
}

static uint32_t connect(int i, int j, uint32_t flags, const char *const *reach,
                        uint32_t reach_count) {
    ta_peer_config peer_config;
    memset(&peer_config, 0, sizeof peer_config);
    peer_config.size = sizeof peer_config;
    peer_config.flags = flags;
    peer_config.id = (ta_string_view){IDS[j], 1};
    ta_string_view r[4];
    for (uint32_t k = 0; k < reach_count; k++)
        r[k] = (ta_string_view){reach[k], (uint32_t)strlen(reach[k])};
    peer_config.reach = (ta_string_list){r, reach_count};
    peer_config.max_queue = 4;
    peer_config.queue_ttl = 1000;
    CHECK_EQUAL(ta_host_peer_add(nodes[i].host, &peer_config, &nodes[i].peer[j]), TA_OK);
    link_up[i][j] = 1;
    return nodes[i].peer[j];
}

static int from_peer(int i, uint32_t peer) {
    for (int k = 0; k < N; k++)
        if (nodes[i].peer[k] == peer) return k;
    return -1;
}

static int pump(void) { /* run every node once; move envelopes; returns 1 if anything happened */
    int busy = 0;
    for (int i = 0; i < N; i++) {
        if (!nodes[i].host) continue;
        ta_run result = ta_host_run(nodes[i].host, 100000);
        if (result.status != TA_RUN_IDLE) busy = 1;
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(nodes[i].host, &effects);
        for (uint32_t k = 0; k < n; k++) {
            const ta_host_effect *effect = &effects[k];
            if (effect->kind == TA_EFFECT_LOG) {
                size_t l = strlen(nodes[i].log);
                snprintf(nodes[i].log + l, sizeof nodes[i].log - l, "%.*s;",
                         (int)effect->event.length, effect->event.pointer);
            }
            int j =
                (effect->kind == TA_HOST_EFFECT_PEER || effect->kind == TA_HOST_EFFECT_PEER_ERROR)
                    ? from_peer(i, effect->peer)
                    : -1;
            if (j < 0) continue;
            busy = 1;
            if (!link_up[i][j]) {
                dropped++;
                continue;
            }
            if (over_bytes) { /* ta_host_envelope, then the read on the other side */
                unsigned char bytes[2048];
                ta_buffer written = {bytes, sizeof bytes, 0};
                CHECK_EQUAL(ta_host_envelope(nodes[i].host, effect, &written), TA_OK);
                ta_bytes wire = {bytes, written.length};
                if (effect->kind == TA_HOST_EFFECT_PEER_ERROR) {
                    ta_peer_error error;
                    CHECK_EQUAL(ta_host_peer_error_read(wire, &error), TA_OK);
                    error.peer = nodes[j].peer[i];
                    CHECK_EQUAL(ta_host_peer_error(nodes[j].host, &error), TA_OK);
                    continue;
                }
                ta_envelope envelope;
                CHECK_EQUAL(ta_host_envelope_read(nodes[j].host, wire, &envelope), TA_OK);
                int32_t outcome = ta_host_deliver(nodes[j].host, nodes[j].peer[i], &envelope);
                CHECK(outcome >= 0);
                if (outcome != TA_ROUTE_DELIVERED && outcome != TA_ROUTE_RELAYED && envelope.data)
                    ta_datamodel_ecmascript_vtable()->release(nodes[j].ecmascript, envelope.data);
                continue;
            }
            if (effect->kind == TA_HOST_EFFECT_PEER_ERROR) {
                ta_host_peer_error(nodes[j].host, &(ta_peer_error){nodes[j].peer[i], effect->token,
                                                                   effect->auxiliary, effect->msgid,
                                                                   effect->sendid});
                continue;
            }
            /* across: the payload as a neutral value, CBOR (separate datamodels) */
            unsigned char cbor[1024];
            ta_buffer wire = {cbor, sizeof cbor, 0};
            if (effect->payload && !effect->wire.length)
                CHECK_EQUAL(ta_datamodel_ecmascript_payload_to_cbor(nodes[i].ecmascript,
                                                                    effect->payload, &wire),
                            TA_OK);
            ta_envelope envelope;
            memset(&envelope, 0, sizeof envelope);
            envelope.size = sizeof envelope;
            envelope.hops = effect->flags;
            envelope.target = effect->target;
            envelope.name = effect->event;
            envelope.sendid = effect->sendid;
            envelope.origin = effect->auxiliary;
            envelope.origintype = effect->origintype;
            envelope.msgid = effect->msgid;
            envelope.deduplication_key = effect->deduplication_key;
            char traceparent[TA_TRACEPARENT_LENGTH]; /* the transport writes it on the wire */
            envelope.traceparent.pointer = traceparent;
            envelope.traceparent.length =
                ta_traceparent_format(&effect->trace,
                                      &(ta_buffer){traceparent, sizeof traceparent, 0}) == TA_OK
                    ? TA_TRACEPARENT_LENGTH
                    : 0;
            if (wire.length)
                CHECK_EQUAL(ta_datamodel_ecmascript_payload_from_cbor(
                                nodes[j].ecmascript, (ta_bytes){cbor, wire.length}, &envelope.data),
                            TA_OK);
            envelope.wire = (ta_bytes){effect->wire.pointer, effect->wire.length}; /* relayed */
            int32_t outcome = ta_host_deliver(nodes[j].host, nodes[j].peer[i], &envelope);
            CHECK(outcome >= 0);
            if (outcome != TA_ROUTE_DELIVERED && outcome != TA_ROUTE_RELAYED && envelope.data)
                ta_datamodel_ecmascript_vtable()->release(nodes[j].ecmascript, envelope.data);
        }
        ta_host_effects_ack(nodes[i].host);
    }
    return busy;
}

static void settle(void) {
    for (int k = 0; k < 50 && pump(); k++)
        ;
}

static uint64_t spawn(int i, const char *chart, const char *name) {
    ta_host_chart_ref ref;
    CHECK_EQUAL(
        ta_host_chart(nodes[i].host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref),
        TA_OK);
    uint64_t s = 0;
    CHECK_EQUAL(ta_host_spawn(nodes[i].host,
                              &(ta_spawn){ref.segment, ref.chart,
                                          (ta_string_view){name, (uint32_t)strlen(name)}, 0,
                                          (ta_string_view){0, 0}},
                              &s),
                TA_OK);
    return s;
}

static void go(int i, uint64_t session, const char *to, int n) {
    char json[256];
    snprintf(json, sizeof json, "{\"to\": \"%s\", \"n\": %d}", to, n);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"go", 2};
    event.data = ta_datamodel_ecmascript_payload_eval(
        nodes[i].ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = session;
    CHECK_EQUAL(ta_host_send(nodes[i].host, &event), TA_OK);
    settle();
}

/* a -> b and back, by session and by name; status counters */
static void test_route_status(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    uint64_t p = spawn(A, "pinger", "p");
    uint64_t e = spawn(B, "echo", "echo");
    char to[96];
    snprintf(to, sizeof to, "tinyactors://b/#_scxml_%llu", (unsigned long long)e);
    go(A, p, to, 1);
    go(A, p, "tinyactors://b/@echo", 2);
    LOGCHK(A, "pong:1;pong:2;");
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, nodes[A].peer[B], &status), TA_OK);
    CHECK_EQUAL(status.state, TA_PEER_OPEN);
    CHECK_EQUAL(status.sent, 2u);
    CHECK_EQUAL(status.received, 2u);
    CHECK_EQUAL(status.errors, 0u);
    CHECK(status.id.length == 1 && status.id.pointer[0] == 'b');
    ta_host_peer_round_trip_time(nodes[A].host, nodes[A].peer[B], 12);
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, nodes[A].peer[B], &status), TA_OK);
    CHECK_EQUAL(status.round_trip_time, 12u);
    close_all();
}

/* errors come back as error.communication: unknown session, unknown name,
 * unreachable system — no silent drops */
static void test_errors_back(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    uint64_t p = spawn(A, "pinger", "p");
    spawn(B, "echo", "echo");
    go(A, p, "tinyactors://b/#_scxml_999", 1);  /* unknown target at b */
    go(A, p, "tinyactors://b/@nobody", 2);      /* unknown name at b */
    go(A, p, "tinyactors://nowhere/@echo", 3);  /* no route */
    go(A, p, "tinyactors://a/#_scxml_4242", 4); /* ourselves, no such session */
    LOGCHK(A, "comm:p1;comm:p1;comm:p1;comm:p1;");
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[B].host, nodes[B].peer[A], &status), TA_OK);
    CHECK_EQUAL(status.errors, 2u); /* b sent two errors back */
    close_all();
}

/* a peer may only send envelopes from systems it announced */
static void test_origin_validation(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0); /* b knows a (and nothing it reaches) */
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    uint64_t e = spawn(B, "echo", "echo");
    (void)e;
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@echo", 20};
    envelope.name = (ta_string_view){"ping", 4};
    envelope.origin =
        (ta_string_view){"tinyactors://c/#_scxml_1", 24}; /* forged: a speaks for "a" only */
    envelope.msgid = (ta_string_view){"m1", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    /* once a announces c, it may */
    const ta_string_view reach[1] = {{"c", 1}};
    CHECK_EQUAL(ta_host_peer_reach(nodes[B].host, nodes[B].peer[A], (ta_string_list){reach, 1}),
                TA_OK);
    envelope.msgid = (ta_string_view){"m2", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    /* a peer can never speak for us */
    const ta_string_view me[1] = {{"b", 1}};
    CHECK_EQUAL(ta_host_peer_reach(nodes[B].host, nodes[B].peer[A], (ta_string_list){me, 1}),
                TA_OK);
    envelope.origin = (ta_string_view){"tinyactors://b/#_scxml_1", 24};
    envelope.msgid = (ta_string_view){"m3", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    /* malformed */
    envelope.target = (ta_string_view){"nonsense", 8};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    close_all();
}

/* untrusted peers: default deny; exported names, event filters */
static void test_policy(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    connect(B, A, 0, NULL, 0); /* b does not trust a */
    uint64_t p = spawn(A, "pinger", "p");
    spawn(B, "echo", "echo");
    spawn(B, "echo", "secret");
    go(A, p, "tinyactors://b/@echo", 1);
    LOGCHK(A, "comm:p1;"); /* denied: nothing exported */
    CHECK_EQUAL(ta_host_export(nodes[B].host,
                               &(ta_export_rule){nodes[B].peer[A], 0, (ta_string_view){"echo", 4},
                                                 (ta_string_view){"ping", 4}}),
                TA_OK);
    nodes[A].log[0] = 0;
    go(A, p, "tinyactors://b/@echo", 2);
    go(A, p, "tinyactors://b/@secret", 3); /* not exported */
    LOGCHK(A, "pong:2;comm:p1;");
    /* event filter: "ping" only — the pinger's sends are "ping", so try another name directly */
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@echo", 20};
    envelope.name = (ta_string_view){"other", 5};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_1", 24};
    envelope.msgid = (ta_string_view){"x1", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    /* "*" exports everything; untrusted peers still cannot relay through b */
    CHECK_EQUAL(ta_host_export(nodes[B].host, &(ta_export_rule){0, 0, (ta_string_view){"*", 1},
                                                                (ta_string_view){"", 0}}),
                TA_OK);
    envelope.msgid = (ta_string_view){"x2", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    envelope.target = (ta_string_view){"tinyactors://c/@echo", 20};
    envelope.msgid = (ta_string_view){"x3", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    close_all();
}

/* the same message id twice: delivered once */
static void test_dedup(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    uint64_t e = spawn(B, "echo", "echo");
    settle();
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@echo", 20};
    envelope.name = (ta_string_view){"other", 5};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_1", 24};
    envelope.msgid = (ta_string_view){"tinyactors://a/#_scxml_1/7", 26};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DUPLICATE);
    /* a dedup key overrides the message id: same key, new msgid -> duplicate */
    envelope.deduplication_key = (ta_string_view){"order-17", 8};
    envelope.msgid = (ta_string_view){"m-a", 3};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    envelope.msgid = (ta_string_view){"m-b", 3};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DUPLICATE);
    settle();
    LOGCHK(B, "other;other;");
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[B].host, nodes[B].peer[A], &status), TA_OK);
    CHECK_EQUAL(status.duplicates, 2u);
    /* the window survives a snapshot of the host (restored as after a restart) */
    size_t length = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot(nodes[B].host, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    void *snap = aligned_alloc(8, (length + 7) & ~(size_t)7);
    written = (ta_buffer){snap, length, 0};
    CHECK_EQUAL(ta_host_snapshot(nodes[B].host, &written), TA_OK);
    length = written.length;
    size_t dl = 0;
    written = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(nodes[B].ecmascript, &written),
                TA_ERROR_NO_MEMORY);
    dl = written.length;
    void *dsnap = malloc(dl);
    written = (ta_buffer){dsnap, dl, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(nodes[B].ecmascript, &written), TA_OK);
    dl = written.length;
    ta_host_free(nodes[B].host);
    ta_datamodel_ecmascript_free(nodes[B].ecmascript);
    nodes[B].ecmascript = ta_datamodel_ecmascript_new(NULL);
    nodes[B].binding = ta_datamodel_ecmascript_binding(nodes[B].ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"b", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &nodes[B].binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    const ta_image *images[1] = {&nodes[B].image};
    CHECK_EQUAL(test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {snap, length}, images, 1, 0}, &nodes[B].host),
                TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(nodes[B].ecmascript, (ta_bytes){dsnap, dl}),
                TA_OK); /* after the charts attached */
    free(snap);
    free(dsnap);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0); /* the transport reconnects */
    envelope.msgid = (ta_string_view){"tinyactors://a/#_scxml_1/7", 26};
    envelope.deduplication_key = (ta_string_view){NULL, 0};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DUPLICATE);
    /* still remembered */
    envelope.msgid = (ta_string_view){"tinyactors://a/#_scxml_1/8", 26};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    (void)e;
    close_all();
}

/* a -> b -> c (b relays for a trusted a); hop limit; errors travel back */
static void test_relay(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    const char *b_reach[1] = {"c"};
    const char *b_back[1] = {"a"};
    connect(A, B, TA_PEER_TRUSTED, b_reach, 1); /* a reaches c through b */
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    connect(B, C, TA_PEER_TRUSTED, NULL, 0);
    connect(C, B, TA_PEER_TRUSTED, b_back, 1); /* c reaches a through b */
    uint64_t p = spawn(A, "pinger", "p");
    spawn(C, "echo", "echo");
    go(A, p, "tinyactors://c/@echo", 1);
    LOGCHK(A, "pong:1;"); /* there and back through b */
    nodes[A].log[0] = 0;
    go(A, p, "tinyactors://c/@nobody", 2); /* c's error relayed back through b */
    LOGCHK(A, "comm:p1;");
    /* hop limit: an envelope that has been around too often */
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.hops = TA_ROUTE_MAX_HOPS + 1;
    envelope.target = (ta_string_view){"tinyactors://c/@echo", 20};
    envelope.name = (ta_string_view){"ping", 4};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_1", 24};
    envelope.msgid = (ta_string_view){"h1", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    close_all();
}

/* a relay keeps the dedup key and the origintype: the PEER effect carries
 * them, and a duplicate by key is caught at the far end (struct or CBOR) */
static void relay_keeps_envelope(int bytes) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    over_bytes = bytes;
    const char *b_reach[1] = {"c"};
    const char *b_back[1] = {"a"};
    connect(A, B, TA_PEER_TRUSTED, b_reach, 1);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    connect(B, C, TA_PEER_TRUSTED, NULL, 0);
    connect(C, B, TA_PEER_TRUSTED, b_back, 1);
    spawn(C, "echo", "echo");
    settle();
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://c/@echo", 20};
    envelope.name = (ta_string_view){"other", 5};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_1", 24};
    envelope.origintype = (ta_string_view){"urn:x-test", 10};
    envelope.msgid = (ta_string_view){"r1", 2};
    envelope.deduplication_key = (ta_string_view){"order-9", 7};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_RELAYED);
    settle();
    envelope.msgid = (ta_string_view){"r2", 2}; /* a new msgid, the same key */
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_RELAYED);
    settle();
    LOGCHK(C, "other;");
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[C].host, nodes[C].peer[B], &status), TA_OK);
    CHECK_EQUAL(status.duplicates, 1u);
    /* the effect and its accessors (taken here, so not forwarded) */
    envelope.msgid = (ta_string_view){"r3", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_RELAYED);
    {
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(nodes[B].host, &effects);
        CHECK_EQUAL(n, 1u);
        CHECK_EQUAL(effects[0].kind, (uint32_t)TA_HOST_EFFECT_PEER);
        ta_string_view origintype =
            ta_host_effect_text(nodes[B].host, 0, TA_EFFECT_FIELD_ORIGINTYPE);
        ta_string_view key =
            ta_host_effect_text(nodes[B].host, 0, TA_EFFECT_FIELD_DEDUPLICATION_KEY);
        CHECK(origintype.length == 10 && !memcmp(origintype.pointer, "urn:x-test", 10));
        CHECK(key.length == 7 && !memcmp(key.pointer, "order-9", 7));
        CHECK_EQUAL(effects[0].flags, 1u); /* hops */
    }
    close_all();
}

static void test_relay_keeps_envelope(void) {
    relay_keeps_envelope(0);
    relay_keeps_envelope(1);
}

/* a peer that is down: queue (in order, up to max_queue, until the TTL),
 * delivered on reconnect; expired -> error.communication */
static void test_route_status_queue(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(A, B, TA_PEER_TRUSTED | TA_PEER_QUEUE, NULL, 0);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    uint64_t p = spawn(A, "pinger", "p");
    spawn(B, "echo", "echo");
    settle();
    CHECK_EQUAL(ta_host_peer_up(nodes[A].host, nodes[A].peer[B], 0), TA_OK);
    link_up[A][B] = 0;
    go(A, p, "tinyactors://b/@echo", 1);
    go(A, p, "tinyactors://b/@echo", 2);
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, nodes[A].peer[B], &status), TA_OK);
    CHECK_EQUAL(status.state, TA_PEER_RECONNECTING);
    CHECK_EQUAL(status.queued, 2u);
    ta_host_advance_time(nodes[A].host, (ta_host_time){300, 0});
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, nodes[A].peer[B], &status), TA_OK);
    CHECK_EQUAL(status.lag, 300u);
    link_up[A][B] = 1;
    CHECK_EQUAL(ta_host_peer_up(nodes[A].host, nodes[A].peer[B], 1), TA_OK);
    settle();
    LOGCHK(A, "pong:1;pong:2;"); /* in order, after reconnecting */
    /* down again; past the TTL: error.communication */
    nodes[A].log[0] = 0;
    CHECK_EQUAL(ta_host_peer_up(nodes[A].host, nodes[A].peer[B], 0), TA_OK);
    link_up[A][B] = 0;
    go(A, p, "tinyactors://b/@echo", 3);
    {
        ta_run result =
            ta_host_run(nodes[A].host, 1000); /* the expiry is the host's next deadline */
        CHECK_EQUAL(result.next_deadline, (ta_time)(300 + 1000 + 1));
        ta_host_effects_ack(nodes[A].host);
    }
    ta_host_advance_time(nodes[A].host, (ta_host_time){300 + 1001, 0});
    settle();
    LOGCHK(A, "comm:p1;");
    /* a full queue fails at once */
    nodes[A].log[0] = 0;
    for (int k = 0; k < 6; k++)
        go(A, p, "tinyactors://b/@echo", 10 + k);
    LOGCHK(A, "comm:p1;comm:p1;"); /* 4 queued, 2 refused */
    /* removing the peer fails what was queued */
    nodes[A].log[0] = 0;
    CHECK_EQUAL(ta_host_peer_remove(nodes[A].host, nodes[A].peer[B]), TA_OK);
    settle();
    LOGCHK(A, "comm:p1;comm:p1;comm:p1;comm:p1;");
    CHECK(dropped == 0);
    close_all();
}

/* configured routes and replacement of a peer by a newer connection */
static void test_relay_routes(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    connect(B, C, TA_PEER_TRUSTED, NULL, 0);
    const char *back[1] = {"a"};
    connect(C, B, TA_PEER_TRUSTED, back, 1);
    CHECK_EQUAL(ta_host_route(nodes[A].host, (ta_string_view){"c", 1}, nodes[A].peer[B]),
                TA_OK); /* configured */
    CHECK_EQUAL(ta_host_route(nodes[A].host, (ta_string_view){"a", 1}, nodes[A].peer[B]),
                TA_ERROR_INVALID); /* us */
    uint64_t p = spawn(A, "pinger", "p");
    spawn(C, "echo", "echo");
    go(A, p, "tinyactors://c/@echo", 1);
    LOGCHK(A, "pong:1;");
    /* a newer connection with the same id replaces the old peer */
    uint32_t old = nodes[A].peer[B];
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    CHECK(nodes[A].peer[B] != 0);
    ta_peer_status status;
    status.size = sizeof status;
    CHECK(old == nodes[A].peer[B] ||
          ta_host_peer_status(nodes[A].host, old, &status) == TA_ERROR_STALE);
    /* the configured route went with the old peer */
    nodes[A].log[0] = 0;
    go(A, p, "tinyactors://c/@echo", 2);
    LOGCHK(A, "comm:p1;");
    close_all();
}

/* handles carry a generation: after removing a peer, its handle is refused
 * (TA_ERROR_STALE) even when a new peer takes the same slot */
static void test_stale_handle(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    uint32_t old = nodes[A].peer[B];
    CHECK_EQUAL(ta_host_peer_remove(nodes[A].host, old), TA_OK);
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, old, &status), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_peer_up(nodes[A].host, old, 1), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_peer_remove(nodes[A].host, old), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_route(nodes[A].host, (ta_string_view){"b", 1}, old), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_export(nodes[A].host, &(ta_export_rule){old, 0, (ta_string_view){"echo", 4},
                                                                (ta_string_view){"", 0}}),
                TA_ERROR_STALE);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@echo", 20};
    envelope.name = (ta_string_view){"ping", 4};
    envelope.origin = (ta_string_view){"tinyactors://a/@host", 20};
    CHECK_EQUAL(ta_host_deliver(nodes[A].host, old, &envelope), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_peer_error(nodes[A].host, &(ta_peer_error){old, TA_ROUTE_ERROR_UNREACHABLE,
                                                                   envelope.origin, envelope.msgid,
                                                                   (ta_string_view){"", 0}}),
                TA_ERROR_STALE);
    ta_host_peer_round_trip_time(nodes[A].host, old,
                                 5); /* a stale handle is a silent no-op, like before */
    /* a new peer into the same slot: a different handle, the old one still dead */
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    CHECK(nodes[A].peer[B] != old); /* the generation differs */
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, old, &status), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_peer_up(nodes[A].host, old, 1), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_peer_status(nodes[A].host, nodes[A].peer[B], &status), TA_OK);
    CHECK_EQUAL(status.state, TA_PEER_OPEN);
    /* the new peer actually works */
    uint64_t p = spawn(A, "pinger", "p");
    spawn(B, "echo", "echo");
    go(A, p, "tinyactors://b/@echo", 7);
    LOGCHK(A, "pong:7;");
    close_all();
}

/* an envelope from host code without a message id gets one (unique per
 * origin), so receivers can deduplicate it like any other */
static void test_host_code_msgid(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@echo", 20};
    envelope.name = (ta_string_view){"other", 5};
    envelope.origin = (ta_string_view){"tinyactors://a/@host", 20};
    char seen[2][64] = {"", ""};
    for (int k = 0; k < 2; k++) {
        CHECK_EQUAL(ta_host_deliver(nodes[A].host, 0, &envelope), TA_ROUTE_RELAYED);
        const ta_host_effect *effect;
        uint32_t n = ta_host_effects(nodes[A].host, &effect);
        for (uint32_t i = 0; i < n; i++)
            if (effect[i].kind == TA_HOST_EFFECT_PEER)
                snprintf(seen[k], sizeof seen[k], "%.*s", (int)effect[i].msgid.length,
                         effect[i].msgid.pointer);
        ta_host_effects_ack(nodes[A].host);
    }
    CHECK(strncmp(seen[0], "tinyactors://a/@host/", 21) == 0);
    CHECK(strncmp(seen[1], "tinyactors://a/@host/", 21) == 0);
    CHECK(strcmp(seen[0], seen[1]) != 0);
    close_all();
}

/* A database has no resident segments. Its lookup and unique-index results
 * drive the same router, and refused input must never reserve a dedup key. */
static void test_external_storage(void) {
    test_host_config config = {0};
    config.size = sizeof config;
    config.system_id = (ta_string_view){"db", 2};
    config.allocator = ta_runtime_malloc();
    ta_host *host;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    ta_peer_config peer_config = {0};
    peer_config.size = sizeof peer_config;
    peer_config.id = (ta_string_view){"web", 3};
    uint32_t peer;
    CHECK_EQUAL(ta_host_peer_add(host, &peer_config, &peer), TA_OK);
    ta_envelope envelope = {0};
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://db/@ledger", 23};
    envelope.name = (ta_string_view){"pay", 3};
    envelope.origin = (ta_string_view){"tinyactors://web/#_scxml_7", 26};
    envelope.msgid = (ta_string_view){"m1", 2};
    envelope.deduplication_key = (ta_string_view){"order-17", 8};
    ta_route_request request;
    request.size = sizeof request;
    request.from = peer;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_DENIED);
    CHECK_EQUAL(request.key.length, 0u);
    CHECK_EQUAL(ta_host_export(host, &(ta_export_rule){peer, 0, (ta_string_view){"ledger", 6},
                                                       (ta_string_view){"pay", 3}}),
                TA_OK);
    request.from = peer;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_LOOKUP);
    CHECK_EQUAL(ta_host_route_step(&request, 91), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_DEDUPLICATE);
    CHECK(request.key.length == 8 && !memcmp(request.key.pointer, "order-17", 8));
    CHECK_EQUAL(ta_host_route_step(&request, 0), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_DUPLICATE);
    CHECK_EQUAL(ta_host_route_step(&request, 1), TA_ERROR_STATE);
    envelope.msgid = (ta_string_view){"m2", 2};
    request.from = peer;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(ta_host_route_step(&request, 0), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_UNKNOWN_TARGET);
    envelope.hops = TA_ROUTE_MAX_HOPS;
    request.from = peer;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(ta_host_route_step(&request, 92), TA_OK);
    CHECK_EQUAL(ta_host_route_step(&request, 1), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_LOCAL);
    CHECK_EQUAL(request.session, 92u);
    envelope.hops++;
    request.from = peer;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_HOPS);
    envelope.hops = 0;
    envelope.origin = (ta_string_view){"tinyactors://db/#_scxml_7", 25};
    request.from = peer;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_DENIED);
    envelope.target = (ta_string_view){"tinyactors://missing/@x", 23};
    request.from = 0;
    request.flags = 0;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_UNREACHABLE);
    request.from = 0;
    request.flags = TA_ROUTE_DURABLE;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_PEER);
    CHECK(request.peer_id.length == 7 && !memcmp(request.peer_id.pointer, "missing", 7));
    CHECK_EQUAL(request.hops, 0u);
    /* a store that loads its peers only when a next hop needs them */
    request.from = 0;
    request.flags = TA_ROUTE_DURABLE | TA_ROUTE_ASK_PEERS;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_PEERS);
    request.from = 0;
    request.flags = TA_ROUTE_LOCAL_ONLY | TA_ROUTE_ASK_PEERS;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_UNREACHABLE);
    envelope.target = (ta_string_view){"tinyactors://db/#_scxml_92", 26};
    request.from = 0;
    request.flags = TA_ROUTE_DURABLE | TA_ROUTE_ASK_PEERS;
    CHECK_EQUAL(ta_host_route_begin(host, &envelope, &request), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_LOOKUP);
    CHECK_EQUAL(request.session, 92u);
    envelope.target = (ta_string_view){"tinyactors://missing/@x", 23};
    CHECK_EQUAL(ta_host_error_route(host, envelope.origin, &request), TA_OK);
    CHECK_EQUAL(request.session, 7u);
    CHECK_EQUAL(
        ta_host_error_route(host, (ta_string_view){"tinyactors://db/@ledger", 23}, &request),
        TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_REFUSED);
    ta_host_free(host);
}

static void test_durable_message(void) {
    test_host_config config = {0};
    config.size = sizeof config;
    config.system_id = (ta_string_view){"db", 2};
    config.allocator = ta_runtime_malloc();
    ta_host *host;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    ta_effect effect = {0};
    effect.size = sizeof effect;
    effect.session = 42;
    effect.target = (ta_string_view){"tinyactors://web/@x", 19};
    effect.event = (ta_string_view){"q\"\n", 3};
    effect.sendid = (ta_string_view){"same", 4};
    ta_host_message a, b;
    a.size = sizeof a;
    b.size = sizeof b;
    a.revision = 17;
    a.index = 3;
    CHECK_EQUAL(ta_host_message_init(host, &effect, &a), TA_OK);
    CHECK(a.key.length == 7 && !memcmp(a.key.pointer, "42:17.3", 7));
    CHECK(a.envelope.msgid.length == 31 &&
          !memcmp(a.envelope.msgid.pointer, "tinyactors://db/#_scxml_42/17.3", 31));
    b.revision = 17;
    b.index = 4;
    CHECK_EQUAL(ta_host_message_init(host, &effect, &b), TA_OK);
    CHECK(memcmp(a.envelope.msgid.pointer, b.envelope.msgid.pointer, a.envelope.msgid.length));
    ta_host_free(host);
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    b.revision = 17;
    b.index = 3;
    CHECK_EQUAL(ta_host_message_init(host, &effect, &b), TA_OK);
    CHECK(!memcmp(a.envelope.msgid.pointer, b.envelope.msgid.pointer, a.envelope.msgid.length));
#define VIEW_IS(view, text) ((view).length == strlen(text) && !memcmp((view).pointer, text, strlen(text)))
    CHECK(VIEW_IS(a.envelope.target, "tinyactors://web/@x"));
    CHECK(VIEW_IS(a.envelope.name, "q\"\n"));
    CHECK(VIEW_IS(a.envelope.origin, "tinyactors://db/#_scxml_42"));
    CHECK(VIEW_IS(a.envelope.origintype, "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"));
    CHECK(VIEW_IS(a.envelope.sendid, "same"));
    CHECK_EQUAL(a.envelope.hops, 0u);
#undef VIEW_IS
    ta_event_in error_event;
    ta_host_error_event(effect.sendid, &error_event);
    CHECK_EQUAL(error_event.flags, TA_EVENT_IN_ERROR_COMMUNICATION);
    CHECK_EQUAL(error_event.sendid.length, 4u);
    CHECK(!memcmp(error_event.name.pointer, "error.communication", error_event.name.length));
    CHECK(!memcmp(ta_host_route_reason(TA_ROUTE_ERROR_HOPS).pointer, "hops", 4));
    ta_host_free(host);
}

/* every field of an envelope reaches the receiving session as SCXML sees it
 * (_event.name, data, sendid, origin, origintype) */
static void test_envelope_fields(void) {
    open_node(A, PINGER);
    open_node(B, SC("fields", "initial='s'><state id='s'><transition event='look'><log expr=\""
                              "[_event.name, _event.data.x, _event.sendid, _event.origin, "
                              "_event.origintype].join('|')\"/></transition></state>"));
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    spawn(B, "fields", "f");
    settle();
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@f", 17};
    envelope.name = (ta_string_view){"look", 4};
    envelope.data = ta_datamodel_ecmascript_payload_eval(nodes[B].ecmascript, 0,
                                                              (ta_string_view){"{\"x\": 42}", 9});
    envelope.sendid = (ta_string_view){"s-9", 3};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_7", 24};
    envelope.origintype = (ta_string_view){"urn:x-test", 10};
    envelope.msgid = (ta_string_view){"e1", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    settle();
    LOGCHK(B, "look|42|s-9|tinyactors://a/#_scxml_7|urn:x-test;");
    close_all();
}

/* the router speaks the whole address grammar (one parser, ta_address):
 * percent-encoded names, the largest session id, malformed ids refused */
static void test_address_grammar(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    uint64_t p = spawn(A, "pinger", "p");
    spawn(B, "echo", "echo");
    go(A, p, "tinyactors://b/@ech%6F", 1);                      /* "echo", percent-encoded */
    go(A, p, "tinyactors://b/#_scxml_18446744073709551615", 2); /* valid, nobody there */
    LOGCHK(A, "pong:1;comm:p1;");
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.name = (ta_string_view){"other", 5};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_18446744073709551615", 43};
    envelope.msgid = (ta_string_view){"g1", 2};
    envelope.target = (ta_string_view){"tinyactors://b/@%65cho", 22};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_DELIVERED);
    const char *bad[] = {"tinyactors://b/#_scxml_01", "tinyactors://b/#_scxml_18446744073709551616",
                         "tinyactors://b/@ech%6", "tinyactors://B/@echo"};
    for (int k = 0; k < 4; k++) {
        envelope.target = (ta_string_view){bad[k], (uint32_t)strlen(bad[k])};
        envelope.msgid = (ta_string_view){bad[k], (uint32_t)strlen(bad[k])};
        CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    }
    settle();
    LOGCHK(B, "other;");
    close_all();
}

/* system ids are explicit configuration: required, validated, and the
 * prefix of every origin the host puts on the wire */
static void test_system_id(void) {
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    const char *bad[] = {"", "Orders", "a_b", "-a"};
    for (int k = 0; k < 4; k++) {
        ta_host *host = NULL;
        config.system_id = (ta_string_view){bad[k], (uint32_t)strlen(bad[k])};
        CHECK(test_host_new(&config, &host) != TA_OK);
        CHECK(host == NULL);
    }
    ta_datamodel_ecmascript_free(ecmascript);
    open_node(A, PINGER);
    open_node(B, ECHO);
    connect(A, B, TA_PEER_TRUSTED, NULL, 0);
    uint64_t p = spawn(A, "pinger", "p");
    char json[64];
    snprintf(json, sizeof json, "{\"to\": \"tinyactors://b/@echo\", \"n\": 1}");
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"go", 2};
    event.data = ta_datamodel_ecmascript_payload_eval(
        nodes[A].ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = p;
    CHECK_EQUAL(ta_host_send(nodes[A].host, &event), TA_OK);
    ta_host_run(nodes[A].host, 100000);
    const ta_host_effect *effect;
    uint32_t n = ta_host_effects(nodes[A].host, &effect);
    char want[64], origin[64] = "";
    snprintf(want, sizeof want, "tinyactors://a/#_scxml_%llu", (unsigned long long)p);
    for (uint32_t k = 0; k < n; k++)
        if (effect[k].kind == TA_HOST_EFFECT_PEER)
            snprintf(origin, sizeof origin, "%.*s", (int)effect[k].auxiliary.length,
                     effect[k].auxiliary.pointer);
    ta_host_effects_ack(nodes[A].host);
    CHECK(strcmp(origin, want) == 0);
    if (strcmp(origin, want)) fprintf(stderr, "  origin \"%s\", want \"%s\"\n", origin, want);
    close_all();
}

/* ---- traces across systems (docs/adr/0006 in the repository root, section 7) ------------- */

typedef struct begin_seen {
    uint64_t session, macrostep, fuel;
    ta_trace_id trace;
    ta_span_id parent;
    char name[32];
} begin_seen;

/* The macrostep beginnings node i's segments wrote, read and acknowledged. */
static uint32_t begins(int i, begin_seen *out, uint32_t capacity) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < ta_host_segment_count(nodes[i].host); k++) {
        ta_system *system = ta_host_system(nodes[i].host, k);
        if (!system) continue;
        ta_host_segment_info info;
        info.size = sizeof info;
        ta_host_segment_info_get(nodes[i].host, k, &info); /* its strings are in its image */
        ta_trace_view view;
        while (ta_system_trace(system, &view)) {
            uint32_t cursor = 0;
            const ta_trace_record *r;
            while ((r = ta_trace_next(&view, &cursor)))
                if (r->kind == TA_TRACE_MACROSTEP_BEGIN && n < capacity) {
                    const ta_trace_macrostep_begin *b = ta_trace_body(r);
                    ta_string_view name = b->name == TA_NIL ? (ta_string_view){"", 0}
                                          : b->name & TA_TRACE_STRING_DYNAMIC
                                              ? ta_trace_dynamic_string(&view, b->name)
                                              : ta_image_string(info.image, b->name);
                    out[n] = (begin_seen){r->key.session, r->key.macrostep, r->fuel,
                                          r->trace,       r->parent,        ""};
                    snprintf(out[n].name, sizeof out[n].name, "%.*s", (int)name.length,
                             name.pointer);
                    n++;
                }
            ta_system_trace_ack(system, view.length);
        }
    }
    return n;
}

static const begin_seen *begin_named(const begin_seen *b, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(b[i].name, name)) return &b[i];
    return NULL;
}

/* One input with a traceparent is one trace from a through b (a relay, its
 * link to c down: the envelope waits in a queue) to c and back. */
static void test_trace_across_systems(void) {
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    const char *b_reach[1] = {"c"};
    const char *b_back[1] = {"a"};
    connect(A, B, TA_PEER_TRUSTED, b_reach, 1);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    connect(B, C, TA_PEER_TRUSTED | TA_PEER_QUEUE, NULL, 0);
    connect(C, B, TA_PEER_TRUSTED, b_back, 1);
    uint64_t p = spawn(A, "pinger", "p");
    uint64_t echo = spawn(C, "echo", "echo");
    settle();
    begin_seen seen[64];
    begins(A, seen, 64);
    begins(C, seen, 64);
    CHECK_EQUAL(ta_host_peer_up(nodes[B].host, nodes[B].peer[C], 0), TA_OK);
    link_up[B][C] = 0;
    const char *parent = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    const char *json = "{\"to\": \"tinyactors://c/@echo\", \"n\": 7}";
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"go", 2};
    event.traceparent = (ta_string_view){parent, (uint32_t)strlen(parent)};
    event.data = ta_datamodel_ecmascript_payload_eval(
        nodes[A].ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = p;
    CHECK_EQUAL(ta_host_send(nodes[A].host, &event), TA_OK);
    settle();
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[B].host, nodes[B].peer[C], &status), TA_OK);
    CHECK_EQUAL(status.queued, 1u); /* waiting in b's queue */
    link_up[B][C] = 1;
    CHECK_EQUAL(ta_host_peer_up(nodes[B].host, nodes[B].peer[C], 1), TA_OK);
    settle();
    LOGCHK(A, "pong:7;");
    begin_seen at_a[16], at_c[16];
    uint32_t na = begins(A, at_a, 16), nc = begins(C, at_c, 16);
    const begin_seen *go_at_a = begin_named(at_a, na, "go"),
                     *ping_at_c = begin_named(at_c, nc, "ping"),
                     *pong_at_a = begin_named(at_a, na, "pong");
    CHECK(go_at_a && ping_at_c && pong_at_a);
    if (go_at_a && ping_at_c && pong_at_a) {
        CHECK_EQUAL(go_at_a->parent, 0xb7ad6b7169203331ull);
        CHECK(ping_at_c->trace.high == 0x0af7651916cd43ddull &&
              ping_at_c->trace.low == 0x8448eb211c80319cull);
        CHECK(pong_at_a->trace.high == 0x0af7651916cd43ddull &&
              pong_at_a->trace.low == 0x8448eb211c80319cull);
        /* each a child of the step that sent it, in the other system */
        CHECK_EQUAL(ping_at_c->parent,
                    ta_step_key_span_id((ta_step_key){p, go_at_a->macrostep, 1, 0}));
        CHECK_EQUAL(pong_at_a->parent,
                    ta_step_key_span_id((ta_step_key){echo, ping_at_c->macrostep, 1, 0}));
    }
    close_all();
}

/* Two segments of one host (two images): a send between them keeps the
 * trace, and both stamp their records on the host's one fuel clock. */
static void test_trace_two_segments(void) {
    open_node(A, PINGER);
    size_t length;
    void *echo_bytes = compile_chart("e.scxml", ECHO, NULL, &length, 0);
    ta_image echo_image;
    CHECK(echo_bytes && ta_image_open((ta_bytes){echo_bytes, length}, &echo_image) == TA_OK);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(nodes[A].host, &echo_image, &segment), TA_OK);
    CHECK_EQUAL(segment, 1);
    uint64_t p = spawn(A, "pinger", "p");
    uint64_t echo = 0;
    CHECK_EQUAL(
        ta_host_spawn(nodes[A].host,
                      &(ta_spawn){1, 0, (ta_string_view){"echo", 4}, 0, (ta_string_view){0, 0}},
                      &echo),
        TA_OK);
    CHECK(echo >> 32 == 1); /* the second segment's sessions */
    settle();
    begin_seen seen[64];
    begins(A, seen, 64);
    const char *parent = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    const char *json = "{\"to\": \"tinyactors://a/@echo\", \"n\": 3}";
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"go", 2};
    event.traceparent = (ta_string_view){parent, (uint32_t)strlen(parent)};
    event.data = ta_datamodel_ecmascript_payload_eval(
        nodes[A].ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = p;
    CHECK_EQUAL(ta_host_send(nodes[A].host, &event), TA_OK);
    settle();
    LOGCHK(A, "pong:3;");
    uint32_t n = begins(A, seen, 64);
    const begin_seen *go_seen = begin_named(seen, n, "go"), *ping = begin_named(seen, n, "ping"),
                     *pong = begin_named(seen, n, "pong");
    CHECK(go_seen && ping && pong);
    if (go_seen && ping && pong) {
        CHECK_EQUAL(ping->session, echo);
        CHECK_EQUAL(ping->trace.low, 0x8448eb211c80319cull);
        CHECK_EQUAL(ping->parent, ta_step_key_span_id((ta_step_key){p, go_seen->macrostep, 1, 0}));
        CHECK_EQUAL(pong->parent, ta_step_key_span_id((ta_step_key){echo, ping->macrostep, 1, 0}));
        CHECK(go_seen->fuel < ping->fuel && ping->fuel < pong->fuel); /* one clock, in order */
    }
    ta_host_stats stats;
    stats.size = sizeof stats;
    host_stats_fill(nodes[A].host, &stats);
    CHECK(pong && stats.fuel_clock >= pong->fuel);
    close_all();
    free(echo_bytes);
}

/* trace.configure reaches every segment, and the ones added later. */
static void test_trace_set(void) {
    open_node(A, PINGER);
    uint64_t p = spawn(A, "pinger", "p");
    settle();
    begin_seen seen[64];
    begins(A, seen, 64);
    static host_ask ask; /* full detail, but no records */
    ta_cbor_writer *configure = ask_begin(&ask, "trace.configure", 2);
    ta_cbor_write_text(configure, ta_string("detail"));
    ta_cbor_write_text(configure, ta_string("full"));
    ta_cbor_write_text(configure, ta_string("records"));
    ta_cbor_write_simple(configure, TA_CBOR_FALSE);
    CHECK_EQUAL(ask_send(nodes[A].host, &ask), TA_OK);
    CHECK(strcmp(ask.text, "{\"id\":1,\"result\":{}}") == 0);
    size_t length;
    void *echo_bytes = compile_chart("e.scxml", ECHO, NULL, &length, 0);
    ta_image echo_image;
    CHECK(echo_bytes && ta_image_open((ta_bytes){echo_bytes, length}, &echo_image) == TA_OK);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(nodes[A].host, &echo_image, &segment), TA_OK);
    uint64_t echo = 0;
    CHECK_EQUAL(ta_host_spawn(
                    nodes[A].host,
                    &(ta_spawn){segment, 0, (ta_string_view){"echo", 4}, 0, (ta_string_view){0, 0}},
                    &echo),
                TA_OK);
    const char *json = "{\"to\": \"tinyactors://a/@echo\", \"n\": 1}";
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"go", 2};
    event.data = ta_datamodel_ecmascript_payload_eval(
        nodes[A].ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = p;
    CHECK_EQUAL(ta_host_send(nodes[A].host, &event), TA_OK);
    settle();
    LOGCHK(A, "pong:1;");
    CHECK_EQUAL(begins(A, seen, 64), 0); /* neither segment wrote one */
    configure = ask_begin(&ask, "trace.configure", 1); /* records again */
    ta_cbor_write_text(configure, ta_string("detail"));
    ta_cbor_write_text(configure, ta_string("full"));
    CHECK_EQUAL(ask_send(nodes[A].host, &ask), TA_OK);
    CHECK(strcmp(ask.text, "{\"id\":1,\"result\":{}}") == 0);
    event.data = ta_datamodel_ecmascript_payload_eval(
        nodes[A].ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = p;
    CHECK_EQUAL(ta_host_send(nodes[A].host, &event), TA_OK);
    settle();
    uint32_t n = begins(A, seen, 64);
    CHECK(begin_named(seen, n, "go") && begin_named(seen, n, "ping"));
    close_all();
    free(echo_bytes);
}

/* across a process boundary: envelopes and errors as CBOR (ta_host_envelope,
 * ta_host_envelope_read, ta_host_peer_error_read) */
static void diagnostic_is(ta_bytes cbor, const char *want) {
    char text[1024];
    ta_buffer written = {text, sizeof text - 1, 0};
    CHECK_EQUAL(ta_cbor_diagnostic(cbor, &written), TA_OK);
    text[written.length] = 0;
    CHECK(strcmp(text, want) == 0);
    if (strcmp(text, want)) fprintf(stderr, "  diagnostic %s\n  want       %s\n", text, want);
}

static void test_envelope_bytes(void) {
    /* the round trip, by session, by name, errors back and relayed */
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_node(C, ECHO);
    over_bytes = 1;
    const char *b_reach[1] = {"c"};
    const char *b_back[1] = {"a"};
    connect(A, B, TA_PEER_TRUSTED, b_reach, 1);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    connect(B, C, TA_PEER_TRUSTED, NULL, 0);
    connect(C, B, TA_PEER_TRUSTED, b_back, 1);
    uint64_t p = spawn(A, "pinger", "p");
    spawn(B, "echo", "echo");
    spawn(C, "echo", "echo");
    go(A, p, "tinyactors://b/@echo", 1);
    go(A, p, "tinyactors://c/@echo", 2);   /* relayed through b: hops */
    go(A, p, "tinyactors://b/@nobody", 3); /* an error envelope back */
    go(A, p, "tinyactors://c/@nobody", 4); /* relayed back through b */
    LOGCHK(A, "pong:1;pong:2;comm:p1;comm:p1;");

    /* the bytes of an envelope and of an error envelope */
    ta_host_effect effect;
    memset(&effect, 0, sizeof effect);
    effect.size = sizeof effect;
    effect.kind = TA_HOST_EFFECT_PEER;
    effect.target = (ta_string_view){"tinyactors://b/@echo", 20};
    effect.event = (ta_string_view){"ping", 4};
    effect.sendid = (ta_string_view){"p1", 2};
    effect.auxiliary = (ta_string_view){"tinyactors://a/#_scxml_1", 24};
    effect.msgid = (ta_string_view){"m1", 2};
    effect.flags = 1;
    effect.payload = ta_datamodel_ecmascript_payload_eval(nodes[A].ecmascript, 0,
                                                          (ta_string_view){"({n: 1})", 8});
    effect.datamodel = 0;
    unsigned char bytes[512];
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_envelope(nodes[A].host, &effect, &written), TA_ERROR_NO_MEMORY);
    size_t needed = written.length;
    written = (ta_buffer){bytes, needed - 1, 0};
    CHECK_EQUAL(ta_host_envelope(nodes[A].host, &effect, &written), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(written.length, needed);
    written = (ta_buffer){bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_envelope(nodes[A].host, &effect, &written), TA_OK);
    CHECK_EQUAL(written.length, needed);
    diagnostic_is((ta_bytes){bytes, written.length},
                  "{\"target\":\"tinyactors://b/@echo\",\"name\":\"ping\",\"data\":24(h'a1616e01'),"
                  "\"sendid\":\"p1\",\"origin\":\"tinyactors://a/#_scxml_1\",\"msgid\":\"m1\","
                  "\"hops\":1}");
    ta_envelope envelope;
    CHECK_EQUAL(ta_host_envelope_read(nodes[B].host, (ta_bytes){bytes, written.length}, &envelope),
                TA_OK);
    CHECK_EQUAL(envelope.hops, 1u);
    CHECK(envelope.msgid.length == 2 && !memcmp(envelope.msgid.pointer, "m1", 2));
    /* the value is not decoded: the target's datamodel reads it */
    CHECK(envelope.data == 0 && envelope.datamodel == TA_NIL);
    CHECK(envelope.wire.length == 4 && !memcmp(envelope.wire.pointer, "\xa1\x61\x6e\x01", 4));
    ta_datamodel_ecmascript_vtable()->release(nodes[A].ecmascript, effect.payload);

    effect.kind = TA_HOST_EFFECT_PEER_ERROR;
    effect.token = TA_ROUTE_ERROR_UNKNOWN_TARGET;
    written = (ta_buffer){bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_envelope(nodes[A].host, &effect, &written), TA_OK);
    diagnostic_is((ta_bytes){bytes, written.length},
                  "{\"error\":\"unknown-target\",\"sendid\":\"p1\","
                  "\"origin\":\"tinyactors://a/#_scxml_1\",\"msgid\":\"m1\"}");
    ta_peer_error error;
    CHECK_EQUAL(ta_host_peer_error_read((ta_bytes){bytes, written.length}, &error), TA_OK);
    CHECK_EQUAL(error.reason, (uint32_t)TA_ROUTE_ERROR_UNKNOWN_TARGET);
    CHECK(error.origin.length == 24 && error.sendid.length == 2 && error.msgid.length == 2);
    /* an error envelope is not an envelope, and the reverse */
    CHECK_EQUAL(ta_host_envelope_read(nodes[B].host, (ta_bytes){bytes, written.length}, &envelope),
                TA_ERROR_INVALID);
    CHECK_EQUAL(envelope.size, 0u);
    effect.kind = TA_EFFECT_LOG;
    CHECK_EQUAL(ta_host_envelope(nodes[A].host, &effect, &written), TA_ERROR_INVALID);

    /* not envelopes: nothing to answer */
    static const unsigned char not_cbor[] = {0xff};
    static const unsigned char not_map[] = {0x01};
    static const unsigned char unknown_key[] = {0xa1, 0x63, 'f', 'o', 'o', 0x01};
    static const unsigned char bad_type[] = {0xa1, 0x64, 'h', 'o', 'p', 's', 0x61, 'x'};
    static const unsigned char bad_reason[] = {0xa2, 0x65, 'e', 'r', 'r', 'o', 'r', 0x61, 'x',
                                               0x66, 'o', 'r', 'i', 'g', 'i', 'n', 0x61, 'o'};
    const ta_bytes bad[] = {{not_cbor, sizeof not_cbor},
                            {not_map, sizeof not_map},
                            {unknown_key, sizeof unknown_key},
                            {bad_type, sizeof bad_type}};
    for (uint32_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        CHECK_EQUAL(ta_host_envelope_read(nodes[B].host, bad[k], &envelope), TA_ERROR_INVALID);
        CHECK_EQUAL(envelope.size, 0u);
        CHECK_EQUAL(ta_host_peer_error_read(bad[k], &error), TA_ERROR_INVALID);
    }
    CHECK_EQUAL(ta_host_peer_error_read((ta_bytes){bad_reason, sizeof bad_reason}, &error),
                TA_ERROR_INVALID);

    /* data that is not a neutral value: refused as malformed, answered */
    static const char bad_data_prefix[] = "\xa5"
                                          "\x66" "target" "\x74" "tinyactors://b/@echo"
                                          "\x64" "name" "\x65" "other"
                                          "\x64" "data" "\xd8\x18\x41\xff"
                                          "\x66" "origin" "\x78\x18" "tinyactors://a/#_scxml_1"
                                          "\x65" "msgid" "\x62" "m9";
    CHECK_EQUAL(ta_host_envelope_read(nodes[B].host,
                                      (ta_bytes){bad_data_prefix, sizeof bad_data_prefix - 1},
                                      &envelope),
                TA_ERROR_INVALID);
    CHECK(envelope.size == sizeof envelope && !envelope.target.length && !envelope.data &&
          !envelope.wire.length);
    ta_run result = ta_host_run(nodes[B].host, 1000);
    (void)result;
    ta_host_effects_ack(nodes[B].host);
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope), TA_ROUTE_REFUSED);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(nodes[B].host, &effects), errors = 0;
    for (uint32_t k = 0; k < n; k++)
        if (effects[k].kind == TA_HOST_EFFECT_PEER_ERROR &&
            effects[k].token == TA_ROUTE_ERROR_MALFORMED)
            errors++;
    CHECK_EQUAL(errors, 1u);
    ta_host_effects_ack(nodes[B].host);
    close_all();
}

/* ---- which datamodel decodes a value that arrived as bytes ---------------------------------- */

static int from_wire_calls[N + 1], to_wire_calls[N + 1]; /* N: the second datamodel of c */
static ta_datamodel_vtable counted[N + 1];

#define COUNTED(k)                                                                                 \
    static ta_status from_wire_##k(void *datamodel, ta_bytes cbor, ta_payload *out) {             \
        from_wire_calls[k]++;                                                                      \
        return ta_datamodel_ecmascript_vtable()->from_wire(datamodel, cbor, out);                  \
    }                                                                                              \
    static ta_status to_wire_##k(void *datamodel, ta_payload p, ta_buffer *out) {                 \
        to_wire_calls[k]++;                                                                        \
        return ta_datamodel_ecmascript_vtable()->to_wire(datamodel, p, out);                       \
    }
COUNTED(0)
COUNTED(1)
COUNTED(2)

/* the null datamodel, reading values (it keeps none: the event has no data) */
static ta_status null_from_wire(void *datamodel, ta_bytes cbor, ta_payload *out) {
    (void)datamodel;
    (void)cbor;
    from_wire_calls[N]++;
    *out = 0;
    return TA_OK;
}

static void count_calls(void) {
    ta_status (*from[N])(void *, ta_bytes, ta_payload *) = {from_wire_0, from_wire_1, from_wire_2};
    ta_status (*to[N])(void *, ta_payload, ta_buffer *) = {to_wire_0, to_wire_1, to_wire_2};
    for (int i = 0; i < N; i++) {
        counted[i] = *ta_datamodel_ecmascript_vtable();
        counted[i].from_wire = from[i];
        counted[i].to_wire = to[i];
        vtable_for[i] = &counted[i];
    }
    counted[N] = *ta_datamodel_null();
    counted[N].size = sizeof counted[N];
    counted[N].from_wire = null_from_wire;
    memset(from_wire_calls, 0, sizeof from_wire_calls);
    memset(to_wire_calls, 0, sizeof to_wire_calls);
}

#define QUIET                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' name='quiet' "              \
    "initial='s'><state id='s'><transition event='ping' target='done'/></state>"                  \
    "<final id='done'/></scxml>"

/* c runs two datamodels: ECMAScript (echo) and null (quiet) */
static void open_two_datamodels(void) {
    node *n = &nodes[C];
    open_node(C, ECHO);
    ta_host_free(n->host);
    ta_datamodel_binding bindings[2] = {n->binding, {&counted[N], NULL}};
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){IDS[C], 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = bindings;
    config.datamodel_count = 2;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(test_host_new(&config, &n->host), TA_OK);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(n->host, &n->image, &segment), TA_OK);
    static ta_image quiet;
    static void *quiet_bytes;
    size_t length;
    free(quiet_bytes);
    quiet_bytes = compile_chart("q.scxml", QUIET, NULL, &length, 0);
    CHECK(quiet_bytes && ta_image_open((ta_bytes){quiet_bytes, length}, &quiet) == TA_OK);
    CHECK_EQUAL(ta_host_add_image(n->host, &quiet, &segment), TA_OK);
}

static void decode_at_target(int bytes) {
    count_calls();
    open_node(A, PINGER);
    open_node(B, ECHO);
    open_two_datamodels();
    over_bytes = bytes;
    const char *b_reach[1] = {"c"};
    const char *b_back[1] = {"a"};
    connect(A, B, TA_PEER_TRUSTED, b_reach, 1);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0);
    connect(B, C, TA_PEER_TRUSTED | TA_PEER_QUEUE, NULL, 0);
    connect(C, B, TA_PEER_TRUSTED, b_back, 1);
    uint64_t p = spawn(A, "pinger", "p");
    spawn(C, "echo", "echo");
    uint64_t quiet = spawn(C, "quiet", "quiet");
    settle();

    /* a ping to c and its pong back, relayed through b both ways */
    go(A, p, "tinyactors://c/@echo", 1);
    LOGCHK(A, "pong:1;");
    CHECK_EQUAL(from_wire_calls[B], 0); /* the relay reads no value */
    CHECK_EQUAL(to_wire_calls[B], 0);
    if (bytes) {
        CHECK_EQUAL(from_wire_calls[C], 1); /* the ping, by echo's ECMAScript */
        CHECK_EQUAL(from_wire_calls[A], 1); /* the pong */
        CHECK_EQUAL(to_wire_calls[A], 2); /* ta_host_envelope: its length, then the value */
        CHECK_EQUAL(to_wire_calls[C], 2);
    } else { /* within a process: payloads only (the test transport converts) */
        CHECK_EQUAL(from_wire_calls[C] + from_wire_calls[A] + to_wire_calls[A] + to_wire_calls[C],
                    0);
    }
    CHECK_EQUAL(from_wire_calls[N], 0);

    /* to the null chart (bytes only: the struct transport above converts with
     * ECMAScript): c's null datamodel reads it, not ECMAScript, and the quiet
     * chart takes the ping and ends */
    if (bytes) {
        int ecmascript_before = from_wire_calls[C];
        go(A, p, "tinyactors://c/@quiet", 2);
        CHECK_EQUAL(from_wire_calls[N], 1);
        CHECK_EQUAL(from_wire_calls[C], ecmascript_before);
        CHECK_EQUAL(from_wire_calls[B], 0);
        ta_host_location location;
        CHECK(ta_host_locate(nodes[C].host, quiet, &location) != TA_OK);
    }
    (void)quiet;

    /* c is down: b queues the relayed value as it arrived, and sends it on */
    CHECK_EQUAL(ta_host_peer_up(nodes[B].host, nodes[B].peer[C], 0), TA_OK);
    nodes[A].log[0] = 0;
    go(A, p, "tinyactors://c/@echo", 3);
    LOGCHK(A, "");
    CHECK_EQUAL(ta_host_peer_up(nodes[B].host, nodes[B].peer[C], 1), TA_OK);
    settle();
    LOGCHK(A, "pong:3;");
    CHECK_EQUAL(from_wire_calls[B], 0);

    if (bytes) {
        /* a value no datamodel reads (tag 99): b forwards it, c refuses it as
         * malformed, and the error goes back to the origin */
        char origin[64];
        int ol = snprintf(origin, sizeof origin, "tinyactors://a/#_scxml_%llu",
                          (unsigned long long)p);
        unsigned char envelope_bytes[256];
        ta_cbor_token tokens[16];
        char strings[256];
        ta_cbor_writer w = {{{0, 0}, tokens, 16, 0}, {strings, sizeof strings, 0}, TA_OK};
        ta_cbor_write_map(&w, 6);
        ta_cbor_write_text(&w, ta_string("target"));
        ta_cbor_write_text(&w, ta_string("tinyactors://c/@echo"));
        ta_cbor_write_text(&w, ta_string("name"));
        ta_cbor_write_text(&w, ta_string("ping"));
        ta_cbor_write_text(&w, ta_string("data"));
        ta_cbor_write_tag(&w, 24);
        ta_cbor_write_bytes(&w, (ta_bytes){"\xd8\x63\x00", 3}); /* 99(0) */
        ta_cbor_write_text(&w, ta_string("sendid"));
        ta_cbor_write_text(&w, ta_string("p9"));
        ta_cbor_write_text(&w, ta_string("origin"));
        ta_cbor_write_text(&w, (ta_string_view){origin, (uint32_t)ol});
        ta_cbor_write_text(&w, ta_string("msgid"));
        ta_cbor_write_text(&w, ta_string("bad-1"));
        ta_buffer out = {envelope_bytes, sizeof envelope_bytes, 0};
        CHECK_EQUAL(ta_cbor_finish(&w, &out), TA_OK);
        ta_envelope envelope;
        CHECK_EQUAL(ta_host_envelope_read(nodes[B].host, (ta_bytes){envelope_bytes, out.length},
                                          &envelope),
                    TA_OK);
        CHECK(envelope.wire.length == 3 && !envelope.data);
        nodes[A].log[0] = 0;
        CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope),
                    TA_ROUTE_RELAYED);
        settle();
        CHECK_EQUAL(from_wire_calls[B], 0); /* forwarded without judging */
        ta_peer_status status;
        status.size = sizeof status;
        CHECK_EQUAL(ta_host_peer_status(nodes[C].host, nodes[C].peer[B], &status), TA_OK);
        CHECK_EQUAL(status.errors, 1u);   /* c answered: malformed */
        LOGCHK(A, "comm:p9;");            /* and the origin heard of it */
        LOGCHK(C, "");                    /* echo never saw it */
    }
    close_all();
}

static void test_decode_at_target(void) {
    decode_at_target(1);
    decode_at_target(0);
}

int main(void) {
    RUN(test_route_status);
    RUN(test_errors_back);
    RUN(test_origin_validation);
    RUN(test_policy);
    RUN(test_dedup);
    RUN(test_relay);
    RUN(test_relay_keeps_envelope);
    RUN(test_route_status_queue);
    RUN(test_relay_routes);
    RUN(test_stale_handle);
    RUN(test_host_code_msgid);
    RUN(test_external_storage);
    RUN(test_durable_message);
    RUN(test_envelope_fields);
    RUN(test_address_grammar);
    RUN(test_system_id);
    RUN(test_trace_across_systems);
    RUN(test_trace_two_segments);
    RUN(test_trace_set);
    RUN(test_envelope_bytes);
    RUN(test_decode_at_target);
    TEST_MAIN_END();
}
