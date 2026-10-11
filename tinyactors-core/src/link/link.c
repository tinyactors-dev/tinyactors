/* link.c — the link protocol between systems (include/tinyactors/link.h,
 * docs/protocol.md). Freestanding: it allocates through the allocator of its
 * configuration and does no I/O. */
#include <tinyactors/address.h>
#include <tinyactors/frame.h>
#include <tinyactors/link.h>

#include "base/format.h"
#include "base/json.h"
#include "base/memory.h"

#define LINK_VERSION 1u
#define LINK_NEVER   UINT64_MAX
#define LINK_VIEW(s) ((ta_string_view){(s), (uint32_t)(sizeof(s) - 1)})

enum { SLOT_FREE = 0, SLOT_CLIENT = 1, SLOT_SERVER = 2, SLOT_ACCEPTED = 3 };

typedef struct link_text { /* an owned string */
    char *pointer;
    uint32_t length;
} link_text;

typedef struct link_slot {
    uint32_t kind, generation, flags;
    uint32_t server; /* accepted: its server's handle */
    uint32_t peer;   /* the router's peer (0: none yet) */
    int transport;   /* a transport is there */
    int open;        /* it said hello (and was admitted) */
    int judging;     /* its hello waits for ta_links_admit */
    int announced_once;
    uint32_t max_frame, peer_max, max_queue, attempts;
    ta_time queue_ttl, resume, ping_interval;
    ta_time retry_at, ping_at, expire_at;
    link_text name, credential, remote, error, announced, hello;
    link_text *reach;
    uint32_t reach_count;
    link_text *announce;
    uint32_t announce_count;
    link_text *exports, *export_events;
    uint32_t export_count;
} link_slot;

typedef struct link_record { /* an output, its text in `bytes` */
    uint32_t kind, connection;
    size_t offset, length;
} link_record;

typedef struct link_failure { /* an envelope a link refused: an error back, later */
    uint32_t peer, reason;
    link_text origin, msgid, sendid;
} link_failure;

typedef struct link_buffer {
    char *pointer;
    size_t length, capacity;
    int failed;
} link_buffer;

struct ta_links {
    ta_links_config config;
    link_text system_id;
    link_slot *slots;
    uint32_t slot_count, slot_capacity;
    link_record *records;
    uint32_t record_count, record_capacity;
    ta_link_output *views;
    uint32_t view_capacity;
    link_buffer bytes, frame, scratch, wire;
    link_failure *failures;
    uint32_t failure_count, failure_capacity;
};

static const char *const link_reasons[] = {"",        "unknown-target", "unreachable", "denied",
                                           "expired", "hops",           "malformed"};
#define REASON_COUNT 7u

/* ---- memory ---------------------------------------------------------------------------- */

static void *link_alloc(ta_links *links, size_t size) {
    return links->config.allocator.alloc(links->config.allocator.context, size ? size : 1, 8);
}

static void link_free(ta_links *links, void *pointer, size_t size) {
    if (pointer)
        links->config.allocator.free(links->config.allocator.context, pointer, size ? size : 1);
}

static int link_grow(ta_links *links, void **array, uint32_t *capacity, uint32_t need,
                     size_t element) {
    if (need <= *capacity) return 1;
    uint32_t c = *capacity ? *capacity : 8;
    while (c < need)
        c *= 2;
    void *grown = link_alloc(links, element * c);
    if (!grown) return 0;
    if (*array) {
        ta_memcpy(grown, *array, element * *capacity);
        link_free(links, *array, element * *capacity);
    }
    *array = grown;
    *capacity = c;
    return 1;
}

static void link_text_clear(ta_links *links, link_text *t) {
    link_free(links, t->pointer, (size_t)t->length + 1);
    t->pointer = NULL;
    t->length = 0;
}

static int link_text_set(ta_links *links, link_text *t, ta_string_view v) {
    link_text_clear(links, t);
    if (!v.length) return 1;
    t->pointer = (char *)link_alloc(links, (size_t)v.length + 1);
    if (!t->pointer) return 0;
    ta_memcpy(t->pointer, v.pointer, v.length);
    t->pointer[v.length] = 0;
    t->length = v.length;
    return 1;
}

static ta_string_view link_text_view(const link_text *t) {
    return (ta_string_view){t->pointer ? t->pointer : "", t->length};
}

static void link_texts_clear(ta_links *links, link_text **list, uint32_t *count) {
    for (uint32_t i = 0; i < *count; i++)
        link_text_clear(links, &(*list)[i]);
    link_free(links, *list, sizeof(link_text) * *count);
    *list = NULL;
    *count = 0;
}

static int link_texts_set(ta_links *links, link_text **list, uint32_t *count,
                          const ta_string_view *views, uint32_t n) {
    link_texts_clear(links, list, count);
    if (!n) return 1;
    *list = (link_text *)link_alloc(links, sizeof(link_text) * n);
    if (!*list) return 0;
    ta_memset(*list, 0, sizeof(link_text) * n);
    *count = n;
    for (uint32_t i = 0; i < n; i++)
        if (!link_text_set(links, &(*list)[i], views[i])) return 0;
    return 1;
}

