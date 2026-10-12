/* host.c — many systems as one actor system (see include/tinyactors/host.h).
 *
 * Segments are ta_systems sharing a base URI with TA_IO_PROCESSOR_SCXML_SHARED
 * on their remote processor (index 0), so sends to a session of another
 * segment come out as TA_EFFECT_SEND effects, which this layer delivers
 * itself. Effects that leave the host are copied (strings into a pool,
 * payloads retained) so segments can be acknowledged right away and keep
 * running; the copies live until ta_host_effects_ack / the next run.
 *
 * Memory comes from the config's ta_allocator only (host_internal.h). When
 * it refuses, a call returns TA_ERROR_NO_MEMORY with the host unchanged, and
 * a run stops where it was (a segment's effects are acknowledged up to the
 * first one the host could not take) and reports TA_RUN_EXHAUSTED; the next
 * run goes on from there. */
#include <tinyactors/address.h>
#include <tinyactors/host.h>

#include "base/cbor_output.h"
#include "base/memory.h"
#include "host/host_internal.h"
#include "otlp/otlp_internal.h"
#include "system/system_internal.h"

/* ---- memory ------------------------------------------------------------------------------ */

void *host_alloc(ta_host *host, size_t size) {
    return host->config.allocator.alloc(host->config.allocator.context, size, 8);
}

void host_free(ta_host *host, void *pointer, size_t size) {
    if (pointer) host->config.allocator.free(host->config.allocator.context, pointer, size);
}

int host_grow(ta_host *host, void **array, uint32_t *capacity, uint32_t need, size_t element) {
    if (need <= *capacity) return 1;
    uint32_t c = *capacity ? *capacity : 8;
    while (c < need)
        c *= 2;
    void *grown = host_alloc(host, element * c);
    if (!grown) return 0;
    if (*capacity) ta_memcpy(grown, *array, element * *capacity);
    ta_memset((char *)grown + element * *capacity, 0, element * (c - *capacity));
    host_free(host, *array, element * *capacity);
    *array = grown;
    *capacity = c;
    return 1;
}

/* A scratch buffer of at least n bytes (its old contents are not kept); 0
 * if refused (the old one stays). */
static int buffer_at(ta_host *host, unsigned char **buffer, size_t *capacity, size_t n) {
    if (n > *capacity) {
        unsigned char *p = (unsigned char *)host_alloc(host, n);
        if (!p) return 0;
        host_free(host, *buffer, *capacity);
        *buffer = p;
        *capacity = n;
    }
    return 1;
}

/* ---- small utilities -------------------------------------------------------------- */

static int name_equal(ta_string_view a, const char *b, uint32_t n) {
    return a.length == n && (!n || ta_memcmp(a.pointer, b, n) == 0);
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

static uint32_t moved_slot(const ta_host *host, uint64_t key) {
    uint32_t i = mhash(key, host->moved_capacity);
    while (host->moved_keys[i] && host->moved_keys[i] != key)
        i = (i + 1) & (host->moved_capacity - 1);
    return i;
}

/* Room for `extra` more sessions without growing (removed entries are
 * dropped when the table is rebuilt); 0 if refused (the table unchanged). */
static int moved_reserve(ta_host *host, uint32_t extra) {
    if ((uint64_t)(host->moved_used + extra) * 2 <= host->moved_capacity) return 1;
    uint32_t c = 64;
    while ((uint64_t)(host->moved_count + extra) * 4 > c)
        c *= 2;
    uint64_t *keys = (uint64_t *)host_alloc(host, sizeof(uint64_t) * c);
    uint32_t *values = (uint32_t *)host_alloc(host, sizeof(uint32_t) * c);
    if (!keys || !values) {
        host_free(host, keys, sizeof(uint64_t) * c);
        host_free(host, values, sizeof(uint32_t) * c);
        return 0;
    }
    ta_memset(keys, 0, sizeof(uint64_t) * c);
    ta_memset(values, 0, sizeof(uint32_t) * c);
    uint64_t *old_keys = host->moved_keys;
    uint32_t *old_values = host->moved_values, old_capacity = host->moved_capacity;
    host->moved_keys = keys;
    host->moved_values = values;
    host->moved_capacity = c;
    host->moved_count = host->moved_used = 0;
    for (uint32_t i = 0; i < old_capacity; i++)
        if (old_keys[i] && old_values[i] != TA_NIL) {
            uint32_t at = moved_slot(host, old_keys[i]);
            host->moved_keys[at] = old_keys[i];
            host->moved_values[at] = old_values[i];
            host->moved_count++;
            host->moved_used++;
        }
    host_free(host, old_keys, sizeof(uint64_t) * old_capacity);
    host_free(host, old_values, sizeof(uint32_t) * old_capacity);
    return 1;
}

/* value TA_NIL: remove (never allocates). Adding needs moved_reserve(h, 1)
 * first, or may fail: 0 if refused. */
static int moved_put(ta_host *host, uint64_t key, uint32_t value) {
    if (value != TA_NIL && !moved_reserve(host, 1)) return 0;
    if (!host->moved_capacity) return 1;
    uint32_t i = moved_slot(host, key);
    if (host->moved_keys[i] == key) { /* existing (removed ones keep the key, value TA_NIL) */
        if (host->moved_values[i] == TA_NIL && value != TA_NIL) host->moved_count++;
        if (host->moved_values[i] != TA_NIL && value == TA_NIL) host->moved_count--;
        host->moved_values[i] = value;
    } else if (value != TA_NIL) {
        host->moved_keys[i] = key;
        host->moved_values[i] = value;
        host->moved_count++;
        host->moved_used++;
    }
    return 1;
}

uint32_t host_moved_get(const ta_host *host, uint64_t key) {
    if (!host->moved_capacity) return TA_NIL;
    uint32_t i = mhash(key, host->moved_capacity);
    while (host->moved_keys[i]) {
        if (host->moved_keys[i] == key) return host->moved_values[i];
        i = (i + 1) & (host->moved_capacity - 1);
    }
    return TA_NIL;
}

/* ---- segments ---------------------------------------------------------------------- */

/* A bigger block for a segment: TA_OK; TA_ERROR_NO_MEMORY if the allocator
 * refused it; TA_ERROR_INVALID if it cannot grow (the segment unchanged). */
static ta_status segment_grow(ta_host *host, host_segment *segment) {
    if (host->running) return TA_ERROR_NO_MEMORY; /* nothing grows during a run */
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
    /* the trace contexts and the journal keep up with the events, as their
     * defaults do (ta_system_config_normalize clamps the journal) */
    config.trace_contexts = config.trace_contexts * 2 > TA_SYSTEM_MAX_POOL
                                ? TA_SYSTEM_MAX_POOL
                                : config.trace_contexts * 2;
    config.trace_bytes = config.trace_bytes * 2;
    size_t need;
    if (ta_system_layout(segment->image, &config, &need) != TA_OK) return TA_ERROR_INVALID;
    void *memory = host_alloc(host, need);
    if (!memory) return TA_ERROR_NO_MEMORY;
    ta_system *new_system = segment->system;
    if (ta_system_relocate(&new_system, (ta_memory){memory, need}, &config) != TA_OK) {
        host_free(host, memory, need);
        return TA_ERROR_INVALID;
    }
    host_free(host, segment->memory, segment->memory_size);
    segment->memory = memory;
    segment->memory_size = need;
    segment->system = new_system;
    segment->config = config;
    return TA_OK;
}

static uint32_t image_charts(const ta_image *image) {
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(image, &image_info);
    return image_info.chart_count;
}

/* Room in the version table for the charts of every live segment and
 * `extra` more, so versions_recompute cannot fail; 0 if refused. */
static int versions_reserve(ta_host *host, uint32_t extra) {
    uint32_t need = extra;
    for (uint32_t si = 0; si < host->segment_count; si++)
        if (host->segments[si].live) need += image_charts(host->segments[si].image);
    return host_grow(host, (void **)&host->versions, &host->version_capacity, need,
                     sizeof(chart_version));
}

/* The current version of every chart name: newer segments win. Needs
 * versions_reserve for the live segments first. */
static void versions_recompute(ta_host *host) {
    host->version_count = 0;
    for (uint32_t si = 0; si < host->segment_count; si++) {
        host_segment *segment = segment_of(host, si);
        if (!segment) continue;
        uint32_t charts = image_charts(segment->image);
        for (uint32_t c = 0; c < charts; c++) {
            ta_string_view k = chart_key(segment->image, c);
            uint32_t v = 0;
            while (v < host->version_count &&
                   !name_equal(k, host->versions[v].name.pointer, host->versions[v].name.length))
                v++;
            if (v == host->version_count) host->version_count++;
            /* the name in the newest segment's image: an older one may be retired and freed */
            host->versions[v].name = k;
            host->versions[v].segment = si;
            host->versions[v].chart = c;
        }
    }
}

static const chart_version *version_of(const ta_host *host, ta_string_view name) {
    for (uint32_t v = 0; v < host->version_count; v++)
        if (name_equal(name, host->versions[v].name.pointer, host->versions[v].name.length))
            return &host->versions[v];
    return NULL;
}

/* The host's tracing policy: `policy` (size-aware) with a copy of its rules
 * and their strings, or the default. 0 if the allocator refused (the old
 * one stays). */
static int host_policy_copy(ta_host *host, const ta_trace_policy *policy) {
    ta_trace_policy copy;
    ta_trace_policy_default(&copy);
    if (policy)
        ta_memcpy(&copy, policy,
                  policy->size && policy->size < sizeof copy ? policy->size : sizeof copy);
    copy.size = sizeof copy;
    if (!(policy && policy->size >= offsetof(ta_trace_policy, rules) + sizeof policy->rules &&
          copy.rules))
        copy.rule_count = 0;
    size_t bytes = sizeof(ta_trace_rule) * copy.rule_count;
    for (uint32_t i = 0; i < copy.rule_count; i++)
        bytes += (size_t)copy.rules[i].chart.length + copy.rules[i].actor.length +
                 copy.rules[i].event.length;
    void *block = NULL;
    if (copy.rule_count) {
        block = host_alloc(host, bytes);
        if (!block) return 0;
        ta_trace_rule *rules = (ta_trace_rule *)block;
        char *text = (char *)(rules + copy.rule_count);
        for (uint32_t i = 0; i < copy.rule_count; i++) {
            rules[i] = copy.rules[i];
            ta_string_view *views[3] = {&rules[i].chart, &rules[i].actor, &rules[i].event};
            for (int k = 0; k < 3; k++) {
                if (views[k]->length) ta_memcpy(text, views[k]->pointer, views[k]->length);
                views[k]->pointer = text;
                text += views[k]->length;
            }
        }
        copy.rules = rules;
    } else {
        copy.rules = NULL;
    }
    host_free(host, host->trace_rules, host->trace_rules_bytes);
    host->trace_rules = block;
    host->trace_rules_bytes = bytes;
    host->trace_policy = copy;
    host->config.system.trace_policy = &host->trace_policy;
    return 1;
}

static void rules_free(ta_host *host, host_rules *r);

/* The resource attributes and their strings in one block, kept until
 * ta_host_free: the encoder copies them, and host.config answers them. */
static int resource_copy(ta_host *host, const host_telemetry *telemetry) {
    uint32_t count = telemetry->resource_count;
    if (!count) return 1;
    size_t bytes = sizeof(ta_otlp_attribute) * count;
    for (uint32_t i = 0; i < count; i++)
        bytes += telemetry->resource[i].key.length + telemetry->resource[i].value.length;
    ta_otlp_attribute *attributes = (ta_otlp_attribute *)host_alloc(host, bytes);
    if (!attributes) return 0;
    char *text = (char *)(attributes + count);
    for (uint32_t i = 0; i < count; i++) {
        ta_string_view key = telemetry->resource[i].key, value = telemetry->resource[i].value;
        ta_memcpy(text, key.pointer, key.length);
        attributes[i].key = (ta_string_view){text, key.length};
        text += key.length;
        ta_memcpy(text, value.pointer, value.length);
        attributes[i].value = (ta_string_view){text, value.length};
        text += value.length;
    }
    host->resource_block = attributes;
    host->resource_bytes = bytes;
    host->config.telemetry.resource = attributes;
    host->config.telemetry.resource_count = count;
    return 1;
}

/* A host from its config, read and checked (host_config_read): the strings
 * it keeps are copied, the rest of the config is not looked at again. */
static ta_status host_new(const host_config *config, ta_host **out) {
    *out = NULL;
    ta_host *host = (ta_host *)config->allocator.alloc(config->allocator.context, sizeof *host, 8);
    if (!host) return TA_ERROR_NO_MEMORY;
    ta_memset(host, 0, sizeof *host);
    host->config = *config;
    /* the effective config: the defaults filled in by the code that fills
     * them for a system and an encoder, so host.config answers what runs */
    ta_system_config_normalize((const ta_image_view *)0, &config->system, &host->config.system);
    if (config->telemetry.otlp.size)
        ta_otlp_config_normalize(&config->telemetry.otlp, &host->config.telemetry.otlp);
    if (!host->config.migrate_quantum) host->config.migrate_quantum = 256;
    if (!host->config.max_subscriptions) host->config.max_subscriptions = 64;
    host->config.telemetry.resource = (const ta_otlp_attribute *)0; /* the config's: copied below */
    host->config.telemetry.resource_count = 0;
    host->trace_kinds = host->config.system.trace_policy
                            ? host->config.system.trace_policy->kinds[TA_TRACE_DETAIL_SAMPLED]
                            : TA_TRACE_FULL;
    host->trace_parts = 1;
    host->trace_whole = 1;
    if (!host_policy_copy(host, host->config.system.trace_policy)) {
        config->allocator.free(config->allocator.context, host, sizeof *host);
        return TA_ERROR_NO_MEMORY;
    }
    /* the base URI every segment shares: tinyactors://<system_id>/ */
    uint32_t bl = 0;
    ta_memcpy(host->base, TA_ADDRESS_SCHEME, 13);
    bl = 13;
    ta_memcpy(host->base + bl, config->system_id.pointer, config->system_id.length);
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
    /* the processors' strings: one block, kept until ta_host_free */
    size_t text = 0;
    for (uint32_t i = 0; i < config->io_processor_count; i++)
        text += config->io_processors[i].type.length + config->io_processors[i].location.length;
    if (text) {
        host->io_processor_text = (char *)host_alloc(host, text);
        if (!host->io_processor_text) {
            ta_host_free(host);
            return TA_ERROR_NO_MEMORY;
        }
        host->io_processor_text_length = text;
    }
    char *at = host->io_processor_text;
    for (uint32_t i = 0; i < config->io_processor_count; i++) {
        ta_io_processor_descriptor *d = &host->io_processors[1 + i];
        *d = config->io_processors[i];
        ta_memcpy(at, d->type.pointer, d->type.length);
        d->type.pointer = at;
        at += d->type.length;
        ta_memcpy(at, d->location.pointer, d->location.length);
        d->location.pointer = at;
        at += d->location.length;
    }
    host->config.io_processors = (const ta_io_processor_descriptor *)0; /* the config's: gone */
    host->io_processor_count = 1 + config->io_processor_count;
    host->draining = TA_NIL;
    if (!resource_copy(host, &config->telemetry)) {
        ta_host_free(host);
        return TA_ERROR_NO_MEMORY;
    }
    ta_status status = host_telemetry_init(host);
    if (status != TA_OK) {
        ta_host_free(host);
        return status;
    }
    *out = host;
    return TA_OK;
}

/* The config read from CBOR plus the environment's pointers. */
static ta_status host_config_of(ta_bytes cbor, const ta_host_environment *environment,
                                const host_config_input *storage, host_config *config) {
    if (!environment || environment->size < sizeof *environment || !environment->allocator.alloc ||
        !environment->allocator.free || environment->datamodel_count > 8)
        return TA_ERROR_INVALID;
    host_config_input input = {cbor, environment->error, storage->resource, storage->io_processors};
    ta_status status = host_config_read(&input, config);
    if (status != TA_OK) return status;
    config->allocator = environment->allocator;
    config->datamodels = environment->datamodels;
    config->datamodel_count = environment->datamodel_count;
    config->system.trace_policy = environment->trace_policy;
    return TA_OK;
}

ta_status ta_host_create(ta_bytes cbor, const ta_host_environment *environment, ta_host **out) {
    ta_otlp_attribute resource[HOST_CONFIG_RESOURCE];
    ta_io_processor_descriptor processors[HOST_CONFIG_IO_PROCESSORS];
    host_config_input storage = {{0, 0}, (ta_buffer *)0, resource, processors};
    host_config config;
    *out = NULL;
    ta_status status = host_config_of(cbor, environment, &storage, &config);
    return status == TA_OK ? host_new(&config, out) : status;
}

static void effects_release(ta_host *h);

static void job_free(ta_host *host, migration_job *job) {
    host_free(host, job->name, job->name_length);
    host_free(host, job->retry, sizeof(uint64_t) * job->retry_capacity);
    host_free(host, job->woken, job->retry_capacity);
    host_free(host, job->left, sizeof(uint64_t) * job->left_capacity);
}

void ta_host_free(ta_host *host) {
    if (!host) return;
    host->effects_ready = host->effect_count; /* all of them */
    effects_release(host);
    ta_router_free(host);
    for (uint32_t i = 0; i < host->segment_count; i++)
        if (host->segments[i].live) {
            ta_system_deinit(host->segments[i].system);
            host_free(host, host->segments[i].memory, host->segments[i].memory_size);
        }
    host_free(host, host->segments, sizeof(host_segment) * host->segment_capacity);
    host_free(host, host->versions, sizeof(chart_version) * host->version_capacity);
    for (uint32_t i = 0; i < host->job_count; i++)
        job_free(host, &host->jobs[i]);
    host_free(host, host->jobs, sizeof(migration_job) * host->job_capacity);
    host_free(host, host->moved_keys, sizeof(uint64_t) * host->moved_capacity);
    host_free(host, host->moved_values, sizeof(uint32_t) * host->moved_capacity);
    host_free(host, host->effects, sizeof(ta_host_effect) * host->effect_capacity);
    host_free(host, host->effect_strings, sizeof(effect_strings) * host->effect_capacity);
    host_free(host, host->pool, host->pool_capacity);
    host_free(host, host->blob, host->blob_capacity);
    host_free(host, host->blob2, host->blob2_capacity);
    host_free(host, host->errors, sizeof(pending_error) * host->error_capacity);
    if (host->subscriptions)
        host_free(host, host->subscriptions, sizeof(uint64_t) * host->config.max_subscriptions);
    if (host->io_processor_text) host_free(host, host->io_processor_text, host->io_processor_text_length);
    for (uint32_t i = 0; i < host->retired_chart_count; i++)
        host_free(host, host->retired_charts[i].name,
                  host->retired_charts[i].name_length ? host->retired_charts[i].name_length : 1);
    host_free(host, host->retired_charts, sizeof(retired_chart) * host->retired_chart_capacity);
    host_free(host, host->trace_rules, host->trace_rules_bytes);
    host_free(host, host->resource_block, host->resource_bytes);
    host_telemetry_free(host);
    rules_free(host, &host->trace_options);
    for (uint32_t i = 0; i < host->watch_count; i++)
        rules_free(host, &host->watches[i].rules);
    host_free(host, host->watches, sizeof(host_watch) * host->watch_capacity);
    ta_allocator allocator = host->config.allocator;
    allocator.free(allocator.context, host, sizeof *host);
}

static void reserve_serials(ta_host *h, uint32_t index);

ta_status ta_host_add_image(ta_host *host, const ta_image *image, uint32_t *segment) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t index = host->segment_count;
    if (index >= (1u << 20)) return TA_ERROR_NO_MEMORY; /* session ids stay exact in a double */
    if (!host_grow(host, (void **)&host->segments, &host->segment_capacity, index + 1,
                   sizeof(host_segment)) ||
        !versions_reserve(host, image_charts(image)))
        return TA_ERROR_NO_MEMORY;
    host_segment *segment_record = &host->segments[index];
    ta_memset(segment_record, 0, sizeof *segment_record);
    segment_record->image = image;
    segment_record->config = host->config.system; /* the effective config (host_new) */
    segment_record->config.io_processors = host->io_processors;
    segment_record->config.io_processor_count = host->io_processor_count;
    segment_record->config.first_serial = (uint64_t)index << SERIALS_SHIFT;
    /* segments draw different trace IDs from one seed */
    segment_record->config.trace_seed =
        host->config.system.trace_seed + (uint64_t)index * 0x9E3779B97F4A7C15ull;
    size_t need;
    ta_status status = ta_system_layout(image, &segment_record->config, &need);
    if (status != TA_OK) return status;
    segment_record->memory = host_alloc(host, need);
    if (!segment_record->memory) return TA_ERROR_NO_MEMORY;
    segment_record->memory_size = need;
    ta_system_setup setup = {{segment_record->memory, need}, image,
                             &segment_record->config,        host->datamodels,
                             host->config.datamodel_count,   0};
    status = ta_system_init(&setup, &segment_record->system);
    if (status != TA_OK) {
        host_free(host, segment_record->memory, need);
        segment_record->memory = NULL;
        return status;
    }
    ta_system_trace_clock(segment_record->system, host->fuel_clock);
    ta_system_advance_time(segment_record->system, host->now);
    segment_record->live = 1;
    segment_record->busy = 1;
    segment_record->deadline = UINT64_MAX;
    host->segment_count++;
    reserve_serials(host, index);
    versions_recompute(host);
    host->encoder->definitions(host, HOST_DEFINITION_LINKED, index);
    if (segment) *segment = index;
    return TA_OK;
}

