/* cli/rpc.c — JSON-RPC 2.0 lines and messages (cli/rpc.h). */
#include "cli/rpc.h"

#include "base/memory.h"
#include "cli/json.h"

typedef enum rpc_constants {
    RPC_TOKEN_ALIGN = 8,
    RPC_WRITER_ROOM = 0 /* writers start empty and grow in the arena */
} rpc_constants;

static const ta_string_view rpc_version = {"2.0", 3};

/* ---- lines ------------------------------------------------------------------- */

void ta_rpc_lines_init(ta_rpc_lines *lines, const ta_rpc_lines_config *config) {
    ta_memset(lines, 0, sizeof *lines);
    lines->pending = ta_output_over((ta_buffer){0, 0, 0}, config->allocator);
    lines->limit = config->limit;
}

/* Drop the bytes handed out; after a large line, move what is left into a
 * small block and give the large one back (when no block can be had, the
 * large one stays: nothing is lost). */
static void rpc_lines_compact(ta_rpc_lines *lines) {
    ta_output *pending = &lines->pending;
    size_t left = pending->length - lines->consumed;
    if (lines->shrink && left <= TA_RPC_LINES_KEEP && pending->capacity > TA_RPC_LINES_KEEP) {
        ta_output smaller = ta_output_over((ta_buffer){0, 0, 0}, pending->allocator);
        if (!left || ta_output_reserve(&smaller, left)) {
            ta_output_write(&smaller, pending->bytes + lines->consumed, left);
            ta_output_free(pending);
            *pending = smaller;
            lines->consumed = 0;
            lines->scanned = 0;
            lines->shrink = 0;
            return;
        }
    }
    if (lines->consumed) {
        if (left) ta_memmove(pending->bytes, pending->bytes + lines->consumed, left);
        pending->length = left;
        lines->scanned -= lines->consumed;
        lines->consumed = 0;
    }
}

ta_status ta_rpc_lines_feed(ta_rpc_lines *lines, ta_bytes chunk) {
    rpc_lines_compact(lines);
    if (!chunk.length) {
        lines->ended = 1;
        return TA_OK;
    }
    const unsigned char *bytes = (const unsigned char *)chunk.pointer;
    size_t start = 0;
    if (lines->skipping) {
        while (start < chunk.length && bytes[start] != '\n')
            start++;
        if (start == chunk.length) return TA_OK; /* all of it dropped */
        start++;
        lines->skipping = 0;
    }
    if (!ta_output_reserve(&lines->pending, chunk.length - start)) return TA_ERROR_NO_MEMORY;
    ta_output_write(&lines->pending, bytes + start, chunk.length - start);
    return TA_OK;
}

static int rpc_blank(ta_string_view text) {
    for (uint32_t i = 0; i < text.length; i++)
        if (text.pointer[i] != ' ' && text.pointer[i] != '\t') return 0;
    return 1;
}

int ta_rpc_lines_next(ta_rpc_lines *lines, ta_rpc_line *out) {
    ta_output *pending = &lines->pending;
    out->text = (ta_string_view){(const char *)0, 0};
    out->too_long = 0;
    for (;;) {
        size_t start = lines->consumed, end = lines->scanned > start ? lines->scanned : start;
        while (end < pending->length && pending->bytes[end] != '\n')
            end++;
        int whole = end < pending->length || (lines->ended && end > start);
        if (!whole) {
            lines->scanned = end;
            if (end - start <= lines->limit) return 0;
            /* past the limit with no newline yet: report it, drop the rest as it comes */
            lines->consumed = pending->length;
            lines->scanned = pending->length;
            lines->skipping = !lines->ended;
            lines->shrink = 1;
            out->too_long = 1;
            return 1;
        }
        lines->consumed = end < pending->length ? end + 1 : end;
        lines->scanned = lines->consumed;
        size_t length = end - start;
        if (length && pending->bytes[end - 1] == '\r') length--;
        if (length > TA_RPC_LINES_KEEP) lines->shrink = 1;
        if (length > lines->limit) {
            out->too_long = 1;
            return 1;
        }
        ta_string_view text = {(const char *)pending->bytes + start, (uint32_t)length};
        if (rpc_blank(text)) continue;
        out->text = text;
        return 1;
    }
}

