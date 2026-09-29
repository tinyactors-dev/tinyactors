/*
 * The OTLP encoder (napi ADR 0016), driven by a real core system without
 * Node-API: a request is well-formed protobuf of the right shape, a
 * macrostep that pauses between turns stays open across takes, lifecycle
 * records are spans of their own, open spans end when the sink goes, and
 * the bytes of a small request are fixed (golden). Every request is put
 * into a heap block of exactly the size the encoder reported, so a write
 * past it fails under AddressSanitizer (napi ADR 0018).
 */
#include "check.h"
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "otlp.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })
#define END {0}

/* ------------------------------------------------------------------------
 * Taking a request: size, then put into memory of exactly that size
 */

typedef size_t (*request_size)(const tah_otlp *otlp);
typedef size_t (*request_put)(tah_otlp *otlp, uint8_t *out, size_t size);

/* The last request taken; freed by the next take and at exit. */
static uint8_t *taken;

static ta_str take_with(tah_otlp *otlp, request_size size_of, request_put put) {
    free(taken);
    taken = NULL;
    size_t size = size_of(otlp);
    if (size == 0) {
        return (ta_str){ 0 };
    }
    taken = malloc(size);
    CHECK(taken != NULL);
    /* A size that is not the current one writes and forgets nothing. */
    CHECK(put(otlp, taken, size - 1u) == 0);
    CHECK(size_of(otlp) == size);
    CHECK(put(otlp, taken, size) == size);
    CHECK(size_of(otlp) == 0);
    return (ta_str){ (const char *)taken, (uint32_t)size };
}

static ta_str take(tah_otlp *otlp) { return take_with(otlp, tah_otlp_traces_size, tah_otlp_put_traces); }
static ta_str take_logs(tah_otlp *otlp) { return take_with(otlp, tah_otlp_logs_size, tah_otlp_put_logs); }

/* ------------------------------------------------------------------------
 * A host with a fixed clock and fixed trace IDs
 */

typedef struct {
    uint64_t now;
    uint8_t next_id;
} stub_host;

static uint64_t stub_now(void *self) {
    stub_host *host = self;
    host->now += 1000u;
    return host->now;
}

static void stub_trace_id(void *self, uint8_t out[16]) {
    stub_host *host = self;
    host->next_id += 1u;
    memset(out, host->next_id, 16);
}

static void sink_write(void *self, const ta_trace_record *record) {
    tah_otlp_write(self, record);
}
static const ta_trace_sink_vtable sink_vtable = { .write = sink_write };

/* ------------------------------------------------------------------------
 * A strict reader: every field is well-formed and every length is in bounds
 */

typedef struct {
    const uint8_t *at;
    const uint8_t *end;
} reader;

static bool read_varint(reader *r, uint64_t *out) {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->at >= r->end) {
            return false;
        }
        uint8_t byte = *r->at++;
        value |= (uint64_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return true;
        }
    }
    return false;
}

/* The next field: its number, and for LEN fields its body. */
static bool read_field(reader *r, uint32_t *field, uint32_t *wire, reader *body, uint64_t *scalar) {
    uint64_t tag = 0;
    if (!read_varint(r, &tag) || (tag >> 3) == 0) {
        return false;
    }
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7u);
    switch (*wire) {
        case 0:
            return read_varint(r, scalar);
        case 1:
            if (r->end - r->at < 8) return false;
            *scalar = 0;
            for (int at = 0; at < 8; at += 1) *scalar |= (uint64_t)r->at[at] << (8 * at);
            r->at += 8;
            return true;
        case 2: {
            uint64_t len = 0;
            if (!read_varint(r, &len) || (uint64_t)(r->end - r->at) < len) return false;
            *body = (reader){ r->at, r->at + len };
            r->at += len;
            return true;
        }
        default:
            return false;
    }
}

