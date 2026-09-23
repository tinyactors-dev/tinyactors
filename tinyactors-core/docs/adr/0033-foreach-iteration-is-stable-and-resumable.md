# ADR 0033: Foreach iteration is stable and resumable

- Status: Accepted; durability wording refined by ADR 0038; pausing inside a `<foreach>` implemented by ADR 0061
- Date: 2026-09-15

## Context

SCXML foreach behaves as if iteration occurs over a shallow copy of the collection. Changes made to the source collection during the loop must not change which elements are visited.

Tinyactors also allows execution to pause and resume in the middle of a macrostep. Therefore a foreach loop cannot rely on a stack-local host iterator that disappears when `ta_actor_resume()` returns.

## Decision

Starting a foreach operation asks the data model to materialize an opaque **iteration snapshot** representing the required shallow-copy semantics.

The core stores:

- the iteration snapshot as a `ta_value`;
- the current iteration position;
- the compiled item and optional index binding names;
- the executable-content continuation.

The data model exposes indexed semantic access to the snapshot rather than a stack-bound iterator cursor.

Before each iteration, the core asks the data model to bind/update the item variable and optional index variable. The binding operation must create the named variable if it does not already exist.

## Why indexed access

An opaque iteration snapshot plus integer position is easy to:

- pause and resume;
- retain in a session image;
- inspect in a debugger;
- fuzz deterministically.

The plugin remains free to implement the iteration snapshot internally as an array, tuple, host object, persistent vector, or custom wrapper.

## Consequences

A foreach iteration snapshot has normal `ta_value` lifetime semantics. An in-process `ta_session_image` retains that host value; it does not require the value to serialize. If a process-independent durability layer checkpoints an actor suspended inside foreach, that layer must provide a stable encoding or rebinding rule for the retained value.
