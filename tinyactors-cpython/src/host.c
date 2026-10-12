/* host.c — tinyactors.Host: one ta_host (tinyactors/host.h) with the
 * "python" datamodel and quickjs (tinyactors-core/docs/bindings.md,
 * section 8), its images, its clock and the application's handlers.
 *
 * Every input (boot, spawn, send, send_many, terminate) runs the turn at
 * the current time (loop.c) once core took it; a call from a handler,
 * inside the turn, only makes the turn run again (bindings.md, section 5:
 * "Between runs, any call is fine, including from an effect handler"). */
#include "extension.h"

#include "pythread.h"

/* ---- the host's allocator ------------------------------------------------------------ */

/* PyMem_RawMalloc gives 16-byte alignment and core asks for at most 8
 * (AGENTS.md, "The host's allocator"); the raw domain needs no GIL. */
static void *ta_python_alloc(void *context, size_t size, size_t align) {
    (void)context;
    assert(align <= 8);
    (void)align;
    return PyMem_RawMalloc(size ? size : 1);
}

static void ta_python_free(void *context, void *ptr, size_t size) {
    (void)context;
    (void)size;
    PyMem_RawFree(ptr);
}

static const ta_allocator ta_python_allocator = {NULL, ta_python_alloc, ta_python_free};

/* ---- the host's state ----------------------------------------------------------------- */

const ta_python_state *ta_python_host_state(const ta_python_host *host) {
    return ta_python_state_of(host->module);
}

int ta_python_host_usable(ta_python_host *host) {
    const ta_python_state *state = ta_python_host_state(host);
    if (host->thread != PyThread_get_thread_ident()) { /* one thread: the creating one (bindings.md, section 5) */
        ta_python_raise_status(state, "Host: another thread", TA_ERROR_STATE);
        return 0;
    }
    if (!host->host || host->closing) {
        ta_python_raise_status(state, "Host: closed", TA_ERROR_STATE);
        return 0;
    }
    return 1;
}

void ta_python_host_free(ta_python_host *host) {
    if (host->clock) ta_python_clock_detach(host->clock, host);
    if (host->host) ta_host_free(host->host);
    host->host = NULL;
    if (host->ecmascript) ta_datamodel_ecmascript_free(host->ecmascript);
    host->ecmascript = NULL;
    for (uint32_t i = 0; i < host->image_count; i++) {
        if (!host->images[i]) continue;
        PyMem_RawFree(host->images[i]->bytes);
        PyMem_RawFree(host->images[i]);
    }
    PyMem_RawFree(host->images);
    host->images = NULL;
    host->image_count = host->image_capacity = 0;
    host->closing = 0;
}

/* ---- payloads ---------------------------------------------------------------------------- */

ta_status ta_python_payload_make(ta_python_host *host, const ta_python_payload_making *making, ta_payload *out) {
    *out = 0;
    if (!making->value) return TA_OK;
    if (making->datamodel >= host->datamodel_count) return TA_ERROR_INVALID; /* a chart of no datamodel we have */
    const ta_datamodel_binding *binding = &host->datamodels[making->datamodel];
    if (!binding->vtable->from_wire) return TA_ERROR_UNSUPPORTED;
    PyObject *bytes = NULL;
    ta_status status = ta_python_values_bytes(ta_python_host_state(host), making->value, &bytes);
    if (status != TA_OK) return status;
    status = binding->vtable->from_wire(binding->datamodel,
                                        (ta_bytes){PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes)}, out);
    Py_DECREF(bytes);
    return status;
}

