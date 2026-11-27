/* tinyactors.read_snapshot(data): a snapshot read without a host, over
 * core's snapshot reader (tinyactors-core/docs/snapshot-requests.md).
 *
 * The reader is one bytearray: ta_host_snapshot_reader_open copies the
 * pack and reads each live segment's region into it. The Snapshot holds
 * it, never hands it out and never resizes it, so its bytes do not move.
 * The pack is copied into an 8-aligned bytearray for the open only.
 * request(method, params) is the reader's request (host.c's
 * ta_python_snapshot_answer, its working memory through the binding's
 * allocator for the call); the other methods mirror core's readers over
 * it, in Python's shapes: keys and the names of statuses and kinds in
 * snake_case, a segment or handle the snapshot does not have an
 * IndexError. */
#include "extension.h"
#include "docs.h"

#include <string.h>

typedef struct ta_python_snapshot {
    PyObject_HEAD
    PyObject *block; /* a bytearray: the reader */
    PyObject *system_id, *datamodel, *state;
    ta_time time;
    uint32_t segments, pad;
    const ta_host_snapshot_reader *reader; /* in block */
} ta_python_snapshot;

/* A bytearray of at least `length` bytes, and the 8-aligned start in it. */
typedef struct ta_python_aligned {
    PyObject *array; /* new reference, or NULL with an exception */
    unsigned char *data;
} ta_python_aligned;

static int ta_python_aligned_new(size_t length, ta_python_aligned *out) {
    out->array = PyByteArray_FromStringAndSize(NULL, (Py_ssize_t)(length + 8u));
    if (!out->array) return -1;
    unsigned char *data = (unsigned char *)PyByteArray_AsString(out->array);
    out->data = data + ((8u - ((uintptr_t)data & 7u)) & 7u);
    return 0;
}

static PyObject *ta_python_snapshot_text(ta_string_view text) {
    return PyUnicode_FromStringAndSize(text.pointer ? text.pointer : "", (Py_ssize_t)text.length);
}

static void ta_python_snapshot_refused(ta_python_module_state *state, const char *message) {
    PyObject *arguments = Py_BuildValue("(s)", message);
    PyObject *keywords = Py_BuildValue("{s:s}", "status", "TA_ERROR_INVALID");
    PyObject *error = arguments && keywords ? PyObject_Call(state->error, arguments, keywords) : NULL;
    if (error) PyErr_SetObject((PyObject *)Py_TYPE(error), error);
    Py_XDECREF(error);
    Py_XDECREF(arguments);
    Py_XDECREF(keywords);
}

/* ---- read_snapshot ---------------------------------------------------------------------- */

/* The reader made in a bytearray of its size, the header from the pack. */
static int ta_python_snapshot_open(ta_python_module_state *state, ta_python_snapshot *snapshot, ta_bytes pack) {
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    size_t size = ta_host_snapshot_reader_size(pack);
    if (!size || ta_host_snapshot_unpack(pack, &contents) != TA_OK) {
        ta_python_snapshot_refused(state, "not a snapshot of this build (another format or layout, or damaged)");
        return -1;
    }
    ta_python_aligned block;
    if (ta_python_aligned_new(size, &block) < 0) return -1;
    snapshot->block = block.array;
    ta_host_snapshot_reader_input input = {sizeof input, 0, pack, {block.data, size, 0}};
    if (ta_host_snapshot_reader_open(&input, &snapshot->reader) != TA_OK) {
        snapshot->reader = NULL;
        ta_python_snapshot_refused(state, "a segment of the snapshot is not this build's, or is damaged");
        return -1;
    }
    snapshot->system_id = ta_python_snapshot_text(contents.system_id);
    snapshot->datamodel = ta_python_snapshot_text(contents.datamodel);
    snapshot->state = PyBytes_FromStringAndSize(contents.state_length ? contents.state : "", (Py_ssize_t)contents.state_length);
    snapshot->time = contents.time;
    snapshot->segments = contents.image_count;
    return snapshot->system_id && snapshot->datamodel && snapshot->state ? 0 : -1;
}

