/* router.c — the router of a host: other systems (docs/protocol.md,
 * Routing, Security, Errors, Delivery, Observability).
 *
 * Peers are other systems as seen through a transport connection; the host
 * owns them as handles (slot and generation: a handle outlives its peer only
 * until the slot is reused — stale ones are TA_ERROR_STALE). Routes map system
 * ids to peers. Outbound
 * SCXML sends to other systems become TA_HOST_EFFECT_PEER effects (or queue while
 * a TA_PEER_QUEUE peer is down, or fail with error.communication). Inbound
 * envelopes are origin-checked, policy-checked, deduplicated, then delivered
 * locally or relayed. Errors travel back as TA_HOST_EFFECT_PEER_ERROR effects, or
 * become error.communication when the origin is local.
 *
 * Hosted (malloc), no I/O, no callbacks. */
#include "host/host_internal.h"
#include <stdio.h>

#define DEDUPLICATION_WINDOW 4096u /* message ids remembered per host (FIFO) */

typedef struct queued_envelope { /* an envelope waiting for a peer */
    char *buffer;                /* target | name | sendid | origin | msgid (one allocation) */
    uint32_t length[5];
    ta_payload data;
    uint32_t datamodel, hops;
    ta_time at;
} queued_envelope;

typedef struct peer {
    int live, up;
    uint32_t generation; /* this slot's generation: bumped on removal, so handles go stale */
    uint32_t flags, state;
    char id[TA_ADDRESS_SYSTEM_MAX + 1];
    uint32_t id_length;
    char **reach; /* announced systems (owned) */
    uint32_t *reach_lengths, reach_count;
    queued_envelope *queue;
    uint32_t queue_head, queue_count, queue_capacity, max_queue;
    ta_time ttl;
    uint32_t round_trip_time;
    uint64_t sent, received, errors, duplicates;
} peer;

typedef struct route { /* configured: system id -> peer */
    char *system;
    uint32_t length, peer;
} route;

typedef struct exported {
    uint32_t peer; /* 0: every peer */
    char *name;    /* "*": everything */
    uint32_t name_length;
    char *events; /* SCXML event descriptors, space separated ("" = all) */
    uint32_t events_length;
} exported;

struct ta_router {
    peer *peers;
    uint32_t peer_count, peer_capacity;
    route *routes;
    uint32_t route_count, route_capacity;
    exported *exports;
    uint32_t export_count, export_capacity;
    /* dedup window: FIFO of hashes of (origin, msgid or dedup) */
    uint64_t seen[DEDUPLICATION_WINDOW];
    uint32_t seen_head, seen_count;
};

/* ---- helpers ------------------------------------------------------------------------ */

/* a peer handle packs its slot (high bits) and the slot's generation (low
 * TA_PEER_GENERATION_BITS): removing a peer bumps the generation, so a handle from a
 * removed peer is refused even when a newer peer takes the slot (AGENTS.md:
 * generation handles). Generation wrap after 4096 reuses of one slot. */
#define TA_PEER_GENERATION_BITS 12u
#define TA_PEER_GENERATION_MASK ((1u << TA_PEER_GENERATION_BITS) - 1u)
#define TA_PEER_MAX_SLOTS       (1u << (32u - TA_PEER_GENERATION_BITS))

static uint32_t peer_handle(uint32_t slot, uint32_t generation) {
    return ((slot + 1) << TA_PEER_GENERATION_BITS) | (generation & TA_PEER_GENERATION_MASK);
}

static struct ta_router *router_of(ta_host *host) {
    if (!host->router) host->router = (struct ta_router *)calloc(1, sizeof *host->router);
    return host->router;
}

static int seq(ta_string_view a, const char *b, uint32_t n) {
    return a.length == n && (!n || memcmp(a.pointer, b, n) == 0);
}

static char *dupn(const char *p, uint32_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, p, n);
    d[n] = 0;
    return d;
}

static peer *peer_of(ta_host *host, uint32_t handle) {
    struct ta_router *router = host->router;
    uint32_t slot = handle >> TA_PEER_GENERATION_BITS;
    if (!router || !slot || slot > router->peer_count) return NULL;
    peer *p = &router->peers[slot - 1];
    if (!p->live || p->generation != (handle & TA_PEER_GENERATION_MASK)) return NULL;
    return p;
}

static int is_me(const ta_host *host, ta_string_view system) {
    return seq(system, host->config.system_id.pointer, host->config.system_id.length);
}

/* the peer a system id routes to: configured routes first, then a peer whose
 * id or announced reach it is; 0: none */
static uint32_t route_for(ta_host *host, ta_string_view system) {
    struct ta_router *router = host->router;
    if (!router) return 0;
    for (uint32_t i = 0; i < router->route_count; i++)
        if (seq(system, router->routes[i].system, router->routes[i].length) &&
            peer_of(host, router->routes[i].peer))
            return router->routes[i].peer;
    for (uint32_t i = 0; i < router->peer_count; i++) {
        peer *p = &router->peers[i];
        if (!p->live) continue;
        if (seq(system, p->id, p->id_length)) return peer_handle(i, p->generation);
    }
    for (uint32_t i = 0; i < router->peer_count; i++) {
        peer *p = &router->peers[i];
        if (!p->live) continue;
        for (uint32_t k = 0; k < p->reach_count; k++)
            if (seq(system, p->reach[k], p->reach_lengths[k])) return peer_handle(i, p->generation);
    }
    return 0;
}

