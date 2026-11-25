/* snapshot.c — what the runtime writes, read with core's own readers
 * (tool.h, docs/cli.md): tinyactors inspect of a snapshot or a blob, and
 * tinyactors actors.
 *
 * A snapshot (ta_host_snapshot_pack's bytes) is unpacked with
 * ta_host_snapshot_unpack and restored with ta_host_restore into a host made
 * in the invocation's arena, whose segments are then read through the
 * system's inspection readers. The datamodels' state is not restored: each
 * datamodel the images name is bound as one that does nothing, and nothing
 * runs, so no datamodel is ever called; the state is shown as it was packed
 * (decoded when it is one CBOR item). A blob is read in place with the
 * system's blob readers (ta_blob_read). Snapshots and blobs are raw memory
 * of one build of the library, so this build reads its own format only and
 * says so of another. Freestanding. */
#include "tool.h"

#include "base/memory.h"
#include "system/system_internal.h"
#include <tinyactors/cbor.h>
#include <tinyactors/host.h>

#define T TA_CLI_TEXT

/* The most datamodels a host binds (ta_host_environment.datamodel_count). */
enum { SNAPSHOT_DATAMODELS_MAX = 8 };

static const uint64_t nanoseconds_per_second = 1000000000u;

/* ---- reading ------------------------------------------------------------------------- */

/* An input's bytes, 8-aligned in the arena; a failure told and t->exit set. */
static ta_status read_input(tool *t, ta_string_view path, ta_bytes *out) {
    tool_read read = {path, t->arena};
    if (t->platform->read(t->platform->context, &read, out) == TA_OK) return TA_OK;
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("cannot read "));
    tool_text_append(&text, path);
    tool_complain(t, tool_text_view(&text));
    t->exit = TOOL_EXIT_FAILURE;
    return TA_ERROR_INVALID;
}

static void out_of_memory(tool *t) {
    tool_complain(t, ta_string("out of memory"));
    t->exit = TOOL_EXIT_FAILURE;
}

/* The version a format writes after its four bytes of magic: every blob and
 * snapshot of the library starts so (docs/durability.md, "Blob formats"). */
static uint32_t format_version(const void *bytes, size_t length) {
    uint32_t version = 0;
    if (bytes && length >= 8) ta_memcpy(&version, (const unsigned char *)bytes + 4, sizeof version);
    return version;
}

static ta_string_view status_name(ta_status status) {
    return ta_string(ta_status_name(status));
}

/* ---- values -------------------------------------------------------------------------- */

/* 1 when `bytes` are one well-formed CBOR item and nothing after it. */
static int is_one_item(ta_bytes bytes) {
    if (!bytes.length) return 0;
    ta_cbor_reader reader;
    ta_cbor_item item;
    ta_cbor_reader_init(&reader, bytes);
    int32_t read;
    while ((read = ta_cbor_read(&reader, &item)) == 1)
        continue;
    return read == 0;
}

/* A datamodel's bytes, as members of the open object: their length, and
 * their value when they are one CBOR item (the bindings' states and blobs
 * are), else that they are the datamodel's own encoding (quickjs's is). */
static void report_datamodel_bytes(tool *t, ta_bytes bytes) {
    ta_report *report = &t->report;
    ta_report_number(report, ta_string("bytes"), bytes.length);
    if (!bytes.length) {
        ta_report_text(report, ta_string("encoding"), ta_string("none"));
    } else if (is_one_item(bytes)) {
        ta_report_text(report, ta_string("encoding"), ta_string("cbor"));
        ta_report_value(report, ta_string("value"), bytes);
    } else {
        ta_report_text(report, ta_string("encoding"), ta_string("opaque"));
        ta_report_text(report, ta_string("note"),
                       ta_string("not one CBOR item: the datamodel's own encoding (the ECMAScript "
                                 "datamodel's is quickjs's), which only that datamodel reads"));
    }
}

/* A time `delta` ticks away as SCXML writes a delay ("10s", "1500ms"),
 * or "never" past 2^64 nanoseconds. */
