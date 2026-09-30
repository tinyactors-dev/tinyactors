#pragma once
/*
 * Offloaded actors (core ADR 0074, napi ADR 0020): the bytes an unloaded
 * actor was offloaded to, kept per address until the actor loads again or
 * the address is removed.
 *
 * One allocation per actor holds its bytes; a map keyed by the address
 * handle core and the directory share finds it. The directory keeps an
 * unloaded address's reference, so the key stays valid for as long as the
 * entry: removing the address removes the entry first.
 *
 * Values an actor's data model kept by handle rather than writing out (napi
 * ADR 0020 notes, the handle fallback of napi's data model) stay alive with
 * the entry: it holds one reference to each, released when the entry goes.
 *
 * This module has no Node-API in it.
 */
#include <stddef.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef struct {
    uint8_t *bytes;
    size_t length;
    /* References the entry holds to values its bytes name by handle. */
    ta_value *handles;
    uint32_t handle_count;
} tah_offload;

typedef struct {
    ta_allocator allocator;
    tah_map entries; /* (uintptr_t)ta_address * -> tah_offload * */
    size_t bytes_in_use;
} tah_offloads;

void tah_offloads_init(tah_offloads *offloads, ta_allocator allocator);
/*
 * Keeps a copy of `bytes` for `address`, taking over `handles` (their
 * references and the array, allocated with the store's allocator), in place
 * of any entry it had. TA_ERR_NOMEM keeps nothing and leaves `handles` the
 * caller's.
 */
typedef struct {
    const ta_address *address;
    ta_bytes bytes;
    ta_value *handles;
    uint32_t handle_count;
} tah_offload_put_params;
ta_result tah_offloads_put(tah_offloads *offloads, const tah_offload_put_params *params);
/* The entry for `address`, or null. */
const tah_offload *tah_offloads_find(const tah_offloads *offloads, const ta_address *address);
/* The bytes kept for `address`, borrowed until the entry is dropped or
 * replaced; false when it has none. */
bool tah_offloads_bytes(const tah_offloads *offloads, const ta_address *address, ta_bytes *out);
/* Frees the entry for `address`, releasing its handles; false when it had none. */
bool tah_offloads_drop(tah_offloads *offloads, const ta_address *address);
uint32_t tah_offloads_count(const tah_offloads *offloads);
/* Frees every entry. */
void tah_offloads_destroy(tah_offloads *offloads);
