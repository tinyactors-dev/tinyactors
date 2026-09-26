# ADR 0077: A definition is reused by identity and freed when nothing holds it

**Status:** Accepted and implemented (2026-09-26); see the implementation notes. Refines ADR 0028 (identity), ADR 0043 (retirement) and ADR 0053 (literal content for invokers). Supersedes napi ADR 0023 and napi ADR 0021 point 3. ADR 0078 adds a paged walk over every held definition, so nested and invoke-time definitions are visible to hosts; nesting stays reported from parent to child, since a child can have several parents.
**Date:** 2026-09-26

## Context

napi's SCXML invoker (napi ADR 0021) compiled a child chart given as SCXML
text when the `<invoke>` ran. To avoid compiling the same text on every
invoke it kept a cache of "documents" keyed by the text's bytes, which
then had to be bounded, evicted and measured (napi ADR 0023).

That cache was a second, weaker copy of something core already has:

- **Identity** (ADR 0028): every definition has a family and a revision,
  and when a chart gives no revision, the compiler fingerprints its
  content (`builder.c`, a 64-bit hash written as 16 hex digits).
- **Lookup by identity:** `ta_system_find_definition`.
- **A lifetime** (ADR 0043): a retired definition is freed after its last
  resident actor.

An inline `<scxml>` in `<content>` is not a runtime "document". It is a
nested chart definition, known when its parent is defined. And a chart
given as text at invoke time (`<content expr>`, `srcexpr`) is an ordinary
definition compiled at run time, which core already supports. What was
missing were two rules connecting the parts.

## Decision

### 1. Registration is idempotent by identity

`ta_system_register_definition()` answers a registered, non-retired
definition with the same identity instead of registering a second one:
the existing definition gains a hold, and the new compile is destroyed.

- **The same identity for different content is refused** with
  `TA_ERR_DEFINITION_MISMATCH`. An explicit revision is the author's
  promise that the chart is the same; a changed chart under an old
  revision is a mistake to report, not a request to answer with the old
  chart. Every definition keeps the compiler's content hash, whatever its
  revision, so the check costs one comparison.
- **The same chart compiled against another data-model adapter** is
  another definition, because it runs differently. It registers beside
  the first, as before.
- **A chart with a host expression is exempt** from both rules. A
  `TA_EXPRESSION_HOST` (a JavaScript function in a builder chart) is
  retained, not hashed, so two charts that differ only in their functions
  have the same fingerprint. Merging them would run the wrong code, and
  refusing them would break defining the same builder chart twice. Such a
  chart registers beside any match. The rules apply to charts whose
  content core sees whole: every SCXML import, and so every inline child.
- **The fingerprint is computed by the compile itself,** so a repeated
  define still decodes and compiles before the duplicate is found. A
  separate pre-pass over the spec would duplicate the compiler's walk,
  which ADR 0060's visit-order rule makes costly to keep in step. The
  implementation notes record what a repeat costs.

### 2. A definition nothing holds is freed once nothing uses it

A definition counts **holds** and **holders**:

- each `ta_system_register_definition()` takes a hold, and the new
  `ta_system_release_definition()` gives one back without retiring;
- each definition that nests it for an inline `<invoke>` is a holder
  (section 3);
- resident actors are counted as before (`actor_count`).

The system frees a definition when holds, holders and actors are all
zero: at the release, the retirement, the parent's freeing or the last
actor's end, whichever comes last. Freeing a definition drops its hold on
each definition it nests, and that cascade runs as a worklist
(ADR 0060).

- **An explicit `define()`** holds its definition until it is retired, as
  today. `ta_system_retire_definition()` still ends availability (no
  spawns, no match by identity) and now drops every hold.
- **A definition made implicitly,** such as a child compiled for one
  invoke, is registered, spawned from, and released: its actors are its
  only holders, so it is freed with its last child. A repeat of the same
  text is answered by identity while one is alive.

### 3. An inline child is a nested definition

`ta_definition_nest(system, { parent, invoke, child })` records that the
parent's `<invoke>` number `invoke` starts `child`. The parent becomes a
holder of the child until the parent is freed. Every invocation of that
`<invoke>` carries the child as `ta_invoke_request.definition`, so the
invoker spawns it without compiling anything.

`ta_definition_invoke()` describes one `<invoke>` by index, for a host
walking a definition it has just defined: its literal type (normalized
as the request's is), whether the type is computed, its literal
`<content>` and literal `src` as content IDs, and what it nests. The
definition inspection gains `invoke_count`, `holds` and `holders`.

Core does not decode SCXML, so the host nests: when it defines a chart,
it compiles each inline `<scxml>` child and each literal `src`, nests it,
and releases its own hold. A broken child is then refused when the parent
is defined, with its position, instead of at the invoke.

### 4. The family of an unnamed child

A chart's family is its `name` when it has no identity. An inline child
often has no name, so an identity given with an empty revision now names
only a default family, used when the chart has none, and the revision is
still the fingerprint. A host gives an unnamed child the family
`<parent family>/invoke`: readable in traces and inspection, and scoped
to the parent's family, so two families' unnamed children never share a
definition by accident. A child with a `name` keeps it as its family, as
any imported chart does.

## Consequences

- napi's document cache, its options, metrics and log, and ADR 0021
  point 3 go away (napi ADR 0024). A child definition is an ordinary
  definition, visible to inspection and to its metrics.
- Defining the same content twice no longer grows the system. It never
  did anything useful before: `find` returned only the newest.
- `ta_invoke_request` grows by one `ta_definition_id`.
- Per definition: two `uint32_t` counters, a `uint64_t` hash, a flag, a
  pointer, and one `ta_definition_id` per `<invoke>` once something is
  nested. Nothing per actor.
- A host that registers a definition only to spawn from it must release
  its hold, or the definition lives until the system closes.

## Testing

- **Core** (`retirement_test`): a second registration answers the first
  definition with two holds and keeps nothing new; the same explicit
  revision for another chart is refused; a released definition stays
  spawnable and matched by identity, and is freed after its last actor;
  a nested child outlives its registrar's release and is freed with its
  parent; `ta_definition_invoke()` over every `<invoke>`.
- **The test host:** the same chart under two data models stays two
  definitions (`null_datamodel.rb`).
- **napi** (napi ADR 0024): an inline child compiled once at define time,
  a broken inline child refused at define time, `<content expr>` text
  reused by identity and freed after its last child, and no growth over
  many invokes of text that varies.

## Implementation notes

- **Where:** `src/system.c` (registration, `ta_system_release_definition`,
  `ta_system_settle_definition` with its worklist), `src/actor.c` (the
  last actor settles its definition), `src/invoke.c`
  (`ta_definition_invoke`, `ta_definition_nest`, and
  `ta_invoke_request.definition`), `src/builder.c` (the content hash, the
  family-only identity, the host-expression flag), `src/definition.{c,h}`.
- **Retiring drops every hold, not one:** `ta_system_retire_definition`
  keeps its meaning of "this definition is done", however many times it
  was registered.
- **Charts with host expressions are exempt** from both registration
  rules (section 1), found while testing: napi's builder charts reuse an
  explicit revision for charts that differ only in their functions.
- **Same identity, other data-model adapter:** registers beside, found
  by the test host's `null_datamodel.rb`, which compiles one chart under
  two data models.
- **Measured** (napi, a parent with an inline child, 2,000 repeated
  defines, Node 22): 22.4 µs and 5,151 bytes of core memory kept per
  define before, 21.8 µs and 0 bytes after (napi ADR 0024).