/* may peer `handle` speak for `system`? Its own id, what it announced, and
 * systems configured to route through it (replies come back the way sends go) */
static int speaks_for(const ta_host *host, uint32_t handle, const peer *p, ta_string_view system) {
    if (seq(system, p->id, p->id_length)) return 1;
    for (uint32_t k = 0; k < p->reach_count; k++)
        if (seq(system, p->reach[k], p->reach_lengths[k])) return 1;
    const struct ta_router *router = host->router;
    for (uint32_t i = 0; i < router->route_count; i++)
        if (router->routes[i].peer == handle &&
            seq(system, router->routes[i].system, router->routes[i].length))
            return 1;
    return 0;
}

static uint64_t fnv(uint64_t h, ta_string_view s) {
    for (uint32_t i = 0; i < s.length; i++) {
        h ^= (unsigned char)s.pointer[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* seen before? (remembers it otherwise) */
static int dedup_seen(struct ta_router *router, ta_string_view origin, ta_string_view key) {
    if (!key.length) return 0; /* nothing to compare */
    uint64_t x = fnv(fnv(0xcbf29ce484222325ull, origin), key);
    x |= 1; /* 0 = empty */
    for (uint32_t i = 0; i < router->seen_count; i++)
        if (router->seen[(router->seen_head + i) % DEDUPLICATION_WINDOW] == x) return 1;
    if (router->seen_count < DEDUPLICATION_WINDOW) {
        router->seen[(router->seen_head + router->seen_count++) % DEDUPLICATION_WINDOW] = x;
    } else {
        router->seen[router->seen_head] = x;
        router->seen_head = (router->seen_head + 1) % DEDUPLICATION_WINDOW;
    }
    return 0;
}

/* SCXML event descriptor matching ("a.b" matches "a.b.c"; "*" all) */
static int descriptor_match(const char *descs, uint32_t dl, ta_string_view name) {
    if (!dl) return 1;
    uint32_t i = 0;
    while (i < dl) {
        while (i < dl && descs[i] == ' ')
            i++;
        uint32_t s = i;
        while (i < dl && descs[i] != ' ')
            i++;
        uint32_t n = i - s;
        if (!n) continue;
        const char *d = descs + s;
        if (n >= 2 && d[n - 2] == '.' && d[n - 1] == '*') n -= 2; /* "a.*" = "a" */
        if (n == 1 && d[0] == '*') return 1;
        if (name.length >= n && !memcmp(name.pointer, d, n) &&
            (name.length == n || name.pointer[n] == '.'))
            return 1;
    }
    return 0;
}

/* may `peer` send `name` to local target `local`? */
static int allowed(ta_host *host, uint32_t handle, const peer *p, const ta_address *to,
                   ta_string_view name) {
    if (p->flags & TA_PEER_TRUSTED) return 1;
    struct ta_router *router = host->router;
    char dec[256];
    uint32_t dl =
        to->kind == TA_ADDRESS_NAME ? ta_address_name_decode(to->name, dec, sizeof dec) : 0;
    for (uint32_t i = 0; i < router->export_count; i++) {
        const exported *x = &router->exports[i];
        if (x->peer && x->peer != handle) continue;
        int any = x->name_length == 1 && x->name[0] == '*';
        int hit = any || (to->kind == TA_ADDRESS_NAME && x->name_length == dl &&
                          !memcmp(x->name, dec, dl));
        if (hit && descriptor_match(x->events, x->events_length, name)) return 1;
    }
    return 0;
}

/* ---- effects ------------------------------------------------------------------------ */

static void effect_message_id(ta_host *host, ta_string_view msgid) {
    effect_strings *strings = &host->effect_strings[host->effect_count - 1];
    ta_host_effect *effect = &host->effects[host->effect_count - 1];
    strings->offset[4] = pool_put(host, msgid);
    effect->msgid.length = strings->offset[4] == (size_t)-1 ? 0 : msgid.length;
}

static int emit_peer(ta_host *host, uint32_t handle, const ta_envelope *envelope) {
    ta_host_effect *x = effect_new(host, TA_HOST_EFFECT_PEER, TA_NIL);
    if (!x) return 0;
    x->peer = handle;
    x->flags = envelope->hops;
    set_effect_strings(host, envelope->target, envelope->name, envelope->sendid, envelope->origin);
    effect_message_id(host, envelope->msgid);
    if (envelope->data &&
        envelope->datamodel <
            host->config.datamodel_count) { /* the effect owns the payload until ack */
        x->payload = envelope->data;
        x->datamodel = envelope->datamodel;
    }
    peer *p = peer_of(host, handle);
    if (p) p->sent++;
    return 1;
}

static void emit_error(ta_host *host, uint32_t handle, ta_string_view origin, ta_string_view msgid,
                       ta_string_view sendid, uint32_t reason) {
    ta_host_effect *x = effect_new(host, TA_HOST_EFFECT_PEER_ERROR, TA_NIL);
    if (!x) return;
    x->peer = handle;
    x->token = reason;
    set_effect_strings(host, (ta_string_view){"", 0}, (ta_string_view){"", 0}, sendid, origin);
    effect_message_id(host, msgid);
    peer *p = peer_of(host, handle);
    if (p) p->errors++;
}

/* error.communication for a local origin session (0 if it is not ours) */
static int local_error(ta_host *host, ta_string_view origin, ta_string_view sendid) {
    ta_route_request request;
    request.size = sizeof request;
    if (ta_host_error_route(host, origin, &request) != TA_OK || request.action != TA_ROUTE_LOOKUP)
        return 0;
    uint32_t si, actor;
    if (ta_host_locate(host, request.session, &si, &actor) != TA_OK)
        return 1; /* gone: nobody to tell */
    communication_error(host, si, actor, sendid);
    return 1;
}

/* an error about `e`, which came from `from` (0: from here) */
static void refuse(ta_host *host, uint32_t from, const ta_envelope *envelope, uint32_t reason) {
    if (local_error(host, envelope->origin, envelope->sendid)) return;
    ta_address o;
    if (!from && ta_address_parse(envelope->origin, &o) == TA_OK)
        from = route_for(host, o.system); /* relayed: back the way replies go */
    if (from) emit_error(host, from, envelope->origin, envelope->msgid, envelope->sendid, reason);
}

/* ---- queues ------------------------------------------------------------------------- */

static void q_release(ta_host *host, queued_envelope *q) {
    if (q->data && q->datamodel < host->config.datamodel_count &&
        host->datamodels[q->datamodel].vtable->release)
        host->datamodels[q->datamodel].vtable->release(host->datamodels[q->datamodel].datamodel,
                                                       q->data);
    free(q->buffer);
    memset(q, 0, sizeof *q);
}

static ta_envelope envelope_of(const queued_envelope *q) {
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    const char *p = q->buffer;
    ta_string_view *f[5] = {&envelope.target, &envelope.name, &envelope.sendid, &envelope.origin,
                            &envelope.msgid};
    for (int k = 0; k < 5; k++) {
        *f[k] = (ta_string_view){p, q->length[k]};
        p += q->length[k];
    }
    envelope.data = q->data;
    envelope.datamodel = q->datamodel;
    envelope.hops = q->hops;
    return envelope;
}

/* keep `e` (payload moved in) for a peer that is down; 0 if the queue is full */
static int enqueue(ta_host *host, peer *p, const ta_envelope *envelope) {
    if (p->queue_count >= p->max_queue) return 0;
    if (p->queue_count == p->queue_capacity) { /* grow the ring, keeping order */
        uint32_t nc = p->queue_capacity ? p->queue_capacity * 2 : 16;
        queued_envelope *nq = (queued_envelope *)calloc(nc, sizeof *nq);
        if (!nq) return 0;
        for (uint32_t i = 0; i < p->queue_count; i++)
            nq[i] = p->queue[(p->queue_head + i) % p->queue_capacity];
        free(p->queue);
        p->queue = nq;
        p->queue_capacity = nc;
        p->queue_head = 0;
    }
    ta_string_view v[5] = {envelope->target, envelope->name, envelope->sendid, envelope->origin,
                           envelope->msgid};
    uint32_t total = 0;
    for (int k = 0; k < 5; k++)
        total += v[k].length;
    queued_envelope *q = &p->queue[(p->queue_head + p->queue_count) % p->queue_capacity];
    q->buffer = (char *)malloc(total ? total : 1);
    if (!q->buffer) return 0;
    char *w = q->buffer;
    for (int k = 0; k < 5; k++) {
        if (v[k].length) memcpy(w, v[k].pointer, v[k].length);
        q->length[k] = v[k].length;
        w += v[k].length;
    }
    q->data = envelope->data;
    q->datamodel = envelope->datamodel;
    q->hops = envelope->hops;
    q->at = host->now;
    p->queue_count++;
    return 1;
}

static void flush(ta_host *host, uint32_t handle) {
    peer *p = peer_of(host, handle);
    while (p && p->up && p->queue_count) {
        queued_envelope *q = &p->queue[p->queue_head];
        ta_envelope envelope = envelope_of(q);
        if (!emit_peer(host, handle, &envelope)) return;
        q->data = 0; /* moved to the effect */
        q_release(host, q);
        p->queue_head = (p->queue_head + 1) % p->queue_capacity;
        p->queue_count--;
        p = peer_of(host, handle);
    }
}

static void fail_queue(ta_host *host, uint32_t handle, uint32_t reason) {
    peer *p = peer_of(host, handle);
    while (p && p->queue_count) {
        queued_envelope *q = &p->queue[p->queue_head];
        ta_envelope envelope = envelope_of(q);
        refuse(host, 0, &envelope, reason); /* to the local origin, or back towards a relayed one */
        q_release(host, q);
        p->queue_head = (p->queue_head + 1) % p->queue_capacity;
        p->queue_count--;
    }
}

void ta_router_time(ta_host *host) {
    struct ta_router *router = host->router;
    if (!router) return;
    for (uint32_t i = 0; i < router->peer_count; i++) {
        peer *p = &router->peers[i];
        if (!p->live || !p->ttl) continue;
        while (p->queue_count && host->now - p->queue[p->queue_head].at > p->ttl) {
            queued_envelope *q = &p->queue[p->queue_head];
            ta_envelope envelope = envelope_of(q);
            refuse(host, 0, &envelope, TA_ROUTE_ERROR_EXPIRED);
            q_release(host, q);
            p->queue_head = (p->queue_head + 1) % p->queue_capacity;
            p->queue_count--;
            p->errors++;
        }
    }
}

/* when the oldest queued envelope of some peer expires (UINT64_MAX: never) */
ta_time ta_router_deadline(const ta_host *host) {
    const struct ta_router *router = host->router;
    ta_time d = UINT64_MAX;
    for (uint32_t i = 0; router && i < router->peer_count; i++) {
        const peer *p = &router->peers[i];
        if (p->live && p->ttl && p->queue_count && p->queue[p->queue_head].at + p->ttl + 1 < d)
            d = p->queue[p->queue_head].at + p->ttl + 1;
    }
    return d;
}

/* ---- sending -------------------------------------------------------------------------- */

/* hand `e` (payload moved in on success) to whatever reaches its target
 * system; returns 0 (and the reason) if nothing does */
static uint32_t forward(ta_host *host, const ta_address *to, const ta_envelope *envelope) {
    if (envelope->hops > TA_ROUTE_MAX_HOPS) return TA_ROUTE_ERROR_HOPS;
    uint32_t handle = route_for(host, to->system);
    peer *p = peer_of(host, handle);
    if (!p) return TA_ROUTE_ERROR_UNREACHABLE;
    if (p->up) return emit_peer(host, handle, envelope) ? 0 : TA_ROUTE_ERROR_UNREACHABLE;
    if ((p->flags & TA_PEER_QUEUE) && enqueue(host, p, envelope)) return 0;
    return TA_ROUTE_ERROR_UNREACHABLE;
}

/* a message id: the origin and a host-wide counter (unique per origin); buffer has room for
 * TA_ADDRESS_SESSION_MAX + 24 bytes */
static ta_string_view new_msgid(ta_host *host, ta_string_view origin, char *buffer) {
    uint32_t ml = origin.length < TA_ADDRESS_SESSION_MAX ? origin.length : TA_ADDRESS_SESSION_MAX;
    if (ml) memcpy(buffer, origin.pointer, ml);
    buffer[ml++] = '/';
    char d[24];
    int nd = 0;
    uint64_t v = ++host->message_sequence;
    do {
        d[nd++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (nd)
        buffer[ml++] = d[--nd];
    return (ta_string_view){buffer, ml};
}

void ta_router_outbound(ta_host *host, uint32_t si, const ta_effect *effect) {
    ta_host_message message;
    message.size = sizeof message;
    if (ta_host_message_init(host, effect, 0, 0, &message) != TA_OK) {
        communication_error(host, si, effect->actor, effect->sendid);
        return;
    }
    ta_envelope envelope = message.envelope;
    envelope.datamodel = effect->payload && effect->datamodel < host->config.datamodel_count
                             ? effect->datamodel
                             : TA_NIL;
    envelope.data = envelope.datamodel == TA_NIL ? 0 : effect->payload;
    if (envelope.data && host->datamodels[envelope.datamodel].vtable->retain)
        host->datamodels[envelope.datamodel].vtable->retain(
            host->datamodels[envelope.datamodel].datamodel, envelope.data);
    uint32_t outcome;
    ta_status status = ta_host_deliver(host, 0, &envelope, &outcome);
    if (status == TA_OK && outcome == TA_ROUTE_DELIVERED) host->routed++;
    if (status == TA_OK && (outcome == TA_ROUTE_DELIVERED || outcome == TA_ROUTE_RELAYED)) return;
    if (envelope.data && host->datamodels[envelope.datamodel].vtable->release)
        host->datamodels[envelope.datamodel].vtable->release(
            host->datamodels[envelope.datamodel].datamodel, envelope.data);
}

/* ---- local delivery -------------------------------------------------------------------- */

static uint32_t local_deliver(ta_host *host, uint64_t session, const ta_envelope *envelope) {
    uint32_t si, actor;
    if (!session || ta_host_locate(host, session, &si, &actor) != TA_OK)
        return TA_ROUTE_ERROR_UNKNOWN_TARGET;
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = envelope->name;
    event.sendid = envelope->sendid;
    event.origin = envelope->origin;
    event.origintype =
        envelope->origintype.length
            ? envelope->origintype
            : (ta_string_view){"http://www.w3.org/TR/scxml/#SCXMLEventProcessor", 47};
    event.data = envelope->data;
    return send_to(host, si, actor, &event) == TA_OK ? 0 : TA_ROUTE_ERROR_UNKNOWN_TARGET;
}

/* ---- public API ------------------------------------------------------------------------ */

ta_string_view ta_host_route_reason(uint32_t reason) {
    static const char *const names[] = {"ok",      "unknown-target", "unreachable", "denied",
                                        "expired", "hops",           "malformed"};
    const char *s = reason < sizeof names / sizeof names[0] ? names[reason] : "unreachable";
    return (ta_string_view){s, (uint32_t)strlen(s)};
}

void ta_host_error_event(ta_string_view sendid, ta_event_in *event) {
    memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    event->flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    event->name = (ta_string_view){"error.communication", 19};
    event->sendid = sendid;
}

ta_status ta_host_error_route(ta_host *host, ta_string_view origin, ta_route_request *request) {
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = origin;
    envelope.name = (ta_string_view){"error.communication", 19};
    ta_status status = ta_host_route_begin(host, 0, &envelope, TA_ROUTE_LOCAL_ONLY, request);
    if (status == TA_OK && request->name.length) {
        request->action = TA_ROUTE_REFUSED;
        request->reason = TA_ROUTE_ERROR_MALFORMED;
    }
    return status;
}

ta_status ta_host_route_begin(ta_host *host, uint32_t from, const ta_envelope *envelope,
                              uint32_t flags, ta_route_request *request) {
    if (!host || !envelope || envelope->size < sizeof *envelope || !request ||
        request->size < sizeof *request)
        return TA_ERROR_INVALID;
    memset(request, 0, sizeof *request);
    request->size = sizeof *request;
    request->action = TA_ROUTE_REFUSED;
    request->flags = flags;
    peer *p = from ? peer_of(host, from) : NULL;
    if (from && !p) return TA_ERROR_STALE;
    ta_address to, org;
    if (ta_address_parse(envelope->target, &to) != TA_OK || !envelope->name.length ||
        (envelope->origin.length && ta_address_parse(envelope->origin, &org) != TA_OK))
        request->reason = TA_ROUTE_ERROR_MALFORMED;
    else if (p && (!envelope->origin.length || !speaks_for(host, from, p, org.system)))
        request->reason = TA_ROUTE_ERROR_DENIED;
    else if (envelope->hops > TA_ROUTE_MAX_HOPS)
        request->reason = TA_ROUTE_ERROR_HOPS;
    if (request->reason) return TA_OK;
    request->system = to.system;
    request->hops = envelope->hops;
    if (is_me(host, to.system)) {
        if (p && !allowed(host, from, p, &to, envelope->name)) {
            request->reason = TA_ROUTE_ERROR_DENIED;
            return TA_OK;
        }
        request->session = to.session;
        request->name = to.name;
        request->origin = envelope->origin;
        if (from || (flags & TA_ROUTE_DEDUPLICATE_HOST))
            request->key =
                envelope->deduplication_key.length ? envelope->deduplication_key : envelope->msgid;
        request->action = TA_ROUTE_LOOKUP;
    } else {
        if (flags & TA_ROUTE_LOCAL_ONLY) {
            request->reason = TA_ROUTE_ERROR_UNREACHABLE;
            return TA_OK;
        }
        if (p && !(p->flags & TA_PEER_TRUSTED)) {
            request->reason = TA_ROUTE_ERROR_DENIED;
            return TA_OK;
        }
        if (from && ++request->hops > TA_ROUTE_MAX_HOPS) {
            request->reason = TA_ROUTE_ERROR_HOPS;
            return TA_OK;
        }
        if (flags & TA_ROUTE_ASK_PEERS) {
            request->action = TA_ROUTE_PEERS;
            return TA_OK;
        }
        request->peer = route_for(host, to.system);
        peer *next = peer_of(host, request->peer);
        if (!next && !(flags & TA_ROUTE_DURABLE)) {
            request->reason = TA_ROUTE_ERROR_UNREACHABLE;
            return TA_OK;
        }
        request->peer_id = next ? (ta_string_view){next->id, next->id_length} : to.system;
        request->action = TA_ROUTE_PEER;
    }
    return TA_OK;
}

ta_status ta_host_route_step(ta_route_request *request, uint64_t result) {
    if (!request || request->size < sizeof *request) return TA_ERROR_INVALID;
    if (request->action == TA_ROUTE_LOOKUP) {
        request->session = result;
        request->action = !result               ? TA_ROUTE_REFUSED
                          : request->key.length ? TA_ROUTE_DEDUPLICATE
                                                : TA_ROUTE_LOCAL;
        if (!result) request->reason = TA_ROUTE_ERROR_UNKNOWN_TARGET;
    } else if (request->action == TA_ROUTE_DEDUPLICATE) {
        request->action = result ? TA_ROUTE_LOCAL : TA_ROUTE_DUPLICATE;
    } else {
        return TA_ERROR_STATE;
    }
    return TA_OK;
}

ta_status ta_host_message_init(ta_host *host, const ta_effect *effect, uint64_t revision,
                               uint32_t index, ta_host_message *message) {
    if (!host || !effect || !message || message->size < sizeof *message) return TA_ERROR_INVALID;
    memset(message, 0, sizeof *message);
    message->size = sizeof *message;
    ta_envelope *envelope = &message->envelope;
    envelope->size = sizeof *envelope;
    char *p = message->storage;
    uint32_t n =
        ta_address_format(host->config.system_id, effect->session, p, TA_ADDRESS_SESSION_MAX);
    if (!n) return TA_ERROR_INVALID;
    envelope->origin = (ta_string_view){p, n};
    p += n;
    char token[48];
    int length =
        revision
            ? snprintf(token, sizeof token, "%llu.%u", (unsigned long long)revision, index)
            : snprintf(token, sizeof token, "%llu", (unsigned long long)++host->message_sequence);
    n = (uint32_t)snprintf(p, 384, "%.*s/%s", (int)envelope->origin.length,
                           envelope->origin.pointer, token);
    envelope->msgid = (ta_string_view){p, n};
    p += n + 1;
    n = (uint32_t)snprintf(p, 80, "%llu:%s", (unsigned long long)effect->session, token);
    message->key = (ta_string_view){p, n};
    p += n + 1;
    message->sendid = effect->sendid;
    if (!message->sendid.length) {
        memcpy(p, "auto.", 5);
        memcpy(p + 5, token, (size_t)length);
        message->sendid = (ta_string_view){p, (uint32_t)length + 5};
    }
    envelope->target = effect->target;
    envelope->name = effect->event;
    envelope->sendid = effect->sendid;
    envelope->origintype = (ta_string_view){"http://www.w3.org/TR/scxml/#SCXMLEventProcessor", 47};
    envelope->data = effect->payload;
    envelope->datamodel = effect->datamodel;
    return TA_OK;
}

/* The wire shape lives here; bindings only encode/decode the data value. */
static size_t json_string(char *out, ta_string_view s) {
    size_t n = 0;
    if (out) out[n] = '"';
    n++;
    const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < s.length; i++) {
        unsigned char c = (unsigned char)s.pointer[i];
        if (c < 32) {
            if (out) {
                memcpy(out + n, "\\u00", 4);
                out[n + 4] = hex[c >> 4];
                out[n + 5] = hex[c & 15];
            }
            n += 6;
        } else {
            if (c == '"' || c == '\\') {
                if (out) out[n] = '\\';
                n++;
            }
            if (out) out[n] = (char)c;
            n++;
        }
    }
    if (out) out[n] = '"';
    return n + 1;
}

size_t ta_host_envelope_json(const ta_envelope *envelope, ta_string_view data, char *buffer,
                             size_t capacity) {
    const char *keys[] = {"target", "name", "origin", "origintype", "sendid", "msgid", "dedup"};
    ta_string_view values[] = {envelope->target,           envelope->name,   envelope->origin,
                               envelope->origintype,       envelope->sendid, envelope->msgid,
                               envelope->deduplication_key};
    size_t need = 0;
    for (int pass = 0; pass < 2; pass++) {
        char *out = pass ? buffer : NULL;
        size_t n = 1;
        if (out) out[0] = '{';
        for (uint32_t i = 0; i < 7; i++) {
            if (i >= 3 && !values[i].length) continue;
            if (i) {
                if (out) out[n] = ',';
                n++;
            }
            ta_string_view key = {keys[i], (uint32_t)strlen(keys[i])};
            n += json_string(out ? out + n : NULL, key);
            if (out) out[n] = ':';
            n++;
            n += json_string(out ? out + n : NULL, values[i]);
        }
        if (data.length) {
            if (out) {
                memcpy(out + n, ",\"data\":", 8);
                memcpy(out + n + 8, data.pointer, data.length);
            }
            n += 8 + data.length;
        }
        char tail[40];
        int tl = snprintf(tail, sizeof tail, ",\"hops\":%u}", envelope->hops);
        if (out) {
            memcpy(out + n, tail, (size_t)tl);
            out[n + (size_t)tl] = 0;
        }
        need = n + (size_t)tl;
        if (!buffer || capacity <= need) break;
    }
    return need;
}

static void reach_free(peer *p) {
    for (uint32_t k = 0; k < p->reach_count; k++)
        free(p->reach[k]);
    free(p->reach);
    free(p->reach_lengths);
    p->reach = NULL;
    p->reach_lengths = NULL;
    p->reach_count = 0;
}

ta_status ta_host_peer_reach(ta_host *host, uint32_t handle, const ta_string_view *reach,
                             uint32_t n) {
    peer *p = peer_of(host, handle);
    if (!p) return TA_ERROR_STALE;
    for (uint32_t k = 0; k < n; k++)
        if (!ta_address_system_valid(reach[k])) return TA_ERROR_INVALID;
    reach_free(p);
    if (!n) return TA_OK;
    p->reach = (char **)calloc(n, sizeof *p->reach);
    p->reach_lengths = (uint32_t *)calloc(n, sizeof *p->reach_lengths);
    if (!p->reach || !p->reach_lengths) {
        reach_free(p);
        return TA_ERROR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < n; k++) {
        if (is_me(host, reach[k])) continue; /* a peer never speaks for us */
        p->reach[p->reach_count] = dupn(reach[k].pointer, reach[k].length);
        if (!p->reach[p->reach_count]) return TA_ERROR_NO_MEMORY;
        p->reach_lengths[p->reach_count++] = reach[k].length;
    }
    return TA_OK;
}

ta_status ta_host_peer_add(ta_host *host, const ta_peer_config *config, uint32_t *out) {
    if (out) *out = 0;
    if (!config || !ta_address_system_valid(config->id) || is_me(host, config->id))
        return TA_ERROR_INVALID;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < router->peer_count; i++) /* the newer connection wins */
        if (router->peers[i].live &&
            seq(config->id, router->peers[i].id, router->peers[i].id_length))
            ta_host_peer_remove(host, peer_handle(i, router->peers[i].generation));
    uint32_t slot = router->peer_count;
    for (uint32_t i = 0; i < router->peer_count; i++)
        if (!router->peers[i].live) {
            slot = i;
            break;
        }
    if (slot + 1 >= TA_PEER_MAX_SLOTS) return TA_ERROR_NO_MEMORY; /* out of handle bits */
    if (slot == router->peer_count) {
        peer *np =
            (peer *)grow(router->peers, &router->peer_capacity, router->peer_count + 1, sizeof *np);
        if (!np) return TA_ERROR_NO_MEMORY;
        router->peers = np;
        router->peer_count++;
    }
    peer *p = &router->peers[slot];
    uint32_t generation = p->generation; /* dead slots keep their bumped generation */
    memset(p, 0, sizeof *p);
    p->generation = generation;
    p->live = 1;
    p->up = 1;
    p->state = TA_PEER_OPEN;
    p->flags = config->flags;
    memcpy(p->id, config->id.pointer, config->id.length);
    p->id_length = config->id.length;
    p->max_queue = config->max_queue ? config->max_queue : 1000;
    p->ttl = config->queue_ttl;
    ta_status status =
        ta_host_peer_reach(host, peer_handle(slot, generation), config->reach, config->reach_count);
    if (status != TA_OK) {
        reach_free(p);
        p->live = 0;
        return status;
    }
    if (out) *out = peer_handle(slot, generation);
    return TA_OK;
}

ta_status ta_host_peer_up(ta_host *host, uint32_t handle, int up) {
    peer *p = peer_of(host, handle);
    if (!p) return TA_ERROR_STALE;
    p->up = up != 0;
    p->state = up ? TA_PEER_OPEN : TA_PEER_RECONNECTING;
    if (up)
        flush(host, handle);
    else if (!(p->flags & TA_PEER_QUEUE))
        fail_queue(host, handle, TA_ROUTE_ERROR_UNREACHABLE);
    return TA_OK;
}

ta_status ta_host_peer_remove(ta_host *host, uint32_t handle) {
    peer *p = peer_of(host, handle);
    if (!p) return TA_ERROR_STALE;
    fail_queue(host, handle, TA_ROUTE_ERROR_UNREACHABLE);
    p = peer_of(host, handle);
    uint32_t generation = p->generation; /* handles of this peer (and older ones) stay stale */
    free(p->queue);
    reach_free(p);
    struct ta_router *router = host->router;
    for (uint32_t i = 0; i < router->route_count;)
        if (router->routes[i].peer == handle) {
            free(router->routes[i].system);
            router->routes[i] = router->routes[--router->route_count];
        } else {
            i++;
        }
    for (uint32_t i = 0; i < router->export_count;)
        if (router->exports[i].peer == handle) {
            free(router->exports[i].name);
            free(router->exports[i].events);
            router->exports[i] = router->exports[--router->export_count];
        } else {
            i++;
        }
    memset(p, 0, sizeof *p);
    p->generation = generation + 1; /* a new peer into this slot gets a different handle */
    return TA_OK;
}

ta_status ta_host_route(ta_host *host, ta_string_view system, uint32_t handle) {
    if (!ta_address_system_valid(system) || is_me(host, system)) return TA_ERROR_INVALID;
    if (handle && !peer_of(host, handle)) return TA_ERROR_STALE;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < router->route_count; i++)
        if (seq(system, router->routes[i].system, router->routes[i].length)) {
            if (handle) {
                router->routes[i].peer = handle;
            } else {
                free(router->routes[i].system);
                router->routes[i] = router->routes[--router->route_count];
            }
            return TA_OK;
        }
    if (!handle) return TA_OK;
    route *nr =
        (route *)grow(router->routes, &router->route_capacity, router->route_count + 1, sizeof *nr);
    if (!nr) return TA_ERROR_NO_MEMORY;
    router->routes = nr;
    route *x = &router->routes[router->route_count];
    x->system = dupn(system.pointer, system.length);
    if (!x->system) return TA_ERROR_NO_MEMORY;
    x->length = system.length;
    x->peer = handle;
    router->route_count++;
    return TA_OK;
}

ta_status ta_host_export(ta_host *host, uint32_t handle, ta_string_view name,
                         ta_string_view events) {
    if (!name.length) return TA_ERROR_INVALID;
    if (handle && !peer_of(host, handle)) return TA_ERROR_STALE;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    exported *nx = (exported *)grow(router->exports, &router->export_capacity,
                                    router->export_count + 1, sizeof *nx);
    if (!nx) return TA_ERROR_NO_MEMORY;
    router->exports = nx;
    exported *x = &router->exports[router->export_count];
    x->peer = handle;
    x->name = dupn(name.pointer, name.length);
    x->events = dupn(events.pointer ? events.pointer : "", events.length);
    if (!x->name || !x->events) {
        free(x->name);
        free(x->events);
        return TA_ERROR_NO_MEMORY;
    }
    x->name_length = name.length;
    x->events_length = events.length;
    router->export_count++;
    return TA_OK;
}

ta_status ta_host_deliver(ta_host *host, uint32_t from, const ta_envelope *envelope,
                          uint32_t *outcome) {
    uint32_t dummy;
    if (!outcome) outcome = &dummy;
    *outcome = TA_ROUTE_REFUSED;
    if (!envelope || envelope->size < sizeof(ta_envelope)) return TA_ERROR_INVALID;
    peer *p = NULL;
    if (from) {
        p = peer_of(host, from);
        if (!p) return TA_ERROR_STALE;
        p->received++;
    }
    ta_route_request request;
    request.size = sizeof request;
    ta_status status = ta_host_route_begin(host, from, envelope, 0, &request);
    if (status != TA_OK) return status;
    if (request.action == TA_ROUTE_LOOKUP) {
        uint64_t session = request.session;
        if (request.name.length) {
            char *name = (char *)malloc(request.name.length);
            if (!name) return TA_ERROR_NO_MEMORY;
            uint32_t length = ta_address_name_decode(request.name, name, request.name.length);
            session = ta_host_actor_by_name(host, (ta_string_view){name, length});
            free(name);
        }
        uint32_t si, actor;
        if (!session || ta_host_locate(host, session, &si, &actor) != TA_OK) session = 0;
        ta_host_route_step(&request, session);
    }
    if (request.action == TA_ROUTE_DEDUPLICATE) {
        struct ta_router *router = router_of(host);
        if (!router) return TA_ERROR_NO_MEMORY;
        ta_host_route_step(&request, !dedup_seen(router, request.origin, request.key));
    }
    if (request.action == TA_ROUTE_DUPLICATE) {
        if (p) p->duplicates++;
        *outcome = TA_ROUTE_DUPLICATE;
        return TA_OK;
    }
    uint32_t reason = request.reason;
    if (request.action == TA_ROUTE_LOCAL) {
        reason = local_deliver(host, request.session, envelope);
        if (!reason) {
            *outcome = TA_ROUTE_DELIVERED;
            return TA_OK;
        }
    } else if (request.action == TA_ROUTE_PEER) {
        ta_envelope forwarded = *envelope;
        forwarded.hops = request.hops;
        char mid[TA_ADDRESS_SESSION_MAX + 24];
        if (!forwarded.msgid.length)
            forwarded.msgid = new_msgid(host, envelope->origin, mid); /* from host code */
        ta_address to;
        ta_address_parse(envelope->target, &to);
        reason = forward(host, &to, &forwarded);
        if (!reason) {
            *outcome = TA_ROUTE_RELAYED;
            return TA_OK;
        }
    }
    if (from)
        emit_error(host, from, envelope->origin, envelope->msgid, envelope->sendid, reason);
    else
        local_error(host, envelope->origin, envelope->sendid);
    return TA_OK;
}

ta_status ta_host_peer_error(ta_host *host, uint32_t from, ta_string_view origin,
                             ta_string_view msgid, ta_string_view sendid, uint32_t reason) {
    peer *p = peer_of(host, from);
    if (!p) return TA_ERROR_STALE;
    if (local_error(host, origin, sendid)) return TA_OK;
    /* about an envelope we relayed: back towards its origin */
    ta_address o;
    if (ta_address_parse(origin, &o) != TA_OK) return TA_ERROR_INVALID;
    uint32_t back = route_for(host, o.system);
    if (back && back != from) emit_error(host, back, origin, msgid, sendid, reason);
    return TA_OK;
}

void ta_host_peer_round_trip_time(ta_host *host, uint32_t handle, uint32_t ms) {
    peer *p = peer_of(host, handle);
    if (p) p->round_trip_time = ms;
}

ta_status ta_host_peer_status(const ta_host *host, uint32_t handle, ta_peer_status *out) {
    peer *p = peer_of((ta_host *)host, handle);
    if (!p) return TA_ERROR_STALE;
    ta_peer_status z;
    memset(&z, 0, sizeof z);
    z.state = p->state;
    z.queued = p->queue_count;
    z.round_trip_time = p->round_trip_time;
    z.lag = p->queue_count ? host->now - p->queue[p->queue_head].at : 0;
    z.sent = p->sent;
    z.received = p->received;
    z.errors = p->errors;
    z.duplicates = p->duplicates;
    z.id = (ta_string_view){p->id, p->id_length};
    size_t n = out->size && out->size < sizeof z ? out->size : sizeof z;
    z.size = (uint32_t)n;
    memcpy(out, &z, n);
    return TA_OK;
}

void ta_router_free(ta_host *host) {
    struct ta_router *router = host->router;
    if (!router) return;
    for (uint32_t i = 0; i < router->peer_count; i++)
        if (router->peers[i].live) {
            peer *p = &router->peers[i];
            while (p->queue_count) {
                q_release(host, &p->queue[p->queue_head]);
                p->queue_head = (p->queue_head + 1) % p->queue_capacity;
                p->queue_count--;
            }
            free(p->queue);
            reach_free(p);
        }
    for (uint32_t i = 0; i < router->route_count; i++)
        free(router->routes[i].system);
    for (uint32_t i = 0; i < router->export_count; i++) {
        free(router->exports[i].name);
        free(router->exports[i].events);
    }
    free(router->peers);
    free(router->routes);
    free(router->exports);
    free(router);
    host->router = NULL;
}

/* the dedup window, for snapshots: count, head, hashes */
size_t ta_router_snapshot(const ta_host *host, unsigned char *out, size_t capacity) {
    const struct ta_router *router = host->router;
    uint32_t n = router ? router->seen_count : 0;
    size_t need = 4 + (size_t)n * 8;
    if (out && capacity >= need) {
        memcpy(out, &n, 4);
        for (uint32_t i = 0; i < n; i++)
            memcpy(out + 4 + (size_t)i * 8,
                   &router->seen[(router->seen_head + i) % DEDUPLICATION_WINDOW], 8);
    }
    return need;
}

int ta_router_restore(ta_host *host, const unsigned char *p, size_t length) {
    uint32_t n;
    if (length < 4) return 0;
    memcpy(&n, p, 4);
    if (n > DEDUPLICATION_WINDOW || length < 4 + (size_t)n * 8) return 0;
    if (!n) return 1;
    struct ta_router *router = router_of(host);
    if (!router) return 0;
    for (uint32_t i = 0; i < n; i++)
        memcpy(&router->seen[i], p + 4 + (size_t)i * 8, 8);
    router->seen_head = 0;
    router->seen_count = n;
    return 1;
}
