/* addon.h — what the addon's files share: core's headers, Node-API's, and
 * the state of one environment (the main thread, or a worker). Node-API
 * values mean nothing outside the environment that made them, so every
 * piece of state lives in the environment's instance data
 * (napi_set_instance_data) or on a Host, never in a global (AGENTS.md,
 * "C rules"). */
#ifndef TA_NAPI_ADDON_H
#define TA_NAPI_ADDON_H

#include <node_api.h>

#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"
#include "tinyactors_runtime.h"

/* ---- memory (AGENTS.md, "The binding's memory") ------------------------------------ */

/* A buffer the addon reuses, grown by doubling, never shrunk; freed once,
 * with its owner. Only host.c and handles.c call the allocator (the lint
 * rule allocation-in-tables-and-host). */
typedef struct ta_napi_buffer {
    char *pointer;
    size_t capacity;
} ta_napi_buffer;
/* Room for `need` bytes (the contents kept); 0 when the allocator refused. */
int ta_napi_buffer_reserve(ta_napi_buffer *buffer, size_t need);
void ta_napi_buffer_free(ta_napi_buffer *buffer);

/* The buffers one owner converts through: an environment (encode, decode,
 * compile), a Host (its calls, requests, effects) or a native datamodel
 * (its entries). Each is reused by every call; views into them last until
 * the next use of the same buffer. */
typedef struct ta_napi_scratch {
    napi_env env;
    ta_napi_buffer text;        /* UTF-8 views a call hands core (ta_napi_values_texts) */
    ta_napi_buffer utf16;       /* a string's code units while it is converted */
    ta_napi_buffer strings;     /* the writer's text and bytes */
    ta_napi_buffer tokens;      /* the writer's tokens */
    ta_napi_buffer read_tokens; /* the reader's tokens */
    ta_napi_buffer out;         /* the writer's CBOR */
    ta_napi_buffer wire;        /* what core writes: replies, envelopes, to_wire */
    ta_napi_buffer events;      /* sendMany's events; compile's sources */
    ta_napi_buffer values;      /* napi_values a call converts at once */
    uint32_t writing;           /* an encode runs: another on this scratch is refused */
} ta_napi_scratch;
/* Free every buffer of `scratch` (with its owner). */
void ta_napi_scratch_free(ta_napi_scratch *scratch);

/* One environment's state: made at module init, freed with the
 * environment. */
typedef struct ta_napi_module {
    napi_ref error_class; /* TinyactorsError */
    ta_napi_scratch scratch;
    uint32_t compiling;  /* compile() runs: one from its resolve is refused */
    ta_napi_buffer self; /* this struct's memory */
} ta_napi_module;

/* The tags of the objects the addon wraps (napi_type_tag_object), so a
 * method called on another object is refused rather than misread. */
extern const napi_type_tag ta_napi_host_tag;
extern const napi_type_tag ta_napi_clock_tag;

typedef struct ta_napi_clock ta_napi_clock;

/* ---- tables of rows (src/datamodel/handles.c) ------------------------------------ */

/* Every row starts with this. A handle is the row's generation and slot,
 * never a pointer (bindings.md, section 8): a stale one is refused. */
typedef struct ta_napi_row {
    uint32_t generation;
    uint32_t references; /* 0: the row is free */
    uint32_t next_free;
    uint32_t pad;
} ta_napi_row;

/* Rows of one size, grown by doubling, freed slots reused, freed once.
 * A row's address lasts until the next add to its table. */
typedef struct ta_napi_table {
    ta_napi_buffer rows;
    size_t row_size;
    uint32_t count, free;
} ta_napi_table;

/* A new row (zeroed but its header, one reference): its handle, 0 when the
 * allocator refused. */
uint64_t ta_napi_table_add(ta_napi_table *table);
/* The live row `handle` names, or NULL. */
void *ta_napi_table_get(const ta_napi_table *table, uint64_t handle);
/* Free row `handle`: its slot goes to the next add. */
void ta_napi_table_remove(ta_napi_table *table, uint64_t handle);
/* Each live row, for a free path: `index` from 0 while it answers a row. */
void *ta_napi_table_row(const ta_napi_table *table, uint32_t index);
void ta_napi_table_free(ta_napi_table *table);

/* References dropped once the outer call into core returns: dropping the
 * last one can let JavaScript run (a finalizer), and core calls release
 * outside a run too (bindings.md, section 8). */
typedef struct ta_napi_drops {
    napi_env env;
    ta_napi_buffer items;
    uint32_t count;
} ta_napi_drops;
void ta_napi_defer_drop(ta_napi_drops *drops, napi_ref reference);
/* Drop them; how many. */
uint32_t ta_napi_drops_drain(ta_napi_drops *drops);
/* Drain, then free the list (with its owner). */
void ta_napi_drops_free(ta_napi_drops *drops);

