/**
 * @file waiting.h
 *
 * # Waiting entries: mail for a target that is not resident
 *
 * A `ta_waiting` holds one **waiting entry** for each destination that has
 * mail waiting for it, or an outstanding request for its load: a named
 * address with no resident actor, or a session that is not resident (gone,
 * finished, or held by another family's actor). Core cannot know whether
 * such a target can come back (ADR 0099; only the host knows its loaders
 * and what it unloaded), so it keeps the mail and asks the host, once per
 * entry, and the host answers: the target is resident now, or it cannot be
 * loaded. Entries are fixed-size items of an index pool (`index_pool.h`),
 * addressed by `uint32_t` index, 0 meaning none.
 *
 * ## The entry
 *
 * - `destination` is the target as the mail named it, a reference the
 *   entry holds: a named address (an interned handle, ADR 0065, so the
 *   address is held once per entry, never once per message), or a runtime
 *   ID with its family.
 * - `list` is the waiting envelopes, in the order they joined, linked
 *   through the envelopes' `next` (`envelope.h`). The run state (`run.h`)
 *   owns the envelopes and links them; this unit only holds the list head.
 * - `count` is how many envelopes name the entry: those on its list, and
 *   mail a macrostep sent to it that has not reached its list yet (on an
 *   outgoing list, or parked with a paused macrostep).
 * - `hint` is what core knew when the entry was taken: `TA_ERR_UNLOADED`
 *   (a session that was unloaded, or an address whose directory entry is
 *   unloaded), `TA_ERR_DONE` (its actor finished), or `TA_ERR_NOT_FOUND`.
 *   The host decides; the hint saves it a lookup.
 * - `generation` makes a request token stale once the entry is freed:
 *   a token is `(generation << 32) | index`.
 *
 * ## Sharing, and the table
 *
 * Mail for the same destination shares one entry: a named address is
 * found by its handle, a session by its runtime ID and family. A table
 * (`table.h`, growing mode) maps the destination's hash to the entry. A
 * runtime reference whose ID core cannot name (0: another family's actor
 * holds it, ADR 0088) is never shared: two such messages may be for
 * different sessions, so each takes an entry of its own, which is not in
 * the table.
 *
 * ## Lifecycle
 *
 * 1. **Taken** when the first mail for the destination is posted
 *    (`ta_waiting_take`), inside a macrostep or outside one.
 * 2. **Owed** when the first envelope joins its list (`ta_waiting_owe`):
 *    it goes on the owed list, which `ta_waiting_next_owed` reads in the
 *    order entries became owed, marking each one **reported**. An entry is
 *    reported once until the host answers.
 * 3. **Retired** when the host answers (`ta_waiting_retire`): it leaves
 *    the table, so later mail for the destination takes a new entry, and
 *    remembers the answer. It is **freed** (`ta_waiting_free`) once no
 *    envelope names it: at once, or when the last mail still on an
 *    outgoing list reaches it, which then follows the answer.
 *
 * An entry whose envelopes all leave before it is owed (the outgoing list
 * of an abandoned macrostep) is freed with the last of them. An entry that
 * is owed or reported stays until the host answers, even when withdrawal
 * empties its list.
 *
 * ## Invariants
 *
 * - An entry is in the table while it is shared and not retired, and in
 *   no other state.
 * - An entry is on the owed list at most once, from `ta_waiting_owe` until
 *   `ta_waiting_next_owed` reads it or it retires.
 * - A live entry holds one reference to `destination`; freeing it gives
 *   the reference back.
 *
 * ## Memory
 *
 * The system owns it; pages come from the system's allocator. Entries and
 * table room are made only at safe points, by `ta_waiting_refill`, which
 * keeps `reserve` entries free and room in the table for every entry the
 * pool could hand out without growing. Inside a macrostep an entry is
 * taken only from what is free; nothing here allocates except
 * `ta_waiting_refill`.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_waiting_take` (find or take), `ta_waiting_owe`, `ta_waiting_next_owed` | O(1) |
 * | `ta_waiting_find_request`, `ta_waiting_retire`, `ta_waiting_free` | O(1) |
 * | `ta_waiting_refill` | O(1), plus a page or a table rehash when it grows |
 *
 * ## Failure
 *
 * `ta_waiting_take` answers `TA_ERR_FULL` when no entry is free or the
 * table has no room, and changes nothing. `ta_waiting_refill` answers
 * `TA_ERR_NOMEM` when an allocation fails, leaving what is free.
 * `ta_waiting_find_request` answers 0 for a stale token.
 *
 * ## Users
 *
 * The run state (`run.h`): mail to a target that is not resident, the
 * host's load requests and answers (`ta_system_load_needed()`,
 * `ta_system_load_settled()`, `ta_system_load_failed()`), withdrawal of a
 * waiting input. ADR 0065, ADR 0088, ADR 0099.
 */
#pragma once

#include "../include/tinyactors.h"
#include "index_pool.h"
#include "list.h"
#include "table.h"

/** Each page holds `1 << TA_WAITING_PAGE_SHIFT` entries. */
#define TA_WAITING_PAGE_SHIFT 6u

/** On the owed list: the host has not been asked yet. */
#define TA_WAITING_OWED ((uint8_t)0x01u)
/** Read by `ta_waiting_next_owed`: the host was asked and has not answered. */
#define TA_WAITING_REPORTED ((uint8_t)0x02u)
/** In the table: mail for the same destination finds it. */
#define TA_WAITING_SHARED ((uint8_t)0x04u)
/** Retired: the host answered (`settled_to`, or failed). */
#define TA_WAITING_RETIRED ((uint8_t)0x08u)
/** Taken and not freed. */
#define TA_WAITING_LIVE ((uint8_t)0x10u)

