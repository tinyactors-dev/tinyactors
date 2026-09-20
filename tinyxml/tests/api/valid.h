// expect types.txml_box.kind = "handle"
// expect functions.txml_box_open.returns.success = "TXML_OK"
// expect functions.txml_box_open.params.out.shape = "handle_out"
// expect functions.txml_box_close.params.box.destroys = true
// expect functions.txml_box_label.returns.lifetime = "document"
// expect functions.txml_box_flags.returns.flags = "txml_box_flags"
// expect functions.txml_box_find.returns.success = "true"
#include <stdbool.h>
#include <stdint.h>
typedef int32_t txml_result;
enum TXML_CONSTANTS_OF(txml_result, "TXML_") { TXML_OK TXML_SUCCESS = 0, TXML_ERR = -1 };
typedef struct txml_box txml_box TXML_HANDLE;
typedef struct { const char *ptr; uint32_t length; } txml_str TXML_STRING;
enum TXML_FLAG_SET(txml_box_flags, "TXML_BOX_") { TXML_BOX_LOCKED = 1u << 0 };
TXML_API txml_result txml_box_open(txml_box **out);
TXML_API void txml_box_close(TXML_DESTROYS txml_box *box);
TXML_API txml_str txml_box_label(const txml_box *box) TXML_LIFETIME(document);
TXML_API uint32_t txml_box_flags(const txml_box *box) TXML_FLAGS(txml_box_flags);
TXML_API bool txml_box_find(const txml_box *box, TXML_OUT TXML_LIFETIME(document) txml_str *value);
