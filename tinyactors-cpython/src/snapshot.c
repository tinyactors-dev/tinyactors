/* tinyactors.read_snapshot(data): a snapshot read without a host
 * (tinyactors-core/docs/embedding.md, section 12).
 *
 * The pack is copied into a bytearray, 8-aligned, and each live segment's
 * region (ta_host_snapshot_region) is read with ta_system_read into a
 * second bytearray, beside the segment's ta_image: a system no one runs,
 * which no datamodel is bound to. The Snapshot holds both bytearrays,
 * never hands them out and never resizes them, so their bytes do not
 * move; the scratch arrays of a call are a bytearray of their own. The
 * methods mirror the system's and the image's readers, one segment at a
 * time. */
#include "extension.h"
#include "docs.h"

#include <string.h>

typedef struct ta_python_snapshot_segment {
    ta_image image;
    ta_system *system; /* NULL: retired */
    uint32_t index, pad;
} ta_python_snapshot_segment;

typedef struct ta_python_snapshot_view {
    uint32_t segment_count, pad;
    ta_python_snapshot_segment *segments;
} ta_python_snapshot_view;

typedef struct ta_python_snapshot {
    PyObject_HEAD
    PyObject *pack, *block; /* bytearrays: the pack's copy, and the segments' systems */
    PyObject *system_id, *datamodel, *state;
    ta_time time;
    ta_python_snapshot_view *view; /* in block */
} ta_python_snapshot;

static size_t ta_python_pad8(size_t length) {
    return (length + 7u) & ~(size_t)7u;
}

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
    memset(out->data, 0, length);
    return 0;
}

static PyObject *ta_python_snapshot_text(ta_string_view text) {
    return PyUnicode_FromStringAndSize(text.pointer ? text.pointer : "", (Py_ssize_t)text.length);
}

static PyObject *ta_python_text_or_none(ta_string_view text) {
    return text.length ? ta_python_snapshot_text(text) : Py_NewRef(Py_None);
}