static int link_view_equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length &&
           (a.length == 0 || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

static void link_buffer_reset(link_buffer *b) {
    b->length = 0;
    b->failed = 0;
}

static int link_buffer_room(ta_links *links, link_buffer *b, size_t more) {
    if (b->failed) return 0;
    if (b->length + more <= b->capacity) return 1;
    size_t c = b->capacity ? b->capacity : 256;
    while (c < b->length + more)
        c *= 2;
    char *grown = (char *)link_alloc(links, c);
    if (!grown) {
        b->failed = 1;
        return 0;
    }
    if (b->pointer) {
        ta_memcpy(grown, b->pointer, b->length);
        link_free(links, b->pointer, b->capacity);
    }
    b->pointer = grown;
    b->capacity = c;
    return 1;
}

static void link_put(ta_links *links, link_buffer *b, ta_string_view v) {
    if (link_buffer_room(links, b, v.length) && v.length) {
        ta_memcpy(b->pointer + b->length, v.pointer, v.length);
        b->length += v.length;
    }
}

static void link_put_string(ta_links *links, link_buffer *b, ta_string_view v) {
    size_t n = ta_json_write_string(NULL, v);
    if (link_buffer_room(links, b, n)) {
        ta_json_write_string(b->pointer + b->length, v);
        b->length += n;
    }
}

static void link_put_u64(ta_links *links, link_buffer *b, uint64_t v) {
    char digits[TA_FORMAT_U64_MAX];
    link_put(links, b, (ta_string_view){digits, ta_format_u64(digits, v)});
}

static void link_buffer_free(ta_links *links, link_buffer *b) {
    link_free(links, b->pointer, b->capacity);
    ta_memset(b, 0, sizeof *b);
}

/* ---- slots and handles ------------------------------------------------------------------ */

#define HANDLE_INDEX_BITS 20u

static uint32_t link_handle_of(const ta_links *links, const link_slot *slot) {
    uint32_t index = (uint32_t)(slot - links->slots);
    return slot->generation << HANDLE_INDEX_BITS | (index + 1);
}

static link_slot *link_slot_of(const ta_links *links, uint32_t handle) {
    uint32_t index = (handle & ((1u << HANDLE_INDEX_BITS) - 1)) - 1;
    if (!handle || index >= links->slot_count) return NULL;
    link_slot *slot = &links->slots[index];
    if (slot->kind == SLOT_FREE || slot->generation != handle >> HANDLE_INDEX_BITS) return NULL;
    return slot;
}

static link_slot *link_slot_new(ta_links *links, uint32_t kind) {
    for (uint32_t i = 0; i < links->slot_count; i++)
        if (links->slots[i].kind == SLOT_FREE) {
            link_slot *slot = &links->slots[i];
            uint32_t generation = (slot->generation + 1) & 0xfffu;
            ta_memset(slot, 0, sizeof *slot);
            slot->generation = generation ? generation : 1;
            slot->kind = kind;
            slot->retry_at = slot->ping_at = slot->expire_at = LINK_NEVER;
            return slot;
        }
    if (links->slot_count == (1u << HANDLE_INDEX_BITS) - 1 ||
        !link_grow(links, (void **)&links->slots, &links->slot_capacity, links->slot_count + 1,
                   sizeof(link_slot)))
        return NULL;
    link_slot *slot = &links->slots[links->slot_count++];
    ta_memset(slot, 0, sizeof *slot);
    slot->generation = 1;
    slot->kind = kind;
    slot->retry_at = slot->ping_at = slot->expire_at = LINK_NEVER;
    return slot;
}

static void link_slot_free(ta_links *links, link_slot *slot) {
    link_text *texts[] = {&slot->name,  &slot->credential, &slot->remote,
                          &slot->error, &slot->announced,  &slot->hello};
    for (uint32_t i = 0; i < sizeof texts / sizeof texts[0]; i++)
        link_text_clear(links, texts[i]);
    link_texts_clear(links, &slot->reach, &slot->reach_count);
    link_texts_clear(links, &slot->announce, &slot->announce_count);
    uint32_t count = slot->export_count; /* one count for both lists */
    link_texts_clear(links, &slot->exports, &count);
    count = slot->export_count;
    link_texts_clear(links, &slot->export_events, &count);
    uint32_t generation = slot->generation;
    ta_memset(slot, 0, sizeof *slot);
    slot->generation = generation;
    slot->kind = SLOT_FREE;
}

static link_slot *link_slot_of_peer(const ta_links *links, uint32_t peer) {
    for (uint32_t i = 0; peer && i < links->slot_count; i++)
        if ((links->slots[i].kind == SLOT_CLIENT || links->slots[i].kind == SLOT_ACCEPTED) &&
            links->slots[i].peer == peer)
            return &links->slots[i];
    return NULL;
}

/* ---- output ---------------------------------------------------------------------------- */

static void link_emit(ta_links *links, uint32_t kind, const link_slot *slot, ta_string_view text) {
    size_t offset = links->bytes.length;
    link_put(links, &links->bytes, text);
    if (links->bytes.failed || !link_grow(links, (void **)&links->records, &links->record_capacity,
                                          links->record_count + 1, sizeof(link_record)))
        return; /* no memory: the output is lost; the transport's peer times out */
    links->records[links->record_count++] =
        (link_record){kind, link_handle_of(links, slot), offset, text.length};
}

static ta_string_view link_frame_view(const ta_links *links) {
    return (ta_string_view){links->frame.pointer, (uint32_t)links->frame.length};
}

uint32_t ta_links_output(ta_links *links, const ta_link_output **out) {
    if (!link_grow(links, (void **)&links->views, &links->view_capacity, links->record_count,
                   sizeof(ta_link_output))) {
        *out = NULL;
        return 0;
    }
    for (uint32_t i = 0; i < links->record_count; i++) {
        const link_record *r = &links->records[i];
        links->views[i] =
            (ta_link_output){r->kind, r->connection,
                             (ta_string_view){r->length ? links->bytes.pointer + r->offset : "",
                                              (uint32_t)r->length}};
    }
    *out = links->views;
    return links->record_count;
}

void ta_links_output_ack(ta_links *links) {
    links->record_count = 0;
    link_buffer_reset(&links->bytes);
}

/* ---- hellos and what is announced -------------------------------------------------------- */

static int link_is_open_link(const link_slot *slot) {
    return (slot->kind == SLOT_CLIENT || slot->kind == SLOT_ACCEPTED) && slot->open &&
           slot->remote.length;
}

static int link_listed(const link_text *list, uint32_t count, ta_string_view id) {
    for (uint32_t i = 0; i < count; i++)
        if (link_view_equal(link_text_view(&list[i]), id)) return 1;
    return 0;
}

/* the systems reachable through us, as announced to `to`: sorted, one each */
static uint32_t link_reach_for(ta_links *links, const link_slot *to, ta_string_view *out,
                               uint32_t capacity) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < links->slot_count; i++) {
        const link_slot *c = &links->slots[i];
        if (c == to || !link_is_open_link(c)) continue;
        for (uint32_t k = 0; k <= c->reach_count; k++) {
            ta_string_view id =
                k == 0 ? link_text_view(&c->remote) : link_text_view(&c->reach[k - 1]);
            if (link_view_equal(id, link_text_view(&links->system_id)) ||
                link_view_equal(id, link_text_view(&to->remote)))
                continue;
            if (to->announce_count && !link_listed(to->announce, to->announce_count, id)) continue;
            uint32_t at = 0;
            while (at < n && !link_view_equal(out[at], id))
                at++;
            if (at < n || n == capacity) continue;
            uint32_t j = n++; /* sorted by bytes, shorter first on a tie */
            while (j > 0) {
                ta_string_view p = out[j - 1];
                uint32_t m = p.length < id.length ? p.length : id.length;
                int c2 = ta_memcmp(p.pointer, id.pointer, m);
                if (c2 < 0 || (c2 == 0 && p.length <= id.length)) break;
                out[j] = p;
                j--;
            }
            out[j] = id;
        }
    }
    return n;
}