static ta_string_view delay_text(tool *t, uint64_t delta, uint64_t ticks_per_second) {
    tool_text text;
    tool_text_init(t, &text);
    uint64_t seconds = ticks_per_second ? delta / ticks_per_second : 0;
    uint64_t rest = ticks_per_second ? delta % ticks_per_second : 0;
    if (!ticks_per_second || seconds > UINT64_MAX / nanoseconds_per_second - 1u) {
        tool_text_append(&text, ta_string("never"));
        return tool_text_view(&text);
    }
    /* rest < ticks_per_second: rest * 10^9 / ticks_per_second < 10^9, in two steps */
    uint64_t fraction = rest < UINT64_MAX / nanoseconds_per_second
                            ? rest * nanoseconds_per_second / ticks_per_second
                            : rest / (ticks_per_second / nanoseconds_per_second);
    tool_text_duration(&text, seconds * nanoseconds_per_second + fraction);
    return tool_text_view(&text);
}

/* ---- a snapshot, restored ------------------------------------------------------------- */

typedef struct restored {
    ta_bytes pack;
    ta_host_snapshot_contents contents;
    ta_status unpacked; /* ta_host_snapshot_unpack's */
    ta_status status;   /* ta_host_restore's, or why it was not tried */
    ta_host *host;      /* NULL when not restored */
    ta_allocator allocator;
} restored;

/* A datamodel bound by name that does nothing: the restore attaches no
 * chart to it and nothing runs, so it is never called. */
static ta_status bind_name(tool *t, ta_string_view name, ta_datamodel_binding *bindings,
                           uint32_t *count) {
    if (tool_equal(name, ta_string("null"))) return TA_OK; /* every system has it */
    for (uint32_t i = 0; i < *count; i++)
        if (tool_equal(name, ta_string(bindings[i].vtable->name))) return TA_OK;
    if (*count >= SNAPSHOT_DATAMODELS_MAX) return TA_ERROR_LIMIT;
    ta_datamodel_vtable *vtable =
        (ta_datamodel_vtable *)ta_growable_arena_alloc(t->arena, sizeof *vtable, 8);
    char *text = (char *)ta_growable_arena_alloc(t->arena, name.length + 1u, 1);
    if (!vtable || !text) return TA_ERROR_NO_MEMORY;
    ta_memset(vtable, 0, sizeof *vtable);
    if (name.length) ta_memcpy(text, name.pointer, name.length);
    text[name.length] = 0;
    vtable->size = sizeof *vtable;
    vtable->name = text;
    bindings[*count].vtable = vtable;
    bindings[*count].datamodel = (void *)0;
    (*count)++;
    return TA_OK;
}

/* Unpack `pack`, and restore it when it unpacks. */
static void restore(tool *t, ta_bytes pack, restored *out) {
    ta_memset(out, 0, sizeof *out);
    out->pack = pack;
    out->contents.size = sizeof out->contents;
    out->allocator = ta_growable_arena_allocator(t->arena);
    out->unpacked = ta_host_snapshot_unpack(pack, &out->contents);
    out->status = out->unpacked;
    if (out->unpacked != TA_OK) return;
    uint32_t count = out->contents.image_count;
    ta_bytes *images = (ta_bytes *)ta_growable_arena_alloc(t->arena, sizeof *images * (count + 1u), 8);
    ta_datamodel_binding bindings[SNAPSHOT_DATAMODELS_MAX];
    uint32_t binding_count = 0;
    if (!images) {
        out->status = TA_ERROR_NO_MEMORY;
        return;
    }
    for (uint32_t i = 0; i < count && out->status == TA_OK; i++) {
        images[i] = ta_host_snapshot_image(pack, i);
        ta_image image;
        if (!images[i].length) continue;
        out->status = ta_image_open(images[i], &image);
        ta_image_info info;
        ta_memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (out->status == TA_OK) ta_image_get_info(&image, &info);
        for (uint32_t c = 0; out->status == TA_OK && c < info.chart_count; c++)
            out->status = bind_name(t, ta_image_chart_datamodel(&image, c), bindings, &binding_count);
    }
    if (out->status != TA_OK) return;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){(void *)0, 0, 0}, &out->allocator);
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("systemID"));
    ta_cbor_write_text(&writer, out->contents.system_id);
    ta_bytes config;
    out->status = ta_cbor_writer_bytes(&writer, &config);
    if (out->status != TA_OK) return;
    ta_host_environment environment;
    ta_memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = binding_count;
    environment.datamodels = bindings;
    environment.allocator = out->allocator;
    ta_host_snapshot_input input = {config,
                                    {out->contents.host, out->contents.host_length},
                                    images,
                                    count,
                                    0};
    out->status = ta_host_restore(&environment, &input, &out->host);
}

static void release(restored *r) {
    if (r->host) ta_host_free(r->host);
    r->host = (ta_host *)0;
}

