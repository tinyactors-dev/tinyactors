#pragma once

#include "types.h"
#include "layout.h"

/*
 * Package ring is the SPSC byte ring and the control block that holds three
 * of them (building blocks C1 and C5 of docs/proposals/ring-proto; the
 * protocol is docs/proposals/ring-proto/reference/ring.md).
 *
 * What it is for. A ring is how core and its host hand each other records
 * without a call per record: one producer writes, one consumer reads, both
 * on the system's own thread, so a ring needs no lock and no atomics. There
 * are three, one per delivery guarantee: h2c (host to core, lossless), c2h
 * (core to host, lossless) and tel (core to host, lossy). This unit is the
 * mechanism only. It knows nothing about kinds, sessions or what a record
 * means; the record generator (which writes the header fields it owns) and
 * the users (the tel writer, the h2c reader) come later and never allocate
 * ring memory themselves.
 *
 * What the ring knows about a record. A record is at least 16 bytes
 * (TA_RING_MIN_RECORD), a multiple of 8, and starts 8-byte aligned. Bytes
 * 4..8 hold its size (u32) and 8..16 its seq (u64): the ring writes those two
 * fields at commit, so the producer fills the rest (kind and flags at 0..4,
 * and everything after 16) and must not rely on what it puts in 4..16. A
 * record's kind is never 0: kind 0 is how the consumer recognizes a pad. A pad
 * is an 8-byte prefix with kind 0, flags 0 and size = the bytes up to the end
 * of the buffer; it carries no seq, and is never shown to the consumer.
 * Native byte order, as the rest of the record.
 *
 * Writing: reserve, fill, commit. ta_ring_reserve picks a place for the
 * record in a private write position. Nothing is visible and no byte of the
 * ring is written until ta_ring_commit, so ta_ring_abandon (the producer's
 * fill failed, and it released its provisional retains) leaves the ring
 * byte-for-byte and counter-for-counter as it was and burns no seq. At most
 * one reservation is open per ring. A record is contiguous: when it does not
 * fit before the end of the buffer, commit first writes a pad over the
 * remainder and the record starts at offset 0. The pad costs up to
 * max_record_bytes - 8 bytes of room per wrap, and a reservation must fit pad
 * and record together; whoever sizes a reserve (the c2h mandatory reserve)
 * must count one pad per record or use the position-aware bound.
 *
 * Reading: read, use, advance. ta_ring_read gives the oldest record in
 * place, after silently skipping (and releasing) any pad in front of it.
 * Reading again without an advance gives the same record. ta_ring_advance
 * releases it. A consumer that keeps anything past the advance copies it, and
 * the bytes its spans point to. Advancing the tail releases ring bytes only:
 * vrefs and blobs named in a record are the user's to release (ring.md,
 * "Values and buffers across a ring").
 *
 * Names. These are the handle-level calls, on a ta_ring. The system's
 * public pair, ta_ring_peek and ta_ring_release (include/tinyactors.h), take
 * a ta_system and a ta_ring_id and call the handle-level ta_ring_read and
 * ta_ring_advance (the consumer's pair); the names differ because
 * the amalgamation is one translation unit and C has no overloading.
 *
 * An absent ring. A ring whose capacity (and max_record_bytes) is 0 does not
 * exist: the control block has its entry, so a reader finds all three, but it
 * holds no bytes and layout adds none for it. Reserving on it answers
 * TA_RING_TOO_BIG, reading answers TA_RING_EMPTY, and ta_ring_would_fit says
 * false. The system creates h2c and c2h this way until the ring-proto phase
 * that introduces them (5.1).
 *
 * Modes. Lossless (h2c, c2h): when the record does not fit, reserve answers
 * TA_RING_FULL and nothing changes. Lossy (tel): reserve answers
 * TA_RING_DROPPED, counts it in `dropped`, and consumes the record's seq, so
 * the consumer sees a jump in seq and ta_ring_record.gap says how many
 * records are missing before this one. Both counters are stored in the
 * control block so the host reads them. One exception to "FULL changes
 * nothing" exists, and is invisible: when the ring holds no unreleased byte,
 * the head is at a nonzero offset and the record cannot fit with its pad
 * (a record over half the capacity, up to capacity == max_record_bytes), reserve commits the pad alone and answers
 * FULL (or DROPPED); the consumer's next peek releases it and the retry
 * succeeds. That is what guarantees progress, and it takes no seq.
 * A request over max_record_bytes is TA_RING_TOO_BIG, a caller error: it
 * changes nothing in either mode and counts neither a seq nor a drop.
 *
 * The control block. ta_ring_control sits at the start of one region, with
 * the three data areas behind it (ta_rings_layout computes the offsets). The
 * host reads it at any time; ta_ring_shared names who writes each cell. head
 * and next_seq are the producer's, tail is the consumer's; capacity, offset,
 * max_record_bytes and mode are core's, once, at init. Each side trusts only
 * its own counter: the handle keeps a private copy of its own and treats the
 * other side's shared cell as untrusted input (the host writes h2c head and
 * the c2h and tel tails). A peer counter that moves backwards, or leaves
 * [tail, head], or puts head past tail + capacity, or a record header that
 * breaks the framing rules, answers TA_RING_CORRUPT: a protocol failure
 * (ring.md, "Corrupt framing is a protocol failure"). The caller stops using
 * that ring; the ring does not try to guess.
 *
 * Types. ta_ring_id, ta_ring_shared, ta_ring_control and ta_ring_record are
 * public (include/tinyactors.h): a host reads the control block and the
 * records through them. This file keeps what only core's unit needs: the
 * specs, the plan and the handles.
 *
 * Lifetimes.
 *   - system: the region (control block and the three data areas), allocated
 *     by the system's creation as one entry of its own layout, with the
 *     size and alignment of ta_rings_plan, and freed with those at
 *     destruction. This unit never allocates and never frees. Nothing grows
 *     or moves. Pointers into the region (a record view, the control block)
 *     die with it.
 *   - system: the ta_rings handles, owned by value by the system. They own
 *     nothing; a zeroed ta_rings is a destroyed one.
 *   - record, producer half: a ta_ring_reservation, from a successful
 *     reserve to commit or abandon. Its bytes pointer is not used after.
 *   - record: a committed record, from commit to release. The pointer in a
 *     ta_ring_record is valid until ta_ring_advance.
 *   - call: a ta_rings_plan.
 *
 * When it allocates: never. ta_rings_layout is arithmetic and
 * ta_rings_init writes into memory the caller owns. Nothing here can fail
 * with TA_ERR_NOMEM except by arithmetic overflow in the layout.
 *
 * Invariants (between calls, per ring, for the private copies):
 *   I1. tail <= head <= tail + capacity, counters in bytes that only grow,
 *       pads included. Positions are counter % capacity. Arithmetic is on
 *       differences (head - tail), never on sums of counters. Counters are
 *       assumed never to wrap u64 (2^64 bytes is unreachable); capacity need
 *       not be a power of two, so a wrap would break I2.
 *   I2. Every committed record and pad starts at a multiple of 8, a record
 *       is contiguous (offset + size <= capacity) and a pad ends exactly at
 *       the buffer's end.
 *   I3. seq of committed records is 1, 2, 3, ... in commit order. In lossy
 *       mode a dropped record takes its seq; next_seq = records committed
 *       + dropped + 1.
 *   I4. At most one reservation is open and at most one record is peeked.
 *
 * Complexity: every call is O(1) in time and space (the only loop is the
 * pad skip in peek, which runs at most once per wrap).
 *
 * Failure behaviour. Layout and init answer ta_result; init failure zeroes
 * the handles and writes nothing into the memory. Producer and consumer
 * calls answer ta_ring_status. On any non-OK answer of reserve the ring is
 * unchanged, with two documented exceptions: DROPPED (seq and `dropped`
 * move) and the lone pad above. A peek that answers EMPTY or CORRUPT may
 * already have released one pad (and moved the tail) on its way; it never
 * consumes a record.
 *
 * Usage. The system creates the region, then produces and consumes:
 *
 *	ta_rings_specs specs = { .ring = {
 *		[TA_RING_H2C] = { 4096, 512, TA_RING_LOSSLESS },
 *		[TA_RING_C2H] = { 4096, 512, TA_RING_LOSSLESS },
 *		[TA_RING_TEL] = { 8192, 256, TA_RING_LOSSY } } };
 *	ta_rings_plan plan;
 *	if (ta_rings_layout(&specs, &plan) != TA_OK) return TA_ERR_NOMEM;
 *	void *memory = ta_mem_alloc(allocator, plan.bytes, plan.alignment);
 *	ta_rings rings;
 *	ta_rings_init(&rings, &(ta_rings_init_params){ memory, plan.bytes, specs });
 *
 *	ta_ring_reservation r;
 *	if (ta_ring_reserve(&rings.ring[TA_RING_TEL], 48, &r) == TA_RING_OK) {
 *		fill(r.bytes, r.size);   // kind and flags at 0..4, the rest from 16
 *		ta_ring_commit(&rings.ring[TA_RING_TEL]);
 *	}
 *	ta_ring_record rec;
 *	while (ta_ring_read(&rings.ring[TA_RING_TEL], &rec) == TA_RING_OK) {
 *		use(rec.bytes, rec.size, rec.seq, rec.gap);
 *		ta_ring_advance(&rings.ring[TA_RING_TEL]);
 *	}
 *	ta_mem_free(allocator, memory, &(ta_allocator_free_params){ memory, plan.bytes, plan.alignment });
 */

