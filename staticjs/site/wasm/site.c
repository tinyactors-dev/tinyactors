/*
 * The documentation site's C layer over staticjs, compiled with the
 * amalgamation into public/staticjs-site.wasm (`mise run site:wasm`).
 *
 * The site's live samples and its pipeline explorer call these functions.
 * They take and return numbers only, so the page's JavaScript reads no
 * struct of the library's: a session (sjs_site) is an opaque address, and
 * what a compile or a run produced is read back through getters. The one
 * layout the page reads is this file's own event record (below).
 *
 * A session drives every stage as an embedder does: the source is pushed to
 * a parser in chunks of a chosen size; a recording sink sees each event
 * before the compiler does; the evaluator runs on slices of fuel; the
 * writer writes through a buffer of a chosen size.
 */
#include "staticjs.h"

#define SJS_SITE __attribute__((visibility("default")))

/* An event record: SJS_SITE_EVENT_WORDS uint32_t words, in this order. The
 * event's text is in the session's text arena, at text_offset. */
enum {
    EV_KIND, EV_COUNT, EV_NODE,
    EV_AT_OFFSET, EV_AT_LINE, EV_AT_COLUMN,
    EV_START_OFFSET, EV_START_LINE, EV_START_COLUMN,
    EV_TEXT_OFFSET, EV_TEXT_LENGTH, EV_CHUNK,
    SJS_SITE_EVENT_WORDS
};

/* Where an error was reported. */
enum { STAGE_NONE, STAGE_PARSER, STAGE_COMPILER, STAGE_EVALUATOR, STAGE_WRITER };

typedef struct {
    uint8_t *ptr;
    uint32_t length;
    uint32_t capacity;
} site_bytes;

typedef struct {
    sjs_context *context;
    sjs_vm *vm;
    sjs_program *program;
    sjs_value result;
    bool has_result;
    site_bytes source;
    site_bytes events;   /* records of SJS_SITE_EVENT_WORDS words */
    site_bytes numbers;  /* one double per event */
    site_bytes text;
    site_bytes output;
    site_bytes tree;      /* records of SJS_SITE_NODE_WORDS words */
    site_bytes tree_text;
    bool tree_truncated;
    uint32_t chunk;      /* the chunk being pushed, for the record */
    uint32_t chunks;
    uint32_t pieces;
    uint32_t program_bytes;
    sjs_compiler *compiler;
    sjs_sink next;
    int32_t error_code;
    uint32_t error_stage;
    sjs_position error_at;
    site_bytes message;
    site_bytes subject;
} sjs_site;

static void copy_bytes(uint8_t *to, const uint8_t *from, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) to[i] = from[i];
}

/* Room for `extra` more bytes; false when the allocator refuses. */
static bool reserve(site_bytes *b, uint32_t extra) {
    if (b->length + extra <= b->capacity) return true;
    uint32_t capacity = b->capacity < 256u ? 256u : b->capacity;
    while (capacity < b->length + extra) capacity *= 2u;
    uint8_t *grown = sjs_wasm_alloc(capacity, 8);
    if (grown == NULL) return false;
    if (b->ptr != NULL) {
        copy_bytes(grown, b->ptr, b->length);
        sjs_wasm_free(b->ptr, b->capacity, 8);
    }
    b->ptr = grown;
    b->capacity = capacity;
    return true;
}

static bool append(site_bytes *b, const void *data, uint32_t length) {
    if (!reserve(b, length)) return false;
    copy_bytes(b->ptr + b->length, data, length);
    b->length += length;
    return true;
}

static void release_bytes(site_bytes *b) {
    if (b->ptr != NULL) sjs_wasm_free(b->ptr, b->capacity, 8);
    *b = (site_bytes){ NULL, 0, 0 };
}

