// expect error: strings and bytes are {ptr, length}
#include <stdint.h>
typedef struct { const char *ptr; uint32_t len; } txml_text TXML_STRING;
