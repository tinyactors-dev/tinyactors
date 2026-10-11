/* quickjs.c — the quickjs "ecmascript" datamodel, forwarded (binding.h). */
#include "binding.h"

/* ---- the quickjs "ecmascript" datamodel (src/datamodel_ecmascript), forwarded: the
 * binding only notes which sessions saw an event and keeps their last variables */

static uint32_t map_slot(nd_system *system, uint64_t key) {
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (system->map_capacity - 1);
    while (system->keys[i] && system->keys[i] != key)
        i = (i + 1) & (system->map_capacity - 1);
    return i;
}

static nd_session *session_at(nd_system *system, uint64_t handle) {
    if (!system->map_capacity || !handle) return NULL;
    uint32_t i = map_slot(system, handle);
    return system->keys[i] == handle && system->sessions[i].image ? &system->sessions[i] : NULL;
}

void session_put(nd_system *system, uint64_t handle, uint64_t serial, const ta_image *image,
                 uint32_t chart) {
    if ((system->map_count + 1) * 2 > system->map_capacity) {
        uint64_t *keys = system->keys;
        nd_session *sessions = system->sessions;
        uint32_t old = system->map_capacity;
        system->map_capacity = old ? old * 2 : 64;
        system->keys = (uint64_t *)calloc(system->map_capacity, sizeof(uint64_t));
        system->sessions = (nd_session *)calloc(system->map_capacity, sizeof(nd_session));
        if (!system->keys || !system->sessions) abort();
        system->map_count = 0;
        for (uint32_t i = 0; i < old; i++)
            if (keys[i] && sessions[i].image) {
                uint32_t j = map_slot(system, keys[i]);
                system->keys[j] = keys[i];
                system->sessions[j] = sessions[i];
                system->map_count++;
            }
        free(keys);
        free(sessions);
    }
    uint32_t i = map_slot(system, handle);
    if (!system->keys[i]) system->map_count++;
    system->keys[i] = handle;
    system->sessions[i] = (nd_session){serial, image, chart, 0};
}

/* A session's variables as a JS value, read with the C datamodel's eval_json:
 * an object of its <data> ids (undefined ones left out). */
napi_value quickjs_variables(nd_system *system, uint64_t handle, const ta_image *image,
                             uint32_t chart) {
    napi_env environment = system->environment;
    text expression = {0};
    text_put(&expression, "({", 2);
    uint32_t n = ta_image_data_count(image, chart);
    for (uint32_t i = 0; i < n; i++) {
        ta_data_info data;
        if (ta_image_data(image, chart, i, &data) != TA_OK) continue;
        if (i) text_char(&expression, ',');
        text_string(&expression, data.id.pointer, data.id.length);
        text_put(&expression, ":typeof ", 8);
        text_put(&expression, data.id.pointer, data.id.length);
        text_put(&expression, "==='undefined'?undefined:", 25);
        text_put(&expression, data.id.pointer, data.id.length);
    }
    text_put(&expression, "})", 2);
    napi_value result = NULL;
    if (!expression.failed) {
        size_t capacity = 4096;
        for (int attempt = 0; attempt < 8 && !result; attempt++) {
            char *buffer = (char *)malloc(capacity);
            if (!buffer) break;
            size_t length = ta_datamodel_ecmascript_eval_json(
                system->quickjs, handle, view_of(expression.bytes, expression.length), buffer,
                capacity);
            if (length == (size_t)-1) {
                free(buffer);
                break;
            }
            if (length < capacity)
                result = value_of_wire(environment, view_of(buffer, length), NULL);
            else
                capacity = length + 1;
            free(buffer);
        }
    }
    free(expression.bytes);
    if (!result) napi_create_object(environment, &result);
    return result;
}

static ta_status q_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = chart_new(chart);
    if (!c) return TA_ERROR_NO_MEMORY;
    ta_status status = Q(system)->chart_attach(system->quickjs, chart, &c->inner);
    if (status != TA_OK) {
        free(c);
        return status;
    }
    chart_add(system, c);
    *context = c;
    return TA_OK;
}

static void q_detach(void *d, void *context) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = (nd_chart *)context;
    if (!c) return;
    Q(system)->chart_detach(system->quickjs, c->inner);
    chart_remove(system, c);
    free(c);
}

static ta_status q_create(void *d, void *context, const ta_datamodel_system_variables *sv,
                          ta_payload init, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = (nd_chart *)context;
    ta_status status = Q(system)->create(system->quickjs, c->inner, sv, init, out);
    if (status == TA_OK) {
        session_put(system, *out, serial_of(sv->sessionid), c->image, c->chart);
        nd_session *s = session_at(system, *out);
        if (s) mark_changed(system, &s->stamp, s->serial);
    }
    return status;
}

static void q_destroy(void *d, uint64_t handle) {
    nd_system *system = (nd_system *)d;
    nd_session *s = session_at(system, handle);
    if (s) {
        if (!system->keep_quiet)
            grave_add(system, s->serial, quickjs_variables(system, handle, s->image, s->chart));
        s->image = NULL; /* a tombstone: keeps the probe chains */
    }
    Q(system)->destroy(system->quickjs, handle);
}

static ta_status q_init_data(void *d, uint64_t handle, ta_datamodel_environment *outside,
                             uint32_t declaration) {
    nd_system *system = (nd_system *)d;
    return Q(system)->init_data(system->quickjs, handle, outside, declaration);
}

static ta_status q_bind_event(void *d, uint64_t handle, ta_datamodel_environment *outside,
                              const ta_event_view *event) {
    nd_system *system = (nd_system *)d;
    nd_session *s = session_at(system, handle);
    if (s) mark_changed(system, &s->stamp, s->serial);
    return Q(system)->bind_event(system->quickjs, handle, outside, event);
}

