/* tinyactors/host.h — many systems as one actor system: the policy every embedder
 * would otherwise rebuild, in one place. All its memory, the segments' blocks
 * included, comes from the ta_allocator of its environment (docs/adr/0005).
 *
 * A ta_host owns *segments*: one ta_system per image. All segments share one
 * base URI and one set of datamodel bindings; segment k creates sessions
 * k * 2^32 + 1, ... so a session id names an actor host-wide, for its whole
 * life — also after it migrated to another segment.
 *
 *   versions   ta_host_add_image() adds a segment; its charts become the
 *              current version of their chart names (<scxml name>). New actors
 *              of a name start in its current version. A segment whose charts
 *              all have newer versions and that has no actors left is freed
 *              (TA_HOST_EFFECT_SEGMENT_FREED: the host may free the image bytes).
 *   routing    SCXML sends between segments (same system id) are delivered
 *              inside the host layer — no host round trip. Sends to other
 *              systems go through the router (peers, routes, relaying,
 *              origin checks, policy, dedup; docs/protocol.md) and come out
 *              as TA_HOST_EFFECT_PEER effects for the transport; undeliverable
 *              sends raise error.communication in the sender. Sends to host
 *              I/O processors come out as TA_EFFECT_SEND effects.
 *   migration  ta_host_migrate() starts a job that moves actors of older
 *              versions to the current one at stable points (export ->
 *              translate -> adopt; error.platform.upgrade), advanced inside
 *              ta_host_run in bounded steps: busy actors are retried,
 *              hibernated ones are woken first (TA_HOST_MIGRATE_WAKE: a
 *              TA_EFFECT_WAKE effect asks the host for the blob) or wait;
 *              actors with running invocations wait until those finish
 *              (a session tree moves unchanged, but is not translated);
 *              a timeout (system ticks) ends the job with the stragglers.
 *   hibernation ta_host_sweep() runs the CLOCK sweep over all segments;
 *              blobs and their storage stay with the host (ta_host_hibernate,
 *              TA_EFFECT_WAKE, ta_host_wake).
 *   growth     segments are relocated into bigger blocks when a run or a call
 *              needs it (TA_RUN_EXHAUSTED / TA_ERROR_NO_MEMORY).
 *   memory     every block comes from config.allocator and goes back to it
 *              (ta_host_free returns all of them). When it refuses one, a
 *              call returns TA_ERROR_NO_MEMORY and leaves the host as it
 *              was; ta_host_run returns TA_RUN_EXHAUSTED (detail: the
 *              segment's TA_POOL_* that could not grow, or TA_POOL_NONE for
 *              the host's own memory) and the next run goes on where it
 *              stopped: nothing is lost or done twice.
 *
 * Calls are synchronous and budgeted like ta_system_run; outcomes are effects
 * (ta_host_effects), borrowed until ta_host_effects_ack, the next ta_host_run
 * or ta_host_free: calls in between that make more effects (ta_host_deliver
 * answering a peer, ta_host_peer_up flushing a queue, ...) leave the borrowed
 * records and their strings where they are; the new effects come with the
 * next ta_host_effects. Payloads passed in are moved on success and stay the caller's
 * on failure. No callbacks besides the datamodel vtables.
 */
#ifndef TINYACTORS_HOST_H
#define TINYACTORS_HOST_H

#include <tinyactors/allocator.h>
#include <tinyactors/otlp.h>
#include <tinyactors/system.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_host ta_host;

/* A new host. `config` is CBOR (docs/host-config.md): the system id, the
 * capacities of each segment, the host's I/O processors, the OTLP encoder
 * (docs/adr/0009 in the repository root, decision 6), and the limits of the
 * host; it is read during the call and nothing points into it afterwards.
 * `environment` holds what cannot be data: the allocator, the datamodels and
 * a trace policy (borrowed for as long as the host lives, except the policy,
 * which is copied).
 * TA_ERROR_INVALID: the config is not one the host takes; when
 * environment->error is set, the reply of a refused request is written there
 * ({"error": {"code", "message", "data": {"reason", "path"}}},
 * docs/requests.md). TA_ERROR_NO_MEMORY: the allocator refused. */
typedef struct ta_host_environment {
    uint32_t size;
    uint32_t datamodel_count;               /* at most 8 */
    const ta_datamodel_binding *datamodels; /* shared by every segment */
    ta_allocator allocator; /* required: every block the host owns (ta_runtime_malloc() is
                               the C library's); requests are aligned to at most 8 */
    const ta_trace_policy *trace_policy; /* what each segment traces at first (NULL:
                                            ta_trace_policy_default); copied */
    ta_buffer *error;                    /* optional: why the config was refused */
} ta_host_environment;
TA_API ta_status ta_host_create(ta_bytes config, const ta_host_environment *environment,
                                ta_host **out);
TA_API void ta_host_free(ta_host *h);

