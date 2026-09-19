// expect error: ta_bag.items: unsupported pointer qualifier restrict
#include <stdint.h>
typedef struct { TA_IN const uint32_t *restrict items; } ta_bag;
