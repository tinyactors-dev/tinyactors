/* tinyactors/datamodel_ecmascript.h — the SCXML ECMAScript datamodel (SCXML 1.0 §B.2),
 * implemented on quickjs-ng, packaged like a tree-sitter language: link
 * libtinyactors_datamodel_ecmascript and pass its binding to ta_system_init().
 *
 *     ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
 *     ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
 *     ta_system_init(memory, length, &image, &config, &binding, 1, &system);
 *     ...
 *     ta_system_deinit(system);
 *     ta_datamodel_ecmascript_free(ecmascript);
 *
 * No I/O: only the ECMAScript engine is linked (no std/os modules, no
 * timers, no file or network access). Payloads are JS values shared within
 * the runtime.
 *
 * Realms (`realm`):
 *   TA_DATAMODEL_ECMASCRIPT_REALM_AUTO (default): sessions of a chart share one locked-down
 *     JS context and each session is a small scope object (~0.5 KB instead of
 *     a ~54 KB context). Expressions compile once per chart and run as strict
 *     code inside `with (scope)`: <data> ids, _event, _sessionid, _name and
 *     _ioprocessors resolve per session, everything else to the shared
 *     builtins. Differences from a private global object: assigning an
 *     undeclared name is an error (as in strict code), the global object is
 *     frozen, and builtin prototypes are shared by all sessions of the
 *     datamodel. A chart whose <script>s declare top-level names (var, let,
 *     const, function, class — as the engine sees them: names in blocks or
 *     nested functions do not count) keeps isolated sessions.
 *   TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED: every session gets its own JS context (global
 *     object), always — for charts that must not share builtins.
 *
 * Many systems, one datamodel: a chart attached again through the same image
 * (or, once nothing uses it, any image with the same content) is shared with
 * its compiled expressions; a chart no system or session uses is freed — the
 * last 16 are kept idle for reuse. A host that builds a short-lived system
 * per actor (adopt, process, export, deinit) therefore runs in constant memory
 * and compiles each expression once.
 *
 * Hibernation, export, snapshots (ta_actor_hibernate, ta_actor_export,
 * ta_datamodel_ecmascript_snapshot): a session's variables and _event are written
 * with JS_WriteObject (object graphs incl. cycles, Date/Map/Set/typed arrays;
 * class instances come back as plain objects). Isolated sessions store their
 * global state: var and implicit globals, let/const bindings (const stays
 * const) and top-level function declarations, which are re-run on restore.
 * Other functions (`const f = () => ...`, functions inside data), classes,
 * Errors, Promises... report TA_ERROR_UNSUPPORTED — the session stays resident
 * and ta_datamodel_ecmascript_last_error() names the variable (and a function's
 * line). Changes to builtins are not stored. Runaway scripts are interrupted
 * after `max_ops` engine checks and reported as error.execution.
 *
 * Determinism (random_seed, clock): with a seed, Math.random draws from the
 * datamodel's own generator; with the host's clock, Date and
 * performance.now() read the system's clocks. The datamodel replaces those
 * builtins in every context that runs a chart's code (the shared realm
 * before it is frozen, each isolated session's, and the one host helpers
 * evaluate in); with both fields 0 it makes its contexts exactly as the
 * engine does.
 */
#ifndef TINYACTORS_DATAMODEL_ECMASCRIPT_H
#define TINYACTORS_DATAMODEL_ECMASCRIPT_H

#include <tinyactors/datamodel.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_datamodel_ecmascript ta_datamodel_ecmascript;