/* ---- the native "ecmascript" datamodel (src/datamodel/) ---------------------------- */

/* Its state, a part of its Host: tables of rows for payloads, sessions,
 * charts and <foreach> iterations, and its scratch. */
typedef struct ta_napi_datamodel {
    napi_env env;
    uint32_t live;
    ta_napi_table payloads, sessions, charts, iterations;
    ta_napi_drops drops;
    ta_napi_scratch scratch;
    napi_ref helpers; /* the helper functions of datamodel.c */
    napi_ref in;      /* In(), shared by every scope */
    /* the entry running, for In() */
    ta_datamodel_environment *environment;
    uint64_t chart;
} ta_napi_datamodel;

/* Make its helpers; 0 when Node-API failed. */
int ta_napi_datamodel_init(ta_napi_datamodel *datamodel, napi_env env);
/* After ta_host_free: its references dropped, its tables freed. */
void ta_napi_datamodel_release(ta_napi_datamodel *datamodel);
ta_datamodel_binding ta_napi_datamodel_binding(ta_napi_datamodel *datamodel);
/* Drop the references released during the outer call into core that just
 * returned; how many. A datamodel not live: none. */
uint32_t ta_napi_datamodel_drain(ta_napi_datamodel *datamodel);

/* An image the host runs: the bytes compile() wrote and the opened view
 * core reads, both held in ArrayBuffers until TA_HOST_EFFECT_SEGMENT_FREED
 * or the host goes (bindings.md, section 6: hold, don't copy). */
typedef struct ta_napi_image {
    ta_napi_row row;
    napi_ref bytes; /* the image's Uint8Array or ArrayBuffer */
    napi_ref view;  /* an ArrayBuffer holding the ta_image */
    uint32_t segment;
} ta_napi_image;

/* A Host: one ta_host, its datamodel and the loop's state. */
typedef struct ta_napi_host {
    napi_env env;
    ta_host *host;
    ta_datamodel_ecmascript *quickjs;
    ta_napi_datamodel native;
    ta_datamodel_binding binding;
    ta_napi_table images; /* ta_napi_image rows */
    ta_napi_scratch scratch;
    uint64_t ticks_per_second; /* host.config's system.ticksPerSecond */
    napi_ref handlers;         /* the `on` object */
    napi_ref clock_object;     /* a SimulatedClock, or NULL: the runtime's clock */
    ta_napi_clock *clock;
    napi_ref self;      /* the Host object: weak, strong while a timer or an immediate is armed */
    uint32_t held;      /* self is strong */
    napi_ref wake;      /* the function the timer and the immediate call */
    napi_ref timer;     /* the pending setTimeout, or NULL */
    napi_ref immediate; /* the pending setImmediate, or NULL */
    napi_ref error;     /* the first exception a handler threw in this turn */
    ta_time timer_deadline;
    uint32_t running;   /* the loop runs: inputs set `again` */
    uint32_t again;     /* an input arrived while the loop ran */
    uint32_t closing;   /* close() was called while the loop ran */
    uint32_t closed;
    uint32_t batching;  /* sendMany fills the host's buffers: another from a getter is refused */
} ta_napi_host;

/* The time a host's turn runs at. */
typedef struct ta_napi_time {
    double milliseconds; /* the clock's: the runtime's, or the simulated time */
    uint64_t wall_ns;
} ta_napi_time;

/* The environment's state. */
ta_napi_module *ta_napi_module_of(napi_env env);

/* ---- errors (module.c) ---------------------------------------------------------- */

/* Whether an exception is pending. */
int ta_napi_exception_pending(napi_env env);
/* Throw a TinyactorsError for a status core answered (`status`: the
 * ta_status) to `function` (the core function, or the API item that
 * refused an argument): "ta_host_spawn: TA_ERROR_INVALID", as the Python
 * binding writes it; unless an exception is pending already: then that
 * one propagates. */
void ta_napi_throw_status(napi_env env, const char *function, ta_status status);
/* Throw a TinyactorsError for the `error` member of a refused request or
 * config ({"code", "message", "data": {"reason", "path"}}, requests.md):
 * its message, code, reason and path, unchanged. */
void ta_napi_throw_reply(napi_env env, napi_value error);

/* ---- arguments --------------------------------------------------------------- */

/* A method's `this` and up to four arguments (undefined when absent). */
typedef struct ta_napi_call {
    napi_value self;
    size_t argc;
    napi_value argv[4];
} ta_napi_call;
/* Read `info` into `call`; 0 when Node-API failed (an exception is pending). */
int ta_napi_call_read(napi_env env, napi_callback_info info, ta_napi_call *call);

/* ---- the host (host.c), the loop (loop.c), effects (effects.c), requests (request.c) -- */

napi_value ta_napi_host_class(napi_env env);
napi_value ta_napi_clock_class(napi_env env);
napi_value ta_napi_compile(napi_env env, napi_callback_info info);

