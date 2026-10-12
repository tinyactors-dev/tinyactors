/* tinyactors/datamodel.h — the synchronous semantic datamodel interface.
 *
 * The datamodel owns values and expression semantics. The core owns *when*
 * each operation happens and *what SCXML state* it may observe. This is the
 * only hot callback surface in the library (docs/architecture.md §9).
 *
 * Re-entrancy: implementations may call the read-only ta_datamodel_environment_* queries
 * below; they must never call ta_system_* functions.
 * Lifetimes: the datamodel's outputs — ta_string_view results (eval_string,
 * log) and ta_bytes views (to_wire, serialize, payload_serialize) — point
 * into memory the datamodel owns and are valid until the next call on the
 * same datamodel; core copies what it keeps before calling it again
 * (docs/adr/0010 in the repository root, decision 1).
 */
#ifndef TINYACTORS_DATAMODEL_H
#define TINYACTORS_DATAMODEL_H

#include <tinyactors/image.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only view of the executing instance, passed into datamodel calls. */
typedef struct ta_datamodel_environment ta_datamodel_environment;

TA_API int ta_datamodel_environment_in(const ta_datamodel_environment *environment,
                                       uint32_t state); /* SCXML In() */
TA_API void ta_datamodel_environment_charge(ta_datamodel_environment *environment,
                                            uint32_t fuel); /* report expensive work */
TA_API const ta_image *ta_datamodel_environment_image(const ta_datamodel_environment *environment);
TA_API uint32_t ta_datamodel_environment_chart(const ta_datamodel_environment *environment);

/* Borrowed view of an event for binding `_event`. The strings are valid only
 * during the bind_event call (copy what you need). `data` stays valid until
 * the next bind_event/detach_event/destroy; call retain to keep it longer.
 * bind_event receives an environment so the datamodel can observe the configuration
 * at the moment the event becomes current (e.g. which final state produced a
 * done.state.* event); fuel charged there is ignored. */
typedef struct ta_event_view {
    uint32_t size;
    uint32_t type; /* TA_EVENT_TYPE_* */
    uint32_t name_id;
    uint32_t pad;
    ta_string_view name;
    ta_string_view sendid;     /* length 0 if absent */
    ta_string_view origin;     /* e.g. "#_scxml_42" */
    ta_string_view origintype; /* e.g. "http://www.w3.org/TR/scxml/#SCXMLEventProcessor" */
    ta_string_view invokeid;
    ta_payload data;
    ta_string_view
        raw; /* I/O-processor specific raw message (e.g. BasicHTTP request), length 0 if none */
} ta_event_view;
enum { TA_EVENT_TYPE_PLATFORM = 0, TA_EVENT_TYPE_INTERNAL = 1, TA_EVENT_TYPE_EXTERNAL = 2 };

typedef struct ta_io_processor_entry {
    ta_string_view type;     /* URI */
    ta_string_view location; /* this session's address for that processor */
} ta_io_processor_entry;

/* System variables. Valid only during the create call (copy them); the
 * datamodel must expose them read-only. */
typedef struct ta_datamodel_system_variables {
    uint32_t size;
    uint32_t io_processor_count;
    ta_string_view sessionid;
    ta_string_view name;
    const ta_io_processor_entry *io_processors;
} ta_datamodel_system_variables;

/* What to capture into a payload: namelist locations, <param> name/expr
 * pairs, or a <content> expression. Arrays point straight into the image
 * (zero copy); resolve names with ta_image_string(). */
typedef struct ta_datamodel_capture {
    uint32_t kind; /* TA_CAPTURE_SEND / _INVOKE / _DONEDATA */
    uint32_t namelist_count;
    uint32_t param_count;
    ta_expression content;    /* TA_NIL if none */
    const uint32_t *namelist; /* namelist_count location expr ids */
    const uint32_t *params;   /* param_count pairs {name string id, expr id} */
} ta_datamodel_capture;
/* TA_CAPTURE_VALUE: the value of `content` alone (a location expression), for a
 * trace record (docs/adr/0006 in the repository root). It must have no effect
 * the chart can see; a failure only leaves the value out of the record. */
