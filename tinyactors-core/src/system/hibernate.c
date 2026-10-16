/* hibernate.c — host-driven unloading of idle actors, and moving them
 * between systems / chart versions (hot reload). No I/O here.
 *
 * Blobs (host-owned bytes, any alignment; byte order of the writing machine
 * — blobs are for this process, not an interchange format):
 *
 *   hibernation: the actor stays (its record), its instance goes
 *     hibernation_header | instance words (ta_core_chart_instance_size) | invoke ids | datamodel
 * bytes export: the actor leaves, with the sessions it invoked (a session tree) export_header | per
 * session, in preorder (a parent before its children): session_header | instance words | invoke ids
 * | datamodel bytes | delayed sends | <data> to initialize (u32 declaration indices; a migrated
 * root) | name   (8-aligned)
 *
 * A session records its parent's session id and invoke slot — handles are
 * never stored. Delayed sends carry their payloads as values (the datamodel's
 * payload_serialize), so a blob can be adopted by another datamodel host —
 * another process, after a restart. Datamodels without that hook pass
 * payload handles: the blob holds those references, adoption takes them over
 * (a failed one leaves them with the blob), and only the writing process can
 * adopt it. Delayed sends of a tree keep their firing order (each records its
 * rank among the tree's).
 *
 * What a session's durable state is, and which path keeps which part of it:
 * docs/durability.md.
 *
 * The instance is copied verbatim except for region references, which are
 * dropped: the pinned _event (the datamodel serializes its own copy), the
 * datamodel handle, invoked children's handles (re-linked from their own
 * records), and invoke id strings (stored as text, re-interned). Queues and
 * the workspace are empty by precondition. A hibernated actor's record stays
 * resident with `instance` = chart index. */
#include "base/memory.h"
#include "system/system_internal.h"
#include "trace/record.h"

#define HIBERNATION_MAGIC   0x42484154u /* "TAHB" */
#define HIBERNATION_VERSION 3u          /* 3: the 64-byte instance header (docs/adr/0006) */

typedef struct hibernation_header {
    uint32_t magic, version;
    uint32_t chart, instance_bytes;
    uint32_t ids_bytes, pad;
    uint64_t image_hash, serial;
    uint64_t datamodel_bytes;
} hibernation_header;

static const ta_timer *timer_at(const ta_system *system, uint32_t ti) {
    return (const ta_timer *)ta_pool_at(&system->timers, ti);
}

static int datamodel_can_hibernate(const ta_datamodel_binding *binding) {
    const ta_datamodel_vtable *vtable = binding->vtable;
    return vtable->size >=
               offsetof(ta_datamodel_vtable, deserialize) + sizeof vtable->deserialize &&
           vtable->serialize && vtable->deserialize;
}

/* The invoked SCXML child in slot >= `slot0` of actor `index` that links back to
 * it (TA_NIL if none). */
static uint32_t child_from(const ta_system *system, uint32_t index, uint32_t slot0) {
    const ta_actor *actor = ta_actor_at(system, index);
    if (actor->state == TA_ACTOR_HIBERNATED) return TA_NIL; /* no instance (and no children) */
    ta_instance *instance = ta_actor_instance(system, actor);
    const ta_chart_record *chart = &system->core.image_view.charts[instance->chart];
    const uint32_t *kids = ta_instance_word(instance, ta_instance_layout_of(chart).invoke_children);
    uint32_t self = ta_pool_handle(&system->actors, index);
    for (uint32_t i = slot0; i < chart->invoke_count; i++) {
        if (!kids[i] || (kids[i] & TA_EXTERNAL_INVOKE_BIT)) continue;
        uint32_t k = ta_system_resolve_actor(system, kids[i]);
        if (k != TA_NIL && ta_actor_at(system, k)->parent == self &&
            ta_actor_at(system, k)->parent_slot == i)
            return k;
    }
    return TA_NIL;
}

/* The session tree of `root` (root, its invoked children, theirs, ...)
 * without a stack: preorder (parents first) for writing, post-order
 * (children first) for removing — post_next reads only records and the
 * instances of ancestors, which are removed after their descendants. */
static uint32_t tree_next(const ta_system *system, uint32_t root, uint32_t index) {
    uint32_t c = child_from(system, index, 0);
    if (c != TA_NIL) return c;
    while (index != root) {
        const ta_actor *actor = ta_actor_at(system, index);
        uint32_t p = ta_system_resolve_actor(system, actor->parent);
        uint32_t sib = child_from(system, p, actor->parent_slot + 1);
        if (sib != TA_NIL) return sib;
        index = p;
    }
    return TA_NIL;
}
static uint32_t leftmost_leaf(const ta_system *system, uint32_t index) {
    for (uint32_t c; (c = child_from(system, index, 0)) != TA_NIL;)
        index = c;
    return index;
}
static uint32_t post_next(const ta_system *system, uint32_t root, uint32_t index) {
    if (index == root) return TA_NIL;
    const ta_actor *actor = ta_actor_at(system, index);
    uint32_t p = ta_system_resolve_actor(system, actor->parent);
    uint32_t sib = child_from(system, p, actor->parent_slot + 1);
    return sib != TA_NIL ? leftmost_leaf(system, sib) : p;
}

/* The place of timer ti in the firing order of all pending delayed sends of
 * the tree of `root` (sessions hold few timers). */
static uint32_t tree_rank(const ta_system *system, uint32_t root, uint32_t ti) {
    const ta_heap_node *me = ta_heap_find(&system->heap, ti);
    uint32_t r = 0;
    for (uint32_t m = root; m != TA_NIL; m = tree_next(system, root, m))
        for (uint32_t t = ta_actor_at(system, m)->timers; t != TA_NIL;
             t = timer_at(system, t)->owner_next) {
            const ta_heap_node *node = ta_heap_find(&system->heap, t);
            if (node && me && ta_heap_before(node, me)) r++;
        }
    return r;
}

/* Idle at a stable point, nothing queued, no request that will write into
 * it. Invoked children: none for hibernation (they read the parent's
 * instance); for export, only SCXML sessions of this system linked back to
 * it (they leave with it) — an invocation the host carries out (external
 * invoke) cannot move. */
static ta_status stable_member(const ta_system *system, uint32_t index, int children) {
    const ta_actor *actor = ta_actor_at(system, index);
    if (actor->state != TA_ACTOR_IDLE || actor->mailbox_length ||
        (actor->flags & ACTOR_FLAG_CANCEL))
        return TA_ERROR_STATE;
    ta_instance *instance = ta_actor_instance(system, actor);
    if (instance->phase != PHASE_STABLE || instance->workspace != TA_NIL ||
        instance->internal_queue_head != TA_NIL || (instance->flags & INSTANCE_FLAG_CANCEL))
        return TA_ERROR_STATE;
    const ta_chart_record *chart = &system->core.image_view.charts[instance->chart];
    ta_instance_layout l = ta_instance_layout_of(chart);
    uint32_t self = ta_pool_handle(&system->actors, index);
    for (uint32_t i = 0; i < chart->invoke_count; i++) {
        uint32_t k = ta_instance_word(instance, l.invoke_children)[i];
        if (!k) continue;
        if (!children || (k & TA_EXTERNAL_INVOKE_BIT)) return TA_ERROR_STATE;
        /* gone without done.invoke (it faulted or was terminated): not part
         * of the tree. A child that finished normally left done.invoke in
         * our mailbox, which is empty here. */
        uint32_t ki = ta_system_resolve_actor(system, k);
        if (ki == TA_NIL) continue;
        if (ta_actor_at(system, ki)->parent != self || ta_actor_at(system, ki)->parent_slot != i)
            return TA_ERROR_STATE;
    }
    for (uint32_t i = 0; i < system->requests.high_water_mark; i++)
        if (ta_pool_live(&system->requests, i) &&
            ((const ta_request *)ta_pool_at(&system->requests, i))->actor == self)
            return TA_ERROR_STATE;
    return datamodel_can_hibernate(ta_core_datamodel(&system->core, instance->chart))
               ? TA_OK
               : TA_ERROR_UNSUPPORTED;
}

static ta_status hibernatable(const ta_system *system, uint32_t index) {
    return stable_member(system, index, 0);
}

static uint32_t align4(uint32_t n) {
    return (n + 3u) & ~3u;
}

/* ---- blob formats ------------------------------------------------------------------ */

#define EXPORT_MAGIC    0x58454154u /* "TAEX" */
#define EXPORT_VERSION  3u /* 3: the 64-byte instance header, delayed sends' trace contexts */
#define EXPORT_MIGRATED 1u /* translated by ta_blob_migrate: one session, to be upgraded */

typedef struct export_header {
    uint32_t magic, version;
    uint32_t session_count, flags;
    uint64_t image_hash, serial; /* the root's session id */
} export_header;

