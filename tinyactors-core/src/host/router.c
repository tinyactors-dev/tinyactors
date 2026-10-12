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
 * Memory through the host's allocator (host_internal.h): a call it refuses
 * returns TA_ERROR_NO_MEMORY with nothing changed, or stops where it was
 * (queues keep what they did not flush or fail). No C library, no I/O, no
 * callbacks. */
#include "host/host_internal.h"

#include "base/cbor_output.h"
#include "base/format.h"
#include "base/memory.h"

#include <tinyactors/cbor.h>

#define DEDUPLICATION_WINDOW 4096u /* message ids remembered per host (FIFO) */

enum { QUEUED_FIELDS = 9 };
typedef struct queued_envelope { /* an envelope waiting for a peer */
    char *buffer; /* target | name | sendid | origin | msgid | traceparent | origintype |
                     dedup key | wire (one allocation) */
    size_t buffer_size;
    uint32_t length[QUEUED_FIELDS];
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
    char **reach; /* announced systems (owned; reach_capacity entries) */
    uint32_t *reach_lengths, reach_count, reach_capacity;
    queued_envelope *queue;
    uint32_t queue_head, queue_count, queue_capacity, max_queue;
    ta_time ttl;
    uint32_t round_trip_time;
    uint64_t sent, received, errors, duplicates;
} peer;

typedef struct route { /* configured: system id -> peer */
    char *system;      /* `length` bytes */
    uint32_t length, peer;
} route;

typedef struct exported {
    uint32_t peer; /* 0: every peer */
    char *name;    /* "*": everything (name_length bytes) */
    uint32_t name_length;
    char *events; /* SCXML event descriptors, space separated (NULL = all; events_length bytes) */
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
    if (!host->router) {
        host->router = (struct ta_router *)host_alloc(host, sizeof *host->router);
        if (host->router) ta_memset(host->router, 0, sizeof *host->router);
    }
    return host->router;
}

static int seq(ta_string_view a, const char *b, uint32_t n) {
    return a.length == n && (!n || ta_memcmp(a.pointer, b, n) == 0);
}

