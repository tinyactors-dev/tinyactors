// expect error: ta_bag.count: TA_SUCCESS does not apply here
// expect error: ta_bag.size: TA_OPTIONAL applies only to pointers
// expect error: ta_bag.system: TA_IN is implied by the type; remove it
#include <stdint.h>
typedef struct ta_system ta_system TA_HANDLE;
typedef struct { TA_SUCCESS uint32_t count; TA_OPTIONAL uint32_t size; TA_IN const ta_system *system; } ta_bag;
TA_API ta_system *ta_unused(void) TA_LIFETIME(call);
TA_API void ta_system_destroy(TA_DESTROYS ta_system *system);
TA_API int ta_system_create(ta_system **out);