typedef struct session_header {
    uint64_t serial, parent; /* the parent's session id (0: the root) */
    uint64_t datamodel_bytes;
    uint32_t chart, slot; /* the parent's invoke slot (TA_NIL: the root) */
    uint32_t instance_bytes, ids_bytes;
    uint32_t timer_count, timers_bytes;
    uint32_t init_count, name_length;
    uint32_t bytes, pad; /* the whole record, 8-aligned */
} session_header;

/* payload of a delayed send in a blob */
enum { TP_NONE = 0, TP_BYTES = 1, TP_HANDLE = 2 };

typedef struct blob_timer { /* followed by target, name and sendid text, then the payload
                               bytes (TP_BYTES; each 4-aligned) */
    uint64_t due, payload;  /* payload: the handle (TP_HANDLE) */
    uint32_t route, arg, type, flags;
    uint32_t rank; /* place in the firing order of the tree's delayed sends */
    uint32_t payload_kind, payload_length;
    uint32_t target_length, name_length, sendid_length; /* TA_NIL length: none */
    uint32_t trace_flags;                               /* its trace context (all 0: none) */
    uint64_t trace_high, trace_low, cause_session, cause_macrostep;
    uint32_t cause_microstep, pad;
} blob_timer;

static int datamodel_payload_bytes(const ta_datamodel_binding *binding) {
    const ta_datamodel_vtable *vtable = binding->vtable;
    return vtable->size >= offsetof(ta_datamodel_vtable, payload_deserialize) +
                               sizeof vtable->payload_deserialize &&
           vtable->payload_serialize && vtable->payload_deserialize;
}

static size_t align8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

static uint32_t text_bytes(uint32_t length) {
    return length == TA_NIL ? 0 : align4(length);
}

static unsigned char *put_text(unsigned char *q, ta_string_view v, uint32_t length) {
    if (length == TA_NIL) return q;
    ta_memcpy(q, v.pointer, v.length);
    ta_memset(q + v.length, 0, align4(v.length) - v.length);
    return q + align4(v.length);
}

static uint32_t string_ref_length(const ta_core *core, uint32_t string_ref) {
    return string_ref == TA_NIL ? TA_NIL : ta_shared_string_get(core, string_ref).length;
}

/* The pending delayed sends of actor `index` in firing order: the i-th smallest
 * (due, seq), by selection (actors hold few timers). */
static uint32_t nth_timer(const ta_system *system, const ta_actor *actor, uint32_t after_ti) {
    const ta_heap_node *last =
        after_ti == TA_NIL ? (const ta_heap_node *)0 : ta_heap_find(&system->heap, after_ti);
    uint32_t best = TA_NIL;
    const ta_heap_node *bn = (const ta_heap_node *)0;
    for (uint32_t ti = actor->timers; ti != TA_NIL;
         ti = ((const ta_timer *)ta_pool_at(&system->timers, ti))->owner_next) {
        const ta_heap_node *node = ta_heap_find(&system->heap, ti);
        if (!node) continue;
        if (last && !ta_heap_before(last, node)) continue;
        if (!bn || ta_heap_before(node, bn)) {
            bn = node;
            best = ti;
        }
    }
    return best;
}

/* ---- blob writing ------------------------------------------------------------------ */

/* instance words + invoke ids of an instance: their sizes */
static void body_sizes(const ta_core *core, const ta_instance *instance, uint32_t *ib,
                       uint32_t *idsz) {
    const ta_chart_record *chart = &core->image_view.charts[instance->chart];
    const uint32_t *ids =
        ta_instance_word((ta_instance *)instance, ta_instance_layout_of(chart).invoke_ids);
    *ib = ta_core_chart_instance_size(core, instance->chart);
    *idsz = 4;
    for (uint32_t i = 0; i < chart->invoke_count; i++)
        if (ids[i] != TA_NIL) *idsz += 8 + align4(ta_shared_string_get(core, ids[i]).length);
}

/* write them at q (region references dropped); returns the end */
static unsigned char *put_body(const ta_core *core, const ta_instance *instance, unsigned char *q) {
    const ta_chart_record *chart = &core->image_view.charts[instance->chart];
    ta_instance_layout l = ta_instance_layout_of(chart);
    const uint32_t *ids = ta_instance_word((ta_instance *)instance, l.invoke_ids);
    uint32_t ib = ta_core_chart_instance_size(core, instance->chart);
    unsigned char *ip = q;
    ta_memcpy(ip, instance, ib);
    ta_instance copy;
    ta_memcpy(&copy, instance, sizeof copy);
    copy.current_event = TA_NIL;
    copy.datamodel = 0;
    ta_memcpy(ip, &copy, sizeof copy);
    q = ip + ib;
    uint32_t id_count = 0;
    unsigned char *count = q;
    q += 4;
    for (uint32_t i = 0; i < chart->invoke_count; i++) {
        uint32_t nil = TA_NIL, none = 0;
        ta_memcpy(ip + (size_t)(l.invoke_ids + i) * 4, &nil, 4);
        ta_memcpy(ip + (size_t)(l.invoke_children + i) * 4, &none, 4);
        if (ids[i] == TA_NIL) continue;
        ta_string_view v = ta_shared_string_get(core, ids[i]);
        ta_memcpy(q, &i, 4);
        ta_memcpy(q + 4, &v.length, 4);
        q = put_text(q + 8, v, v.length);
        id_count++;
    }
    ta_memcpy(count, &id_count, 4);
    return q;
}

/* `n` bytes at the output's end, to write in place; NULL when they do not
 * fit (they are counted all the same). */
static unsigned char *blob_take(ta_output *out, size_t n) {
    unsigned char *at = ta_output_reserve(out, n) ? out->bytes + out->length : (unsigned char *)0;
    out->length += n;
    return at;
}

/* Text padded to 4 bytes (nothing for TA_NIL). */
static void blob_text(ta_output *out, ta_string_view v, uint32_t length) {
    if (length == TA_NIL) return;
    unsigned char *at = blob_take(out, align4(v.length));
    if (at) put_text(at, v, length);
}

static void blob_zeros(ta_output *out, size_t n) {
    unsigned char *at = blob_take(out, n);
    if (at) ta_memset(at, 0, n);
}

/* The instance words and invoke ids of an instance. */
static void blob_body(const ta_core *core, const ta_instance *instance, ta_output *out) {
    uint32_t ib, idsz;
    body_sizes(core, instance, &ib, &idsz);
    unsigned char *at = blob_take(out, (size_t)ib + idsz);
    if (at) put_body(core, instance, at);
}

/* The datamodel's state of an instance, copied from the view serialize
 * gives (before anything else calls the datamodel); its length in *length. */
static ta_status blob_datamodel(const ta_core *core, const ta_instance *instance, ta_output *out,
                                  uint64_t *length) {
    const ta_datamodel_binding *binding = ta_core_datamodel(core, instance->chart);
    ta_bytes state = {(const void *)0, 0};
    ta_status status = binding->vtable->serialize(
        &(ta_datamodel_call){binding->datamodel, instance->datamodel, 0}, &state);
    if (status != TA_OK) return status;
    ta_output_write(out, state.pointer, state.length);
    *length = state.length;
    return TA_OK;
}

/* The hibernation blob of actor `index`, its header filled last (nothing
 * changes yet). */
static ta_status write_hib(ta_system *system, uint32_t index, ta_output *out) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    ta_instance *instance = ta_actor_instance(system, actor);
    uint32_t ib, idsz;
    body_sizes(core, instance, &ib, &idsz);
    hibernation_header header;
    ta_memset(&header, 0, sizeof header);
    header.magic = HIBERNATION_MAGIC;
    header.version = HIBERNATION_VERSION;
    header.chart = instance->chart;
    header.instance_bytes = ib;
    header.ids_bytes = idsz;
    header.image_hash = core->image_view.hash;
    header.serial = actor->serial;
    size_t start = out->length;
    ta_output_write(out, &header, sizeof header);
    blob_body(core, instance, out);
    ta_status status = blob_datamodel(core, instance, out, &header.datamodel_bytes);
    if (status != TA_OK) return status;
    ta_output_patch(out, start, (ta_bytes){&header, sizeof header});
    return TA_OK;
}

/* A delayed send of an export blob: its record, texts and payload. Adds its
 * bytes to *bytes. */