/* A copy of `s` (s.length bytes, no terminator; NULL when empty or refused). */
static char *string_copy(ta_host *host, ta_string_view s) {
    if (!s.length) return NULL;
    char *d = (char *)host_alloc(host, s.length);
    if (d) ta_memcpy(d, s.pointer, s.length);
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

static uint64_t dedup_hash(ta_string_view origin, ta_string_view key) {
    return fnv(fnv(0xcbf29ce484222325ull, origin), key) | 1; /* 0 = empty */
}

/* seen before? (an empty key: nothing to compare) */
static int dedup_seen(const struct ta_router *router, ta_string_view origin, ta_string_view key) {
    if (!key.length) return 0;
    uint64_t x = dedup_hash(origin, key);
    for (uint32_t i = 0; i < router->seen_count; i++)
        if (router->seen[(router->seen_head + i) % DEDUPLICATION_WINDOW] == x) return 1;
    return 0;
}

/* remember (origin, key), once the envelope was handled */
static void dedup_remember(struct ta_router *router, ta_string_view origin, ta_string_view key) {
    if (!key.length) return;
    uint64_t x = dedup_hash(origin, key);
    if (router->seen_count < DEDUPLICATION_WINDOW) {
        router->seen[(router->seen_head + router->seen_count++) % DEDUPLICATION_WINDOW] = x;
    } else {
        router->seen[router->seen_head] = x;
        router->seen_head = (router->seen_head + 1) % DEDUPLICATION_WINDOW;
    }
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
        if (name.length >= n && !ta_memcmp(name.pointer, d, n) &&
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
    ta_buffer decoded = {dec, sizeof dec, 0};
    uint32_t dl = to->kind == TA_ADDRESS_NAME && ta_address_name_decode(to->name, &decoded) == TA_OK
                      ? (uint32_t)decoded.length
                      : 0;
    for (uint32_t i = 0; i < router->export_count; i++) {
        const exported *x = &router->exports[i];
        if (x->peer && x->peer != handle) continue;
        int any = x->name_length == 1 && x->name[0] == '*';
        int hit = any || (to->kind == TA_ADDRESS_NAME && x->name_length == dl &&
                          !ta_memcmp(x->name, dec, dl));
        if (hit && descriptor_match(x->events, x->events_length, name)) return 1;
    }
    return 0;
}

/* The origintype of an envelope without one; the wire leaves it out. */
#define SCXML_EVENT_PROCESSOR                                                                    \
    ((ta_string_view){"http://www.w3.org/TR/scxml/#SCXMLEventProcessor", 47})

/* ---- effects ------------------------------------------------------------------------ */

/* String k (EFFECT_STRINGS) of the newest effect, past the four
 * host_effect_strings_set writes. */
static void effect_string_put(ta_host *host, uint32_t k, ta_string_view value,
                              ta_string_view *view) {
    effect_strings *strings = &host->effect_strings[host->effect_count - 1];
    strings->offset[k] = host_pool_put(host, value);
    view->length = strings->offset[k] == (size_t)-1 ? 0 : value.length;
}

/* an envelope for the transport (payload moved in on success) */
static ta_status emit_peer(ta_host *host, uint32_t handle, const ta_envelope *envelope) {
    if (!host_effects_reserve(host, 1,
                              (size_t)envelope->target.length + envelope->name.length +
                                  envelope->sendid.length + envelope->origin.length +
                                  envelope->msgid.length + envelope->origintype.length +
                                  envelope->deduplication_key.length + envelope->wire.length))
        return TA_ERROR_NO_MEMORY;
    ta_host_effect *x = host_effect_new(host, TA_HOST_EFFECT_PEER, TA_NIL);
    x->peer = handle;
    x->flags = envelope->hops;
    if (ta_traceparent_parse(envelope->traceparent, &x->trace) != TA_OK) /* none, or not one */
        ta_memset(&x->trace, 0, sizeof x->trace);
    host_effect_strings_set(host, envelope->target, envelope->name, envelope->sendid,
                            envelope->origin);
    effect_string_put(host, 4, envelope->msgid, &x->msgid);
    effect_string_put(host, 5, envelope->origintype, &x->origintype);
    effect_string_put(host, 6, envelope->deduplication_key, &x->deduplication_key);
    /* a value that arrived as bytes leaves as the same bytes: never decoded here */
    ta_string_view wire = {(const char *)envelope->wire.pointer, (uint32_t)envelope->wire.length};
    ta_string_view put = {0, 0};
    effect_string_put(host, 7, wire, &put);
    x->wire.length = put.length;
    if (envelope->data &&
        envelope->datamodel <
            host->config.datamodel_count) { /* the effect owns the payload until ack */
        x->payload = envelope->data;
        x->datamodel = envelope->datamodel;
    }
    peer *p = peer_of(host, handle);
    if (p) p->sent++;
    return TA_OK;
}

static ta_status emit_error(ta_host *host, uint32_t handle, ta_string_view origin,
                            ta_string_view msgid, ta_string_view sendid, uint32_t reason) {
    if (!host_effects_reserve(host, 1, (size_t)origin.length + msgid.length + sendid.length))
        return TA_ERROR_NO_MEMORY;
    ta_host_effect *x = host_effect_new(host, TA_HOST_EFFECT_PEER_ERROR, TA_NIL);
    x->peer = handle;
    x->token = reason;
    host_effect_strings_set(host, (ta_string_view){"", 0}, (ta_string_view){"", 0}, sendid, origin);
    effect_string_put(host, 4, msgid, &x->msgid);
    peer *p = peer_of(host, handle);
    if (p) p->errors++;
    return TA_OK;
}

/* error.communication for a local origin session; *handled = 0 if it is not ours */
static ta_status local_error(ta_host *host, ta_string_view origin, ta_string_view sendid,
                             int *handled) {
    *handled = 0;
    ta_route_request request;
    request.size = sizeof request;
    if (ta_host_error_route(host, origin, &request) != TA_OK || request.action != TA_ROUTE_LOOKUP)
        return TA_OK;
    *handled = 1;
    uint32_t si, actor;
    if (host_locate(host, request.session, &si, &actor) != TA_OK)
        return TA_OK; /* gone: nobody to tell */
    return host_communication_error(host, si, actor, sendid);
}

/* an error about `e`, which came from `from` (0: from here) */
static ta_status refuse(ta_host *host, uint32_t from, const ta_envelope *envelope,
                        uint32_t reason) {
    int handled;
    ta_status status = local_error(host, envelope->origin, envelope->sendid, &handled);
    if (status != TA_OK || handled) return status;
    ta_address o;
    if (!from && ta_address_parse(envelope->origin, &o) == TA_OK)
        from = route_for(host, o.system); /* relayed: back the way replies go */
    if (from)
        return emit_error(host, from, envelope->origin, envelope->msgid, envelope->sendid, reason);
    return TA_OK;
}

/* ---- queues ------------------------------------------------------------------------- */

static void q_release(ta_host *host, queued_envelope *q) {
    if (q->data && q->datamodel < host->config.datamodel_count &&
        host->datamodels[q->datamodel].vtable->release)
        host->datamodels[q->datamodel].vtable->release(host->datamodels[q->datamodel].datamodel,
                                                       q->data);
    host_free(host, q->buffer, q->buffer_size);
    ta_memset(q, 0, sizeof *q);
}

static ta_envelope envelope_of(const queued_envelope *q) {
    ta_envelope envelope;
    ta_memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    const char *p = q->buffer;
    ta_string_view wire = {0, 0};
    ta_string_view *f[QUEUED_FIELDS] = {&envelope.target,     &envelope.name,
                                        &envelope.sendid,     &envelope.origin,
                                        &envelope.msgid,      &envelope.traceparent,
                                        &envelope.origintype, &envelope.deduplication_key,
                                        &wire};
    for (int k = 0; k < QUEUED_FIELDS; k++) {
        *f[k] = (ta_string_view){p, q->length[k]};
        p += q->length[k];
    }
    envelope.wire = (ta_bytes){wire.pointer, wire.length};
    envelope.data = q->data;
    envelope.datamodel = q->datamodel;
    envelope.hops = q->hops;
    return envelope;
}

/* drop the oldest queued envelope of p (after it was flushed or failed) */
static void dequeue(ta_host *host, peer *p) {
    q_release(host, &p->queue[p->queue_head]);
    p->queue_head = (p->queue_head + 1) % p->queue_capacity;
    p->queue_count--;
}

/* keep `e` (payload moved in) for a peer that is down: TA_OK; TA_ERROR_STATE
 * if the queue is full; TA_ERROR_NO_MEMORY if refused (nothing kept) */
static ta_status enqueue(ta_host *host, peer *p, const ta_envelope *envelope) {
    if (p->queue_count >= p->max_queue) return TA_ERROR_STATE;
    if (p->queue_count == p->queue_capacity) { /* grow the ring, keeping order */
        uint32_t nc = p->queue_capacity ? p->queue_capacity * 2 : 16;
        queued_envelope *nq = (queued_envelope *)host_alloc(host, sizeof *nq * nc);
        if (!nq) return TA_ERROR_NO_MEMORY;
        ta_memset(nq, 0, sizeof *nq * nc);
        for (uint32_t i = 0; i < p->queue_count; i++)
            nq[i] = p->queue[(p->queue_head + i) % p->queue_capacity];
        host_free(host, p->queue, sizeof *nq * p->queue_capacity);
        p->queue = nq;
        p->queue_capacity = nc;
        p->queue_head = 0;
    }
    ta_string_view v[QUEUED_FIELDS] = {
        envelope->target,     envelope->name,
        envelope->sendid,     envelope->origin,
        envelope->msgid,      envelope->traceparent,
        envelope->origintype, envelope->deduplication_key,
        {(const char *)envelope->wire.pointer, (uint32_t)envelope->wire.length}};
    size_t total = 0;
    for (int k = 0; k < QUEUED_FIELDS; k++)
        total += v[k].length;
    queued_envelope *q = &p->queue[(p->queue_head + p->queue_count) % p->queue_capacity];
    q->buffer_size = total ? total : 1;
    q->buffer = (char *)host_alloc(host, q->buffer_size);
    if (!q->buffer) {
        q->buffer_size = 0;
        return TA_ERROR_NO_MEMORY;
    }
    char *w = q->buffer;
    for (int k = 0; k < QUEUED_FIELDS; k++) {
        if (v[k].length) ta_memcpy(w, v[k].pointer, v[k].length);
        q->length[k] = v[k].length;
        w += v[k].length;
    }
    q->data = envelope->data;
    q->datamodel = envelope->datamodel;
    q->hops = envelope->hops;
    q->at = host->now;
    p->queue_count++;
    return TA_OK;
}

/* the queued envelopes out to an open peer, in order; TA_ERROR_NO_MEMORY:
 * the rest stay queued */
static ta_status flush(ta_host *host, uint32_t handle) {
    peer *p = peer_of(host, handle);
    while (p && p->up && p->queue_count) {
        queued_envelope *q = &p->queue[p->queue_head];
        ta_envelope envelope = envelope_of(q);
        ta_status status = emit_peer(host, handle, &envelope);
        if (status != TA_OK) return status;
        q->data = 0; /* moved to the effect */
        dequeue(host, p);
        p = peer_of(host, handle);
    }
    return TA_OK;
}

/* fail the queued envelopes back to their origins; TA_ERROR_NO_MEMORY: the
 * rest stay queued */
static ta_status fail_queue(ta_host *host, uint32_t handle, uint32_t reason) {
    peer *p = peer_of(host, handle);
    while (p && p->queue_count) {
        ta_envelope envelope = envelope_of(&p->queue[p->queue_head]);
        /* to the local origin, or back towards a relayed one */
        ta_status status = refuse(host, 0, &envelope, reason);
        if (status != TA_OK) return status;
        dequeue(host, p);
    }
    return TA_OK;
}

void ta_router_time(ta_host *host) {
    struct ta_router *router = host->router;
    host->expiry_behind = 0;
    if (!router) return;
    for (uint32_t i = 0; i < router->peer_count; i++) {
        peer *p = &router->peers[i];
        if (!p->live || !p->ttl) continue;
        while (p->queue_count && host->now - p->queue[p->queue_head].at > p->ttl) {
            ta_envelope envelope = envelope_of(&p->queue[p->queue_head]);
            if (refuse(host, 0, &envelope, TA_ROUTE_ERROR_EXPIRED) != TA_OK) {
                host->expiry_behind = 1; /* still queued: reported by the next run */
                return;
            }
            dequeue(host, p);
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
 * system; *reason: 0, or why nothing does. TA_ERROR_NO_MEMORY: refused,
 * nothing done. */
static ta_status forward(ta_host *host, const ta_address *to, const ta_envelope *envelope,
                         uint32_t *reason) {
    *reason = 0;
    if (envelope->hops > TA_ROUTE_MAX_HOPS) {
        *reason = TA_ROUTE_ERROR_HOPS;
        return TA_OK;
    }
    uint32_t handle = route_for(host, to->system);
    peer *p = peer_of(host, handle);
    ta_status status = TA_ERROR_STATE;
    if (p && p->up)
        status = emit_peer(host, handle, envelope);
    else if (p && (p->flags & TA_PEER_QUEUE))
        status = enqueue(host, p, envelope);
    if (status == TA_ERROR_NO_MEMORY) return status;
    if (status != TA_OK) *reason = TA_ROUTE_ERROR_UNREACHABLE;
    return TA_OK;
}

#define MESSAGE_ID_MAX (TA_ADDRESS_SESSION_MAX + 1 + TA_FORMAT_U64_MAX)

/* a message id: the origin and a host-wide counter (unique per origin);
 * buffer has room for MESSAGE_ID_MAX bytes (an origin longer than a session
 * address is cut there: the counter alone keeps it unique) */
static ta_string_view new_msgid(ta_host *host, ta_string_view origin, char *buffer) {
    ta_text text = ta_text_init(buffer, MESSAGE_ID_MAX);
    if (origin.length > TA_ADDRESS_SESSION_MAX) origin.length = TA_ADDRESS_SESSION_MAX;
    ta_text_append(&text, origin);
    ta_text_append(&text, (ta_string_view){"/", 1});
    ta_text_append_u64(&text, ++host->message_sequence);
    return (ta_string_view){buffer, text.length};
}

ta_status ta_router_outbound(ta_host *host, uint32_t si, const ta_effect *effect) {
    ta_host_message message;
    message.size = sizeof message;
    message.revision = 0;
    message.index = 0;
    if (ta_host_message_init(host, effect, &message) != TA_OK)
        return host_communication_error(host, si, effect->actor, effect->sendid);
    ta_envelope envelope = message.envelope;
    envelope.datamodel = effect->payload && effect->datamodel < host->config.datamodel_count
                             ? effect->datamodel
                             : TA_NIL;
    envelope.data = envelope.datamodel == TA_NIL ? 0 : effect->payload;
    if (envelope.data && host->datamodels[envelope.datamodel].vtable->retain)
        host->datamodels[envelope.datamodel].vtable->retain(
            host->datamodels[envelope.datamodel].datamodel, envelope.data);
    int32_t outcome = ta_host_deliver(host, 0, &envelope);
    if (outcome == TA_ROUTE_DELIVERED) host->routed++;
    if (outcome == TA_ROUTE_DELIVERED || outcome == TA_ROUTE_RELAYED) return TA_OK;
    if (envelope.data && host->datamodels[envelope.datamodel].vtable->release)
        host->datamodels[envelope.datamodel].vtable->release(
            host->datamodels[envelope.datamodel].datamodel, envelope.data);
    return outcome == TA_ERROR_NO_MEMORY ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* ---- local delivery -------------------------------------------------------------------- */

/* The value of an envelope that arrived as bytes, read by the datamodel of
 * the chart the target session runs: values are never decoded by another
 * datamodel, nor before the target is known. *reason: MALFORMED when that
 * datamodel cannot read it (or reads no values). */
static ta_status wire_decode(ta_host *host, uint32_t si, uint32_t actor, ta_bytes wire,
                             ta_payload *out, const ta_datamodel_binding **binding,
                             uint32_t *reason) {
    ta_actor_info info;
    info.size = sizeof info;
    ta_actor_info_get(ta_host_system(host, si), actor, &info);
    *binding = host_chart_datamodel(host, si, info.chart);
    int reads = *binding && (*binding)->vtable->size >= offsetof(ta_datamodel_vtable, from_wire) +
                                                          sizeof(*binding)->vtable->from_wire &&
                (*binding)->vtable->from_wire;
    ta_status status = reads
                           ? (*binding)->vtable->from_wire((*binding)->datamodel, wire, out)
                           : TA_ERROR_INVALID;
    if (status == TA_ERROR_NO_MEMORY) return status;
    if (status != TA_OK) {
        *out = 0;
        *reason = TA_ROUTE_ERROR_MALFORMED;
    }
    return TA_OK;
}

/* *reason: 0 delivered, or why not; TA_ERROR_NO_MEMORY: the target's segment
 * could not grow (nothing delivered) */
static ta_status local_deliver(ta_host *host, uint64_t session, const ta_envelope *envelope,
                               uint32_t *reason) {
    uint32_t si, actor;
    *reason = TA_ROUTE_ERROR_UNKNOWN_TARGET;
    if (!session || host_locate(host, session, &si, &actor) != TA_OK) return TA_OK;
    ta_payload data = envelope->data;
    const ta_datamodel_binding *decoded = (const ta_datamodel_binding *)0;
    if (envelope->wire.length) { /* from bytes: only now is the reader known */
        *reason = 0;
        ta_status status =
            wire_decode(host, si, actor, envelope->wire, &data, &decoded, reason);
        if (status != TA_OK) return status;
        if (*reason) return TA_OK;
        *reason = TA_ROUTE_ERROR_UNKNOWN_TARGET;
    }
    ta_event_in event;
    ta_memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = envelope->name;
    event.sendid = envelope->sendid;
    event.origin = envelope->origin;
    event.origintype =
        envelope->origintype.length
            ? envelope->origintype
            : SCXML_EVENT_PROCESSOR;
    event.data = data;
    event.traceparent = envelope->traceparent; /* the trace crosses systems unbroken */
    event.trace_flags = envelope->trace_flags; /* and keeps its detail class in this process */
    ta_status status = host_send_to(host, si, actor, &event);
    if (status != TA_OK && data && decoded && decoded->vtable->release)
        decoded->vtable->release(decoded->datamodel, data); /* ours: the caller had bytes */
    if (status == TA_ERROR_NO_MEMORY) return status;
    if (status == TA_OK) *reason = 0;
    return TA_OK;
}

/* ---- public API ------------------------------------------------------------------------ */

#define VIEW(literal) {literal, sizeof literal - 1}

ta_string_view ta_host_route_reason(uint32_t reason) {
    static const ta_string_view names[] = {
        VIEW("ok"),      VIEW("unknown-target"), VIEW("unreachable"), VIEW("denied"),
        VIEW("expired"), VIEW("hops"),           VIEW("malformed")};
    return reason < sizeof names / sizeof names[0] ? names[reason]
                                                   : names[TA_ROUTE_ERROR_UNREACHABLE];
}

void ta_host_error_event(ta_string_view sendid, ta_event_in *event) {
    ta_memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    event->flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    event->name = (ta_string_view){"error.communication", 19};
    event->sendid = sendid;
}

ta_status ta_host_error_route(ta_host *host, ta_string_view origin, ta_route_request *request) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    ta_envelope envelope;
    ta_memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = origin;
    envelope.name = (ta_string_view){"error.communication", 19};
    request->flags = TA_ROUTE_LOCAL_ONLY;
    request->from = 0;
    ta_status status = ta_host_route_begin(host, &envelope, request);
    if (status == TA_OK && request->name.length) {
        request->action = TA_ROUTE_REFUSED;
        request->reason = TA_ROUTE_ERROR_MALFORMED;
    }
    return status;
}

ta_status ta_host_route_begin(ta_host *host, const ta_envelope *envelope,
                              ta_route_request *request) {
    if (!host || !envelope || envelope->size < sizeof *envelope || !request ||
        request->size < sizeof *request)
        return TA_ERROR_INVALID;
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t flags = request->flags, from = request->from;
    ta_memset(request, 0, sizeof *request);
    request->size = sizeof *request;
    request->action = TA_ROUTE_REFUSED;
    request->flags = flags;
    request->from = from;
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

ta_status ta_host_message_init(ta_host *host, const ta_effect *effect, ta_host_message *message) {
    if (!host || !effect || !message || message->size < sizeof *message) return TA_ERROR_INVALID;
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint64_t revision = message->revision;
    uint32_t index = message->index;
    ta_memset(message, 0, sizeof *message);
    message->size = sizeof *message;
    message->revision = revision;
    message->index = index;
    ta_envelope *envelope = &message->envelope;
    envelope->size = sizeof *envelope;
    /* storage: origin | msgid (origin "/" token) | key (session ":" token) | "auto." token */
    char *p = message->storage;
    ta_buffer origin = {p, TA_ADDRESS_SESSION_MAX, 0};
    if (ta_address_format(host->config.system_id, effect->session, &origin) != TA_OK)
        return TA_ERROR_INVALID;
    uint32_t n = (uint32_t)origin.length;
    envelope->origin = (ta_string_view){p, n};
    char token_bytes[2 * TA_FORMAT_U64_MAX + 1];
    ta_text token = ta_text_init(token_bytes, sizeof token_bytes);
    if (revision) {
        ta_text_append_u64(&token, revision);
        ta_text_append(&token, (ta_string_view){".", 1});
        ta_text_append_u64(&token, index);
    } else {
        ta_text_append_u64(&token, ++host->message_sequence);
    }
    ta_string_view token_view = {token_bytes, token.length};
    ta_text text = ta_text_init(p + n, (uint32_t)sizeof message->storage - n);
    ta_text_append(&text, envelope->origin);
    ta_text_append(&text, (ta_string_view){"/", 1});
    ta_text_append(&text, token_view);
    envelope->msgid = (ta_string_view){p + n, text.length};
    uint32_t start = text.length;
    ta_text_append_u64(&text, effect->session);
    ta_text_append(&text, (ta_string_view){":", 1});
    ta_text_append(&text, token_view);
    message->key = (ta_string_view){p + n + start, text.length - start};
    message->sendid = effect->sendid;
    if (!message->sendid.length) {
        start = text.length;
        ta_text_append(&text, (ta_string_view){"auto.", 5});
        ta_text_append(&text, token_view);
        message->sendid = (ta_string_view){p + n + start, text.length - start};
    }
    if (token.overflow || text.overflow) return TA_ERROR_INVALID; /* never a cut identity */
    envelope->target = effect->target;
    envelope->name = effect->event;
    envelope->sendid = effect->sendid;
    envelope->origintype = SCXML_EVENT_PROCESSOR;
    envelope->data = effect->payload;
    envelope->datamodel = effect->datamodel;
    envelope->trace_flags = effect->trace.flags & ~(uint32_t)0xFFu;
    ta_buffer traceparent = {message->traceparent, TA_TRACEPARENT_LENGTH, 0};
    envelope->traceparent.pointer = message->traceparent;
    envelope->traceparent.length =
        ta_traceparent_format(&effect->trace, &traceparent) == TA_OK ? TA_TRACEPARENT_LENGTH : 0;
    return TA_OK;
}

/* ---- peers --------------------------------------------------------------------------- */

static void reach_free(ta_host *host, peer *p) {
    for (uint32_t k = 0; k < p->reach_count; k++)
        host_free(host, p->reach[k], p->reach_lengths[k]);
    host_free(host, p->reach, sizeof *p->reach * p->reach_capacity);
    host_free(host, p->reach_lengths, sizeof *p->reach_lengths * p->reach_capacity);
    p->reach = NULL;
    p->reach_lengths = NULL;
    p->reach_count = p->reach_capacity = 0;
}

/* The announced systems, copied into `out` (reach fields only); nothing
 * kept if refused. */
static ta_status reach_build(ta_host *host, const ta_string_view *reach, uint32_t n, peer *out) {
    ta_memset(out, 0, sizeof *out);
    for (uint32_t k = 0; k < n; k++)
        if (!ta_address_system_valid(reach[k])) return TA_ERROR_INVALID;
    if (!n) return TA_OK;
    out->reach = (char **)host_alloc(host, sizeof *out->reach * n);
    out->reach_lengths = (uint32_t *)host_alloc(host, sizeof *out->reach_lengths * n);
    out->reach_capacity = n;
    if (!out->reach || !out->reach_lengths) {
        reach_free(host, out);
        return TA_ERROR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < n; k++) {
        if (is_me(host, reach[k])) continue; /* a peer never speaks for us */
        out->reach[out->reach_count] = string_copy(host, reach[k]);
        if (!out->reach[out->reach_count]) {
            reach_free(host, out);
            return TA_ERROR_NO_MEMORY;
        }
        out->reach_lengths[out->reach_count++] = reach[k].length;
    }
    return TA_OK;
}

static void reach_install(ta_host *host, peer *p, peer *built) {
    reach_free(host, p);
    p->reach = built->reach;
    p->reach_lengths = built->reach_lengths;
    p->reach_count = built->reach_count;
    p->reach_capacity = built->reach_capacity;
}

ta_status ta_host_peer_reach(ta_host *host, uint32_t handle, ta_string_list reach) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    peer *p = peer_of(host, handle);
    if (!p) return TA_ERROR_STALE;
    peer built;
    ta_status status = reach_build(host, reach.pointer, reach.count, &built);
    if (status != TA_OK) return status;
    reach_install(host, p, &built);
    return TA_OK;
}

ta_status ta_host_peer_add(ta_host *host, const ta_peer_config *config, uint32_t *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    if (out) *out = 0;
    if (!config || !ta_address_system_valid(config->id) || is_me(host, config->id))
        return TA_ERROR_INVALID;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    /* everything the new peer needs, before the one it replaces goes */
    peer built;
    ta_status status = reach_build(host, config->reach.pointer, config->reach.count, &built);
    if (status != TA_OK) return status;
    if (!host_grow(host, (void **)&router->peers, &router->peer_capacity, router->peer_count + 1,
                   sizeof(peer))) {
        reach_free(host, &built);
        return TA_ERROR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < router->peer_count; i++) /* the newer connection wins */
        if (router->peers[i].live &&
            seq(config->id, router->peers[i].id, router->peers[i].id_length)) {
            status = ta_host_peer_remove(host, peer_handle(i, router->peers[i].generation));
            if (status != TA_OK) {
                reach_free(host, &built);
                return status;
            }
        }
    uint32_t slot = router->peer_count;
    for (uint32_t i = 0; i < router->peer_count; i++)
        if (!router->peers[i].live) {
            slot = i;
            break;
        }
    if (slot + 1 >= TA_PEER_MAX_SLOTS) { /* out of handle bits */
        reach_free(host, &built);
        return TA_ERROR_NO_MEMORY;
    }
    if (slot == router->peer_count) router->peer_count++; /* (room grown above) */
    peer *p = &router->peers[slot];
    uint32_t generation = p->generation; /* dead slots keep their bumped generation */
    ta_memset(p, 0, sizeof *p);
    p->generation = generation;
    p->live = 1;
    p->up = 1;
    p->state = TA_PEER_OPEN;
    p->flags = config->flags;
    ta_memcpy(p->id, config->id.pointer, config->id.length);
    p->id_length = config->id.length;
    p->max_queue = config->max_queue ? config->max_queue : 1000;
    p->ttl = config->queue_ttl;
    reach_install(host, p, &built);
    if (out) *out = peer_handle(slot, generation);
    return TA_OK;
}

ta_status ta_host_peer_up(ta_host *host, uint32_t handle, int up) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    peer *p = peer_of(host, handle);
    if (!p) return TA_ERROR_STALE;
    p->up = up != 0;
    p->state = up ? TA_PEER_OPEN : TA_PEER_RECONNECTING;
    if (up) return flush(host, handle);
    if (!(p->flags & TA_PEER_QUEUE)) return fail_queue(host, handle, TA_ROUTE_ERROR_UNREACHABLE);
    return TA_OK;
}

ta_status ta_host_peer_remove(ta_host *host, uint32_t handle) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    peer *p = peer_of(host, handle);
    if (!p) return TA_ERROR_STALE;
    ta_status status = fail_queue(host, handle, TA_ROUTE_ERROR_UNREACHABLE);
    if (status != TA_OK) return status; /* the rest stay queued: remove it again */
    p = peer_of(host, handle);
    uint32_t generation = p->generation; /* handles of this peer (and older ones) stay stale */
    host_free(host, p->queue, sizeof *p->queue * p->queue_capacity);
    reach_free(host, p);
    struct ta_router *router = host->router;
    for (uint32_t i = 0; i < router->route_count;)
        if (router->routes[i].peer == handle) {
            host_free(host, router->routes[i].system, router->routes[i].length);
            router->routes[i] = router->routes[--router->route_count];
        } else {
            i++;
        }
    for (uint32_t i = 0; i < router->export_count;)
        if (router->exports[i].peer == handle) {
            host_free(host, router->exports[i].name, router->exports[i].name_length);
            host_free(host, router->exports[i].events, router->exports[i].events_length);
            router->exports[i] = router->exports[--router->export_count];
        } else {
            i++;
        }
    ta_memset(p, 0, sizeof *p);
    p->generation = generation + 1; /* a new peer into this slot gets a different handle */
    return TA_OK;
}

ta_status ta_host_route(ta_host *host, ta_string_view system, uint32_t handle) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    if (!ta_address_system_valid(system) || is_me(host, system)) return TA_ERROR_INVALID;
    if (handle && !peer_of(host, handle)) return TA_ERROR_STALE;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < router->route_count; i++)
        if (seq(system, router->routes[i].system, router->routes[i].length)) {
            if (handle) {
                router->routes[i].peer = handle;
            } else {
                host_free(host, router->routes[i].system, router->routes[i].length);
                router->routes[i] = router->routes[--router->route_count];
            }
            return TA_OK;
        }
    if (!handle) return TA_OK;
    if (!host_grow(host, (void **)&router->routes, &router->route_capacity, router->route_count + 1,
                   sizeof(route)))
        return TA_ERROR_NO_MEMORY;
    route *x = &router->routes[router->route_count];
    x->system = string_copy(host, system);
    if (!x->system) return TA_ERROR_NO_MEMORY;
    x->length = system.length;
    x->peer = handle;
    router->route_count++;
    return TA_OK;
}

ta_status ta_host_export(ta_host *host, const ta_export_rule *rule) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t handle = rule->peer;
    ta_string_view name = rule->name, events = rule->events;
    if (!name.length) return TA_ERROR_INVALID;
    if (handle && !peer_of(host, handle)) return TA_ERROR_STALE;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    if (!host_grow(host, (void **)&router->exports, &router->export_capacity,
                   router->export_count + 1, sizeof(exported)))
        return TA_ERROR_NO_MEMORY;
    exported *x = &router->exports[router->export_count];
    x->peer = handle;
    x->name = string_copy(host, name);
    x->events = string_copy(host, events);
    if (!x->name || (events.length && !x->events)) {
        host_free(host, x->name, name.length);
        host_free(host, x->events, events.length);
        return TA_ERROR_NO_MEMORY;
    }
    x->name_length = name.length;
    x->events_length = events.length;
    router->export_count++;
    return TA_OK;
}

