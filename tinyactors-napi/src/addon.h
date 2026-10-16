/* The addon @tinyactors/node: what its files share. Every name is ours,
 * ta_napi_* (AGENTS.md, "Names"); the platform's names appear only where
 * Node-API requires them (module.c's NAPI_MODULE_INIT).
 *
 * State lives in the environment's instance data (ta_napi_instance, one per
 * main thread or worker; AGENTS.md, "C rules": no mutable globals) or, from
 * the host step on, on the Host. */
#ifndef TA_NAPI_ADDON_H
#define TA_NAPI_ADDON_H

#include <node_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"

/* What one environment holds, made at module init and freed at the
 * environment's teardown (napi_set_instance_data's finalizer).
 *
 *   allocator  malloc and free, each block reported to the engine with
 *              napi_adjust_external_memory (AGENTS.md, "The host's
 *              allocator"); its context is this instance;
 *   arena      the scratch of the module's own calls (encode): a mark at
 *              entry, a release at exit, so a nested call (a getter that
 *              calls encode) allocates above the outer one; its chunks are
 *              kept, so a warmed call allocates nothing;
 *   error      the class TinyactorsError, exported;
 *   object     Object.prototype of this environment's realm: a plain
 *              object's prototype (or null);
 *   keys       an object holding the symbols the addon keys its own
 *              properties with, which no export reaches: `handlers`, the
 *              key of a Host's handlers on its wrapper (host.c), and
 *              `clock`, the key of a SimulatedClock's state (loop.c). Node-API
 *              has no private symbols, so a plain Symbol held here is the
 *              key; a reference to an object, not to the symbol, since
 *              references to symbols need Node-API 10;
 *   turn       the function the event loop calls for a turn of a host
 *              (loop.c), given the Host as its argument: one per
 *              environment, so scheduling a turn makes no function;
 *   compiled   the class of a compile result (compile.c), made only by
 *              the compile calls;
 *   compile_error, resolve_error  the classes CompileError and
 *              ResolveError, subclasses of TinyactorsError, exported;
 *   address    the class Address, exported (address.c);
 *   send, entry  the classes of the thin views a handler gets, a Send and a
 *              LogEntry (effects.c), made only by the drain;
 *   rejected   the function a handler's promise calls when it rejects
 *              (effects.c), bound to its view: it fails an unanswered send
 *              and throws the rejection on;
 *   vm, realm  node:vm (process.getBuiltinModule) and the vm.Script of
 *              the realm's script every context of the native datamodel
 *              runs (datamodel/realms.c), made on first use;
 *   realms     a WeakSet of those contexts' Object.prototype: a plain
 *              object of theirs is a value (values.c);
 *   in         In(), the function every context's scopes hold: it reads
 *              `evaluating`, the native datamodel whose entry runs now
 *              (NULL outside one), so it holds no pointer of its own;
 *   stringify  JSON.stringify, for <log> of an object. */
typedef struct ta_napi_instance {
    napi_env env;
    ta_allocator allocator;
    ta_growable_arena arena;
    napi_ref error;
    napi_ref object;
    napi_ref keys;
    napi_ref turn;
    napi_ref compiled;
    napi_ref compile_error;
    napi_ref resolve_error;
    napi_ref address;
    napi_ref send, entry;
    napi_ref rejected;
    napi_ref vm, realm, realms, in, stringify;
    struct ta_napi_native *evaluating;
} ta_napi_instance;

/* The environment's instance, or NULL (and an exception pending) before
 * module init finished. */
ta_napi_instance *ta_napi_instance_of(napi_env env);

/* The allocator every host and the module's arena grow through: malloc
 * and free, each block reported to `env`'s engine with
 * napi_adjust_external_memory, a basic-env call, so it may run in a
 * finalizer. Its context is `env` itself and nothing else, so a host's
 * finalizer frees through it whatever the instance's state at the
 * environment's teardown. */
ta_allocator ta_napi_allocator(napi_env env);

/* A refusal: the message and the status core names (ta_status_name). */
typedef struct ta_napi_refusal {
    ta_status status;
    const char *message;
} ta_napi_refusal;

/* Throws a TinyactorsError with `message` and `status` set to
 * ta_status_name(refusal->status); returns NULL, for a napi_callback to
 * return. */
napi_value ta_napi_throw(napi_env env, const ta_napi_refusal *refusal);

/* An error core wrote, {"code", "message", "data": {"reason", "path"}}
 * (a refused config's, a reply's), and the status the call gave. */
