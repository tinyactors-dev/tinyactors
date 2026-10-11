/* binding.h — the native side of the Node.js binding (Node-API).
 *
 * One C file per topic (docs/adr/0008); this header holds what they share.
 *
 * Pure functions (hand-written wrappers of the core's public headers):
 *
 *   writeXML(ops, strings) -> xml                      ta_xml_writer_*
 *   compile(sources, files) -> image                   ta_compile_charts
 *   compileSystem(manifest, files) -> image            ta_compile_system
 *   openImage(bytes) -> image, imageBytes(image)       ta_image_open
 *   imageInfo(image) -> { hash, charts, actors }       ta_image_get_info, ta_image_chart_*,
 *                                                      ta_image_state_info, ta_image_boot_actor
 *   parseAddress(text), formatAddress(system, local)   ta_address_*
 *   frameHeader(length), frameReader(max), frameRead   ta_frame_*
 *   parseTraceparent, formatTraceparent, stepSpanID    ta_traceparent_*, ta_step_key_span_id
 *   decodeTraces, decodeMetrics, metricsToOTLP         ta_otlp_next_*, ta_otlp_add_metric
 *   constants                                          numeric constants of the headers
 *
 * A System: one ta_host, its datamodel ("ecmascript": the native one below,
 * or the C one, quickjs-ng), its run loop and effect delivery.
 *
 *   createSystem(callbacks, config) / restoreSystem(callbacks, config, images, host, datamodel)
 *     callbacks: { resolve(source, texts, kinds) -> functions (native ecmascript),
 *                  effects(effects, changed, traces, logs), ref(address) -> value,
 *                  clock() -> ms since the epoch (trace times) }
 *   addImage(system, image) -> segment, chart(system, name) -> [segment, chart] | null
 *   boot(system, segment), actorByName(system, name), actors(system)
 *   spawn(system, segment, chart, name, input, traceparent, now) -> session
 *   send(system, session, name, data, traceparent, now) -> run code
 *   sendMany(system, sessions, names, datas, traceparents, now) -> [accepted, run code]
 *   post(system, session, { name, data, sendid, origin, origintype, raw, traceparent,
 *        flags, token }, now) -> run code              ta_host_send with every field
 *   deliver(system, peer, envelope, now) -> outcome    ta_host_deliver
 *   pump(system, now) -> run code                      the run loop on its own
 *   terminate(system, session, now)
 *   inspect(system, session) -> { segment, chart, state, states } | null
 *   context(system, session) -> the session's variables
 *   sweep, hibernate, wake, exportActor, adopt, migrate, snapshot, stats, peer*,
 *   route, exportName, traceConfigure, traceEnd, metrics, destroy
 *
 * The run loop (pump): ta_host_advance_time, then ta_host_run in rounds
 * until idle or the round limit; every effect goes into one array, handed
 * to callbacks.effects once per call with the sessions whose datamodel saw
 * an event (changed) and the OTLP bytes of the runs. The run code: -2 more
 * work is runnable now, -1 idle without timers, else the next deadline (ms).
 * TA_EFFECT_REQUEST (an <invoke src> that is not a chart of its image) is
 * answered here (ta_host_resume): the chart of that source name in the
 * segment's image, if any. Calls are never nested: JavaScript does not call
 * into a System while it runs (core.mjs queues such calls).
 *
 * The native "ecmascript" datamodel: values are JS values held by
 * references. A session's variables are one object, its scope: <data> ids
 * as properties, and _event, _sessionid, _name, _ioprocessors and In as
 * non-enumerable ones. An expression is a function of the scope, from
 * callbacks.resolve when a chart is attached: a chart's c.expr function, or
 * its text compiled once. Neutral values (docs/protocol.md, "Values") are
 * written and read here (to_wire / from_wire), and blobs, snapshots and
 * envelopes hold them. */
#ifndef TINYACTORS_NODE_BINDING_H
#define TINYACTORS_NODE_BINDING_H

#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/runtime.h>

#define IO_PROCESSORS_MAX 16u

/* the quickjs datamodel's own vtable (src/datamodel_ecmascript) */
#define Q(system) ((system)->quickjs_vtable)

/* A growable byte buffer (JSON text, blobs). */
typedef struct text {
    char *bytes;
    size_t length, capacity;
    int failed;
} text;

/* ---- images ------------------------------------------------------------------------- */

typedef struct nd_image {
    void *bytes;
    size_t length;
    ta_image image;
} nd_image;

