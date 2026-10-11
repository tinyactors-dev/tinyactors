/* persist.c — hibernation, export, adopt, migration, snapshots and restore (binding.h). */
#include "binding.h"

/* ---- hibernation, moving actors, migration -------------------------------------------------- */

napi_value js_sweep(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    uint32_t max = get_u32(environment, argv[1]);
    if (!max) max = 1u << 30;
    uint32_t capacity = max < 65536 ? max : 65536;
    uint64_t *sessions = (uint64_t *)malloc(sizeof(uint64_t) * capacity);
    uint32_t n = sessions ? ta_host_sweep(system->host, max, sessions, capacity) : 0;
    napi_create_array_with_length(environment, n, &out);
    for (uint32_t i = 0; i < n; i++)
        napi_set_element(environment, out, i, num(environment, (double)sessions[i]));
    free(sessions);
    return out;
}

static void throw_status(napi_env environment, const char *what, ta_status status,
                         const char *detail) {
    char message[700];
    const char *why = status == TA_ERROR_STATE
                          ? "the actor is not at a stable point (busy, invoking, or queued events)"
                      : status == TA_ERROR_UNSUPPORTED ? detail
                      : status == TA_ERROR_INVALID
                          ? "no such actor, or the blob does not belong here"
                      : status == TA_ERROR_NO_MEMORY ? "out of memory"
                                                     : "failed";
    snprintf(message, sizeof message, "%s: %s", what, why && *why ? why : "cannot be stored");
    napi_throw_error(environment, NULL, message);
}

static ta_status write_hibernate(nd_system *system, uint64_t session, void *b, size_t c,
                                 size_t *l) {
    return ta_host_hibernate(system->host, session, b, c, l);
}

static ta_status write_export(nd_system *system, uint64_t session, void *b, size_t c, size_t *l) {
    return ta_host_export_actor(system->host, session, b, c, l);
}

static ta_status write_blob(nd_system *system, blob_writer writer, uint64_t session, text *out) {
    size_t length = 0;
    ta_status status = TA_ERROR_NO_MEMORY;
    system->error[0] = 0;
    system->keep_quiet = 1;
    int was = enter(system);
    for (int attempt = 0; attempt < 8 && status == TA_ERROR_NO_MEMORY; attempt++) {
        if (!text_room(out, length > out->capacity ? length : 4096)) break;
        status = writer(system, session, out->bytes, out->capacity, &length);
    }
    leave(system, was);
    system->keep_quiet = 0;
    out->length = status == TA_OK ? length : 0;
    return status;
}

/* hibernate(system, session) -> blob; throws (saying why) when it cannot */
napi_value js_hibernate(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    text blob = {0};
    ta_status status =
        write_blob(system, write_hibernate, get_session(environment, argv[1]), &blob);
    napi_value r = status == TA_OK ? bytes_value(environment, blob.bytes, blob.length) : NULL;
    if (status != TA_OK)
        throw_status(environment, "cannot hibernate the actor", status, system->error);
    free(blob.bytes);
    return r;
}

/* wake(system, session, blob): the actor is back; its work runs with the next pump */
napi_value js_wake(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    void *data = NULL;
    size_t n = 0;
    if (!system) return NULL;
    if (!bytes_of(environment, argv[2], &data, &n)) {
        napi_throw_type_error(environment, NULL, "wake(system, session, blob: Uint8Array)");
        return NULL;
    }
    void *aligned = malloc(n ? n : 1); /* the blob is read in place */
    if (!aligned) abort();
    memcpy(aligned, data, n);
    int was = enter(system);
    ta_status status = ta_host_wake(system->host, get_session(environment, argv[1]), aligned, n);
    leave(system, was);
    free(aligned);
    if (status != TA_OK) {
        throw_status(environment, "cannot wake the actor", status, NULL);
        return NULL;
    }
    return undef(environment); /* its work runs with the next pump */
}

