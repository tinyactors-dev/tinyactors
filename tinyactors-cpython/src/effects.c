/* The effects (AGENTS.md, "Lifetimes and thin views"): dispatch of one
 * effect of the open batch, and the objects Python sees of them. A thin
 * view holds the Host, the ack count of its batch and the effect's index,
 * and converts a field on first read; after the ack a read raises
 * TA_ERROR_STALE. A record (an async def processor's send) holds every
 * field, made before the ack. */
#include "extension.h"
#include <string.h>

/* ---- the fields of a view -------------------------------------------------------- */

enum {
    TA_PYTHON_SEND_SESSION, TA_PYTHON_SEND_SENDID, TA_PYTHON_SEND_EVENT, TA_PYTHON_SEND_TARGET,
    TA_PYTHON_SEND_DATA, TA_PYTHON_SEND_TRACEPARENT, TA_PYTHON_SEND_TYPE, TA_PYTHON_SEND_FIELDS
};
enum { TA_PYTHON_ENTRY_SESSION, TA_PYTHON_ENTRY_LABEL, TA_PYTHON_ENTRY_TEXT, TA_PYTHON_ENTRY_FIELDS };
enum { TA_PYTHON_ENVELOPE_PEER, TA_PYTHON_ENVELOPE_ERROR, TA_PYTHON_ENVELOPE_CBOR, TA_PYTHON_ENVELOPE_FIELDS };

/* What every view holds: where its effect is. */
typedef struct ta_python_view {
    PyObject_HEAD
    ta_python_host *host; /* a reference */
    uint64_t acks;        /* the batch: host->acks when it was made */
    uint32_t index;       /* in the batch */
    uint32_t record;      /* 1: every field is held, no batch to go stale */
    uint32_t answered;    /* a send: answered or failed */
    uint32_t processor;   /* a send: its I/O processor's index */
    PyObject *fields[TA_PYTHON_SEND_FIELDS];
} ta_python_view;

/* The view's batch is still open: 0, or -1 with TA_ERROR_STALE. */
static int ta_python_view_live(ta_python_view *view) {
    if (view->record) return 0;
    if (!view->host->host || view->host->acks != view->acks) {
        ta_python_raise_status(view->host->state, TA_ERROR_STALE);
        return -1;
    }
    return 0;
}

/* A text field as a str; empty text as None when `empty_none`. */
typedef struct ta_python_text_field {
    uint32_t field;
    int empty_none;
} ta_python_text_field;

static PyObject *ta_python_effect_text(ta_python_view *view, ta_python_text_field field) {
    ta_string_view text = ta_host_effect_text(view->host->host, view->index, field.field);
    if (!text.length && field.empty_none) return Py_NewRef(Py_None);
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", text.length, "strict");
}

static PyObject *ta_python_effect_payload(ta_python_view *view) {
    ta_host *core = view->host->host;
    ta_python_payload_of payload = {ta_host_effect_number(core, view->index, TA_EFFECT_FIELD_PAYLOAD),
                                    (uint32_t)ta_host_effect_number(core, view->index, TA_EFFECT_FIELD_DATAMODEL)};
    return ta_python_host_payload_value(view->host, &payload);
}

/* A send's field `which`, converted on first read and cached. */
static PyObject *ta_python_send_read(ta_python_view *view, int which) {
    if (view->fields[which]) return Py_NewRef(view->fields[which]);
    if (ta_python_view_live(view) < 0) return NULL;
    PyObject *value = NULL;
    switch (which) {
    case TA_PYTHON_SEND_SESSION:
        value = PyLong_FromUnsignedLongLong(ta_host_effect_number(view->host->host, view->index, TA_EFFECT_FIELD_SESSION));
        break;
    case TA_PYTHON_SEND_SENDID: value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_SENDID, 1}); break;
    case TA_PYTHON_SEND_EVENT: value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_EVENT, 0}); break;
    case TA_PYTHON_SEND_TARGET: value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_TARGET, 1}); break;
    case TA_PYTHON_SEND_DATA: value = ta_python_effect_payload(view); break;
    case TA_PYTHON_SEND_TRACEPARENT: /* formatted by core on first ask; empty when untraced */
        value = ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_TRACEPARENT, 1});
        break;
    default: {
        PyObject *row = PyList_GetItem(view->host->processors, view->processor);
        value = row ? Py_NewRef(PyTuple_GetItem(row, 0)) : NULL;
    }
    }
    if (value) view->fields[which] = Py_NewRef(value);
    return value;
}

