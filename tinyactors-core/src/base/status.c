#include <tinyactors/types.h>

/* Status names belong to core so an embedder or binding never maintains a
 * second table. String literals are immutable and need no allocator. */
const char *ta_status_name(ta_status status) {
    switch (status) {
    case TA_OK: return "TA_OK";
    case TA_ERROR_EXECUTION: return "TA_ERROR_EXECUTION";
    case TA_ERROR_COMMUNICATION: return "TA_ERROR_COMMUNICATION";
    case TA_ERROR_INVALID: return "TA_ERROR_INVALID";
    case TA_ERROR_NO_MEMORY: return "TA_ERROR_NO_MEMORY";
    case TA_ERROR_STALE: return "TA_ERROR_STALE";
    case TA_ERROR_STATE: return "TA_ERROR_STATE";
    case TA_ERROR_UNSUPPORTED: return "TA_ERROR_UNSUPPORTED";
    case TA_ERROR_FATAL: return "TA_ERROR_FATAL";
    case TA_ERROR_ABORTED: return "TA_ERROR_ABORTED";
    default: return NULL;
    }
}
