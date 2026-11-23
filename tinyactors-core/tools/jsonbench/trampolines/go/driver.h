/* The C side of the Go trampoline spike: the driver loop that calls a
 * function as the json datamodel does, the pure-C baselines, and the
 * helpers. Three result modes:
 *
 *   writer       jb_writer_function (the json datamodel's ABI before it
 *                changed to bytes, tinyactors/datamodel_json.h): a fresh
 *                writer over a fixed room, no allocator, one jb_writer_call,
 *                ta_cbor_writer_bytes after; Go's encoded bytes are
 *                re-emitted item by item (jb_write_item).
 *   bytes        jb_bytes_function: the result is encoded bytes the
 *                function writes into a ta_buffer the driver lends (a fixed
 *                room), setting its length.
 *   bytes+check  as bytes, then jb_check over the result, timed with it. */
#ifndef JB_DRIVER_H
#define JB_DRIVER_H
#include <stdint.h>
#include <tinyactors/cbor.h>

/* Both call structs are the driver's own, not the header's ta_json_call
 * (which is changing in core): the writer mode keeps the ABI as it was. */

enum { JB_WRITER = 0, JB_BYTES = 1, JB_BYTES_CHECK = 2 };

typedef struct jb_writer_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;          /* the function's own */
    ta_bytes arguments;     /* one CBOR array of the arguments, valid during the call */
    ta_cbor_writer *result; /* write exactly one item */
    ta_string_view message; /* a failing function sets it */
} jb_writer_call;

typedef ta_status (*jb_writer_function)(jb_writer_call *call);

/* The call of the bytes modes. */
typedef struct jb_bytes_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;          /* the function's own */
    ta_bytes arguments;     /* one CBOR array of the arguments, valid during the call */
    ta_buffer *result;      /* the room: write one encoded item from the start, set length */
    ta_string_view message; /* a failing function sets it */
} jb_bytes_call;

typedef ta_status (*jb_bytes_function)(jb_bytes_call *call);

typedef struct jb_case {
    jb_writer_function function;      /* JB_WRITER */
    jb_bytes_function bytes_function; /* JB_BYTES, JB_BYTES_CHECK */
    void *context;
    ta_bytes arguments; /* prebuilt CBOR array, C memory */
    uint32_t writes_nothing; /* the crossing alone: no result expected */
    uint32_t mode;           /* JB_WRITER, JB_BYTES, JB_BYTES_CHECK */
} jb_case;

/* n calls of the case; ns per call, or a negative status if a call failed. */
double jb_time(const jb_case *c, uint64_t n);
/* One call; the result's bytes copied into out (length set). */
ta_status jb_once(const jb_case *c, ta_buffer *out);
/* The check pass over a result: every item read to the end; a tag other
 * than 32 or a simple value other than 20..23 refused. */
ta_status jb_check(ta_bytes result);
/* Write the one encoded CBOR item `item` into `w`, re-emitted item by item
 * through ta_cbor_reader and the writer's calls (the writer has no raw
 * write). */
ta_status jb_write_item(ta_cbor_writer *w, ta_bytes item);
/* A cgo.Handle carried as the call's context. */
void *jb_handle_context(uintptr_t handle);

ta_status jb_c_empty(jb_writer_call *call);
ta_status jb_c_has_items(jb_writer_call *call);
ta_status jb_c_add_item(jb_writer_call *call);
ta_status jb_c_new_item(jb_writer_call *call);
ta_status jb_c_reemit(jb_writer_call *call); /* context: a ta_bytes to re-emit */

ta_status jb_c_empty_bytes(jb_bytes_call *call);
ta_status jb_c_has_items_bytes(jb_bytes_call *call);
ta_status jb_c_add_item_bytes(jb_bytes_call *call);
ta_status jb_c_new_item_bytes(jb_bytes_call *call);

/* The Go trampolines, from _cgo_export.h, with the ABIs' signatures. */
ta_status jb_go_trampoline(jb_writer_call *call);
ta_status jb_go_bytes_trampoline(jb_bytes_call *call);
#endif