/* exportActor(system, session) -> blob: the actor leaves; its delayed sends
 * are due relative to time 0 (ta_blob_rebase), so any clock adopts it */
napi_value js_export_actor(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    text blob = {0};
    ta_time now = (ta_time)system_now(system);
    ta_host_advance_time(system->host, now);
    ta_status status = write_blob(system, write_export, get_session(environment, argv[1]), &blob);
    if (status == TA_OK) status = ta_blob_rebase(blob.bytes, blob.length, now, 0);
    napi_value r = status == TA_OK ? bytes_value(environment, blob.bytes, blob.length) : NULL;
    if (status != TA_OK)
        throw_status(environment, "cannot export the actor", status, system->error);
    free(blob.bytes);
    return r;
}

/* adopt(system, blob) -> session: into the newest segment whose image the blob belongs to */
napi_value js_adopt(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    void *data = NULL;
    size_t n = 0;
    if (!system) return NULL;
    if (!bytes_of(environment, argv[1], &data, &n)) {
        napi_throw_type_error(environment, NULL, "adopt(system, blob: Uint8Array)");
        return NULL;
    }
    void *copy = malloc(n ? n : 1);
    if (!copy) abort();
    memcpy(copy, data, n);
    ta_time now = (ta_time)system_now(system);
    ta_status status = ta_blob_rebase(copy, n, 0, now);
    uint64_t session = 0;
    ta_host_advance_time(system->host, now);
    int was = enter(system);
    for (uint32_t k = ta_host_segment_count(system->host); status == TA_OK && k-- > 0;) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        if (ta_host_segment_info_get(system->host, k, &segment_info) != TA_OK || !segment_info.live)
            continue;
        status = ta_host_adopt(system->host, k, copy, n, &session);
        if (status != TA_ERROR_INVALID) break;
        status = TA_OK;
    }
    leave(system, was);
    free(copy);
    if (status != TA_OK || !session) {
        throw_status(environment, "cannot adopt the actor",
                     status == TA_OK ? TA_ERROR_INVALID : status, "");
        return NULL;
    }
    return num(environment, (double)session); /* it goes on with the next pump */
}

/* migrate(system, name, session, wake, timeout) -> job */
napi_value js_migrate(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 5 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t nl = 0;
    char *name = optional_string(environment, argv[1], &nl);
    bool wake = false;
    napi_get_value_bool(environment, argv[3], &wake);
    uint32_t job = 0;
    ta_status status = ta_host_migrate(
        system->host, view_of(name, nl), get_session(environment, argv[2]),
        wake ? TA_HOST_MIGRATE_WAKE : 0, (ta_time)get_double(environment, argv[4]), &job);
    free(name);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot start the migration");
        return NULL;
    }
    return num(environment, job);
}

/* ---- snapshots ------------------------------------------------------------------------------ */

/* The native datamodel's tables as neutral JSON: {"i": [[handle, serial, hash,
 * chart, state], ...], "p": [[handle, refs, value], ...]} */
