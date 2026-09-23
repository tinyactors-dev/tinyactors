#pragma once
/*
 * The native system (ADR 0012, ADR 0014): one ta_system and everything the
 * addon keeps for it. This module has no Node-API in it, so native tests can
 * create, close and free systems directly; napi_system.{c,h} wraps it as the
 * JavaScript System class.
 *
 * Lifecycle:
 * - tna_system_create() allocates the record with malloc (it holds the
 *   counting allocator, so it cannot count itself), initializes the
 *   allocator and the scratch arenas, and creates the ta_system with the
 *   allocator's core lane as core's allocator.
 * - tna_system_close() is idempotent and authoritative: it destroys the
 *   ta_system and the arenas. After it, the allocator's bytes in use are
 *   zero unless something leaked, and tna_system_leaked_bytes() says how
 *   many.
 * - tna_system_free() closes the system if needed and frees the record.
 *
 * Later steps add the plumbing (queues, router, directory, pump, values);
 * each one is released by tna_system_close() in the order ADR 0012 gives.
 */
#include <stdbool.h>
#include <stddef.h>

#include "allocator.h"
#include "tinyactors.h"

typedef struct tna_system {
    tna_allocator allocator;
    tna_scratch scratch;
    ta_system *core;
    bool closed;
    /* Bytes still in use when close finished; nonzero means a leak. */
    size_t leaked_bytes;
} tna_system;

/* TA_ERR_NOMEM when the record or core cannot be allocated. */
ta_result tna_system_create(tna_system **out);
void tna_system_close(tna_system *system);
bool tna_system_closed(const tna_system *system);
size_t tna_system_leaked_bytes(const tna_system *system);
void tna_system_free(tna_system *system);
