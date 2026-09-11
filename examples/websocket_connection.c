/* One ephemeral actor per accepted WebSocket; transport and session storage
 * remain host-owned. */
#include "../src/tinyactors.h"
#include <stddef.h>
#include <stdint.h>

static const uint64_t HEARTBEAT_NS = 15000000000ull; /* 15 seconds. */
enum ws_action_code { ACT_READY = 1, ACT_UPDATE, ACT_HEARTBEAT, ACT_CLOSED, ACT_PONG };

typedef struct connection_state {
    uint64_t socket_token;
    uint64_t session_generation;
    uint64_t last_peer_rx_ns;
} connection_state_t;
struct connection_alignment_probe { char c; connection_state_t value; };

typedef struct ws_host_ops {
    void *user;
    /* OK and AGAIN both transfer this request lease to the host. AGAIN means
     * it is retained for retry in a bounded pending queue, not yet writable.
     * Other results mean fatal failure and transfer no lease. If even the
     * pending queue is full, return an error, not AGAIN. No inline completion:
     * complete after this callback returns. */
    ta_status_t (*queue_write)(void *user, uint64_t socket_token,
                               const ta_io_request_t *request);
    /* Nonblocking, idempotent socket shutdown by the exact actor incarnation.
     * Record terminal actors for release later; do not reenter the system here. */
    void (*close_once)(void *user, ta_actor_ref_t actor);
} ws_host_ops_t;

static ta_status_t schedule_heartbeat(ta_context_t *context)
{
    ta_send_t timer = { 0 };
    uint64_t message_id = 0u;
    timer.event = ta_text_from_cstr("heartbeat.timeout");
    timer.target.kind = TA_TARGET_SELF;
    timer.sendid = ta_text_from_cstr("heartbeat");
    timer.delay_ns = HEARTBEAT_NS;
    return ta_context_send(context, &timer, &message_id);
}

static ta_status_t connection_run(ta_context_t *context,
                                  ta_action_kind_t kind, ta_code_t code)
{
    ta_mut_bytes_t bytes;
    ta_context_view_t view;
    connection_state_t *state;
    ta_status_t status;
    if (kind != TA_ACTION_CUSTOM) return TA_EXECUTION_ERROR;
    status = ta_context_bytes(context, &bytes);
    if (status != TA_OK) return status;
    if (bytes.size != sizeof(connection_state_t)) return TA_EXECUTION_ERROR;
    state = (connection_state_t *)bytes.data;
    ta_context_inspect(context, &view);
    switch ((unsigned)code) {
    case ACT_READY:
        if (state->session_generation == 0u || state->socket_token == 0u)
            return TA_EXECUTION_ERROR;
        state->last_peer_rx_ns = view.now_ns;
        return schedule_heartbeat(context);
    case ACT_UPDATE: {
        ta_send_t send = { 0 }; uint64_t message_id = 0u;
        send.event = ta_text_from_cstr("server.update");
        send.type = ta_text_from_cstr("websocket");
        send.target.kind = TA_TARGET_URI;
        send.target.text = ta_text_from_cstr("/browser");
        if (view.system->event != NULL) {
            send.body.kind = TA_BODY_CONTENT;
            send.body.content = view.system->event->data;
        }
        return ta_context_send(context, &send, &message_id);
    }
    case ACT_HEARTBEAT: {
        ta_send_t ping = { 0 };
        ta_payload_t absent = { 0 };
        uint64_t message_id;
        if (view.now_ns - state->last_peer_rx_ns >= 2u * HEARTBEAT_NS)
            return ta_context_raise(context,
                ta_text_from_cstr("ws.timeout"), &absent);
        ping.event = ta_text_from_cstr("ws.ping");
        ping.type = ta_text_from_cstr("websocket");
        ping.target.kind = TA_TARGET_URI;
        ping.target.text = ta_text_from_cstr("/browser");
        status = ta_context_send(context, &ping, &message_id);
        if (status != TA_OK) return status;
        return schedule_heartbeat(context);
    }
    case ACT_CLOSED:
        return ta_context_cancel(context, ta_text_from_cstr("heartbeat"));
    case ACT_PONG:
        state->last_peer_rx_ns = view.now_ns;
        return TA_OK;
    default: return TA_INVALID;
    }
}