static void set_error(sjs_site *s, const sjs_error *error, uint32_t stage) {
    s->error_code = (int32_t)error->code;
    s->error_stage = stage;
    s->error_at = error->at;
    s->message.length = 0;
    s->subject.length = 0;
    append(&s->message, error->message.ptr, error->message.length);
    append(&s->subject, error->subject.ptr, error->subject.length);
}

static void clear_error(sjs_site *s) {
    s->error_code = 0;
    s->error_stage = STAGE_NONE;
    s->error_at = (sjs_position){ 0, 0, 0 };
    s->message.length = 0;
    s->subject.length = 0;
}

/* The recording sink: one record per event, then the compiler's turn. */
static sjs_result record_event(void *self, const sjs_event *event) {
    sjs_site *s = self;
    uint32_t words[SJS_SITE_EVENT_WORDS] = {
        (uint32_t)event->kind, event->count, event->node,
        event->at.offset, event->at.line, event->at.column,
        event->start.offset, event->start.line, event->start.column,
        s->text.length, event->text.length, s->chunk,
    };
    if (!append(&s->text, event->text.ptr, event->text.length) || !append(&s->events, words, sizeof words) ||
        !append(&s->numbers, &event->number, sizeof event->number)) {
        return SJS_ERR_NOMEM;
    }
    return s->next.v->event(s->next.self, event);
}

static const sjs_sink_vtable recording_sink = { record_event };

static void drop_program(sjs_site *s) {
    if (s->has_result) sjs_release(s->context, s->result);
    s->has_result = false;
    if (s->program != NULL) sjs_program_release(s->program);
    s->program = NULL;
}

SJS_SITE sjs_site *sjs_site_new(void) {
    sjs_site *s = sjs_wasm_alloc(sizeof *s, 8);
    if (s == NULL) return NULL;
    *s = (sjs_site){ 0 };
    sjs_context_config config = { sjs_wasm_allocator(), SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&s->context, &config) != SJS_OK) return NULL;
    sjs_vm_config vm = { s->context, false, false };
    if (sjs_vm_init(&s->vm, &vm) != SJS_OK) return NULL;
    return s;
}

/* Room for the next source, `length` bytes, for the page to fill. */
SJS_SITE uint8_t *sjs_site_source(sjs_site *s, uint32_t length) {
    s->source.length = 0;
    if (!reserve(&s->source, length + 1u)) return NULL;
    s->source.length = length;
    return s->source.ptr;
}

/*
 * Compile the source: flags bit 0 is a module, bit 1 compiles without
 * shapes. It is pushed to the parser in chunks of `chunk` bytes (0: whole).
 * SJS_OK, or SJS_ERR_PROGRAM with the error set.
 */
SJS_SITE int32_t sjs_site_compile(sjs_site *s, uint32_t flags, uint32_t chunk) {
    drop_program(s);
    clear_error(s);
    s->events.length = 0;
    s->numbers.length = 0;
    s->text.length = 0;
    s->chunks = 0;
    s->chunk = 0;
    s->program_bytes = 0;
    sjs_goal goal = (flags & 1u) != 0 ? SJS_GOAL_MODULE : SJS_GOAL_EXPRESSION;
    size_t before = sjs_context_live_bytes(s->context);

    sjs_compiler_config cc = { s->context, goal, NULL, 0, { NULL, NULL }, (flags & 2u) != 0 };
    sjs_result r = sjs_compiler_init(&s->compiler, &cc);
    if (r != SJS_OK) return r;
    s->next = sjs_compiler_sink(s->compiler);
    sjs_parser *parser;
    sjs_parser_config pc = { sjs_wasm_allocator(), SJS_LIMITS_DEFAULT, goal, { s, &recording_sink } };
    r = sjs_parser_init(&parser, &pc);
    if (r != SJS_OK) {
        sjs_compiler_destroy(s->compiler);
        return r;
    }
    uint32_t step = chunk == 0u || chunk > s->source.length ? s->source.length : chunk;
    for (uint32_t at = 0; r == SJS_OK && at < s->source.length; at += step) {
        uint32_t length = s->source.length - at < step ? s->source.length - at : step;
        s->chunk = s->chunks++;
        r = sjs_parser_push(parser, (sjs_bytes){ s->source.ptr + at, length });
    }
    if (r == SJS_OK) r = sjs_parser_finish(parser);
    sjs_error error;
    if (r == SJS_ERR_PROGRAM) {
        /* The compiler refused an event, or the parser refused the source. */
        if (sjs_compiler_error(s->compiler, &error) == SJS_OK) set_error(s, &error, STAGE_COMPILER);
        else if (sjs_parser_error(parser, &error) == SJS_OK) set_error(s, &error, STAGE_PARSER);
    }
    if (r == SJS_OK) {
        r = sjs_compiler_finish(s->compiler, &s->program);
        if (r == SJS_ERR_PROGRAM && sjs_compiler_error(s->compiler, &error) == SJS_OK) {
            set_error(s, &error, STAGE_COMPILER);
        }
    }
    sjs_parser_destroy(parser);
    sjs_compiler_destroy(s->compiler);
    s->compiler = NULL;
    if (r == SJS_OK) s->program_bytes = (uint32_t)(sjs_context_live_bytes(s->context) - before);
    return r;
}

