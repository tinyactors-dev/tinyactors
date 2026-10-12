/* module.c — the module's entry point: the environment's state, the
 * TinyactorsError class and the package's exports. NAPI_MODULE_INIT()
 * defines Node-API's entry points (napi_register_module_v1,
 * node_api_module_get_api_version_v1: Node.js's include/node/node_api.h);
 * everything else here is ours. */
#include "addon.h"

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
    ta_napi_module *module = (ta_napi_module *)data;
    ta_napi_scratch_free(&module->scratch);
    ta_napi_buffer self = module->self;
    ta_napi_buffer_free(&self);
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

/* Core's statuses (types.h), by name without TA_: the package's constants
 * (OK, ERROR_INVALID, ...), and the message of an error with a status, as
 * the Python binding has them. */
typedef struct ta_napi_status_name {
    ta_status status;
    const char *name;
} ta_napi_status_name;

static const ta_napi_status_name ta_napi_status_names[] = {
    {TA_OK, "OK"},
    {TA_ERROR_EXECUTION, "ERROR_EXECUTION"},
    {TA_ERROR_COMMUNICATION, "ERROR_COMMUNICATION"},
    {TA_ERROR_INVALID, "ERROR_INVALID"},
    {TA_ERROR_NO_MEMORY, "ERROR_NO_MEMORY"},
    {TA_ERROR_STALE, "ERROR_STALE"},
    {TA_ERROR_STATE, "ERROR_STATE"},
    {TA_ERROR_UNSUPPORTED, "ERROR_UNSUPPORTED"},
    {TA_ERROR_FATAL, "ERROR_FATAL"},
    {TA_ERROR_ABORTED, "ERROR_ABORTED"},
};

static const char *ta_napi_status_name_of(ta_status status) {
    for (size_t i = 0; i < sizeof ta_napi_status_names / sizeof ta_napi_status_names[0]; i++)
        if (ta_napi_status_names[i].status == status) return ta_napi_status_names[i].name;
    return NULL;
}

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

int ta_napi_exception_pending(napi_env env) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    return pending;
}

void ta_napi_throw_status(napi_env env, const char *function, ta_status status) {
    if (ta_napi_exception_pending(env)) return;
    const char *name = ta_napi_status_name_of(status);
    char message[160];
    size_t length = 0;
    const char *const parts[] = {function, ": ", name ? "TA_" : "an unknown status", name ? name : ""};
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        size_t part = strlen(parts[i]);
        if (part > sizeof message - length) part = sizeof message - length;
        memcpy(message + length, parts[i], part);
        length += part;
    }
    ta_napi_error error;
    napi_value number;
    napi_create_string_utf8(env, message, length, &error.message);
    napi_create_object(env, &error.fields);
    napi_create_int32(env, status, &number);
    napi_set_named_property(env, error.fields, "status", number);
    ta_napi_throw(env, &error);
}

void ta_napi_throw_reply(napi_env env, napi_value reply_error) {
    if (ta_napi_exception_pending(env)) return;
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
    ta_napi_buffer memory = {NULL, 0};
    if (!ta_napi_buffer_reserve(&memory, sizeof(ta_napi_module))) {
        napi_throw_error(env, NULL, "TA_ERROR_NO_MEMORY");
        return NULL;
    }
    ta_napi_module *module = (ta_napi_module *)memory.pointer;
    memset(module, 0, sizeof *module);
    module->self = memory;
    module->scratch.env = env;
    if (napi_set_instance_data(env, module, ta_napi_module_free, NULL) != napi_ok) {
        ta_napi_buffer_free(&memory);
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
        {"compile", NULL, ta_napi_compile, NULL, NULL, NULL, napi_enumerable, NULL},
        {"Host", NULL, NULL, NULL, NULL, ta_napi_host_class(env), napi_enumerable, NULL},
        {"SimulatedClock", NULL, NULL, NULL, NULL, ta_napi_clock_class(env), napi_enumerable, NULL},
    };
    if (napi_define_properties(env, exports, sizeof exports_table / sizeof exports_table[0], exports_table) !=
        napi_ok)
        return NULL;
    for (size_t i = 0; i < sizeof ta_napi_status_names / sizeof ta_napi_status_names[0]; i++) {
        napi_value number;
        if (napi_create_int32(env, ta_napi_status_names[i].status, &number) != napi_ok ||
            napi_set_named_property(env, exports, ta_napi_status_names[i].name, number) != napi_ok)
            return NULL;
    }
    return exports;
}
