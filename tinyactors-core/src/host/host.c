/* host.c — many systems as one actor system (see include/tinyactors/host.h).
 *
 * Hosted layer (malloc). Segments are ta_systems sharing a base URI with
 * TA_IO_PROCESSOR_SCXML_SHARED on their remote processor (index 0), so sends to a
 * session of another segment come out as TA_EFFECT_SEND effects, which this layer
 * delivers itself. Effects that leave the host are copied (strings into a
 * pool, payloads retained) so segments can be acknowledged right away and
 * keep running; the copies live until ta_host_effects_ack / the next run. */
#include <tinyactors/address.h>
#include <tinyactors/host.h>

#include <stdlib.h>
#include <string.h>

#include "host/host_internal.h"

/* ---- small utilities -------------------------------------------------------------- */

void *grow(void *p, uint32_t *capacity, uint32_t need, size_t elem) {
    if (need <= *capacity) return p;
    uint32_t c = *capacity ? *capacity : 8;
    while (c < need)
        c *= 2;
    void *q = realloc(p, elem * c);
    if (!q) return NULL;
    memset((char *)q + elem * *capacity, 0, elem * (c - *capacity));
    *capacity = c;
    return q;
}

static int string_equal(ta_string_view a, const char *b, uint32_t n) {
    return a.length == n && (!n || memcmp(a.pointer, b, n) == 0);
}

static ta_string_view chart_key(const ta_image *image, uint32_t chart) {
    ta_string_view n = ta_image_chart_name(image, chart);
    return n.length ? n : ta_image_chart_source(image, chart);
}

static host_segment *segment_of(const ta_host *host, uint32_t i) {
    return i < host->segment_count && host->segments[i].live ? &host->segments[i] : NULL;
}

/* ---- moved sessions ------------------------------------------------------------------ */

static uint32_t mhash(uint64_t k, uint32_t capacity) {
    return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1);
}

static void moved_put(ta_host *host, uint64_t key, uint32_t value) { /* value TA_NIL: remove */
    if (value != TA_NIL && (host->moved_count + 1) * 2 > host->moved_capacity) {
        uint32_t oc = host->moved_capacity, nc = oc ? oc * 2 : 64;
        uint64_t *ok = host->moved_keys;
        uint32_t *ov = host->moved_values;
        host->moved_keys = (uint64_t *)calloc(nc, sizeof(uint64_t));
        host->moved_values = (uint32_t *)calloc(nc, sizeof(uint32_t));
        host->moved_capacity = nc;
        host->moved_count = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (ok[i] && ov[i] != TA_NIL) moved_put(host, ok[i], ov[i]);
        free(ok);
        free(ov);
    }
    if (!host->moved_capacity) return;
    uint32_t i = mhash(key, host->moved_capacity);
    while (host->moved_keys[i] && host->moved_keys[i] != key)
        i = (i + 1) & (host->moved_capacity - 1);
    if (host->moved_keys[i] == key) { /* existing (tombstones keep the key, value TA_NIL) */
        if (host->moved_values[i] == TA_NIL && value != TA_NIL) host->moved_count++;
        if (host->moved_values[i] != TA_NIL && value == TA_NIL) host->moved_count--;
        host->moved_values[i] = value;
    } else if (value != TA_NIL) {
        host->moved_keys[i] = key;
        host->moved_values[i] = value;
        host->moved_count++;
    }
}

uint32_t moved_get(const ta_host *host, uint64_t key) {
    if (!host->moved_capacity) return TA_NIL;
    uint32_t i = mhash(key, host->moved_capacity);
    while (host->moved_keys[i]) {
        if (host->moved_keys[i] == key) return host->moved_values[i];
        i = (i + 1) & (host->moved_capacity - 1);
    }
    return TA_NIL;
}

/* ---- segments ---------------------------------------------------------------------- */

static int segment_grow(host_segment *segment) {
    ta_system_config config = segment->config;
    config.max_actors =
        config.max_actors * 2 > TA_SYSTEM_MAX_ACTORS ? TA_SYSTEM_MAX_ACTORS : config.max_actors * 2;
    config.max_events =
        config.max_events * 2 > TA_SYSTEM_MAX_POOL ? TA_SYSTEM_MAX_POOL : config.max_events * 2;
    config.max_timers =
        config.max_timers * 2 > TA_SYSTEM_MAX_POOL ? TA_SYSTEM_MAX_POOL : config.max_timers * 2;
    config.max_requests = config.max_requests * 2;
    config.workspaces = config.workspaces * 2;
    config.instance_bytes = 0; /* derived from max_actors */
    config.string_bytes = config.string_bytes * 2;
    size_t need;
    if (ta_system_layout(segment->image, &config, &need) != TA_OK) return 0;
    void *memory = malloc(need);
    ta_system *new_system = NULL;
    if (!memory ||
        ta_system_relocate(segment->system, memory, need, &config, &new_system) != TA_OK) {
        free(memory);
        return 0;
    }
    free(segment->memory);
    segment->memory = memory;
    segment->system = new_system;
    segment->config = config;
    return 1;
}

static void versions_recompute(ta_host *host) {
    for (uint32_t i = 0; i < host->version_count; i++)
        free(host->versions[i].name);
    host->version_count = 0;
    for (uint32_t si = 0; si < host->segment_count; si++) { /* newer segments win */
        host_segment *segment = segment_of(host, si);
        if (!segment) continue;
        ta_image_info image_info;
        image_info.size = sizeof image_info;
        ta_image_get_info(segment->image, &image_info);
        for (uint32_t c = 0; c < image_info.chart_count; c++) {
            ta_string_view k = chart_key(segment->image, c);
            uint32_t v = 0;
            while (v < host->version_count &&
                   !string_equal(k, host->versions[v].name, host->versions[v].length))
                v++;
            if (v == host->version_count) {
                chart_version *nv =
                    (chart_version *)grow(host->versions, &host->version_capacity,
                                          host->version_count + 1, sizeof(chart_version));
                if (!nv) return;
                host->versions = nv;
                chart_version *e = &host->versions[host->version_count++];
                e->name = (char *)malloc(k.length + 1);
                if (k.length) memcpy(e->name, k.pointer, k.length);
                e->name[k.length] = 0;
                e->length = k.length;
            }
            host->versions[v].segment = si;
            host->versions[v].chart = c;
        }
    }
}

static const chart_version *version_of(const ta_host *host, ta_string_view name) {
    for (uint32_t v = 0; v < host->version_count; v++)
        if (string_equal(name, host->versions[v].name, host->versions[v].length))
            return &host->versions[v];
    return NULL;
}

ta_status ta_host_new(const ta_host_config *config, ta_host **out) {
    *out = NULL;
    if (!config || config->datamodel_count > 8 || config->io_processor_count > 16 ||
        !ta_address_system_valid(config->system_id))
        return TA_ERROR_INVALID;
    ta_host *host = (ta_host *)calloc(1, sizeof *host);
    if (!host) return TA_ERROR_NO_MEMORY;
    memcpy(&host->config, config,
           config->size && config->size < sizeof host->config ? config->size : sizeof host->config);
    host->config.size = sizeof host->config;
    if (!host->config.migrate_quantum) host->config.migrate_quantum = 256;
    /* the base URI every segment shares: tinyactors://<system_id>/ */
    uint32_t bl = 0;
    memcpy(host->base, TA_ADDRESS_SCHEME, 13);
    bl = 13;
    memcpy(host->base + bl, config->system_id.pointer, config->system_id.length);
    bl += config->system_id.length;
    host->base[bl++] = '/';
    host->base_length = bl;
    host->config.system_id = (ta_string_view){host->base + 13, config->system_id.length};
    for (uint32_t i = 0; i < config->datamodel_count; i++)
        host->datamodels[i] = config->datamodels[i];
    host->config.datamodels = host->datamodels;
    host->io_processors[0].type = (ta_string_view){"tinyactors", 10};
    host->io_processors[0].location = (ta_string_view){host->base, host->base_length};
    host->io_processors[0].flags = TA_IO_PROCESSOR_SCXML_REMOTE | TA_IO_PROCESSOR_SCXML_SHARED;
    for (uint32_t i = 0; i < config->io_processor_count; i++)
        host->io_processors[1 + i] = config->io_processors[i];
    host->io_processor_count = 1 + config->io_processor_count;
    host->draining = TA_NIL;
    *out = host;
    return TA_OK;
}

