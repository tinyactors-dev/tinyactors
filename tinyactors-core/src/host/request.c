/* request.c — the rarely called host API as CBOR requests and replies
 * (ta_host_request, docs/requests.md; docs/adr/0009 in the repository root,
 * decision 2). A request has JSON-RPC's shape without its "jsonrpc" member:
 * {"method", "params", "id"} in, {"id", "result"} or {"id", "error"} out. It
 * is decoded into tokens on the stack, its params are checked here, and the
 * reply is written straight into the caller's buffer.
 * A request that changes the host writes its reply first and applies itself
 * only when the reply fits, so TA_ERROR_NO_MEMORY means nothing changed. */
#include <tinyactors/cbor.h>
#include <tinyactors/image.h>
#include <tinyactors/otlp.h>
#include "base/cbor_output.h"
#include "base/format.h"
#include "base/memory.h"
#include "host/host_internal.h"

#define REQUEST_RULES      32u   /* trace rules a request holds */
#define REQUEST_ACTORS     512u  /* sessions an "actors" reply holds */
#define WATCH_SECONDS      900u  /* a watch's default length: 15 minutes */
#define SAMPLE_WHOLE       1000000u
#define TICKS_PER_SECOND   1000000000ull /* ta_system_config's default */

/* Error codes: JSON-RPC's where they fit, the server range for the host's own. */
enum {
    REQUEST_PARSE = -32700,   /* not one well-formed CBOR item */
    REQUEST_INVALID = -32600, /* not a request: its members are wrong */
    REQUEST_METHOD = -32601,  /* no such method */
    REQUEST_PARAMS = -32602,  /* a param has the wrong type, is missing, unknown, or out of range */
    REQUEST_STALE = -32001,   /* the session or watch does not exist (any more) */
    REQUEST_STATE = -32002,   /* the host cannot do it now */
    REQUEST_LIMIT = -32003,   /* more than the request or the host holds */
    REQUEST_MEMORY = -32004,  /* the host's allocator refused */
    REQUEST_RULE = -32005,    /* the host refused a rule as malformed */
    REQUEST_BYTES = -32006    /* bytes in params are not what the method reads */
};

typedef struct request {
    ta_host *host;
    ta_cbor_document *document;
    cbor_output out;
    uint32_t id;   /* the request's "id" token; TA_NIL: none */
    int refused;   /* an error reply was written */
    char path[96]; /* where the error is, for {"error": {"data": {"path"}}} */
} request;

static ta_string_view request_text_of(const char *text) {
    uint32_t length = 0;
    while (text[length])
        length++;
    return (ta_string_view){text, length};
}

static int request_text_is(ta_string_view text, const char *name) {
    ta_string_view other = request_text_of(name);
    return text.length == other.length && ta_memcmp(text.pointer, other.pointer, text.length) == 0;
}

static void request_put_key(request *r, const char *key) { cbor_output_text(&r->out, request_text_of(key)); }

static void request_put_int(request *r, int64_t value) {
    if (value >= 0) cbor_output_head(&r->out, CBOR_MAJOR_UINT, (uint64_t)value);
    else cbor_output_head(&r->out, CBOR_MAJOR_NEGINT, (uint64_t)(-1 - value));
}

static void request_put_double(request *r, double value) {
    uint64_t bits;
    ta_memcpy(&bits, &value, sizeof bits);
    cbor_output_float(&r->out, bits);
}

static void request_put_null(request *r) { cbor_output_simple(&r->out, 22); }
static void request_put_bool(request *r, int value) { cbor_output_simple(&r->out, value ? 21u : 20u); }

/* ---- the reply's frame ------------------------------------------------------------- */

/* {"id": <the request's>, `key`: ...}: the id first when the request had one;
 * the caller writes the value of `key` next. */
static void request_open(request *r, const char *key) {
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, r->id != TA_NIL ? 2u : 1u);
    if (r->id != TA_NIL) {
        const ta_cbor_token *id = &r->document->tokens[r->id];
        request_put_key(r, "id");
        if (id->kind == TA_CBOR_UINT) cbor_output_uint(&r->out, id->value);
        else cbor_output_text(&r->out, ta_cbor_string(r->document, r->id));
    }
    request_put_key(r, key);
}

/* ---- errors ------------------------------------------------------------------------ */

/* The reply becomes {"id", "error": {"code", "message", "data": {"reason",
 * "path"}}}; the first one stays. */
static void request_refuse_with(request *r, int code, const char *reason, const char *message) {
    if (r->refused) return;
    r->refused = 1;
    r->out.length = 0;
    request_open(r, "error");
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 3);
    request_put_key(r, "code");
    request_put_int(r, code);
    request_put_key(r, "message");
    request_put_key(r, message);
    request_put_key(r, "data");
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 2);
    request_put_key(r, "reason");
    request_put_key(r, reason);
    request_put_key(r, "path");
    request_put_key(r, r->path);
}

