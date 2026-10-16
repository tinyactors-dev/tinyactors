/* The Host (AGENTS.md, "The binding's memory"; docs/api-map.md): the class,
 * `new Host(config, options)` over ta_host_create, the host's arena with a
 * mark per call, its tables, `close()`, the dispose symbols, and the
 * finalizer. The inputs, the loop, the effects and the typed methods are
 * the files beside it.
 *
 * Who holds what (AGENTS.md, "Rules the suite cannot see"):
 *   - the wrapper (the Host's JavaScript object) holds the ta_napi_host
 *     (napi_wrap, type-tagged) and, under the instance's `handlers`
 *     symbol, the handlers object, so a handler that closes over its host
 *     makes a cycle the collector sees through: no strong napi_ref holds a
 *     handler;
 *   - the ta_napi_host holds a weak reference to the wrapper;
 *   - the finalizer frees what close() did not, with basic-env calls only
 *     (napi_delete_reference, napi_adjust_external_memory through the
 *     allocator) and no last take, so the same path serves the
 *     environment's teardown. */
#include "addon.h"

#include <string.h>

/* napi_type_tag_object's tag of a Host's wrapper. */
static const napi_type_tag ta_napi_host_tag = {0x7461686f73744e41ULL, 0x5049686f73743031ULL};

/* The room for a refused config's error (environment->error), in the
 * call's arena. */
#define TA_NAPI_CONFIG_ERROR 8192u

/* ---- the tables and their release ------------------------------------------ */

/* The tables a host keeps and, for each, where its row's napi_ref is
 * (every row kind but pending holds one, first). */
typedef struct ta_napi_rows {
    ta_table *table;
    bool referenced;
} ta_napi_rows;

static void ta_napi_rows_release(napi_env env, const ta_napi_rows *rows) {
    ta_table *table = rows->table;
    for (uint32_t slot = 0; rows->referenced && slot < table->capacity; slot++) {
        napi_ref *reference = (napi_ref *)ta_table_at(table, slot);
        if (reference != NULL && *reference != NULL) napi_delete_reference(env, *reference);
    }
    ta_table_free(table);
}

static void ta_napi_tables_init(ta_napi_host *host) {
    const struct {
        ta_table *table;
        uint32_t row;
    } tables[] = {
        {&host->charts, sizeof(ta_napi_chart_row)},     {&host->sessions, sizeof(ta_napi_session_row)},
        {&host->payloads, sizeof(ta_napi_payload_row)}, {&host->loops, sizeof(ta_napi_loop_row)},
        {&host->pending, sizeof(ta_napi_pending_row)},
    };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        ta_table_config config = {sizeof config, tables[i].row, &host->allocator};
        ta_table_init(tables[i].table, &config);
    }
}

/* Frees core's host and everything made with it, the struct aside: the
 * host first (its datamodel calls release rows), then the rows'
 * references, the tables, quickjs, the arena. Basic-env calls only, so a
 * finalizer may call it; pending handlers are dropped, never called. */
static void ta_napi_host_release(napi_env env, ta_napi_host *host) {
    if (host->host != NULL) ta_host_free(host->host);
    host->host = NULL;
    /* the loop's references: a timer's handle is cleared by close() first
     * (ta_napi_loop_stop); a finalizer finds none, since a pending one holds
     * the wrapper */
    napi_ref *references[] = {&host->clock, &host->immediate, &host->timer, &host->idle_promise, &host->error};
    for (size_t i = 0; i < sizeof references / sizeof references[0]; i++) {
        if (*references[i] != NULL) napi_delete_reference(env, *references[i]);
        *references[i] = NULL;
    }
    const ta_napi_rows rows[] = {
        {&host->charts, true}, {&host->sessions, true}, {&host->payloads, true},
        {&host->loops, true},  {&host->pending, false},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) ta_napi_rows_release(env, &rows[i]);
    if (host->quickjs != NULL) ta_datamodel_ecmascript_free(host->quickjs); /* after the host that used it */
    host->quickjs = NULL;
    ta_growable_arena_free(&host->arena);
}