int32_t ta_host_deliver(ta_host *host, uint32_t from, const ta_envelope *envelope) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    if (!envelope || envelope->size < sizeof(ta_envelope)) return TA_ERROR_INVALID;
    peer *p = NULL;
    if (from) {
        p = peer_of(host, from);
        if (!p) return TA_ERROR_STALE;
    }
    ta_route_request request;
    request.size = sizeof request;
    request.flags = 0;
    request.from = from;
    ta_status status = ta_host_route_begin(host, envelope, &request);
    if (status != TA_OK) return status;
    if (request.action == TA_ROUTE_LOOKUP) {
        uint64_t session = request.session;
        if (request.name.length) { /* decoded is never longer than encoded */
            char small[256], *name = small;
            if (request.name.length > sizeof small) {
                name = (char *)host_alloc(host, request.name.length);
                if (!name) return TA_ERROR_NO_MEMORY;
            }
            ta_buffer decoded = {name, request.name.length, 0};
            ta_address_name_decode(request.name, &decoded);
            session = ta_host_actor_by_name(host, (ta_string_view){name, (uint32_t)decoded.length});
            if (name != small) host_free(host, name, request.name.length);
        }
        uint32_t si, actor;
        if (!session || host_locate(host, session, &si, &actor) != TA_OK) session = 0;
        ta_host_route_step(&request, session);
    }
    struct ta_router *router = NULL;
    if (request.action == TA_ROUTE_DEDUPLICATE) {
        router = router_of(host);
        if (!router) return TA_ERROR_NO_MEMORY;
        ta_host_route_step(&request, !dedup_seen(router, request.origin, request.key));
    }
    if (request.action == TA_ROUTE_DUPLICATE) {
        if (p) {
            p->received++;
            p->duplicates++;
        }
        return TA_ROUTE_DUPLICATE;
    }
    /* what follows either succeeds or, refused memory, leaves nothing behind:
     * the envelope is remembered (dedup) and counted only once handled */
    uint32_t reason = request.reason, handled = TA_ROUTE_REFUSED;
    if (request.action == TA_ROUTE_LOCAL) {
        status = local_deliver(host, request.session, envelope, &reason);
        if (status != TA_OK) return status;
        if (!reason) handled = TA_ROUTE_DELIVERED;
    } else if (request.action == TA_ROUTE_PEER) {
        ta_envelope forwarded = *envelope;
        forwarded.hops = request.hops;
        char mid[MESSAGE_ID_MAX];
        if (!forwarded.msgid.length)
            forwarded.msgid = new_msgid(host, envelope->origin, mid); /* from host code */
        ta_address to;
        ta_address_parse(envelope->target, &to);
        status = forward(host, &to, &forwarded, &reason);
        if (status != TA_OK) return status;
        if (!reason) handled = TA_ROUTE_RELAYED;
    }
    if (handled == TA_ROUTE_REFUSED) {
        int local = 0;
        status = from ? emit_error(host, from, envelope->origin, envelope->msgid, envelope->sendid,
                                   reason)
                      : local_error(host, envelope->origin, envelope->sendid, &local);
        if (status != TA_OK) return status;
    }
    if (router) dedup_remember(router, request.origin, request.key);
    if (p) p->received++;
    return (int32_t)handled;
}

