/**
 * @file definition.h
 *
 * # Compiled definitions
 *
 * `ta_definition` is one compiled, immutable statechart revision: the
 * program actors of that revision run. The compiler (`builder.h`) makes
 * it from a spec; the system registers it; the interpreter, invocations,
 * session images and the inspection and export calls read it. This
 * header holds its layout and the functions over it that the compiler
 * and the interpreter share.
 *
 * A definition holds:
 *
 * - the state hierarchy and kinds, in document order, with each state's
 *   children, transitions, `<onentry>` / `<onexit>` handlers (one action
 *   block per handler, SCXML 3.8, 3.9), completion event and done data;
 * - transitions with their sources, targets, type (`internal` or
 *   `external`), conditions, event descriptors and actions, and the
 *   initial transitions with their executable content;
 * - executable content as one stream of actions in blocks, and where each
 *   action sits (ADR 0071); invocation declarations; late or early data
 *   binding; the history and configuration layouts;
 * - the data model and the state it prepared for the definition, and the
 *   expressions and their roles (ADR 0076);
 * - the authored text the compiler normalizes, so that
 *   `ta_definition_export` rebuilds a spec with the same fingerprint
 *   (ADR 0042), and, when the spec knew them, where its elements were
 *   written (ADR 0085), in a side table that nothing at run time reads and
 *   that is not part of the revision.
 *
 * ## Contract and invariants
 *
 * - **Immutable program data.** A definition is compiled and validated
 *   before registration and never changed while actors or session images
 *   use it. The fields that change after registration are bookkeeping, not
 *   program: the counters and state occupancy (ADR 0072), the microstep
 *   limit (ADR 0075), `actor_count`, the holds and holders (ADR 0077),
 *   `retired` (ADR 0043), the nested definitions of its `<invoke>`s, and
 *   the spans a registration by identity hands over.
 * - **Identity.** Each definition owns its `ta_definition_identity`: its
 *   UTF-8 family and an opaque revision, the compiler's fingerprint unless
 *   the spec gave one (`identity` of `ta_scxml_spec`); the definition
 *   keeps its own copy of the revision bytes. A change that could alter how definition-local
 *   session state is read makes a new revision. Several revisions of one
 *   family may be registered at once; a session image reconstructs against
 *   its exact revision, and an application upgrades a named actor by
 *   starting it under the new revision and replacing its directory entry.
 * - **IDs.** The system keeps registered definitions in a slot table: an
 *   ID is a generation handle of its slot (`handle.h`), and freeing a
 *   definition moves the slot's generation on, so `ta_definition_lookup`
 *   answers NULL for a stale ID. Slots are reused, so each definition also
 *   keeps a registration serial, and `ta_system_find_definition` answers
 *   the newest match.
 * - **Inspection** (ADR 0042): a registered definition reports its ID,
 *   identity, `<scxml name>`, data-model adapter, binding mode, state
 *   count, and lifecycle and memory facts (`ta_definition_inspect`), and
 *   `ta_system_find_definition` resolves an exact identity to it.
 * - **Retirement.** A retired definition takes no new actors and matches
 *   no identity; the system frees it once its holds, holders and
 *   `actor_count` are all zero, when its last actor is freed, after that
 *   actor's data-model instance, session storage and invocations.
 *
 * ## Event descriptors and matching
 *
 * A transition's `event` attribute compiles to one `ta_event_pattern` per
 * descriptor: a run of `ta_event_pattern_segment`s, each an atom the
 * compiler interned when the definition was compiled, or the wildcard
 * `*`. Descriptors are authored names; nothing at run time adds to them.
 *
 * `ta_event_pattern_matches` matches one descriptor against an event's
 * name given as its segments, a list of atoms (ADR 0060 section 5): a
 * descriptor that runs out before the event matches by prefix, and a `*`
 * segment takes zero or more event segments anywhere in the descriptor.
 * It compares the event's atoms with equality against the descriptor's,
 * and needs nothing else about the event: not its trie node, not its
 * text. So an event segment that is no atom at all (the name resolver's
 * UNKNOWN, `TA_ATOM_NONE`) equals no descriptor segment, since every
 * descriptor segment that is not a wildcard is a real atom, and is taken
 * only by `*`. Matching segments looked up by text therefore gives the
 * same answers as matching interned ones.
 *
 * The interpreter turns a taken event's name into its segments once per
 * selection, before it matches any transition, and matches every
 * descriptor of every candidate transition against that one list. An
 * event carries its name as a node of the system's event-name trie, so its
 * segments are the node's path (`ta_trie_path`); the name resolver
 * (`names.h`) gives the same list from a name's bytes, by lookup, except
 * where a trie segment holds a dot, which bytes split in two (the
 * generated invocation ID of an invocation's error name;
 * `docs/internal/names.md`).
 *
 * ## Memory
 *
 * A definition owns everything in it through its arena, from the
 * allocator given to the compiler, except the span side table, one
 * allocation from `span_allocator` that a registration by identity may
 * hand to the live definition (`ta_definition_take_spans`), and the data
 * model's prepared state, which the data model frees. The atoms and event
 * names it uses belong to its system, which outlives it. It lives until
 * the system frees it (see retirement above) or, unregistered, until
 * `ta_definition_destroy`.
 *
 * Compiling allocates; `ta_definition_place_actions` allocates in the
 * arena while the compiler seals the definition. Every other function
 * here only reads, never allocates, and may run inside a macrostep.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_definition_lookup`, `ta_definition_is_atomic`, `ta_definition_session_bytes`, `ta_definition_element_count`, `ta_definition_span_base` | O(1) |
 * | `ta_definition_is_descendant` | O(depth of the state) |
 * | `ta_event_pattern_matches` | O(event segments × descriptor segments) time, O(1) space, no recursion (ADR 0060) |
 * | `ta_definition_place_actions` | O(actions) |
 * | `ta_definition_take_spans` | O(1) |
 *
 * ## Failure
 *
 * `ta_definition_place_actions` answers `TA_ERR_NOMEM` when the arena
 * cannot grow. The rest cannot fail: a lookup answers NULL for an unknown
 * or stale ID, and a match answers false for an event with no segments.
 *
 * ## Users
 *
 * The compiler, the interpreter (transition selection, executable
 * content, invocations), session images, inspection
 * (`ta_definition_inspect`, `ta_definition_state`, ...) and export. ADRs
 * 0042, 0043, 0053, 0060, 0063, 0071, 0072, 0075, 0076, 0077, 0085, 0087,
 * 0088.
 */