enum { TA_CAPTURE_SEND = 0, TA_CAPTURE_INVOKE = 1, TA_CAPTURE_DONEDATA = 2, TA_CAPTURE_VALUE = 3 };

typedef struct ta_datamodel_chart {
    uint32_t size;
    uint32_t chart;
    const ta_image *image;
    uint32_t binding; /* TA_BINDING_EARLY / TA_BINDING_LATE */
    uint32_t pad;
} ta_datamodel_chart;
enum { TA_BINDING_EARLY = 0, TA_BINDING_LATE = 1 };

enum {
    TA_DATAMODEL_EVENT_DETACHABLE = 1u
    << 0 /* detach_event() materializes _event; core may free it */
};

/* One call into a datamodel for one instance: what every per-instance entry
 * of the vtable takes first. `environment` is NULL for the entries that run
 * outside a step (destroy, detach_event, assign_string, foreach_item,
 * foreach_end, serialize, variable). */
typedef struct ta_datamodel_call {
    void *datamodel;
    uint64_t instance;
    ta_datamodel_environment *environment;
} ta_datamodel_call;

/* What a new instance starts from: create reads `init` (the invoke's or the
 * spawn's data, 0 for none), deserialize reads `state` (the bytes serialize
 * wrote). The system variables are supplied again on every start. */
typedef struct ta_datamodel_start {
    void *chart_context;
    const ta_datamodel_system_variables *system_variables;
    ta_payload init;
    ta_bytes state;
} ta_datamodel_start;

/* A <foreach>: foreach_begin sets `iter` and `n` (the number of items);
 * foreach_item binds item `i` to the locations `item` and `index` (index
 * TA_NIL: none); foreach_end ends `iter`. */
typedef struct ta_datamodel_loop {
    uint64_t iter;
    uint32_t n;
    uint32_t i;
    ta_expression item;
    ta_expression index;
} ta_datamodel_loop;

