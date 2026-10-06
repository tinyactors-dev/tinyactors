/* file_util.h — write test fixture files into a scratch directory. */
#ifndef TINYACTORS_TESTS_SUPPORT_FILE_UTIL_H
#define TINYACTORS_TESTS_SUPPORT_FILE_UTIL_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef TA_TEST_TEMPORARY_DIRECTORY
#define TA_TEST_TEMPORARY_DIRECTORY "ta-test-tmp"
#endif

static const char *fixture_path(const char *name) {
    static char buffer[1024];
    snprintf(buffer, sizeof buffer, "%s/%s", TA_TEST_TEMPORARY_DIRECTORY, name);
    return buffer;
}

__attribute__((unused)) static int fixture_write(const char *name, const char *text) {
#ifdef _WIN32
    mkdir(TA_TEST_TEMPORARY_DIRECTORY);
#else
    mkdir(TA_TEST_TEMPORARY_DIRECTORY, 0755);
#endif
    FILE *f = fopen(fixture_path(name), "wb");
    if (!f) return 0;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 1;
}

#endif
