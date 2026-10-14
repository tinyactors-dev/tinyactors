/* host/session_map.h — sessions to a u32 value: an open-addressing table
 * (linear probing) the host keeps in blocks of its allocator.
 *
 * It answers "where does this session live" (the moved sessions) and "is
 * this job waiting for it" (a migration job's members). Adding needs
 * session_map_reserve first, which is the only call that allocates: it
 * refuses (0) leaving the map as it was, so a caller reserves before it
 * changes anything else. A removed entry keeps its key, value TA_NIL, until
 * a reserve rebuilds the table; lookups probe past it. */
#ifndef TINYACTORS_HOST_SESSION_MAP_H
#define TINYACTORS_HOST_SESSION_MAP_H

#include <tinyactors/allocator.h>
#include <tinyactors/types.h>

typedef struct session_map {
    uint64_t *keys;   /* 0: an empty slot (sessions are never 0) */
    uint32_t *values; /* TA_NIL: removed */
    uint32_t capacity; /* slots: 0 or a power of two */
    uint32_t count;    /* live entries */
    uint32_t used;     /* live and removed entries */
} session_map;

/* Room for `extra` more sessions without growing; rebuilt (dropping removed
 * entries, at most a quarter full) when it would be more than half used.
 * 1, or 0 if the allocator refused (the map unchanged). */
int session_map_reserve(session_map *map, uint32_t extra, const ta_allocator *allocator);
/* Set `key` to `value`; TA_NIL removes it (never allocates). A new key needs
 * room reserved first. */
void session_map_put(session_map *map, uint64_t key, uint32_t value);
/* The value of `key`, TA_NIL if it has none. */
uint32_t session_map_get(const session_map *map, uint64_t key);
/* The live entries in slot order: from *cursor (start at 0), the next key
 * into *key and its value; TA_NIL when there are no more. */
uint32_t session_map_next(const session_map *map, uint32_t *cursor, uint64_t *key);
void session_map_free(session_map *map, const ta_allocator *allocator);

#endif /* TINYACTORS_HOST_SESSION_MAP_H */
