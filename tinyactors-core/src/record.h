#pragma once

#include "types.h"
#include "ring.h"

/*
 * Package record is the checker for records that cross a ring: the one place
 * in core where bytes that came from outside are judged (building block C2 of
 * docs/proposals/ring-proto; the layout rules are in
 * tinyactors-core/protocol/README.md, the rings in
 * docs/proposals/ring-proto/reference/ring.md).
 *
 * What it is for. Every record on a ring starts with a 32-byte header (kind,
 * flags, size, seq, session, at), then the message's fixed fields, then a
 * tail of text and bytes that the fixed fields point to with spans (a u32
 * offset from the start of the record and a u32 length). On h2c the producer
 * is the host, so every number in the record is hostile until checked: a span
 * may point outside the record, a length may overflow a u32 sum, a kind may be
 * unknown, a seq may skip. ta_record_check turns "bytes of unknown quality"
 * into a ta_record, a token that says: this record is whole, of a known kind,
 * next in sequence, and every span in it lies inside it. Everything after
 * that, the generated getters of src/ring_proto.h included, decodes from the
 * token and never looks at the ring bytes' fixed part again.
 *
 * Why one checker, driven by data. The messages differ only in a table: where
 * the spans sit in the fixed part, which bytes hold an enum or a bool, which
 * flag bits are defined. tinyactors-core/protocol/codec.rb generates that
 * table (ta_record_descriptor, one per message) beside the typed code, and
 * this package interprets it. The security argument is therefore made once,
 * here, in a few dozen lines, and can be fuzzed and unit-tested with a table
 * that has awkward shapes (tests/record_test.c). Thirty-eight generated
 * copies of "is this span inside the record" would be thirty-eight places to
 * get it wrong.
 *
 * Lifetimes. The descriptor table is static const data (system lifetime) and
 * outlives every reader. A ta_record_reader is a small value owned by the
 * caller, next to the ta_ring handle it reads; it owns nothing and may be
 * moved or copied only while no record taken from it is still being used (a
 * copy has its own next_seq). A ta_record lives on the caller's stack from a
 * successful check until ta_ring_advance of the bytes it came from: its
 * `bytes` pointer, and every ta_bytes a getter derives from it, die at the
 * release. The fixed part is a private snapshot inside the token, so the
 * scalars a getter returns are copies and may outlive the record. Nothing in
 * this package allocates, ever: no function takes an allocator, none calls
 * ta_mem_alloc, and a failing allocator cannot affect them.
 *
 * The contract of ta_record_check. Given length bytes at `bytes` (a pointer
 * of any alignment: a ring record is 8-aligned, a record in a test file is
 * not, so every load is a ta_mem_copy), it reads only bytes inside
 * [bytes, bytes + length), and only after proving that they exist:
 *
 *   1. length >= 32 (TA_RECORD_TRUNCATED), so the header may be read;
 *   2. the header's size equals length, is a multiple of 8 and is at most
 *      the reader's maximum (TA_RECORD_SIZE). `length` is a size_t and is
 *      compared with 0xFFFFFFFF before it is narrowed;
 *   3. the kind is in the table (TA_RECORD_KIND; kind 0 is the ring's pad and
 *      never in a table) and its message belongs to the reader's ring
 *      (TA_RECORD_WRONG_RING);
 *   4. length >= the message's fixed size (TA_RECORD_TRUNCATED), so the fixed
 *      fields may be read, and they are copied into the token;
 *   5. seq (TA_RECORD_SEQ). Seq is never 0. A lossless reader needs exactly
 *      its next_seq. A lossy reader accepts any seq at or above next_seq and
 *      reports the jump as `gap`. A seq of 2^64-1 is refused because
 *      next_seq could not move past it;
 *   6. no header flag bit outside the message's defined bits (TA_RECORD_VALUE);
 *   7. every span of the message, read from the snapshot (TA_RECORD_SPAN): an
 *      empty span is (offset 0, length 0) and nothing else; a non-empty one
 *      starts at or after the fixed part and ends at or before the record's
 *      end. The test is offset <= size and length <= size - offset: never an
 *      addition, so a u32 sum cannot wrap, on a 32-bit size_t host either.
 *      Spans may overlap each other; that is harmless to memory safety;
 *   8. every enum and bool the descriptor lists holds a defined value
 *      (TA_RECORD_VALUE), so a switch over it has no out-of-range case.
 *
 * The order is part of the contract, because it decides which fault a record
 * with two faults reports and a length is never trusted before it is checked
 * against what has been proven. On success the reader's next_seq moves to
 * seq + 1. On any fault it does not move and nothing is consumed: the caller
 * decides whether a fault is a protocol failure (h2c: refuse the host and
 * stop) or a record to skip. `out` has no meaning after a fault.
 *
 * What is not checked. Text is not checked for UTF-8; that is the business
 * of whoever uses the text. Span contents stay untrusted bytes for as long as
 * they exist. Values named by a vref or a blob are the user's to validate
 * (ring.md, "Values and buffers across a ring"). Whether a message makes
 * sense in the session's state is the consumer's.
 *
 * No double fetch. A shared ring can be rewritten by the host while core
 * reads it. The checker copies the header and the fixed part into the token
 * once and validates that copy; getters decode from the copy. The record
 * bytes behind the spans are read only by the user, as bytes.
 *
 * Complexity. ta_record_check is O(log kinds + spans + domains + fixed size)
 * with a bound of 7 spans and a few domains per message in the catalog;
 * ta_record_reader_init is O(table), checked once. Both use O(1) stack and
 * make no recursive call.
 *
 * Example (the h2c pump):
 *
 *     ta_record_reader reader;
 *     ta_record_reader_init(&reader, &(ta_record_reader_params){
 *         .table = ta_msg_table(), .ring = TA_RING_H2C,
 *         .mode = TA_RING_LOSSLESS, .max_record_bytes = 65536 });
 *     ta_ring_record raw;
 *     while (ta_ring_read(&rings.ring[TA_RING_H2C], &raw) == TA_RING_OK) {
 *         ta_record record;
 *         ta_record_fault fault = ta_record_check(&reader,
 *             &(ta_record_input){ .bytes = raw.bytes, .length = raw.size }, &record);
 *         if (fault != TA_RECORD_OK) { stop(ta_record_fault_name(fault)); break; }
 *         if (record.kind == TA_MSG_H2C_EVENT) {
 *             ta_msg_h2c_event event;
 *             ta_msg_h2c_event_get(&record, &event);   // spans point into raw.bytes
 *             use(&event);
 *         }
 *         ta_ring_advance(&rings.ring[TA_RING_H2C]);   // record and its spans die here
 *     }
 */