SJS_SITE uint32_t sjs_site_event_count(const sjs_site *s) {
    return s->events.length / (SJS_SITE_EVENT_WORDS * 4u);
}
SJS_SITE const uint8_t *sjs_site_events(const sjs_site *s) { return s->events.ptr; }
SJS_SITE const uint8_t *sjs_site_numbers(const sjs_site *s) { return s->numbers.ptr; }
SJS_SITE const uint8_t *sjs_site_text(const sjs_site *s) { return s->text.ptr; }
SJS_SITE uint32_t sjs_site_chunks(const sjs_site *s) { return s->chunks; }
SJS_SITE uint32_t sjs_site_program_bytes(const sjs_site *s) { return s->program_bytes; }

/* The clock, in milliseconds since the epoch, and the random seed. */
SJS_SITE int32_t sjs_site_set_clock(sjs_site *s, double milliseconds) {
    int64_t ms = (int64_t)milliseconds;
    if ((double)ms > milliseconds) ms -= 1;
    int64_t seconds = ms / 1000;
    int64_t rest = ms % 1000;
    if (rest < 0) {
        seconds -= 1;
        rest += 1000;
    }
    sjs_epoch now = { seconds, (uint32_t)rest * 1000000u };
    return sjs_vm_set_clock(s->vm, &now);
}
SJS_SITE int32_t sjs_site_set_seed(sjs_site *s, double seed) { return sjs_vm_set_seed(s->vm, (int64_t)seed); }

/* Start an evaluation of the last program compiled. */
SJS_SITE int32_t sjs_site_start(sjs_site *s) {
    if (s->program == NULL) return SJS_ERR_INVALID;
    if (s->has_result) sjs_release(s->context, s->result);
    s->has_result = false;
    clear_error(s);
    sjs_run_params run = { s->program, NULL, 0, { 0 }, NULL, 0 };
    return sjs_vm_start(s->vm, &run);
}

/* Run on `fuel` more units: SJS_RUN_DONE, SJS_RUN_PAUSED or SJS_RUN_FAULT. */
SJS_SITE int32_t sjs_site_run(sjs_site *s, double fuel) {
    sjs_run status = sjs_vm_run(s->vm, (uint64_t)fuel);
    if (status == SJS_RUN_DONE) {
        s->has_result = sjs_vm_take_result(s->vm, &s->result) == SJS_OK;
    } else if (status == SJS_RUN_FAULT) {
        sjs_error error;
        if (sjs_vm_error(s->vm, &error) == SJS_OK) set_error(s, &error, STAGE_EVALUATOR);
    }
    return (int32_t)status;
}