/* src="..." at compile time (<invoke src>, <data src>, <script src>): the files
 * passed along, by name (file: and ./ stripped), else by base name */
typedef struct nd_files {
    const ta_source *source;
    uint32_t count;
} nd_files;

typedef struct nd_frames {
    ta_frame_reader reader;
    uint8_t *buffer;
} nd_frames;

/* ---- the System --------------------------------------------------------------------------- */

typedef struct nd_chart {
    const ta_image *image;
    uint32_t chart, count;
    uint64_t hash;
    napi_ref *function; /* native: per expression id, the function of the scope */
    void *inner;        /* quickjs: its own chart context */
} nd_chart;

typedef struct nd_instance { /* native: one session's datamodel state */
    napi_ref scope, input;
    nd_chart *chart;
    uint64_t serial;
    uint32_t stamp, next_free;
    int live;
    char *string; /* the last eval_string / log result */
    ta_datamodel_environment *outside;
} nd_instance;

typedef struct nd_session { /* quickjs: what the binding knows of one of its instances */
    uint64_t serial;
    const ta_image *image;
    uint32_t chart, stamp;
} nd_session;

typedef struct nd_grave { /* a session that ended in this pump: its last variables */
    uint64_t serial;
    napi_ref context;
} nd_grave;

typedef struct nd_failure { /* an exception in an expression */
    uint64_t serial;
    napi_ref error;
} nd_failure;

typedef struct nd_system {
    napi_env environment; /* of the current native call */
    ta_host *host;
    int native; /* the native ecmascript datamodel; else quickjs */
    ta_datamodel_ecmascript *quickjs;
    const ta_datamodel_vtable *quickjs_vtable;
    ta_datamodel_binding binding;
    napi_ref resolve, effects, sync, refs, clock, in_function;
    char system_id[TA_ADDRESS_SYSTEM_MAX + 1];
    ta_io_processor_descriptor io[IO_PROCESSORS_MAX];
    char *io_text[2 * IO_PROCESSORS_MAX];
    uint32_t io_count;
    /* native datamodel: instances (handle = index + 1) and payloads (refcounted) */
    nd_instance *instances;
    uint32_t instance_count, instance_capacity, free_instance, live_instances;
    napi_ref *payloads;
    uint32_t *payload_refs;
    uint32_t payload_count, payload_capacity, free_payload, live_payloads;
    nd_instance *evaluating;
    nd_chart **charts;
    uint32_t chart_count, chart_capacity;
    /* quickjs: instance handle -> session (open addressing) */
    uint64_t *keys;
    nd_session *sessions;
    uint32_t map_capacity, map_count;
    /* the run loop */
    int running, keep_quiet; /* keep_quiet: destroy() is not a session's end (hibernate, export) */
    uint32_t pump;
    napi_value effect_list, sync_list; /* sync_list: sends to TA_IO_PROCESSOR_SYNC processors */
    uint32_t effect_count, sync_count;
    uint64_t *changed;
    uint32_t changed_count, changed_capacity;
    nd_grave *graves;
    uint32_t grave_count, grave_capacity;
    nd_failure *failures;
    uint32_t failure_count, failure_capacity;
    napi_ref *images; /* per segment: the image, until TA_HOST_EFFECT_SEGMENT_FREED */
    uint32_t image_count, image_capacity;
    text wire;       /* scratch for neutral values */
    char error[512]; /* why the last serialize failed */
    /* tracing (docs/adr/0006 in the repository root) */
    ta_otlp *otlp;
    size_t otlp_length;
    unsigned char *traces, *logs;
    size_t traces_length, traces_capacity, logs_length, logs_capacity;
} nd_system;

/* Attributes given as [[key, value], ...] of strings: ta_otlp_attribute
 * views of texts the caller frees with attributes_free. */
typedef struct js_attributes {
    ta_otlp_attribute *list;
    char **texts;
    uint32_t count;
} js_attributes;

/* A blob through a call that sizes it first (*length = needed on TA_ERROR_NO_MEMORY). */
typedef ta_status (*blob_writer)(nd_system *system, uint64_t session, void *buffer, size_t capacity,
                                 size_t *length);

