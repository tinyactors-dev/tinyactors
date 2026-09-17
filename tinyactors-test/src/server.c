#include "server.h"

#include "jq.h"
#include "rpc.h"
#include "tinyactors.h"
#include "version.h"

typedef struct {
    rpc_server rpc;
    jv dispatch;
    bool running;
} tat_server_impl;

typedef enum {
    TAT_METHOD_DISCOVER = 1,
    TAT_METHOD_PING,
    TAT_METHOD_JSON_TRANSFORM,
    TAT_METHOD_METRICS,
    TAT_METHOD_SHUTDOWN,
    TAT_METHOD_COUNT
} tat_method;

typedef jv (*tat_handler_fn)(tat_server_impl *server, jv params, int *error_code);

static jv tat_dispatch_table(void) {
    jv table = jv_object();
    table = jv_object_set(table, jv_string("rpc.discover"),
                          jv_number(TAT_METHOD_DISCOVER));
    table = jv_object_set(table, jv_string("system.ping"),
                          jv_number(TAT_METHOD_PING));
    table = jv_object_set(table, jv_string("json.transform"),
                          jv_number(TAT_METHOD_JSON_TRANSFORM));
    table = jv_object_set(table, jv_string("metrics.snapshot"),
                          jv_number(TAT_METHOD_METRICS));
    return jv_object_set(table, jv_string("host.shutdown"),
                         jv_number(TAT_METHOD_SHUTDOWN));
}

static jv tat_discover(const tat_server_impl *server) {
    jv methods = jv_keys(jv_copy(server->dispatch));

    jv result = jv_object();
    result = jv_object_set(result, jv_string("name"), jv_string("tinyactors-test"));
    result = jv_object_set(result, jv_string("protocol"),
                           jv_string("JSON-RPC 2.0 subset over NDJSON"));
    result = jv_object_set(result, jv_string("jq"), jv_string(JQ_VERSION));
    result = jv_object_set(result, jv_string("datamodels"),
                           jv_array_append(jv_array(), jv_string("json")));
    result = jv_object_set(result, jv_string("methods"), methods);
    return jv_object_set(result, jv_string("tinyactors_actor_id_bytes"),
                         jv_number((double)sizeof(ta_actor_id)));
}

static jv tat_metrics(const tat_server_impl *server) {
    const rpc_server *rpc = &server->rpc;
    jv result = jv_object();
    result = jv_object_set(result, jv_string("requests"),
                           jv_number((double)rpc->message_count));
    result = jv_object_set(result, jv_string("parse_errors"),
                           jv_number((double)rpc->parse_error_count));
    result = jv_object_set(result, jv_string("invalid_requests"),
                           jv_number((double)rpc->invalid_request_count));
    result = jv_object_set(result, jv_string("notifications_received"),
                           jv_number((double)rpc->notifications_received));
    return jv_object_set(result, jv_string("notifications_sent"),
                         jv_number((double)rpc->notifications_sent));
}

static jv tat_transform(jv params, int *error_code) {
    if (jv_get_kind(params) != JV_KIND_OBJECT) {
        jv_free(params);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("params must be an object");
    }

    jv filter = jv_object_get(jv_copy(params), jv_string("filter"));
    jv input = jv_object_get(jv_copy(params), jv_string("input"));
    if (jv_get_kind(filter) != JV_KIND_STRING || !jv_is_valid(input)) {
        jv_free(filter);
        jv_free(input);
        jv_free(params);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("params.filter must be a string and params.input is required");
    }

    jq_state *jq = jq_init();
    if (jq == NULL) {
        jv_free(filter);
        jv_free(input);
        jv_free(params);
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("could not initialize libjq");
    }
    int compiled = jq_compile(jq, jv_string_value(filter));
    jv_free(filter);
    jv_free(params);
    if (!compiled) {
        jv_free(input);
        jq_teardown(&jq);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("jq filter did not compile");
    }

    jq_start(jq, input, 0);
    jv outputs = jv_array();
    for (;;) {
        jv output = jq_next(jq);
        if (!jv_is_valid(output)) {
            if (jv_invalid_has_msg(jv_copy(output))) {
                jv message = jv_invalid_get_msg(output);
                jv_free(outputs);
                jq_teardown(&jq);
                *error_code = RPC_INTERNAL_ERROR;
                return message;
            }
            jv_free(output);
            break;
        }
        outputs = jv_array_append(outputs, output);
    }
    jq_teardown(&jq);
    return outputs;
}