typedef enum ta_ring_mode {
    TA_RING_LOSSLESS = 0, /* full: refuse, nothing changes (h2c, c2h) */
    TA_RING_LOSSY = 1     /* full: drop the record, count it, consume its seq (tel) */
} ta_ring_mode;

#define TA_RING_ALIGN 8u       /* every record starts, and every size is, a multiple of this */
#define TA_RING_MIN_RECORD 16u /* kind, flags, size, seq */

/* The cells of ring `id` in the control block (the public struct names its three, one per ring); NULL for an unknown id. */
static inline ta_ring_shared *ta_ring_cell(ta_ring_control *control, ta_ring_id id) {
    switch (id) {
    case TA_RING_H2C: return &control->h2c;
    case TA_RING_C2H: return &control->c2h;
    case TA_RING_TEL: return &control->tel;
    default: return NULL;
    }
}

/* What one ring is to be. A value read during the call. */
typedef struct ta_ring_spec {
    uint32_t capacity;         /* bytes; a multiple of 8; at least max_record_bytes; 0: the ring does not exist */
    uint32_t max_record_bytes; /* TA_RING_MIN_RECORD..capacity, a multiple of 8; 0 exactly when capacity is 0 */
    ta_ring_mode mode;         /* TA_RING_LOSSLESS or TA_RING_LOSSY */
} ta_ring_spec;

