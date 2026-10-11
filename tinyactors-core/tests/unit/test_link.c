/* The link protocol (include/tinyactors/link.h, docs/protocol.md): a server
 * host "a" and client hosts "b" and "c" on links, their transports a test
 * queue of text messages between ta_links outputs and ta_links_received. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/link.h>
#include <tinyactors/runtime.h>

#include <stdlib.h>
#include <string.h>

#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"
/* "go" sends "ping" to _event.data.to; logs replies and errors */
#define PINGER                                                                                     \
    SC("pinger", "initial='s'><state id='s'>"                                                      \
                 "<transition event='go'><send targetexpr='_event.data.to' event='ping' "          \
                 "id='p1'><param name='n' expr='_event.data.n'/></send></transition>"              \
                 "<transition event='pong'><log expr=\"'pong:' + _event.data.n\"/></transition>"   \
                 "<transition event='error.communication'><log "                                   \
                 "expr=\"'comm:' + _event.sendid\"/></transition></state>")
/* replies "pong" to _event.origin */
#define ECHO                                                                                       \
    SC("echo", "initial='s'><state id='s'><transition event='ping'>"                               \
               "<send targetexpr='_event.origin' event='pong'>"                                    \
               "<param name='n' expr='_event.data.n'/></send></transition></state>")
#define VIEW_OF(s) ((ta_string_view){(s), (uint32_t)strlen(s)})

enum { A, B, C, N };
static const char *IDS[N] = {"a", "b", "c"};

typedef struct node {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    ta_links *links;
    void *bytes;
    ta_image image;
    char log[4096];
} node;

static node nodes[N];
static ta_time now;

/* the test's transports: each a pair of ends, the connection handle on each side */
typedef struct pipe {
    int open;
    int client, server; /* node indices */
    uint32_t client_link, server_connection;
} pipe;
static pipe pipes[16];
static uint32_t pipe_count;
static uint32_t server_handle; /* a's server */
static char last_hello[512];   /* the hello a judged last */
static uint32_t judging;       /* its connection */

static void open_node(int i, const char *chart) {
    node *n = &nodes[i];
    memset(n, 0, sizeof *n);
    n->ecmascript = ta_datamodel_ecmascript_new(NULL);
    n->binding = ta_datamodel_ecmascript_binding(n->ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = VIEW_OF(IDS[i]);
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &n->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(ta_host_new(&config, &n->host), TA_OK);
    ta_links_config links;
    memset(&links, 0, sizeof links);
    links.size = sizeof links;
    links.host = n->host;
    links.system_id = config.system_id;
    links.datamodels = &n->binding;
    links.datamodel_count = 1;
    links.allocator = ta_runtime_malloc();
    CHECK_EQUAL(ta_links_new(&links, &n->links), TA_OK);
    size_t length;
    n->bytes = compile_chart("c.scxml", chart, NULL, &length, 0);
    CHECK(n->bytes && ta_image_open(n->bytes, length, &n->image) == TA_OK);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(n->host, &n->image, &segment), TA_OK);
}

static void close_all(void) {
    for (int i = 0; i < N; i++) {
        if (nodes[i].links) ta_links_free(nodes[i].links);
        if (nodes[i].host) ta_host_free(nodes[i].host);
        if (nodes[i].ecmascript) ta_datamodel_ecmascript_free(nodes[i].ecmascript);
        free(nodes[i].bytes);
        memset(&nodes[i], 0, sizeof nodes[i]);
    }
    memset(pipes, 0, sizeof pipes);
    pipe_count = 0;
    now = 0;
}

static pipe *pipe_of(int i, uint32_t handle) {
    for (uint32_t k = 0; k < pipe_count; k++) {
        pipe *p = &pipes[k];
        if (p->open && ((p->client == i && p->client_link == handle) ||
                        (p->server == i && p->server_connection == handle)))
            return p;
    }
    return NULL;
}

static void pipe_close(pipe *p, const char *why) {
    if (!p->open) return;
    p->open = 0;
    ta_links_dropped(nodes[p->client].links, p->client_link, VIEW_OF(why), now);
    ta_links_dropped(nodes[p->server].links, p->server_connection, VIEW_OF(why), now);
}

static int refuse_opens; /* the server is unreachable: a new transport fails at once */