static void effects_release(ta_host *h);

void ta_host_free(ta_host *host) {
    if (!host) return;
    host->effects_ready = host->effect_count; /* all of them */
    effects_release(host);
    ta_router_free(host);
    for (uint32_t i = 0; i < host->segment_count; i++)
        if (host->segments[i].live) {
            ta_system_deinit(host->segments[i].system);
            free(host->segments[i].memory);
        }
    free(host->segments);
    for (uint32_t i = 0; i < host->version_count; i++)
        free(host->versions[i].name);
    free(host->versions);
    for (uint32_t i = 0; i < host->job_count; i++) {
        free(host->jobs[i].name);
        free(host->jobs[i].retry);
        free(host->jobs[i].woken);
        free(host->jobs[i].left);
    }
    free(host->jobs);
    free(host->moved_keys);
    free(host->moved_values);
    free(host->effects);
    free(host->effect_strings);
    free(host->pool);
    free(host->blob);
    free(host->blob2);
    free(host->errors);
    free(host);
}

static void reserve_serials(ta_host *h, uint32_t index);

ta_status ta_host_add_image(ta_host *host, const ta_image *image, uint32_t *segment) {
    uint32_t index = host->segment_count;
    if (index >= (1u << 20)) return TA_ERROR_NO_MEMORY; /* session ids stay exact in a double */
    host_segment *new_segments = (host_segment *)grow(host->segments, &host->segment_capacity,
                                                      index + 1, sizeof(host_segment));
    if (!new_segments) return TA_ERROR_NO_MEMORY;
    host->segments = new_segments;
    host_segment *segment_record = &host->segments[index];
    memset(segment_record, 0, sizeof *segment_record);
    segment_record->image = image;
    segment_record->config = host->config.system;
    segment_record->config.size = sizeof segment_record->config;
    if (!segment_record->config.max_actors) segment_record->config.max_actors = 64;
    if (!segment_record->config.max_events) segment_record->config.max_events = 256;
    if (!segment_record->config.max_timers) segment_record->config.max_timers = 64;
    if (!segment_record->config.max_requests) segment_record->config.max_requests = 16;
    if (!segment_record->config.workspaces) segment_record->config.workspaces = 4;
    if (!segment_record->config.string_bytes) segment_record->config.string_bytes = 32768;
    segment_record->config.io_processors = host->io_processors;
    segment_record->config.io_processor_count = host->io_processor_count;
    segment_record->config.first_serial = (uint64_t)index << SERIALS_SHIFT;
    size_t need;
    ta_status status = ta_system_layout(image, &segment_record->config, &need);
    if (status != TA_OK) return status;
    segment_record->memory = malloc(need);
    if (!segment_record->memory) return TA_ERROR_NO_MEMORY;
    status =
        ta_system_init(segment_record->memory, need, image, &segment_record->config,
                       host->datamodels, host->config.datamodel_count, &segment_record->system);
    if (status != TA_OK) {
        free(segment_record->memory);
        segment_record->memory = NULL;
        return status;
    }
    ta_system_advance_time(segment_record->system, host->now);
    segment_record->live = 1;
    segment_record->busy = 1;
    segment_record->deadline = UINT64_MAX;
    host->segment_count++;
    reserve_serials(host, index);
    versions_recompute(host);
    if (segment) *segment = index;
    return TA_OK;
}

ta_status ta_host_chart(const ta_host *host, ta_string_view name, uint32_t *segment,
                        uint32_t *chart) {
    const chart_version *version = version_of(host, name);
    if (!version) return TA_ERROR_INVALID;
    if (segment) *segment = version->segment;
    if (chart) *chart = version->chart;
    return TA_OK;
}

uint32_t ta_host_segment_count(const ta_host *host) {
    return host->segment_count;
}

static uint32_t segment_actors(const host_segment *segment) {
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(segment->system, &stats);
    return stats.actors;
}

ta_status ta_host_segment_info_get(const ta_host *host, uint32_t segment,
                                   ta_host_segment_info *out) {
    if (segment >= host->segment_count) return TA_ERROR_INVALID;
    const host_segment *segment_record = &host->segments[segment];
    ta_host_segment_info z;
    memset(&z, 0, sizeof z);
    z.size = sizeof z;
    z.live = (uint32_t)segment_record->live;
    z.image = segment_record->image;
    if (segment_record->live) {
        z.actors = segment_actors(segment_record);
        for (uint32_t v = 0; v < host->version_count; v++)
            z.current += host->versions[v].segment == segment;
    }
    uint32_t n = out->size && out->size < sizeof z ? out->size : (uint32_t)sizeof z;
    memcpy(out, &z, n);
    out->size = n;
    return TA_OK;
}

ta_system *ta_host_system(ta_host *host, uint32_t segment) {
    host_segment *segment_record = segment_of(host, segment);
    return segment_record ? segment_record->system : NULL;
}

/* ---- actors ------------------------------------------------------------------------- */

ta_status ta_host_locate(const ta_host *host, uint64_t session, uint32_t *segment,
                         uint32_t *actor) {
    uint32_t si = moved_get(host, session);
    if (si == TA_NIL) si = (uint32_t)(session >> SERIALS_SHIFT);
    const host_segment *segment_record = segment_of(host, si);
    uint32_t a = segment_record ? ta_system_actor_by_session(segment_record->system, session) : 0;
    if (!a) return TA_ERROR_STALE;
    if (segment) *segment = si;
    if (actor) *actor = a;
    return TA_OK;
}

ta_status ta_host_spawn(ta_host *host, uint32_t segment, uint32_t chart, ta_string_view name,
                        ta_payload params, uint64_t *session) {
    host_segment *segment_record = segment_of(host, segment);
    if (!segment_record) return TA_ERROR_INVALID;
    uint32_t actor = 0;
    ta_status status;
    while ((status = ta_system_spawn(segment_record->system, chart, name, params, &actor)) ==
               TA_ERROR_NO_MEMORY &&
           segment_grow(segment_record)) {
    }
    if (status != TA_OK) return status;
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    ta_actor_info_get(segment_record->system, actor, &actor_info);
    if (session) *session = actor_info.serial;
    segment_record->busy = 1;
    return TA_OK;
}

ta_status send_to(ta_host *host, uint32_t si, uint32_t actor, const ta_event_in *event) {
    host_segment *segment = &host->segments[si];
    ta_event_in event_in = *event;
    event_in.target = actor;
    ta_status status;
    while ((status = ta_system_send(segment->system, &event_in)) == TA_ERROR_NO_MEMORY &&
           segment_grow(segment)) {
    }
    if (status == TA_OK) segment->busy = 1;
    return status;
}