ta_status ta_host_chart(const ta_host *host, ta_string_view name, ta_host_chart_ref *out) {
    const chart_version *version = version_of(host, name);
    if (!version) return TA_ERROR_INVALID;
    out->segment = version->segment;
    out->chart = version->chart;
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
    ta_memset(&z, 0, sizeof z);
    z.size = sizeof z;
    z.live = (uint32_t)segment_record->live;
    z.image = segment_record->image;
    if (segment_record->live) {
        z.actors = segment_actors(segment_record);
        for (uint32_t v = 0; v < host->version_count; v++)
            z.current += host->versions[v].segment == segment;
    }
    uint32_t n = out->size && out->size < sizeof z ? out->size : (uint32_t)sizeof z;
    ta_memcpy(out, &z, n);
    out->size = n;
    return TA_OK;
}

ta_system *ta_host_system(ta_host *host, uint32_t segment) {
    host_segment *segment_record = segment_of(host, segment);
    return segment_record ? segment_record->system : NULL;
}

/* ---- actors ------------------------------------------------------------------------- */

ta_status ta_host_locate(const ta_host *host, uint64_t session, ta_host_location *out) {
    return host_locate(host, session, &out->segment, &out->actor);
}

ta_status host_locate(const ta_host *host, uint64_t session, uint32_t *segment, uint32_t *actor) {
    uint32_t si = host_moved_get(host, session);
    if (si == TA_NIL) si = (uint32_t)(session >> SERIALS_SHIFT);
    const host_segment *segment_record = segment_of(host, si);
    uint32_t a = segment_record ? ta_system_actor_by_session(segment_record->system, session) : 0;
    if (!a) return TA_ERROR_STALE;
    if (segment) *segment = si;
    if (actor) *actor = a;
    return TA_OK;
}

/* ---- subscriptions (actor.subscribe) --------------------------------------------------- */

static uint32_t subscription_find(const ta_host *host, uint64_t session) {
    for (uint32_t i = 0; i < host->subscription_count; i++)
        if (host->subscriptions[i] == session) return i;
    return TA_NIL;
}

ta_status host_subscribe(ta_host *host, uint64_t session) {
    if (host_locate(host, session, NULL, NULL) != TA_OK) return TA_ERROR_STALE;
    if (subscription_find(host, session) != TA_NIL) return TA_OK;
    if (host->subscription_count == host->config.max_subscriptions) return TA_ERROR_STATE;
    if (!host->subscriptions) { /* the whole table, once, with the first (never during a run) */
        host->subscriptions =
            (uint64_t *)host_alloc(host, sizeof(uint64_t) * host->config.max_subscriptions);
        if (!host->subscriptions) return TA_ERROR_NO_MEMORY;
    }
    host->subscriptions[host->subscription_count++] = session;
    return TA_OK;
}

ta_status host_unsubscribe(ta_host *host, uint64_t session) {
    uint32_t i = subscription_find(host, session), segment, actor;
    if (i == TA_NIL) return TA_ERROR_STALE;
    host->subscriptions[i] = host->subscriptions[--host->subscription_count];
    if (host_locate(host, session, &segment, &actor) == TA_OK)
        ta_actor_watch(segment_of(host, segment)->system, actor, 0);
    return TA_OK;
}

/* Before a run: watch every subscribed session again (its watch ends with
 * the CHANGED of the last run, or the actor was woken or moved since);
 * sessions that ended leave the list. */
static void subscriptions_arm(ta_host *host) {
    for (uint32_t i = 0; i < host->subscription_count;) {
        uint32_t segment, actor;
        if (host_locate(host, host->subscriptions[i], &segment, &actor) != TA_OK) {
            host->subscriptions[i] = host->subscriptions[--host->subscription_count];
            continue;
        }
        ta_actor_watch(segment_of(host, segment)->system, actor, 1); /* hibernated: next time */
        i++;
    }
}

/* ---- state values ---------------------------------------------------------------------- */

/* A session's configuration as a state value (docs/requests.md, "state"):
 * the active states in document order, each written once its parent is,
 * so every map's count is known before its items. TA_ERROR_INVALID when
 * there is no such session or its configuration has more than
 * HOST_STATE_VALUE_STATES states. */