/* what node i's links asked for: frames to the other end, closes, opens */
static int outputs(int i) {
    const ta_link_output *out;
    uint32_t n = ta_links_output(nodes[i].links, &out);
    int busy = n > 0;
    static char copies[16][2048];
    ta_link_output batch[16];
    uint32_t count = n < 16 ? n : 16;
    for (uint32_t k = 0; k < count; k++) { /* the next calls may add output: copy first */
        batch[k] = out[k];
        memcpy(copies[k], out[k].text.pointer, out[k].text.length);
        batch[k].text.pointer = copies[k];
    }
    ta_links_output_ack(nodes[i].links);
    for (uint32_t k = 0; k < count; k++) {
        const ta_link_output *o = &batch[k];
        pipe *p = pipe_of(i, o->connection);
        if (o->kind == TA_LINK_OPEN) {
            if (refuse_opens) {
                ta_links_dropped(nodes[i].links, o->connection, VIEW_OF("refused"), now);
                continue;
            }
            pipe *q = &pipes[pipe_count++];
            q->open = 1;
            q->client = i;
            q->server = A;
            q->client_link = o->connection;
            CHECK_EQUAL(ta_links_accept(nodes[A].links, server_handle, &q->server_connection),
                        TA_OK);
            ta_links_opened(nodes[i].links, o->connection, now);
        } else if (o->kind == TA_LINK_SEND && p) {
            int to = p->client == i ? p->server : p->client;
            uint32_t handle = p->client == i ? p->server_connection : p->client_link;
            ta_links_received(nodes[to].links, handle, o->text, now);
        } else if (o->kind == TA_LINK_CLOSE && p) {
            pipe_close(p, "closed");
        } else if (o->kind == TA_LINK_HELLO) {
            snprintf(last_hello, sizeof last_hello, "%.*s", (int)o->text.length, o->text.pointer);
            judging = o->connection;
        }
    }
    return busy;
}

static int run(int i) {
    node *n = &nodes[i];
    ta_host_advance_time(n->host, now);
    ta_run result = ta_host_run(n->host, 100000);
    const ta_host_effect *effects;
    uint32_t count = ta_host_effects(n->host, &effects);
    for (uint32_t k = 0; k < count; k++) {
        if (effects[k].kind == TA_EFFECT_LOG) {
            size_t l = strlen(n->log);
            snprintf(n->log + l, sizeof n->log - l, "%.*s;", (int)effects[k].event.length,
                     effects[k].event.pointer);
        }
        ta_links_effect(n->links, &effects[k], now);
    }
    ta_host_effects_ack(n->host);
    ta_links_time(n->links, now);
    return result.status != TA_RUN_IDLE || count > 0;
}

static void settle(void) {
    for (int round = 0; round < 200; round++) {
        int busy = 0;
        for (int i = 0; i < N; i++)
            if (nodes[i].host) busy |= run(i) | outputs(i);
        if (!busy) return;
    }
    CHECK(0); /* it never settled */
}

static void at(ta_time t) {
    now = t;
    settle();
}

static uint64_t spawn(int i, const char *chart, const char *name) {
    uint32_t segment, c;
    CHECK_EQUAL(ta_host_chart(nodes[i].host, VIEW_OF(chart), &segment, &c), TA_OK);
    uint64_t s = 0;
    CHECK_EQUAL(
        ta_host_spawn(nodes[i].host, segment, c, VIEW_OF(name), 0, (ta_string_view){0, 0}, &s),
        TA_OK);
    return s;
}

static void go(int i, uint64_t session, const char *to, int n) {
    char json[160];
    snprintf(json, sizeof json, "{\"to\": \"%s\", \"n\": %d}", to, n);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = VIEW_OF("go");
    event.data = ta_datamodel_ecmascript_payload_from_json(nodes[i].ecmascript, VIEW_OF(json));
    CHECK_EQUAL(ta_host_send(nodes[i].host, session, &event), TA_OK);
}

#define LOGCHK(i, want)                                                                            \
    do {                                                                                           \
        CHECK(strcmp(nodes[i].log, want) == 0);                                                    \
        if (strcmp(nodes[i].log, want))                                                            \
            fprintf(stderr, "  log %s: \"%s\", want \"%s\"\n", IDS[i], nodes[i].log, want);        \
        nodes[i].log[0] = 0;                                                                       \
    } while (0)