static ta_status native_snapshot(nd_system *system, text *t) {
    napi_env environment = system->environment;
    text state = {0};
    t->length = 0;
    text_put(t, "{\"i\":[", 6);
    int first = 1;
    for (uint32_t i = 0; i < system->instance_count; i++) {
        nd_instance *instance = &system->instances[i];
        if (!instance->live) continue;
        ta_status status = scope_wire(system, get_ref(environment, instance->scope), &state);
        if (status != TA_OK) {
            free(state.bytes);
            return status;
        }
        char head[96];
        int n = snprintf(head, sizeof head, "%s[%u,\"%llu\",\"%016llx\",%u,", first ? "" : ",",
                         i + 1, (unsigned long long)instance->serial,
                         (unsigned long long)instance->chart->hash, instance->chart->chart);
        first = 0;
        text_put(t, head, (size_t)n);
        text_put(t, state.bytes, state.length);
        text_char(t, ']');
    }
    free(state.bytes);
    text_put(t, "],\"p\":[", 7);
    first = 1;
    for (uint32_t i = 0; i < system->payload_count; i++) {
        if (!system->payload_refs[i]) continue;
        char head[48];
        int n = snprintf(head, sizeof head, "%s[%u,%u,", first ? "" : ",", i + 1,
                         system->payload_refs[i]);
        first = 0;
        text_put(t, head, (size_t)n);
        if (wire_value(environment, get_ref(environment, system->payloads[i]), t, 1) != TA_OK) {
            snprintf(system->error, sizeof system->error,
                     "a queued event's data is not a neutral value");
            return TA_ERROR_UNSUPPORTED;
        }
        text_char(t, ']');
    }
    text_put(t, "]}", 2);
    return t->failed ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* snapshot(system) -> { host, datamodel, segments: [image | null] } (quiescent: no migration) */
napi_value js_snapshot(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out, segments;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t length = 0;
    ta_status status = ta_host_snapshot(system->host, NULL, 0, &length);
    void *host =
        status == TA_OK || status == TA_ERROR_NO_MEMORY ? malloc(length ? length : 1) : NULL;
    if (host) status = ta_host_snapshot(system->host, host, length, &length);
    if (!host || status != TA_OK) {
        free(host);
        throw_status(environment, "snapshot", status == TA_OK ? TA_ERROR_NO_MEMORY : status, "");
        return NULL;
    }
    text datamodel = {0};
    system->error[0] = 0;
    if (system->native) {
        status = native_snapshot(system, &datamodel);
    } else {
        size_t dl = 0;
        status = ta_datamodel_ecmascript_snapshot(system->quickjs, NULL, 0, &dl);
        if ((status == TA_OK || status == TA_ERROR_NO_MEMORY) && text_room(&datamodel, dl + 1))
            status = ta_datamodel_ecmascript_snapshot(system->quickjs, datamodel.bytes,
                                                      datamodel.capacity, &dl);
        datamodel.length = dl;
        if (status == TA_ERROR_UNSUPPORTED)
            snprintf(system->error, sizeof system->error, "%s",
                     ta_datamodel_ecmascript_last_error(system->quickjs));
    }
    if (status != TA_OK) {
        free(host);
        free(datamodel.bytes);
        throw_status(environment, "snapshot", status, system->error);
        return NULL;
    }
    napi_create_object(environment, &out);
    set(environment, out, "host", bytes_value(environment, host, length));
    set(environment, out, "datamodel", bytes_value(environment, datamodel.bytes, datamodel.length));
    free(host);
    free(datamodel.bytes);
    uint32_t count = ta_host_segment_count(system->host);
    napi_create_array_with_length(environment, count, &segments);
    for (uint32_t k = 0; k < count; k++) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        int live = ta_host_segment_info_get(system->host, k, &segment_info) == TA_OK &&
                   segment_info.live && k < system->image_count && system->images[k];
        napi_set_element(environment, segments, k,
                         live ? get_ref(environment, system->images[k]) : null_value(environment));
    }
    set(environment, out, "segments", segments);
    return out;
}

static nd_chart *chart_by_hash(nd_system *system, uint64_t hash, uint32_t chart) {
    for (uint32_t i = 0; i < system->chart_count; i++)
        if (system->charts[i]->hash == hash && system->charts[i]->chart == chart)
            return system->charts[i];
    return NULL;
}

static uint64_t hex_of(napi_env environment, napi_value v) {
    size_t n = 0;
    char *t = get_string(environment, v, &n);
    uint64_t high = 0, low = 0;
    if (!t || !hex_text(t, n, &high, &low)) low = 0;
    free(t);
    return low;
}