PyObject *ta_python_payload_value(ta_python_host *host, const ta_python_payload_reading *reading) {
    const ta_python_state *state = ta_python_host_state(host);
    if (!reading->payload || reading->datamodel >= host->datamodel_count) Py_RETURN_NONE;
    const ta_datamodel_binding *binding = &host->datamodels[reading->datamodel];
    char room[1024];
    ta_buffer buffer = {room, sizeof room, 0};
    ta_status status = binding->vtable->to_wire(binding->datamodel, reading->payload, &buffer);
    if (status == TA_ERROR_NO_MEMORY && buffer.length > buffer.capacity) {
        size_t length = buffer.length;
        buffer = (ta_buffer){PyMem_Malloc(length), length, 0};
        status = buffer.pointer ? binding->vtable->to_wire(binding->datamodel, reading->payload, &buffer)
                                : TA_ERROR_NO_MEMORY;
    }
    PyObject *value = NULL;
    if (status == TA_OK) status = ta_python_values_read(state, (ta_bytes){buffer.pointer, buffer.length}, &value);
    if (buffer.pointer != room) PyMem_Free(buffer.pointer);
    if (status != TA_OK && !PyErr_Occurred()) ta_python_raise_status(state, "to_wire", status);
    return value;
}

void ta_python_payload_release(ta_python_host *host, const ta_python_payload_reading *reading) {
    if (reading->payload && reading->datamodel < host->datamodel_count)
        host->datamodels[reading->datamodel].vtable->release(host->datamodels[reading->datamodel].datamodel,
                                                            reading->payload);
}

/* The index in host->datamodels of the datamodel chart `chart` of
 * `segment` names; TA_NIL when the host has none of that name. */
static uint32_t ta_python_chart_datamodel(ta_python_host *host, const ta_host_chart_ref *chart) {
    ta_host_segment_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (ta_host_segment_info_get(host->host, chart->segment, &info) != TA_OK || !info.image) return TA_NIL;
    ta_string_view name = ta_image_chart_datamodel(info.image, chart->chart);
    for (uint32_t i = 0; i < host->datamodel_count; i++) {
        const char *ours = host->datamodels[i].vtable->name;
        if (strlen(ours) == name.length && !memcmp(ours, name.pointer, name.length)) return i;
    }
    return TA_NIL;
}

/* The datamodel of the chart session `session` runs. */
static uint32_t ta_python_session_datamodel(ta_python_host *host, uint64_t session) {
    ta_host_location location;
    if (ta_host_locate(host->host, session, &location) != TA_OK) return TA_NIL;
    ta_actor_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (ta_actor_info_get(ta_host_system(host->host, location.segment), location.actor, &info) != TA_OK) return TA_NIL;
    return ta_python_chart_datamodel(host, &(ta_host_chart_ref){location.segment, info.chart});
}

/* ---- arguments ---------------------------------------------------------------------------- */

/* A session id: an int from 1 to 2^64-1. */
static int ta_python_session_of(PyObject *object, uint64_t *out) {
    if (!PyLong_Check(object)) {
        PyErr_SetString(PyExc_TypeError, "a session is an int");
        return 0;
    }
    *out = PyLong_AsUnsignedLongLong(object);
    return !PyErr_Occurred();
}

/* The UTF-8 of a str, borrowed from it. */
static int ta_python_text_of(PyObject *object, ta_string_view *out) {
    Py_ssize_t length = 0;
    const char *text = PyUnicode_Check(object) ? PyUnicode_AsUTF8AndSize(object, &length) : NULL;
    if (!text) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "expected a str");
        return 0;
    }
    *out = (ta_string_view){text, (uint32_t)length};
    return 1;
}

/* ---- Host(config, *, clock=None, on_log=None, on_done=None, on_error=None, on_peer=None) ---- */

static const char *const ta_python_handler_names[TA_PYTHON_HANDLERS] = {"on_log", "on_done", "on_error", "on_peer"};

/* The config's system.ticksPerSecond, 0 when it has none (ROADMAP.md,
 * request 2). */
static uint64_t ta_python_config_ticks(PyObject *config) {
    PyObject *system = NULL, *ticks = NULL;
    uint64_t value = 0;
    if (PyDict_Check(config) && PyDict_GetItemStringRef(config, "system", &system) > 0 && PyDict_Check(system) &&
        PyDict_GetItemStringRef(system, "ticksPerSecond", &ticks) > 0 && PyLong_Check(ticks) && !PyBool_Check(ticks))
        value = PyLong_AsUnsignedLongLong(ticks);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        value = 0;
    }
    Py_XDECREF(system);
    Py_XDECREF(ticks);
    return value;
}

