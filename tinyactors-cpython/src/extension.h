/* extension.h — what the files of the tinyactors extension share: the
 * module's state, and each file's functions (AGENTS.md, "Layout").
 *
 * The extension is built twice from the same sources: abi3 against
 * CPython 3.13's limited API (Py_LIMITED_API, set by the build), and
 * without it for the free-threaded test interpreter (AGENTS.md, "C rules").
 * Every CPython function it calls is in both. */
#ifndef TA_PYTHON_EXTENSION_H
#define TA_PYTHON_EXTENSION_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string.h>

#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"
#include "tinyactors_runtime.h"

/* The module's state (PyModule_GetState): every type and object the
 * extension makes once per interpreter. No C globals (AGENTS.md, "No
 * mutable globals"). */
typedef struct ta_python_state {
    PyObject *error_type;     /* tinyactors.TinyactorsError */
    PyObject *address_type;   /* tinyactors.Address */
    PyObject *undefined_type; /* type(tinyactors.UNDEFINED) */
    PyObject *undefined;      /* tinyactors.UNDEFINED */
    PyObject *host_type;      /* tinyactors.Host */
    PyObject *clock_type;     /* tinyactors.SimulatedClock */
    PyObject *time;           /* the time module: the real clock (loop.c) */
    PyObject *event_type;     /* tinyactors.Event, the type of _event (datamodel/datamodel.c) */
} ta_python_state;

/* The state of the module `module`. */
ta_python_state *ta_python_state_of(PyObject *module);

/* ---- errors (module.c) ---------------------------------------------------------- */

/* TinyactorsError for a core call that failed with `status`: its `status`
 * is set, `code`, `reason` and `path` are None, and its message names the
 * function and the status (docs/errors.md). */
void ta_python_raise_status(const ta_python_state *state, const char *function, ta_status status);
/* TinyactorsError from an error reply ({"code", "message", "data":
 * {"reason", "path"}}, docs/requests.md) as a Python dict: `code`,
 * `reason`, `path` and the message unchanged; `status` is the call's
 * when it failed (ta_host_create), None when it answered TA_OK with the
 * error in its reply (ta_host_request). */
typedef struct ta_python_refusal {
    PyObject *error; /* the reply's "error" map */
    ta_status status;
} ta_python_refusal;
void ta_python_raise_reply(const ta_python_state *state, const ta_python_refusal *refusal);

/* ---- values (values.c) ------------------------------------------------------------ */

int ta_python_values_init(PyObject *module, ta_python_state *state);
/* `value` as CBOR into `out` (ta_buffer: TA_ERROR_NO_MEMORY with
 * out->length what it needs, and no Python error). TA_ERROR_INVALID: a
 * value outside the table (docs/values.md). A Python error stays set when
 * Python's allocator failed. */
ta_status ta_python_values_write(const ta_python_state *state, PyObject *value, ta_buffer *out);
/* The same as a new bytes object. */
ta_status ta_python_values_bytes(const ta_python_state *state, PyObject *value, PyObject **out);
/* One CBOR item as a new Python value: ta_cbor_decode's status, or
 * TA_ERROR_INVALID for an item outside the table. */
ta_status ta_python_values_read(const ta_python_state *state, ta_bytes cbor, PyObject **out);
/* `value` copied through the value table, as what crosses between actors
 * is (bindings.md, section 8): containers made again, the table's
 * immutable values shared. TA_ERROR_INVALID: a value outside the table. */
ta_status ta_python_values_copy(const ta_python_state *state, PyObject *value, PyObject **out);
/* A new tinyactors.Address of `text`. */
PyObject *ta_python_address_from(const ta_python_state *state, ta_string_view text);
/* tinyactors.encode(value) and tinyactors.decode(data). */
PyObject *ta_python_encode(PyObject *module, PyObject *value);
PyObject *ta_python_decode(PyObject *module, PyObject *data);

/* ---- the host (host.c) ------------------------------------------------------------- */

/* The "python" datamodel (datamodel/datamodel.h). */
typedef struct ta_python_datamodel ta_python_datamodel;
const ta_datamodel_vtable *ta_python_datamodel_vtable(void);
ta_python_datamodel *ta_python_datamodel_new(const ta_python_state *state);
void ta_python_datamodel_free(ta_python_datamodel *datamodel);
int ta_python_datamodel_settle(ta_python_datamodel *datamodel);
int ta_python_event_init_type(PyObject *module, ta_python_state *state);
/* A payload of the datamodel holding a copy of `value` through the value
 * table, with no CBOR in between (the hot path: bindings.md, section 2). */
ta_status ta_python_datamodel_payload(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out);
/* A copy of a payload's value, a new reference (0: None). */
PyObject *ta_python_datamodel_value(ta_python_datamodel *datamodel, ta_payload payload);

/* An image the host runs, kept until its segment is freed
 * (TA_HOST_EFFECT_SEGMENT_FREED) or the host is (bindings.md, section 6). */
