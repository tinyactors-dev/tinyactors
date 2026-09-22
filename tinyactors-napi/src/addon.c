#include "binding.h"

#include <math.h>
#include <string.h>

#ifndef TINYACTORS_NAPI_VERSION
#define TINYACTORS_NAPI_VERSION "0.0.0"
#endif

typedef struct tinyactors_napi_environment tinyactors_napi_environment;
typedef struct tinyactors_napi_system tinyactors_napi_system;

struct tinyactors_napi_system {
    tinyactors_napi_environment *environment;
    ta_system *core;
    bool closed;
    tinyactors_napi_system *next;
};

struct tinyactors_napi_environment {
    napi_env env;
    napi_ref system_constructor;
    napi_ref error_constructor;
    tinyactors_napi_system *systems;
    bool cleaning;
};

static bool tinyactors_napi_throw_error(napi_env env, const char *code, const char *message) {
    tinyactors_napi_environment *environment = NULL;
    napi_status status = napi_get_instance_data(env, (void **)&environment);
    if (status != napi_ok || environment == NULL || environment->error_constructor == NULL) {
        (void)napi_throw_error(env, NULL, message);
        return false;
    }

    napi_value constructor;
    napi_value message_value;
    napi_value error;
    if (napi_get_reference_value(env, environment->error_constructor, &constructor) != napi_ok ||
        napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
        napi_new_instance(env, constructor, 1, &message_value, &error) != napi_ok) {
        (void)napi_throw_error(env, NULL, message);
        return false;
    }
    napi_value code_value;
    if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) == napi_ok) {
        (void)napi_set_named_property(env, error, "code", code_value);
    }
    (void)napi_throw(env, error);
    return false;
}

static const char *tinyactors_napi_result_code(ta_result result) {
    switch (result) {
    case TA_ERR_INVALID: return "invalid";
    case TA_ERR_NOMEM: return "out-of-memory";
    case TA_ERR_NOT_FOUND: return "not-found";
    case TA_ERR_NOT_RECONSTRUCTIBLE: return "not-reconstructible";
    case TA_ERR_BUSY: return "busy";
    case TA_ERR_UNSUPPORTED: return "unsupported";
    case TA_ERR_CORRUPT: return "corrupt";
    case TA_ERR_DEFINITION_MISMATCH: return "definition-mismatch";
    case TA_ERR_UNLOADED: return "unloaded";
    case TA_ERR_STALE: return "stale";
    default: return "invalid";
    }
}

static bool tinyactors_napi_throw_result(napi_env env, ta_result result, const char *operation) {
    if (result == TA_OK) {
        return true;
    }
    char message[160];
    (void)snprintf(message, sizeof(message), "%s: %s (%d)", operation,
                   tinyactors_napi_result_name(result), (int)result);
    return tinyactors_napi_throw_error(env, tinyactors_napi_result_code(result), message);
}

static bool tinyactors_napi_require_open(napi_env env, tinyactors_napi_system *system,
                                          const char *operation) {
    if (system != NULL && !system->closed && system->core != NULL) {
        return true;
    }
    char message[128];
    (void)snprintf(message, sizeof(message), "%s: system is closed", operation);
    return tinyactors_napi_throw_error(env, "closed", message);
}

static void tinyactors_napi_destroy_core(tinyactors_napi_system *system) {
    if (system != NULL && system->core != NULL) {
        ta_system_destroy(system->core);
        system->core = NULL;
    }
    if (system != NULL) {
        system->closed = true;
    }
}