/* ta_host_create with the config's CBOR; a refused config raises the
 * error core wrote, unchanged (bindings.md, section 3). */
static int ta_python_host_create(ta_python_host *host, PyObject *config) {
    const ta_python_state *state = ta_python_host_state(host);
    PyObject *bytes = NULL;
    ta_status status = ta_python_values_bytes(state, config, &bytes);
    if (status != TA_OK) {
        if (!PyErr_Occurred()) ta_python_raise_status(state, "ta_cbor_encode", status);
        return -1;
    }
    char error[8192];
    ta_buffer refusal = {error, sizeof error, 0};
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = host->datamodel_count;
    environment.datamodels = host->datamodels;
    environment.allocator = ta_python_allocator;
    environment.error = &refusal;
    status = ta_host_create((ta_bytes){PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes)}, &environment,
                            &host->host);
    Py_DECREF(bytes);
    if (status == TA_OK) return 0;
    host->host = NULL;
    PyObject *reply = NULL, *refused = NULL;
    if (refusal.length && refusal.length <= refusal.capacity &&
        ta_python_values_read(state, (ta_bytes){error, refusal.length}, &reply) == TA_OK && PyDict_Check(reply) &&
        PyDict_GetItemStringRef(reply, "error", &refused) > 0)
        ta_python_raise_reply(state, &(ta_python_refusal){refused, status});
    else if (status == TA_ERROR_NO_MEMORY) PyErr_NoMemory();
    else ta_python_raise_status(state, "ta_host_create", status);
    Py_XDECREF(reply);
    Py_XDECREF(refused);
    return -1;
}

static PyObject *ta_python_host_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"config", "clock", "on_log", "on_done", "on_error", "on_peer", NULL};
    PyObject *config = NULL, *clock = Py_None, *handlers[TA_PYTHON_HANDLERS] = {Py_None, Py_None, Py_None, Py_None};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$OOOOO:Host", (char **)keywords, &config, &clock, &handlers[0],
                                     &handlers[1], &handlers[2], &handlers[3]))
        return NULL;
    PyObject *module = PyType_GetModule(type);
    const ta_python_state *state = module ? ta_python_state_of(module) : NULL;
    if (!state) return NULL;
    if (clock != Py_None && !PyObject_TypeCheck(clock, (PyTypeObject *)state->clock_type)) {
        PyErr_SetString(PyExc_TypeError, "clock is a tinyactors.SimulatedClock or None");
        return NULL;
    }
    for (int i = 0; i < TA_PYTHON_HANDLERS; i++)
        if (handlers[i] != Py_None && !PyCallable_Check(handlers[i])) {
            PyErr_Format(PyExc_TypeError, "%s is callable or None", ta_python_handler_names[i]);
            return NULL;
        }
    ta_python_host *host = (ta_python_host *)PyType_GenericAlloc(type, 0);
    if (!host) return NULL;
    host->module = Py_NewRef(module);
    host->thread = PyThread_get_thread_ident();
    host->next_deadline = UINT64_MAX;
    host->ticks_per_second = ta_python_config_ticks(config);
    for (int i = 0; i < TA_PYTHON_HANDLERS; i++)
        host->handlers[i] = handlers[i] == Py_None ? NULL : Py_NewRef(handlers[i]);
    if (ta_python_loop_start(host) < 0) goto fail;
    host->ecmascript = ta_datamodel_ecmascript_new(NULL);
    if (!host->ecmascript) {
        PyErr_NoMemory();
        goto fail;
    }
    host->datamodels[host->datamodel_count++] = ta_datamodel_ecmascript_binding(host->ecmascript);
    if (ta_python_host_create(host, config) < 0) goto fail;
    if (clock != Py_None && ta_python_clock_attach(clock, host) < 0) goto fail;
    return (PyObject *)host;
