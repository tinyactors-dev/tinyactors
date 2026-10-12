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

/* One environment's state: made at module init, freed with the
 * environment. */
typedef struct ta_napi_module {
    napi_ref error_class; /* TinyactorsError */
} ta_napi_module;

/* The tags of the objects the addon wraps (napi_type_tag_object), so a
 * method called on another object is refused rather than misread. */
extern const napi_type_tag ta_napi_host_tag;
extern const napi_type_tag ta_napi_clock_tag;

typedef struct ta_napi_clock ta_napi_clock;

/* An image the host runs: its bytes, copied, and the view core reads
 * (both stay put until TA_HOST_EFFECT_SEGMENT_FREED or the host goes). */
typedef struct ta_napi_image {
    void *bytes;
    size_t length;
    uint32_t segment;
    uint32_t live;
    ta_image image;
} ta_napi_image;

/* A Host: one ta_host, its datamodel and the loop's state. */
typedef struct ta_napi_host {
    napi_env env;
    ta_host *host;
    ta_datamodel_ecmascript *quickjs;
    ta_datamodel_binding binding;
    ta_napi_image **images;
    uint32_t image_count, image_capacity;
    uint64_t ticks_per_second; /* the config's, 0 when it gave none (ROADMAP.md, Requests to core) */
    napi_ref handlers;         /* the `on` object */
    napi_ref clock_object;     /* a SimulatedClock, or NULL: the runtime's clock */
    ta_napi_clock *clock;
    napi_ref wake;      /* the function the timer and the immediate call */
    napi_ref timer;     /* the pending setTimeout, or NULL */
    napi_ref immediate; /* the pending setImmediate, or NULL */
    napi_ref error;     /* the first exception a handler threw in this turn */
    ta_time timer_deadline;
    uint32_t running;   /* the loop runs: inputs set `again` */
    uint32_t again;     /* an input arrived while the loop ran */
    uint32_t closing;   /* close() was called while the loop ran */
    uint32_t closed;
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
/* A refused config: the error ta_host_create wrote, thrown. */
void ta_napi_throw_config(napi_env env, ta_bytes error);

/* ---- values (values.c): the value table of docs/values.md ------------------------ */

/* `value` as CBOR in out->pointer (malloc'd; the caller frees it). */
ta_status ta_napi_values_encode(napi_env env, napi_value value, ta_buffer *out);
/* The value of `cbor`; TA_ERROR_INVALID when it is not a neutral value. */
ta_status ta_napi_values_decode(napi_env env, ta_bytes cbor, napi_value *out);
/* A copy of the bytes of an ArrayBuffer, a typed array or a DataView
 * (malloc'd: free out->pointer); 0 when `value` is none of them. */
int ta_napi_values_bytes(napi_env env, napi_value value, ta_bytes *out);
/* A string's text as UTF-8 (malloc'd: free out->pointer); a lone surrogate,
 * or a value that is not a string, is TA_ERROR_INVALID. */
ta_status ta_napi_values_text(napi_env env, napi_value value, ta_string_view *out);
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

#endif /* TA_NAPI_ADDON_H */