/* Why a snapshot was not restored, in words. */
static ta_string_view restore_refusal(tool *t, const restored *r) {
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, status_name(r->status));
    tool_text_append(&text, r->status == TA_ERROR_INVALID
                                ? ta_string(": this build of the library cannot restore it (a "
                                            "snapshot of another build's format, or a damaged one)")
                            : r->status == TA_ERROR_LIMIT
                                ? ta_string(": its images name more datamodels than a host binds")
                                : ta_string(""));
    return tool_text_view(&text);
}

/* The segment's system and image view; 0 when the segment is retired. */
typedef struct segment_view {
    ta_system *system;
    const ta_image *image;
    const ta_image_view *view;
} segment_view;

static int segment_of(const restored *r, uint32_t segment, segment_view *out) {
    ta_host_segment_info info;
    ta_memset(out, 0, sizeof *out);
    info.size = sizeof info;
    if (!r->host || ta_host_segment_info_get(r->host, segment, &info) != TA_OK || !info.live) return 0;
    out->system = ta_host_system(r->host, segment);
    out->image = info.image;
    out->view = ta_image_view_of(info.image);
    return out->system != (ta_system *)0;
}

/* Every live actor of a segment, a page at a time. */
typedef struct actor_cursor {
    uint32_t cursor, at, count;
    uint32_t handles[64];
} actor_cursor;

static uint32_t next_actor(const segment_view *segment, actor_cursor *cursor) {
    if (cursor->at == cursor->count) {
        cursor->count = ta_system_actors(segment->system, &cursor->cursor,
                                         (ta_uint32_array){cursor->handles, 64});
        cursor->at = 0;
        if (!cursor->count) return 0;
    }
    return cursor->handles[cursor->at++];
}

static ta_string_view actor_state_name(uint32_t state) {
    static const ta_string_view names[] = {
        [TA_ACTOR_FREE] = T("free"),
        [TA_ACTOR_IDLE] = T("idle"),
        [TA_ACTOR_RUNNABLE] = T("runnable"),
        [TA_ACTOR_WAIT_WORKSPACE] = T("waiting for a workspace"),
        [TA_ACTOR_WAIT_EXTERNAL] = T("waiting for the host to acknowledge a send"),
        [TA_ACTOR_DONE] = T("done"),
        [TA_ACTOR_HIBERNATED] = T("hibernated")};
    return state < sizeof names / sizeof names[0] ? names[state] : ta_string("unknown");
}

/* The charts of an image, by name, separated by commas. */
static ta_string_view chart_names(tool *t, const ta_image *image) {
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(image, &info);
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t c = 0; c < info.chart_count; c++) {
        if (c) tool_text_append(&text, ta_string(", "));
        tool_text_append(&text, ta_image_chart_name(image, c));
    }
    return tool_text_view(&text);
}

static uint64_t ticks_per_second_of(const restored *r) {
    segment_view segment;
    for (uint32_t s = 0; r->host && s < ta_host_segment_count(r->host); s++)
        if (segment_of(r, s, &segment)) return segment.system->config.ticks_per_second;
    return 0;
}

static int identity_of(const restored *r, uint32_t *identity) {
    segment_view segment;
    for (uint32_t s = 0; r->host && s < ta_host_segment_count(r->host); s++)
        if (segment_of(r, s, &segment)) {
            *identity = segment.system->config.identity;
            return 1;
        }
    return 0;
}

/* The pack's own format: the number after "tinyactors-snapshot ". */
static uint64_t pack_format(ta_bytes pack) {
    static const char prefix[] = "tinyactors-snapshot ";
    uint64_t version = 0;
    const char *text = (const char *)pack.pointer;
    for (size_t at = sizeof prefix - 1; at < pack.length && text[at] >= '0' && text[at] <= '9'; at++)
        version = version * 10u + (uint64_t)(text[at] - '0');
    return version;
}

/* ---- inspect: a snapshot ---------------------------------------------------------------- */

