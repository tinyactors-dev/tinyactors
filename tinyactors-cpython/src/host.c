/* The Host (AGENTS.md, "The API"): ta_host_create from Python options, the
 * inputs (add_image, chart, spawn, send, send_many, terminate, peers),
 * the call's arena mark, and close. The host's allocator is here, and
 * nowhere else calls an allocator (tools/lint/rules/
 * allocation-in-tables-and-host.yml). The loop is loop.c, the effects
 * effects.c, the typed methods methods.c. */
#include "extension.h"
#include <string.h>

/* ---- the host's allocator ------------------------------------------------------ */

/* Core's alloc: a raw block or NULL to refuse. Core asks for an alignment
 * of at most 8 (allocator.h), and the raw domain gives 16 (AGENTS.md,
 * "The host's allocator"). Called without the GIL's help: the raw domain
 * needs none. */
static void *ta_python_alloc(void *context, size_t size, size_t align) {
    (void)context;
    assert(align <= 8);
    (void)align;
    return PyMem_RawMalloc(size);
}

static void ta_python_free(void *context, void *block, size_t size) {
    (void)context;
    (void)size;
    PyMem_RawFree(block);
}

static const ta_allocator ta_python_allocator = {NULL, ta_python_alloc, ta_python_free};

/* A writer in `room` growing through `allocator`, or the host's raw
 * allocator (tinyactors.encode, which has no host). */
void ta_python_cbor_init(ta_cbor_writer *writer, const ta_python_room *room) {
    ta_cbor_writer_init(writer, room->room, room->allocator ? room->allocator : &ta_python_allocator);
}

/* The compiler allocates through the host's allocator too (compile.c). */
ta_status ta_python_compile(ta_compile_options *options, ta_source_list charts, ta_compile_result **out) {
    options->alloc = ta_python_allocator;
    return ta_compile_charts(options, charts, out);
}

/* ---- errors -------------------------------------------------------------------- */

static PyObject *ta_python_error_new(ta_python_module_state *state, PyObject *arguments, PyObject *keywords) {
    return PyObject_Call(state->error, arguments, keywords);
}

void ta_python_raise_status(ta_python_module_state *state, ta_status status) {
    if (status == TA_ERROR_NO_MEMORY) {
        PyErr_NoMemory();
        return;
    }
    const char *name = ta_status_name(status);
    PyObject *keywords = Py_BuildValue("{s:s}", "status", name ? name : "unknown");
    PyObject *arguments = keywords ? Py_BuildValue("(s)", name ? name : "unknown status") : NULL;
    PyObject *error = arguments ? ta_python_error_new(state, arguments, keywords) : NULL;
    Py_XDECREF(arguments);
    Py_XDECREF(keywords);
    if (error) {
        PyErr_SetObject(state->error, error);
        Py_DECREF(error);
    }
}

/* {"code", "message", "data": {"reason", "path"}} as core wrote it. */
PyObject *ta_python_error_from_reply(ta_python_module_state *state, PyObject *error) {
    PyObject *message = NULL, *code = NULL, *data = NULL, *reason = NULL, *path = NULL;
    if (PyDict_Check(error)) {
        PyDict_GetItemStringRef(error, "message", &message);
        PyDict_GetItemStringRef(error, "code", &code);
        if (PyDict_GetItemStringRef(error, "data", &data) > 0 && PyDict_Check(data)) {
            PyDict_GetItemStringRef(data, "reason", &reason);
            PyDict_GetItemStringRef(data, "path", &path);
        }
    }
    PyObject *keywords = Py_BuildValue("{s:O,s:O,s:O}", "code", code ? code : Py_None, "reason",
                                       reason ? reason : Py_None, "path", path ? path : Py_None);
    PyObject *arguments = keywords ? PyTuple_Pack(1, message ? message : Py_None) : NULL;
    PyObject *result = arguments ? ta_python_error_new(state, arguments, keywords) : NULL;
    Py_XDECREF(arguments);
    Py_XDECREF(keywords);
    Py_XDECREF(message);
    Py_XDECREF(code);
    Py_XDECREF(data);
    Py_XDECREF(reason);
    Py_XDECREF(path);
    return result;
}

/* ---- a call --------------------------------------------------------------------- */

int ta_python_enter(ta_python_host *host, ta_python_call *call) {
    if (!host->host) {
        ta_python_raise_status(host->state, TA_ERROR_STATE);
        return -1;
    }
    if (PyThread_get_thread_ident() != host->thread) { /* bindings.md, section 5 */
        PyErr_SetString(host->state->error, "a Host is called only from the thread that made it");
        return -1;
    }
    call->host = host;
    call->mark = ta_growable_arena_position(&host->arena);
    host->depth++;
    return 0;
}

void ta_python_leave(ta_python_call *call) {
    ta_python_host *host = call->host;
    ta_growable_arena_release(&host->arena, call->mark);
    host->depth--;
    if (host->depth == 0 && !host->turning) {
        if (host->closing) ta_python_host_close(host, 0);
        else ta_python_datamodel_flush(&host->python);
    }
}

int ta_python_text(PyObject *text, ta_string_view *out) {
    if (text == NULL || text == Py_None) {
        *out = (ta_string_view){"", 0};
        return 0;
    }
    if (!PyUnicode_Check(text)) {
        PyErr_SetString(PyExc_TypeError, "expected a str");
        return -1;
    }
    Py_ssize_t length;
    const char *pointer = PyUnicode_AsUTF8AndSize(text, &length);
    if (!pointer) return -1;
    if ((size_t)length > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "text past core's length limit");
        return -1;
    }
    *out = (ta_string_view){pointer, (uint32_t)length};
    return 0;
}