ta_status host_state_value(const ta_host *host, uint64_t session, cbor_output *out) {
    uint32_t si = 0, actor = 0, states[HOST_STATE_VALUE_STATES];
    uint8_t depths[HOST_STATE_VALUE_STATES], kinds[HOST_STATE_VALUE_STATES];
    if (host_locate(host, session, &si, &actor) != TA_OK) return TA_ERROR_INVALID;
    const host_segment *segment_record = segment_of(host, si);
    ta_actor_info info;
    info.size = sizeof info;
    if (ta_actor_info_get(segment_record->system, actor, &info) != TA_OK) return TA_ERROR_INVALID;
    uint32_t count = ta_actor_config(segment_record->system, actor,
                                     (ta_uint32_array){states, HOST_STATE_VALUE_STATES});
    if (count > HOST_STATE_VALUE_STATES) return TA_ERROR_INVALID;
    for (uint32_t i = 1; i < count; i++) /* document order: a parent before its children */
        for (uint32_t j = i; j > 0 && states[j - 1] > states[j]; j--) {
            uint32_t t = states[j];
            states[j] = states[j - 1];
            states[j - 1] = t;
        }
    /* the states of the configuration: those whose parent is active too */
    uint32_t kept = 0, top = 0;
    for (uint32_t i = 0; i < count; i++) {
        ta_state_info state;
        state.size = sizeof state;
        if (states[i] == 0 ||
            ta_image_state_info(segment_record->image, (ta_chart_item){info.chart, states[i]},
                                &state) != TA_OK ||
            state.depth == 0 || state.depth > 255 || state.kind == TA_STATE_HISTORY_SHALLOW ||
            state.kind == TA_STATE_HISTORY_DEEP)
            continue;
        if (state.depth > top + 1) continue; /* its parent is not active: not a configuration */
        top = state.depth;
        states[kept] = states[i];
        depths[kept] = (uint8_t)state.depth;
        kinds[kept++] = (uint8_t)state.kind;
    }
    if (!kept) {
        cbor_output_head(out, CBOR_MAJOR_MAP, 0);
        return TA_OK;
    }
    uint8_t parents[256]; /* the kind of the state open at each depth; the root acts compound */
    parents[0] = TA_STATE_COMPOUND;
    for (uint32_t i = 0; i < kept; i++) {
        ta_state_info state;
        state.size = sizeof state;
        ta_image_state_info(segment_record->image, (ta_chart_item){info.chart, states[i]}, &state);
        uint32_t depth = depths[i];
        int in_parallel = parents[depth - 1] == TA_STATE_PARALLEL;
        if (in_parallel) cbor_output_text(out, state.id);
        if (kinds[i] == TA_STATE_COMPOUND || kinds[i] == TA_STATE_PARALLEL) {
            if (!in_parallel) {
                cbor_output_head(out, CBOR_MAJOR_MAP, 1);
                cbor_output_text(out, state.id);
            }
            uint32_t children = 0;
            for (uint32_t j = i + 1; j < kept && depths[j] > depth; j++)
                children += depths[j] == depth + 1;
            if (kinds[i] == TA_STATE_PARALLEL) cbor_output_head(out, CBOR_MAJOR_MAP, children);
            else if (!children) cbor_output_head(out, CBOR_MAJOR_MAP, 0);
        } else if (in_parallel) {
            cbor_output_head(out, CBOR_MAJOR_MAP, 0);
        } else {
            cbor_output_text(out, state.id);
        }
        parents[depth] = kinds[i];
    }
    return TA_OK;
}

ta_status ta_host_spawn(ta_host *host, const ta_spawn *spawn, uint64_t *session) {
    host_segment *segment_record = segment_of(host, spawn->segment);
    if (!segment_record) return TA_ERROR_INVALID;
    uint32_t actor = 0;
    ta_status status;
    while ((status = ta_system_spawn(segment_record->system, spawn, &actor)) ==
               TA_ERROR_NO_MEMORY &&
           segment_grow(host, segment_record) == TA_OK) {
    }
    if (status != TA_OK) return status;
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    ta_actor_info_get(segment_record->system, actor, &actor_info);
    if (session) *session = actor_info.serial;
    segment_record->busy = 1;
    return TA_OK;
}

ta_status host_send_to(ta_host *host, uint32_t si, uint32_t actor, const ta_event_in *event) {
    host_segment *segment = &host->segments[si];
    ta_event_in event_in = *event;
    event_in.target = actor;
    ta_status status;
    while ((status = ta_system_send(segment->system, &event_in)) == TA_ERROR_NO_MEMORY &&
           segment_grow(host, segment) == TA_OK) {
    }
    if (status == TA_OK) segment->busy = 1;
    return status;
}

ta_status ta_host_send(ta_host *host, const ta_event_in *event) {
    uint32_t si, actor;
    ta_status status = host_locate(host, event->session, &si, &actor);
    return status == TA_OK ? host_send_to(host, si, actor, event) : status;
}

uint32_t ta_host_send_many(ta_host *host, const ta_event_in *events, uint32_t n) {
    ta_event_in batch[64];
    uint32_t done = 0;
    while (done < n) {
        uint32_t si, actor;
        if (host_locate(host, events[done].session, &si, &actor) != TA_OK) return done;
        /* a run of events for the same segment, one ta_system_send_many */
        uint32_t k = 0;
        while (done + k < n && k < 64) {
            uint32_t s2, a2;
            if (k && (host_locate(host, events[done + k].session, &s2, &a2) != TA_OK || s2 != si))
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
            if (host_send_to(host, si, batch[got].target, &batch[got]) != TA_OK) return done;
            done++;
        }
    }
    return done;
}

ta_status ta_host_terminate(ta_host *host, uint64_t session) {
    uint32_t si, actor;
    ta_status status = host_locate(host, session, &si, &actor);
    if (status != TA_OK) return status;
    host->segments[si].busy = 1;
    return ta_system_terminate(host->segments[si].system, actor);
}

ta_status ta_host_resume(ta_host *host, const ta_resume *resume) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    host_segment *segment_record = segment_of(host, resume->segment);
    if (!segment_record) return TA_ERROR_INVALID;
    segment_record->busy = 1;
    return ta_system_resume(segment_record->system, resume);
}

/* ---- effects ------------------------------------------------------------------------ */

/* The old block of the effect array or the pool, replaced by a bigger one:
 * freed now, or held while a published batch still points into it. */
static void effects_block_replaced(ta_host *host, void *block, size_t size) {
    if (!block) return;
    if (!host->effects_ready) {
        host_free(host, block, size);
        return;
    }
    host->held[host->held_count] = block;
    host->held_size[host->held_count++] = size;
}

/* The pool offset of string k (EFFECT_STRINGS, in that order: seven texts,
 * then the wire bytes) of effect e, and its length. */
static size_t *effect_string(ta_host *host, uint32_t e, uint32_t k, uint32_t *length) {
    const ta_host_effect *effect = &host->effects[e];
    const ta_string_view *views[EFFECT_STRINGS - 1] = {
        &effect->target, &effect->event,      &effect->sendid,           &effect->auxiliary,
        &effect->msgid,  &effect->origintype, &effect->deduplication_key};
    *length = k < EFFECT_STRINGS - 1 ? views[k]->length : (uint32_t)effect->wire.length;
    return &host->effect_strings[e].offset[k];
}

/* Move the strings still referenced — the kept effects' and the pending
 * errors' sendids — to the front of the pool. Both lists are in pool order
 * (strings are appended as effects and errors are made, and this keeps their
 * order), so taking them merged, lowest offset first, moves every string
 * towards the front, over bytes already taken. Nobody borrows them: the kept
 * effects were made after the last ta_host_effects. */
static void pool_compact(ta_host *host) {
    size_t write = 0;
    uint32_t e = 0, k = 0, r = 0;
    for (;;) {
        size_t *slot = NULL;
        uint32_t length = 0;
        while (e < host->effect_count) { /* the next effect string in the pool */
            slot = effect_string(host, e, k, &length);
            if (*slot != (size_t)-1) break;
            slot = NULL;
            if (++k == EFFECT_STRINGS) {
                k = 0;
                e++;
            }
        }
        while (r < host->error_count && host->errors[r].sendid == (size_t)-1)
            r++;
        pending_error *error = r < host->error_count ? &host->errors[r] : NULL;
        if (!slot && !error) break;
        int take_error = error && (!slot || error->sendid < *slot);
        size_t *at = take_error ? &error->sendid : slot;
        uint32_t n = take_error ? error->sendid_length : length;
        TA_ASSERT(write <= *at && *at + n <= host->pool_length);
        if (write != *at) ta_memmove(host->pool + write, host->pool + *at, n);
        *at = write;
        write += n;
        if (take_error) {
            r++;
        } else if (++k == EFFECT_STRINGS) {
            k = 0;
            e++;
        }
    }
    host->pool_length = write;
}

/* Release the effects the host has seen (ta_host_effects): those before
 * effects_ready. Effects made since — by calls between runs, such as
 * ta_host_deliver answering with an `err` — stay for the next batch. The
 * borrow ends here: the blocks it held are freed. */
static void effects_release(ta_host *host) {
    for (uint32_t i = 0; i < host->held_count; i++)
        host_free(host, host->held[i], host->held_size[i]);
    host->held_count = 0;
    uint32_t seen = host->effects_ready;
    for (uint32_t i = 0; i < seen; i++) {
        ta_host_effect *effect = &host->effects[i];
        if (effect->payload && effect->datamodel < host->config.datamodel_count &&
            host->datamodels[effect->datamodel].vtable->release)
            host->datamodels[effect->datamodel].vtable->release(
                host->datamodels[effect->datamodel].datamodel, effect->payload);
    }
    uint32_t keep = host->effect_count - seen;
    if (keep) { /* the unseen ones to the front */
        ta_memmove(host->effects, host->effects + seen, keep * sizeof *host->effects);
        ta_memmove(host->effect_strings, host->effect_strings + seen,
                   keep * sizeof *host->effect_strings);
    }
    host->effect_count = keep;
    host->effects_ready = 0;
    pool_compact(host); /* (nothing kept and no error pending: the pool empties) */
}

int host_effects_reserve(ta_host *host, uint32_t count, size_t bytes) {
    uint32_t need = host->effect_count + count;
    int grow_effects = need > host->effect_capacity;
    int grow_pool = host->pool_length + bytes > host->pool_capacity;
    /* room to hold both old blocks while a batch is borrowed (refused otherwise) */
    if (host->effects_ready &&
        host->held_count + (uint32_t)grow_effects + (uint32_t)grow_pool > HELD_BLOCKS_MAX)
        return 0;
    if (grow_effects) { /* both arrays together: they share the capacity */
        uint32_t c = host->effect_capacity ? host->effect_capacity : 8;
        while (c < need)
            c *= 2;
        ta_host_effect *effects = (ta_host_effect *)host_alloc(host, sizeof *effects * c);
        effect_strings *strings = (effect_strings *)host_alloc(host, sizeof *strings * c);
        if (!effects || !strings) {
            host_free(host, effects, sizeof *effects * c);
            host_free(host, strings, sizeof *strings * c);
            return 0;
        }
        if (host->effect_count) {
            ta_memcpy(effects, host->effects, sizeof *effects * host->effect_count);
            ta_memcpy(strings, host->effect_strings, sizeof *strings * host->effect_count);
        }
        /* (effect_strings is the host's own: no borrower points into it) */
        effects_block_replaced(host, host->effects, sizeof *effects * host->effect_capacity);
        host_free(host, host->effect_strings, sizeof *strings * host->effect_capacity);
        host->effects = effects;
        host->effect_strings = strings;
        host->effect_capacity = c;
    }
    if (grow_pool) {
        size_t c = host->pool_capacity ? host->pool_capacity : 4096;
        while (c < host->pool_length + bytes)
            c *= 2;
        char *pool = (char *)host_alloc(host, c);
        if (!pool) return 0;
        if (host->pool_length) ta_memcpy(pool, host->pool, host->pool_length);
        effects_block_replaced(host, host->pool, host->pool_capacity);
        host->pool = pool;
        host->pool_capacity = c;
    }
    return 1;
}

/* A string into the pool: its offset, (size_t)-1 when empty. Needs
 * host_effects_reserve for its bytes first. */
size_t host_pool_put(ta_host *host, ta_string_view s) {
    if (!s.length || !host_effects_reserve(host, 0, s.length)) return (size_t)-1;
    ta_memcpy(host->pool + host->pool_length, s.pointer, s.length);
    host->pool_length += s.length;
    return host->pool_length - s.length;
}

/* A new effect; NULL only without host_effects_reserve first. */
const ta_datamodel_binding *host_chart_datamodel(const ta_host *host, uint32_t segment,
                                                 uint32_t chart) {
    ta_string_view name = ta_image_chart_datamodel(host->segments[segment].image, chart);
    for (uint32_t i = 0; i < host->config.datamodel_count; i++) {
        const char *want = host->datamodels[i].vtable->name;
        uint32_t length = 0;
        while (want[length]) length++;
        if (length == name.length && ta_memcmp(want, name.pointer, length) == 0)
            return &host->datamodels[i];
    }
    return (const ta_datamodel_binding *)0;
}

