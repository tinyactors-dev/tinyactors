#include "server.h"

#include <string.h>

#include "binding.h"
#include "hosted.h"
#include "instruments.h"
#include "jq.h"
#include "rpc.h"
#include "ta_rpc.h"
#include "value_codec.h"
#include "version.h"

typedef struct {
    rpc_server rpc;
    tat_bind bind;
    /* Method name -> index into `methods`; discovery is derived from it. */
    jv dispatch;
    /* Method name -> parsed schema. */
    jv schemas;
    const tat_method **methods;
    size_t method_count;
    bool running;
} tat_server_impl;

static tat_server_impl *tat_host(tat_bind *bind) {
    return bind->host;
}

static jv tat_handle_discover(tat_bind *bind, jv params, int *error_code) {
    (void)params;
    (void)error_code;
    const tat_server_impl *server = tat_host(bind);
    jv unimplemented = jv_object();
    jv_object_foreach(jv_copy(server->schemas), name, schema) {
        jv implemented = jv_object_get(schema, jv_string("implemented"));
        if (jv_get_kind(implemented) == JV_KIND_FALSE) {
            unimplemented = jv_object_set(unimplemented, jv_copy(name), jv_true());
        }
        jv_free(implemented);
        jv_free(name);
    }

    jv result = jv_object();
    result = jv_object_set(result, jv_string("name"), jv_string("tinyactors-test"));
    result = jv_object_set(result, jv_string("protocol"),
                           jv_string("JSON-RPC 2.0 subset over NDJSON"));
    result = jv_object_set(result, jv_string("jq"), jv_string(JQ_VERSION));
    /* Every data model src/adapters.c registers, in the order it lists them. */
    jv datamodels = jv_array();
    datamodels = jv_array_append(datamodels, jv_string("jq"));
    datamodels = jv_array_append(datamodels, jv_string("ecmascript"));
    datamodels = jv_array_append(datamodels, jv_string("null"));
    result = jv_object_set(result, jv_string("datamodels"), datamodels);
    result = jv_object_set(result, jv_string("methods"), jv_keys(jv_copy(server->dispatch)));
    result = jv_object_set(result, jv_string("schemas"), jv_copy(server->schemas));
    result = jv_object_set(result, jv_string("unimplemented"), jv_keys(unimplemented));
    jv adapters = jv_array();
    for (size_t index = 0; index < TAT_ADAPTER_COUNT; index += 1) {
        adapters = jv_array_append(adapters, jv_string(TAT_ADAPTERS[index].name));
    }
    result = jv_object_set(result, jv_string("adapters"), adapters);
    return jv_object_set(result, jv_string("tinyactors_actor_id_bytes"),
                         jv_number((double)sizeof(ta_actor_id)));
}

static jv tat_handle_metrics(tat_bind *bind, jv params, int *error_code) {
    (void)params;
    (void)error_code;
    const rpc_server *rpc = &tat_host(bind)->rpc;
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

static jv tat_handle_ping(tat_bind *bind, jv params, int *error_code) {
    (void)bind;
    (void)error_code;
    return jv_copy(params);
}

static jv tat_handle_transform(tat_bind *bind, jv params, int *error_code) {
    (void)bind;
    jv filter = jv_object_get(jv_copy(params), jv_string("filter"));
    jv input = jv_object_get(jv_copy(params), jv_string("input"));
    if (jv_get_kind(filter) != JV_KIND_STRING || !jv_is_valid(input)) {
        jv_free(filter);
        jv_free(input);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("params.filter must be a string and params.input is required");
    }

    jq_state *jq = jq_init();
    if (jq == NULL) {
        jv_free(filter);
        jv_free(input);
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("could not initialize libjq");
    }
    int compiled = jq_compile(jq, jv_string_value(filter));
    jv_free(filter);
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

static jv tat_handle_shutdown(tat_bind *bind, jv params, int *error_code) {
    (void)params;
    (void)error_code;
    tat_host(bind)->running = false;
    return jv_bool(true);
}

/* Test-only: registers a handle of any type that never reaches native code,
 * so handle validation can be exercised before core functions exist. */
static jv tat_handle_fixture_method(tat_bind *bind, jv params, int *error_code) {
    jv type = jv_object_get(jv_copy(params), jv_string("type"));
    if (jv_get_kind(type) != JV_KIND_STRING) {
        jv_free(type);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("params.type must be a handle type such as \"system\"");
    }
    static const char *const types[] = {"system", "definition", "session_image"};
    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); index += 1) {
        if (strcmp(types[index], jv_string_value(type)) == 0) {
            jv_free(type);
            return tat_handle_fixture(bind, types[index]);
        }
    }
    jv_free(type);
    *error_code = RPC_INVALID_PARAMS;
    return jv_string("params.type is not a handle type");
}