ta_status ta_host_peer_error(ta_host *host, const ta_peer_error *error) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t from = error->peer, reason = error->reason;
    ta_string_view origin = error->origin, msgid = error->msgid, sendid = error->sendid;
    peer *p = peer_of(host, from);
    if (!p) return TA_ERROR_STALE;
    int handled;
    ta_status status = local_error(host, origin, sendid, &handled);
    if (status != TA_OK || handled) return status;
    /* about an envelope we relayed: back towards its origin */
    ta_address o;
    if (ta_address_parse(origin, &o) != TA_OK) return TA_ERROR_INVALID;
    uint32_t back = route_for(host, o.system);
    if (back && back != from) return emit_error(host, back, origin, msgid, sendid, reason);
    return TA_OK;
}

/* ---- envelopes across a process boundary: CBOR (docs/protocol.md, Envelope) ----------- */

enum { ENVELOPE_CBOR_TAG_ENCODED = 24 }; /* RFC 8949 §3.4.5.1: an encoded CBOR data item */

/* The keys, in the order an envelope is written. */
enum {
    ENVELOPE_KEY_ERROR,
    ENVELOPE_KEY_TARGET,
    ENVELOPE_KEY_NAME,
    ENVELOPE_KEY_DATA,
    ENVELOPE_KEY_SENDID,
    ENVELOPE_KEY_ORIGIN,
    ENVELOPE_KEY_ORIGINTYPE,
    ENVELOPE_KEY_MSGID,
    ENVELOPE_KEY_HOPS,
    ENVELOPE_KEY_DEDUP,
    ENVELOPE_KEY_TRACEPARENT,
    ENVELOPE_KEY_COUNT
};
static const ta_string_view ENVELOPE_KEYS[ENVELOPE_KEY_COUNT] = {
    VIEW("error"), VIEW("target"),     VIEW("name"),  VIEW("data"), VIEW("sendid"),     VIEW("origin"),
    VIEW("origintype"), VIEW("msgid"), VIEW("hops"),  VIEW("dedup"), VIEW("traceparent")};

