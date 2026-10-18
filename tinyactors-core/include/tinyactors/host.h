/* tinyactors/host.h — many systems as one actor system: the policy every embedder
 * would otherwise rebuild, in one place. All its memory, the segments' blocks
 * included, comes from the ta_allocator of its environment (docs/adr/0005).
 *
 * How to use it — the loop (take, handle, ack, run), inputs, effects, the
 * control plane, images, values, memory, threads, and a complete loop in C —
 * is docs/embedding.md; the contract of every call below (when it applies,
 * what it produces, who owns it, how long it lives, what refuses it) is
 * docs/boundary.md. Everything the host hands out lives until
 * ta_host_effects_ack, and ta_host_run refuses while a batch is open.
 *
 * A ta_host owns *segments*: one ta_system per image. All segments share one
 * base URI and one set of datamodel bindings; segment k creates sessions
 * k * 2^32 + 1, ... so a session id names an actor host-wide, for its whole
 * life — also after it migrated to another segment.
 *
 *   versions   ta_host_add_image() adds a segment running a copy of an image
 *              the host keeps; its charts become the current version of their
 *              chart names (<scxml name>). New actors of a name start in its
 *              current version. A segment whose charts all have newer versions
 *              and that has no actors left is freed with its copy of the image
 *              (TA_HOST_EFFECT_SEGMENT_FREED says it retired).
 *   routing    SCXML sends between segments (same system id) are delivered
 *              inside the host layer — no host round trip — with origin the
 *              sender's address. Other systems: a send to an address whose
 *              system id is not this host's is a TA_EFFECT_SEND for the
 *              host's transport (io_processor 0, the SCXML processor; the
 *              application's, which needs no config; TA_EFFECT_FIELD_ORIGIN
 *              is the sender's address). An embedder with no transport fails
 *              each one (TA_EVENT_IN_ERROR_COMMUNICATION). What arrives from another
 *              system is an input: ta_host_resolve its local address, then
 *              ta_host_send with origin. Sends to host
 *              I/O processors come out as TA_EFFECT_SEND effects.
 *   migration  ta_host_migrate() starts a migration that moves actors of older
 *              versions to the current one at stable points (export ->
 *              translate -> adopt; error.platform.upgrade), advanced inside
 *              ta_host_run in bounded steps (a quantum of actors per run,
 *              however many it waits for): busy actors are retried,
 *              hibernated ones wait until ta_host_wake brings them back,
 *              asked for with TA_HOST_MIGRATE_WAKE (a TA_EFFECT_WAKE whose
 *              migration is the migration's id), again each time the migration finds one
 *              hibernated after a wake; actors with running invocations wait until those
 *              finish (a session tree moves unchanged, but is not
 *              translated); a timeout (system ticks) ends the migration with the
 *              stragglers, and a migration with an actor nobody wakes ends only
 *              there.
 *   hibernation ta_host_sweep() runs the CLOCK sweep over all segments;
 *              blobs and their storage stay with the host (ta_host_hibernate,
 *              TA_EFFECT_WAKE, ta_host_wake).
 *   growth     segments are relocated into bigger blocks when a run or a call
 *              needs it (TA_RUN_EXHAUSTED / TA_ERROR_NO_MEMORY), each capacity
 *              doubled up to its hard limit (TA_SYSTEM_MAX_*). A segment
 *              whose capacities are all at their limits does not grow: the
 *              call returns the system's status, a run TA_RUN_FAULT. Nothing
 *              is grown for what no growth can hold (a text past
 *              TA_SYSTEM_MAX_STRING is TA_ERROR_INVALID at the input).
 *   memory     every block comes from environment->allocator and goes back to it
 *              (ta_host_free returns all of them). When it refuses one, a
 *              call returns TA_ERROR_NO_MEMORY and leaves the host as it
 *              was; ta_host_run returns TA_RUN_EXHAUSTED (detail: the
 *              segment's TA_POOL_* that could not grow, or TA_POOL_NONE for
 *              the host's own memory) and the next run goes on where it
 *              stopped: nothing is lost or done twice.
 *
 * Calls are synchronous and budgeted like ta_system_run; outcomes are effects
 * (ta_host_effects), borrowed until ta_host_effects_ack or ta_host_free:
 * calls in between that make more effects (ta_host_post appending its
 * REPLY, ta_host_send failing a sender, ...) leave the borrowed
 * records and their strings where they are; the new effects come with the
 * next batch, after the ack. Repeated takes return the same pointer and
 * count; ta_host_run refuses while a batch is open. Payloads passed in are
 * moved on success and stay the caller's
 * on failure. No callbacks besides the datamodel vtables.
 *
 * What the host writes for its caller — traces, logs, metrics, a
 * hibernation blob — comes out as a
 * ta_bytes view of memory the host owns, valid until ta_host_effects_ack or
 * ta_host_free, whatever calls come between (copy it to keep it past the
 * ack). Taken while a batch is open, it joins that batch; taken with none
 * open, it opens a batch of its own: ta_host_effects returns 0 and
 * ta_host_run refuses until the ack. The host grows that memory through its
 * allocator, between runs, and keeps it for reuse until ta_host_free;
 * TA_ERROR_NO_MEMORY from one of these calls means the allocator refused
 * room. Snapshots are written into the caller's ta_buffer.
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
/* Spans still open are ended and dropped with the host: a caller who wants
 * them posts the trace.end message and takes the traces first. Not from a
 * datamodel's code: during a run or while the host is inside a datamodel
 * (below, "During a run") it does nothing, and the host stays. */
