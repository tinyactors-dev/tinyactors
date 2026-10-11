/* tinyactors/basic_http.h — SCXML BasicHTTP Event I/O Processor (SCXML 1.0 §C.2) as
 * a transport-agnostic message codec. The host owns the sockets (or any
 * transport); this library turns a TA_EFFECT_SEND effect into an HTTP POST
 * request and an incoming HTTP request into ta_event_in fields.
 *
 * Outbound (§C.2.1): POST to the target URL with
 * application/x-www-form-urlencoded fields: `_scxmleventname` (if the send
 * has an event name) plus namelist/<param> fields; or <content> as the body.
 * Inbound (§C.2.2): event name = `_scxmleventname` field, else
 * "HTTP.<METHOD>"; remaining form fields become _event.data; the whole
 * message becomes _event.raw.
 * Freestanding: no libc, no allocation (caller buffers).
 */
#ifndef TINYACTORS_BASIC_HTTP_H
#define TINYACTORS_BASIC_HTTP_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TA_BASIC_HTTP_TYPE "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor"

typedef struct ta_http_field {
    ta_string_view name, value; /* decoded (outbound: raw values to encode) */
} ta_http_field;

/* A POST request to build. `fields` may be NULL; `body` (used when fields
 * are empty) is percent-encoded. */
typedef struct ta_http_post {
    ta_string_view url; /* http://host[:port]/path */
    ta_string_view event;
    const ta_http_field *fields;
    uint32_t field_count;
    uint32_t pad;
    ta_string_view body;
} ta_http_post;
/* Write the request into `out` (ta_buffer). TA_ERROR_INVALID if the URL is
 * not http://host[:port]/path. */
TA_API ta_status ta_http_encode_post(const ta_http_post *post, ta_buffer *out);

typedef struct ta_http_request {
    ta_string_view method, target, body;
    ta_string_view event; /* SCXML event name to deliver */
    uint32_t field_count;
    ta_http_field fields[32]; /* decoded form fields except _scxmleventname */
} ta_http_request;

/* Parse a request; decoded strings are written into `scratch` (ta_buffer;
 * it needs at most the message's length). TA_ERROR_INVALID if malformed. */
TA_API ta_status ta_http_parse_request(ta_bytes message, ta_buffer *scratch, ta_http_request *out);

#ifdef __cplusplus
}
#endif
#endif