/* ---- segments and versions ----------------------------------------------- */
/* A new segment running `image` (the bytes must stay valid until
 * TA_HOST_EFFECT_SEGMENT_FREED for it, or ta_host_free). */
TA_API ta_status ta_host_add_image(ta_host *h, const ta_image *image, uint32_t *segment);
/* A chart in the host: the segment that runs it and its index there. */
typedef struct ta_host_chart_ref {
    uint32_t segment;
    uint32_t chart;
} ta_host_chart_ref;
/* The current version of chart `name` (<scxml name>, else its source name). */
TA_API ta_status ta_host_chart(const ta_host *h, ta_string_view name, ta_host_chart_ref *out);
typedef struct ta_host_segment_info {
    uint32_t size;
    uint32_t live;    /* 0 once freed */
    uint32_t actors;  /* actors in it (including adopted ones) */
    uint32_t current; /* charts of it that are the current version of their name */
    const ta_image *image;
} ta_host_segment_info;
TA_API uint32_t ta_host_segment_count(const ta_host *h); /* segments ever added */
TA_API ta_status ta_host_segment_info_get(const ta_host *h, uint32_t segment,
                                          ta_host_segment_info *out);
/* The ta_system of a segment, for inspection and per-system calls (borrowed:
 * valid until the next ta_host call that can grow or free it). */
TA_API ta_system *ta_host_system(ta_host *h, uint32_t segment);

/* ---- system images ------------------------------------------------------------ */
/* Spawn the actors a system image declares (<system><actor name chart/>) in
 * `segment` (grown first so all of them fit). */
TA_API ta_status ta_host_boot(ta_host *h, uint32_t segment);
/* The session of a live actor spawned with `name` (boot actors carry their
 * manifest name), newest segment first; 0 if none. */
TA_API uint64_t ta_host_actor_by_name(const ta_host *h, ta_string_view name);

/* ---- actors (by session id, host-wide) -------------------------------------- */
/* During a run. A datamodel the host calls while ta_host_run runs (an
 * expression of a binding's own datamodel, say) may call ta_host_spawn,
 * ta_host_send, ta_host_send_many and ta_host_terminate. They take the paths
 * of an internal <invoke>, <send> and cancel: no queue of their own, no copy,
 * and nothing grows, so a full pool is TA_ERROR_NO_MEMORY with nothing
 * changed (a payload stays the caller's). When the input is processed:
 *   - an event for the actor whose expression sent it: at the end of the
 *     macrostep it is in (its next stable point), never inside it;
 *   - an event for another actor, or a spawned actor: at the back of its
 *     segment's run queue, later in this run if the budget allows, else in
 *     the next one (the run returns TA_RUN_YIELDED);
 *   - a terminated actor exits at its next microstep boundary, as an
 *     invoked child its parent cancels does (for the actor whose expression
 *     terminated it: after the microstep it is in; the rest of the
 *     macrostep does not run).
 * Lookups (ta_host_chart, ta_host_locate, ta_host_actor_by_name,
 * ta_host_segment_count) answer as usual. Every other call is refused:
 * TA_ERROR_STATE, an empty answer or nothing done (ta_host_effects returns
 * 0; ta_host_effects_ack, ta_host_advance_time do nothing; ta_host_sweep
 * returns 0; ta_host_run returns TA_RUN_FAULT with TA_ERROR_STATE). */

/* Spawn in (spawn->segment, spawn->chart) — usually the current version
 * (ta_host_chart). */
TA_API ta_status ta_host_spawn(ta_host *h, const ta_spawn *spawn, uint64_t *session);
/* Where a session lives now: its segment and its actor handle there. */
typedef struct ta_host_location {
    uint32_t segment;
    uint32_t actor;
} ta_host_location;
TA_API ta_status ta_host_locate(const ta_host *h, uint64_t session, ta_host_location *out);
/* An event for the session event->session (event->target is ignored). */
TA_API ta_status ta_host_send(ta_host *h, const ta_event_in *event);
/* Batch (runs of events for the same segment go through ta_system_send_many):
 * sends in order and stops at the first failure, returning the length of the
 * accepted prefix; events[returned] failed and its payload stays the caller's. */
TA_API uint32_t ta_host_send_many(ta_host *h, const ta_event_in *events, uint32_t n);
TA_API ta_status ta_host_terminate(ta_host *h, uint64_t session);
/* Answer a TA_EFFECT_REQUEST of segment resume->segment (see ta_system_resume). */
TA_API ta_status ta_host_resume(ta_host *h, const ta_resume *resume);

/* ---- running ------------------------------------------------------------------ */
/* The host's clocks: `monotonic` drives timers, deadlines and TTLs (the
 * unit of <send delay>, as ta_system_advance_time); `wall_ns` is the time
 * of day, nanoseconds since the Unix epoch, that span times are
 * interpolated from (0: unknown). Neither goes back. */