SJS_SITE double sjs_site_fuel_used(const sjs_site *s) { return (double)sjs_vm_fuel_used(s->vm); }
SJS_SITE void sjs_site_cancel(sjs_site *s) { sjs_vm_reset(s->vm); }
SJS_SITE int32_t sjs_site_result_kind(const sjs_site *s) { return s->has_result ? s->result.kind : -1; }

/* Write the result as text through a buffer of `piece` bytes. */
SJS_SITE int32_t sjs_site_write(sjs_site *s, uint32_t piece) {
    s->output.length = 0;
    s->pieces = 0;
    if (!s->has_result) return SJS_ERR_INVALID;
    sjs_writer *writer;
    sjs_writer_config config = { s->context, s->result, SJS_STR("<function>") };
    sjs_result r = sjs_writer_init(&writer, &config);
    if (r != SJS_OK) return r;
    if (piece == 0u) piece = 4096u;
    do {
        if (!reserve(&s->output, piece)) {
            r = SJS_ERR_NOMEM;
            break;
        }
        sjs_buffer buffer = { s->output.ptr + s->output.length, piece, 0 };
        r = sjs_writer_write(writer, &buffer);
        s->output.length += buffer.length;
        s->pieces++;
    } while (r == SJS_NEED_SPACE);
    sjs_writer_destroy(writer);
    return r;
}

/*
 * The result as a host reads it through the value API, for the pipeline
 * explorer: one record of SJS_SITE_NODE_WORDS words per value, depth first.
 *   kind, depth, count (elements, properties, or entries), via, index,
 *   key offset, key length, text offset, text length
 * `via` is the call that reached the value: the result itself
 * (sjs_vm_take_result), sjs_array_at, sjs_object_at (its key is the
 * property's name), sjs_set_at, or sjs_map_at (an entry's key, then its
 * value). The text is a string's bytes (sjs_string_text), a BigInt's digits
 * (sjs_bigint_text), and for the other kinds that are not containers what
 * the writer writes for them. Offsets are into the tree's text arena.
 */
enum {
    NODE_KIND, NODE_DEPTH, NODE_COUNT, NODE_VIA, NODE_INDEX,
    NODE_KEY_OFFSET, NODE_KEY_LENGTH, NODE_TEXT_OFFSET, NODE_TEXT_LENGTH,
    SJS_SITE_NODE_WORDS
};
enum { VIA_RESULT, VIA_ARRAY_AT, VIA_OBJECT_AT, VIA_SET_AT, VIA_MAP_KEY, VIA_MAP_VALUE };
#define TREE_DEPTH 32u

typedef struct {
    uint32_t depth;
    uint32_t via;
    uint32_t index;
    sjs_str key;
} site_place;

typedef struct {
    sjs_value value;
    uint32_t next;
    uint32_t count;
} tree_frame;

static bool is_container(sjs_value v) {
    return v.kind == SJS_ARRAY || v.kind == SJS_OBJECT || v.kind == SJS_SET || v.kind == SJS_MAP;
}

/* Children a container's walk visits: a map's entries count twice. */
static uint32_t children(sjs_value v) { return v.kind == SJS_MAP ? 2u * sjs_map_size(v) : sjs_length(v); }

/* The writer's text for `value`, appended to `to`. */
static sjs_result append_written(sjs_site *s, sjs_value value, site_bytes *to) {
    sjs_writer *writer;
    sjs_writer_config config = { s->context, value, SJS_STR("<function>") };
    sjs_result r = sjs_writer_init(&writer, &config);
    if (r != SJS_OK) return r;
    do {
        if (!reserve(to, 256u)) {
            r = SJS_ERR_NOMEM;
            break;
        }
        sjs_buffer buffer = { to->ptr + to->length, 256u, 0 };
        r = sjs_writer_write(writer, &buffer);
        to->length += buffer.length;
    } while (r == SJS_NEED_SPACE);
    sjs_writer_destroy(writer);
    return r;
}