/* The wrapper's finalizer, and the end of a construction that failed:
 * whatever close() did not free, then the struct, through the allocator
 * that made it. */
static void ta_napi_host_finalize(napi_env env, void *data, void *hint) {
    ta_napi_host *host = (ta_napi_host *)data;
    (void)hint;
    ta_napi_host_release(env, host);
    if (host->self != NULL) napi_delete_reference(env, host->self);
    ta_allocator allocator = host->allocator;
    allocator.free(allocator.context, host, sizeof *host);
}

/* ---- a call ------------------------------------------------------------------ */

static napi_value ta_napi_host_refuse(napi_env env, const char *message) {
    return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STATE, message});
}

ta_napi_host *ta_napi_host_of(napi_env env, const ta_napi_host_check *check) {
    bool tagged = false;
    napi_valuetype type = napi_undefined;
    if (check->self == NULL || napi_typeof(env, check->self, &type) != napi_ok || type != napi_object ||
        napi_check_object_type_tag(env, check->self, &ta_napi_host_tag, &tagged) != napi_ok || !tagged) {
        napi_throw_type_error(env, NULL, "not a Host");
        return NULL;
    }
    void *data = NULL;
    if (napi_unwrap(env, check->self, &data) != napi_ok || data == NULL) {
        napi_throw_type_error(env, NULL, "not a Host");
        return NULL;
    }
    ta_napi_host *host = (ta_napi_host *)data;
    if (host->env != env) { /* bindings.md, section 5: one thread per host */
        ta_napi_host_refuse(env, "a Host is called only from the environment that made it");
        return NULL;
    }
    if (check->open && host->host == NULL) {
        ta_napi_host_refuse(env, "the Host is closed");
        return NULL;
    }
    return host;
}

bool ta_napi_enter(napi_env env, napi_value self, ta_napi_call *call) {
    ta_napi_host *host = ta_napi_host_of(env, &(ta_napi_host_check){self, true});
    if (host == NULL) return false;
    call->host = host;
    call->mark = ta_growable_arena_position(&host->arena);
    host->depth++;
    return true;
}

/* close()'s work: the loop's timers cleared and a waiting idle()
 * rejected, the last take of telemetry (ta_napi_loop_stop), then the host
 * freed and the handlers dropped. An error of the last take stays kept on
 * the host, for close() or the turn to throw. */
static void ta_napi_host_close_now(napi_env env, ta_napi_host *host);

void ta_napi_leave(ta_napi_call *call) {
    ta_napi_host *host = call->host;
    ta_growable_arena_release(&host->arena, call->mark);
    host->depth--;
    if (host->depth == 0 && !host->turning && host->closing) ta_napi_host_close_now(host->env, host);
}

/* ---- options ------------------------------------------------------------------- */

static void ta_napi_host_close_now(napi_env env, ta_napi_host *host) {
    host->closing = false;
    ta_napi_loop_stop(env, host);
    napi_value kept = NULL;
    if (host->error != NULL) napi_get_reference_value(env, host->error, &kept); /* outlives the release */
    ta_napi_host_release(env, host);
    if (kept != NULL) napi_create_reference(env, kept, 1, &host->error);
    napi_value self, undefined, key, keys;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    /* the handlers go with the host: nothing a closed host holds keeps them */
    if (instance != NULL && host->self != NULL && napi_get_reference_value(env, host->self, &self) == napi_ok &&
        self != NULL && napi_get_reference_value(env, instance->keys, &keys) == napi_ok &&
        napi_get_named_property(env, keys, "handlers", &key) == napi_ok &&
        napi_get_undefined(env, &undefined) == napi_ok)
        napi_set_property(env, self, key, undefined);
}

/* A string option compared with `names`: its index, or -1 with a
 * TypeError thrown naming `what`. */
typedef struct ta_napi_choice {
    const char *what;
    const char *const *names;
    int count;
} ta_napi_choice;