/* The native datamodel's tables back at their handles. */
static ta_status native_restore(nd_system *system, ta_string_view json) {
    napi_env environment = system->environment;
    napi_value tables = value_of_wire(environment, json, get_ref(environment, system->refs));
    if (!tables) return TA_ERROR_INVALID;
    napi_value instances = property(environment, tables, "i"),
               payloads = property(environment, tables, "p");
    uint32_t n = array_length(environment, instances), max = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, v;
        napi_get_element(environment, instances, i, &row);
        napi_get_element(environment, row, 0, &v);
        uint32_t handle = get_u32(environment, v);
        if (handle > max) max = handle;
    }
    grow((void **)&system->instances, &system->instance_capacity, max, sizeof(nd_instance));
    system->instance_count = max;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, handle, serial, hash, chart, state;
        napi_get_element(environment, instances, i, &row);
        napi_get_element(environment, row, 0, &handle);
        napi_get_element(environment, row, 1, &serial);
        napi_get_element(environment, row, 2, &hash);
        napi_get_element(environment, row, 3, &chart);
        napi_get_element(environment, row, 4, &state);
        nd_chart *c = chart_by_hash(system, hex_of(environment, hash), get_u32(environment, chart));
        if (!c) return TA_ERROR_INVALID;
        uint32_t h = get_u32(environment, handle);
        nd_instance *instance = &system->instances[h - 1];
        memset(instance, 0, sizeof *instance);
        instance->live = 1;
        instance->chart = c;
        instance->serial = decimal_of(environment, serial);
        char sessionid[24];
        snprintf(sessionid, sizeof sessionid, "%llu", (unsigned long long)instance->serial);
        napi_value io;
        napi_create_object(environment, &io);
        for (uint32_t k = 0; k < system->io_count; k++) { /* as the system advertises them */
            napi_value entry;
            napi_create_object(environment, &entry);
            set(environment, entry, "location", str_value(environment, system->io[k].location));
            napi_set_property(environment, io, str_value(environment, system->io[k].type), entry);
        }
        napi_value scope =
            scope_new(system, str_value(environment, view_of(sessionid, strlen(sessionid))),
                      undef(environment), io);
        copy_properties(environment, scope, property(environment, state, "v"));
        napi_value event = property(environment, state, "e");
        if (type_of(environment, event) == napi_object)
            define_hidden(environment, scope, "_event", event, 0);
        instance->scope = make_ref(environment, scope);
        system->live_instances++;
    }
    system->free_instance = 0;
    for (uint32_t i = max; i-- > 0;)
        if (!system->instances[i].live) {
            system->instances[i].next_free = system->free_instance;
            system->free_instance = i + 1;
        }
    n = array_length(environment, payloads);
    max = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, v;
        napi_get_element(environment, payloads, i, &row);
        napi_get_element(environment, row, 0, &v);
        if (get_u32(environment, v) > max) max = get_u32(environment, v);
    }
    uint32_t c = system->payload_capacity;
    grow((void **)&system->payloads, &c, max, sizeof(napi_ref));
    grow((void **)&system->payload_refs, &system->payload_capacity, max, sizeof(uint32_t));
    system->payload_count = max;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, handle, refs, value;
        napi_get_element(environment, payloads, i, &row);
        napi_get_element(environment, row, 0, &handle);
        napi_get_element(environment, row, 1, &refs);
        napi_get_element(environment, row, 2, &value);
        uint32_t h = get_u32(environment, handle);
        system->payloads[h - 1] = make_ref(environment, value);
        system->payload_refs[h - 1] = get_u32(environment, refs);
        system->live_payloads++;
    }
    system->free_payload = 0;
    for (uint32_t i = max; i-- > 0;)
        if (!system->payload_refs[i]) {
            system->payloads[i] = (napi_ref)(uintptr_t)system->free_payload;
            system->free_payload = i + 1;
        }
    return TA_OK;
}