/* A refused param or host condition: the code follows from the reason. */
static void request_refuse(request *r, const char *reason, const char *message) {
    static const struct {
        const char *reason;
        int code;
    } codes[] = {{"stale", REQUEST_STALE},   {"state", REQUEST_STATE},   {"limit", REQUEST_LIMIT},
                 {"memory", REQUEST_MEMORY}, {"invalid", REQUEST_RULE}, {"malformed", REQUEST_BYTES}};
    int code = REQUEST_PARAMS; /* type, missing, unknown, range */
    for (uint32_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if (request_text_is(request_text_of(reason), codes[i].reason)) code = codes[i].code;
    request_refuse_with(r, code, reason, message);
}

/* The path of a request member ("method", "params", "id"). */
static void request_path_member(request *r, const char *member) {
    ta_text text = ta_text_init(r->path, sizeof r->path - 1);
    ta_text_append(&text, request_text_of(member));
    r->path[text.overflow ? 0 : text.length] = 0;
}

/* The path of a param: `params.field`, or `params.outer[index].field` inside
 * a list; "" for the request as a whole. */
static void request_path_view(request *r, const char *outer, uint32_t index, ta_string_view field) {
    ta_text text = ta_text_init(r->path, sizeof r->path - 1);
    if (outer || field.length) ta_text_append(&text, request_text_of("params."));
    if (outer) {
        ta_text_append(&text, request_text_of(outer));
        ta_text_append(&text, request_text_of("["));
        ta_text_append_u64(&text, index);
        ta_text_append(&text, request_text_of(field.length ? "]." : "]"));
    }
    ta_text_append(&text, field);
    r->path[text.overflow ? 0 : text.length] = 0;
}

static void request_path_set(request *r, const char *outer, uint32_t index, const char *field) {
    request_path_view(r, outer, index, request_text_of(field));
}

/* ---- reading fields ----------------------------------------------------------------- */

static const ta_cbor_token *request_token(const request *r, uint32_t at) {
    return &r->document->tokens[at];
}

/* Every key of the map at `map` is one of `known` (NULL-terminated); no map
 * (TA_NIL: no params) has none. */
static int request_keys_known(request *r, uint32_t map, const char *outer, uint32_t index,
                      const char *const *known) {
    if (map == TA_NIL) return 1;
    uint32_t pairs = request_token(r, map)->count, at = map + 1;
    for (uint32_t p = 0; p < pairs; p++) {
        ta_string_view key = ta_cbor_string(r->document, at);
        int found = 0;
        if (request_token(r, at)->kind == TA_CBOR_TEXT)
            for (uint32_t k = 0; known[k] && !found; k++) {
                ta_string_view name = request_text_of(known[k]);
                found = key.length == name.length && ta_memcmp(key.pointer, name.pointer, key.length) == 0;
            }
        if (!found) {
            request_path_view(r, outer, index, key);
            request_refuse(r, "unknown", "params has a field this method does not take");
            return 0;
        }
        at = ta_cbor_next(r->document, ta_cbor_next(r->document, at));
    }
    return 1;
}

static uint32_t request_member(const request *r, uint32_t map, const char *key) {
    return ta_cbor_member(r->document, map, request_text_of(key));
}

static int request_is_null(const request *r, uint32_t at) {
    return request_token(r, at)->kind == TA_CBOR_SIMPLE && request_token(r, at)->value == TA_CBOR_NULL;
}

/* Each reader: 1 with the value (or the default when the field is absent),
 * 0 after refusing the request. */
static int request_read_uint(request *r, uint32_t at, uint64_t fallback, uint64_t *value) {
    if (at == TA_NIL) {
        *value = fallback;
        return 1;
    }
    if (request_token(r, at)->kind != TA_CBOR_UINT) {
        request_refuse(r, "type", "expected an unsigned integer");
        return 0;
    }
    *value = request_token(r, at)->value;
    return 1;
}

static int request_read_number(request *r, uint32_t at, double fallback, double *value) {
    const ta_cbor_token *t = at == TA_NIL ? NULL : request_token(r, at);
    if (!t) *value = fallback;
    else if (t->kind == TA_CBOR_UINT) *value = (double)t->value;
    else if (t->kind == TA_CBOR_NEGINT) *value = -1.0 - (double)t->value;
    else if (t->kind == TA_CBOR_FLOAT) ta_memcpy(value, &t->value, sizeof *value);
    else {
        request_refuse(r, "type", "expected a number");
        return 0;
    }
    return 1;
}

static int request_read_bool(request *r, uint32_t at, int fallback, int *value) {
    const ta_cbor_token *t = at == TA_NIL ? NULL : request_token(r, at);
    if (!t) *value = fallback;
    else if (t->kind == TA_CBOR_SIMPLE && (t->value == TA_CBOR_TRUE || t->value == TA_CBOR_FALSE))
        *value = t->value == TA_CBOR_TRUE;
    else {
        request_refuse(r, "type", "expected true or false");
        return 0;
    }
    return 1;
}

static int request_read_text(request *r, uint32_t at, ta_string_view *value) {
    if (at == TA_NIL) {
        *value = (ta_string_view){"", 0};
        return 1;
    }
    if (request_token(r, at)->kind != TA_CBOR_TEXT) {
        request_refuse(r, "type", "expected text");
        return 0;
    }
    *value = ta_cbor_string(r->document, at);
    return 1;
}

/* A detail preset by name ("summary", "decisions", "full", "off"), plus values. */
static int request_read_detail(request *r, const char *outer, uint32_t index, uint32_t detail_at,
                       uint32_t values_at, uint32_t *kinds) {
    ta_string_view name;
    int values;
    request_path_set(r, outer, index, "detail");
    if (!request_read_text(r, detail_at, &name)) return 0;
    if (detail_at == TA_NIL) name = request_text_of("summary");
    if (ta_trace_detail_by_name(name, kinds) != TA_OK) {
        request_refuse(r, "unknown", "detail is one of summary, decisions, full, off");
        return 0;
    }
    request_path_set(r, outer, index, "values");
    if (!request_read_bool(r, values_at, 0, &values)) return 0;
    if (values) *kinds |= TA_TRACE_VALUES;
    return 1;
}

static int request_read_ratio(request *r, uint32_t at, uint32_t *parts) {
    double share;
    if (!request_read_number(r, at, 1.0, &share)) return 0;
    if (!(share >= 0.0 && share <= 1.0)) {
        request_refuse(r, "range", "a share is between 0 and 1");
        return 0;
    }
    *parts = (uint32_t)(share * SAMPLE_WHOLE + 0.5);
    return 1;
}

/* ---- trace rules -------------------------------------------------------------------- */

static const char *const RULE_FIELDS[] = {"at",     "chart",  "actor",     "event",  "session",
                                          "from",   "sample", "detail",    "values", "perSecond",
                                          "burst",  NULL};

static int request_read_rule(request *r, const char *list, uint32_t index, uint32_t map, ta_trace_rule *rule) {
    ta_memset(rule, 0, sizeof *rule);
    rule->size = sizeof *rule;
    rule->detail = TA_NIL;
    request_path_set(r, list, index, "");
    if (request_token(r, map)->kind != TA_CBOR_MAP) {
        request_refuse(r, "type", "a rule is a map");
        return 0;
    }
    if (!request_keys_known(r, map, list, index, RULE_FIELDS)) return 0;
    ta_string_view at;
    request_path_set(r, list, index, "at");
    if (!request_read_text(r, request_member(r, map, "at"), &at)) return 0;
    if (request_text_is(at, "start")) rule->at = TA_TRACE_AT_START;
    else if (request_text_is(at, "take")) rule->at = TA_TRACE_AT_TAKE;
    else if (at.length) {
        request_refuse(r, "unknown", "at is start or take");
        return 0;
    }
    request_path_set(r, list, index, "chart");
    if (!request_read_text(r, request_member(r, map, "chart"), &rule->chart)) return 0;
    request_path_set(r, list, index, "actor");
    if (!request_read_text(r, request_member(r, map, "actor"), &rule->actor)) return 0;
    request_path_set(r, list, index, "event");
    if (!request_read_text(r, request_member(r, map, "event"), &rule->event)) return 0;
    request_path_set(r, list, index, "session");
    if (!request_read_uint(r, request_member(r, map, "session"), 0, &rule->session)) return 0;
    uint32_t from = request_member(r, map, "from");
    request_path_set(r, list, index, "from");
    if (from != TA_NIL) {
        static const char *const sources[] = {"host",    "actor",   "timer",
                                              "spawn",   "sampled", "unsampled"};
        if (request_token(r, from)->kind != TA_CBOR_ARRAY) {
            request_refuse(r, "type", "from is a list of host, actor, timer, spawn, sampled, unsampled");
            return 0;
        }
        uint32_t items = request_token(r, from)->count, item = from + 1;
        for (uint32_t i = 0; i < items; i++, item = ta_cbor_next(r->document, item)) {
            ta_string_view name;
            uint32_t bit = 0;
            if (!request_read_text(r, item, &name)) return 0;
            for (uint32_t k = 0; k < 6 && !bit; k++)
                if (request_text_is(name, sources[k])) bit = 1u << k;
            if (!bit) {
                request_refuse(r, "unknown", "from is a list of host, actor, timer, spawn, sampled, unsampled");
                return 0;
            }
            rule->from |= bit;
        }
    }
    uint32_t sample = request_member(r, map, "sample");
    request_path_set(r, list, index, "sample");
    if (sample == TA_NIL) {
        rule->sample = TA_TRACE_KEEP;
    } else if (request_token(r, sample)->kind == TA_CBOR_TEXT) {
        ta_string_view name = ta_cbor_string(r->document, sample);
        if (request_text_is(name, "keep")) rule->sample = TA_TRACE_KEEP;
        else if (request_text_is(name, "sample")) rule->sample = TA_TRACE_SAMPLE;
        else if (request_text_is(name, "drop")) rule->sample = TA_TRACE_DROP;
        else {
            request_refuse(r, "unknown", "sample is keep, sample, drop or a share between 0 and 1");
            return 0;
        }
    } else {
        rule->sample = TA_TRACE_RATIO;
        rule->sample_whole = SAMPLE_WHOLE;
        if (!request_read_ratio(r, sample, &rule->sample_parts)) return 0;
    }
    uint32_t detail = request_member(r, map, "detail"), values = request_member(r, map, "values");
    if (detail != TA_NIL || values != TA_NIL) {
        if (!request_read_detail(r, list, index, detail, values, &rule->detail)) return 0;
    }
    uint64_t per_second, burst;
    request_path_set(r, list, index, "perSecond");
    if (!request_read_uint(r, request_member(r, map, "perSecond"), 0, &per_second)) return 0;
    request_path_set(r, list, index, "burst");
    if (!request_read_uint(r, request_member(r, map, "burst"), 0, &burst)) return 0;
    if (per_second > UINT32_MAX || burst > UINT32_MAX) {
        request_refuse(r, "range", "perSecond and burst are at most 4294967295");
        return 0;
    }
    rule->per_second = (uint32_t)per_second;
    rule->burst = (uint32_t)burst;
    return 1;
}

/* The rules of `rules` (a list; absent: none) into `out`. */
static int request_read_rules(request *r, uint32_t rules, ta_trace_rule *out, uint32_t *count) {
    *count = 0;
    if (rules == TA_NIL) return 1;
    request_path_set(r, NULL, 0, "rules");
    if (request_token(r, rules)->kind != TA_CBOR_ARRAY) {
        request_refuse(r, "type", "rules is a list");
        return 0;
    }
    if (request_token(r, rules)->count > REQUEST_RULES) {
        request_refuse(r, "limit", "a request holds at most 32 rules");
        return 0;
    }
    uint32_t at = rules + 1;
    for (uint32_t i = 0; i < request_token(r, rules)->count; i++, at = ta_cbor_next(r->document, at))
        if (!request_read_rule(r, "rules", i, at, &out[i])) return 0;
    *count = request_token(r, rules)->count;
    return 1;
}

/* What the trace calls answered, as an error reply. */
static void request_refuse_trace(request *r, ta_status status) {
    request_path_set(r, NULL, 0, "");
    if (status == TA_ERROR_STATE) request_refuse(r, "state", "the host's systems do not trace");
    else if (status == TA_ERROR_INVALID) request_refuse(r, "invalid", "a rule is malformed");
    else request_refuse(r, "limit", "more details or rules than the host's systems hold");
}

/* ---- operations ---------------------------------------------------------------------- */

static int request_fits(const request *r) { return r->out.length <= r->out.capacity; }

static void request_run_trace_configure(request *r, uint32_t map) {
    static const char *const fields[] = {"detail", "values", "sample", "records", "rules", NULL};
    ta_trace_rule rules[REQUEST_RULES];
    host_trace_options options;
    int records;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    ta_memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.sample_whole = SAMPLE_WHOLE;
    if (!request_read_detail(r, NULL, 0, request_member(r, map, "detail"), request_member(r, map, "values"), &options.kinds))
        return;
    request_path_set(r, NULL, 0, "sample");
    if (!request_read_ratio(r, request_member(r, map, "sample"), &options.sample_parts)) return;
    request_path_set(r, NULL, 0, "records");
    if (!request_read_bool(r, request_member(r, map, "records"), 1, &records)) return;
    if (!records) options.flags |= HOST_TRACE_NO_RECORDS;
    if (!request_read_rules(r, request_member(r, map, "rules"), rules, &options.rule_count)) return;
    options.rules = rules;
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 0);
    if (!request_fits(r)) return;
    ta_status status = host_trace_set(r->host, &options);
    if (status != TA_OK) request_refuse_trace(r, status);
}

