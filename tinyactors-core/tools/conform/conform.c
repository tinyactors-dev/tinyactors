/* ta-conform: the C reference implementation of the conformance suite's
 * driver protocol (../tinyactors-test/docs/driver.md). It reads calls from
 * standard input and writes one reply for each to standard output, until
 * its input ends; the suite's runner starts it:
 *
 *     mise run //tinyactors-test:run -- build/dev/bin/ta-conform
 *
 * This file: frames, calls and their params, replies, and the method table
 * (the methods are in vectors.c, scenario.c and w3c.c). Its CBOR is core's
 * (tinyactors/cbor.h). */
#include "conform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bytes the program owns ---------------------------------------------------- */

void conform_buffer_free(ta_buffer *buffer) {
    free(buffer->pointer);
    *buffer = (ta_buffer){NULL, 0, 0};
}

int conform_buffer_reserve(ta_buffer *buffer, size_t capacity) {
    if (buffer->capacity >= capacity && buffer->pointer) return 1;
    void *grown = realloc(buffer->pointer, capacity ? capacity : 1);
    if (!grown) return 0;
    buffer->pointer = grown;
    buffer->capacity = capacity;
    return 1;
}

ta_status conform_write_growing(const conform_writing *writing, ta_buffer *out) {
    if (!conform_buffer_reserve(out, out->capacity > 256 ? out->capacity : 256)) return TA_ERROR_NO_MEMORY;
    for (int attempt = 0; attempt < 8; attempt++) {
        out->length = 0;
        ta_status status = writing->write(writing->context, out);
        if (status != TA_ERROR_NO_MEMORY || out->length <= out->capacity) return status;
        if (!conform_buffer_reserve(out, out->length)) return TA_ERROR_NO_MEMORY;
    }
    return TA_ERROR_NO_MEMORY;
}

static void *heap_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align;
    return malloc(size);
}

static void heap_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static const ta_allocator CONFORM_HEAP = {NULL, heap_alloc, heap_free};

void conform_writer_begin(ta_cbor_writer *writer, const ta_buffer *out) {
    ta_cbor_writer_init(writer, (ta_buffer){out->pointer, out->pointer ? out->capacity : 0, 0}, &CONFORM_HEAP);
}

ta_status conform_writer_end(ta_cbor_writer *writer, ta_buffer *out) {
    ta_bytes bytes;
    ta_status status = ta_cbor_writer_bytes(writer, &bytes);
    if (writer->bytes != (unsigned char *)out->pointer) { /* it grew: the block is the buffer now */
        free(out->pointer);
        out->pointer = writer->bytes;
        out->capacity = writer->capacity;
    }
    out->length = status == TA_OK ? bytes.length : 0;
    return status;
}

void conform_write_token(ta_cbor_writer *writer, const ta_cbor_document *document, uint32_t at) {
    const ta_cbor_token *token = &document->tokens[at];
    ta_string_view text = ta_cbor_string(document, at);
    double number;
    switch (token->kind) {
    case TA_CBOR_UINT: ta_cbor_write_uint(writer, token->value); break;
    case TA_CBOR_NEGINT: ta_cbor_write_negative(writer, token->value); break;
    case TA_CBOR_TEXT: ta_cbor_write_text(writer, text); break;
    case TA_CBOR_BYTES: ta_cbor_write_bytes(writer, (ta_bytes){text.pointer, text.length}); break;
    case TA_CBOR_ARRAY: ta_cbor_write_array(writer, token->count); break;
    case TA_CBOR_MAP: ta_cbor_write_map(writer, token->count); break;
    case TA_CBOR_TAG: ta_cbor_write_tag(writer, token->value); break;
    case TA_CBOR_SIMPLE: ta_cbor_write_simple(writer, (uint32_t)token->value); break;
    default: /* TA_CBOR_FLOAT */
        memcpy(&number, &token->value, sizeof number);
        ta_cbor_write_float(writer, number);
        break;
    }
}

ta_bytes conform_keep(conform *state, ta_bytes bytes) {
    uint32_t slots = (uint32_t)(sizeof state->kept / sizeof state->kept[0]);
    if (state->kept_count == slots) return (ta_bytes){NULL, 0};
    ta_buffer *kept = &state->kept[state->kept_count];
    *kept = (ta_buffer){NULL, 0, 0};
    if (!conform_buffer_reserve(kept, bytes.length)) return (ta_bytes){NULL, 0};
    if (bytes.length) memcpy(kept->pointer, bytes.pointer, bytes.length);
    kept->length = bytes.length;
    state->kept_count++;
    return (ta_bytes){kept->pointer, kept->length};
}

