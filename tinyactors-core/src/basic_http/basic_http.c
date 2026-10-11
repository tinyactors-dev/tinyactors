/* basic_http.c — BasicHTTP message codec (see tinyactors/basic_http.h). */
#include <tinyactors/basic_http.h>

/* Writes while there is room and counts either way, so a request that does
 * not fit still learns the length it needs. */
typedef struct string_buffer {
    char *data;
    size_t length, capacity;
} string_buffer;

static void buffer_put(string_buffer *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++, b->length++)
        if (b->length < b->capacity) b->data[b->length] = s[i];
}
static void puts_(string_buffer *b, const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    buffer_put(b, s, n);
}
static void putnum(string_buffer *b, size_t v) {
    char t[24];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        buffer_put(b, &t[--n], 1);
}
static int unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}
static void put_enc(string_buffer *b, ta_string_view s) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < s.length; i++) {
        unsigned char c = (unsigned char)s.pointer[i];
        if (unreserved((char)c)) {
            buffer_put(b, (const char *)&c, 1);
        } else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            buffer_put(b, e, 3);
        }
    }
}
static size_t encoded_length(ta_string_view s) {
    size_t n = 0;
    for (uint32_t i = 0; i < s.length; i++)
        n += unreserved(s.pointer[i]) ? 1 : 3;
    return n;
}

ta_status ta_http_encode_post(const ta_http_post *post, ta_buffer *out) {
    ta_string_view url = post->url, event = post->event, body = post->body;
    const ta_http_field *fields = post->fields;
    uint32_t field_count = post->field_count;
    const char *pre = "http://";
    uint32_t i = 0;
    out->length = 0;
    while (pre[i] && i < url.length && url.pointer[i] == pre[i])
        i++;
    if (pre[i]) return TA_ERROR_INVALID;
    uint32_t hs = i;
    while (i < url.length && url.pointer[i] != '/')
        i++;
    ta_string_view host = {url.pointer + hs, i - hs};
    ta_string_view path = {i < url.length ? url.pointer + i : "/",
                           i < url.length ? url.length - i : 1};
    if (!host.length) return TA_ERROR_INVALID;
    /* body length first (Content-Length) */
    size_t blen = 0;
    int form = field_count || event.length;
    if (form) {
        ta_string_view k = {"_scxmleventname", 15};
        if (event.length) blen += encoded_length(k) + 1 + encoded_length(event);
        for (uint32_t f = 0; f < field_count; f++)
            blen += (blen ? 1 : 0) + encoded_length(fields[f].name) + 1 +
                    encoded_length(fields[f].value);
    } else {
        blen = encoded_length(body);
    }
    string_buffer b = {(char *)out->pointer, 0, out->capacity};
    puts_(&b, "POST ");
    buffer_put(&b, path.pointer, path.length);
    puts_(&b, " HTTP/1.1\r\nHost: ");
    buffer_put(&b, host.pointer, host.length);
    puts_(&b, "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ");
    putnum(&b, blen);
    puts_(&b, "\r\n\r\n");
    if (form) {
        int first = 1;
        if (event.length) {
            puts_(&b, "_scxmleventname=");
            put_enc(&b, event);
            first = 0;
        }
        for (uint32_t f = 0; f < field_count; f++) {
            if (!first) buffer_put(&b, "&", 1);
            put_enc(&b, fields[f].name);
            buffer_put(&b, "=", 1);
            put_enc(&b, fields[f].value);
            first = 0;
        }
    } else {
        put_enc(&b, body);
    }
    out->length = b.length;
    return b.length <= out->capacity ? TA_OK : TA_ERROR_NO_MEMORY;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode a urlencoded component into scratch; returns the view. */
static ta_string_view decode(const char *s, size_t n, char *scratch, size_t *used,
                             size_t capacity) {
    ta_string_view r = {scratch + *used, 0};
    for (size_t i = 0; i < n && *used < capacity; i++) {
        char c = s[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            c = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        }
        scratch[(*used)++] = c;
        r.length++;
    }
    return r;
}

static int equals(ta_string_view a, const char *b) {
    uint32_t n = 0;
    while (b[n])
        n++;
    if (a.length != n) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (a.pointer[i] != b[i]) return 0;
    return 1;
}

ta_status ta_http_parse_request(ta_bytes bytes, ta_buffer *work, ta_http_request *out) {
    const char *message = (const char *)bytes.pointer;
    size_t length = bytes.length;
    char *scratch = (char *)work->pointer;
    size_t capacity = work->capacity;
    size_t i = 0, used = 0;
    out->field_count = 0;
    out->event.pointer = 0;
    out->event.length = 0;
    while (i < length && message[i] != ' ')
        i++;
    if (i == 0 || i >= length) return TA_ERROR_INVALID;
    out->method.pointer = message;
    out->method.length = (uint32_t)i;
    size_t ts = ++i;
    while (i < length && message[i] != ' ')
        i++;
    if (i >= length) return TA_ERROR_INVALID;
    out->target.pointer = message + ts;
    out->target.length = (uint32_t)(i - ts);
    /* headers end at CRLFCRLF */
    size_t body = 0;
    for (size_t k = i; k + 3 < length; k++)
        if (message[k] == '\r' && message[k + 1] == '\n' && message[k + 2] == '\r' &&
            message[k + 3] == '\n') {
            body = k + 4;
            break;
        }
    if (!body) return TA_ERROR_INVALID;
    out->body.pointer = message + body;
    out->body.length = (uint32_t)(length - body);
    /* form fields (a body without '=' is opaque content) */
    int is_form = 0;
    for (size_t k = body; k < length; k++)
        if (message[k] == '=') is_form = 1;
    if (is_form) {
        size_t p = body;
        while (p < length) {
            size_t e = p;
            while (e < length && message[e] != '&')
                e++;
            size_t eqp = p;
            while (eqp < e && message[eqp] != '=')
                eqp++;
            ta_string_view name = decode(message + p, eqp - p, scratch, &used, capacity);
            ta_string_view value = decode(message + (eqp < e ? eqp + 1 : e),
                                          eqp < e ? e - eqp - 1 : 0, scratch, &used, capacity);
            if (equals(name, "_scxmleventname"))
                out->event = value;
            else if (out->field_count < 32 && name.length) {
                out->fields[out->field_count].name = name;
                out->fields[out->field_count].value = value;
                out->field_count++;
            }
            p = e + 1;
        }
    }
    if (!out->event.length) { /* "HTTP.<METHOD>" */
        ta_string_view event = {scratch + used, 0};
        const char *pre = "HTTP.";
        for (int k = 0; pre[k] && used < capacity; k++, event.length++)
            scratch[used++] = pre[k];
        for (uint32_t k = 0; k < out->method.length && used < capacity; k++, event.length++)
            scratch[used++] = out->method.pointer[k];
        out->event = event;
    }
    work->length = used;
    return used <= capacity ? TA_OK : TA_ERROR_NO_MEMORY;
}