/* Every field of a message is well-formed (bodies are not descended). */
static bool well_formed(reader r) {
    while (r.at < r.end) {
        uint32_t field = 0, wire = 0;
        reader body = { 0 };
        uint64_t scalar = 0;
        if (!read_field(&r, &field, &wire, &body, &scalar)) return false;
    }
    return true;
}

typedef struct {
    char name[96];
    uint64_t span_id;
    uint64_t parent_span_id;
    uint64_t start;
    uint64_t end;
    uint32_t events;
    uint32_t attributes;
    bool incomplete;
} span_view;

static uint64_t id64(reader body) {
    uint64_t value = 0;
    for (const uint8_t *at = body.at; at < body.end; at += 1) value = (value << 8) | *at;
    return value;
}

/* Descends request → resource spans → scope spans → spans. */
static uint32_t read_spans(ta_str bytes, span_view *out, uint32_t capacity) {
    reader request = { (const uint8_t *)bytes.ptr, (const uint8_t *)bytes.ptr + bytes.len };
    uint32_t count = 0;
    uint32_t field = 0, wire = 0;
    uint64_t scalar = 0;
    reader rs = { 0 };
    CHECK(well_formed(request));
    CHECK(read_field(&request, &field, &wire, &rs, &scalar) && field == 1 && wire == 2);
    CHECK(request.at == request.end);
    reader part = { 0 };
    while (rs.at < rs.end) {
        CHECK(read_field(&rs, &field, &wire, &part, &scalar) && wire == 2);
        if (field == 1) {
            CHECK(well_formed(part)); /* Resource */
            continue;
        }
        CHECK(field == 2);
        reader ss = part;
        while (ss.at < ss.end) {
            reader item = { 0 };
            CHECK(read_field(&ss, &field, &wire, &item, &scalar) && wire == 2);
            if (field == 1) continue; /* InstrumentationScope */
            CHECK(field == 2);
            CHECK(well_formed(item));
            span_view view = { 0 };
            reader span = item;
            while (span.at < span.end) {
                reader value = { 0 };
                CHECK(read_field(&span, &field, &wire, &value, &scalar));
                switch (field) {
                    case 1: CHECK(value.end - value.at == 16); break;
                    case 2: CHECK(value.end - value.at == 8); view.span_id = id64(value); break;
                    case 4: CHECK(value.end - value.at == 8); view.parent_span_id = id64(value); break;
                    case 5: {
                        size_t len = (size_t)(value.end - value.at);
                        CHECK(len < sizeof view.name);
                        memcpy(view.name, value.at, len);
                        break;
                    }
                    case 7: view.start = scalar; break;
                    case 8: view.end = scalar; break;
                    case 9: {
                        view.attributes += 1u;
                        CHECK(well_formed(value));
                        reader kv = value;
                        reader key = { 0 };
                        CHECK(read_field(&kv, &field, &wire, &key, &scalar) && field == 1);
                        if ((size_t)(key.end - key.at) == strlen("scxml.incomplete") &&
                            memcmp(key.at, "scxml.incomplete", 16) == 0) {
                            view.incomplete = true;
                        }
                        break;
                    }
                    case 11: view.events += 1u; CHECK(well_formed(value)); break;
                    default: break;
                }
            }
            CHECK(view.end >= view.start);
            if (count < capacity) out[count] = view;
            count += 1u;
        }
    }
    return count;
}

/* One ScopeSpans (napi ADR 0028): its scope's name and definition attributes, and its spans. */
typedef struct {
    char name[32];
    char family[32];
    uint8_t revision[32];
    uint32_t revision_len;
    bool revision_bytes; /* AnyValue.bytes_value rather than string_value */
    int64_t id;
    char document[32];   /* tinyactors.definition.name, "" when absent */
    char source[64];     /* tinyactors.definition.source, "" when absent */
    uint32_t spans;
} scope_view;

static void copy_text(char *out, size_t size, reader value) {
    size_t len = (size_t)(value.end - value.at);
    CHECK(len < size);
    memcpy(out, value.at, len);
    out[len] = '\0';
}

