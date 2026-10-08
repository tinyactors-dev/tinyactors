/* decode.c — OTLP protobuf traces to plain views (tinyactors/otlp.h). A
 * reader of the wire format: every view points into the bytes given, and
 * unknown fields are skipped, so any producer's requests read. */
#include "otlp/otlp_internal.h"

#include "base/memory.h"

/* A piece of the bytes being read. */
typedef struct pb {
    const unsigned char *at, *end;
} pb;

static int pb_varint(pb *in, uint64_t *value) {
    uint64_t v = 0;
    for (uint32_t shift = 0; shift < 64; shift += 7) {
        if (in->at >= in->end) return 0;
        unsigned char byte = *in->at++;
        v |= (uint64_t)(byte & 0x7Fu) << shift;
        if (!(byte & 0x80u)) {
            *value = v;
            return 1;
        }
    }
    return 0;
}

static int pb_fixed(pb *in, uint32_t bytes, uint64_t *value) {
    if ((size_t)(in->end - in->at) < bytes) return 0;
    uint64_t v = 0;
    for (uint32_t i = 0; i < bytes; i++)
        v |= (uint64_t)in->at[i] << (8 * i);
    in->at += bytes;
    *value = v;
    return 1;
}

/* The next field: its number and wire type, and for a length-delimited one
 * its bytes (`*inner`), else its value. 1, 0 at the end, -1 malformed. */
static int pb_field(pb *in, uint32_t *field, uint32_t *wire, uint64_t *value, pb *inner) {
    if (in->at >= in->end) return 0;
    uint64_t tag;
    if (!pb_varint(in, &tag) || (tag >> 3) == 0 || (tag >> 3) > 0x1FFFFFFFu) return -1;
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7u);
    switch (*wire) {
    case WIRE_VARINT:
        return pb_varint(in, value) ? 1 : -1;
    case WIRE_I64:
        return pb_fixed(in, 8, value) ? 1 : -1;
    case WIRE_I32:
        return pb_fixed(in, 4, value) ? 1 : -1;
    case WIRE_LENGTH: {
        uint64_t length;
        if (!pb_varint(in, &length) || length > (uint64_t)(in->end - in->at)) return -1;
        inner->at = in->at;
        inner->end = in->at + length;
        in->at += length;
        *value = length;
        return 1;
    }
    default:
        return -1;
    }
}

static ta_string_view pb_view(pb piece) {
    ta_string_view view = {(const char *)piece.at, (uint32_t)(piece.end - piece.at)};
    return view;
}

static ta_otlp_fields pb_fields(pb message, uint32_t field) {
    ta_otlp_fields fields = {pb_view(message), field, 0};
    return fields;
}

static pb pb_of(ta_string_view view) {
    pb piece = {(const unsigned char *)view.pointer,
                (const unsigned char *)view.pointer + view.length};
    return piece;
}

static uint64_t pb_id64(const unsigned char *at, const unsigned char *end) {
    uint64_t v = 0;
    for (; at < end; at++)
        v = (v << 8) | *at;
    return v;
}

/* The next length-delimited field `want` of a message from *at: 1 with its
 * bytes, 0 at the end, -1 malformed. */
static int pb_next(pb message, uint32_t want, uint64_t *at, pb *out) {
    pb in = {message.at + *at, message.end};
    for (;;) {
        uint32_t field, wire;
        uint64_t value;
        pb inner = {(const unsigned char *)0, (const unsigned char *)0};
        int got = pb_field(&in, &field, &wire, &value, &inner);
        if (got <= 0) {
            *at = (uint64_t)(message.end - message.at);
            return got;
        }
        if (field == want && wire == WIRE_LENGTH) {
            *at = (uint64_t)(in.at - message.at);
            *out = inner;
            return 1;
        }
    }
}