static uint32_t link_reach_total(const ta_links *links) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < links->slot_count; i++)
        if (link_is_open_link(&links->slots[i])) n += 1 + links->slots[i].reach_count;
    return n;
}

/* our hello to `slot`, unless what it would announce did not change */
static void link_send_hello(ta_links *links, link_slot *slot) {
    if (!slot->transport) return;
    uint32_t capacity = (slot->flags & TA_LINK_ANNOUNCE) ? link_reach_total(links) : 0, n = 0;
    ta_string_view *reach =
        capacity ? (ta_string_view *)link_alloc(links, sizeof(ta_string_view) * capacity) : NULL;
    if (reach) n = link_reach_for(links, slot, reach, capacity);
    link_buffer *b = &links->scratch;
    link_buffer_reset(b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) link_put(links, b, LINK_VIEW(" "));
        link_put(links, b, reach[i]);
    }
    ta_string_view key = {b->pointer ? b->pointer : "", (uint32_t)b->length};
    if (slot->announced_once && link_view_equal(key, link_text_view(&slot->announced))) {
        link_free(links, reach, sizeof(ta_string_view) * capacity);
        return;
    }
    link_text_set(links, &slot->announced, key);
    slot->announced_once = 1;
    link_buffer *f = &links->frame;
    link_buffer_reset(f);
    link_put(links, f, LINK_VIEW("{\"t\":\"hello\",\"v\":1,\"id\":"));
    link_put_string(links, f, link_text_view(&links->system_id));
    link_put(links, f, LINK_VIEW(",\"reach\":["));
    for (uint32_t i = 0; i < n; i++) {
        if (i) link_put(links, f, LINK_VIEW(","));
        link_put_string(links, f, reach[i]);
    }
    link_put(
        links, f,
        LINK_VIEW(
            "],\"caps\":{\"profile\":\"neutral\",\"ordered\":true,\"acks\":false,\"max_frame\":"));
    link_put_u64(links, f, slot->max_frame);
    link_put(links, f, LINK_VIEW("}"));
    if (slot->credential.length) {
        link_put(links, f, LINK_VIEW(",\"cred\":"));
        link_put(links, f, link_text_view(&slot->credential));
    }
    link_put(links, f, LINK_VIEW("}"));
    link_free(links, reach, sizeof(ta_string_view) * capacity);
    if (!f->failed) link_emit(links, TA_LINK_SEND, slot, link_frame_view(links));
}

static void link_announce(ta_links *links) {
    for (uint32_t i = 0; i < links->slot_count; i++) {
        link_slot *slot = &links->slots[i];
        if ((slot->flags & TA_LINK_ANNOUNCE) && slot->open && slot->transport)
            link_send_hello(links, slot);
    }
}

/* the peer is up, reaching `remote` (client: under its alias) and its reach */
static void link_peer_up(ta_links *links, link_slot *slot) {
    uint32_t n = slot->reach_count + (slot->kind == SLOT_CLIENT);
    ta_string_view small[16],
        *reach = n <= 16 ? small : (ta_string_view *)link_alloc(links, sizeof(ta_string_view) * n);
    if (reach) {
        uint32_t k = 0;
        if (slot->kind == SLOT_CLIENT) reach[k++] = link_text_view(&slot->remote);
        for (uint32_t i = 0; i < slot->reach_count; i++)
            reach[k++] = link_text_view(&slot->reach[i]);
        ta_host_peer_reach(links->config.host, slot->peer, reach, k);
        if (reach != small) link_free(links, reach, sizeof(ta_string_view) * n);
    }
    ta_host_peer_up(links->config.host, slot->peer, 1);
    link_announce(links);
}

/* link_forget an accepted connection's system: its peer goes (what was queued fails) */
static void link_forget(ta_links *links, link_slot *slot) {
    if (slot->peer) ta_host_peer_remove(links->config.host, slot->peer);
    link_slot_free(links, slot);
    link_announce(links);
}

/* ---- links ----------------------------------------------------------------------------- */

ta_status ta_links_new(const ta_links_config *config, ta_links **out) {
    *out = NULL;
    if (!config || config->size < sizeof *config || !config->host || !config->allocator.alloc ||
        !config->allocator.free || !ta_address_system_valid(config->system_id))
        return TA_ERROR_INVALID;
    ta_links *links =
        (ta_links *)config->allocator.alloc(config->allocator.context, sizeof *links, 8);
    if (!links) return TA_ERROR_NO_MEMORY;
    ta_memset(links, 0, sizeof *links);
    links->config = *config;
    if (!link_text_set(links, &links->system_id, config->system_id)) {
        config->allocator.free(config->allocator.context, links, sizeof *links);
        return TA_ERROR_NO_MEMORY;
    }
    *out = links;
    return TA_OK;
}

static void link_failure_clear(ta_links *links, link_failure *f) {
    link_text_clear(links, &f->origin);
    link_text_clear(links, &f->msgid);
    link_text_clear(links, &f->sendid);
}