/* An input was taken: run the turn (a simulated clock), or arm one on the
 * event loop; while the loop runs, it runs again before it returns. */
void ta_napi_loop_input(ta_napi_host *host);
/* Run the turn now, at the clock's time. */
void ta_napi_loop_turn(ta_napi_host *host);
/* Disarm the timer and the immediate (close). */
void ta_napi_loop_disarm(ta_napi_host *host);
/* The Host a method is called on (request.c), or NULL with an exception. */
ta_napi_host *ta_napi_host_of(napi_env env, napi_callback_info info, ta_napi_call *call);
/* Before an input that depends on the time (spawn, deliver): the clocks. */
void ta_napi_loop_advance(ta_napi_host *host);
/* Free the host's core and its datamodel (close, or its finalizer). */
void ta_napi_host_release(ta_napi_host *host);
/* Forget a host closing (a simulated clock). */
void ta_napi_clock_forget(ta_napi_clock *clock, ta_napi_host *host);
/* Remember a host the clock drives. */
ta_status ta_napi_clock_add(ta_napi_clock *clock, ta_napi_host *host);
/* The clock's time for a host: simulated, or the runtime's. */
ta_napi_time ta_napi_loop_now(ta_napi_host *host);

/* Handle effect `index` of the batch the run published. */
void ta_napi_effect_handle(ta_napi_host *host, uint32_t index);

/* request(method, params): the method's result, or a TinyactorsError. */
napi_value ta_napi_request(napi_env env, napi_callback_info info);
/* Read the host's tick rate (host.config) into host->ticks_per_second. */
ta_status ta_napi_host_read_ticks(ta_napi_host *host);
/* A refused config: the error ta_host_create wrote, thrown. */
void ta_napi_throw_config(napi_env env, ta_bytes error);

/* ---- values (values.c): the value table of docs/values.md ------------------------ */

/* `value` as CBOR: a view into scratch->out, until the next encode on it.
 * TA_ERROR_STATE: an encode on this scratch runs already (a getter or a
 * toJSON that encodes on the same owner). */
ta_status ta_napi_values_encode(ta_napi_scratch *scratch, napi_value value, ta_bytes *out);
/* The value of `cbor`; TA_ERROR_INVALID when it is not a neutral value. */
ta_status ta_napi_values_decode(ta_napi_scratch *scratch, ta_bytes cbor, napi_value *out);
/* The bytes of an ArrayBuffer, a typed array or a DataView, borrowed for
 * the call (the value's own memory); 0 when `value` is none of them. */
int ta_napi_values_bytes(napi_env env, napi_value value, ta_bytes *out);
/* Strings as UTF-8 views into scratch->text, valid until the next texts on
 * it: `count` values, each a string (undefined: an empty view). A lone
 * surrogate, or a value that is neither, is TA_ERROR_INVALID. No
 * JavaScript runs, so read them after whatever runs some. */
typedef struct ta_napi_texts {
    const napi_value *values;
    uint32_t count;
    ta_string_view *out;
} ta_napi_texts;
ta_status ta_napi_values_texts(ta_napi_scratch *scratch, const ta_napi_texts *texts);
/* A string as UTF-8 in a new ArrayBuffer (`holder`, the caller keeps it
 * alive), for text that must outlive the scratch's next use. */
typedef struct ta_napi_held_text {
    napi_value holder;
    ta_string_view text;
} ta_napi_held_text;
ta_status ta_napi_values_hold_text(ta_napi_scratch *scratch, napi_value value, ta_napi_held_text *out);
/* A payload of the host's datamodel holding `value` (0 for undefined). */
ta_status ta_napi_values_payload(ta_napi_host *host, napi_value value, ta_payload *out);
/* The value of payload `payload` of datamodel `datamodel` (TA_NIL or 0: undefined). */
typedef struct ta_napi_payload_read {
    uint32_t datamodel;
    ta_payload payload;
} ta_napi_payload_read;
ta_status ta_napi_values_of_payload(ta_napi_host *host, const ta_napi_payload_read *read, napi_value *out);
/* Bytes as a new Uint8Array. */
napi_status ta_napi_values_uint8array(napi_env env, ta_bytes bytes, napi_value *out);
/* The exports encode(value) and decode(bytes). */
napi_value ta_napi_encode(napi_env env, napi_callback_info info);
napi_value ta_napi_decode(napi_env env, napi_callback_info info);

/* What core writes into a ta_buffer, grown until it fits: `write` called
 * with `context` and scratch->wire, again in the length it asked for. A
 * view of what it wrote, until the next use of the wire buffer. */
typedef struct ta_napi_writing {
    ta_status (*write)(void *context, ta_buffer *out);
    void *context;
} ta_napi_writing;
ta_status ta_napi_wire(ta_napi_scratch *scratch, const ta_napi_writing *writing, ta_bytes *out);

#endif /* TA_NAPI_ADDON_H */