static void tinyactors_napi_unlink_system(tinyactors_napi_system *system) {
    if (system == NULL || system->environment == NULL) {
        return;
    }
    tinyactors_napi_system **cursor = &system->environment->systems;
    while (*cursor != NULL) {
        if (*cursor == system) {
            *cursor = system->next;
            system->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static void tinyactors_napi_system_finalize(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    tinyactors_napi_system *system = data;
    tinyactors_napi_destroy_core(system);
    tinyactors_napi_unlink_system(system);
    free(system);
}

static void tinyactors_napi_environment_cleanup(void *data) {
    tinyactors_napi_environment *environment = data;
    if (environment == NULL) {
        return;
    }
    environment->cleaning = true;
    for (tinyactors_napi_system *system = environment->systems;
         system != NULL; system = system->next) {
        tinyactors_napi_destroy_core(system);
    }
}

static napi_value tinyactors_napi_error_constructor(napi_env env, napi_callback_info info) {
    napi_value this_value;
    napi_value argv[1];
    size_t argc = 1;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, &this_value, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }

    napi_value name;
    napi_value message;
    if (!tinyactors_napi_check(env,
            napi_create_string_utf8(env, "TinyactorsError", NAPI_AUTO_LENGTH, &name),
            "napi_create_string_utf8") ||
        !tinyactors_napi_check(env,
            napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &message),
            "napi_create_string_utf8")) {
        return NULL;
    }
    if (argc > 0 && argv[0] != NULL) {
        message = argv[0];
    }
    if (!tinyactors_napi_check(env, napi_set_named_property(env, this_value, "name", name),
                               "napi_set_named_property") ||
        !tinyactors_napi_check(env, napi_set_named_property(env, this_value, "message", message),
                               "napi_set_named_property")) {
        return NULL;
    }
    return this_value;
}

static tinyactors_napi_system *tinyactors_napi_unwrap_system(napi_env env, napi_value this_value) {
    tinyactors_napi_system *system = NULL;
    if (napi_unwrap(env, this_value, (void **)&system) != napi_ok || system == NULL) {
        (void)tinyactors_napi_throw_error(env, "invalid", "value is not a Tinyactors System");
        return NULL;
    }
    return system;
}

static napi_value tinyactors_napi_system_close(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_system *system = tinyactors_napi_unwrap_system(env, this_value);
    if (system == NULL) {
        return NULL;
    }
    tinyactors_napi_destroy_core(system);
    return NULL;
}

static napi_value tinyactors_napi_system_closed(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_system *system = tinyactors_napi_unwrap_system(env, this_value);
    if (system == NULL) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_check(env, napi_get_boolean(env, system->closed, &result),
                               "napi_get_boolean")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_system_clock(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }
    if (tinyactors_napi_unwrap_system(env, this_value) == NULL) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_check(env,
            napi_create_string_utf8(env, "manual", NAPI_AUTO_LENGTH, &result),
            "napi_create_string_utf8")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_system_time(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_system *system = tinyactors_napi_unwrap_system(env, this_value);
    if (system == NULL || !tinyactors_napi_require_open(env, system, "System.time")) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_check(env,
            napi_create_double(env, (double)ta_system_time(system->core), &result),
            "napi_create_double")) {
        return NULL;
    }
    return result;
}

static bool tinyactors_napi_read_options(napi_env env, napi_value options, ta_time *initial_time) {
    if (options == NULL || initial_time == NULL) {
        return tinyactors_napi_throw_error(env, "invalid", "invalid System options");
    }

    napi_valuetype type;
    if (!tinyactors_napi_check(env, napi_typeof(env, options, &type), "napi_typeof")) {
        return false;
    }
    if (type == napi_undefined) {
        *initial_time = 0;
        return true;
    }
    if (type != napi_object || options == NULL) {
        return tinyactors_napi_throw_error(env, "invalid", "System options must be an object");
    }

    bool has_clock = false;
    if (!tinyactors_napi_check(env, napi_has_named_property(env, options, "clock", &has_clock),
                               "napi_has_named_property")) {
        return false;
    }
    if (has_clock) {
        napi_value clock;
        char *clock_text = NULL;
        if (!tinyactors_napi_check(env,
                napi_get_named_property(env, options, "clock", &clock),
                "napi_get_named_property") ||
            !tinyactors_napi_get_utf8(env, clock, &clock_text, NULL)) {
            return false;
        }
        bool manual = strcmp(clock_text, "manual") == 0;
        bool realtime = strcmp(clock_text, "realtime") == 0;
        free(clock_text);
        if (!manual) {
            if (realtime) {
                return tinyactors_napi_throw_error(env, "unsupported",
                                                   "realtime clock is not implemented yet");
            }
            return tinyactors_napi_throw_error(env, "invalid", "clock must be manual");
        }
    }

    *initial_time = 0;
    bool has_initial_time = false;
    if (!tinyactors_napi_check(env,
            napi_has_named_property(env, options, "initialTime", &has_initial_time),
            "napi_has_named_property")) {
        return false;
    }
    if (!has_initial_time) {
        return true;
    }
    napi_value value;
    if (!tinyactors_napi_check(env,
            napi_get_named_property(env, options, "initialTime", &value),
            "napi_get_named_property")) {
        return false;
    }
    if (!tinyactors_napi_check(env, napi_typeof(env, value, &type), "napi_typeof")) {
        return false;
    }
    if (type == napi_undefined) {
        return true;
    }
    if (type != napi_number) {
        return tinyactors_napi_throw_error(env, "invalid", "initialTime must be a non-negative integer");
    }
    double number;
    if (!tinyactors_napi_check(env, napi_get_value_double(env, value, &number),
                               "napi_get_value_double")) {
        return false;
    }
    if (!isfinite(number) || number < 0 || floor(number) != number ||
        number > 9007199254740991.0) {
        return tinyactors_napi_throw_error(env, "invalid", "initialTime must be a safe non-negative integer");
    }
    *initial_time = (ta_time)number;
    return true;
}

static napi_value tinyactors_napi_system_constructor(napi_env env, napi_callback_info info) {
    napi_value this_value;
    napi_value argv[1];
    size_t argc = 1;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, &this_value, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }
    if (argc != 1) {
        (void)tinyactors_napi_throw_error(env, "invalid", "System cannot be constructed directly");
        return NULL;
    }
    tinyactors_napi_system *system = NULL;
    if (napi_get_value_external(env, argv[0], (void **)&system) != napi_ok || system == NULL) {
        (void)tinyactors_napi_throw_error(env, "invalid", "invalid native System handle");
        return NULL;
    }
    if (!tinyactors_napi_check(env,
            napi_wrap(env, this_value, system, tinyactors_napi_system_finalize, NULL, NULL),
            "napi_wrap")) {
        return NULL;
    }
    return this_value;
}