static uint32_t envelope_key_index(ta_string_view key) {
    for (uint32_t k = 0; k < ENVELOPE_KEY_COUNT; k++)
        if (ENVELOPE_KEYS[k].length == key.length && !ta_memcmp(ENVELOPE_KEYS[k].pointer, key.pointer, key.length))
            return k;
    return TA_NIL;
}

static void envelope_put_text(cbor_output *out, uint32_t key, ta_string_view value) {
    if (!value.length) return;
    cbor_output_text(out, ENVELOPE_KEYS[key]);
    cbor_output_text(out, value);
}

/* The keys of `bytes` into views (views[KEY_DATA]: the value's bytes, from
 * its tag-24 byte string) and *hops; the set of keys seen, 0 if it is not an
 * envelope's map (not CBOR, an unknown or repeated key, a wrong type). */
static uint32_t envelope_fields(ta_bytes bytes, ta_string_view *views, uint32_t *hops) {
    ta_cbor_token tokens[2 * ENVELOPE_KEY_COUNT + 2];
    ta_cbor_document d = {bytes, tokens, sizeof tokens / sizeof tokens[0], 0};
    if (ta_cbor_decode(&d) != TA_OK || tokens[0].kind != TA_CBOR_MAP) return 0;
    uint32_t seen = 0;
    for (uint32_t at = 1, pair = 0; pair < tokens[0].count; pair++) {
        if (tokens[at].kind != TA_CBOR_TEXT) return 0;
        uint32_t key = envelope_key_index(ta_cbor_string(&d, at));
        if (key == TA_NIL || (seen & (1u << key))) return 0;
        seen |= 1u << key;
        at = ta_cbor_next(&d, at);
        const ta_cbor_token *value = &tokens[at];
        if (key == ENVELOPE_KEY_HOPS) {
            if (value->kind != TA_CBOR_UINT || value->value > UINT32_MAX) return 0;
            *hops = (uint32_t)value->value;
        } else if (key == ENVELOPE_KEY_DATA) {
            if (value->kind != TA_CBOR_TAG || value->value != ENVELOPE_CBOR_TAG_ENCODED ||
                value[1].kind != TA_CBOR_BYTES)
                return 0;
            views[key] = ta_cbor_string(&d, at + 1);
        } else {
            if (value->kind != TA_CBOR_TEXT) return 0;
            views[key] = ta_cbor_string(&d, at);
        }
        at = ta_cbor_next(&d, at);
    }
    return seen;
}

