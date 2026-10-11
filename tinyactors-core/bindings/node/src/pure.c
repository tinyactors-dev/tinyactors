/* pure.c — addresses, frames and trace values: wrappers of pure core functions (binding.h). */
#include "binding.h"

/* ---- addresses (docs/protocol.md, "Addresses") ---------------------------------------- */

static napi_value decimal_value(napi_env environment, uint64_t value) {
    char text[24];
    int n = snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return str_value(environment, view_of(text, (size_t)n));
}

uint64_t decimal_of(napi_env environment, napi_value v) {
    size_t n = 0;
    napi_value s = v;
    if (type_of(environment, v) != napi_string &&
        napi_coerce_to_string(environment, v, &s) != napi_ok)
        return 0;
    char *t = get_string(environment, s, &n);
    uint64_t value = 0;
    for (size_t i = 0; t && i < n && t[i] >= '0' && t[i] <= '9'; i++)
        value = value * 10 + (uint64_t)(t[i] - '0');
    free(t);
    return value;
}

/* parseAddress(text) -> { system, local, kind: 'session' | 'name', session: decimal,
 * name (decoded), encoded } | null */
napi_value js_parse_address(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (!argc || type_of(environment, argv[0]) != napi_string) return null_value(environment);
    size_t n = 0;
    char *t = get_string(environment, argv[0], &n);
    ta_address address;
    napi_value result = null_value(environment);
    if (t && ta_address_parse(view_of(t, n), &address) == TA_OK) {
        napi_create_object(environment, &o);
        set(environment, o, "system", str_value(environment, address.system));
        set(environment, o, "local", str_value(environment, address.local));
        if (address.kind == TA_ADDRESS_SESSION) {
            set(environment, o, "kind", str_value(environment, view_of("session", 7)));
            set(environment, o, "session", decimal_value(environment, address.session));
            result = o;
        } else {
            char *decoded = (char *)malloc(address.name.length + 1);
            uint32_t length =
                decoded ? ta_address_name_decode(address.name, decoded, address.name.length + 1)
                        : 0;
            if (length) {
                set(environment, o, "kind", str_value(environment, view_of("name", 4)));
                set(environment, o, "name", str_value(environment, view_of(decoded, length)));
                set(environment, o, "encoded", str_value(environment, address.name));
                result = o;
            }
            free(decoded);
        }
    }
    free(t);
    return result;
}

/* formatAddress(system, local) -> text: local is a session id (number, bigint
 * or decimal string with "#_scxml_") or a name; throws if invalid */
napi_value js_format_address(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 2 || type_of(environment, argv[0]) != napi_string) {
        napi_throw_type_error(environment, NULL, "formatAddress(system: string, local)");
        return NULL;
    }
    size_t sl = 0, nl = 0;
    char *system = get_string(environment, argv[0], &sl), buffer[TA_ADDRESS_SESSION_MAX];
    uint32_t length = 0;
    napi_valuetype t = type_of(environment, argv[1]);
    napi_value result = NULL;
    if (t == napi_number || t == napi_bigint) {
        length = ta_address_format(view_of(system, sl), decimal_of(environment, argv[1]), buffer,
                                   sizeof buffer);
        if (length) result = str_value(environment, view_of(buffer, length));
    } else {
        char *name = get_string(environment, argv[1], &nl);
        uint32_t capacity = (uint32_t)(sl + 3 * nl + 32);
        char *out = (char *)malloc(capacity);
        length = name && nl && out
                     ? ta_address_format_name(view_of(system, sl), view_of(name, nl), out, capacity)
                     : 0;
        if (length) result = str_value(environment, view_of(out, length));
        free(out);
        free(name);
    }
    free(system);
    if (!result)
        napi_throw_type_error(environment, NULL,
                              "formatAddress: not a valid system id, session id or name");
    return result;
}

napi_value js_system_valid(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (!argc || type_of(environment, argv[0]) != napi_string) return bool_value(environment, 0);
    size_t n = 0;
    char *t = get_string(environment, argv[0], &n);
    int valid = t && ta_address_system_valid(view_of(t, n));
    free(t);
    return bool_value(environment, valid);
}

/* ---- frames (docs/protocol.md, "Framing") ----------------------------------------------- */

/* frameHeader(length) -> Uint8Array(4) */
napi_value js_frame_header(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint8_t header[TA_FRAME_HEADER];
    ta_frame_header(argc ? get_u32(environment, argv[0]) : 0, header);
    return bytes_value(environment, header, sizeof header);
}

static void frames_finalize(napi_env environment, void *data, void *hint) {
    (void)environment;
    (void)hint;
    nd_frames *frames = (nd_frames *)data;
    free(frames->buffer);
    free(frames);
}