static void inspect_snapshot(tool *t, ta_bytes pack) {
    ta_report *report = &t->report;
    restored r;
    restore(t, pack, &r);
    ta_report_text(report, ta_string("from"), ta_string("snapshot"));
    ta_report_number(report, ta_string("bytes"), pack.length);
    ta_report_number(report, ta_string("format"), pack_format(pack));
    if (r.unpacked != TA_OK) {
        ta_report_boolean(report, ta_string("readable"), 0);
        ta_report_text(report, ta_string("why"),
                       ta_string("it does not unpack: a snapshot of another format, or a damaged one"));
        t->exit = TOOL_EXIT_REFUSED;
        return;
    }
    ta_report_number(report, ta_string("hostFormat"), format_version(r.contents.host, r.contents.host_length));
    ta_report_text(report, ta_string("systemID"), r.contents.system_id);
    ta_report_number(report, ta_string("time"), r.contents.time);
    uint64_t ticks_per_second = ticks_per_second_of(&r);
    if (ticks_per_second) ta_report_number(report, ta_string("ticksPerSecond"), ticks_per_second);
    uint32_t identity;
    if (identity_of(&r, &identity)) ta_report_text(report, ta_string("identity"), tool_hexadecimal(t, identity));
    ta_report_boolean(report, ta_string("restored"), r.host != (ta_host *)0);
    if (!r.host) ta_report_text(report, ta_string("why"), restore_refusal(t, &r));
    uint64_t actors = 0, hibernated = 0;
    ta_report_list(report, (ta_report_list_names){ta_string("segments"), ta_string("segment")});
    for (uint32_t s = 0; s < r.contents.image_count; s++) {
        ta_bytes bytes = ta_host_snapshot_image(pack, s);
        ta_image image;
        int opened = bytes.length && ta_image_open(bytes, &image) == TA_OK;
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("index"), s);
        ta_report_boolean(report, ta_string("live"), bytes.length != 0);
        ta_image_info info;
        info.size = sizeof info;
        if (opened) ta_image_get_info(&image, &info);
        ta_report_text(report, ta_string("imageHash"), opened ? tool_hexadecimal(t, info.content_hash) : ta_string(""));
        ta_report_number(report, ta_string("imageBytes"), bytes.length);
        ta_report_text(report, ta_string("charts"), opened ? chart_names(t, &image) : ta_string(""));
        segment_view segment;
        uint64_t here = 0, here_hibernated = 0;
        if (segment_of(&r, s, &segment)) {
            actor_cursor cursor = {0, 0, 0, {0}};
            for (uint32_t actor; (actor = next_actor(&segment, &cursor)) != 0;) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(segment.system, actor, &actor_info) != TA_OK) continue;
                here++;
                here_hibernated += actor_info.state == TA_ACTOR_HIBERNATED;
            }
        }
        ta_report_number(report, ta_string("actors"), here);
        ta_report_number(report, ta_string("hibernated"), here_hibernated);
        actors += here;
        hibernated += here_hibernated;
        ta_report_end(report);
    }
    ta_report_end(report);
    if (r.host) {
        ta_report_number(report, ta_string("actors"), actors);
        ta_report_number(report, ta_string("hibernated"), hibernated);
    }
    /* the datamodels' state last: it may be long */
    ta_report_object(report, ta_string("datamodelState"));
    ta_report_text(report, ta_string("label"), r.contents.datamodel);
    report_datamodel_bytes(t, (ta_bytes){r.contents.state, r.contents.state_length});
    ta_report_end(report);
    release(&r);
}

/* A host snapshot alone (ta_host_snapshot's bytes): its segments' image
 * hashes, read by ta_host_snapshot_segments; its actors need the images,
 * which a pack holds. */
static void inspect_host_snapshot(tool *t, ta_bytes bytes) {
    ta_report *report = &t->report;
    ta_report_text(report, ta_string("from"), ta_string("host snapshot"));
    ta_report_number(report, ta_string("bytes"), bytes.length);
    ta_report_number(report, ta_string("format"), format_version(bytes.pointer, bytes.length));
    uint32_t count = ta_host_snapshot_segments(bytes, (ta_uint64_array){(uint64_t *)0, 0});
    ta_report_boolean(report, ta_string("readable"), count != 0);
    ta_report_text(report, ta_string("note"),
                   ta_string("no images and no datamodel state: give a pack (ta_host_snapshot_pack) "
                             "to read its actors"));
    if (!count) {
        t->exit = TOOL_EXIT_REFUSED;
        return;
    }
    uint64_t *hashes = (uint64_t *)ta_growable_arena_alloc(t->arena, sizeof *hashes * count, 8);
    if (!hashes) {
        out_of_memory(t);
        return;
    }
    ta_host_snapshot_segments(bytes, (ta_uint64_array){hashes, count});
    ta_report_list(report, (ta_report_list_names){ta_string("segments"), ta_string("segment")});
    for (uint32_t s = 0; s < count; s++) {
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("index"), s);
        ta_report_boolean(report, ta_string("live"), hashes[s] != 0);
        ta_report_text(report, ta_string("imageHash"), hashes[s] ? tool_hexadecimal(t, hashes[s]) : ta_string(""));
        ta_report_end(report);
    }
    ta_report_end(report);
}