static napi_value tinyactors_napi_create_system(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t argc = 1;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, NULL, NULL),
            "napi_get_cb_info")) {
        return NULL;
    }
    ta_time initial_time = 0;
    if (argc > 0 && !tinyactors_napi_read_options(env, argv[0], &initial_time)) {
        return NULL;
    }

    tinyactors_napi_environment *environment = NULL;
    if (!tinyactors_napi_check(env,
            napi_get_instance_data(env, (void **)&environment),
            "napi_get_instance_data") || environment == NULL) {
        return NULL;
    }
    tinyactors_napi_system *system = calloc(1, sizeof(*system));
    if (system == NULL) {
        (void)tinyactors_napi_throw_error(env, "out-of-memory", "out of memory creating System");
        return NULL;
    }
    ta_system_config config = {
        .allocator = tinyactors_napi_allocator(),
        .initial_time = initial_time,
    };
    ta_result result = ta_system_create(&config, &system->core);
    if (!tinyactors_napi_throw_result(env, result, "ta_system_create")) {
        free(system);
        return NULL;
    }
    system->environment = environment;
    system->next = environment->systems;
    environment->systems = system;

    napi_value constructor;
    napi_value external;
    napi_value instance;
    if (!tinyactors_napi_check(env,
            napi_get_reference_value(env, environment->system_constructor, &constructor),
            "napi_get_reference_value") ||
        !tinyactors_napi_check(env,
            napi_create_external(env, system, NULL, NULL, &external),
            "napi_create_external") ||
        !tinyactors_napi_check(env,
            napi_new_instance(env, constructor, 1, &external, &instance),
            "napi_new_instance")) {
        tinyactors_napi_destroy_core(system);
        tinyactors_napi_unlink_system(system);
        free(system);
        return NULL;
    }
    return instance;
}