static void connection_terminal(void *user,
                                const ta_actor_termination_t *termination)
{
    ws_host_ops_t *ops = (ws_host_ops_t *)user;
    if (ops != NULL && ops->close_once != NULL)
        ops->close_once(ops->user, termination->actor);
}

/* Plain C99 structs: nesting supplies parents, strings supply targets.
 * The connected parent owns heartbeat lifetime across ready/blocked changes. */
static const ta_chart_t connection_chart = {
    .name = "websocket-connection",
    .data_model = "bytes",
    .data_layout = {
        .storage = TA_DATA_BYTES,
        .bytes_layout = {
            .size = sizeof(connection_state_t),
            .alignment = offsetof(struct connection_alignment_probe, value)
        }
    },
    .initial = { .target = "connecting" },
    .states = (const ta_chart_state_t[]) {
        {
            .id = "connecting",
            .transitions = (const ta_chart_transition_t[]) {
                { .event = "ws.open", .target = "connected" },
                { .event = "ws.closed ws.error ws.timeout error", .target = "closing" },
                {0}
            }
        },
        {
            .id = "connected",
            .initial = { .target = "ready" },
            .onentry = {
                .count = 1,
                .blocks = (const ta_block_t[]) {{
                    .count = 1,
                    .actions = (const ta_action_t[]) {{
                        .kind = TA_ACTION_CUSTOM, .as.custom.code = ACT_READY
                    }}
                }}
            },
            .onexit = {
                .count = 1,
                .blocks = (const ta_block_t[]) {{
                    .count = 1,
                    .actions = (const ta_action_t[]) {{
                        .kind = TA_ACTION_CUSTOM, .as.custom.code = ACT_CLOSED
                    }}
                }}
            },
            .transitions = (const ta_chart_transition_t[]) {
                {
                    .event = "heartbeat.timeout",
                    .content = {
                        .count = 1,
                        .actions = (const ta_action_t[]) {{
                            .kind = TA_ACTION_CUSTOM, .as.custom.code = ACT_HEARTBEAT
                        }}
                    }
                },
                {
                    .event = "ws.pong",
                    .content = {
                        .count = 1,
                        .actions = (const ta_action_t[]) {{
                            .kind = TA_ACTION_CUSTOM, .as.custom.code = ACT_PONG
                        }}
                    }
                },
                { .event = "ws.closed ws.error ws.timeout error", .target = "closing" },
                {0}
            },
            .states = (const ta_chart_state_t[]) {
                {
                    .id = "ready",
                    .transitions = (const ta_chart_transition_t[]) {
                        {
                            .event = "server.update",
                            .content = {
                                .count = 1,
                                .actions = (const ta_action_t[]) {{
                                    .kind = TA_ACTION_CUSTOM, .as.custom.code = ACT_UPDATE
                                }}
                            }
                        },
                        { .event = "ws.write_blocked", .target = "blocked" },
                        {0}
                    }
                },
                {
                    .id = "blocked",
                    .transitions = (const ta_chart_transition_t[]) {
                        { .event = "ws.drain", .target = "ready" },
                        {0}
                    }
                },
                {0}
            }
        },
        { .id = "closing", .kind = TA_CHART_FINAL },
        {0}
    }
};

/* Static storage because make_connection_config returns before system create.
 * No location callback is needed for this send-only processor. */
static const ta_ioprocessor_desc_t connection_processors[] = {
    {
        .type = "urn:tinyactors:example:websocket",
        .aliases = (const char *const[]) { "websocket", "ws", NULL }
    },
    {0}
};

