#include "rpc.h"

#include <string.h>

static jv rpc_member(jv object, const char *name) {
    return jv_object_get(jv_copy(object), jv_string(name));
}

static jv rpc_response(jv id, const char *field, jv value) {
    jv response = jv_object();
    response = jv_object_set(response, jv_string("jsonrpc"), jv_string("2.0"));
    response = jv_object_set(response, jv_string(field), value);
    return jv_object_set(response, jv_string("id"), id);
}

static jv rpc_error(jv id, int code, const char *message, jv data) {
    jv error = jv_object();
    error = jv_object_set(error, jv_string("code"), jv_number(code));
    error = jv_object_set(error, jv_string("message"), jv_string(message));
    if (jv_get_kind(data) != JV_KIND_NULL) {
        error = jv_object_set(error, jv_string("data"), data);
    } else {
        jv_free(data);
    }
    return rpc_response(id, "error", error);
}

static void rpc_write(rpc_server *server, jv value) {
    jv_dumpf(value, server->config.output, JV_PRINT_SORTED | JV_PRINT_INVALID);
    fputc('\n', server->config.output);
    fflush(server->config.output);
}

static bool rpc_valid_id(jv id) {
    jv_kind kind = jv_get_kind(id);
    return kind == JV_KIND_NULL || kind == JV_KIND_NUMBER || kind == JV_KIND_STRING;
}

static const char *rpc_error_message(int code) {
    switch (code) {
        case RPC_METHOD_NOT_FOUND:
            return "Method not found";
        case RPC_INVALID_PARAMS:
            return "Invalid params";
        default:
            /* -32099..-32000 are implementation-defined server errors. */
            return code >= -32099 && code <= -32000 ? "Server error" : "Internal error";
    }
}

void rpc_server_init(rpc_server *server, const rpc_server_config *config) {
    *server = (rpc_server){.config = *config};
}

bool rpc_server_send_notification(rpc_server *server, const char *method, jv params) {
    if (method == NULL || method[0] == '\0') {
        jv_free(params);
        return false;
    }
    if (jv_is_valid(params)) {
        jv_kind kind = jv_get_kind(params);
        if (kind != JV_KIND_ARRAY && kind != JV_KIND_OBJECT) {
            jv_free(params);
            return false;
        }
    }

    jv notification = jv_object();
    notification = jv_object_set(notification, jv_string("jsonrpc"), jv_string("2.0"));
    notification = jv_object_set(notification, jv_string("method"), jv_string(method));
    if (jv_is_valid(params)) {
        notification = jv_object_set(notification, jv_string("params"), params);
    } else {
        jv_free(params);
    }
    server->notifications_sent += 1;
    rpc_write(server, notification);
    return true;
}

void rpc_server_handle_line(rpc_server *server, const char *line, size_t length) {
    server->message_count += 1;
    jv request = jv_parse_sized(line, (int)length);
    if (!jv_is_valid(request)) {
        server->parse_error_count += 1;
        jv data = jv_invalid_has_msg(jv_copy(request))
                      ? jv_invalid_get_msg(request)
                      : jv_null();
        rpc_write(server, rpc_error(jv_null(), RPC_PARSE_ERROR, "Parse error", data));
        return;
    }

    if (jv_get_kind(request) != JV_KIND_OBJECT) {
        server->invalid_request_count += 1;
        jv_free(request);
        rpc_write(server,
                  rpc_error(jv_null(), RPC_INVALID_REQUEST, "Invalid Request", jv_null()));
        return;
    }

    jv version = rpc_member(request, "jsonrpc");
    jv method = rpc_member(request, "method");
    jv id = rpc_member(request, "id");
    jv params = rpc_member(request, "params");
    bool notification = !jv_is_valid(id);
    bool valid = jv_get_kind(version) == JV_KIND_STRING &&
                 strcmp(jv_string_value(version), "2.0") == 0 &&
                 jv_get_kind(method) == JV_KIND_STRING &&
                 (notification || rpc_valid_id(id));
    jv_free(version);

    if (!valid) {
        server->invalid_request_count += 1;
        jv_free(method);
        jv_free(params);
        if (jv_is_valid(id)) {
            jv_free(id);
        }
        jv_free(request);
        rpc_write(server,
                  rpc_error(jv_null(), RPC_INVALID_REQUEST, "Invalid Request", jv_null()));
        return;
    }

    if (!jv_is_valid(params)) {
        params = jv_null();
    }
    if (notification) {
        server->notifications_received += 1;
    }
    int error_code = 0;
    jv result = server->config.dispatch(server->config.context,
                                        jv_string_value(method), params, &error_code);
    jv_free(method);
    jv_free(request);

    if (notification) {
        jv_free(id);
        jv_free(result);
        return;
    }
    if (error_code == 0) {
        rpc_write(server, rpc_response(id, "result", result));
    } else {
        rpc_write(server, rpc_error(id, error_code, rpc_error_message(error_code), result));
    }
}

