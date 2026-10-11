/* tinyactors/image.h — verify and inspect a compiled system image (.tai).
 *
 * The image is immutable, position independent, little-endian, and may live
 * in ROM/flash/mmap. It is never written. One image may back any number of
 * systems concurrently. A single-chart image is a system image with one chart.
 */
#ifndef TINYACTORS_IMAGE_H
#define TINYACTORS_IMAGE_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side validated view (section base pointers). Fixed-size opaque storage
 * so hosts can place it anywhere without an allocation. */
typedef struct ta_image {
    uint64_t opaque[32];
} ta_image;

typedef struct ta_image_info {
    uint32_t size;
    uint32_t version;
    uint32_t chart_count;
    uint32_t boot_actor_count;
    uint32_t datamodel_count;
    uint32_t event_name_count;
    uint64_t content_hash; /* recorded in snapshots */
} ta_image_info;

/* Bounds-checks every section/offset/index once. Safe on untrusted input. */
TA_API ta_status ta_image_open(const void *bytes, size_t length, ta_image *out);
TA_API void ta_image_get_info(const ta_image *image, ta_image_info *out);
/* The bytes the image was opened from (ta_image_open). */
TA_API const void *ta_image_bytes(const ta_image *image, size_t *length);

TA_API uint32_t ta_image_chart_by_name(const ta_image *image,
                                       ta_string_view name); /* TA_NIL if absent */
TA_API ta_string_view ta_image_chart_name(const ta_image *image, uint32_t chart);
/* Source the chart was compiled from (file / ta_source name; inline documents
 * are "<source>#inline<n>"). Unlike the name (<scxml name>), unique per image. */
TA_API ta_string_view ta_image_chart_source(const ta_image *image, uint32_t chart);
TA_API ta_string_view ta_image_chart_datamodel(const ta_image *image, uint32_t chart);
TA_API uint32_t ta_image_state_count(const ta_image *image, uint32_t chart);
TA_API ta_string_view ta_image_state_name(const ta_image *image, uint32_t chart, uint32_t state);
TA_API uint32_t ta_image_state_by_name(const ta_image *image, uint32_t chart, ta_string_view name);
TA_API ta_string_view ta_image_event_name(const ta_image *image, uint32_t name_id);
/* The state tree, for hosts that present configurations (e.g. as nested state
 * values). State 0 is the <scxml> root (parent TA_NIL); states are in document
 * order, so a parent always precedes its children. */
typedef struct ta_state_info {
    uint32_t size;
    uint32_t parent;   /* TA_NIL for the root */
    uint32_t kind;     /* TA_STATE_* */
    uint32_t depth;    /* 0 for the root */
    ta_string_view id; /* as ta_image_state_name (generated for states without an id) */
} ta_state_info;
enum {
    TA_STATE_ATOMIC = 0,
    TA_STATE_COMPOUND = 1,
    TA_STATE_PARALLEL = 2,
    TA_STATE_FINAL = 3,
    TA_STATE_HISTORY_SHALLOW = 4,
    TA_STATE_HISTORY_DEEP = 5
};
/* An item of a chart: a state, a <data> declaration or an expression. */
typedef struct ta_chart_item {
    uint32_t chart;
    uint32_t index;
} ta_chart_item;
TA_API ta_status ta_image_state_info(const ta_image *image, ta_chart_item state,
                                     ta_state_info *out);
/* An actor a system image boots (<system><actor name chart/>). */
typedef struct ta_boot_actor {
    ta_string_view name;
    uint32_t chart;
    uint32_t pad;
} ta_boot_actor;
/* The i-th actor a system image boots (i < ta_image_info.boot_actor_count). */
TA_API ta_status ta_image_boot_actor(const ta_image *image, uint32_t i, ta_boot_actor *out);
/* Resolve a string id found in image tables (e.g. ta_datamodel_capture params). */
TA_API ta_string_view ta_image_string(const ta_image *image, uint32_t sid);

/* <data> declarations, for datamodel init_data(). */
typedef struct ta_data_info {
    ta_string_view id;
    ta_expression
        expression; /* TA_NIL if none (content, if any, is a TA_EXPRESSION_CONTENT expr) */
    uint32_t state; /* owning state */
} ta_data_info;
TA_API uint32_t ta_image_data_count(const ta_image *image, uint32_t chart);
TA_API ta_status ta_image_data(const ta_image *image, ta_chart_item data, ta_data_info *out);

/* Expression table access for datamodels (chart_attach / AOT compilers). */
typedef struct ta_expression_info {
    uint32_t kind; /* TA_EXPRESSION_* */
    uint32_t line, column;
    ta_string_view source;
} ta_expression_info;
enum {
    TA_EXPRESSION_CONDITION = 1,
    TA_EXPRESSION_VALUE = 2,
    TA_EXPRESSION_LOCATION = 3,
    TA_EXPRESSION_SCRIPT = 4,
    TA_EXPRESSION_STRING = 5, /* *expr attributes: eventexpr, targetexpr, delayexpr, ... */
    TA_EXPRESSION_CONTENT = 6
};
TA_API uint32_t ta_image_expression_count(const ta_image *image, uint32_t chart);
/* expression.index: the ta_expression */
TA_API ta_status ta_image_expression(const ta_image *image, ta_chart_item expression,
                                     ta_expression_info *out);
/* Optional datamodel-specific AOT section for (datamodel name, chart). */
TA_API ta_string_view ta_image_ahead_of_time_section(const ta_image *image, uint32_t chart,
                                                     ta_string_view datamodel);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_IMAGE_H */
