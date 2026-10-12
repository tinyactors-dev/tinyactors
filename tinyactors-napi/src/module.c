/* module.c — the module's entry point: the environment's state, the
 * TinyactorsError class and the package's exports. NAPI_MODULE_INIT()
 * defines Node-API's entry points (napi_register_module_v1,
 * node_api_module_get_api_version_v1: Node.js's include/node/node_api.h);
 * everything else here is ours. */
#include "addon.h"

#include <stdlib.h>
#include <string.h>

ta_napi_module *ta_napi_module_of(napi_env env) {
    void *data = NULL;
    napi_get_instance_data(env, &data);
    return (ta_napi_module *)data;
}

/* The environment ends: its state goes. Its references die with it. */
static void ta_napi_module_free(node_api_basic_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    free(data);
}

/* ---- TinyactorsError ------------------------------------------------------------ */

/* The class of what core refuses: an Error with the fields core gave
 * (docs/decisions.md, "Errors"). Defined in JavaScript because Node-API's
 * classes cannot extend Error. */
static const char ta_napi_error_source[] =
    "(class TinyactorsError extends Error {\n"
    "  constructor(message, fields) {\n"
    "    super(message);\n"
    "    this.name = 'TinyactorsError';\n"
    "    if (fields) Object.assign(this, fields);\n"
    "  }\n"
    "})";

/* The names of types.h's statuses, by -status. */
static const char *const ta_napi_status_names[] = {
    "TA_OK",           "TA_ERROR_EXECUTION", "TA_ERROR_COMMUNICATION", "TA_ERROR_INVALID", "TA_ERROR_NO_MEMORY",
    "TA_ERROR_STALE", "TA_ERROR_STATE",     "TA_ERROR_UNSUPPORTED",   "TA_ERROR_FATAL",   "TA_ERROR_ABORTED",
};

/* A TinyactorsError with `message` (a JavaScript string) and `fields`. */
typedef struct ta_napi_error {
    napi_value message, fields;
} ta_napi_error;

static void ta_napi_throw(napi_env env, const ta_napi_error *error) {
    ta_napi_module *module = ta_napi_module_of(env);
    napi_value class, thrown, argv[2] = {error->message, error->fields};
    if (napi_get_reference_value(env, module->error_class, &class) != napi_ok ||
        napi_new_instance(env, class, 2, argv, &thrown) != napi_ok)
        return;
    napi_throw(env, thrown);
}

static int ta_napi_pending(napi_env env) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    return pending;
}

void ta_napi_throw_status(napi_env env, ta_status status) {
    if (ta_napi_pending(env)) return;
    uint32_t index = status <= 0 && -status < (int32_t)(sizeof ta_napi_status_names / sizeof ta_napi_status_names[0])
                         ? (uint32_t)-status
                         : 0;
    ta_napi_error error;
    napi_value number;
    const char *name = ta_napi_status_names[index];
    napi_create_string_utf8(env, name, strlen(name), &error.message);
    napi_create_object(env, &error.fields);
    napi_create_int32(env, status, &number);
    napi_set_named_property(env, error.fields, "status", number);
    ta_napi_throw(env, &error);
}

void ta_napi_throw_reply(napi_env env, napi_value reply_error) {
    if (ta_napi_pending(env)) return;
    ta_napi_error error;
    napi_value code, data, reason, path, undefined;
    napi_get_undefined(env, &undefined);
    error.message = data = reason = path = code = undefined;
    napi_get_named_property(env, reply_error, "message", &error.message);
    napi_get_named_property(env, reply_error, "code", &code);
    if (napi_get_named_property(env, reply_error, "data", &data) == napi_ok) {
        napi_valuetype type = napi_undefined;
        napi_typeof(env, data, &type);
        if (type == napi_object) {
            napi_get_named_property(env, data, "reason", &reason);
            napi_get_named_property(env, data, "path", &path);
        }
    }
    napi_create_object(env, &error.fields);
    napi_set_named_property(env, error.fields, "code", code);
    napi_set_named_property(env, error.fields, "reason", reason);
    napi_set_named_property(env, error.fields, "path", path);
    ta_napi_throw(env, &error);
}

/* ---- init ------------------------------------------------------------------------- */

NAPI_MODULE_INIT() {
    ta_napi_module *module = (ta_napi_module *)calloc(1, sizeof *module);
    if (!module) {
        napi_throw_error(env, NULL, "TA_ERROR_NO_MEMORY");
        return NULL;
    }
    if (napi_set_instance_data(env, module, ta_napi_module_free, NULL) != napi_ok) {
        free(module);
        return NULL;
    }
    napi_value source, error_class;
    if (napi_create_string_utf8(env, ta_napi_error_source, sizeof ta_napi_error_source - 1, &source) != napi_ok ||
        napi_run_script(env, source, &error_class) != napi_ok ||
        napi_create_reference(env, error_class, 1, &module->error_class) != napi_ok)
        return NULL;
    napi_property_descriptor exports_table[] = {
        {"encode", NULL, ta_napi_encode, NULL, NULL, NULL, napi_enumerable, NULL},
        {"decode", NULL, ta_napi_decode, NULL, NULL, NULL, napi_enumerable, NULL},
        {"TinyactorsError", NULL, NULL, NULL, NULL, error_class, napi_enumerable, NULL},
    };
    if (napi_define_properties(env, exports, sizeof exports_table / sizeof exports_table[0], exports_table) !=
        napi_ok)
        return NULL;
    return exports;
}