static int ta_napi_choose(napi_env env, napi_value value, const ta_napi_choice *choice) {
    char text[16];
    size_t length = 0;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, value, &type) == napi_ok && type == napi_string &&
        napi_get_value_string_utf8(env, value, text, sizeof text, &length) == napi_ok) {
        for (int i = 0; i < choice->count; i++)
            if (strlen(choice->names[i]) == length && memcmp(choice->names[i], text, length) == 0) return i;
    }
    napi_throw_type_error(env, NULL, choice->what);
    return -1;
}

/* A non-negative integer option, at most `limit`; false with a TypeError
 * naming `what`. */
typedef struct ta_napi_count {
    const char *what;
    double limit;
    double value; /* out */
} ta_napi_count;

static bool ta_napi_count_of(napi_env env, napi_value value, ta_napi_count *count) {
    napi_valuetype type = napi_undefined;
    double number = -1;
    if (napi_typeof(env, value, &type) == napi_ok && type == napi_number &&
        napi_get_value_double(env, value, &number) == napi_ok && number >= 0 && number <= count->limit &&
        number == (double)(int64_t)number) {
        count->value = number;
        return true;
    }
    napi_throw_type_error(env, NULL, count->what);
    return false;
}

/* Whether every own key of `object` is one of `names`; a TypeError naming
 * the first that is not (an option misspelled is refused, not ignored). */
typedef struct ta_napi_known {
    const char *what; /* "a Host option", "a quickjs option" */
    const char *const *names;
    uint32_t count;
} ta_napi_known;

static bool ta_napi_only_known(napi_env env, napi_value object, const ta_napi_known *known) {
    napi_value keys, key;
    uint32_t length = 0;
    if (napi_get_property_names(env, object, &keys) != napi_ok || napi_get_array_length(env, keys, &length) != napi_ok)
        return false;
    for (uint32_t i = 0; i < length; i++) {
        char name[32];
        size_t size = 0;
        if (napi_get_element(env, keys, i, &key) != napi_ok ||
            napi_get_value_string_utf8(env, key, name, sizeof name, &size) != napi_ok)
            return false;
        bool found = false;
        for (uint32_t j = 0; j < known->count && !found; j++)
            found = strlen(known->names[j]) == size && memcmp(known->names[j], name, size) == 0;
        if (!found) {
            napi_value message, error;
            char text[96];
            size_t at = 0;
            const char *parts[] = {"unknown ", known->what, ": ", name};
            for (size_t p = 0; p < 4; p++)
                for (const char *c = parts[p]; *c != '\0' && at + 1 < sizeof text; c++) text[at++] = *c;
            if (napi_create_string_utf8(env, text, at, &message) == napi_ok &&
                napi_create_type_error(env, NULL, message, &error) == napi_ok)
                napi_throw(env, error);
            return false;
        }
    }
    return true;
}

/* An option of `options` (undefined when absent or when options is). */
typedef struct ta_napi_options {
    napi_value object; /* NULL: none given */
} ta_napi_options;

static napi_value ta_napi_option(napi_env env, const ta_napi_options *options, const char *name) {
    napi_value value = NULL;
    if (options->object == NULL || napi_get_named_property(env, options->object, name, &value) != napi_ok)
        napi_get_undefined(env, &value);
    return value;
}

static bool ta_napi_is(napi_env env, napi_value value, napi_valuetype want) {
    napi_valuetype type = napi_undefined;
    return napi_typeof(env, value, &type) == napi_ok && type == want;
}

/* The quickjs option: the sandbox's limits (ta_datamodel_ecmascript_config,
 * datamodel_ecmascript.h). */