static ta_status write_timer(ta_system *system, uint32_t root, uint32_t ti, ta_output *out,
                             uint64_t *bytes) {
    ta_core *core = &system->core;
    const ta_timer *timer = timer_at(system, ti);
    const ta_event *event = ta_event_at(core, timer->event);
    size_t start = out->length;
    blob_timer r;
    ta_memset(&r, 0, sizeof r);
    r.due = ta_heap_find(&system->heap, ti)->due;
    r.rank = tree_rank(system, root, ti);
    const ta_datamodel_binding *pb = event->payload && event->datamodel < core->datamodel_count
                                         ? &core->datamodels[event->datamodel]
                                         : 0;
    ta_bytes value = {(const void *)0, 0};
    if (pb && datamodel_payload_bytes(pb)) { /* the value itself, written once */
        ta_status status = pb->vtable->payload_serialize(pb->datamodel, event->payload, &value);
        if (status != TA_OK) return status;
        if (value.length >= TA_NIL) return TA_ERROR_UNSUPPORTED;
        r.payload_kind = TP_BYTES;
        r.payload_length = (uint32_t)value.length;
    } else if (pb) { /* a handle: this process only */
        r.payload_kind = TP_HANDLE;
        r.payload = event->payload;
    }
    /* a send to an invoked child (#_<invokeid>) holds the child's handle:
     * stored as the owner's invoke slot + 1 (0: the child is gone), which
     * adoption maps to the child adopted with it */
    r.route = timer->route;
    r.arg = timer->arg;
    if (timer->route == TA_ROUTE_CHILD) {
        const ta_actor *owner = ta_actor_at(system, ta_system_resolve_actor(system, timer->owner));
        const ta_instance *instance = ta_actor_instance(system, (ta_actor *)owner);
        const ta_chart_record *chart = &core->image_view.charts[instance->chart];
        const uint32_t *kids =
            ta_instance_word((ta_instance *)instance, ta_instance_layout_of(chart).invoke_children);
        r.arg = 0;
        for (uint32_t i = 0; i < chart->invoke_count && !r.arg; i++)
            if (kids[i] == timer->arg && !(timer->arg & TA_EXTERNAL_INVOKE_BIT)) r.arg = i + 1;
    }
    r.type = event->type;
    r.flags = event->flags;
    if (event->trace != TA_NIL) { /* the context goes as a value (docs/durability.md) */
        const ta_trace_entry *entry = ta_trace_entry_at(core, event->trace);
        r.trace_high = entry->trace.high;
        r.trace_low = entry->trace.low;
        r.trace_flags = entry->flags;
        r.cause_session = entry->session;
        r.cause_macrostep = entry->macrostep;
        r.cause_microstep = entry->microstep;
    }
    r.target_length = string_ref_length(core, timer->target);
    r.name_length = string_ref_length(core, event->name);
    r.sendid_length = string_ref_length(core, event->sendid);
    ta_output_write(out, &r, sizeof r);
    blob_text(out, ta_shared_string_get(core, timer->target), r.target_length);
    blob_text(out, ta_shared_string_get(core, event->name), r.name_length);
    blob_text(out, ta_shared_string_get(core, event->sendid), r.sendid_length);
    if (r.payload_kind == TP_BYTES) {
        ta_output_write(out, value.pointer, value.length);
        blob_zeros(out, align4(r.payload_length) - r.payload_length);
    }
    *bytes += out->length - start;
    return TA_OK;
}

/* The session record of actor `index` in an export blob, its header filled
 * last (nothing changes yet): its state, delayed sends and name, and its
 * place in the tree. */
static ta_status write_session(ta_system *system, uint32_t index, uint32_t root, ta_output *out) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    ta_instance *instance = ta_actor_instance(system, actor);
    uint32_t ib, idsz;
    body_sizes(core, instance, &ib, &idsz);
    session_header h;
    ta_memset(&h, 0, sizeof h);
    h.serial = actor->serial;
    h.slot = TA_NIL;
    if (index != root) {
        h.parent = ta_actor_at(system, ta_system_resolve_actor(system, actor->parent))->serial;
        h.slot = actor->parent_slot;
    }
    h.chart = instance->chart;
    h.instance_bytes = ib;
    h.ids_bytes = idsz;
    h.name_length = ta_shared_string_get(core, actor->name).length;
    size_t start = out->length;
    ta_output_write(out, &h, sizeof h);
    blob_body(core, instance, out);
    ta_status status = blob_datamodel(core, instance, out, &h.datamodel_bytes);
    if (status != TA_OK) return status;
    uint64_t timer_bytes = 0;
    for (uint32_t ti = nth_timer(system, actor, TA_NIL); ti != TA_NIL;
         ti = nth_timer(system, actor, ti)) {
        status = write_timer(system, root, ti, out, &timer_bytes);
        if (status != TA_OK) return status;
        h.timer_count++;
    }
    if (timer_bytes > 0xFFFFFFFFu) return TA_ERROR_UNSUPPORTED;
    h.timers_bytes = (uint32_t)timer_bytes;
    blob_text(out, ta_shared_string_get(core, actor->name), h.name_length);
    size_t used = out->length - start, bytes = align8(used);
    if (bytes > 0xFFFFFFFFu) return TA_ERROR_UNSUPPORTED;
    blob_zeros(out, bytes - used);
    h.bytes = (uint32_t)bytes;
    ta_output_patch(out, start, (ta_bytes){&h, sizeof h});
    return TA_OK;
}

/* A lifecycle record of hibernation (TA_TRACE_UNLOADED, TA_TRACE_LOADED):
 * outside every macrostep, in a trace of its own. */
static void trace_residency(ta_system *system, uint32_t index, uint32_t kind, uint64_t macrostep) {
    ta_trace_context none;
    ta_memset(&none, 0, sizeof none);
    ta_trace_record *record = ta_system_trace_lifecycle(
        system, index, kind, (uint32_t)(sizeof *record + sizeof(ta_trace_lifecycle)), &none,
        macrostep, kind == TA_TRACE_UNLOADED ? TA_TRACE_STEP_UNLOADED : TA_TRACE_STEP_LOADED);
    if (!record) return;
    ta_trace_lifecycle *body = (ta_trace_lifecycle *)ta_trace_record_body(record);
    body->name = ta_actor_at(system, index)->name;
    ta_shared_string_retain(&system->core, body->name);
    ta_trace_record_commit(system->core.journal, record);
}

/* Free the instance of a (blob-written) actor; the record stays, hibernated. */
static void unload(ta_system *system, uint32_t index) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    ta_instance *instance = ta_actor_instance(system, actor);
    trace_residency(system, index, TA_TRACE_UNLOADED, instance->macrostep);
    uint32_t chart = instance->chart;
    const ta_chart_record *chart_record = &core->image_view.charts[chart];
    uint32_t *ids = ta_instance_word(instance, ta_instance_layout_of(chart_record).invoke_ids);
    const ta_datamodel_binding *binding = ta_core_datamodel(core, chart);
    if (instance->current_event != TA_NIL) ta_event_release(core, instance->current_event);
    for (uint32_t i = 0; i < chart_record->invoke_count; i++)
        ta_shared_string_release(core, ids[i]);
    if (binding->vtable->destroy)
        binding->vtable->destroy(&(ta_datamodel_call){binding->datamodel, instance->datamodel, 0});
    ta_slab_free(&system->slab, actor->instance);
    actor->instance = chart;
    actor->state = TA_ACTOR_HIBERNATED;
    actor->flags &= (uint8_t)~(ACTOR_FLAG_IN_WAKE_QUEUE | ACTOR_FLAG_WAKE_SENT | ACTOR_FLAG_TOUCHED |
                               ACTOR_FLAG_WATCHED); /* a watch ends here (ta_actor_watch) */
    system->hibernated_count++;
    core->chart_stats[chart].hibernated++;
}

/* One pass into `out` (system_internal.h): serialize is called once and its
 * bytes copied into `out` before destroy. A growing `out` (the host's
 * output) calls its allocator here, outside a run only: in_call refuses one,
 * and the host takes its output before its guard. */
ta_status ta_actor_write_hibernation(ta_system *system, uint32_t actor, ta_output *out) {
    if (system->in_call) return TA_ERROR_STATE; /* inside a run: nothing allocates */
    uint32_t index = ta_system_resolve_actor(system, actor);
    if (index == TA_NIL) return TA_ERROR_STALE;
    ta_status status = hibernatable(system, index);
    if (status == TA_OK) status = write_hib(system, index, out);
    if (status != TA_OK) return status;
    if (!ta_output_fits(out)) return TA_ERROR_NO_MEMORY; /* nothing changed */
    unload(system, index); /* the datamodel's state was copied before destroy */
    system->stats_hibernations++;
    return TA_OK;
}

/* The public call keeps ta_buffer (docs/adr/0010, decision 2): a system has
 * no allocator, so the output is the caller's room alone. When it is too
 * small nothing changes and out->length is what it needs; the caller grows
 * the room and calls again, and serialize runs again then. The host avoids
 * that second run by growing its own output instead. */
ta_status ta_actor_hibernate(ta_system *system, uint32_t actor, ta_buffer *out) {
    ta_output output = ta_output_over(*out, (const ta_allocator *)0);
    ta_status status = ta_actor_write_hibernation(system, actor, &output);
    out->length = output.length;
    return status;
}

/* ---- export ---------------------------------------------------------------------- */

/* Remove an exported actor silently (no done event, no effects); its delayed
 * sends left with the blob: payloads written as values are released here,
 * handles now belong to the blob. */