/* ---- inspect: a blob ------------------------------------------------------------------------- */

/* The image a blob was written with, from the inputs after it: an image,
 * charts, a manifest, or a snapshot (the segment that ran that image). */
typedef struct blob_image {
    int given, found;
    ta_image image;
    int identity_known; /* a snapshot's host: whether the blob wakes there */
    uint32_t identity;
} blob_image;

static ta_status find_blob_image(tool *t, uint64_t hash, blob_image *out) {
    ta_memset(out, 0, sizeof *out);
    uint32_t count = t->parsed->argument_count - 1u;
    const ta_string_view *paths = t->parsed->arguments + 1;
    if (!count) return TA_OK;
    out->given = 1;
    ta_bytes first;
    if (read_input(t, paths[0], &first) != TA_OK) return TA_ERROR_INVALID;
    if (count == 1 && tool_input_kind_of(first) == TOOL_INPUT_SNAPSHOT) {
        restored r;
        restore(t, first, &r);
        out->identity_known = identity_of(&r, &out->identity);
        release(&r);
        for (uint32_t s = 0; r.unpacked == TA_OK && s < r.contents.image_count && !out->found; s++) {
            ta_bytes bytes = ta_host_snapshot_image(first, s);
            ta_image_info info;
            info.size = sizeof info;
            if (!bytes.length || ta_image_open(bytes, &out->image) != TA_OK) continue;
            ta_image_get_info(&out->image, &info);
            out->found = info.content_hash == hash;
        }
        return TA_OK;
    }
    tool_image loaded;
    tool_inputs inputs = {count, paths,
                          tool_count(t, TOOL_OPTION_WARNINGS_AS_ERRORS) ? TA_COMPILE_WERROR : 0u, TA_NIL};
    if (tool_load(t, inputs, &loaded) != TA_OK) {
        if (t->exit == TOOL_EXIT_REFUSED) tool_report_diagnostics(t, &loaded);
        return TA_ERROR_INVALID;
    }
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&loaded.image, &info);
    out->image = loaded.image;
    out->found = info.content_hash == hash;
    return TA_OK;
}

static void report_configuration(tool *t, const ta_image *image, const ta_uint32_array states,
                                  uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    ta_report_list(&t->report, (ta_report_list_names){ta_string("configuration"), ta_string("state")});
    for (uint32_t i = 0; i < states.capacity; i++)
        ta_report_text(&t->report, ta_string(""), tool_state_id(view, &view->charts[chart], states.pointer[i]));
    ta_report_end(&t->report);
}

static void report_blob_session(tool *t, const ta_blob_session *session, const blob_image *image) {
    ta_report *report = &t->report;
    ta_report_object(report, ta_string(""));
    ta_report_number(report, ta_string("session"), session->session);
    if (session->name.length) ta_report_text(report, ta_string("name"), session->name);
    if (session->parent) {
        ta_report_number(report, ta_string("parent"), session->parent);
        ta_report_number(report, ta_string("invokeSlot"), session->slot);
    }
    ta_report_number(report, ta_string("chartIndex"), session->chart);
    if (image->found && session->chart < ta_image_view_of(&image->image)->chart_count)
        ta_report_text(report, ta_string("chart"), ta_image_chart_name(&image->image, session->chart));
    ta_report_number(report, ta_string("macrosteps"), session->macrostep);
    if (image->found) {
        uint32_t count = ta_blob_configuration(session, &image->image, (ta_uint32_array){(uint32_t *)0, 0});
        uint32_t *states = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof *states * (count + 1u), 4);
        if (!states) {
            out_of_memory(t);
            return;
        }
        ta_blob_configuration(session, &image->image, (ta_uint32_array){states, count});
        report_configuration(t, &image->image, (ta_uint32_array){states, count}, session->chart);
    }
    ta_report_list(report, (ta_report_list_names){ta_string("delayed"), ta_string("send")});
    for (uint32_t i = 0; i < session->delayed_count; i++) {
        ta_blob_delayed delayed;
        if (ta_blob_delayed_at(session, i, &delayed) != TA_OK) break;
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("event"), delayed.event);
        ta_report_text(report, ta_string("sendid"), delayed.sendid);
        ta_report_text(report, ta_string("target"), delayed.target);
        ta_report_number(report, ta_string("due"), delayed.due);
        ta_report_number(report, ta_string("rank"), delayed.rank);
        ta_report_text(report, ta_string("data"),
                       delayed.payload_kind == 1   ? ta_string("value")
                       : delayed.payload_kind == 2 ? ta_string("handle (the writing process only)")
                                                   : ta_string("none"));
        if (delayed.payload_kind == 1 && is_one_item(delayed.payload))
            ta_report_value(report, ta_string("value"), delayed.payload);
        ta_report_end(report);
    }
    ta_report_end(report);
    ta_report_object(report, ta_string("datamodel"));
    report_datamodel_bytes(t, session->datamodel);
    ta_report_end(report);
    ta_report_end(report);
}