static int decode_read_span(pb span, ta_otlp_span *out) {
    pb in = span;
    uint32_t field, wire;
    uint64_t value;
    pb inner;
    int got;
    out->attributes = pb_fields(span, F_SPAN_ATTRIBUTES);
    out->events = pb_fields(span, F_SPAN_EVENTS);
    out->links = pb_fields(span, F_SPAN_LINKS);
    while ((got = pb_field(&in, &field, &wire, &value, &inner)) > 0) {
        if (wire == WIRE_LENGTH) {
            switch (field) {
            case F_SPAN_TRACE_ID:
                if (inner.end - inner.at != 16) return -1;
                out->trace.high = pb_id64(inner.at, inner.at + 8);
                out->trace.low = pb_id64(inner.at + 8, inner.end);
                break;
            case F_SPAN_SPAN_ID:
                if (inner.end - inner.at != 8) return -1;
                out->span = pb_id64(inner.at, inner.end);
                break;
            case F_SPAN_PARENT_SPAN_ID:
                if (inner.end - inner.at != 8 && inner.end != inner.at) return -1;
                out->parent = pb_id64(inner.at, inner.end);
                break;
            case F_SPAN_NAME:
                out->name = pb_view(inner);
                break;
            case F_SPAN_STATUS: {
                pb status = inner;
                uint32_t status_field, status_wire;
                uint64_t status_value;
                pb status_inner;
                int status_got;
                while ((status_got = pb_field(&status, &status_field, &status_wire, &status_value,
                                              &status_inner)) > 0) {
                    if (status_field == F_STATUS_CODE && status_wire == WIRE_VARINT)
                        out->status = (uint32_t)status_value;
                    if (status_field == F_STATUS_MESSAGE && status_wire == WIRE_LENGTH)
                        out->status_message = pb_view(status_inner);
                }
                if (status_got < 0) return -1;
                break;
            }
            default:
                break;
            }
        } else if (field == F_SPAN_KIND && wire == WIRE_VARINT) {
            out->kind = (uint32_t)value;
        } else if (field == F_SPAN_START && wire == WIRE_I64) {
            out->start = value;
        } else if (field == F_SPAN_END && wire == WIRE_I64) {
            out->end = value;
        } else if (field == F_SPAN_FLAGS && wire == WIRE_I32) {
            out->flags = (uint32_t)value;
        }
    }
    return got < 0 ? -1 : 1;
}

/* The cursor: at[0] in the request (ResourceSpans), at[1] in the current
 * ResourceSpans (ScopeSpans), at[2] in the current ScopeSpans (spans);
 * at[3], at[4] where the ResourceSpans begins and ends (0, 0: none yet);
 * at[5] where the ScopeSpans begins (high word) and its length (low). */
int32_t ta_otlp_next_span(const void *bytes, size_t length, ta_otlp_cursor *cursor,
                          ta_otlp_span *out) {
    pb request = {(const unsigned char *)bytes, (const unsigned char *)bytes + length};
    for (;;) {
        if (cursor->at[3] == 0 && cursor->at[4] == 0) {
            pb resource_spans;
            int got = pb_next(request, F_REQUEST_RESOURCE, &cursor->at[0], &resource_spans);
            if (got <= 0) return got < 0 ? TA_ERROR_INVALID : 0;
            cursor->at[3] = (uint64_t)(resource_spans.at - request.at);
            cursor->at[4] = (uint64_t)(resource_spans.end - request.at);
            cursor->at[1] = 0;
            cursor->at[5] = 0;
            if (cursor->at[3] == cursor->at[4]) { /* empty: next */
                cursor->at[3] = cursor->at[4] = 0;
                continue;
            }
        }
        pb resource_spans = {request.at + cursor->at[3], request.at + cursor->at[4]};
        if (cursor->at[5] == 0) {
            pb scope_spans;
            int got = pb_next(resource_spans, F_RS_SCOPES, &cursor->at[1], &scope_spans);
            if (got < 0) return TA_ERROR_INVALID;
            if (!got) {
                cursor->at[3] = cursor->at[4] = 0;
                continue;
            }
            cursor->at[5] = (uint64_t)(scope_spans.at - request.at) << 32 |
                            (uint64_t)(scope_spans.end - scope_spans.at);
            cursor->at[2] = 0;
        }
        pb scope_spans = {request.at + (cursor->at[5] >> 32),
                          request.at + (cursor->at[5] >> 32) + (cursor->at[5] & 0xFFFFFFFFu)};
        pb span;
        int got = pb_next(scope_spans, F_SS_ITEMS, &cursor->at[2], &span);
        if (got < 0) return TA_ERROR_INVALID;
        if (!got) {
            cursor->at[5] = 0;
            continue;
        }
        ta_memset(out, 0, sizeof *out);
        uint64_t at = 0;
        pb resource = {(const unsigned char *)0, (const unsigned char *)0};
        if (pb_next(resource_spans, F_RS_RESOURCE, &at, &resource) > 0)
            out->resource = pb_fields(resource, F_RESOURCE_ATTRIBUTES);
        at = 0;
        pb scope;
        if (pb_next(scope_spans, F_SS_SCOPE, &at, &scope) > 0) {
            out->scope_attributes = pb_fields(scope, F_SCOPE_ATTRIBUTES);
            pb in = scope;
            uint32_t field, wire;
            uint64_t value;
            pb inner;
            int scope_got;
            while ((scope_got = pb_field(&in, &field, &wire, &value, &inner)) > 0) {
                if (field == F_SCOPE_NAME && wire == WIRE_LENGTH) out->scope_name = pb_view(inner);
                if (field == F_SCOPE_VERSION && wire == WIRE_LENGTH)
                    out->scope_version = pb_view(inner);
            }
            if (scope_got < 0) return TA_ERROR_INVALID;
        }
        return decode_read_span(span, out) > 0 ? 1 : TA_ERROR_INVALID;
    }
}

static int32_t decode_next_of(ta_otlp_fields fields, uint32_t *at, pb *out) {
    uint64_t offset = *at;
    int got = pb_next(pb_of(fields.message), fields.field, &offset, out);
    *at = (uint32_t)offset;
    return got < 0 ? TA_ERROR_INVALID : got;
}