static bool key_is(reader key, const char *text) {
    return (size_t)(key.end - key.at) == strlen(text) && memcmp(key.at, text, strlen(text)) == 0;
}

/* Descends request → resource spans → scope spans, reading each scope. */
static uint32_t read_scopes(ta_str bytes, scope_view *out, uint32_t capacity) {
    reader request = { (const uint8_t *)bytes.ptr, (const uint8_t *)bytes.ptr + bytes.len };
    uint32_t count = 0;
    uint32_t field = 0, wire = 0;
    uint64_t scalar = 0;
    reader rs = { 0 };
    CHECK(read_field(&request, &field, &wire, &rs, &scalar) && field == 1);
    while (rs.at < rs.end) {
        reader part = { 0 };
        CHECK(read_field(&rs, &field, &wire, &part, &scalar) && wire == 2);
        if (field != 2) continue;
        scope_view view = { 0 };
        while (part.at < part.end) {
            reader item = { 0 };
            CHECK(read_field(&part, &field, &wire, &item, &scalar) && wire == 2);
            if (field == 2) {
                view.spans += 1u;
                continue;
            }
            CHECK(field == 1);
            while (item.at < item.end) {
                reader value = { 0 };
                CHECK(read_field(&item, &field, &wire, &value, &scalar) && wire == 2);
                if (field == 1) {
                    copy_text(view.name, sizeof view.name, value);
                } else if (field == 3) {
                    reader key = { 0 };
                    reader any = { 0 };
                    CHECK(read_field(&value, &field, &wire, &key, &scalar) && field == 1);
                    CHECK(read_field(&value, &field, &wire, &any, &scalar) && field == 2);
                    reader inner = { 0 };
                    CHECK(read_field(&any, &field, &wire, &inner, &scalar));
                    if (key_is(key, "tinyactors.definition.family")) {
                        CHECK(field == 1);
                        copy_text(view.family, sizeof view.family, inner);
                    } else if (key_is(key, "tinyactors.definition.revision")) {
                        CHECK(field == 1 || field == 7);
                        view.revision_bytes = field == 7;
                        view.revision_len = (uint32_t)(inner.end - inner.at);
                        CHECK(view.revision_len <= sizeof view.revision);
                        memcpy(view.revision, inner.at, view.revision_len);
                    } else if (key_is(key, "tinyactors.definition.id")) {
                        CHECK(field == 3);
                        view.id = (int64_t)scalar;
                    } else if (key_is(key, "tinyactors.definition.name")) {
                        copy_text(view.document, sizeof view.document, inner);
                    } else if (key_is(key, "tinyactors.definition.source")) {
                        copy_text(view.source, sizeof view.source, inner);
                    }
                }
            }
        }
        if (count < capacity) out[count] = view;
        count += 1u;
    }
    return count;
}

static const span_view *named(const span_view *spans, uint32_t count, const char *name) {
    for (uint32_t at = 0; at < count; at += 1u) {
        if (strcmp(spans[at].name, name) == 0) return &spans[at];
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * The chart: a --> b --> c, eventless, then c --go--> c raising "x"
 */

static const ta_str to_b[] = { S("b"), {0} };
static const ta_str to_c[] = { S("c"), {0} };
static const ta_scxml_transition_spec a_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_b },
    END,
};
static const ta_scxml_transition_spec b_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_c },
    END,
};
static const ta_scxml_action_spec go_actions[] = {
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = S("x") } },
    END,
};
static const ta_scxml_transition_spec c_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = S("go"), .actions = go_actions },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = a_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("b"), .transitions = b_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("c"), .transitions = c_transitions },
    END,
};
static const ta_scxml_spec chart = { .name = S("chain"), .states = roots };

typedef struct {
    tah_allocator allocator;
    ta_system *core;
    ta_definition_id definition;
    ta_actor_id actor;
    stub_host host;
    tah_otlp otlp;
} fixture;