static void request_run_trace_watch(request *r, uint32_t map) {
    static const char *const fields[] = {"rules", "seconds", NULL};
    ta_trace_rule rules[REQUEST_RULES];
    host_watch_rules watch;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    ta_memset(&watch, 0, sizeof watch);
    if (!request_read_rules(r, request_member(r, map, "rules"), rules, &watch.rule_count)) return;
    if (!watch.rule_count) {
        request_path_set(r, NULL, 0, "rules");
        request_refuse(r, "missing", "a watch has at least one rule");
        return;
    }
    watch.rules = rules;
    uint32_t seconds_at = request_member(r, map, "seconds");
    watch.until = UINT64_MAX;
    if (seconds_at == TA_NIL || !request_is_null(r, seconds_at)) {
        double seconds;
        request_path_set(r, NULL, 0, "seconds");
        if (!request_read_number(r, seconds_at, WATCH_SECONDS, &seconds)) return;
        if (!(seconds >= 0.0 && seconds < 1e9)) {
            request_refuse(r, "range", "seconds is between 0 and 1e9, or null for no end");
            return;
        }
        uint64_t tps = r->host->config.system.ticks_per_second;
        watch.until = r->host->now + (uint64_t)(seconds * (double)(tps ? tps : TICKS_PER_SECOND) + 0.5);
    }
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 1);
    request_put_key(r, "id");
    cbor_output_uint(&r->out, r->host->watch_sequence + 1u); /* the id it will get */
    if (!request_fits(r)) return;
    uint32_t id = 0;
    ta_status status = host_trace_watch(r->host, &watch, &id);
    if (status != TA_OK) request_refuse_trace(r, status);
}