ta_host_effect *host_effect_new(ta_host *host, uint32_t kind, uint32_t segment) {
    if (!host_effects_reserve(host, 1, 0)) return NULL;
    ta_host_effect *effect = &host->effects[host->effect_count];
    ta_memset(effect, 0, sizeof *effect);
    effect->size = sizeof *effect;
    effect->kind = kind;
    effect->segment = segment;
    effect->job = TA_NIL;
    effect->chart = TA_NIL;
    effect->datamodel = TA_NIL;
    for (int k = 0; k < EFFECT_STRINGS; k++)
        host->effect_strings[host->effect_count].offset[k] = (size_t)-1;
    host->effect_count++;
    return effect;
}

void host_effect_strings_set(ta_host *host, ta_string_view t, ta_string_view event,
                             ta_string_view sid, ta_string_view aux) {
    effect_strings *strings = &host->effect_strings[host->effect_count - 1];
    ta_host_effect *effect = &host->effects[host->effect_count - 1];
    ta_string_view v[4] = {t, event, sid, aux};
    ta_string_view *dst[4] = {&effect->target, &effect->event, &effect->sendid, &effect->auxiliary};
    for (int k = 0; k < 4; k++) {
        strings->offset[k] = host_pool_put(host, v[k]);
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
static ta_status effect_copy(ta_host *host, uint32_t si, const ta_effect *effect,
                             uint32_t io_processor) {
    if (!host_effects_reserve(host, 1,
                              (size_t)effect->target.length + effect->event.length +
                                  effect->sendid.length + effect->auxiliary.length))
        return TA_ERROR_NO_MEMORY;
    ta_host_effect *host_effect = host_effect_new(host, effect->kind, si);
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
    host_effect->trace = effect->trace;
    host_effect_strings_set(host, effect->target, effect->event, effect->sendid, effect->auxiliary);
    if (effect->payload && effect->datamodel < host->config.datamodel_count) {
        host_effect->payload = effect->payload;
        host_effect->datamodel = effect->datamodel;
        if (host->datamodels[effect->datamodel].vtable->retain)
            host->datamodels[effect->datamodel].vtable->retain(
                host->datamodels[effect->datamodel].datamodel, effect->payload);
    }
    return TA_OK;
}

uint32_t ta_host_effects(ta_host *host, const ta_host_effect **out) {
    if (host->running) { /* not during a run (host.h) */
        *out = (const ta_host_effect *)0;
        return 0;
    }
    host->effects_ready =
        host->effect_count; /* these are seen now: released by the next ack or run */
    for (uint32_t i = 0; i < host->effect_count; i++) {
        ta_host_effect *effect = &host->effects[i];
        ta_string_view *dst[EFFECT_STRINGS - 1] = {
            &effect->target, &effect->event,      &effect->sendid,           &effect->auxiliary,
            &effect->msgid,  &effect->origintype, &effect->deduplication_key};
        const size_t *offset = host->effect_strings[i].offset;
        for (int k = 0; k < EFFECT_STRINGS - 1; k++)
            dst[k]->pointer = offset[k] == (size_t)-1 ? "" : host->pool + offset[k];
        effect->wire.pointer = offset[EFFECT_STRINGS - 1] == (size_t)-1
                                   ? (const void *)""
                                   : host->pool + offset[EFFECT_STRINGS - 1];
    }
    *out = host->effects;
    return host->effect_count;
}

uint64_t ta_host_effect_number(const ta_host *host, uint32_t index, uint32_t field) {
    if (index >= host->effects_ready) return 0;
    const ta_host_effect *effect = &host->effects[index];
    switch (field) {
    case TA_EFFECT_FIELD_KIND: return effect->kind;
    case TA_EFFECT_FIELD_SEGMENT: return effect->segment;
    case TA_EFFECT_FIELD_ACTOR: return effect->actor;
    case TA_EFFECT_FIELD_SESSION: return effect->session;
    case TA_EFFECT_FIELD_ROOT: return effect->root;
    case TA_EFFECT_FIELD_TOKEN: return effect->token;
    case TA_EFFECT_FIELD_IO_PROCESSOR: return effect->io_processor;
    case TA_EFFECT_FIELD_FLAGS: return effect->flags;
    case TA_EFFECT_FIELD_JOB: return effect->job;
    case TA_EFFECT_FIELD_PAYLOAD: return effect->payload;
    case TA_EFFECT_FIELD_DATAMODEL: return effect->datamodel;
    case TA_EFFECT_FIELD_DELAY_HINT: return effect->delay_hint;
    case TA_EFFECT_FIELD_CHART: return effect->chart;
    case TA_EFFECT_FIELD_MIGRATED: return effect->migrated;
    case TA_EFFECT_FIELD_FAILED: return effect->failed;
    case TA_EFFECT_FIELD_PENDING: return effect->pending;
    case TA_EFFECT_FIELD_PEER: return effect->peer;
    default: return 0;
    }
}

ta_bytes ta_host_effect_bytes(const ta_host *host, uint32_t index, uint32_t field) {
    ta_bytes none = {"", 0};
    if (index >= host->effects_ready) return none;
    const ta_host_effect *effect = &host->effects[index];
    switch (field) {
    case TA_EFFECT_FIELD_WIRE: return effect->wire;
    default: return none;
    }
}

ta_string_view ta_host_effect_text(ta_host *host, uint32_t index, uint32_t field) {
    ta_string_view none = {"", 0};
    if (index >= host->effects_ready) return none;
    const ta_host_effect *effect = &host->effects[index];
    switch (field) {
    case TA_EFFECT_FIELD_TARGET: return effect->target;
    case TA_EFFECT_FIELD_EVENT: return effect->event;
    case TA_EFFECT_FIELD_SENDID: return effect->sendid;
    case TA_EFFECT_FIELD_AUXILIARY: return effect->auxiliary;
    case TA_EFFECT_FIELD_MSGID: return effect->msgid;
    case TA_EFFECT_FIELD_ORIGINTYPE: return effect->origintype;
    case TA_EFFECT_FIELD_DEDUPLICATION_KEY: return effect->deduplication_key;
    case TA_EFFECT_FIELD_TRACEPARENT: {
        ta_buffer out = {host->effect_traceparent, sizeof host->effect_traceparent, 0};
        if (ta_traceparent_format(&effect->trace, &out) != TA_OK) return none;
        return (ta_string_view){host->effect_traceparent, (uint32_t)out.length};
    }
    default: return none;
    }
}

void ta_host_effects_ack(ta_host *host) {
    if (host->running) return; /* not during a run (host.h) */
    effects_release(host);
}

/* ---- routing ---------------------------------------------------------------------- */

static ta_status communication_error_now(ta_host *host, uint32_t si, uint32_t actor,
                                         ta_string_view sendid) {
    ta_event_in event;
    ta_host_error_event(sendid, &event);
    /* a sender that is gone has nobody to tell; only memory is worth retrying */
    return host_send_to(host, si, actor, &event) == TA_ERROR_NO_MEMORY ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* error.communication for a sender. While a segment's effects are being
 * drained it cannot take events (not before its ack): those are kept and
 * delivered right after (drain). */
ta_status host_communication_error(ta_host *host, uint32_t si, uint32_t actor,
                                   ta_string_view sendid) {
    if (host->draining != si) return communication_error_now(host, si, actor, sendid);
    if (!host_grow(host, (void **)&host->errors, &host->error_capacity, host->error_count + 1,
                   sizeof *host->errors) ||
        !host_effects_reserve(host, 0, sendid.length))
        return TA_ERROR_NO_MEMORY;
    pending_error *error = &host->errors[host->error_count++];
    error->segment = si;
    error->actor = actor;
    error->sendid = host_pool_put(host, sendid);
    error->sendid_length = sendid.length;
    return TA_OK;
}

/* Deliver the pending errors in order; those from the first one a segment
 * cannot take (no memory) on stay pending. */
static ta_status errors_deliver(ta_host *host) {
    uint32_t done = 0;
    ta_status status = TA_OK;
    for (; done < host->error_count; done++) {
        const pending_error *error = &host->errors[done];
        if (!segment_of(host, error->segment)) continue;
        ta_string_view sendid = {error->sendid == (size_t)-1 ? "" : host->pool + error->sendid,
                                 error->sendid == (size_t)-1 ? 0 : error->sendid_length};
        status = communication_error_now(host, error->segment, error->actor, sendid);
        if (status != TA_OK) break;
    }
    host->error_count -= done;
    ta_memmove(host->errors, host->errors + done, host->error_count * sizeof *host->errors);
    return status;
}

/* Drain a segment's effects: route, copy out, acknowledge. TA_ERROR_NO_MEMORY:
 * the effects from the first one the host could not take on are not
 * acknowledged (they come again with the next drain), or an error for a
 * sender is still pending. */
static ta_status drain(ta_host *host, uint32_t si) {
    host_segment *segment = &host->segments[si];
    ta_system *system = segment->system;
    const ta_effect *effect;
    uint32_t n = ta_system_effects(system, &effect), done = 0;
    ta_status status = TA_OK;
    host->draining = si;
    for (; done < n; done++) {
        const ta_effect *effect_view = &effect[done];
        if (effect_view->kind == TA_EFFECT_CHANGED &&
            subscription_find(host, effect_view->session) == TA_NIL)
            status = TA_OK; /* unsubscribed since it was watched */
        else if (effect_view->kind == TA_EFFECT_SEND && effect_view->io_processor == 0)
            status = ta_router_outbound(host, si, effect_view);
        else if (effect_view->kind == TA_EFFECT_SEND)
            status = effect_copy(
                host, si, effect_view,
                effect_view->io_processor == TA_NIL ? TA_NIL : effect_view->io_processor - 1);
        else
            status = effect_copy(host, si, effect_view, TA_NIL);
        if (status == TA_OK &&
            (effect_view->kind == TA_EFFECT_DONE || effect_view->kind == TA_EFFECT_ERROR) &&
            host_moved_get(host, effect_view->session) != TA_NIL)
            moved_put(host, effect_view->session, TA_NIL);
        if (segment->system != system) { /* grown while routing: its effects moved with it */
            system = segment->system;
            ta_system_effects(system, &effect);
        }
        if (status != TA_OK) break;
    }
    ta_system_effects_ack(system, done);
    host->draining = TA_NIL;
    if (done < n) segment->busy = 1;
    ta_status delivered = errors_deliver(host);
    return status != TA_OK ? status : delivered;
}

/* ---- migration ------------------------------------------------------------------------ */

static migration_job *job_by_id(const ta_host *host, uint32_t id) {
    for (uint32_t i = 0; i < host->job_count; i++)
        if (host->jobs[i].id == id) return &host->jobs[i];
    return NULL;
}

/* Room for `extra` more sessions in a job's retry list; 0 if refused. */
static int job_reserve(ta_host *host, migration_job *job, uint32_t extra) {
    uint32_t need = job->retry_count + extra;
    if (need <= job->retry_capacity) return 1;
    uint32_t c = job->retry_capacity ? job->retry_capacity : 8;
    while (c < need)
        c *= 2;
    uint64_t *retry = (uint64_t *)host_alloc(host, sizeof(uint64_t) * c);
    uint8_t *woken = (uint8_t *)host_alloc(host, c);
    if (!retry || !woken) {
        host_free(host, retry, sizeof(uint64_t) * c);
        host_free(host, woken, c);
        return 0;
    }
    if (job->retry_count) {
        ta_memcpy(retry, job->retry, sizeof(uint64_t) * job->retry_count);
        ta_memcpy(woken, job->woken, job->retry_count);
    }
    host_free(host, job->retry, sizeof(uint64_t) * job->retry_capacity);
    host_free(host, job->woken, job->retry_capacity);
    job->retry = retry;
    job->woken = woken;
    job->retry_capacity = c;
    return 1;
}

ta_status ta_host_migrate(ta_host *host, const ta_host_migration *migration, uint32_t *id) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    ta_string_view name = migration->name;
    uint64_t session = migration->session;
    uint32_t flags = migration->flags;
    ta_time timeout = migration->timeout;
    if (!host_grow(host, (void **)&host->jobs, &host->job_capacity, host->job_count + 1,
                   sizeof(migration_job)))
        return TA_ERROR_NO_MEMORY;
    migration_job job;
    ta_memset(&job, 0, sizeof job);
    if (name.length) {
        job.name = (char *)host_alloc(host, name.length);
        if (!job.name) return TA_ERROR_NO_MEMORY;
        ta_memcpy(job.name, name.pointer, name.length);
        job.name_length = name.length;
    }
    if (session) {
        if (!job_reserve(host, &job, 1)) {
            job_free(host, &job);
            return TA_ERROR_NO_MEMORY;
        }
        job.retry[0] = session;
        job.woken[0] = 0;
        job.retry_count = 1;
    }
    job.id = ++host->job_sequence;
    job.active = 1;
    job.flags = flags;
    job.session = session;
    job.deadline = timeout ? host->now + timeout : 0;
    job.walking = session == 0;
    host->jobs[host->job_count++] = job;
    if (id) *id = job.id;
    return TA_OK;
}

uint32_t ta_host_job_pending(const ta_host *host, uint32_t id, ta_uint64_array out) {
    const migration_job *job = job_by_id(host, id);
    if (!job) return 0;
    uint32_t n = job->active ? job->retry_count : job->left_count;
    const uint64_t *source = job->active ? job->retry : job->left;
    for (uint32_t i = 0; i < n && i < out.capacity; i++)
        out.pointer[i] = source[i];
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
    if (job->name_length && !name_equal(k, job->name, job->name_length)) return NULL;
    const chart_version *version = version_of(host, k);
    return version && version->segment != si ? version : NULL;
}

enum { MIGRATE_NO_MEMORY = -3, MIGRATE_SKIP = -2, MIGRATE_FAILED = -1, MIGRATE_LATER = 0 };

/* Adopt an exported blob back where it came from. The export just freed
 * what it needs there, so this does not need to grow. */
static void adopt_home(ta_host *host, host_segment *from, const unsigned char *blob,
                       size_t length) {
    uint32_t actor;
    while (ta_system_adopt(from->system, (ta_bytes){blob, length}, &actor) == TA_ERROR_NO_MEMORY &&
           segment_grow(host, from) == TA_OK) {
    }
    from->busy = 1;
}

/* 1 moved; MIGRATE_LATER (busy / asleep), MIGRATE_FAILED, MIGRATE_SKIP (not a
 * candidate anymore), MIGRATE_NO_MEMORY (the actor stays where it was; later) */
static int migrate_one(ta_host *host, migration_job *job, uint64_t session, uint8_t *woken) {
    uint32_t si, actor;
    if (host_locate(host, session, &si, &actor) != TA_OK) return MIGRATE_SKIP;
    ta_actor_info actor_info;
    const chart_version *version = candidate(host, job, si, actor, &actor_info);
    if (!version) return MIGRATE_SKIP;
    host_segment *from = &host->segments[si], *to = &host->segments[version->segment];
    if (actor_info.state == TA_ACTOR_HIBERNATED) {
        if ((job->flags & TA_HOST_MIGRATE_WAKE) && woken && !*woken) {
            if (!host_effects_reserve(host, 1, 0)) return MIGRATE_NO_MEMORY;
            ta_host_effect *effect =
                host_effect_new(host, TA_EFFECT_WAKE, si); /* the host loads it */
            effect->actor = actor;
            effect->session = effect->root = session;
            effect->job = job->id;
            *woken = 1;
        }
        return MIGRATE_LATER;
    }
    if (actor_info.invocations) return MIGRATE_LATER; /* a session tree: once they finished */
    uint32_t chart = version->chart, tseg = version->segment;
    ta_string_view key = chart_key(to->image, chart);
    /* everything the move needs once the actor has left its segment */
    if (!host_effects_reserve(host, 1, key.length) || !moved_reserve(host, 1) ||
        !buffer_at(host, &host->blob, &host->blob_capacity, 4096))
        return MIGRATE_NO_MEMORY;
    ta_buffer exported = {host->blob, host->blob_capacity, 0};
    ta_status status = ta_actor_export(from->system, actor, &exported);
    if (status == TA_ERROR_NO_MEMORY) {
        if (!buffer_at(host, &host->blob, &host->blob_capacity, exported.length))
            return MIGRATE_NO_MEMORY;
        exported = (ta_buffer){host->blob, host->blob_capacity, 0};
        status = ta_actor_export(from->system, actor, &exported);
    }
    if (status == TA_ERROR_STATE) return MIGRATE_LATER;
    if (status != TA_OK) return MIGRATE_FAILED;
    size_t length = exported.length;
    int moved = 0, refused = 0;
    ta_blob_migration migration = {from->image, to->image, chart, 0, {host->blob, length}};
    ta_buffer migrated = {(void *)0, 0, 0};
    ta_status ms = ta_blob_migrate(&migration, &migrated);
    if (ms == TA_ERROR_NO_MEMORY &&
        !buffer_at(host, &host->blob2, &host->blob2_capacity, migrated.length)) {
        refused = 1;
    } else if (ms == TA_ERROR_NO_MEMORY) {
        migrated = (ta_buffer){host->blob2, host->blob2_capacity, 0};
        if (ta_blob_migrate(&migration, &migrated) == TA_OK) {
            ta_bytes blob = {host->blob2, migrated.length};
            uint32_t na;
            ta_status grown = TA_OK;
            while ((status = ta_system_adopt(to->system, blob, &na)) == TA_ERROR_NO_MEMORY &&
                   (grown = segment_grow(host, to)) == TA_OK) {
            }
            moved = status == TA_OK;
            refused = !moved && grown == TA_ERROR_NO_MEMORY;
        }
    }
    if (!moved) { /* back home, unchanged */
        adopt_home(host, from, host->blob, length);
        if (refused) return MIGRATE_NO_MEMORY;
        /* running invocations (a session tree): migrated once they finished */
        return ms == TA_ERROR_UNSUPPORTED ? MIGRATE_LATER : MIGRATE_FAILED;
    }
    moved_put(host, session, (uint32_t)(session >> SERIALS_SHIFT) == tseg ? TA_NIL : tseg);
    to->busy = 1;
    host->migrated++;
    ta_host_effect *effect = host_effect_new(host, TA_HOST_EFFECT_MIGRATED, tseg);
    uint32_t na = 0;
    host_locate(host, session, NULL, &na);
    effect->actor = na;
    effect->session = effect->root = session;
    effect->job = job->id;
    effect->chart = chart;
    host_effect_strings_set(host, (ta_string_view){0, 0}, key, (ta_string_view){0, 0},
                            (ta_string_view){0, 0});
    return 1;
}

/* Needs job_reserve(h, job, 1) first. */
static uint32_t retry_push(migration_job *job, uint64_t session) {
    for (uint32_t i = 0; i < job->retry_count; i++)
        if (job->retry[i] == session) return i;
    job->retry[job->retry_count] = session;
    job->woken[job->retry_count] = 0;
    return job->retry_count++;
}

/* End a job: TA_ERROR_NO_MEMORY leaves it active (it ends in a later run). */
static ta_status job_end(ta_host *host, migration_job *job) {
    if (job->walking) { /* timed out mid-walk: the rest of the candidates are left behind too */
        for (uint32_t si = job->walk_segment; si < host->segment_count; si++) {
            host_segment *segment = segment_of(host, si);
            if (!segment) continue;
            uint32_t cur = si == job->walk_segment ? job->cursor : 0, list[256], n;
            while ((n = ta_system_actors(segment->system, &cur, (ta_uint32_array){list, 256})) != 0)
                for (uint32_t k = 0; k < n; k++) {
                    ta_actor_info actor_info;
                    if (!candidate(host, job, si, list[k], &actor_info)) continue;
                    if (!job_reserve(host, job, 1)) return TA_ERROR_NO_MEMORY;
                    retry_push(job, actor_info.serial); /* (again next time: no duplicates) */
                }
        }
    }
    if (!host_effects_reserve(host, 1, 0)) return TA_ERROR_NO_MEMORY;
    job->walking = 0;
    job->active = 0;
    job->left = job->retry;
    job->left_count = job->retry_count;
    job->left_capacity = job->retry_capacity;
    host_free(host, job->woken, job->retry_capacity);
    job->retry = NULL;
    job->woken = NULL;
    job->retry_count = job->retry_capacity = 0;
    ta_host_effect *effect = host_effect_new(host, TA_HOST_EFFECT_JOB_DONE, TA_NIL);
    effect->job = job->id;
    effect->migrated = job->migrated;
    effect->failed = job->failed;
    effect->pending = job->left_count;
    return TA_OK;
}

/* Advance a job by up to `quantum` actors. TA_ERROR_NO_MEMORY: some actors
 * could not move for lack of memory, or the job could not end; they are
 * tried again in the next run. */
static ta_status job_step(ta_host *host, migration_job *job, uint32_t quantum) {
    /* every actor the walk looks at may join the retry list */
    if (job->walking && !job_reserve(host, job, quantum)) return TA_ERROR_NO_MEMORY;
    ta_status status = TA_OK;
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
        uint32_t n = ta_system_actors(segment->system, &job->cursor, (ta_uint32_array){list, want});
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
            if (r == MIGRATE_FAILED) job->failed++;
            if (r == MIGRATE_NO_MEMORY) status = TA_ERROR_NO_MEMORY;
            if (r == MIGRATE_LATER || r == MIGRATE_NO_MEMORY) {
                uint32_t at = retry_push(job, actor_info.serial);
                if (woken) job->woken[at] = 1;
            }
        }
    }
    uint32_t keep = 0;
    for (uint32_t i = 0; i < job->retry_count; i++) {
        int r = MIGRATE_LATER;
        if (looked < quantum) {
            r = migrate_one(host, job, job->retry[i], &job->woken[i]);
            looked++;
        }
        if (r > 0) job->migrated++;
        if (r == MIGRATE_FAILED) job->failed++;
        if (r == MIGRATE_NO_MEMORY) status = TA_ERROR_NO_MEMORY;
        if (r == MIGRATE_LATER || r == MIGRATE_NO_MEMORY) {
            job->retry[keep] = job->retry[i];
            job->woken[keep] = job->woken[i];
            keep++;
        }
    }
    job->retry_count = keep;
    if ((!job->walking && !job->retry_count) || (job->deadline && host->now >= job->deadline)) {
        ta_status ended = job_end(host, job);
        if (ended != TA_OK) status = ended;
    }
    return status;
}