static bool ta_napi_quickjs_config(napi_env env, napi_value given, ta_datamodel_ecmascript_config *config) {
    static const char *const names[] = {"maxOps", "memoryLimit", "stackLimit", "realm"};
    static const char *const realms[] = {"auto", "isolated"};
    memset(config, 0, sizeof *config);
    config->size = sizeof *config;
    config->realm = TA_DATAMODEL_ECMASCRIPT_REALM_AUTO;
    if (ta_napi_is(env, given, napi_undefined)) return true;
    if (!ta_napi_is(env, given, napi_object)) {
        napi_throw_type_error(env, NULL, "quickjs is an object of the sandbox's limits");
        return false;
    }
    if (!ta_napi_only_known(env, given, &(ta_napi_known){"quickjs option", names, 4})) return false;
    ta_napi_options options = {given};
    napi_value value = ta_napi_option(env, &options, "maxOps");
    ta_napi_count count = {"quickjs.maxOps is an integer from 0 to 2^32-1", 4294967295.0, 0};
    if (!ta_napi_is(env, value, napi_undefined)) {
        if (!ta_napi_count_of(env, value, &count)) return false;
        config->max_ops = (uint32_t)count.value;
    }
    value = ta_napi_option(env, &options, "memoryLimit");
    count = (ta_napi_count){"quickjs.memoryLimit is a safe integer of bytes, 0 or more", 9007199254740991.0, 0};
    if (!ta_napi_is(env, value, napi_undefined)) {
        if (!ta_napi_count_of(env, value, &count)) return false;
        config->memory_limit = (size_t)count.value;
    }
    value = ta_napi_option(env, &options, "stackLimit");
    count = (ta_napi_count){"quickjs.stackLimit is a safe integer of bytes, 0 or more", 9007199254740991.0, 0};
    if (!ta_napi_is(env, value, napi_undefined)) {
        if (!ta_napi_count_of(env, value, &count)) return false;
        config->stack_limit = (size_t)count.value;
    }
    value = ta_napi_option(env, &options, "realm");
    if (!ta_napi_is(env, value, napi_undefined)) {
        int realm = ta_napi_choose(env, value, &(ta_napi_choice){"quickjs.realm is \"auto\" or \"isolated\"", realms, 2});
        if (realm < 0) return false;
        config->realm = realm == 0 ? TA_DATAMODEL_ECMASCRIPT_REALM_AUTO : TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED;
    }
    return true;
}

/* The handlers given as options, checked: each a function or absent;
 * ioProcessors and services objects of functions. Kept by name in
 * `handlers`, the object the wrapper holds. */
static bool ta_napi_handlers_read(napi_env env, const ta_napi_options *options, napi_value handlers) {
    static const char *const functions[] = {"log", "transport", "telemetry", "wake", "migrated", "migrationDone"};
    static const char *const maps[] = {"ioProcessors", "services"};
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; i++) {
        napi_value value = ta_napi_option(env, options, functions[i]);
        if (ta_napi_is(env, value, napi_undefined)) continue;
        if (!ta_napi_is(env, value, napi_function)) {
            napi_throw_type_error(env, NULL, "a Host's handler is a function");
            return false;
        }
        if (napi_set_named_property(env, handlers, functions[i], value) != napi_ok) return false;
    }
    for (size_t i = 0; i < sizeof maps / sizeof maps[0]; i++) {
        napi_value value = ta_napi_option(env, options, maps[i]), names, name, handler;
        uint32_t count = 0;
        if (ta_napi_is(env, value, napi_undefined)) continue;
        if (!ta_napi_is(env, value, napi_object) || napi_get_property_names(env, value, &names) != napi_ok ||
            napi_get_array_length(env, names, &count) != napi_ok) {
            napi_throw_type_error(env, NULL, "ioProcessors and services are objects of a type -> its handler");
            return false;
        }
        for (uint32_t j = 0; j < count; j++) {
            if (napi_get_element(env, names, j, &name) != napi_ok || napi_get_property(env, value, name, &handler) != napi_ok)
                return false;
            if (!ta_napi_is(env, handler, napi_function)) {
                napi_throw_type_error(env, NULL, "ioProcessors and services are objects of a type -> its handler");
                return false;
            }
        }
        if (napi_set_named_property(env, handlers, maps[i], value) != napi_ok) return false;
    }
    return true;
}

/* ---- creation ------------------------------------------------------------------------ */

/* host.config, posted once before the host is handed out (no batch is
 * open: bindings.md, section 3), drained at once: the effective config,
 * or NULL with an exception pending. */