typedef struct ta_host_time {
    ta_time monotonic;
    uint64_t wall_ns;
} ta_host_time;
/* Move every segment's clock; queued envelopes past their TTL fail (an
 * expiry the allocator leaves no room to report is reported by the next
 * ta_host_run). With an encoder, the trace records written since the last
 * call are stamped between its wall time and this one: call it before each
 * run. */
TA_API void ta_host_advance_time(ta_host *h, ta_host_time now);
/* Run segments with work for up to `budget` fuel in total, deliver sends
 * between them, advance migration jobs, free retired segments. status:
 * TA_RUN_IDLE / TA_RUN_YIELDED / TA_RUN_EFFECTS_FULL / TA_RUN_FAULT /
 * TA_RUN_EXHAUSTED (the allocator refused memory: run again once it has
 * some; detail as for ta_system_run, TA_POOL_NONE for the host's own) /
 * TA_RUN_EFFECTS (the run stopped right after a send to a
 * TA_IO_PROCESSOR_SYNC processor, the last effect of its segment: answer it
 * — ta_host_send of events or of TA_EVENT_IN_ERROR_COMMUNICATION to the
 * sender — and run again; the sender goes on only then);
 * next_deadline: the earliest timer of all segments. */
TA_API ta_run ta_host_run(ta_host *h, uint32_t budget);

enum {
    /* TA_EFFECT_SEND .. TA_EFFECT_WAKE (1..8) as in tinyactors/system.h, plus: */
    TA_HOST_EFFECT_MIGRATED = 16, /* session moved to (segment, chart); job */
    TA_HOST_EFFECT_JOB_DONE = 17, /* job ended: migrated, failed, pending (ta_host_job_pending) */
    TA_HOST_EFFECT_SEGMENT_FREED = 18, /* segment retired: its image may be freed */
    /* the router (docs/protocol.md): */
    TA_HOST_EFFECT_PEER = 19,      /* send an envelope to `peer`: target, event, sendid, payload,
                                  aux = origin, msgid, hops (in `flags`) */
    TA_HOST_EFFECT_PEER_ERROR = 20, /* send an `err` to `peer`: aux = origin, msgid, sendid,
                                token = reason */
    /* the encoder (the config's "telemetry"): a request is ready, because the
     * staging is full or flush_interval passed since its first record; take
     * it with ta_host_traces (ta_host_logs). At most one of each kind is
     * outstanding: the next comes after a take. */
    TA_HOST_EFFECT_TRACES = 21,
    TA_HOST_EFFECT_LOGS = 22
    /* TA_EFFECT_CHANGED (9, tinyactors/system.h): a session subscribed with
     * the actor.subscribe request finished a macrostep in this run; at most
     * one per session per run, and none for other sessions. Read its state
     * or context with a request after the run. */
};
typedef struct ta_host_effect {
    uint32_t size;
    uint32_t kind;
    uint32_t segment;
    uint32_t actor;        /* handle in the segment (system effects) */
    uint64_t session;      /* the actor's session id (0: none) */
    uint64_t root;         /* the session of its host-spawned ancestor (attribution) */
    uint32_t token;        /* request / invoke token (per segment) */
    uint32_t io_processor; /* TA_EFFECT_SEND: index in the config's ioProcessors */
    uint32_t flags;        /* PEER: hops */
    uint32_t job;
    ta_string_view target, event, sendid, auxiliary; /* as in ta_effect; MIGRATED: event = chart
                                          name; PEER, PEER_ERR: aux = origin */
    ta_payload payload;                              /* borrowed */
    ta_time delay_hint;
    uint32_t chart;                     /* MIGRATED: chart index in `segment` */
    uint32_t migrated, failed, pending; /* JOB_DONE */
    uint32_t peer;                      /* PEER, PEER_ERR: the peer handle */
    uint32_t datamodel;                 /* PEER: datamodel index of payload (TA_NIL: none) */
    ta_string_view msgid;               /* PEER, PEER_ERR */
    ta_string_view origintype;          /* PEER: the envelope's origintype (may be empty) */
    ta_string_view deduplication_key;   /* PEER: the envelope's dedup key (may be empty) */
    ta_trace_context trace; /* as in ta_effect; PEER: the envelope's traceparent (format it
                               with ta_traceparent_format for the wire) */
} ta_host_effect;
TA_API uint32_t ta_host_effects(ta_host *h, const ta_host_effect **out);
TA_API void ta_host_effects_ack(ta_host *h);

