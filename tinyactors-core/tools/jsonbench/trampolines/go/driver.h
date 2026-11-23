/* The C side of the Go trampoline spike: the driver loop that calls a
 * ta_json_function as the json datamodel does (src/datamodel_json, the
 * call of a function: a fresh writer over a fixed room, no allocator, one
 * ta_json_call, ta_cbor_writer_bytes after), the pure-C baselines, and the
 * helper that writes an encoded item into the result writer. */
#ifndef JB_DRIVER_H
#define JB_DRIVER_H
#include <stdint.h>
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_json.h>

typedef struct jb_case {
    ta_json_function function;
    void *context;
    ta_bytes arguments; /* prebuilt CBOR array, C memory */
    uint32_t writes_nothing; /* the crossing alone: no result expected */
} jb_case;

/* n calls of the case; ns per call, or a negative status if a call failed. */
double jb_time(const jb_case *c, uint64_t n);
/* One call; the result's bytes copied into out (length set). */
ta_status jb_once(const jb_case *c, ta_buffer *out);
/* Write the one encoded CBOR item `item` into `w`, re-emitted item by item
 * through ta_cbor_reader and the writer's calls (the writer has no raw
 * write). */
ta_status jb_write_item(ta_cbor_writer *w, ta_bytes item);
/* A cgo.Handle carried as the call's context. */
void *jb_handle_context(uintptr_t handle);

ta_status jb_c_empty(ta_json_call *call);
ta_status jb_c_has_items(ta_json_call *call);
ta_status jb_c_add_item(ta_json_call *call);
ta_status jb_c_reemit(ta_json_call *call); /* context: a ta_bytes to re-emit */

/* The Go trampoline, from _cgo_export.h, with the ABI's signature. */
ta_status jb_go_trampoline(ta_json_call *call);
#endif
