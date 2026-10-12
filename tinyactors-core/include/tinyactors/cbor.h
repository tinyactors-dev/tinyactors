/* tinyactors/cbor.h — CBOR (RFC 8949): the encoding of host requests and
 * replies, configs, envelopes and neutral values (ADR 0009, decision 3;
 * ADR 0010, decision 4).
 *
 * Values stream: the writer writes bytes item by item into a room the
 * caller gives, growing it through an allocator the caller may give; the
 * reader is a cursor over a message that yields one item at a time. For
 * random access, the decoder turns a message into a flat list of tokens in
 * pre-order (a container's token is followed by its items, each with its
 * own children). Text and byte strings read are views into the message, so
 * nothing is copied.
 *
 * Freestanding (src/base/cbor.c): no C library, no recursion, and no
 * allocation but through the allocator a caller gives a writer.
 */
#ifndef TINYACTORS_CBOR_H
#define TINYACTORS_CBOR_H

#include <tinyactors/allocator.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- tokens ---------------------------------------------------------------- */
enum {
    TA_CBOR_UINT = 1, /* value: the integer (0 … 2^64-1) */
    TA_CBOR_NEGINT,   /* value: n, for the integer -1 - n */
    TA_CBOR_BYTES,    /* count: length; value: offset into the document's bytes */
    TA_CBOR_TEXT,     /* the same, UTF-8 (checked) */
    TA_CBOR_ARRAY,    /* count: items; the items follow, each with its children */
    TA_CBOR_MAP,      /* count: pairs; key and value tokens follow, in turn */
    TA_CBOR_TAG,      /* value: the tag; the tagged item follows */
    TA_CBOR_SIMPLE,   /* value: the simple value (0-23, 32-255): TA_CBOR_FALSE … */
    TA_CBOR_FLOAT     /* value: the double's bits (half and single are widened) */
};
enum { TA_CBOR_FALSE = 20, TA_CBOR_TRUE = 21, TA_CBOR_NULL = 22, TA_CBOR_UNDEFINED = 23 };
enum { TA_CBOR_TAG_URI = 32 }; /* an actor reference: its address */
/* Containers and tags nested deeper than this are TA_ERROR_INVALID. */
#define TA_CBOR_DEPTH_MAX 64

typedef struct ta_cbor_token {
    uint32_t kind; /* TA_CBOR_* */
    uint32_t count;
    uint64_t value;
} ta_cbor_token;

/* A message and its tokens. Decoding: `bytes` is the message and the tokens
 * are written. Encoding: the tokens are read, and text and byte tokens point
 * into `bytes`. */
typedef struct ta_cbor_document {
    ta_bytes bytes;
    ta_cbor_token *tokens; /* the caller's */
    uint32_t capacity, count;
} ta_cbor_document;

/* ---- decoding -------------------------------------------------------------- */
/* Decode one item, which must fill `bytes`, into `tokens`; `count` is the
 * number of tokens. TA_ERROR_NO_MEMORY: `count` is the number it needs, so
 * one retry with that capacity succeeds. TA_ERROR_INVALID: malformed, text
 * that is not UTF-8, nested deeper than TA_CBOR_DEPTH_MAX, or bytes after
 * the item. TA_ERROR_UNSUPPORTED: indefinite lengths, or a text or byte
 * string longer than 2^32-1. Well-formed input need not be deterministic:
 * any integer width and any key order is read; duplicate keys are not
 * checked (ta_cbor_member finds the first). */
TA_API ta_status ta_cbor_decode(ta_cbor_document *document);

/* The text or bytes of token `at`, as a view into the document's bytes; an
 * empty view for any other token. */
TA_API ta_string_view ta_cbor_string(const ta_cbor_document *document, uint32_t at);
/* The index after the item at `at` and all its children; TA_NIL if `at` is
 * not a token of the document. */
TA_API uint32_t ta_cbor_next(const ta_cbor_document *document, uint32_t at);
/* In the map at `map`, the index of the value whose key is the text `key`;
 * TA_NIL if there is none or `map` is not a map. */
TA_API uint32_t ta_cbor_member(const ta_cbor_document *document, uint32_t map, ta_string_view key);

/* ---- diagnostic notation --------------------------------------------------- */
/* One item as text (RFC 8949 §8), compact: no spaces, so a value made of
 * maps, arrays, text, integers, true, false and null reads as its JSON.
 * Text is escaped as JSON escapes it; bytes are h'00ff'; a tag is 32("…");
 * a float is the shortest decimal that reads back as it, with a point or an
 * exponent (1.0, 0.1, 1.0e+300), or NaN, Infinity, -Infinity; other simple
 * values are simple(16).
 * TA_ERROR_NO_MEMORY: out holds the text's first out->capacity bytes and
 * out->length is the capacity it needs. TA_ERROR_INVALID and
 * TA_ERROR_UNSUPPORTED as for ta_cbor_decode. For logs, traces and tests. */
TA_API ta_status ta_cbor_diagnostic(ta_bytes cbor, ta_buffer *out);

/* ---- reading, one item at a time ----------------------------------------- */
/* A cursor over one CBOR item, read in pre-order as the decoder's tokens
 * are, and checked as it goes: what ta_cbor_decode refuses, ta_cbor_read
 * refuses at the item where it finds it. It holds no memory but itself. */
