/*
 * The table of prototype names: sjs_name_id finds an entry by binary
 * search, so the entries must be sorted by bytes, each once. An entry out of
 * order would silently stop being found.
 */
#include <stdio.h>
#include <string.h>

#include "../src/internal.h"

int main(void) {
    int failures = 0;
    uint32_t count;
    const sjs_name *names = sjs_names(&count);
    for (uint32_t i = 0; i < count; i++) {
        sjs_str name = names[i].name;
        if (i > 0) {
            sjs_str before = names[i - 1u].name;
            uint32_t n = before.length < name.length ? before.length : name.length;
            int order = memcmp(before.ptr, name.ptr, n);
            if (order > 0 || (order == 0 && before.length >= name.length)) {
                printf("FAIL: \"%.*s\" is not after \"%.*s\"\n", (int)name.length, name.ptr, (int)before.length,
                       before.ptr);
                failures++;
            }
        }
        if (sjs_name_id(name) != i + 1u) {
            printf("FAIL: sjs_name_id does not find \"%.*s\"\n", (int)name.length, name.ptr);
            failures++;
        }
    }
    sjs_str missing = { "notAName", 8u };
    if (sjs_name_id(missing) != 0) {
        printf("FAIL: sjs_name_id finds \"notAName\"\n");
        failures++;
    }
    printf("names_test: %u names, %d failures\n", count, failures);
    return failures == 0 ? 0 : 1;
}