typedef struct ta_napi_core_error {
    napi_value error; /* the decoded map; anything else: no code, reason, path */
    ta_status status;
} ta_napi_core_error;

/* Throws a TinyactorsError with the error's message, `code`, `reason` and
 * `path`, and `status` ta_status_name(status), none for TA_OK (a reply's
 * error); returns NULL. */
napi_value ta_napi_throw_core_error(napi_env env, const ta_napi_core_error *error);

/* ---- values.c: the value table (docs/values.md) ------------------------- */

/* Where a value is written: the CBOR writer, and the arena the call's
 * text goes to (the module's for encode, the host's for a host's call). */
typedef struct ta_napi_writing {
    ta_cbor_writer *writer;
    ta_growable_arena *arena;
    bool wire; /* the native datamodel's to_wire: values.txt's ECMAScript rules (values.c) */
} ta_napi_writing;

/* Writes `value` into writing->writer as the value table's CBOR. On a
 * refusal a TinyactorsError is thrown (or the exception a getter threw
 * stays pending) and the result is false; the writer is then the caller's
 * to free, its contents partial. Allocates the call's text in
 * writing->arena: the caller holds a mark around the call. */
bool ta_napi_value_write(napi_env env, napi_value value, const ta_napi_writing *writing);

/* The value of one CBOR item filling `cbor`, or NULL with a TinyactorsError
 * thrown. The bytes are read during the call only. */
napi_value ta_napi_value_read(napi_env env, ta_bytes cbor);

/* A string as UTF-8 in text->arena (the copy Node-API forces), a lone
 * surrogate refused: text->view. A value that is not a string throws a
 * TypeError saying text->what; a refusal a TinyactorsError. The caller
 * holds a mark around the call. */
typedef struct ta_napi_text {
    ta_growable_arena *arena;
    const char *what;
    ta_string_view view; /* out */
} ta_napi_text;
bool ta_napi_text_of(napi_env env, napi_value value, ta_napi_text *text);

/* A block kept between calls (the native datamodel's last output): a
 * buffer grown through `allocator`, its old block freed; `view` the text
 * a call wrote into it. */
typedef struct ta_napi_room {
    ta_buffer *buffer;
    const ta_allocator *allocator;
    ta_string_view view; /* out */
} ta_napi_room;
/* At least `capacity` bytes in room->buffer, its contents not kept. */
bool ta_napi_room_grow(ta_napi_room *room, size_t capacity);
/* A string's UTF-8 in the room (room->view), a lone surrogate refused
 * (TinyactorsError TA_ERROR_INVALID); false with an exception pending. */
bool ta_napi_text_kept(napi_env env, napi_value value, ta_napi_room *room);

/* Optional text: undefined or null is none (an empty view), a string its
 * UTF-8 in text->arena, anything else a TypeError saying text->what. */
bool ta_napi_optional_text(napi_env env, napi_value value, ta_napi_text *text);

/* The exports encode(value) and decode(bytes). */
napi_value ta_napi_encode(napi_env env, napi_callback_info info);
napi_value ta_napi_decode(napi_env env, napi_callback_info info);

/* ---- host.c: the Host (AGENTS.md, "The binding's memory") ------------- */

/* The rows of the host's tables (ta_table, tinyactors/table.h): handles
 * are kept, never row pointers across an add. Each holds at most one
 * napi_ref, deleted before its row is removed and when the tables are
 * freed with the host (napi_delete_reference is a basic-env call). The
 * native datamodel (p1-datamodel) and the typed methods (p1-methods) fill
 * them; the host makes and frees the tables.
 *
 *   chart    a chart the native datamodel attached: its compiled
 *            functions, made lazily;
 *   session  a session's scope (or its context), and its chart's row;
 *   payload  a value crossing between actors: its reference and count;
 *   loop     a <foreach> in progress: the items it walks;
 *   pending  a typed method's message: its handlers are kept on the
 *            wrapper (never by a strong napi_ref, which would keep a host
 *            whose handlers close over it from the collector); `slot` is
 *            their place there. */
typedef struct ta_napi_chart_row {
    napi_ref functions;   /* its record (datamodel/datamodel.h) */
    const ta_image *image; /* the image it is attached from, until chart_detach */
    uint32_t chart;       /* its index in the image */
    uint32_t flags;       /* TA_NAPI_CHART_* */
} ta_napi_chart_row;

typedef struct ta_napi_session_row {
    napi_ref scope;
    uint64_t chart;
} ta_napi_session_row;

typedef struct ta_napi_payload_row {
    napi_ref value; /* the value, or a one-element array holding a primitive */
    uint32_t count;
    uint32_t boxed; /* `value` is that array: Node-API 8 references objects only */
} ta_napi_payload_row;

