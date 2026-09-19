// expect error: ta_name_params.out: this output holds borrowed references; add TA_LIFETIME(scope) or TA_RELEASE(function)
#include <stdint.h>
typedef int32_t ta_result;
enum TA_CONSTANTS_OF(ta_result, "TA_") { TA_OK TA_SUCCESS = 0 };
typedef struct { const char *ptr; uint32_t len; } ta_str TA_STRING;
typedef struct { TA_OUT ta_str *out; } ta_name_params;
TA_API ta_result ta_name(TA_IN const ta_name_params *params);