fail:
    Py_DECREF(host);
    return NULL;
}

static int ta_python_host_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_host *host = (ta_python_host *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(host->module);
    Py_VISIT(host->clock);
    for (int i = 0; i < TA_PYTHON_HANDLERS; i++)
        Py_VISIT(host->handlers[i]);
    Py_VISIT(host->raised);
    return 0;
}

/* The cycle collector breaks a cycle through a handler here (bindings.md,
 * section 6): the host is freed with the references it held. */
static int ta_python_host_clear(PyObject *self) {
    ta_python_host *host = (ta_python_host *)self;
    if (!host->looping) ta_python_host_free(host);
    Py_CLEAR(host->clock);
    for (int i = 0; i < TA_PYTHON_HANDLERS; i++)
        Py_CLEAR(host->handlers[i]);
    Py_CLEAR(host->raised);
    return 0;
}

static void ta_python_host_dealloc(PyObject *self) {
    ta_python_host *host = (ta_python_host *)self;
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    if (host->weakreferences) PyObject_ClearWeakRefs(self);
    ta_python_host_clear(self);
    ta_python_host_free(host);
    Py_CLEAR(host->module);
    PyObject_GC_Del(self);
    Py_DECREF(type);
}

/* ---- close, the context manager ---------------------------------------------------------- */

/* Frees the host now, or, from a handler, once the turn ends (bindings.md,
 * section 5: "mark it, finish the loop, then free"). */
static PyObject *ta_python_host_close(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_host *host = (ta_python_host *)self;
    if (host->thread != PyThread_get_thread_ident()) {
        ta_python_raise_status(ta_python_host_state(host), "Host: another thread", TA_ERROR_STATE);
        return NULL;
    }
    if (host->looping) host->closing = 1;
    else ta_python_host_free(host);
    Py_RETURN_NONE;
}

static PyObject *ta_python_host_enter(PyObject *self, PyObject *unused) {
    (void)unused;
    return Py_NewRef(self);
}

static PyObject *ta_python_host_exit(PyObject *self, PyObject *args) {
    (void)args;
    PyObject *closed = ta_python_host_close(self, NULL);
    if (!closed) return NULL;
    Py_DECREF(closed);
    Py_RETURN_FALSE;
}

/* ---- images ---------------------------------------------------------------------------- */

/* A src="" of a chart: the document `files` maps the src to, as written. */
static ta_status ta_python_resolve(void *context, const ta_resolve_request *request, ta_source *out) {
    PyObject *files = (PyObject *)context, *key, *text = NULL;
    if (!files) return TA_ERROR_INVALID;
    key = PyUnicode_DecodeUTF8(request->source.pointer, (Py_ssize_t)request->source.length, "strict");
    int found = key ? PyDict_GetItemRef(files, key, &text) : -1;
    Py_XDECREF(key);
    ta_string_view view;
    int ok = found > 0 && ta_python_text_of(text, &view);
    if (found > 0) Py_DECREF(text); /* the dict holds it while the compile runs */
    if (!ok) {
        PyErr_Clear();
        return TA_ERROR_INVALID;
    }
    out->name = request->source;
    out->text = view;
    return TA_OK;
}

/* A refused compile: the TinyactorsError, with `diagnostics`, core's, as
 * (severity, code, line, column, file, message) tuples. */