typedef struct ta_datamodel_ecmascript_config {
    uint32_t size;
    uint32_t max_ops;    /* interrupt checks per evaluation before abort; 0 = the default */
    size_t memory_limit; /* bytes for the whole JS runtime; 0 = the default,
                            TA_DATAMODEL_ECMASCRIPT_MEMORY_LIMIT_DEFAULT; SIZE_MAX: none */
    size_t stack_limit;  /* bytes of native stack for JS; 0 = engine default */
    uint32_t
        realm; /* TA_DATAMODEL_ECMASCRIPT_REALM_* (fields from here on are checked against size) */
    /* A flag the host sets asynchronously to stop JavaScript now (e.g. a
     * database's pending-interrupt flag): checked every 10,000 operations,
     * the running evaluation then fails like a runaway one. NULL: none.
     * JavaScript work is also charged to the actor's fuel (one unit per
     * 100 operations), so the system's budget bounds it. */
    const volatile int *abort_flag;
    /* Math.random's seed. 0: the engine's own (each context seeded from the
     * time of day, a different sequence on every run). Any other value: one
     * generator per datamodel (xorshift64*, its state mixed from the seed),
     * shared by every context and session it makes, so draws come in the
     * order the system runs evaluations and two datamodels with the same seed
     * and the same inputs draw the same numbers. Its state is in the
     * datamodel's snapshot, not in a session's blob (hibernation and export
     * keep no part of it), so a restored datamodel goes on drawing where the
     * snapshot was taken. */
    uint64_t random_seed;
    /* What Date and performance.now() read: a ta_datamodel_ecmascript_clock. */
    uint32_t clock;
} ta_datamodel_ecmascript_config;

/* The clock of a datamodel's contexts (ta_datamodel_ecmascript_config.clock).
 *   ENGINE (0): the engine's own, the machine's time of day and monotonic clock.
 *   HOST: the system's clocks (ta_system_advance_time, ta_host_advance_time),
 *     read during an evaluation through ta_datamodel_environment_time: `new
 *     Date()`, `Date()` and `Date.now()` give its wall time in milliseconds
 *     (0, the epoch, while the embedder gives none), `performance.now()` its
 *     monotonic ticks in milliseconds (`performance.timeOrigin` is 0: the
 *     engine's is the machine's time); every read within one run gives the
 *     same time. Outside an evaluation (ta_datamodel_ecmascript_payload_eval)
 *     they read the time the last evaluation saw. `new Date(...)` with
 *     arguments, Date.UTC, Date.parse and every method of Date.prototype are
 *     the engine's (`instanceof Date` holds). The local-time methods
 *     (getHours, getTimezoneOffset, toString, ...) still follow the process's
 *     time zone (TZ) under either clock: set TZ to repeat them. */
typedef enum ta_datamodel_ecmascript_clock {
    TA_DATAMODEL_ECMASCRIPT_CLOCK_ENGINE = 0,
    TA_DATAMODEL_ECMASCRIPT_CLOCK_HOST = 1
} ta_datamodel_ecmascript_clock;

/* The engine's memory limit when the config gives none: 1 GiB. An
 * evaluation that allocates past it fails with error.execution (reason
 * "memory") instead of growing until the process is killed. Every test,
 * example and suite case of the repository fits in it with room: the
 * largest peak measured was 112 MB (40,000 resident actors). */
#define TA_DATAMODEL_ECMASCRIPT_MEMORY_LIMIT_DEFAULT ((size_t)1 << 30)

/* The default max_ops: interrupt checks one evaluation may make. quickjs
 * checks every 10,000 operations, so this stops a busy loop after about
 * 10^8 operations, a second or two, rather than holding the host for
 * minutes; a chart whose evaluations do more raises it. */
#define TA_DATAMODEL_ECMASCRIPT_MAX_OPS_DEFAULT 10000u

#define TA_DATAMODEL_ECMASCRIPT_REALM_AUTO     0u
#define TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED 1u

TA_API ta_datamodel_ecmascript *
ta_datamodel_ecmascript_new(const ta_datamodel_ecmascript_config *config);
/* Free after every system using it was finalized. */
TA_API void ta_datamodel_ecmascript_free(ta_datamodel_ecmascript *ecmascript);

TA_API const ta_datamodel_vtable *ta_datamodel_ecmascript_vtable(void); /* name: "ecmascript" */
TA_API ta_datamodel_binding ta_datamodel_ecmascript_binding(ta_datamodel_ecmascript *es);

/* Host helpers. Neutral values as CBOR (docs/protocol.md, "Values"): the
 * vtable's to_wire and from_wire, callable without it. to_cbor's `out` is a
 * view of the datamodel's memory, valid until its next call. */