void ta_rpc_lines_free(ta_rpc_lines *lines) {
    ta_output_free(&lines->pending);
    lines->consumed = 0;
    lines->scanned = 0;
}

/* ---- messages ---------------------------------------------------------------------- */

ta_status ta_rpc_decode(const ta_rpc_decoding *decoding, ta_cbor_document *out) {
    out->bytes = decoding->cbor;
    out->tokens = (ta_cbor_token *)0;
    out->capacity = 0;
    out->count = 0;
    ta_status status = ta_cbor_decode(out);
    if (status != TA_ERROR_NO_MEMORY) return status;
    uint32_t needed = out->count;
    out->tokens = (ta_cbor_token *)ta_growable_arena_alloc(decoding->arena, sizeof *out->tokens * needed,
                                                           RPC_TOKEN_ALIGN);
    if (!out->tokens) return TA_ERROR_NO_MEMORY;
    out->capacity = needed;
    out->count = 0;
    return ta_cbor_decode(out);
}

static void rpc_refuse(ta_rpc_message *message, int32_t code, const char *reason) {
    message->error.code = code;
    message->error.reason = ta_string(reason);
}

static int rpc_equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (!a.length || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

/* The envelope of the message at out->at, as core's posted messages have it
 * (docs/requests.md, "The envelope") with JSON-RPC's "jsonrpc": "2.0". */
static void rpc_envelope(ta_rpc_message *out) {
    const ta_cbor_document *document = &out->document;
    out->method = out->params = out->id = TA_NIL;
    out->kind = TA_RPC_NOTIFICATION;
    out->error.path = ta_string("");
    const ta_cbor_token *message = &document->tokens[out->at];
    if (message->kind != TA_CBOR_MAP) {
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, "type");
        out->error.message = ta_string("a message is a JSON-RPC 2.0 request: an object");
        return;
    }
    uint32_t version = TA_NIL;
    uint32_t at = out->at + 1;
    for (uint32_t pair = 0; pair < message->count; pair++) {
        ta_string_view key = ta_cbor_string(document, at);
        uint32_t value = at + 1;
        if (rpc_equal(key, ta_string("jsonrpc"))) version = value;
        else if (rpc_equal(key, ta_string("method"))) out->method = value;
        else if (rpc_equal(key, ta_string("params"))) out->params = value;
        else if (rpc_equal(key, ta_string("id"))) out->id = value;
        else {
            rpc_refuse(out, TA_RPC_INVALID_REQUEST, "unknown");
            out->error.path = key;
            out->error.message = ta_string("a request has the members jsonrpc, method, params and id only");
            return;
        }
        at = ta_cbor_next(document, value);
    }
    const ta_cbor_token *tokens = document->tokens;
    if (version == TA_NIL || tokens[version].kind != TA_CBOR_TEXT ||
        !rpc_equal(ta_cbor_string(document, version), rpc_version)) {
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, version == TA_NIL ? "missing" : "type");
        out->error.path = ta_string("jsonrpc");
        out->error.message = ta_string("a request has \"jsonrpc\": \"2.0\"");
    } else if (out->method == TA_NIL || tokens[out->method].kind != TA_CBOR_TEXT) {
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, out->method == TA_NIL ? "missing" : "type");
        out->error.path = ta_string("method");
        out->error.message = ta_string("a request has a method, a string");
    } else if (out->params != TA_NIL && tokens[out->params].kind != TA_CBOR_MAP) {
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, "type");
        out->error.path = ta_string("params");
        out->error.message = ta_string("params, when given, are an object");
    } else if (out->id != TA_NIL && tokens[out->id].kind != TA_CBOR_UINT && tokens[out->id].kind != TA_CBOR_NEGINT &&
               tokens[out->id].kind != TA_CBOR_TEXT &&
               !(tokens[out->id].kind == TA_CBOR_SIMPLE && tokens[out->id].value == TA_CBOR_NULL)) {
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, "type");
        out->error.path = ta_string("id");
        out->error.message = ta_string("an id is an integer, a string or null");
    } else if (out->id != TA_NIL) {
        out->kind = TA_RPC_REQUEST;
    }
}

