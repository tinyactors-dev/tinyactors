// expect error: ta_bag.items: no member named total
// expect error: ta_list.items: list element uint32_t has no member kind
#include <stdint.h>
typedef struct { TA_COUNTED_BY(total) const uint32_t *items; uint32_t count; } ta_bag;
typedef struct { TA_TERMINATED_BY(kind) const uint32_t *items; } ta_list;