typedef struct ta_napi_loop_row {
    napi_ref items;
} ta_napi_loop_row;

typedef struct ta_napi_pending_row {
    uint32_t slot;
} ta_napi_pending_row;

/* The native "ecmascript" datamodel of a host (src/datamodel/; AGENTS.md,
 * "The native "ecmascript" datamodel"): its rows are the host's tables.
 *
 *   shared       the record [context, kit] of the host's one shared
 *                node:vm context, made on first use;
 *   drops        an array holding the values whose last reference went,
 *                emptied by the flush after the outer call into core;
 *   strings      ["platform", "internal", "external", the I/O processors'
 *                types...]: _event's strings by core's ids;
 *   environment  the evaluating entry's (In() reads it);
 *   output       the datamodel's last output (eval_string, log, to_wire),
 *                valid until its next call;
 *   freeing      the host is being freed: rows go with basic-env calls
 *                only (a finalizer may be freeing it). */
struct ta_napi_host;
typedef struct ta_napi_native {
    struct ta_napi_host *host;
    napi_ref shared, drops, strings;
    ta_datamodel_environment *environment;
    ta_buffer output;
    bool freeing;
} ta_napi_native;

/* The datamodels a host can hold: one of the two "ecmascript" ones
 * (AGENTS.md, "The native "ecmascript" datamodel"). */
typedef enum ta_napi_datamodel_kind {
    TA_NAPI_DATAMODEL_NATIVE = 0,
    TA_NAPI_DATAMODEL_QUICKJS = 1,
} ta_napi_datamodel_kind;

/* One Host, wrapped by its JavaScript object (napi_wrap, type-tagged).
 * Made with the host's allocator, freed by the wrapper's finalizer; the
 * core host inside is freed by close() or, as a fallback, by the
 * finalizer.
 *
 *   env         the environment that made it: every call checks it;
 *   self        a weak reference to the wrapper, whose own property under
 *               the instance's `handlers` symbol holds the handlers;
 *   allocator   ta_napi_allocator(env), the context of every block;
 *   host        core's host; NULL once freed (`closed`);
 *   datamodels  the environment's one datamodel (core reads the list for
 *               the host's life);
 *   arena       the scratch of the host's calls, a mark per call;
 *   depth       calls of the Host on the stack (ta_napi_enter, _leave);
 *   turning     a turn is running: close() waits for its end;
 *   closing     close() was asked while a call or a turn ran;
 *   batch       a take is open, from it to its ack: an output taken then
 *               joins it, one taken without opens a batch of its own;
 *   inputs      an input came since the turn's last run: turn again;
 *   retake      a take of traces (1) or logs (2) the allocator refused:
 *               retried at the next turn (core gives no second effect);
 *   acks        acks so far, bumped right after each ta_host_effects_ack:
 *               a thin view made at another count is stale;
 *   ticks_per_second  system.ticksPerSecond, read once at creation;
 *   deadline    the last idle run's next_deadline (UINT64_MAX: none);
 *   wall        the last wall time given to core: it never goes back;
 *   pool, fault the detail of the last TA_RUN_EXHAUSTED, TA_RUN_FAULT;
 *   clock       a SimulatedClock's time (an ArrayBuffer of one uint64 of
 *               nanoseconds), or NULL: the runtime's clocks;
 *   immediate, timer  the handles of the one pending setImmediate and the
 *               one setTimeout at the deadline, or NULL; each holds the
 *               Host as its callback's argument, so a pending timer keeps
 *               it from the collector;
 *   idle        the deferred of the promise idle() returns until the next
 *               idle turn (`idle_promise`, the same to every caller);
 *   error       the turn's first error (a handler threw: any value, in a
 *               one-element array), reported after
 *               the turn;
 *   encoder     the effective config has `telemetry`;
 *   telemetry   a telemetry handler was given (the last take goes to it);
 *   processors  the effective config's ioProcessors, numbered from 1 as
 *               the effects' IO_PROCESSOR numbers them (0: the SCXML
 *               processor, the transport's). */
