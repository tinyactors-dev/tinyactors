/* link.c — links between systems over JavaScript's transports (src/link, docs/adr/0008):
 * the protocol is core's (ta_links); core.mjs moves the text (binding.h). */
#include "binding.h"

#include <tinyactors/link.h>

/* the router's view of a peer (ta_host_peer_status) as an object; null when it is gone */
static napi_value peer_status_value(napi_env environment, nd_system *host, uint32_t peer) {
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

/* the system's links, made with the first one */
static ta_links *links_of(nd_system *system) {
    if (system->links) return system->links;
    ta_links_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.host = system->host;
    config.system_id = view_of(system->system_id, strlen(system->system_id));
    config.datamodels = &system->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    if (ta_links_new(&config, &system->links) != TA_OK) system->links = NULL;
    if (!system->links) napi_throw_error(system->environment, NULL, "links: no memory");
    return system->links;
}

void links_free(nd_system *system) {
    ta_links_free(system->links);
    system->links = NULL;
}

ta_time links_now(nd_system *system) {
    double now = system_now(system);
    return now > 0 ? (ta_time)now : 0;
}

/* what the links asked of the transports, as effects { kind: 'link', op, connection, text } */
void links_output(nd_system *system) {
    if (!system->links) return;
    napi_env environment = system->environment;
    static const char *const operations[] = {"", "send", "close", "open", "hello"};
    const ta_link_output *out;
    uint32_t n = ta_links_output(system->links, &out);
    for (uint32_t i = 0; i < n; i++) {
        napi_value e = effect_new(system, "link");
        const char *operation = out[i].kind < 5 ? operations[out[i].kind] : "";
        set(environment, e, "op", str_value(environment, view_of(operation, strlen(operation))));
        set(environment, e, "connection", num(environment, out[i].connection));
        if (out[i].text.length) set(environment, e, "text", str_value(environment, out[i].text));
    }
    ta_links_output_ack(system->links);
}

/* string views of an array of strings (its texts the caller's to free with texts_free) */
static uint32_t views_of(napi_env environment, napi_value array, ta_string_view **views,
                         char ***texts) {
    uint32_t n = array_length(environment, array);
    *views = (ta_string_view *)calloc(n + 1, sizeof(ta_string_view));
    *texts = (char **)calloc(n + 1, sizeof(char *));
    if (!*views || !*texts) abort();
    for (uint32_t i = 0; i < n; i++) {
        napi_value v;
        size_t length = 0;
        napi_get_element(environment, array, i, &v);
        (*texts)[i] =
            type_of(environment, v) == napi_string ? get_string(environment, v, &length) : NULL;
        (*views)[i] = view_of((*texts)[i] ? (*texts)[i] : "", length);
    }
    return n;
}

static void views_free(ta_string_view *views, char **texts, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        free(texts[i]);
    free(texts);
    free(views);
}

/* { trusted, judge, announce (true or ids), maxQueue, queueTTLMs, resumeMs, pingMs,
 * maxFrame, name, credential (JSON text), exports: [[name, events], ...] } */
static napi_value link_open(napi_env environment, napi_callback_info info, int server) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    ta_links *links = system ? links_of(system) : NULL;
    if (!links) return NULL;
    napi_value o = argv[1];
    ta_link_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    if (property_bool(environment, o, "trusted")) options.flags |= TA_LINK_TRUSTED;
    if (property_bool(environment, o, "judge")) options.flags |= TA_LINK_JUDGE;
    napi_value announce = property(environment, o, "announce");
    bool announce_list = false;
    napi_is_array(environment, announce, &announce_list);
    if (announce_list || property_bool(environment, o, "announce"))
        options.flags |= TA_LINK_ANNOUNCE;
    options.max_queue = (uint32_t)property_double(environment, o, "maxQueue");
    options.queue_ttl = (ta_time)property_double(environment, o, "queueTTLMs");
    options.resume = (ta_time)property_double(environment, o, "resumeMs");
    options.ping_interval = (ta_time)property_double(environment, o, "pingMs");
    options.max_frame = (uint32_t)property_double(environment, o, "maxFrame");
    size_t nl = 0, cl = 0;
    char *name = property_string(environment, o, "name", &nl),
         *credential = property_string(environment, o, "credential", &cl);
    options.name = view_of(name ? name : "", nl);
    options.credential = view_of(credential ? credential : "", cl);
    ta_string_view *ids = NULL, *names = NULL, *events = NULL;
    char **id_texts = NULL, **name_texts = NULL, **event_texts = NULL;
    uint32_t id_count = 0, export_count = 0;
    if (announce_list) {
        id_count = views_of(environment, announce, &ids, &id_texts);
        options.announce = ids;
        options.announce_count = id_count;
    }
    napi_value exports = property(environment, o, "exports");
    export_count = array_length(environment, exports);
    names = (ta_string_view *)calloc(export_count + 1, sizeof *names);
    events = (ta_string_view *)calloc(export_count + 1, sizeof *events);
    name_texts = (char **)calloc(2 * export_count + 1, sizeof(char *));
    if (!names || !events || !name_texts) abort();
    for (uint32_t i = 0; i < export_count; i++) {
        napi_value pair, v;
        size_t length = 0;
        napi_get_element(environment, exports, i, &pair);
        napi_get_element(environment, pair, 0, &v);
        name_texts[2 * i] =
            type_of(environment, v) == napi_string ? get_string(environment, v, &length) : NULL;
        names[i] = view_of(name_texts[2 * i] ? name_texts[2 * i] : "", length);
        length = 0;
        napi_get_element(environment, pair, 1, &v);
        name_texts[2 * i + 1] =
            type_of(environment, v) == napi_string ? get_string(environment, v, &length) : NULL;
        events[i] = view_of(name_texts[2 * i + 1] ? name_texts[2 * i + 1] : "", length);
    }
    options.exports = names;
    options.export_events = events;
    options.export_count = export_count;
    uint32_t handle = 0;
    ta_status status = server ? ta_links_serve(links, &options, &handle)
                              : ta_links_connect(links, &options, links_now(system), &handle);
    free(name);
    free(credential);
    if (id_texts) views_free(ids, id_texts, id_count);
    for (uint32_t i = 0; i < 2 * export_count; i++)
        free(name_texts[i]);
    free(name_texts);
    free(names);
    free(events);
    (void)event_texts;
    if (status != TA_OK) {
        napi_throw_error(environment, NULL,
                         status == TA_ERROR_INVALID ? "createLink: name is not a system id"
                                                    : "links: cannot add the peer");
        return NULL;
    }
    return num(environment, handle); /* the next pump asks for a link's transport */
}