static void ta_python_compile_refused(const ta_python_state *state, ta_compile_result *result, ta_status status) {
    ta_python_raise_status(state, "ta_compile_charts", status);
    PyObject *error = PyErr_GetRaisedException();
    const ta_diagnostic *diagnostics = NULL;
    uint32_t count = result ? ta_compile_diagnostics(result, &diagnostics) : 0;
    PyObject *list = PyList_New(0);
    for (uint32_t i = 0; list && i < count; i++) {
        const ta_diagnostic *diagnostic = &diagnostics[i];
        PyObject *item = Py_BuildValue("(IIIIs#s#)", diagnostic->severity, diagnostic->code, diagnostic->line,
                                       diagnostic->column, diagnostic->file.pointer ? diagnostic->file.pointer : "",
                                       (Py_ssize_t)diagnostic->file.length,
                                       diagnostic->message.pointer ? diagnostic->message.pointer : "",
                                       (Py_ssize_t)diagnostic->message.length);
        if (!item || PyList_Append(list, item) < 0) Py_CLEAR(list);
        Py_XDECREF(item);
    }
    if (list && error) PyObject_SetAttrString(error, "diagnostics", list);
    Py_XDECREF(list);
    PyErr_Clear();
    PyErr_SetRaisedException(error);
}

/* The charts of `charts` (a dict, name to SCXML text, the first the
 * image's default chart) compiled into an image. */
typedef struct ta_python_compiling {
    PyObject *charts;
    PyObject *files;
} ta_python_compiling;

static ta_python_image *ta_python_compile(ta_python_host *host, const ta_python_compiling *compiling) {
    const ta_python_state *state = ta_python_host_state(host);
    Py_ssize_t count = PyDict_Size(compiling->charts), position = 0, i = 0;
    ta_source *sources = PyMem_Malloc((size_t)(count ? count : 1) * sizeof *sources);
    if (!sources) return (ta_python_image *)PyErr_NoMemory();
    PyObject *name, *text;
    while (PyDict_Next(compiling->charts, &position, &name, &text)) {
        if (!ta_python_text_of(name, &sources[i].name) || !ta_python_text_of(text, &sources[i].text)) {
            PyMem_Free(sources);
            return NULL;
        }
        i++;
    }
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = ta_python_allocator;
    options.resolve = ta_python_resolve;
    options.resolve_context = compiling->files;
    ta_compile_result *result = NULL;
    ta_status status = ta_compile_charts(&options, (ta_source_list){sources, (uint32_t)count}, &result);
    PyMem_Free(sources);
    if (status != TA_OK) {
        ta_python_compile_refused(state, result, status);
        if (result) ta_compile_free(result);
        return NULL;
    }
    ta_string_view bytes = ta_compile_image(result);
    ta_python_image *image = PyMem_RawMalloc(sizeof *image);
    void *copy = PyMem_RawMalloc(bytes.length ? bytes.length : 1);
    if (!image || !copy) {
        PyMem_RawFree(image);
        PyMem_RawFree(copy);
        ta_compile_free(result);
        return (ta_python_image *)PyErr_NoMemory();
    }
    memcpy(copy, bytes.pointer, bytes.length);
    ta_compile_free(result);
    image->bytes = copy;
    status = ta_image_open((ta_bytes){copy, bytes.length}, &image->image);
    if (status != TA_OK) {
        PyMem_RawFree(copy);
        PyMem_RawFree(image);
        ta_python_raise_status(state, "ta_image_open", status);
        return NULL;
    }
    return image;
}

/* Keeps `image` as segment `segment`'s. */
static int ta_python_image_keep(ta_python_host *host, ta_python_image *image, uint32_t segment) {
    if (segment >= host->image_capacity) {
        uint32_t capacity = host->image_capacity ? host->image_capacity * 2 : 4;
        while (capacity <= segment)
            capacity *= 2;
        ta_python_image **grown = PyMem_RawMalloc(capacity * sizeof *grown);
        if (!grown) return -1;
        memset(grown, 0, capacity * sizeof *grown);
        if (host->image_count) memcpy(grown, host->images, host->image_count * sizeof *grown);
        PyMem_RawFree(host->images);
        host->images = grown;
        host->image_capacity = capacity;
    }
    host->images[segment] = image;
    if (segment >= host->image_count) host->image_count = segment + 1;
    return 0;
}