#pragma once

#include "configuration.h"
#include "datamodel.h"
#include "history.h"
#include "span.h"
#include "text_writer.h"
#include "../include/tinyactors.h"

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

/*
 * Where an action sits (ADR 0071): its block, the block's owner, which of
 * the owner's blocks of that kind, and its place in the block. The public
 * ta_action_info, less the element kind, which the action itself holds.
 */
typedef struct {
    ta_action_block block;
    uint32_t owner;
    uint32_t block_index;
    uint32_t index;
} ta_action_place;

struct ta_definition {
    ta_allocator allocator;
    /* Owns every array and string below. */
    ta_arena arena;
    /* Counters and state occupancy (ADR 0072): bookkeeping about the
     * definition's actors, the only fields that change after registration.
     * `occupancy` has `state_count` entries, from registration on. */
    ta_counters counters;
    uint32_t *occupancy;
    /* The microstep limit (ADR 0075): 0 takes the system's,
     * TA_MICROSTEP_LIMIT_NONE is none. Bookkeeping, like the counters. */
    uint32_t microstep_limit;
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
    /* identity.family as a system atom: what a runtime reference to one of
     * its actors names (ADR 0088). */
    ta_atom family;
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
    /* The deepest nesting of blocks of executable content: 1 for blocks with
     * no <if> or <foreach> in them, 0 for a chart with no blocks at all.
     * Sizes the executor's frames (ADR 0060). */
    uint32_t max_action_depth;
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
    /* Where each action sits, indexed like action_defs; computed once the
     * definition is sealed (ta_definition_place_actions). */
    ta_action_place *action_places;
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
    ta_expr_role *expression_roles; /* by expression, for prepare_definition (ADR 0076) */
    uint32_t expression_count;
    /*
     * Authored input the compiler normalizes, kept so ta_definition_export()
     * rebuilds a spec with the same fingerprint (ADR 0042).
     */
    ta_str *authored_texts;
    uint32_t authored_text_count;
    /*
     * Where each element was written (ADR 0085), when the spec knew for at
     * least one; null otherwise. One span per element, by kind and then by
     * ID: the states, the transitions (initial ones included), the actions
     * and the expressions (ta_definition_span_base()). Unknown spans are
     * zero. `sources` are the names they point into. All of it is one
     * allocation of `span_bytes` from `span_allocator`, starting at `spans`,
     * not the arena, so that a registration answered by this definition can
     * hand it newer spans (ta_definition_take_spans()).
     */
    ta_source_span *spans;
    uint32_t span_count;
    ta_scxml_source *sources;
    uint32_t source_count;
    size_t span_bytes;
    ta_allocator span_allocator;
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
    /* One actor of this definition is one block (ADR 0063; ta_actor_layout()). */
    size_t actor_block_bytes;
    size_t actor_block_alignment;
    size_t actor_session_offset;
    size_t actor_instance_offset;
    /* Resident actors whose `definition` points here; see ta_actor_free(). */
    uint32_t actor_count;
    /*
     * Holds (ADR 0077): each ta_system_register_definition() takes one and
     * ta_system_release_definition() gives one back. `holders` counts the
     * definitions that nest this one for an inline <invoke>. The system
     * frees the definition once holds, holders and actor_count are all zero.
     */
    uint32_t holds;
    uint32_t holders;
    /*
     * Set by ta_system_retire_definition(): no new actors, no match by
     * identity, and its holds dropped (ADR 0043, ADR 0077).
     */
    bool retired;
    /* Registration order; see ta_system_find_definition(). */
    uint64_t registration_serial;
    /* The compiler's content hash, whatever the revision (ADR 0077). */
    uint64_t content_hash;
    /*
     * The chart has a host expression, which the content hash marks but
     * cannot see into: registration answers it only with a definition that
     * holds the same handles (ADR 0087).
     */
    bool host_expressions;
    /*
     * The definition each <invoke> nests, by invoke declaration, or 0; null
     * until the first ta_definition_nest(). Allocated in `arena`.
     */
    ta_definition_id *nested;
    /* The system's free list while a release cascades (ADR 0060: no recursion). */
    ta_definition *free_next;
};