ta_status ta_rpc_parse(const ta_rpc_parsing *parsing, ta_rpc_message *out) {
    ta_memset(out, 0, sizeof *out);
    out->method = out->params = out->id = TA_NIL;
    out->error.path = ta_string("");
    ta_allocator allocator = ta_growable_arena_allocator(parsing->arena);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, RPC_WRITER_ROOM, 0}, &allocator);
    ta_json_reading reading = {parsing->line, &writer, parsing->arena, {0, {"", 0}}};
    ta_status status = ta_json_read(&reading);
    if (status == TA_ERROR_NO_MEMORY) return status;
    if (status == TA_ERROR_LIMIT) {
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, "limit");
        out->error.message = ta_string("nested deeper than a message may be");
        return TA_OK;
    }
    if (status != TA_OK) {
        rpc_refuse(out, TA_RPC_PARSE_ERROR, "malformed");
        out->error.message = reading.failure.reason;
        return TA_OK;
    }
    ta_bytes cbor;
    status = ta_cbor_writer_bytes(&writer, &cbor);
    if (status == TA_OK) status = ta_rpc_decode(&(ta_rpc_decoding){cbor, parsing->arena}, &out->document);
    if (status == TA_ERROR_NO_MEMORY) return status;
    if (status != TA_OK) { /* the reader made CBOR the decoder refuses: a string past 2^32-1 */
        rpc_refuse(out, TA_RPC_INVALID_REQUEST, "limit");
        out->error.message = ta_string("a string longer than a message may hold");
        return TA_OK;
    }
    const ta_cbor_token *top = &out->document.tokens[0];
    if (top->kind == TA_CBOR_ARRAY) {
        out->kind = TA_RPC_BATCH;
        out->count = top->count;
        if (!top->count) {
            rpc_refuse(out, TA_RPC_INVALID_REQUEST, "missing");
            out->error.message = ta_string("an empty batch");
        }
        return TA_OK;
    }
    rpc_envelope(out);
    return TA_OK;
}

void ta_rpc_element_read(const ta_rpc_element *element, ta_rpc_message *out) {
    ta_memset(out, 0, sizeof *out);
    out->document = element->batch->document;
    out->at = element->at;
    rpc_envelope(out);
}

/* ---- writing ------------------------------------------------------------------------- */

static double rpc_double(uint64_t bits) {
    double value;
    ta_memcpy(&value, &bits, sizeof value);
    return value;
}

void ta_rpc_write_item(ta_cbor_writer *writer, ta_rpc_item item) {
    const ta_cbor_document *document = item.document;
    uint32_t end = ta_cbor_next(document, item.at);
    if (end == TA_NIL) return;
    /* pre-order with every container's count known: written in order, the
     * heads and their items make the same item again */
    for (uint32_t at = item.at; at < end; at++) {
        const ta_cbor_token *token = &document->tokens[at];
        ta_string_view text = ta_cbor_string(document, at);
        switch (token->kind) {
        case TA_CBOR_UINT: ta_cbor_write_uint(writer, token->value); break;
        case TA_CBOR_NEGINT: ta_cbor_write_negative(writer, token->value); break;
        case TA_CBOR_BYTES: ta_cbor_write_bytes(writer, (ta_bytes){text.pointer, text.length}); break;
        case TA_CBOR_TEXT: ta_cbor_write_text(writer, text); break;
        case TA_CBOR_ARRAY: ta_cbor_write_array(writer, token->count); break;
        case TA_CBOR_MAP: ta_cbor_write_map(writer, token->count); break;
        case TA_CBOR_TAG: ta_cbor_write_tag(writer, token->value); break;
        case TA_CBOR_SIMPLE: ta_cbor_write_simple(writer, (uint32_t)token->value); break;
        case TA_CBOR_FLOAT: ta_cbor_write_float(writer, rpc_double(token->value)); break;
        default: break;
        }
    }
}

