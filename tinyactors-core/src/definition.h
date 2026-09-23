#pragma once

#include "configuration.h"
#include "datamodel.h"
#include "history.h"

/* Functions over compiled definitions, shared by the compiler and interpreter. */

typedef enum {
    TA_TRANSITION_EXTERNAL,
    TA_TRANSITION_INTERNAL
} ta_transition_type;

typedef enum {
    TA_BINDING_EARLY,
    TA_BINDING_LATE
} ta_datamodel_binding_mode;

typedef enum {
    TA_VALUE_SOURCE_NONE,
    TA_VALUE_SOURCE_EXPR,
    TA_VALUE_SOURCE_LOCATION,
    TA_VALUE_SOURCE_CONTENT
} ta_value_source_kind;

typedef struct {
    ta_value_source_kind kind;
    union {
        ta_expr_id expr;
        ta_expr_id location;
        ta_content_id content;
    } as;
} ta_value_source;

typedef enum {
    TA_EVENT_SOURCE_NAME,
    TA_EVENT_SOURCE_EXPR
} ta_event_source_kind;

typedef struct {
    ta_event_source_kind kind;
    union {
        ta_event_name_id name;
        ta_expr_id expr;
    } as;
} ta_event_source;

/*
 * A compiled `x` / `xexpr` attribute pair. ATOM holds interned literal text;
 * NONE is used only where the attribute is optional.
 */
typedef enum {
    TA_TEXT_SOURCE_NONE,
    TA_TEXT_SOURCE_ATOM,
    TA_TEXT_SOURCE_EXPR
} ta_text_source_kind;

typedef struct {
    ta_text_source_kind kind;
    union {
        ta_atom atom;
        ta_expr_id expr;
    } as;
} ta_text_source;

/*
 * A compiled <send>/<invoke> `id` / `idlocation`. LITERAL holds the author's
 * interned ID. GENERATED and LOCATION make the runtime generate an ID on each
 * execution; LOCATION also assigns it to the location.
 */
typedef enum {
    TA_ID_SOURCE_GENERATED,
    TA_ID_SOURCE_LITERAL,
    TA_ID_SOURCE_LOCATION
} ta_id_source_kind;

typedef struct {
    ta_id_source_kind kind;
    union {
        ta_atom literal;
        ta_expr_id location;
    } as;
} ta_id_source;

/*
 * Index into ta_definition.authored_texts: authored text the compiler
 * normalizes and the definition keeps for ta_definition_export().
 */
typedef uint32_t ta_authored_text_id;
#define TA_AUTHORED_TEXT_NONE UINT32_MAX

/*
 * LITERAL is a duration parsed at compile time, in ta_time milliseconds;
 * `text` is the authored duration (e.g. "2.5s"), which the parse discards.
 */
typedef enum {
    TA_DELAY_SOURCE_NONE,
    TA_DELAY_SOURCE_LITERAL,
    TA_DELAY_SOURCE_EXPR
} ta_delay_source_kind;

typedef struct {
    ta_delay_source_kind kind;
    ta_authored_text_id text;
    union {
        ta_time literal;
        ta_expr_id expr;
    } as;
} ta_delay_source;

typedef enum {
    TA_ACTION_RAISE,
    TA_ACTION_SEND,
    TA_ACTION_CANCEL,
    TA_ACTION_ASSIGN,
    TA_ACTION_SCRIPT,
    TA_ACTION_LOG,
    TA_ACTION_IF,
    TA_ACTION_FOREACH,
    TA_ACTION_INVOKE
} ta_action_kind;

typedef struct {
    ta_event_name_id event;
    ta_value_source data;
} ta_raise_action;

/*
 * A NONE type selects the built-in SCXML Event I/O Processor. A literal
 * target is TA_VALUE_SOURCE_CONTENT.
 */
typedef struct {
    ta_id_source id;
    ta_text_source type;
    ta_event_source event;
    ta_value_source target;
    ta_delay_source delay;
    ta_value_source data;
    ta_span32 params;
} ta_send_action;