/* add_image(charts, files=None) -> segment */
static PyObject *ta_python_host_add_image(PyObject *self, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"charts", "files", NULL};
    ta_python_host *host = (ta_python_host *)self;
    ta_python_compiling compiling = {NULL, NULL};
    PyObject *files = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!|O:add_image", (char **)keywords, &PyDict_Type,
                                     &compiling.charts, &files) ||
        !ta_python_host_usable(host))
        return NULL;
    if (files != Py_None && !PyDict_Check(files)) {
        PyErr_SetString(PyExc_TypeError, "files is a dict or None");
        return NULL;
    }
    compiling.files = files == Py_None ? NULL : files;
    ta_python_image *image = ta_python_compile(host, &compiling);
    if (!image) return NULL;
    uint32_t segment = 0;
    ta_status status = ta_host_add_image(host->host, &image->image, &segment);
    if (status == TA_OK && ta_python_image_keep(host, image, segment) < 0) {
        PyMem_RawFree(image->bytes); /* never reached core's segment: it holds the image, so this cannot be */
        PyMem_RawFree(image);
        return PyErr_NoMemory();
    }
    if (status != TA_OK) {
        PyMem_RawFree(image->bytes);
        PyMem_RawFree(image);
        ta_python_raise_status(ta_python_host_state(host), "ta_host_add_image", status);
        return NULL;
    }
    return PyLong_FromUnsignedLong(segment);
}

/* Drops segment `segment`'s image (TA_HOST_EFFECT_SEGMENT_FREED). */
void ta_python_image_drop(ta_python_host *host, uint32_t segment) {
    if (segment >= host->image_count || !host->images[segment]) return;
    PyMem_RawFree(host->images[segment]->bytes);
    PyMem_RawFree(host->images[segment]);
    host->images[segment] = NULL;
}

/* ---- inputs ------------------------------------------------------------------------------ */

/* After an input core took: the turn, at the current time. */
static PyObject *ta_python_after_input(ta_python_host *host, PyObject *result) {
    if (!result) return NULL;
    if (ta_python_loop_turn(host) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

/* boot(segment) */
static PyObject *ta_python_host_boot(PyObject *self, PyObject *args) {
    ta_python_host *host = (ta_python_host *)self;
    unsigned int segment = 0;
    if (!PyArg_ParseTuple(args, "I:boot", &segment) || !ta_python_host_usable(host)) return NULL;
    ta_status status = ta_host_boot(host->host, segment);
    if (status != TA_OK) {
        ta_python_raise_status(ta_python_host_state(host), "ta_host_boot", status);
        return NULL;
    }
    return ta_python_after_input(host, Py_NewRef(Py_None));
}

/* spawn(chart, params=None, name=None) -> session */
static PyObject *ta_python_host_spawn(PyObject *self, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"chart", "params", "name", NULL};
    ta_python_host *host = (ta_python_host *)self;
    PyObject *chart = NULL, *params = Py_None, *name = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "U|OO:spawn", (char **)keywords, &chart, &params, &name) ||
        !ta_python_host_usable(host))
        return NULL;
    const ta_python_state *state = ta_python_host_state(host);
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    ta_string_view chart_name;
    if (!ta_python_text_of(chart, &chart_name) || (name != Py_None && !ta_python_text_of(name, &spawn.name)))
        return NULL;
    ta_host_chart_ref reference;
    ta_status status = ta_host_chart(host->host, chart_name, &reference);
    if (status != TA_OK) {
        ta_python_raise_status(state, "ta_host_chart", status);
        return NULL;
    }
    uint32_t datamodel = ta_python_chart_datamodel(host, &reference);
    status = ta_python_payload_make(host, &(ta_python_payload_making){params == Py_None ? NULL : params, datamodel},
                                    &spawn.params);
    if (status != TA_OK) {
        if (!PyErr_Occurred()) ta_python_raise_status(state, "from_wire", status);
        return NULL;
    }
    spawn.segment = reference.segment;
    spawn.chart = reference.chart;
    uint64_t session = 0;
    status = ta_host_spawn(host->host, &spawn, &session);
    if (status != TA_OK) {
        ta_python_payload_release(host, &(ta_python_payload_reading){spawn.params, datamodel});
        ta_python_raise_status(state, "ta_host_spawn", status);
        return NULL;
    }
    return ta_python_after_input(host, PyLong_FromUnsignedLongLong(session));
}