ta_status ta_host_send(ta_host *host, uint64_t session, const ta_event_in *event) {
    uint32_t si, actor;
    ta_status status = ta_host_locate(host, session, &si, &actor);
    return status == TA_OK ? send_to(host, si, actor, event) : status;
}

uint32_t ta_host_send_many(ta_host *host, const uint64_t *sessions, const ta_event_in *events,
                           uint32_t n) {
    ta_event_in batch[64];
    uint32_t done = 0;
    while (done < n) {
        uint32_t si, actor;
        if (ta_host_locate(host, sessions[done], &si, &actor) != TA_OK) return done;
        /* a run of events for the same segment, one ta_system_send_many */
        uint32_t k = 0;
        while (done + k < n && k < 64) {
            uint32_t s2, a2;
            if (k && (ta_host_locate(host, sessions[done + k], &s2, &a2) != TA_OK || s2 != si))
                break;
            if (!k) a2 = actor;
            batch[k] = events[done + k];
            batch[k].target = a2;
            k++;
        }
        host_segment *segment = &host->segments[si];
        uint32_t got = ta_system_send_many(segment->system, batch, k);
        if (got) segment->busy = 1;
        done += got;
        if (got < k) { /* batch[got] failed: grow and go on, or stop there */
            if (send_to(host, si, batch[got].target, &batch[got]) != TA_OK) return done;
            done++;
        }
    }
    return done;
}

ta_status ta_host_terminate(ta_host *host, uint64_t session) {
    uint32_t si, actor;
    ta_status status = ta_host_locate(host, session, &si, &actor);
    if (status != TA_OK) return status;
    host->segments[si].busy = 1;
    return ta_system_terminate(host->segments[si].system, actor);
}

ta_status ta_host_resume(ta_host *host, uint32_t segment, uint32_t request, ta_status status,
                         ta_payload result) {
    host_segment *segment_record = segment_of(host, segment);
    if (!segment_record) return TA_ERROR_INVALID;
    segment_record->busy = 1;
    return ta_system_resume(segment_record->system, request, status, result);
}

/* ---- effects ------------------------------------------------------------------------ */

/* Release the effects the host has seen (ta_host_effects): those before
 * effects_ready. Effects made since — by calls between runs, such as
 * ta_host_deliver answering with an `err` — stay for the next batch. */
static void effects_release(ta_host *host) {
    uint32_t seen = host->effects_ready;
    for (uint32_t i = 0; i < seen; i++) {
        ta_host_effect *effect = &host->effects[i];
        if (effect->payload && effect->datamodel < host->config.datamodel_count &&
            host->datamodels[effect->datamodel].vtable->release)
            host->datamodels[effect->datamodel].vtable->release(
                host->datamodels[effect->datamodel].datamodel, effect->payload);
    }
    uint32_t keep = host->effect_count - seen;
    if (!keep) {
        host->effect_count = 0;
        host->pool_length = 0;
        host->effects_ready = 0;
        return;
    }
    /* move the unseen ones to the front; their strings stay in the pool (it
     * is reset only when no effect is left) */
    memmove(host->effects, host->effects + seen, keep * sizeof *host->effects);
    memmove(host->effect_strings, host->effect_strings + seen, keep * sizeof *host->effect_strings);
    host->effect_count = keep;
    host->effects_ready = 0;
}

size_t pool_put(ta_host *host, ta_string_view s) {
    if (!s.length) return (size_t)-1;
    if (host->pool_length + s.length > host->pool_capacity) {
        size_t c = host->pool_capacity ? host->pool_capacity : 4096;
        while (c < host->pool_length + s.length)
            c *= 2;
        char *p = (char *)realloc(host->pool, c);
        if (!p) return (size_t)-1;
        host->pool = p;
        host->pool_capacity = c;
    }
    memcpy(host->pool + host->pool_length, s.pointer, s.length);
    host->pool_length += s.length;
    return host->pool_length - s.length;
}

ta_host_effect *effect_new(ta_host *host, uint32_t kind, uint32_t segment) {
    ta_host_effect *new_effects = (ta_host_effect *)grow(
        host->effects, &host->effect_capacity, host->effect_count + 1, sizeof *new_effects);
    if (!new_effects) return NULL;
    host->effects = new_effects;
    uint32_t cs = host->effect_capacity;
    effect_strings *ns =
        (effect_strings *)realloc(host->effect_strings, sizeof(effect_strings) * cs);
    if (!ns) return NULL;
    host->effect_strings = ns;
    ta_host_effect *effect = &host->effects[host->effect_count];
    memset(effect, 0, sizeof *effect);
    effect->size = sizeof *effect;
    effect->kind = kind;
    effect->segment = segment;
    effect->job = TA_NIL;
    effect->chart = TA_NIL;
    effect->datamodel = TA_NIL;
    for (int k = 0; k < 5; k++)
        host->effect_strings[host->effect_count].offset[k] = (size_t)-1;
    host->effect_count++;
    return effect;
}

void set_effect_strings(ta_host *host, ta_string_view t, ta_string_view event, ta_string_view sid,
                        ta_string_view aux) {
    effect_strings *strings = &host->effect_strings[host->effect_count - 1];
    ta_host_effect *effect = &host->effects[host->effect_count - 1];
    ta_string_view v[4] = {t, event, sid, aux};
    ta_string_view *dst[4] = {&effect->target, &effect->event, &effect->sendid, &effect->auxiliary};
    for (int k = 0; k < 4; k++) {
        strings->offset[k] = pool_put(host, v[k]);
        dst[k]->length = strings->offset[k] == (size_t)-1 ? 0 : v[k].length;
    }
}

static uint64_t root_of(host_segment *segment, uint32_t actor) {
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    uint64_t root = 0;
    for (int depth = 0; depth < 64 && actor; depth++) {
        if (ta_actor_info_get(segment->system, actor, &actor_info) != TA_OK) break;
        root = actor_info.serial;
        actor = actor_info.parent;
    }
    return root;
}

/* copy a segment effect out to the host */
static void effect_copy(ta_host *host, uint32_t si, const ta_effect *effect,
                        uint32_t io_processor) {
    ta_host_effect *host_effect = effect_new(host, effect->kind, si);
    if (!host_effect) return;
    host_effect->actor = effect->actor;
    host_effect->session = effect->session;
    host_effect->root = effect->kind == TA_EFFECT_DONE
                            ? effect->session
                            : root_of(&host->segments[si], effect->actor);
    if (!host_effect->root) host_effect->root = effect->session;
    host_effect->token = effect->token;
    host_effect->io_processor = io_processor;
    host_effect->flags = effect->flags;
    host_effect->delay_hint = effect->delay_hint;
    set_effect_strings(host, effect->target, effect->event, effect->sendid, effect->auxiliary);
    if (effect->payload && effect->datamodel < host->config.datamodel_count) {
        host_effect->payload = effect->payload;
        host_effect->datamodel = effect->datamodel;
        if (host->datamodels[effect->datamodel].vtable->retain)
            host->datamodels[effect->datamodel].vtable->retain(
                host->datamodels[effect->datamodel].datamodel, effect->payload);
    }
}

uint32_t ta_host_effects(ta_host *host, const ta_host_effect **out) {
    host->effects_ready =
        host->effect_count; /* these are seen now: released by the next ack or run */
    for (uint32_t i = 0; i < host->effect_count; i++) {
        ta_host_effect *effect = &host->effects[i];
        ta_string_view *dst[5] = {&effect->target, &effect->event, &effect->sendid,
                                  &effect->auxiliary, &effect->msgid};
        for (int k = 0; k < 5; k++)
            dst[k]->pointer = host->effect_strings[i].offset[k] == (size_t)-1
                                  ? ""
                                  : host->pool + host->effect_strings[i].offset[k];
    }
    *out = host->effects;
    return host->effect_count;
}

