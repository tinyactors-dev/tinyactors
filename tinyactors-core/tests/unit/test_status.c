/* The public diagnostic names must match the constants a caller writes. */
#include <string.h>
#include <tinyactors/types.h>
#include "../harness/harness.h"

static void test_names(void) {
#define NAME(status) CHECK(strcmp(ta_status_name(status), #status) == 0)
    NAME(TA_OK);
    NAME(TA_ERROR_EXECUTION);
    NAME(TA_ERROR_COMMUNICATION);
    NAME(TA_ERROR_INVALID);
    NAME(TA_ERROR_NO_MEMORY);
    NAME(TA_ERROR_STALE);
    NAME(TA_ERROR_STATE);
    NAME(TA_ERROR_UNSUPPORTED);
    NAME(TA_ERROR_FATAL);
    NAME(TA_ERROR_ABORTED);
#undef NAME
}

static void test_unknown(void) {
    CHECK(ta_status_name(1) == NULL);
    CHECK(ta_status_name(-10) == NULL);
    CHECK(ta_status_name(INT32_MIN) == NULL);
    CHECK(ta_status_name(INT32_MAX) == NULL);
}

int main(void) {
    RUN(test_names);
    RUN(test_unknown);
    TEST_MAIN_END();
}
