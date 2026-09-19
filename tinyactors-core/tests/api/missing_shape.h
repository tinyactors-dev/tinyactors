// expect error: ta_bag.items: pointer needs one of TA_IN
#include <stdint.h>
typedef struct { const uint32_t *items; } ta_bag;
