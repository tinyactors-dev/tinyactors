/* The router of ta_host (docs/protocol.md: Routing, Security, Errors,
 * Delivery, Observability): three hosts "a", "b", "c" wired by a test
 * transport that moves TA_HOST_EFFECT_PEER / TA_HOST_EFFECT_PEER_ERROR effects into
 * the other host's ta_host_deliver / ta_host_peer_error. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

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
static int link_up[N][N]; /* the test transport between i and j */
static int dropped;       /* envelopes the transport lost (link down, no queue) */

static void open_node(int i, const char *chart) {
    node *n = &nodes[i];
    memset(n, 0, sizeof *n);
    n->ecmascript = ta_datamodel_ecmascript_new(NULL);
    n->binding = ta_datamodel_ecmascript_binding(n->ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){IDS[i], 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &n->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(ta_host_new(&config, &n->host), TA_OK);
    size_t length;
    n->bytes = compile_chart("c.scxml", chart, NULL, &length, 0);
    CHECK(n->bytes && ta_image_open(n->bytes, length, &n->image) == TA_OK);
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
    dropped = 0;
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
    peer_config.reach = r;
    peer_config.reach_count = reach_count;
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
            if (effect->kind == TA_HOST_EFFECT_PEER_ERROR) {
                ta_host_peer_error(nodes[j].host, nodes[j].peer[i], effect->auxiliary,
                                   effect->msgid, effect->sendid, effect->token);
                continue;
            }
            /* across: payload as JSON (separate datamodels) */
            char json[1024] = "";
            if (effect->payload)
                ta_datamodel_ecmascript_payload_to_json(nodes[i].ecmascript, effect->payload, json,
                                                        sizeof json);
            ta_envelope envelope;
            memset(&envelope, 0, sizeof envelope);
            envelope.size = sizeof envelope;
            envelope.hops = effect->flags;
            envelope.target = effect->target;
            envelope.name = effect->event;
            envelope.sendid = effect->sendid;
            envelope.origin = effect->auxiliary;
            envelope.msgid = effect->msgid;
            envelope.data =
                json[0] ? ta_datamodel_ecmascript_payload_from_json(
                              nodes[j].ecmascript, (ta_string_view){json, (uint32_t)strlen(json)})
                        : 0;
            uint32_t outcome;
            CHECK_EQUAL(ta_host_deliver(nodes[j].host, nodes[j].peer[i], &envelope, &outcome),
                        TA_OK);
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
    uint32_t segment, c;
    CHECK_EQUAL(ta_host_chart(nodes[i].host, (ta_string_view){chart, (uint32_t)strlen(chart)},
                              &segment, &c),
                TA_OK);
    uint64_t s = 0;
    CHECK_EQUAL(ta_host_spawn(nodes[i].host, segment, c,
                              (ta_string_view){name, (uint32_t)strlen(name)}, 0,
                              (ta_string_view){0, 0}, &s),
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
    event.data = ta_datamodel_ecmascript_payload_from_json(
        nodes[i].ecmascript, (ta_string_view){json, (uint32_t)strlen(json)});
    CHECK_EQUAL(ta_host_send(nodes[i].host, session, &event), TA_OK);
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
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
    /* once a announces c, it may */
    const ta_string_view reach[1] = {{"c", 1}};
    CHECK_EQUAL(ta_host_peer_reach(nodes[B].host, nodes[B].peer[A], reach, 1), TA_OK);
    envelope.msgid = (ta_string_view){"m2", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
    /* a peer can never speak for us */
    const ta_string_view me[1] = {{"b", 1}};
    CHECK_EQUAL(ta_host_peer_reach(nodes[B].host, nodes[B].peer[A], me, 1), TA_OK);
    envelope.origin = (ta_string_view){"tinyactors://b/#_scxml_1", 24};
    envelope.msgid = (ta_string_view){"m3", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
    /* malformed */
    envelope.target = (ta_string_view){"nonsense", 8};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
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
    CHECK_EQUAL(ta_host_export(nodes[B].host, nodes[B].peer[A], (ta_string_view){"echo", 4},
                               (ta_string_view){"ping", 4}),
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
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
    /* "*" exports everything; untrusted peers still cannot relay through b */
    CHECK_EQUAL(ta_host_export(nodes[B].host, 0, (ta_string_view){"*", 1}, (ta_string_view){"", 0}),
                TA_OK);
    envelope.msgid = (ta_string_view){"x2", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
    envelope.target = (ta_string_view){"tinyactors://c/@echo", 20};
    envelope.msgid = (ta_string_view){"x3", 2};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
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
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DUPLICATE);
    /* a dedup key overrides the message id: same key, new msgid -> duplicate */
    envelope.deduplication_key = (ta_string_view){"order-17", 8};
    envelope.msgid = (ta_string_view){"m-a", 3};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
    envelope.msgid = (ta_string_view){"m-b", 3};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DUPLICATE);
    settle();
    LOGCHK(B, "other;other;");
    ta_peer_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_host_peer_status(nodes[B].host, nodes[B].peer[A], &status), TA_OK);
    CHECK_EQUAL(status.duplicates, 2u);
    /* the window survives a snapshot of the host (restored as after a restart) */
    size_t length = 0;
    CHECK_EQUAL(ta_host_snapshot(nodes[B].host, NULL, 0, &length), TA_OK);
    void *snap = aligned_alloc(8, (length + 7) & ~(size_t)7);
    CHECK_EQUAL(ta_host_snapshot(nodes[B].host, snap, length, &length), TA_OK);
    size_t dl = 0;
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(nodes[B].ecmascript, NULL, 0, &dl), TA_OK);
    void *dsnap = malloc(dl);
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(nodes[B].ecmascript, dsnap, dl, &dl), TA_OK);
    ta_host_free(nodes[B].host);
    ta_datamodel_ecmascript_free(nodes[B].ecmascript);
    nodes[B].ecmascript = ta_datamodel_ecmascript_new(NULL);
    nodes[B].binding = ta_datamodel_ecmascript_binding(nodes[B].ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"b", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &nodes[B].binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    const ta_image *images[1] = {&nodes[B].image};
    CHECK_EQUAL(ta_host_restore(&config, images, 1, snap, length, &nodes[B].host), TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(nodes[B].ecmascript, dsnap, dl),
                TA_OK); /* after the charts attached */
    free(snap);
    free(dsnap);
    connect(B, A, TA_PEER_TRUSTED, NULL, 0); /* the transport reconnects */
    envelope.msgid = (ta_string_view){"tinyactors://a/#_scxml_1/7", 26};
    envelope.deduplication_key = (ta_string_view){NULL, 0};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DUPLICATE); /* still remembered */
    envelope.msgid = (ta_string_view){"tinyactors://a/#_scxml_1/8", 26};
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
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
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
    close_all();
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
    ta_host_advance_time(nodes[A].host, 300);
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
    ta_host_advance_time(nodes[A].host, 300 + 1001);
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
    CHECK_EQUAL(
        ta_host_export(nodes[A].host, old, (ta_string_view){"echo", 4}, (ta_string_view){"", 0}),
        TA_ERROR_STALE);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://b/@echo", 20};
    envelope.name = (ta_string_view){"ping", 4};
    envelope.origin = (ta_string_view){"tinyactors://a/@host", 20};
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[A].host, old, &envelope, &outcome), TA_ERROR_STALE);
    CHECK_EQUAL(ta_host_peer_error(nodes[A].host, old, envelope.origin, envelope.msgid,
                                   (ta_string_view){"", 0}, TA_ROUTE_ERROR_UNREACHABLE),
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
        uint32_t outcome = 99;
        CHECK_EQUAL(ta_host_deliver(nodes[A].host, 0, &envelope, &outcome), TA_OK);
        CHECK_EQUAL(outcome, TA_ROUTE_RELAYED);
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
    ta_host_config config = {0};
    config.size = sizeof config;
    config.system_id = (ta_string_view){"db", 2};
    config.allocator = ta_runtime_malloc();
    ta_host *host;
    CHECK_EQUAL(ta_host_new(&config, &host), TA_OK);
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
    CHECK_EQUAL(ta_host_route_begin(host, peer, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_DENIED);
    CHECK_EQUAL(request.key.length, 0u);
    CHECK_EQUAL(
        ta_host_export(host, peer, (ta_string_view){"ledger", 6}, (ta_string_view){"pay", 3}),
        TA_OK);
    CHECK_EQUAL(ta_host_route_begin(host, peer, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_LOOKUP);
    CHECK_EQUAL(ta_host_route_step(&request, 91), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_DEDUPLICATE);
    CHECK(request.key.length == 8 && !memcmp(request.key.pointer, "order-17", 8));
    CHECK_EQUAL(ta_host_route_step(&request, 0), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_DUPLICATE);
    CHECK_EQUAL(ta_host_route_step(&request, 1), TA_ERROR_STATE);
    envelope.msgid = (ta_string_view){"m2", 2};
    CHECK_EQUAL(ta_host_route_begin(host, peer, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(ta_host_route_step(&request, 0), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_UNKNOWN_TARGET);
    envelope.hops = TA_ROUTE_MAX_HOPS;
    CHECK_EQUAL(ta_host_route_begin(host, peer, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(ta_host_route_step(&request, 92), TA_OK);
    CHECK_EQUAL(ta_host_route_step(&request, 1), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_LOCAL);
    CHECK_EQUAL(request.session, 92u);
    envelope.hops++;
    CHECK_EQUAL(ta_host_route_begin(host, peer, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_HOPS);
    envelope.hops = 0;
    envelope.origin = (ta_string_view){"tinyactors://db/#_scxml_7", 25};
    CHECK_EQUAL(ta_host_route_begin(host, peer, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_DENIED);
    envelope.target = (ta_string_view){"tinyactors://missing/@x", 23};
    CHECK_EQUAL(ta_host_route_begin(host, 0, &envelope, 0, &request), TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_UNREACHABLE);
    CHECK_EQUAL(ta_host_route_begin(host, 0, &envelope, TA_ROUTE_DURABLE, &request), TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_PEER);
    CHECK(request.peer_id.length == 7 && !memcmp(request.peer_id.pointer, "missing", 7));
    CHECK_EQUAL(request.hops, 0u);
    /* a store that loads its peers only when a next hop needs them */
    CHECK_EQUAL(
        ta_host_route_begin(host, 0, &envelope, TA_ROUTE_DURABLE | TA_ROUTE_ASK_PEERS, &request),
        TA_OK);
    CHECK_EQUAL(request.action, TA_ROUTE_PEERS);
    CHECK_EQUAL(
        ta_host_route_begin(host, 0, &envelope, TA_ROUTE_LOCAL_ONLY | TA_ROUTE_ASK_PEERS, &request),
        TA_OK);
    CHECK_EQUAL(request.reason, TA_ROUTE_ERROR_UNREACHABLE);
    envelope.target = (ta_string_view){"tinyactors://db/#_scxml_92", 26};
    CHECK_EQUAL(
        ta_host_route_begin(host, 0, &envelope, TA_ROUTE_DURABLE | TA_ROUTE_ASK_PEERS, &request),
        TA_OK);
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
    ta_host_config config = {0};
    config.size = sizeof config;
    config.system_id = (ta_string_view){"db", 2};
    config.allocator = ta_runtime_malloc();
    ta_host *host;
    CHECK_EQUAL(ta_host_new(&config, &host), TA_OK);
    ta_effect effect = {0};
    effect.size = sizeof effect;
    effect.session = 42;
    effect.target = (ta_string_view){"tinyactors://web/@x", 19};
    effect.event = (ta_string_view){"q\"\n", 3};
    effect.sendid = (ta_string_view){"same", 4};
    ta_host_message a, b;
    a.size = sizeof a;
    b.size = sizeof b;
    CHECK_EQUAL(ta_host_message_init(host, &effect, 17, 3, &a), TA_OK);
    CHECK(a.key.length == 7 && !memcmp(a.key.pointer, "42:17.3", 7));
    CHECK(a.envelope.msgid.length == 31 &&
          !memcmp(a.envelope.msgid.pointer, "tinyactors://db/#_scxml_42/17.3", 31));
    CHECK_EQUAL(ta_host_message_init(host, &effect, 17, 4, &b), TA_OK);
    CHECK(memcmp(a.envelope.msgid.pointer, b.envelope.msgid.pointer, a.envelope.msgid.length));
    ta_host_free(host);
    CHECK_EQUAL(ta_host_new(&config, &host), TA_OK);
    CHECK_EQUAL(ta_host_message_init(host, &effect, 17, 3, &b), TA_OK);
    CHECK(!memcmp(a.envelope.msgid.pointer, b.envelope.msgid.pointer, a.envelope.msgid.length));
    char json[1024];
    size_t n = ta_host_envelope_json(&a.envelope, (ta_string_view){"[1,2]", 5}, NULL, 0);
    memset(json, '!', sizeof json);
    CHECK_EQUAL(ta_host_envelope_json(&a.envelope, (ta_string_view){"[1,2]", 5}, json, n), n);
    CHECK(json[0] == '!');
    CHECK_EQUAL(ta_host_envelope_json(&a.envelope, (ta_string_view){"[1,2]", 5}, json, n + 1), n);
    CHECK_EQUAL(strlen(json), n);
    CHECK(!strcmp(json, "{\"target\":\"tinyactors://web/@x\",\"name\":\"q\\\"\\u000a\","
                        "\"origin\":\"tinyactors://db/#_scxml_42\","
                        "\"origintype\":\"http://www.w3.org/TR/scxml/#SCXMLEventProcessor\","
                        "\"sendid\":\"same\",\"msgid\":\"tinyactors://db/#_scxml_42/17.3\","
                        "\"data\":[1,2],\"hops\":0}"));
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
    envelope.data = ta_datamodel_ecmascript_payload_from_json(nodes[B].ecmascript,
                                                              (ta_string_view){"{\"x\": 42}", 9});
    envelope.sendid = (ta_string_view){"s-9", 3};
    envelope.origin = (ta_string_view){"tinyactors://a/#_scxml_7", 24};
    envelope.origintype = (ta_string_view){"urn:x-test", 10};
    envelope.msgid = (ta_string_view){"e1", 2};
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
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
    uint32_t outcome;
    CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
    CHECK_EQUAL(outcome, TA_ROUTE_DELIVERED);
    const char *bad[] = {"tinyactors://b/#_scxml_01", "tinyactors://b/#_scxml_18446744073709551616",
                         "tinyactors://b/@ech%6", "tinyactors://B/@echo"};
    for (int k = 0; k < 4; k++) {
        envelope.target = (ta_string_view){bad[k], (uint32_t)strlen(bad[k])};
        envelope.msgid = (ta_string_view){bad[k], (uint32_t)strlen(bad[k])};
        CHECK_EQUAL(ta_host_deliver(nodes[B].host, nodes[B].peer[A], &envelope, &outcome), TA_OK);
        CHECK_EQUAL(outcome, TA_ROUTE_REFUSED);
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
    ta_host_config config;
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
        CHECK(ta_host_new(&config, &host) != TA_OK);
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
    event.data = ta_datamodel_ecmascript_payload_from_json(
        nodes[A].ecmascript, (ta_string_view){json, (uint32_t)strlen(json)});
    CHECK_EQUAL(ta_host_send(nodes[A].host, p, &event), TA_OK);
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

int main(void) {
    RUN(test_route_status);
    RUN(test_errors_back);
    RUN(test_origin_validation);
    RUN(test_policy);
    RUN(test_dedup);
    RUN(test_relay);
    RUN(test_route_status_queue);
    RUN(test_relay_routes);
    RUN(test_stale_handle);
    RUN(test_host_code_msgid);
    RUN(test_external_storage);
    RUN(test_durable_message);
    RUN(test_envelope_fields);
    RUN(test_address_grammar);
    RUN(test_system_id);
    TEST_MAIN_END();
}
