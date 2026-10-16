/* event_pool.c — refcounted slab strings and shared event records. */
#include "core/core_internal.h"
#include "base/format.h"
#include "base/memory.h"

/* ---- strings ----------------------------------------------------------------- */

typedef struct sstr {
    uint32_t refs, length;
} sstr;

static sstr *sstr_at(const ta_core *core, uint32_t string_ref) {
    return (sstr *)ta_slab_pointer(core->slab, (string_ref & ~TA_STRING_REF_SLAB) << 3);
}

/* the longest string is one slab object: its header, its bytes and a terminator */
TA_STATIC_ASSERT(TA_SYSTEM_MAX_STRING + sizeof(sstr) + 1 == TA_SLAB_PAGE, max_string_is_a_page);

uint32_t ta_shared_string_new(ta_core *core, const char *p, uint32_t length) {
    if (length > TA_SYSTEM_MAX_STRING) return TA_NIL;
    uint32_t offset = ta_slab_alloc(core->slab, (uint32_t)sizeof(sstr) + length + 1);
    if (offset == TA_NIL) return TA_NIL;
    sstr *s = (sstr *)ta_slab_pointer(core->slab, offset);
    s->refs = 1;
    s->length = length;
    char *d = (char *)(s + 1);
    if (length) ta_memcpy(d, p, length);
    d[length] = 0;
    return TA_STRING_REF_SLAB | (offset >> 3);
}

void ta_shared_string_retain_slab(ta_core *core, uint32_t string_ref) {
    sstr_at(core, string_ref)->refs++;
}

void ta_shared_string_release_slab(ta_core *core, uint32_t string_ref) {
    sstr *s = sstr_at(core, string_ref);
    TA_ASSERT(s->refs > 0);
    if (--s->refs == 0) ta_slab_free(core->slab, (string_ref & ~TA_STRING_REF_SLAB) << 3);
}

ta_string_view ta_shared_string_get(const ta_core *core, uint32_t string_ref) {
    if (TA_STRING_REF_IS_SLAB(string_ref)) {
        sstr *s = sstr_at(core, string_ref);
        ta_string_view r = {(const char *)(s + 1), s->length};
        return r;
    }
    return ta_image_view_string(&core->image_view, string_ref);
}

int ta_shared_string_equal(const ta_core *core, uint32_t a, ta_string_view b) {
    ta_string_view x = ta_shared_string_get(core, a);
    return x.length == b.length &&
           (b.length == 0 || ta_memcmp(x.pointer, b.pointer, b.length) == 0);
}

/* CSS2 time: <number>(s|ms), optional surrounding spaces. */
int ta_core_parse_duration(ta_string_view s, uint64_t *ns) {
    uint32_t i = 0;
    while (i < s.length && (s.pointer[i] == ' ' || s.pointer[i] == '\t'))
        i++;
    uint64_t ip = 0, frac = 0, scale = 1;
    int digits = 0;
    while (i < s.length && s.pointer[i] >= '0' && s.pointer[i] <= '9') {
        ip = ip * 10 + (uint64_t)(s.pointer[i++] - '0');
        digits++;
        if (ip > 3600ull * 1000000000ull) return 0;
    }
    if (i < s.length && s.pointer[i] == '.') {
        i++;
        while (i < s.length && s.pointer[i] >= '0' && s.pointer[i] <= '9') {
            if (scale < 1000000000ull) {
                frac = frac * 10 + (uint64_t)(s.pointer[i] - '0');
                scale *= 10;
            }
            i++;
            digits++;
        }
    }
    if (!digits) return 0;
    uint64_t unit;
    if (i + 2 <= s.length && s.pointer[i] == 'm' && s.pointer[i + 1] == 's') {
        unit = 1000000ull;
        i += 2;
    } else if (i < s.length && s.pointer[i] == 's') {
        unit = 1000000000ull;
        i += 1;
    } else {
        return 0;
    }
    while (i < s.length && (s.pointer[i] == ' ' || s.pointer[i] == '\t'))
        i++;
    if (i != s.length) return 0;
    /* beyond what 64 bits of nanoseconds hold (~584 years): never, not a wrapped soon */
    *ns = ip > (UINT64_MAX - unit) / unit ? UINT64_MAX : ip * unit + frac * unit / scale;
    return 1;
}

/* ---- events ------------------------------------------------------------------ */

uint32_t ta_event_new(ta_core *core, int reserve_ok) {
    if (!reserve_ok && ta_pool_free_count(core->events) <= core->reserve) return TA_NIL;
    uint32_t i = ta_pool_alloc(core->events);
    if (i == TA_NIL) return TA_NIL;
    ta_event *event = ta_event_at(core, i);
    ta_memset(event, 0, sizeof *event);
    event->next = TA_NIL;
    event->refs = 1;
    event->datamodel = 0xFF;
    event->name = event->sendid = event->origin_string = event->origintype = event->invokeid =
        TA_NIL;
    event->invoke_slot = TA_EVENT_NO_SLOT;
    event->raw = TA_NIL;
    event->trace = TA_NIL;
    return i;
}