typedef struct ta_datamodel_vtable {
    uint32_t size;
    uint32_t flags;
    const char *name; /* matched against the chart's datamodel attribute */

    ta_status (*chart_attach)(void *datamodel, const ta_datamodel_chart *chart,
                              void **chart_context);
    void (*chart_detach)(void *datamodel, void *chart_context);
    ta_status (*create)(void *datamodel, const ta_datamodel_start *start, uint64_t *instance);
    void (*destroy)(const ta_datamodel_call *call);

    ta_status (*init_data)(const ta_datamodel_call *call, uint32_t data_declaration);
    ta_status (*bind_event)(const ta_datamodel_call *call, const ta_event_view *event);
    void (*detach_event)(const ta_datamodel_call *call);

    ta_status (*eval_condition)(const ta_datamodel_call *call, ta_expression e, int *out);
    ta_status (*eval_string)(const ta_datamodel_call *call, ta_expression e, ta_string_view *out);
    ta_status (*assign)(const ta_datamodel_call *call, ta_expression loc, ta_expression value);
    ta_status (*assign_string)(const ta_datamodel_call *call, ta_expression loc,
                               ta_string_view value);
    ta_status (*script)(const ta_datamodel_call *call, ta_expression e);
    ta_status (*log)(const ta_datamodel_call *call, ta_expression e, ta_string_view *out);

    ta_status (*foreach_begin)(const ta_datamodel_call *call, ta_expression array,
                               ta_datamodel_loop *loop);
    ta_status (*foreach_item)(const ta_datamodel_call *call, const ta_datamodel_loop *loop);
    void (*foreach_end)(const ta_datamodel_call *call, const ta_datamodel_loop *loop);

    ta_status (*capture)(const ta_datamodel_call *call, const ta_datamodel_capture *spec,
                         ta_payload *out);
    void (*retain)(void *datamodel, ta_payload p);
    void (*release)(void *datamodel, ta_payload p);

    ta_status (*custom)(const ta_datamodel_call *call, uint32_t custom_def);

    /* Hibernation (optional; present when `size` covers them and non-NULL).
     * serialize: the instance's state — its variables and the bound _event,
     * NOT the system variables — as bytes in `out`, a view of memory the
     * datamodel owns, valid until the next call on the datamodel (core copies
     * it before calling destroy); TA_ERROR_UNSUPPORTED if the state cannot be
     * serialized (e.g. functions in the data). The core calls destroy right
     * after a successful serialize.
     * deserialize: create an instance from start->state; the system variables
     * are supplied again (same _sessionid/_name/_ioprocessors). Called only
     * at a stable point with the same chart context. */
    ta_status (*serialize)(const ta_datamodel_call *call, ta_bytes *out);
    ta_status (*deserialize)(void *datamodel, const ta_datamodel_start *start, uint64_t *instance);

    /* Live migration (optional): called by ta_system_adopt after deserialize
     * when the state was written for an older version of the chart (the
     * instance now belongs to the new chart). Return the previous <datamodel>
     * bindings as one payload (e.g. an object of id -> value; 0 for none) and
     * drop variables the new chart does not declare. The core then
     * initializes the new chart's added <data> and raises
     * error.platform.upgrade with that payload as its data. */
    ta_status (*migrate)(const ta_datamodel_call *call, ta_payload *bindings);

    /* Neutral values (optional; docs/protocol.md, "Values"): event data
     * between systems, and in traces, is one CBOR item (RFC 8949): integers,
     * floats, text, bytes, arrays, maps with text keys, true, false, null,
     * undefined, and an actor reference as tag 32 on its address.
     * to_wire: payload p as CBOR in `out`, a view of memory the datamodel
     * owns, valid until the next call on the datamodel; TA_ERROR_INVALID if p
     * cannot be a neutral value (a cycle, nested too deep). Core calls it
     * once per value it writes.
     * from_wire: a new payload (one reference) from CBOR; TA_ERROR_INVALID if
     * it is not one well-formed neutral value. Transports between systems
     * use them; within a system payloads never leave the datamodel. */
    ta_status (*to_wire)(void *datamodel, ta_payload p, ta_bytes *out);
    ta_status (*from_wire)(void *datamodel, ta_bytes cbor, ta_payload *out);

    /* Payloads in blobs (optional; docs/durability.md). ta_actor_export
     * writes the payload of every pending delayed send with
     * payload_serialize, and ta_system_adopt creates it again with
     * payload_deserialize (one reference), so an export blob is a value that
     * any datamodel host can adopt: another process, after a restart.
     * Conventions as for serialize (a view of the datamodel's memory, valid
     * until its next call; TA_ERROR_UNSUPPORTED if the value cannot be
     * written).
     * Without them, delayed-send payloads travel as handles: such a blob is
     * valid only in the process (datamodel host) that wrote it. */
    ta_status (*payload_serialize)(void *datamodel, ta_payload p, ta_bytes *out);
    ta_status (*payload_deserialize)(void *datamodel, ta_bytes blob, ta_payload *out);

    /* A variable for inspection (optional; the actor.context request,
     * docs/requests.md): the value of the location `name` (a <data> id) in
     * `instance`, as a new payload (one reference). It must have no effect the
     * chart can see. TA_ERROR_INVALID when the instance has no such value. */
    ta_status (*variable)(const ta_datamodel_call *call, ta_string_view name, ta_payload *out);
} ta_datamodel_vtable;

/* Binding of a datamodel implementation to a system. */
typedef struct ta_datamodel_binding {
    const ta_datamodel_vtable *vtable;
    void *datamodel;
} ta_datamodel_binding;

/* The SCXML "null" datamodel (SCXML Appendix B.1), built into ta_core. */
TA_API const ta_datamodel_vtable *ta_datamodel_null(void);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_DATAMODEL_H */
