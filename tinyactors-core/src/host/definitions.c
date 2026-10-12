/* Definition logs (docs/architecture.md, 16.3): one OTLP log
 * record when a chart version is linked (ta_host_add_image), retired (a
 * newer image links a chart of the same name) and freed (its segment goes).
 * They happen between runs, never per event, and only the OTLP encoder
 * writes them (host_encoder.definitions): the null encoder does nothing. */
#include "base/format.h"
#include "base/memory.h"
#include "host/host_internal.h"

#define DEFINITION_TEXT(literal) ((ta_string_view){(literal), (uint32_t)(sizeof(literal) - 1u)})

/* A chart's key, as the version table matches it: its name, else its source. */
static ta_string_view definition_chart_key(const ta_image *image, uint32_t chart) {
    ta_string_view name = ta_image_chart_name(image, chart);
    return name.length ? name : ta_image_chart_source(image, chart);
}

static int definition_key_equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (!a.length || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

static ta_image_info definition_image_info(const ta_image *image) {
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(image, &info);
    return info;
}

/* The chart of `image` with key `key`, or TA_NIL. */
static uint32_t definition_chart_with_key(const ta_image *image, ta_string_view key) {
    uint32_t charts = definition_image_info(image).chart_count;
    for (uint32_t c = 0; c < charts; c++)
        if (definition_key_equal(definition_chart_key(image, c), key)) return c;
    return TA_NIL;
}

static void definition_log(ta_host *host, uint32_t kind, uint32_t segment, uint32_t chart) {
    static const ta_string_view event_names[] = {DEFINITION_TEXT("tinyactors.definition.linked"),
                                                 DEFINITION_TEXT("tinyactors.definition.retired"),
                                                 DEFINITION_TEXT("tinyactors.definition.freed")};
    static const ta_string_view verbs[] = {DEFINITION_TEXT("linked "), DEFINITION_TEXT("retired "), DEFINITION_TEXT("freed ")};
    const host_segment *record = &host->segments[segment];
    ta_string_view name = definition_chart_key(record->image, chart);
    uint64_t hash = definition_image_info(record->image).content_hash;
    char hex[16];
    for (int i = 0; i < 16; i++)
        hex[i] = "0123456789abcdef"[(hash >> (60 - 4 * i)) & 15u];
    ta_otlp_attribute texts[] = {{DEFINITION_TEXT("tinyactors.chart.name"), name},
                                 {DEFINITION_TEXT("tinyactors.image.hash"), {hex, 16}}};
    ta_otlp_integer_attribute integers[2] = {{DEFINITION_TEXT("tinyactors.segment"), segment}, {{0, 0}, 0}};
    uint32_t integer_count = 1;
    char text[160];
    ta_text body = ta_text_init(text, sizeof text);
    ta_text_append(&body, verbs[kind]);
    ta_text_append(&body, name);
    ta_text_append(&body, DEFINITION_TEXT(" (segment "));
    ta_text_append_u64(&body, segment);
    if (kind == HOST_DEFINITION_RETIRED) {
        /* the actors still on it: they finish there or migrate */
        ta_chart_stats stats;
        stats.size = sizeof stats;
        uint64_t actors = 0;
        if (ta_system_chart_stats_get(record->system, chart, &stats) == TA_OK)
            actors = stats.actors;
        integers[integer_count++] =
            (ta_otlp_integer_attribute){DEFINITION_TEXT("tinyactors.chart.actors"), (int64_t)actors};
        ta_text_append(&body, DEFINITION_TEXT(", "));
        ta_text_append_u64(&body, actors);
        ta_text_append(&body, actors == 1 ? DEFINITION_TEXT(" actor") : DEFINITION_TEXT(" actors"));
    }
    ta_text_append(&body, DEFINITION_TEXT(")"));
    ta_otlp_log log = {host->wall_ns,
                       kind == HOST_DEFINITION_RETIRED ? TA_OTLP_SEVERITY_INFO
                                                       : TA_OTLP_SEVERITY_DEBUG,
                       2,
                       integer_count,
                       event_names[kind],
                       {text, body.length},
                       texts,
                       integers};
    /* A staging with no room drops the record, as it would drop any record
     * it cannot hold; the logs are due at once so the next take makes room. */
    if (ta_otlp_add_log(host->otlp, &log) == TA_ERROR_NO_MEMORY) host->staging_full = 1;
}

void host_definitions_otlp(ta_host *host, uint32_t kind, uint32_t segment) {
    const ta_image *image = host->segments[segment].image;
    uint32_t charts = definition_image_info(image).chart_count;
    for (uint32_t c = 0; c < charts; c++) {
        if (kind == HOST_DEFINITION_FREED) {
            definition_log(host, HOST_DEFINITION_FREED, segment, c);
            continue;
        }
        definition_log(host, HOST_DEFINITION_LINKED, segment, c);
        /* the version it replaces: newer segments win (versions_recompute) */
        ta_string_view key = definition_chart_key(image, c);
        for (uint32_t si = segment; si-- > 0;) {
            const host_segment *older = &host->segments[si];
            if (!older->live) continue;
            uint32_t chart = definition_chart_with_key(older->image, key);
            if (chart == TA_NIL) continue;
            definition_log(host, HOST_DEFINITION_RETIRED, si, chart);
            break;
        }
    }
}
