/* host/session_map.c — see session_map.h. */
#include "host/session_map.h"
#include "base/memory.h"

static uint32_t session_map_slot(const session_map *map, uint64_t key) {
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (map->capacity - 1);
    while (map->keys[i] && map->keys[i] != key)
        i = (i + 1) & (map->capacity - 1);
    return i;
}

int session_map_reserve(session_map *map, uint32_t extra, const ta_allocator *allocator) {
    if ((uint64_t)(map->used + extra) * 2 <= map->capacity) return 1;
    uint32_t c = 64;
    while ((uint64_t)(map->count + extra) * 4 > c)
        c *= 2;
    uint64_t *keys = (uint64_t *)allocator->alloc(allocator->context, sizeof(uint64_t) * c, 8);
    uint32_t *values = keys ? (uint32_t *)allocator->alloc(allocator->context, sizeof(uint32_t) * c, 8) : NULL;
    if (!values) {
        if (keys) allocator->free(allocator->context, keys, sizeof(uint64_t) * c);
        return 0;
    }
    ta_memset(keys, 0, sizeof(uint64_t) * c);
    session_map old = *map;
    map->keys = keys;
    map->values = values;
    map->capacity = c;
    map->count = map->used = 0;
    for (uint32_t i = 0; i < old.capacity; i++)
        if (old.keys[i] && old.values[i] != TA_NIL) {
            uint32_t at = session_map_slot(map, old.keys[i]);
            map->keys[at] = old.keys[i];
            map->values[at] = old.values[i];
            map->count++;
            map->used++;
        }
    session_map_free(&old, allocator);
    return 1;
}

void session_map_put(session_map *map, uint64_t key, uint32_t value) {
    if (!map->capacity) return; /* nothing reserved: nothing to remove */
    uint32_t i = session_map_slot(map, key);
    if (map->keys[i] == key) { /* existing, or removed (key kept, value TA_NIL) */
        if (map->values[i] == TA_NIL && value != TA_NIL) map->count++;
        if (map->values[i] != TA_NIL && value == TA_NIL) map->count--;
        map->values[i] = value;
    } else if (value != TA_NIL) {
        map->keys[i] = key;
        map->values[i] = value;
        map->count++;
        map->used++;
    }
}

uint32_t session_map_get(const session_map *map, uint64_t key) {
    if (!map->capacity) return TA_NIL;
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (map->capacity - 1);
    while (map->keys[i]) {
        if (map->keys[i] == key) return map->values[i];
        i = (i + 1) & (map->capacity - 1);
    }
    return TA_NIL;
}

uint32_t session_map_next(const session_map *map, uint32_t *cursor, uint64_t *key) {
    for (; *cursor < map->capacity; (*cursor)++)
        if (map->keys[*cursor] && map->values[*cursor] != TA_NIL) {
            *key = map->keys[*cursor];
            return map->values[(*cursor)++];
        }
    return TA_NIL;
}

void session_map_free(session_map *map, const ta_allocator *allocator) {
    if (map->keys) allocator->free(allocator->context, map->keys, sizeof(uint64_t) * map->capacity);
    if (map->values) allocator->free(allocator->context, map->values, sizeof(uint32_t) * map->capacity);
    map->keys = NULL;
    map->values = NULL;
    map->capacity = map->count = map->used = 0;
}