void ta_python_host_keep_error(ta_python_host *host) {
    PyObject *error = PyErr_GetRaisedException();
    if (!host->error) host->error = error;
    else Py_XDECREF(error);
}

/* ---- payloads: a value for a session's datamodel ----------------------------------- */

static uint32_t ta_python_datamodel_index(ta_string_view name) {
    if (name.length == 6 && !memcmp(name.pointer, "python", 6)) return TA_PYTHON_DATAMODEL_PYTHON;
    if (name.length == 10 && !memcmp(name.pointer, "ecmascript", 10)) return TA_PYTHON_DATAMODEL_ECMASCRIPT;
    return TA_NIL;
}

static uint32_t ta_python_chart_datamodel(ta_python_host *host, ta_host_chart_ref chart) {
    ta_host_segment_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (ta_host_segment_info_get(host->host, chart.segment, &info) != TA_OK || !info.image) return TA_NIL;
    return ta_python_datamodel_index(ta_image_chart_datamodel(info.image, chart.chart));
}

uint32_t ta_python_host_session_datamodel(ta_python_host *host, uint64_t session) {
    ta_host_location location;
    if (ta_host_locate(host->host, session, &location) != TA_OK) return TA_NIL;
    ta_system *system = ta_host_system(host->host, location.segment);
    ta_actor_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (!system || ta_actor_info_get(system, location.actor, &info) != TA_OK) return TA_NIL;
    return ta_python_chart_datamodel(host, (ta_host_chart_ref){location.segment, info.chart});
}

int ta_python_host_payload(ta_python_host *host, ta_python_payload_for *target) {
    target->payload = 0;
    target->datamodel = TA_NIL;
    if (target->value == NULL || target->value == Py_None) return 0;
    uint32_t datamodel = target->session ? ta_python_host_session_datamodel(host, target->session)
                                         : ta_python_chart_datamodel(host, target->chart);
    if (datamodel == TA_PYTHON_DATAMODEL_PYTHON) {
        if (ta_python_payload_new(&host->python, target->value, &target->payload) < 0) return -1;
    } else if (datamodel == TA_PYTHON_DATAMODEL_ECMASCRIPT) {
        /* CBOR toward quickjs, in the call's arena (AGENTS.md, "The binding's memory") */
        ta_cbor_writer writer;
        ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, &host->arena_allocator});
        if (ta_python_value_write(host->state, &writer, target->value) < 0) return -1;
        ta_bytes cbor;
        ta_status status = ta_cbor_writer_bytes(&writer, &cbor);
        if (status == TA_OK) status = ta_datamodel_ecmascript_payload_from_cbor(host->ecmascript, cbor, &target->payload);
        if (status != TA_OK) {
            ta_python_raise_status(host->state, status);
            return -1;
        }
    } else if (target->session && !ta_host_locate(host->host, target->session, &(ta_host_location){0, 0})) {
        PyErr_SetString(PyExc_TypeError, "the session's chart has a datamodel that takes no data");
        return -1;
    } else {
        ta_python_raise_status(host->state, TA_ERROR_STALE); /* no such session */
        return -1;
    }
    target->datamodel = datamodel;
    return 0;
}

void ta_python_host_payload_release(ta_python_host *host, ta_payload payload, uint32_t datamodel) {
    if (!payload || datamodel >= TA_PYTHON_DATAMODELS) return;
    host->datamodels[datamodel].vtable->release(host->datamodels[datamodel].datamodel, payload);
}

PyObject *ta_python_host_payload_value(ta_python_host *host, const ta_python_payload_of *payload) {
    if (!payload->payload) return Py_NewRef(Py_None);
    if (payload->datamodel == TA_PYTHON_DATAMODEL_PYTHON) {
        PyObject *value = ta_python_payload_value(&host->python, payload->payload);
        return Py_NewRef(value ? value : Py_None);
    }
    if (payload->datamodel == TA_PYTHON_DATAMODEL_ECMASCRIPT) {
        ta_bytes cbor; /* quickjs's, until its next call: read at once */
        ta_status status = ta_datamodel_ecmascript_payload_to_cbor(host->ecmascript, payload->payload, &cbor);
        if (status != TA_OK) {
            ta_python_raise_status(host->state, status);
            return NULL;
        }
        return ta_python_value_read(host->state, cbor);
    }
    return Py_NewRef(Py_None);
}

/* ---- ChartRef ---------------------------------------------------------------------- */

typedef struct ta_python_chart_ref {
    PyObject_HEAD
    uint32_t segment;
    uint32_t chart;
} ta_python_chart_ref;

static PyMemberDef ta_python_chart_members[] = {
    {"segment", Py_T_UINT, offsetof(ta_python_chart_ref, segment), Py_READONLY, "the segment that runs it"},
    {"chart", Py_T_UINT, offsetof(ta_python_chart_ref, chart), Py_READONLY, "its index in the segment's image"},
    {NULL, 0, 0, 0, NULL}
};

static PyObject *ta_python_chart_repr(PyObject *self) {
    ta_python_chart_ref *ref = (ta_python_chart_ref *)self;
    return PyUnicode_FromFormat("ChartRef(segment=%u, chart=%u)", ref->segment, ref->chart);
}

static void ta_python_simple_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyType_Slot ta_python_chart_slots[] = {
    {Py_tp_doc, "A chart in a host (Host.chart): its segment and index, for Host.spawn."},
    {Py_tp_members, ta_python_chart_members},
    {Py_tp_repr, ta_python_chart_repr},
    {Py_tp_dealloc, ta_python_simple_dealloc},
    {0, NULL}
};

