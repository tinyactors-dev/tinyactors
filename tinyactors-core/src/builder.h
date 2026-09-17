#pragma once

#include "definition.h"

/*
 * Declarative SCXML authoring tree.
 *
 * Every child list is terminated by a zero-initialized element: `{0}`. The
 * spec enums deliberately reserve zero for that terminator; do not use the
 * corresponding runtime enums here, because some valid runtime values are
 * zero. The compiler consumes the tree synchronously and retains no pointer
 * into it.
 */

enum {
    TA_SEND_NONE = -1,
    TA_INVOKE_NONE = -1
};

/*
 * Storage for dynamically authored specification trees. A tree owns all
 * allocations made through it until ta_tree_destroy(). Its first allocation
 * failure is retained and reported by ta_tree_result(); subsequent writes are
 * ignored so import code can report one result at its outer boundary.
 */
typedef struct {
    ta_allocator allocator;
    ta_result result;
    void *impl;
} ta_tree;

/* A temporary, type-erased child list owned by a ta_tree. */
typedef struct {
    ta_tree *tree;
    size_t element_size;
    size_t element_alignment;
    void *impl;
} ta_tree_list;

/**
 * Initialize storage used while dynamically constructing a specification.
 *
 * @code{.c}
 * ta_tree tree;
 * ta_tree_list states;
 * ta_tree_init(&tree, allocator);
 * ta_tree_list_init(&states, &tree,
 *                   sizeof(ta_scxml_state_spec),
 *                   _Alignof(ta_scxml_state_spec));
 *
 * ta_scxml_state_spec idle = {
 *     .kind = TA_SCXML_STATE_ATOMIC,
 *     .id = { .ptr = "idle", .len = 4 },
 * };
 * ta_tree_put(&states, &idle);
 *
 * const ta_scxml_state_spec *state_list = ta_tree_close(&states);
 * if (ta_tree_result(&tree) != TA_OK) {
 *     ta_tree_destroy(&tree);
 *     return TA_ERR_NOMEM;
 * }
 * @endcode
 */
void ta_tree_init(ta_tree *tree, ta_allocator allocator);
void ta_tree_destroy(ta_tree *tree);
void *ta_tree_alloc(ta_tree *tree, size_t size, size_t alignment);
ta_result ta_tree_result(const ta_tree *tree);

void ta_tree_list_init(ta_tree_list *list,
                       ta_tree *tree,
                       size_t element_size,
                       size_t element_alignment);
void ta_tree_put(ta_tree_list *list, const void *element);
const void *ta_tree_close(ta_tree_list *list);

typedef enum {
    TA_SCXML_VALUE_NONE = 0,
    TA_SCXML_VALUE_EXPR,
    TA_SCXML_VALUE_LOCATION,
    TA_SCXML_VALUE_CONTENT
} ta_scxml_value_kind;

typedef struct {
    ta_scxml_value_kind kind;
    union {
        ta_expression expr;
        ta_expression location;
        ta_bytes content;
    } as;
} ta_scxml_value_spec;

typedef enum {
    TA_SCXML_EVENT_NONE = 0,
    TA_SCXML_EVENT_NAME,
    TA_SCXML_EVENT_EXPR
} ta_scxml_event_kind;

typedef struct {
    ta_scxml_event_kind kind;
    union {
        ta_str name;
        ta_expression expr;
    } as;
} ta_scxml_event_spec;

typedef enum {
    TA_SCXML_ACTION_NONE = 0,
    TA_SCXML_ACTION_RAISE,
    TA_SCXML_ACTION_SEND,
    TA_SCXML_ACTION_CANCEL,
    TA_SCXML_ACTION_ASSIGN,
    TA_SCXML_ACTION_SCRIPT,
    TA_SCXML_ACTION_IF,
    TA_SCXML_ACTION_FOREACH,
    TA_SCXML_ACTION_INVOKE,
    TA_SCXML_ACTION_EXTENSION
} ta_scxml_action_kind;

typedef enum {
    TA_SCXML_TRANSITION_NONE = 0,
    TA_SCXML_TRANSITION_EXTERNAL,
    TA_SCXML_TRANSITION_INTERNAL
} ta_scxml_transition_kind;

typedef enum {
    TA_SCXML_STATE_NONE = 0,
    TA_SCXML_STATE_ATOMIC,
    TA_SCXML_STATE_COMPOUND,
    TA_SCXML_STATE_PARALLEL,
    TA_SCXML_STATE_FINAL,
    TA_SCXML_STATE_HISTORY_SHALLOW,
    TA_SCXML_STATE_HISTORY_DEEP
} ta_scxml_state_kind;

typedef enum {
    TA_SCXML_INVOKE_NONE = 0,
    TA_SCXML_INVOKE
} ta_scxml_invoke_kind;

typedef struct ta_scxml_action_spec ta_scxml_action_spec;
typedef struct ta_scxml_transition_spec ta_scxml_transition_spec;
typedef struct ta_scxml_state_spec ta_scxml_state_spec;

typedef struct {
    ta_str name;
    ta_scxml_value_spec value;
} ta_scxml_param_spec;

typedef struct {
    ta_send_id id;
    ta_atom io_type;
    ta_scxml_event_spec event;
    ta_scxml_value_spec target;
    ta_expression delay;
    ta_scxml_value_spec data;
    const ta_scxml_param_spec *params;
} ta_scxml_send_spec;

typedef struct {
    ta_scxml_invoke_kind kind;
    ta_invoke_id id;
    ta_atom type;
    ta_scxml_value_spec src;
    ta_scxml_value_spec data;
    bool autoforward;
    const ta_scxml_param_spec *params;
    const ta_scxml_action_spec *finalize;
} ta_scxml_invoke_spec;