/* A field of effect `index` of the batch ta_host_effects published, so that a
 * binding reads effects without knowing ta_host_effect's layout. Numbers:
 * KIND .. PEER; text: TARGET .. DEDUPLICATION_KEY. An index past the batch, a field
 * the effect kind does not set, or a text field asked as a number (or the
 * reverse) answers 0 or an empty view. Text is borrowed like the effect
 * (until the ack, the next run or ta_host_free); TRACEPARENT's text is
 * written for the call and lasts until the next ta_host_effect_text.
 *
 * A send to a service the host runs (an SCXML <send target="#_<invokeid>">
 * to an external invocation) is a TA_EFFECT_SEND with IO_PROCESSOR
 * TA_NIL and TOKEN the invocation's token: the TOKEN of the
 * TA_EFFECT_INVOKE that started the service, in the same SEGMENT. A binding
 * keys its services by (segment, token); it never matches invoke ids. */
enum {
    TA_EFFECT_FIELD_KIND = 1,
    TA_EFFECT_FIELD_SEGMENT = 2,
    TA_EFFECT_FIELD_ACTOR = 3,
    TA_EFFECT_FIELD_SESSION = 4,
    TA_EFFECT_FIELD_ROOT = 5,
    TA_EFFECT_FIELD_TOKEN = 6,
    TA_EFFECT_FIELD_IO_PROCESSOR = 7, /* TA_NIL: a send to an invoked service */
    TA_EFFECT_FIELD_FLAGS = 8,
    TA_EFFECT_FIELD_JOB = 9,
    TA_EFFECT_FIELD_PAYLOAD = 10, /* the payload handle (ta_payload) */
    TA_EFFECT_FIELD_DATAMODEL = 11,
    TA_EFFECT_FIELD_DELAY_HINT = 12,
    TA_EFFECT_FIELD_CHART = 13,
    TA_EFFECT_FIELD_MIGRATED = 14,
    TA_EFFECT_FIELD_FAILED = 15,
    TA_EFFECT_FIELD_PENDING = 16,
    TA_EFFECT_FIELD_PEER = 17,
    TA_EFFECT_FIELD_TARGET = 32,
    TA_EFFECT_FIELD_EVENT = 33,
    TA_EFFECT_FIELD_SENDID = 34,
    TA_EFFECT_FIELD_AUXILIARY = 35,
    TA_EFFECT_FIELD_MSGID = 36,
    TA_EFFECT_FIELD_TRACEPARENT = 37, /* W3C text of `trace`; empty when untraced */
    TA_EFFECT_FIELD_ORIGINTYPE = 38,
    TA_EFFECT_FIELD_DEDUPLICATION_KEY = 39
};
TA_API uint64_t ta_host_effect_number(const ta_host *h, uint32_t index, uint32_t field);
TA_API ta_string_view ta_host_effect_text(ta_host *h, uint32_t index, uint32_t field);

/* ---- telemetry ------------------------------------------------------------------ */
/* The ExportTraceServiceRequest (`/v1/traces`, application/x-protobuf) of
 * the spans staged so far, written into `out` (length 0 when there are
 * none); the trace records written since the last run are read first. The
 * spans leave the staging only on TA_OK: TA_ERROR_NO_MEMORY means nothing
 * changed and out->length is what the request needs. Posting it, retrying
 * and capping what waits are the caller's. TA_ERROR_STATE during a run or
 * without an encoder (the config's "telemetry"). ta_host_logs: the same for the
 * ExportLogsServiceRequest (`/v1/logs`). */
TA_API ta_status ta_host_traces(ta_host *h, ta_buffer *out);
TA_API ta_status ta_host_logs(ta_host *h, ta_buffer *out);

/* ---- the router: other systems (docs/protocol.md, Routing) -------------------------
 * A peer is another system as seen through one transport connection (a link,
 * a store link, a broker subject...). The host owns peers as integer handles
 * that pack their slot and a generation: removing a peer (ta_host_peer_remove)
 * makes its handle stale (TA_ERROR_STALE), even when a newer peer reuses the
 * slot; the transport (host code, any language) moves envelopes: inbound with
 * ta_host_deliver, outbound from TA_HOST_EFFECT_PEER effects. Routes map system
 * ids to peers: configured (ta_host_route) and announced (the systems a peer
 * reaches, ta_host_peer_add / ta_host_peer_reach). The host never does I/O. */

/* An envelope (docs/protocol.md, Envelope): one SCXML event on its way to one
 * target. Views are borrowed for the call. */
typedef struct ta_envelope {
    uint32_t size;
    uint32_t hops;             /* relays passed so far */
    ta_string_view target;     /* address */
    ta_string_view name;       /* event name */
    ta_payload data;           /* datamodel payload (moved on success; the caller's on failure) */
    uint32_t datamodel;        /* datamodel index of `data` (ta_host_environment.datamodels) */
    uint32_t trace_flags;      /* local to this process: the sender's detail class (ta_event_in) */
    ta_string_view sendid;     /* the sender's sendid (may be empty) */
    ta_string_view origin;     /* the sending session's address */
    ta_string_view origintype; /* empty: the SCXML event processor */
    ta_string_view msgid;      /* unique per origin: errors, acks and dedup refer to it */
    ta_string_view deduplication_key; /* optional key for duplicate detection instead of msgid */
    ta_string_view traceparent;       /* W3C: the sending step's trace (empty: none), kept across
                                         systems and relays */
} ta_envelope;