/* Never NONE: SCXML requires exactly one of sendid or sendidexpr. */
typedef struct {
    ta_text_source sendid;
} ta_cancel_action;

typedef struct {
    ta_atom label;
    bool has_label;
    ta_expr_id expr;
    bool has_expr;
} ta_log_action;

typedef struct {
    ta_expr_id location;
    ta_value_source value;
} ta_assign_action;

typedef struct {
    ta_expr_id condition;
    ta_span32 then_actions;
    ta_span32 else_actions;
} ta_if_action;

typedef struct {
    ta_expr_id array;
    ta_atom item;
    ta_atom index;
    bool has_index;
    ta_span32 body_actions;
} ta_foreach_action;

typedef struct {
    ta_action_kind kind;
    union {
        ta_raise_action raise;
        ta_send_action send;
        ta_cancel_action cancel;
        ta_assign_action assign;
        ta_expr_id script;
        ta_log_action log;
        ta_if_action conditional;
        ta_foreach_action foreach;
        ta_invoke_decl_id invoke;
    } as;
} ta_action_def;

typedef struct {
    ta_atom name;
    ta_value_source value;
} ta_param_def;

typedef struct {
    ta_atom name;
    ta_state_id owner_state;
    ta_initializer_id initializer;
} ta_data_decl;

typedef struct {
    ta_value_source value;
} ta_initializer_def;

typedef struct {
    ta_value_source content;
    ta_span32 params;
} ta_done_data_def;

/*
 * Literal content, and the text of a literal <send target> or <invoke src>.
 * `media_type` is what the front end said the bytes are (ADR 0053); it is
 * empty for literal text, and hashed into the revision either way.
 */
typedef struct {
    ta_str media_type;
    ta_bytes bytes;
} ta_content_def;

#define TA_CONTENT_NONE ((ta_content_id)UINT32_MAX)

/* A literal src is TA_VALUE_SOURCE_CONTENT. */
typedef struct {
    ta_id_source id;
    ta_text_source type;
    ta_value_source src;
    ta_value_source data;
    ta_span32 params;
    ta_span32 finalize_actions;
    bool autoforward;
    /* The state whose entry starts this invocation. */
    ta_state_id owner;
} ta_invoke_decl;

typedef struct {
    ta_atom type;
    ta_bytes payload;
} ta_extension_def;

typedef struct {
    ta_atom name;
    ta_state_id parent;
    ta_state_kind kind;
    ta_span32 children;
    ta_span32 transitions;
    ta_initial_transition_id initial;
    /* Ranges of definition.handlers: one action block per <onentry>/<onexit>. */
    ta_span32 onentry_handlers;
    ta_span32 onexit_handlers;
    ta_event_name_id done_event;
    ta_done_data_id done_data;
    /* Index into history.slots for history states, else UINT32_MAX. */
    uint32_t history_slot;
} ta_state_def;

typedef struct {
    ta_span32 segments;
} ta_event_pattern;

typedef struct {
    ta_atom atom;
    uint32_t flags;
} ta_event_pattern_segment;

enum {
    TA_EVENT_PATTERN_SEGMENT_WILDCARD = 1u << 0
};

typedef struct {
    const ta_event_pattern_segment *segments;
    uint32_t segment_count;
} ta_event_pattern_spec;

typedef struct {
    ta_state_id source;
    ta_span32 targets;
    ta_span32 event_patterns;
    ta_expr_id condition;
    ta_transition_type type;
    ta_span32 actions;
    uint32_t document_order;
    /*
     * The authored event descriptor text, which event_patterns normalize
     * (whitespace, ".*" forms); TA_AUTHORED_TEXT_NONE when absent.
     */
    ta_authored_text_id event_text;
} ta_transition_def;

typedef struct {
    ta_state_id owner;
    ta_span32 targets;
    ta_span32 actions;
    /* False when `targets` is the SCXML default (the first child), not authored. */
    bool authored_targets;
} ta_initial_transition_def;