void ta_host_effects_ack(ta_host *host) {
    effects_release(host);
}

/* ---- routing ---------------------------------------------------------------------- */

static void communication_error_now(ta_host *host, uint32_t si, uint32_t actor,
                                    ta_string_view sendid) {
    ta_event_in event;
    ta_host_error_event(sendid, &event);
    send_to(host, si, actor, &event);
}

/* error.communication for a sender. While a segment's effects are being
 * drained it cannot take events (not before its ack): those are kept and
 * delivered right after (drain). */
void communication_error(ta_host *host, uint32_t si, uint32_t actor, ta_string_view sendid) {
    if (host->draining != si) {
        communication_error_now(host, si, actor, sendid);
        return;
    }
    void *ne =
        grow(host->errors, &host->error_capacity, host->error_count + 1, sizeof *host->errors);
    if (!ne) return;
    host->errors = ne;
    host->errors[host->error_count].actor = actor;
    host->errors[host->error_count].session_id = pool_put(host, sendid);
    host->errors[host->error_count].session_id_length = sendid.length;
    host->error_count++;
}

/* A send through the remote processor: another segment of ours, or out. */
static void route_effect(ta_host *host, uint32_t si, const ta_effect *effect) {
    ta_router_outbound(host, si, effect);
}

/* Drain a segment's effects: route, copy out, acknowledge. */
static void drain(ta_host *host, uint32_t si) {
    host_segment *segment = &host->segments[si];
    const ta_effect *effect;
    uint32_t n = ta_system_effects(segment->system, &effect);
    host->draining = si;
    for (uint32_t i = 0; i < n; i++) {
        const ta_effect *effect_view = &effect[i];
        if (effect_view->kind == TA_EFFECT_SEND && effect_view->io_processor == 0)
            route_effect(host, si, effect_view);
        else if (effect_view->kind == TA_EFFECT_SEND)
            effect_copy(host, si, effect_view,
                        effect_view->io_processor == TA_NIL ? TA_NIL
                                                            : effect_view->io_processor - 1);
        else
            effect_copy(host, si, effect_view, TA_NIL);
        if (effect_view->kind == TA_EFFECT_DONE || effect_view->kind == TA_EFFECT_ERROR)
            if (moved_get(host, effect_view->session) != TA_NIL)
                moved_put(host, effect_view->session, TA_NIL);
    }
    ta_system_effects_ack(host->segments[si].system,
                          n); /* (send_to may have grown another segment) */
    host->draining = TA_NIL;
    for (uint32_t i = 0; i < host->error_count; i++) {
        ta_string_view sid = {
            host->errors[i].session_id == (size_t)-1 ? "" : host->pool + host->errors[i].session_id,
            host->errors[i].session_id == (size_t)-1 ? 0 : host->errors[i].session_id_length};
        communication_error_now(host, si, host->errors[i].actor, sid);
    }
    host->error_count = 0;
}

/* ---- migration ------------------------------------------------------------------------ */

static migration_job *job_by_id(const ta_host *host, uint32_t id) {
    for (uint32_t i = 0; i < host->job_count; i++)
        if (host->jobs[i].id == id) return &host->jobs[i];
    return NULL;
}

ta_status ta_host_migrate(ta_host *host, ta_string_view name, uint64_t session, uint32_t flags,
                          ta_time timeout, uint32_t *id) {
    migration_job *new_jobs = (migration_job *)grow(host->jobs, &host->job_capacity,
                                                    host->job_count + 1, sizeof(migration_job));
    if (!new_jobs) return TA_ERROR_NO_MEMORY;
    host->jobs = new_jobs;
    migration_job *job = &host->jobs[host->job_count++];
    memset(job, 0, sizeof *job);
    job->id = ++host->job_sequence;
    job->active = 1;
    job->flags = flags;
    job->session = session;
    job->deadline = timeout ? host->now + timeout : 0;
    job->name = (char *)malloc(name.length + 1);
    if (name.length) memcpy(job->name, name.pointer, name.length);
    job->name[name.length] = 0;
    job->name_length = name.length;
    job->walking = session == 0;
    if (session) {
        job->retry = (uint64_t *)malloc(sizeof(uint64_t));
        job->woken = (uint8_t *)calloc(1, 1);
        job->retry[0] = session;
        job->retry_count = job->retry_capacity = 1;
    }
    if (id) *id = job->id;
    return TA_OK;
}

uint32_t ta_host_job_pending(const ta_host *host, uint32_t id, uint64_t *out, uint32_t capacity) {
    const migration_job *job = job_by_id(host, id);
    if (!job) return 0;
    uint32_t n = job->active ? job->retry_count : job->left_count;
    const uint64_t *source = job->active ? job->retry : job->left;
    for (uint32_t i = 0; i < n && i < capacity; i++)
        out[i] = source[i];
    return n;
}

/* Should (segment, actor) move for this job? Returns the target version. */
static const chart_version *candidate(ta_host *host, migration_job *job, uint32_t si,
                                      uint32_t actor, ta_actor_info *actor_info) {
    host_segment *segment = &host->segments[si];
    actor_info->size = sizeof *actor_info;
    if (ta_actor_info_get(segment->system, actor, actor_info) != TA_OK || actor_info->parent)
        return NULL;
    ta_string_view k = chart_key(segment->image, actor_info->chart);
    if (job->name_length && !string_equal(k, job->name, job->name_length)) return NULL;
    const chart_version *version = version_of(host, k);
    return version && version->segment != si ? version : NULL;
}

static unsigned char *buffer_at(unsigned char **b, size_t *capacity, size_t n) {
    if (n > *capacity) {
        unsigned char *p = (unsigned char *)realloc(*b, n);
        if (!p) return NULL;
        *b = p;
        *capacity = n;
    }
    return *b;
}

/* 1 moved, 0 later (busy / asleep), -1 failed, -2 not a candidate (anymore) */
static int migrate_one(ta_host *host, migration_job *job, uint64_t session, uint8_t *woken) {
    uint32_t si, actor;
    if (ta_host_locate(host, session, &si, &actor) != TA_OK) return -2;
    ta_actor_info actor_info;
    const chart_version *version = candidate(host, job, si, actor, &actor_info);
    if (!version) return -2;
    host_segment *from = &host->segments[si], *to = &host->segments[version->segment];
    if (actor_info.state == TA_ACTOR_HIBERNATED) {
        if ((job->flags & TA_HOST_MIGRATE_WAKE) && woken && !*woken) {
            ta_host_effect *effect =
                effect_new(host, TA_EFFECT_WAKE, si); /* the host loads the blob */
            if (effect) {
                effect->actor = actor;
                effect->session = effect->root = session;
                effect->job = job->id;
                *woken = 1;
            }
        }
        return 0;
    }
    if (actor_info.invocations) return 0; /* a session tree: not translated; once they finished */
    size_t length = 0;
    ta_status status =
        ta_actor_export(from->system, actor, buffer_at(&host->blob, &host->blob_capacity, 4096),
                        host->blob_capacity, &length);
    if (status == TA_ERROR_NO_MEMORY && buffer_at(&host->blob, &host->blob_capacity, length))
        status = ta_actor_export(from->system, actor, host->blob, host->blob_capacity, &length);
    if (status == TA_ERROR_STATE) return 0;
    if (status != TA_OK) return -1;
    size_t ml = 0;
    int moved = 0;
    uint32_t chart = version->chart, tseg = version->segment;
    ta_status ms = ta_blob_migrate(from->image, to->image, chart, host->blob, length, NULL, 0, &ml);
    if (ms == TA_ERROR_NO_MEMORY && buffer_at(&host->blob2, &host->blob2_capacity, ml) &&
        ta_blob_migrate(from->image, to->image, chart, host->blob, length, host->blob2, ml, &ml) ==
            TA_OK) {
        uint32_t na;
        while ((status = ta_system_adopt(to->system, host->blob2, ml, &na)) == TA_ERROR_NO_MEMORY &&
               segment_grow(to)) {
        }
        moved = status == TA_OK;
    }
    if (!moved) { /* back home, unchanged */
        uint32_t na;
        while ((status = ta_system_adopt(from->system, host->blob, length, &na)) ==
                   TA_ERROR_NO_MEMORY &&
               segment_grow(from)) {
        }
        from->busy = 1;
        /* running invocations (a session tree): migrated once they finished */
        return ms == TA_ERROR_UNSUPPORTED ? 0 : -1;
    }
    moved_put(host, session, (uint32_t)(session >> SERIALS_SHIFT) == tseg ? TA_NIL : tseg);
    to->busy = 1;
    host->migrated++;
    ta_host_effect *effect = effect_new(host, TA_HOST_EFFECT_MIGRATED, tseg);
    if (effect) {
        uint32_t na = 0;
        ta_host_locate(host, session, NULL, &na);
        effect->actor = na;
        effect->session = effect->root = session;
        effect->job = job->id;
        effect->chart = chart;
        ta_string_view k = chart_key(to->image, chart);
        set_effect_strings(host, (ta_string_view){0, 0}, k, (ta_string_view){0, 0},
                           (ta_string_view){0, 0});
    }
    return 1;
}