static PyObject *ta_python_entry_read(ta_python_view *view, int which) {
    if (view->fields[which]) return Py_NewRef(view->fields[which]);
    if (ta_python_view_live(view) < 0) return NULL;
    PyObject *value =
        which == TA_PYTHON_ENTRY_SESSION
            ? PyLong_FromUnsignedLongLong(ta_host_effect_number(view->host->host, view->index, TA_EFFECT_FIELD_SESSION))
        : which == TA_PYTHON_ENTRY_LABEL ? ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_AUXILIARY, 0})
                                         : ta_python_effect_text(view, (ta_python_text_field){TA_EFFECT_FIELD_EVENT, 0});
    if (value) view->fields[which] = Py_NewRef(value);
    return value;
}

/* An envelope's bytes, written by ta_host_envelope (a view of the host's
 * until the ack) and copied once into a bytes the application keeps. */
static PyObject *ta_python_envelope_read(ta_python_view *view, int which) {
    if (view->fields[which]) return Py_NewRef(view->fields[which]);
    if (ta_python_view_live(view) < 0) return NULL;
    ta_host *core = view->host->host;
    PyObject *value = NULL;
    if (which == TA_PYTHON_ENVELOPE_PEER)
        value = PyLong_FromUnsignedLongLong(ta_host_effect_number(core, view->index, TA_EFFECT_FIELD_PEER));
    else if (which == TA_PYTHON_ENVELOPE_ERROR)
        value = PyBool_FromLong(ta_host_effect_number(core, view->index, TA_EFFECT_FIELD_KIND) == TA_HOST_EFFECT_PEER_ERROR);
    else {
        const ta_host_effect *effects;
        ta_bytes bytes;
        ta_status status = ta_host_effects(core, &effects) > view->index
                               ? ta_host_envelope(core, &effects[view->index], &bytes)
                               : TA_ERROR_STALE;
        if (status != TA_OK) ta_python_raise_status(view->host->state, status);
        else value = PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
    }
    if (value) view->fields[which] = Py_NewRef(value);
    return value;
}

/* ---- the view types ----------------------------------------------------------------- */

static int ta_python_view_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_view *view = (ta_python_view *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(view->host);
    for (int i = 0; i < TA_PYTHON_SEND_FIELDS; i++) Py_VISIT(view->fields[i]);
    return 0;
}

static int ta_python_view_clear(PyObject *self) {
    ta_python_view *view = (ta_python_view *)self;
    Py_CLEAR(view->host);
    for (int i = 0; i < TA_PYTHON_SEND_FIELDS; i++) Py_CLEAR(view->fields[i]);
    return 0;
}

static void ta_python_view_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_view_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static ta_python_view *ta_python_view_new(ta_python_host *host, PyObject *type, uint32_t index) {
    ta_python_view *view = (ta_python_view *)PyType_GenericAlloc((PyTypeObject *)type, 0);
    if (!view) return NULL;
    view->host = (ta_python_host *)Py_NewRef((PyObject *)host);
    view->acks = host->acks;
    view->index = index;
    return view;
}

#define TA_PYTHON_GETTER(kind, name, which)                                                                  \
    static PyObject *ta_python_##kind##_##name(PyObject *self, void *closure) {                             \
        (void)closure;                                                                                       \
        return ta_python_##kind##_read((ta_python_view *)self, which);                                        \
    }

