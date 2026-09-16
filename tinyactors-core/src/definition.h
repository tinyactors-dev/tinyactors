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
    ta_span32 actions;
    uint32_t document_order;
} ta_transition_def;

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
    ta_event_pattern *event_patterns;
    uint32_t event_pattern_count;
    ta_event_pattern_segment *event_pattern_segments;
    uint32_t event_pattern_segment_count;
    ta_state_id *targets;
    uint32_t target_count;
    ta_action_id *actions;
    uint32_t action_count;
    ta_datamodel datamodel;
    void *datamodel_shared;
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
};