typedef struct ta_napi_host {
    napi_env env;
    ta_napi_native native;
    napi_ref self;
    ta_allocator allocator;
    ta_host *host;
    ta_napi_datamodel_kind datamodel;
    ta_datamodel_ecmascript *quickjs;
    ta_datamodel_binding datamodels[1];
    ta_growable_arena arena;
    ta_allocator arena_allocator;
    ta_table charts, sessions, payloads, loops, pending;
    uint32_t depth;
    bool turning, closing, batch, inputs;
    uint8_t retake;
    uint64_t acks;
    uint64_t ticks_per_second;
    uint64_t deadline, wall;
    uint32_t pool;
    ta_status fault;
    napi_ref clock, immediate, timer, idle_promise, error;
    napi_deferred idle;
    bool encoder, telemetry;
    uint32_t processors;
} ta_napi_host;

/* A call of the Host: the host and its arena's mark at entry. */
typedef struct ta_napi_call {
    ta_napi_host *host;
    ta_growable_arena_mark mark;
} ta_napi_call;

/* The Host `self` stands for, checked: its type tag (a method called on
 * another object is refused, never misread), the environment that made
 * it, and, with `open`, that it is not closed. NULL with an exception
 * pending. */
typedef struct ta_napi_host_check {
    napi_value self;
    bool open;
} ta_napi_host_check;
ta_napi_host *ta_napi_host_of(napi_env env, const ta_napi_host_check *check);

/* Enters a call of an open host (its arena's mark, depth); false with an
 * exception pending. Every enter is matched by a leave on every path. */
bool ta_napi_enter(napi_env env, napi_value self, ta_napi_call *call);
/* Releases the arena to the call's mark; the outermost call does a close
 * asked meanwhile. */
void ta_napi_leave(ta_napi_call *call);

/* A value for a session's datamodel (AGENTS.md, "The binding's memory"):
 * `value` made a payload of the datamodel of `session`'s chart, or, with
 * session 0, of `chart`'s. Undefined is no payload (0). On a quickjs host
 * it is the value's CBOR in the host's arena, read by
 * ta_datamodel_ecmascript_payload_from_cbor. False with an exception
 * pending (a session that is not there: TA_ERROR_STALE; a chart whose
 * datamodel takes no data: TypeError). */
typedef struct ta_napi_payload_for {
    uint64_t session;
    ta_host_chart_ref chart;
    napi_value value;
    ta_payload payload; /* out */
} ta_napi_payload_for;
bool ta_napi_host_payload(napi_env env, ta_napi_host *host, ta_napi_payload_for *target);
/* A payload the host refused, released: it is still ours. */
void ta_napi_host_payload_release(ta_napi_host *host, ta_payload payload);

/* Defines the class Host on `exports`. */
napi_status ta_napi_host_define(napi_env env, napi_value exports);

/* ---- datamodel/: the native "ecmascript" datamodel ----------------------- */

const ta_datamodel_vtable *ta_napi_native_vtable(void);
/* `value` copied into the shared context, a payload with one reference;
 * false with an exception pending (a value that cannot cross: TypeError). */
bool ta_napi_native_payload(ta_napi_native *native, napi_value value, ta_payload *out);
/* A payload's value itself (the row's, not a copy); undefined for none. */
napi_value ta_napi_native_value(ta_napi_native *native, ta_payload payload);
/* The drops let go: after the outer call into core returns. */
void ta_napi_native_flush(ta_napi_native *native);
/* After ta_host_free: the datamodel's own references and output, with
 * basic-env calls only. */
void ta_napi_native_free(ta_napi_native *native);

/* to_wire's CBOR of `value` in `room` (values.c's wire mode), the call's
 * text in `arena` under a mark of its own; false with an exception
 * pending. */
typedef struct ta_napi_wiring {
    ta_napi_room room;
    ta_growable_arena *arena;
    ta_bytes out; /* out: a view of the room */
} ta_napi_wiring;
bool ta_napi_value_wire(napi_env env, napi_value value, ta_napi_wiring *wiring);

/* ---- address.c: Address (AGENTS.md, "Other systems") -------------------- */

/* An Address of `text` (UTF-8), or NULL with an exception pending. */
napi_value ta_napi_address_new(napi_env env, ta_string_view text);
/* `value`'s text when it is an Address, else `value` itself: what an input
 * that takes an address or a string reads. */
napi_value ta_napi_address_text(napi_env env, napi_value value);
/* Defines the class Address on `exports`. */
napi_status ta_napi_address_define(napi_env env, napi_value exports);

/* ---- http.c: BasicHTTP's codec (tinyactors/basic_http.h) ---------------- */

/* encodeHTTPPost and parseHTTPRequest on `exports`. */
napi_status ta_napi_http_define(napi_env env, napi_value exports);

/* ---- compile.c: the compiler ------------------------------------------- */

/* compileCharts, compileSystem, CompileError and ResolveError on
 * `exports`; the class of a compile result in the instance. */