/* `value` (stolen) set as `key` of `dict`; -1 when either failed. */
static int ta_python_put(PyObject *dict, const char *key, PyObject *value) {
    if (!value) return -1;
    int result = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return result;
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

/* Each live segment's image opened and region read, into the block. */
static int ta_python_snapshot_read(ta_python_module_state *state, ta_python_snapshot *snapshot, ta_bytes pack) {
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    if (ta_host_snapshot_unpack(pack, &contents) != TA_OK) {
        ta_python_snapshot_refused(state, "not a snapshot of this build (another format or layout, or damaged)");
        return -1;
    }
    uint32_t count = contents.image_count;
    size_t at = ta_python_pad8(sizeof(ta_python_snapshot_view)) + ta_python_pad8(sizeof(ta_python_snapshot_segment) * count);
    size_t total = at;
    for (uint32_t i = 0; i < count; i++) total += ta_python_pad8(ta_host_snapshot_region(pack, i).length);
    ta_python_aligned block;
    if (ta_python_aligned_new(total, &block) < 0) return -1;
    snapshot->block = block.array;
    snapshot->view = (ta_python_snapshot_view *)(void *)block.data;
    snapshot->view->segment_count = count;
    snapshot->view->segments =
        (ta_python_snapshot_segment *)(void *)(block.data + ta_python_pad8(sizeof(ta_python_snapshot_view)));
    for (uint32_t i = 0; i < count; i++) {
        ta_python_snapshot_segment *segment = &snapshot->view->segments[i];
        segment->index = i;
        ta_bytes image = ta_host_snapshot_image(pack, i), region = ta_host_snapshot_region(pack, i);
        if (!image.length || !region.length) continue; /* retired */
        ta_system_setup setup = {{block.data + at, region.length}, &segment->image, NULL, NULL, 0, 0};
        if (ta_image_open(image, &segment->image) != TA_OK || ta_system_read(&setup, region, &segment->system) != TA_OK) {
            segment->system = NULL;
            ta_python_snapshot_refused(state, "a segment of the snapshot is not this build's, or is damaged");
            return -1;
        }
        at += ta_python_pad8(region.length);
    }
    snapshot->system_id = ta_python_snapshot_text(contents.system_id);
    snapshot->datamodel = ta_python_snapshot_text(contents.datamodel);
    snapshot->state = PyBytes_FromStringAndSize(contents.state_length ? contents.state : "", (Py_ssize_t)contents.state_length);
    snapshot->time = contents.time;
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
    if (!snapshot) {
        Py_DECREF(pack.array);
        return NULL;
    }
    snapshot->pack = pack.array;
    if (ta_python_snapshot_read(state, snapshot, copied) < 0) {
        Py_DECREF(snapshot);
        return NULL;
    }
    return (PyObject *)snapshot;
}

/* ---- the methods -------------------------------------------------------------------------- */

/* A call's segment, and its handle when the method takes one. */
typedef struct ta_python_snapshot_call {
    int handles; /* 1: (segment, handle) */
    ta_python_snapshot_segment *segment;
    unsigned int handle;
} ta_python_snapshot_call;

static int ta_python_snapshot_enter(PyObject *self, PyObject *arguments, ta_python_snapshot_call *call) {
    ta_python_snapshot *snapshot = (ta_python_snapshot *)self;
    unsigned int index = 0;
    if (call->handles ? !PyArg_ParseTuple(arguments, "II", &index, &call->handle) : !PyArg_ParseTuple(arguments, "I", &index))
        return -1;
    if (!snapshot->view || index >= snapshot->view->segment_count || !snapshot->view->segments[index].system) {
        PyErr_SetString(PyExc_IndexError, "no live segment of that index in the snapshot");
        return -1;
    }
    call->segment = &snapshot->view->segments[index];
    ta_actor_info actor;
    actor.size = sizeof actor;
    if (call->handles && ta_actor_info_get(call->segment->system, call->handle, &actor) != TA_OK) {
        PyErr_SetString(PyExc_IndexError, "no actor of that handle in the segment");
        return -1;
    }
    return 0;
}

static PyObject *ta_python_snapshot_actors(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {0, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    PyObject *result = PyList_New(0);
    uint32_t cursor = 0, handles[64];
    for (uint32_t count; result && (count = ta_system_actors(call.segment->system, &cursor, (ta_uint32_array){handles, 64})) != 0;)
        for (uint32_t i = 0; i < count && result; i++) {
            PyObject *handle = PyLong_FromUnsignedLong(handles[i]);
            if (!handle || PyList_Append(result, handle) < 0) Py_CLEAR(result);
            Py_XDECREF(handle);
        }
    return result;
}

static const char *const ta_python_actor_states[] = {"free", "idle", "runnable", "waiting_workspace", "waiting_ack", "done",
                                                     "hibernated"};

static PyObject *ta_python_snapshot_actor(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {1, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    ta_system *system = call.segment->system;
    ta_actor_info actor, parent;
    actor.size = parent.size = sizeof actor;
    ta_actor_info_get(system, call.handle, &actor);
    const char *status = actor.state < sizeof ta_python_actor_states / sizeof ta_python_actor_states[0]
                             ? ta_python_actor_states[actor.state]
                             : "unknown";
    PyObject *result = PyDict_New();
    if (!result || ta_python_put(result, "session", PyLong_FromUnsignedLongLong(actor.serial)) < 0 ||
        ta_python_put(result, "name", ta_python_text_or_none(ta_system_actor_name(system, call.handle))) < 0 ||
        ta_python_put(result, "chart", PyLong_FromUnsignedLong(actor.chart)) < 0 ||
        ta_python_put(result, "chart_name", ta_python_snapshot_text(ta_image_chart_name(&call.segment->image, actor.chart))) < 0 ||
        ta_python_put(result, "status", PyUnicode_FromString(status)) < 0 ||
        ta_python_put(result, "parent", actor.parent && ta_actor_info_get(system, actor.parent, &parent) == TA_OK
                                            ? PyLong_FromUnsignedLongLong(parent.serial)
                                            : Py_NewRef(Py_None)) < 0 ||
        ta_python_put(result, "mailbox", PyLong_FromUnsignedLong(actor.mailbox_length)) < 0 ||
        ta_python_put(result, "invocations", PyLong_FromUnsignedLong(actor.invocations)) < 0 ||
        ta_python_put(result, "services", PyLong_FromUnsignedLong(actor.services)) < 0)
        Py_CLEAR(result);
    return result;
}

/* A bytearray for a call's scratch array of `size` bytes; `*data` its start. */
static PyObject *ta_python_scratch(size_t size, void **data) {
    ta_python_aligned aligned;
    if (ta_python_aligned_new(size ? size : 8u, &aligned) < 0) return NULL;
    *data = aligned.data;
    return aligned.array;
}

static PyObject *ta_python_u32_list(ta_uint32_array values) {
    PyObject *result = PyList_New((Py_ssize_t)values.capacity);
    for (uint32_t i = 0; result && i < values.capacity; i++) {
        PyObject *value = PyLong_FromUnsignedLong(values.pointer[i]);
        if (!value) Py_CLEAR(result);
        else PyList_SetItem(result, (Py_ssize_t)i, value);
    }
    return result;
}

static PyObject *ta_python_snapshot_configuration(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {1, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    uint32_t count = ta_actor_config(call.segment->system, call.handle, (ta_uint32_array){NULL, 0});
    void *data = NULL;
    PyObject *scratch = ta_python_scratch(sizeof(uint32_t) * count, &data);
    if (!scratch) return NULL;
    ta_actor_config(call.segment->system, call.handle, (ta_uint32_array){(uint32_t *)data, count});
    PyObject *result = ta_python_u32_list((ta_uint32_array){(uint32_t *)data, count});
    Py_DECREF(scratch);
    return result;
}

static PyObject *ta_python_snapshot_mailbox(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {1, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    uint32_t count = ta_system_actor_mailbox(call.segment->system, call.handle, (ta_actor_queued_array){NULL, 0});
    void *data = NULL;
    PyObject *scratch = ta_python_scratch(sizeof(ta_actor_queued) * count, &data);
    if (!scratch) return NULL;
    ta_actor_queued *queued = (ta_actor_queued *)data;
    ta_system_actor_mailbox(call.segment->system, call.handle, (ta_actor_queued_array){queued, count});
    PyObject *result = PyList_New((Py_ssize_t)count);
    for (uint32_t i = 0; result && i < count; i++) {
        PyObject *item = PyDict_New();
        if (!item || ta_python_put(item, "event", ta_python_snapshot_text(queued[i].event)) < 0 ||
            ta_python_put(item, "sendid", ta_python_text_or_none(queued[i].sendid)) < 0) {
            Py_XDECREF(item);
            Py_CLEAR(result);
        } else {
            PyList_SetItem(result, (Py_ssize_t)i, item);
        }
    }
    Py_DECREF(scratch);
    return result;
}

static PyObject *ta_python_snapshot_timers(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {1, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    uint32_t count = ta_system_actor_timers(call.segment->system, call.handle, (ta_actor_timer_array){NULL, 0});
    void *data = NULL;
    PyObject *scratch = ta_python_scratch(sizeof(ta_actor_timer) * count, &data);
    if (!scratch) return NULL;
    ta_actor_timer *timers = (ta_actor_timer *)data;
    ta_system_actor_timers(call.segment->system, call.handle, (ta_actor_timer_array){timers, count});
    PyObject *result = PyList_New((Py_ssize_t)count);
    for (uint32_t i = 0; result && i < count; i++) {
        PyObject *item = PyDict_New();
        if (!item || ta_python_put(item, "due", PyLong_FromUnsignedLongLong(timers[i].due)) < 0 ||
            ta_python_put(item, "event", ta_python_snapshot_text(timers[i].event)) < 0 ||
            ta_python_put(item, "sendid", ta_python_text_or_none(timers[i].sendid)) < 0) {
            Py_XDECREF(item);
            Py_CLEAR(result);
        } else {
            PyList_SetItem(result, (Py_ssize_t)i, item);
        }
    }
    Py_DECREF(scratch);
    return result;
}

static PyObject *ta_python_snapshot_stats(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {0, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(call.segment->system, &stats);
    PyObject *result = PyDict_New();
    if (!result || ta_python_put(result, "now", PyLong_FromUnsignedLongLong(stats.now)) < 0 ||
        ta_python_put(result, "ticks_per_second", PyLong_FromUnsignedLongLong(stats.ticks_per_second)) < 0 ||
        ta_python_put(result, "next_deadline", PyLong_FromUnsignedLongLong(stats.next_deadline)) < 0 ||
        ta_python_put(result, "actors", PyLong_FromUnsignedLong(stats.actors)) < 0 ||
        ta_python_put(result, "hibernated", PyLong_FromUnsignedLong(stats.hibernated)) < 0 ||
        ta_python_put(result, "runnable", PyLong_FromUnsignedLong(stats.runnable)) < 0 ||
        ta_python_put(result, "events", PyLong_FromUnsignedLong(stats.events)) < 0 ||
        ta_python_put(result, "timers", PyLong_FromUnsignedLong(stats.timers)) < 0 ||
        ta_python_put(result, "requests", PyLong_FromUnsignedLong(stats.requests)) < 0 ||
        ta_python_put(result, "region_bytes", PyLong_FromUnsignedLongLong(stats.region_bytes)) < 0 ||
        ta_python_put(result, "wall_ns", PyLong_FromUnsignedLongLong(stats.wall_ns)) < 0)
        Py_CLEAR(result);
    return result;
}

static const char *const ta_python_state_kinds[] = {"atomic", "compound", "parallel", "final", "shallow_history",
                                                    "deep_history"};
static const char *const ta_python_transition_types[] = {"external", "internal", "initial"};

/* A table of names, by index. */
typedef struct ta_python_names {
    const char *const *names;
    uint32_t count;
} ta_python_names;

static const ta_python_names ta_python_state_kind_names = {ta_python_state_kinds, 6};
static const ta_python_names ta_python_transition_type_names = {ta_python_transition_types, 3};

static PyObject *ta_python_name_of(const ta_python_names *table, uint32_t index) {
    return PyUnicode_FromString(index < table->count ? table->names[index] : "unknown");
}

static PyObject *ta_python_chart_states(const ta_image *image, uint32_t chart) {
    uint32_t count = ta_image_state_count(image, chart);
    PyObject *result = PyList_New((Py_ssize_t)count);
    for (uint32_t i = 0; result && i < count; i++) {
        ta_state_info state;
        state.size = sizeof state;
        PyObject *item = PyDict_New();
        if (!item || ta_image_state_info(image, (ta_chart_item){chart, i}, &state) != TA_OK ||
            ta_python_put(item, "id", ta_python_snapshot_text(state.id)) < 0 ||
            ta_python_put(item, "kind", ta_python_name_of(&ta_python_state_kind_names, state.kind)) < 0 ||
            ta_python_put(item, "parent", state.parent == TA_NIL ? Py_NewRef(Py_None) : PyLong_FromUnsignedLong(state.parent)) < 0) {
            Py_XDECREF(item);
            Py_CLEAR(result);
        } else {
            PyList_SetItem(result, (Py_ssize_t)i, item);
        }
    }
    return result;
}

static PyObject *ta_python_chart_transition(const ta_image *image, ta_chart_item item) {
    ta_transition_info transition;
    transition.size = sizeof transition;
    if (ta_image_transition(image, item, &transition) != TA_OK) return PyErr_NoMemory();
    PyObject *events = PyList_New((Py_ssize_t)transition.event_count);
    for (uint32_t i = 0; events && i < transition.event_count; i++) {
        PyObject *event = ta_python_snapshot_text(ta_image_transition_event(image, item, i));
        if (!event) Py_CLEAR(events);
        else PyList_SetItem(events, (Py_ssize_t)i, event);
    }
    void *data = NULL;
    PyObject *scratch = ta_python_scratch(sizeof(uint32_t) * transition.target_count, &data);
    PyObject *targets = NULL;
    if (scratch) {
        ta_image_transition_targets(image, item, (ta_uint32_array){(uint32_t *)data, transition.target_count});
        targets = ta_python_u32_list((ta_uint32_array){(uint32_t *)data, transition.target_count});
        Py_DECREF(scratch);
    }
    ta_expression_info condition;
    PyObject *cond = transition.condition != TA_NIL &&
                             ta_image_expression(image, (ta_chart_item){item.chart, transition.condition}, &condition) == TA_OK
                         ? ta_python_snapshot_text(condition.source)
                         : Py_NewRef(Py_None);
    PyObject *result = PyDict_New();
    if (!result || !events || !targets || ta_python_put(result, "source", PyLong_FromUnsignedLong(transition.source)) < 0 ||
        ta_python_put(result, "type", ta_python_name_of(&ta_python_transition_type_names, transition.type)) < 0 ||
        ta_python_put(result, "events", Py_NewRef(events)) < 0 || ta_python_put(result, "cond", Py_XNewRef(cond)) < 0 ||
        ta_python_put(result, "targets", Py_NewRef(targets)) < 0)
        Py_CLEAR(result);
    Py_XDECREF(events);
    Py_XDECREF(targets);
    Py_XDECREF(cond);
    return result;
}

static PyObject *ta_python_chart(const ta_image *image, uint32_t chart) {
    uint32_t transition_count = ta_image_transition_count(image, chart), data_count = ta_image_data_count(image, chart);
    PyObject *transitions = PyList_New((Py_ssize_t)transition_count), *data = PyList_New((Py_ssize_t)data_count);
    for (uint32_t i = 0; transitions && i < transition_count; i++) {
        PyObject *transition = ta_python_chart_transition(image, (ta_chart_item){chart, i});
        if (!transition) Py_CLEAR(transitions);
        else PyList_SetItem(transitions, (Py_ssize_t)i, transition);
    }
    for (uint32_t i = 0; data && i < data_count; i++) {
        ta_data_info datum;
        PyObject *id = ta_image_data(image, (ta_chart_item){chart, i}, &datum) == TA_OK ? ta_python_snapshot_text(datum.id) : NULL;
        if (!id) Py_CLEAR(data);
        else PyList_SetItem(data, (Py_ssize_t)i, id);
    }
    PyObject *result = PyDict_New();
    if (!result || !transitions || !data || ta_python_put(result, "name", ta_python_snapshot_text(ta_image_chart_name(image, chart))) < 0 ||
        ta_python_put(result, "datamodel", ta_python_snapshot_text(ta_image_chart_datamodel(image, chart))) < 0 ||
        ta_python_put(result, "states", ta_python_chart_states(image, chart)) < 0 ||
        ta_python_put(result, "transitions", Py_NewRef(transitions)) < 0 || ta_python_put(result, "data", Py_NewRef(data)) < 0)
        Py_CLEAR(result);
    Py_XDECREF(transitions);
    Py_XDECREF(data);
    return result;
}

static PyObject *ta_python_snapshot_charts(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {0, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&call.segment->image, &info);
    PyObject *result = PyList_New((Py_ssize_t)info.chart_count);
    for (uint32_t c = 0; result && c < info.chart_count; c++) {
        PyObject *chart = ta_python_chart(&call.segment->image, c);
        if (!chart) Py_CLEAR(result);
        else PyList_SetItem(result, (Py_ssize_t)c, chart);
    }
    return result;
}

static PyObject *ta_python_snapshot_trace_kept(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {0, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    ta_trace_kept kept;
    kept.size = sizeof kept;
    if (ta_system_trace_kept(call.segment->system, &kept) != TA_OK) Py_RETURN_NONE;
    PyObject *result = PyDict_New();
    if (!result || ta_python_put(result, "records", PyLong_FromUnsignedLong(kept.records)) < 0 ||
        ta_python_put(result, "acknowledged_records", PyLong_FromUnsignedLong(kept.acknowledged_records)) < 0 ||
        ta_python_put(result, "bytes", PyLong_FromSize_t((size_t)kept.first.length + kept.second.length)) < 0 ||
        ta_python_put(result, "acknowledged_bytes", PyLong_FromUnsignedLong(kept.acknowledged)) < 0 ||
        ta_python_put(result, "lost", PyLong_FromUnsignedLongLong(kept.lost)) < 0)
        Py_CLEAR(result);
    return result;
}

static PyObject *ta_python_snapshot_traces(PyObject *self, PyObject *arguments) {
    ta_python_snapshot_call call = {0, NULL, 0};
    if (ta_python_snapshot_enter(self, arguments, &call) < 0) return NULL;
    ta_host_trace_history_input input = {sizeof input, call.segment->index, call.segment->system, &call.segment->image, NULL};
    return ta_python_trace_history(&input);
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
    return PyLong_FromUnsignedLong(snapshot->view ? snapshot->view->segment_count : 0);
}

static void ta_python_snapshot_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    ta_python_snapshot *snapshot = (ta_python_snapshot *)self;
    Py_XDECREF(snapshot->pack);
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
