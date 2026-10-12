/* effects.c — each effect of a run, to the application (tinyactors-core/
 * docs/bindings.md, section 4). Fields are read with the accessors
 * (ta_host_effect_number, _text, _bytes) and handed to the handler as one
 * call per effect; what a handler keeps is copied first, since the views
 * end at the ack.
 *
 *   TA_EFFECT_LOG                on_log(session, label, text)
 *   TA_EFFECT_DONE               on_done(session, data)
 *   TA_EFFECT_ERROR              on_error(session, code)
 *   TA_HOST_EFFECT_PEER,         on_peer(peer, envelope): the envelope's
 *   TA_HOST_EFFECT_PEER_ERROR    bytes (ta_host_envelope), for the transport
 *   TA_HOST_EFFECT_SEGMENT_FREED the segment's image is let go
 *
 * The other effects (SEND, INVOKE, UNINVOKE, REQUEST, WAKE, CHANGED,
 * MIGRATED, JOB_DONE, TRACES, LOGS) are later milestones' (ROADMAP.md). */
#include "extension.h"

/* Text of effect `index`, a new str. */
typedef struct ta_python_field {
    uint32_t index, field;
} ta_python_field;

static PyObject *ta_python_effect_text(ta_python_host *host, const ta_python_field *field) {
    ta_string_view text = ta_host_effect_text(host->host, field->index, field->field);
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", (Py_ssize_t)text.length, "strict");
}

static PyObject *ta_python_effect_number(ta_python_host *host, const ta_python_field *field) {
    return PyLong_FromUnsignedLongLong(ta_host_effect_number(host->host, field->index, field->field));
}

/* The PEER or PEER_ERROR effect `effect` as envelope bytes: ta_host_envelope's
 * view of the host's memory, copied into a bytes object before anything
 * else can call into the host. */
static PyObject *ta_python_effect_envelope(ta_python_host *host, const ta_host_effect *effect) {
    ta_bytes envelope = {NULL, 0};
    ta_status status = ta_host_envelope(host->host, effect, &envelope);
    if (status != TA_OK) {
        ta_python_raise_status(ta_python_host_state(host), "ta_host_envelope", status);
        return NULL;
    }
    return PyBytes_FromStringAndSize(envelope.pointer, (Py_ssize_t)envelope.length);
}

/* The arguments of the handler for effect `index`, a new tuple, or NULL
 * with no handler to call (and no error) or with a Python error. */
static PyObject *ta_python_effect_arguments(ta_python_host *host, const ta_python_batch *batch, uint32_t index) {
    uint32_t kind = (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_KIND);
    PyObject *session = ta_python_effect_number(host, &(ta_python_field){index, TA_EFFECT_FIELD_SESSION});
    PyObject *first = NULL, *second = NULL, *arguments = NULL;
    switch (kind) {
    case TA_EFFECT_LOG:
        first = ta_python_effect_text(host, &(ta_python_field){index, TA_EFFECT_FIELD_AUXILIARY});
        second = ta_python_effect_text(host, &(ta_python_field){index, TA_EFFECT_FIELD_EVENT});
        if (session && first && second) arguments = PyTuple_Pack(3, session, first, second);
        break;
    case TA_EFFECT_DONE: {
        ta_python_payload_reading reading = {
            ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_PAYLOAD),
            (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_DATAMODEL)};
        first = ta_python_payload_value(host, &reading);
        if (session && first) arguments = PyTuple_Pack(2, session, first);
        break;
    }
    case TA_EFFECT_ERROR:
        first = ta_python_effect_number(host, &(ta_python_field){index, TA_EFFECT_FIELD_FLAGS});
        if (session && first) arguments = PyTuple_Pack(2, session, first);
        break;
    case TA_HOST_EFFECT_PEER:
    case TA_HOST_EFFECT_PEER_ERROR:
        first = ta_python_effect_number(host, &(ta_python_field){index, TA_EFFECT_FIELD_PEER});
        second = ta_python_effect_envelope(host, &batch->effects[index]);
        if (first && second) arguments = PyTuple_Pack(2, first, second);
        break;
    default: break;
    }
    Py_XDECREF(session);
    Py_XDECREF(first);
    Py_XDECREF(second);
    return arguments;
}

/* The handler of an effect kind, or NULL. */
static PyObject *ta_python_effect_handler(ta_python_host *host, uint32_t kind) {
    switch (kind) {
    case TA_EFFECT_LOG: return host->handlers[TA_PYTHON_ON_LOG];
    case TA_EFFECT_DONE: return host->handlers[TA_PYTHON_ON_DONE];
    case TA_EFFECT_ERROR: return host->handlers[TA_PYTHON_ON_ERROR];
    case TA_HOST_EFFECT_PEER:
    case TA_HOST_EFFECT_PEER_ERROR: return host->handlers[TA_PYTHON_ON_PEER];
    default: return NULL;
    }
}

void ta_python_effects_dispatch(ta_python_host *host, const ta_python_batch *batch) {
    for (uint32_t i = 0; i < batch->count; i++) {
        uint32_t kind = (uint32_t)ta_host_effect_number(host->host, i, TA_EFFECT_FIELD_KIND);
        if (kind == TA_HOST_EFFECT_SEGMENT_FREED) {
            ta_python_image_drop(host, (uint32_t)ta_host_effect_number(host->host, i, TA_EFFECT_FIELD_SEGMENT));
            continue;
        }
        PyObject *handler = ta_python_effect_handler(host, kind);
        if (!handler) continue;
        Py_INCREF(handler); /* a handler may close the host, or replace itself */
        PyObject *arguments = ta_python_effect_arguments(host, batch, i);
        PyObject *result = arguments ? PyObject_CallObject(handler, arguments) : NULL;
        Py_XDECREF(arguments);
        Py_XDECREF(result);
        Py_DECREF(handler);
        if (PyErr_Occurred()) { /* the first error is raised after the turn (bindings.md, section 3) */
            if (!host->raised) host->raised = PyErr_GetRaisedException();
            else PyErr_Clear();
        }
    }
}