static napi_value tinyactors_napi_init(napi_env env, napi_value exports) {
    tinyactors_napi_environment *environment = calloc(1, sizeof(*environment));
    if (environment == NULL) {
        (void)napi_throw_error(env, NULL, "out of memory initializing tinyactors");
        return NULL;
    }
    environment->env = env;
    if (!tinyactors_napi_check(env,
            napi_set_instance_data(env, environment, NULL, NULL),
            "napi_set_instance_data") ||
        !tinyactors_napi_check(env,
            napi_add_env_cleanup_hook(env, tinyactors_napi_environment_cleanup, environment),
            "napi_add_env_cleanup_hook")) {
        free(environment);
        return NULL;
    }

    napi_value error_constructor;
    if (!tinyactors_napi_check(env,
            napi_define_class(env, "TinyactorsError", NAPI_AUTO_LENGTH,
                              tinyactors_napi_error_constructor, NULL, 0, NULL,
                              &error_constructor),
            "napi_define_class")) {
        return NULL;
    }
    napi_value error_prototype;
    napi_value global;
    napi_value error_function;
    napi_value error_base_prototype;
    napi_value prototype_name;
    if (!tinyactors_napi_check(env, napi_create_string_utf8(env, "__proto__", NAPI_AUTO_LENGTH,
                                                             &prototype_name),
                               "napi_create_string_utf8") ||
        !tinyactors_napi_check(env, napi_get_named_property(env, error_constructor, "prototype",
                                                             &error_prototype),
                               "napi_get_named_property") ||
        !tinyactors_napi_check(env, napi_get_global(env, &global), "napi_get_global") ||
        !tinyactors_napi_check(env, napi_get_named_property(env, global, "Error", &error_function),
                               "napi_get_named_property") ||
        !tinyactors_napi_check(env, napi_get_named_property(env, error_function, "prototype",
                                                             &error_base_prototype),
                               "napi_get_named_property") ||
        !tinyactors_napi_check(env, napi_set_property(env, error_prototype, prototype_name,
                                                      error_base_prototype),
                               "napi_set_property") ||
        !tinyactors_napi_check(env,
            napi_create_reference(env, error_constructor, 1, &environment->error_constructor),
            "napi_create_reference")) {
        return NULL;
    }

    napi_property_descriptor system_properties[] = {
        { .utf8name = "close", .method = tinyactors_napi_system_close,
          .attributes = napi_writable | napi_configurable },
        { .utf8name = "closed", .getter = tinyactors_napi_system_closed,
          .attributes = napi_enumerable },
        { .utf8name = "clock", .getter = tinyactors_napi_system_clock,
          .attributes = napi_enumerable },
        { .utf8name = "time", .getter = tinyactors_napi_system_time,
          .attributes = napi_enumerable },
    };
    napi_value system_constructor;
    if (!tinyactors_napi_check(env,
            napi_define_class(env, "System", NAPI_AUTO_LENGTH,
                              tinyactors_napi_system_constructor, NULL,
                              sizeof(system_properties) / sizeof(system_properties[0]),
                              system_properties, &system_constructor),
            "napi_define_class") ||
        !tinyactors_napi_check(env,
            napi_create_reference(env, system_constructor, 1, &environment->system_constructor),
            "napi_create_reference")) {
        return NULL;
    }

    napi_value create_system;
    if (!tinyactors_napi_check(env,
            napi_create_function(env, "createSystem", NAPI_AUTO_LENGTH,
                                 tinyactors_napi_create_system, environment, &create_system),
            "napi_create_function")) {
        return NULL;
    }
    napi_value version;
    if (!tinyactors_napi_check(env,
            napi_create_string_utf8(env, TINYACTORS_NAPI_VERSION, NAPI_AUTO_LENGTH, &version),
            "napi_create_string_utf8") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, exports, "version", version),
            "napi_set_named_property") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, exports, "createSystem", create_system),
            "napi_set_named_property") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, exports, "TinyactorsError", error_constructor),
            "napi_set_named_property")) {
        return NULL;
    }
    return exports;
}

NAPI_MODULE(tinyactors_napi, tinyactors_napi_init)