#define TA_RECORD_HEADER_BYTES 32u
/* The largest fixed part (header included) of any message: the size of the snapshot in a ta_record. The generator checks the catalog against it. */
#define TA_RECORD_FIXED_MAX 320u
/* An enum or bool field holds a value below this; its set of allowed values is a 64-bit mask. */
#define TA_RECORD_DOMAIN_BITS 64u

/* Byte offsets in the common header, the same for every message. */
#define TA_RECORD_AT_KIND 0u
#define TA_RECORD_AT_FLAGS 2u
#define TA_RECORD_AT_SIZE 4u
#define TA_RECORD_AT_SEQ 8u
#define TA_RECORD_AT_SESSION 16u
#define TA_RECORD_AT_TIME 24u

/* Why ta_record_check refused a record, in the order the checks run. */
typedef enum ta_record_fault {
    TA_RECORD_OK = 0,
    TA_RECORD_TRUNCATED,  /* length < 32, or < the message's fixed size, or no bytes */
    TA_RECORD_SIZE,       /* size field != length, not a multiple of 8, or above the reader's maximum */
    TA_RECORD_KIND,       /* unknown kind (including 0, the pad) */
    TA_RECORD_WRONG_RING, /* a known kind that belongs to another ring */
    TA_RECORD_SEQ,        /* lossless: not next_seq; lossy: below next_seq; seq 0; seq + 1 would wrap */
    TA_RECORD_SPAN,       /* a span outside the tail, inside the fixed part, a non-canonical empty one, or overflowing u32 */
    TA_RECORD_VALUE       /* an undefined flag bit, an enum value outside its set, a bool above 1 */
} ta_record_fault;