void ta_links_free(ta_links *links) {
    if (!links) return;
    for (uint32_t i = 0; i < links->slot_count; i++)
        if (links->slots[i].kind != SLOT_FREE) link_slot_free(links, &links->slots[i]);
    link_free(links, links->slots, sizeof(link_slot) * links->slot_capacity);
    link_free(links, links->records, sizeof(link_record) * links->record_capacity);
    link_free(links, links->views, sizeof(ta_link_output) * links->view_capacity);
    for (uint32_t i = 0; i < links->failure_count; i++)
        link_failure_clear(links, &links->failures[i]);
    link_free(links, links->failures, sizeof(link_failure) * links->failure_capacity);
    link_buffer_free(links, &links->bytes);
    link_buffer_free(links, &links->frame);
    link_buffer_free(links, &links->scratch);
    link_buffer_free(links, &links->wire);
    link_text_clear(links, &links->system_id);
    ta_allocator allocator = links->config.allocator;
    allocator.free(allocator.context, links, sizeof *links);
}

static int link_options_ok(const ta_link_options *o) {
    return o && o->size >= sizeof *o;
}

static int link_slot_options(ta_links *links, link_slot *slot, const ta_link_options *o) {
    slot->flags = o->flags;
    slot->max_frame = o->max_frame ? o->max_frame : TA_FRAME_MAX;
    slot->peer_max = TA_FRAME_MAX;
    slot->max_queue = o->max_queue;
    slot->queue_ttl = o->queue_ttl;
    slot->resume = o->resume;
    slot->ping_interval = o->ping_interval;
    return link_texts_set(links, &slot->announce, &slot->announce_count, o->announce,
                          o->announce_count);
}

ta_status ta_links_connect(ta_links *links, const ta_link_options *o, ta_time now, uint32_t *link) {
    (void)now;
    if (!link_options_ok(o) || !ta_address_system_valid(o->name)) return TA_ERROR_INVALID;
    link_slot *slot = link_slot_new(links, SLOT_CLIENT);
    if (!slot) return TA_ERROR_NO_MEMORY;
    if (!link_slot_options(links, slot, o) || !link_text_set(links, &slot->name, o->name) ||
        !link_text_set(links, &slot->credential, o->credential)) {
        link_slot_free(links, slot);
        return TA_ERROR_NO_MEMORY;
    }
    ta_peer_config peer;
    ta_memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    peer.flags =
        ((o->flags & TA_LINK_TRUSTED) ? TA_PEER_TRUSTED : 0) | (o->max_queue ? TA_PEER_QUEUE : 0);
    peer.id = o->name;
    peer.max_queue = o->max_queue;
    peer.queue_ttl = o->queue_ttl;
    ta_status status = ta_host_peer_add(links->config.host, &peer, &slot->peer);
    if (status != TA_OK) {
        link_slot_free(links, slot);
        return status;
    }
    ta_host_peer_up(links->config.host, slot->peer, 0); /* not connected yet: envelopes queue */
    *link = link_handle_of(links, slot);
    link_emit(links, TA_LINK_OPEN, slot, (ta_string_view){"", 0});
    return TA_OK;
}

ta_status ta_links_serve(ta_links *links, const ta_link_options *o, uint32_t *server) {
    if (!link_options_ok(o) || (o->export_count && (!o->exports || !o->export_events)))
        return TA_ERROR_INVALID;
    link_slot *slot = link_slot_new(links, SLOT_SERVER);
    if (!slot) return TA_ERROR_NO_MEMORY;
    uint32_t count = 0;
    if (!link_slot_options(links, slot, o) ||
        !link_texts_set(links, &slot->exports, &slot->export_count, o->exports, o->export_count) ||
        !link_texts_set(links, &slot->export_events, &count, o->export_events, o->export_count)) {
        link_slot_free(links, slot);
        return TA_ERROR_NO_MEMORY;
    }
    *server = link_handle_of(links, slot);
    return TA_OK;
}

ta_status ta_links_accept(ta_links *links, uint32_t server, uint32_t *connection) {
    link_slot *s = link_slot_of(links, server);
    if (!s || s->kind != SLOT_SERVER) return TA_ERROR_STALE;
    link_slot *slot = link_slot_new(links, SLOT_ACCEPTED);
    if (!slot) return TA_ERROR_NO_MEMORY;
    s = link_slot_of(links, server); /* the slots may have moved */
    slot->server = server;
    slot->flags = s->flags;
    slot->max_frame = s->max_frame;
    slot->peer_max = TA_FRAME_MAX;
    slot->max_queue = s->max_queue;
    slot->resume = s->resume;
    slot->transport = 1;
    ta_string_view *ids = NULL;
    uint32_t n = s->announce_count;
    if (n && (ids = (ta_string_view *)link_alloc(links, sizeof(ta_string_view) * n)))
        for (uint32_t i = 0; i < n; i++)
            ids[i] = link_text_view(&s->announce[i]);
    int ok = !n || (ids && link_texts_set(links, &slot->announce, &slot->announce_count, ids, n));
    link_free(links, ids, sizeof(ta_string_view) * n);
    if (!ok) {
        link_slot_free(links, slot);
        return TA_ERROR_NO_MEMORY;
    }
    *connection = link_handle_of(links, slot);
    return TA_OK;
}

void ta_links_close(ta_links *links, uint32_t handle, ta_time now) {
    (void)now;
    link_slot *slot = link_slot_of(links, handle);
    if (!slot) return;
    if (slot->kind == SLOT_SERVER) {
        for (uint32_t i = 0; i < links->slot_count; i++) {
            link_slot *c = &links->slots[i];
            if (c->kind != SLOT_ACCEPTED || c->server != handle) continue;
            if (c->transport) link_emit(links, TA_LINK_CLOSE, c, (ta_string_view){"", 0});
            if (c->peer) ta_host_peer_remove(links->config.host, c->peer);
            link_slot_free(links, c);
        }
        link_slot_free(links, slot);
        link_announce(links);
        return;
    }
    if (slot->transport) link_emit(links, TA_LINK_CLOSE, slot, (ta_string_view){"", 0});
    link_forget(links, slot);
}