static napi_value ta_napi_host_config(napi_env env, ta_napi_host *host) {
    unsigned char room[64];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){room, sizeof room, 0}, &host->arena_allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("host.config"));
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, 0);
    ta_bytes message;
    ta_status status = ta_cbor_writer_bytes(&writer, &message);
    if (status == TA_OK) status = ta_host_post(host->host, message);
    ta_cbor_writer_free(&writer);
    if (status != TA_OK) return ta_napi_throw(env, &(ta_napi_refusal){status, "host.config was refused"});
    ta_napi_drain_state drain = {host, NULL, false};
    ta_napi_drain(env, &drain);
    if (ta_napi_throw_kept(env, host)) return NULL;
    napi_value result = NULL;
    if (drain.reply == NULL || !ta_napi_is(env, drain.reply, napi_object) ||
        napi_get_named_property(env, drain.reply, "result", &result) != napi_ok || !ta_napi_is(env, result, napi_object))
        return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STATE, "host.config gave no config"});
    return result;
}

/* What the effective config tells the addon, read once: the tick rate,
 * whether an encoder runs, and the I/O processors, each type's handler at
 * its number (`processors[i + 1]` for ioProcessors[i]; 0 the transport).
 * A type options.ioProcessors names that the config lacks is refused. */
typedef struct ta_napi_effective {
    napi_value config;   /* the effective config */
    napi_value handlers; /* gets `processors` */
} ta_napi_effective;

static bool ta_napi_host_read_config(napi_env env, ta_napi_host *host, const ta_napi_effective *effective) {
    napi_value system, rate, entries, processors, transport, given, type, handler;
    double ticks = 0;
    bool has = false;
    if (napi_get_named_property(env, effective->config, "system", &system) != napi_ok || !ta_napi_is(env, system, napi_object) ||
        napi_get_named_property(env, system, "ticksPerSecond", &rate) != napi_ok || !ta_napi_is(env, rate, napi_number) ||
        napi_get_value_double(env, rate, &ticks) != napi_ok || ticks < 1) {
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STATE, "host.config gave no system.ticksPerSecond"});
        return false;
    }
    host->ticks_per_second = (uint64_t)ticks;
    if (napi_has_named_property(env, effective->config, "telemetry", &has) != napi_ok) return false;
    host->encoder = has;
    uint32_t count = 0;
    if (napi_get_named_property(env, effective->config, "ioProcessors", &entries) != napi_ok) return false;
    bool array = false;
    if (napi_is_array(env, entries, &array) != napi_ok) return false;
    if (array && napi_get_array_length(env, entries, &count) != napi_ok) return false;
    if (napi_get_named_property(env, effective->handlers, "ioProcessors", &given) != napi_ok ||
        napi_get_named_property(env, effective->handlers, "transport", &transport) != napi_ok ||
        napi_create_array_with_length(env, (size_t)count + 1, &processors) != napi_ok ||
        napi_set_element(env, processors, 0, transport) != napi_ok)
        return false;
    bool handlers = ta_napi_is(env, given, napi_object);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; i++) {
        napi_value entry;
        if (napi_get_element(env, entries, i, &entry) != napi_ok) return false;
        if (napi_get_undefined(env, &handler) != napi_ok) return false;
        if (handlers && ta_napi_is(env, entry, napi_object) &&
            napi_get_named_property(env, entry, "type", &type) == napi_ok && ta_napi_is(env, type, napi_string)) {
            bool own = false;
            if (napi_has_own_property(env, given, type, &own) != napi_ok) return false;
            if (own) {
                if (napi_get_property(env, given, type, &handler) != napi_ok) return false;
                found++;
            }
        }
        if (napi_set_element(env, processors, i + 1, handler) != napi_ok) return false;
    }
    if (handlers) {
        napi_value names;
        uint32_t named = 0;
        if (napi_get_property_names(env, given, &names) != napi_ok || napi_get_array_length(env, names, &named) != napi_ok)
            return false;
        if (named != found) {
            napi_throw_type_error(env, NULL, "ioProcessors names a type the config's ioProcessors does not have");
            return false;
        }
    }
    host->processors = count;
    return napi_set_named_property(env, effective->handlers, "processors", processors) == napi_ok;
}

