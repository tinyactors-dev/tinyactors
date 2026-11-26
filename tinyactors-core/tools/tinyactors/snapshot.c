/* snapshot.c — what the runtime writes, read with core's own readers
 * (tool.h, docs/cli.md): tinyactors inspect of a snapshot or a blob, and
 * tinyactors actors.
 *
 * A snapshot (ta_host_snapshot_pack's bytes) is unpacked with
 * ta_host_snapshot_unpack, and each live segment's region
 * (ta_host_snapshot_region) is read with ta_system_read into the
 * invocation's arena, with its image (ta_host_snapshot_image): a system no
 * one runs, read through the system's inspection readers. No host is made
 * and no datamodel is bound; the datamodels' state is shown as it was
 * packed (decoded when it is one CBOR item). A blob is read in place with the
 * system's blob readers (ta_blob_read). Snapshots and blobs are raw memory
 * of one build of the library, so this build reads its own format only and
 * says so of another. Freestanding. */
#include "tool.h"

#include "base/memory.h"
#include "system/system_internal.h"
#include <tinyactors/cbor.h>
#include <tinyactors/host.h>

#define T TA_CLI_TEXT

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

/* ---- a snapshot, read ------------------------------------------------------------------ */

/* A segment's system and image view; system NULL when the segment is
 * retired, or could not be read. */
typedef struct segment_view {
    ta_system *system;
    const ta_image *image;
    const ta_image_view *view;
} segment_view;

typedef struct restored {
    ta_bytes pack;
    ta_host_snapshot_contents contents;
    ta_status unpacked; /* ta_host_snapshot_unpack's */
    ta_status status;   /* the first ta_system_read's that failed, or why none was tried */
    int read;           /* every live segment was read */
    uint32_t segment_count;
    segment_view *segments; /* in the arena */
    ta_image *images;
} restored;

/* Unpack `pack`, and read each live segment's region (ta_system_read) into
 * the arena, with its image. */
static void restore(tool *t, ta_bytes pack, restored *out) {
    ta_memset(out, 0, sizeof *out);
    out->pack = pack;
    out->contents.size = sizeof out->contents;
    out->unpacked = ta_host_snapshot_unpack(pack, &out->contents);
    out->status = out->unpacked;
    if (out->unpacked != TA_OK) return;
    uint32_t count = out->contents.image_count;
    out->segments = (segment_view *)ta_growable_arena_alloc(t->arena, sizeof *out->segments * (count + 1u), 8);
    out->images = (ta_image *)ta_growable_arena_alloc(t->arena, sizeof *out->images * (count + 1u), 8);
    if (!out->segments || !out->images) {
        out->status = TA_ERROR_NO_MEMORY;
        return;
    }
    ta_memset(out->segments, 0, sizeof *out->segments * count);
    out->segment_count = count;
    for (uint32_t i = 0; i < count && out->status == TA_OK; i++) {
        ta_bytes image = ta_host_snapshot_image(pack, i);
        ta_bytes region = ta_host_snapshot_region(pack, i);
        if (!image.length || !region.length) continue; /* retired */
        out->status = ta_image_open(image, &out->images[i]);
        void *memory = out->status == TA_OK ? ta_growable_arena_alloc(t->arena, region.length, 8) : (void *)0;
        if (out->status == TA_OK && !memory) out->status = TA_ERROR_NO_MEMORY;
        if (out->status != TA_OK) break;
        ta_system_setup setup = {{memory, region.length}, &out->images[i], (const ta_system_config *)0,
                                 (const ta_datamodel_binding *)0, 0, 0};
        out->status = ta_system_read(&setup, region, &out->segments[i].system);
        out->segments[i].image = &out->images[i];
        out->segments[i].view = ta_image_view_of(&out->images[i]);
    }
    out->read = out->status == TA_OK;
}

/* Why a snapshot was not read, in words. */
static ta_string_view restore_refusal(tool *t, const restored *r) {
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, status_name(r->status));
    tool_text_append(&text, r->status == TA_ERROR_INVALID
                                ? ta_string(": this build of the library cannot read it (a "
                                            "snapshot of another build's layout, or a damaged one)")
                                : ta_string(""));
    return tool_text_view(&text);
}