/* ---- what comes in --------------------------------------------------------------------- */

/* a string member's value link_decoded into the scratch buffer (empty when it is
 * none); the views stay valid until the scratch buffer is reset */
static ta_string_view link_decoded(ta_links *links, ta_string_view value) {
    link_buffer *b = &links->scratch;
    if (!value.length || value.pointer[0] != '"' || !link_buffer_room(links, b, value.length))
        return (ta_string_view){"", 0};
    int32_t n = ta_json_string(value, b->pointer + b->length, (uint32_t)value.length);
    if (n < 0) return (ta_string_view){"", 0};
    ta_string_view out = {b->pointer + b->length, (uint32_t)n};
    b->length += (size_t)n;
    return out;
}

static int link_key_is(ta_string_view key, const char *name) {
    uint32_t n = 0;
    while (name[n])
        n++;
    return key.length == n && ta_memcmp(key.pointer, name, n) == 0;
}

typedef struct hello_fields {
    uint64_t version;
    ta_string_view id;    /* link_decoded */
    ta_string_view reach; /* the array's text */
    uint64_t max_frame;
} hello_fields;

static int link_read_hello(ta_links *links, ta_string_view text, hello_fields *h) {
    ta_memset(h, 0, sizeof *h);
    uint32_t at = 0;
    ta_string_view key, value;
    int more;
    /* the scratch buffer holds the id; it must not move while the view lives */
    link_buffer_reset(&links->scratch);
    if (!link_buffer_room(links, &links->scratch, text.length)) return 0;
    for (int first = 1; (more = ta_json_member(text, &at, first, &key, &value)) == 1; first = 0) {
        if (link_key_is(key, "v"))
            ta_json_integer(value, &h->version);
        else if (link_key_is(key, "id"))
            h->id = link_decoded(links, value);
        else if (link_key_is(key, "reach") && value.length && value.pointer[0] == '[')
            h->reach = value;
        else if (link_key_is(key, "caps") && value.length && value.pointer[0] == '{') {
            uint32_t inner = 0;
            ta_string_view k2, v2;
            for (int f = 1; ta_json_member(value, &inner, f, &k2, &v2) == 1; f = 0)
                if (link_key_is(k2, "max_frame")) ta_json_integer(v2, &h->max_frame);
        }
    }
    return more == 0;
}

/* the valid system ids of a hello's reach */
static int link_set_reach(ta_links *links, link_slot *slot, ta_string_view reach) {
    link_texts_clear(links, &slot->reach, &slot->reach_count);
    uint32_t at = 0, n = 0;
    ta_string_view value;
    for (int first = 1; reach.length && ta_json_element(reach, &at, first, &value) == 1; first = 0)
        n++;
    if (!n) return 1;
    slot->reach = (link_text *)link_alloc(links, sizeof(link_text) * n);
    if (!slot->reach) return 0;
    ta_memset(slot->reach, 0, sizeof(link_text) * n);
    at = 0;
    for (int first = 1; ta_json_element(reach, &at, first, &value) == 1; first = 0) {
        char id[TA_ADDRESS_SYSTEM_MAX + 1];
        int32_t length = ta_json_string(value, id, sizeof id);
        ta_string_view v = {id, length > 0 ? (uint32_t)length : 0};
        if (length > 0 && ta_address_system_valid(v) && slot->reach_count < n)
            link_text_set(links, &slot->reach[slot->reach_count++], v);
    }
    return 1;
}

static void link_close_transport(ta_links *links, link_slot *slot) {
    link_emit(links, TA_LINK_CLOSE, slot, (ta_string_view){"", 0});
}

static void link_finish_open(ta_links *links, link_slot *slot) {
    slot->open = 1;
    link_send_hello(links, slot);
    link_peer_up(links, slot);
}

static int link_create_peer(ta_links *links, link_slot *slot, int trusted) {
    link_slot *server = link_slot_of(links, slot->server);
    uint32_t n = slot->reach_count;
    ta_string_view small[16],
        *reach = n <= 16 ? small : (ta_string_view *)link_alloc(links, sizeof(ta_string_view) * n);
    if (!reach) return 0;
    for (uint32_t i = 0; i < n; i++)
        reach[i] = link_text_view(&slot->reach[i]);
    ta_peer_config peer;
    ta_memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    peer.flags = (trusted ? TA_PEER_TRUSTED : 0) | (slot->resume ? TA_PEER_QUEUE : 0);
    peer.id = link_text_view(&slot->remote);
    peer.reach = reach;
    peer.reach_count = n;
    peer.max_queue = slot->max_queue;
    peer.queue_ttl = slot->resume;
    ta_status status = ta_host_peer_add(links->config.host, &peer, &slot->peer);
    if (reach != small) link_free(links, reach, sizeof(ta_string_view) * n);
    if (status != TA_OK) return 0;
    for (uint32_t i = 0; server && i < server->export_count; i++)
        ta_host_export(links->config.host, slot->peer, link_text_view(&server->exports[i]),
                       link_text_view(&server->export_events[i]));
    return 1;
}