/* ---- params ------------------------------------------------------------------------ */

uint32_t conform_param(const conform_call *call, const char *name) {
    if (call->params == TA_NIL) return TA_NIL;
    return ta_cbor_member(&call->document, call->params, ta_string(name));
}

void conform_fail(conform_call *call, const conform_failure *failure, const char *message) {
    if (call->error.code) return; /* the first error stands */
    call->error.code = failure->code;
    call->error.reason = failure->reason;
    snprintf(call->error.path, sizeof call->error.path, "%s", failure->path);
    snprintf(call->error.message, sizeof call->error.message, "%s", message);
}

/* -32602 about params.<name>. */
static int param_fails(conform_call *call, const char *name, const char *reason) {
    char path[96], message[160];
    snprintf(path, sizeof path, "params.%s", name);
    snprintf(message, sizeof message, "%s: %s", path,
             !strcmp(reason, "missing") ? "missing" : !strcmp(reason, "type") ? "of the wrong type" : "out of range");
    conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, reason, path}, message);
    return 0;
}

int conform_token_text(const conform_call *call, uint32_t at, ta_string_view *out) {
    if (at == TA_NIL || call->document.tokens[at].kind != TA_CBOR_TEXT) return 0;
    *out = ta_cbor_string(&call->document, at);
    return 1;
}

int conform_text(conform_call *call, const char *name, ta_string_view *out) {
    uint32_t at = conform_param(call, name);
    if (at == TA_NIL || conform_token_text(call, at, out)) return 1;
    return param_fails(call, name, "type");
}

int conform_bytes(conform_call *call, const char *name, ta_bytes *out) {
    uint32_t at = conform_param(call, name);
    if (at == TA_NIL) return 1;
    if (call->document.tokens[at].kind != TA_CBOR_BYTES) return param_fails(call, name, "type");
    ta_string_view view = ta_cbor_string(&call->document, at);
    *out = (ta_bytes){view.pointer, view.length};
    return 1;
}

int conform_integer(conform_call *call, const char *name, int64_t *out) {
    uint32_t at = conform_param(call, name);
    if (at == TA_NIL) return 1;
    const ta_cbor_token *token = &call->document.tokens[at];
    if (token->kind != TA_CBOR_UINT && token->kind != TA_CBOR_NEGINT) return param_fails(call, name, "type");
    if (token->value > INT64_MAX) return param_fails(call, name, "range");
    *out = token->kind == TA_CBOR_UINT ? (int64_t)token->value : -1 - (int64_t)token->value;
    return 1;
}

int conform_boolean(conform_call *call, const char *name, int *out) {
    uint32_t at = conform_param(call, name);
    if (at == TA_NIL) return 1;
    const ta_cbor_token *token = &call->document.tokens[at];
    if (token->kind != TA_CBOR_SIMPLE || (token->value != TA_CBOR_TRUE && token->value != TA_CBOR_FALSE))
        return param_fails(call, name, "type");
    *out = token->value == TA_CBOR_TRUE;
    return 1;
}

int conform_required(conform_call *call, const char *name) {
    return conform_param(call, name) != TA_NIL || param_fails(call, name, "missing");
}

int conform_sources_read(conform_call *call, const char *name, conform_sources *out) {
    out->count = 0;
    uint32_t at = conform_param(call, name);
    if (at == TA_NIL) return 1;
    const ta_cbor_document *document = &call->document;
    uint32_t count = document->tokens[at].count;
    if (document->tokens[at].kind != TA_CBOR_ARRAY) return param_fails(call, name, "type");
    if (count > sizeof out->items / sizeof out->items[0]) return param_fails(call, name, "range");
    uint32_t item = at + 1;
    for (uint32_t i = 0; i < count; i++, item = ta_cbor_next(document, item)) {
        ta_string_view text, source;
        if (document->tokens[item].kind != TA_CBOR_MAP ||
            !conform_token_text(call, ta_cbor_member(document, item, ta_string("name")), &source) ||
            !conform_token_text(call, ta_cbor_member(document, item, ta_string("text")), &text)) {
            char path[96];
            snprintf(path, sizeof path, "params.%s[%u]", name, i);
            conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, "type", path},
                         "a document is a map of text name and text");
            return 0;
        }
        out->items[out->count++] = (ta_source){source, text};
    }
    return 1;
}

