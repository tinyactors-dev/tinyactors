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

/* The writer of one call (AGENTS.md, "The binding's memory"; docs/adr/0010
 * in the repository root, decision 4): it starts in `room`, on the C stack
 * with the call, and grows through the host's allocator (PyMem_RawMalloc)
 * when a value outgrows it; ta_python_cbor_free gives that back before the
 * call returns. Each call has its own, so a call made while it writes
 * (Python code, a nested call into the Host) cannot overwrite it. It must
 * not move while it is in use: the writer points into `room`. */
typedef struct ta_python_cbor {
    ta_cbor_writer writer;
    ta_bytes bytes; /* what ta_python_values_write wrote: a view of the writer's memory */
    unsigned char room[1024];
} ta_python_cbor;
void ta_python_cbor_init(ta_python_cbor *cbor);
void ta_python_cbor_free(ta_python_cbor *cbor);

/* ---- values (values.c) ------------------------------------------------------------ */

int ta_python_values_init(PyObject *module, ta_python_state *state);
/* `value` as CBOR, written by `out`'s writer, in out->bytes (a view of the
 * writer's memory). TA_ERROR_INVALID: a value outside the table
 * (docs/values.md); TA_ERROR_NO_MEMORY: the allocator refused room. */
ta_status ta_python_values_write(const ta_python_state *state, PyObject *value, ta_python_cbor *out);
/* One CBOR item as a new Python value, read as it streams: the reader's
 * status (malformed, not UTF-8, too deep, bytes after it), or
 * TA_ERROR_INVALID for an item outside the table. The bytes must not
 * change while it reads: it makes Python objects, and a finalizer the
 * cycle collector runs then may call into the host. */
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

/* ---- the handle tables (datamodel/handles.c) -------------------------------------------- */

/* A table's rows: each starts with this header. A handle is (generation <<
 * 32) | (slot + 1), so 0 is no handle. */
typedef struct ta_python_row {
    uint32_t generation;
    uint32_t count; /* payloads: references; free rows: the next free slot + 1 */
    uint32_t used;
    uint32_t pad;
} ta_python_row;

/* One block of rows of `size` bytes, grown by doubling, freed once. */
typedef struct ta_python_table {
    void *rows;
    uint32_t size, count, capacity;
    uint32_t free; /* the first free slot + 1, 0: none */
} ta_python_table;

/* A new zeroed row (its header set, one reference) and its handle; NULL
 * when memory ran out. */
void *ta_python_table_add(ta_python_table *table, uint64_t *handle);
/* The row of `handle`, or NULL for a handle that is not (any more). */
void *ta_python_table_row(const ta_python_table *table, uint64_t handle);
/* The row at slot `slot` (0 …), or NULL when it is free: for walking. */
void *ta_python_table_slot(const ta_python_table *table, uint32_t slot);
/* Frees the row of `handle` (its references dropped first). */
void ta_python_table_remove(ta_python_table *table, uint64_t handle);
void ta_python_table_free(ta_python_table *table);

/* ---- the "python" datamodel (datamodel/) --------------------------------------------- */

/* Held by its Host, in the Host object itself: no allocation of its own. */
typedef struct ta_python_datamodel {
    const ta_python_state *state;
    ta_python_table sessions; /* ta_python_session rows */
    ta_python_table charts;   /* ta_python_chart rows */
    ta_python_table payloads; /* ta_python_value rows: a payload's value, count its references */
    ta_python_table loops;    /* ta_python_value rows: a <foreach>'s items */
    PyObject *builtins;        /* the builtins module's dict, each session's __builtins__ */
    PyObject *json_loads;      /* json.loads, for <content> */
    PyObject *refuse_constant; /* json.loads's parse_constant, int: NaN and Infinity are not JSON */
    PyObject *in;              /* In(state), bound to this datamodel */
    PyObject *deferred;        /* a list: objects whose last reference waits for the outer call to return */
    PyObject *ended;           /* a list: the globals of ended sessions, cleared at settle (they make cycles) */
    PyObject *kept;            /* the first BaseException that is not an Exception, raised after the run */
    PyObject *text;            /* the str whose UTF-8 eval_string and log returned last */
    ta_python_cbor wire;       /* the writer of the last to_wire, its view valid until the next call */
    ta_datamodel_environment *environment; /* of the call in progress, for In() */
    uint64_t current;                      /* the session of the call in progress */
} ta_python_datamodel;

const ta_datamodel_vtable *ta_python_datamodel_vtable(void);
/* Fills a datamodel for a host: -1 with a Python error. */
int ta_python_datamodel_init(ta_python_datamodel *datamodel, const ta_python_state *state);
/* After core destroyed every session and released every payload
 * (ta_host_free): drops what is left and frees the tables. */
void ta_python_datamodel_clear(ta_python_datamodel *datamodel);
/* What the datamodel holds, for the host's Py_tp_traverse. */
int ta_python_datamodel_traverse(ta_python_datamodel *datamodel, visitproc visit, void *arg);
int ta_python_datamodel_settle(ta_python_datamodel *datamodel);
/* A chart's evaluation raised a BaseException that is not an Exception:
 * the turn stops, and settle raises it. */
int ta_python_datamodel_interrupted(const ta_python_datamodel *datamodel);
int ta_python_event_init_type(PyObject *module, ta_python_state *state);
/* A payload of the datamodel holding a copy of `value` through the value
 * table, with no CBOR in between (the hot path: bindings.md, section 2). */
ta_status ta_python_datamodel_payload(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out);
/* A copy of a payload's value, a new reference (0: None). */
PyObject *ta_python_datamodel_value(ta_python_datamodel *datamodel, ta_payload payload);

/* What effects the application handles: one callable per kind, or NULL. */
enum {
    TA_PYTHON_ON_LOG = 0,
    TA_PYTHON_ON_DONE = 1,
    TA_PYTHON_ON_ERROR = 2,
    TA_PYTHON_ON_PEER = 3,
    TA_PYTHON_HANDLERS = 4
};

/* A segment's image: core's compile result, held as it is, and the
 * ta_image over its bytes, which core keeps a pointer to and so must not
 * move with the row: it is in a bytearray of its own. */
typedef struct ta_python_image {
    ta_python_row row;
    ta_compile_result *result;
    PyObject *image; /* a bytearray holding the ta_image */
    uint32_t segment;
    uint32_t pad;
} ta_python_image;

/* tinyactors.Host: one ta_host, its datamodels, its images, its clock and
 * its handlers. Single-threaded: every call from the thread that made it
 * (bindings.md, section 5). */
typedef struct ta_python_host {
    PyObject_HEAD
    PyObject *module; /* the module whose state it uses */
    ta_host *host;    /* NULL once closed */
    ta_python_datamodel python; /* datamodels[0], held here: no allocation of its own */
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding datamodels[2];
    uint32_t datamodel_count;
    unsigned long thread; /* PyThread_get_thread_ident of the creating thread */
    PyObject *clock;      /* a SimulatedClock, or NULL for the real clock */
    PyObject *handlers[TA_PYTHON_HANDLERS];
    ta_python_table images; /* ta_python_image rows: each segment's compile result, held until
                               TA_HOST_EFFECT_SEGMENT_FREED (bindings.md, section 6: "Hold, don't copy") */
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
