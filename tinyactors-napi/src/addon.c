#include "binding.h"
#include "null_datamodel.h"

#include <math.h>
#include <string.h>

#ifndef TINYACTORS_NAPI_VERSION
#define TINYACTORS_NAPI_VERSION "0.0.0"
#endif

typedef struct tinyactors_napi_environment tinyactors_napi_environment;
typedef struct tinyactors_napi_system tinyactors_napi_system;
typedef struct tinyactors_napi_definition tinyactors_napi_definition;
typedef struct tinyactors_napi_actor tinyactors_napi_actor;

struct tinyactors_napi_definition {
    tinyactors_napi_system *system;
    napi_ref system_ref;
    ta_definition_id id;
    char *family;
    uint32_t family_len;
    uint8_t *revision;
    uint32_t revision_len;
    bool revision_is_string;
    bool retired;
};

struct tinyactors_napi_actor {
    tinyactors_napi_system *system;
    tinyactors_napi_definition *definition;
    napi_ref system_ref;
    napi_ref definition_ref;
    ta_actor_id id;
    bool destroyed;
};

struct tinyactors_napi_system {
    tinyactors_napi_environment *environment;
    ta_system *core;
    bool closed;
    ta_actor_id *runnable;
    size_t runnable_count;
    size_t runnable_head;
    size_t runnable_capacity;
    tinyactors_napi_system *next;
};

