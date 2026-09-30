#pragma once
/*
 * The kernel's own layout of what tinyactors_host/loads.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/loads.h"
#include "scheduler_private.h"

struct tah_load {
    ta_address *address;   /* one reference; NULL while the record is free */
    tah_mailbox mail;
    ta_trace_parent trace; /* the first message's trace parent, or all zero */
    uint64_t started_ns;   /* monotonic, when the load started */
    uint32_t max_waiting;
    uint32_t generation;   /* half of the token JavaScript holds; never 0 */
    uint32_t next_free;
    uint8_t why;           /* tah_load_why */
    bool keyed;            /* in `by_address`: later mail for the address joins it */
};

typedef struct tah_loader_family {
    char *name; /* a copy of the family's name */
    uint32_t length;
    uint32_t max_waiting;
} tah_loader_family;

typedef struct tah_wake {
    ta_time due;
    ta_address *address; /* one reference */
} tah_wake;

struct tah_loads {
    ta_allocator allocator;
    tah_vec families;   /* tah_loader_family */
    tah_vec loads;      /* tah_load; index 0 is never issued */
    uint32_t free_head;
    uint32_t in_flight; /* loads started and not finished */
    tah_map by_address; /* (uintptr_t)address -> load index */
    tah_vec requests;   /* uint32_t: loads JavaScript has not been told about */
    uint32_t requests_head;
    tah_vec wakes;      /* tah_wake: a min-heap by due */
    tah_map wake_due;   /* (uintptr_t)address -> the due time of its live wake-up */
    uint32_t waiting;   /* messages in every load's mailbox */
    uint64_t outcomes[TAH_LOAD_OUTCOMES];
};

/*
 * The earliest wake-up's due time; false when there is none. It may be a
 * replaced or cancelled one: that only asks for a turn early, which drops
 * it (tah_loads_fire_wakes).
 */
static inline bool tah_loads_next_wake(const tah_loads *loads, ta_time *due) {
    if (loads->wakes.count == 0) {
        return false;
    }
    *due = ((const tah_wake *)loads->wakes.items)[0].due;
    return true;
}