typedef struct {
    jv ids;
    bool runnable_only;
} tat_actor_list;

static ta_result tat_collect_actor(void *self, const ta_actor_inspection *inspection) {
    tat_actor_list *list = self;
    if (!list->runnable_only || inspection->macrostep_in_progress) {
        list->ids = jv_array_append(list->ids, tat_encode_u64(NULL, &inspection->actor));
    }
    return TA_OK;
}

/*
 * Lists the IDs of every resident actor, including children that invokers
 * spawned, so a scenario can run all of them to quiescence. With
 * "runnable": true, only actors with a macrostep in progress are listed.
 * It walks with core's ta_system_visit_actors in one call, from position 0
 * with no limit; its callback cannot cross RPC, so the list is built here.
 */
static jv tat_handle_system_actors(tat_bind *bind, jv params, int *error_code) {
    jv system = jv_object_get(jv_copy(params), jv_string("system"));
    jv runnable = jv_object_get(jv_copy(params), jv_string("runnable"));
    tat_actor_list list = {.ids = jv_array(), .runnable_only = jv_get_kind(runnable) == JV_KIND_TRUE};
    jv_free(runnable);
    void *handle = NULL;
    bool ok = tat_handle_resolve(bind, system, "system", &handle);
    jv_free(system);
    if (!ok) {
        jv_free(list.ids);
        return tat_bind_take_error(bind, error_code);
    }
    ta_result result = ta_system_visit_actors(
        handle, &(ta_actor_visit_params){.inspector = {.self = &list, .visit = tat_collect_actor}});
    jv ids = list.ids;
    if (result != TA_OK) {
        jv_free(ids);
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("ta_system_visit_actors failed");
    }
    return ids;
}

static const tat_method TAT_HOST_METHODS[] = {
    {"system.actors", tat_handle_system_actors,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:system\",\"required\":true},"
     "\"runnable\":{\"type\":\"bool\",\"required\":false}}}"},
    {"rpc.discover", tat_handle_discover, "{\"host\":true,\"params\":{}}"},
    {"system.ping", tat_handle_ping, "{\"host\":true}"},
    {"json.transform", tat_handle_transform,
     "{\"host\":true,\"params\":{\"filter\":{\"type\":\"string\",\"required\":true},"
     "\"input\":{\"type\":\"json\",\"required\":true}}}"},
    {"metrics.snapshot", tat_handle_metrics, "{\"host\":true,\"params\":{}}"},
    {"host.shutdown", tat_handle_shutdown, "{\"host\":true,\"params\":{}}"},
    {"instrument.list", tat_instrument_list_method, "{\"host\":true,\"params\":{}}"},
    {"instrument.journal", tat_instrument_journal_method,
     "{\"host\":true,\"params\":{\"name\":{\"type\":\"string\",\"required\":true},"
     "\"since\":{\"type\":\"u64\",\"required\":false}}}"},
    {"instrument.control", tat_instrument_control_method,
     "{\"host\":true,\"params\":{\"name\":{\"type\":\"string\",\"required\":true},"
     "\"command\":{\"type\":\"string\",\"required\":true},"
     "\"args\":{\"type\":\"json\",\"required\":false}}}"},
    {"handle.fixture", tat_handle_fixture_method,
     "{\"host\":true,\"params\":{\"type\":{\"type\":\"string\",\"required\":true}}}"},
    {"hosted.create", tat_hosted_create_method,
     "{\"host\":true,\"params\":{\"incarnation\":{\"type\":\"u64\",\"required\":false}}}"},
    {"hosted.close", tat_hosted_close_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true}}}"},
    {"hosted.spawn", tat_hosted_spawn_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"definition\":{\"type\":\"u64\",\"required\":true},"
     "\"family\":{\"type\":\"string\",\"required\":false},\"name\":{\"type\":\"string\",\"required\":false}}}"},
    {"hosted.post", tat_hosted_post_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"actor\":{\"type\":\"u64\",\"required\":true},"
     "\"event\":{\"type\":\"string\",\"required\":true}}}"},
    {"hosted.turn", tat_hosted_turn_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"limit\":{\"type\":\"u64\",\"required\":false}}}"},
    {"hosted.advance", tat_hosted_advance_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"time\":{\"type\":\"u64\",\"required\":true}}}"},
    {"hosted.log", tat_hosted_log_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"families\":{\"type\":\"json\",\"required\":true}}}"},
    {"hosted.batches", tat_hosted_batches_method, "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true}}}"},
    {"hosted.evict", tat_hosted_evict_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"actor\":{\"type\":\"u64\",\"required\":true}}}"},
    {"hosted.restore", tat_hosted_restore_method,
     "{\"host\":true,\"params\":{\"system\":{\"type\":\"handle:hosted\",\"required\":true},\"definition\":{\"type\":\"u64\",\"required\":true},"
     "\"family\":{\"type\":\"string\",\"required\":true},\"name\":{\"type\":\"string\",\"required\":true},"
     "\"log\":{\"type\":\"string\",\"required\":true}}}"},
};