/**
 * The registered definition `id` names, retired or not, borrowed from the
 * system; NULL for an unknown or stale ID. Never allocates; may run inside
 * a macrostep.
 */
const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id);
/** Bytes of one actor's session storage: two configurations and history. Never allocates. */
size_t ta_definition_session_bytes(const ta_definition *definition);
/**
 * Fills `action_places` from the sealed blocks, in the definition's arena.
 * Every action belongs to exactly one block, so one pass over the top-level
 * blocks and one over the actions (for the blocks nested in <if> and
 * <foreach>) place them all, with no recursion (ADR 0060). Runs while the
 * compiler seals the definition, never in a macrostep; allocates, and
 * answers `TA_ERR_NOMEM` when the arena cannot grow.
 */
ta_result ta_definition_place_actions(ta_definition *definition);
/**
 * The layout of `spans` (ADR 0085): elements of `kind` are numbered from
 * ta_definition_span_base(), and there are ta_definition_element_count() of
 * them. ta_definition_export() numbers the exported spec's spans the same
 * way. Never allocates.
 */
uint32_t ta_definition_element_count(const ta_definition *definition, ta_element_kind kind);
/** The number of the first span of an element of `kind`; see ta_definition_element_count(). */
uint32_t ta_definition_span_base(const ta_definition *definition, ta_element_kind kind);
/**
 * Registration by identity answered `definition` with `live` (ADR 0077):
 * when `definition` knows where its elements are written, and not exactly
 * where `live` says, `live` takes its spans, and `definition`, about to be
 * destroyed, takes `live`'s old ones with it (ADR 0085). Allocates nothing,
 * so it cannot fail.
 */
void ta_definition_take_spans(ta_definition *live, ta_definition *definition);
/** Whether `state` is atomic or final. Never allocates; may run inside a macrostep. */
bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state);
/**
 * Whether `state` is a proper descendant of `ancestor`; with
 * `TA_STATE_NONE` as the ancestor, whether `state` is a state at all.
 * Walks the parents. Never allocates; may run inside a macrostep.
 */
bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor);
/**
 * Whether `pattern`, one of `definition`'s event descriptors, matches the
 * event whose name has the segments `event`, first segment first (see
 * "Event descriptors and matching" above): by prefix, with `*` taking zero
 * or more segments, and a segment that is no atom (`TA_ATOM_NONE`) taken
 * only by `*`. False for an event of no segments. Borrows `event` for the
 * call. Never allocates and never fails; may run inside a macrostep.
 * O(event segments × descriptor segments), O(1) space.
 */
bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              const ta_atom_list *event);
