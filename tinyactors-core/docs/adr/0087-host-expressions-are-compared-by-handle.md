# ADR 0087: Host expressions are compared by handle

**Status:** Accepted (2026-09-28); not implemented. Amends ADR 0077 section 1 (the exemption of charts with host expressions). Makes napi ADR 0029's JSON workaround unnecessary; napi ADR 0030 applies this decision to the addon.
**Date:** 2026-09-28

## Context

ADR 0077 made registration idempotent by identity: a live definition with
the same identity answers, and the same explicit identity for other
content is refused with `TA_ERR_DEFINITION_MISMATCH`. The content is the
compiler's hash, which it computes as it compiles.

A `TA_EXPRESSION_HOST` expression is opaque to that hash. The compiler
hashes nothing for it and sets `definition->host_expressions`
(`src/builder.c`), and registration then skips both rules
(`src/system.c`, `ta_system_register_definition`). ADR 0077 exempted
such charts because merging two that differ only in their functions would
run the wrong code, and refusing them would break defining the same
builder chart twice.

The exemption covers too much:

- **One host expression hides the whole chart.** A chart with a single
  function is never answered by identity, and a changed structure under
  the same explicit revision is never reported, although core sees that
  structure whole.
- **Hosts invent workarounds.** napi's builder wrote plain values as JSON
  text so that its charts would not be exempt (napi ADR 0029's notes).
  That turned a value the author passed into a copy, and made
  deduplication depend on the type of a value.

Core already has what it needs to tell handles apart. Every host
expression is a `(v, self, value)` triple, and the compiled definition
keeps the value its adapter's `retain` returned. Two triples that are
equal name the same handle; that is what a handle is. No new operation is
needed to compare them.

## Decision

1. **The content hash covers a chart's structure, host expressions
   included.** The compiler hashes a marker for each host expression in
   place of its contents. Replacing a function with source text, adding
   one, or moving one to another element changes the hash; the handle
   itself never enters it. The content hash stays what a fingerprinted
   definition's revision is made of, so revisions stay the same in every
   process.

2. **Registration compares host expressions by handle.** A definition
   being registered is checked against the live definitions with the same
   identity and the same data-model adapter:

   | Content hash | Host expressions, in expression order | Result |
   |---|---|---|
   | different | — | `TA_ERR_DEFINITION_MISMATCH` |
   | same | every triple equal to one live definition's | That live definition answers and gains a hold |
   | same | no live definition's are all equal | Registers beside them |

   A triple is equal when `v`, `self` and the retained `value` are equal
   as pointers. A chart without host expressions compares nothing and is
   answered as before. Several definitions with the same identity can be
   live (registered beside, or compiled against another adapter), so the
   check looks at each.

3. **A host decides whether its handles can match.** The public header
   says: two host expressions with equal `(v, self, value)` are the same
   handle, and core compares the values `retain` returned. A host that
   wants its charts answered by identity gives the same value for the
   same host object, and a `retain` that returns the value it was given.
   A host whose `retain` returns a new value each time never matches, and
   its charts register beside each other as they do today.

4. **Handles are compared, never hashed or kept.** A pointer means
   something only in this process and only while it is retained. Core
   compares only against live definitions, which retain their handles, so
   a freed and reused pointer cannot match. No handle enters a revision, a
   session image, an export's identity or a trace.

5. **Removed:** the `host_expressions` exemption from both registration
   rules. The flag stays only if something else still reads it; otherwise
   it goes.

## Consequences

- A host that interns its handles gets identity for builder charts:
  defining the same chart with the same functions and values answers the
  live definition, and defining a definition's own export answers that
  definition.
- A chart whose functions are made anew each time (closures created by
  the code that builds the chart) registers beside, as ADR 0077 wanted.
- **Changing a chart's structure under the same explicit revision is now
  reported for every chart**, including one with host expressions, while
  the old definition is live. An explicit revision is the author's
  promise that the chart has not changed (ADR 0077); a development loop
  that redefines an edited chart must change its revision or retire the
  old definition first.
- Fingerprints of charts with host expressions change once, because of
  the marker. They were never stable across hosts, since the host
  expressions they describe are not.
- Registration costs one pointer comparison per host expression for each
  live definition with the same identity and content, which is usually
  one.

## Implementation notes

- **Where:** `src/builder.c` (the marker in `ta_compiler_expression`),
  `src/system.c` (`ta_system_register_definition`, looking at every live
  definition with the identity), `src/definition.h` (`host_expressions`,
  if nothing else reads it), `include/tinyactors.h` (the contract on
  `ta_expression_host`), then `mise run amalgamate` and `mise run api`.
- **Tests:** a chart with host expressions registered twice with the same
  handles answers one definition and takes a second hold; with another
  handle for one expression it registers beside; the same identity with a
  different structure is `TA_ERR_DEFINITION_MISMATCH`; a host whose
  retain returns new values never matches; two definitions registered
  beside, then a third with the handles of the second, is answered by the
  second; retiring and freeing keep working with several live definitions
  of one identity (ADR 0077 section 2, ADR 0079); an out-of-memory sweep
  over registration if the lookup can allocate.
