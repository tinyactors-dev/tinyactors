# ADR 0048: Core owns the send ID text, in both directions

**Status:** Accepted; applies ADR 0044, closes the gap ADR 0046 recorded
**Date:** 2026-09-20

## Context

A `<send>` has an ID, and core already writes it as text in three places a
chart can see: `idlocation` stores it, `_event.sendid` carries it, and
`<cancel sendidexpr>` reads it back. The grammar is core's: a literal
`<send id>` is the author's text, and a generated ID is `#_send.<n>`, the
actor-local counter. `src/interpreter.c` implements both directions
(`ta_send_id_write`, `ta_send_id_from_text`) so that "idlocation,
sendidexpr, and `_event.sendid` agree", as its comment says.

None of that was public. A host holds the *other* form: an I/O processor
receives `ta_send.send_id` as a `ta_send_id`, a number. So the chart holds
`#_send.1` and the processor holds `9223372036854775809`, they name the same
send, and nothing lets either side get to the other's form.

This surfaced while writing the scenarios for ADR 0046, where a custom action
is a `<send>` to a host processor and the reply has to name the call it
answers. The obvious recipe — the chart stores the ID with `idlocation`, the
host echoes it back — could not be written. The scenarios correlate on a
token the chart passes as a `<param>` instead, and ADR 0046 recorded the gap.

A host that wanted the SCXML ID would have to re-derive `#_send.<n>` from the
integer: the prefix, the counter, and the bit that distinguishes generated
from literal IDs. That is ADR 0044's situation exactly — "core prints a URI,
so core parses it" — one rule implemented twice, drifting silently, with the
host's copy depending on a bit layout core is free to change.

## Decision

**The send ID text grammar is public, in both directions**, as the target URI
grammar is:

- `ta_send_id_parse(system, text, out)` resolves the text of a send ID to the
  `ta_send_id` it names. The generated form always parses, whether or not
  such a send exists, as a session URI does (ADR 0044). A literal ID must
  already be interned in that system, because no send of that system can
  carry text it has never seen: otherwise `TA_ERR_NOT_FOUND`. Text that is no
  send ID at all, such as an empty string, is `TA_ERR_INVALID`.
- `ta_send_id_format(system, params)` writes the text of a `ta_send_id` into
  the caller's buffer, with the `capacity`/`needed` convention every other
  list and buffer in the API uses: a too-small buffer answers `TA_ERR_NOMEM`
  and reports what it needed, so a zero capacity is a size query. A send with
  no ID (`0`) has no text: `TA_ERR_NOT_FOUND`.

The two are inverses for every ID a system can produce, which is the property
that makes them worth having and the one the tests state.

## Consequences

- A host can now correlate on the ID SCXML already defines: the processor
  formats the `send_id` it was handed and answers with the text its chart
  stored through `idlocation`. ADR 0046's recipe gains that option; a
  chart-supplied `<param>` token remains the simpler one, and both are
  legitimate.
- A host may also go the other way — parse a send ID out of its own records
  and hand it to core — which is what a durability layer needs when it
  reconstructs a session and wants to name the sends it knew about.
- Core keeps the freedom the internal representation had: the bit that marks
  a generated ID, the counter's width, and the prefix are implementation
  details again, because no host has to know them to render the text.
- `tests/send_id_test.c` specifies the grammar the way `tests/scxml_uri_test.c`
  specifies the target URIs: one place, both directions, including the
  size-query convention and the `#_send.0` edge, which is not a generated ID.
- ADR 0046's note that "core's text rule is not public, so a host cannot
  render one from the other" is superseded by this ADR.