/* The event of send() and of each item of send_many(), its payload made
 * for the target's datamodel. */
typedef struct ta_python_event_reading {
    PyObject *session, *event, *data; /* data: NULL for none */
} ta_python_event_reading;

static int ta_python_event_read(ta_python_host *host, const ta_python_event_reading *reading, ta_event_in *event) {
    memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    if (!ta_python_session_of(reading->session, &event->session) || !ta_python_text_of(reading->event, &event->name))
        return 0;
    ta_status status = ta_python_payload_make(
        host, &(ta_python_payload_making){reading->data, ta_python_session_datamodel(host, event->session)},
        &event->data);
    if (status == TA_OK) return 1;
    if (!PyErr_Occurred()) ta_python_raise_status(ta_python_host_state(host), "from_wire", status);
    return 0;
}

static void ta_python_event_release(ta_python_host *host, const ta_event_in *event) {
    if (event->data)
        ta_python_payload_release(
            host, &(ta_python_payload_reading){event->data, ta_python_session_datamodel(host, event->session)});
}

/* send(session, event, data=None) */
static PyObject *ta_python_host_send(PyObject *self, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"session", "event", "data", NULL};
    ta_python_host *host = (ta_python_host *)self;
    ta_python_event_reading reading = {NULL, NULL, Py_None};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OU|O:send", (char **)keywords, &reading.session, &reading.event,
                                     &reading.data) ||
        !ta_python_host_usable(host))
        return NULL;
    if (reading.data == Py_None) reading.data = NULL;
    ta_event_in event;
    if (!ta_python_event_read(host, &reading, &event)) return NULL;
    ta_status status = ta_host_send(host->host, &event);
    if (status != TA_OK) {
        ta_python_event_release(host, &event);
        ta_python_raise_status(ta_python_host_state(host), "ta_host_send", status);
        return NULL;
    }
    return ta_python_after_input(host, Py_NewRef(Py_None));
}

/* send_many(events) -> int: each a (session, event) or (session, event,
 * data) tuple; how many core took, in order (ta_host_send_many). */
static PyObject *ta_python_host_send_many(PyObject *self, PyObject *events) {
    ta_python_host *host = (ta_python_host *)self;
    if (!ta_python_host_usable(host)) return NULL;
    PyObject *list = PySequence_List(events);
    if (!list) return NULL;
    Py_ssize_t count = PyList_Size(list), read = 0;
    ta_event_in *batch = PyMem_Malloc((size_t)(count ? count : 1) * sizeof *batch);
    if (!batch) {
        Py_DECREF(list);
        return PyErr_NoMemory();
    }
    for (; read < count; read++) {
        PyObject *item = PyList_GetItem(list, read);
        Py_ssize_t size = PyTuple_Check(item) ? PyTuple_Size(item) : 0;
        if (size != 2 && size != 3) {
            PyErr_SetString(PyExc_TypeError, "an event is a (session, event) or (session, event, data) tuple");
            break;
        }
        PyObject *data = size == 3 ? PyTuple_GetItem(item, 2) : Py_None;
        ta_python_event_reading reading = {PyTuple_GetItem(item, 0), PyTuple_GetItem(item, 1),
                                           data == Py_None ? NULL : data};
        if (!ta_python_event_read(host, &reading, &batch[read])) break;
    }
    uint32_t taken = read == count ? ta_host_send_many(host->host, batch, (uint32_t)count) : 0;
    for (Py_ssize_t i = taken; i < read; i++)
        ta_python_event_release(host, &batch[i]);
    PyMem_Free(batch);
    Py_DECREF(list);
    if (read < count) return NULL;
    return ta_python_after_input(host, PyLong_FromUnsignedLong(taken));
}