typedef struct ta_rings_specs {
    ta_ring_spec ring[TA_RING_COUNT];
} ta_rings_specs;

/* Where everything goes in the one region; a call-lifetime value. The control block is at offset 0. */
typedef struct ta_rings_plan {
    size_t bytes;                      /* total, unpadded (the ta_layout rule): the size of the allocation and of its sized free */
    size_t alignment;                  /* the region's alignment */
    size_t data_offset[TA_RING_COUNT]; /* from the region's origin */
} ta_rings_plan;

/*
 * A handle on one ring: private copies of the counters and the one open
 * reservation. Fields are private to ring.c. Both halves live in the same
 * struct, because a test (and a debugger) drives both sides; in production
 * core uses only the half it is, and the host reads the shared cells.
 */
typedef struct ta_ring {
    ta_ring_shared *shared; /* into the control block */
    unsigned char *data;    /* into the region */
    uint64_t capacity;      /* private: the shared cell is not trusted after init */
    uint64_t head;          /* producer-authoritative */
    uint64_t peer_tail;     /* producer: the last tail it validated from the shared cell */
    uint64_t next_seq;      /* producer-authoritative */
    uint64_t dropped;       /* producer-authoritative: records dropped so far */
    uint64_t pending_seq;   /* open reservation: the seq it carries */
    uint64_t tail;          /* consumer-authoritative */
    uint64_t peer_head;     /* consumer: the last head it validated from the shared cell */
    uint64_t expected_seq;  /* consumer: the next seq it accepts, from 1 */
    uint64_t peeked_seq;    /* the record last peeked and not yet released */
    uint32_t pending_size;  /* open reservation: rounded size; 0 = none open */
    uint32_t pending_pad;   /* open reservation: pad bytes to write at commit; 0 = none */
    uint32_t peeked_size;   /* 0 = nothing peeked */
    uint32_t max_record_bytes; /* private copy: the shared cell is not trusted after init */
    ta_ring_mode mode;         /* private copy, for the same reason */
} ta_ring;

