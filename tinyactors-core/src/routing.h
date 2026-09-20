#pragma once

#include "event.h"
#include "fault.h"

/* External delivery and the router boundary are fully public. */

/*
 * A named ta_actor_ref borrows its family and name text from whoever built
 * it. An owner that outlives that text — a delivery the router retains, a
 * named actor's own self reference — copies it with ta_address_copy() and
 * gives it back with ta_address_release(). Both ignore a runtime ref.
 */
ta_result ta_address_copy(ta_actor_ref *ref, ta_allocator allocator);
void ta_address_release(ta_actor_ref *ref, ta_allocator allocator);