ta_status ta_host_envelope_read(ta_host *host, ta_bytes cbor, ta_envelope *out) {
    if (!host || !out) return TA_ERROR_INVALID;
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    ta_memset(out, 0, sizeof *out);
    ta_string_view views[ENVELOPE_KEY_COUNT];
    ta_memset(views, 0, sizeof views);
    uint32_t hops = 0;
    uint32_t seen = envelope_fields(cbor, views, &hops);
    if (!seen || (seen & (1u << ENVELOPE_KEY_ERROR))) return TA_ERROR_INVALID; /* nobody to answer */
    out->size = sizeof *out;
    out->hops = hops;
    out->target = views[ENVELOPE_KEY_TARGET];
    out->name = views[ENVELOPE_KEY_NAME];
    out->sendid = views[ENVELOPE_KEY_SENDID];
    out->origin = views[ENVELOPE_KEY_ORIGIN];
    out->origintype = views[ENVELOPE_KEY_ORIGINTYPE];
    out->msgid = views[ENVELOPE_KEY_MSGID];
    out->deduplication_key = views[ENVELOPE_KEY_DEDUP];
    out->traceparent = views[ENVELOPE_KEY_TRACEPARENT];
    out->datamodel = TA_NIL;
    if (!(seen & (1u << ENVELOPE_KEY_DATA))) return TA_OK;
    /* not decoded: the target's datamodel reads it, a relay forwards it. One
     * well-formed item is checked here (the decoder validates all of it while
     * it counts the tokens it would need; none are kept). */
    ta_bytes wire = {views[ENVELOPE_KEY_DATA].pointer, views[ENVELOPE_KEY_DATA].length};
    ta_cbor_token token;
    ta_cbor_document value = {wire, &token, 1, 0};
    ta_status status = ta_cbor_decode(&value);
    if (wire.length && (status == TA_OK || status == TA_ERROR_NO_MEMORY)) {
        out->wire = wire;
        return TA_OK;
    }
    out->target = (ta_string_view){0, 0}; /* ta_host_deliver refuses it as malformed */
    return TA_ERROR_INVALID;
}