struct tinyactors_napi_environment {
    napi_env env;
    napi_ref system_constructor;
    napi_ref definition_constructor;
    napi_ref actor_constructor;
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
        free(system->runnable);
        system->runnable = NULL;
        system->runnable_count = 0;
        system->runnable_head = 0;
        system->runnable_capacity = 0;
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

#define TINYACTORS_NAPI_RESUME_BUDGET 1024u

static bool tinyactors_napi_runnable_enqueue(napi_env env, tinyactors_napi_system *system,
                                             ta_actor_id id) {
    if (system->runnable_head == system->runnable_count) {
        system->runnable_head = 0;
        system->runnable_count = 0;
    } else if (system->runnable_head != 0 &&
               system->runnable_count == system->runnable_capacity) {
        size_t pending = system->runnable_count - system->runnable_head;
        memmove(system->runnable, system->runnable + system->runnable_head,
                pending * sizeof(*system->runnable));
        system->runnable_count = pending;
        system->runnable_head = 0;
    }
    if (system->runnable_count == system->runnable_capacity) {
        size_t capacity = system->runnable_capacity == 0 ? 16u : system->runnable_capacity * 2u;
        ta_actor_id *runnable = realloc(system->runnable, capacity * sizeof(*runnable));
        if (runnable == NULL) {
            (void)tinyactors_napi_throw_error(env, "out-of-memory",
                                               "out of memory growing runnable queue");
            return false;
        }
        system->runnable = runnable;
        system->runnable_capacity = capacity;
    }
    system->runnable[system->runnable_count++] = id;
    return true;
}

static void tinyactors_napi_runnable_remove(tinyactors_napi_system *system, ta_actor_id id) {
    size_t write = system->runnable_head;
    for (size_t read = system->runnable_head; read < system->runnable_count; read += 1u) {
        if (system->runnable[read] != id) {
            system->runnable[write++] = system->runnable[read];
        }
    }
    system->runnable_count = write;
    if (system->runnable_head == system->runnable_count) {
        system->runnable_head = 0;
        system->runnable_count = 0;
    }
}

/* Run one finite turn. A paused actor is put back on the queue for a later turn. */
static ta_result tinyactors_napi_pump_runnable(napi_env env, tinyactors_napi_system *system) {
    size_t turn_count = system->runnable_count - system->runnable_head;
    for (size_t turn = 0; turn < turn_count; turn += 1u) {
        ta_actor_id id = system->runnable[system->runnable_head++];
        ta_run_status status = TA_RUN_ERROR;
        ta_result result = ta_actor_resume(system->core,
            &(ta_actor_resume_params){.actor = id,
                                      .budget = TINYACTORS_NAPI_RESUME_BUDGET,
                                      .status = &status});
        if (result != TA_OK) {
            return result;
        }
        if (status == TA_RUN_PAUSED) {
            if (!tinyactors_napi_runnable_enqueue(env, system, id)) {
                return TA_ERR_NOMEM;
            }
        }
    }
    if (system->runnable_head == system->runnable_count) {
        system->runnable_head = 0;
        system->runnable_count = 0;
    }
    return TA_OK;
}

typedef struct {
    ta_scxml_spec spec;
    ta_definition_identity identity;
    char *family;
    uint8_t *revision;
    bool revision_is_string;
    ta_scxml_initial_spec initial;
    ta_scxml_state_spec *states;
    uint32_t state_count;
} tinyactors_napi_spec;

static bool tinyactors_napi_is_object(napi_env env, napi_value value, const char *what) {
    napi_valuetype type;
    if (value == NULL || !tinyactors_napi_check(env, napi_typeof(env, value, &type),
                                                 "napi_typeof") ||
        type != napi_object) {
        return tinyactors_napi_throw_error(env, "invalid", what);
    }
    if (type == napi_null) {
        return tinyactors_napi_throw_error(env, "invalid", what);
    }
    return true;
}

static bool tinyactors_napi_is_array(napi_env env, napi_value value, const char *what) {
    bool is_array = false;
    if (!tinyactors_napi_check(env, napi_is_array(env, value, &is_array), "napi_is_array") ||
        !is_array) {
        return tinyactors_napi_throw_error(env, "invalid", what);
    }
    return true;
}

static bool tinyactors_napi_has(napi_env env, napi_value object, const char *name,
                                bool *out) {
    return tinyactors_napi_check(env, napi_has_named_property(env, object, name, out),
                                 "napi_has_named_property");
}

static bool tinyactors_napi_get(napi_env env, napi_value object, const char *name,
                                napi_value *out) {
    return tinyactors_napi_check(env, napi_get_named_property(env, object, name, out),
                                 "napi_get_named_property");
}

static bool tinyactors_napi_allowed_key(const char *name, const char *const *allowed,
                                        size_t count) {
    for (size_t index = 0; index < count; index += 1) {
        if (strcmp(name, allowed[index]) == 0) {
            return true;
        }
    }
    return false;
}

/* Reject fields that this deliberately small native decoder cannot implement. */
static bool tinyactors_napi_check_keys(napi_env env, napi_value object,
                                       const char *const *allowed, size_t count,
                                       const char *what) {
    napi_value names;
    if (!tinyactors_napi_check(env, napi_get_property_names(env, object, &names),
                               "napi_get_property_names")) {
        return false;
    }
    uint32_t length = 0;
    if (!tinyactors_napi_check(env, napi_get_array_length(env, names, &length),
                               "napi_get_array_length")) {
        return false;
    }
    for (uint32_t index = 0; index < length; index += 1) {
        napi_value key;
        char *text = NULL;
        if (!tinyactors_napi_check(env, napi_get_element(env, names, index, &key),
                                   "napi_get_element") ||
            !tinyactors_napi_get_utf8(env, key, &text, NULL)) {
            free(text);
            return false;
        }
        bool allowed_key = tinyactors_napi_allowed_key(text, allowed, count);
        free(text);
        if (!allowed_key) {
            char message[160];
            (void)snprintf(message, sizeof(message), "%s contains unsupported field", what);
            return tinyactors_napi_throw_error(env, "unsupported", message);
        }
    }
    return true;
}

static bool tinyactors_napi_copy_string(napi_env env, napi_value value, const char *what,
                                        ta_str *out, char **storage, bool nonempty) {
    napi_valuetype type;
    if (!tinyactors_napi_check(env, napi_typeof(env, value, &type), "napi_typeof") ||
        type != napi_string) {
        return tinyactors_napi_throw_error(env, "invalid", what);
    }
    size_t length = 0;
    char *text = NULL;
    if (!tinyactors_napi_get_utf8(env, value, &text, &length)) {
        return false;
    }
    if (nonempty && length == 0) {
        free(text);
        return tinyactors_napi_throw_error(env, "invalid", what);
    }
    *out = (ta_str){.ptr = text, .len = (uint32_t)length};
    if (storage != NULL) {
        *storage = text;
    }
    return true;
}

static bool tinyactors_napi_copy_revision(napi_env env, napi_value value,
                                          uint8_t **out, uint32_t *length) {
    napi_valuetype type;
    if (!tinyactors_napi_check(env, napi_typeof(env, value, &type), "napi_typeof")) {
        return false;
    }
    if (type == napi_string) {
        char *text = NULL;
        size_t size = 0;
        if (!tinyactors_napi_get_utf8(env, value, &text, &size)) {
            return false;
        }
        *out = (uint8_t *)text;
        *length = (uint32_t)size;
        return true;
    }
    bool is_typedarray = false;
    if (!tinyactors_napi_check(env, napi_is_typedarray(env, value, &is_typedarray),
                               "napi_is_typedarray") || !is_typedarray) {
        return tinyactors_napi_throw_error(env, "invalid",
                                           "revision must be a string or Uint8Array");
    }
    napi_typedarray_type kind;
    size_t size = 0;
    void *data = NULL;
    napi_value arraybuffer;
    size_t offset = 0;
    if (!tinyactors_napi_check(env,
            napi_get_typedarray_info(env, value, &kind, &size, &data, &arraybuffer, &offset),
            "napi_get_typedarray_info") || kind != napi_uint8_array) {
        return tinyactors_napi_throw_error(env, "invalid", "revision must be a Uint8Array");
    }
    if (size > UINT32_MAX) {
        return tinyactors_napi_throw_error(env, "invalid", "revision is too large");
    }
    uint8_t *copy = NULL;
    if (size != 0) {
        copy = malloc(size);
        if (copy == NULL) {
            return tinyactors_napi_throw_error(env, "out-of-memory", "out of memory copying revision");
        }
        memcpy(copy, data, size);
    }
    *out = copy;
    *length = (uint32_t)size;
    return true;
}

static void tinyactors_napi_free_targets(ta_str *targets) {
    if (targets == NULL) {
        return;
    }
    for (ta_str *target = targets; target->ptr != NULL; target += 1) {
        free((void *)target->ptr);
    }
    free(targets);
}

static bool tinyactors_napi_copy_targets(napi_env env, napi_value value, const char *what,
                                         ta_str **out) {
    bool is_array = false;
    if (!tinyactors_napi_check(env, napi_is_array(env, value, &is_array), "napi_is_array")) {
        return false;
    }
    uint32_t length = 0;
    if (is_array) {
        if (!tinyactors_napi_check(env, napi_get_array_length(env, value, &length),
                                   "napi_get_array_length") || length == 0) {
            return tinyactors_napi_throw_error(env, "invalid", "target array must not be empty");
        }
    } else {
        length = 1;
    }
    ta_str *targets = calloc((size_t)length + 1u, sizeof(*targets));
    if (targets == NULL) {
        return tinyactors_napi_throw_error(env, "out-of-memory", "out of memory copying targets");
    }
    for (uint32_t index = 0; index < length; index += 1) {
        napi_value item = value;
        if (is_array && !tinyactors_napi_check(env,
                napi_get_element(env, value, index, &item), "napi_get_element")) {
            tinyactors_napi_free_targets(targets);
            return false;
        }
        if (!tinyactors_napi_copy_string(env, item, what, &targets[index], NULL, true)) {
            tinyactors_napi_free_targets(targets);
            return false;
        }
    }
    *out = targets;
    return true;
}

static bool tinyactors_napi_parse_transition(napi_env env, napi_value value,
                                             ta_scxml_transition_spec *transition) {
    static const char *const keys[] = {"event", "kind", "target"};
    if (!tinyactors_napi_is_object(env, value, "transition must be an object") ||
        !tinyactors_napi_check_keys(env, value, keys, sizeof(keys) / sizeof(keys[0]),
                                    "transition")) {
        return false;
    }
    bool has_event = false;
    bool has_target = false;
    if (!tinyactors_napi_has(env, value, "event", &has_event) ||
        !tinyactors_napi_has(env, value, "target", &has_target) || !has_event || !has_target) {
        return tinyactors_napi_throw_error(env, "invalid",
                                           "event transition requires event and target");
    }
    napi_value event;
    napi_value target;
    if (!tinyactors_napi_get(env, value, "event", &event) ||
        !tinyactors_napi_get(env, value, "target", &target) ||
        !tinyactors_napi_copy_string(env, event, "transition event must be a string",
                                     &transition->event, NULL, true) ||
        !tinyactors_napi_copy_targets(env, target, "transition target must be a string",
                                      (ta_str **)&transition->targets)) {
        return false;
    }
    transition->kind = TA_SCXML_TRANSITION_EXTERNAL;
    bool has_kind = false;
    if (!tinyactors_napi_has(env, value, "kind", &has_kind)) {
        return false;
    }
    if (has_kind) {
        napi_value kind;
        ta_str kind_text;
        if (!tinyactors_napi_get(env, value, "kind", &kind) ||
            !tinyactors_napi_copy_string(env, kind,
                                         "transition kind must be a string", &kind_text, NULL,
                                         true)) {
            return false;
        }
        if (strncmp(kind_text.ptr, "external", kind_text.len) == 0 &&
            kind_text.len == strlen("external")) {
            transition->kind = TA_SCXML_TRANSITION_EXTERNAL;
        } else if (strncmp(kind_text.ptr, "internal", kind_text.len) == 0 &&
                   kind_text.len == strlen("internal")) {
            transition->kind = TA_SCXML_TRANSITION_INTERNAL;
        } else {
            free((void *)kind_text.ptr);
            return tinyactors_napi_throw_error(env, "invalid",
                                               "transition kind must be external or internal");
        }
        free((void *)kind_text.ptr);
    }
    return true;
}

static void tinyactors_napi_free_spec(tinyactors_napi_spec *parsed) {
    if (parsed == NULL) {
        return;
    }
    if (parsed->states != NULL) {
        for (uint32_t state_index = 0; state_index < parsed->state_count; state_index += 1) {
            ta_scxml_state_spec *state = &parsed->states[state_index];
            free((void *)state->id.ptr);
            if (state->transitions != NULL) {
                for (const ta_scxml_transition_spec *transition = state->transitions;
                     transition->event.ptr != NULL; transition += 1) {
                    free((void *)transition->event.ptr);
                    tinyactors_napi_free_targets((ta_str *)transition->targets);
                }
                free((void *)state->transitions);
            }
        }
        free(parsed->states);
    }
    tinyactors_napi_free_targets((ta_str *)parsed->initial.targets);
    free(parsed->family);
    free(parsed->revision);
    *parsed = (tinyactors_napi_spec){0};
}

static bool tinyactors_napi_parse_spec(napi_env env, napi_value value,
                                       tinyactors_napi_spec *parsed) {
    static const char *const keys[] = {"family", "revision", "initial", "states"};
    napi_valuetype type;
    if (parsed == NULL || !tinyactors_napi_is_object(env, value, "definition must be an object") ||
        !tinyactors_napi_check_keys(env, value, keys, sizeof(keys) / sizeof(keys[0]),
                                    "definition")) {
        return false;
    }
    *parsed = (tinyactors_napi_spec){0};
    bool has_family = false;
    bool has_revision = false;
    bool has_states = false;
    if (!tinyactors_napi_has(env, value, "family", &has_family) ||
        !tinyactors_napi_has(env, value, "revision", &has_revision) ||
        !tinyactors_napi_has(env, value, "states", &has_states) ||
        !has_family || !has_revision || !has_states) {
        return tinyactors_napi_throw_error(env, "invalid",
                                           "definition requires family, revision, and states");
    }
    napi_value family;
    napi_value revision;
    napi_value states;
    if (!tinyactors_napi_get(env, value, "family", &family) ||
        !tinyactors_napi_get(env, value, "revision", &revision) ||
        !tinyactors_napi_get(env, value, "states", &states) ||
        !tinyactors_napi_copy_string(env, family, "family must be a non-empty string",
                                     &(ta_str){0}, &parsed->family, true) ||
        !tinyactors_napi_check(env, napi_typeof(env, revision, &type), "napi_typeof") ||
        !tinyactors_napi_copy_revision(env, revision, &parsed->revision,
                                       &parsed->identity.revision.len) ||
        !tinyactors_napi_is_array(env, states, "states must be an array")) {
        tinyactors_napi_free_spec(parsed);
        return false;
    }
    parsed->revision_is_string = type == napi_string;
    parsed->identity.family.name = (ta_str){.ptr = parsed->family,
                                             .len = (uint32_t)strlen(parsed->family)};
    parsed->identity.revision.ptr = parsed->revision;
    uint32_t state_count = 0;
    if (!tinyactors_napi_check(env, napi_get_array_length(env, states, &state_count),
                               "napi_get_array_length") || state_count == 0) {
        tinyactors_napi_free_spec(parsed);
        return tinyactors_napi_throw_error(env, "invalid", "states must not be empty");
    }
    parsed->states = calloc((size_t)state_count + 1u, sizeof(*parsed->states));
    if (parsed->states == NULL) {
        tinyactors_napi_free_spec(parsed);
        return tinyactors_napi_throw_error(env, "out-of-memory", "out of memory copying states");
    }
    parsed->state_count = state_count;
    for (uint32_t state_index = 0; state_index < state_count; state_index += 1) {
        napi_value state_value;
        if (!tinyactors_napi_check(env,
                napi_get_element(env, states, state_index, &state_value), "napi_get_element")) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
        static const char *const state_keys[] = {"id", "kind", "transitions"};
        if (!tinyactors_napi_is_object(env, state_value, "state must be an object") ||
            !tinyactors_napi_check_keys(env, state_value, state_keys,
                                        sizeof(state_keys) / sizeof(state_keys[0]), "state")) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
        bool has_id = false;
        if (!tinyactors_napi_has(env, state_value, "id", &has_id) || !has_id) {
            tinyactors_napi_free_spec(parsed);
            return tinyactors_napi_throw_error(env, "invalid", "state requires id");
        }
        napi_value id;
        if (!tinyactors_napi_get(env, state_value, "id", &id) ||
            !tinyactors_napi_copy_string(env, id, "state id must be a non-empty string",
                                         &parsed->states[state_index].id, NULL, true)) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
        parsed->states[state_index].kind = TA_SCXML_STATE_ATOMIC;
        bool has_kind = false;
        if (!tinyactors_napi_has(env, state_value, "kind", &has_kind)) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
        if (has_kind) {
            napi_value kind;
            ta_str kind_text;
            if (!tinyactors_napi_get(env, state_value, "kind", &kind) ||
                !tinyactors_napi_copy_string(env, kind,
                                             "state kind must be a string", &kind_text, NULL,
                                             true)) {
                tinyactors_napi_free_spec(parsed);
                return false;
            }
            if (strncmp(kind_text.ptr, "atomic", kind_text.len) == 0 &&
                kind_text.len == strlen("atomic")) {
                parsed->states[state_index].kind = TA_SCXML_STATE_ATOMIC;
            } else if (strncmp(kind_text.ptr, "final", kind_text.len) == 0 &&
                       kind_text.len == strlen("final")) {
                parsed->states[state_index].kind = TA_SCXML_STATE_FINAL;
            } else {
                free((void *)kind_text.ptr);
                tinyactors_napi_free_spec(parsed);
                return tinyactors_napi_throw_error(env, "unsupported",
                                                   "only atomic and final states are supported");
            }
            free((void *)kind_text.ptr);
        }
        bool has_transitions = false;
        if (!tinyactors_napi_has(env, state_value, "transitions", &has_transitions)) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
        if (has_transitions) {
            if (parsed->states[state_index].kind == TA_SCXML_STATE_FINAL) {
                tinyactors_napi_free_spec(parsed);
                return tinyactors_napi_throw_error(env, "invalid",
                                                   "final states cannot have transitions");
            }
            napi_value transitions;
            uint32_t transition_count = 0;
            if (!tinyactors_napi_get(env, state_value, "transitions", &transitions) ||
                !tinyactors_napi_is_array(env, transitions, "transitions must be an array") ||
                !tinyactors_napi_check(env,
                    napi_get_array_length(env, transitions, &transition_count),
                    "napi_get_array_length")) {
                tinyactors_napi_free_spec(parsed);
                return false;
            }
            parsed->states[state_index].transitions =
                calloc((size_t)transition_count + 1u, sizeof(ta_scxml_transition_spec));
            if (parsed->states[state_index].transitions == NULL) {
                tinyactors_napi_free_spec(parsed);
                return tinyactors_napi_throw_error(env, "out-of-memory",
                                                   "out of memory copying transitions");
            }
            for (uint32_t transition_index = 0; transition_index < transition_count;
                 transition_index += 1) {
                napi_value transition;
                if (!tinyactors_napi_check(env,
                        napi_get_element(env, transitions, transition_index, &transition),
                        "napi_get_element") ||
                    !tinyactors_napi_parse_transition(
                        env, transition,
                        (ta_scxml_transition_spec *)&parsed->states[state_index]
                            .transitions[transition_index])) {
                    tinyactors_napi_free_spec(parsed);
                    return false;
                }
            }
        }
    }
    bool has_initial = false;
    if (!tinyactors_napi_has(env, value, "initial", &has_initial)) {
        tinyactors_napi_free_spec(parsed);
        return false;
    }
    if (has_initial) {
        napi_value initial;
        static const char *const initial_keys[] = {"target"};
        if (!tinyactors_napi_get(env, value, "initial", &initial) ||
            !tinyactors_napi_is_object(env, initial, "initial must be an object") ||
            !tinyactors_napi_check_keys(env, initial, initial_keys,
                                        sizeof(initial_keys) / sizeof(initial_keys[0]), "initial")) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
        bool has_target = false;
        napi_value target;
        if (!tinyactors_napi_has(env, initial, "target", &has_target) || !has_target ||
            !tinyactors_napi_get(env, initial, "target", &target) ||
            !tinyactors_napi_copy_targets(env, target, "initial target must be a string",
                                           (ta_str **)&parsed->initial.targets)) {
            tinyactors_napi_free_spec(parsed);
            return false;
        }
    }
    parsed->spec = (ta_scxml_spec){
        .datamodel = {.ptr = "null", .len = 4},
        .identity = &parsed->identity,
        .initial = has_initial ? &parsed->initial : NULL,
        .states = parsed->states,
    };
    return true;
}

static tinyactors_napi_definition *tinyactors_napi_unwrap_definition(
    napi_env env, napi_value this_value) {
    tinyactors_napi_definition *definition = NULL;
    if (napi_unwrap(env, this_value, (void **)&definition) != napi_ok || definition == NULL) {
        (void)tinyactors_napi_throw_error(env, "invalid", "value is not a Tinyactors Definition");
        return NULL;
    }
    return definition;
}

static void tinyactors_napi_definition_finalize(napi_env env, void *data, void *hint);

static napi_value tinyactors_napi_definition_constructor(napi_env env,
                                                         napi_callback_info info) {
    napi_value this_value;
    napi_value argv[2];
    size_t argc = 2;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    if (argc != 2) {
        (void)tinyactors_napi_throw_error(env, "invalid",
                                           "Definition cannot be constructed directly");
        return NULL;
    }
    tinyactors_napi_definition *definition = NULL;
    if (napi_get_value_external(env, argv[0], (void **)&definition) != napi_ok ||
        definition == NULL || !tinyactors_napi_is_object(env, argv[1], "invalid system")) {
        (void)tinyactors_napi_throw_error(env, "invalid", "invalid native Definition handle");
        return NULL;
    }
    if (!tinyactors_napi_check(env,
            napi_create_reference(env, argv[1], 1, &definition->system_ref),
            "napi_create_reference") ||
        !tinyactors_napi_check(env,
            napi_wrap(env, this_value, definition, tinyactors_napi_definition_finalize, NULL,
                      NULL), "napi_wrap")) {
        if (definition->system_ref != NULL) {
            (void)napi_delete_reference(env, definition->system_ref);
            definition->system_ref = NULL;
        }
        return NULL;
    }
    return this_value;
}

static void tinyactors_napi_definition_finalize(napi_env env, void *data, void *hint) {
    (void)hint;
    tinyactors_napi_definition *definition = data;
    if (definition == NULL) {
        return;
    }
    if (definition->system_ref != NULL) {
        (void)napi_delete_reference(env, definition->system_ref);
    }
    free(definition->family);
    free(definition->revision);
    free(definition);
}

static napi_value tinyactors_napi_definition_system(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_definition *definition = tinyactors_napi_unwrap_definition(env, this_value);
    if (definition == NULL || definition->system_ref == NULL) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_check(env,
            napi_get_reference_value(env, definition->system_ref, &result),
            "napi_get_reference_value")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_definition_id(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_definition *definition = tinyactors_napi_unwrap_definition(env, this_value);
    if (definition == NULL) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_create_uint64(env, definition->id, &result)) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_definition_identity(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_definition *definition = tinyactors_napi_unwrap_definition(env, this_value);
    if (definition == NULL) {
        return NULL;
    }
    napi_value identity;
    napi_value family;
    napi_value revision;
    napi_value revision_buffer = NULL;
    void *revision_data = NULL;
    if (!tinyactors_napi_check(env, napi_create_object(env, &identity), "napi_create_object") ||
        !tinyactors_napi_check(env,
            napi_create_string_utf8(env, definition->family, definition->family_len,
                                    &family), "napi_create_string_utf8") ||
        !tinyactors_napi_check(env,
            definition->revision_is_string
                ? napi_create_string_utf8(env, (const char *)definition->revision,
                                          definition->revision_len, &revision)
                : napi_create_arraybuffer(env, definition->revision_len, &revision_data,
                                          &revision_buffer),
            "napi_create_revision")) {
        return NULL;
    }
    if (!definition->revision_is_string) {
        if (!tinyactors_napi_check(env,
                napi_create_typedarray(env, napi_uint8_array, definition->revision_len,
                                       revision_buffer, 0, &revision),
                "napi_create_typedarray") ||
            (definition->revision_len != 0 && revision_data == NULL)) {
            return NULL;
        }
        if (definition->revision_len != 0) {
            memcpy(revision_data, definition->revision, definition->revision_len);
        }
    }
    if (!tinyactors_napi_check(env,
            napi_set_named_property(env, identity, "family", family),
            "napi_set_named_property") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, identity, "revision", revision),
            "napi_set_named_property")) {
        return NULL;
    }
    return identity;
}