struct ta_definition {
    ta_allocator allocator;
    /* Owns every array and string below. */
    ta_arena arena;
    /* The system whose atoms and event names this definition uses. */
    ta_system *system;
    bool registered;
    /* Intrusive list of compiled definitions not yet registered. */
    ta_definition *unregistered_previous;
    ta_definition *unregistered_next;
    /* ta_state_def.children ranges index this array. */
    ta_state_id *children;
    uint32_t child_count;
    ta_definition_id id;
    ta_atom name;
    ta_definition_identity identity;
    /* The <scxml name> attribute, exposed as _name; TA_ATOM_NONE when absent. */
    ta_atom document_name;
    ta_configuration_layout configuration;
    ta_history_layout history;
    ta_state_def *states;
    uint32_t state_count;
    /* The deepest nesting of states: 1 for a chart of root states only.
     * Sizes the interpreter's entry-set frames (ADR 0060). */
    uint32_t max_state_depth;
    ta_transition_def *transitions;
    uint32_t transition_count;
    ta_initial_transition_def *initial_transitions;
    uint32_t initial_transition_count;
    ta_initial_transition_id root_initial;
    /*
     * The <scxml> <script>: a block of zero or one TA_ACTION_SCRIPT in the
     * action stream, run once per session before the root initial transition.
     * A block, not a bare ta_expr_id, so it shares executable-content error
     * semantics (SCXML 4.9) and continuation frames with every other block.
     */
    ta_span32 global_script_actions;
    ta_event_pattern *event_patterns;
    uint32_t event_pattern_count;
    ta_event_pattern_segment *event_pattern_segments;
    uint32_t event_pattern_segment_count;
    ta_state_id *targets;
    uint32_t target_count;
    /* Action blocks are ta_span32 ranges of action_defs. */
    ta_action_def *action_defs;
    uint32_t action_count;
    /*
     * The action block of each <onentry>/<onexit> handler. SCXML runs every
     * handler as a separate block (3.8, 3.9), so the handlers of a state are
     * not merged into one block.
     */
    ta_span32 *handlers;
    uint32_t handler_count;
    ta_param_def *params;
    uint32_t param_count;
    ta_data_decl *data_decls;
    uint32_t data_decl_count;
    ta_initializer_def *initializers;
    uint32_t initializer_count;
    ta_content_def *contents;
    uint32_t content_count;
    ta_done_data_def *done_data;
    uint32_t done_data_count;
    ta_invoke_decl *invokes;
    uint32_t invoke_decl_count;
    ta_extension_def *extensions;
    uint32_t extension_count;
    ta_expression *expressions;
    uint32_t expression_count;
    /*
     * Authored input the compiler normalizes, kept so ta_definition_export()
     * rebuilds a spec with the same fingerprint (ADR 0042).
     */
    ta_str *authored_texts;
    uint32_t authored_text_count;
    /* The authored <scxml binding>; DEFAULT and EARLY both compile to early. */
    ta_scxml_binding_kind authored_binding;
    /* The authored <scxml datamodel>; empty when absent. */
    ta_str authored_datamodel;
    /* True when `identity.revision` is the compiler's fingerprint, not an explicit identity. */
    bool fingerprinted;
    ta_datamodel_binding_mode binding_mode;
    ta_datamodel datamodel;
    void *datamodel_shared;
    bool datamodel_prepared;
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
    /* Resident actors whose `definition` points here; see ta_actor_free(). */
    uint32_t actor_count;
    /*
     * Set by ta_system_retire_definition(): no new actors, and the system
     * frees the definition once actor_count reaches zero (ADR 0043).
     */
    bool retired;
    /* Registration order; see ta_system_find_definition(). */
    uint64_t registration_serial;
};

/* A registered definition, retired or not; NULL for an unknown or stale ID. */
const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id);
/* Bytes of one actor's session storage: two configurations and history. */
size_t ta_definition_session_bytes(const ta_definition *definition);
/* Bytes allocated for one actor's data-model instance (never zero). */
size_t ta_definition_instance_bytes(const ta_definition *definition);
bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state);
bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor);
bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event);