static void request_run_trace_unwatch(request *r, uint32_t map) {
    static const char *const fields[] = {"id", NULL};
    uint64_t id;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    uint32_t at = request_member(r, map, "id");
    request_path_set(r, NULL, 0, "id");
    if (at == TA_NIL) {
        request_refuse(r, "missing", "id is the watch's id");
        return;
    }
    if (!request_read_uint(r, at, 0, &id)) return;
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 0);
    if (!request_fits(r)) return;
    if (id > UINT32_MAX || host_trace_unwatch(r->host, (uint32_t)id) != TA_OK)
        request_refuse(r, "stale", "no such watch: it ended or never was");
}

static void request_put_chart(request *r, const ta_chart_stats *chart) {
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 12);
    request_put_key(r, "name");
    cbor_output_text(&r->out, chart->name);
    request_put_key(r, "actors");
    cbor_output_uint(&r->out, chart->actors);
    request_put_key(r, "hibernated");
    cbor_output_uint(&r->out, chart->hibernated);
    request_put_key(r, "spawned");
    cbor_output_uint(&r->out, chart->spawned);
    request_put_key(r, "ended");
    cbor_output_uint(&r->out, chart->ended);
    request_put_key(r, "turns");
    cbor_output_uint(&r->out, chart->turns);
    request_put_key(r, "macrosteps");
    cbor_output_uint(&r->out, chart->macrosteps);
    request_put_key(r, "fuel");
    cbor_output_uint(&r->out, chart->fuel);
    request_put_key(r, "errorsExecution");
    cbor_output_uint(&r->out, chart->errors_execution);
    request_put_key(r, "errorsCommunication");
    cbor_output_uint(&r->out, chart->errors_communication);
    request_put_key(r, "errorsPlatform");
    cbor_output_uint(&r->out, chart->errors_platform);
    request_put_key(r, "faults");
    cbor_output_uint(&r->out, chart->faults);
}