void make_connection_config(ta_system_config_t *config, ws_host_ops_t *ops)
{
    ta_system_config_init(config);
    config->session_prefix = ta_text_from_cstr("ws-server");
    config->data_model = ta_text_from_cstr("bytes");
    config->host_user = ops;
    config->data.run = connection_run;
    config->terminal = connection_terminal;
    config->ioprocessors = connection_processors;
    config->actor_defaults.external_events = 64u;
    config->actor_defaults.internal_events = 8u;
    config->actor_defaults.delayed_sends = 1u;
    config->actor_defaults.io_requests = 8u;
    config->actor_defaults.maximum_bytes = 32768u;
    config->actor_defaults.io_request_bytes = 32768u;
}
ta_status_t prepare_connection_definition(ta_system_t *system, ta_definition_t **out_definition)
{
    ta_definition_result_t result;
    ta_status_t status = ta_definition_build(system, &connection_chart, &result);
    if (status == TA_OK) *out_definition = result.definition;
    return status;
}

typedef struct ws_spawn_input {
    ta_definition_t *definition;
    ta_bytes_t initial_bytes; /* includes socket_token and session_generation */
} ws_spawn_input_t;
ta_status_t spawn_connection(ta_system_t *system, const ws_spawn_input_t *input,
                             ta_actor_ref_t *out_actor)
{
    ta_spawn_options_t options = { 0 };
    options.definition = input->definition;
    options.initial_bytes = input->initial_bytes;
    return ta_actor_spawn(system, &options, out_actor);
}

ta_status_t post_update(ta_system_t *system, ta_actor_ref_t connection, ta_payload_t payload)
{
    ta_post_options_t delivery = { 0 };
    uint64_t id;
    delivery.target.kind = TA_TARGET_INSTANCE;
    delivery.target.instance = connection;
    delivery.message.name = ta_text_from_cstr("server.update");
    delivery.message.data = payload;
    return ta_post(system, &delivery, &id);
}

ta_status_t ws_dispatch_one(ta_system_t *system, ws_host_ops_t *ops)
{
    const ta_io_request_t *r;
    ta_actor_info_t info;
    ta_status_t s = ta_io_next(system, &r);
    if (s != TA_OK) return s;
    if (r->kind != TA_IO_SEND || ops == NULL || ops->queue_write == NULL)
        return ta_io_complete(system, r->request, TA_CANCELLED);
    s = ta_actor_inspect(system, r->actor, &info);
    if (s != TA_OK || info.status == TA_ACTOR_DONE ||
        info.status == TA_ACTOR_STOPPED || info.status == TA_ACTOR_STOPPING ||
        info.status == TA_ACTOR_FAILED || info.state.storage != TA_DATA_BYTES ||
        info.state.as.bytes.size != sizeof(connection_state_t))
        return ta_io_complete(system, r->request, TA_CANCELLED);
    s = ops->queue_write(ops->user,
        ((const connection_state_t *)info.state.as.bytes.data)->socket_token, r);
    if (s == TA_AGAIN) {
        ta_post_options_t blocked = { 0 };
        uint64_t id;
        ta_status_t completed;
        /* The host retains r for retry. Backpressure is not a completed send,
         * and a transport write is not a browser application acknowledgement. */
        blocked.target.kind = TA_TARGET_INSTANCE;
        blocked.target.instance = r->actor;
        blocked.message.name = ta_text_from_cstr("ws.write_blocked");
        completed = ta_post(system, &blocked, &id);
        if (completed != TA_OK) {
            (void)ta_actor_stop(system, blocked.target.instance);
            return completed;
        }
    } else if (s != TA_OK) {
        ta_status_t completed = ta_io_complete(system, r->request,
                                             TA_COMMUNICATION_ERROR);
        if (completed != TA_OK) return completed;
    }
    return s;
}
ta_status_t ws_complete_write(ta_system_t *system, ta_request_ref_t request, ta_status_t result)
{
    return ta_io_complete(system, request, result);
}

ta_status_t drive_once(ta_system_t *system, uint64_t now_ns, ta_run_result_t *out_result)
{
    ta_run_options_t options = { now_ns, { 256u, 64u } };
    return ta_system_run(system, &options, out_result);
}