static PyType_Spec ta_python_chart_spec = {
    "tinyactors.ChartRef", sizeof(ta_python_chart_ref), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION, ta_python_chart_slots
};

/* ---- creation ------------------------------------------------------------------------ */

/* The CBOR of a value, in the call's arena. */
static int ta_python_host_cbor(ta_python_host *host, PyObject *value, ta_bytes *out) {
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{NULL, 0, 0}, &host->arena_allocator});
    if (ta_python_value_write(host->state, &writer, value) < 0) return -1;
    ta_status status = ta_cbor_writer_bytes(&writer, out);
    if (status != TA_OK) {
        ta_python_raise_status(host->state, status);
        return -1;
    }
    return 0;
}

/* host.config, posted once before the host is handed out (no batch is
 * open: bindings.md, section 3): the effective config as Python values. */
static PyObject *ta_python_host_config(ta_python_host *host) {
    unsigned char room[64];
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{room, sizeof room, 0}, &host->arena_allocator});
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("host.config"));
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, 0);
    ta_bytes message;
    ta_status status = ta_cbor_writer_bytes(&writer, &message);
    if (status == TA_OK) status = ta_host_post(host->host, message);
    if (status != TA_OK) {
        ta_python_raise_status(host->state, status);
        return NULL;
    }
    const ta_host_effect *effects;
    uint32_t count = ta_host_effects(host->host, &effects);
    PyObject *reply = count ? ta_python_value_read(host->state, ta_host_effect_bytes(host->host, count - 1, TA_EFFECT_FIELD_WIRE))
                            : NULL;
    ta_host_effects_ack(host->host);
    if (!reply) {
        if (!PyErr_Occurred()) ta_python_raise_status(host->state, TA_ERROR_STATE);
        return NULL;
    }
    PyObject *result = NULL;
    if (PyDict_Check(reply)) PyDict_GetItemStringRef(reply, "result", &result);
    Py_DECREF(reply);
    if (!result || !PyDict_Check(result)) {
        Py_XDECREF(result);
        ta_python_raise_status(host->state, TA_ERROR_STATE);
        return NULL;
    }
    return result;
}

/* The I/O processors' handlers by their index in the effective config's
 * ioProcessors: (type, handler or None, is async). */
static int ta_python_host_processors(ta_python_host *host, PyObject *config, PyObject *handlers) {
    PyObject *entries = NULL;
    if (PyDict_GetItemStringRef(config, "ioProcessors", &entries) < 0) return -1;
    host->processors = PyList_New(0);
    if (!host->processors) {
        Py_XDECREF(entries);
        return -1;
    }
    Py_ssize_t found = 0, count = entries && PyList_Check(entries) ? PyList_Size(entries) : 0;
    int status = 0;
    for (Py_ssize_t i = 0; i < count && status == 0; i++) {
        PyObject *entry = PyList_GetItem(entries, i), *type = NULL, *handler = NULL;
        if (PyDict_Check(entry)) PyDict_GetItemStringRef(entry, "type", &type);
        if (type && handlers) status = PyDict_GetItemRef(handlers, type, &handler) < 0 ? -1 : 0;
        if (handler) found++;
        int is_async = 0;
        if (status == 0 && handler) {
            PyObject *answer = PyObject_CallFunctionObjArgs(host->state->coroutine_function, handler, NULL);
            is_async = answer ? PyObject_IsTrue(answer) : -1;
            Py_XDECREF(answer);
            if (is_async < 0) status = -1;
            if (is_async) host->has_async = 1;
        }
        PyObject *row = status == 0 ? Py_BuildValue("(OOO)", type ? type : Py_None, handler ? handler : Py_None,
                                                     is_async ? Py_True : Py_False)
                                    : NULL;
        if (!row || PyList_Append(host->processors, row) < 0) status = -1;
        Py_XDECREF(row);
        Py_XDECREF(type);
        Py_XDECREF(handler);
    }
    Py_XDECREF(entries);
    if (status == 0 && handlers && found != PyDict_Size(handlers)) {
        PyErr_SetString(PyExc_ValueError, "io_processors names a type the config's ioProcessors does not have");
        status = -1;
    }
    return status;
}