TA_API void ta_host_free(ta_host *h);

/* ---- segments and versions ----------------------------------------------- */
/* A new segment running the image `image` (.tai bytes, any alignment). The
 * host reads `image` during the call only, before any datamodel call: it
 * copies the bytes into a block from environment->allocator, opens them
 * there (ta_image_open), and frees the block when the segment is freed or
 * with ta_host_free. The caller may change or free its bytes once the call
 * returns. TA_ERROR_INVALID: not an image; TA_ERROR_NO_MEMORY: the
 * allocator refused (nothing added); TA_ERROR_STATE during a run. */
TA_API ta_status ta_host_add_image(ta_host *h, ta_bytes image, uint32_t *segment);
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
    uint32_t actors;  /* actors in it (including ones a migration moved in) */
    uint32_t current; /* charts of it that are the current version of their name */
    const ta_image *image; /* the host's copy (borrowed: valid while the segment is live);
                              NULL once the segment is freed */
} ta_host_segment_info;
TA_API uint32_t ta_host_segment_count(const ta_host *h); /* segments ever added */
TA_API ta_status ta_host_segment_info_get(const ta_host *h, uint32_t segment,
                                          ta_host_segment_info *out);
/* The ta_system of a segment, for inspection and per-system calls (borrowed:
 * valid until the next ta_host call that can grow or free it). */
TA_API ta_system *ta_host_system(ta_host *h, uint32_t segment);

/* ---- system images ------------------------------------------------------------ */
/* Spawn the actors a system image declares (<system><actor name chart/>) in
 * `segment`, in declaration order. The actor pool is grown first so all of
 * them fit: TA_ERROR_NO_MEMORY from that growth spawns none, and boot may be
 * called again. A spawn can still fail after that (the instance or name
 * space, or the datamodel's create refusing): boot then returns its status
 * and the actors declared before it stay live. Booting again spawns every
 * boot actor again, those included, as does a second boot after success;
 * after such a failure, terminate the ones that live (ta_host_actor_by_name)
 * or free the host. TA_ERROR_STATE during a run. */
TA_API ta_status ta_host_boot(ta_host *h, uint32_t segment);
/* The session of a live actor spawned with `name` (boot actors carry their
 * manifest name), newest segment first, and in a segment the oldest such
 * actor (ta_system_actor_by_name); 0 if none. O(segments), not O(actors). */
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
 * Lookups answer as usual: ta_host_chart, ta_host_locate,
 * ta_host_actor_by_name, ta_host_segment_count, ta_host_segment_info_get,
 * ta_host_system, ta_host_resolve, ta_host_migration_pending, and the effect
 * accessors (which answer 0 or empty: no batch is open in a run).
 * ta_host_free does nothing. Every other call is refused:
 * TA_ERROR_STATE, an empty answer or nothing done (ta_host_effects returns
 * 0; ta_host_effects_ack, ta_host_advance_time do nothing; ta_host_sweep
 * returns 0; ta_host_run returns TA_RUN_FAULT with TA_ERROR_STATE).
 *
 * Outside a run, the same rules hold while the host is inside a datamodel:
 * a posted message (actor.context), hibernating, waking, the telemetry takes,
 * `release` in the ack and at the start of a run, and the like. A
 * datamodel's code that calls back into the host there can add inputs but
 * cannot change what the host is using (docs/adr/0010 in the repository
 * root, decision 3). Inside ta_host_free every call is refused, the inputs
 * too. */