static void leave(ta_system *system, uint32_t index) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    while (actor->timers != TA_NIL) {
        uint32_t ti = actor->timers;
        ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
        actor->timers = timer->owner_next;
        ta_heap_remove(&system->heap, ti);
        ta_event *event = ta_event_at(core, timer->event);
        if (event->payload && !(event->datamodel < core->datamodel_count &&
                                datamodel_payload_bytes(&core->datamodels[event->datamodel])))
            event->payload = 0;
        ta_event_release(core, timer->event);
        ta_shared_string_release(core, timer->target);
        ta_pool_free(&system->timers, ti);
    }
    unload(system, index);
    /* it leaves, it is not destroyed: it goes on where the blob is adopted */
    ta_system_reclaim(system, index, RECLAIM_LEFT);
}

/* One pass into `out` (system_internal.h): serialize once per session and
 * payload_serialize once per delayed send, the header patched last. A
 * growing `out` (the host's output, or its blob scratch for migration and
 * adoption) calls its allocator here, outside a run only, as for
 * ta_actor_write_hibernation. */
ta_status ta_actor_write_export(ta_system *system, uint32_t actor, ta_output *out) {
    if (system->in_call) return TA_ERROR_STATE; /* inside a run: nothing allocates */
    uint32_t root = ta_system_resolve_actor(system, actor);
    if (root == TA_NIL) return TA_ERROR_STALE;
    if (ta_actor_at(system, root)->parent)
        return TA_ERROR_STATE; /* sessions leave with their root */
    for (uint32_t m = root; m != TA_NIL; m = tree_next(system, root, m)) {
        ta_status status = stable_member(system, m, 1);
        if (status != TA_OK) return status;
    }
    export_header h;
    ta_memset(&h, 0, sizeof h);
    h.magic = EXPORT_MAGIC;
    h.version = EXPORT_VERSION;
    h.image_hash = system->core.image_view.hash;
    h.serial = ta_actor_at(system, root)->serial;
    size_t start = out->length;
    ta_output_write(out, &h, sizeof h);
    for (uint32_t m = root; m != TA_NIL; m = tree_next(system, root, m), h.session_count++) {
        ta_status status = write_session(system, m, root, out);
        if (status != TA_OK) return status;
    }
    ta_output_patch(out, start, (ta_bytes){&h, sizeof h});
    if (!ta_output_fits(out)) return TA_ERROR_NO_MEMORY; /* nothing changed */
    for (uint32_t m = leftmost_leaf(system, root), nx; m != TA_NIL; m = nx) {
        nx = post_next(system, root, m); /* before m goes */
        leave(system, m);
    }
    return TA_OK;
}

/* ta_buffer, as ta_actor_hibernate: the caller's room, no allocator. */
ta_status ta_actor_export(ta_system *system, uint32_t actor, ta_buffer *out) {
    ta_output output = ta_output_over(*out, (const ta_allocator *)0);
    ta_status status = ta_actor_write_export(system, actor, &output);
    out->length = output.length;
    return status;
}

/* ---- blob reading ---------------------------------------------------------------------- */

/* a session of a blob, parsed: hibernation (no timers, init or name) or export */
typedef struct blob_view {
    uint32_t chart, instance_bytes, ids_bytes, slot;
    uint32_t timer_count, timers_bytes, init_count, name_length;
    uint64_t serial, parent, datamodel_bytes, image_hash;
    const unsigned char *instance, *ids, *datamodel, *timers, *init, *name;
} blob_view;

static int body_ok(const blob_view *view) {
    return view->ids_bytes >= 4 && view->instance_bytes >= sizeof(ta_instance);
}

static ta_status parse_hib(const void *blob, size_t length, blob_view *view) {
    const unsigned char *p = (const unsigned char *)blob;
    hibernation_header header;
    ta_memset(view, 0, sizeof *view);
    if (!p || length < sizeof header) return TA_ERROR_INVALID;
    ta_memcpy(&header, p, sizeof header);
    view->chart = header.chart;
    view->instance_bytes = header.instance_bytes;
    view->ids_bytes = header.ids_bytes;
    view->serial = header.serial;
    view->datamodel_bytes = header.datamodel_bytes;
    view->image_hash = header.image_hash;
    if (header.magic != HIBERNATION_MAGIC || header.version != HIBERNATION_VERSION ||
        !body_ok(view) || header.datamodel_bytes > length ||
        length < sizeof header + (uint64_t)header.instance_bytes + header.ids_bytes +
                     header.datamodel_bytes)
        return TA_ERROR_INVALID;
    view->instance = p + sizeof header;
    view->ids = view->instance + view->instance_bytes;
    view->datamodel = view->ids + view->ids_bytes;
    return TA_OK;
}

/* the next session record of an export blob at *q (advanced); 0 if malformed */
static int next_session(const unsigned char **q, const unsigned char *end, uint64_t image_hash,
                        blob_view *view) {
    session_header h;
    ta_memset(view, 0, sizeof *view);
    if ((size_t)(end - *q) < sizeof h) return 0;
    ta_memcpy(&h, *q, sizeof h);
    view->chart = h.chart;
    view->instance_bytes = h.instance_bytes;
    view->ids_bytes = h.ids_bytes;
    view->slot = h.slot;
    view->timer_count = h.timer_count;
    view->timers_bytes = h.timers_bytes;
    view->init_count = h.init_count;
    view->name_length = h.name_length;
    view->serial = h.serial;
    view->parent = h.parent;
    view->datamodel_bytes = h.datamodel_bytes;
    view->image_hash = image_hash;
    if (h.bytes < sizeof h || h.bytes > (size_t)(end - *q) || !body_ok(view) ||
        h.datamodel_bytes > h.bytes || h.init_count > h.bytes ||
        h.bytes < sizeof h + (uint64_t)h.instance_bytes + h.ids_bytes + h.datamodel_bytes +
                      h.timers_bytes + (uint64_t)h.init_count * 4 + align4(h.name_length))
        return 0;
    view->instance = *q + sizeof h;
    view->ids = view->instance + view->instance_bytes;
    view->datamodel = view->ids + view->ids_bytes;
    view->timers = view->datamodel + view->datamodel_bytes;
    view->init = view->timers + view->timers_bytes;
    view->name = view->init + (size_t)view->init_count * 4;
    *q += h.bytes;
    return 1;
}

/* Allocate and fill the instance of actor `index` (record set up, chart chosen)
 * from a parsed blob: instance words, invoke ids, datamodel state. Returns
 * the slab offset in *offset. On failure nothing is left allocated. */
static ta_status restore(ta_system *system, uint32_t index, uint32_t chart, const blob_view *view,
                         uint32_t *offset_out) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    const ta_chart_record *chart_record = &core->image_view.charts[chart];
    ta_instance_layout l = ta_instance_layout_of(chart_record);
    const ta_datamodel_binding *binding = ta_core_datamodel(core, chart);
    if (!datamodel_can_hibernate(binding)) return TA_ERROR_UNSUPPORTED;
    uint32_t offset = ta_slab_alloc(&system->slab, view->instance_bytes);
    if (offset == TA_NIL) return TA_ERROR_NO_MEMORY;
    ta_instance *instance = (ta_instance *)ta_slab_pointer(&system->slab, offset);
    ta_memcpy(instance, view->instance, view->instance_bytes);
    instance->actor = ta_pool_handle(&system->actors, index);
    instance->chart = chart;
    instance->current_event = instance->internal_queue_head = instance->internal_queue_tail =
        instance->workspace = TA_NIL;
    instance->datamodel = 0;
    instance->trace = TA_NIL; /* a blob is written at a stable point, outside every macrostep */
    uint32_t *ids = ta_instance_word(instance, l.invoke_ids);
    for (uint32_t i = 0; i < chart_record->invoke_count; i++)
        ids[i] = TA_NIL;
    const unsigned char *q = view->ids, *qe = q + view->ids_bytes;
    uint32_t id_count;
    ta_memcpy(&id_count, q, 4);
    q += 4;
    ta_status status = TA_OK;
    for (uint32_t k = 0; k < id_count && status == TA_OK; k++) {
        uint32_t slot, n;
        if (q + 8 > qe) {
            status = TA_ERROR_INVALID;
            break;
        }
        ta_memcpy(&slot, q, 4);
        ta_memcpy(&n, q + 4, 4);
        if (slot >= chart_record->invoke_count || n > (uint32_t)(qe - q - 8) ||
            n > TA_SYSTEM_MAX_STRING) {
            status = TA_ERROR_INVALID;
            break;
        }
        ids[slot] = ta_shared_string_new(core, (const char *)q + 8, n);
        if (ids[slot] == TA_NIL) status = TA_ERROR_NO_MEMORY;
        q += 8 + align4(n);
    }
    if (status == TA_OK) {
        ta_system_variables_buffer buffer;
        ta_datamodel_system_variables variables;
        ta_system_actor_variables(system, actor, &buffer, &variables);
        ta_datamodel_start start = {core->chart_context[chart],
                                    &variables,
                                    0,
                                    {view->datamodel, (size_t)view->datamodel_bytes}};
        status = binding->vtable->deserialize(binding->datamodel, &start, &instance->datamodel);
    }
    if (status != TA_OK) {
        for (uint32_t i = 0; i < chart_record->invoke_count; i++)
            ta_shared_string_release(core, ids[i]);
        ta_slab_free(&system->slab, offset);
        return status;
    }
    *offset_out = offset;
    return TA_OK;
}