static PyObject *ta_python_host_create(PyTypeObject *type, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"config", "io_processors", "log", "clock", "peer", NULL};
    PyObject *config, *handlers = Py_None, *log = Py_None, *clock = Py_None, *peer = Py_None;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "O|O$OOO:Host", names, &config, &handlers, &log, &clock,
                                     &peer))
        return NULL;
    ta_python_module_state *state = ta_python_state_of_type(type);
    if (!state) return NULL;
    if (handlers != Py_None && !PyDict_Check(handlers)) {
        PyErr_SetString(PyExc_TypeError, "io_processors is a dict of type -> handler");
        return NULL;
    }
    if (clock != Py_None && !Py_IS_TYPE(clock, (PyTypeObject *)state->clock_type)) {
        PyErr_SetString(PyExc_TypeError, "clock is a tinyactors.SimulatedClock or None");
        return NULL;
    }
    ta_python_host *host = (ta_python_host *)PyType_GenericAlloc(type, 0);
    if (!host) return NULL;
    host->state = state;
    host->thread = PyThread_get_thread_ident();
    host->allocator = ta_python_allocator;
    host->deadline = UINT64_MAX;
    host->log = Py_NewRef(log);
    host->peer = Py_NewRef(peer);
    host->clock = Py_NewRef(clock);
    host->loop = Py_NewRef(Py_None);
    host->timer = Py_NewRef(Py_None);
    host->scheduled = Py_NewRef(Py_None);
    host->waiters = PyList_New(0);
    ta_growable_arena_config arena = {sizeof arena, 0, 16384, &host->allocator};
    ta_growable_arena_init(&host->arena, &arena);
    host->arena_allocator = ta_growable_arena_allocator(&host->arena);
    ta_table_config pending = {sizeof pending, sizeof(ta_python_pending_row), &host->allocator};
    ta_table_init(&host->pending, &pending);
    if (!host->waiters || ta_python_datamodel_init(&host->python, host) < 0) goto fail;
    ta_datamodel_ecmascript_config ecmascript;
    memset(&ecmascript, 0, sizeof ecmascript);
    ecmascript.size = sizeof ecmascript;
    host->ecmascript = ta_datamodel_ecmascript_new(&ecmascript);
    if (!host->ecmascript) {
        PyErr_NoMemory();
        goto fail;
    }
    host->datamodels[TA_PYTHON_DATAMODEL_PYTHON] = (ta_datamodel_binding){ta_python_datamodel_vtable(), &host->python};
    host->datamodels[TA_PYTHON_DATAMODEL_ECMASCRIPT] = ta_datamodel_ecmascript_binding(host->ecmascript);

    ta_bytes cbor;
    ta_growable_arena_mark mark = ta_growable_arena_position(&host->arena);
    if (ta_python_host_cbor(host, config, &cbor) < 0) goto fail;
    ta_buffer refusal = {ta_growable_arena_alloc(&host->arena, 8192, 8), 8192, 0};
    ta_host_environment environment = {sizeof environment, TA_PYTHON_DATAMODELS, host->datamodels,
                                       host->allocator, NULL, refusal.pointer ? &refusal : NULL};
    ta_status status = ta_host_create(cbor, &environment, &host->host);
    if (status != TA_OK) {
        host->host = NULL;
        PyObject *reply = status == TA_ERROR_INVALID && refusal.length && refusal.length <= refusal.capacity
                              ? ta_python_value_read(state, (ta_bytes){refusal.pointer, refusal.length})
                              : NULL;
        PyObject *error = NULL;
        if (reply && PyDict_Check(reply)) PyDict_GetItemStringRef(reply, "error", &error);
        PyObject *exception = error ? ta_python_error_from_reply(state, error) : NULL;
        if (exception) PyErr_SetObject(state->error, exception);
        else if (!PyErr_Occurred()) ta_python_raise_status(state, status);
        Py_XDECREF(exception);
        Py_XDECREF(error);
        Py_XDECREF(reply);
        ta_growable_arena_release(&host->arena, mark);
        goto fail;
    }
    PyObject *effective = ta_python_host_config(host);
    ta_growable_arena_release(&host->arena, mark);
    if (!effective) goto fail;
    PyObject *system = NULL, *rate = NULL;
    if (PyDict_GetItemStringRef(effective, "system", &system) > 0 && PyDict_Check(system))
        PyDict_GetItemStringRef(system, "ticksPerSecond", &rate);
    host->ticks_per_second = rate ? PyLong_AsUnsignedLongLong(rate) : 0;
    Py_XDECREF(system);
    Py_XDECREF(rate);
    int processors = PyErr_Occurred() || !host->ticks_per_second ? -1
                                                                  : ta_python_host_processors(host, effective,
                                                                                              handlers == Py_None ? NULL : handlers);
    Py_DECREF(effective);
    if (processors < 0) {
        if (!PyErr_Occurred()) ta_python_raise_status(state, TA_ERROR_STATE);
        goto fail;
    }
    if (clock != Py_None && ta_python_clock_register(clock, (PyObject *)host) < 0) goto fail;
    return (PyObject *)host;
fail:
    Py_DECREF(host);
    return NULL;
}

/* ---- close ----------------------------------------------------------------------- */

/* trace.end, then the last take of traces and logs, before ta_host_free
 * (bindings.md, section 4). There is no exporter in the API yet: what is
 * taken is dropped. */
static void ta_python_host_end_traces(ta_python_host *host) {
    unsigned char room[64];
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{room, sizeof room, 0}, NULL});
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("trace.end"));
    ta_bytes message;
    if (ta_cbor_writer_bytes(&writer, &message) == TA_OK && ta_host_post(host->host, message) == TA_OK) {
        const ta_host_effect *effects;
        while (ta_host_effects(host->host, &effects) > 0) {
            ta_bytes taken;
            while (ta_host_traces(host->host, &taken) == TA_OK && taken.length) {}
            while (ta_host_logs(host->host, &taken) == TA_OK && taken.length) {}
            ta_host_effects_ack(host->host);
        }
    }
    ta_cbor_writer_free(&writer);
}

static void ta_python_host_free(ta_python_host *host) {
    if (host->host) ta_host_free(host->host);
    host->host = NULL;
    ta_python_pending_drop(host);
    ta_table_free(&host->pending);
    ta_python_datamodel_free(&host->python);
    if (host->ecmascript) ta_datamodel_ecmascript_free(host->ecmascript); /* after the host that used it */
    host->ecmascript = NULL;
    ta_growable_arena_free(&host->arena);
}

void ta_python_host_close(ta_python_host *host, int quiet) {
    if (!host->host) return;
    if (host->depth > 0 || host->turning) { /* from a handler or chart code: after the loop returns */
        host->closing = 1;
        return;
    }
    host->closing = 0;
    ta_python_loop_stop(host);
    if (!quiet) ta_python_host_end_traces(host);
    ta_python_host_free(host);
    if (host->clock && host->clock != Py_None) ta_python_clock_forget(host->clock, (PyObject *)host);
}