static napi_value tinyactors_napi_definition_retired(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_definition *definition = tinyactors_napi_unwrap_definition(env, this_value);
    if (definition == NULL ||
        !tinyactors_napi_require_open(env, definition->system, "Definition.retired")) {
        return NULL;
    }
    bool retired = definition->retired;
    if (!retired) {
        ta_definition_inspection inspection;
        if (!tinyactors_napi_throw_result(env,
                ta_definition_inspect(definition->system->core, definition->id, &inspection),
                "ta_definition_inspect")) {
            return NULL;
        }
        retired = inspection.retired;
    }
    napi_value result;
    if (!tinyactors_napi_check(env, napi_get_boolean(env, retired, &result),
                               "napi_get_boolean")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_definition_retire(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_definition *definition = tinyactors_napi_unwrap_definition(env, this_value);
    if (definition == NULL ||
        !tinyactors_napi_require_open(env, definition->system, "Definition.retire")) {
        return NULL;
    }
    if (!tinyactors_napi_throw_result(env,
            ta_system_retire_definition(definition->system->core, definition->id),
            "ta_system_retire_definition")) {
        return NULL;
    }
    definition->retired = true;
    return NULL;
}

static void tinyactors_napi_actor_finalize(napi_env env, void *data, void *hint) {
    (void)hint;
    tinyactors_napi_actor *actor = data;
    if (actor == NULL) {
        return;
    }
    if (actor->system_ref != NULL) {
        (void)napi_delete_reference(env, actor->system_ref);
    }
    if (actor->definition_ref != NULL) {
        (void)napi_delete_reference(env, actor->definition_ref);
    }
    free(actor);
}

static tinyactors_napi_actor *tinyactors_napi_unwrap_actor(napi_env env,
                                                            napi_value this_value) {
    tinyactors_napi_actor *actor = NULL;
    if (napi_unwrap(env, this_value, (void **)&actor) != napi_ok || actor == NULL) {
        (void)tinyactors_napi_throw_error(env, "invalid", "value is not a Tinyactors Actor");
        return NULL;
    }
    return actor;
}

static bool tinyactors_napi_require_actor_open(napi_env env, tinyactors_napi_actor *actor,
                                                const char *operation) {
    return actor != NULL && tinyactors_napi_require_open(env, actor->system, operation);
}

static napi_value tinyactors_napi_actor_constructor(napi_env env, napi_callback_info info) {
    napi_value this_value;
    napi_value argv[3];
    size_t argc = 3;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    if (argc != 3) {
        (void)tinyactors_napi_throw_error(env, "invalid",
                                           "Actor cannot be constructed directly");
        return NULL;
    }
    tinyactors_napi_actor *actor = NULL;
    tinyactors_napi_system *system = NULL;
    tinyactors_napi_definition *definition = NULL;
    if (napi_get_value_external(env, argv[0], (void **)&actor) != napi_ok || actor == NULL ||
        napi_unwrap(env, argv[1], (void **)&system) != napi_ok || system == NULL ||
        napi_unwrap(env, argv[2], (void **)&definition) != napi_ok || definition == NULL ||
        actor->system != system || actor->definition != definition) {
        (void)tinyactors_napi_throw_error(env, "invalid", "invalid native Actor handle");
        return NULL;
    }
    if (!tinyactors_napi_check(env,
            napi_create_reference(env, argv[1], 1, &actor->system_ref),
            "napi_create_reference") ||
        !tinyactors_napi_check(env,
            napi_create_reference(env, argv[2], 1, &actor->definition_ref),
            "napi_create_reference") ||
        !tinyactors_napi_check(env,
            napi_wrap(env, this_value, actor, tinyactors_napi_actor_finalize, NULL, NULL),
            "napi_wrap")) {
        if (actor->system_ref != NULL) {
            (void)napi_delete_reference(env, actor->system_ref);
            actor->system_ref = NULL;
        }
        if (actor->definition_ref != NULL) {
            (void)napi_delete_reference(env, actor->definition_ref);
            actor->definition_ref = NULL;
        }
        return NULL;
    }
    return this_value;
}

static napi_value tinyactors_napi_actor_system(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = tinyactors_napi_unwrap_actor(env, this_value);
    if (actor == NULL || actor->system_ref == NULL) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_check(env,
            napi_get_reference_value(env, actor->system_ref, &result),
            "napi_get_reference_value")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_actor_definition(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = tinyactors_napi_unwrap_actor(env, this_value);
    if (actor == NULL || actor->definition_ref == NULL) {
        return NULL;
    }
    napi_value result;
    if (!tinyactors_napi_check(env,
            napi_get_reference_value(env, actor->definition_ref, &result),
            "napi_get_reference_value")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_actor_id(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = tinyactors_napi_unwrap_actor(env, this_value);
    napi_value result;
    if (actor == NULL || !tinyactors_napi_create_uint64(env, actor->id, &result)) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_actor_destroyed(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = tinyactors_napi_unwrap_actor(env, this_value);
    napi_value result;
    if (actor == NULL || !tinyactors_napi_check(env,
            napi_get_boolean(env, actor->destroyed || actor->system->closed, &result),
            "napi_get_boolean")) {
        return NULL;
    }
    return result;
}

static napi_value tinyactors_napi_actor_destroy(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = tinyactors_napi_unwrap_actor(env, this_value);
    if (actor == NULL || !tinyactors_napi_require_actor_open(env, actor, "Actor.destroy")) {
        return NULL;
    }
    if (actor->destroyed) {
        return NULL;
    }
    tinyactors_napi_runnable_remove(actor->system, actor->id);
    ta_result result = ta_actor_destroy(actor->system->core, actor->id);
    if (result == TA_OK) {
        actor->destroyed = true;
        return NULL;
    }
    (void)tinyactors_napi_throw_result(env, result, "ta_actor_destroy");
    return NULL;
}

static bool tinyactors_napi_set_bool(napi_env env, napi_value object, const char *name,
                                     bool value) {
    napi_value js_value;
    return tinyactors_napi_check(env, napi_get_boolean(env, value, &js_value),
                                 "napi_get_boolean") &&
           tinyactors_napi_check(env, napi_set_named_property(env, object, name, js_value),
                                 "napi_set_named_property");
}

static napi_value tinyactors_napi_actor_inspect(napi_env env, napi_callback_info info) {
    napi_value this_value;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, NULL, NULL, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = tinyactors_napi_unwrap_actor(env, this_value);
    if (actor == NULL || !tinyactors_napi_require_actor_open(env, actor, "Actor.inspect")) {
        return NULL;
    }
    ta_actor_inspection facts;
    if (!tinyactors_napi_throw_result(env,
            ta_actor_inspect(actor->system->core, actor->id, &facts), "ta_actor_inspect")) {
        return NULL;
    }
    uint32_t state_count = facts.active_state_count;
    ta_result result = TA_OK;
    ta_state_id *states = NULL;
    if (state_count != 0) {
        states = calloc(state_count, sizeof(*states));
        if (states == NULL) {
            (void)tinyactors_napi_throw_error(env, "out-of-memory", "out of memory inspecting Actor");
            return NULL;
        }
        result = ta_actor_active_states(actor->system->core,
            &(ta_actor_active_states_params){.actor = actor->id,
                                              .states = states,
                                              .capacity = state_count,
                                              .count = &state_count});
        if (!tinyactors_napi_throw_result(env, result, "ta_actor_active_states")) {
            free(states);
            return NULL;
        }
    }

    napi_value inspection;
    napi_value value;
    if (!tinyactors_napi_check(env, napi_create_object(env, &inspection), "napi_create_object") ||
        !tinyactors_napi_create_uint64(env, actor->id, &value) ||
        !tinyactors_napi_check(env, napi_set_named_property(env, inspection, "actor", value),
                               "napi_set_named_property")) {
        free(states);
        return NULL;
    }
    napi_value definition_value;
    napi_value identity;
    if (!tinyactors_napi_check(env,
            napi_get_reference_value(env, actor->definition_ref, &definition_value),
            "napi_get_reference_value") ||
        !tinyactors_napi_check(env,
            napi_get_named_property(env, definition_value, "identity", &identity),
            "napi_get_named_property") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, inspection, "definition", identity),
            "napi_set_named_property")) {
        free(states);
        return NULL;
    }

    napi_value active_states;
    if (!tinyactors_napi_check(env, napi_create_array_with_length(env, state_count, &active_states),
                               "napi_create_array_with_length")) {
        free(states);
        return NULL;
    }
    for (uint32_t index = 0; index < state_count; index += 1u) {
        ta_state_info state;
        if (!tinyactors_napi_throw_result(env,
                ta_definition_state(actor->system->core,
                    &(ta_definition_state_params){.definition = facts.definition_id,
                                                  .state = states[index], .out = &state}),
                "ta_definition_state")) {
            free(states);
            return NULL;
        }
        napi_value name;
        if (!tinyactors_napi_check(env,
                napi_create_string_utf8(env, state.id.ptr, state.id.len, &name),
                "napi_create_string_utf8") ||
            !tinyactors_napi_check(env, napi_set_element(env, active_states, index, name),
                                   "napi_set_element")) {
            free(states);
            return NULL;
        }
    }
    free(states);
    if (!tinyactors_napi_check(env,
            napi_set_named_property(env, inspection, "activeStates", active_states),
            "napi_set_named_property") ||
        !tinyactors_napi_set_bool(env, inspection, "macrostepInProgress",
                                  facts.macrostep_in_progress) ||
        !tinyactors_napi_set_bool(env, inspection, "done", facts.has_completion) ||
        !tinyactors_napi_set_bool(env, inspection, "published", facts.published)) {
        return NULL;
    }
    const char *residency = "running";
    if (facts.residency == TA_ACTOR_IDLE) {
        residency = "resident";
    } else if (facts.residency == TA_ACTOR_DONE) {
        residency = "done";
    } else if (facts.residency == TA_ACTOR_UNLOADED) {
        residency = "unloaded";
    }
    if (!tinyactors_napi_check(env,
            napi_create_string_utf8(env, residency, NAPI_AUTO_LENGTH, &value),
            "napi_create_string_utf8") ||
        !tinyactors_napi_check(env, napi_set_named_property(env, inspection, "residency", value),
                               "napi_set_named_property") ||
        !tinyactors_napi_check(env,
            napi_create_uint32(env, facts.internal_count, &value), "napi_create_uint32") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, inspection, "internalEventCount", value),
            "napi_set_named_property") ||
        !tinyactors_napi_check(env,
            napi_create_double(env, (double)facts.allocated_bytes, &value),
            "napi_create_double") ||
        !tinyactors_napi_check(env,
            napi_set_named_property(env, inspection, "allocatedBytes", value),
            "napi_set_named_property")) {
        return NULL;
    }
    if (facts.has_completion) {
        if (!tinyactors_napi_check(env, napi_get_null(env, &value), "napi_get_null") ||
            !tinyactors_napi_check(env,
                napi_set_named_property(env, inspection, "completion", value),
                "napi_set_named_property")) {
            return NULL;
        }
    }
    return inspection;
}

static void tinyactors_napi_actor_drop_after_spawn_failure(tinyactors_napi_actor *actor) {
    if (actor == NULL) {
        return;
    }
    (void)ta_actor_destroy(actor->system->core, actor->id);
    actor->destroyed = true;
}

static napi_value tinyactors_napi_system_spawn(napi_env env, napi_callback_info info) {
    napi_value this_value;
    napi_value argv[2];
    size_t argc = 2;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_system *system = tinyactors_napi_unwrap_system(env, this_value);
    if (system == NULL || !tinyactors_napi_require_open(env, system, "System.spawn")) {
        return NULL;
    }
    if (argc < 1 || argc > 2) {
        (void)tinyactors_napi_throw_error(env, "invalid", "System.spawn requires a Definition");
        return NULL;
    }
    tinyactors_napi_definition *definition = tinyactors_napi_unwrap_definition(env, argv[0]);
    if (definition == NULL || definition->system != system) {
        (void)tinyactors_napi_throw_error(env, "invalid",
                                           "Definition belongs to another System");
        return NULL;
    }
    if (argc == 2) {
        napi_valuetype type;
        if (!tinyactors_napi_check(env, napi_typeof(env, argv[1], &type), "napi_typeof")) {
            return NULL;
        }
        if (type != napi_undefined) {
            (void)tinyactors_napi_throw_error(env, "unsupported",
                                               "anonymous System.spawn does not accept options");
            return NULL;
        }
    }

    ta_actor_id id = 0;
    if (!tinyactors_napi_throw_result(env,
            ta_actor_spawn(system->core,
                &(ta_actor_spawn_params){.definition = definition->id, .out = &id}),
            "ta_actor_spawn")) {
        return NULL;
    }
    tinyactors_napi_actor *actor = calloc(1, sizeof(*actor));
    if (actor == NULL) {
        (void)ta_actor_destroy(system->core, id);
        (void)tinyactors_napi_throw_error(env, "out-of-memory", "out of memory creating Actor");
        return NULL;
    }
    actor->system = system;
    actor->definition = definition;
    actor->id = id;

    tinyactors_napi_environment *environment = NULL;
    napi_value constructor;
    napi_value external;
    napi_value instance;
    if (!tinyactors_napi_check(env,
            napi_get_instance_data(env, (void **)&environment), "napi_get_instance_data") ||
        environment == NULL || environment->actor_constructor == NULL ||
        !tinyactors_napi_check(env,
            napi_get_reference_value(env, environment->actor_constructor, &constructor),
            "napi_get_reference_value") ||
        !tinyactors_napi_check(env,
            napi_create_external(env, actor, NULL, NULL, &external), "napi_create_external") ||
        !tinyactors_napi_check(env,
            napi_new_instance(env, constructor, 3,
                              (napi_value[]){external, this_value, argv[0]}, &instance),
            "napi_new_instance")) {
        tinyactors_napi_actor_drop_after_spawn_failure(actor);
        free(actor);
        return NULL;
    }
    if (!tinyactors_napi_runnable_enqueue(env, system, id)) {
        actor->destroyed = true;
        (void)ta_actor_destroy(system->core, id);
        return NULL;
    }
    ta_result pump_result = tinyactors_napi_pump_runnable(env, system);
    if (!tinyactors_napi_throw_result(env, pump_result, "ta_actor_resume")) {
        actor->destroyed = true;
        (void)ta_actor_destroy(system->core, id);
        return NULL;
    }
    return instance;
}

static napi_value tinyactors_napi_system_define(napi_env env, napi_callback_info info) {
    napi_value this_value;
    napi_value argv[1];
    size_t argc = 1;
    if (!tinyactors_napi_check(env,
            napi_get_cb_info(env, info, &argc, argv, &this_value, NULL), "napi_get_cb_info")) {
        return NULL;
    }
    tinyactors_napi_system *system = tinyactors_napi_unwrap_system(env, this_value);
    if (system == NULL || !tinyactors_napi_require_open(env, system, "System.define")) {
        return NULL;
    }
    if (argc != 1) {
        (void)tinyactors_napi_throw_error(env, "invalid", "System.define requires a specification");
        return NULL;
    }
    tinyactors_napi_spec parsed;
    if (!tinyactors_napi_parse_spec(env, argv[0], &parsed)) {
        return NULL;
    }
    ta_definition *compiled = NULL;
    ta_result result = ta_definition_compile_spec(
        system->core,
        &(ta_definition_compile_params){
            .spec = &parsed.spec,
            .datamodel = tinyactors_napi_null_datamodel(),
            .allocator = tinyactors_napi_allocator(),
            .out = &compiled,
        });
    if (!tinyactors_napi_throw_result(env, result, "ta_definition_compile_spec")) {
        if (compiled != NULL) {
            ta_definition_destroy(compiled);
        }
        tinyactors_napi_free_spec(&parsed);
        return NULL;
    }
    ta_definition_id id = 0;
    result = ta_system_register_definition(system->core, compiled, &id);
    if (!tinyactors_napi_throw_result(env, result, "ta_system_register_definition")) {
        if (compiled != NULL) {
            ta_definition_destroy(compiled);
        }
        tinyactors_napi_free_spec(&parsed);
        return NULL;
    }

    tinyactors_napi_definition *definition = calloc(1, sizeof(*definition));
    if (definition == NULL) {
        (void)tinyactors_napi_throw_error(env, "out-of-memory",
                                           "out of memory creating Definition");
        (void)ta_system_retire_definition(system->core, id);
        tinyactors_napi_free_spec(&parsed);
        return NULL;
    }
    definition->system = system;
    definition->id = id;
    definition->family = parsed.family;
    definition->family_len = parsed.identity.family.name.len;
    definition->revision = parsed.revision;
    definition->revision_len = parsed.identity.revision.len;
    definition->revision_is_string = parsed.revision_is_string;
    parsed.family = NULL;
    parsed.revision = NULL;
    tinyactors_napi_free_spec(&parsed);

    tinyactors_napi_environment *environment = NULL;
    if (!tinyactors_napi_check(env,
            napi_get_instance_data(env, (void **)&environment), "napi_get_instance_data") ||
        environment == NULL || environment->definition_constructor == NULL) {
        free(definition->family);
        free(definition->revision);
        free(definition);
        (void)ta_system_retire_definition(system->core, id);
        (void)tinyactors_napi_throw_error(env, "invalid", "Definition constructor unavailable");
        return NULL;
    }
    napi_value constructor;
    napi_value external;
    napi_value instance;
    if (!tinyactors_napi_check(env,
            napi_get_reference_value(env, environment->definition_constructor, &constructor),
            "napi_get_reference_value") ||
        !tinyactors_napi_check(env, napi_create_external(env, definition, NULL, NULL, &external),
                               "napi_create_external") ||
        !tinyactors_napi_check(env,
            napi_new_instance(env, constructor, 2,
                              (napi_value[]){external, this_value}, &instance),
            "napi_new_instance")) {
        free(definition->family);
        free(definition->revision);
        free(definition);
        (void)ta_system_retire_definition(system->core, id);
        return NULL;
    }
    return instance;
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

    napi_property_descriptor definition_properties[] = {
        { .utf8name = "system", .getter = tinyactors_napi_definition_system,
          .attributes = napi_enumerable },
        { .utf8name = "id", .getter = tinyactors_napi_definition_id,
          .attributes = napi_enumerable },
        { .utf8name = "identity", .getter = tinyactors_napi_definition_identity,
          .attributes = napi_enumerable },
        { .utf8name = "retired", .getter = tinyactors_napi_definition_retired,
          .attributes = napi_enumerable },
        { .utf8name = "retire", .method = tinyactors_napi_definition_retire,
          .attributes = napi_writable | napi_configurable },
    };
    napi_value definition_constructor;
    if (!tinyactors_napi_check(env,
            napi_define_class(env, "Definition", NAPI_AUTO_LENGTH,
                              tinyactors_napi_definition_constructor, NULL,
                              sizeof(definition_properties) / sizeof(definition_properties[0]),
                              definition_properties, &definition_constructor),
            "napi_define_class") ||
        !tinyactors_napi_check(env,
            napi_create_reference(env, definition_constructor, 1,
                                  &environment->definition_constructor),
            "napi_create_reference")) {
        return NULL;
    }

    napi_property_descriptor actor_properties[] = {
        { .utf8name = "system", .getter = tinyactors_napi_actor_system,
          .attributes = napi_enumerable },
        { .utf8name = "id", .getter = tinyactors_napi_actor_id,
          .attributes = napi_enumerable },
        { .utf8name = "definition", .getter = tinyactors_napi_actor_definition,
          .attributes = napi_enumerable },
        { .utf8name = "destroyed", .getter = tinyactors_napi_actor_destroyed,
          .attributes = napi_enumerable },
        { .utf8name = "inspect", .method = tinyactors_napi_actor_inspect,
          .attributes = napi_writable | napi_configurable },
        { .utf8name = "destroy", .method = tinyactors_napi_actor_destroy,
          .attributes = napi_writable | napi_configurable },
    };
    napi_value actor_constructor;
    if (!tinyactors_napi_check(env,
            napi_define_class(env, "Actor", NAPI_AUTO_LENGTH,
                              tinyactors_napi_actor_constructor, NULL,
                              sizeof(actor_properties) / sizeof(actor_properties[0]),
                              actor_properties, &actor_constructor),
            "napi_define_class") ||
        !tinyactors_napi_check(env,
            napi_create_reference(env, actor_constructor, 1, &environment->actor_constructor),
            "napi_create_reference")) {
        return NULL;
    }

    napi_property_descriptor system_properties[] = {
        { .utf8name = "define", .method = tinyactors_napi_system_define,
          .attributes = napi_writable | napi_configurable },
        { .utf8name = "spawn", .method = tinyactors_napi_system_spawn,
          .attributes = napi_writable | napi_configurable },
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
