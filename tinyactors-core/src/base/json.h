/* json.h — writing a JSON string, for ta_host_state_value, the one JSON core
 * still writes (ADR 0009 moves it to CBOR). Freestanding, no allocation. */
#ifndef TINYACTORS_BASE_JSON_H
#define TINYACTORS_BASE_JSON_H

#include <tinyactors/types.h>

/* `s` as a JSON string, quoted and escaped, into out (NULL: measured only);
 * returns its length. */
size_t ta_json_write_string(char *out, ta_string_view s);

#endif