/* ---- running ------------------------------------------------------------------------- */

static void watches_expire(ta_host *host);

void ta_host_advance_time(ta_host *host, ta_host_time now) {
    if (host->running) return; /* not during a run (host.h) */
    host->now = now.monotonic;
    host->encoder->mark(host, now.wall_ns);
    for (uint32_t i = 0; i < host->segment_count; i++)
        if (host->segments[i].live) ta_system_advance_time(host->segments[i].system, now.monotonic);
    ta_router_time(host); /* envelopes queued for absent peers past their TTL */
    if (host->watch_count) watches_expire(host);
}

/* Free segments that are no longer current and have no actors. 0 if one
 * could not be reported (no memory): it is retired in a later run. */
/* Every segment's fuel clock to the host's: what is written between runs, or
 * by a segment that did not run, is stamped on the one clock. */
static void clocks_sync(ta_host *host) {
    for (uint32_t si = 0; si < host->segment_count; si++) {
        host_segment *segment = segment_of(host, si);
        if (segment) ta_system_trace_clock(segment->system, host->fuel_clock);
    }
}

static uint64_t segment_clock(const host_segment *segment) {
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(segment->system, &stats);
    return stats.fuel_clock;
}

/* ---- per-chart stats (tinyactors/stats.h) ---------------------------------------------- */

static void chart_stats_add(ta_chart_stats *sum, const ta_chart_stats *part) {
    sum->actors += part->actors;
    sum->hibernated += part->hibernated;
    sum->spawned += part->spawned;
    sum->ended += part->ended;
    sum->turns += part->turns;
    sum->macrosteps += part->macrosteps;
    sum->fuel += part->fuel;
    sum->errors_execution += part->errors_execution;
    sum->errors_communication += part->errors_communication;
    sum->errors_platform += part->errors_platform;
    sum->faults += part->faults;
}

static retired_chart *retired_find(const ta_host *host, ta_string_view name) {
    for (uint32_t i = 0; i < host->retired_chart_count; i++)
        if (name_equal(name, host->retired_charts[i].name, host->retired_charts[i].name_length))
            return &host->retired_charts[i];
    return NULL;
}

/* An entry for `name` (its totals zero when new); NULL if the allocator refused. */
static retired_chart *retired_entry(ta_host *host, ta_string_view name) {
    retired_chart *entry = retired_find(host, name);
    if (entry) return entry;
    if (!host_grow(host, (void **)&host->retired_charts, &host->retired_chart_capacity,
                   host->retired_chart_count + 1, sizeof(retired_chart)))
        return NULL;
    char *copy = (char *)host_alloc(host, name.length ? name.length : 1);
    if (!copy) return NULL;
    if (name.length) ta_memcpy(copy, name.pointer, name.length);
    entry = &host->retired_charts[host->retired_chart_count++];
    ta_memset(entry, 0, sizeof *entry);
    entry->name = copy;
    entry->name_length = name.length;
    return entry;
}

/* Keep a segment's per-chart counts before it is freed; 0 if the allocator
 * refused (nothing is added then: it is retired in a later run). */
static int retired_fold(ta_host *host, const host_segment *segment) {
    uint32_t charts = image_charts(segment->image);
    for (uint32_t c = 0; c < charts; c++) /* every entry first: adding cannot fail */
        if (!retired_entry(host, chart_key(segment->image, c))) return 0;
    for (uint32_t c = 0; c < charts; c++) {
        ta_chart_stats part;
        part.size = sizeof part;
        ta_system_chart_stats_get(segment->system, c, &part);
        chart_stats_add(&retired_find(host, chart_key(segment->image, c))->totals, &part);
    }
    return 1;
}