static void start(fixture *f, bool install) {
    memset(f, 0, sizeof *f);
    tah_allocator_init(&f->allocator, check_platform_allocator());
    ta_allocator core_allocator = tah_allocator_for(&f->allocator, TAH_MEMORY_CORE);
    CHECK(ta_system_create(&(ta_system_config){ .allocator = core_allocator }, &f->core) == TA_OK);
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(f->core, &(ta_definition_compile_params){ .spec = &chart,
                                                                                .datamodel = ta_null_datamodel(),
                                                                                .allocator = core_allocator,
                                                                                .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(f->core, definition, &f->definition) == TA_OK);
    tah_otlp_init(&f->otlp, &(tah_otlp_config){
                                .core = f->core,
                                .allocator = tah_allocator_for(&f->allocator, TAH_MEMORY_OTHER),
                                .host = { .self = &f->host, .now = stub_now, .trace_id = stub_trace_id },
                                .version = S("0.0.0"),
                                .wanted_kinds = UINT32_MAX,
                                .wanted_watched_kinds = UINT32_MAX,
                                .wanted_unsampled_kinds = UINT32_MAX,
                            });
    if (install) {
        CHECK(ta_system_set_trace_sink(f->core, (ta_trace_sink){ .self = &f->otlp, .v = &sink_vtable,
                                                                 .kinds = UINT32_MAX }) == TA_OK);
    }
}

static ta_run_status resume(fixture *f, uint32_t budget) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(f->core, &(ta_actor_resume_params){ .actor = f->actor, .budget = budget,
                                                              .status = &status }) == TA_OK);
    return status;
}

static void stop(fixture *f) {
    ta_system_destroy(f->core);
    tah_otlp_destroy(&f->otlp);
    CHECK(tah_allocator_total(&f->allocator).bytes_in_use == 0);
}

/* A macrostep that pauses between turns stays open across takes, and its
 * microsteps are children of it. */
static void open_across_turns(void) {
    fixture f;
    start(&f, true);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 1) == TA_RUN_PAUSED);

    span_view spans[16];
    uint32_t count = read_spans(take(&f.otlp), spans, 16);
    CHECK(count == 1); /* spawned: the macrostep and its microstep are still open */
    CHECK(named(spans, count, "scxml.spawned") != NULL);
    CHECK(tah_otlp_open_count(&f.otlp) == 1);

    CHECK(resume(&f, 100) == TA_RUN_STABLE);
    uint32_t more = read_spans(take(&f.otlp), spans, 16);
    const span_view *macro = named(spans, more, "scxml.macrostep bootstrap");
    CHECK(macro != NULL);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.core, f.actor, &view) == TA_OK);
    CHECK(macro->span_id == ta_step_key_span_id((ta_step_key){ .session = view.session_id, .macrostep = 1 }));
    uint32_t micro = 0;
    for (uint32_t at = 0; at < more; at += 1u) {
        if (strncmp(spans[at].name, "scxml.microstep", 15) == 0) {
            micro += 1u;
            CHECK(spans[at].parent_span_id == macro->span_id);
        }
    }
    CHECK(micro == 3u); /* bootstrap, a → b, b → c */
    CHECK(tah_otlp_open_count(&f.otlp) == 0);
    CHECK(take(&f.otlp).len == 0);

    /* A delivered event: a macrostep, its microstep, and a raise event on it. */
    ta_atom go = 0;
    CHECK(ta_system_intern_atom(f.core, S("go"), &go) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(f.core, &(ta_atom_list){ .atoms = &go, .count = 1 }, &name) == TA_OK);
    CHECK(ta_actor_deliver(f.core, &(ta_actor_deliver_params){ .actor = f.actor,
                                                               .event = &(ta_event){ .name = name } }) == TA_OK);
    CHECK(resume(&f, 100) == TA_RUN_STABLE);
    count = read_spans(take(&f.otlp), spans, 16);
    const span_view *go_macro = named(spans, count, "scxml.macrostep go");
    const span_view *go_micro = named(spans, count, "scxml.microstep go");
    CHECK(go_macro != NULL && go_micro != NULL);
    CHECK(go_micro->parent_span_id == go_macro->span_id);
    CHECK(go_micro->events == 2u); /* the raise, and x ignored: nothing takes it */

    /* Destroying is a lifecycle span of its own. */
    CHECK(ta_actor_destroy(f.core, f.actor) == TA_OK);
    count = read_spans(take(&f.otlp), spans, 16);
    CHECK(count == 1 && named(spans, count, "scxml.destroyed") != NULL);
    CHECK(f.otlp.dropped == 0);
    stop(&f);
}