/* ---- an item as bytes ------------------------------------------------------------- */

/* The alias a text "@<alias>" names, or NULL. */
static const conform_alias *alias_of(const conform_aliases *aliases, ta_string_view text) {
    if (!aliases || text.length < 2 || text.pointer[0] != '@') return NULL;
    for (uint32_t i = 0; i < aliases->count; i++) {
        const conform_alias *alias = &aliases->pointer[i];
        if (strlen(alias->name) == text.length - 1 && !memcmp(alias->name, text.pointer + 1, text.length - 1))
            return alias;
    }
    return NULL;
}

ta_status conform_item_bytes(const conform_call *call, const conform_item *item, ta_buffer *out) {
    const ta_cbor_document *document = &call->document;
    uint32_t end = ta_cbor_next(document, item->at);
    if (end == TA_NIL) return TA_ERROR_INVALID;
    ta_cbor_writer writer;
    conform_writer_begin(&writer, out);
    for (uint32_t i = item->at; i < end; i++) {
        const conform_alias *alias =
            document->tokens[i].kind == TA_CBOR_TEXT ? alias_of(item->aliases, ta_cbor_string(document, i)) : NULL;
        if (!alias) {
            conform_write_token(&writer, document, i);
            continue;
        }
        char address[32];
        int written = snprintf(address, sizeof address, "#_scxml_%llu", (unsigned long long)alias->session);
        ta_cbor_write_text(&writer, (ta_string_view){address, (uint32_t)written});
    }
    return conform_writer_end(&writer, out);
}

/* ---- results ---------------------------------------------------------------------- */

static conform_field *field_add(conform_result *result, const char *name, uint32_t kind) {
    if (result->count == sizeof result->fields / sizeof result->fields[0]) return NULL;
    conform_field *field = &result->fields[result->count++];
    memset(field, 0, sizeof *field);
    field->name = name;
    field->kind = kind;
    return field;
}

void conform_result_integer(conform_result *result, const char *name, int64_t value) {
    conform_field *field = field_add(result, name, CONFORM_FIELD_INTEGER);
    if (field) field->integer = value;
}

void conform_result_text(conform_result *result, const char *name, ta_string_view text) {
    conform_field *field = field_add(result, name, CONFORM_FIELD_TEXT);
    if (field) field->text = text;
}

void conform_result_bytes(conform_result *result, const char *name, ta_bytes bytes) {
    conform_field *field = field_add(result, name, CONFORM_FIELD_BYTES);
    if (field) field->bytes = bytes;
}

void conform_result_list(conform_result *result, const char *name, conform_bytes_list list) {
    conform_field *field = field_add(result, name, CONFORM_FIELD_BYTES_LIST);
    if (field) field->list = list;
}

const char *conform_outcome(const ta_host_effect *effect) {
    if (effect->kind != TA_EFFECT_LOG || effect->auxiliary.length != 7 || memcmp(effect->auxiliary.pointer, "Outcome", 7))
        return NULL;
    return effect->event.length == 4 && !memcmp(effect->event.pointer, "pass", 4) ? "pass" : "fail";
}

/* ---- replies ------------------------------------------------------------------------ */

/* What a reply holds: the call's id (TA_NIL: null), and a result or an
 * error. */
typedef struct reply {
    const conform_call *call;
    uint32_t id;
    const conform_result *result;
} reply;

static void write_field(ta_cbor_writer *writer, const conform_field *field) {
    ta_cbor_write_text(writer, ta_string(field->name));
    switch (field->kind) {
    case CONFORM_FIELD_INTEGER: ta_cbor_write_int(writer, field->integer); break;
    case CONFORM_FIELD_TEXT: ta_cbor_write_text(writer, field->text); break;
    case CONFORM_FIELD_BYTES: ta_cbor_write_bytes(writer, field->bytes); break;
    default:
        ta_cbor_write_array(writer, field->list.count);
        for (uint32_t i = 0; i < field->list.count; i++)
            ta_cbor_write_bytes(writer, field->list.pointer[i]);
    }
}

