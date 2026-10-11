/* json.h — reading and writing JSON text in place, for the link protocol
 * (docs/protocol.md). Freestanding, no allocation, no recursion: values are
 * views into the caller's text, found by skipping, and decoded on demand. */
#ifndef TINYACTORS_BASE_JSON_H
#define TINYACTORS_BASE_JSON_H

#include <tinyactors/types.h>

/* The first character of the value at text[*at] (after white space, which
 * *at moves past): '{', '[', '"', 't', 'f', 'n' or '0' for a number; 0 at
 * the end. */
char ta_json_peek(ta_string_view text, uint32_t *at);
/* Move *at past the value there; 0 when it is malformed. `value` (may be
 * NULL) is its text. */
int ta_json_skip(ta_string_view text, uint32_t *at, ta_string_view *value);
/* The members of the object at *at, one per call: start with *at at the
 * object, then 1 and its key's text (escapes not decoded) and its value's
 * for each member, 0 after the last, -1 when it is malformed. `first` is 1
 * for the first call. */
int ta_json_member(ta_string_view text, uint32_t *at, int first, ta_string_view *key,
                   ta_string_view *value);
/* The elements of the array at *at, as ta_json_member. */
int ta_json_element(ta_string_view text, uint32_t *at, int first, ta_string_view *value);
/* A string value (with its quotes) decoded into out[capacity]: its UTF-8
 * length, or -1 when it is no string or does not fit. */
int32_t ta_json_string(ta_string_view value, char *out, uint32_t capacity);
/* A non-negative integer value (a fraction is dropped); 0 when it is none. */
int ta_json_integer(ta_string_view value, uint64_t *out);
/* `s` as a JSON string, quoted and escaped, into out (NULL: measured only);
 * returns its length. */
size_t ta_json_write_string(char *out, ta_string_view s);

#endif