ta_status ta_actor_wake(ta_system *system, uint32_t actor, ta_bytes bytes) {
    if (system->in_call) return TA_ERROR_STATE;
    const void *blob = bytes.pointer;
    size_t length = bytes.length;
    ta_core *core = &system->core;
    uint32_t index = ta_system_resolve_actor(system, actor);
    if (index == TA_NIL) return TA_ERROR_STALE;
    ta_actor *actor_record = ta_actor_at(system, index);
    if (actor_record->state != TA_ACTOR_HIBERNATED) return TA_ERROR_STATE;
    blob_view view;
    uint32_t chart = actor_record->instance, off;
    if (parse_hib(blob, length, &view) != TA_OK || view.chart != chart ||
        view.serial != actor_record->serial || view.image_hash != core->image_view.hash ||
        view.instance_bytes != ta_core_chart_instance_size(core, chart))
        return TA_ERROR_INVALID;
    ta_status status = restore(system, index, chart, &view, &off);
    if (status != TA_OK) return status;
    ta_instance *instance = (ta_instance *)ta_slab_pointer(&system->slab, off);
    actor_record->instance = off;
    if (actor_record->flags & ACTOR_FLAG_IN_WAKE_QUEUE) { /* woken before the request was emitted */
        uint32_t prev = TA_NIL, cur = system->wake_queue.head;
        while (cur != index) {
            prev = cur;
            cur = ta_actor_at(system, cur)->next;
        }
        if (prev == TA_NIL)
            system->wake_queue.head = actor_record->next;
        else
            ta_actor_at(system, prev)->next = actor_record->next;
        if (system->wake_queue.tail == index) system->wake_queue.tail = prev;
    }
    if (actor_record->flags & ACTOR_FLAG_WAKE_SENT) system->pending_wake_count--;
    actor_record->flags =
        (uint8_t)((actor_record->flags & ~(ACTOR_FLAG_IN_WAKE_QUEUE | ACTOR_FLAG_WAKE_SENT)) |
                  ACTOR_FLAG_TOUCHED);
    actor_record->state = TA_ACTOR_IDLE;
    system->hibernated_count--;
    system->stats_wakes++;
    core->chart_stats[instance->chart].hibernated--;
    trace_residency(system, index, TA_TRACE_LOADED, instance->macrostep);
    /* error.communication reported while hibernated belongs to the internal
     * queue (processed before the mailbox, as if raised at the time) */
    uint32_t prev = TA_NIL;
    for (uint32_t event_index = actor_record->mailbox_head; event_index != TA_NIL;) {
        ta_event *event = ta_event_at(core, event_index);
        uint32_t nx = event->next;
        if (event->flags & TA_EVENT_FLAG_RAISE_ERROR_COMMUNICATION) {
            uint32_t raised =
                ta_core_raise_platform(core, instance, "error.communication", event->sendid);
            if (raised != TA_NIL && ta_event_at(core, raised)->trace == TA_NIL) {
                ta_event_at(core, raised)->trace = event->trace; /* the failed send's cause */
                ta_trace_context_retain(core, event->trace);
            }
            if (prev == TA_NIL)
                actor_record->mailbox_head = nx;
            else
                ta_event_at(core, prev)->next = nx;
            if (actor_record->mailbox_tail == event_index) actor_record->mailbox_tail = prev;
            actor_record->mailbox_length--;
            ta_event_release(core, event_index);
        } else {
            prev = event_index;
        }
        event_index = nx;
    }
    if (actor_record->flags & ACTOR_FLAG_CANCEL) ta_core_cancel(core, instance);
    if (actor_record->mailbox_length || instance->internal_queue_head != TA_NIL ||
        (actor_record->flags & ACTOR_FLAG_CANCEL))
        ta_system_make_runnable(system, index);
    return TA_OK;
}

/* ---- adoption ---------------------------------------------------------------------------- */

/* a timer record and its texts and payload bytes (texts[3]) */
static int timer_rec(const unsigned char **q, const unsigned char *qe, blob_timer *r,
                     const char **texts) {
    if ((size_t)(qe - *q) < sizeof *r) return 0;
    ta_memcpy(r, *q, sizeof *r);
    *q += sizeof *r;
    if (r->payload_kind > TP_HANDLE || (r->payload_kind != TP_BYTES && r->payload_length)) return 0;
    uint32_t lens[4] = {r->target_length, r->name_length, r->sendid_length,
                        r->payload_kind == TP_BYTES ? r->payload_length : TA_NIL};
    for (int i = 0; i < 4; i++) {
        texts[i] = (const char *)*q;
        if (lens[i] == TA_NIL) continue;
        if ((i < 3 && lens[i] > TA_SYSTEM_MAX_STRING) || lens[i] > (uint32_t)(qe - *q) || text_bytes(lens[i]) > (uint32_t)(qe - *q)) return 0;
        *q += text_bytes(lens[i]);
    }
    return r->name_length != TA_NIL;
}

/* Drop the timers of a half-adopted actor: payloads made from bytes go, those
 * passed as handles stay with the blob. */
static void drop_timers(ta_system *system, ta_actor *actor) {
    ta_core *core = &system->core;
    while (actor->timers != TA_NIL) {
        uint32_t ti = actor->timers;
        ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
        actor->timers = timer->owner_next;
        ta_heap_remove(&system->heap, ti);
        ta_event *event = ta_event_at(core, timer->event);
        if (event->flags & TA_EVENT_FLAG_BLOB_PAYLOAD) event->payload = 0;
        ta_event_release(core, timer->event);
        ta_shared_string_release(core, timer->target);
        ta_pool_free(&system->timers, ti);
    }
}

/* Re-arm the delayed sends of a session; `seq0` + rank keeps the tree's
 * firing order (ranks < n_ranks). */
static ta_status arm_timers(ta_system *system, uint32_t index, uint32_t chart,
                            const blob_view *view, uint64_t seq0, uint32_t rank_count) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    uint32_t handle = ta_pool_handle(&system->actors, index);
    const unsigned char *q = view->timers, *qe = q + view->timers_bytes;
    uint32_t tail = TA_NIL;
    for (uint32_t k = 0; k < view->timer_count; k++) {
        blob_timer r;
        const char *tx[4];
        if (!timer_rec(&q, qe, &r, tx) || r.rank >= rank_count) return TA_ERROR_INVALID;
        uint32_t ti = ta_pool_alloc(&system->timers);
        uint32_t event_index = ti == TA_NIL ? TA_NIL : ta_event_new(core, 0);
        if (event_index == TA_NIL) {
            if (ti != TA_NIL) ta_pool_free(&system->timers, ti);
            return TA_ERROR_NO_MEMORY;
        }
        ta_event *event = ta_event_at(core, event_index);
        ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
        timer->owner = handle;
        timer->event = event_index;
        timer->route = r.route;
        timer->arg = r.arg;
        timer->target =
            r.target_length == TA_NIL ? TA_NIL : ta_shared_string_new(core, tx[0], r.target_length);
        timer->owner_next = TA_NIL;
        if (tail == TA_NIL) /* keep the firing order in the owner list too */
            actor->timers = ti;
        else
            ((ta_timer *)ta_pool_at(&system->timers, tail))->owner_next = ti;
        tail = ti;
        ta_status status = ta_heap_push(&system->heap, r.due, (uint32_t)(seq0 + r.rank), ti);
        if (status == TA_OK) status = ta_event_set_name(core, event, tx[1], r.name_length);
        event->type = (uint8_t)r.type;
        event->flags = (uint16_t)r.flags;
        event->origin = handle;
        if (r.trace_high || r.trace_low) { /* untraced if the pool is full or tracing is off */
            ta_trace_id trace = {r.trace_high, r.trace_low};
            ta_step_key cause = {r.cause_session, r.cause_macrostep, r.cause_microstep, 0};
            event->trace = ta_trace_context_new(core, trace, r.trace_flags, cause);
        }
        if (r.sendid_length != TA_NIL) {
            event->sendid = ta_shared_string_new(core, tx[2], r.sendid_length);
            if (event->sendid == TA_NIL) status = TA_ERROR_NO_MEMORY;
        }
        if (r.target_length != TA_NIL && timer->target == TA_NIL) status = TA_ERROR_NO_MEMORY;
        if (status != TA_OK) return status == TA_ERROR_INVALID ? TA_ERROR_NO_MEMORY : status;
        const ta_datamodel_binding *binding = ta_core_datamodel(core, chart);
        if (r.payload_kind == TP_BYTES) { /* a new payload, ours */
            if (!datamodel_payload_bytes(binding)) return TA_ERROR_UNSUPPORTED;
            ta_payload payload = 0;
            status = binding->vtable->payload_deserialize(
                binding->datamodel, (ta_bytes){tx[3], r.payload_length}, &payload);
            if (status != TA_OK) return status == TA_ERROR_NO_MEMORY ? status : TA_ERROR_INVALID;
            event->payload = payload;
            event->datamodel = core->chart_datamodel[chart];
        } else if (r.payload_kind == TP_HANDLE) { /* the blob's until the adoption succeeds */
            event->payload = r.payload;
            event->datamodel = core->chart_datamodel[chart];
            event->flags |= TA_EVENT_FLAG_BLOB_PAYLOAD;
        }
    }
    return q == qe ? TA_OK : TA_ERROR_INVALID;
}

