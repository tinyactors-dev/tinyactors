// expect error: no function destroys this handle
#include <stdint.h>
typedef int32_t txml_result;
enum TXML_CONSTANTS_OF(txml_result, "TXML_") { TXML_OK TXML_SUCCESS = 0 };
typedef struct txml_leak txml_leak TXML_HANDLE;
TXML_API txml_result txml_leak_create(txml_leak **out);
