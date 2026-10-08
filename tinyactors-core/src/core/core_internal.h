/* core_internal.h — kernel internals: event records, instance and workspace
 * layouts, phases, the step contract used by ta_system. */
#ifndef TINYACTORS_CORE_CORE_INTERNAL_H
#define TINYACTORS_CORE_CORE_INTERNAL_H

#include <tinyactors/core.h>
#include <tinyactors/system.h>
#include <tinyactors/trace.h>
#include "base/bitset.h"
#include "base/journal.h"
#include "image/records.h"
#include "base/pool.h"
#include "base/slab.h"

/* ---- tagged string references ------------------------------------------------
 * < 0x80000000: image string id; TA_STRING_REF_SLAB|(offset>>3): refcounted slab string;
 * TA_NIL: none. Slab string layout: {u32 refs; u32 length; char data[length+1]}. */
#define TA_STRING_REF_SLAB       0x80000000u
#define TA_STRING_REF_IS_SLAB(r) ((r) != TA_NIL && ((r) & TA_STRING_REF_SLAB))

/* ---- events ------------------------------------------------------------------ */
enum {
    TA_EVENT_FLAG_FROM_CHILD = 1u << 0,
    TA_EVENT_FLAG_FROM_EXTERNAL_INVOKE = 1u << 1,
    TA_EVENT_FLAG_DONE = 1u << 2,
    TA_EVENT_FLAG_RAISE_ERROR_COMMUNICATION = 1u
        << 3, /* parked for a hibernated actor: raise error.communication */
    TA_EVENT_FLAG_BLOB_PAYLOAD = 1u
        << 4 /* adoption in progress: the payload handle is the blob's */
};

typedef struct ta_event {
    uint32_t next; /* intrusive link: mailbox / internal queue / timer / free list */
    uint16_t refs;
    uint8_t type;      /* TA_EVENT_TYPE_* */
    uint8_t datamodel; /* datamodel binding owning `payload`, 0xFF = none */
    uint32_t name_id;
    uint32_t name;          /* string reference */
    uint32_t sendid;        /* string reference or NIL */
    uint32_t origin;        /* sender actor handle, 0 = none */
    uint32_t origin_string; /* string reference (external origin) or NIL */
    uint32_t origintype;    /* string reference or NIL (NIL + origin != 0 => SCXML processor) */
    uint32_t invokeid;      /* string reference or NIL */
    uint16_t invoke_slot;   /* receiver-side invoke slot or TA_EVENT_NO_SLOT */
    uint16_t flags;         /* TA_EVENT_FLAG_* */
    uint32_t raw;           /* string reference (I/O-processor raw message) or NIL */
    uint32_t trace; /* the context of the step that caused it (context pool), TA_NIL if untraced */
    uint64_t payload;
} ta_event;
/* The trace reference took its four bytes from invoke_slot and flags: an event
 * costs nothing more with tracing (docs/adr/0006). */
TA_STATIC_ASSERT(sizeof(ta_event) == 56, event_record_is_56_bytes);
#define TA_EVENT_NO_SLOT 0xFFFFu
TA_STATIC_ASSERT(TA_CHART_MAX_INVOKES < TA_EVENT_NO_SLOT, invoke_slots_fit_events);

/* ---- trace contexts -----------------------------------------------------------------
 * A pool of reference-counted contexts in the system block (docs/adr/0006,
 * section 5): the trace, the W3C flags and the step that caused what holds
 * it. The parent span is that step's span (ta_step_key_span_id of the cause;
 * a host's cause has session 0 and its span ID in `macrostep`). Events, timers
 * and an instance's current macrostep hold references. */
typedef struct ta_trace_entry {
    ta_trace_id trace;
    uint64_t session, macrostep; /* the cause */
    uint32_t microstep;
    uint32_t flags;
    uint32_t refs;
    uint32_t pad;
} ta_trace_entry;
TA_STATIC_ASSERT(sizeof(ta_trace_entry) == 48, trace_entry_is_48_bytes);

/* ---- step results (public ones from tinyactors/core.h, plus internal) ---------------- */
enum {
    TA_STEP_LOG = 10,         /* out.label, out.text */
    TA_STEP_AUTOFORWARD = 11, /* out.slot, out.event */
    TA_STEP_EXHAUSTED = 12    /* event pool / slab exhausted; retry after growth */
};

