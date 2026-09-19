// An output needs a result that says when it is valid.
// expect error: ta_count(out): the function returns no success result; mark the output TA_ALWAYS or TA_VALID_ON
// expect error: ta_hidden: public functions are declared with TA_API
#include <stdint.h>
TA_API int ta_count(TA_OUT uint32_t *out);
void ta_hidden(void);