static void write_reply(ta_cbor_writer *writer, const reply *answer) {
    const conform_call *call = answer->call;
    ta_cbor_write_map(writer, 2);
    ta_cbor_write_text(writer, ta_string("id"));
    const ta_cbor_token *id = answer->id == TA_NIL ? NULL : &call->document.tokens[answer->id];
    if (!id) ta_cbor_write_simple(writer, TA_CBOR_NULL);
    else if (id->kind == TA_CBOR_UINT) ta_cbor_write_uint(writer, id->value);
    else ta_cbor_write_int(writer, -1 - (int64_t)id->value);
    if (call->error.code) {
        ta_cbor_write_text(writer, ta_string("error"));
        ta_cbor_write_map(writer, 3);
        ta_cbor_write_text(writer, ta_string("code"));
        ta_cbor_write_int(writer, call->error.code);
        ta_cbor_write_text(writer, ta_string("message"));
        ta_cbor_write_text(writer, ta_string(call->error.message));
        ta_cbor_write_text(writer, ta_string("data"));
        ta_cbor_write_map(writer, 2);
        ta_cbor_write_text(writer, ta_string("reason"));
        ta_cbor_write_text(writer, ta_string(call->error.reason));
        ta_cbor_write_text(writer, ta_string("path"));
        ta_cbor_write_text(writer, ta_string(call->error.path));
        return;
    }
    ta_cbor_write_text(writer, ta_string("result"));
    ta_cbor_write_map(writer, answer->result->count);
    for (uint32_t i = 0; i < answer->result->count; i++)
        write_field(writer, &answer->result->fields[i]);
}

/* The reply's bytes into `out`, grown as they need. */
static ta_status reply_write(void *context, ta_buffer *out) {
    const reply *answer = (const reply *)context;
    ta_cbor_writer writer;
    conform_writer_begin(&writer, out);
    write_reply(&writer, answer);
    return conform_writer_end(&writer, out);
}

static void frame_write(const reply *answer) {
    static ta_buffer bytes;
    if (conform_write_growing(&(conform_writing){reply_write, (void *)answer}, &bytes) != TA_OK) {
        fprintf(stderr, "ta-conform: cannot write a reply\n");
        exit(1);
    }
    unsigned char head[4] = {(unsigned char)(bytes.length >> 24), (unsigned char)(bytes.length >> 16),
                             (unsigned char)(bytes.length >> 8), (unsigned char)bytes.length};
    if (fwrite(head, 1, 4, stdout) != 4 || fwrite(bytes.pointer, 1, bytes.length, stdout) != bytes.length ||
        fflush(stdout)) {
        fprintf(stderr, "ta-conform: cannot write to standard output\n");
        exit(1);
    }
}

/* ---- calls ------------------------------------------------------------------------------ */

static const conform_method *const method_tables[] = {conform_vector_methods, conform_scenario_methods,
                                                      conform_w3c_methods};

static const conform_method *method_named(ta_string_view name) {
    for (size_t t = 0; t < sizeof method_tables / sizeof method_tables[0]; t++)
        for (const conform_method *method = method_tables[t]; method->name; method++)
            if (strlen(method->name) == name.length && !memcmp(method->name, name.pointer, name.length))
                return method;
    return NULL;
}

/* Every param is one the method takes. */
static int params_known(conform_call *call, const conform_method *method) {
    if (call->params == TA_NIL) return 1;
    const ta_cbor_document *document = &call->document;
    uint32_t pairs = document->tokens[call->params].count, at = call->params + 1;
    for (uint32_t i = 0; i < pairs; i++) {
        ta_string_view key;
        int known = 0;
        if (conform_token_text(call, at, &key))
            for (const char *const *name = method->params; *name && !known; name++)
                known = strlen(*name) == key.length && !memcmp(*name, key.pointer, key.length);
        if (!known) {
            char path[96];
            snprintf(path, sizeof path, "params.%.*s", (int)key.length, key.pointer ? key.pointer : "");
            conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, "unknown", path}, "a param the method does not take");
            return 0;
        }
        at = ta_cbor_next(document, ta_cbor_next(document, at));
    }
    return 1;
}

/* The call's members: "method" (text), "params" (a map), "id" (an
 * integer); anything else is not a call (-32600). */
static int member_is(ta_string_view key, const char *name) {
    return key.length == strlen(name) && !memcmp(key.pointer, name, key.length);
}