enum {
    TA_ROUTE_SELF = 0,
    TA_ROUTE_PARENT = 1,
    TA_ROUTE_CHILD = 2,   /* arg = child actor handle */
    TA_ROUTE_SESSION = 3, /* target = "#_scxml_<id>" string reference */
    TA_ROUTE_EXTERNAL = 4 /* arg = io_processor index, target = string reference */
};

typedef struct ta_core_out {
    uint32_t event; /* send / autoforward: event pool index (one reference owned by caller) */
    uint32_t route, arg, target;
    uint32_t sendid; /* cancel: string reference (borrowed) */
    uint32_t slot, chart, source, type, invokeid, autoforward, child;
    uint32_t label;      /* log label string reference */
    ta_string_view text; /* log text (borrowed from datamodel; copy immediately) */
    uint64_t delay_ns;
    uint64_t payload;     /* invoke params / done data (ownership passes to caller) */
    uint32_t final_state; /* done: top-level final state that ended the session, or TA_NIL */
    uint32_t fault;
} ta_core_out;

/* ---- kernel context ---------------------------------------------------------- */
struct ta_core {
    ta_image_view image_view;
    const ta_image *image;
    ta_pool *events;
    const ta_pool *timers; /* a delayed send needs a free slot here (NULL: unlimited) */
    ta_pool *workspaces;
    ta_slab *slab;
    const ta_datamodel_binding *datamodels;
    uint32_t datamodel_count;
    uint32_t reserve;               /* event slots reserved for platform error events */
    const uint8_t *chart_datamodel; /* per chart: datamodel binding index */
    void *const *chart_context;     /* per chart: datamodel chart context */
    const ta_io_processor_descriptor *io_processors;
    uint32_t io_processor_count;
    uint32_t dropped_errors;
    /* Fuel overrun: when fuel runs out inside a microstep, finish it on up to
     * `overrun_max` units of debt and yield at the boundary (workspace
     * released). `overrun` reports the debt incurred by the last step. */
    uint32_t overrun_max;
    uint32_t overrun;
    uint64_t (*serial_of)(void *system, uint32_t actor);
    void *system;
    /* tracing (docs/adr/0006): the kinds to write (0: none), where, the
     * context pool, and what stamps a record: the fuel clock at the start of
     * the current step (the system keeps it), the fuel the step started with,
     * and the logical time */
    ta_journal *journal;
    ta_pool *contexts;
    uint32_t trace_kinds;
    uint32_t trace_step_fuel;
    uint64_t trace_clock;
    uint64_t trace_now;
    uint64_t contexts_dropped; /* events left untraced because the pool was full */
    ta_core_out out;
    char scratch[1024]; /* formatting: origins, generated ids, done.invoke names, document keys */
};

/* ---- instance ---------------------------------------------------------------- */
enum {
    PHASE_INIT = 0,
    PHASE_INIT_SCRIPT,
    PHASE_INIT_ENTER,
    PHASE_EVENTLESS,
    PHASE_INTERNAL,
    PHASE_INVOKE,
    PHASE_STABLE,
    PHASE_EXTERNAL,
    PHASE_MICROSTEP,
    PHASE_EXIT,
    PHASE_DONE
};
enum {
    INSTANCE_FLAG_FINAL = 1u << 0,   /* top-level final entered: stop after this microstep */
    INSTANCE_FLAG_CANCEL = 1u << 1,  /* exit at the next stable point */
    INSTANCE_FLAG_DETACHED = 1u << 2 /* datamodel detached _event (TA_DATAMODEL_EVENT_DETACHABLE) */
};

struct ta_instance {
    uint32_t chart;
    uint8_t phase, flags;
    uint16_t subphase;
    uint32_t cursor, cursor2;
    uint32_t current_event; /* _event pin (event pool index) or TA_NIL */
    uint32_t internal_queue_head, internal_queue_tail;
    uint32_t workspace; /* workspace pool index or TA_NIL */
    uint32_t actor;     /* owning actor handle (origin of sends) */
    uint32_t ids;       /* ids generated so far (idlocation sendids, invokeids): session state, so
                           the ids a session generates depend on nothing else (docs/durability.md) */
    uint64_t datamodel;
    /* the step counters (docs/adr/0006): macrosteps count from 1 per session,
     * microsteps from 1 within their macrostep; session state like `ids` */
    uint64_t macrostep;
    uint32_t microstep;
    uint32_t trace; /* the current macrostep's context, TA_NIL outside one or untraced */
};
TA_STATIC_ASSERT(sizeof(struct ta_instance) == 64, instance_header_is_64_bytes);

