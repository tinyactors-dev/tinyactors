// expect error: lifetime forever is not one of
#include <stdint.h>
typedef struct { const char *ptr; uint32_t length; } txml_str TXML_STRING;
TXML_API txml_str txml_name(void) TXML_LIFETIME(forever);