struct ta_scxml_action_spec {
    ta_scxml_action_kind kind;
    union {
        struct { ta_str event; ta_scxml_value_spec data; } raise;
        ta_scxml_send_spec send;
        struct { ta_send_id id; ta_expression id_expr; } cancel;
        struct { ta_expression location; ta_scxml_value_spec value; } assign;
        struct { ta_expression script; } script;
        struct {
            ta_expression condition;
            const ta_scxml_action_spec *then_actions;
            const ta_scxml_action_spec *else_actions;
        } conditional;
        struct {
            ta_expression array;
            ta_str item;
            ta_str index;
            const ta_scxml_action_spec *body;
        } foreach;
        ta_scxml_invoke_spec invoke;
        struct { ta_atom type; ta_bytes payload; } extension;
    } as;
};

typedef struct {
    const ta_str *targets;
    const ta_scxml_action_spec *actions;
} ta_scxml_initial_spec;

struct ta_scxml_transition_spec {
    ta_scxml_transition_kind kind;
    ta_str event;
    ta_expression condition;
    const ta_str *targets;
    const ta_scxml_action_spec *actions;
};

typedef struct {
    ta_str name;
    ta_scxml_value_spec initializer;
} ta_scxml_data_spec;

struct ta_scxml_state_spec {
    ta_scxml_state_kind kind;
    ta_str id;
    const ta_scxml_state_spec *states;
    const ta_scxml_initial_spec *initial;
    const ta_scxml_transition_spec *transitions;
    const ta_scxml_action_spec *onentry;
    const ta_scxml_action_spec *onexit;
    const ta_scxml_data_spec *data;
    const ta_scxml_invoke_spec *invokes;
};

typedef struct {
    ta_str name;
    const ta_definition_identity *identity;
    ta_datamodel_binding_mode binding_mode;
    const ta_scxml_initial_spec *initial;
    const ta_scxml_state_spec *states;
    const ta_scxml_data_spec *data;
} ta_scxml_spec;

/**
 * Compile a sentinel-terminated SCXML specification into an immutable
 * definition.
 *
 * The compiler resolves state IDs and event descriptors, interns named send
 * events, validates the complete tree, and copies all data it retains. Thus
 * the specification may be a nested compound literal with automatic storage.
 * A zero-initialized `ta_expression` is absent. `TA_SEND_NONE` and
 * `TA_INVOKE_NONE` mean that their respective optional identifiers are absent.
 *
 * @code{.c}
 * // These expressions can carry source text or host-language handles.
 * ta_expression request_reply_to, unauthorized_response, authorization_redirect;
 * ta_expression callback_state_is_valid, token_exchange_request, callback_reply_to;
 * ta_expression invalid_callback_response, access_token, event_access_token;
 * ta_expression login_complete_response, token_failure_response;
 * ta_expression authorized_response, no_access_token;
 * ta_atom http_io;       // registered HTTP response I/O processor
 * ta_atom oauth_io;      // registered OAuth token-client I/O processor
 * ta_result result = ta_definition_compile_spec(
 *     system,
 *     &(ta_scxml_spec){
 *         .name = { .ptr = "oauth-login", .len = 11 },
 *         .initial = &(ta_scxml_initial_spec){
 *             .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *         },
 *         .states = (const ta_scxml_state_spec[]){
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "idle", .len = 4 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     // A protected request with no session receives 401.
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_INTERNAL,
 *                         .event = { .ptr = "http.resource", .len = 13 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = request_reply_to },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = unauthorized_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                     },
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "login.requested", .len = 15 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             // Generates state + PKCE data and sends 302.
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = request_reply_to },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = authorization_redirect },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "authorizing", .len = 11 }, {0}},
 *                     },
 *                     {0},
 *                 },
 *             },
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "authorizing", .len = 11 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.callback", .len = 14 },
 *                         .condition = callback_state_is_valid,
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = oauth_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "oauth.token.exchange", .len = 20 } },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = token_exchange_request },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "exchanging", .len = 10 }, {0}},
 *                     },
 *                     // A bad callback is answered; it is not left pending.
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.callback", .len = 14 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = callback_reply_to },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = invalid_callback_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *                     },
 *                     {0},
 *                 },
 *             },
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "exchanging", .len = 10 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.token.success", .len = 19 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_ASSIGN,
 *                                 .as.assign = { .location = access_token,
 *                                     .value = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = event_access_token } },
 *                             },
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = callback_reply_to },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = login_complete_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "authenticated", .len = 13 }, {0}},
 *                     },
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.token.failure", .len = 19 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = callback_reply_to },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = token_failure_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *                     }, {0},
 *                 },
 *             },
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "authenticated", .len = 13 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_INTERNAL,
 *                         .event = { .ptr = "http.resource", .len = 13 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .id = TA_SEND_NONE, .io_type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = request_reply_to },
 *                                     .delay = {0},
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = authorized_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                     },
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "logout.requested", .len = 16 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_ASSIGN,
 *                                 .as.assign = { .location = access_token,
 *                                     .value = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = no_access_token } },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *                     }, {0},
 *                 },
 *             },
 *             {0},
 *         },
 *     }, datamodel, allocator, &definition);
 * @endcode
 */
ta_result ta_definition_compile_spec(ta_system *system,
                                     const ta_scxml_spec *spec,
                                     ta_datamodel datamodel,
                                     ta_allocator allocator,
                                     ta_definition **out);