/* The three handles over one region. Owns nothing; system lifetime. */
typedef struct ta_rings {
    ta_ring_control *control;
    ta_ring ring[TA_RING_COUNT];
} ta_rings;

typedef struct ta_rings_init_params {
    void *memory;        /* plan.bytes bytes aligned to plan.alignment, zero or not; outlives the rings */
    size_t memory_bytes; /* checked against the recomputed plan */
    ta_rings_specs specs; /* what each of the three rings is to be */
} ta_rings_init_params;

typedef enum ta_ring_status {
    TA_RING_OK = 0,
    TA_RING_FULL,    /* lossless, no room; the ring is unchanged except for the invisible lone pad */
    TA_RING_DROPPED, /* lossy, no room; dropped += 1 and the seq in the out struct is consumed */
    TA_RING_TOO_BIG, /* rounded size above max_record_bytes or below TA_RING_MIN_RECORD; nothing changes */
    TA_RING_BUSY,    /* a reservation is already open; nothing changes */
    TA_RING_EMPTY,   /* peek only: no committed record */
    TA_RING_CORRUPT  /* the peer's counter or a record header broke an invariant: a protocol failure */
} ta_ring_status;

/* An open reservation: the place to fill. */
typedef struct ta_ring_reservation {
    unsigned char *bytes; /* `size` bytes to fill, 8-aligned, contiguous; NULL when FULL or DROPPED */
    uint32_t size;        /* the requested size rounded up to 8 */
    uint64_t seq;         /* the seq this record carries (OK), would carry on retry (FULL) or consumed (DROPPED) */
} ta_ring_reservation;

/*
 * ta_rings_layout computes where the control block and the three data areas
 * go in one region, with ta_layout. It validates the specs (capacity and
 * max_record_bytes multiples of 8, TA_RING_MIN_RECORD <= max_record_bytes <=
 * capacity, a known mode) and answers TA_ERR_INVALID if one is wrong, and
 * TA_ERR_NOMEM if a size does not fit a size_t. *plan is untouched on
 * failure. It never allocates: the caller makes the one allocation of
 * plan->bytes, plan->alignment, at a safe point, and keeps both numbers for
 * the sized free.
 */
ta_result ta_rings_layout(const ta_rings_specs *specs, ta_rings_plan *plan);

/*
 * ta_rings_init builds the rings over caller memory: it recomputes the plan,
 * checks that the memory is non-NULL, aligned and large enough, zeroes the
 * control block, writes magic, version, capacities, offsets, limits and
 * modes, starts every seq at 1 and fills the handles. The data areas are not
 * scrubbed. It never allocates. On failure (TA_ERR_INVALID, or the layout's
 * answer) *rings is zeroed and the memory is untouched.
 */
ta_result ta_rings_init(ta_rings *rings, const ta_rings_init_params *params);