static sjs_result add_node(sjs_site *s, sjs_value value, const site_place *place) {
    uint32_t words[SJS_SITE_NODE_WORDS] = { 0 };
    words[NODE_KIND] = value.kind;
    words[NODE_DEPTH] = place->depth;
    words[NODE_COUNT] = value.kind == SJS_MAP ? sjs_map_size(value) : sjs_length(value);
    words[NODE_VIA] = place->via;
    words[NODE_INDEX] = place->index;
    words[NODE_KEY_OFFSET] = s->tree_text.length;
    words[NODE_KEY_LENGTH] = place->key.length;
    if (!append(&s->tree_text, place->key.ptr, place->key.length)) return SJS_ERR_NOMEM;
    words[NODE_TEXT_OFFSET] = s->tree_text.length;
    sjs_result r = SJS_OK;
    if (value.kind == SJS_STRING) {
        sjs_str text = sjs_string_text(value);
        if (!append(&s->tree_text, text.ptr, text.length)) return SJS_ERR_NOMEM;
    } else if (value.kind == SJS_BIGINT) {
        uint32_t bound = sjs_bigint_text_bound(value);
        if (!reserve(&s->tree_text, bound)) return SJS_ERR_NOMEM;
        sjs_buffer buffer = { s->tree_text.ptr + s->tree_text.length, bound, 0 };
        r = sjs_bigint_text(s->context, value, &buffer);
        s->tree_text.length += buffer.length;
    } else if (!is_container(value)) {
        r = append_written(s, value, &s->tree_text);
    }
    if (r != SJS_OK) return r;
    words[NODE_TEXT_LENGTH] = s->tree_text.length - words[NODE_TEXT_OFFSET];
    return append(&s->tree, words, sizeof words) ? SJS_OK : SJS_ERR_NOMEM;
}

/* Read the result into records, at most `max` of them. */
SJS_SITE int32_t sjs_site_read(sjs_site *s, uint32_t max) {
    s->tree.length = 0;
    s->tree_text.length = 0;
    s->tree_truncated = false;
    if (!s->has_result || max == 0u) return SJS_ERR_INVALID;
    sjs_result r = add_node(s, s->result, &(site_place){ 0, VIA_RESULT, 0, { NULL, 0 } });
    uint32_t nodes = 1;
    tree_frame stack[TREE_DEPTH];
    uint32_t depth = 0;
    if (is_container(s->result)) stack[depth++] = (tree_frame){ s->result, 0, children(s->result) };
    while (r == SJS_OK && depth > 0) {
        tree_frame *frame = &stack[depth - 1u];
        if (frame->next == frame->count) {
            depth--;
            continue;
        }
        if (nodes == max) {
            s->tree_truncated = true;
            break;
        }
        uint32_t i = frame->next++;
        site_place place = { depth, VIA_ARRAY_AT, i, { NULL, 0 } };
        sjs_value child;
        if (frame->value.kind == SJS_ARRAY) {
            child = sjs_array_at(frame->value, i);
        } else if (frame->value.kind == SJS_SET) {
            child = sjs_set_at(frame->value, i);
            place.via = VIA_SET_AT;
        } else if (frame->value.kind == SJS_OBJECT) {
            sjs_property property;
            r = sjs_object_at(frame->value, i, &property);
            child = property.value;
            place.via = VIA_OBJECT_AT;
            place.key = property.key;
        } else {
            sjs_map_entry entry;
            r = sjs_map_at(frame->value, i / 2u, &entry);
            child = i % 2u == 0u ? entry.key : entry.value;
            place.via = i % 2u == 0u ? VIA_MAP_KEY : VIA_MAP_VALUE;
            place.index = i / 2u;
        }
        if (r != SJS_OK) break;
        r = add_node(s, child, &place);
        nodes++;
        if (r == SJS_OK && is_container(child) && children(child) > 0u) {
            if (depth == TREE_DEPTH) s->tree_truncated = true;
            else stack[depth++] = (tree_frame){ child, 0, children(child) };
        }
    }
    return r;
}