static void link_hello_in(ta_links *links, link_slot *slot, ta_string_view text, ta_time now) {
    hello_fields h;
    if (!link_read_hello(links, text, &h) || h.version != LINK_VERSION ||
        !ta_address_system_valid(h.id) ||
        (slot->kind == SLOT_ACCEPTED && link_view_equal(h.id, link_text_view(&links->system_id)))) {
        link_close_transport(links, slot);
        return;
    }
    link_set_reach(links, slot, h.reach);
    slot->peer_max =
        h.max_frame && h.max_frame <= 0xffffffffu ? (uint32_t)h.max_frame : TA_FRAME_MAX;
    if (slot->kind == SLOT_CLIENT) {
        int first = !slot->open;
        link_text_set(links, &slot->remote, h.id);
        slot->open = 1;
        slot->attempts = 0;
        link_text_clear(links, &slot->error);
        link_peer_up(links, slot);
        if (first && slot->ping_interval) slot->ping_at = now; /* at once, then every interval */
        return;
    }
    if (slot->judging) return;
    if (slot->peer) { /* a hello again: new reach */
        link_peer_up(links, slot);
        return;
    }
    link_text_set(links, &slot->remote, h.id);
    for (uint32_t i = 0; i < links->slot_count; i++) {
        link_slot *before = &links->slots[i];
        if (before == slot || before->kind != SLOT_ACCEPTED || before->server != slot->server ||
            !link_view_equal(link_text_view(&before->remote), h.id) || !before->peer)
            continue;
        /* the same system again: the newer connection wins and takes over its peer */
        if (before->transport) link_close_transport(links, before);
        slot->peer = before->peer;
        before->peer = 0;
        link_slot_free(links, before);
        link_finish_open(links, slot);
        return;
    }
    if (slot->flags & TA_LINK_JUDGE) {
        slot->judging = 1;
        link_text_set(links, &slot->hello, text);
        link_emit(links, TA_LINK_HELLO, slot, text);
        return;
    }
    if (!link_create_peer(links, slot, (slot->flags & TA_LINK_TRUSTED) != 0)) {
        link_close_transport(links, slot);
        return;
    }
    link_finish_open(links, slot);
}

void ta_links_admit(ta_links *links, uint32_t connection, uint32_t verdict, ta_time now) {
    (void)now;
    link_slot *slot = link_slot_of(links, connection);
    if (!slot || !slot->judging) return;
    slot->judging = 0;
    link_text_clear(links, &slot->hello);
    if (verdict == TA_LINK_REFUSE ||
        !link_create_peer(links, slot, verdict == TA_LINK_ADMIT_TRUSTED)) {
        link_close_transport(links, slot);
        return;
    }
    link_finish_open(links, slot);
}

static void link_envelope_in(ta_links *links, link_slot *slot, ta_string_view text) {
    ta_envelope envelope;
    ta_memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    ta_string_view data = {0, 0}, key, value;
    int has_data = 0, more;
    uint32_t at = 0;
    link_buffer_reset(&links->scratch);
    if (!link_buffer_room(links, &links->scratch, text.length)) return;
    for (int first = 1; (more = ta_json_member(text, &at, first, &key, &value)) == 1; first = 0) {
        if (link_key_is(key, "target"))
            envelope.target = link_decoded(links, value);
        else if (link_key_is(key, "name"))
            envelope.name = link_decoded(links, value);
        else if (link_key_is(key, "origin"))
            envelope.origin = link_decoded(links, value);
        else if (link_key_is(key, "origintype"))
            envelope.origintype = link_decoded(links, value);
        else if (link_key_is(key, "sendid"))
            envelope.sendid = link_decoded(links, value);
        else if (link_key_is(key, "msgid"))
            envelope.msgid = link_decoded(links, value);
        else if (link_key_is(key, "dedup"))
            envelope.deduplication_key = link_decoded(links, value);
        else if (link_key_is(key, "traceparent"))
            envelope.traceparent = link_decoded(links, value);
        else if (link_key_is(key, "hops")) {
            uint64_t hops = 0;
            if (value.length && value.pointer[0] != '-' && ta_json_integer(value, &hops) &&
                hops < 0xffffffffu)
                envelope.hops = (uint32_t)hops;
        } else if (link_key_is(key, "data")) {
            data = value;
            has_data = 1;
        }
    }
    if (more != 0) return;
    const ta_datamodel_binding *model =
        links->config.datamodel_count ? &links->config.datamodels[0] : NULL;
    if (has_data && model && model->vtable->from_wire &&
        model->vtable->from_wire(model->datamodel, data, &envelope.data) != TA_OK)
        envelope.data = 0;
    uint32_t outcome = 0;
    if (ta_host_deliver(links->config.host, slot->peer, &envelope, &outcome) != TA_OK &&
        envelope.data && model && model->vtable->release)
        model->vtable->release(model->datamodel, envelope.data);
}

static uint32_t link_reason_of(ta_string_view name) {
    for (uint32_t i = 1; i < REASON_COUNT; i++)
        if (link_key_is(name, link_reasons[i])) return i;
    return TA_ROUTE_ERROR_UNREACHABLE;
}

static void link_error_in(ta_links *links, link_slot *slot, ta_string_view text) {
    ta_string_view origin = {"", 0}, msgid = {"", 0}, sendid = {"", 0}, reason = {"", 0}, key,
                   value;
    uint32_t at = 0;
    link_buffer_reset(&links->scratch);
    if (!link_buffer_room(links, &links->scratch, text.length)) return;
    for (int first = 1; ta_json_member(text, &at, first, &key, &value) == 1; first = 0) {
        if (link_key_is(key, "origin"))
            origin = link_decoded(links, value);
        else if (link_key_is(key, "msgid"))
            msgid = link_decoded(links, value);
        else if (link_key_is(key, "sendid"))
            sendid = link_decoded(links, value);
        else if (link_key_is(key, "reason"))
            reason = link_decoded(links, value);
    }
    ta_host_peer_error(links->config.host, slot->peer, origin, msgid, sendid,
                       link_reason_of(reason));
}