/*
 * ta_ring_reserve (producer) rounds `size` up to 8 with a checked add and
 * finds room for it, after the pad when it would straddle the end. It reads
 * the consumer's tail from the control block as untrusted input. It answers:
 *   OK       *out names the bytes to fill and the seq; nothing is visible yet,
 *            and no shared cell changed: only ta_ring_commit publishes.
 *   FULL     lossless; *out has NULL bytes. Retry after the consumer releases.
 *   DROPPED  lossy; *out has NULL bytes and the consumed seq.
 *   TOO_BIG, BUSY, CORRUPT   *out is not written (a BUSY caller's open
 *            reservation therefore survives passing the same struct).
 * It never allocates and, except for DROPPED and the lone pad (see the
 * package comment), changes nothing on any answer other than OK.
 */
ta_ring_status ta_ring_reserve(ta_ring *ring, uint32_t size, ta_ring_reservation *out);

/*
 * ta_ring_would_fit (producer) says whether ta_ring_reserve(ring, size, ...)
 * would answer TA_RING_OK right now, and changes nothing a reader or a later
 * reserve can see (it reads the consumer's tail like reserve does, and
 * keeps the validated value). It is exact, the wrap pad included: it is
 * the same test reserve makes, and it is false in the cases where reserve
 * answers anything but OK, the lone-pad case (see the package comment)
 * among them, since no record would be written. It is how a lossy producer
 * checks that a second record fits before it writes a first, so that a
 * record it must not lose to a drop (tel.dropped) is never reserved by
 * guesswork: a failed reserve on a lossy ring burns a seq and counts a drop.
 * It answers false while a reservation is open, for a corrupt tail, for a
 * size over max_record_bytes and for an absent ring.
 */
bool ta_ring_would_fit(ta_ring *ring, uint32_t size);

/*
 * ta_ring_drop (producer) records a record the producer decided not to write
 * as lost, on a lossy ring: it consumes the record's seq and counts one drop
 * exactly as a refused ta_ring_reserve does (both counters stored at once), and
 * returns the seq consumed, so that the consumer sees the gap. It is how a
 * producer that cannot even build a record (it is over max_record_bytes
 * once its texts are rendered) keeps the gap honest. On a lossless ring, while
 * a reservation is open and on an absent ring it does nothing and returns 0:
 * a lossless record that cannot be written is the producer's to retry, not
 * to lose. It never allocates.
 */
uint64_t ta_ring_drop(ta_ring *ring);

/*
 * ta_ring_commit (producer) publishes the open reservation: it writes size
 * and seq into bytes 4..16 of the record, the pad if the record wrapped, and
 * only then stores head and next_seq, so a reader sees whole records. The
 * reservation's pointer is dead after it. With no open reservation it does
 * nothing.
 */
void ta_ring_commit(ta_ring *ring);

/*
 * ta_ring_abandon (producer) forgets the open reservation. No byte, counter
 * or seq changes, so the next reserve returns the same place and seq. It is
 * how a failed fill gives its space back. With none open it does nothing.
 */
void ta_ring_abandon(ta_ring *ring);

/*
 * ta_ring_read (consumer) shows the oldest committed record in place. It
 * reads head from the control block as untrusted input, validates it and the
 * record's framing (size at least TA_RING_MIN_RECORD, a multiple of 8,
 * within what is committed and before the end of the buffer; a pad that ends
 * exactly at the end), skips and releases a pad, and checks the seq (C5): it
 * must be the next one expected on a lossless ring, and may jump forward on a
 * lossy one, reported in out->gap. Answers OK, EMPTY or CORRUPT; *out is
 * written only on OK. Peeking twice without a release gives the same record.
 * A record larger than max_record_bytes that is otherwise well framed is
 * shown, not refused: bouncing it is the user's policy (ring.md).
 */
ta_ring_status ta_ring_read(ta_ring *ring, ta_ring_record *out);

/*
 * ta_ring_advance (consumer) releases the record last peeked: it advances
 * the tail, stores it where the producer reads it and expects the next seq.
 * The record's bytes may be overwritten from then on. With nothing peeked it
 * does nothing, so it can never move the tail past the head.
 */
void ta_ring_advance(ta_ring *ring);