uint32_t host_chart_stats(const ta_host *host, ta_chart_stats *out, uint32_t capacity) {
    for (uint32_t v = 0; v < host->version_count && v < capacity; v++) {
        ta_string_view name = host->versions[v].name;
        ta_chart_stats z;
        ta_memset(&z, 0, sizeof z);
        for (uint32_t si = 0; si < host->segment_count; si++) {
            const host_segment *segment = segment_of(host, si);
            if (!segment) continue;
            uint32_t charts = image_charts(segment->image);
            for (uint32_t c = 0; c < charts; c++) {
                ta_string_view key = chart_key(segment->image, c);
                if (!name_equal(name, key.pointer, key.length)) continue;
                ta_chart_stats part;
                part.size = sizeof part;
                ta_system_chart_stats_get(segment->system, c, &part);
                chart_stats_add(&z, &part);
            }
        }
        const retired_chart *retired = retired_find(host, name);
        if (retired) chart_stats_add(&z, &retired->totals);
        z.size = sizeof z;
        z.chart = host->versions[v].chart;
        z.name = name;
        uint32_t n = out[v].size && out[v].size < sizeof z ? out[v].size : (uint32_t)sizeof z;
        ta_memcpy(&out[v], &z, n);
        out[v].size = n;
    }
    return host->version_count;
}

static int retire(ta_host *host) {
    int all = 1;
    for (uint32_t si = 0; si < host->segment_count; si++) {
        host_segment *segment = segment_of(host, si);
        if (!segment || segment->busy) continue;
        int current = 0;
        for (uint32_t v = 0; v < host->version_count; v++)
            current |= host->versions[v].segment == si;
        if (current || segment_actors(segment)) {
            segment->retiring = 0;
            continue;
        }
        /* its last records (its last actors' ends) are read after this run:
         * it goes once they were, or a run later if nobody reads them */
        ta_trace_view view;
        if (ta_system_trace(segment->system, &view) && !segment->retiring) {
            segment->retiring = 1;
            continue;
        }
        if (!host_effects_reserve(host, 1, 0) || !retired_fold(host, segment)) {
            all = 0;
            continue;
        }
        host->encoder->definitions(host, HOST_DEFINITION_FREED, si);
        ta_system_deinit(segment->system);
        host_free(host, segment->memory, segment->memory_size);
        segment->memory = NULL;
        segment->memory_size = 0;
        segment->system = NULL;
        segment->live = 0;
        host->retired++;
        for (uint32_t i = 0; i < host->moved_capacity; i++)
            if (host->moved_keys[i] && host->moved_values[i] == si)
                moved_put(host, host->moved_keys[i], TA_NIL);
        host_effect_new(host, TA_HOST_EFFECT_SEGMENT_FREED, si);
    }
    return all;
}

ta_run ta_host_run(ta_host *host, uint32_t budget) {
    ta_run out;
    ta_memset(&out, 0, sizeof out);
    if (host->running) { /* called from a datamodel during a run */
        out.status = TA_RUN_FAULT;
        out.detail = (uint32_t)TA_ERROR_STATE;
        out.next_deadline = UINT64_MAX;
        return out;
    }
    effects_release(host); /* the previous batch, if not acknowledged */
    host->encoder->drain(host); /* records written up to the last ta_host_advance_time */
    host->encoder->effects(host);
    subscriptions_arm(host);
    uint32_t left = budget;
    int full = 0, fault = 0, exhausted = 0, sync = 0;
    uint32_t detail = TA_POOL_NONE;
    /* what an earlier run or call could not finish for lack of memory */
    if (host->error_count && errors_deliver(host) != TA_OK) exhausted = 1;
    if (host->expiry_behind) {
        ta_router_time(host);
        if (host->expiry_behind) exhausted = 1;
    }
    for (int round = 0; round < 64 && !full && !exhausted && !sync; round++) {
        int ran = 0;
        /* fair shares: a runaway segment must not starve the others */
        uint32_t runnable = 0;
        for (uint32_t k = 0; k < host->segment_count; k++) {
            const host_segment *segment = segment_of(host, k);
            runnable += segment && (segment->busy || segment->deadline <= host->now);
        }
        for (uint32_t k = 0; k < host->segment_count && !full && !exhausted && !sync; k++) {
            uint32_t si = (host->round_robin + k) % host->segment_count;
            host_segment *segment = segment_of(host, si);
            if (!segment || (!segment->busy && segment->deadline > host->now)) continue;
            if (!left) break;
            uint32_t share = runnable > 1 ? left / runnable : left;
            if (runnable > 1) runnable--;
            ta_system_trace_clock(segment->system, host->fuel_clock);
            host->running = 1;
            ta_run result = ta_system_run(segment->system, share ? share : 1);
            host->running = 0;
            host->fuel_clock = segment_clock(segment);
            left = result.used >= left ? 0 : left - result.used;
            ran = 1;
            segment->deadline = result.next_deadline;
            segment->busy =
                result.status != TA_RUN_IDLE; /* draining may add work (errors, deliveries) */
            if (drain(host, si) != TA_OK) exhausted = 1;
            segment = &host->segments[si];
            if (result.status == TA_RUN_EXHAUSTED) {
                ta_status grown = segment_grow(host, segment);
                if (grown == TA_ERROR_NO_MEMORY) { /* refused: the same run again later */
                    exhausted = 1;
                    detail = result.detail;
                    segment->busy = 1;
                } else if (grown != TA_OK) {
                    fault = 1;
                    segment->busy = 0;
                }
            }
            if (result.status == TA_RUN_FAULT) {
                fault = 1;
                segment->busy = 0;
            }
            /* a TA_IO_PROCESSOR_SYNC send: the caller answers it before the sender goes on */
            if (result.status == TA_RUN_EFFECTS) sync = 1;
            if (host->effect_count >= EFFECTS_PER_RUN) full = 1;
        }
        for (uint32_t i = 0; i < host->job_count && round == 0; i++) /* one quantum per run */
            if (host->jobs[i].active) {
                if (job_step(host, &host->jobs[i], host->config.migrate_quantum) != TA_OK)
                    exhausted = 1;
                ran = 1;
            }
        if (!ran || !left || sync) break;
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
        if (job->active || ended_now)
            host->jobs[kj++] = *job;
        else
            job_free(host, job);
    }
    host->job_count = kj;
    if (!retire(host)) exhausted = 1;
    clocks_sync(host);
    /* yielded: segments have work, or a job has actors left to look at. A job
     * that only waits (busy or sleeping actors) moves on when they change —
     * the next run after an event — or at its timeout (next_deadline). */
    int busy = 0;
    out.next_deadline = UINT64_MAX;
    for (uint32_t i = 0; i < host->segment_count; i++) {
        host_segment *segment = segment_of(host, i);
        if (!segment) continue;
        busy |= segment->busy | segment->retiring; /* retired by the next run */
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
    for (uint32_t i = 0; i < host->watch_count; i++) /* watches end then */
        if (host->watches[i].until < out.next_deadline) out.next_deadline = host->watches[i].until;
    host->encoder->note(host);
    ta_time flush = host->encoder->deadline(host); /* a request is ready then */
    if (flush < out.next_deadline) out.next_deadline = flush;
    out.used = budget - left;
    out.effect_count = host->effect_count;
    out.detail = exhausted ? detail : TA_POOL_NONE;
    out.status = full        ? TA_RUN_EFFECTS_FULL
                 : sync      ? TA_RUN_EFFECTS
                 : fault     ? TA_RUN_FAULT
                 : exhausted ? TA_RUN_EXHAUSTED
                 : busy      ? TA_RUN_YIELDED
                             : TA_RUN_IDLE;
    return out;
}

/* ---- hibernation ----------------------------------------------------------------------- */

uint32_t ta_host_sweep(ta_host *host, uint32_t max_scan, ta_uint64_array sessions) {
    if (host->running) return 0; /* not during a run (host.h) */
    uint64_t *out = sessions.pointer;
    uint32_t capacity = sessions.capacity, n = 0, scanned = 0;
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
            /* The handles go into the free part of `out`, read as uint32_t:
             * handle i sits in the first half of the eight bytes session i
             * takes, so converting from the last to the first overwrites
             * only handles already converted. */
            uint32_t *handles = (uint32_t *)(void *)(out + n);
            ta_sweep sweep = {host->sweep_cursor, budget};
            uint32_t got = ta_system_cold_actors(segment->system, &sweep,
                                                 (ta_uint32_array){handles, capacity - n});
            host->sweep_cursor = sweep.cursor;
            uint32_t kept = got;
            for (uint32_t i = got; i-- > 0;) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                uint32_t handle = handles[i];
                out[n + i] = ta_actor_info_get(segment->system, handle, &actor_info) == TA_OK
                                 ? actor_info.serial
                                 : 0;
            }
            for (uint32_t i = 0, w = 0; i < got; i++) /* (a session id is never 0) */
                if (out[n + i])
                    out[n + w++] = out[n + i];
                else
                    kept--;
            n += kept;
            scanned += budget < stats.actors_capacity ? budget : stats.actors_capacity;
            if (n == capacity) break;              /* the output is full: continue here next time */
            if (host->sweep_cursor > start) break; /* out of budget mid-revolution: continue here */
        }
        host->sweep_segment++;
        host->sweep_cursor = 0;
    }
    return n;
}

ta_status ta_host_hibernate(ta_host *host, uint64_t session, ta_buffer *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t si, actor;
    ta_status status = host_locate(host, session, &si, &actor);
    return status == TA_OK ? ta_actor_hibernate(host->segments[si].system, actor, out) : status;
}

ta_status ta_host_wake(ta_host *host, uint64_t session, ta_bytes blob) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t si, actor;
    ta_status status = host_locate(host, session, &si, &actor);
    if (status != TA_OK) return status;
    host_segment *segment = &host->segments[si];
    while ((status = ta_actor_wake(segment->system, actor, blob)) == TA_ERROR_NO_MEMORY &&
           segment_grow(host, segment) == TA_OK) {
    }
    if (status == TA_OK) segment->busy = 1;
    return status;
}

/* ---- moving actors between hosts ------------------------------------------------------- */

ta_status ta_host_export_actor(ta_host *host, uint64_t session, ta_buffer *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t si, actor;
    ta_status status = host_locate(host, session, &si, &actor);
    if (status != TA_OK) return status;
    status = ta_actor_export(host->segments[si].system, actor, out);
    if (status == TA_OK) {
        if (host_moved_get(host, session) != TA_NIL) moved_put(host, session, TA_NIL);
        host->segments[si].busy = 1;
    }
    return status;
}

/* An adopted session lives in segment si: routed there, and its id never handed
 * out by its home segment (one added later is set up past it, reserve_serials).
 * Needs moved_reserve first. */
static void place_session(ta_host *host, uint32_t si, uint64_t serial) {
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
    uint32_t m = host_moved_get(host, serial);
    host_segment *o = segment_of(host, m != TA_NIL ? m : (uint32_t)(serial >> SERIALS_SHIFT));
    return o && o != segment && ta_system_actor_by_session(o->system, serial);
}

/* The sessions of an adopted tree in segment si (root and the sessions it
 * invoked): check (1 if an id lives elsewhere; *count: how many there are)
 * or place them. */
static int tree_pass(ta_host *host, uint32_t si, uint32_t root, uint64_t root_serial, int check,
                     uint32_t *count) {
    host_segment *segment = &host->segments[si];
    if (check ? lives_elsewhere(host, segment, root_serial)
              : (place_session(host, si, root_serial), 0))
        return 1;
    if (count) *count = 1;
    uint32_t cursor = 0, list[64], n;
    while ((n = ta_system_actors(segment->system, &cursor, (ta_uint32_array){list, 64})) != 0)
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
            if (check ? lives_elsewhere(host, segment, serial)
                      : (place_session(host, si, serial), 0))
                return 1;
            if (count) ++*count;
        }
    return 0;
}

ta_status ta_host_adopt(ta_host *host, const ta_host_adoption *adoption, uint64_t *session) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    uint32_t segment = adoption->segment;
    size_t length = adoption->blob.length;
    host_segment *segment_record = segment_of(host, segment);
    if (session) *session = 0;
    if (!segment_record) return TA_ERROR_INVALID;
    /* room to export it again if it cannot stay: a tree exports to the blob it came from */
    if (!buffer_at(host, &host->blob, &host->blob_capacity, length)) return TA_ERROR_NO_MEMORY;
    uint32_t root = 0;
    ta_status status;
    while ((status = ta_system_adopt(segment_record->system, adoption->blob, &root)) ==
               TA_ERROR_NO_MEMORY &&
           segment_grow(host, segment_record) == TA_OK) {
    }
    if (status != TA_OK) return status;
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    ta_actor_info_get(segment_record->system, root, &actor_info);
    uint32_t count = 0;
    int elsewhere = tree_pass(host, segment, root, actor_info.serial, 1, &count);
    if (elsewhere || !moved_reserve(host, count)) {
        /* one of its sessions lives in another segment, or no room to route
         * to them: out again, nothing changed */
        ta_buffer out = {host->blob, host->blob_capacity, 0};
        status = ta_actor_export(segment_record->system, root, &out);
        if (status == TA_ERROR_NO_MEMORY &&
            buffer_at(host, &host->blob, &host->blob_capacity, out.length)) {
            out = (ta_buffer){host->blob, host->blob_capacity, 0};
            ta_actor_export(segment_record->system, root, &out);
        }
        return elsewhere ? TA_ERROR_STATE : TA_ERROR_NO_MEMORY;
    }
    segment_record->busy = 1;
    tree_pass(host, segment, root, actor_info.serial, 0, NULL);
    if (session) *session = actor_info.serial;
    return TA_OK;
}