/** One waiting entry; see the file comment. */
typedef struct {
    /** The free list while free; the owed list while owed. */
    uint32_t next;
    /** Bumped when the entry is freed, so an old request token is stale. */
    uint32_t generation;
    /** The waiting envelopes, through their `next`. */
    ta_list list;
    /** The destination the mail named, a reference the entry holds. */
    ta_actor_ref destination;
    /** The destination's hash, its key in the table. */
    uint32_t hash;
    /** Envelopes that name the entry: on its list, or still outgoing. */
    uint32_t count;
    /** `TA_WAITING_*`. */
    uint8_t flags;
    /** What core knew when it was taken; see the file comment. */
    ta_result hint;
    /** After a load succeeded: the actor that holds the destination now;
     * 0 after a load failed. */
    ta_actor_id settled_to;
    /** After a load failed: the fault's code, for mail that reaches the
     * entry later. */
    ta_result failed_code;
} ta_waiting_entry;

/** What `ta_waiting_init` needs. */
typedef struct {
    /** Where pages and the table come from; borrowed, and must outlive it. */
    ta_allocator allocator;
    /** The most entries: `poolEnvelopes`, since no more destinations can
     * wait than there are envelopes. */
    uint32_t bound;
    /** Free entries a refill keeps: from 1 to `bound`. */
    uint32_t reserve;
} ta_waiting_params;

/** A system's waiting entries; see the file comment. Fields are for
 * reading only. */
typedef struct {
    ta_index_pool items;
    ta_table table;
    /** Entries owing a request to the host, oldest first, through `next`. */
    ta_list owed;
    uint32_t reserve;
} ta_waiting;

/**
 * Sets up an empty set of entries with no page and no table room. Allocates
 * nothing. Answers `TA_ERR_INVALID`, leaving `*waiting` untouched, for a
 * bound or reserve its rules exclude.
 */
ta_result ta_waiting_init(ta_waiting *waiting, const ta_waiting_params *params);

/**
 * Gives back every live entry's destination reference and frees the pages
 * and the table. The envelopes on the entries' lists are the run state's,
 * which releases them first. Only frees; never fails.
 */
void ta_waiting_destroy(ta_waiting *waiting);

/**
 * Keeps `reserve` entries free and room in the table for every entry in
 * use and every free one, allocating when there is less. Runs only at a
 * safe point, never inside a macrostep. Answers `TA_OK`, or `TA_ERR_NOMEM`
 * when an allocation fails, leaving what is free.
 */
ta_result ta_waiting_refill(ta_waiting *waiting);

/** The entry at `index`, or NULL for 0 or one never allocated. O(1). */
static inline ta_waiting_entry *ta_waiting_at(const ta_waiting *waiting, uint32_t index) {
    return ta_index_pool_at(&waiting->items, index);
}

/** What `ta_waiting_take` takes an entry for. */
typedef struct {
    /** The destination, borrowed; the entry retains it when it is new. */
    const ta_actor_ref *destination;
    /** What core knows about it; see `ta_waiting_entry.hint`. */
    ta_result hint;
} ta_waiting_key;

/**
 * Answers in `*out` the live entry for `key->destination`, when one is
 * shared and not retired, or takes a free one for it: it retains the
 * destination and, unless the destination cannot be shared (a runtime ID
 * of 0), enters it into the table. Does not count an envelope: the caller
 * does, in `count`. O(1). Never allocates; may run inside a macrostep.
 * Answers `TA_ERR_FULL`, changing nothing, when a new entry is needed and
 * none is free or the table has no room.
 */
ta_result ta_waiting_take(ta_waiting *waiting, const ta_waiting_key *key, uint32_t *out);

/**
 * Marks entry `index` owed, and appends it to the owed list, unless it is
 * owed, reported or retired already. O(1). Never allocates.
 */
void ta_waiting_owe(ta_waiting *waiting, uint32_t index);

/**
 * Takes the oldest owed entry off the owed list, marks it reported and
 * answers its index; 0 when none is owed. O(1). Never allocates.
 */
uint32_t ta_waiting_next_owed(ta_waiting *waiting);

/** The request token of entry `index`: its generation and index. */
static inline uint64_t ta_waiting_token(const ta_waiting *waiting, uint32_t index) {
    return ((uint64_t)ta_waiting_at(waiting, index)->generation << 32) | index;
}

/**
 * The live, reported, unretired entry that request `token` names, or 0
 * when the token is stale or names none. O(1). Never allocates.
 */
uint32_t ta_waiting_find_request(const ta_waiting *waiting, uint64_t token);

/**
 * Retires entry `index` with the host's answer: it leaves the table and
 * the owed list, and keeps `settled_to` (0 for a failed load) and
 * `failed_code` for mail that reaches it later. O(1). Never allocates.
 */
void ta_waiting_retire(ta_waiting *waiting, uint32_t index, ta_actor_id settled_to, ta_result failed_code);

/**
 * Frees entry `index`, which no envelope names: it leaves the table and the
 * owed list if it is still in them, gives back its destination reference,
 * and becomes free with a new generation. O(1). Never allocates.
 */
void ta_waiting_free(ta_waiting *waiting, uint32_t index);