void ta_links_received(ta_links *links, uint32_t connection, ta_string_view text, ta_time now) {
    link_slot *slot = link_slot_of(links, connection);
    if (!slot || slot->kind == SLOT_SERVER || !slot->transport) return;
    ta_string_view key, value, type = {"", 0}, at_value = {"0", 1};
    uint32_t at = 0;
    char type_text[8];
    int more;
    for (int first = 1; (more = ta_json_member(text, &at, first, &key, &value)) == 1; first = 0) {
        if (link_key_is(key, "t")) {
            int32_t n = ta_json_string(value, type_text, sizeof type_text);
            type = (ta_string_view){type_text, n > 0 ? (uint32_t)n : 0};
        } else if (link_key_is(key, "at") && value.length && value.pointer[0] != '"') {
            at_value = value;
        }
    }
    if (more != 0 || !type.length) return; /* not a message: ignored */
    if (link_key_is(type, "hello")) {
        link_hello_in(links, slot, text, now);
    } else if (link_key_is(type, "ping")) {
        link_buffer *f = &links->frame;
        link_buffer_reset(f);
        link_put(links, f, LINK_VIEW("{\"t\":\"pong\",\"at\":"));
        link_put(links, f, at_value);
        link_put(links, f, LINK_VIEW("}"));
        if (!f->failed) link_emit(links, TA_LINK_SEND, slot, link_frame_view(links));
    } else if (link_key_is(type, "pong")) {
        uint64_t sent = 0;
        if (slot->peer && ta_json_integer(at_value, &sent) && sent <= now)
            ta_host_peer_round_trip_time(links->config.host, slot->peer,
                                         now - sent > 0xffffffffu ? 0xffffffffu
                                                                  : (uint32_t)(now - sent));
    } else if ((link_key_is(type, "ev") || link_key_is(type, "err")) && slot->peer && slot->open) {
        if (type.length == 2)
            link_envelope_in(links, slot, text);
        else
            link_error_in(links, slot, text);
    } /* ack and unknown types are ignored */
}

void ta_links_opened(ta_links *links, uint32_t connection, ta_time now) {
    (void)now;
    link_slot *slot = link_slot_of(links, connection);
    if (!slot || slot->kind != SLOT_CLIENT) return;
    slot->transport = 1;
    slot->announced_once = 0;
    link_send_hello(links, slot);
}

void ta_links_dropped(ta_links *links, uint32_t connection, ta_string_view why, ta_time now) {
    link_slot *slot = link_slot_of(links, connection);
    if (!slot || slot->kind == SLOT_SERVER) return;
    int was = slot->transport;
    slot->transport = 0;
    slot->open = 0;
    slot->judging = 0;
    if (slot->kind == SLOT_CLIENT) {
        if (!was && slot->retry_at != LINK_NEVER) return; /* already waiting to retry */
        link_text_set(links, &slot->error, why);
        ta_host_peer_up(links->config.host, slot->peer, 0);
        slot->attempts++;
        uint32_t shift = slot->attempts - 1 < 7 ? slot->attempts - 1 : 7;
        ta_time wait = (ta_time)250 << shift;
        slot->retry_at = now + (wait < 30000 ? wait : 30000);
        slot->ping_at = LINK_NEVER;
        link_announce(links);
        return;
    }
    link_slot *server = link_slot_of(links, slot->server);
    if (!slot->peer) {
        link_slot_free(links, slot);
    } else if (slot->resume && server) { /* queue for it until it is back or the window ends */
        ta_host_peer_up(links->config.host, slot->peer, 0);
        slot->expire_at = now + slot->resume;
        link_announce(links);
    } else {
        link_forget(links, slot);
    }
}

/* ---- time and effects ------------------------------------------------------------------- */

void ta_links_time(ta_links *links, ta_time now) {
    for (uint32_t i = 0; i < links->failure_count; i++) { /* envelopes refused in the effects */
        link_failure *f = &links->failures[i];
        ta_host_peer_error(links->config.host, f->peer, link_text_view(&f->origin),
                           link_text_view(&f->msgid), link_text_view(&f->sendid), f->reason);
        link_failure_clear(links, f);
    }
    links->failure_count = 0;
    for (uint32_t i = 0; i < links->slot_count; i++) {
        link_slot *slot = &links->slots[i];
        if (slot->kind == SLOT_CLIENT && slot->retry_at <= now) {
            slot->retry_at = LINK_NEVER;
            link_emit(links, TA_LINK_OPEN, slot, (ta_string_view){"", 0});
        }
        if (slot->kind == SLOT_CLIENT && slot->ping_at <= now) {
            slot->ping_at = slot->open && slot->transport ? now + slot->ping_interval : LINK_NEVER;
            if (slot->ping_at == LINK_NEVER) continue;
            link_buffer *f = &links->frame;
            link_buffer_reset(f);
            link_put(links, f, LINK_VIEW("{\"t\":\"ping\",\"at\":"));
            link_put_u64(links, f, now);
            link_put(links, f, LINK_VIEW("}"));
            if (!f->failed) link_emit(links, TA_LINK_SEND, slot, link_frame_view(links));
        }
        if (slot->kind == SLOT_ACCEPTED && slot->expire_at <= now) link_forget(links, slot);
    }
}

ta_time ta_links_deadline(const ta_links *links) {
    ta_time next = links->failure_count ? 0 : LINK_NEVER;
    for (uint32_t i = 0; i < links->slot_count; i++) {
        const link_slot *slot = &links->slots[i];
        if (slot->kind == SLOT_FREE) continue;
        if (slot->retry_at < next) next = slot->retry_at;
        if (slot->ping_at < next) next = slot->ping_at;
        if (slot->expire_at < next) next = slot->expire_at;
    }
    return next;
}

static void link_refuse(ta_links *links, const ta_host_effect *effect, uint32_t reason) {
    if (!link_grow(links, (void **)&links->failures, &links->failure_capacity,
                   links->failure_count + 1, sizeof(link_failure)))
        return;
    link_failure *f = &links->failures[links->failure_count++];
    ta_memset(f, 0, sizeof *f);
    f->peer = effect->peer;
    f->reason = reason;
    link_text_set(links, &f->origin, effect->auxiliary);
    link_text_set(links, &f->msgid, effect->msgid);
    link_text_set(links, &f->sendid, effect->sendid);
}

/* the envelope's data as neutral JSON in links->wire; 0 when it is none */
static int link_wire_of(ta_links *links, const ta_host_effect *effect, ta_string_view *out) {
    *out = (ta_string_view){"", 0};
    if (!effect->payload || effect->datamodel >= links->config.datamodel_count) return 1;
    const ta_datamodel_binding *model = &links->config.datamodels[effect->datamodel];
    if (!model->vtable->to_wire) return 0;
    link_buffer *w = &links->wire;
    link_buffer_reset(w);
    size_t length = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        ta_status status = model->vtable->to_wire(model->datamodel, effect->payload, w->pointer,
                                                  w->capacity, &length);
        if (status == TA_OK) {
            *out = (ta_string_view){w->pointer, (uint32_t)length};
            return 1;
        }
        if (status != TA_ERROR_NO_MEMORY || !link_buffer_room(links, w, length + 64)) return 0;
    }
    return 0;
}