/* ---- inspection ------------------------------------------------------------------------ */

/* ---- tracing: options and watches make the policy ------------------------------------- */

static void rules_free(ta_host *host, host_rules *r) {
    host_free(host, r->rules, r->bytes);
    r->rules = NULL;
    r->count = 0;
    r->bytes = 0;
}

/* The host's copy of `count` rules and their strings. */
static int rules_copy(ta_host *host, const ta_trace_rule *rules, uint32_t count, host_rules *out) {
    out->rules = NULL;
    out->count = 0;
    out->bytes = 0;
    if (!count || !rules) return 1;
    size_t bytes = sizeof(ta_trace_rule) * count;
    for (uint32_t i = 0; i < count; i++)
        bytes += (size_t)rules[i].chart.length + rules[i].actor.length + rules[i].event.length;
    ta_trace_rule *copy = (ta_trace_rule *)host_alloc(host, bytes);
    if (!copy) return 0;
    char *text = (char *)(copy + count);
    for (uint32_t i = 0; i < count; i++) {
        copy[i] = rules[i];
        copy[i].size = sizeof copy[i];
        ta_string_view *views[3] = {&copy[i].chart, &copy[i].actor, &copy[i].event};
        for (int k = 0; k < 3; k++) {
            if (views[k]->length) ta_memcpy(text, views[k]->pointer, views[k]->length);
            views[k]->pointer = text;
            text += views[k]->length;
        }
    }
    out->rules = copy;
    out->count = count;
    out->bytes = bytes;
    return 1;
}

static ta_status policy_apply(ta_host *host, const ta_trace_policy *policy) {
    if (!host_policy_copy(host, policy)) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < host->segment_count; i++) {
        host_segment *segment = segment_of(host, i);
        if (!segment) continue;
        ta_status status = ta_system_trace_set(segment->system, &host->trace_policy);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* The policy of the options and the watches (newest first, then the options'
 * rules), each different detail a class of its own; set in every segment. */
static ta_status trace_compose(ta_host *host) {
    uint32_t total = host->trace_options.count;
    for (uint32_t w = 0; w < host->watch_count; w++)
        total += host->watches[w].rules.count;
    ta_trace_rule *rules = NULL;
    if (total && !(rules = (ta_trace_rule *)host_alloc(host, sizeof(ta_trace_rule) * total)))
        return TA_ERROR_NO_MEMORY;
    uint32_t classes[TA_TRACE_DETAILS] = {0}, count = 2, k = 0;
    classes[TA_TRACE_DETAIL_UNSAMPLED] = host->trace_kinds & TA_TRACE_LIFECYCLE;
    classes[TA_TRACE_DETAIL_SAMPLED] = host->trace_kinds;
    for (uint32_t w = host->watch_count; w-- > 0;)
        for (uint32_t i = 0; i < host->watches[w].rules.count; i++)
            rules[k++] = host->watches[w].rules.rules[i];
    for (uint32_t i = 0; i < host->trace_options.count; i++)
        rules[k++] = host->trace_options.rules[i];
    ta_status status = TA_OK;
    for (uint32_t i = 0; i < total && status == TA_OK; i++) {
        if (rules[i].detail == TA_NIL) continue;
        uint32_t c = 2;
        while (c < count && classes[c] != rules[i].detail)
            c++;
        if (c == count) {
            if (count == TA_TRACE_DETAILS) status = TA_ERROR_NO_MEMORY;
            else classes[count++] = rules[i].detail;
        }
        rules[i].detail = c;
    }
    if (status == TA_OK) {
        ta_trace_policy policy;
        ta_trace_policy_default(&policy);
        policy.sample_parts = host->trace_parts;
        policy.sample_whole = host->trace_whole;
        /* with nowhere for records to go the systems write none (dropped alone) */
        for (uint32_t i = 0; i < TA_TRACE_DETAILS; i++)
            policy.kinds[i] = !(host->trace_flags & HOST_TRACE_NO_RECORDS) && classes[i]
                                  ? classes[i]
                                  : TA_TRACE_BIT(TA_TRACE_DROPPED);
        policy.rules = rules;
        policy.rule_count = total;
        status = policy_apply(host, &policy);
    }
    host_free(host, rules, sizeof(ta_trace_rule) * total);
    return status;
}

ta_status host_trace_set(ta_host *host, const host_trace_options *options) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    if (host->config.system.tracing == TA_TRACING_DISABLED) return TA_ERROR_STATE;
    if (!options || options->size < sizeof *options) return TA_ERROR_INVALID;
    host_rules rules, before = host->trace_options;
    if (!rules_copy(host, options->rules, options->rule_count, &rules)) return TA_ERROR_NO_MEMORY;
    uint32_t was[4] = {host->trace_flags, host->trace_kinds, host->trace_parts, host->trace_whole};
    host->trace_options = rules;
    host->trace_flags = options->flags;
    host->trace_kinds = options->kinds;
    host->trace_parts = options->sample_parts;
    host->trace_whole = options->sample_whole;
    ta_status status = trace_compose(host);
    if (status != TA_OK) { /* as it was */
        rules_free(host, &host->trace_options);
        host->trace_options = before;
        host->trace_flags = was[0];
        host->trace_kinds = was[1];
        host->trace_parts = was[2];
        host->trace_whole = was[3];
        trace_compose(host);
        return status;
    }
    rules_free(host, &before);
    return TA_OK;
}

ta_status host_trace_watch(ta_host *host, const host_watch_rules *request, uint32_t *id) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    const ta_trace_rule *rules = request->rules;
    uint32_t count = request->rule_count;
    ta_time until = request->until;
    if (host->config.system.tracing == TA_TRACING_DISABLED) return TA_ERROR_STATE;
    if (!host_grow(host, (void **)&host->watches, &host->watch_capacity, host->watch_count + 1,
                   sizeof(host_watch)))
        return TA_ERROR_NO_MEMORY;
    host_watch *watch = &host->watches[host->watch_count];
    if (!rules_copy(host, rules, count, &watch->rules)) return TA_ERROR_NO_MEMORY;
    watch->id = ++host->watch_sequence;
    watch->until = until;
    host->watch_count++;
    ta_status status = trace_compose(host);
    if (status != TA_OK) {
        rules_free(host, &host->watches[--host->watch_count].rules);
        trace_compose(host);
        return status;
    }
    if (id) *id = watch->id;
    return TA_OK;
}

static void watch_remove(ta_host *host, uint32_t at) {
    rules_free(host, &host->watches[at].rules);
    for (uint32_t i = at + 1; i < host->watch_count; i++)
        host->watches[i - 1] = host->watches[i];
    host->watch_count--;
}

ta_status host_trace_unwatch(ta_host *host, uint32_t id) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    for (uint32_t i = 0; i < host->watch_count; i++)
        if (host->watches[i].id == id) {
            watch_remove(host, i);
            return trace_compose(host);
        }
    return TA_ERROR_STALE;
}

/* Watches whose time is up end (ta_host_advance_time). */
static void watches_expire(ta_host *host) {
    int ended = 0;
    for (uint32_t i = host->watch_count; i-- > 0;)
        if (host->watches[i].until <= host->now) {
            watch_remove(host, i);
            ended = 1;
        }
    if (ended) trace_compose(host);
}

void host_stats_get(const ta_host *host, ta_host_stats *o) {
    ta_host_stats z;
    ta_memset(&z, 0, sizeof z);
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
    z.fuel_clock = host->fuel_clock;
    for (uint32_t i = 0; i < host->job_count; i++)
        z.jobs += (uint32_t)host->jobs[i].active;
    z.routed = host->routed;
    z.migrated = host->migrated;
    z.retired = host->retired;
    uint32_t n = o->size && o->size < sizeof z ? o->size : (uint32_t)sizeof z;
    ta_memcpy(o, &z, n);
    o->size = n;
}