/* terminate(session) */
static PyObject *ta_python_host_terminate(PyObject *self, PyObject *session) {
    ta_python_host *host = (ta_python_host *)self;
    uint64_t id = 0;
    if (!ta_python_host_usable(host) || !ta_python_session_of(session, &id)) return NULL;
    ta_status status = ta_host_terminate(host->host, id);
    if (status != TA_OK) {
        ta_python_raise_status(ta_python_host_state(host), "ta_host_terminate", status);
        return NULL;
    }
    return ta_python_after_input(host, Py_NewRef(Py_None));
}

static PyObject *ta_python_host_request_method(PyObject *self, PyObject *args, PyObject *kwargs) {
    return ta_python_host_request((ta_python_host *)self, args, kwargs);
}

static PyObject *ta_python_host_run(PyObject *self, PyObject *unused) {
    return ta_python_loop_run((ta_python_host *)self, unused);
}

static PyMethodDef ta_python_host_methods[] = {
    {"request", (PyCFunction)(void (*)(void))ta_python_host_request_method, METH_VARARGS | METH_KEYWORDS,
     "request(method, params=None) -> result\n\nA method of docs/requests.md (ta_host_request)."},
    {"add_image", (PyCFunction)(void (*)(void))ta_python_host_add_image, METH_VARARGS | METH_KEYWORDS,
     "add_image(charts, files=None) -> segment\n\nCompiles the charts (a dict of name to SCXML text, the first the "
     "default chart; files: what a src names) into an image and adds it as a segment."},
    {"boot", ta_python_host_boot, METH_VARARGS, "boot(segment)\n\nSpawns the actors a system image declares."},
    {"spawn", (PyCFunction)(void (*)(void))ta_python_host_spawn, METH_VARARGS | METH_KEYWORDS,
     "spawn(chart, params=None, name=None) -> session"},
    {"send", (PyCFunction)(void (*)(void))ta_python_host_send, METH_VARARGS | METH_KEYWORDS,
     "send(session, event, data=None)"},
    {"send_many", ta_python_host_send_many, METH_O,
     "send_many(events) -> int\n\nEach event a (session, event[, data]) tuple; returns how many were taken."},
    {"terminate", ta_python_host_terminate, METH_O, "terminate(session)"},
    {"run", ta_python_host_run, METH_NOARGS,
     "run()\n\nRuns the host on the real clock until it has no deadline left, sleeping until each."},
    {"close", ta_python_host_close, METH_NOARGS, "close()\n\nFrees the host (ta_host_free)."},
    {"__enter__", ta_python_host_enter, METH_NOARGS, NULL},
    {"__exit__", ta_python_host_exit, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyMemberDef ta_python_host_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(ta_python_host, weakreferences), Py_READONLY, NULL},
    {NULL, 0, 0, 0, NULL},
};

static PyType_Slot ta_python_host_slots[] = {
    {Py_tp_doc, "Host(config, *, clock=None, on_log=None, on_done=None, on_error=None, on_peer=None)\n\n"
                "A tinyactors host (ta_host_create): its config is docs/host-config.md's, as Python values."},
    {Py_tp_new, ta_python_host_new},
    {Py_tp_dealloc, ta_python_host_dealloc},
    {Py_tp_traverse, ta_python_host_traverse},
    {Py_tp_clear, ta_python_host_clear},
    {Py_tp_methods, ta_python_host_methods},
    {Py_tp_members, ta_python_host_members},
    {0, NULL},
};

static PyType_Spec ta_python_host_spec = {
    .name = "tinyactors.Host",
    .basicsize = sizeof(ta_python_host),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = ta_python_host_slots,
};

int ta_python_host_init_type(PyObject *module, ta_python_state *state) {
    state->host_type = PyType_FromModuleAndSpec(module, &ta_python_host_spec, NULL);
    if (!state->host_type) return -1;
    return PyModule_AddObjectRef(module, "Host", state->host_type);
}