/* When the sink goes, open spans end, marked incomplete. */
static void end_all(void) {
    fixture f;
    start(&f, true);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 1) == TA_RUN_PAUSED);
    (void)take(&f.otlp);
    tah_otlp_end_all(&f.otlp);
    span_view spans[4];
    uint32_t count = read_spans(take(&f.otlp), spans, 4);
    CHECK(count == 2); /* the bootstrap macrostep, and its first microstep */
    CHECK(spans[0].incomplete && spans[1].incomplete);
    CHECK(named(spans, count, "scxml.macrostep bootstrap") != NULL);
    CHECK(named(spans, count, "scxml.microstep (eventless)") != NULL);
    stop(&f);
}

/* An abandoned macrostep (core ADR 0082) ends its spans, complete, with the
 * ABANDONED record as a span event: MACROSTEP_END follows ABANDONED. */
static void abandoned(void) {
    fixture f;
    start(&f, true);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 1) == TA_RUN_PAUSED);
    (void)take(&f.otlp);
    CHECK(tah_otlp_open_count(&f.otlp) == 1);
    CHECK(ta_actor_abandon(f.core, f.actor) == TA_OK);
    CHECK(tah_otlp_open_count(&f.otlp) == 0);
    span_view spans[4];
    uint32_t count = read_spans(take(&f.otlp), spans, 4);
    const span_view *macro = named(spans, count, "scxml.macrostep bootstrap");
    CHECK(macro != NULL && !macro->incomplete);
    uint32_t events = 0;
    for (uint32_t at = 0; at < count; at += 1u) {
        CHECK(!spans[at].incomplete);
        events += spans[at].events;
    }
    /* The macrostep and its open microstep, which carries PAUSED and ABANDONED. */
    CHECK(count == 2u && events == 2u);
    CHECK(f.otlp.dropped == 0);
    stop(&f);
}

/* The bytes of a small request, with a resource and a definition's scope, are fixed. The session is
 * its text, 0.4294967296.chain (core ADR 0088, napi ADR 0032 section 1). */
static void golden(void) {
    fixture f;
    start(&f, true);
    CHECK(tah_otlp_set_resource(&f.otlp, &(tah_otlp_attribute){ .key = S("service.name"), .value = S("test") }, 1) ==
          TA_OK);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    ta_str bytes = take(&f.otlp);
    static const char expected[] = "0aa4030a180a160a0c736572766963652e6e616d6512060a04746573741287030abd010a0a74696e796163746f72731205302e302e301a270a1c74696e796163746f72732e646566696e6974696f6e2e66616d696c7912070a05636861696e1a340a1e74696e796163746f72732e646566696e6974696f6e2e7265766973696f6e12120a10653734363930303030393138383039651a220a1874696e796163746f72732e646566696e6974696f6e2e696412061880808080101a250a1a74696e796163746f72732e646566696e6974696f6e2e6e616d6512070a05636861696e12c4010a100101010101010101010101010101010112088b7d46327ff76c852a0d7363786d6c2e737061776e6564300139e80300000000000041e8030000000000004a280a107363786d6c2e73657373696f6e5f696412140a12302e343239343936373239362e636861696e4a150a0f7363786d6c2e6d6163726f73746570120218004a140a0b7363786d6c2e6163746f7212050a03303a314a1a0a107363786d6c2e646566696e6974696f6e12061880808080104a100a0a7363786d6c2e74696d6512021800";
    char hex[2048];
    CHECK(bytes.len * 2u < sizeof hex);
    for (uint32_t at = 0; at < bytes.len; at += 1u) {
        static const char digits[] = "0123456789abcdef";
        hex[2u * at] = digits[(uint8_t)bytes.ptr[at] >> 4];
        hex[2u * at + 1u] = digits[(uint8_t)bytes.ptr[at] & 15u];
    }
    hex[2u * bytes.len] = '\0';
    if (strcmp(hex, expected) != 0) {
        fprintf(stderr, "golden bytes differ:\n  %s\n", hex);
    }
    CHECK(strcmp(hex, expected) == 0);
    stop(&f);
}

