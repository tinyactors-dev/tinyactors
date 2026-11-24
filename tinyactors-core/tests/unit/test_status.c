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

/* The library's version is the header's it was built with. */
static void test_library_version(void) {
    ta_version version = ta_library_version();
    CHECK_EQUAL(version.major, TA_VERSION_MAJOR);
    CHECK_EQUAL(version.minor, TA_VERSION_MINOR);
    CHECK_EQUAL(version.patch, TA_VERSION_PATCH);
    CHECK_EQUAL(version.abi, TA_ABI_VERSION);
}

int main(void) {
    RUN(test_names);
    RUN(test_library_version);
    RUN(test_unknown);
    TEST_MAIN_END();
}
