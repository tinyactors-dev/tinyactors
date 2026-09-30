#pragma once
/*
 * The kernel's own layout of what tinyactors_host/watch.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/watch.h"

/*
 * One open-addressing hash set of IDs, with linear probing. Its capacity
 * is a power of two at least twice its count, so a lookup touches one or
 * two slots whatever the size of the watch. When removals bring the load
 * down to an eighth, the set moves to a table where it is at most a
 * quarter (never below 8 slots), so the watch's memory
 * follows what is alive rather than its high-water mark. UINT64_MAX marks
 * an empty slot; an ID equal to it is kept in `has_max` instead.
 */
struct tah_id_set {
    uint64_t *slots;
    uint32_t capacity; /* 0, or a power of two */
    uint32_t count;
    bool has_max;
};

/*
 * Watched families are names, resolved to definitions when a delivery first
 * meets one (core ADR 0078): a definition registered after watchTraces,
 * by JavaScript or by core itself (a nested or invoke-time child), is
 * caught the same way. `matched` holds the definitions whose family is
 * watched and `checked` every definition already looked at, so each
 * definition's family is read once per watch. A definition core frees
 * leaves both (core ADR 0079), so a watch on a system that churns through
 * definitions stays the size of what is alive.
 */
struct tah_trace_watch {
    ta_allocator allocator;
    tah_id_set sessions;
    ta_str *families; /* owned copies */
    uint32_t family_count;
    tah_id_set matched;
    tah_id_set checked;
};

static inline bool tah_trace_watch_empty(const tah_trace_watch *watch) {
    return watch == NULL || watch->sessions.count + watch->family_count == 0;
}