/* ta_host_create with the config as CBOR in the call's arena and the
 * environment's one datamodel; a refused config throws core's error. */
static bool ta_napi_host_create(napi_env env, ta_napi_host *host, napi_value config) {
    ta_growable_arena_mark mark = ta_growable_arena_position(&host->arena);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &host->arena_allocator);
    bool ok = ta_napi_value_write(env, config, &(ta_napi_writing){&writer, &host->arena});
    ta_bytes cbor = {NULL, 0};
    ta_status status = ok ? ta_cbor_writer_bytes(&writer, &cbor) : TA_OK;
    if (ok && status != TA_OK) {
        ta_napi_throw(env, &(ta_napi_refusal){status, "the config could not be written"});
        ok = false;
    }
    if (ok) {
        ta_buffer refusal = {ta_growable_arena_alloc(&host->arena, TA_NAPI_CONFIG_ERROR, 8), TA_NAPI_CONFIG_ERROR, 0};
        ta_host_environment environment = {sizeof environment, 1, host->datamodels, host->allocator, NULL,
                                           refusal.pointer != NULL ? &refusal : NULL};
        status = ta_host_create(cbor, &environment, &host->host);
        if (status != TA_OK) {
            host->host = NULL;
            napi_value reply = NULL, error = NULL;
            if (refusal.length > 0 && refusal.length <= refusal.capacity) {
                reply = ta_napi_value_read(env, (ta_bytes){refusal.pointer, refusal.length});
                if (reply == NULL) {
                    napi_value ignored;
                    napi_get_and_clear_last_exception(env, &ignored);
                } else if (!ta_napi_is(env, reply, napi_object) ||
                           napi_get_named_property(env, reply, "error", &error) != napi_ok)
                    error = NULL;
            }
            ta_napi_throw_core_error(env, &(ta_napi_core_error){error, status});
            ok = false;
        }
    }
    ta_cbor_writer_free(&writer);
    ta_growable_arena_release(&host->arena, mark);
    return ok;
}

/* The datamodel option: "native" (the default) or "quickjs". The native
 * datamodel is built in a later step (docs/plan.xml, p1-datamodel): until
 * then a native host is refused with TA_ERROR_UNSUPPORTED. */
static bool ta_napi_host_datamodel(napi_env env, ta_napi_host *host, const ta_napi_options *options) {
    static const char *const names[] = {"native", "quickjs"};
    napi_value value = ta_napi_option(env, options, "datamodel"), limits = ta_napi_option(env, options, "quickjs");
    int kind = TA_NAPI_DATAMODEL_NATIVE;
    if (!ta_napi_is(env, value, napi_undefined)) {
        kind = ta_napi_choose(env, value, &(ta_napi_choice){"datamodel is \"native\" or \"quickjs\"", names, 2});
        if (kind < 0) return false;
    }
    host->datamodel = (ta_napi_datamodel_kind)kind;
    if (kind == TA_NAPI_DATAMODEL_NATIVE) {
        if (!ta_napi_is(env, limits, napi_undefined)) {
            napi_throw_type_error(env, NULL, "quickjs's limits are for a host with datamodel \"quickjs\"");
            return false;
        }
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_UNSUPPORTED,
                                              "the native \"ecmascript\" datamodel is not built yet: "
                                              "pass datamodel: \"quickjs\""});
        return false;
    }
    ta_datamodel_ecmascript_config config;
    if (!ta_napi_quickjs_config(env, limits, &config)) return false;
    host->quickjs = ta_datamodel_ecmascript_new(&config);
    if (host->quickjs == NULL) {
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "no memory for the quickjs datamodel"});
        return false;
    }
    host->datamodels[0] = ta_datamodel_ecmascript_binding(host->quickjs);
    return true;
}

/* The handlers object on the wrapper, under the instance's symbol: not
 * enumerable, not configurable; writable, so close() drops it. */