typedef struct ta_python_image {
    void *bytes; /* PyMem_RawMalloc: the compiled image, aligned as core asks */
    ta_image image;
} ta_python_image;

/* What effects the application handles: one callable per kind, or NULL. */
enum {
    TA_PYTHON_ON_LOG = 0,
    TA_PYTHON_ON_DONE = 1,
    TA_PYTHON_ON_ERROR = 2,
    TA_PYTHON_ON_PEER = 3,
    TA_PYTHON_HANDLERS = 4
};

/* tinyactors.Host: one ta_host, its datamodels, its images, its clock and
 * its handlers. Single-threaded: every call from the thread that made it
 * (bindings.md, section 5). */
typedef struct ta_python_host {
    PyObject_HEAD
    PyObject *module; /* the module whose state it uses */
    ta_host *host;    /* NULL once closed */
    ta_python_datamodel *python; /* datamodels[0] */
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding datamodels[2];
    uint32_t datamodel_count;
    unsigned long thread; /* PyThread_get_thread_ident of the creating thread */
    PyObject *clock;      /* a SimulatedClock, or NULL for the real clock */
    PyObject *handlers[TA_PYTHON_HANDLERS];
    ta_python_image **images; /* by segment; NULL once freed */
    uint32_t image_count, image_capacity;
    uint64_t ticks_per_second;  /* host.config's system.ticksPerSecond */
    uint64_t origin_ns;         /* the real clock's start (time.monotonic_ns) */
    ta_time next_deadline;      /* of the last run: UINT64_MAX for none */
    int looping;                /* inside ta_python_loop_turn */
    int pending;                /* an input arrived while looping: run again */
    int closing;                /* close() was called while looping */
    PyObject *raised;           /* the first exception a handler raised in this turn */
    PyObject *weakreferences;   /* a SimulatedClock refers to its hosts weakly */
} ta_python_host;

int ta_python_host_init_type(PyObject *module, ta_python_state *state);
/* The module's state of a host. */
const ta_python_state *ta_python_host_state(const ta_python_host *host);
/* The host is open and called from its thread; else a TinyactorsError
 * (ERROR_STATE) is set and 0 returned. */
int ta_python_host_usable(ta_python_host *host);
/* Frees the host and what it holds of core (images, datamodels). */
void ta_python_host_free(ta_python_host *host);
/* A payload of the datamodel `datamodel` (an index of host->datamodels)
 * holding `value` (NULL: no payload, 0). TA_ERROR_INVALID: a value
 * outside the table. */
typedef struct ta_python_payload_making {
    PyObject *value;
    uint32_t datamodel;
} ta_python_payload_making;
ta_status ta_python_payload_make(ta_python_host *host, const ta_python_payload_making *making, ta_payload *out);
/* The value of a payload, as a new Python object (0: None). */
typedef struct ta_python_payload_reading {
    ta_payload payload;
    uint32_t datamodel;
} ta_python_payload_reading;
PyObject *ta_python_payload_value(ta_python_host *host, const ta_python_payload_reading *reading);
void ta_python_payload_release(ta_python_host *host, const ta_python_payload_reading *reading);
/* Drops segment `segment`'s image (TA_HOST_EFFECT_SEGMENT_FREED). */
void ta_python_image_drop(ta_python_host *host, uint32_t segment);

/* ---- requests (request.c) ----------------------------------------------------------- */

PyObject *ta_python_host_request(ta_python_host *host, PyObject *args, PyObject *kwargs);

/* ---- the loop (loop.c) ---------------------------------------------------------------- */

int ta_python_clock_init_type(PyObject *module, ta_python_state *state);
/* Joins `host` to the SimulatedClock `clock` (its turns follow it). */
int ta_python_clock_attach(PyObject *clock, ta_python_host *host);
void ta_python_clock_detach(PyObject *clock, ta_python_host *host);
/* The real clock's start, for a new host: -1 with a Python error. */
int ta_python_loop_start(ta_python_host *host);
/* One turn at the current time: ta_host_advance_time, then ta_host_run
 * until idle with every run status handled and the effects dispatched and
 * acknowledged. -1 with a Python error set: the first a handler raised
 * (after the turn finished), or the run's fault. Inside a turn (an input
 * from a handler) it only marks that the turn must run again. */
int ta_python_loop_turn(ta_python_host *host);
/* host.run(): the synchronous driver, until no deadline is left. */
PyObject *ta_python_loop_run(ta_python_host *host, PyObject *unused);

/* ---- effects (effects.c) --------------------------------------------------------------- */

/* Each effect of the batch ta_host_effects published, to its handler. A
 * handler's exception is kept in host->raised (the first one), never
 * stops the batch. */
typedef struct ta_python_batch {
    const ta_host_effect *effects;
    uint32_t count;
} ta_python_batch;
void ta_python_effects_dispatch(ta_python_host *host, const ta_python_batch *batch);

#endif /* TA_PYTHON_EXTENSION_H */