/* Where one span field sits: its (offset, length) pair of two u32 starts at `at` in the record, inside the fixed part. */
typedef struct ta_record_span_at {
    uint16_t at;
} ta_record_span_at;

/* One checked scalar: the byte at `at` must be a member of `allowed` (bit v set means v is allowed), for values 0..63. */
typedef struct ta_record_domain {
    uint16_t at;
    uint64_t allowed;
} ta_record_domain;

/*
 * ta_record_descriptor is everything the checker needs to know about one
 * message. It is generated (src/ring_proto.c) and static const: system
 * lifetime. The pointers point at static const arrays of span_count and
 * domain_count elements.
 */
typedef struct ta_record_descriptor {
    uint16_t kind;
    uint8_t ring;           /* a ta_ring_id: the ring the message travels on */
    uint8_t span_count;
    uint8_t domain_count;
    uint16_t fixed_size;    /* header included; a multiple of 8, 32..TA_RECORD_FIXED_MAX */
    uint16_t flags_allowed; /* defined bits of the header flags: message bits 0..7, common bits 8..15 */
    const ta_record_span_at *spans;
    const ta_record_domain *domains;
} ta_record_descriptor;

/* The descriptors of every message the build knows, sorted by kind with no duplicates. System lifetime. */
typedef struct ta_record_table {
    const ta_record_descriptor *descriptor;
    uint32_t count;
} ta_record_table;

/* What a reader is to be. A value read during ta_record_reader_init; the table must outlive the reader. */
typedef struct ta_record_reader_params {
    const ta_record_table *table;
    ta_ring_id ring;           /* the ring whose records this reader reads */
    ta_ring_mode mode;         /* TA_RING_LOSSLESS: seq must match exactly; TA_RING_LOSSY: seq may jump forward */
    uint32_t max_record_bytes; /* a multiple of 8, at least TA_RECORD_HEADER_BYTES; the ring's own maximum */
} ta_record_reader_params;

/* A reader: the table, the ring it serves and the seq it expects next. Owns nothing; system lifetime; a zeroed one is unusable: ta_record_check refuses every input with TA_RECORD_KIND. */
typedef struct ta_record_reader {
    const ta_record_table *table;
    ta_ring_id ring;
    ta_ring_mode mode;
    uint32_t max_record_bytes;
    uint64_t next_seq; /* the seq the next record must carry (lossless) or reach (lossy), from 1 */
} ta_record_reader;

/*
 * ta_record is a validated record: the token every getter reads. Record
 * lifetime (consumer half): it is valid from a successful ta_record_check
 * until ta_ring_advance of the bytes it came from. `fixed` is a copy, so the
 * scalars decoded from it may be kept; `bytes` and the spans derived from it
 * may not.
 */
typedef struct ta_record {
    const unsigned char *bytes; /* the record in place; valid until ta_ring_advance */
    uint32_t size;
    uint16_t kind;
    uint64_t seq;
    uint64_t gap; /* lossy: records missing before this one; 0 on lossless */
    const ta_record_descriptor *descriptor;
    unsigned char fixed[TA_RECORD_FIXED_MAX]; /* the header and fixed part as they were when checked: no double fetch */
} ta_record;

/* The bytes to check: any alignment, `length` of them. */
typedef struct ta_record_input {
    const unsigned char *bytes;
    size_t length;
} ta_record_input;