static PyObject *ta_python_host_close_method(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    if (host->host && PyThread_get_thread_ident() != host->thread) {
        PyErr_SetString(host->state->error, "a Host is called only from the thread that made it");
        return NULL;
    }
    ta_python_host_close(host, 0);
    Py_RETURN_NONE;
}

static int ta_python_host_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_host *host = (ta_python_host *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(host->processors);
    Py_VISIT(host->log);
    Py_VISIT(host->peer);
    Py_VISIT(host->clock);
    Py_VISIT(host->loop);
    Py_VISIT(host->timer);
    Py_VISIT(host->scheduled);
    Py_VISIT(host->waiters);
    Py_VISIT(host->error);
    for (uint32_t slot = 0; slot < host->pending.capacity; slot++) {
        ta_python_pending_row *row = ta_table_at(&host->pending, slot);
        if (!row) continue;
        Py_VISIT(row->handler);
        Py_VISIT(row->on_error);
    }
    return ta_python_datamodel_traverse(&host->python, visit, arg);
}

/* The collector's path: free the host without calling handlers, then drop
 * every reference (handlers close over the host: AGENTS.md, "Rules the
 * suite cannot see"). */
static int ta_python_host_clear(PyObject *self) {
    ta_python_host *host = (ta_python_host *)self;
    if (host->host && host->depth == 0 && !host->turning) ta_python_host_close(host, 1);
    Py_CLEAR(host->processors);
    Py_CLEAR(host->log);
    Py_CLEAR(host->peer);
    Py_CLEAR(host->clock);
    Py_CLEAR(host->loop);
    Py_CLEAR(host->timer);
    Py_CLEAR(host->scheduled);
    Py_CLEAR(host->waiters);
    Py_CLEAR(host->error);
    return 0;
}

static void ta_python_host_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    ta_python_host *host = (ta_python_host *)self;
    PyObject_GC_UnTrack(self);
    PyObject_ClearWeakRefs(self);
    host->depth = 0;
    host->turning = 0;
    if (host->host) ta_python_host_close(host, 1);
    else if (host->python.state) ta_python_host_free(host); /* a creation that failed half way */
    ta_python_host_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

/* ---- inputs ----------------------------------------------------------------------- */

static PyObject *ta_python_host_status(ta_python_host *host, ta_status status) {
    if (status == TA_OK) Py_RETURN_NONE;
    ta_python_raise_status(host->state, status);
    return NULL;
}

static PyObject *ta_python_host_add_image(PyObject *self, PyObject *image) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    Py_buffer view;
    PyObject *result = NULL;
    if (PyObject_GetBuffer(image, &view, PyBUF_SIMPLE) == 0) {
        uint32_t segment = 0; /* the host copies the bytes during the call */
        ta_status status = ta_host_add_image(host->host, (ta_bytes){view.buf, (size_t)view.len}, &segment);
        PyBuffer_Release(&view);
        result = status == TA_OK ? PyLong_FromUnsignedLong(segment) : ta_python_host_status(host, status);
    }
    ta_python_leave(&call);
    return result;
}

static PyObject *ta_python_host_chart(PyObject *self, PyObject *name) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_string_view text;
    ta_host_chart_ref chart;
    if (PyUnicode_Check(name) && ta_python_text(name, &text) == 0) {
        ta_status status = ta_host_chart(host->host, text, &chart);
        if (status != TA_OK) ta_python_raise_status(host->state, status);
        else {
            ta_python_chart_ref *ref = (ta_python_chart_ref *)PyType_GenericAlloc((PyTypeObject *)host->state->chart_type, 0);
            if (ref) {
                ref->segment = chart.segment;
                ref->chart = chart.chart;
            }
            result = (PyObject *)ref;
        }
    } else if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "chart takes the chart's name, a str");
    ta_python_leave(&call);
    return result;
}

static int ta_python_session(PyObject *value, uint64_t *out) {
    *out = PyLong_Check(value) ? PyLong_AsUnsignedLongLong(value) : 0;
    if (PyErr_Occurred()) return -1;
    if (!PyLong_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "a session is an int");
        return -1;
    }
    return 0;
}

static PyObject *ta_python_host_spawn(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"ref", "name", "params", "traceparent", NULL};
    ta_python_host *host = (ta_python_host *)self;
    PyObject *ref, *name = Py_None, *params = Py_None, *traceparent = Py_None;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "O|OOO:spawn", names, &ref, &name, &params, &traceparent))
        return NULL;
    if (!Py_IS_TYPE(ref, (PyTypeObject *)host->state->chart_type)) {
        PyErr_SetString(PyExc_TypeError, "spawn takes a ChartRef from Host.chart");
        return NULL;
    }
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = ((ta_python_chart_ref *)ref)->segment;
    spawn.chart = ((ta_python_chart_ref *)ref)->chart;
    ta_python_payload_for payload = {0, {spawn.segment, spawn.chart}, params, 0, TA_NIL};
    if (ta_python_text(name, &spawn.name) == 0 && ta_python_text(traceparent, &spawn.traceparent) == 0 &&
        ta_python_host_payload(host, &payload) == 0) {
        uint64_t session = 0;
        spawn.params = payload.payload;
        ta_status status = ta_host_spawn(host->host, &spawn, &session);
        if (status != TA_OK) ta_python_host_payload_release(host, payload.payload, payload.datamodel);
        result = status == TA_OK ? PyLong_FromUnsignedLongLong(session) : ta_python_host_status(host, status);
        if (status == TA_OK) ta_python_loop_schedule(host);
    }
    ta_python_leave(&call);
    return result;
}