static jv tat_handle_discover(tat_server_impl *server, jv params, int *error_code) {
    (void)error_code;
    jv_free(params);
    return tat_discover(server);
}

static jv tat_handle_ping(tat_server_impl *server, jv params, int *error_code) {
    (void)server;
    (void)error_code;
    return params;
}

static jv tat_handle_transform(tat_server_impl *server, jv params, int *error_code) {
    (void)server;
    return tat_transform(params, error_code);
}

static jv tat_handle_metrics(tat_server_impl *server, jv params, int *error_code) {
    (void)error_code;
    jv_free(params);
    return tat_metrics(server);
}

static jv tat_handle_shutdown(tat_server_impl *server, jv params, int *error_code) {
    (void)error_code;
    jv_free(params);
    server->running = false;
    return jv_bool(true);
}

static const tat_handler_fn TAT_HANDLERS[TAT_METHOD_COUNT] = {
    [TAT_METHOD_DISCOVER] = tat_handle_discover,
    [TAT_METHOD_PING] = tat_handle_ping,
    [TAT_METHOD_JSON_TRANSFORM] = tat_handle_transform,
    [TAT_METHOD_METRICS] = tat_handle_metrics,
    [TAT_METHOD_SHUTDOWN] = tat_handle_shutdown,
};

static jv tat_dispatch(void *context, const char *method, jv params, int *error_code) {
    tat_server_impl *server = context;
    jv entry = jv_object_get(jv_copy(server->dispatch), jv_string(method));
    if (jv_get_kind(entry) == JV_KIND_NUMBER) {
        tat_method selected = (tat_method)(int)jv_number_value(entry);
        jv_free(entry);
        if (selected > 0 && selected < TAT_METHOD_COUNT &&
            TAT_HANDLERS[selected] != NULL) {
            return TAT_HANDLERS[selected](server, params, error_code);
        }
    } else {
        jv_free(entry);
    }

    jv_free(params);
    *error_code = RPC_METHOD_NOT_FOUND;
    return jv_string_fmt("unknown method: %s", method);
}

ta_result tat_server_init(tat_server *server, FILE *output, ta_allocator allocator) {
    if (server == NULL || output == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_arena_init(&server->arena, allocator, 4096);
    if (result != TA_OK) {
        return result;
    }
    tat_server_impl *impl = ta_arena_alloc(&server->arena, sizeof(*impl),
                                           _Alignof(tat_server_impl));
    if (impl == NULL) {
        ta_arena_destroy(&server->arena);
        return TA_ERR_NOMEM;
    }
    server->impl = impl;
    impl->dispatch = tat_dispatch_table();
    impl->running = true;
    rpc_server_init(&impl->rpc,
                    &(rpc_server_config){
                        .context = impl,
                        .dispatch = tat_dispatch,
                        .output = output,
                    });
    jv ready = jv_object_set(jv_object(), jv_string("transport"), jv_string("stdio"));
    if (!rpc_server_send_notification(&impl->rpc, "host.ready", ready)) {
        jv_free(impl->dispatch);
        ta_arena_destroy(&server->arena);
        *server = (tat_server){0};
        return TA_ERR_NOMEM;
    }
    return TA_OK;
}

void tat_server_destroy(tat_server *server) {
    if (server == NULL || server->impl == NULL) {
        return;
    }
    tat_server_impl *impl = server->impl;
    jv_free(impl->dispatch);
    ta_arena_destroy(&server->arena);
    *server = (tat_server){0};
}

bool tat_server_running(const tat_server *server) {
    const tat_server_impl *impl = server->impl;
    return impl->running;
}

void tat_server_handle_line(tat_server *server, const char *line, size_t length) {
    tat_server_impl *impl = server->impl;
    rpc_server_handle_line(&impl->rpc, line, length);
}