ta_status ta_host_peer_error_read(ta_bytes cbor, ta_peer_error *out) {
    if (!out) return TA_ERROR_INVALID;
    ta_string_view views[ENVELOPE_KEY_COUNT];
    ta_memset(views, 0, sizeof views);
    uint32_t hops = 0;
    uint32_t seen = envelope_fields(cbor, views, &hops);
    uint32_t allowed = (1u << ENVELOPE_KEY_ERROR) | (1u << ENVELOPE_KEY_SENDID) | (1u << ENVELOPE_KEY_ORIGIN) | (1u << ENVELOPE_KEY_MSGID);
    if (!(seen & (1u << ENVELOPE_KEY_ERROR)) || (seen & ~allowed) || !views[ENVELOPE_KEY_ORIGIN].length)
        return TA_ERROR_INVALID;
    uint32_t reason = 0;
    for (uint32_t r = TA_ROUTE_ERROR_UNKNOWN_TARGET; r <= TA_ROUTE_ERROR_MALFORMED; r++) {
        ta_string_view name = ta_host_route_reason(r);
        if (name.length == views[ENVELOPE_KEY_ERROR].length &&
            !ta_memcmp(name.pointer, views[ENVELOPE_KEY_ERROR].pointer, name.length))
            reason = r;
    }
    if (!reason) return TA_ERROR_INVALID;
    out->reason = reason;
    out->origin = views[ENVELOPE_KEY_ORIGIN];
    out->msgid = views[ENVELOPE_KEY_MSGID];
    out->sendid = views[ENVELOPE_KEY_SENDID];
    return TA_OK;
}