/* linkConnect(system, options) -> link: the next pump asks for its transport (an "open" effect) */
napi_value js_link_connect(napi_env environment, napi_callback_info info) {
    return link_open(environment, info, 0);
}

/* linkServe(system, options) -> server */
napi_value js_link_serve(napi_env environment, napi_callback_info info) {
    return link_open(environment, info, 1);
}

/* the system and the links of a call (system, handle, ...) */
static nd_system *links_call(napi_env environment, napi_callback_info info, size_t want,
                             napi_value *argv, uint32_t *handle) {
    size_t argc = want;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= want ? get_system(environment, argv[0]) : NULL;
    if (!system || !links_of(system)) return NULL;
    *handle = get_u32(environment, argv[1]);
    return system;
}

/* linkAccept(system, server) -> connection: a transport the server took */
napi_value js_link_accept(napi_env environment, napi_callback_info info) {
    napi_value argv[2];
    uint32_t server = 0, connection = 0;
    nd_system *system = links_call(environment, info, 2, argv, &server);
    if (!system) return NULL;
    if (ta_links_accept(system->links, server, &connection) != TA_OK)
        return null_value(environment);
    return num(environment, connection);
}

/* linkOpened(system, connection) */
napi_value js_link_opened(napi_env environment, napi_callback_info info) {
    napi_value argv[2];
    uint32_t connection = 0;
    nd_system *system = links_call(environment, info, 2, argv, &connection);
    if (!system) return NULL;
    ta_links_opened(system->links, connection, links_now(system));
    return input_done(system, undef(environment));
}

