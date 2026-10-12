/* request.c — Host.request(method, params=None): a method of
 * tinyactors-core/docs/requests.md through ta_host_request. One line, as
 * the guide has it (bindings.md, section 10): the map from the caller's
 * values, sent, and its `result` returned or its `error` raised unchanged.
 * The caller's values pass through unchecked: core validates them. */
#include "extension.h"

/* The reply to `request`: ta_host_request's view of the host's memory,
 * valid until the next call into the host. It is copied into a bytes
 * object before its values are made, since making them can run a
 * finalizer (the cycle collector) that calls into the host. */
static ta_status ta_python_request_send(ta_python_host *host, ta_bytes request, PyObject **reply) {
    ta_bytes view = {NULL, 0};
    ta_status status = ta_host_request(host->host, request, &view);
    if (status != TA_OK) return status;
    PyObject *bytes = PyBytes_FromStringAndSize(view.pointer, (Py_ssize_t)view.length);
    if (!bytes) return TA_ERROR_NO_MEMORY;
    status = ta_python_values_read(ta_python_host_state(host), (ta_bytes){PyBytes_AsString(bytes), view.length}, reply);
    Py_DECREF(bytes);
    return status;
}

PyObject *ta_python_host_request(ta_python_host *host, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"method", "params", NULL};
    PyObject *method = NULL, *params = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:request", (char **)keywords, &method, &params) ||
        !ta_python_host_usable(host))
        return NULL;
    const ta_python_state *state = ta_python_host_state(host);
    PyObject *request = PyDict_New(), *reply = NULL, *error = NULL, *result = NULL;
    ta_python_cbor cbor;
    ta_python_cbor_init(&cbor);
    PyObject *id = PyLong_FromLong(1);
    if (!request || !id || PyDict_SetItemString(request, "method", method) < 0 ||
        (params != Py_None && PyDict_SetItemString(request, "params", params) < 0) ||
        PyDict_SetItemString(request, "id", id) < 0)
        goto done;
    ta_status status = ta_python_values_write(state, request, &cbor);
    if (status == TA_OK) status = ta_python_request_send(host, cbor.bytes, &reply);
    if (status != TA_OK) {
        if (!PyErr_Occurred()) ta_python_raise_status(state, "ta_host_request", status);
        goto done;
    }
    if (PyDict_Check(reply) && PyDict_GetItemStringRef(reply, "error", &error) > 0) {
        ta_python_raise_reply(state, &(ta_python_refusal){error, TA_OK});
        goto done;
    }
    if (PyDict_Check(reply) && PyDict_GetItemStringRef(reply, "result", &result) <= 0 && !PyErr_Occurred())
        ta_python_raise_status(state, "ta_host_request", TA_ERROR_INVALID); /* a reply that is neither */
done:
    if (ta_python_datamodel_settle(&host->python) < 0) Py_CLEAR(result); /* the drops of actor.context */
    Py_XDECREF(request);
    Py_XDECREF(id);
    ta_python_cbor_free(&cbor);
    Py_XDECREF(reply);
    Py_XDECREF(error);
    return result;
}