/* Spawn in (spawn->segment, spawn->chart) — usually the current version
 * (ta_host_chart). A name longer than TA_SYSTEM_MAX_STRING (system.h) is
 * TA_ERROR_INVALID: nothing is allocated and no segment grows for it. */
TA_API ta_status ta_host_spawn(ta_host *h, const ta_spawn *spawn, uint64_t *session);
/* Where a session lives now: its segment and its actor handle there. */
typedef struct ta_host_location {
    uint32_t segment;
    uint32_t actor;
} ta_host_location;
TA_API ta_status ta_host_locate(const ta_host *h, uint64_t session, ta_host_location *out);
/* An event for the session event->session (event->target is ignored). An
 * event with an origin and no origintype gets the SCXML event processor's
 * (an address is an SCXML processor's, SCXML C.1). A name, sendid, origin,
 * origintype or raw longer than TA_SYSTEM_MAX_STRING (system.h) is
 * TA_ERROR_INVALID, the payload the caller's: no growth makes room for it. */
TA_API ta_status ta_host_send(ta_host *h, const ta_event_in *event);
/* The session a local address names: "tinyactors://<this system id>/#_scxml_<id>"
 * or ".../@<name>" (the live actor of that name). How a transport delivers
 * what another system sent: resolve the target, then ta_host_send. Another
 * system's address, or text that is no address: TA_ERROR_INVALID; no such
 * session or name: TA_ERROR_STALE. */
TA_API ta_status ta_host_resolve(const ta_host *h, ta_string_view address, uint64_t *session);
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
/* Move every segment's clock. With an encoder, the trace records written since the last
 * call are stamped between its wall time and this one: call it before each
 * run. */
TA_API void ta_host_advance_time(ta_host *h, ta_host_time now);
/* Run segments with work for up to `budget` fuel in total, deliver sends
 * between them, advance migrations, free retired segments.
 * With an open effect batch, or from a guarded call, return TA_RUN_FAULT,
 * detail TA_ERROR_STATE, used 0 and next_deadline UINT64_MAX; nothing runs
 * or is released. Acknowledge every batch before calling run.
 * status: TA_RUN_IDLE / TA_RUN_YIELDED / TA_RUN_EFFECTS_FULL / TA_RUN_FAULT /
 * TA_RUN_EXHAUSTED (the allocator refused memory: run again once it has
 * some; detail as for ta_system_run, TA_POOL_NONE for the host's own) /
 * TA_RUN_EFFECTS (senders wait for sends handed out in this batch,
 * TA_EFFECT_FLAG_WAITING: handle them — a failure is ta_host_send of
 * TA_EVENT_IN_ERROR_COMMUNICATION to the sender, before the ack — ack and
 * run again; each sender goes on only then, the other actors meanwhile);
 * next_deadline: the earliest of the segments' timers, the migrations'
 * timeouts, the end of a trace watch, and the encoder's flush (flush_interval after a request's
 * first record); UINT64_MAX for none. Arm one timer for it and call
 * ta_host_advance_time, then ta_host_run, when it comes. */
TA_API ta_run ta_host_run(ta_host *h, uint32_t budget);