static uint32_t retry_push(migration_job *job, uint64_t session) {
    for (uint32_t i = 0; i < job->retry_count; i++)
        if (job->retry[i] == session) return i;
    uint32_t c = job->retry_capacity;
    uint64_t *r = (uint64_t *)grow(job->retry, &c, job->retry_count + 1, sizeof(uint64_t));
    if (!r) return TA_NIL;
    job->retry = r;
    uint32_t c2 = job->retry_capacity;
    uint8_t *w = (uint8_t *)grow(job->woken, &c2, job->retry_count + 1, 1);
    if (!w) return TA_NIL;
    job->woken = w;
    job->retry_capacity = c;
    job->retry[job->retry_count] = session;
    job->woken[job->retry_count] = 0;
    return job->retry_count++;
}

static void job_end(ta_host *host, migration_job *job) {
    if (job->walking) { /* timed out mid-walk: the rest of the candidates are left behind too */
        for (uint32_t si = job->walk_segment; si < host->segment_count; si++) {
            host_segment *segment = segment_of(host, si);
            if (!segment) continue;
            uint32_t cur = si == job->walk_segment ? job->cursor : 0, list[256], n;
            while ((n = ta_system_actors(segment->system, &cur, list, 256)) != 0)
                for (uint32_t k = 0; k < n; k++) {
                    ta_actor_info actor_info;
                    if (candidate(host, job, si, list[k], &actor_info))
                        retry_push(job, actor_info.serial);
                }
        }
        job->walking = 0;
    }
    job->active = 0;
    job->left = job->retry;
    job->left_count = job->retry_count;
    job->retry = NULL;
    job->retry_count = job->retry_capacity = 0;
    free(job->woken);
    job->woken = NULL;
    ta_host_effect *effect = effect_new(host, TA_HOST_EFFECT_JOB_DONE, TA_NIL);
    if (effect) {
        effect->job = job->id;
        effect->migrated = job->migrated;
        effect->failed = job->failed;
        effect->pending = job->left_count;
    }
}

/* Advance a job by up to `quantum` actors. */
static void job_step(ta_host *host, migration_job *job, uint32_t quantum) {
    uint32_t looked = 0;
    while (job->walking && looked < quantum) {
        host_segment *segment = segment_of(host, job->walk_segment);
        if (job->walk_segment >= host->segment_count) {
            job->walking = 0;
            break;
        }
        if (!segment) {
            job->walk_segment++;
            job->cursor = 0;
            continue;
        }
        uint32_t list[64], want = quantum - looked < 64 ? quantum - looked : 64;
        uint32_t n = ta_system_actors(segment->system, &job->cursor, list, want);
        if (!n) {
            job->walk_segment++;
            job->cursor = 0;
            continue;
        }
        for (uint32_t k = 0; k < n; k++, looked++) {
            ta_actor_info actor_info;
            if (!candidate(host, job, job->walk_segment, list[k], &actor_info)) continue;
            uint8_t woken = 0;
            int r = migrate_one(host, job, actor_info.serial, &woken);
            if (r > 0) job->migrated++;
            if (r == -1) job->failed++;
            if (r == 0) {
                uint32_t at = retry_push(job, actor_info.serial);
                if (woken && at != TA_NIL) job->woken[at] = 1;
            }
        }
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < job->retry_count; i++) {
        int r = 0;
        if (looked < quantum) {
            r = migrate_one(host, job, job->retry[i], &job->woken[i]);
            looked++;
        }
        if (r > 0) job->migrated++;
        if (r == -1) job->failed++;
        if (r == 0) {
            job->retry[keep] = job->retry[i];
            job->woken[keep] = job->woken[i];
            keep++;
        }
    }
    job->retry_count = keep;
    if ((!job->walking && !job->retry_count) || (job->deadline && host->now >= job->deadline))
        job_end(host, job);
}

/* ---- running ------------------------------------------------------------------------- */

void ta_host_advance_time(ta_host *host, ta_time now) {
    host->now = now;
    for (uint32_t i = 0; i < host->segment_count; i++)
        if (host->segments[i].live) ta_system_advance_time(host->segments[i].system, now);
    ta_router_time(host); /* envelopes queued for absent peers past their TTL */
}

static void retire(ta_host *host) {
    for (uint32_t si = 0; si < host->segment_count; si++) {
        host_segment *segment = segment_of(host, si);
        if (!segment || segment->busy) continue;
        int current = 0;
        for (uint32_t v = 0; v < host->version_count; v++)
            current |= host->versions[v].segment == si;
        if (current || segment_actors(segment)) continue;
        ta_system_deinit(segment->system);
        free(segment->memory);
        segment->memory = NULL;
        segment->system = NULL;
        segment->live = 0;
        host->retired++;
        for (uint32_t i = 0; i < host->moved_capacity; i++)
            if (host->moved_keys[i] && host->moved_values[i] == si)
                moved_put(host, host->moved_keys[i], TA_NIL);
        effect_new(host, TA_HOST_EFFECT_SEGMENT_FREED, si);
    }
}