PyObject *ta_python_read_snapshot(PyObject *module, PyObject *data) {
    ta_python_module_state *state = ta_python_codec_state(module);
    if (state == NULL) return NULL;
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) return NULL;
    ta_python_aligned pack;
    if (ta_python_aligned_new((size_t)view.len, &pack) < 0) {
        PyBuffer_Release(&view);
        return NULL;
    }
    if (view.len) memcpy(pack.data, view.buf, (size_t)view.len);
    ta_bytes copied = {pack.data, (size_t)view.len};
    PyBuffer_Release(&view);
    PyTypeObject *type = (PyTypeObject *)state->snapshot_type;
    ta_python_snapshot *snapshot = (ta_python_snapshot *)PyType_GenericAlloc(type, 0);
    int opened = snapshot ? ta_python_snapshot_open(state, snapshot, copied) : -1;
    Py_DECREF(pack.array); /* the reader keeps its own copy */
    if (opened < 0) {
        Py_XDECREF((PyObject *)snapshot);
        return NULL;
    }
    return (PyObject *)snapshot;
}

/* ---- request ------------------------------------------------------------------------------ */

/* The result of `method` with `params` (a dict, or NULL for none): a new
 * reference, or NULL with the reply's error raised as a TinyactorsError. */
typedef struct ta_python_snapshot_call {
    const char *method;
    PyObject *params;
} ta_python_snapshot_call;

static PyObject *ta_python_snapshot_ask(ta_python_snapshot *snapshot, const ta_python_snapshot_call *call) {
    ta_python_module_state *state = ta_python_state_of_type(Py_TYPE((PyObject *)snapshot));
    if (state == NULL || snapshot->reader == NULL) return NULL;
    ta_cbor_writer writer;
    unsigned char room[256];
    ta_python_cbor_init(&writer, &(ta_python_room){{room, sizeof room, 0}, NULL});
    ta_cbor_write_map(&writer, call->params ? 3 : 2);
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string(call->method));
    PyObject *result = NULL;
    if (call->params) {
        ta_cbor_write_text(&writer, ta_string("params"));
        if (ta_python_value_write(state, &writer, call->params) < 0) {
            ta_cbor_writer_free(&writer);
            return NULL;
        }
    }
    ta_bytes message;
    if (ta_cbor_writer_bytes(&writer, &message) != TA_OK) {
        ta_cbor_writer_free(&writer);
        PyErr_NoMemory();
        return NULL;
    }
    PyObject *reply = ta_python_snapshot_answer(&(ta_python_snapshot_asking){snapshot->reader, message});
    ta_cbor_writer_free(&writer);
    if (!reply) return NULL;
    PyObject *decoded = ta_python_value_read(state, (ta_bytes){PyByteArray_AsString(reply), (size_t)PyByteArray_Size(reply)});
    Py_DECREF(reply);
    if (!decoded) return NULL;
    PyObject *error = NULL;
    if (PyDict_GetItemStringRef(decoded, "result", &result) == 0 && PyDict_GetItemStringRef(decoded, "error", &error) > 0) {
        PyObject *raised = ta_python_error_from_reply(state, error);
        if (raised) PyErr_SetObject((PyObject *)Py_TYPE(raised), raised);
        Py_XDECREF(raised);
        Py_DECREF(error);
    }
    Py_DECREF(decoded);
    return result;
}

static PyObject *ta_python_snapshot_request(PyObject *self, PyObject *arguments) {
    const char *method = NULL;
    PyObject *params = Py_None;
    if (!PyArg_ParseTuple(arguments, "s|O:request", &method, &params)) return NULL;
    return ta_python_snapshot_ask((ta_python_snapshot *)self, &(ta_python_snapshot_call){method, params == Py_None ? NULL : params});
}

/* ---- the mirrored readers, in Python's shapes ------------------------------------------------ */

/* camelCase as snake_case: "chartName" -> "chart_name"; core's
 * "wallNanoseconds" is wall_ns, as the system's own field. A new reference. */
static PyObject *ta_python_snake(PyObject *text) {
    Py_ssize_t length = 0;
    const char *from = PyUnicode_AsUTF8AndSize(text, &length);
    if (!from) return NULL;
    if (length == 15 && memcmp(from, "wallNanoseconds", 15) == 0) return PyUnicode_FromString("wall_ns");
    char into[64];
    Py_ssize_t at = 0;
    for (Py_ssize_t i = 0; i < length; i++) {
        if (at + 2 >= (Py_ssize_t)sizeof into) return Py_NewRef(text); /* longer than any key of core's */
        if (from[i] >= 'A' && from[i] <= 'Z') {
            into[at++] = '_';
            into[at++] = (char)(from[i] - 'A' + 'a');
        } else {
            into[at++] = from[i];
        }
    }
    return PyUnicode_FromStringAndSize(into, at);
}