/* Create actor `index` from a parsed session (checked: image, chart, instance
 * size, serial unused): record, name, instance, datamodel state,
 * delayed sends, session id. On failure nothing is left (the blob keeps its
 * payloads). */
static ta_status adopt_member(ta_system *system, const blob_view *view, uint64_t seq0,
                              uint32_t rank_count, uint32_t *index_out) {
    ta_core *core = &system->core;
    uint32_t chart = view->chart;
    uint32_t index = ta_pool_alloc(&system->actors);
    if (index == TA_NIL) return TA_ERROR_NO_MEMORY;
    ta_actor *actor = ta_actor_at(system, index);
    ta_memset(actor, 0, sizeof *actor);
    actor->next = TA_NIL;
    actor->mailbox_head = actor->mailbox_tail = actor->timers = TA_NIL;
    actor->parent_slot = TA_NIL;
    actor->serial = view->serial;
    ta_string_view cname =
        ta_image_view_string(&core->image_view, core->image_view.charts[chart].name);
    int same = view->name_length == cname.length &&
               (!cname.length || ta_memcmp(view->name, cname.pointer, cname.length) == 0);
    actor->name = same ? core->image_view.charts[chart].name
                       : ta_shared_string_new(core, (const char *)view->name, view->name_length);
    uint32_t offset = TA_NIL;
    ta_status status = actor->name != TA_NIL ? restore(system, index, chart, view, &offset)
                       : view->name_length > TA_SYSTEM_MAX_STRING ? TA_ERROR_INVALID /* malformed */
                                                                  : TA_ERROR_NO_MEMORY;
    if (status == TA_OK) {
        actor->instance = offset;
        actor->state = TA_ACTOR_IDLE;
        status = arm_timers(system, index, chart, view, seq0, rank_count);
        if (status != TA_OK) {
            drop_timers(system, actor);
            ta_core_instance_deinit(core, ta_actor_instance(system, actor));
            ta_slab_free(&system->slab, offset);
        }
    }
    if (status != TA_OK) {
        ta_shared_string_release(core, actor->name);
        actor->state = TA_ACTOR_FREE;
        ta_pool_free(&system->actors, index);
        return status;
    }
    ta_system_session_insert(system, index);
    ta_system_name_insert(system, index);
    core->chart_stats[chart].actors++;
    actor->flags |= ACTOR_FLAG_TOUCHED;
    trace_residency(system, index, TA_TRACE_LOADED, ta_actor_instance(system, actor)->macrostep);
    *index_out = index;
    return TA_OK;
}

/* Remove an adopted member again (a tree adoption failed): its delayed
 * sends' payloads stay with the blob. */
static void unadopt(ta_system *system, uint32_t index) {
    drop_timers(system, ta_actor_at(system, index));
    ta_system_reclaim(system, index, RECLAIM_LEFT); /* it was never here */
}

/* the export header of a blob; 0 if it is not one for this image */
static int read_exp(const void *blob, size_t length, uint64_t image_hash, export_header *h) {
    if (!blob || length < sizeof *h) return 0;
    ta_memcpy(h, blob, sizeof *h);
    return h->magic == EXPORT_MAGIC && h->version == EXPORT_VERSION &&
           h->image_hash == image_hash && h->session_count && h->serial &&
           h->session_count <= (length - sizeof *h) / sizeof(session_header) &&
           (!(h->flags & EXPORT_MIGRATED) || h->session_count == 1);
}

ta_status ta_system_adopt(ta_system *system, ta_bytes bytes, uint32_t *actor) {
    if (system->in_call) return TA_ERROR_STATE;
    const void *blob = bytes.pointer;
    size_t length = bytes.length;
    ta_core *core = &system->core;
    if (actor) *actor = 0;
    export_header th;
    if (!read_exp(blob, length, core->image_view.hash, &th)) return TA_ERROR_INVALID;
    const unsigned char *base = (const unsigned char *)blob, *end = base + length;
    /* check everything first: a failed adoption changes nothing */
    uint32_t timers = 0;
    const unsigned char *q = base + sizeof th;
    for (uint32_t k = 0; k < th.session_count; k++) {
        blob_view view;
        if (!next_session(&q, end, th.image_hash, &view) || view.chart >= system->chart_count ||
            view.instance_bytes != ta_core_chart_instance_size(core, view.chart) || !view.serial)
            return TA_ERROR_INVALID;
        if (k == 0 ? (view.parent || view.serial != th.serial) : (!view.parent || view.init_count))
            return TA_ERROR_INVALID;
        for (uint32_t d = 0; d < view.init_count; d++) {
            uint32_t declaration;
            ta_memcpy(&declaration, view.init + 4 * d, 4);
            if (declaration >= core->image_view.charts[view.chart].data_count)
                return TA_ERROR_INVALID;
        }
        if (ta_system_session_lookup(system, view.serial) != TA_NIL) return TA_ERROR_STATE;
        /* the parent is an earlier session, the slot one of its invokes; ids unique */
        int parent_ok = k == 0;
        const unsigned char *p = base + sizeof th;
        for (uint32_t j = 0; j < k; j++) {
            blob_view pv;
            if (!next_session(&p, end, th.image_hash, &pv)) return TA_ERROR_INVALID;
            if (pv.serial == view.serial) return TA_ERROR_INVALID;
            if (pv.serial == view.parent &&
                view.slot < core->image_view.charts[pv.chart].invoke_count)
                parent_ok = 1;
        }
        if (!parent_ok) return TA_ERROR_INVALID;
        timers += view.timer_count;
    }
    if (ta_pool_free_count(&system->actors) < th.session_count ||
        ta_pool_free_count(&system->timers) < timers ||
        ta_pool_free_count(&system->events) < timers + core->reserve + 1)
        return TA_ERROR_NO_MEMORY;
    /* sessions in preorder: each one's parent is already here */
    uint32_t root = TA_NIL;
    blob_view rv;
    uint64_t seq0 = system->sequence; /* + rank: the tree's delayed sends keep their firing order */
    q = base + sizeof th;
    for (uint32_t k = 0; k < th.session_count; k++) {
        blob_view view;
        uint32_t index;
        ta_status status = next_session(&q, end, th.image_hash, &view)
                               ? adopt_member(system, &view, seq0, timers, &index)
                               : TA_ERROR_INVALID;
        if (status != TA_OK) {
            for (uint32_t m = root == TA_NIL ? TA_NIL : leftmost_leaf(system, root), nx;
                 m != TA_NIL; m = nx) {
                nx = post_next(system, root, m);
                unadopt(system, m);
            }
            return status;
        }
        if (k == 0) {
            root = index;
            rv = view;
            continue;
        }
        uint32_t p = ta_system_session_lookup(system, view.parent);
        ta_actor *actor_record = ta_actor_at(system, index);
        actor_record->parent = ta_pool_handle(&system->actors, p);
        actor_record->parent_slot = view.slot;
        ta_core_set_invoke_child(core, ta_actor_instance(system, ta_actor_at(system, p)), view.slot,
                                 ta_pool_handle(&system->actors, index));
    }
    system->sequence = seq0 + timers;
    /* sends to invoked children (#_<invokeid>) waiting in timers: the
     * owner's slot + 1 -> the child adopted into it; payload handles are ours */
    for (uint32_t m = root; m != TA_NIL; m = tree_next(system, root, m)) {
        ta_instance *instance = ta_actor_instance(system, ta_actor_at(system, m));
        const ta_chart_record *chart = &core->image_view.charts[instance->chart];
        const uint32_t *kids =
            ta_instance_word(instance, ta_instance_layout_of(chart).invoke_children);
        for (uint32_t ti = ta_actor_at(system, m)->timers; ti != TA_NIL;) {
            ta_timer *timer = (ta_timer *)ta_pool_at(&system->timers, ti);
            if (timer->route == TA_ROUTE_CHILD)
                timer->arg =
                    timer->arg && timer->arg <= chart->invoke_count ? kids[timer->arg - 1] : 0;
            ta_event_at(core, timer->event)->flags &= ~(uint32_t)TA_EVENT_FLAG_BLOB_PAYLOAD;
            ti = timer->owner_next;
        }
    }
    ta_instance *instance = ta_actor_instance(system, ta_actor_at(system, root));
    if (th.flags & EXPORT_MIGRATED) { /* a translated actor: upgraded before its mailbox */
        ta_payload bindings = 0;
        if (ta_core_datamodel_migrate(core, instance, &bindings) != TA_OK) bindings = 0;
        for (uint32_t d = 0; d < rv.init_count; d++) {
            uint32_t declaration;
            ta_memcpy(&declaration, rv.init + 4 * d, 4);
            ta_core_init_declaration(core, instance, declaration);
        }
        ta_core_raise_platform_data(core, instance, "error.platform.upgrade", bindings);
    }
    if (instance->internal_queue_head != TA_NIL) ta_system_make_runnable(system, root);
    if (actor) *actor = ta_pool_handle(&system->actors, root);
    return TA_OK;
}