static void request_run_stats(request *r, uint32_t map) {
    static const char *const fields[] = {NULL};
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    ta_host_stats stats;
    stats.size = sizeof stats;
    host_stats_get(r->host, &stats);
    uint32_t count = host_chart_stats(r->host, NULL, 0);
    ta_chart_stats *charts = count ? host_alloc(r->host, sizeof *charts * count) : NULL;
    if (count && !charts) {
        request_path_set(r, NULL, 0, "");
        request_refuse(r, "memory", "the host's allocator refused room for the charts");
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        charts[i].size = sizeof charts[i];
    host_chart_stats(r->host, charts, count);
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 2);
    request_put_key(r, "host");
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 11);
    request_put_key(r, "segments");
    cbor_output_uint(&r->out, stats.segments);
    request_put_key(r, "actors");
    cbor_output_uint(&r->out, stats.actors);
    request_put_key(r, "hibernated");
    cbor_output_uint(&r->out, stats.hibernated);
    request_put_key(r, "runnable");
    cbor_output_uint(&r->out, stats.runnable);
    request_put_key(r, "jobs");
    cbor_output_uint(&r->out, stats.jobs);
    request_put_key(r, "regionBytes");
    cbor_output_uint(&r->out, stats.region_bytes);
    request_put_key(r, "delivered");
    cbor_output_uint(&r->out, stats.delivered);
    request_put_key(r, "routed");
    cbor_output_uint(&r->out, stats.routed);
    request_put_key(r, "migrated");
    cbor_output_uint(&r->out, stats.migrated);
    request_put_key(r, "retired");
    cbor_output_uint(&r->out, stats.retired);
    request_put_key(r, "fuelClock");
    cbor_output_uint(&r->out, stats.fuel_clock);
    request_put_key(r, "charts");
    cbor_output_head(&r->out, CBOR_MAJOR_ARRAY, count);
    for (uint32_t i = 0; i < count; i++)
        request_put_chart(r, &charts[i]);
    if (charts) host_free(r->host, charts, sizeof *charts * count);
}

static void request_run_actors(request *r, uint32_t map) {
    static const char *const fields[] = {"cursor", "limit", NULL};
    uint64_t sessions[REQUEST_ACTORS], cursor, limit;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    request_path_set(r, NULL, 0, "cursor");
    if (!request_read_uint(r, request_member(r, map, "cursor"), 0, &cursor)) return;
    request_path_set(r, NULL, 0, "limit");
    if (!request_read_uint(r, request_member(r, map, "limit"), 256, &limit)) return;
    if (limit < 1 || limit > REQUEST_ACTORS) {
        request_refuse(r, "range", "limit is between 1 and 512");
        return;
    }
    uint32_t n = host_actors(r->host, &cursor, (ta_uint64_array){sessions, (uint32_t)limit});
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 2);
    request_put_key(r, "sessions");
    cbor_output_head(&r->out, CBOR_MAJOR_ARRAY, n);
    for (uint32_t i = 0; i < n; i++)
        cbor_output_uint(&r->out, sessions[i]);
    request_put_key(r, "cursor"); /* where to go on; null when every session was listed */
    if (n < limit) request_put_null(r);
    else cbor_output_uint(&r->out, cursor);
}

/* The session of a request that names one. */
static int request_read_session(request *r, uint32_t map, uint64_t *session, uint32_t *segment,
                        uint32_t *actor) {
    uint32_t at = request_member(r, map, "session");
    request_path_set(r, NULL, 0, "session");
    if (at == TA_NIL) {
        request_refuse(r, "missing", "session is the actor's session id");
        return 0;
    }
    if (!request_read_uint(r, at, 0, session)) return 0;
    if (host_locate(r->host, *session, segment, actor) != TA_OK) {
        request_refuse(r, "stale", "no such session in this host");
        return 0;
    }
    return 1;
}