typedef struct ta_instance_layout {
    uint32_t state_words; /* words per state bitset */
    uint32_t config, to_invoke, data_init, history_recorded, history, invoke_children, invoke_ids,
        size;
} ta_instance_layout;

TA_INLINE ta_instance_layout ta_instance_layout_of(const ta_chart_record *chart) {
    ta_instance_layout l;
    l.state_words = (chart->state_count + 31) / 32;
    uint32_t offset = (uint32_t)sizeof(struct ta_instance) / 4; /* in words */
    l.config = offset;
    offset += l.state_words;
    l.to_invoke = offset;
    offset += l.state_words;
    l.data_init = offset; /* only late binding needs per-state data flags */
    offset += chart->binding == TA_BINDING_LATE ? l.state_words : 0;
    l.history_recorded = offset; /* only charts with history need "recorded" flags */
    offset += chart->history_word_count ? l.state_words : 0;
    l.history = offset;
    offset += chart->history_word_count;
    l.invoke_children = offset;
    offset += chart->invoke_count;
    l.invoke_ids = offset;
    offset += chart->invoke_count;
    l.size = offset * 4;
    return l;
}
TA_INLINE uint32_t *ta_instance_word(ta_instance *instance, uint32_t word) {
    return (uint32_t *)(void *)instance + word;
}

/* ---- workspace (held only inside a microstep or while running code) ---------- */
typedef struct ta_foreach_frame {
    uint64_t iteration;
    uint32_t i, count, body, item, index, pad;
} ta_foreach_frame;

typedef struct ta_workspace {
    uint8_t subphase, vm_active, foreach_top, pad;
    uint32_t instruction, block_end;
    uint32_t cursor, cursor2;
    uint32_t selected_count;
    uint32_t stack_top;
    uint32_t auxiliary;
    uint32_t final_state; /* exitInterpreter: the top-level final that ended the session */
    uint32_t trace;       /* the context the step's events share (one reference), TA_NIL */
    uint64_t done_payload; /* exitInterpreter: <donedata> of the top-level final */
    uint64_t trace_fuel;   /* the fuel clock when the microstep began */
} ta_workspace;

typedef struct ta_workspace_layout {
    uint32_t state_words, transition_words, selected, domains, transitions_seen, exit, entry,
        default_entry, history_default_entry, stack, stack_capacity, foreach_frames, size;
} ta_workspace_layout;

TA_INLINE ta_workspace_layout ta_workspace_layout_of(const ta_chart_record *chart) {
    ta_workspace_layout l;
    l.state_words = (chart->state_count + 31) / 32;
    l.transition_words = (chart->transition_count + 31) / 32;
    uint32_t offset = (uint32_t)sizeof(ta_workspace) / 4;
    l.selected = offset;
    offset += chart->state_count;
    l.domains = offset;
    offset += chart->state_count;
    l.transitions_seen = offset;
    offset += l.transition_words;
    l.exit = offset;
    offset += l.state_words;
    l.entry = offset;
    offset += l.state_words;
    l.default_entry = offset;
    offset += l.state_words;
    l.history_default_entry = offset;
    offset += l.state_words;
    l.stack = offset;
    l.stack_capacity = 4 * chart->state_count + 16;
    offset += l.stack_capacity;
    offset = (offset + 1) & ~1u; /* 8-align frames */
    l.foreach_frames = offset;
    offset +=
        (uint32_t)(sizeof(ta_foreach_frame) / 4) * (chart->max_foreach ? chart->max_foreach : 1);
    l.size = offset * 4;
    return l;
}

/* ---- internal API used by ta_system ------------------------------------------- */
uint32_t ta_core_chart_instance_size(const ta_core *core, uint32_t chart);
uint32_t ta_core_chart_workspace_size(const ta_core *core, uint32_t chart);
ta_status ta_core_instance_init(ta_core *core, ta_instance *in, uint32_t chart, uint32_t actor,
                                const ta_datamodel_system_variables *sv, ta_payload init);
void ta_core_instance_deinit(ta_core *core, ta_instance *in);
void ta_core_cancel(ta_core *core, ta_instance *in);
void ta_core_release_out(ta_core *core); /* release strings owned by core->out */
void ta_core_set_invoke_child(ta_core *core, ta_instance *in, uint32_t slot, uint32_t child);
void ta_core_clear_invoke_child(ta_core *core, ta_instance *in, uint32_t slot);
uint32_t ta_core_invoke_child(ta_core *core, ta_instance *in, uint32_t slot);
uint32_t ta_core_invoke_id(ta_core *core, ta_instance *in, uint32_t slot);
/* Raise a platform event (e.g. error.communication) into the internal queue;
 * returns it (TA_NIL if there was no room). It gets the context of the
 * instance's current step. */