/* ---- migration: translate a blob to another version of its chart ----------------------- */

typedef struct migration_context {
    const ta_image *from_image, *to_image;
    const ta_image_view *from_view, *to_view;
    uint32_t from_chart, to_chart;
    const ta_chart_record *from_chart_record, *to_chart_record;
    const ta_state_record *from_states, *to_states;
    ta_instance_layout from_layout, to_layout;
} migration_context;

static uint32_t map_state(const migration_context *migration, uint32_t old) {
    if (old == 0) return 0;
    return ta_image_state_by_name(
        migration->to_image, migration->to_chart,
        ta_image_state_name(migration->from_image, migration->from_chart, old));
}

static void set_with_ancestors(const migration_context *migration, uint32_t *config, uint32_t state,
                               uint32_t stop) {
    while (state != TA_NIL && state != stop) {
        ta_bitset_set(config, state);
        state = migration->to_states[state].parent;
    }
}

/* Enter state x by default (x is a target of an initial transition under s). */
static void enter_default(const migration_context *migration, uint32_t *config, uint32_t x,
                          uint32_t s) {
    if (migration->to_states[x].kind >=
        TA_STATE_HISTORY_SHALLOW) { /* a history target: its default */
        uint32_t d = migration->to_states[x].initial;
        if (d == TA_NIL) {
            set_with_ancestors(migration, config, migration->to_states[x].parent, s);
            return;
        }
        const ta_transition_record *transition =
            &ta_chart_transitions(migration->to_view, migration->to_chart_record)[d];
        for (uint32_t j = 0; j < transition->target_count; j++) {
            uint32_t y = migration->to_view->u32[transition->targets_offset + j];
            if (migration->to_states[y].kind < TA_STATE_HISTORY_SHALLOW)
                set_with_ancestors(migration, config, y, s);
        }
        return;
    }
    set_with_ancestors(migration, config, x, s);
}

/* Complete a set of active states to a legal configuration, top-down. */
static void legalize(const migration_context *migration, uint32_t *config) {
    uint32_t n = migration->to_chart_record->state_count;
    ta_bitset_set(config, 0);
    for (uint32_t s = 0; s < n; s++) {
        if (!ta_bitset_test(config, s)) continue;
        uint8_t kind = migration->to_states[s].kind;
        if (kind >= TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_ATOMIC || kind == TA_STATE_FINAL) {
            if (kind >= TA_STATE_HISTORY_SHALLOW) ta_bitset_clear(config, s);
            for (uint32_t d = s + 1; d < migration->to_states[s].end; d++)
                ta_bitset_clear(config, d);
            continue;
        }
        uint32_t first = TA_NIL, any = 0;
        for (uint32_t c = s + 1; c < migration->to_states[s].end; c = migration->to_states[c].end) {
            if (migration->to_states[c].kind >= TA_STATE_HISTORY_SHALLOW) {
                ta_bitset_clear(config, c);
                continue;
            }
            if (kind == TA_STATE_PARALLEL) {
                ta_bitset_set(config, c);
                continue;
            }
            if (!ta_bitset_test(config, c)) continue;
            if (any++) { /* a compound state has one active child */
                for (uint32_t d = c; d < migration->to_states[c].end; d++)
                    ta_bitset_clear(config, d);
            } else {
                first = c;
            }
        }
        if (kind == TA_STATE_PARALLEL || first != TA_NIL) continue;
        uint32_t init = migration->to_states[s].initial;
        if (init != TA_NIL) {
            const ta_transition_record *transition =
                &ta_chart_transitions(migration->to_view, migration->to_chart_record)[init];
            for (uint32_t j = 0; j < transition->target_count; j++)
                enter_default(migration, config,
                              migration->to_view->u32[transition->targets_offset + j], s);
        } else {
            for (uint32_t c = s + 1; c < migration->to_states[s].end;
                 c = migration->to_states[c].end)
                if (migration->to_states[c].kind < TA_STATE_HISTORY_SHALLOW) {
                    ta_bitset_set(config, c);
                    break;
                }
        }
    }
}

