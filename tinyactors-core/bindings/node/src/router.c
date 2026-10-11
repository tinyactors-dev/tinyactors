/* router.c — router peers, routes and exports (binding.h). */
#include "binding.h"

void strs_free(ta_string_view *s, uint32_t n) {
    for (uint32_t i = 0; s && i < n; i++)
        if (s[i].length) free((void *)s[i].pointer);
    free(s);
}

/* peerAdd(host, id, reach[], flags, maxQueue, queueTTLMs) -> peer handle (0: refused) */
napi_value js_peer_add(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 4 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t il = 0;
    char *id = get_string(environment, argv[1], &il);
    uint32_t nr = 0;
    ta_string_view *reach = get_strs(environment, argv[2], &nr);
    ta_peer_config c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.flags = get_u32(environment, argv[3]);
    c.id = (ta_string_view){id ? id : "", (uint32_t)il};
    c.reach = reach;
    c.reach_count = nr;
    c.max_queue = argc > 4 ? get_u32(environment, argv[4]) : 0;
    c.queue_ttl = argc > 5 ? (ta_time)get_session(environment, argv[5]) : 0;
    uint32_t peer = 0;
    if (id && ta_host_peer_add(host->host, &c, &peer) != TA_OK) peer = 0;
    free(id);
    strs_free(reach, nr);
    return num(environment, peer);
}

/* peerUp(host, peer, up) -> bool */
napi_value js_peer_up(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    bool up = false;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_get_value_bool(environment, argv[2], &up);
    return bool_value(environment,
                      ta_host_peer_up(host->host, get_u32(environment, argv[1]), up) == TA_OK);
}

/* peerRemove(host, peer) -> bool */
napi_value js_peer_remove(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    return bool_value(environment,
                      ta_host_peer_remove(host->host, get_u32(environment, argv[1])) == TA_OK);
}

/* peerReach(host, peer, reach[]) -> bool */
napi_value js_peer_reach(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint32_t nr = 0;
    ta_string_view *reach = get_strs(environment, argv[2], &nr);
    ta_status status = ta_host_peer_reach(host->host, get_u32(environment, argv[1]), reach, nr);
    strs_free(reach, nr);
    return bool_value(environment, status == TA_OK);
}

/* route(host, systemID, peer) -> bool (peer 0: remove the route) */
napi_value js_route(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t l = 0;
    char *system = get_string(environment, argv[1], &l);
    ta_status status = system ? ta_host_route(host->host, (ta_string_view){system, (uint32_t)l},
                                              get_u32(environment, argv[2]))
                              : TA_ERROR_INVALID;
    free(system);
    return bool_value(environment, status == TA_OK);
}

/* exportName(host, peer, name, events) -> bool: policy for untrusted peers */
napi_value js_export(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 4 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t nl = 0, el = 0;
    char *name = get_string(environment, argv[2], &nl);
    char *events = get_string(environment, argv[3], &el);
    ta_status status = name && events ? ta_host_export(host->host, get_u32(environment, argv[1]),
                                                       (ta_string_view){name, (uint32_t)nl},
                                                       (ta_string_view){events, (uint32_t)el})
                                      : TA_ERROR_INVALID;
    free(name);
    free(events);
    return bool_value(environment, status == TA_OK);
}

/* peerError(host, peer, origin, msgid, sendid, reason) -> bool: an `err` frame */
napi_value js_peer_error(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 6 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t ol = 0, ml = 0, sl = 0;
    char *o = get_string(environment, argv[2], &ol), *m = get_string(environment, argv[3], &ml),
         *sd = get_string(environment, argv[4], &sl);
    ta_status status = ta_host_peer_error(host->host, get_u32(environment, argv[1]),
                                          (ta_string_view){o ? o : "", o ? (uint32_t)ol : 0},
                                          (ta_string_view){m ? m : "", m ? (uint32_t)ml : 0},
                                          (ta_string_view){sd ? sd : "", sd ? (uint32_t)sl : 0},
                                          get_u32(environment, argv[5]));
    free(o);
    free(m);
    free(sd);
    return bool_value(environment, status == TA_OK);
}

/* peerRtt(host, peer, ms) */
napi_value js_peer_round_trip_time(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    ta_host_peer_round_trip_time(host->host, get_u32(environment, argv[1]),
                                 get_u32(environment, argv[2]));
    return undef(environment);
}

/* peerStatus(host, peer) -> { id, state, queued, rtt, lag, sent, received, errors,
 * duplicates } | null */
/* the router's view of a peer (ta_host_peer_status) as an object; null when it is gone */
napi_value peer_status_value(napi_env environment, nd_system *host, uint32_t peer) {
    napi_value o;
    ta_peer_status status;
    memset(&status, 0, sizeof status);
    status.size = sizeof status;
    if (!peer || ta_host_peer_status(host->host, peer, &status) != TA_OK)
        return null_value(environment);
    static const char *const states[] = {"connecting", "open", "reconnecting", "closed"};
    napi_create_object(environment, &o);
    napi_set_named_property(environment, o, "id", str_value(environment, status.id));
    napi_set_named_property(
        environment, o, "state",
        str_value(environment, (ta_string_view){states[status.state & 3],
                                                (uint32_t)strlen(states[status.state & 3])}));
    napi_set_named_property(environment, o, "queued", num(environment, status.queued));
    napi_set_named_property(environment, o, "rtt", num(environment, status.round_trip_time));
    napi_set_named_property(environment, o, "lag", num(environment, (double)status.lag));
    napi_set_named_property(environment, o, "sent", num(environment, (double)status.sent));
    napi_set_named_property(environment, o, "received", num(environment, (double)status.received));
    napi_set_named_property(environment, o, "errors", num(environment, (double)status.errors));
    napi_set_named_property(environment, o, "duplicates",
                            num(environment, (double)status.duplicates));
    return o;
}

napi_value js_peer_status(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    return peer_status_value(environment, host, get_u32(environment, argv[1]));
}

/* constants: the numbers of the public headers core.mjs needs (trace kinds and
 * presets, flags), so no JavaScript twin keeps them */
void constant(napi_env environment, napi_value o, const char *name, double value) {
    set(environment, o, name, num(environment, value));
}
