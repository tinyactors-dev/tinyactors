#include "tinyactors_host/adapter_trace.h"

#include "tinyactors_host/system.h"
#include "system_private.h"

static void tah_trace_write(void *self, const ta_trace_record *record) {
    tah_system_trace(self, record); /* writing a record cannot fail (core ADR 0067) */
}

static const ta_trace_sink_vtable tah_trace_vtable = { .write = tah_trace_write };

ta_trace_sink tah_trace_sink_adapter(tah_system *system, const tah_trace_masks *masks) {
    return (ta_trace_sink){ .self = system, .v = &tah_trace_vtable, .kinds = masks->kinds,
                            .watched_kinds = masks->watched_kinds, .unsampled_kinds = masks->unsampled_kinds };
}