/*
 * Many sessions open at once, their macrosteps ended in an order that
 * shifts entries in the open-span index (napi ADR 0017 section 5), and
 * unloading one ends its open span.
 */
static void many_open(void) {
    enum { COUNT = 200 };
    fixture f;
    start(&f, true);
    static ta_actor_id actors[COUNT];
    for (uint32_t at = 0; at < COUNT; at += 1u) {
        CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &actors[at] }) ==
              TA_OK);
        f.actor = actors[at];
        CHECK(resume(&f, 1) == TA_RUN_PAUSED);
    }
    CHECK(tah_otlp_open_count(&f.otlp) == COUNT);
    (void)take(&f.otlp);
    /* Every third, then the rest, from the back. */
    for (uint32_t pass = 0; pass < 2; pass += 1u) {
        for (uint32_t at = COUNT; at > 0; at -= 1u) {
            if (((at - 1u) % 3u == 0) == (pass == 0)) {
                f.actor = actors[at - 1u];
                CHECK(resume(&f, 1000) == TA_RUN_STABLE);
            }
        }
    }
    CHECK(tah_otlp_open_count(&f.otlp) == 0);
    (void)take(&f.otlp);

    /* An actor unloaded mid-macrostep ends its span as incomplete. */
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 1) == TA_RUN_PAUSED);
    CHECK(tah_otlp_open_count(&f.otlp) == 1);
    ta_actor_destroy(f.core, f.actor);
    CHECK(tah_otlp_open_count(&f.otlp) == 0);
    span_view spans[8];
    uint32_t count = read_spans(take(&f.otlp), spans, 8);
    const span_view *boot = named(spans, count, "scxml.macrostep bootstrap");
    CHECK(boot != NULL && boot->incomplete);
    CHECK(named(spans, count, "scxml.destroyed") != NULL);
    stop(&f);
}

/*
 * Logs take the same way. Their size is the running total, and it is right
 * across several puts, with records added between them.
 */
static void logs(void) {
    fixture f;
    start(&f, true);
    f.otlp.config.log_limit = 1000; /* the fixture writes no logs by default */
    CHECK(tah_otlp_set_resource(&f.otlp, &(tah_otlp_attribute){ .key = S("service.name"), .value = S("test") }, 1) ==
          TA_OK);
    CHECK(take_logs(&f.otlp).len == 0);
    for (uint32_t round = 0; round < 3; round += 1u) {
        for (uint32_t at = 0; at <= round; at += 1u) {
            tah_otlp_host_log(&f.otlp, &(tah_otlp_host_log_params){ .event_name = S("tinyactors.message.dead_lettered"),
                                                                   .body = S("no such address"),
                                                                   .key = S("tinyactors.dead_letter.reason"),
                                                                   .value = S("no-route") });
        }
        ta_str bytes = take_logs(&f.otlp);
        CHECK(bytes.len != 0);
        reader r = { .at = (const uint8_t *)bytes.ptr, .end = (const uint8_t *)bytes.ptr + bytes.len };
        CHECK(well_formed(r));
    }
    stop(&f);
}

/*
 * A definition core frees takes its cached names with it (core ADR 0079):
 * the event names, cached under definition 0, stay; the next record naming
 * the definition's states caches them again.
 */