/* An event of send or send_many: fills `event` and its payload. */
typedef struct ta_python_send_item {
    PyObject *session, *name, *data, *traceparent;
    uint32_t datamodel; /* out: the payload's */
} ta_python_send_item;

static int ta_python_event(ta_python_host *host, ta_python_send_item *item, ta_event_in *event) {
    memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    item->datamodel = TA_NIL;
    if (ta_python_session(item->session, &event->session) < 0 || ta_python_text(item->traceparent, &event->traceparent) < 0)
        return -1;
    if (!PyUnicode_Check(item->name)) {
        PyErr_SetString(PyExc_TypeError, "an event's name is a str");
        return -1;
    }
    if (ta_python_text(item->name, &event->name) < 0) return -1;
    ta_python_payload_for payload = {event->session, {0, 0}, item->data, 0, TA_NIL};
    if (ta_python_host_payload(host, &payload) < 0) return -1;
    event->data = payload.payload;
    item->datamodel = payload.datamodel;
    return 0;
}

static PyObject *ta_python_host_send(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"session", "event", "data", "traceparent", NULL};
    ta_python_host *host = (ta_python_host *)self;
    ta_python_send_item item = {NULL, NULL, Py_None, Py_None, TA_NIL};
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "OO|O$O:send", names, &item.session, &item.name, &item.data,
                                     &item.traceparent))
        return NULL;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_event_in event;
    if (ta_python_event(host, &item, &event) == 0) {
        ta_status status = ta_host_send(host->host, &event);
        if (status != TA_OK) ta_python_host_payload_release(host, event.data, item.datamodel);
        else ta_python_loop_schedule(host);
        result = ta_python_host_status(host, status);
    }
    ta_python_leave(&call);
    return result;
}

/* Events a send_many converts and hands core at once: the arena holds a
 * slice, not the whole list, so one large call leaves no large chunk
 * kept for the host's life. */
#define TA_PYTHON_SLICE 256

/* One slice of send_many, events [first, first + count) of `list`: the
 * accepted prefix of the slice, or -1 with an exception (nothing of the
 * slice taken). */
typedef struct ta_python_slice {
    PyObject *list;
    Py_ssize_t first, count;
} ta_python_slice;

static Py_ssize_t ta_python_send_slice(ta_python_host *host, const ta_python_slice *slice) {
    ta_growable_arena_mark mark = ta_growable_arena_position(&host->arena);
    ta_event_in *events = ta_growable_arena_alloc(&host->arena, sizeof(ta_event_in) * TA_PYTHON_SLICE, 8);
    uint32_t *datamodels = ta_growable_arena_alloc(&host->arena, sizeof(uint32_t) * TA_PYTHON_SLICE, 4);
    Py_ssize_t made = 0, taken = -1;
    if (!events || !datamodels) PyErr_NoMemory();
    else {
        for (; made < slice->count; made++) {
            PyObject *entry = PyList_GetItem(slice->list, slice->first + made);
            Py_ssize_t size = PyTuple_Check(entry) ? PyTuple_Size(entry) : 0;
            if (size < 2 || size > 4) {
                PyErr_SetString(PyExc_TypeError, "a send_many item is (session, event[, data[, traceparent]])");
                break;
            }
            ta_python_send_item item = {PyTuple_GetItem(entry, 0), PyTuple_GetItem(entry, 1),
                                        size > 2 ? PyTuple_GetItem(entry, 2) : Py_None,
                                        size > 3 ? PyTuple_GetItem(entry, 3) : Py_None, TA_NIL};
            if (ta_python_event(host, &item, &events[made]) < 0) break;
            datamodels[made] = item.datamodel;
        }
        if (made == slice->count) taken = ta_host_send_many(host->host, events, (uint32_t)made);
        for (Py_ssize_t i = taken < 0 ? 0 : taken; i < made; i++)
            ta_python_host_payload_release(host, events[i].data, datamodels[i]); /* still ours */
    }
    ta_growable_arena_release(&host->arena, mark);
    return taken;
}

/* send_many(items): each item (session, event[, data[, traceparent]]),
 * handed to core in slices (ta_host_send_many). Returns the length of the
 * accepted prefix; the payloads after it are released (ours). An item
 * that cannot be converted raises, and the slices before its own were
 * sent. */
static PyObject *ta_python_host_send_many(PyObject *self, PyObject *items) {
    ta_python_host *host = (ta_python_host *)self;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *list = PySequence_List(items), *result = NULL;
    Py_ssize_t count = list ? PyList_Size(list) : 0, accepted = 0;
    int failed = list ? 0 : 1;
    for (Py_ssize_t first = 0; !failed && first < count; first += TA_PYTHON_SLICE) {
        ta_python_slice slice = {list, first, count - first < TA_PYTHON_SLICE ? count - first : TA_PYTHON_SLICE};
        Py_ssize_t taken = ta_python_send_slice(host, &slice);
        if (taken < 0) failed = 1;
        else accepted += taken;
        if (taken != slice.count) break; /* refused at an event: the rest is not sent */
    }
    if (accepted) ta_python_loop_schedule(host);
    if (!failed) result = PyLong_FromSsize_t(accepted);
    Py_XDECREF(list); /* after the calls: the views pointed into its items */
    ta_python_leave(&call);
    return result;
}

static PyObject *ta_python_host_terminate(PyObject *self, PyObject *value) {
    ta_python_host *host = (ta_python_host *)self;
    uint64_t session;
    if (ta_python_session(value, &session) < 0) return NULL;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    ta_status status = ta_host_terminate(host->host, session);
    if (status == TA_OK) ta_python_loop_schedule(host);
    PyObject *result = ta_python_host_status(host, status);
    ta_python_leave(&call);
    return result;
}