SJS_SITE uint32_t sjs_site_node_count(const sjs_site *s) { return s->tree.length / (SJS_SITE_NODE_WORDS * 4u); }
SJS_SITE const uint8_t *sjs_site_nodes(const sjs_site *s) { return s->tree.ptr; }
SJS_SITE const uint8_t *sjs_site_node_text(const sjs_site *s) { return s->tree_text.ptr; }
SJS_SITE uint32_t sjs_site_nodes_truncated(const sjs_site *s) { return s->tree_truncated ? 1u : 0u; }

SJS_SITE const uint8_t *sjs_site_output(const sjs_site *s) { return s->output.ptr; }
SJS_SITE uint32_t sjs_site_output_length(const sjs_site *s) { return s->output.length; }
SJS_SITE uint32_t sjs_site_pieces(const sjs_site *s) { return s->pieces; }

SJS_SITE int32_t sjs_site_error_code(const sjs_site *s) { return s->error_code; }
SJS_SITE uint32_t sjs_site_error_stage(const sjs_site *s) { return s->error_stage; }
SJS_SITE uint32_t sjs_site_error_line(const sjs_site *s) { return s->error_at.line; }
SJS_SITE uint32_t sjs_site_error_column(const sjs_site *s) { return s->error_at.column; }
SJS_SITE uint32_t sjs_site_error_offset(const sjs_site *s) { return s->error_at.offset; }
SJS_SITE const uint8_t *sjs_site_error_message(const sjs_site *s) { return s->message.ptr; }
SJS_SITE uint32_t sjs_site_error_message_length(const sjs_site *s) { return s->message.length; }
SJS_SITE const uint8_t *sjs_site_error_subject(const sjs_site *s) { return s->subject.ptr; }
SJS_SITE uint32_t sjs_site_error_subject_length(const sjs_site *s) { return s->subject.length; }
SJS_SITE uint32_t sjs_site_live_bytes(const sjs_site *s) { return (uint32_t)sjs_context_live_bytes(s->context); }

/*
 * Inspection, for the pipeline explorer: a frame or a function as a record
 * of uint32_t words the page reads (the text pointers are the library's
 * own, valid while the module is loaded).
 *   frame:    kind, at offset, line, column, index, length,
 *             object ptr, object length, name ptr, name length
 *   function: at offset, line, column, params, shape ptr, shape length
 */
static uint32_t record[10];

SJS_SITE uint32_t sjs_site_frame_count(const sjs_site *s) { return sjs_vm_frame_count(s->vm); }

SJS_SITE const uint32_t *sjs_site_frame(const sjs_site *s, uint32_t index) {
    sjs_frame frame;
    if (sjs_vm_frame(s->vm, index, &frame) != SJS_OK) return NULL;
    uint32_t words[10] = {
        (uint32_t)frame.kind, frame.at.offset, frame.at.line, frame.at.column, frame.index, frame.length,
        (uint32_t)(uintptr_t)frame.object.ptr, frame.object.length, (uint32_t)(uintptr_t)frame.name.ptr, frame.name.length,
    };
    copy_bytes((uint8_t *)record, (const uint8_t *)words, sizeof words);
    return record;
}

SJS_SITE uint32_t sjs_site_function_count(const sjs_site *s) { return sjs_program_function_count(s->program); }

SJS_SITE const uint32_t *sjs_site_function(const sjs_site *s, uint32_t index) {
    sjs_function_summary fn;
    if (sjs_program_function(s->program, index, &fn) != SJS_OK) return NULL;
    uint32_t words[6] = { fn.at.offset, fn.at.line, fn.at.column, fn.params, (uint32_t)(uintptr_t)fn.shape.ptr, fn.shape.length };
    copy_bytes((uint8_t *)record, (const uint8_t *)words, sizeof words);
    return record;
}