static void forget_definition(void) {
    fixture f;
    start(&f, true);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 64) == TA_RUN_STABLE);
    (void)take(&f.otlp);
    uint32_t cached = f.otlp.name_count;
    uint32_t mine = 0;
    for (uint32_t at = 0; at < f.otlp.name_capacity; at += 1u) {
        mine += f.otlp.name_slots[at].used && f.otlp.name_slots[at].definition == f.definition ? 1u : 0u;
    }
    CHECK(mine != 0);
    tah_otlp_forget_definition(&f.otlp, f.definition);
    CHECK(f.otlp.name_count == cached - mine);
    for (uint32_t at = 0; at < f.otlp.name_capacity; at += 1u) {
        CHECK(!(f.otlp.name_slots[at].used && f.otlp.name_slots[at].definition == f.definition));
    }
    /* Forgetting what is not cached changes nothing. */
    tah_otlp_forget_definition(&f.otlp, f.definition);
    CHECK(f.otlp.name_count == cached - mine);
    /* The encoder still names the definition's states: they are cached again. */
    CHECK(ta_actor_destroy(f.core, f.actor) == TA_OK);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 64) == TA_RUN_STABLE);
    CHECK(take(&f.otlp).len != 0);
    CHECK(f.otlp.name_count > cached - mine);
    stop(&f);
}

/*
 * The other chart: one state, written in a named file, under an explicit
 * identity whose revision is not UTF-8 and with no <scxml name>.
 */
static const ta_source_span other_spans[] = { { .source = 0, .line = 3, .column = 5 } };
static const ta_scxml_source other_sources[] = { { .name = S("file:///charts/other.scxml") } };
static const ta_scxml_state_spec other_roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("only"), .span = 1 },
    END,
};
static const uint8_t other_revision[] = { 0xff, 0xfe, 0x00 };
static const ta_definition_identity other_identity = {
    .family = { .name = S("other") },
    .revision = { .ptr = other_revision, .len = sizeof other_revision },
};
static const ta_scxml_spec other_chart = { .identity = &other_identity, .states = other_roots,
                                           .spans = other_spans, .span_count = 1,
                                           .sources = other_sources, .source_count = 1 };

static ta_definition_id define_other(fixture *f) {
    ta_allocator core_allocator = tah_allocator_for(&f->allocator, TAH_MEMORY_CORE);
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(f->core, &(ta_definition_compile_params){ .spec = &other_chart,
                                                                                .datamodel = ta_null_datamodel(),
                                                                                .allocator = core_allocator,
                                                                                .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(f->core, definition, &id) == TA_OK);
    return id;
}

static const scope_view *scope_of(const scope_view *scopes, uint32_t count, ta_definition_id definition) {
    for (uint32_t at = 0; at < count; at += 1u) {
        if (scopes[at].id == (int64_t)definition) return &scopes[at];
    }
    return NULL;
}

/*
 * Two definitions' spans in one request make two ScopeSpans, each naming
 * its definition (napi ADR 0028): a UTF-8 revision is a string and another
 * is bytes; a chart with no name and no spans leaves those out.
 */
static void scopes_per_definition(void) {
    fixture f;
    start(&f, true);
    ta_definition_id other = define_other(&f);
    ta_actor_id second = 0;
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) == TA_OK);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = other, .out = &second }) == TA_OK);
    CHECK(resume(&f, 100) == TA_RUN_STABLE);
    f.actor = second;
    CHECK(resume(&f, 100) == TA_RUN_STABLE);
    ta_str bytes = take(&f.otlp);
    span_view spans[32];
    uint32_t span_count = read_spans(bytes, spans, 32);
    scope_view scopes[4];
    uint32_t count = read_scopes(bytes, scopes, 4);
    CHECK(count == 2);
    CHECK(scopes[0].spans + scopes[1].spans == span_count);
    const scope_view *chain = scope_of(scopes, count, f.definition);
    const scope_view *mine = scope_of(scopes, count, other);
    CHECK(chain != NULL && mine != NULL);
    CHECK(strcmp(chain->name, "tinyactors") == 0 && strcmp(mine->name, "tinyactors") == 0);
    CHECK(strcmp(chain->family, "chain") == 0 && !chain->revision_bytes && chain->revision_len != 0);
    CHECK(strcmp(chain->document, "chain") == 0 && chain->source[0] == '\0');
    CHECK(strcmp(mine->family, "other") == 0 && mine->revision_bytes);
    CHECK(mine->revision_len == sizeof other_revision && memcmp(mine->revision, other_revision, 3) == 0);
    CHECK(mine->document[0] == '\0' && strcmp(mine->source, "file:///charts/other.scxml") == 0);
    stop(&f);
}