/* addon.c */
char *get_string(napi_env environment, napi_value v, size_t *length_out);
napi_value str_value(napi_env environment, ta_string_view s);
napi_value undef(napi_env environment);
napi_value null_value(napi_env environment);
napi_value bool_value(napi_env environment, int b);
napi_value num(napi_env environment, double d);
napi_valuetype type_of(napi_env environment, napi_value v);
uint32_t get_u32(napi_env environment, napi_value v);
double get_double(napi_env environment, napi_value v);
uint64_t get_session(napi_env environment, napi_value v);
napi_ref make_ref(napi_env environment, napi_value v);
napi_value get_ref(napi_env environment, napi_ref r);
void set_ref(napi_env environment, napi_ref *r, napi_value v);
napi_value bytes_value(napi_env environment, const void *p, size_t n);
int bytes_of(napi_env environment, napi_value v, void **data, size_t *n);
napi_value property(napi_env environment, napi_value o, const char *name);
char *property_string(napi_env environment, napi_value o, const char *name, size_t *n);
double property_double(napi_env environment, napi_value o, const char *name);
int property_bool(napi_env environment, napi_value o, const char *name);
uint32_t array_length(napi_env environment, napi_value v);
void clear_exception(napi_env environment);
napi_value call_global(napi_env environment, const char *object, const char *method, size_t argc,
                       napi_value *argv);
napi_value call(napi_env environment, napi_value f, size_t argc, napi_value *argv);
void set(napi_env environment, napi_value o, const char *name, napi_value v);
int text_room(text *t, size_t more);
void text_put(text *t, const char *p, size_t n);
void text_char(text *t, char c);
void text_string(text *t, const char *p, size_t n);
ta_string_view view_of(const char *p, size_t n);

/* values.c */
ta_status wire_value(napi_env environment, napi_value v, text *t, int depth);
ta_status wire_of(napi_env environment, napi_value v, text *t);
napi_value value_of_wire(napi_env environment, ta_string_view json, napi_value refs);

/* images.c */
nd_image *get_image(napi_env environment, napi_value v);
ta_string_view source_name(ta_string_view source);
napi_value js_compile(napi_env environment, napi_callback_info info);
napi_value js_compile_system(napi_env environment, napi_callback_info info);
napi_value js_write_xml(napi_env environment, napi_callback_info info);
napi_value js_open_image(napi_env environment, napi_callback_info info);
napi_value js_image_bytes(napi_env environment, napi_callback_info info);
napi_value js_image_info(napi_env environment, napi_callback_info info);

/* pure.c */
uint64_t decimal_of(napi_env environment, napi_value v);
napi_value js_parse_address(napi_env environment, napi_callback_info info);
napi_value js_format_address(napi_env environment, napi_callback_info info);
napi_value js_system_valid(napi_env environment, napi_callback_info info);
napi_value js_frame_header(napi_env environment, napi_callback_info info);
napi_value js_frame_reader(napi_env environment, napi_callback_info info);
napi_value js_frame_read(napi_env environment, napi_callback_info info);
int hex_text(const char *t, size_t length, uint64_t *high, uint64_t *low);
napi_value js_step_span_id(napi_env environment, napi_callback_info info);
napi_value js_parse_traceparent(napi_env environment, napi_callback_info info);
napi_value js_format_traceparent(napi_env environment, napi_callback_info info);

/* datamodel.c */
ta_payload payload_new(nd_system *system, napi_value v);
napi_value payload_get(nd_system *system, ta_payload payload);
nd_instance *instance_at(nd_system *system, uint64_t handle);
napi_value js_in(napi_env environment, napi_callback_info info);
void define_hidden(napi_env environment, napi_value o, const char *name, napi_value v,
                   int writable);
napi_value scope_new(nd_system *system, napi_value sessionid, napi_value name,
                     napi_value io_processors);
void instance_free(nd_system *system, uint64_t handle);
ta_status scope_wire(nd_system *system, napi_value scope, text *t);
void copy_properties(napi_env environment, napi_value to, napi_value from);

/* quickjs.c */
void session_put(nd_system *system, uint64_t handle, uint64_t serial, const ta_image *image,
                 uint32_t chart);
napi_value quickjs_variables(nd_system *system, uint64_t handle, const ta_image *image,
                             uint32_t chart);

/* system.c */
void grow(void **p, uint32_t *capacity, uint32_t need, size_t size);
void mark_changed(nd_system *system, uint32_t *stamp, uint64_t serial);
uint64_t serial_of(ta_string_view sessionid);
void grave_add(nd_system *system, uint64_t serial, napi_value context);
void take_exception(nd_system *system, uint64_t serial);
void chart_add(nd_system *system, nd_chart *chart);
void chart_remove(nd_system *system, nd_chart *chart);
nd_chart *chart_new(const ta_datamodel_chart *chart);
void system_finalize(napi_env environment, void *data, void *hint);
nd_system *get_system(napi_env environment, napi_value v);
double pump(nd_system *system, double now);
napi_value code_value(napi_env environment, double code);
double now_of(napi_env environment, napi_value v);
nd_system *system_new(napi_env environment, napi_value callbacks, napi_value options,
                      ta_host_config *config);
