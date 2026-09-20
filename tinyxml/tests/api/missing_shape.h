// expect error: txml_scale.items: pointer needs one of
#include <stdint.h>
typedef struct { const uint32_t *items; uint32_t count; } txml_scale;