/* linkReceived(system, connection, text) -> whether the link is open (its hello came) */
napi_value js_link_received(napi_env environment, napi_callback_info info) {
    napi_value argv[3];
    uint32_t connection = 0;
    nd_system *system = links_call(environment, info, 3, argv, &connection);
    if (!system) return NULL;
    size_t length = 0;
    char *text = get_string(environment, argv[2], &length);
    ta_time now = links_now(system);
    ta_host_advance_time(system->host, now);
    if (text) ta_links_received(system->links, connection, view_of(text, length), now);
    free(text);
    ta_link_status status;
    status.size = sizeof status;
    int open = ta_links_status(system->links, connection, &status) == TA_OK && status.open;
    return input_done(system, bool_value(environment, open));
}

/* linkDropped(system, connection, why) */
napi_value js_link_dropped(napi_env environment, napi_callback_info info) {
    napi_value argv[3];
    uint32_t connection = 0;
    nd_system *system = links_call(environment, info, 3, argv, &connection);
    if (!system) return NULL;
    size_t length = 0;
    char *why = get_string(environment, argv[2], &length);
    ta_links_dropped(system->links, connection, view_of(why ? why : "", length), links_now(system));
    free(why);
    return input_done(system, undef(environment));
}

/* linkAdmit(system, connection, verdict): a judged hello (0 refuse, 1 admit, 2 trusted) */
napi_value js_link_admit(napi_env environment, napi_callback_info info) {
    napi_value argv[3];
    uint32_t connection = 0;
    nd_system *system = links_call(environment, info, 3, argv, &connection);
    if (!system) return NULL;
    ta_links_admit(system->links, connection, get_u32(environment, argv[2]), links_now(system));
    return input_done(system, undef(environment));
}

/* linkClose(system, handle): a link, a server or a connection */
napi_value js_link_close(napi_env environment, napi_callback_info info) {
    napi_value argv[2];
    uint32_t handle = 0;
    nd_system *system = links_call(environment, info, 2, argv, &handle);
    if (!system) return NULL;
    ta_links_close(system->links, handle, links_now(system));
    return input_done(system, undef(environment));
}

/* linkStatus(system, connection) -> the router's view of its peer with { open,
 * remote, attempts, error }, or null */
napi_value js_link_status(napi_env environment, napi_callback_info info) {
    napi_value argv[2];
    uint32_t connection = 0;
    nd_system *system = links_call(environment, info, 2, argv, &connection);
    if (!system) return NULL;
    ta_link_status status;
    status.size = sizeof status;
    if (ta_links_status(system->links, connection, &status) != TA_OK)
        return null_value(environment);
    napi_value o = peer_status_value(environment, system, status.peer);
    if (type_of(environment, o) != napi_object) napi_create_object(environment, &o);
    set(environment, o, "open", bool_value(environment, (int)status.open));
    set(environment, o, "remote", str_value(environment, status.remote));
    set(environment, o, "attempts", num(environment, status.attempts));
    set(environment, o, "error",
        status.error.length ? str_value(environment, status.error) : null_value(environment));
    return o;
}

/* linkConnections(system, server) -> its connections that said hello (also while away) */
napi_value js_link_connections(napi_env environment, napi_callback_info info) {
    napi_value argv[2], out;
    uint32_t server = 0;
    nd_system *system = links_call(environment, info, 2, argv, &server);
    if (!system) return NULL;
    uint32_t n = ta_links_connections(system->links, server, NULL, 0);
    uint32_t *handles = (uint32_t *)calloc(n + 1, sizeof(uint32_t));
    if (!handles) abort();
    n = ta_links_connections(system->links, server, handles, n);
    napi_create_array_with_length(environment, n, &out);
    for (uint32_t i = 0; i < n; i++)
        napi_set_element(environment, out, i, num(environment, handles[i]));
    free(handles);
    return out;
}
