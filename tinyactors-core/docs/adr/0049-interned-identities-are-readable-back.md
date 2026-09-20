# ADR 0049: Identities core interns are readable back

**Status:** Accepted; applies ADR 0044 and ADR 0048
**Date:** 2026-09-20

## Context

ADR 0044 made core own the target URI grammar in both directions, and ADR
0048 did the same for send IDs, both for one reason: core printed a form,
hosts had to re-derive it, and the two copies drifted. A sweep of the rest of
the public surface for that shape found three more, each with evidence in
this repository rather than in theory.

**Atoms.** `ta_system_intern_atom` interns text into a `ta_atom`, and nothing
reads one back, although three public structures hand hosts atoms:
`ta_send.io_type`, `ta_invoke_request.type`, and the reserved type
`ta_system_scxml_io_type()` returns. A host cannot say what it was given. The
test host's I/O journal records the type as a bare integer
(`instruments.c`, `tat_u64(send->io_type)`), and a scenario that wants to
check which processor a send selected has to intern the name again just to
compare numbers. An embedder that wants to log or route by processor or
invoker name cannot.

**State IDs.** `ta_definition_state` resolves a `ta_state_id` to the author's
ID, and nothing resolves the author's ID to a `ta_state_id`. The consequence
is not a missing convenience: **every data model re-implements the lookup as
a linear scan**, because `In()` receives the author's text and
`ta_dm_host_in_state` takes a `ta_state_id`:

```c
/* src/null_datamodel.c in tinyactors-test; the jq and ECMAScript adapters
   have the same loop */
for (ta_state_id state = 0; ta_dm_host_state_name(host, state, &name) == TA_OK; state += 1)
    if (name.len == id.len && memcmp(name.ptr, id.ptr, id.len) == 0)
        return ta_dm_host_in_state(host, state);
```

Every `In("s1")` of every chart costs O(states), each adapter separately
decides what an unknown state means, and the mapping being scanned is one
core built at compile time, from atoms it interned. The scenario DSL's
`state_id` does the same over the protocol.

## Decision

**An identity core interns is readable back in the form the chart sees.**
Four functions, within the three-parameter rule (`AGENTS.md`), which is why
the definition-wide lookup takes a struct:

```c
/* The text an atom interns, borrowed from the system. */
TA_API ta_result ta_system_atom_text(const ta_system *system,
                                     ta_atom atom,
                                     TA_OUT TA_LIFETIME(system) ta_str *out);

/* The state of a definition with this author ID. */
typedef struct {
    ta_definition_id definition;
    ta_str id;
    TA_OUT ta_state_id *out;
} ta_definition_find_state_params;
TA_API ta_result ta_definition_find_state(const ta_system *system,
                                          TA_IN const ta_definition_find_state_params *params);

/* The same lookup from inside a data-model callback, for In(). */
TA_API ta_result ta_dm_host_find_state(const ta_dm_host *host,
                                       ta_str id,
                                       TA_OUT ta_state_id *out);

/* Which invocation a ta_invoke_id is. */
TA_API ta_result ta_system_invoke_id_text(const ta_system *system,
                                          ta_invoke_id invoke,
                                          TA_OUT TA_LIFETIME(system) ta_str *out);
```

`ta_active_invoke.id` is the fourth handle of this kind: an invocation's ID
is an atom plus one, so `ta_system_atom_text` cannot render it and the offset
is private. A host inspecting an actor could list its invocations but not say
which one each was, although the same text is what `done.invoke.<id>` carries
and what a `#_<invokeid>` target spells. It is a getter rather than a
parse/format pair, as the text always exists: an ID that does not name one is
`TA_ERR_NOT_FOUND`, and the parsing direction is already public through
`ta_scxml_target_parse`.

`TA_ERR_NOT_FOUND` means no such atom or state, which is a question, not a
failure: `In()` of a state the machine does not have is simply false
(SCXML B.1), and an adapter answers that without inventing a rule of its own.
`TA_ERR_INVALID` is reserved for a null output or a foreign system, as
elsewhere.

`ta_system_atom_text` borrows from the system's atom table, which lives as
long as the system, so it needs no buffer and no `needed` count — unlike the
formatting calls, an atom's text already exists.

### What this decision does not extend to

**Event names stay structured.** There is no dotted-string parser, and a name
segment may never contain a dot. `ta_event_name_intern` keeps taking an
ordered `ta_atom_list`, and a host that holds text splits it into segments
itself and interns each one. The asymmetry with `ta_event_name_format` is
deliberate: a name is a token path, and letting text in would make "one
segment" and "several segments" depend on the spelling of the text rather
than on what the caller meant. `docs/internal/testing-and-api-change.md` §8
cited that splitting as a reason to change core; that example is withdrawn
with this ADR, and the two other examples there stand.

## Consequences

- The three adapters in `tinyactors-test` lose their `In()` scans, and so
  does every embedder's. The lookup happens once, in core, against the atom
  table the compiler already filled.
- The test host journals I/O and invoker types as their names. Scenarios that
  compared interned numbers compare text instead, which is what they meant.
- `ta_definition_find_state` gives tools — inspectors, debuggers, importers
  resolving a target after the fact — the direction they lacked. It is the
  inverse of `ta_definition_state`, and the tests state that.
- An atom of another system is `TA_ERR_NOT_FOUND`, not undefined behaviour:
  atoms are system-local (ADR 0007), and reading one back is where that can
  first be observed.
- A host inspecting an actor can name its invocations, not only count them,
  which is what an admin endpoint or a debugger showing invoked children
  needs. The invocation an event came from can be matched to the invocation
  a view lists.
- Nothing about the internal representation is promised. An atom, a state ID
  and an invocation ID stay opaque handles; only their text is public.