/* restoreSystem(callbacks, config, images: [image | null], host, datamodel) -> system */
napi_value js_restore_system(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *host = NULL, *datamodel = NULL;
    size_t hl = 0, dl = 0;
    if (argc < 5 || !bytes_of(environment, argv[3], &host, &hl) ||
        !bytes_of(environment, argv[4], &datamodel, &dl)) {
        napi_throw_type_error(environment, NULL,
                              "restoreSystem(callbacks, config, images, host, datamodel)");
        return NULL;
    }
    uint32_t n = array_length(environment, argv[2]);
    void *aligned = malloc(hl ? hl : 1); /* the snapshot's records are read in place */
    if (!aligned) abort();
    memcpy(aligned, host, hl);
    /* the images it needs: one per segment, by content hash (0: retired) */
    uint32_t segments = ta_host_snapshot_segments(aligned, hl, NULL, 0);
    uint64_t *hashes = (uint64_t *)calloc(segments + 1, sizeof(uint64_t));
    if (segments) ta_host_snapshot_segments(aligned, hl, hashes, segments);
    for (uint32_t k = 0; k < segments; k++) {
        napi_value v = NULL;
        nd_image *image = NULL;
        ta_image_info image_info;
        image_info.size = sizeof image_info;
        if (k < n) napi_get_element(environment, argv[2], k, &v);
        if (type_of(environment, v) == napi_external) {
            napi_get_value_external(environment, v, (void **)&image);
            if (image) ta_image_get_info(&image->image, &image_info);
        }
        if (hashes[k] && (!image || image_info.content_hash != hashes[k])) {
            char message[160];
            snprintf(
                message, sizeof message,
                "restore: segment %u ran the image %016llx, which is not among the images given", k,
                (unsigned long long)hashes[k]);
            free(hashes);
            free(aligned);
            napi_throw_error(environment, NULL, message);
            return NULL;
        }
    }
    free(hashes);
    if (!segments) {
        free(aligned);
        napi_throw_type_error(environment, NULL, "restore: not a host snapshot");
        return NULL;
    }
    ta_host_config config;
    nd_system *system = system_new(environment, argv[0], argv[1], &config);
    if (!system) {
        free(aligned);
        return NULL;
    }
    const ta_image **images = (const ta_image **)calloc(n + 1, sizeof *images);
    for (uint32_t k = 0; k < n; k++) {
        napi_value v;
        napi_get_element(environment, argv[2], k, &v);
        if (type_of(environment, v) != napi_external) continue;
        nd_image *image = get_image(environment, v);
        if (!image) break;
        images[k] = &image->image;
        keep_image(system, k, v);
    }
    system->running = 1;
    ta_status status = ta_host_restore(&config, images, n, aligned, hl, &system->host);
    free(aligned);
    const char *step = "the host's snapshot does not match these images or this build";
    if (status == TA_OK) {
        step = "the datamodel's snapshot does not match";
        if (system->native) {
            status = native_restore(system, view_of((const char *)datamodel, dl));
        } else {
            status = ta_datamodel_ecmascript_restore(system->quickjs, datamodel, dl);
            uint64_t cursor = 0, batch[256]; /* what the binding knows of each session */
            uint32_t count;
            while (status == TA_OK &&
                   (count = ta_host_actors(system->host, &cursor, batch, 256)) > 0)
                for (uint32_t i = 0; i < count; i++) {
                    ta_system *segment_system;
                    uint32_t segment = 0;
                    ta_actor_info actor_info;
                    ta_host_segment_info segment_info;
                    segment_info.size = sizeof segment_info;
                    if (locate(system, batch[i], &segment_system, &segment, &actor_info) &&
                        actor_info.datamodel_instance &&
                        ta_host_segment_info_get(system->host, segment, &segment_info) == TA_OK)
                        session_put(system, actor_info.datamodel_instance, actor_info.serial,
                                    segment_info.image, actor_info.chart);
                }
        }
    }
    system->running = 0;
    free(images);
    if (status != TA_OK) {
        system_finalize(environment, system, NULL);
        char message[160];
        snprintf(message, sizeof message, "restore: %s (%d)", step, status);
        napi_throw_error(environment, NULL, message);
        return NULL;
    }
    return system_value(environment, system);
}