static void request_run_actor(request *r, uint32_t map) {
    static const char *const fields[] = {"session", NULL};
    uint64_t session;
    uint32_t segment, actor;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    if (!request_read_session(r, map, &session, &segment, &actor)) return;
    ta_system *system = ta_host_system(r->host, segment);
    ta_actor_info info;
    info.size = sizeof info;
    ta_actor_info_get(system, actor, &info);
    const ta_image *image = r->host->segments[segment].image;
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 6);
    request_put_key(r, "session");
    cbor_output_uint(&r->out, session);
    request_put_key(r, "segment");
    cbor_output_uint(&r->out, segment);
    request_put_key(r, "chart");
    cbor_output_text(&r->out, ta_image_chart_name(image, info.chart));
    request_put_key(r, "parent"); /* the session that invoked it; null for a root actor */
    if (info.parent) {
        ta_actor_info parent;
        parent.size = sizeof parent;
        if (ta_actor_info_get(system, info.parent, &parent) == TA_OK)
            cbor_output_uint(&r->out, parent.serial);
        else request_put_null(r);
    } else {
        request_put_null(r);
    }
    request_put_key(r, "mailbox");
    cbor_output_uint(&r->out, info.mailbox_length);
    request_put_key(r, "invocations");
    cbor_output_uint(&r->out, info.invocations);
}

static void request_run_state(request *r, uint32_t map) {
    static const char *const fields[] = {"session", NULL};
    uint64_t session;
    uint32_t segment, actor;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    if (!request_read_session(r, map, &session, &segment, &actor)) return;
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 1);
    request_put_key(r, "value");
    if (host_state_value(r->host, session, &r->out) != TA_OK) {
        request_path_set(r, NULL, 0, "session");
        request_refuse(r, "limit", "the configuration has more than 1024 states");
    }
}

/* A payload's neutral value (to_wire) straight into the reply; null when the
 * datamodel cannot write it. */
static void request_put_payload(request *r, const ta_datamodel_binding *binding, ta_payload payload) {
    size_t room = r->out.length < r->out.capacity ? r->out.capacity - r->out.length : 0;
    ta_buffer out = {room ? r->out.bytes + r->out.length : (void *)0, room, 0};
    ta_status status = binding->vtable->to_wire(binding->datamodel, payload, &out);
    if (status == TA_OK || status == TA_ERROR_NO_MEMORY) r->out.length += out.length;
    else request_put_null(r);
}

/* The datamodel binding that runs `chart` of `image`: its vtable's name is
 * the chart's datamodel attribute. */
static const ta_datamodel_binding *request_datamodel(request *r, const ta_image *image, uint32_t chart) {
    ta_string_view name = ta_image_chart_datamodel(image, chart);
    for (uint32_t i = 0; i < r->host->config.datamodel_count; i++)
        if (request_text_is(name, r->host->datamodels[i].vtable->name)) return &r->host->datamodels[i];
    return (const ta_datamodel_binding *)0;
}

static void request_run_actor_context(request *r, uint32_t map) {
    static const char *const fields[] = {"session", NULL};
    uint64_t session;
    uint32_t segment, actor;
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    if (!request_read_session(r, map, &session, &segment, &actor)) return;
    ta_actor_info info;
    info.size = sizeof info;
    ta_actor_info_get(ta_host_system(r->host, segment), actor, &info);
    const ta_image *image = r->host->segments[segment].image;
    const ta_datamodel_binding *binding = request_datamodel(r, image, info.chart);
    request_path_set(r, NULL, 0, "session");
    if (!binding || binding->vtable->size < sizeof(ta_datamodel_vtable) || !binding->vtable->variable ||
        !binding->vtable->to_wire) {
        request_refuse(r, "state", "the actor's datamodel does not show its variables");
        return;
    }
    if (!info.datamodel_instance) {
        request_refuse(r, "state", "the actor is hibernated: wake it first");
        return;
    }
    uint32_t count = ta_image_data_count(image, info.chart);
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 1);
    request_put_key(r, "variables");
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, count);
    for (uint32_t i = 0; i < count; i++) {
        ta_data_info data;
        ta_payload payload = 0;
        ta_image_data(image, (ta_chart_item){info.chart, i}, &data);
        cbor_output_text(&r->out, data.id);
        if (binding->vtable->variable(binding->datamodel, info.datamodel_instance, data.id, &payload) != TA_OK) {
            cbor_output_simple(&r->out, TA_CBOR_UNDEFINED);
            continue;
        }
        request_put_payload(r, binding, payload);
        if (binding->vtable->release) binding->vtable->release(binding->datamodel, payload);
    }
}

/* ---- decoded spans ------------------------------------------------------------------- */

static void request_put_trace_id(request *r, ta_trace_id trace) {
    char text[TA_TRACE_ID_TEXT_LENGTH];
    ta_buffer buffer = {text, sizeof text, 0};
    ta_trace_id_format(trace, &buffer);
    cbor_output_text(&r->out, (ta_string_view){text, (uint32_t)buffer.length});
}