static bool tat_register_methods(tat_server_impl *impl, ta_arena *arena) {
    size_t host_count = sizeof(TAT_HOST_METHODS) / sizeof(TAT_HOST_METHODS[0]);
    impl->method_count = host_count + TAT_GENERATED_METHOD_COUNT;
    impl->methods = ta_arena_alloc(arena, impl->method_count * sizeof(*impl->methods),
                                   _Alignof(const tat_method *));
    if (impl->methods == NULL) {
        return false;
    }
    impl->dispatch = jv_object();
    impl->schemas = jv_object();
    for (size_t index = 0; index < impl->method_count; index += 1) {
        const tat_method *method = index < host_count
                                       ? &TAT_HOST_METHODS[index]
                                       : &TAT_GENERATED_METHODS[index - host_count];
        impl->methods[index] = method;
        impl->dispatch = jv_object_set(impl->dispatch, jv_string(method->name),
                                       jv_number((double)index));
        impl->schemas = jv_object_set(impl->schemas, jv_string(method->name),
                                      jv_parse(method->schema));
    }
    return true;
}

static jv tat_dispatch(void *context, const char *method, jv params, int *error_code) {
    tat_server_impl *server = context;
    jv entry = jv_object_get(jv_copy(server->dispatch), jv_string(method));
    if (jv_get_kind(entry) != JV_KIND_NUMBER) {
        jv_free(entry);
        jv_free(params);
        *error_code = RPC_METHOD_NOT_FOUND;
        return jv_string_fmt("unknown method: %s", method);
    }
    size_t selected = (size_t)jv_number_value(entry);
    jv_free(entry);
    if (jv_get_kind(params) == JV_KIND_NULL) {
        jv_free(params);
        params = jv_object();
    }
    if (jv_get_kind(params) != JV_KIND_OBJECT) {
        jv_free(params);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("params must be an object");
    }
    tat_bind_begin(&server->bind);
    tat_value_codec_select(&server->bind, method, params);
    jv result = server->methods[selected]->handler(&server->bind, params, error_code);
    tat_bind_end(&server->bind);
    jv_free(params);
    return result;
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
    *impl = (tat_server_impl){.running = true};
    result = tat_bind_init(&impl->bind, allocator, impl);
    if (result != TA_OK) {
        ta_arena_destroy(&server->arena);
        return result;
    }
    if (!tat_register_methods(impl, &server->arena)) {
        tat_bind_destroy(&impl->bind);
        ta_arena_destroy(&server->arena);
        return TA_ERR_NOMEM;
    }
    server->impl = impl;
    rpc_server_init(&impl->rpc,
                    &(rpc_server_config){
                        .context = impl,
                        .dispatch = tat_dispatch,
                        .output = output,
                    });
    jv ready = jv_object_set(jv_object(), jv_string("transport"), jv_string("stdio"));
    if (!rpc_server_send_notification(&impl->rpc, "host.ready", ready)) {
        tat_server_destroy(server);
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
    jv_free(impl->schemas);
    tat_bind_destroy(&impl->bind);
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