typedef struct ta_cbor_reader {
    ta_bytes message;
    size_t at;      /* the next item's offset */
    uint32_t depth; /* containers and tags open */
    ta_status status; /* the first error; it sticks */
    uint64_t remaining[TA_CBOR_DEPTH_MAX]; /* items each open one still has */
} ta_cbor_reader;
/* An item: its kind and value as a token has them (TA_CBOR_*: value is the
 * integer, tag, simple value or the double's bits; count is an array's
 * items, a map's pairs, a string's length), the text or bytes of a string
 * as a view into the message, and `depth`, the containers and tags around
 * it. */
typedef struct ta_cbor_item {
    uint32_t kind;
    uint32_t count;
    uint64_t value;
    ta_bytes bytes;
    uint32_t depth;
    uint32_t pad;
} ta_cbor_item;
TA_API void ta_cbor_reader_init(ta_cbor_reader *reader, ta_bytes message);
/* The next item into *item: 1; 0 once the item is read whole and nothing
 * follows it; or an error as ta_cbor_decode has them (TA_ERROR_INVALID:
 * malformed, text that is not UTF-8, nested deeper than TA_CBOR_DEPTH_MAX,
 * bytes after the item; TA_ERROR_UNSUPPORTED: indefinite lengths, a string
 * longer than 2^32-1), which every later call answers too. */
TA_API int32_t ta_cbor_read(ta_cbor_reader *reader, ta_cbor_item *item);

/* ---- writing, one item at a time ------------------------------------------ */
/* A writer writes one CBOR item as bytes, a piece per call, into the room
 * ta_cbor_writer_init gives it (a caller's buffer, which may be on the
 * stack). When the room is full it grows through `allocator`: a block twice
 * as big, the bytes so far copied over; without one (or when it refuses),
 * the write that does not fit is TA_ERROR_NO_MEMORY, in `status`, and
 * nothing more is written (`length` goes on counting what the item needs).
 * The first error sticks in `status`; the calls return nothing, so check it
 * once, at the end, with ta_cbor_writer_bytes.
 *
 * Heads, integers, lengths and tags take their shortest form, and floats the
 * shortest width that keeps the value exactly (any NaN is f9 7e00): the core
 * deterministic rules of RFC 8949 §4.2.1 but key order, which is the order
 * written. A container's count is given when it opens, or, with
 * TA_CBOR_COUNT_LATER, fixed when ta_cbor_write_end closes it (its head is
 * then moved to its shortest form), so a container whose items a caller
 * drops as it goes is still exact. TA_ERROR_INVALID: text that is not UTF-8,
 * a simple value in 24-31, nesting deeper than TA_CBOR_DEPTH_MAX, more items
 * than one, an end that closes nothing or a map with an odd number of items.
 * Fields are the writer's: read them through the calls. */
#define TA_CBOR_COUNT_LATER UINT32_C(0xFFFFFFFF)
typedef struct ta_cbor_writer {
    unsigned char *bytes; /* the room, or the block it grew to */
    size_t capacity, length;
    const ta_allocator *allocator; /* NULL: the room never grows */
    unsigned char *room;           /* the caller's room, never given back */
    ta_status status;
    uint32_t depth;                /* containers and tags open */
    uint32_t items;                /* top-level items written: one at most */
    uint32_t pad;
    /* for each open one: its head's offset, and the items it still needs
     * (a count given up front) or has (TA_CBOR_COUNT_LATER) */
    size_t heads[TA_CBOR_DEPTH_MAX];
    uint64_t counts[TA_CBOR_DEPTH_MAX];
    uint8_t kinds[TA_CBOR_DEPTH_MAX]; /* TA_CBOR_ARRAY, _MAP, _TAG; | 0x80: counted later */
} ta_cbor_writer;
/* Start a writer in `room` (its pointer and capacity; NULL and 0 for none),
 * growing through `allocator` (NULL: never). */
TA_API void ta_cbor_writer_init(ta_cbor_writer *writer, ta_buffer room,
                                const ta_allocator *allocator);
TA_API void ta_cbor_write_map(ta_cbor_writer *writer, uint32_t pairs); /* or TA_CBOR_COUNT_LATER */
TA_API void ta_cbor_write_array(ta_cbor_writer *writer, uint32_t items); /* or TA_CBOR_COUNT_LATER */
/* Close the innermost container opened with TA_CBOR_COUNT_LATER. */
TA_API void ta_cbor_write_end(ta_cbor_writer *writer);
TA_API void ta_cbor_write_text(ta_cbor_writer *writer, ta_string_view text);
TA_API void ta_cbor_write_bytes(ta_cbor_writer *writer, ta_bytes bytes);
TA_API void ta_cbor_write_uint(ta_cbor_writer *writer, uint64_t value);
TA_API void ta_cbor_write_int(ta_cbor_writer *writer, int64_t value);
/* The integer -1 - n: every negative integer CBOR has, down to -2^64. */
TA_API void ta_cbor_write_negative(ta_cbor_writer *writer, uint64_t n);
TA_API void ta_cbor_write_float(ta_cbor_writer *writer, double value);
TA_API void ta_cbor_write_simple(ta_cbor_writer *writer, uint32_t simple);
TA_API void ta_cbor_write_tag(ta_cbor_writer *writer, uint64_t tag);
/* The item written, in `out`: a view of the writer's memory, valid until the
 * next write or ta_cbor_writer_free. The writer's status if it is an error
 * (`out` empty), TA_ERROR_INVALID when no item, or not a whole one, was
 * written. */
TA_API ta_status ta_cbor_writer_bytes(const ta_cbor_writer *writer, ta_bytes *out);
/* Give back the block the writer grew (not the caller's room). */
TA_API void ta_cbor_writer_free(ta_cbor_writer *writer);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_CBOR_H */