/* Peer flags. */
enum {
    TA_PEER_TRUSTED = 1u << 0, /* policy: everything allowed (default: only exported names) */
    TA_PEER_QUEUE = 1u << 1    /* while down: queue envelopes up to queue_ttl (else fail at once) */
};
typedef struct ta_peer_config {
    uint32_t size;
    uint32_t flags;       /* TA_PEER_* */
    ta_string_view id;    /* the peer's own system id */
    ta_string_list reach; /* further systems reachable through it (announced) */
    uint32_t max_queue;   /* TA_PEER_QUEUE: envelopes kept while down (default 1000) */
    ta_time queue_ttl;  /* TA_PEER_QUEUE: ticks an envelope may wait (0 = forever) */
} ta_peer_config;

/* Reasons (docs/protocol.md, Errors). */
enum {
    TA_ROUTE_ERROR_UNKNOWN_TARGET = 1,
    TA_ROUTE_ERROR_UNREACHABLE = 2,
    TA_ROUTE_ERROR_DENIED = 3,
    TA_ROUTE_ERROR_EXPIRED = 4,
    TA_ROUTE_ERROR_HOPS = 5,
    TA_ROUTE_ERROR_MALFORMED = 6
};
#define TA_ROUTE_MAX_HOPS 8u

/* External storage driver. No segments or callbacks are needed: begin validates
 * and routes, then the caller answers LOOKUP (session/name exists) and DEDUP
 * (atomically insert (origin,key), true if inserted) with route_step. All views
 * borrow the envelope/host until the request finishes. Storage and delivery must
 * commit together; aborting the transaction also aborts the request.
 * PEER carries the next hop; an empty handle with a peer_id is a durable route
 * waiting for hello. LOCAL carries a resolved session, ready for an inbox.
 * PEERS (only with TA_ROUTE_ASK_PEERS): the next hop depends on the peers the
 * caller has not loaded yet; add them (ta_host_peer_add) and begin again
 * without the flag. Local sends never need them.
 * The resident host drives this same protocol with its own tables. */
enum {
    TA_ROUTE_LOOKUP = 4,
    TA_ROUTE_DEDUPLICATE = 5,
    TA_ROUTE_LOCAL = 6,
    TA_ROUTE_PEER = 7,
    TA_ROUTE_PEERS = 8
};
enum {
    TA_ROUTE_DURABLE = 1u << 0,          /* unknown remote systems wait under their own id */
    TA_ROUTE_DEDUPLICATE_HOST = 1u << 1, /* deduplicate host-code input too */
    TA_ROUTE_LOCAL_ONLY = 1u << 2,       /* endpoint transport: do not accept transit traffic */
    TA_ROUTE_ASK_PEERS = 1u << 3         /* host without its peers yet: PEERS before a next hop */
};
typedef struct ta_route_request {
    uint32_t size;
    uint32_t action, reason, peer;
    uint32_t hops;
    uint32_t flags; /* in: TA_ROUTE_* for ta_host_route_begin */
    uint32_t from;  /* in: the peer the envelope came from (0: this host's own code) */
    uint32_t pad;
    uint64_t session;
    ta_string_view system, name, peer_id, origin, key; /* name is percent encoded */
} ta_route_request;
/* Set request->size, ->flags and ->from first; the rest is written. */
TA_API ta_status ta_host_route_begin(ta_host *h, const ta_envelope *envelope,
                                     ta_route_request *request);
/* LOOKUP: session=0 means missing. DEDUP: session=0 means already present. */
TA_API ta_status ta_host_route_step(ta_route_request *request, uint64_t session);
TA_API ta_string_view ta_host_route_reason(uint32_t reason);
/* Construct the SCXML error event (the caller supplies target/data). */
TA_API void ta_host_error_event(ta_string_view sendid, ta_event_in *event);
/* Resolve an error's local origin session; REFUSED if foreign/malformed/a name.
 * The caller answers LOOKUP as for route_begin; errors are never deduplicated. */
TA_API ta_status ta_host_error_route(ta_host *h, ta_string_view origin, ta_route_request *request);

/* Stable outbound identity. revision>0: durable processing number and effect
 * index; revision=0: host counter. Identity strings borrow this result;
 * target/name/explicit sendid borrow the effect. `sendid` includes a generated
 * fallback for transports whose outbox requires one; envelope.sendid preserves
 * the SCXML sendid, including its absence.
 * Persist the revision with the actor, in the same transaction as its outbox.
 * Set size, revision and index first; the rest is written. */