enum {
    /* TA_EFFECT_SEND .. TA_EFFECT_WAKE (1..8) as in tinyactors/system.h, plus: */
    TA_HOST_EFFECT_MIGRATED = 16, /* session moved to (segment, chart); migration */
    TA_HOST_EFFECT_MIGRATION_DONE = 17, /* migration ended: migrated, failed, pending (ta_host_migration_pending) */
    TA_HOST_EFFECT_SEGMENT_FREED = 18, /* segment retired (the host freed its image copy) */
    /* the encoder (the config's "telemetry"): a request is ready, because the
     * staging is full or flush_interval passed since its first record; take
     * it with ta_host_traces (ta_host_logs). At most one of each kind is
     * outstanding: the next comes after a take. */
    TA_HOST_EFFECT_TRACES = 21,
    TA_HOST_EFFECT_LOGS = 22,
    /* the control plane (ta_host_post, docs/requests.md): */
    TA_HOST_EFFECT_REPLY = 23,       /* WIRE: {"id", "result"} or {"id", "error"}; the last
                                        effect of the ta_host_post that answered it */
    TA_HOST_EFFECT_NOTIFICATION = 24 /* WIRE: {"method", "params"}; EVENT: the method; SESSION:
                                        the session it is about (0: none). No notification is
                                        defined yet (docs/requests.md, "Notifications"): drop a
                                        method you do not know. */
};
typedef struct ta_host_effect {
    uint32_t size;
    uint32_t kind;
    uint32_t segment;
    uint32_t actor;        /* handle in the segment (system effects) */
    uint64_t session;      /* the actor's session id (0: none) */
    uint64_t root;         /* the session of its host-spawned ancestor (attribution) */
    uint32_t token;        /* request / invoke token (per segment) */
    uint32_t io_processor; /* TA_EFFECT_SEND, as _ioprocessors numbers them: 0 the SCXML
                              processor (another system: the host's transport), i + 1 the
                              config's ioProcessors[i], TA_NIL an invoked service */
    /* (migration: MIGRATED, MIGRATION_DONE, and a WAKE a migration asked for; 0 otherwise) */
    uint32_t flags;        /* TA_EFFECT_FLAG_* (system.h) */
    uint32_t migration;
    ta_string_view target, event, sendid, auxiliary; /* as in ta_effect; MIGRATED: event = chart
                                          name */
    ta_payload payload;                              /* borrowed */
    ta_time delay_hint;
    uint32_t chart;                     /* MIGRATED: chart index in `segment` */
    uint32_t migrated, failed, pending; /* MIGRATION_DONE */
    uint32_t datamodel;                 /* SEND: datamodel index of payload (TA_NIL: none) */
    uint32_t pad;
    ta_trace_context trace; /* as in ta_effect */
    ta_bytes wire;          /* REPLY, NOTIFICATION: the message, CBOR */
} ta_host_effect;
/* First nonempty take opens one batch. Further takes return that same
 * pointer and count, including after calls append effects or grow storage.
 * Effects appended during dispatch wait until after the ack. An empty take
 * opens no batch. While a batch of outputs alone is open (an output taken
 * with no batch open), returns 0 with *out NULL: pending effects wait for
 * the ack. During a guarded call, returns 0 with *out NULL. */
TA_API uint32_t ta_host_effects(ta_host *h, const ta_host_effect **out);
/* End the published borrow and release only that batch; later effects stay
 * pending. Publication is closed before payload-release callbacks run. */
TA_API void ta_host_effects_ack(ta_host *h);

/* A field of effect `index` of the batch ta_host_effects published, so that a
 * binding reads effects without knowing ta_host_effect's layout. Numbers:
 * KIND .. PENDING (ta_host_effect_number); text: TARGET .. ORIGIN
 * (ta_host_effect_text); bytes: WIRE (ta_host_effect_bytes). An index past
 * the batch, a field the effect kind does not set, or a field asked through
 * the wrong accessor answers 0 or empty. Text and bytes are borrowed like the effect
 * (until the ack or ta_host_free); TRACEPARENT's text is written into the
 * batch's memory on the first ask (empty if that allocation is refused) and
 * lasts until the ack too, however many fields are read meanwhile, and so
 * does ORIGIN's.
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
    TA_EFFECT_FIELD_MIGRATION = 9,
    TA_EFFECT_FIELD_PAYLOAD = 10, /* the payload handle (ta_payload) */
    TA_EFFECT_FIELD_DATAMODEL = 11,
    TA_EFFECT_FIELD_DELAY_HINT = 12,
    TA_EFFECT_FIELD_CHART = 13,
    TA_EFFECT_FIELD_MIGRATED = 14,
    TA_EFFECT_FIELD_FAILED = 15,
    TA_EFFECT_FIELD_PENDING = 16,
    TA_EFFECT_FIELD_TARGET = 32,
    TA_EFFECT_FIELD_EVENT = 33,
    TA_EFFECT_FIELD_SENDID = 34,
    TA_EFFECT_FIELD_AUXILIARY = 35,
    TA_EFFECT_FIELD_TRACEPARENT = 36, /* W3C text of `trace`; empty when untraced */
    TA_EFFECT_FIELD_ORIGIN = 37, /* SEND: the sender's address, tinyactors://<system id>/#_scxml_<session> */
    TA_EFFECT_FIELD_WIRE = 64 /* bytes (ta_host_effect_bytes). REPLY, NOTIFICATION: the message's CBOR */
};
TA_API uint64_t ta_host_effect_number(const ta_host *h, uint32_t index, uint32_t field);
TA_API ta_string_view ta_host_effect_text(ta_host *h, uint32_t index, uint32_t field);
TA_API ta_bytes ta_host_effect_bytes(const ta_host *h, uint32_t index, uint32_t field);