static void link_envelope_out(ta_links *links, link_slot *slot, const ta_host_effect *effect) {
    if (!slot->open || !slot->transport) {
        link_refuse(links, effect, TA_ROUTE_ERROR_UNREACHABLE);
        return;
    }
    ta_string_view data;
    if (!link_wire_of(links, effect, &data)) { /* data that is no neutral value */
        link_refuse(links, effect, TA_ROUTE_ERROR_MALFORMED);
        return;
    }
    char traceparent[TA_TRACEPARENT_LENGTH];
    ta_envelope envelope;
    ta_memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.hops = effect->flags;
    envelope.target = effect->target;
    envelope.name = effect->event;
    envelope.sendid = effect->sendid;
    envelope.origin = effect->auxiliary;
    envelope.msgid = effect->msgid;
    envelope.traceparent = (ta_string_view){
        traceparent, ta_traceparent_format(&effect->trace, traceparent, sizeof traceparent)};
    /* our name for the other system (its alias) becomes its own id on the wire */
    link_buffer *s = &links->scratch;
    link_buffer_reset(s);
    ta_string_view alias = link_text_view(&slot->name), remote = link_text_view(&slot->remote);
    if (slot->kind == SLOT_CLIENT && remote.length && !link_view_equal(alias, remote) &&
        effect->target.length > 13 + alias.length &&
        ta_memcmp(effect->target.pointer, TA_ADDRESS_SCHEME, 13) == 0 &&
        ta_memcmp(effect->target.pointer + 13, alias.pointer, alias.length) == 0 &&
        effect->target.pointer[13 + alias.length] == '/') {
        link_put(links, s, (ta_string_view){TA_ADDRESS_SCHEME, 13});
        link_put(links, s, remote);
        link_put(links, s,
                 (ta_string_view){effect->target.pointer + 13 + alias.length,
                                  effect->target.length - 13 - alias.length});
        if (!s->failed) envelope.target = (ta_string_view){s->pointer, (uint32_t)s->length};
    }
    size_t length = ta_host_envelope_json(&envelope, data, NULL, 0);
    link_buffer *f = &links->frame;
    link_buffer_reset(f);
    if (!link_buffer_room(links, f, length + 8)) {
        link_refuse(links, effect, TA_ROUTE_ERROR_UNREACHABLE);
        return;
    }
    link_put(links, f, LINK_VIEW("{\"t\":\"ev\","));
    size_t comma = f->length - 1; /* the envelope's '{' lands here, and becomes ',' again */
    ta_host_envelope_json(&envelope, data, f->pointer + comma, length + 1);
    f->pointer[comma] = ',';
    f->length = comma + length;
    if (f->length > slot->peer_max) {
        link_refuse(links, effect, TA_ROUTE_ERROR_MALFORMED);
        return;
    }
    link_emit(links, TA_LINK_SEND, slot, link_frame_view(links));
}

static void link_error_out(ta_links *links, link_slot *slot, const ta_host_effect *effect) {
    if (!slot->open || !slot->transport) return;
    link_buffer *f = &links->frame;
    link_buffer_reset(f);
    link_put(links, f, LINK_VIEW("{\"t\":\"err\",\"origin\":"));
    link_put_string(links, f, effect->auxiliary);
    link_put(links, f, LINK_VIEW(",\"msgid\":"));
    link_put_string(links, f, effect->msgid);
    link_put(links, f, LINK_VIEW(",\"sendid\":"));
    link_put_string(links, f, effect->sendid);
    link_put(links, f, LINK_VIEW(",\"reason\":\""));
    const char *reason =
        effect->token < REASON_COUNT && effect->token ? link_reasons[effect->token] : "unreachable";
    uint32_t n = 0;
    while (reason[n])
        n++;
    link_put(links, f, (ta_string_view){reason, n});
    link_put(links, f, LINK_VIEW("\"}"));
    if (!f->failed) link_emit(links, TA_LINK_SEND, slot, link_frame_view(links));
}

int ta_links_effect(ta_links *links, const ta_host_effect *effect, ta_time now) {
    (void)now;
    if (effect->kind != TA_HOST_EFFECT_PEER && effect->kind != TA_HOST_EFFECT_PEER_ERROR) return 0;
    link_slot *slot = link_slot_of_peer(links, effect->peer);
    if (!slot) return 0;
    if (effect->kind == TA_HOST_EFFECT_PEER)
        link_envelope_out(links, slot, effect);
    else
        link_error_out(links, slot, effect);
    return 1;
}

/* ---- status ---------------------------------------------------------------------------- */

ta_status ta_links_status(const ta_links *links, uint32_t connection, ta_link_status *out) {
    const link_slot *slot = link_slot_of(links, connection);
    if (!slot || slot->kind == SLOT_SERVER) return TA_ERROR_STALE;
    ta_link_status status;
    ta_memset(&status, 0, sizeof status);
    status.size = sizeof status;
    status.open = (uint32_t)slot->open;
    status.peer = slot->peer;
    status.attempts = slot->attempts;
    status.remote = link_text_view(&slot->remote);
    status.error = link_text_view(&slot->error);
    uint32_t n = out->size && out->size < sizeof status ? out->size : (uint32_t)sizeof status;
    ta_memcpy(out, &status, n);
    out->size = n;
    return TA_OK;
}

uint32_t ta_links_connections(const ta_links *links, uint32_t server, uint32_t *out,
                              uint32_t capacity) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < links->slot_count; i++) {
        const link_slot *c = &links->slots[i];
        if (c->kind != SLOT_ACCEPTED || c->server != server || !c->remote.length || !c->peer)
            continue;
        if (n < capacity) out[n] = link_handle_of(links, c);
        n++;
    }
    return n;
}
