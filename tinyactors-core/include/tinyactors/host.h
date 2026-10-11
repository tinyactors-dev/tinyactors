/* tinyactors/host.h — many systems as one actor system: the policy every embedder
 * would otherwise rebuild, in one place. All its memory, the segments' blocks
 * included, comes from the ta_allocator in its config (docs/adr/0005).
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
#include <tinyactors/system.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_host ta_host;

typedef struct ta_host_config {
    uint32_t size;
    uint32_t datamodel_count;
    ta_string_view system_id;               /* this system's id (docs/protocol.md, Addresses): its
                                       addresses are tinyactors://<system_id>/...; required */
    ta_system_config system;                /* capacities of each new segment (grown as needed);
                                               io_processors and first_serial are set by the host layer */
    const ta_datamodel_binding *datamodels; /* datamodels, shared by every segment */
    const ta_io_processor_descriptor
        *io_processors; /* host I/O processors (TA_EFFECT_SEND io_processor = index here) */
    uint32_t io_processor_count;
    uint32_t migrate_quantum; /* actors a migration job looks at per run (default 256) */
    ta_allocator allocator;   /* required: every block the host owns (ta_runtime_malloc() is
                                 the C library's); requests are aligned to at most 8 */
} ta_host_config;

TA_API ta_status ta_host_new(const ta_host_config *config, ta_host **out);
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
/* Spawn in (spawn->segment, spawn->chart) — usually the current version
 * (ta_host_chart). */
TA_API ta_status ta_host_spawn(ta_host *h, const ta_spawn *spawn, uint64_t *session);
/* Where a session lives now: its segment and its actor handle there. */
typedef struct ta_host_location {
    uint32_t segment;
    uint32_t actor;
} ta_host_location;
TA_API ta_status ta_host_locate(const ta_host *h, uint64_t session, ta_host_location *out);
/* A session's configuration as a nested state value, in JSON (the shape of
 * xstate's `value`): an atomic state's id ("s"), a compound state with its
 * active child ({"c": ...}), a parallel state with each region ({"p": {"r1":
 * ..., "r2": {}}}), {} for none, written into `out` (ta_buffer, no
 * terminator). TA_ERROR_INVALID when there is no such session or its
 * configuration has more than TA_STATE_VALUE_STATES states. */
enum { TA_STATE_VALUE_STATES = 1024 };
TA_API ta_status ta_host_state_value(const ta_host *h, uint64_t session, ta_buffer *out);
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
/* Move every segment's clock; queued envelopes past their TTL fail (an
 * expiry the allocator leaves no room to report is reported by the next
 * ta_host_run). */
TA_API void ta_host_advance_time(ta_host *h, ta_time now);
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
    TA_HOST_EFFECT_PEER_ERROR = 20 /* send an `err` to `peer`: aux = origin, msgid, sendid,
                                token = reason */
};
typedef struct ta_host_effect {
    uint32_t size;
    uint32_t kind;
    uint32_t segment;
    uint32_t actor;        /* handle in the segment (system effects) */
    uint64_t session;      /* the actor's session id (0: none) */
    uint64_t root;         /* the session of its host-spawned ancestor (attribution) */
    uint32_t token;        /* request / invoke token (per segment) */
    uint32_t io_processor; /* TA_EFFECT_SEND: index in ta_host_config.io_processors */
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
    ta_trace_context trace; /* as in ta_effect; PEER: the envelope's traceparent (format it
                               with ta_traceparent_format for the wire) */
} ta_host_effect;
TA_API uint32_t ta_host_effects(ta_host *h, const ta_host_effect **out);
TA_API void ta_host_effects_ack(ta_host *h);

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
    uint32_t datamodel;        /* datamodel index of `data` (ta_host_config.datamodels) */
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
    ta_bytes snapshot;
    const ta_image *const *images;
    uint32_t image_count;
    uint32_t pad;
} ta_host_snapshot_input;
/* A new host (config as for ta_host_new) running the snapshot. Charts are
 * attached to the datamodels again during the call. */
TA_API ta_status ta_host_restore(const ta_host_config *config, const ta_host_snapshot_input *input,
                                 ta_host **out);
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

/* ---- inspection ------------------------------------------------------------------ */
/* The host's counters (tinyactors/stats.h). Set `size` first. */
TA_API void ta_host_stats_get(const ta_host *h, ta_host_stats *out);
/* What the actors of each chart name did, summed over every segment with a
 * chart of that name, the totals of retired segments included, so a count
 * never goes back when a version retires. Fills up to `capacity` entries
 * (set each one's `size` first) in the order of the current versions and
 * returns how many chart names there are; call again with more room when
 * that is more than `capacity`. Names are valid until the next mutating
 * call on the host. */
TA_API uint32_t ta_host_chart_stats(const ta_host *h, ta_chart_stats *out, uint32_t capacity);
/* What every segment traces, and new segments start with (docs/adr/0007).
 * Rules and watches name the kinds they give (rule.detail: TA_TRACE_* masks
 * with TA_TRACE_VALUES, TA_NIL to keep the trace's own); the host gives each
 * different detail a class of the policy (ta_trace_policy) it sets in every
 * segment: class 0 the lifecycle kinds of `kinds`, class 1 `kinds`, then one
 * per detail the watches and rules ask for. A watch's rules come before the
 * options' rules, newer watches first.
 * TA_ERROR_STATE when the host's systems do not trace
 * (config.system.tracing); TA_ERROR_NO_MEMORY for more than
 * TA_HOST_TRACE_DETAILS different details, more rules than a system holds
 * (config.system.trace_rules) or no memory; TA_ERROR_INVALID for a malformed
 * rule. Nothing changes then. Each segment's records: ta_host_system, then
 * ta_system_trace. */
enum { TA_HOST_TRACE_NO_RECORDS = 1u << 0 }; /* no records (traceparents still flow) */
#define TA_HOST_TRACE_DETAILS (TA_TRACE_DETAILS - 2u)
typedef struct ta_host_trace_options {
    uint32_t size;
    uint32_t flags;                      /* TA_HOST_TRACE_* */
    uint32_t kinds;                      /* what a sampled trace writes */
    uint32_t sample_parts, sample_whole; /* as ta_trace_policy */
    uint32_t rule_count;
    const ta_trace_rule *rules; /* copied */
} ta_host_trace_options;
TA_API ta_status ta_host_trace_set(ta_host *h, const ta_host_trace_options *options);
/* A watch: rules (as in ta_host_trace_options) that hold until the host's
 * time reaches `until` (UINT64_MAX: until ta_host_trace_unwatch). The time a
 * watch ends is a deadline of ta_host_run. Errors as ta_host_trace_set. */
typedef struct ta_host_watch {
    const ta_trace_rule *rules; /* copied */
    uint32_t rule_count;
    uint32_t pad;
    ta_time until;
} ta_host_watch;
TA_API ta_status ta_host_trace_watch(ta_host *h, const ta_host_watch *watch, uint32_t *id);
/* End a watch early; TA_ERROR_STALE when it has ended. */
TA_API ta_status ta_host_trace_unwatch(ta_host *h, uint32_t id);
/* Enumerate sessions host-wide (cursor starts at 0); returns how many (0 = done). */
TA_API uint32_t ta_host_actors(const ta_host *h, uint64_t *cursor, ta_uint64_array out);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_HOST_H */