static void serve(uint32_t flags, ta_time resume) {
    ta_string_view exports[] = {VIEW_OF("echo")}, events[] = {VIEW_OF("ping")};
    ta_link_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.flags = flags;
    options.max_queue = 100;
    options.resume = resume;
    options.exports = exports;
    options.export_events = events;
    options.export_count = 1;
    CHECK_EQUAL(ta_links_serve(nodes[A].links, &options, &server_handle), TA_OK);
}

static uint32_t connect_to_a(int i, const char *alias, uint32_t flags, ta_time ping) {
    ta_link_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.flags = flags;
    options.name = VIEW_OF(alias);
    options.max_queue = 100;
    options.ping_interval = ping;
    uint32_t link = 0;
    CHECK_EQUAL(ta_links_connect(nodes[i].links, &options, now, &link), TA_OK);
    return link;
}

/* a client's alias for the server, exports, ping and round trips, the hello */
static void test_link_round_trip(void) {
    open_node(A, ECHO);
    open_node(B, PINGER);
    spawn(A, "echo", "echo");
    uint64_t p = spawn(B, "pinger", "p");
    serve(0, 0);
    uint32_t link = connect_to_a(B, "server", TA_LINK_TRUSTED, 1000);
    at(10);
    ta_link_status status;
    status.size = sizeof status;
    CHECK_EQUAL(ta_links_status(nodes[B].links, link, &status), TA_OK);
    CHECK(status.open && status.remote.length == 1 && status.remote.pointer[0] == 'a');
    uint32_t connections[4];
    CHECK_EQUAL(ta_links_connections(nodes[A].links, server_handle, connections, 4), 1u);
    go(B, p, "tinyactors://server/@echo", 7); /* the alias, rewritten to "a" on the wire */
    at(20);
    LOGCHK(B, "pong:7;");
    /* the client pinged at once; the pong measured the round trip (0 ms here) */
    ta_peer_status peer;
    peer.size = sizeof peer;
    CHECK_EQUAL(ta_host_peer_status(nodes[B].host, status.peer, &peer), TA_OK);
    CHECK_EQUAL(peer.state, TA_PEER_OPEN);
    CHECK_EQUAL(ta_links_deadline(nodes[B].links), 1010u); /* the next ping */
    /* untrusted on a: only echo may be sent to */
    go(B, p, "tinyactors://server/@other", 1);
    at(30);
    LOGCHK(B, "comm:p1;");
    close_all();
}

/* the client reconnects with a backoff and the server resumes its peer;
 * what was sent meanwhile is queued and delivered in order */
static void test_link_reconnect(void) {
    open_node(A, ECHO);
    open_node(B, PINGER);
    spawn(A, "echo", "echo");
    uint64_t p = spawn(B, "pinger", "p");
    serve(0, 5000);
    uint32_t link = connect_to_a(B, "a", TA_LINK_TRUSTED, 0);
    at(10);
    pipe_close(&pipes[0], "connection closed");
    refuse_opens = 1;
    at(11);
    ta_link_status status;
    status.size = sizeof status;
    ta_links_status(nodes[B].links, link, &status);
    CHECK(!status.open && status.attempts == 1);
    CHECK(status.error.length == 17 && !memcmp(status.error.pointer, "connection closed", 17));
    CHECK_EQUAL(ta_links_deadline(nodes[B].links), 260u); /* 250 ms after the drop */
    go(B, p, "tinyactors://a/@echo", 1);                  /* queued for the peer while it is away */
    at(261);
    ta_links_status(nodes[B].links, link, &status);
    CHECK_EQUAL(status.attempts, 2u);
    CHECK_EQUAL(ta_links_deadline(nodes[B].links), 761u); /* then 500 ms */
    refuse_opens = 0;
    at(761);
    LOGCHK(B, "pong:1;");
    ta_links_status(nodes[B].links, link, &status);
    CHECK(status.open && status.attempts == 0);
    uint32_t connections[4];
    CHECK_EQUAL(ta_links_connections(nodes[A].links, server_handle, connections, 4), 1u);
    /* away longer than the server's resume window: its peer goes */
    ta_links_close(nodes[B].links, link, now);
    at(800);
    CHECK_EQUAL(ta_links_connections(nodes[A].links, server_handle, connections, 4), 1u);
    at(5801);
    CHECK_EQUAL(ta_links_connections(nodes[A].links, server_handle, connections, 4), 0u);
    close_all();
}

