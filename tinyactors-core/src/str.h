/**
 * @file str.h
 *
 * # String views
 *
 * Helpers for `ta_str`, the public `{ptr, len}` view of bytes that core
 * uses for every name, URI and text (`include/tinyactors.h`): comparing
 * two views and making one from a NUL-terminated C string.
 *
 * ## Contract
 *
 * - A `ta_str` borrows its bytes; it is not NUL-terminated, and `ptr` may
 *   be NULL when `len` is 0.
 * - `ta_str_equal` compares lengths, then bytes: two empty views are
 *   equal whatever their pointers.
 * - `ta_str_from_cstr` answers a view of `text` up to, not including, its
 *   terminating NUL. `text` is never NULL.
 *
 * ## Memory
 *
 * Nothing here owns or allocates memory: a view borrows bytes its caller
 * keeps alive. Both functions may run inside a macrostep.
 *
 * ## Complexity
 *
 * `ta_str_equal` is O(length) and stops at the first difference;
 * `ta_str_from_cstr` is O(length of the C string).
 *
 * ## Failure
 *
 * Neither function fails. A NULL `text`, or a view whose `ptr` does not
 * hold `len` bytes, is a programmer error, not checked.
 *
 * ## Users
 *
 * The atom table, addresses and sessions, the builder and compiler, the
 * interpreter, the data model, the SCXML URI parser, the codec, the
 * system, the session image, and the tests.
 */
#pragma once

#include "../include/tinyactors.h"

/** Whether `left` and `right` hold the same bytes. Never allocates. */
bool ta_str_equal(ta_str left, ta_str right);

/** A view of the NUL-terminated `text`, without its NUL. Borrows `text`; never allocates. */
static inline ta_str ta_str_from_cstr(const char *text) {
    ta_str out = { .ptr = text, .len = 0 };
    while (text[out.len] != '\0') {
        out.len += 1;
    }
    return out;
}