uint32_t ta_core_raise_platform(ta_core *core, ta_instance *in, const char *name, uint32_t sendid);
/* ... with a payload (owned by the instance's datamodel; released on failure). */
ta_status ta_core_raise_platform_data(ta_core *core, ta_instance *in, const char *name,
                                      ta_payload data);
/* Initialize one <data> declaration now (error.execution on failure). */
void ta_core_init_declaration(ta_core *core, ta_instance *in, uint32_t declaration);
/* The datamodel's migrate hook (bindings = 0 if it has none). */
ta_status ta_core_datamodel_migrate(ta_core *core, ta_instance *in, ta_payload *bindings);
int ta_core_in_state(const ta_core *core, ta_instance *in, uint32_t state);

/* tracing (core/trace_step.c) */
/* A new context (one reference), TA_NIL when tracing is off or the pool is
 * full (counted in contexts_dropped). */
uint32_t ta_trace_context_new(ta_core *core, ta_trace_id trace, uint32_t flags, ta_step_key cause);
void ta_trace_context_retain(ta_core *core, uint32_t context);
void ta_trace_context_release(ta_core *core, uint32_t context);
TA_INLINE ta_trace_entry *ta_trace_entry_at(const ta_core *core, uint32_t context) {
    return (ta_trace_entry *)ta_pool_at(core->contexts, context);
}
/* The value of a context: its trace, flags, and the cause's span as parent. */
ta_trace_context ta_trace_context_value(const ta_core *core, uint32_t context);
/* The context the events a step of `instance` creates get (one reference
 * for the caller), TA_NIL if its macrostep is untraced. */
uint32_t ta_core_step_context(ta_core *core, ta_instance *instance);
/* The step's context as a value (for effects); no trace if untraced. */
ta_trace_context ta_core_step_trace(const ta_core *core, const ta_instance *instance);
/* The fuel clock now: inside a step, `fuel` is the step's fuel; outside
 * one, NULL. */
uint64_t ta_core_trace_fuel(const ta_core *core, const uint32_t *fuel);
/* A record of `kind` about the current step of `instance` (`microstep` 0:
 * about its macrostep), its header filled, or NULL when the kind is off,
 * the macrostep untraced or the journal full. Commit it with
 * ta_trace_record_commit. */
ta_trace_record *ta_core_trace_step(ta_core *core, ta_instance *instance, uint32_t kind,
                                    uint32_t size, uint32_t microstep, const uint32_t *fuel);
/* Release what a record holds (strings, payloads), after it was read. */
void ta_core_trace_release(ta_core *core, const ta_trace_record *record);
TA_INLINE int ta_core_traces(const ta_core *core, uint32_t kind) {
    return (core->trace_kinds & TA_TRACE_BIT(kind)) != 0;
}

/* events */
uint32_t ta_event_new(ta_core *core, int reserve_ok);
void ta_event_retain(ta_core *core, uint32_t event);
void ta_event_release(ta_core *core, uint32_t event);
TA_INLINE ta_event *ta_event_at(const ta_core *core, uint32_t event) {
    return (ta_event *)ta_pool_at(core->events, event);
}
/* Set name from a string (trie lookup; slab copy if not an exact node). */
ta_status ta_event_set_name(ta_core *core, ta_event *e, const char *name, uint32_t length);
void ta_event_view_of(ta_core *core, uint32_t event, ta_event_view *out);

/* strings */
uint32_t ta_shared_string_new(ta_core *core, const char *p, uint32_t length);
void ta_shared_string_retain(ta_core *core, uint32_t string_ref);
void ta_shared_string_release(ta_core *core, uint32_t string_ref);
ta_string_view ta_shared_string_get(const ta_core *core, uint32_t string_ref);
int ta_shared_string_equal(const ta_core *core, uint32_t a, ta_string_view b);
int ta_core_parse_duration(ta_string_view s, uint64_t *ns);

/* datamodel helpers */
TA_INLINE const ta_datamodel_binding *ta_core_datamodel(const ta_core *core, uint32_t chart) {
    return &core->datamodels[core->chart_datamodel[chart]];
}

#define TA_SCXML_IO_PROCESSOR "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

#endif /* TINYACTORS_CORE_CORE_INTERNAL_H */