ta_run ta_host_run(ta_host *host, uint32_t budget) {
    ta_run out;
    memset(&out, 0, sizeof out);
    effects_release(host); /* the previous batch, if not acknowledged */
    uint32_t left = budget;
    int full = 0, fault = 0;
    for (int round = 0; round < 64 && !full; round++) {
        int ran = 0;
        /* fair shares: a runaway segment must not starve the others */
        uint32_t runnable = 0;
        for (uint32_t k = 0; k < host->segment_count; k++) {
            const host_segment *segment = segment_of(host, k);
            runnable += segment && (segment->busy || segment->deadline <= host->now);
        }
        for (uint32_t k = 0; k < host->segment_count && !full; k++) {
            uint32_t si = (host->round_robin + k) % host->segment_count;
            host_segment *segment = segment_of(host, si);
            if (!segment || (!segment->busy && segment->deadline > host->now)) continue;
            if (!left) break;
            uint32_t share = runnable > 1 ? left / runnable : left;
            if (runnable > 1) runnable--;
            ta_run result = ta_system_run(segment->system, share ? share : 1);
            left = result.used >= left ? 0 : left - result.used;
            ran = 1;
            segment->deadline = result.next_deadline;
            segment->busy =
                result.status != TA_RUN_IDLE; /* draining may add work (errors, deliveries) */
            drain(host, si);
            segment = &host->segments[si];
            if (result.status == TA_RUN_EXHAUSTED && !segment_grow(segment)) {
                fault = 1;
                segment->busy = 0;
            }
            if (result.status == TA_RUN_FAULT) {
                fault = 1;
                segment->busy = 0;
            }
            if (host->effect_count >= EFFECTS_PER_RUN) full = 1;
        }
        for (uint32_t i = 0; i < host->job_count && round == 0; i++) /* one quantum per run */
            if (host->jobs[i].active) {
                job_step(host, &host->jobs[i], host->config.migrate_quantum);
                ran = 1;
            }
        if (!ran || !left) break;
        int more = 0;
        for (uint32_t i = 0; i < host->segment_count; i++)
            more |= host->segments[i].live && host->segments[i].busy;
        if (!more) break;
    }
    host->round_robin = host->segment_count ? (host->round_robin + 1) % host->segment_count : 0;
    /* drop jobs that ended before this run (their `left` lists were readable until now) */
    uint32_t kj = 0;
    for (uint32_t i = 0; i < host->job_count; i++) {
        migration_job *job = &host->jobs[i];
        int ended_now = 0;
        for (uint32_t e = 0; e < host->effect_count; e++)
            ended_now |=
                host->effects[e].kind == TA_HOST_EFFECT_JOB_DONE && host->effects[e].job == job->id;
        if (job->active || ended_now) {
            host->jobs[kj++] = *job;
        } else {
            free(job->name);
            free(job->retry);
            free(job->woken);
            free(job->left);
        }
    }
    host->job_count = kj;
    retire(host);
    /* yielded: segments have work, or a job has actors left to look at. A job
     * that only waits (busy or sleeping actors) moves on when they change —
     * the next run after an event — or at its timeout (next_deadline). */
    int busy = 0;
    out.next_deadline = UINT64_MAX;
    for (uint32_t i = 0; i < host->segment_count; i++) {
        host_segment *segment = segment_of(host, i);
        if (!segment) continue;
        busy |= segment->busy;
        if (segment->deadline < out.next_deadline) out.next_deadline = segment->deadline;
    }
    for (uint32_t i = 0; i < host->job_count; i++) {
        const migration_job *job = &host->jobs[i];
        if (!job->active) continue;
        busy |= job->walking;
        if (job->deadline && job->deadline < out.next_deadline) out.next_deadline = job->deadline;
    }
    ta_time qd = ta_router_deadline(host); /* queued envelopes expire then */
    if (qd < out.next_deadline) out.next_deadline = qd;
    out.used = budget - left;
    out.effect_count = host->effect_count;
    out.status = full    ? TA_RUN_EFFECTS_FULL
                 : fault ? TA_RUN_FAULT
                 : busy  ? TA_RUN_YIELDED
                         : TA_RUN_IDLE;
    return out;
}

/* ---- hibernation ----------------------------------------------------------------------- */

uint32_t ta_host_sweep(ta_host *host, uint32_t max_scan, uint64_t *out, uint32_t capacity) {
    uint32_t n = 0, scanned = 0,
             *handles = (uint32_t *)malloc(sizeof(uint32_t) * (capacity ? capacity : 1));
    if (!handles) return 0;
    /* each live segment once at most: its share is up to one revolution (no lapping) */
    for (uint32_t visited = 0; visited < host->segment_count && scanned < max_scan && n < capacity;
         visited++) {
        if (host->sweep_segment >= host->segment_count) {
            host->sweep_segment = 0;
            host->sweep_cursor = 0;
        }
        host_segment *segment = segment_of(host, host->sweep_segment);
        if (segment) {
            ta_system_stats stats;
            stats.size = sizeof stats;
            ta_system_stats_get(segment->system, &stats);
            uint32_t budget = max_scan - scanned, start = host->sweep_cursor;
            uint32_t got = ta_system_cold_actors(segment->system, &host->sweep_cursor, budget,
                                                 handles, capacity - n);
            for (uint32_t i = 0; i < got; i++) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(segment->system, handles[i], &actor_info) == TA_OK)
                    out[n++] = actor_info.serial;
            }
            scanned += budget < stats.actors_capacity ? budget : stats.actors_capacity;
            if (n == capacity) break;              /* the output is full: continue here next time */
            if (host->sweep_cursor > start) break; /* out of budget mid-revolution: continue here */
        }
        host->sweep_segment++;
        host->sweep_cursor = 0;
    }
    free(handles);
    return n;
}

ta_status ta_host_hibernate(ta_host *host, uint64_t session, void *buffer, size_t capacity,
                            size_t *length) {
    uint32_t si, actor;
    ta_status status = ta_host_locate(host, session, &si, &actor);
    return status == TA_OK
               ? ta_actor_hibernate(host->segments[si].system, actor, buffer, capacity, length)
               : status;
}

ta_status ta_host_wake(ta_host *host, uint64_t session, const void *blob, size_t length) {
    uint32_t si, actor;
    ta_status status = ta_host_locate(host, session, &si, &actor);
    if (status != TA_OK) return status;
    host_segment *segment = &host->segments[si];
    while ((status = ta_actor_wake(segment->system, actor, blob, length)) == TA_ERROR_NO_MEMORY &&
           segment_grow(segment)) {
    }
    if (status == TA_OK) segment->busy = 1;
    return status;
}

/* ---- moving actors between hosts ------------------------------------------------------- */

ta_status ta_host_export_actor(ta_host *host, uint64_t session, void *buffer, size_t capacity,
                               size_t *length) {
    uint32_t si, actor;
    ta_status status = ta_host_locate(host, session, &si, &actor);
    if (status != TA_OK) return status;
    status = ta_actor_export(host->segments[si].system, actor, buffer, capacity, length);
    if (status == TA_OK) {
        if (moved_get(host, session) != TA_NIL) moved_put(host, session, TA_NIL);
        host->segments[si].busy = 1;
    }
    return status;
}

/* An adopted session lives in segment si: routed there, and its id never handed
 * out by its home segment (one added later is set up past it, reserve_serials). */
static void place(ta_host *host, uint32_t si, uint64_t serial) {
    uint32_t home = (uint32_t)(serial >> SERIALS_SHIFT);
    moved_put(host, serial, home == si ? TA_NIL : si);
    host_segment *hs = segment_of(host, home);
    if (hs) ta_system_set_next_serial(hs->system, serial); /* (already past it: unchanged) */
}

/* A new segment index: past the ids of sessions adopted elsewhere from its range. */
static void reserve_serials(ta_host *host, uint32_t index) {
    for (uint32_t i = 0; i < host->moved_capacity; i++)
        if (host->moved_keys[i] && host->moved_values[i] != TA_NIL &&
            (uint32_t)(host->moved_keys[i] >> SERIALS_SHIFT) == index)
            ta_system_set_next_serial(host->segments[index].system, host->moved_keys[i]);
}

/* A session id that lives in another segment than s (its home, or where it was moved). */
static int lives_elsewhere(ta_host *host, const host_segment *segment, uint64_t serial) {
    uint32_t m = moved_get(host, serial);
    host_segment *o = segment_of(host, m != TA_NIL ? m : (uint32_t)(serial >> SERIALS_SHIFT));
    return o && o != segment && ta_system_actor_by_session(o->system, serial);
}

