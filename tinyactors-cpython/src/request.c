/* request.c — Host.request(method, params=None): a method of
 * tinyactors-core/docs/requests.md through ta_host_request. One line, as
 * the guide has it (bindings.md, section 10): the map from the caller's
 * values, sent, and its `result` returned or its `error` raised unchanged.
 * The caller's values pass through unchecked: core validates them. */
#include "extension.h"

/* The reply to `request` (CBOR), written by ta_host_request straight into
 * a bytes object (ta_buffer: TA_ERROR_NO_MEMORY applied nothing, so the
 * second call is the request's only effect). */
typedef struct ta_python_request_writing {
    ta_host *host;
    ta_bytes request;
} ta_python_request_writing;

static ta_status ta_python_request_write(const void *context, ta_buffer *out) {
    const ta_python_request_writing *writing = context;
    return ta_host_request(writing->host, writing->request, out);
}

static ta_status ta_python_request_send(ta_python_host *host, ta_bytes request, PyObject **reply) {
    ta_python_request_writing writing = {host->host, request};
    PyObject *bytes = NULL;
    ta_status status = ta_python_written(&(ta_python_writing){ta_python_request_write, &writing}, &bytes);
    if (status == TA_OK)
        status = ta_python_values_read(ta_python_host_state(host),
                                       (ta_bytes){PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes)}, reply);
    Py_XDECREF(bytes);
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
    if (ta_python_datamodel_settle(&host->python) < 0) Py_CLEAR(result); /* the drops of actor.context */
    Py_XDECREF(request);
    Py_XDECREF(id);
    Py_XDECREF(bytes);
    Py_XDECREF(reply);
    Py_XDECREF(error);
    return result;
}