/* frameReader(maxFrame) -> reader */
napi_value js_frame_reader(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], r;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint32_t max = argc ? get_u32(environment, argv[0]) : 0;
    nd_frames *frames = (nd_frames *)calloc(1, sizeof *frames);
    if (!max) max = TA_FRAME_MAX;
    if (!frames || !(frames->buffer = (uint8_t *)malloc(max))) {
        free(frames);
        napi_throw_error(environment, NULL, "frameReader: out of memory");
        return NULL;
    }
    frames->reader.size = sizeof frames->reader;
    ta_frame_reader_init(&frames->reader, max);
    napi_create_external(environment, frames, frames_finalize, NULL, &r);
    return r;
}

/* frameRead(reader, chunk) -> [Uint8Array (a frame) | number (a refused frame's length)] */
napi_value js_frame_read(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *p = NULL, *data = NULL;
    size_t n = 0;
    if (argc < 2 || napi_get_value_external(environment, argv[0], &p) != napi_ok || !p ||
        !bytes_of(environment, argv[1], &data, &n)) {
        napi_throw_type_error(environment, NULL, "frameRead(reader, chunk: Uint8Array)");
        return NULL;
    }
    nd_frames *frames = (nd_frames *)p;
    const uint8_t *in = (const uint8_t *)data;
    uint32_t k = 0;
    napi_create_array(environment, &out);
    while (n) {
        uint32_t event = 0,
                 used = ta_frame_read(&frames->reader, frames->buffer, in, (uint32_t)n, &event);
        in += used;
        n -= used;
        if (event == TA_FRAME_READY)
            napi_set_element(environment, out, k++,
                             bytes_value(environment, frames->buffer, frames->reader.length));
        else if (event == TA_FRAME_REFUSED)
            napi_set_element(environment, out, k++, num(environment, frames->reader.length));
    }
    return out;
}

/* ---- trace values ----------------------------------------------------------------------- */

int hex_text(const char *t, size_t length, uint64_t *high, uint64_t *low) {
    uint64_t parts[2] = {0, 0};
    if (length != 16 && length != 32) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = t[i];
        uint64_t d = c >= '0' && c <= '9'   ? (uint64_t)(c - '0')
                     : c >= 'a' && c <= 'f' ? (uint64_t)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (uint64_t)(c - 'A' + 10)
                                            : 16;
        if (d == 16) return 0;
        size_t half = length == 32 && i < 16 ? 0 : 1;
        parts[half] = parts[half] << 4 | d;
    }
    *high = parts[0];
    *low = parts[1];
    return 1;
}

/* stepSpanID(session, macrostep, microstep) -> 16 hex digits (decimal strings in) */
napi_value js_step_span_id(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 3) return NULL;
    ta_step_key key = {decimal_of(environment, argv[0]), decimal_of(environment, argv[1]),
                       get_u32(environment, argv[2]), 0};
    char t[TA_SPAN_ID_TEXT_LENGTH];
    ta_span_id_format(ta_step_key_span_id(key), t, sizeof t);
    return str_value(environment, view_of(t, sizeof t));
}

/* parseTraceparent(text) -> { traceID, parentSpanID, flags } or null */
napi_value js_parse_traceparent(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (!argc || type_of(environment, argv[0]) != napi_string) return null_value(environment);
    size_t n = 0;
    char *t = get_string(environment, argv[0], &n);
    ta_trace_context context;
    ta_status status = ta_traceparent_parse(view_of(t, n), &context);
    free(t);
    if (status != TA_OK) return null_value(environment);
    char trace[TA_TRACE_ID_TEXT_LENGTH], parent[TA_SPAN_ID_TEXT_LENGTH];
    ta_trace_id_format(context.trace, trace, sizeof trace);
    ta_span_id_format(context.parent, parent, sizeof parent);
    napi_create_object(environment, &out);
    set(environment, out, "traceID", str_value(environment, view_of(trace, sizeof trace)));
    set(environment, out, "parentSpanID", str_value(environment, view_of(parent, sizeof parent)));
    set(environment, out, "flags", num(environment, context.flags));
    return out;
}

/* formatTraceparent(traceID, spanID, flags) -> text, or null if invalid */
napi_value js_format_traceparent(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 3) return null_value(environment);
    size_t tl = 0, sl = 0;
    char *trace = get_string(environment, argv[0], &tl),
         *span = get_string(environment, argv[1], &sl);
    ta_trace_context context;
    memset(&context, 0, sizeof context);
    uint64_t ignored;
    int ok = trace && span && tl == 32 && sl == 16 &&
             hex_text(trace, tl, &context.trace.high, &context.trace.low) &&
             hex_text(span, sl, &ignored, &context.parent);
    free(trace);
    free(span);
    context.flags = get_u32(environment, argv[2]);
    char t[TA_TRACEPARENT_LENGTH];
    uint32_t n = ok ? ta_traceparent_format(&context, t, sizeof t) : 0;
    return n ? str_value(environment, view_of(t, n)) : null_value(environment);
}