typedef struct ta_host_message {
    uint32_t size;
    uint32_t index;    /* in: the effect's index (with a revision) */
    uint64_t revision; /* in: durable processing number; 0: a host counter */
    ta_envelope envelope;
    ta_string_view key;
    ta_string_view sendid;
    char storage[768];
    char traceparent[TA_TRACEPARENT_LENGTH]; /* envelope.traceparent's bytes */
    uint32_t pad2;
} ta_host_message;
TA_API ta_status ta_host_message_init(ta_host *h, const ta_effect *effect,
                                      ta_host_message *message);

/* Add a peer (connected). Its id and reach become routes to it; an id that
 * already has a peer replaces it (the newer connection wins). */
TA_API ta_status ta_host_peer_add(ta_host *h, const ta_peer_config *config, uint32_t *peer);
/* The transport lost the connection (TA_PEER_QUEUE peers keep their routes and
 * queue; others fail at once) or got it back (queued envelopes come out as
 * TA_HOST_EFFECT_PEER effects, in order). TA_ERROR_NO_MEMORY: the envelopes
 * not flushed or failed yet stay queued; call it again. */
TA_API ta_status ta_host_peer_up(ta_host *h, uint32_t peer, int up);
/* Remove a peer and its routes; queued envelopes fail (unreachable).
 * TA_ERROR_NO_MEMORY: the peer stays, with the envelopes not failed yet. */
TA_API ta_status ta_host_peer_remove(ta_host *h, uint32_t peer);
/* Replace what a peer announced it reaches. */
TA_API ta_status ta_host_peer_reach(ta_host *h, uint32_t peer, ta_string_list reach);
/* A configured route: envelopes for `system` go to `peer` (0: remove). */
TA_API ta_status ta_host_route(ta_host *h, ta_string_view system, uint32_t peer);
/* Policy for untrusted peers: export a local @name (or "*" for every name and
 * session) to `peer` (0: to every peer), optionally limited to event names
 * matching `events` (SCXML event descriptors; empty: all). */
typedef struct ta_export_rule {
    uint32_t peer;
    uint32_t pad;
    ta_string_view name;
    ta_string_view events;
} ta_export_rule;
TA_API ta_status ta_host_export(ta_host *h, const ta_export_rule *rule);

/* An envelope from `peer` (0: from this host's own code, no checks):
 * origin validated against the peer's announced systems, policy applied,
 * duplicates dropped (TA_ROUTE_DUPLICATE), then delivered locally or relayed
 * to another peer. Returns the outcome, TA_ROUTE_* (0 or more), or an error
 * (negative). A refusal is reported both as TA_ROUTE_REFUSED and as a
 * TA_HOST_EFFECT_PEER_ERROR effect addressed back to the peer, so the
 * sender's system can turn it into error.communication. TA_ERROR_NO_MEMORY:
 * nothing was done or remembered (delivering it again is not a duplicate). */
enum { TA_ROUTE_DELIVERED = 0, TA_ROUTE_RELAYED = 1, TA_ROUTE_DUPLICATE = 2, TA_ROUTE_REFUSED = 3 };
TA_API int32_t ta_host_deliver(ta_host *h, uint32_t peer, const ta_envelope *envelope);
/* An `err` from a peer about an envelope this host sent: the origin session
 * gets error.communication (with the sendid); relayed errors go back further. */
typedef struct ta_peer_error {
    uint32_t peer;   /* the peer it came from */
    uint32_t reason; /* TA_ROUTE_ERROR_* */
    ta_string_view origin, msgid, sendid;
} ta_peer_error;
TA_API ta_status ta_host_peer_error(ta_host *h, const ta_peer_error *error);

/* Envelopes across a process boundary (docs/protocol.md, Envelope): CBOR.
 * Within a process, a transport fills ta_envelope and ta_peer_error from the
 * effects directly; no bytes are written. Across one, the sender writes each
 * TA_HOST_EFFECT_PEER and TA_HOST_EFFECT_PEER_ERROR effect with
 * ta_host_envelope and sends the bytes to the effect's peer; the receiver
 * reads an envelope with ta_host_envelope_read and passes it to
 * ta_host_deliver, and an error envelope with ta_host_peer_error_read (it
 * sets `peer`) and passes it to ta_host_peer_error.
 *
 * ta_host_envelope: the effect's envelope (its data written by the
 * datamodel's to_wire) or error envelope, into `out`. The effect keeps its
 * payload, borrowed until the ack. TA_ERROR_NO_MEMORY: out->length is what
 * it needs (the bytes in `out` are not an envelope); TA_ERROR_INVALID: another effect kind,
 * or data that is not a neutral value; TA_ERROR_STATE during a run. */