/* A value of a reply with its keys snake_case, and the values of
 * `status` and `kind` (names, not data); a new reference. */
static PyObject *ta_python_shaped(PyObject *value, int depth) {
    if (depth > 8) return Py_NewRef(value);
    if (PyList_CheckExact(value)) {
        Py_ssize_t count = PyList_Size(value);
        PyObject *list = PyList_New(count);
        for (Py_ssize_t i = 0; list && i < count; i++) {
            PyObject *item = ta_python_shaped(PyList_GetItem(value, i), depth + 1);
            if (!item) {
                Py_CLEAR(list);
                break;
            }
            PyList_SetItem(list, i, item);
        }
        return list;
    }
    if (!PyDict_CheckExact(value)) return Py_NewRef(value);
    PyObject *dict = PyDict_New(), *key, *item;
    Py_ssize_t at = 0;
    while (dict && PyDict_Next(value, &at, &key, &item)) {
        PyObject *name = ta_python_snake(key);
        int named = PyUnicode_Check(item) &&
                    (PyUnicode_CompareWithASCIIString(key, "status") == 0 || PyUnicode_CompareWithASCIIString(key, "kind") == 0);
        PyObject *shaped = named ? ta_python_snake(item) : ta_python_shaped(item, depth + 1);
        if (!name || !shaped || PyDict_SetItem(dict, name, shaped) < 0) Py_CLEAR(dict);
        Py_XDECREF(name);
        Py_XDECREF(shaped);
    }
    return dict;
}

/* One mirrored reader: its method, and whether it takes a handle. */
typedef struct ta_python_mirror {
    const char *method;
    int handles;
} ta_python_mirror;

static PyObject *ta_python_snapshot_mirror(PyObject *self, PyObject *arguments, const ta_python_mirror *mirror) {
    unsigned int segment = 0, handle = 0;
    if (mirror->handles ? !PyArg_ParseTuple(arguments, "II", &segment, &handle) : !PyArg_ParseTuple(arguments, "I", &segment))
        return NULL;
    PyObject *params = mirror->handles ? Py_BuildValue("{s:I,s:I}", "segment", segment, "handle", handle)
                                       : Py_BuildValue("{s:I}", "segment", segment);
    if (!params) return NULL;
    PyObject *result = ta_python_snapshot_ask((ta_python_snapshot *)self, &(ta_python_snapshot_call){mirror->method, params});
    Py_DECREF(params);
    if (!result) {
        /* a segment or handle the snapshot does not have: IndexError, as a sequence's */
        PyObject *raised = PyErr_GetRaisedException();
        PyObject *reason = raised ? PyObject_GetAttrString(raised, "reason") : NULL;
        if (reason && PyUnicode_Check(reason) &&
            (PyUnicode_CompareWithASCIIString(reason, "range") == 0 || PyUnicode_CompareWithASCIIString(reason, "stale") == 0)) {
            PyObject *message = PyObject_Str(raised);
            PyErr_SetObject(PyExc_IndexError, message ? message : Py_None);
            Py_XDECREF(message);
            Py_DECREF(raised);
        } else {
            PyErr_Clear();
            PyErr_SetRaisedException(raised);
        }
        Py_XDECREF(reason);
        return NULL;
    }
    PyObject *shaped = ta_python_shaped(result, 0);
    Py_DECREF(result);
    return shaped;
}

static const ta_python_mirror ta_python_mirror_actors = {"snapshot.handles", 0};
static const ta_python_mirror ta_python_mirror_actor = {"snapshot.actor", 1};
static const ta_python_mirror ta_python_mirror_configuration = {"snapshot.configuration", 1};
static const ta_python_mirror ta_python_mirror_mailbox = {"snapshot.mailbox", 1};
static const ta_python_mirror ta_python_mirror_timers = {"snapshot.timers", 1};
static const ta_python_mirror ta_python_mirror_stats = {"snapshot.stats", 0};
static const ta_python_mirror ta_python_mirror_charts = {"snapshot.charts", 0};
static const ta_python_mirror ta_python_mirror_trace_kept = {"snapshot.traceKept", 0};
static const ta_python_mirror ta_python_mirror_traces = {"snapshot.traces", 0};

