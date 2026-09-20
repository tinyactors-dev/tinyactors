// expect error: this output holds borrowed references; add TXML_LIFETIME(scope)
#include <stdint.h>
typedef int32_t txml_result;
enum TXML_CONSTANTS_OF(txml_result, "TXML_") { TXML_OK TXML_SUCCESS = 0 };
typedef struct { const char *name; } txml_info;
TXML_API txml_result txml_info_read(TXML_OUT TXML_VALID_ON(TXML_OK) txml_info *out);