/* The sessions of an adopted tree in segment si (root and the sessions it
 * invoked): check (1 if an id lives elsewhere) or place them. */
static int tree_pass(ta_host *host, uint32_t si, uint32_t root, uint64_t root_serial, int check) {
    host_segment *segment = &host->segments[si];
    if (check ? lives_elsewhere(host, segment, root_serial) : (place(host, si, root_serial), 0))
        return 1;
    uint32_t cursor = 0, list[64], n;
    while ((n = ta_system_actors(segment->system, &cursor, list, 64)) != 0)
        for (uint32_t k = 0; k < n; k++) {
            ta_actor_info ci;
            ci.size = sizeof ci;
            if (list[k] == root || ta_actor_info_get(segment->system, list[k], &ci) != TA_OK)
                continue;
            uint64_t serial = ci.serial;
            while (ci.parent && ci.parent != root &&
                   ta_actor_info_get(segment->system, ci.parent, &ci) == TA_OK) {
            }
            if (ci.parent != root) continue;
            if (check ? lives_elsewhere(host, segment, serial) : (place(host, si, serial), 0))
                return 1;
        }
    return 0;
}

ta_status ta_host_adopt(ta_host *host, uint32_t segment, const void *blob, size_t length,
                        uint64_t *session) {
    host_segment *segment_record = segment_of(host, segment);
    if (session) *session = 0;
    if (!segment_record) return TA_ERROR_INVALID;
    uint32_t root = 0;
    ta_status status;
    while ((status = ta_system_adopt(segment_record->system, blob, length, &root)) ==
               TA_ERROR_NO_MEMORY &&
           segment_grow(segment_record)) {
    }
    if (status != TA_OK) return status;
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    ta_actor_info_get(segment_record->system, root, &actor_info);
    if (tree_pass(host, segment, root, actor_info.serial, 1)) {
        /* one of its sessions lives in another segment: out again, nothing changed */
        size_t n = 0;
        status = ta_actor_export(segment_record->system, root,
                                 buffer_at(&host->blob, &host->blob_capacity, length),
                                 host->blob_capacity, &n);
        if (status == TA_ERROR_NO_MEMORY && buffer_at(&host->blob, &host->blob_capacity, n))
            ta_actor_export(segment_record->system, root, host->blob, host->blob_capacity, &n);
        return TA_ERROR_STATE;
    }
    segment_record->busy = 1;
    tree_pass(host, segment, root, actor_info.serial, 0);
    if (session) *session = actor_info.serial;
    return TA_OK;
}

/* ---- inspection ------------------------------------------------------------------------ */

void ta_host_stats_get(const ta_host *host, ta_host_stats *o) {
    ta_host_stats z;
    memset(&z, 0, sizeof z);
    z.size = sizeof z;
    for (uint32_t i = 0; i < host->segment_count; i++) {
        const host_segment *segment = segment_of(host, i);
        if (!segment) continue;
        ta_system_stats stats;
        stats.size = sizeof stats;
        ta_system_stats_get(segment->system, &stats);
        z.segments++;
        z.actors += stats.actors;
        z.hibernated += stats.hibernated;
        z.runnable += stats.runnable;
        z.region_bytes += stats.region_bytes;
        z.delivered += stats.delivered;
    }
    for (uint32_t i = 0; i < host->job_count; i++)
        z.jobs += (uint32_t)host->jobs[i].active;
    z.routed = host->routed;
    z.migrated = host->migrated;
    z.retired = host->retired;
    uint32_t n = o->size && o->size < sizeof z ? o->size : (uint32_t)sizeof z;
    memcpy(o, &z, n);
    o->size = n;
}

uint32_t ta_host_actors(const ta_host *host, uint64_t *cursor, uint64_t *out, uint32_t capacity) {
    uint32_t si = (uint32_t)(*cursor >> 32), c = (uint32_t)*cursor, n = 0;
    while (si < host->segment_count && n < capacity) {
        const host_segment *segment = segment_of(host, si);
        uint32_t list[64], got = 0;
        if (segment) {
            uint32_t want = capacity - n < 64 ? capacity - n : 64;
            got = ta_system_actors(segment->system, &c, list, want);
            for (uint32_t k = 0; k < got; k++) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(segment->system, list[k], &actor_info) == TA_OK)
                    out[n++] = actor_info.serial;
            }
        }
        if (!got) {
            si++;
            c = 0;
        }
    }
    *cursor = ((uint64_t)si << 32) | c;
    return n;
}

/* ---- system images ------------------------------------------------------------------ */

ta_status ta_host_boot(ta_host *host, uint32_t segment) {
    host_segment *segment_record = segment_of(host, segment);
    if (!segment_record) return TA_ERROR_INVALID;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(segment_record->image, &image_info);
    for (;;) { /* room for every boot actor first: a partial boot cannot be retried */
        ta_system_stats stats;
        stats.size = sizeof stats;
        ta_system_stats_get(segment_record->system, &stats);
        if (stats.actors + image_info.boot_actor_count <= stats.actors_capacity) break;
        if (!segment_grow(segment_record)) return TA_ERROR_NO_MEMORY;
    }
    ta_status r = ta_system_boot(segment_record->system);
    segment_record->busy = 1;
    return r;
}

uint64_t ta_host_actor_by_name(const ta_host *host, ta_string_view name) {
    for (uint32_t k = host->segment_count; k-- > 0;) {
        const host_segment *segment = segment_of(host, k);
        uint32_t a = segment ? ta_system_actor_by_name(segment->system, name) : 0;
        ta_actor_info actor_info;
        memset(&actor_info, 0, sizeof actor_info);
        actor_info.size = sizeof actor_info;
        if (a && ta_actor_info_get(segment->system, a, &actor_info) == TA_OK)
            return actor_info.serial;
    }
    return 0;
}

/* ---- snapshot / restore ------------------------------------------------------------- */

#define SNAPSHOT_MAGIC   0x53484154u /* "TAHS" */
#define SNAPSHOT_VERSION 1u

typedef struct snapshot_header {
    uint32_t magic, version, segment_count, moved_count;
    uint32_t round_robin, job_sequence, config_size, pad;
    uint64_t now, routed, migrated, retired;
    uint64_t message_sequence,
        router_length; /* the router's dedup window follows the moved sessions */
} snapshot_header;

typedef struct snapshot_segment {
    uint32_t live, pad;
    uint64_t hash, region_length;
    ta_system_config config; /* capacities (host pointers are supplied again) */
} snapshot_segment;

typedef struct snapshot_moved {
    uint64_t session;
    uint32_t segment, pad;
} snapshot_moved;

static size_t pad8(size_t n) {
    return (n + 7) & ~(size_t)7;
}