static void request_put_span_id(request *r, ta_span_id span) {
    char text[16];
    ta_buffer buffer = {text, sizeof text, 0};
    ta_span_id_format(span, &buffer);
    cbor_output_text(&r->out, (ta_string_view){text, (uint32_t)buffer.length});
}

/* A value with no nested lists in it; anything deeper is null. */
static void request_put_scalar(request *r, const ta_otlp_value *value) {
    switch (value->kind) {
    case TA_OTLP_VALUE_STRING: cbor_output_text(&r->out, value->bytes); break;
    case TA_OTLP_VALUE_BOOL: request_put_bool(r, value->integer != 0); break;
    case TA_OTLP_VALUE_INT: request_put_int(r, value->integer); break;
    case TA_OTLP_VALUE_DOUBLE: request_put_double(r, value->number); break;
    case TA_OTLP_VALUE_BYTES:
        cbor_output_head(&r->out, CBOR_MAJOR_BYTES, value->bytes.length);
        cbor_output_bytes(&r->out, value->bytes.pointer, value->bytes.length);
        break;
    default: request_put_null(r);
    }
}

static void request_put_value(request *r, const ta_otlp_value *value) {
    uint32_t at = 0, count = 0;
    if (value->kind == TA_OTLP_VALUE_ARRAY) {
        ta_otlp_value item;
        while (ta_otlp_next_value(value->items, &at, &item) == 1)
            count++;
        cbor_output_head(&r->out, CBOR_MAJOR_ARRAY, count);
        for (at = 0; ta_otlp_next_value(value->items, &at, &item) == 1;)
            request_put_scalar(r, &item);
    } else if (value->kind == TA_OTLP_VALUE_LIST) {
        ta_otlp_key_value pair;
        while (ta_otlp_next_attribute(value->items, &at, &pair) == 1)
            count++;
        cbor_output_head(&r->out, CBOR_MAJOR_MAP, count);
        for (at = 0; ta_otlp_next_attribute(value->items, &at, &pair) == 1;) {
            cbor_output_text(&r->out, pair.key);
            request_put_scalar(r, &pair.value);
        }
    } else {
        request_put_scalar(r, value);
    }
}

static void request_put_attributes(request *r, ta_otlp_fields attributes) {
    ta_otlp_key_value pair;
    uint32_t at = 0, count = 0;
    while (ta_otlp_next_attribute(attributes, &at, &pair) == 1)
        count++;
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, count);
    for (at = 0; ta_otlp_next_attribute(attributes, &at, &pair) == 1;) {
        cbor_output_text(&r->out, pair.key);
        request_put_value(r, &pair.value);
    }
}

static void request_put_span(request *r, const ta_otlp_span *span) {
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 11);
    request_put_key(r, "trace");
    request_put_trace_id(r, span->trace);
    request_put_key(r, "span");
    request_put_span_id(r, span->span);
    request_put_key(r, "parent");
    if (span->parent) request_put_span_id(r, span->parent);
    else request_put_null(r);
    request_put_key(r, "name");
    cbor_output_text(&r->out, span->name);
    request_put_key(r, "kind");
    cbor_output_uint(&r->out, span->kind);
    request_put_key(r, "start");
    cbor_output_uint(&r->out, span->start);
    request_put_key(r, "end");
    cbor_output_uint(&r->out, span->end);
    request_put_key(r, "status");
    cbor_output_uint(&r->out, span->status);
    request_put_key(r, "statusMessage");
    cbor_output_text(&r->out, span->status_message);
    request_put_key(r, "attributes");
    request_put_attributes(r, span->attributes);
    request_put_key(r, "events");
    ta_otlp_event event;
    uint32_t at = 0, count = 0;
    while (ta_otlp_next_event(span->events, &at, &event) == 1)
        count++;
    cbor_output_head(&r->out, CBOR_MAJOR_ARRAY, count);
    for (at = 0; ta_otlp_next_event(span->events, &at, &event) == 1;) {
        cbor_output_head(&r->out, CBOR_MAJOR_MAP, 3);
        request_put_key(r, "name");
        cbor_output_text(&r->out, event.name);
        request_put_key(r, "time");
        cbor_output_uint(&r->out, event.time);
        request_put_key(r, "attributes");
        request_put_attributes(r, event.attributes);
    }
}

static void request_run_trace_spans_decode(request *r, uint32_t map) {
    static const char *const fields[] = {"traces", NULL};
    if (!request_keys_known(r, map, NULL, 0, fields)) return;
    uint32_t at = request_member(r, map, "traces");
    request_path_set(r, NULL, 0, "traces");
    if (at == TA_NIL || request_token(r, at)->kind != TA_CBOR_BYTES) {
        request_refuse(r, at == TA_NIL ? "missing" : "type",
               "traces is the bytes of an ExportTraceServiceRequest");
        return;
    }
    ta_string_view view = ta_cbor_string(r->document, at);
    ta_bytes traces = {view.pointer, view.length};
    ta_otlp_cursor cursor;
    ta_otlp_span span;
    uint32_t count = 0;
    int32_t got;
    ta_memset(&cursor, 0, sizeof cursor);
    while ((got = ta_otlp_next_span(traces, &cursor, &span)) == 1)
        count++;
    if (got != 0) {
        request_refuse(r, "malformed", "traces is not an ExportTraceServiceRequest");
        return;
    }
    cbor_output_head(&r->out, CBOR_MAJOR_MAP, 1);
    request_put_key(r, "spans");
    cbor_output_head(&r->out, CBOR_MAJOR_ARRAY, count);
    ta_memset(&cursor, 0, sizeof cursor);
    while (ta_otlp_next_span(traces, &cursor, &span) == 1)
        request_put_span(r, &span);
}