static void q_detach_event(void *d, uint64_t handle) {
    nd_system *system = (nd_system *)d;
    if (Q(system)->detach_event) Q(system)->detach_event(system->quickjs, handle);
}

static ta_status q_condition(void *d, uint64_t handle, ta_datamodel_environment *outside,
                             ta_expression e, int *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->eval_condition(system->quickjs, handle, outside, e, out);
}

static ta_status q_string(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          ta_expression e, ta_string_view *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->eval_string(system->quickjs, handle, outside, e, out);
}

static ta_status q_assign(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          ta_expression location, ta_expression value) {
    nd_system *system = (nd_system *)d;
    return Q(system)->assign(system->quickjs, handle, outside, location, value);
}

static ta_status q_assign_string(void *d, uint64_t handle, ta_expression location,
                                 ta_string_view value) {
    nd_system *system = (nd_system *)d;
    return Q(system)->assign_string(system->quickjs, handle, location, value);
}

static ta_status q_script(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          ta_expression e) {
    nd_system *system = (nd_system *)d;
    return Q(system)->script(system->quickjs, handle, outside, e);
}

static ta_status q_log(void *d, uint64_t handle, ta_datamodel_environment *outside, ta_expression e,
                       ta_string_view *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->log(system->quickjs, handle, outside, e, out);
}

static ta_status q_foreach_begin(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                 ta_expression array, uint64_t *iterator, uint32_t *n) {
    nd_system *system = (nd_system *)d;
    return Q(system)->foreach_begin(system->quickjs, handle, outside, array, iterator, n);
}

static ta_status q_foreach_item(void *d, uint64_t handle, uint64_t iterator, uint32_t i,
                                ta_expression item, ta_expression index) {
    nd_system *system = (nd_system *)d;
    return Q(system)->foreach_item(system->quickjs, handle, iterator, i, item, index);
}

static void q_foreach_end(void *d, uint64_t handle, uint64_t iterator) {
    nd_system *system = (nd_system *)d;
    Q(system)->foreach_end(system->quickjs, handle, iterator);
}

static ta_status q_capture(void *d, uint64_t handle, ta_datamodel_environment *outside,
                           const ta_datamodel_capture *capture, ta_payload *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->capture(system->quickjs, handle, outside, capture, out);
}

static void q_retain(void *d, ta_payload p) {
    nd_system *system = (nd_system *)d;
    Q(system)->retain(system->quickjs, p);
}

static void q_release(void *d, ta_payload p) {
    nd_system *system = (nd_system *)d;
    Q(system)->release(system->quickjs, p);
}

static ta_status q_custom(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          uint32_t custom) {
    nd_system *system = (nd_system *)d;
    return Q(system)->custom ? Q(system)->custom(system->quickjs, handle, outside, custom)
                             : TA_ERROR_UNSUPPORTED;
}

static ta_status q_serialize(void *d, uint64_t handle, void *buffer, size_t capacity,
                             size_t *length) {
    nd_system *system = (nd_system *)d;
    ta_status status = Q(system)->serialize(system->quickjs, handle, buffer, capacity, length);
    if (status == TA_ERROR_UNSUPPORTED)
        snprintf(system->error, sizeof system->error, "%s",
                 ta_datamodel_ecmascript_last_error(system->quickjs));
    return status;
}

static ta_status q_deserialize(void *d, void *context, const ta_datamodel_system_variables *sv,
                               const void *buffer, size_t length, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = (nd_chart *)context;
    ta_status status = Q(system)->deserialize(system->quickjs, c->inner, sv, buffer, length, out);
    if (status == TA_OK) session_put(system, *out, serial_of(sv->sessionid), c->image, c->chart);
    return status;
}

static ta_status q_migrate(void *d, uint64_t handle, ta_datamodel_environment *outside,
                           ta_payload *bindings) {
    nd_system *system = (nd_system *)d;
    return Q(system)->migrate(system->quickjs, handle, outside, bindings);
}

static ta_status q_to_wire(void *d, ta_payload p, char *buffer, size_t capacity, size_t *length) {
    nd_system *system = (nd_system *)d;
    return Q(system)->to_wire(system->quickjs, p, buffer, capacity, length);
}

static ta_status q_from_wire(void *d, ta_string_view json, ta_payload *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->from_wire(system->quickjs, json, out);
}

static ta_status q_payload_serialize(void *d, ta_payload p, void *buffer, size_t capacity,
                                     size_t *length) {
    nd_system *system = (nd_system *)d;
    return Q(system)->payload_serialize(system->quickjs, p, buffer, capacity, length);
}

static ta_status q_payload_deserialize(void *d, const void *buffer, size_t length,
                                       ta_payload *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->payload_deserialize(system->quickjs, buffer, length, out);
}

const ta_datamodel_vtable quickjs_vtable = {sizeof(ta_datamodel_vtable),
                                            TA_DATAMODEL_EVENT_DETACHABLE,
                                            "ecmascript",
                                            q_attach,
                                            q_detach,
                                            q_create,
                                            q_destroy,
                                            q_init_data,
                                            q_bind_event,
                                            q_detach_event,
                                            q_condition,
                                            q_string,
                                            q_assign,
                                            q_assign_string,
                                            q_script,
                                            q_log,
                                            q_foreach_begin,
                                            q_foreach_item,
                                            q_foreach_end,
                                            q_capture,
                                            q_retain,
                                            q_release,
                                            q_custom,
                                            q_serialize,
                                            q_deserialize,
                                            q_migrate,
                                            q_to_wire,
                                            q_from_wire,
                                            q_payload_serialize,
                                            q_payload_deserialize};
