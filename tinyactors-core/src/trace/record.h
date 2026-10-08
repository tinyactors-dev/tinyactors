/* record.h — writing trace records into a journal: what core and the system
 * share (docs/adr/0006 in the repository root, section 3). A writer reserves
 * a record, fills its header and body, and commits it. When the journal
 * lost records since the last one written, a TA_TRACE_DROPPED record saying
 * how many goes first. */
#ifndef TINYACTORS_TRACE_RECORD_H
#define TINYACTORS_TRACE_RECORD_H

#include <tinyactors/trace.h>

#include "base/journal.h"

/* A record of `size` bytes (header included), its header zeroed but for
 * kind and size, or NULL when the journal has no room (the record is then
 * counted as lost). `fuel` and `now` stamp a TA_TRACE_DROPPED record written
 * first. */
ta_trace_record *ta_trace_record_reserve(ta_journal *journal, uint32_t kind, uint32_t size,
                                         uint64_t fuel, uint64_t now);
/* Commit the record reserved last. */
TA_INLINE void ta_trace_record_commit(ta_journal *journal, const ta_trace_record *record) {
    ta_journal_commit(journal, record->size);
}
TA_INLINE void *ta_trace_record_body(ta_trace_record *record) {
    return record + 1;
}

#endif /* TINYACTORS_TRACE_RECORD_H */