ta_status ta_host_envelope(ta_host *host, const ta_host_effect *effect, ta_buffer *out) {
    if (!host || !effect || !out ||
        (effect->kind != TA_HOST_EFFECT_PEER && effect->kind != TA_HOST_EFFECT_PEER_ERROR))
        return TA_ERROR_INVALID;
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    size_t capacity = out->pointer ? out->capacity : 0;
    cbor_output o = {(unsigned char *)out->pointer, capacity, 0};
    if (effect->kind == TA_HOST_EFFECT_PEER_ERROR) {
        cbor_output_head(&o, CBOR_MAJOR_MAP, 1u + !!effect->sendid.length +
                                                 !!effect->auxiliary.length + !!effect->msgid.length);
        envelope_put_text(&o, ENVELOPE_KEY_ERROR, ta_host_route_reason(effect->token));
        envelope_put_text(&o, ENVELOPE_KEY_SENDID, effect->sendid);
        envelope_put_text(&o, ENVELOPE_KEY_ORIGIN, effect->auxiliary);
        envelope_put_text(&o, ENVELOPE_KEY_MSGID, effect->msgid);
    } else {
        /* the value first: its length is the byte string's head. A relayed
         * value is the bytes it arrived as; otherwise the datamodel writes it */
        const ta_datamodel_binding *binding =
            !effect->wire.length && effect->payload &&
                    effect->datamodel < host->config.datamodel_count
                ? &host->datamodels[effect->datamodel]
                : (const ta_datamodel_binding *)0;
        int has_data = binding || effect->wire.length;
        size_t value = effect->wire.length;
        if (binding) {
            if (!binding->vtable->to_wire) return TA_ERROR_INVALID;
            ta_buffer probe = {(void *)0, 0, 0};
            ta_status status = binding->vtable->to_wire(binding->datamodel, effect->payload, &probe);
            if (status != TA_OK && status != TA_ERROR_NO_MEMORY) return status;
            value = probe.length;
        }
        char traceparent[TA_TRACEPARENT_LENGTH];
        ta_buffer written = {traceparent, sizeof traceparent, 0};
        ta_string_view trace = {traceparent, 0};
        if (ta_traceparent_format(&effect->trace, &written) == TA_OK)
            trace.length = (uint32_t)written.length;
        ta_string_view origintype = effect->origintype; /* the default stays off the wire */
        if (origintype.length == SCXML_EVENT_PROCESSOR.length &&
            !ta_memcmp(origintype.pointer, SCXML_EVENT_PROCESSOR.pointer, origintype.length))
            origintype.length = 0;
        uint32_t pairs = 2u + !!has_data + !!effect->sendid.length + !!effect->auxiliary.length +
                         !!origintype.length + !!effect->msgid.length + !!effect->flags +
                         !!effect->deduplication_key.length + !!trace.length;
        cbor_output_head(&o, CBOR_MAJOR_MAP, pairs);
        envelope_put_text(&o, ENVELOPE_KEY_TARGET, effect->target);
        envelope_put_text(&o, ENVELOPE_KEY_NAME, effect->event);
        if (has_data) {
            cbor_output_text(&o, ENVELOPE_KEYS[ENVELOPE_KEY_DATA]);
            cbor_output_head(&o, CBOR_MAJOR_TAG, ENVELOPE_CBOR_TAG_ENCODED);
            cbor_output_head(&o, CBOR_MAJOR_BYTES, value);
            if (o.length + value <= o.capacity) {
                if (!binding) {
                    ta_memcpy(o.bytes + o.length, effect->wire.pointer, value);
                } else {
                    ta_buffer data = {o.bytes + o.length, value, 0};
                    ta_status status =
                        binding->vtable->to_wire(binding->datamodel, effect->payload, &data);
                    if (status != TA_OK) return TA_ERROR_INVALID;
                }
            }
            o.length += value;
        }
        envelope_put_text(&o, ENVELOPE_KEY_SENDID, effect->sendid);
        envelope_put_text(&o, ENVELOPE_KEY_ORIGIN, effect->auxiliary);
        envelope_put_text(&o, ENVELOPE_KEY_ORIGINTYPE, origintype);
        envelope_put_text(&o, ENVELOPE_KEY_MSGID, effect->msgid);
        if (effect->flags) {
            cbor_output_text(&o, ENVELOPE_KEYS[ENVELOPE_KEY_HOPS]);
            cbor_output_uint(&o, effect->flags);
        }
        envelope_put_text(&o, ENVELOPE_KEY_DEDUP, effect->deduplication_key);
        envelope_put_text(&o, ENVELOPE_KEY_TRACEPARENT, trace);
    }
    out->length = o.length;
    return o.length <= capacity ? TA_OK : TA_ERROR_NO_MEMORY;
}

void ta_host_peer_round_trip_time(ta_host *host, uint32_t handle, uint32_t ms) {
    peer *p = peer_of(host, handle);
    if (p) p->round_trip_time = ms;
}

ta_status ta_host_peer_status(const ta_host *host, uint32_t handle, ta_peer_status *out) {
    peer *p = peer_of((ta_host *)host, handle);
    if (!p) return TA_ERROR_STALE;
    ta_peer_status z;
    ta_memset(&z, 0, sizeof z);
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
    ta_memcpy(out, &z, n);
    return TA_OK;
}

void ta_router_free(ta_host *host) {
    struct ta_router *router = host->router;
    if (!router) return;
    for (uint32_t i = 0; i < router->peer_count; i++)
        if (router->peers[i].live) {
            peer *p = &router->peers[i];
            while (p->queue_count)
                dequeue(host, p);
            host_free(host, p->queue, sizeof *p->queue * p->queue_capacity);
            reach_free(host, p);
        }
    for (uint32_t i = 0; i < router->route_count; i++)
        host_free(host, router->routes[i].system, router->routes[i].length);
    for (uint32_t i = 0; i < router->export_count; i++) {
        host_free(host, router->exports[i].name, router->exports[i].name_length);
        host_free(host, router->exports[i].events, router->exports[i].events_length);
    }
    host_free(host, router->peers, sizeof(peer) * router->peer_capacity);
    host_free(host, router->routes, sizeof(route) * router->route_capacity);
    host_free(host, router->exports, sizeof(exported) * router->export_capacity);
    host_free(host, router, sizeof *router);
    host->router = NULL;
}

/* the dedup window, for snapshots: count, head, hashes */
size_t ta_router_snapshot(const ta_host *host, unsigned char *out, size_t capacity) {
    const struct ta_router *router = host->router;
    uint32_t n = router ? router->seen_count : 0;
    size_t need = 4 + (size_t)n * 8;
    if (out && capacity >= need) {
        ta_memcpy(out, &n, 4);
        for (uint32_t i = 0; i < n; i++)
            ta_memcpy(out + 4 + (size_t)i * 8,
                      &router->seen[(router->seen_head + i) % DEDUPLICATION_WINDOW], 8);
    }
    return need;
}

ta_status ta_router_restore(ta_host *host, const unsigned char *p, size_t length) {
    uint32_t n;
    if (length < 4) return TA_ERROR_INVALID;
    ta_memcpy(&n, p, 4);
    if (n > DEDUPLICATION_WINDOW || length < 4 + (size_t)n * 8) return TA_ERROR_INVALID;
    if (!n) return TA_OK;
    struct ta_router *router = router_of(host);
    if (!router) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < n; i++)
        ta_memcpy(&router->seen[i], p + 4 + (size_t)i * 8, 8);
    router->seen_head = 0;
    router->seen_count = n;
    return TA_OK;
}