/* ---- telemetry ------------------------------------------------------------------ */
/* The ExportTraceServiceRequest (`/v1/traces`, application/x-protobuf) of
 * the spans staged so far, in `out`: a view of the host's memory, valid until
 * the ack (length 0 when there are none); the trace
 * records written since the last run are read first. The spans leave the
 * staging only on TA_OK: TA_ERROR_NO_MEMORY means the allocator refused room
 * and nothing changed. Posting it, retrying and capping what waits are the
 * caller's. TA_ERROR_STATE during a run or without an encoder (the config's
 * "telemetry"). ta_host_logs: the same for the ExportLogsServiceRequest
 * (`/v1/logs`), a view valid until the ack. */
TA_API ta_status ta_host_traces(ta_host *h, ta_bytes *out);
TA_API ta_status ta_host_logs(ta_host *h, ta_bytes *out);

/* ---- migration ---------------------------------------------------------------- */
enum { TA_HOST_MIGRATE_WAKE = 1u << 0 };
/* Move actors of older versions to the current version of their chart.
 * Refused when it could move nothing: TA_ERROR_INVALID when the host has no
 * chart of that name, TA_ERROR_STALE when the session has ended. */
typedef struct ta_host_migration {
    ta_string_view name; /* empty: every chart */
    uint64_t session;    /* != 0: only that actor */
    uint32_t flags;      /* TA_HOST_MIGRATE_* */
    uint32_t pad;
    ta_time timeout; /* system ticks from now (0 = none) */
} ta_host_migration;
TA_API ta_status ta_host_migrate(ta_host *h, const ta_host_migration *request, uint32_t *migration);
/* The sessions a migration waits for while it runs, in no order; those a finished
 * migration left behind (valid until the next ta_host_run). */
TA_API uint32_t ta_host_migration_pending(const ta_host *h, uint32_t migration, ta_uint64_array out);

/* ---- hibernation (the host stores the blobs) ------------------------------------ */
/* CLOCK sweep over all segments: up to `max_scan` slots, at most
 * out.capacity sessions that did not run since the previous sweep and could
 * be hibernated now. */
TA_API uint32_t ta_host_sweep(ta_host *h, uint32_t max_scan, ta_uint64_array out);
/* ta_actor_hibernate of `session`: the blob in `out`, a view of the host's
 * memory, valid until the ack (store or copy it before then); written in
 * one pass, the datamodel's serialize called
 * once. TA_ERROR_NO_MEMORY: the allocator refused room, and the actor stays. */
TA_API ta_status ta_host_hibernate(ta_host *h, uint64_t session, ta_bytes *out);
TA_API ta_status ta_host_wake(ta_host *h, uint64_t session, ta_bytes blob);