uint32_t host_actors(const ta_host *host, uint64_t *cursor, ta_uint64_array sessions) {
    uint64_t *out = sessions.pointer;
    uint32_t capacity = sessions.capacity;
    uint32_t si = (uint32_t)(*cursor >> 32), c = (uint32_t)*cursor, n = 0;
    while (si < host->segment_count && n < capacity) {
        const host_segment *segment = segment_of(host, si);
        uint32_t list[64], got = 0;
        if (segment) {
            uint32_t want = capacity - n < 64 ? capacity - n : 64;
            got = ta_system_actors(segment->system, &c, (ta_uint32_array){list, want});
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
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
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
        if (segment_grow(host, segment_record) != TA_OK) return TA_ERROR_NO_MEMORY;
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
        ta_memset(&actor_info, 0, sizeof actor_info);
        actor_info.size = sizeof actor_info;
        if (a && ta_actor_info_get(segment->system, a, &actor_info) == TA_OK)
            return actor_info.serial;
    }
    return 0;
}

/* ---- snapshot / restore ------------------------------------------------------------- */

#define SNAPSHOT_MAGIC   0x53484154u /* "TAHS" */
#define SNAPSHOT_VERSION 2u

typedef struct snapshot_header {
    uint32_t magic, version, segment_count, moved_count;
    uint32_t round_robin, job_sequence, config_size, retired_charts;
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

/* after the router's bytes: each retired chart's totals, then its name (8-aligned) */
typedef struct snapshot_retired {
    uint32_t name_length, pad;
    ta_chart_stats totals; /* name: none */
} snapshot_retired;

static size_t pad8(size_t n) {
    return (n + 7) & ~(size_t)7;
}

/* The length of a segment's region in a snapshot: ta_system_snapshot with no
 * room reports it. */
static ta_status region_length(const ta_system *system, size_t *length) {
    ta_buffer probe = {(void *)0, 0, 0};
    ta_status status = ta_system_snapshot(system, &probe);
    *length = probe.length;
    return status == TA_ERROR_NO_MEMORY ? TA_OK : status;
}

ta_status ta_host_snapshot(ta_host *host, ta_buffer *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    if (host->effect_count) return TA_ERROR_STATE; /* acknowledge the effects first */
    for (uint32_t i = 0; i < host->job_count; i++)
        if (host->jobs[i].active) return TA_ERROR_STATE;
    size_t need = sizeof(snapshot_header);
    for (uint32_t i = 0; i < host->segment_count; i++) {
        need += sizeof(snapshot_segment);
        if (!host->segments[i].live) continue;
        size_t rl = 0;
        ta_status status = region_length(host->segments[i].system, &rl);
        if (status != TA_OK) return status;
        need += pad8(rl);
    }
    uint32_t moved_count = 0;
    for (uint32_t i = 0; i < host->moved_capacity; i++)
        moved_count += host->moved_keys[i] && host->moved_values[i] != TA_NIL;
    need += (size_t)moved_count * sizeof(snapshot_moved);
    size_t rlen = ta_router_snapshot(host, NULL, 0);
    need += rlen;
    size_t retired_offset = need;
    for (uint32_t i = 0; i < host->retired_chart_count; i++)
        need += sizeof(snapshot_retired) + pad8(host->retired_charts[i].name_length);
    out->length = need;
    if (out->capacity < need) return TA_ERROR_NO_MEMORY;
    void *buffer = out->pointer;
    unsigned char *p = (unsigned char *)buffer;
    ta_memset(p, 0, need);
    snapshot_header hd;
    ta_memset(&hd, 0, sizeof hd);
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
    hd.retired_charts = host->retired_chart_count;
    ta_memcpy(p, &hd, sizeof hd);
    p += sizeof hd;
    for (uint32_t i = 0; i < host->segment_count; i++) {
        const host_segment *segment = &host->segments[i];
        snapshot_segment ss;
        ta_memset(&ss, 0, sizeof ss);
        ss.live = (uint32_t)segment->live;
        if (segment->live) {
            ta_image_info image_info;
            image_info.size = sizeof image_info;
            ta_image_get_info(segment->image, &image_info);
            ss.hash = image_info.content_hash;
            size_t rl = 0;
            region_length(segment->system, &rl);
            ss.region_length = rl;
            ss.config = segment->config;
            ss.config.io_processors = NULL;
            ss.config.trace_policy = NULL; /* the region keeps the segment's policy */
        }
        ta_memcpy(p, &ss, sizeof ss);
        p += sizeof ss;
        if (segment->live) {
            ta_buffer region = {p, (size_t)ss.region_length, 0};
            ta_status status = ta_system_snapshot(segment->system, &region);
            if (status != TA_OK) return status;
            p += pad8(region.length);
        }
    }
    for (uint32_t i = 0; i < host->moved_capacity; i++)
        if (host->moved_keys[i] && host->moved_values[i] != TA_NIL) {
            snapshot_moved m = {host->moved_keys[i], host->moved_values[i], 0};
            ta_memcpy(p, &m, sizeof m);
            p += sizeof m;
        }
    ta_router_snapshot(host, p, rlen);
    p = (unsigned char *)buffer + retired_offset;
    for (uint32_t i = 0; i < host->retired_chart_count; i++) {
        snapshot_retired r;
        ta_memset(&r, 0, sizeof r);
        r.name_length = host->retired_charts[i].name_length;
        r.totals = host->retired_charts[i].totals;
        r.totals.name.pointer = NULL;
        r.totals.name.length = 0;
        ta_memcpy(p, &r, sizeof r);
        p += sizeof r;
        if (r.name_length) ta_memcpy(p, host->retired_charts[i].name, r.name_length);
        p += pad8(r.name_length);
    }
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
        ta_memcpy(&ss, (const unsigned char *)snap + offset, sizeof ss);
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
    offset += (size_t)hd->router_length;
    for (uint32_t i = 0; i < hd->retired_charts; i++) {
        if (length - offset < sizeof(snapshot_retired)) return NULL;
        snapshot_retired r;
        ta_memcpy(&r, (const unsigned char *)snap + offset, sizeof r);
        offset += sizeof r;
        if (pad8(r.name_length) > length - offset) return NULL;
        offset += pad8(r.name_length);
    }
    return hd;
}

uint32_t ta_host_snapshot_segments(ta_bytes snapshot, ta_uint64_array hashes) {
    const void *snap = snapshot.pointer;
    const snapshot_header *hd = snap_check(snap, snapshot.length);
    if (!hd) return 0;
    size_t offset = sizeof *hd;
    for (uint32_t i = 0; i < hd->segment_count; i++) {
        snapshot_segment ss;
        ta_memcpy(&ss, (const unsigned char *)snap + offset, sizeof ss);
        if (i < hashes.capacity) hashes.pointer[i] = ss.live ? ss.hash : 0;
        offset += sizeof ss + (ss.live ? pad8((size_t)ss.region_length) : 0);
    }
    return hd->segment_count;
}

static ta_status restore_failed(ta_host *host, ta_status status) {
    ta_host_free(host);
    return status;
}

ta_status ta_host_restore(const ta_host_environment *environment,
                          const ta_host_snapshot_input *input, ta_host **out) {
    const ta_image *const *images = input->images;
    uint32_t image_count = input->image_count;
    const void *snap = input->snapshot.pointer;
    size_t length = input->snapshot.length;
    *out = NULL;
    const snapshot_header *hd = snap_check(snap, length);
    if (!hd || image_count < hd->segment_count) return TA_ERROR_INVALID;
    ta_otlp_attribute resource[HOST_CONFIG_RESOURCE];
    ta_io_processor_descriptor processors[HOST_CONFIG_IO_PROCESSORS];
    host_config_input storage = {{0, 0}, (ta_buffer *)0, resource, processors};
    host_config config;
    ta_status status = host_config_of(input->config, environment, &storage, &config);
    if (status != TA_OK) return status;
    ta_host *host;
    status = host_new(&config, &host);
    if (status != TA_OK) return status;
    if (hd->segment_count && !host_grow(host, (void **)&host->segments, &host->segment_capacity,
                                        hd->segment_count, sizeof(host_segment)))
        return restore_failed(host, TA_ERROR_NO_MEMORY);
    host->now = hd->now;
    const unsigned char *p = (const unsigned char *)snap + sizeof *hd;
    for (uint32_t i = 0; i < hd->segment_count; i++) {
        snapshot_segment sv;
        ta_memcpy(&sv, p, sizeof sv);
        const snapshot_segment *ss = &sv;
        p += sizeof sv;
        host_segment *segment = &host->segments[i];
        ta_memset(segment, 0, sizeof *segment);
        host->segment_count = i + 1;
        if (!ss->live) continue;
        ta_image_info image_info;
        image_info.size = sizeof image_info;
        if (images[i]) ta_image_get_info(images[i], &image_info);
        if (!images[i] || image_info.content_hash != ss->hash)
            return restore_failed(host, TA_ERROR_INVALID); /* not the image this segment ran */
        segment->image = images[i];
        segment->config = ss->config;
        segment->config.io_processors = host->io_processors;
        segment->config.io_processor_count = host->io_processor_count;
        segment->config.trace_policy = &host->trace_policy;
        segment->memory = host_alloc(host, (size_t)ss->region_length);
        if (!segment->memory) return restore_failed(host, TA_ERROR_NO_MEMORY);
        segment->memory_size = (size_t)ss->region_length;
        ta_system_setup setup = {{segment->memory, (size_t)ss->region_length},
                                 segment->image,
                                 &segment->config,
                                 host->datamodels,
                                 host->config.datamodel_count,
                                 0};
        status =
            ta_system_restore(&setup, (ta_bytes){p, (size_t)ss->region_length}, &segment->system);
        if (status != TA_OK) {
            host_free(host, segment->memory, segment->memory_size);
            segment->memory = NULL;
            return restore_failed(host, status);
        }
        segment->live = 1;
        segment->busy = 1; /* look at it in the first run */
        segment->deadline = UINT64_MAX;
        uint64_t clock = segment_clock(segment); /* the host's clock: the latest */
        if (clock > host->fuel_clock) host->fuel_clock = clock;
        p += pad8((size_t)ss->region_length);
    }
    if (!moved_reserve(host, hd->moved_count) || !versions_reserve(host, 0))
        return restore_failed(host, TA_ERROR_NO_MEMORY);
    for (uint32_t i = 0; i < hd->moved_count; i++) {
        snapshot_moved m;
        ta_memcpy(&m, p, sizeof m);
        p += sizeof m;
        moved_put(host, m.session, m.segment);
    }
    if (hd->router_length) {
        status = ta_router_restore(host, p, (size_t)hd->router_length);
        if (status != TA_OK) return restore_failed(host, status);
    }
    p += hd->router_length;
    for (uint32_t i = 0; i < hd->retired_charts; i++) {
        snapshot_retired r;
        ta_memcpy(&r, p, sizeof r);
        p += sizeof r;
        ta_string_view name = {(const char *)p, r.name_length};
        retired_chart *entry = retired_entry(host, name);
        if (!entry) return restore_failed(host, TA_ERROR_NO_MEMORY);
        entry->totals = r.totals;
        p += pad8(r.name_length);
    }
    host->message_sequence = hd->message_sequence;
    host->round_robin = hd->segment_count ? hd->round_robin % hd->segment_count : 0;
    host->job_sequence = hd->job_sequence;
    host->routed = hd->routed;
    host->migrated = hd->migrated;
    host->retired = hd->retired;
    versions_recompute(host);
    clocks_sync(host);
    *out = host;
    return TA_OK;
}

/* ---- whole-system snapshots ------------------------------------------------------------- */

/* "tinyactors-snapshot 3\n", 2 zero bytes; u32 system id length, u32 datamodel name
 * length; u64 time; u64 host length, u64 state length; u32 image count, u32 0; u64
 * image lengths; then the system id, the datamodel name, the host snapshot, the
 * state and the images, each from an 8-byte boundary. Little-endian. */
static const char pack_magic[24] = "tinyactors-snapshot 3\n";
#define PACK_FIXED 64u

static size_t pack_align(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

static void pack_u64(unsigned char *out, uint64_t v) {
    for (int i = 0; i < 8; i++)
        out[i] = (unsigned char)(v >> (8 * i));
}

static void pack_u32(unsigned char *out, uint32_t v) {
    for (int i = 0; i < 4; i++)
        out[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t unpack_u64(const unsigned char *in) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | in[i];
    return v;
}

static uint32_t unpack_u32(const unsigned char *in) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = v << 8 | in[i];
    return v;
}

ta_status ta_host_snapshot_pack(ta_host *host, const ta_datamodel_state *contents,
                                ta_buffer *destination) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    ta_string_view datamodel = contents->datamodel;
    const void *state = contents->state.pointer;
    size_t state_length = contents->state.length;
    ta_buffer probe = {(void *)0, 0, 0};
    ta_status status = ta_host_snapshot(host, &probe);
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY) return status;
    size_t host_length = probe.length;
    uint32_t count = host->segment_count;
    ta_string_view id = {host->base + 13, 0}; /* tinyactors://<id>/ */
    while (id.pointer[id.length] && id.pointer[id.length] != '/')
        id.length++;
    size_t total = pack_align(PACK_FIXED + 8u * count) + pack_align(id.length) +
                   pack_align(datamodel.length) + pack_align(host_length) +
                   pack_align(state_length);
    for (uint32_t i = 0; i < count; i++) {
        const host_segment *segment = segment_of(host, i);
        if (segment && segment->live) total += pack_align(ta_image_bytes(segment->image).length);
    }
    destination->length = total;
    if (destination->capacity < total) return TA_ERROR_NO_MEMORY;
    unsigned char *out = (unsigned char *)destination->pointer;
    ta_memset(out, 0, total);
    ta_memcpy(out, pack_magic, sizeof pack_magic);
    pack_u32(out + 24, id.length);
    pack_u32(out + 28, datamodel.length);
    pack_u64(out + 32, host->now);
    pack_u64(out + 40, host_length);
    pack_u64(out + 48, state_length);
    pack_u32(out + 56, count);
    size_t at = pack_align(PACK_FIXED + 8u * count);
    ta_memcpy(out + at, id.pointer, id.length);
    at += pack_align(id.length);
    ta_memcpy(out + at, datamodel.pointer, datamodel.length);
    at += pack_align(datamodel.length);
    ta_buffer snapshot = {out + at, host_length, 0};
    status = ta_host_snapshot(host, &snapshot);
    if (status != TA_OK) return status;
    at += pack_align(host_length);
    if (state_length) ta_memcpy(out + at, state, state_length);
    at += pack_align(state_length);
    for (uint32_t i = 0; i < count; i++) {
        ta_bytes bytes = {0};
        const host_segment *segment = segment_of(host, i);
        if (segment && segment->live) bytes = ta_image_bytes(segment->image);
        pack_u64(out + PACK_FIXED + 8u * i, bytes.length);
        if (bytes.length) ta_memcpy(out + at, bytes.pointer, bytes.length);
        at += pack_align(bytes.length);
    }
    return TA_OK;
}

/* The part from `at` (8-aligned) of `n` bytes, if it lies in the pack. */
static int pack_part(size_t length, size_t *at, uint64_t n, size_t *offset) {
    if (n > length || *at > length - n) return 0;
    *offset = *at;
    *at += pack_align((size_t)n);
    return 1;
}

ta_status ta_host_snapshot_unpack(ta_bytes pack, ta_host_snapshot_contents *out) {
    const unsigned char *in = (const unsigned char *)pack.pointer;
    size_t length = pack.length;
    if (!in || length < PACK_FIXED || ta_memcmp(in, pack_magic, sizeof pack_magic) != 0)
        return TA_ERROR_INVALID;
    uint32_t count = unpack_u32(in + 56);
    if (count > (length - PACK_FIXED) / 8u) return TA_ERROR_INVALID;
    size_t at = pack_align(PACK_FIXED + 8u * count), id = 0, name = 0, host = 0, state = 0;
    uint64_t host_length = unpack_u64(in + 40), state_length = unpack_u64(in + 48);
    if (!pack_part(length, &at, unpack_u32(in + 24), &id) ||
        !pack_part(length, &at, unpack_u32(in + 28), &name) ||
        !pack_part(length, &at, host_length, &host) ||
        !pack_part(length, &at, state_length, &state))
        return TA_ERROR_INVALID;
    for (uint32_t i = 0; i < count; i++) {
        size_t image = 0;
        if (!pack_part(length, &at, unpack_u64(in + PACK_FIXED + 8u * i), &image))
            return TA_ERROR_INVALID;
    }
    uint32_t size = out->size < sizeof *out ? out->size : (uint32_t)sizeof *out;
    ta_host_snapshot_contents contents;
    ta_memset(&contents, 0, sizeof contents);
    contents.size = size;
    contents.image_count = count;
    contents.system_id = (ta_string_view){(const char *)in + id, unpack_u32(in + 24)};
    contents.datamodel = (ta_string_view){(const char *)in + name, unpack_u32(in + 28)};
    contents.time = unpack_u64(in + 32);
    contents.host = in + host;
    contents.host_length = (size_t)host_length;
    contents.state = in + state;
    contents.state_length = (size_t)state_length;
    ta_memcpy(out, &contents, size);
    return TA_OK;
}

ta_bytes ta_host_snapshot_image(ta_bytes pack, uint32_t i) {
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    ta_bytes image = {(const void *)0, 0};
    if (ta_host_snapshot_unpack(pack, &contents) != TA_OK || i >= contents.image_count)
        return image;
    const unsigned char *in = (const unsigned char *)pack.pointer;
    size_t at = (size_t)((const unsigned char *)contents.state - in) +
                pack_align(contents.state_length);
    for (uint32_t k = 0; k < i; k++)
        at += pack_align((size_t)unpack_u64(in + PACK_FIXED + 8u * k));
    image.length = (size_t)unpack_u64(in + PACK_FIXED + 8u * i);
    if (image.length) image.pointer = in + at;
    return image;
}