/*
 * A definition retired and freed in the turn of its last span is still
 * named: its scope was written with its first span, while it was alive.
 */
static void freed_in_turn(void) {
    fixture f;
    start(&f, true);
    ta_definition_id other = define_other(&f);
    CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = other, .out = &f.actor }) == TA_OK);
    CHECK(resume(&f, 100) == TA_RUN_STABLE);
    CHECK(ta_actor_destroy(f.core, f.actor) == TA_OK);
    CHECK(ta_system_retire_definition(f.core, other) == TA_OK);
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(f.core, other, &view) == TA_ERR_NOT_FOUND); /* freed */
    tah_otlp_forget_definition(&f.otlp, other);
    scope_view scopes[2];
    CHECK(read_scopes(take(&f.otlp), scopes, 2) == 1);
    CHECK(scopes[0].id == (int64_t)other && strcmp(scopes[0].family, "other") == 0);
    CHECK(strcmp(scopes[0].source, "file:///charts/other.scxml") == 0);
    stop(&f);
}

/*
 * Every allocation the encoder makes can fail: refused from each size in
 * turn, a run of two definitions' records drops what did not fit, and
 * every request is still well-formed, with each span in exactly one scope
 * and nothing leaked (stop() checks).
 */
static void scopes_out_of_memory(void) {
    uint64_t dropped = 0;
    for (size_t refuse = 16; refuse <= 8192; refuse *= 2u) {
        for (int round = 0; round < 2; round += 1) {
            fixture f;
            start(&f, true);
            ta_definition_id other = define_other(&f);
            ta_actor_id second = 0;
            CHECK(ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) ==
                  TA_OK);
            if (round == 1) {
                (void)take(&f.otlp); /* the buffers already hold some capacity */
            }
            tah_allocator_refuse_from(&f.allocator, refuse);
            (void)ta_actor_spawn(f.core, &(ta_actor_spawn_params){ .definition = other, .out = &second });
            (void)ta_actor_resume(f.core, &(ta_actor_resume_params){ .actor = f.actor, .budget = 100,
                                                                     .status = &(ta_run_status){ 0 } });
            tah_allocator_refuse_from(&f.allocator, 0);
            ta_str bytes = take(&f.otlp);
            if (bytes.len != 0) {
                span_view spans[32];
                uint32_t span_count = read_spans(bytes, spans, 32);
                scope_view scopes[4];
                uint32_t count = read_scopes(bytes, scopes, 4);
                uint32_t in_scopes = 0;
                for (uint32_t at = 0; at < count && at < 4; at += 1u) {
                    CHECK(scopes[at].spans != 0);
                    in_scopes += scopes[at].spans;
                }
                CHECK(in_scopes == span_count);
            }
            dropped += f.otlp.dropped;
            stop(&f);
        }
    }
    CHECK(dropped != 0); /* the sweep reached the encoder's own allocations */
}

int main(void) {
    scopes_per_definition();
    freed_in_turn();
    scopes_out_of_memory();
    forget_definition();
    logs();
    many_open();
    open_across_turns();
    end_all();
    abandoned();
    golden();
    free(taken);
    puts("otlp_test passed");
    return 0;
}