TA_API ta_status ta_host_envelope(ta_host *h, const ta_host_effect *effect, ta_buffer *out);
/* An envelope from bytes: views into `cbor` (keep the bytes until the
 * ta_host_deliver); its data, if any, read with from_wire by the host's
 * first datamodel that reads and writes neutral values, into out->data (one
 * reference: ta_host_deliver moves it on success; on a failure it stays the
 * caller's, to release with that datamodel, out->datamodel). TA_OK.
 * TA_ERROR_INVALID with out->size 0: not an envelope (not CBOR, not a map,
 * an unknown or repeated key, a wrong type, an error envelope); nothing to
 * answer, drop it. TA_ERROR_INVALID with out->size set: an envelope whose
 * data is not a neutral value; out holds no target and no data, so
 * ta_host_deliver of it refuses it as malformed (TA_ROUTE_ERROR_MALFORMED)
 * and answers the sender with a TA_HOST_EFFECT_PEER_ERROR, as for any
 * malformed envelope. TA_ERROR_NO_MEMORY: the datamodel could not hold the
 * data; nothing was made. TA_ERROR_STATE during a run. */
TA_API ta_status ta_host_envelope_read(ta_host *h, ta_bytes cbor, ta_envelope *out);
/* An error envelope from bytes: views into `cbor`; out->peer is left for the
 * caller to set. TA_ERROR_INVALID: not an error envelope. */
TA_API ta_status ta_host_peer_error_read(ta_bytes cbor, ta_peer_error *out);
/* A round trip the transport measured (ms), for the route status. */
TA_API void ta_host_peer_round_trip_time(ta_host *h, uint32_t peer, uint32_t ms);

/* Route status (docs/protocol.md, Observability). */
enum { TA_PEER_CONNECTING = 0, TA_PEER_OPEN = 1, TA_PEER_RECONNECTING = 2, TA_PEER_CLOSED = 3 };
typedef struct ta_peer_status {
    uint32_t size;
    uint32_t state;                              /* TA_PEER_* */
    uint32_t queued;                             /* envelopes waiting for the peer */
    uint32_t round_trip_time;                    /* ms, last measured (0: unknown) */
    ta_time lag;                                 /* ticks the oldest queued envelope has waited */
    uint64_t sent, received, errors, duplicates; /* monotonic */
    ta_string_view id; /* the peer's system id (valid until the peer is removed) */
} ta_peer_status;
TA_API ta_status ta_host_peer_status(const ta_host *h, uint32_t peer, ta_peer_status *out);

/* ---- migration ---------------------------------------------------------------- */
enum { TA_HOST_MIGRATE_WAKE = 1u << 0 };
/* Move actors of older versions to the current version of their chart. */
typedef struct ta_host_migration {
    ta_string_view name; /* empty: every chart */
    uint64_t session;    /* != 0: only that actor */
    uint32_t flags;      /* TA_HOST_MIGRATE_* */
    uint32_t pad;
    ta_time timeout; /* system ticks from now (0 = none) */
} ta_host_migration;
TA_API ta_status ta_host_migrate(ta_host *h, const ta_host_migration *migration, uint32_t *job);
/* Sessions a finished job left behind (valid until the next ta_host_run). */
TA_API uint32_t ta_host_job_pending(const ta_host *h, uint32_t job, ta_uint64_array out);

/* ---- hibernation (the host stores the blobs) ------------------------------------ */
/* CLOCK sweep over all segments: up to `max_scan` slots, at most
 * out.capacity sessions that did not run since the previous sweep and could
 * be hibernated now. */
TA_API uint32_t ta_host_sweep(ta_host *h, uint32_t max_scan, ta_uint64_array out);
TA_API ta_status ta_host_hibernate(ta_host *h, uint64_t session, ta_buffer *out);
TA_API ta_status ta_host_wake(ta_host *h, uint64_t session, ta_bytes blob);

/* ---- moving actors between hosts (another process, a restart) --------------------
 * ta_host_export_actor: ta_actor_export of the root actor `session` wherever it
 * lives in the host (with the sessions it invoked); it leaves the host. The blob
 * is a value when the datamodels implement payload_serialize (docs/durability.md).
 * ta_host_adopt: ta_system_adopt into `segment` (the blob's image: TA_ERROR_INVALID
 * otherwise), growing it as needed. Sessions keep their ids (and addresses, under
 * this host's system id); the host routes to them in `segment`, and segments of
 * their home range — existing or added later — never hand out their ids again.
 * TA_ERROR_STATE (nothing adopted) if any of its sessions exists in the host.
 * *session: the root's id.
 * Delayed sends keep their due times: for hosts with separate clocks, ta_blob_rebase
 * the blob first. */
TA_API ta_status ta_host_export_actor(ta_host *h, uint64_t session, ta_buffer *out);
typedef struct ta_host_adoption {
    uint32_t segment;
    uint32_t pad;
    ta_bytes blob;
} ta_host_adoption;
TA_API ta_status ta_host_adopt(ta_host *h, const ta_host_adoption *adoption, uint64_t *session);