static int32_t decode_read_value(pb any, ta_otlp_value *value) {
    ta_memset(value, 0, sizeof *value);
    uint32_t field, wire;
    uint64_t raw;
    pb inner;
    int got;
    while ((got = pb_field(&any, &field, &wire, &raw, &inner)) > 0) {
        if (field == F_ANY_STRING && wire == WIRE_LENGTH) {
            value->kind = TA_OTLP_VALUE_STRING;
            value->bytes = pb_view(inner);
        } else if (field == F_ANY_BOOL && wire == WIRE_VARINT) {
            value->kind = TA_OTLP_VALUE_BOOL;
            value->integer = raw != 0;
        } else if (field == F_ANY_INT && wire == WIRE_VARINT) {
            value->kind = TA_OTLP_VALUE_INT;
            value->integer = (int64_t)raw;
        } else if (field == F_ANY_DOUBLE && wire == WIRE_I64) {
            value->kind = TA_OTLP_VALUE_DOUBLE;
            ta_memcpy(&value->number, &raw, sizeof value->number);
        } else if (field == F_ANY_ARRAY && wire == WIRE_LENGTH) {
            value->kind = TA_OTLP_VALUE_ARRAY;
            value->items = pb_fields(inner, F_ARRAY_VALUES);
        } else if (field == F_ANY_LIST && wire == WIRE_LENGTH) {
            value->kind = TA_OTLP_VALUE_LIST;
            value->items = pb_fields(inner, 1); /* KeyValueList.values */
        } else if (field == F_ANY_BYTES && wire == WIRE_LENGTH) {
            value->kind = TA_OTLP_VALUE_BYTES;
            value->bytes = pb_view(inner);
        }
    }
    return got < 0 ? TA_ERROR_INVALID : 1;
}

int32_t ta_otlp_next_attribute(ta_otlp_fields attributes, uint32_t *at, ta_string_view *key,
                               ta_otlp_value *value) {
    pb pair;
    int32_t got = decode_next_of(attributes, at, &pair);
    if (got <= 0) return got;
    key->pointer = "";
    key->length = 0;
    ta_memset(value, 0, sizeof *value);
    uint32_t field, wire;
    uint64_t raw;
    pb inner;
    int field_got;
    while ((field_got = pb_field(&pair, &field, &wire, &raw, &inner)) > 0) {
        if (field == F_KEY_VALUE_KEY && wire == WIRE_LENGTH) *key = pb_view(inner);
        if (field == F_KEY_VALUE_VALUE && wire == WIRE_LENGTH &&
            decode_read_value(inner, value) < 0)
            return TA_ERROR_INVALID;
    }
    return field_got < 0 ? TA_ERROR_INVALID : 1;
}

int32_t ta_otlp_next_value(ta_otlp_fields array, uint32_t *at, ta_otlp_value *value) {
    pb any;
    int32_t got = decode_next_of(array, at, &any);
    if (got <= 0) return got;
    return decode_read_value(any, value);
}

int32_t ta_otlp_next_event(ta_otlp_fields events, uint32_t *at, ta_otlp_event *out) {
    pb event;
    int32_t got = decode_next_of(events, at, &event);
    if (got <= 0) return got;
    ta_memset(out, 0, sizeof *out);
    out->attributes = pb_fields(event, F_EVENT_ATTRIBUTES);
    uint32_t field, wire;
    uint64_t value;
    pb inner;
    int field_got;
    while ((field_got = pb_field(&event, &field, &wire, &value, &inner)) > 0) {
        if (field == F_EVENT_TIME && wire == WIRE_I64) out->time = value;
        if (field == F_EVENT_NAME && wire == WIRE_LENGTH) out->name = pb_view(inner);
    }
    return field_got < 0 ? TA_ERROR_INVALID : 1;
}

int32_t ta_otlp_next_link(ta_otlp_fields links, uint32_t *at, ta_otlp_link *out) {
    pb link;
    int32_t got = decode_next_of(links, at, &link);
    if (got <= 0) return got;
    ta_memset(out, 0, sizeof *out);
    out->attributes = pb_fields(link, F_LINK_ATTRIBUTES);
    uint32_t field, wire;
    uint64_t value;
    pb inner;
    int field_got;
    while ((field_got = pb_field(&link, &field, &wire, &value, &inner)) > 0) {
        if (field == F_LINK_TRACE_ID && wire == WIRE_LENGTH && inner.end - inner.at == 16) {
            out->trace.high = pb_id64(inner.at, inner.at + 8);
            out->trace.low = pb_id64(inner.at + 8, inner.end);
        }
        if (field == F_LINK_SPAN_ID && wire == WIRE_LENGTH && inner.end - inner.at == 8)
            out->span = pb_id64(inner.at, inner.end);
    }
    return field_got < 0 ? TA_ERROR_INVALID : 1;
}
