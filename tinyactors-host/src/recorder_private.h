#pragma once
/*
 * The kernel's own layout of what tinyactors_host/recorder.h declares
 * opaque (kernel ADR 0006 section 2): private to the kernel.
 */
#include "tinyactors_host/containers.h"
#include "tinyactors_host/recorder.h"
#include "tinyactors_host/scheduler.h"

/* An event a recorded actor took, owned: its address and the event are retained. */
typedef struct {
    ta_actor_id actor;
    ta_address *address;
    ta_event event;
    ta_time at;
    uint64_t session; /* the receiver's session, which renders a computed origin */
} tah_recorded;

/* An address whose wake this turn reports; `fixed` gives it outright (an eviction, a forget). */
typedef struct {
    ta_address *address; /* retained */
    bool fixed;
    bool has_at;
    ta_time at;
} tah_recorder_touch;

struct tah_recorder {
    bool enabled;
    ta_allocator allocator;
    tah_vec families;      /* ta_str, each allocated */
    /* The delivery or report core is being offered, until it takes it or not. */
    bool offering;
    tah_recorded offered;
    /* The events whose macrosteps are open, by actor. */
    tah_vec open;          /* tah_recorded */
    /* This turn's records, and the addresses whose wakes it reports. */
    tah_vec records;       /* tah_recorded */
    tah_vec touched;       /* tah_recorder_touch */
    tah_map touched_index; /* address -> index in `touched` */
    tah_vec wakes;         /* tah_record_wake, built for the binding */
    ta_arena arena;        /* the wakes' address texts, until release */
    bool arena_ready;
    uint64_t lost;
};

void tah_recorder_init(tah_recorder *recorder, ta_allocator allocator);
void tah_recorder_destroy(tah_recorder *recorder);
/* The scheduler's note, as the recorder reads it (scheduler.h). */
void tah_recorder_note(tah_system *system, const tah_sched_note *note);
/* `actor` is gone: an open macrostep of its records nothing. */
void tah_recorder_removed(tah_system *system, ta_actor_id actor);
