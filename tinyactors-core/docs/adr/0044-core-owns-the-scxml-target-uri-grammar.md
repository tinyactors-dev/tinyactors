# ADR 0044: Core owns the SCXML target URI grammar

**Status:** Accepted; amends ADR 0040, applies ADR 0031. The session URI grammar is extended by ADR 0088: `#_scxml_<incarnation>.<session>.<family>`, the text of every session ID; a bare `#_scxml_<n>` still parses.
**Date:** 2026-09-20

## Context

A `<send>` names its target two ways, and they took different paths:

- `target="#_parent"` is a literal, which core parsed itself.
- `targetexpr="..."` is evaluated to an opaque data-model value, which the
  adapter's `decode_scxml_target` turned into a `ta_scxml_target`.

The URI grammar is the same in both cases, so it had to exist once in core
and once in every data model. The test host had two copies for its two
adapters before merging them, which made three implementations of one
grammar for two data models.

They drifted, and the drift was invisible until a test compared the two
spellings of the same text:

| Target | `target="..."` | `targetexpr` |
|---|---|---|
| `#_internal` | delivered | delivered |
| `#_self` | `error.communication` | delivered |
| `#_parent` (no parent) | `error.communication` | `error.communication` |
| `#_scxml_99999999` | `error.communication` | `error.communication` |
| `#_scxml_4x` | `error.communication` | `error.execution` |
| `elsewhere` | `error.execution` | `error.execution` |

Two rows disagree, and both matter:

- **`#_scxml_4x`.** IRP test496 (mandatory, C.1) requires
  `error.communication` for a session that does not exist or is
  inaccessible, and its own target is `#_scxml_foo`. Core treats a session
  URI whose text is not a session ID as naming no session. An adapter can
  only report a fault, which core maps to `error.execution`, so the adapter
  path could not express "a valid address that is not reachable". That
  judgment is core's, not the data model's.
- **`#_self`.** SCXML defines `#_internal`, `#_parent`, `#_<invokeid>`, and
  `#_scxml_<sessionid>`. `#_self` is none of them, so core reads it as the
  invocation named "self". The adapters had invented it, because each
  adapter was free to define its own dialect.

Core also only ever went one way. It wrote `#_scxml_<id>` for
`$_event.origin` and for the I/O processor location, but never read that
form back, so every host wrote the parser for the URIs core prints and
nothing made printing and parsing agree. A chart replying to
`$_event.origin` depended on two implementations matching by hand.

ADR 0031 already says the data-model ABI is semantic, not syntactic.
Parsing a URI is syntax.

## Decision

**Core owns the grammar, in both directions.**

- `ta_scxml_target_parse()` parses a target URI. Core uses it for a literal
  target and for an evaluated one, so both spellings mean the same thing by
  construction.
- `ta_scxml_target_format()` writes the URI of a target. The text core gives
  a chart for `$_event.origin` and for the I/O processor location goes
  through the same writer, so format and parse are inverses and a reply to
  an origin parses back to its sender.

**Adapters supply text, not a parsed target.** `decode_scxml_target` is
removed from `ta_datamodel_vtable`. Core evaluates `targetexpr`, asks the
adapter for the value's text through `value_text`, which every adapter
already implements for the other `*expr` attributes, and parses that text
itself. Core therefore decides the SCXML outcome uniformly: an invalid
target is `error.execution` (6.2), and a well-formed address that names
nothing reachable is `error.communication` (C.1).

`value_text` gives text that is "borrowed from `value` and valid while it is
retained". A send record retains the evaluated target value, so a parsed
named address stays valid for as long as the send exists, including a
delayed send waiting in the timer heap.

**The grammar includes the logical address of a named actor**,
`#_actor_<family>/<name>` (ADR 0037), in both directions. Core resolves the
target to a destination and hands a named destination to the external-event
router, because the host owns location and residency: the actor may be
unloaded and need loading from a checkpoint, it may live in another process
or shard, and the host may hold an address fence around routing. Core keeps
the address protocol, origin construction, and the mapping of a router
refusal to `error.communication`.

## Consequences

- One implementation of the grammar, in one file, with one unit test.
- A data model that wants a target value that is not text is not served by
  `value_text`. No such model exists here; if one appears, it gets an
  optional hook that returns a `ta_scxml_target` directly, and the reason to
  add it is that its targets are not text, not that it wants its own
  grammar.
- A target expression whose value is not a string now takes the adapter's
  ordinary `value_text` conversion instead of an adapter-specific "target
  must be a string" fault. Both end in `error.execution` unless the text
  happens to be a valid target.
- `#_self` is gone from the test host's dialect. It was never SCXML.
- ADR 0040's "the selected data-model adapter decodes an evaluated opaque
  target value into a structured `ta_scxml_target`" is replaced by the
  above. The rest of ADR 0040 stands: core provides the SCXML Event I/O
  Processor, implements the target and envelope rules, and submits external
  delivery through the host's router.