static int string_equal_(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (!a.length || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

static int old_declares(const migration_context *migration, ta_string_view id) {
    for (uint32_t i = 0; i < migration->from_chart_record->data_count; i++) {
        ta_data_info data_info;
        if (ta_image_data(migration->from_image, (ta_chart_item){migration->from_chart, i},
                          &data_info) == TA_OK &&
            string_equal_(data_info.id, id))
            return 1;
    }
    return 0;
}

/* Old instance words live in the blob, which may be unaligned. */
static uint32_t rd(const unsigned char *words, uint32_t w) {
    uint32_t x;
    ta_memcpy(&x, words + (size_t)w * 4, 4);
    return x;
}
static int rd_bit(const unsigned char *words, uint32_t w0, uint32_t bit) {
    return (int)((rd(words, w0 + (bit >> 5)) >> (bit & 31)) & 1u);
}

/* Translate history recordings by name; a recording survives only whole. */
static void translate_history(const migration_context *migration, const unsigned char *oin,
                              uint32_t *nin) {
    if (!migration->to_chart_record->history_word_count) return;
    for (uint32_t h = 1; h < migration->to_chart_record->state_count; h++) {
        if (migration->to_states[h].kind < TA_STATE_HISTORY_SHALLOW) continue;
        uint32_t p = migration->to_states[h].parent;
        uint32_t *nh = nin + migration->to_layout.history + migration->to_states[h].history_offset;
        uint32_t nbits = migration->to_states[p].end - p - 1;
        ta_bitset_zero(nh, ta_bitset_words(nbits));
        if (!migration->from_chart_record->history_word_count) continue;
        uint32_t o = ta_image_state_by_name(
            migration->from_image, migration->from_chart,
            ta_image_state_name(migration->to_image, migration->to_chart, h));
        if (o == TA_NIL || migration->from_states[o].kind != migration->to_states[h].kind ||
            !rd_bit(oin, migration->from_layout.history_recorded, o))
            continue;
        uint32_t old_parent = migration->from_states[o].parent,
                 obits = migration->from_states[old_parent].end - old_parent - 1;
        uint32_t w0 = migration->from_layout.history + migration->from_states[o].history_offset;
        int ok = 1;
        for (uint32_t b = 0; b < obits && ok; b++) {
            if (!rd_bit(oin, w0, b)) continue;
            uint32_t y = map_state(migration, old_parent + 1 + b);
            if (y == TA_NIL || y <= p || y >= migration->to_states[p].end)
                ok = 0;
            else
                ta_bitset_set(nh, y - p - 1);
        }
        if (ok)
            ta_bitset_set(nin + migration->to_layout.history_recorded, h);
        else
            ta_bitset_zero(nh, ta_bitset_words(nbits));
    }
}

ta_status ta_blob_migrate(const ta_blob_migration *request, ta_buffer *destination) {
    const ta_image *from_image = request->from_image, *to_image = request->to_image;
    uint32_t to_chart = request->to_chart;
    const void *blob = request->blob.pointer;
    size_t length = request->blob.length;
    void *out = destination->pointer;
    size_t capacity = destination->capacity;
    size_t *out_length = &destination->length;
    migration_context migration;
    blob_view view;
    export_header eh;
    *out_length = 0;
    if (!from_image || !to_image || ((uintptr_t)out & 3u)) return TA_ERROR_INVALID;
    migration.from_image = from_image;
    migration.to_image = to_image;
    migration.from_view = ta_image_view_of(from_image);
    migration.to_view = ta_image_view_of(to_image);
    if (!read_exp(blob, length, migration.from_view->hash, &eh)) return TA_ERROR_INVALID;
    if (eh.session_count != 1)
        return TA_ERROR_UNSUPPORTED; /* running invocations: not translated */
    const unsigned char *q0 = (const unsigned char *)blob + sizeof eh;
    if (!next_session(&q0, (const unsigned char *)blob + length, eh.image_hash, &view) ||
        view.chart >= migration.from_view->chart_count ||
        to_chart >= migration.to_view->chart_count)
        return TA_ERROR_INVALID;
    migration.from_chart = view.chart;
    migration.to_chart = to_chart;
    migration.from_chart_record = &migration.from_view->charts[migration.from_chart];
    migration.to_chart_record = &migration.to_view->charts[migration.to_chart];
    migration.from_states = ta_chart_states(migration.from_view, migration.from_chart_record);
    migration.to_states = ta_chart_states(migration.to_view, migration.to_chart_record);
    migration.from_layout = ta_instance_layout_of(migration.from_chart_record);
    migration.to_layout = ta_instance_layout_of(migration.to_chart_record);
    if (view.instance_bytes != migration.from_layout.size) return TA_ERROR_INVALID;

    uint32_t new_count = 0; /* <data> the new chart adds (at most this many to initialize) */
    for (uint32_t d = 0; d < migration.to_chart_record->data_count; d++) {
        ta_data_info di;
        if (ta_image_data(to_image, (ta_chart_item){migration.to_chart, d}, &di) == TA_OK &&
            !old_declares(&migration, di.id))
            new_count++;
    }
    size_t fixed = sizeof(export_header) + sizeof(session_header);
    uint64_t raw = fixed + (uint64_t)migration.to_layout.size + 4 + view.datamodel_bytes +
                   view.timers_bytes + (uint64_t)new_count * 4 + align4(view.name_length);
    if (raw > (size_t)-1 - 8) return TA_ERROR_NO_MEMORY; /* cannot be addressed here */
    size_t need = align8((size_t)raw);
    *out_length = need;
    if (capacity < need) return TA_ERROR_NO_MEMORY;

    unsigned char *o = (unsigned char *)out;
    uint32_t *nin = (uint32_t *)(void *)(o + fixed); /* 4-aligned: out is */
    const unsigned char *oin = view.instance;
    uint32_t *config = nin + migration.to_layout.config, *inv = nin + migration.to_layout.to_invoke;
    ta_memset(nin, 0, migration.to_layout.size);
    /* configuration: mapped old states and their ancestors, completed to a
     * legal one; to_invoke marks the mapped states meanwhile */
    for (uint32_t state = 1; state < migration.from_chart_record->state_count; state++) {
        if (!rd_bit(oin, migration.from_layout.config, state)) continue;
        uint32_t y = map_state(&migration, state);
        if (y == TA_NIL || migration.to_states[y].kind >= TA_STATE_HISTORY_SHALLOW) continue;
        set_with_ancestors(&migration, config, y, TA_NIL);
        ta_bitset_set(inv, y);
    }
    legalize(&migration, config);
    for (uint32_t state = 0; state < migration.to_chart_record->state_count;
         state++) { /* newly entered states invoke */
        int entered = ta_bitset_test(config, state) && !ta_bitset_test(inv, state);
        if (entered && migration.to_states[state].invoke_count)
            ta_bitset_set(inv, state);
        else
            ta_bitset_clear(inv, state);
    }
    /* late binding: data flags follow their states; active states have data */
    if (migration.to_chart_record->binding == TA_BINDING_LATE) {
        uint32_t *di = nin + migration.to_layout.data_init;
        for (uint32_t state = 0; state < migration.from_chart_record->state_count; state++) {
            int had = migration.from_chart_record->binding == TA_BINDING_LATE
                          ? rd_bit(oin, migration.from_layout.data_init, state)
                          : 1;
            uint32_t y = had ? map_state(&migration, state) : TA_NIL;
            if (y != TA_NIL) ta_bitset_set(di, y);
        }
        for (uint32_t state = 0; state < migration.to_chart_record->state_count; state++)
            if (ta_bitset_test(config, state)) ta_bitset_set(di, state);
    }
    translate_history(&migration, oin, nin);
    ta_instance header; /* scalar instance fields: a stable instance of the new chart */
    ta_memcpy(&header, oin, sizeof header);
    header.chart = migration.to_chart;
    header.phase = PHASE_STABLE;
    header.flags &= (uint8_t)INSTANCE_FLAG_DETACHED;
    header.subphase = 0;
    header.cursor = header.cursor2 = 0;
    header.current_event = header.internal_queue_head = header.internal_queue_tail =
        header.workspace = TA_NIL;
    header.actor = 0;
    header.datamodel = 0;
    header.trace = TA_NIL; /* the step counters go on */
    ta_memcpy(nin, &header, sizeof header);
    for (uint32_t i = 0; i < migration.to_chart_record->invoke_count; i++)
        nin[migration.to_layout.invoke_ids + i] = TA_NIL;

    session_header sh;
    ta_memset(&sh, 0, sizeof sh);
    sh.serial = view.serial;
    sh.slot = TA_NIL;
    sh.datamodel_bytes = view.datamodel_bytes;
    sh.chart = migration.to_chart;
    sh.instance_bytes = migration.to_layout.size;
    sh.ids_bytes = 4; /* no invokes are running (one session) */
    sh.timer_count = view.timer_count;
    sh.timers_bytes = view.timers_bytes;
    sh.name_length = view.name_length;
    unsigned char *q = o + fixed + migration.to_layout.size;
    ta_memset(q, 0, 4);
    q += 4;
    ta_memcpy(q, view.datamodel, (size_t)view.datamodel_bytes);
    q += view.datamodel_bytes;
    ta_memcpy(q, view.timers, view.timers_bytes);
    q += view.timers_bytes;
    for (uint32_t d = 0; d < migration.to_chart_record->data_count; d++) {
        ta_data_info di;
        if (ta_image_data(to_image, (ta_chart_item){migration.to_chart, d}, &di) != TA_OK ||
            old_declares(&migration, di.id))
            continue;
        if (migration.to_chart_record->binding == TA_BINDING_LATE &&
            !ta_bitset_test(config, di.state))
            continue;
        ta_memcpy(q, &d, 4);
        q += 4;
        sh.init_count++;
    }
    ta_memcpy(q, view.name, align4(view.name_length));
    q += align4(view.name_length);
    size_t used = (size_t)(q - o);
    ta_memset(q, 0, align8(used) - used);
    q = o + align8(used);
    sh.bytes = (uint32_t)(q - o - sizeof(export_header));
    eh.flags |= EXPORT_MIGRATED;
    eh.image_hash = migration.to_view->hash;
    ta_memcpy(o, &eh, sizeof eh);
    ta_memcpy(o + sizeof eh, &sh, sizeof sh);
    *out_length = (size_t)(q - o);
    return TA_OK;
}

void ta_system_wake_request(ta_system *system, uint32_t index) {
    ta_actor *actor = ta_actor_at(system, index);
    if (actor->flags & (ACTOR_FLAG_IN_WAKE_QUEUE | ACTOR_FLAG_WAKE_SENT))
        return; /* one request per hibernation */
    actor->flags |= ACTOR_FLAG_IN_WAKE_QUEUE;
    ta_index_queue_push(&system->wake_queue, &system->actors, 0, index);
    ta_system_emit_wakes(system);
}

void ta_system_emit_wakes(ta_system *system) {
    while (!ta_index_queue_empty(&system->wake_queue) && !ta_system_effect_full(system)) {
        uint32_t index = ta_index_queue_pop(&system->wake_queue, &system->actors, 0);
        ta_actor *actor = ta_actor_at(system, index);
        actor->flags &= (uint8_t)~ACTOR_FLAG_IN_WAKE_QUEUE;
        if (actor->state != TA_ACTOR_HIBERNATED) continue;
        if (!ta_system_effect_push(system, TA_EFFECT_WAKE,
                                   index)) { /* cannot happen: room was checked */
            actor->flags |= ACTOR_FLAG_IN_WAKE_QUEUE;
            ta_index_queue_push(&system->wake_queue, &system->actors, 0, index);
            return;
        }
        actor->flags |= ACTOR_FLAG_WAKE_SENT;
        system->pending_wake_count++;
    }
}

uint32_t ta_system_cold_actors(ta_system *system, ta_sweep *sweep, ta_uint32_array out) {
    uint32_t *cursor = &sweep->cursor, max_scan = sweep->max_scan, capacity = out.capacity;
    uint32_t high_water_mark = system->actors.high_water_mark, n = 0, i = *cursor;
    if (!high_water_mark || system->in_call) return 0;
    if (max_scan > high_water_mark)
        max_scan = high_water_mark; /* at most one revolution per call */
    for (uint32_t k = 0; k < max_scan && n < capacity; k++, i++) {
        if (i >= high_water_mark) i = 0;
        if (!ta_pool_live(&system->actors, i)) continue;
        ta_actor *actor = ta_actor_at(system, i);
        if (actor->state != TA_ACTOR_IDLE) continue;
        if (actor->flags & ACTOR_FLAG_TOUCHED)
            actor->flags &= (uint8_t)~ACTOR_FLAG_TOUCHED; /* second chance */
        else if (hibernatable(system, i) == TA_OK)
            out.pointer[n++] = ta_pool_handle(&system->actors, i);
    }
    *cursor = i >= high_water_mark ? 0 : i;
    return n;
}
