/* link.h — the link protocol between systems over the caller's transports
 * (docs/protocol.md, "Link protocol"; docs/adr/0008).
 *
 * A link carries envelopes between this host's router and another system
 * over a message transport (a WebSocket, a framed byte stream, postMessage):
 * hello (version, id, reach, caps, credential), ev (an envelope), err, ping
 * and pong. Each connection is one router peer (ta_host_peer_*). The layer
 * does no I/O, reads no clock and calls nothing back but the datamodels'
 * to_wire / from_wire: the caller tells it what its transports did
 * (ta_links_opened, ta_links_received, ta_links_dropped), what time it is
 * (ta_links_time, ta_links_deadline), and hands it the host's
 * TA_HOST_EFFECT_PEER and TA_HOST_EFFECT_PEER_ERROR effects
 * (ta_links_effect). What the transports are to do comes out as output:
 * frames to send, transports to close, transports to open (a client link
 * reconnecting), hellos to judge. Inputs to the host (deliveries, peers
 * going up and down) are made at once: the caller runs the host afterwards.
 *
 *   client   ta_links_connect: a link to one other system under a name of
 *            ours (its alias); reconnects with a backoff (250 ms doubling
 *            up to 30 s) while not closed; pings every ping_interval.
 *   server   ta_links_serve, then ta_links_accept for each transport it
 *            takes: untrusted peers may send only to what `exports` names
 *            (ta_host_export); a system that comes back within resume
 *            (ticks) takes over its peer and what was queued for it; the
 *            newer connection of one system id wins.
 *
 * Handles are nonzero and name a link, a server or an accepted connection;
 * a closed one is stale. Free the links before their host. */
#ifndef TINYACTORS_LINK_H
#define TINYACTORS_LINK_H

#include <tinyactors/allocator.h>
#include <tinyactors/host.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_links ta_links;

typedef struct ta_links_config {
    uint32_t size;
    ta_host *host;
    ta_string_view system_id;               /* the host's (ta_host_config.system_id) */
    const ta_datamodel_binding *datamodels; /* the host's: payloads to and from neutral JSON */
    uint32_t datamodel_count;
    ta_allocator allocator;
} ta_links_config;
TA_API ta_status ta_links_new(const ta_links_config *config, ta_links **out);
TA_API void ta_links_free(ta_links *links);

/* Link flags. */
enum {
    TA_LINK_TRUSTED = 1u << 0,  /* the other side may send anything and relay */
    TA_LINK_ANNOUNCE = 1u << 1, /* announce the systems reachable through us in hellos */
    TA_LINK_JUDGE = 1u << 2     /* server: every hello waits for ta_links_admit */
};
typedef struct ta_link_options {
    uint32_t size;
    uint32_t flags;                 /* TA_LINK_* */
    ta_string_view name;            /* client: our name for the other system */
    ta_string_view credential;      /* client: JSON value sent as hello.cred (empty: none) */
    uint32_t max_queue;             /* envelopes queued while away (0: none, fail at once) */
    ta_time queue_ttl;              /* client: how long they may wait (0: forever) */
    ta_time resume;                 /* server: how long a system that left may come back */
    ta_time ping_interval;          /* client: 0 never */
    uint32_t max_frame;             /* the largest frame we take (0: TA_FRAME_MAX) */
    const ta_string_view *announce; /* the only ids announced (announce_count 0: all) */
    uint32_t announce_count;
    const ta_string_view *exports;       /* server: names untrusted peers may send to ... */
    const ta_string_view *export_events; /* ... and their events (empty: any) */
    uint32_t export_count;
} ta_link_options;
/* A client link; its first transport is asked for at once (TA_LINK_OPEN). */
TA_API ta_status ta_links_connect(ta_links *links, const ta_link_options *options, ta_time now,
                                  uint32_t *link);
TA_API ta_status ta_links_serve(ta_links *links, const ta_link_options *options, uint32_t *server);
/* A transport `server` took; it is open. */
TA_API ta_status ta_links_accept(ta_links *links, uint32_t server, uint32_t *connection);
/* Close a link (its peer goes: what was queued fails), a server (and its
 * connections and peers), or one accepted connection. */
TA_API void ta_links_close(ta_links *links, uint32_t handle, ta_time now);

/* What a transport did. `connection` is a link or an accepted connection. */
TA_API void ta_links_opened(ta_links *links, uint32_t connection, ta_time now);
TA_API void ta_links_received(ta_links *links, uint32_t connection, ta_string_view text,
                              ta_time now);
TA_API void ta_links_dropped(ta_links *links, uint32_t connection, ta_string_view why, ta_time now);
/* A hello that TA_LINK_JUDGE held: trusted, untrusted, or refused (closed). */
enum { TA_LINK_REFUSE = 0, TA_LINK_ADMIT = 1, TA_LINK_ADMIT_TRUSTED = 2 };
TA_API void ta_links_admit(ta_links *links, uint32_t connection, uint32_t verdict, ta_time now);
/* Pings, reconnects and the ends of resumption windows due by `now`. */
TA_API void ta_links_time(ta_links *links, ta_time now);
/* When ta_links_time has something to do next (UINT64_MAX: nothing). */
TA_API ta_time ta_links_deadline(const ta_links *links);
/* One of the host's effects: 1 when it was a TA_HOST_EFFECT_PEER or
 * TA_HOST_EFFECT_PEER_ERROR of one of these links (handled), else 0. Call
 * it before ta_host_effects_ack. */
TA_API int ta_links_effect(ta_links *links, const ta_host_effect *effect, ta_time now);

/* What the transports are to do, in order. */
enum {
    TA_LINK_SEND = 1,  /* send `text` (one message, one frame) */
    TA_LINK_CLOSE = 2, /* close the transport */
    TA_LINK_OPEN = 3,  /* open a new transport for this link */
    TA_LINK_HELLO = 4  /* judge the hello `text` (TA_LINK_JUDGE): ta_links_admit */
};
typedef struct ta_link_output {
    uint32_t kind;
    uint32_t connection;
    ta_string_view text;
} ta_link_output;
/* The output so far; valid until ta_links_output_ack. */
TA_API uint32_t ta_links_output(ta_links *links, const ta_link_output **out);
TA_API void ta_links_output_ack(ta_links *links);

typedef struct ta_link_status {
    uint32_t size;
    uint32_t open;         /* the transport said hello */
    uint32_t peer;         /* the router's (ta_host_peer_status), 0 before a hello */
    uint32_t attempts;     /* client: failed connections since the last hello */
    ta_string_view remote; /* the other system's id (empty before its hello) */
    ta_string_view error;  /* client: why the last transport dropped */
} ta_link_status;
TA_API ta_status ta_links_status(const ta_links *links, uint32_t connection, ta_link_status *out);
/* A server's connections that said hello (also while away): returns how
 * many; the first `capacity` are written. */
TA_API uint32_t ta_links_connections(const ta_links *links, uint32_t server, uint32_t *out,
                                     uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_LINK_H */