/* ---- the entry point ------------------------------------------------------------------ */

typedef struct request_method {
    const char *name;
    void (*run)(request *r, uint32_t params); /* params: the map, or TA_NIL for none */
} request_method;

static const request_method REQUEST_METHODS[] = {
    {"trace.configure", request_run_trace_configure},
    {"trace.watch", request_run_trace_watch},
    {"trace.unwatch", request_run_trace_unwatch},
    {"trace.spans.decode", request_run_trace_spans_decode},
    {"stats", request_run_stats},
    {"actors", request_run_actors},
    {"actor", request_run_actor},
    {"actor.context", request_run_actor_context},
    {"state", request_run_state},
};

/* The request's members: "id" (uint or text), "method" (text), "params" (a
 * map); the method it names, or NULL after refusing. */
static const request_method *request_envelope(request *r, uint32_t *params) {
    static const char *const members[] = {"method", "params", "id", NULL};
    uint32_t id = ta_cbor_member(r->document, 0, request_text_of("id"));
    if (id != TA_NIL) {
        uint32_t kind = request_token(r, id)->kind;
        if (kind != TA_CBOR_UINT && kind != TA_CBOR_TEXT) {
            request_path_member(r, "id");
            request_refuse_with(r, REQUEST_INVALID, "type", "id is an unsigned integer or text");
            return NULL;
        }
        r->id = id;
    }
    uint32_t pairs = request_token(r, 0)->count, at = 1;
    for (uint32_t p = 0; p < pairs; p++, at = ta_cbor_next(r->document, ta_cbor_next(r->document, at))) {
        int known = 0;
        for (uint32_t k = 0; members[k] && !known; k++)
            known = request_token(r, at)->kind == TA_CBOR_TEXT &&
                    request_text_is(ta_cbor_string(r->document, at), members[k]);
        if (!known) {
            ta_text text = ta_text_init(r->path, sizeof r->path - 1);
            if (request_token(r, at)->kind == TA_CBOR_TEXT) ta_text_append(&text, ta_cbor_string(r->document, at));
            r->path[text.overflow ? 0 : text.length] = 0;
            request_refuse_with(r, REQUEST_INVALID, "unknown", "a request has only method, params and id");
            return NULL;
        }
    }
    uint32_t method = ta_cbor_member(r->document, 0, request_text_of("method"));
    request_path_member(r, "method");
    if (method == TA_NIL) {
        request_refuse_with(r, REQUEST_INVALID, "missing", "method names what the request does");
        return NULL;
    }
    if (request_token(r, method)->kind != TA_CBOR_TEXT) {
        request_refuse_with(r, REQUEST_INVALID, "type", "method is text");
        return NULL;
    }
    *params = ta_cbor_member(r->document, 0, request_text_of("params"));
    if (*params != TA_NIL && request_token(r, *params)->kind != TA_CBOR_MAP) {
        request_path_member(r, "params");
        request_refuse_with(r, REQUEST_INVALID, "type", "params is a map");
        return NULL;
    }
    ta_string_view name = ta_cbor_string(r->document, method);
    for (uint32_t i = 0; i < sizeof REQUEST_METHODS / sizeof REQUEST_METHODS[0]; i++)
        if (request_text_is(name, REQUEST_METHODS[i].name)) return &REQUEST_METHODS[i];
    request_refuse_with(r, REQUEST_METHOD, "unknown", "no such method");
    return NULL;
}

ta_status ta_host_request(ta_host *host, ta_bytes bytes, ta_buffer *reply) {
    reply->length = 0;
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    ta_cbor_token tokens[TA_HOST_REQUEST_TOKENS];
    ta_cbor_document document = {bytes, tokens, TA_HOST_REQUEST_TOKENS, 0};
    request r;
    ta_memset(&r, 0, sizeof r);
    r.host = host;
    r.document = &document;
    r.id = TA_NIL;
    r.out = (cbor_output){(unsigned char *)reply->pointer, reply->pointer ? reply->capacity : 0, 0};
    ta_status status = ta_cbor_decode(&document);
    if (status == TA_ERROR_NO_MEMORY) {
        request_refuse_with(&r, REQUEST_LIMIT, "limit", "a request has at most 256 items");
    } else if (status != TA_OK) {
        request_refuse_with(&r, REQUEST_PARSE, "malformed", "the request is not one well-formed CBOR item");
    } else if (tokens[0].kind != TA_CBOR_MAP) {
        request_refuse_with(&r, REQUEST_INVALID, "type", "a request is a map");
    } else {
        uint32_t params = TA_NIL;
        const request_method *method = request_envelope(&r, &params);
        if (method) {
            request_open(&r, "result");
            method->run(&r, params);
        }
    }
    reply->length = r.out.length;
    return request_fits(&r) ? TA_OK : TA_ERROR_NO_MEMORY;
}
