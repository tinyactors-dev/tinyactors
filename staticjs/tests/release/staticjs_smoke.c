/* The release's smoke test of libstaticjs (mise run release): built against
 * the packed static and shared library, run on this machine and, for each
 * Linux release target, in a container of that architecture. It uses the
 * public header alone: a program compiled, run on a fuel budget pushed a
 * little at a time, its result written as text and checked, and every
 * allocation given back. */
#include "staticjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUEL_STEP 16u
#define FUEL_BUDGET 100000u

static const char SOURCE[] =
    "({ total: [1, 2, 3, 4, 5].map((n) => n * n).reduce((sum, n) => sum + n, 0),"
    " name: `static${\"js\"}`, big: 2n ** 70n })";
static const char EXPECTED[] = "{\"total\":55,\"name\":\"staticjs\",\"big\":1180591620717411303424n}";

static void *smoke_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    return malloc(size);
}

static void smoke_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const sjs_allocator_vtable smoke_vtable = { smoke_alloc, smoke_free };

static int fail(const char *what) {
    fprintf(stderr, "staticjs smoke: %s\n", what);
    return 1;
}

int main(void) {
    if (sjs_version() != SJS_VERSION) return fail("the library is not the header's version");
    sjs_context *context = NULL;
    sjs_context_config config = { { NULL, &smoke_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return fail("no context");

    sjs_program *program = NULL;
    sjs_error error = { 0 };
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { SOURCE, (uint32_t)(sizeof SOURCE - 1u) }, &error };
    if (sjs_compile(&params, &program) != SJS_OK) {
        fprintf(stderr, "staticjs smoke: does not compile: %.*s\n", (int)error.message.length, error.message.ptr);
        return 1;
    }

    sjs_vm *vm = NULL;
    sjs_vm_config vm_config = { context, false, false };
    sjs_run_params run = { program, NULL, 0 };
    if (sjs_vm_init(&vm, &vm_config) != SJS_OK || sjs_vm_start(vm, &run) != SJS_OK) return fail("no evaluator");
    unsigned pauses = 0;
    sjs_run status = SJS_RUN_PAUSED;
    while (status == SJS_RUN_PAUSED && sjs_vm_fuel_used(vm) < FUEL_BUDGET) {
        status = sjs_vm_run(vm, FUEL_STEP);
        if (status == SJS_RUN_PAUSED) pauses++;
    }
    if (status != SJS_RUN_DONE) return fail("the program did not finish on its fuel budget");
    uint64_t fuel = sjs_vm_fuel_used(vm);
    if (pauses == 0) return fail("the program never paused for fuel");

    sjs_value result;
    if (sjs_vm_take_result(vm, &result) != SJS_OK) return fail("no result");
    char text[256];
    sjs_writer *writer = NULL;
    sjs_writer_config writer_config = { context, result, { NULL, 0 } };
    sjs_buffer buffer = { (uint8_t *)text, (uint32_t)sizeof text - 1u, 0 };
    if (sjs_writer_init(&writer, &writer_config) != SJS_OK || sjs_writer_write(writer, &buffer) != SJS_OK) {
        return fail("the result cannot be written");
    }
    text[buffer.length] = '\0';
    sjs_writer_destroy(writer);
    sjs_release(context, result);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    if (strcmp(text, EXPECTED) != 0) {
        fprintf(stderr, "staticjs smoke: got %s, expected %s\n", text, EXPECTED);
        return 1;
    }
    if (sjs_context_live_bytes(context) != 0) return fail("the context still holds memory");
    sjs_context_destroy(context);

    printf("staticjs %d.%d.%d: %s in %llu fuel, paused %u times\n", SJS_VERSION_MAJOR, SJS_VERSION_MINOR,
           SJS_VERSION_PATCH, text, (unsigned long long)fuel, pauses);
    return 0;
}