napi_status ta_napi_compile_define(napi_env env, napi_value exports);

/* ---- loop.c: the loop (AGENTS.md, "The loop") ----------------------------- */

/* What a drain hands each effect to. `reply` is set while the host is
 * made: the WIRE of the host.config REPLY, read before the ack. */
typedef struct ta_napi_drain_state {
    ta_napi_host *host;
    napi_value reply; /* out: the last REPLY's decoded WIRE, or NULL */
    bool bounded;     /* out: it stopped at its bound of batches */
} ta_napi_drain_state;

/* A take: the drain's state and the take's records, until the ack. */
typedef struct ta_napi_batch {
    ta_napi_drain_state *state;
    const ta_host_effect *effects;
} ta_napi_batch;

/* The Host's wrapper; NULL when it is gone. */
napi_value ta_napi_self(napi_env env, const ta_napi_host *host);

/* One drain: take, handle every effect of the batch, ack (acks bumped
 * right after), and take again until a take is empty or the bound of
 * batches is reached, checked before a take. Never nested; a handler
 * never takes or acks. A handler's error is kept on the host (`error`). */
void ta_napi_drain(napi_env env, ta_napi_drain_state *state);

/* Keeps the pending exception as the host's error, unless one is kept
 * already (the first is reported); the exception is cleared. */
void ta_napi_keep_error(napi_env env, ta_napi_host *host);
/* Throws the kept error, cleared; false when none was kept. */
bool ta_napi_throw_kept(napi_env env, ta_napi_host *host);

/* Both clocks, converted with ticksPerSecond: before an input that
 * depends on them and at creation (a turn does it before every run). */
void ta_napi_advance(napi_env env, ta_napi_host *host);

/* An input from outside a turn schedules one turn (setImmediate, one
 * pending at a time); from a turn's handler it makes the turn go on. */
void ta_napi_schedule(napi_env env, ta_napi_host *host);

/* The host made with options.clock (a SimulatedClock, or undefined):
 * its time read from then on, the host held weakly by the clock. False
 * with a TypeError thrown for anything else. After the wrap. */
bool ta_napi_clock_attach(napi_env env, ta_napi_host *host, napi_value clock);

/* close()'s loop side, before the host is freed: the timers cleared, a
 * waiting idle() rejected, and with a telemetry handler trace.end and the
 * last take of traces and logs (bounded). */
void ta_napi_loop_stop(napi_env env, ta_napi_host *host);

/* ---- effects.c: the effects (AGENTS.md, "Lifetimes and thin views") ------ */

/* One effect of the open batch, in a handle scope of its own: a SEND to
 * its I/O processor's handler (or the transport's) as a thin Send view, a
 * LOG to the log handler as a LogEntry view, a REQUEST answered in C,
 * TRACES and LOGS taken, a REPLY's WIRE kept for the caller; the rest
 * dropped. A handler's error is kept on the host (ta_napi_keep_error). */
void ta_napi_dispatch(napi_env env, const ta_napi_batch *batch, uint32_t index);

/* A take of traces (`traces`) or logs, dropped until the telemetry step;
 * one the allocator refused is reported once and retried at the next turn
 * (host->retake). */
void ta_napi_take(napi_env env, ta_napi_host *host, bool traces);

/* The classes Send and LogEntry and the rejection function, in the
 * instance; nothing is exported (index.d.ts declares them as interfaces). */
napi_status ta_napi_effects_define(napi_env env, napi_value exports);

/* ---- methods.c: the typed methods (AGENTS.md, "What reaches the application") */

/* A REPLY of the open batch: a typed method's (its id a pending row's
 * handle) has its row removed and its handler called, NULL returned; a
 * stale or unknown id is dropped (NULL). The reply of a message posted with
 * id 0, the creation's host.config, is returned decoded for its caller. A
 * handler's error is kept on the host. */
napi_value ta_napi_reply(napi_env env, ta_napi_host *host, uint32_t index);

/* The typed methods (actorState ... traceSpansDecode) on the prototype of
 * `host_class`. */
napi_status ta_napi_methods_define(napi_env env, napi_value host_class);

/* host.idle() and host.nextDeadline. */
napi_value ta_napi_host_idle(napi_env env, napi_callback_info info);
napi_value ta_napi_host_next_deadline(napi_env env, napi_callback_info info);

/* The turn function in the instance, the clock's symbol, and the class
 * SimulatedClock on `exports`. */
napi_status ta_napi_loop_define(napi_env env, napi_value exports);

#endif /* TA_NAPI_ADDON_H */
