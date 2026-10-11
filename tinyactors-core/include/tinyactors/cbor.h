/* tinyactors/cbor.h — CBOR (RFC 8949) as token lists in caller memory: the
 * encoding of host requests and replies (ADR 0009, decision 3).
 *
 * The decoder turns a message into a flat list of tokens; the encoder turns
 * a token list into a message. A token list is in pre-order: a container's
 * token is followed by its items, each with its own children. Text and byte
 * tokens are views into the message, so nothing is copied.
 *
 * Freestanding (src/base/cbor.c): no C library, no allocation, no recursion.
 */
#ifndef TINYACTORS_CBOR_H
#define TINYACTORS_CBOR_H

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

/* ---- encoding -------------------------------------------------------------- */
/* The tokens as one CBOR item, written into `out` from its start: shortest
 * integers, lengths and tags, definite lengths, and floats in the shortest
 * width that keeps the value exactly (any NaN is f9 7e00). These are the
 * core deterministic rules of RFC 8949 §4.2.1 except key order: map pairs
 * are written in token order, not sorted. TA_ERROR_INVALID when the tokens
 * do not form exactly one item, nest deeper than TA_CBOR_DEPTH_MAX, point
 * outside `bytes`, hold text that is not UTF-8, or a simple value in 24-31.
 * TA_ERROR_NO_MEMORY: out->length is the capacity it needs. */
TA_API ta_status ta_cbor_encode(const ta_cbor_document *document, ta_buffer *out);

/* ---- writing tokens from C ------------------------------------------------- */
/* A document being built: tokens go to `document`, their text and bytes to
 * `strings`, and a text or byte token's value is its offset in `strings`.
 * Start it zeroed but for document.tokens, document.capacity, strings.pointer
 * and strings.capacity. The first error sticks in `status`; check it once, at
 * the end. On TA_ERROR_NO_MEMORY, writing goes on counting: document.count
 * and strings.length are what the document needs. A container's count is
 * given up front; the writer does not check that its items follow
 * (ta_cbor_finish does). */
typedef struct ta_cbor_writer {
    ta_cbor_document document;
    ta_buffer strings;
    ta_status status;
} ta_cbor_writer;
TA_API void ta_cbor_write_map(ta_cbor_writer *writer, uint32_t pairs);
TA_API void ta_cbor_write_array(ta_cbor_writer *writer, uint32_t items);
TA_API void ta_cbor_write_text(ta_cbor_writer *writer, ta_string_view text);
TA_API void ta_cbor_write_bytes(ta_cbor_writer *writer, ta_bytes bytes);
TA_API void ta_cbor_write_uint(ta_cbor_writer *writer, uint64_t value);
TA_API void ta_cbor_write_int(ta_cbor_writer *writer, int64_t value);
TA_API void ta_cbor_write_float(ta_cbor_writer *writer, double value);
TA_API void ta_cbor_write_simple(ta_cbor_writer *writer, uint32_t simple);
TA_API void ta_cbor_write_tag(ta_cbor_writer *writer, uint64_t tag);
/* Encode what was written: the writer's status if it is an error (out->length
 * is then 0), else ta_cbor_encode of the document over `strings`. */
TA_API ta_status ta_cbor_finish(ta_cbor_writer *writer, ta_buffer *out);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_CBOR_H */