static int segment_of(const restored *r, uint32_t segment, segment_view *out) {
    ta_memset(out, 0, sizeof *out);
    if (!r->read || segment >= r->segment_count || !r->segments[segment].system) return 0;
    *out = r->segments[segment];
    return 1;
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
    for (uint32_t s = 0; s < r->segment_count; s++)
        if (segment_of(r, s, &segment)) return segment.system->config.ticks_per_second;
    return 0;
}

static int identity_of(const restored *r, uint32_t *identity) {
    segment_view segment;
    for (uint32_t s = 0; s < r->segment_count; s++)
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

/* The library version a pack's layout names (its "library"), empty when it
 * names none. */
static ta_string_view layout_library(tool *t, ta_bytes layout) {
    ta_string_view none = {(const char *)0, 0};
    ta_cbor_document document = {layout, (ta_cbor_token *)0, 0, 0};
    if (ta_cbor_decode(&document) != TA_ERROR_NO_MEMORY) return none;
    document.tokens = (ta_cbor_token *)ta_growable_arena_alloc(
        t->arena, sizeof *document.tokens * document.count, sizeof(uint64_t));
    document.capacity = document.count;
    if (!document.tokens || ta_cbor_decode(&document) != TA_OK) return none;
    uint32_t at = ta_cbor_member(&document, 0, ta_string("library"));
    return at == TA_NIL ? none : ta_cbor_string(&document, at);
}

/* This build's layout id (ta_system_layout_description); 0 if it cannot be
 * written. */
static uint64_t this_layout_id(tool *t) {
    ta_buffer probe = {(void *)0, 0, 0};
    if (ta_system_layout_description(&probe, (uint64_t *)0) != TA_ERROR_NO_MEMORY) return 0;
    ta_buffer description = {ta_growable_arena_alloc(t->arena, probe.length, 1), probe.length, 0};
    uint64_t id = 0;
    if (!description.pointer || ta_system_layout_description(&description, &id) != TA_OK) return 0;
    return id;
}

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
    ta_report_text(report, ta_string("library"), layout_library(t, r.contents.layout));
    ta_report_text(report, ta_string("layoutID"), tool_hexadecimal(t, r.contents.layout_id));
    ta_report_boolean(report, ta_string("thisLayout"), r.contents.layout_id == this_layout_id(t));
    ta_report_text(report, ta_string("systemID"), r.contents.system_id);
    ta_report_number(report, ta_string("time"), r.contents.time);
    uint64_t ticks_per_second = ticks_per_second_of(&r);
    if (ticks_per_second) ta_report_number(report, ta_string("ticksPerSecond"), ticks_per_second);
    uint32_t identity;
    if (identity_of(&r, &identity)) ta_report_text(report, ta_string("identity"), tool_hexadecimal(t, identity));
    ta_report_boolean(report, ta_string("restored"), r.read);
    if (!r.read) ta_report_text(report, ta_string("why"), restore_refusal(t, &r));
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
    if (r.read) {
        ta_report_number(report, ta_string("actors"), actors);
        ta_report_number(report, ta_string("hibernated"), hibernated);
    }
    /* the datamodels' state last: it may be long */
    ta_report_object(report, ta_string("datamodelState"));
    ta_report_text(report, ta_string("label"), r.contents.datamodel);
    report_datamodel_bytes(t, (ta_bytes){r.contents.state, r.contents.state_length});
    ta_report_end(report);
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
            for (uint32_t s = 0; s < r->segment_count && !found; s++)
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
    if (!r.read) {
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
            return;
    }
    ta_report *report = &t->report;
    ta_report_text(report, ta_string("snapshot"), path);
    ta_report_text(report, ta_string("systemID"), r.contents.system_id);
    ta_report_number(report, ta_string("time"), r.contents.time);
    ta_report_list(report, (ta_report_list_names){ta_string("actors"), ta_string("actor")});
    for (uint32_t s = 0; s < r.segment_count; s++) {
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
}