/* A peer: another system through one connection (ta_host_peer_add). */
static PyObject *ta_python_host_peer_add(PyObject *self, PyObject *arguments, PyObject *keywords) {
    static char *const names[] = {"id", "trusted", NULL};
    ta_python_host *host = (ta_python_host *)self;
    PyObject *id;
    int trusted = 0;
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "U|$p:peer_add", names, &id, &trusted)) return NULL;
    ta_python_call call;
    if (ta_python_enter(host, &call) < 0) return NULL;
    PyObject *result = NULL;
    ta_peer_config peer;
    memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    peer.flags = trusted ? TA_PEER_TRUSTED : 0;
    if (ta_python_text(id, &peer.id) == 0) {
        uint32_t handle = 0;
        ta_status status = ta_host_peer_add(host->host, &peer, &handle);
        result = status == TA_OK ? PyLong_FromUnsignedLong(handle) : ta_python_host_status(host, status);
    }
    ta_python_leave(&call);
    return result;
}

/* deliver(peer, envelope): an envelope's bytes from `peer` (0: this
 * host's own code): ta_host_envelope_read, then ta_host_deliver, also
 * for data that is not one item (deliver answers it as malformed,
 * host.h). Returns the TA_ROUTE_* outcome. */
static PyObject *ta_python_host_deliver(PyObject *self, PyObject *arguments) {
    ta_python_host *host = (ta_python_host *)self;
    unsigned long peer;
    Py_buffer view;
    if (!PyArg_ParseTuple(arguments, "ky*:deliver", &peer, &view)) return NULL;
    ta_python_call call;
    PyObject *result = NULL;
    if (ta_python_enter(host, &call) == 0) {
        ta_envelope envelope;
        memset(&envelope, 0, sizeof envelope);
        ta_status status = ta_host_envelope_read(host->host, (ta_bytes){view.buf, (size_t)view.len}, &envelope);
        int32_t route = status == TA_OK || envelope.size ? ta_host_deliver(host->host, (uint32_t)peer, &envelope) : status;
        if (route >= 0) {
            result = PyLong_FromLong(route);
            ta_python_loop_schedule(host);
        } else ta_python_raise_status(host->state, route);
        ta_python_leave(&call);
    }
    PyBuffer_Release(&view);
    return result;
}

/* ---- context managers ---------------------------------------------------------------- */

static PyObject *ta_python_host_enter_with(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    if (host->has_async) {
        for (Py_ssize_t i = 0; i < PyList_Size(host->processors); i++) {
            PyObject *row = PyList_GetItem(host->processors, i);
            if (PyTuple_GetItem(row, 2) == Py_True) {
                PyErr_Format(PyExc_TypeError,
                             "the I/O processor %R has an async def handler: use async with, the synchronous driver "
                             "runs no event loop",
                             PyTuple_GetItem(row, 0));
                return NULL;
            }
        }
    }
    return Py_NewRef(self);
}

static PyObject *ta_python_host_exit_with(PyObject *self, PyObject *arguments) {
    (void)arguments;
    ta_python_host_close((ta_python_host *)self, 0);
    Py_RETURN_FALSE;
}

/* A future of the running loop holding `value` already. */
static PyObject *ta_python_done_future(PyObject *loop, PyObject *value) {
    PyObject *future = PyObject_CallMethod(loop, "create_future", NULL);
    if (!future) return NULL;
    PyObject *set = PyObject_CallMethod(future, "set_result", "O", value);
    if (!set) {
        Py_DECREF(future);
        return NULL;
    }
    Py_DECREF(set);
    return future;
}

static PyObject *ta_python_running_loop(void) {
    PyObject *asyncio = PyImport_ImportModule("asyncio");
    if (!asyncio) return NULL;
    PyObject *loop = PyObject_CallMethod(asyncio, "get_running_loop", NULL);
    Py_DECREF(asyncio);
    return loop;
}

static PyObject *ta_python_host_enter_async(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    PyObject *loop = ta_python_running_loop();
    if (!loop) return NULL;
    ta_python_replace(&host->loop, loop);
    return ta_python_done_future(loop, self);
}

static PyObject *ta_python_host_exit_async(PyObject *self, PyObject *arguments) {
    (void)arguments;
    ta_python_host *host = (ta_python_host *)self;
    PyObject *loop = Py_NewRef(host->loop);
    ta_python_host_close(host, 0);
    PyObject *result = loop != Py_None ? ta_python_done_future(loop, Py_False) : NULL;
    Py_DECREF(loop);
    if (!result && !PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "async with outside an event loop");
    return result;
}

static PyObject *ta_python_host_closed(PyObject *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(((ta_python_host *)self)->host == NULL);
}

static PyObject *ta_python_host_ticks(PyObject *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(((ta_python_host *)self)->ticks_per_second);
}