/* ---- snapshot / restore -------------------------------------------------------------
 * A snapshot is the whole host — every segment's region (actors, queues,
 * timers, continuations), the moved-session map, the clock — as one blob.
 * It is for inspection and for taking a host out of memory for a while, not
 * for durability or backups: it holds raw region memory (see below).
 * It needs a quiescent host: effects acknowledged and no migration job
 * running (TA_ERROR_STATE otherwise). Written into `out` (ta_buffer).
 *
 * Datamodel state is not in it: regions refer to datamodel instances and
 * payloads by handle, and the host restores its datamodels so those handles
 * are valid again before the first ta_host_run (the JS bindings store their
 * value tables next to the snapshot; the C ECMAScript datamodel has
 * ta_datamodel_ecmascript_snapshot/_restore). Images are supplied again, one per
 * segment (NULL for retired ones); ta_host_snapshot_segments tells which.
 * Snapshots are for the same build of the library (regions are raw memory). */
TA_API ta_status ta_host_snapshot(ta_host *h, ta_buffer *out);
/* The segments a snapshot has: returns their number; hashes[i] is segment i's
 * image content hash (ta_image_info.content_hash), 0 for a retired segment.
 * 0 if the blob is not a host snapshot. */
TA_API uint32_t ta_host_snapshot_segments(ta_bytes snapshot, ta_uint64_array hashes);
/* A snapshot to restore and the image of each of its segments: images[i]
 * must be the image segment i ran (same content hash) or NULL if it was
 * retired; the image bytes must stay valid as with ta_host_add_image. */
typedef struct ta_host_snapshot_input {
    ta_bytes config; /* as for ta_host_create */
    ta_bytes snapshot;
    const ta_image *const *images;
    uint32_t image_count;
    uint32_t pad;
} ta_host_snapshot_input;
/* A new host (input->config and environment as for ta_host_create) running
 * the snapshot. Charts are attached to the datamodels again during the call. */
TA_API ta_status ta_host_restore(const ta_host_environment *environment,
                                 const ta_host_snapshot_input *input, ta_host **out);
/* The datamodels' state as the caller keeps it, labelled with their name. */
typedef struct ta_datamodel_state {
    ta_string_view datamodel;
    ta_bytes state;
} ta_datamodel_state;
/* A whole system in one blob: the host's snapshot (ta_host_snapshot), the
 * datamodels' state, and the image of every live segment, with the system
 * id and the host's time, written into `out` (ta_buffer); otherwise as
 * ta_host_snapshot. The blob's parts are 8-aligned when the blob is. */
TA_API ta_status ta_host_snapshot_pack(ta_host *h, const ta_datamodel_state *state, ta_buffer *out);
typedef struct ta_host_snapshot_contents {
    uint32_t size;
    uint32_t image_count; /* segments; ta_host_snapshot_image gives each one's image */
    ta_string_view system_id, datamodel;
    ta_time time; /* the host's time when it was packed */
    const void *host; /* for ta_host_restore */
    size_t host_length;
    const void *state; /* the datamodels' state, as given to ta_host_snapshot_pack */
    size_t state_length;
} ta_host_snapshot_contents;
/* The parts of a blob of ta_host_snapshot_pack (views into it). Set
 * out->size first. TA_ERROR_INVALID when it is not one. */
TA_API ta_status ta_host_snapshot_unpack(ta_bytes pack, ta_host_snapshot_contents *out);
/* Segment i's image in the blob; empty for a retired one. */
TA_API ta_bytes ta_host_snapshot_image(ta_bytes pack, uint32_t i);

/* ---- requests: the rarely called API (docs/requests.md) --------------------------
 * JSON-RPC's shape without its "jsonrpc" member, in CBOR: one map in,
 * {"method", "params", "id"}; one map out, {"id", "result"} or {"id",
 * "error": {"code", "message", "data": {"reason", "path"}}} for a request the
 * host refuses ("id" only when the request had one). Trace configuration,
 * watches, stats, inspection, state values and decoded spans are methods;
 * docs/requests.md lists every method, its params, defaults and errors.
 * TA_OK: `reply` holds the reply (a result or an error). TA_ERROR_NO_MEMORY:
 * nothing was applied and reply->length is what the reply needs; grow and
 * ask again. TA_ERROR_STATE during a run (nothing written). A request is
 * decoded into at most TA_HOST_REQUEST_TOKENS tokens, on the stack. */
enum { TA_HOST_REQUEST_TOKENS = 256 };
TA_API ta_status ta_host_request(ta_host *h, ta_bytes request, ta_buffer *reply);
/* The host's counters now, as an ExportMetricsServiceRequest (`/v1/metrics`,
 * docs/adr/0007), written into `out`: per chart, per segment and the host's
 * own. There is no metrics timer or effect: call it when the application
 * wants them. TA_ERROR_NO_MEMORY: out->length is what it needs. TA_ERROR_STATE
 * during a run or without an encoder (the config's "telemetry"). */
TA_API ta_status ta_host_metrics(ta_host *h, ta_buffer *out);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_HOST_H */