ta_status ta_host_snapshot(ta_host *host, void *buffer, size_t capacity, size_t *length) {
    if (host->effect_count) return TA_ERROR_STATE; /* acknowledge the effects first */
    for (uint32_t i = 0; i < host->job_count; i++)
        if (host->jobs[i].active) return TA_ERROR_STATE;
    size_t need = sizeof(snapshot_header);
    for (uint32_t i = 0; i < host->segment_count; i++) {
        need += sizeof(snapshot_segment);
        if (!host->segments[i].live) continue;
        size_t rl = 0;
        ta_status status = ta_system_snapshot(host->segments[i].system, NULL, 0, &rl);
        if (status != TA_OK) return status;
        need += pad8(rl);
    }
    uint32_t moved_count = 0;
    for (uint32_t i = 0; i < host->moved_capacity; i++)
        moved_count += host->moved_keys[i] && host->moved_values[i] != TA_NIL;
    need += (size_t)moved_count * sizeof(snapshot_moved);
    size_t rlen = ta_router_snapshot(host, NULL, 0);
    need += rlen;
    *length = need;
    if (!buffer) return TA_OK;
    if (capacity < need) return TA_ERROR_NO_MEMORY;
    unsigned char *p = (unsigned char *)buffer;
    memset(p, 0, need);
    snapshot_header hd;
    memset(&hd, 0, sizeof hd);
    hd.magic = SNAPSHOT_MAGIC;
    hd.version = SNAPSHOT_VERSION;
    hd.segment_count = host->segment_count;
    hd.moved_count = moved_count;
    hd.round_robin = host->round_robin;
    hd.job_sequence = host->job_sequence;
    hd.config_size = (uint32_t)sizeof(ta_system_config);
    hd.now = host->now;
    hd.routed = host->routed;
    hd.migrated = host->migrated;
    hd.retired = host->retired;
    hd.message_sequence = host->message_sequence;
    hd.router_length = rlen;
    memcpy(p, &hd, sizeof hd);
    p += sizeof hd;
    for (uint32_t i = 0; i < host->segment_count; i++) {
        const host_segment *segment = &host->segments[i];
        snapshot_segment ss;
        memset(&ss, 0, sizeof ss);
        ss.live = (uint32_t)segment->live;
        if (segment->live) {
            ta_image_info image_info;
            image_info.size = sizeof image_info;
            ta_image_get_info(segment->image, &image_info);
            ss.hash = image_info.content_hash;
            size_t rl = 0;
            ta_system_snapshot(segment->system, NULL, 0, &rl);
            ss.region_length = rl;
            ss.config = segment->config;
            ss.config.io_processors = NULL;
        }
        memcpy(p, &ss, sizeof ss);
        p += sizeof ss;
        if (segment->live) {
            size_t rl = 0;
            ta_status status =
                ta_system_snapshot(segment->system, p, (size_t)ss.region_length, &rl);
            if (status != TA_OK) return status;
            p += pad8(rl);
        }
    }
    for (uint32_t i = 0; i < host->moved_capacity; i++)
        if (host->moved_keys[i] && host->moved_values[i] != TA_NIL) {
            snapshot_moved m = {host->moved_keys[i], host->moved_values[i], 0};
            memcpy(p, &m, sizeof m);
            p += sizeof m;
        }
    ta_router_snapshot(host, p, rlen);
    return TA_OK;
}

/* walk a snapshot: the header, or NULL if malformed */
static const snapshot_header *snap_check(const void *snap, size_t length) {
    if (!snap || length < sizeof(snapshot_header) || ((uintptr_t)snap & 7)) return NULL;
    const snapshot_header *hd = (const snapshot_header *)snap;
    if (hd->magic != SNAPSHOT_MAGIC || hd->version != SNAPSHOT_VERSION ||
        hd->config_size != sizeof(ta_system_config) || hd->segment_count > (1u << 20))
        return NULL;
    size_t offset = sizeof *hd;
    for (uint32_t i = 0; i < hd->segment_count; i++) {
        if (length - offset < sizeof(snapshot_segment)) return NULL;
        snapshot_segment ss;
        memcpy(&ss, (const unsigned char *)snap + offset, sizeof ss);
        offset += sizeof ss;
        if (ss.live) {
            if (ss.region_length > length - offset) return NULL;
            offset += pad8((size_t)ss.region_length);
            if (offset > length) return NULL;
        }
    }
    if ((length - offset) / sizeof(snapshot_moved) < hd->moved_count) return NULL;
    offset += (size_t)hd->moved_count * sizeof(snapshot_moved);
    if (hd->router_length > length - offset) return NULL;
    return hd;
}

uint32_t ta_host_snapshot_segments(const void *snap, size_t length, uint64_t *hashes,
                                   uint32_t capacity) {
    const snapshot_header *hd = snap_check(snap, length);
    if (!hd) return 0;
    size_t offset = sizeof *hd;
    for (uint32_t i = 0; i < hd->segment_count; i++) {
        snapshot_segment ss;
        memcpy(&ss, (const unsigned char *)snap + offset, sizeof ss);
        if (hashes && i < capacity) hashes[i] = ss.live ? ss.hash : 0;
        offset += sizeof ss + (ss.live ? pad8((size_t)ss.region_length) : 0);
    }
    return hd->segment_count;
}

ta_status ta_host_restore(const ta_host_config *config, const ta_image *const *images,
                          uint32_t image_count, const void *snap, size_t length, ta_host **out) {
    *out = NULL;
    const snapshot_header *hd = snap_check(snap, length);
    if (!hd || image_count < hd->segment_count) return TA_ERROR_INVALID;
    ta_host *host;
    ta_status status = ta_host_new(config, &host);
    if (status != TA_OK) return status;
    if (hd->segment_count &&
        !(host->segments = (host_segment *)grow(host->segments, &host->segment_capacity,
                                                hd->segment_count, sizeof(host_segment)))) {
        ta_host_free(host);
        return TA_ERROR_NO_MEMORY;
    }
    host->now = hd->now;
    const unsigned char *p = (const unsigned char *)snap + sizeof *hd;
    for (uint32_t i = 0; i < hd->segment_count; i++) {
        snapshot_segment sv;
        memcpy(&sv, p, sizeof sv);
        const snapshot_segment *ss = &sv;
        p += sizeof sv;
        host_segment *segment = &host->segments[i];
        memset(segment, 0, sizeof *segment);
        host->segment_count = i + 1;
        if (!ss->live) continue;
        ta_image_info image_info;
        image_info.size = sizeof image_info;
        if (images[i]) ta_image_get_info(images[i], &image_info);
        if (!images[i] || image_info.content_hash != ss->hash) {
            ta_host_free(host);
            return TA_ERROR_INVALID; /* not the image this segment ran */
        }
        segment->image = images[i];
        segment->config = ss->config;
        segment->config.io_processors = host->io_processors;
        segment->config.io_processor_count = host->io_processor_count;
        segment->memory = malloc((size_t)ss->region_length);
        status = segment->memory ? ta_system_restore(segment->memory, (size_t)ss->region_length,
                                                     segment->image, p, (size_t)ss->region_length,
                                                     &segment->config, host->datamodels,
                                                     host->config.datamodel_count, &segment->system)
                                 : TA_ERROR_NO_MEMORY;
        if (status != TA_OK) {
            free(segment->memory);
            segment->memory = NULL;
            ta_host_free(host);
            return status;
        }
        segment->live = 1;
        segment->busy = 1; /* look at it in the first run */
        segment->deadline = UINT64_MAX;
        p += pad8((size_t)ss->region_length);
    }
    for (uint32_t i = 0; i < hd->moved_count; i++) {
        snapshot_moved m;
        memcpy(&m, p, sizeof m);
        p += sizeof m;
        moved_put(host, m.session, m.segment);
    }
    if (hd->router_length && !ta_router_restore(host, p, (size_t)hd->router_length)) {
        ta_host_free(host);
        return TA_ERROR_INVALID;
    }
    host->message_sequence = hd->message_sequence;
    host->round_robin = hd->segment_count ? hd->round_robin % hd->segment_count : 0;
    host->job_sequence = hd->job_sequence;
    host->routed = hd->routed;
    host->migrated = hd->migrated;
    host->retired = hd->retired;
    versions_recompute(host);
    *out = host;
    return TA_OK;
}