void ta_event_retain(ta_core *core, uint32_t event) {
    ta_event_at(core, event)->refs++;
}

void ta_event_release(ta_core *core, uint32_t event_index) {
    ta_event *event = ta_event_at(core, event_index);
    TA_ASSERT(event->refs > 0);
    if (--event->refs) return;
    ta_shared_string_release(core, event->name);
    ta_shared_string_release(core, event->sendid);
    ta_shared_string_release(core, event->origin_string);
    ta_shared_string_release(core, event->origintype);
    ta_shared_string_release(core, event->invokeid);
    ta_shared_string_release(core, event->raw);
    ta_trace_context_release(core, event->trace);
    if (event->payload && event->datamodel < core->datamodel_count &&
        core->datamodels[event->datamodel].vtable->release)
        core->datamodels[event->datamodel].vtable->release(
            core->datamodels[event->datamodel].datamodel, event->payload);
    ta_pool_free(core->events, event_index);
}

ta_status ta_event_set_name(ta_core *core, ta_event *event, const char *name, uint32_t length) {
    int exact = 0;
    event->name_id = ta_event_name_lookup(&core->image_view, name, length, &exact);
    if (exact) {
        event->name = core->image_view.event_nodes[event->name_id].name;
        return TA_OK;
    }
    event->name = ta_shared_string_new(core, name, length);
    return event->name == TA_NIL ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* An origintype's index among the session's I/O processors (system variables): 0 for SCXML's,
 * i + 1 for the config's processor i, TA_NIL for none of them. */
static uint32_t io_processor_id(const ta_core *core, ta_string_view type) {
    if (!type.length) return TA_NIL;
    if (type.length == sizeof TA_SCXML_IO_PROCESSOR - 1 &&
        ta_memcmp(type.pointer, TA_SCXML_IO_PROCESSOR, type.length) == 0)
        return 0;
    for (uint32_t i = 0; i < core->io_processor_count; i++) {
        ta_string_view x = core->io_processors[i].type;
        if (x.length == type.length && ta_memcmp(x.pointer, type.pointer, type.length) == 0) return i + 1;
    }
    return TA_NIL;
}

void ta_event_view_of(ta_core *core, uint32_t event_index, ta_event_view *view) {
    const ta_event *event = ta_event_at(core, event_index);
    ta_memset(view, 0, sizeof *view);
    view->size = sizeof *view;
    view->type = event->type;
    view->name_id = event->name_id;
    view->name = ta_shared_string_get(core, event->name);
    view->sendid = ta_shared_string_get(core, event->sendid);
    view->invokeid = ta_shared_string_get(core, event->invokeid);
    view->data = event->payload;
    view->raw = ta_shared_string_get(core, event->raw);
    view->invoke_slot = event->invoke_slot == TA_EVENT_NO_SLOT ? TA_NIL : event->invoke_slot;
    view->origintype_id = TA_NIL;
    if (event->flags & TA_EVENT_FLAG_FROM_EXTERNAL_INVOKE) {
        /* from a service the embedder runs: a reply goes to #_<invokeid>, through SCXML's
           processor (SCXML 5.10.1: origin and origintype let the receiver <send> back) */
        ta_string_view id = view->invokeid;
        uint32_t n = 2;
        core->scratch[0] = '#';
        core->scratch[1] = '_';
        for (uint32_t i = 0; i < id.length && n < sizeof core->scratch; i++)
            core->scratch[n++] = id.pointer[i];
        view->origin.pointer = core->scratch;
        view->origin.length = n;
        view->origintype.pointer = TA_SCXML_IO_PROCESSOR;
        view->origintype.length = (uint32_t)sizeof TA_SCXML_IO_PROCESSOR - 1;
        view->origintype_id = 0;
    } else if (event->origin_string != TA_NIL) {
        view->origin = ta_shared_string_get(core, event->origin_string);
        view->origintype = ta_shared_string_get(core, event->origintype);
        view->origintype_id = io_processor_id(core, view->origintype);
    } else if (event->origin && core->serial_of) {
        static const char pre[] = "#_scxml_";
        ta_memcpy(core->scratch, pre, sizeof pre - 1);
        uint32_t n = (uint32_t)sizeof pre - 1;
        n += ta_format_u64(core->scratch + n, core->serial_of(core->system, event->origin));
        view->origin.pointer = core->scratch;
        view->origin.length = n;
        view->origintype.pointer = TA_SCXML_IO_PROCESSOR;
        view->origintype.length = (uint32_t)sizeof TA_SCXML_IO_PROCESSOR - 1;
        view->origintype_id = 0;
    }
}
