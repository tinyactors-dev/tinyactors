/* The native system: create, close and free without leaks (ADR 0012). */
#include "check.h"
#include "tinyactors_host/system.h"

static void closes_without_leaks(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    CHECK(system != NULL);
    CHECK(!tah_system_closed(system));
    /* Core allocated its system through the core lane. */
    CHECK(tah_allocator_counters(&system->allocator, TAH_MEMORY_CORE).bytes_in_use > 0);

    tah_system_close(system);
    CHECK(tah_system_closed(system));
    CHECK(system->core == NULL);
    CHECK(tah_allocator_total(&system->allocator).bytes_in_use == 0);
    CHECK(tah_system_leaked_bytes(system) == 0);

    /* Close is idempotent. */
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

static void frees_an_open_system(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    tah_system_free(system); /* closes first */
}

static void many_systems_in_any_order(void) {
    enum { COUNT = 64 };
    tah_system *systems[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &systems[index]) == TA_OK);
    }
    /* Close the odd ones, then free everything from the back. */
    for (int index = 1; index < COUNT; index += 2) {
        tah_system_close(systems[index]);
        CHECK(tah_system_leaked_bytes(systems[index]) == 0);
    }
    for (int index = COUNT - 1; index >= 0; index -= 1) {
        tah_system_close(systems[index]);
        CHECK(tah_system_leaked_bytes(systems[index]) == 0);
        tah_system_free(systems[index]);
    }
}

static void rejects_null(void) {
    CHECK(tah_system_create(0, NULL) == TA_ERR_INVALID);
    tah_system_close(NULL);
    tah_system_free(NULL);
    CHECK(tah_system_closed(NULL));
}

int main(void) {
    closes_without_leaks();
    frees_an_open_system();
    many_systems_in_any_order();
    rejects_null();
    puts("system_test passed");
    return 0;
}
