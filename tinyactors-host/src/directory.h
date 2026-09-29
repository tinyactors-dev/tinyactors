#pragma once
/*
 * The in-memory actor directory (ADR 0006), installed on every system when it
 * is created. This module has no Node-API in it, so native tests drive it.
 *
 * Core interns named addresses (tinyactors-core ADR 0065): equal addresses
 * are the same ta_address pointer, so the directory is a map keyed by that
 * pointer, with no string hashing. The value is the resident actor's ID, or
 * 0 for an unloaded entry (no actor ID is 0). Each entry holds one reference
 * to its address, taken when the entry is added and given back when it goes.
 *
 * The operations are core's compare-and-swaps (ADR 0014 of core): claim
 * fails with TA_ERR_STALE when the address has an entry; replace and remove
 * fail with TA_ERR_STALE unless the current entry equals `expected` (an
 * absent entry equals kind NONE); resolve of an absent address writes kind
 * NONE and answers TA_ERR_NOT_FOUND.
 *
 * Teardown: tah_directory_clear() gives every reference back. The system
 * calls it after uninstalling the directory and before ta_system_destroy(),
 * because core frees its address records at the end of that call.
 */
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef struct {
    tah_map entries; /* (uintptr_t)ta_address * -> ta_actor_id, 0 when unloaded */
} tah_directory;

void tah_directory_init(tah_directory *directory, ta_allocator allocator);
/* The adapter core calls (ta_system_set_actor_directory). */
ta_actor_directory tah_directory_adapter(tah_directory *directory);
/* The entry for `address`: kind NONE when it has none. */
ta_actor_directory_entry tah_directory_find(const tah_directory *directory, const ta_address *address);
uint32_t tah_directory_count(const tah_directory *directory);
/* Gives back every entry's address reference and empties the map. */
void tah_directory_clear(tah_directory *directory);
void tah_directory_destroy(tah_directory *directory);
