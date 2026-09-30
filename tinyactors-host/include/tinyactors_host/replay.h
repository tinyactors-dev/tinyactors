#pragma once
/*
 * Replay and checkpoints (kernel ADR 0002 sections 6 and 7): the kernel
 * rebuilds a named actor from its input log by calling core with the logged
 * arguments, and writes and restores checkpoints from core's exported state
 * (core ADR 0094 section 3).
 *
 * A replay reads a log from bytes pushed in pieces, cut anywhere, and holds
 * only one record (or one checkpoint unit), the piece being read, and the
 * messages the log shows accepted and not yet delivered: memory is bounded
 * by the mail owed at any point, not by the length of the log.
 *
 * - tah_replay_prepare() reads the log up to its first life's first record:
 *   `spawned` makes a replaying actor whose clock starts at the record's
 *   time and whose data is its binding; a checkpoint restores the state it
 *   holds. Records before it (mail that waited for a load) are kept as owed.
 * - tah_replay_run() applies records to the replaying actor, a window at a
 *   time: ACCEPTED holds a message; DELIVERED (or JUMP) delivers the held
 *   message with the logged time and resumes the actor until idle; DROPPED
 *   forgets one; WITHDRAWN, ABANDONED and RELEASED make the operator's call
 *   again; FIRED and EVICTED only carry time, which the next input's time
 *   (or the publish) applies; FINISHED and DESTROYED end the life. Records a
 *   checkpoint the replay started from covers are skipped.
 * - tah_replay_redeliver() puts the messages still owed back in the
 *   published actor's mailbox, keeping their IDs.
 *
 * Nothing a replaying actor does is recorded: it is unpublished.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef struct tah_system tah_system;
typedef struct tah_replay tah_replay;

/* A replay over `system`'s log records; NULL without memory. */
tah_replay *tah_replay_open(tah_system *system);
/* Releases what the replay holds; idempotent while the system lives. */
void tah_replay_close(tah_replay *replay);
/* Frees the replay; closes it first. */
void tah_replay_free(tah_replay *replay);
bool tah_replay_closed(const tah_replay *replay);
tah_system *tah_replay_system(const tah_replay *replay);

/* Adds the next piece of the log; TA_ERR_NOMEM keeps what it had. */
ta_result tah_replay_push(tah_replay *replay, const uint8_t *bytes, size_t length);
/* Bytes pushed and not yet read. */
size_t tah_replay_buffered(const tah_replay *replay);

typedef struct {
    ta_definition_id definition;
    ta_actor_id incumbent;             /* replace this actor, or 0 */
    const ta_actor_address *address;   /* or take this address */
    TA_OUT ta_actor_id *out;
    TA_OUT bool *found;                /* false: more bytes are needed, or the log has no (further) life */
} tah_replay_prepare_params;
/*
 * Prepares a replaying actor from the log's next life. TA_ERR_CORRUPT for
 * bytes that are no log, TA_ERR_DEFINITION_MISMATCH for a checkpoint of
 * another revision, and core's results.
 */
ta_result tah_replay_prepare(tah_replay *replay, const tah_replay_prepare_params *params);

typedef enum {
    TAH_REPLAY_MORE = 0,  /* the window was applied; call again */
    TAH_REPLAY_NEED,      /* the next record is not all there: push more */
    TAH_REPLAY_FINISHED,  /* the actor reached a top-level final state */
    TAH_REPLAY_ENDED,     /* a DESTROYED record: this life ended */
    TAH_REPLAY_LIFE       /* between lives: the next life's first record */
} tah_replay_state;

typedef struct {
    ta_actor_id actor;
    uint32_t most;        /* records to apply at most */
    bool between;         /* only mail between two lives, up to the next life */
    /* An event the current revision did not use: its name and time. */
    void (*ignored)(void *context, ta_str name, ta_time time);
    void *context;
    TA_OUT tah_replay_state *state;
    TA_OUT uint32_t *records;
} tah_replay_run_params;
ta_result tah_replay_run(tah_replay *replay, const tah_replay_run_params *params);

/* The latest time of the records read, and whether there was one. A
 * delivery's record carries the delivery's time and follows records of the
 * later turns its macrostep ran in (kernel ADR 0004), so the last record's
 * time can be an earlier one. */
bool tah_replay_last_time(const tah_replay *replay, ta_time *out);
/* The replaying actor is about to be published: the scheduler takes it
 * back, and its record numbers continue from the log's. */
void tah_replay_finish(tah_replay *replay, ta_actor_id actor);
/* Puts the messages still owed back in `actor`'s mailbox; `count` is how many. */
ta_result tah_replay_redeliver(tah_replay *replay, ta_actor_id actor, uint32_t *count);

/*
 * Writes a checkpoint of `actor`, an idle, published, named actor of a
 * logged family, through `sink` as one unit: the CHECKPOINT record, its
 * parts, and an ACCEPTED record for each message its mailbox still owes.
 * TA_ERR_INVALID for an actor that is not logged, TA_ERR_BUSY for one that is
 * not idle, TA_ERR_DONE for one that finished, TA_ERR_UNSUPPORTED for a value
 * with no durable form, TA_ERR_NOMEM when the sink refused or memory ran
 * out.
 */
ta_result tah_checkpoint_write(tah_system *system, ta_actor_id actor, ta_byte_sink sink);