static void inspect_blob(tool *t, ta_bytes bytes) {
    ta_report *report = &t->report;
    ta_blob_contents contents;
    ta_status status = ta_blob_read(bytes, &contents);
    ta_report_text(report, ta_string("from"), contents.kind == TA_BLOB_EXPORT ? ta_string("export blob")
                                                                             : ta_string("hibernation blob"));
    ta_report_number(report, ta_string("bytes"), bytes.length);
    ta_report_number(report, ta_string("format"), contents.version);
    ta_report_boolean(report, ta_string("readable"), status == TA_OK);
    if (status != TA_OK) {
        ta_report_text(report, ta_string("why"),
                       status == TA_ERROR_UNSUPPORTED
                           ? ta_string("a blob of another format version: this build of the library "
                                       "reads, and wakes, its own only")
                           : ta_string("a damaged blob"));
        t->exit = TOOL_EXIT_REFUSED;
        return;
    }
    blob_image image;
    if (find_blob_image(t, contents.image_hash, &image) != TA_OK) return;
    ta_report_number(report, ta_string("session"), contents.session);
    ta_report_text(report, ta_string("imageHash"), tool_hexadecimal(t, contents.image_hash));
    if (image.given) ta_report_boolean(report, ta_string("imageMatches"), image.found);
    if (contents.kind == TA_BLOB_HIBERNATION) {
        ta_report_text(report, ta_string("identity"), tool_hexadecimal(t, contents.identity));
        ta_report_text(report, ta_string("wakes"),
                       ta_string("only in the host that hibernated it, or a host restored from its "
                                 "snapshot: one of this identity, running this image"));
        if (image.identity_known)
            ta_report_boolean(report, ta_string("wakesInThatSnapshot"),
                              image.found && image.identity == contents.identity);
    } else {
        ta_report_number(report, ta_string("sessions"), contents.session_count);
        ta_report_boolean(report, ta_string("migrated"), contents.migrated != 0);
    }
    ta_report_list(report, (ta_report_list_names){ta_string("sessions"), ta_string("session")});
    for (uint32_t i = 0; i < contents.session_count; i++) {
        ta_blob_session session;
        if (ta_blob_session_at(bytes, i, &session) != TA_OK) break;
        report_blob_session(t, &session, &image);
    }
    ta_report_end(report);
}

int tool_inspect_stored(tool *t) {
    ta_bytes bytes;
    if (!t->parsed->argument_count) return 0;
    ta_string_view path = t->parsed->arguments[0];
    tool_read read = {path, t->arena};
    if (t->platform->read(t->platform->context, &read, &bytes) != TA_OK) return 0; /* inspect says */
    uint32_t kind = tool_input_kind_of(bytes);
    if (kind != TOOL_INPUT_SNAPSHOT && kind != TOOL_INPUT_BLOB && kind != TOOL_INPUT_HOST_SNAPSHOT)
        return 0;
    if (kind != TOOL_INPUT_BLOB && t->parsed->argument_count > 1) {
        tool_complain(t, ta_string("inspect takes one snapshot, or a blob and the image it was written with"));
        t->exit = TOOL_EXIT_USAGE;
        return 1;
    }
    if (kind == TOOL_INPUT_SNAPSHOT) inspect_snapshot(t, bytes);
    else if (kind == TOOL_INPUT_HOST_SNAPSHOT) inspect_host_snapshot(t, bytes);
    else inspect_blob(t, bytes);
    return 1;
}

/* ---- actors ---------------------------------------------------------------------------------- */

/* 1 when --chart names none, or names this chart. */
static int chart_wanted(const tool *t, ta_string_view chart) {
    if (!tool_count(t, TOOL_OPTION_CHART)) return 1;
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope == TA_CLI_SCOPE_COMMAND && value->option.option == TOOL_OPTION_CHART &&
            tool_equal(value->text, chart))
            return 1;
    }
    return 0;
}

