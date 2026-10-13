/* Allocation support only at this stage: there is no Python Host type or
 * actor loop here yet. Core's CBOR writer receives the allocator below;
 * Python objects remain owned and allocated by CPython itself. */
#include "extension.h"

/* Core allocator callback: return a raw block or NULL on refusal. Core
 * requests alignment at most 8 (allocator.h), which the raw domain meets.
 * The callbacks keep no context and may serve independent writers. Do not
 * turn this callback into a Python exception: core reports allocation
 * refusal as a status, which the conversion entry point translates. */
static void *ta_python_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align;
    return PyMem_RawMalloc(size);
}

/* Core's matching release callback. block must come from ta_python_alloc;
 * size is core's original allocation size, unused by the raw allocator.
 * This releases raw storage, not a Python object or a payload reference. */
static void ta_python_free(void *context, void *block, size_t size) {
    (void)context;
    (void)size;
    PyMem_RawFree(block);
}

/* Immutable callback table; all allocation state belongs to each writer. */
static const ta_allocator ta_python_allocator = {NULL, ta_python_alloc, ta_python_free};

/* The paired init/free contract is declared in extension.h. Starting with
 * an empty room lets core own its growable block for this whole conversion;
 * it is released before the Python encode call returns. */
void ta_python_cbor_init(ta_cbor_writer *writer) {
    ta_cbor_writer_init(writer, (ta_buffer){0}, &ta_python_allocator);
}