TA_API ta_status ta_datamodel_ecmascript_payload_to_cbor(ta_datamodel_ecmascript *ecmascript,
                                                         ta_payload p, ta_bytes *out);
TA_API ta_status ta_datamodel_ecmascript_payload_from_cbor(ta_datamodel_ecmascript *ecmascript,
                                                           ta_bytes cbor, ta_payload *out);
/* A payload from an ECMAScript expression; 0 on error. With instance 0 it
 * is evaluated outside any session (e.g. "new Uint8Array([1, 2])"); else in
 * that session's datamodel (the instance handle as seen by the datamodel,
 * e.g. for inspection or debugging), where a variable's name reads it
 * without compiling. One reference, released by the system or with the
 * vtable's release. */
TA_API ta_payload ta_datamodel_ecmascript_payload_eval(ta_datamodel_ecmascript *ecmascript,
                                                       uint64_t instance,
                                                       ta_string_view expression);
/* Snapshot / restore of the whole datamodel — every session's variables and
 * _event, every payload and <foreach> in progress, at their handles — for
 * restoring a system or host snapshot (ta_system_restore, ta_host_restore),
 * whose regions refer to them by handle. One object graph (JS_WriteObject:
 * shared references and cycles are kept); an isolated session is a blob of
 * its own inside it. What cannot be stored (see Hibernation) reports
 * TA_ERROR_UNSUPPORTED (ta_datamodel_ecmascript_last_error says why). Written into `out`
 * (ta_buffer). Restore into a fresh datamodel (no
 * sessions or payloads yet: TA_ERROR_STATE) after the system/host restore attached the charts;
 * sessions find their chart by image content hash and chart index (TA_ERROR_INVALID if missing). */
TA_API ta_status ta_datamodel_ecmascript_snapshot(ta_datamodel_ecmascript *ecmascript,
                                                  ta_buffer *out);
TA_API ta_status ta_datamodel_ecmascript_restore(ta_datamodel_ecmascript *ecmascript,
                                                 ta_bytes snapshot);
/* Why the last serialize (hibernate/export) or snapshot of this datamodel
 * reported TA_ERROR_UNSUPPORTED, naming the variable and, for functions, the
 * line in the document — e.g. "variable 'helper' cannot be stored: it holds a
 * function (line 12); ...". "" after a success. Valid until the next call. */
TA_API const char *ta_datamodel_ecmascript_last_error(const ta_datamodel_ecmascript *ecmascript);
/* Bytes the engine currently has allocated (all sessions, payloads, code),
 * maintained by the datamodel's allocator: O(1), cheap enough to sample. */
TA_API size_t ta_datamodel_ecmascript_heap_bytes(const ta_datamodel_ecmascript *ecmascript);
/* The blocks those bytes are in, counted the same way. Each block also
 * holds the allocator's header, two size_t (16 bytes on 64-bit targets, 8
 * on wasm32), which the heap's bytes leave out: the engine holds
 * heap_bytes + blocks * 2 * sizeof(size_t) of the C library's allocator. */
TA_API size_t ta_datamodel_ecmascript_heap_blocks(const ta_datamodel_ecmascript *ecmascript);
/* Change the engine's memory limit (bytes; 0 = the default, SIZE_MAX: none),
 * as memory_limit in the config: allocations beyond it fail the evaluation
 * (error.execution). */
TA_API void ta_datamodel_ecmascript_set_memory_limit(ta_datamodel_ecmascript *ecmascript,
                                                     size_t bytes);
/* The underlying JSRuntime* (for embedders adding their own globals). */
TA_API void *ta_datamodel_ecmascript_js_runtime(ta_datamodel_ecmascript *ecmascript);

#if defined(__wasm__)
/* tinyactors_datamodel_ecmascript.wasm imports nothing, so it has no clock
 * of its own: Date, performance.now() and the random seed read the time the
 * host last set here, in milliseconds since the Unix epoch (0 until then).
 * A browser host calls it with Date.now() before each run. */
TA_API void ta_datamodel_ecmascript_wasm_set_time(double unix_milliseconds);
#endif

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_DATAMODEL_ECMASCRIPT_H */
