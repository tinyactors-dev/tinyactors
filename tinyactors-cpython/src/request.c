/* request.c — Host.request(method, params=None): a method of
 * tinyactors-core/docs/requests.md through ta_host_request. One line, as
 * the guide has it (bindings.md, section 10): the map from the caller's
 * values, sent, and its `result` returned or its `error` raised unchanged.
 * The caller's values pass through unchecked: core validates them. */
#include "extension.h"

/* The reply to `request` (CBOR) into a buffer grown to the length core
 * asks for (ta_buffer: TA_ERROR_NO_MEMORY applied nothing). */
static ta_status ta_python_request_send(ta_python_host *host, ta_bytes request, PyObject **reply) {
    const ta_python_state *state = ta_python_host_state(host);
    char room[4096];
    ta_buffer buffer = {room, sizeof room, 0};
    ta_status status = ta_host_request(host->host, request, &buffer);
    if (status == TA_ERROR_NO_MEMORY && buffer.length > buffer.capacity) {
        size_t length = buffer.length;
        buffer = (ta_buffer){PyMem_Malloc(length), length, 0};
        if (!buffer.pointer) {
            PyErr_NoMemory();
            return TA_ERROR_NO_MEMORY;
        }
        status = ta_host_request(host->host, request, &buffer);
    }
    if (status == TA_OK) status = ta_python_values_read(state, (ta_bytes){buffer.pointer, buffer.length}, reply);
    if (buffer.pointer != room) PyMem_Free(buffer.pointer);
    return status;
}

PyObject *ta_python_host_request(ta_python_host *host, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"method", "params", NULL};
    PyObject *method = NULL, *params = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O:request", (char **)keywords, &method, &params) ||
        !ta_python_host_usable(host))
        return NULL;
    const ta_python_state *state = ta_python_host_state(host);
    PyObject *request = PyDict_New(), *bytes = NULL, *reply = NULL, *error = NULL, *result = NULL;
    PyObject *id = PyLong_FromLong(1);
    if (!request || !id || PyDict_SetItemString(request, "method", method) < 0 ||
        (params != Py_None && PyDict_SetItemString(request, "params", params) < 0) ||
        PyDict_SetItemString(request, "id", id) < 0)
        goto done;
    ta_status status = ta_python_values_bytes(state, request, &bytes);
    if (status == TA_OK)
        status = ta_python_request_send(host, (ta_bytes){PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes)}, &reply);
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
    if (host->python && ta_python_datamodel_settle(host->python) < 0) Py_CLEAR(result); /* the drops of actor.context */
    Py_XDECREF(request);
    Py_XDECREF(id);
    Py_XDECREF(bytes);
    Py_XDECREF(reply);
    Py_XDECREF(error);
    return result;
}
