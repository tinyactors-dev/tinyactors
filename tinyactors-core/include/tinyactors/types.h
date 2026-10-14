/* tinyactors/types.h — basic ABI types shared by every public header.
 *
 * ABI rules (see docs/architecture.md §11):
 *   - only fixed-width integers, pointers and {pointer, length} views cross the ABI;
 *   - enums are passed as uint32_t, never as C enum types inside structs;
 *   - every extensible struct starts with `uint32_t size` (= sizeof by caller);
 *   - ids come in three kinds:
 *       indices into what a host holds (a segment, a chart in its image,
 *       an I/O processor, a datamodel binding) are uint32_t from 0, and
 *       TA_NIL means none;
 *       handles (an actor in its system, an invoke or request token, a
 *       peer, a migration job, a trace watch) are uint32_t and never 0,
 *       and 0 means none; an actor's and a token's carry a generation, so
 *       a stale one is refused;
 *       ids that outlive their segment or name values (a session, a
 *       payload, a ta_table handle) are uint64_t, and 0 means none;
 *   - a function takes at most three parameters: what travels together is
 *     grouped in a struct (ta_bytes, ta_buffer, ta_spawn, ...).
 */
#ifndef TINYACTORS_TYPES_H
#define TINYACTORS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A build may define TA_API itself: a release compiles the library with
 * TA_API at default visibility under -fvisibility=hidden, so only the API
 * is exported. */
#if defined(TA_API)
#elif defined(TA_SHARED) && defined(_WIN32)
#if defined(TA_BUILDING)
#define TA_API __declspec(dllexport)
#else
#define TA_API __declspec(dllimport)
#endif
#elif defined(TA_SHARED) && defined(__GNUC__)
#define TA_API __attribute__((visibility("default")))
#else
#define TA_API
#endif

/* The release, MAJOR.MINOR.PATCH (of libtinyactors and libtinyactors_runtime,
 * released together); a release sets them. */
#define TA_VERSION_MAJOR 0
#define TA_VERSION_MINOR 2
#define TA_VERSION_PATCH 0

#define TA_ABI_VERSION 1u

/* "no index" sentinel for u32 indices/offsets (not for handles, which use 0). */
#define TA_NIL UINT32_C(0xFFFFFFFF)

/* Borrowed byte-string view. Not NUL-terminated unless documented. */
typedef struct ta_string_view {
    const char *pointer;
    uint32_t length;
} ta_string_view;

/* A view of a NUL-terminated string (freestanding: no strlen). */
static inline ta_string_view ta_string(const char *text) {
    uint32_t length = 0;
    while (text[length])
        length++;
    ta_string_view view;
    view.pointer = text;
    view.length = length;
    return view;
}

/* Borrowed bytes. */
typedef struct ta_bytes {
    const void *pointer;
    size_t length;
} ta_bytes;

/* A buffer the caller lends a call that writes for it. The call writes from
 * the start and sets `length` to what it wrote; on TA_ERROR_NO_MEMORY,
 * `length` is the capacity it needs and the contents are not usable. */
typedef struct ta_buffer {
    void *pointer;
    size_t capacity, length;
} ta_buffer;

/* A block of memory the caller owns and lends: a system's or an encoder's
 * region, or bytes a call changes in place. */
typedef struct ta_memory {
    void *pointer;
    size_t length;
} ta_memory;
/* Room for up to `capacity` items a call writes for its caller; the call
 * returns how many it wrote (or how many there are, as it documents). */
typedef struct ta_uint32_array {
    uint32_t *pointer;
    uint32_t capacity;
} ta_uint32_array;
typedef struct ta_uint64_array {
    uint64_t *pointer;
    uint32_t capacity;
} ta_uint64_array;
/* `count` strings a call reads (borrowed for the call). */
typedef struct ta_string_list {
    const ta_string_view *pointer;
    uint32_t count;
} ta_string_list;
/* Status codes. Negative = error. */
typedef int32_t ta_status;
enum {
    TA_OK = 0,
    TA_ERROR_EXECUTION = -1, /* datamodel: raise error.execution, skip rest of block */
    TA_ERROR_COMMUNICATION = -2,
    TA_ERROR_INVALID = -3,   /* bad argument / malformed image */
    TA_ERROR_NO_MEMORY = -4, /* caller-supplied region too small / pool exhausted */
    TA_ERROR_STALE = -5,     /* stale handle */
    TA_ERROR_STATE = -6,     /* call not legal in current state (e.g. re-entrancy) */
    TA_ERROR_UNSUPPORTED = -7,
    TA_ERROR_FATAL = -8,
    TA_ERROR_ABORTED = -9 /* the host's abort flag stopped the call (e.g. a compile) */
};

/* Use in diagnostics when reporting a ta_status: the exact constant name
 * above, or NULL for an unknown value. The library owns the immutable,
 * NUL-terminated text for its lifetime; no allocation or formatting occurs.
 * This names statuses, not run statuses or application-defined errors. */
TA_API const char *ta_status_name(ta_status status);

/* Host-defined monotonic time unit (config.ticks_per_second). */
typedef uint64_t ta_time;

/* Expression id: index into a chart's expression table; meaning is decided by
 * the datamodel. */
typedef uint32_t ta_expression;

/* Opaque datamodel-owned payload handle (event data, donedata, invoke params).
 * 0 = no payload. */
typedef uint64_t ta_payload;

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_TYPES_H */