/*
 * ta_record_reader_init validates params and the table behind it, then
 * writes the reader into caller memory; next_seq starts at 1. It answers
 * TA_ERR_INVALID, and leaves *reader untouched, when the table is NULL or
 * not sorted by kind without duplicates, when a descriptor is malformed (a
 * ring out of range, a fixed size that is not a multiple of 8 or lies outside
 * 32..TA_RECORD_FIXED_MAX, a span or domain that does not lie wholly inside
 * the fixed part after the header), when the ring or mode is out of range, or
 * when max_record_bytes is not a multiple of 8 or is below 32. Checking the
 * table here is what lets ta_record_check trust it. Never allocates.
 */
ta_result ta_record_reader_init(ta_record_reader *reader, const ta_record_reader_params *params);

/*
 * ta_record_check judges one record and, when it is whole, fills *out with
 * the token and moves reader->next_seq. It is the security boundary of the
 * package comment: it reads no byte outside the input, trusts no length
 * before checking it, and reads the fixed part exactly once. Needs an
 * initialized reader and a non-NULL input and out; the input bytes need not be
 * aligned and may change under it. Guarantees: TA_RECORD_OK implies the
 * record satisfies every check of the package comment; any other result
 * leaves reader->next_seq as it was and *out meaningless. Never allocates.
 */
ta_record_fault ta_record_check(ta_record_reader *reader, const ta_record_input *input, ta_record *out);

/* ta_record_fault_name returns the fault's name as a string literal ("span", "seq", ...), for logs and test messages. Never NULL. */
const char *ta_record_fault_name(ta_record_fault fault);

/*
 * ta_record_u8, ta_record_u16, ta_record_u32 and ta_record_u64 decode one
 * scalar of the fixed part, at byte offset `at`, from the snapshot in the
 * token and never from the ring bytes (no double fetch). A scalar that does
 * not lie wholly inside the fixed part decodes as 0: generated getters pass
 * constants that lie inside, so this is a guard against a wrong table, not a
 * path. Native byte order, as the ring. Never allocates.
 */
uint8_t ta_record_u8(const ta_record *record, uint32_t at);
uint16_t ta_record_u16(const ta_record *record, uint32_t at);
uint32_t ta_record_u32(const ta_record *record, uint32_t at);
uint64_t ta_record_u64(const ta_record *record, uint32_t at);

/*
 * ta_record_span returns the bytes the span field at fixed-part offset `at`
 * names, as a view into record->bytes: valid until ta_ring_advance. An empty
 * span, or one that does not lie inside the record, answers {NULL, 0}; after
 * a successful check the second cannot happen, and the test is repeated so a
 * hand-built token cannot make this read outside the record. Never allocates.
 */
ta_bytes ta_record_span(const ta_record *record, uint32_t at);

/* ta_record_in_domain says whether value is a member of the 64-bit set `allowed` (values 0..63; a larger value is not a member). */
bool ta_record_in_domain(uint64_t allowed, uint32_t value);

/*
 * ta_record_bytes_ok says whether a ta_bytes a caller wants written is
 * well formed: a pointer, or a length of 0. A non-empty view without bytes
 * is the one input of a writer that cannot be measured or copied.
 */
bool ta_record_bytes_ok(ta_bytes bytes);

/*
 * ta_record_tail is the writer's cursor over the variable part of one record
 * being filled in a reservation: `record` is the reservation's first byte and
 * `at` the offset of the next free tail byte, starting at the fixed size. It
 * lives on the writer's stack for one call of a generated writer, and holds
 * no ownership: the reservation is the ring's, valid until commit.
 */
typedef struct ta_record_tail {
    unsigned char *record;
    uint32_t at;
} ta_record_tail;

/*
 * ta_record_put_span is the writer's half of a span field. It copies bytes to
 * tail->record + tail->at (nothing when empty), stores the span that names
 * them in *span ((0, 0) when empty: the canonical empty span), and advances
 * tail->at past them. The caller has already measured the whole record with
 * a ta_layout, so tail->at + bytes.len is inside the reservation and cannot
 * wrap; this function does no bounds check of its own and must not be called
 * without that measurement. `bytes` must not overlap the record. Never
 * allocates.
 */
void ta_record_put_span(ta_record_tail *tail, ta_span32 *span, ta_bytes bytes);
