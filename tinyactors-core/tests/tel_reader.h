/**
 * @file tel_reader.h
 *
 * # Reading tel in a test
 *
 * Core writes its trace records to the system's `tel` ring (ADR 0098), and
 * a host reads them between calls. A test that checks records reads them
 * the same way: after each call that runs the system, `tel_drain` hands
 * every trace record on the ring, oldest first, to a function of the
 * test's, and releases it. It counts the drop reports it skips, so a test
 * can prove nothing was lost.
 *
 * It lives with the tests and is never compiled into the library. Every
 * test program is one translation unit, so the helper is a header of a
 * static function; it includes the public header itself.
 *
 * ## Use
 *
 * ```c
 * static void seen(void *self, const ta_trace_record *record) { ... }
 * ...
 * CHECK(test_drive_run(system, actor, NULL) == TA_OK);
 * tel_drain(system, seen, NULL);
 * ```
 *
 * A record's texts and lists point into the ring and are released when the
 * function returns: copy what you keep.
 */
#pragma once

#include "../include/tinyactors.h"

/** What a test does with one trace record. */
typedef void (*tel_write_fn)(void *self, const ta_trace_record *record);

/** Drop reports `tel_drain` has skipped since the program started. */
static long tel_drops_seen = 0;

/**
 * Hands every trace record on `system`'s tel ring to `write`, oldest first,
 * releasing each, and answers how many it handed. A system without a ring
 * has none.
 */
static inline long tel_drain(ta_system *system, tel_write_fn write, void *self) {
    long count = 0;
    ta_tel_record record;
    while (ta_system_tel_peek(system, &record) == TA_OK) {
        if (record.kind == TA_TEL_TRACE) {
            write(self, &record.trace);
            count += 1;
        } else {
            tel_drops_seen += 1;
        }
        (void)ta_system_tel_release(system);
    }
    return count;
}