static int session_wanted(const tool *t, uint64_t session) {
    if (!tool_count(t, TOOL_ACTORS_SESSION)) return 1;
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        uint64_t wanted;
        if (value->option.scope == TA_CLI_SCOPE_COMMAND && value->option.option == TOOL_ACTORS_SESSION &&
            tool_decimal(value->text, &wanted) == TA_OK && wanted == session)
            return 1;
    }
    return 0;
}

/* The options' values checked: sessions are numbers, a segment one the
 * snapshot runs, each chart one of its images has. */
static ta_status check_options(tool *t, const restored *r, uint32_t *segment) {
    *segment = TA_NIL;
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        uint64_t number;
        if (value->option.scope != TA_CLI_SCOPE_COMMAND) continue;
        if (value->option.option == TOOL_ACTORS_SESSION && tool_decimal(value->text, &number) != TA_OK) {
            tool_text_append(&text, ta_string("--session takes a session's number"));
            break;
        }
        if (value->option.option == TOOL_OPTION_SEGMENT) {
            segment_view view;
            if (tool_decimal(value->text, &number) != TA_OK || number >= TA_NIL ||
                !segment_of(r, (uint32_t)number, &view)) {
                tool_text_append(&text, ta_string("no live segment "));
                tool_text_append(&text, value->text);
                tool_text_append(&text, ta_string(" in the snapshot"));
                break;
            }
            *segment = (uint32_t)number;
        }
        if (value->option.option == TOOL_OPTION_CHART) {
            int found = 0;
            segment_view view;
            for (uint32_t s = 0; s < ta_host_segment_count(r->host) && !found; s++)
                found = segment_of(r, s, &view) && ta_image_chart_by_name(view.image, value->text) != TA_NIL;
            if (!found) {
                tool_text_append(&text, ta_string("no chart '"));
                tool_text_append(&text, value->text);
                tool_text_append(&text, ta_string("' in the snapshot's images"));
                break;
            }
        }
    }
    ta_string_view complaint = tool_text_view(&text);
    if (!complaint.length) return TA_OK;
    tool_complain(t, complaint);
    t->exit = TOOL_EXIT_USAGE;
    return TA_ERROR_INVALID;
}

typedef struct actor_place {
    const segment_view *segment;
    uint32_t index; /* the segment's */
    uint32_t actor; /* its handle there */
    ta_time now;    /* the snapshot's time */
} actor_place;