TA_PYTHON_GETTER(send, session, TA_PYTHON_SEND_SESSION)
TA_PYTHON_GETTER(send, sendid, TA_PYTHON_SEND_SENDID)
TA_PYTHON_GETTER(send, event, TA_PYTHON_SEND_EVENT)
TA_PYTHON_GETTER(send, target, TA_PYTHON_SEND_TARGET)
TA_PYTHON_GETTER(send, data, TA_PYTHON_SEND_DATA)
TA_PYTHON_GETTER(send, traceparent, TA_PYTHON_SEND_TRACEPARENT)
TA_PYTHON_GETTER(send, type, TA_PYTHON_SEND_TYPE)
TA_PYTHON_GETTER(entry, session, TA_PYTHON_ENTRY_SESSION)
TA_PYTHON_GETTER(entry, label, TA_PYTHON_ENTRY_LABEL)
TA_PYTHON_GETTER(entry, text, TA_PYTHON_ENTRY_TEXT)
TA_PYTHON_GETTER(envelope, peer, TA_PYTHON_ENVELOPE_PEER)
TA_PYTHON_GETTER(envelope, error, TA_PYTHON_ENVELOPE_ERROR)
TA_PYTHON_GETTER(envelope, cbor, TA_PYTHON_ENVELOPE_CBOR)

/* ---- answering a send ------------------------------------------------------------------ */

/* The answer or the failure of a send: ta_host_send to its session with
 * its sendid (bindings.md, section 4). `name` NULL: the failure. */
typedef struct ta_python_answer {
    PyObject *name, *data, *traceparent;
} ta_python_answer;

static int ta_python_send_answer_with(ta_python_view *view, const ta_python_answer *answer) {
    if (ta_python_view_live(view) < 0) return -1; /* a thin view answers before its ack, whatever was read */
    if (view->answered) {
        PyErr_SetString(view->host->state->error, "the send was answered already");
        return -1;
    }
    PyObject *session = ta_python_send_read(view, TA_PYTHON_SEND_SESSION);
    PyObject *sendid = session ? ta_python_send_read(view, TA_PYTHON_SEND_SENDID) : NULL;
    PyObject *type = sendid ? ta_python_send_read(view, TA_PYTHON_SEND_TYPE) : NULL;
    int status = type ? 0 : -1;
    ta_python_call call;
    if (status == 0) status = ta_python_enter(view->host, &call);
    if (status == 0) {
        ta_python_host *host = view->host;
        ta_event_in event;
        memset(&event, 0, sizeof event);
        event.size = sizeof event;
        event.session = PyLong_AsUnsignedLongLong(session);
        ta_python_payload_for payload = {event.session, {0, 0}, answer->data, 0, TA_NIL};
        if (ta_python_text(sendid, &event.sendid) < 0 || ta_python_text(answer->traceparent, &event.traceparent) < 0)
            status = -1;
        else if (!answer->name) event.flags = TA_EVENT_IN_ERROR_COMMUNICATION; /* the name is not repeated */
        else if (ta_python_text(answer->name, &event.name) < 0 || ta_python_text(type, &event.origintype) < 0 ||
                 ta_python_host_payload(host, &payload) < 0)
            status = -1;
        if (status == 0) {
            event.data = payload.payload;
            ta_status sent = ta_host_send(host->host, &event);
            if (sent != TA_OK) {
                ta_python_host_payload_release(host, payload.payload, payload.datamodel);
                ta_python_raise_status(host->state, sent);
                status = -1;
            } else {
                view->answered = 1;
                ta_python_loop_schedule(host);
            }
        }
        ta_python_leave(&call);
    }
    Py_XDECREF(session);
    Py_XDECREF(sendid);
    Py_XDECREF(type);
    return status;
}