/* hellos judged by the server, refused ones closed; a relay announces what
 * it reaches; malformed messages are ignored */
static void test_link_judge_and_announce(void) {
    open_node(A, ECHO);
    open_node(B, PINGER);
    open_node(C, ECHO);
    spawn(C, "echo", "echo");
    uint64_t p = spawn(B, "pinger", "p");
    serve(TA_LINK_JUDGE | TA_LINK_ANNOUNCE, 0);
    connect_to_a(B, "a", TA_LINK_TRUSTED, 0);
    at(1);
    CHECK(strstr(last_hello, "\"id\":\"b\"") != NULL);
    ta_links_admit(nodes[A].links, judging, TA_LINK_ADMIT_TRUSTED, now);
    at(2);
    /* c says hello with a credential; refused, then admitted on its next try */
    ta_link_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.flags = TA_LINK_TRUSTED;
    options.name = VIEW_OF("a");
    options.credential = VIEW_OF("{\"token\":\"x\"}");
    uint32_t link = 0;
    CHECK_EQUAL(ta_links_connect(nodes[C].links, &options, now, &link), TA_OK);
    at(3);
    CHECK(strstr(last_hello, "\"cred\":{\"token\":\"x\"}") != NULL);
    ta_links_admit(nodes[A].links, judging, TA_LINK_REFUSE, now);
    at(300); /* closed: c tries again 250 ms later */
    at(600);
    ta_links_admit(nodes[A].links, judging, TA_LINK_ADMIT_TRUSTED, now);
    at(601);
    /* b reaches c through a, which announced c to b */
    go(B, p, "tinyactors://c/@echo", 5);
    at(610);
    LOGCHK(B, "pong:5;");
    pipe *bp = pipe_of(B, pipes[0].client_link);
    CHECK(bp != NULL);
    ta_links_received(nodes[A].links, bp->server_connection, VIEW_OF("not json"), now);
    ta_links_received(nodes[A].links, bp->server_connection, VIEW_OF("{\"t\":\"ack\",\"msgid\":1}"),
                      now);
    ta_links_received(nodes[A].links, bp->server_connection, VIEW_OF("{\"t\":\"ev\",\"target\":"),
                      now);
    at(620);
    close_all();
}

/* frames above the peer's max_frame are refused where they are; the newer
 * connection of one system takes over its peer */
static void test_link_limits(void) {
    open_node(A, ECHO);
    open_node(B, PINGER);
    spawn(A, "echo", "echo");
    uint64_t p = spawn(B, "pinger", "p");
    ta_string_view exports[] = {VIEW_OF("echo")}, events[] = {VIEW_OF("")};
    ta_link_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.max_frame = 300; /* announced in a's hello */
    options.max_queue = 10;
    options.resume = 1000;
    options.exports = exports;
    options.export_events = events;
    options.export_count = 1;
    CHECK_EQUAL(ta_links_serve(nodes[A].links, &options, &server_handle), TA_OK);
    uint32_t link = connect_to_a(B, "a", TA_LINK_TRUSTED, 0);
    at(1);
    char json[600];
    snprintf(json, sizeof json, "{\"to\": \"tinyactors://a/@echo\", \"n\": \"%0400d\"}", 1);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = VIEW_OF("go");
    event.data = ta_datamodel_ecmascript_payload_from_json(nodes[B].ecmascript, VIEW_OF(json));
    CHECK_EQUAL(ta_host_send(nodes[B].host, p, &event), TA_OK); /* too big for a's frames */
    go(B, p, "tinyactors://a/@echo", 2);
    at(2);
    LOGCHK(B, "comm:p1;pong:2;");
    /* b again over a second transport: the first is closed, the peer stays */
    pipe *first = &pipes[0];
    ta_links_close(nodes[B].links, link, now);
    first->open = 0;
    link = connect_to_a(B, "a", TA_LINK_TRUSTED, 0);
    at(3);
    uint32_t connections[4];
    CHECK_EQUAL(ta_links_connections(nodes[A].links, server_handle, connections, 4), 1u);
    go(B, p, "tinyactors://a/@echo", 3);
    at(4);
    LOGCHK(B, "pong:3;");
    close_all();
}

int main(void) {
    RUN(test_link_limits);
    RUN(test_link_round_trip);
    RUN(test_link_reconnect);
    RUN(test_link_judge_and_announce);
    TEST_MAIN_END();
}