static PyObject *ta_python_snapshot_actors(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_actors);
}
static PyObject *ta_python_snapshot_actor(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_actor);
}
static PyObject *ta_python_snapshot_configuration(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_configuration);
}
static PyObject *ta_python_snapshot_mailbox(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_mailbox);
}
static PyObject *ta_python_snapshot_timers(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_timers);
}
static PyObject *ta_python_snapshot_stats(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_stats);
}
static PyObject *ta_python_snapshot_charts(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_charts);
}
static PyObject *ta_python_snapshot_trace_kept(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_trace_kept);
}
static PyObject *ta_python_snapshot_traces(PyObject *self, PyObject *arguments) {
    return ta_python_snapshot_mirror(self, arguments, &ta_python_mirror_traces);
}

/* ---- the type ------------------------------------------------------------------------------- */

static PyObject *ta_python_snapshot_system_id(PyObject *self, void *closure) {
    (void)closure;
    return Py_NewRef(((ta_python_snapshot *)self)->system_id);
}

static PyObject *ta_python_snapshot_datamodel(PyObject *self, void *closure) {
    (void)closure;
    return Py_NewRef(((ta_python_snapshot *)self)->datamodel);
}

static PyObject *ta_python_snapshot_state(PyObject *self, void *closure) {
    (void)closure;
    return Py_NewRef(((ta_python_snapshot *)self)->state);
}

static PyObject *ta_python_snapshot_time(PyObject *self, void *closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(((ta_python_snapshot *)self)->time);
}

static PyObject *ta_python_snapshot_segments(PyObject *self, void *closure) {
    (void)closure;
    ta_python_snapshot *snapshot = (ta_python_snapshot *)self;
    return PyLong_FromUnsignedLong(snapshot->segments);
}

static void ta_python_snapshot_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    ta_python_snapshot *snapshot = (ta_python_snapshot *)self;
    Py_XDECREF(snapshot->block);
    Py_XDECREF(snapshot->system_id);
    Py_XDECREF(snapshot->datamodel);
    Py_XDECREF(snapshot->state);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyGetSetDef ta_python_snapshot_properties[] = {
    {"system_id", ta_python_snapshot_system_id, NULL, ta_python_doc_snapshot_system_id, NULL},
    {"datamodel", ta_python_snapshot_datamodel, NULL, ta_python_doc_snapshot_datamodel, NULL},
    {"time", ta_python_snapshot_time, NULL, ta_python_doc_snapshot_time, NULL},
    {"segments", ta_python_snapshot_segments, NULL, ta_python_doc_snapshot_segments, NULL},
    {"state", ta_python_snapshot_state, NULL, ta_python_doc_snapshot_state, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef ta_python_snapshot_methods[] = {
    {"actors", ta_python_snapshot_actors, METH_VARARGS, ta_python_doc_snapshot_actors},
    {"actor", ta_python_snapshot_actor, METH_VARARGS, ta_python_doc_snapshot_actor},
    {"configuration", ta_python_snapshot_configuration, METH_VARARGS, ta_python_doc_snapshot_configuration},
    {"mailbox", ta_python_snapshot_mailbox, METH_VARARGS, ta_python_doc_snapshot_mailbox},
    {"timers", ta_python_snapshot_timers, METH_VARARGS, ta_python_doc_snapshot_timers},
    {"stats", ta_python_snapshot_stats, METH_VARARGS, ta_python_doc_snapshot_stats},
    {"charts", ta_python_snapshot_charts, METH_VARARGS, ta_python_doc_snapshot_charts},
    {"trace_kept", ta_python_snapshot_trace_kept, METH_VARARGS, ta_python_doc_snapshot_trace_kept},
    {"traces", ta_python_snapshot_traces, METH_VARARGS, ta_python_doc_snapshot_traces},
    {"request", ta_python_snapshot_request, METH_VARARGS, ta_python_doc_snapshot_request},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot ta_python_snapshot_slots[] = {
    {Py_tp_doc, (void *)ta_python_doc_snapshot},
    {Py_tp_dealloc, ta_python_snapshot_dealloc},
    {Py_tp_getset, ta_python_snapshot_properties},
    {Py_tp_methods, ta_python_snapshot_methods},
    {0, NULL}
};

static PyType_Spec ta_python_snapshot_spec = {
    "tinyactors.Snapshot", sizeof(ta_python_snapshot), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION, ta_python_snapshot_slots
};

int ta_python_snapshot_init_type(PyObject *module, ta_python_module_state *state) {
    state->snapshot_type = PyType_FromModuleAndSpec(module, &ta_python_snapshot_spec, NULL);
    if (!state->snapshot_type) return -1;
    return PyModule_AddObjectRef(module, "Snapshot", state->snapshot_type);
}