static void report_actor(tool *t, const actor_place *place, const ta_actor_info *info) {
    ta_report *report = &t->report;
    ta_system *system = place->segment->system;
    const ta_image_view *view = place->segment->view;
    const ta_chart_record *chart = &view->charts[info->chart];
    uint64_t ticks_per_second = system->config.ticks_per_second;
    ta_report_object(report, ta_string(""));
    ta_report_number(report, ta_string("session"), info->serial);
    ta_report_text(report, ta_string("name"), ta_report_copy(report, ta_system_actor_name(system, place->actor)));
    ta_report_text(report, ta_string("chart"),
                   ta_report_copy(report, ta_image_chart_name(place->segment->image, info->chart)));
    ta_report_number(report, ta_string("segment"), place->index);
    ta_report_text(report, ta_string("state"), actor_state_name(info->state));
    if (info->parent) {
        ta_actor_info parent;
        parent.size = sizeof parent;
        if (ta_actor_info_get(system, info->parent, &parent) == TA_OK)
            ta_report_number(report, ta_string("parent"), parent.serial);
    }
    ta_report_number(report, ta_string("invocations"), info->invocations);
    ta_report_number(report, ta_string("queued"), info->mailbox_length);
    int hibernated = info->state == TA_ACTOR_HIBERNATED;
    if (hibernated)
        ta_report_text(report, ta_string("note"),
                       ta_string("hibernated: its configuration and datamodel are in its blob, in "
                                 "the host's store (tinyactors inspect BLOB SNAPSHOT reads it)"));
    /* the configuration, in document order */
    uint32_t count = ta_actor_config(system, place->actor, (ta_uint32_array){(uint32_t *)0, 0});
    uint32_t *states = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof *states * (count + 1u), 4);
    if (!states) {
        out_of_memory(t);
        return;
    }
    ta_actor_config(system, place->actor, (ta_uint32_array){states, count});
    ta_report_list(report, (ta_report_list_names){ta_string("configuration"), ta_string("state")});
    for (uint32_t i = 0; i < count; i++)
        ta_report_text(report, ta_string(""), ta_report_copy(report, tool_state_id(view, chart, states[i])));
    ta_report_end(report);
    /* its mailbox, in the order it takes the events */
    count = ta_system_actor_mailbox(system, place->actor, (ta_actor_queued_array){(ta_actor_queued *)0, 0});
    ta_actor_queued *queued = (ta_actor_queued *)ta_growable_arena_alloc(t->arena, sizeof *queued * (count + 1u), 8);
    if (!queued) {
        out_of_memory(t);
        return;
    }
    ta_system_actor_mailbox(system, place->actor, (ta_actor_queued_array){queued, count});
    ta_report_list(report, (ta_report_list_names){ta_string("mailbox"), ta_string("event")});
    for (uint32_t i = 0; i < count; i++) {
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("event"), ta_report_copy(report, queued[i].event));
        ta_report_text(report, ta_string("sendid"), ta_report_copy(report, queued[i].sendid));
        ta_report_end(report);
    }
    ta_report_end(report);
    /* its delayed sends, in firing order */
    count = ta_system_actor_timers(system, place->actor, (ta_actor_timer_array){(ta_actor_timer *)0, 0});
    ta_actor_timer *timers = (ta_actor_timer *)ta_growable_arena_alloc(t->arena, sizeof *timers * (count + 1u), 8);
    if (!timers) {
        out_of_memory(t);
        return;
    }
    ta_system_actor_timers(system, place->actor, (ta_actor_timer_array){timers, count});
    ta_report_list(report, (ta_report_list_names){ta_string("delayed"), ta_string("send")});
    for (uint32_t i = 0; i < count; i++) {
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("event"), ta_report_copy(report, timers[i].event));
        ta_report_text(report, ta_string("sendid"), ta_report_copy(report, timers[i].sendid));
        ta_report_number(report, ta_string("due"), timers[i].due);
        ta_report_text(report, ta_string("in"),
                       timers[i].due <= place->now
                           ? ta_string("overdue")
                           : delay_text(t, timers[i].due - place->now, ticks_per_second));
        ta_report_end(report);
    }
    ta_report_end(report);
    ta_report_end(report);
}

void tool_actors(tool *t) {
    ta_bytes bytes;
    ta_string_view path = t->parsed->arguments[0];
    if (read_input(t, path, &bytes) != TA_OK) return;
    if (tool_input_kind_of(bytes) != TOOL_INPUT_SNAPSHOT) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, path);
        tool_text_append(&text, ta_string(" is not a snapshot (ta_host_snapshot_pack's): actors reads one; "
                                          "inspect reads a blob"));
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    restored r;
    restore(t, bytes, &r);
    if (!r.host) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, path);
        tool_text_append(&text, ta_string(" cannot be restored: "));
        tool_text_append(&text, r.unpacked != TA_OK ? ta_string("it does not unpack (another format, "
                                                                "or damaged)")
                                                    : restore_refusal(t, &r));
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_REFUSED;
        return;
    }
    uint32_t only;
    if (check_options(t, &r, &only) != TA_OK) {
        release(&r);
        return;
    }
    ta_report *report = &t->report;
    ta_report_text(report, ta_string("snapshot"), path);
    ta_report_text(report, ta_string("systemID"), r.contents.system_id);
    ta_report_number(report, ta_string("time"), r.contents.time);
    ta_report_list(report, (ta_report_list_names){ta_string("actors"), ta_string("actor")});
    for (uint32_t s = 0; s < ta_host_segment_count(r.host); s++) {
        segment_view segment;
        if ((only != TA_NIL && s != only) || !segment_of(&r, s, &segment)) continue;
        actor_cursor cursor = {0, 0, 0, {0}};
        for (uint32_t actor; (actor = next_actor(&segment, &cursor)) != 0 && t->exit == TOOL_EXIT_OK;) {
            ta_actor_info info;
            info.size = sizeof info;
            if (ta_actor_info_get(segment.system, actor, &info) != TA_OK) continue;
            if (!session_wanted(t, info.serial) ||
                !chart_wanted(t, ta_image_chart_name(segment.image, info.chart)))
                continue;
            actor_place place = {&segment, s, actor, r.contents.time};
            report_actor(t, &place, &info);
        }
    }
    ta_report_end(report);
    release(&r);
}