void ta_rpc_forward_write(const ta_rpc_forward *forward, ta_cbor_writer *writer) {
    const ta_rpc_message *message = forward->message;
    const ta_cbor_document *document = &message->document;
    ta_cbor_write_map(writer, TA_CBOR_COUNT_LATER);
    ta_cbor_write_text(writer, ta_string("method"));
    ta_rpc_write_item(writer, (ta_rpc_item){document, message->method, 0});
    if (message->params != TA_NIL) {
        ta_cbor_write_text(writer, ta_string("params"));
        ta_rpc_write_item(writer, (ta_rpc_item){document, message->params, 0});
    }
    if (message->kind == TA_RPC_REQUEST) {
        ta_cbor_write_text(writer, ta_string("id"));
        ta_cbor_write_uint(writer, forward->id);
    }
    ta_cbor_write_end(writer);
}

static void rpc_write_version(ta_cbor_writer *writer) {
    ta_cbor_write_text(writer, ta_string("jsonrpc"));
    ta_cbor_write_text(writer, rpc_version);
}

void ta_rpc_response_write(const ta_rpc_response *response, ta_cbor_writer *writer) {
    ta_cbor_write_map(writer, 3);
    rpc_write_version(writer);
    ta_cbor_write_text(writer, ta_string("id"));
    if (response->id.document && response->id.at != TA_NIL) ta_rpc_write_item(writer, response->id);
    else ta_cbor_write_simple(writer, TA_CBOR_NULL);
    const ta_rpc_error *error = response->error;
    if (!error || !error->code) {
        ta_cbor_write_text(writer, ta_string("result"));
        if (response->result.document) ta_rpc_write_item(writer, response->result);
        else ta_cbor_write_map(writer, 0);
        return;
    }
    ta_cbor_write_text(writer, ta_string("error"));
    ta_cbor_write_map(writer, 3);
    ta_cbor_write_text(writer, ta_string("code"));
    ta_cbor_write_int(writer, error->code);
    ta_cbor_write_text(writer, ta_string("message"));
    ta_cbor_write_text(writer, error->message);
    ta_cbor_write_text(writer, ta_string("data"));
    ta_cbor_write_map(writer, error->status.length ? 3u : 2u);
    ta_cbor_write_text(writer, ta_string("reason"));
    ta_cbor_write_text(writer, error->reason);
    ta_cbor_write_text(writer, ta_string("path"));
    ta_cbor_write_text(writer, error->path);
    if (error->status.length) {
        ta_cbor_write_text(writer, ta_string("status"));
        ta_cbor_write_text(writer, error->status);
    }
}

void ta_rpc_notification_begin(ta_cbor_writer *writer, ta_string_view method) {
    ta_cbor_write_map(writer, 3);
    rpc_write_version(writer);
    ta_cbor_write_text(writer, ta_string("method"));
    ta_cbor_write_text(writer, method);
    ta_cbor_write_text(writer, ta_string("params"));
}

ta_status ta_rpc_line_write(ta_bytes cbor, ta_output *out) {
    ta_json_writing writing = {cbor, out, 0};
    ta_status status = ta_json_write(&writing);
    if (status != TA_OK) return status;
    ta_output_write(out, "\n", 1);
    return ta_output_fits(out) ? TA_OK : TA_ERROR_NO_MEMORY;
}