/* ---- snapshot / restore -------------------------------------------------------------
 * A snapshot is the whole host — every segment's region (actors, queues,
 * timers, continuations), the moved-session map, the clock — as one blob.
 * It is for inspection and for taking a host out of memory for a while, not
 * for durability or backups: it holds raw region memory (see below).
 * It needs a quiescent host: effects acknowledged and no migration
 * running (TA_ERROR_STATE otherwise). Written into `out` (ta_buffer).
 *
 * Datamodel state is not in it: regions refer to datamodel instances and
 * payloads by handle, and the host restores its datamodels so those handles
 * are valid again before the first ta_host_run (a binding's datamodel saves
 * its tables' namespaces, ta_table_snapshot, and their rows' values next to
 * the snapshot; the C ECMAScript datamodel has
 * ta_datamodel_ecmascript_snapshot/_restore). Images are supplied again, one per
 * segment (empty for retired ones); ta_host_snapshot_segments tells which.
 * Snapshots are for the same build of the library (regions are raw memory). */
TA_API ta_status ta_host_snapshot(ta_host *h, ta_buffer *out);
/* The segments a snapshot has: returns their number; hashes[i] is segment i's
 * image content hash (ta_image_info.content_hash), 0 for a retired segment.
 * 0 if the blob is not a host snapshot. */
TA_API uint32_t ta_host_snapshot_segments(ta_bytes snapshot, ta_uint64_array hashes);
/* A snapshot to restore and the image of each of its segments: images[i]
 * is the bytes of the image segment i ran (same content hash), or empty if
 * it was retired; the host copies them as ta_host_add_image does, and reads
 * nothing of `images` after the call. */
typedef struct ta_host_snapshot_input {
    ta_bytes config; /* as for ta_host_create */
    ta_bytes snapshot;
    const ta_bytes *images;
    uint32_t image_count;
    uint32_t pad;
} ta_host_snapshot_input;
/* A new host (input->config and environment as for ta_host_create) running
 * the snapshot. Charts are attached to the datamodels again during the call.
 * Restore the datamodels' state before the first ta_host_run: that run
 * announces again each external invocation that was running, before any
 * actor runs, as a TA_EFFECT_INVOKE flagged TA_EFFECT_FLAG_RESTORED (its
 * token, type, src, invoke id and params as before), so the embedder
 * starts its service again or answers the invoker with
 * TA_EVENT_IN_ERROR_COMMUNICATION. */
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

/* ---- the control plane: posted messages (docs/requests.md) -----------------------
 * A JSON-RPC 2.0 message without "jsonrpc", in CBOR (docs/requests.md),
 * read during the call. A message with an id (or one that cannot be read)
 * is answered: the reply is the LAST effect this call appends,
 * TA_HOST_EFFECT_REPLY, after any effects the method itself appends
 * (trace.end: TRACES, LOGS). Its WIRE is {"id", "result"} or {"id",
 * "error": {"code", "message", "data": {"reason", "path"}}}; "id" is echoed,
 * null when the message could not be read or is not a request (a batch is
 * one: -32600). A message without "id" is applied, with no REPLY and no
 * error reported. Trace configuration, watches, stats, inspection, state
 * values and decoded spans are methods; docs/requests.md
 * lists every method, its params, defaults and errors.
 *
 * ta_host_effects publishes everything appended since the last ack, with or
 * without a run: a REPLY is read like every other effect (WIRE with
 * ta_host_effect_bytes) and lives until ta_host_effects_ack, the next
 * ta_host_run or ta_host_free. TA_OK: the message was read (a JSON-RPC error
 * travels inside the REPLY). TA_ERROR_NO_MEMORY: the allocator refused room
 * for the reply, or the published batch holds too many replaced blocks;
 * nothing was applied and no REPLY appended. TA_ERROR_STATE during a run,
 * or while the host is inside a datamodel (nothing appended). A message is
 * decoded into at most TA_HOST_REQUEST_TOKENS tokens, on the stack. */
enum { TA_HOST_REQUEST_TOKENS = 256 };
TA_API ta_status ta_host_post(ta_host *h, ta_bytes message);
/* The host's counters now, as an ExportMetricsServiceRequest (`/v1/metrics`,
 * docs/adr/0007), in `out`: per chart, per segment and the host's own, a view
 * of the host's memory, valid until the ack. There is no
 * metrics timer or effect: call it when the application wants them.
 * TA_ERROR_NO_MEMORY: the allocator refused room. TA_ERROR_STATE during a run
 * or without an encoder (the config's "telemetry"). */
TA_API ta_status ta_host_metrics(ta_host *h, ta_bytes *out);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_HOST_H */
