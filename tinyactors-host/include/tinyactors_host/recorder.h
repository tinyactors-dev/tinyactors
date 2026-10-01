#pragma once
/*
 * The recorder (kernel ADR 0006 section 2): the events the named actors of
 * the recorded families take from the kernel, handed to the binding once
 * per turn for the application to journal.
 *
 * An event is recorded when the macrostep it started ends, after its
 * effects were handed out: a delivery, or an invocation's error report as
 * the event the chart takes (error.communication.invoke.<id>.<reason>, type
 * platform). Its time is the delivery's. A macrostep that starts without
 * an event from the kernel (a bootstrap, internal events queued outside a
 * run) records nothing. The binding takes each turn's records and wakes
 * after the turn (tah_recorder_count(), tah_recorder_record(),
 * tah_recorder_wakes()) and then releases them (tah_recorder_release()).
 *
 * A wake is the time an address must be woken: its earliest pending
 * delayed send, or none (`has_at` false). It is reported for each address
 * whose actor ended a macrostep this turn, finished, was evicted
 * (tah_recorder_evicted()) or forgotten (tah_recorder_forget()).
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tah_system tah_system;
typedef struct tah_recorder tah_recorder;

/* One recorded event: its actor's address and the event as the chart took it, borrowed until release. */
typedef struct {
    ta_actor_address address;
    const ta_event *event;
    ta_time at;
} tah_record;

/* One address's wake, borrowed until release. */
typedef struct {
    ta_actor_address address;
    bool has_at;
    ta_time at;
} tah_record_wake;

/* Records the named actors of `families` from now on; `count` 0 records nothing. TA_ERR_NOMEM keeps what was. */
ta_result tah_recorder_configure(tah_system *system, const ta_str *families, uint32_t count);
bool tah_recorder_enabled(const tah_system *system);

/* This turn's records, in the order their macrosteps ended. */
uint32_t tah_recorder_count(const tah_system *system);
tah_record tah_recorder_record(const tah_system *system, uint32_t index);
/* A record's event as texts, the fields of the event the chart saw (`_event`); an absent one is empty. */
typedef struct {
    ta_str name;
    ta_event_type type;
    ta_str send_id;
    ta_str origin;
    ta_str origin_type;
    ta_str invoke_id;
} tah_record_texts;
/* Renders record `index`'s texts into `arena`; TA_ERR_NOMEM when it cannot. */
ta_result tah_recorder_texts(const tah_system *system, uint32_t index, ta_allocator arena, tah_record_texts *out);
/* This turn's wakes; NULL when there are none. */
const tah_record_wake *tah_recorder_wakes(tah_system *system, uint32_t *count);
/* Releases this turn's records and wakes, once the binding took them. */
void tah_recorder_release(tah_system *system);

/* `actor`, a recorded actor about to be destroyed by an eviction, is to be woken at `wake` (or never). */
void tah_recorder_evicted(tah_system *system, ta_actor_id actor, bool has_wake, ta_time wake);
/* `actor`, a recorded actor about to be destroyed for good, needs no wake. */
void tah_recorder_forget(tah_system *system, ta_actor_id actor);
/* Records or wakes lost for lack of memory since the system was made. */
uint64_t tah_recorder_lost(const tah_system *system);
