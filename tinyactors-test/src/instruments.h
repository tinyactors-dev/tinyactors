#pragma once

/*
 * Instrumented adapters: named, process-lifetime adapter instances that
 * record what core asks of them in a journal and answer with faults a test
 * arms beforehand. A scenario reads and steers them between steps through
 * the instrument.* host methods; callbacks never become nested RPC
 * conversations.
 *
 * Every instrument takes config {"name": <unique name>, ...}. Naming an
 * existing instrument with {"name": ...} alone reuses it, so one counting
 * allocator can back a system and the definitions it compiles.
 */

#include <stdbool.h>

#include "binding.h"

bool tat_make_counting_allocator(tat_bind *bind, jv config, void *out);
bool tat_make_recording_router(tat_bind *bind, jv config, void *out);
bool tat_make_memory_directory(tat_bind *bind, jv config, void *out);
bool tat_make_scripted_invoker(tat_bind *bind, jv config, void *out);
bool tat_make_recording_io(tat_bind *bind, jv config, void *out);
bool tat_make_recording_log(tat_bind *bind, jv config, void *out);
bool tat_make_recording_inspector(tat_bind *bind, jv config, void *out);

/* instrument.list, instrument.journal, instrument.control. */
jv tat_instrument_list_method(tat_bind *bind, jv params, int *error_code);
jv tat_instrument_journal_method(tat_bind *bind, jv params, int *error_code);
jv tat_instrument_control_method(tat_bind *bind, jv params, int *error_code);
