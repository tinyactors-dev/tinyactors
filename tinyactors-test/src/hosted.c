#include "hosted.h"

#include <stdlib.h>

#include <tinyactors_host.h>

#include "allocator.h"
#include "rpc.h"

/*
 * The test host's platform for the kernel: libc memory, and a clock the
 * scenario owns. It starts at zero and moves only when a scenario says so,
 * so a hosted run is reproducible; the random seed is fixed for the same
 * reason.
 */
typedef struct {
    uint64_t now_ns;
} tat_hosted_clock;

typedef struct {
    tah_system *system;
    tat_hosted_clock clock;
} tat_hosted;

static uint64_t tat_hosted_monotonic(void *self) {
    return ((tat_hosted_clock *)self)->now_ns;
}

static uint64_t tat_hosted_wall(void *self) {
    return ((tat_hosted_clock *)self)->now_ns;
}

static uint64_t tat_hosted_seed(void *self) {
    (void)self;
    return 0x74696e7961637473ull;
}

static const tah_platform_vtable tat_hosted_platform_vtable = {
    .monotonic_ns = tat_hosted_monotonic,
    .wall_ns = tat_hosted_wall,
    .random_seed = tat_hosted_seed,
};

jv tat_hosted_create_method(tat_bind *bind, jv params, int *error_code) {
    jv incarnation = jv_object_get(jv_copy(params), jv_string("incarnation"));
    uint64_t chosen = 0;
    if (jv_is_valid(incarnation) && jv_get_kind(incarnation) == JV_KIND_NUMBER) {
        chosen = (uint64_t)jv_number_value(incarnation);
    }
    jv_free(incarnation);
    tat_hosted *hosted = calloc(1, sizeof *hosted);
    if (hosted == NULL) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.create: out of memory");
    }
    tah_platform platform = {
        .self = &hosted->clock, .v = &tat_hosted_platform_vtable, .allocator = tat_libc_allocator()};
    ta_result result = tah_system_create(
        &(tah_system_create_params){.platform = platform, .incarnation = chosen}, &hosted->system);
    if (result != TA_OK) {
        free(hosted);
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.create: the kernel could not create a system");
    }
    return tat_handle_register(bind, "hosted", hosted);
}

jv tat_hosted_close_method(tat_bind *bind, jv params, int *error_code) {
    jv handle = jv_object_get(jv_copy(params), jv_string("system"));
    void *resolved = NULL;
    bool ok = tat_handle_resolve(bind, handle, "hosted", &resolved);
    jv_free(handle);
    if (!ok) {
        return tat_bind_take_error(bind, error_code);
    }
    tat_hosted *hosted = resolved;
    tah_system_close(hosted->system);
    size_t leaked = tah_system_leaked_bytes(hosted->system);
    tah_system_free(hosted->system);
    tat_handle_retire(bind, hosted);
    free(hosted);
    return jv_object_set(jv_object(), jv_string("leaked"), jv_number((double)leaked));
}