static int call_read(conform_call *call, uint32_t *id) {
    const ta_cbor_document *document = &call->document;
    if (document->tokens[0].kind != TA_CBOR_MAP) {
        conform_fail(call, &(conform_failure){CONFORM_INVALID_REQUEST, "type", ""}, "a call is a map");
        return 0;
    }
    uint32_t members = document->tokens[0].count, at = 1;
    for (uint32_t i = 0; i < members; i++) {
        ta_string_view key = {"", 0};
        conform_token_text(call, at, &key);
        uint32_t value = ta_cbor_next(document, at);
        const ta_cbor_token *token = &document->tokens[value];
        const char *wrong = NULL; /* the member, when its value is not of its type */
        if (member_is(key, "id")) {
            if (token->kind == TA_CBOR_UINT || (token->kind == TA_CBOR_NEGINT && token->value <= INT64_MAX))
                *id = value;
            else wrong = "id";
        } else if (member_is(key, "params")) {
            call->params = value;
            if (token->kind != TA_CBOR_MAP) wrong = "params";
        } else if (member_is(key, "method")) {
            if (token->kind != TA_CBOR_TEXT) wrong = "method";
        } else {
            conform_fail(call, &(conform_failure){CONFORM_INVALID_REQUEST, "unknown", ""}, "not a member of a call");
        }
        if (wrong) conform_fail(call, &(conform_failure){CONFORM_INVALID_REQUEST, "type", wrong}, "a member of the wrong type");
        at = ta_cbor_next(document, value);
    }
    if (ta_cbor_member(document, 0, ta_string("method")) == TA_NIL)
        conform_fail(call, &(conform_failure){CONFORM_INVALID_REQUEST, "missing", "method"}, "a call names its method");
    if (*id == TA_NIL)
        conform_fail(call, &(conform_failure){CONFORM_INVALID_REQUEST, "missing", "id"}, "every call has an integer id");
    return call->error.code == 0;
}

/* One call: decoded, run, answered. */
static void call_answer(conform *state, ta_bytes frame) {
    static ta_buffer tokens;
    conform_call call;
    memset(&call, 0, sizeof call);
    call.params = TA_NIL;
    call.document.bytes = frame;
    ta_status status;
    do {
        call.document.tokens = (ta_cbor_token *)tokens.pointer;
        call.document.capacity = (uint32_t)(tokens.capacity / sizeof(ta_cbor_token));
        status = ta_cbor_decode(&call.document);
    } while (status == TA_ERROR_NO_MEMORY &&
             conform_buffer_reserve(&tokens, call.document.count * sizeof(ta_cbor_token)));
    conform_result result;
    memset(&result, 0, sizeof result);
    reply answer = {&call, TA_NIL, &result};
    if (status != TA_OK) {
        conform_fail(&call, &(conform_failure){CONFORM_PARSE_ERROR, "malformed", ""}, "not one CBOR item");
    } else if (call_read(&call, &answer.id)) {
        ta_string_view name = ta_cbor_string(&call.document, ta_cbor_member(&call.document, 0, ta_string("method")));
        const conform_method *method = method_named(name);
        if (!method) conform_fail(&call, &(conform_failure){CONFORM_UNKNOWN_METHOD, "unknown", "method"}, "no such method");
        else if (params_known(&call, method)) method->function(state, &call, &result);
    }
    frame_write(&answer);
    for (uint32_t i = 0; i < state->kept_count; i++)
        conform_buffer_free(&state->kept[i]);
    state->kept_count = 0;
}

/* The next frame's bytes into `frame`; 0 at the end of the input. */
static int frame_read(ta_buffer *frame) {
    unsigned char head[4];
    size_t got = fread(head, 1, 4, stdin);
    if (got == 0 && feof(stdin)) return 0;
    if (got != 4) {
        fprintf(stderr, "ta-conform: the input ended inside a frame's length\n");
        exit(1);
    }
    size_t length = (size_t)head[0] << 24 | (size_t)head[1] << 16 | (size_t)head[2] << 8 | head[3];
    if (!conform_buffer_reserve(frame, length) || fread(frame->pointer, 1, length, stdin) != length) {
        fprintf(stderr, "ta-conform: a frame of %zu bytes could not be read\n", length);
        exit(1);
    }
    frame->length = length;
    return 1;
}

int main(void) {
    static conform state;
    ta_buffer frame = {NULL, 0, 0};
    while (frame_read(&frame))
        call_answer(&state, (ta_bytes){frame.pointer, frame.length});
    conform_vectors_close(&state.vectors);
    conform_buffer_free(&frame);
    return 0;
}
