#pragma once
/* The kernel's table of event names it holds as text (tinyactors_host/events.h). */
#include "tinyactors_host/events.h"
#include "tinyactors_host/containers.h"

typedef struct {
    tah_vec spans;  /* tah_name_span, one per name; ID - 1 is the index */
    tah_vec bytes;  /* every name's text, back to back */
    tah_vec slots;  /* uint32_t: a span index + 1, or 0 for empty; a power of two */
    uint32_t depth; /* the deepest name accepted, in segments */
} tah_names;

void tah_names_init(tah_names *names, ta_allocator allocator, uint32_t depth);
void tah_names_destroy(tah_names *names);

/* The kernel ID of `text`, added on first use. TA_ERR_INVALID for malformed
 * text, TA_ERR_NOMEM when the table cannot grow. */
ta_result tah_names_intern(tah_names *names, ta_str text, ta_event_name_id *out);

/* The text of a kernel name ID, valid until the next tah_names_intern();
 * empty for an ID that is not one. */
ta_str tah_names_text(const tah_names *names, ta_event_name_id id);

/* `*name` as a kernel ID: a core name is formatted by `core` and added. */
ta_result tah_names_adopt(tah_names *names, const ta_system *core, ta_event_name_id *name);

/* Like ta_event_name_format(), for a kernel ID or a core name. */
ta_result tah_names_format(const tah_names *names, const ta_system *core, const ta_event_name_format_params *params);