static bool ta_napi_host_keep(napi_env env, napi_value self, napi_value handlers) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value keys, key;
    if (instance == NULL || napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_get_named_property(env, keys, "handlers", &key) != napi_ok)
        return false;
    napi_property_descriptor property = {NULL, key, NULL, NULL, NULL, handlers, napi_writable, NULL};
    return napi_define_properties(env, self, 1, &property) == napi_ok;
}

/* new Host(config, options) */
static napi_value ta_napi_host_new(napi_env env, napi_callback_info info) {
    static const char *const known[] = {"ioProcessors", "transport", "log", "clock", "telemetry", "wake",
                                        "migrated", "migrationDone", "services", "datamodel", "quickjs"};
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL}, self, target, handlers, undefined;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok || napi_get_new_target(env, info, &target) != napi_ok)
        return NULL;
    if (target == NULL) {
        napi_throw_type_error(env, NULL, "Host is a class: new Host(config, options)");
        return NULL;
    }
    if (napi_get_undefined(env, &undefined) != napi_ok) return NULL;
    if (argc < 1) argv[0] = undefined;
    ta_napi_options options = {NULL};
    if (argc >= 2 && !ta_napi_is(env, argv[1], napi_undefined)) {
        if (!ta_napi_is(env, argv[1], napi_object)) {
            napi_throw_type_error(env, NULL, "a Host's options are an object");
            return NULL;
        }
        options.object = argv[1];
        if (!ta_napi_only_known(env, argv[1], &(ta_napi_known){"Host option", known, sizeof known / sizeof known[0]}))
            return NULL;
    }
    if (napi_create_object(env, &handlers) != napi_ok || !ta_napi_handlers_read(env, &options, handlers)) return NULL;

    ta_allocator allocator = ta_napi_allocator(env);
    ta_napi_host *host = (ta_napi_host *)allocator.alloc(allocator.context, sizeof *host, 8);
    if (host == NULL) return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "no memory for a Host"});
    memset(host, 0, sizeof *host);
    host->env = env;
    host->allocator = allocator;
    host->deadline = UINT64_MAX;
    host->telemetry = !ta_napi_is(env, ta_napi_option(env, &options, "telemetry"), napi_undefined);
    ta_growable_arena_config arena = {sizeof arena, 0, 16384, &host->allocator};
    ta_growable_arena_init(&host->arena, &arena);
    host->arena_allocator = ta_growable_arena_allocator(&host->arena);
    ta_napi_tables_init(host);

    napi_value effective = NULL;
    bool ok = ta_napi_host_datamodel(env, host, &options) && ta_napi_host_create(env, host, argv[0]);
    if (ok) {
        /* the config read in a call's frame: a mark, so its scratch goes */
        ta_growable_arena_mark mark = ta_growable_arena_position(&host->arena);
        effective = ta_napi_host_config(env, host);
        ok = effective != NULL &&
             ta_napi_host_read_config(env, host, &(ta_napi_effective){effective, handlers});
        ta_growable_arena_release(&host->arena, mark);
    }
    ok = ok && ta_napi_host_keep(env, self, handlers) &&
         napi_wrap(env, self, host, ta_napi_host_finalize, NULL, NULL) == napi_ok;
    if (!ok) {
        ta_napi_host_finalize(env, host, NULL);
        return NULL;
    }
    /* wrapped: from here the finalizer frees it */
    if (napi_type_tag_object(env, self, &ta_napi_host_tag) != napi_ok ||
        napi_create_reference(env, self, 0, &host->self) != napi_ok ||
        !ta_napi_clock_attach(env, host, ta_napi_option(env, &options, "clock")))
        return NULL;
    ta_napi_advance(env, host); /* the host's clock starts now, not at 0 */
    return self;
}

/* ---- methods ----------------------------------------------------------------------- */

static napi_value ta_napi_this(napi_env env, napi_callback_info info) {
    size_t argc = 0;
    napi_value self = NULL;
    if (napi_get_cb_info(env, info, &argc, NULL, &self, NULL) != napi_ok) return NULL;
    return self;
}

/* close(): clears the timers, rejects a waiting idle(), gives the last
 * take to the telemetry handler and frees the host; again, nothing. From a
 * handler (a call or a turn on the stack) it is done when the outermost
 * one returns. */