napi_value system_value(napi_env environment, nd_system *system);
napi_value js_create_system(napi_env environment, napi_callback_info info);
void keep_image(nd_system *system, uint32_t segment, napi_value image);
napi_value js_add_image(napi_env environment, napi_callback_info info);
napi_value js_chart(napi_env environment, napi_callback_info info);
napi_value js_boot(napi_env environment, napi_callback_info info);
napi_value js_actor_by_name(napi_env environment, napi_callback_info info);
napi_value js_actors(napi_env environment, napi_callback_info info);
char *optional_string(napi_env environment, napi_value v, size_t *n);
napi_value js_spawn(napi_env environment, napi_callback_info info);
napi_value js_send(napi_env environment, napi_callback_info info);
napi_value js_send_many(napi_env environment, napi_callback_info info);
napi_value js_post(napi_env environment, napi_callback_info info);
napi_value js_deliver(napi_env environment, napi_callback_info info);
napi_value js_pump(napi_env environment, napi_callback_info info);
napi_value js_terminate(napi_env environment, napi_callback_info info);
int locate(nd_system *system, uint64_t session, ta_system **segment_system, uint32_t *segment,
           ta_actor_info *info);
napi_value js_inspect(napi_env environment, napi_callback_info info);
napi_value js_context(napi_env environment, napi_callback_info info);
napi_value js_stats(napi_env environment, napi_callback_info info);
napi_value js_destroy(napi_env environment, napi_callback_info info);
ta_string_view *get_strs(napi_env environment, napi_value arr, uint32_t *n);

/* persist.c */
napi_value js_sweep(napi_env environment, napi_callback_info info);
napi_value js_hibernate(napi_env environment, napi_callback_info info);
napi_value js_wake(napi_env environment, napi_callback_info info);
napi_value js_export_actor(napi_env environment, napi_callback_info info);
napi_value js_adopt(napi_env environment, napi_callback_info info);
napi_value js_migrate(napi_env environment, napi_callback_info info);
napi_value js_snapshot(napi_env environment, napi_callback_info info);
napi_value js_restore_system(napi_env environment, napi_callback_info info);

/* router.c */
void strs_free(ta_string_view *s, uint32_t n);
napi_value js_peer_add(napi_env environment, napi_callback_info info);
napi_value js_peer_up(napi_env environment, napi_callback_info info);
napi_value js_peer_remove(napi_env environment, napi_callback_info info);
napi_value js_peer_reach(napi_env environment, napi_callback_info info);
napi_value js_route(napi_env environment, napi_callback_info info);
napi_value js_export(napi_env environment, napi_callback_info info);
napi_value js_peer_error(napi_env environment, napi_callback_info info);
napi_value js_peer_round_trip_time(napi_env environment, napi_callback_info info);
napi_value js_peer_status(napi_env environment, napi_callback_info info);
void constant(napi_env environment, napi_value o, const char *name, double value);

/* trace.c */
napi_value decoded_id(napi_env environment, uint64_t high, uint64_t low, int wide);
napi_value decoded_attributes(napi_env environment, ta_otlp_fields fields);
napi_value js_decode_traces(napi_env environment, napi_callback_info info);
uint64_t trace_wall(nd_system *host);
uint64_t trace_fuel(nd_system *host);
void trace_put(nd_system *host);
void trace_drain(nd_system *host);
void attributes_of(napi_env environment, napi_value pairs, js_attributes *out);
void attributes_free(js_attributes *attributes);
void otlp_resource_set(napi_env environment, ta_otlp *otlp, napi_value pairs);
napi_value js_trace_configure(napi_env environment, napi_callback_info info);
napi_value js_trace_end(napi_env environment, napi_callback_info info);

/* metrics.c */
double named_double(napi_env environment, napi_value object, const char *name);
uint32_t named_length(napi_env environment, napi_value object, const char *name, napi_value *array);
napi_value js_metrics(napi_env environment, napi_callback_info info);
napi_value js_metrics_to_otlp(napi_env environment, napi_callback_info info);
napi_value js_decode_metrics(napi_env environment, napi_callback_info info);

/* datamodel.c, quickjs.c: the datamodels' vtables */
extern const ta_datamodel_vtable native_vtable, quickjs_vtable;

#endif
