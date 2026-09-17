#pragma once

#include "configuration.h"
#include "datamodel.h"
#include "history.h"

typedef enum {
    TA_STATE_ATOMIC,
    TA_STATE_COMPOUND,
    TA_STATE_PARALLEL,
    TA_STATE_FINAL,
    TA_STATE_HISTORY_SHALLOW,
    TA_STATE_HISTORY_DEEP
} ta_state_kind;

typedef enum {
    TA_TRANSITION_EXTERNAL,
    TA_TRANSITION_INTERNAL
} ta_transition_type;

typedef enum {
    TA_BINDING_EARLY,
    TA_BINDING_LATE
} ta_datamodel_binding_mode;

typedef enum {
    TA_EXPRESSION_NONE = 0,
    TA_EXPRESSION_SOURCE,
    TA_EXPRESSION_HOST
} ta_expression_kind;

typedef struct ta_expression_host_vtable {
    ta_result (*retain)(void *self, void *value, ta_allocator allocator, void **out);
    void (*release)(void *self, void *value, ta_allocator allocator);
} ta_expression_host_vtable;

typedef struct {
    void *self;
    const ta_expression_host_vtable *v;
    void *value;
} ta_expression_host;

/*
 * A frontend expression retained by a compiled definition. Source expressions
 * are copied by the definition compiler. Host expressions are retained and
 * released through their adapter; for example, a Node-API adapter retains a
 * napi_ref rather than a callback-scoped napi_value.
 */
typedef struct {
    ta_expression_kind kind;
    union {
        ta_str source;
        ta_expression_host host;
    } as;
} ta_expression;

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

typedef enum {
    TA_ACTION_RAISE,
    TA_ACTION_SEND,
    TA_ACTION_CANCEL,
    TA_ACTION_ASSIGN,
    TA_ACTION_SCRIPT,
    TA_ACTION_IF,
    TA_ACTION_FOREACH,
    TA_ACTION_INVOKE,
    TA_ACTION_EXTENSION
} ta_action_kind;

typedef struct {
    ta_event_name_id event;
    ta_value_source data;
} ta_raise_action;

typedef struct {
    ta_send_id id;
    ta_atom io_type;
    ta_event_source event;
    ta_value_source target;
    ta_expr_id delay_expr;
    ta_value_source data;
    ta_span32 params;
} ta_send_action;

typedef struct {
    ta_send_id id;
    ta_expr_id id_expr;
} ta_cancel_action;

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
        ta_if_action conditional;
        ta_foreach_action foreach;
        ta_invoke_decl_id invoke;
        ta_extension_id extension;
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
    ta_bytes bytes;
} ta_content_def;

typedef struct {
    ta_invoke_id id;
    ta_atom type;
    ta_value_source src;
    ta_value_source data;
    ta_span32 params;
    ta_span32 finalize_actions;
    bool autoforward;
} ta_invoke_decl;

typedef struct {
    ta_atom type;
    ta_bytes payload;
} ta_extension_def;

/*
 * Process-independent identity of one exact immutable definition revision.
 * `family` is the stable logical statechart family; `revision` is an opaque
 * fingerprint copied/owned by the compiled definition.  Any change that can
 * alter interpretation of a session image must produce a different revision.
 */
typedef struct {
    ta_definition_family family;
    ta_bytes revision;
} ta_definition_identity;

typedef struct {
    ta_atom name;
    ta_state_id parent;
    ta_state_kind kind;
    ta_span32 children;
    ta_span32 transitions;
    ta_initial_transition_id initial;
    ta_span32 onentry_actions;
    ta_span32 onexit_actions;
    ta_event_name_id done_event;
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
} ta_transition_def;

typedef struct {
    ta_state_id owner;
    ta_span32 targets;
    ta_span32 actions;
} ta_initial_transition_def;

struct ta_definition {
    ta_definition_id id;
    ta_atom name;
    ta_definition_identity identity;
    ta_configuration_layout configuration;
    ta_history_layout history;
    ta_state_def *states;
    uint32_t state_count;
    ta_transition_def *transitions;
    uint32_t transition_count;
    ta_initial_transition_def *initial_transitions;
    uint32_t initial_transition_count;
    ta_initial_transition_id root_initial;
    ta_event_pattern *event_patterns;
    uint32_t event_pattern_count;
    ta_event_pattern_segment *event_pattern_segments;
    uint32_t event_pattern_segment_count;
    ta_state_id *targets;
    uint32_t target_count;
    ta_action_id *actions;
    uint32_t action_count;
    ta_action_def *action_defs;
    ta_param_def *params;
    uint32_t param_count;
    ta_data_decl *data_decls;
    uint32_t data_decl_count;
    ta_initializer_def *initializers;
    uint32_t initializer_count;
    ta_content_def *contents;
    uint32_t content_count;
    ta_invoke_decl *invokes;
    uint32_t invoke_decl_count;
    ta_extension_def *extensions;
    uint32_t extension_count;
    ta_expression *expressions;
    uint32_t expression_count;
    ta_datamodel_binding_mode binding_mode;
    ta_datamodel datamodel;
    void *datamodel_shared;
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
};
