/* tinyactors/address.h — tinyactors addresses (docs/protocol.md, "Addresses").
 *
 *     address = "tinyactors://" system "/" local
 *     local   = "#_scxml_" <session id> | "@" <name, percent-encoded>
 *
 * The one parser every layer, binding and tool uses: the system (routing),
 * the host layer, the PostgreSQL extension and torture-bench. Freestanding
 * (ta_base): no allocation; results are views into the input.
 *
 * Session ids are unsigned 64-bit, 1 .. 2^64-1, decimal without leading
 * zeros. System ids are dot-separated labels of lowercase a-z, 0-9 and "-"
 * (1-63 characters each, not starting or ending with "-"), at most 253
 * characters. Names are 1+ characters of RFC 3986 unreserved characters or
 * percent-encodings; ta_address_name_decode yields the UTF-8 bytes.
 */
#ifndef TINYACTORS_ADDRESS_H
#define TINYACTORS_ADDRESS_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TA_ADDRESS_SESSION = 1, /* #_scxml_<id> */
    TA_ADDRESS_NAME = 2     /* @<name> */
};

typedef struct ta_address {
    uint32_t kind; /* TA_ADDRESS_SESSION / TA_ADDRESS_NAME */
    uint32_t pad;
    ta_string_view system; /* the system id */
    ta_string_view local;  /* everything after "tinyactors://<system>/" */
    ta_string_view name;   /* TA_ADDRESS_NAME: the name, still percent-encoded */
    uint64_t session;      /* TA_ADDRESS_SESSION: the session id */
} ta_address;

#define TA_ADDRESS_SCHEME     "tinyactors://"
#define TA_ADDRESS_SYSTEM_MAX 253u
/* the longest session address: scheme + system + "/#_scxml_" + 20 digits */
#define TA_ADDRESS_SESSION_MAX (13u + TA_ADDRESS_SYSTEM_MAX + 9u + 20u)

/* Parse a full address. TA_ERROR_INVALID if it is not one. */
TA_API ta_status ta_address_parse(ta_string_view text, ta_address *out);
/* Parse the local part alone ("#_scxml_<id>" or "@<name>"); out->system is empty. */
TA_API ta_status ta_address_parse_local(ta_string_view local, ta_address *out);
/* Is `system` a valid system id? */
TA_API int ta_address_system_valid(ta_string_view system);
/* Write "tinyactors://<system>/#_scxml_<session>" into `out` (no terminator;
 * ta_buffer). TA_ERROR_INVALID if the system id is invalid or session is 0;
 * TA_ERROR_NO_MEMORY when it does not fit (TA_ADDRESS_SESSION_MAX always
 * does). */
TA_API ta_status ta_address_format(ta_string_view system, uint64_t session, ta_buffer *out);
/* Write "tinyactors://<system>/@<name>", percent-encoding `name` (any bytes).
 * TA_ERROR_INVALID for an invalid system id or an empty name. */
TA_API ta_status ta_address_format_name(ta_string_view system, ta_string_view name, ta_buffer *out);
/* Decode a parsed name's percent-encodings into `out` (a decoded name is
 * never longer than its encoding). */
TA_API ta_status ta_address_name_decode(ta_string_view name, ta_buffer *out);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_ADDRESS_H */