static PyObject *ta_python_send_answer(PyObject *self, PyObject *arguments, PyObject *keywords) {
    ta_python_view *view = (ta_python_view *)self;
    PyObject *name, *data = Py_None, *traceparent = NULL;
    static char *const names[] = {"event", "data", "traceparent", NULL};
    if (!PyArg_ParseTupleAndKeywords(arguments, keywords, "U|O$O:answer", names, &name, &data, &traceparent)) return NULL;
    PyObject *kept = traceparent ? Py_NewRef(traceparent) : ta_python_send_read(view, TA_PYTHON_SEND_TRACEPARENT);
    if (!kept) return NULL;
    ta_python_answer answer = {name, data, kept};
    int status = ta_python_send_answer_with(view, &answer);
    Py_DECREF(kept);
    if (status < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *ta_python_send_fail(PyObject *self, PyObject *unused) {
    (void)unused;
    ta_python_view *view = (ta_python_view *)self;
    PyObject *traceparent = ta_python_send_read(view, TA_PYTHON_SEND_TRACEPARENT);
    if (!traceparent) return NULL;
    ta_python_answer answer = {NULL, Py_None, traceparent};
    int status = ta_python_send_answer_with(view, &answer);
    Py_DECREF(traceparent);
    if (status < 0) return NULL;
    Py_RETURN_NONE;
}

/* A handler raised before answering: fail the send for it (ADR 0009: a
 * runtime that cannot do what an effect asks answers with a failure); the
 * exception the handler raised stays set. */
static void ta_python_send_failed(ta_python_view *view) {
    if (view->answered) return;
    PyObject *raised = PyErr_GetRaisedException();
    PyObject *done = ta_python_send_fail((PyObject *)view, NULL);
    Py_XDECREF(done);
    PyErr_Clear();
    PyErr_SetRaisedException(raised);
}

static PyObject *ta_python_view_repr(PyObject *self) {
    ta_python_view *view = (ta_python_view *)self;
    return PyUnicode_FromFormat("<%s %s, effect %u of batch %llu>", Py_TYPE(self) == (PyTypeObject *)view->host->state->send_type ? "send" : "view",
                                view->record ? "record" : "view", view->index, (unsigned long long)view->acks);
}

static PyGetSetDef ta_python_send_properties[] = {
    {"session", ta_python_send_session, NULL, "The sending session (int).", NULL},
    {"sendid", ta_python_send_sendid, NULL, "The send's sendid (str or None).", NULL},
    {"event", ta_python_send_event, NULL, "The event's name.", NULL},
    {"target", ta_python_send_target, NULL, "The send's target (str or None).", NULL},
    {"data", ta_python_send_data, NULL, "The event's data: a \"python\" payload's value itself, or quickjs's decoded.", NULL},
    {"traceparent", ta_python_send_traceparent, NULL, "The sending step's trace as W3C text, None when untraced.", NULL},
    {"type", ta_python_send_type, NULL, "The I/O processor's type.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef ta_python_send_methods[] = {
    {"answer", (PyCFunction)(void (*)(void))ta_python_send_answer, METH_VARARGS | METH_KEYWORDS,
     "answer(event, data=None, *, traceparent=send.traceparent)\n--\n\nAn event for the sending session with this "
     "send's sendid; it keeps the send's trace unless traceparent says otherwise."},
    {"fail", ta_python_send_fail, METH_NOARGS,
     "fail()\n--\n\nerror.communication for the sending session, with this send's sendid and trace."},
    {NULL, NULL, 0, NULL}
};

static PyType_Slot ta_python_send_slots[] = {
    {Py_tp_doc, "A send to an I/O processor: a thin view until the batch's ack, or a record for an async handler."},
    {Py_tp_dealloc, ta_python_view_dealloc},
    {Py_tp_traverse, ta_python_view_traverse},
    {Py_tp_clear, ta_python_view_clear},
    {Py_tp_getset, ta_python_send_properties},
    {Py_tp_methods, ta_python_send_methods},
    {Py_tp_repr, ta_python_view_repr},
    {0, NULL}
};

static PyGetSetDef ta_python_entry_properties[] = {
    {"session", ta_python_entry_session, NULL, "The logging session (int).", NULL},
    {"label", ta_python_entry_label, NULL, "The <log>'s label.", NULL},
    {"text", ta_python_entry_text, NULL, "The <log>'s text.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_entry_slots[] = {
    {Py_tp_doc, "A <log> line: a thin view until the batch's ack."},
    {Py_tp_dealloc, ta_python_view_dealloc},
    {Py_tp_traverse, ta_python_view_traverse},
    {Py_tp_clear, ta_python_view_clear},
    {Py_tp_getset, ta_python_entry_properties},
    {0, NULL}
};

static PyGetSetDef ta_python_envelope_properties[] = {
    {"peer", ta_python_envelope_peer, NULL, "The peer it goes to (int).", NULL},
    {"error", ta_python_envelope_error, NULL, "True for an error envelope.", NULL},
    {"cbor", ta_python_envelope_cbor, NULL, "The envelope as CBOR bytes, written once.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyType_Slot ta_python_envelope_slots[] = {
    {Py_tp_doc, "An envelope for another system: a thin view until the batch's ack."},
    {Py_tp_dealloc, ta_python_view_dealloc},
    {Py_tp_traverse, ta_python_view_traverse},
    {Py_tp_clear, ta_python_view_clear},
    {Py_tp_getset, ta_python_envelope_properties},
    {0, NULL}
};

#define TA_PYTHON_VIEW_FLAGS (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION)
static PyType_Spec ta_python_send_spec = {"tinyactors.Send", sizeof(ta_python_view), 0, TA_PYTHON_VIEW_FLAGS, ta_python_send_slots};
static PyType_Spec ta_python_entry_spec = {"tinyactors.LogEntry", sizeof(ta_python_view), 0, TA_PYTHON_VIEW_FLAGS, ta_python_entry_slots};
static PyType_Spec ta_python_envelope_spec = {"tinyactors.Envelope", sizeof(ta_python_view), 0, TA_PYTHON_VIEW_FLAGS,
                                              ta_python_envelope_slots};

/* ---- dispatch -------------------------------------------------------------------------- */

static PyMethodDef ta_python_task_done_def = {"done", ta_python_send_task_done, METH_O, "An async processor's task ended."};

/* An async def processor's task ended: one that raised before answering
 * has its send failed, and the exception is kept for the driver. */
PyObject *ta_python_send_task_done(PyObject *send, PyObject *task) {
    ta_python_view *view = (ta_python_view *)send;
    PyObject *cancelled = PyObject_CallMethod(task, "cancelled", NULL);
    int was_cancelled = cancelled ? PyObject_IsTrue(cancelled) : 1;
    Py_XDECREF(cancelled);
    PyObject *exception = was_cancelled ? NULL : PyObject_CallMethod(task, "exception", NULL);
    if (exception && exception != Py_None && view->host->host) {
        if (!view->answered) {
            PyObject *done = ta_python_send_fail(send, NULL);
            Py_XDECREF(done);
            PyErr_Clear();
        }
        if (!view->host->error) view->host->error = Py_NewRef(exception);
        ta_python_loop_schedule(view->host); /* the turn hands it to idle() */
    }
    Py_XDECREF(exception);
    PyErr_Clear();
    Py_RETURN_NONE;
}

static PyObject *ta_python_telemetry_done(PyObject *host, PyObject *task);

static PyMethodDef ta_python_telemetry_done_def = {"done", ta_python_telemetry_done, METH_O,
                                                   "An async telemetry handler's task ended."};

/* An async handler's task ended (telemetry, wake): its exception is kept
 * for the driver, as a plain handler's is. */
static PyObject *ta_python_telemetry_done(PyObject *self, PyObject *task) {
    ta_python_host *host = (ta_python_host *)self;
    PyObject *cancelled = PyObject_CallMethod(task, "cancelled", NULL);
    int was_cancelled = cancelled ? PyObject_IsTrue(cancelled) : 1;
    Py_XDECREF(cancelled);
    PyObject *exception = was_cancelled ? NULL : PyObject_CallMethod(task, "exception", NULL);
    if (exception && exception != Py_None && host->host) {
        if (!host->error) host->error = Py_NewRef(exception);
        ta_python_loop_schedule(host);
    }
    Py_XDECREF(exception);
    PyErr_Clear();
    Py_RETURN_NONE;
}

PyObject *ta_python_host_task(ta_python_host *host, PyObject *coroutine) {
    PyObject *task = host->loop != Py_None ? PyObject_CallMethod(host->loop, "create_task", "O", coroutine) : NULL;
    PyObject *done = task ? PyCFunction_New(&ta_python_telemetry_done_def, (PyObject *)host) : NULL;
    PyObject *added = done ? PyObject_CallMethod(task, "add_done_callback", "O", done) : NULL;
    if (!task && !PyErr_Occurred()) {
        PyObject *closed = PyObject_CallMethod(coroutine, "close", NULL); /* no "never awaited" warning */
        Py_XDECREF(closed);
        PyErr_SetString(PyExc_RuntimeError, "an async def handler needs async with");
    }
    Py_XDECREF(done);
    Py_DECREF(coroutine);
    if (!added) Py_CLEAR(task);
    Py_XDECREF(added);
    return task;
}

/* A WAKE: an event came for a hibernated session (one effect per
 * hibernation, however many come). The application's handler loads the
 * session's blob and calls host.wake(session, blob), now or later (an
 * async def handler runs as a task); the events wait in its mailbox. */
static void ta_python_dispatch_wake(ta_python_host *host, uint32_t index) {
    if (host->wake == Py_None) return;
    PyObject *session = PyLong_FromUnsignedLongLong(ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_SESSION));
    PyObject *result = session ? PyObject_CallFunctionObjArgs(host->wake, (PyObject *)host, session, NULL) : NULL;
    if (result && host->wake_async) result = ta_python_host_task(host, result);
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF(session);
}

/* The application's exporter gets the request as bytes it keeps (one copy,
 * boundary.md section 7); posting it, retrying and capping what waits are
 * the application's. Without a handler the request is dropped. */
void ta_python_telemetry(ta_python_host *host, const ta_python_export *export) {
    if (!host->telemetry || host->telemetry == Py_None) return;
    PyObject *signal = PyUnicode_InternFromString(export->signal);
    PyObject *data = signal ? PyBytes_FromStringAndSize(export->data.pointer, (Py_ssize_t)export->data.length) : NULL;
    PyObject *result = data ? PyObject_CallFunctionObjArgs(host->telemetry, (PyObject *)host, signal, data, NULL) : NULL;
    if (result && host->telemetry_async) { /* a coroutine: a task on the host's loop */
        PyObject *task = ta_python_host_task(host, result);
        if (task && host->ending && host->ending_tasks) PyList_Append(host->ending_tasks, task); /* __aexit__ awaits */
        result = task;
    }
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF(data);
    Py_XDECREF(signal);
}

void ta_python_take(ta_python_host *host, int traces) {
    ta_bytes taken; /* the host's view, until the ack */
    ta_status status = traces ? ta_host_traces(host->host, &taken) : ta_host_logs(host->host, &taken);
    uint32_t bit = traces ? 1u : 2u;
    if (status == TA_OK) {
        host->retake &= ~bit;
        if (taken.length) ta_python_telemetry(host, &(ta_python_export){traces ? "traces" : "logs", taken});
    } else if (status == TA_ERROR_NO_MEMORY && !(host->retake & bit)) {
        host->retake |= bit; /* the request stays staged; no other effect comes until a take */
        ta_python_raise_status(host->state, status);
        ta_python_host_keep_error(host);
    }
}

static void ta_python_dispatch_send(ta_python_host *host, uint32_t index) {
    uint32_t processor = (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_IO_PROCESSOR);
    PyObject *row = processor < (uint32_t)PyList_Size(host->processors) ? PyList_GetItem(host->processors, processor) : NULL;
    if (!row) return; /* a send to an invoked service (TA_NIL): no service runs in the API yet */
    ta_python_view *view = ta_python_view_new(host, host->state->send_type, index);
    if (!view) {
        ta_python_host_keep_error(host);
        return;
    }
    view->processor = processor;
    PyObject *handler = PyTuple_GetItem(row, 1);
    if (handler == Py_None) { /* no handler: a failed completion */
        PyObject *done = ta_python_send_fail((PyObject *)view, NULL);
        if (!done) ta_python_host_keep_error(host);
        Py_XDECREF(done);
    } else if (PyTuple_GetItem(row, 2) == Py_True) { /* async def: a record, then a task */
        int status = 0;
        for (int which = 0; which < TA_PYTHON_SEND_FIELDS && status == 0; which++) {
            PyObject *value = ta_python_send_read(view, which);
            if (!value) status = -1;
            Py_XDECREF(value);
        }
        view->record = 1;
        PyObject *coroutine = status == 0 ? PyObject_CallFunctionObjArgs(handler, (PyObject *)host, (PyObject *)view, NULL) : NULL;
        PyObject *task = coroutine ? PyObject_CallMethod(host->loop, "create_task", "O", coroutine) : NULL;
        PyObject *done = task ? PyCFunction_New(&ta_python_task_done_def, (PyObject *)view) : NULL;
        PyObject *added = done ? PyObject_CallMethod(task, "add_done_callback", "O", done) : NULL;
        if (!added) {
            ta_python_send_failed(view);
            ta_python_host_keep_error(host);
        }
        Py_XDECREF(coroutine);
        Py_XDECREF(task);
        Py_XDECREF(done);
        Py_XDECREF(added);
    } else {
        PyObject *result = PyObject_CallFunctionObjArgs(handler, (PyObject *)host, (PyObject *)view, NULL);
        if (!result) {
            ta_python_send_failed(view);
            ta_python_host_keep_error(host);
        }
        Py_XDECREF(result);
    }
    Py_DECREF(view);
}

/* A log line or an envelope to its handler, as a thin view. */
typedef struct ta_python_handler {
    PyObject *handler; /* None: the effect is dropped */
    PyObject *type;    /* the view's */
} ta_python_handler;

static void ta_python_dispatch_view(ta_python_host *host, ta_python_handler handler, uint32_t index) {
    if (handler.handler == Py_None) return;
    ta_python_view *view = ta_python_view_new(host, handler.type, index);
    PyObject *result = view ? PyObject_CallFunctionObjArgs(handler.handler, (PyObject *)host, (PyObject *)view, NULL) : NULL;
    if (!result) ta_python_host_keep_error(host);
    Py_XDECREF(result);
    Py_XDECREF((PyObject *)view);
}

void ta_python_dispatch(ta_python_host *host, uint32_t index) {
    ta_host *core = host->host;
    switch ((uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_KIND)) {
    case TA_EFFECT_SEND: ta_python_dispatch_send(host, index); break;
    case TA_EFFECT_LOG: ta_python_dispatch_view(host, (ta_python_handler){host->log, host->state->entry_type}, index); break;
    case TA_HOST_EFFECT_PEER:
    case TA_HOST_EFFECT_PEER_ERROR: ta_python_dispatch_view(host, (ta_python_handler){host->peer, host->state->envelope_type}, index); break;
    case TA_EFFECT_REQUEST: { /* a fetch at run time: refused in C until it is built (AGENTS.md) */
        ta_resume resume = {(uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_SEGMENT),
                            (uint32_t)ta_host_effect_number(core, index, TA_EFFECT_FIELD_TOKEN), TA_ERROR_INVALID, 0, 0};
        ta_host_resume(core, &resume);
        ta_python_loop_schedule(host); /* an input: the turn runs it */
        break;
    }
    case TA_HOST_EFFECT_REPLY: ta_python_reply(host, index); break;
    case TA_EFFECT_WAKE: ta_python_dispatch_wake(host, index); break;
    case TA_HOST_EFFECT_TRACES: /* a request is ready: taken (the next comes only after a take) */
    case TA_HOST_EFFECT_LOGS: {
        ta_python_take(host, ta_host_effect_number(core, index, TA_EFFECT_FIELD_KIND) == TA_HOST_EFFECT_TRACES);
        break;
    }
    default: /* DONE, ERROR: the datamodel's rows went with destroy; a NOTIFICATION we do not know; the
                rest has no API yet */
        break;
    }
}

int ta_python_effects_init_types(PyObject *module, ta_python_module_state *state) {
    state->send_type = PyType_FromModuleAndSpec(module, &ta_python_send_spec, NULL);
    state->entry_type = state->send_type ? PyType_FromModuleAndSpec(module, &ta_python_entry_spec, NULL) : NULL;
    state->envelope_type = state->entry_type ? PyType_FromModuleAndSpec(module, &ta_python_envelope_spec, NULL) : NULL;
    if (!state->envelope_type) return -1;
    if (PyModule_AddObjectRef(module, "Send", state->send_type) < 0 ||
        PyModule_AddObjectRef(module, "LogEntry", state->entry_type) < 0)
        return -1;
    return PyModule_AddObjectRef(module, "Envelope", state->envelope_type);
}