static PyGetSetDef ta_python_host_properties[] = {
    {"closed", ta_python_host_closed, NULL, "True once the host was freed.", NULL},
    {"ticks_per_second", ta_python_host_ticks, NULL, "host.config's system.ticksPerSecond, read at creation.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

PyObject *ta_python_method_actor_state(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_actor_context(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_actor_list(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_actor_get(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_host_stats(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_host_config(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_configure(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_watch(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_unwatch(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_end(PyObject *self, PyObject *arguments, PyObject *keywords);
PyObject *ta_python_method_trace_spans_decode(PyObject *self, PyObject *arguments, PyObject *keywords);

#define TA_PYTHON_TYPED(name, method)                                                                          \
    {name, (PyCFunction)(void (*)(void))method, METH_VARARGS | METH_KEYWORDS,                                  \
     name "(handler, on_error=None, /, **params)\n--\n\nThe typed method over the posted message of the same "  \
     "name (docs/requests.md): params passed through as given; handler(host, result) or on_error(host, error) "\
     "runs at a later drain."}

static PyMethodDef ta_python_host_methods[] = {
    {"add_image", ta_python_host_add_image, METH_O,
     "add_image(image, /)\n--\n\nA new segment running the image (any buffer: Compiled.image, the bytes of a .tai); "
     "the host copies it. Returns the segment."},
    {"chart", ta_python_host_chart, METH_O, "chart(name, /)\n--\n\nThe current version of chart `name`: a ChartRef."},
    {"spawn", (PyCFunction)(void (*)(void))ta_python_host_spawn, METH_VARARGS | METH_KEYWORDS,
     "spawn(ref, name=None, params=None, traceparent=None)\n--\n\nA root actor of the chart; returns its session."},
    {"send", (PyCFunction)(void (*)(void))ta_python_host_send, METH_VARARGS | METH_KEYWORDS,
     "send(session, event, data=None, *, traceparent=None)\n--\n\nAn event for the session, applied at the call."},
    {"send_many", ta_python_host_send_many, METH_O,
     "send_many(items, /)\n--\n\nEvents in order, each (session, event[, data[, traceparent]]); returns the length "
     "of the accepted prefix."},
    {"terminate", ta_python_host_terminate, METH_O, "terminate(session, /)\n--\n\nEnd a root actor."},
    {"peer_add", (PyCFunction)(void (*)(void))ta_python_host_peer_add, METH_VARARGS | METH_KEYWORDS,
     "peer_add(id, *, trusted=False)\n--\n\nAnother system through one connection; returns its peer handle."},
    {"deliver", ta_python_host_deliver, METH_VARARGS,
     "deliver(peer, envelope, /)\n--\n\nAn envelope's bytes from `peer` (0: this host's code); returns the route "
     "outcome (0 delivered, 1 relayed, 2 duplicate, 3 refused)."},
    {"run_until_idle", ta_python_host_run_until_idle, METH_NOARGS,
     "run_until_idle()\n--\n\nTurn until the host is idle at the current time."},
    {"run", ta_python_host_run_forever, METH_NOARGS,
     "run()\n--\n\nTurn, sleeping to each deadline, until the host is idle with no deadline left."},
    {"idle", ta_python_host_idle, METH_NOARGS, "idle()\n--\n\nasyncio: a future done when the host is idle."},
    {"close", ta_python_host_close_method, METH_NOARGS,
     "close()\n--\n\nFree the host: trace.end, the last take, ta_host_free; pending handlers are dropped."},
    {"__enter__", ta_python_host_enter_with, METH_NOARGS, NULL},
    {"__exit__", ta_python_host_exit_with, METH_VARARGS, NULL},
    {"__aenter__", ta_python_host_enter_async, METH_NOARGS, NULL},
    {"__aexit__", ta_python_host_exit_async, METH_VARARGS, NULL},
    TA_PYTHON_TYPED("actor_state", ta_python_method_actor_state),
    TA_PYTHON_TYPED("actor_context", ta_python_method_actor_context),
    TA_PYTHON_TYPED("actor_list", ta_python_method_actor_list),
    TA_PYTHON_TYPED("actor_get", ta_python_method_actor_get),
    TA_PYTHON_TYPED("host_stats", ta_python_method_host_stats),
    TA_PYTHON_TYPED("host_config", ta_python_method_host_config),
    TA_PYTHON_TYPED("trace_configure", ta_python_method_trace_configure),
    TA_PYTHON_TYPED("trace_watch", ta_python_method_trace_watch),
    TA_PYTHON_TYPED("trace_unwatch", ta_python_method_trace_unwatch),
    TA_PYTHON_TYPED("trace_end", ta_python_method_trace_end),
    TA_PYTHON_TYPED("trace_spans_decode", ta_python_method_trace_spans_decode),
    {NULL, NULL, 0, NULL}
};

/* Weak references to a Host (the limited API's way: a member naming the
 * slot): a SimulatedClock holds its hosts weakly. */
static PyMemberDef ta_python_host_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(ta_python_host, weakreferences), Py_READONLY, NULL},
    {NULL, 0, 0, 0, NULL}
};

static PyType_Slot ta_python_host_slots[] = {
    {Py_tp_members, ta_python_host_members},
    {Py_tp_doc, "Host(config, io_processors=None, *, log=None, clock=None, peer=None)\n--\n\n"
                "A host (ta_host): many actor systems as one, with the \"python\" datamodel and quickjs."},
    {Py_tp_new, ta_python_host_create},
    {Py_tp_dealloc, ta_python_host_dealloc},
    {Py_tp_traverse, ta_python_host_traverse},
    {Py_tp_clear, ta_python_host_clear},
    {Py_tp_methods, ta_python_host_methods},
    {Py_tp_getset, ta_python_host_properties},
    {0, NULL}
};

static PyType_Spec ta_python_host_spec = {
    "tinyactors.Host", sizeof(ta_python_host), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE, ta_python_host_slots
};

int ta_python_host_init_type(PyObject *module, ta_python_module_state *state) {
    state->host_type = PyType_FromModuleAndSpec(module, &ta_python_host_spec, NULL);
    if (!state->host_type || PyModule_AddObjectRef(module, "Host", state->host_type) < 0) return -1;
    state->chart_type = PyType_FromModuleAndSpec(module, &ta_python_chart_spec, NULL);
    if (!state->chart_type) return -1;
    return PyModule_AddObjectRef(module, "ChartRef", state->chart_type);
}
