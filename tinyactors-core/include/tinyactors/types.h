/* tinyactors/types.h — basic ABI types shared by every public header.
 *
 * ABI rules (see docs/architecture.md §11):
 *   - only fixed-width integers, pointers and {pointer, length} views cross the ABI;
 *   - enums are passed as uint32_t, never as C enum types inside structs;
 *   - every extensible struct starts with `uint32_t size` (= sizeof by caller);
 *   - ids/handles are uint32_t; 0 is never a valid handle.
 */
#ifndef TINYACTORS_TYPES_H
#define TINYACTORS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TA_SHARED) && defined(_WIN32)
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

#define TA_ABI_VERSION 1u

/* "no index" sentinel for u32 indices/offsets (not for handles, which use 0). */
#define TA_NIL UINT32_C(0xFFFFFFFF)

/* Borrowed byte-string view. Not NUL-terminated unless documented. */
typedef struct ta_string_view {
    const char *pointer;
    uint32_t length;
} ta_string_view;

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