static napi_value ta_napi_host_close(napi_env env, napi_callback_info info) {
    ta_napi_host *host = ta_napi_host_of(env, &(ta_napi_host_check){ta_napi_this(env, info), false});
    if (host == NULL) return NULL;
    napi_value undefined;
    if (napi_get_undefined(env, &undefined) != napi_ok) return NULL;
    if (host->host == NULL) return undefined;
    if (host->depth > 0 || host->turning) host->closing = true;
    else ta_napi_host_close_now(env, host);
    if (host->host == NULL && ta_napi_throw_kept(env, host)) return NULL; /* the last take's error */
    return undefined;
}

/* [Symbol.asyncDispose](): close(), then a promise; it will also wait for
 * the promises the last take's handlers return (the telemetry step). */
static napi_value ta_napi_host_async_dispose(napi_env env, napi_callback_info info) {
    napi_deferred deferred;
    napi_value promise, result = ta_napi_host_close(env, info);
    if (napi_create_promise(env, &deferred, &promise) != napi_ok) return NULL;
    if (result != NULL) {
        napi_resolve_deferred(env, deferred, result);
        return promise;
    }
    napi_value error;
    if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return NULL;
    napi_reject_deferred(env, deferred, error);
    return promise;
}

static napi_value ta_napi_host_closed(napi_env env, napi_callback_info info) {
    ta_napi_host *host = ta_napi_host_of(env, &(ta_napi_host_check){ta_napi_this(env, info), false});
    napi_value result = NULL;
    if (host != NULL) napi_get_boolean(env, host->host == NULL, &result);
    return result;
}

static napi_value ta_napi_host_ticks(napi_env env, napi_callback_info info) {
    ta_napi_host *host = ta_napi_host_of(env, &(ta_napi_host_check){ta_napi_this(env, info), false});
    napi_value result = NULL;
    if (host != NULL) napi_create_double(env, (double)host->ticks_per_second, &result);
    return result;
}

/* Symbol.<name> of this realm, or NULL when it has none. */
static napi_value ta_napi_well_known(napi_env env, const char *name) {
    napi_value global, symbol, value;
    if (napi_get_global(env, &global) != napi_ok || napi_get_named_property(env, global, "Symbol", &symbol) != napi_ok ||
        napi_get_named_property(env, symbol, name, &value) != napi_ok || !ta_napi_is(env, value, napi_symbol))
        return NULL;
    return value;
}

napi_status ta_napi_host_define(napi_env env, napi_value exports) {
    napi_value dispose = ta_napi_well_known(env, "dispose"), async_dispose = ta_napi_well_known(env, "asyncDispose");
    napi_property_descriptor properties[] = {
        {"close", NULL, ta_napi_host_close, NULL, NULL, NULL, napi_default_method, NULL},
        {"closed", NULL, NULL, ta_napi_host_closed, NULL, NULL, napi_default_jsproperty & ~napi_writable, NULL},
        {"ticksPerSecond", NULL, NULL, ta_napi_host_ticks, NULL, NULL, napi_default_jsproperty & ~napi_writable, NULL},
        {"nextDeadline", NULL, NULL, ta_napi_host_next_deadline, NULL, NULL, napi_default_jsproperty & ~napi_writable, NULL},
        {"idle", NULL, ta_napi_host_idle, NULL, NULL, NULL, napi_default_method, NULL},
        {NULL, dispose, ta_napi_host_close, NULL, NULL, NULL, napi_default_method, NULL},
        {NULL, async_dispose, ta_napi_host_async_dispose, NULL, NULL, NULL, napi_default_method, NULL},
    };
    size_t count = sizeof properties / sizeof properties[0];
    if (async_dispose == NULL) count--; /* a runtime without the symbols: close() only */
    if (dispose == NULL) count = 5;
    napi_value class;
    napi_status status = napi_define_class(env, "Host", NAPI_AUTO_LENGTH, ta_napi_host_new, NULL, count, properties, &class);
    if (status == napi_ok) status = napi_set_named_property(env, exports, "Host", class);
    return status;
}
